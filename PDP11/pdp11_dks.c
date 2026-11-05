/* pdp11_dks.c: ДКС terminal concentrator on Электроника-60 (Э-60)

   Copyright (c) 2026, Leonid Broukhis, Serguei K, the SIMH ДКС project

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
   OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

   Except as contained in this notice, the names of the authors shall not be
   used in advertising or otherwise to promote the sale, use or other
   dealings in this Software without prior written authorization from the
   authors.

   dks           ДКС terminal concentrator

   The ДКС is the display concentrator of the Диспак system: a micro-ЭВМ
   «Электроника-60» (a PDP-11 clone) that connects up to 24 terminals to
   the БЭСМ-6.  The БЭСМ-6 reaches the ОЗУ of the Э-60 through КАДОПАМ
   (instructions 032/0132); the reference OS-side implementation lives in
   BESM6/besm6_dks.c.  Here the PDP-11 simulates the Э-60 itself, so its
   32K-word ОЗУ (e60_mem) lives inside the device and the firmware reaches
   it through the KADDR/KDATA window — the same way КАДОПАМ reaches the
   ОЗУ of the real machine even when its CPU is stopped.

   Registers (Unibus, IOLN_DKS = 022 bytes, autoconfigured address):

        00  CSR     control/status: IE 0100 (rw), RR 0200, LC 0400,
                    ATN 1000 (ro), MCLR 040000 (write, master clear)
        02  RBUF    received: line<<8 | char, DV 040000; read pops the FIFO
        04  TSEL    transmit line select (1..24, Э-60 numbering)
        06  TBUF    transmit: character out to the selected line (KOI-7)
        10  KADDR   word address in the ОЗУ Э-60; bit 0100000 = auto-increment
        12  KDATA   data of the ОЗУ window; writing 077777 = 0400 rings the Э-60
        14  KCTL    read: channel status (0400<<2 present) | selected channel;
                    write: bits 0-2 channel select, 0400 = signal БЭСМ-6 (ПРП6)
        16  LSTAT0  telnet lines 1..12 connected
        20  LSTAT1  telnet lines 13..24 connected

   Interrupts (BR4): DKSRX vector V (received data), DKSATN vector V+4
   (doorbell from the БЭСМ-6, line state change).  Lines are telnet
   connections on the ATTACH port; lines are numbered 1..24 everywhere in
   the device (TMXR lines 0..23 correspond to Э-60 lines 1..24).

   Stages (see the docs in the patch series):

        0   this skeleton: registers, TMXR, FIFO, interrupts;
        A   protocol engine in C (port of BESM6/besm6_dks.c) + TESTOS stub:
            SET DKS ENGINE=SIM (default), SET DKS TESTOS=ON|PROMPT;
        B1  the engine moves into the Э-60 firmware executed by the CPU:
            SET DKS ENGINE=HOST; the device is only the hardware;
        B2  КАДОПАМ bridge over TCP to a real БЭСМ-6 simulator.
*/

#include "pdp11_defs.h"
#include "pdp11_dks.h"
#include "sim_tmxr.h"

#define DKS_FIFO_SIZE   512                             /* received data FIFO */

/* ОЗУ Э-60, one bank per КРК channel; only channel DKS_CHAN is backed. */
static uint16 e60_mem[4][32768];
static uint32 cur_chan = DKS_CHAN;                      /* window channel */

static uint16 dks_csr = 0;                              /* control/status */
static uint16 dks_rbuf = 0;                             /* last received */
static uint16 dks_tsel = 0;                             /* transmit line select */
static uint16 dks_kaddr = 0;                            /* ОЗУ window address */
static uint16 dks_lstat[2] = { 0, 0 };                  /* line state bitmaps */

static uint16 dks_fifo[DKS_FIFO_SIZE];                  /* (line<<8)|char */
static int32 fifo_in = 0, fifo_out = 0;

static int32 dks_wait = 1000;                           /* polling interval */
static int32 dks_engine = ENGINE_SIM;                   /* protocol engine */
static int32 dks_testos = TESTOS_PROMPT;                /* OS stub mode */

TMLN dks_ldsc[DKS_LINES];                               /* line descriptors */
TMXR dks_desc = { DKS_LINES, NULL, 0, dks_ldsc };

t_stat dks_svc (UNIT *uptr);
t_stat dks_reset (DEVICE *dptr);
t_stat dks_attach (UNIT *uptr, CONST char *cptr);
t_stat dks_detach (UNIT *uptr);
t_stat dks_rd (int32 *data, int32 PA, int32 access);
t_stat dks_wr (int32 data, int32 PA, int32 access);
int32 dks_rxinta (void);
int32 dks_atninta (void);
t_stat dks_setnl (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_set_engine (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_engine (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_set_testos (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_testos (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_set_log (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_set_nolog (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_log (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_show_vec (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
t_stat dks_help_attach (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *dks_description (DEVICE *dptr);

/* DIB: autoconfigured address, two vectors (RX, ATTN). */

DIB dks_dib = {
    IOBA_AUTO, IOLN_DKS, &dks_rd, &dks_wr,
    2, IVCL (DKSRX), VEC_AUTO, { &dks_rxinta, &dks_atninta },
    IOLN_DKS
    };

UNIT dks_unit = { UDATA (&dks_svc, UNIT_IDLE|UNIT_ATTABLE, 0) };

REG dks_reg[] = {
    { ORDATAD (CSR,   dks_csr,   16, "control/status register") },
    { ORDATAD (RBUF,  dks_rbuf,  16, "last received data") },
    { ORDATAD (TSEL,  dks_tsel,  16, "transmit line select") },
    { ORDATAD (KADDR, dks_kaddr, 16, "ОЗУ Э-60 window address") },
    { ORDATAD (KCHAN, cur_chan,  3,  "КРК channel of the window") },
    { GRDATAD (LSTAT0, dks_lstat[0], DEV_RDX, 16, 0, "lines 1-12 connected") },
    { GRDATAD (LSTAT1, dks_lstat[1], DEV_RDX, 16, 0, "lines 13-24 connected") },
    { DRDATAD (TIME,  dks_wait,  24, "polling interval"), PV_LEFT },
    { BRDATA (MEM, e60_mem[DKS_CHAN], DEV_RDX, 16, 32768) },
    { GRDATA (DEVADDR, dks_dib.ba, DEV_RDX, 32, 0), REG_HRO },
    { GRDATA (DEVVEC, dks_dib.vec, DEV_RDX, 16, 0), REG_HRO },
    { NULL }
    };

MTAB dks_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 1, NULL, "DISCONNECT",
        &tmxr_dscln, NULL, &dks_desc, "Disconnect a specific line" },
    { UNIT_ATT, UNIT_ATT, "summary", NULL,
        NULL, &tmxr_show_summ, (void *) &dks_desc, "Display a summary of line states" },
    { MTAB_XTD|MTAB_VDV|MTAB_NMO, 1, "CONNECTIONS", NULL,
        NULL, &tmxr_show_cstat, (void *) &dks_desc, "Display current connections" },
    { MTAB_XTD|MTAB_VDV|MTAB_NMO, 0, "STATISTICS", NULL,
        NULL, &tmxr_show_cstat, (void *) &dks_desc, "Display multiplexer statistics" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 010, "ADDRESS", "ADDRESS",
        &set_addr, &show_addr, NULL, "Bus address" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "VECTOR", "VECTOR",
        &set_vec, &dks_show_vec, (void *) &dks_desc, "Interrupt vector" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "LINES", "LINES=n",
        &dks_setnl, &tmxr_show_lines, (void *) &dks_desc, "Display number of lines" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "ENGINE", "ENGINE=SIM|HOST",
        &dks_set_engine, &dks_show_engine, NULL,
        "Protocol engine: SIM (in C, stage A) or HOST (in firmware, stage B1)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "TESTOS", "TESTOS=OFF|ON|PROMPT",
        &dks_set_testos, &dks_show_testos, NULL,
        "OS-side stub for stage A: echo loop, prompt dialog, or off" },
    { MTAB_XTD|MTAB_VDV|MTAB_NC, 0, NULL, "LOG=n=file",
        &dks_set_log, NULL, &dks_desc, "Enables logging on designated line" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, NULL, "NOLOG=n",
        &dks_set_nolog, NULL, &dks_desc, "Disable logging on designated line" },
    { MTAB_XTD|MTAB_VDV|MTAB_NMO, 0, "LOG", NULL,
        NULL, &dks_show_log, &dks_desc, "Display logging for all lines" },
    { 0 }
    };

DEBTAB dks_deb[] = {
    { "KADOPAM", DBG_KADOPAM, "KADDR/KDATA window accesses" },
    { "RING",    DBG_RING,    "ring slots, system requests, area writes" },
    { "IRQ",     DBG_IRQ,     "interrupts raised" },
    { "DATA",    DBG_DATA,    "characters to/from the lines" },
    { "PROTO",   DBG_PROTO,   "line state machine" },
    { NULL, 0 }
    };

DEVICE dks_dev = {
    "DKS", &dks_unit, dks_reg, dks_mod,
    1, DEV_RDX, 8, 1, DEV_RDX, 8,
    &tmxr_ex, &tmxr_dep, &dks_reset,
    NULL, &dks_attach, &dks_detach,
    &dks_dib, DEV_DISABLE | DEV_DIS | DEV_UBUS | DEV_QBUS | DEV_DEBUG | DEV_MUX,
    0, dks_deb, NULL, NULL,
    &dks_help, &dks_help_attach,          /* help and attach_help routines */
    (void *) &dks_desc,                   /* help context variable */
    &dks_description                      /* description routine */
    };

/* Received data FIFO.  The interrupt is raised while IE && RR. */

static int32 dks_fifo_cnt (void)
{
    return fifo_in - fifo_out;
    }

static void dks_update_rxi (void)
{
    if ((dks_csr & DKS_CSR_IE) && (dks_csr & DKS_CSR_RR))
        SET_INT (DKSRX);
    else
        CLR_INT (DKSRX);
    return;
    }

static void dks_update_atni (void)
{
    if ((dks_csr & DKS_CSR_IE) && (dks_csr & (DKS_CSR_ATN | DKS_CSR_LC)))
        SET_INT (DKSATN);
    else
        CLR_INT (DKSATN);
    return;
    }

static void dks_fifo_put (int32 ln, int32 ch)
{
    if (dks_fifo_cnt () >= DKS_FIFO_SIZE) {
        sim_debug (DBG_DATA, &dks_dev, "line %d: FIFO overrun, %03o dropped\n",
                   ln, ch & RBUF_CHAR);
        return;
        }
    dks_fifo[fifo_in & (DKS_FIFO_SIZE - 1)] =
        (uint16) ((ln << RBUF_V_LINE) | (ch & RBUF_CHAR));
    ++fifo_in;
    dks_csr |= DKS_CSR_RR;
    dks_update_rxi ();
    return;
    }

/* A character from a terminal: in this skeleton everything goes to the
   hardware FIFO; the SIM engine (stage A) takes the characters directly. */

static void dks_rx_char (int n, int c)
{
    sim_debug (DBG_DATA, &dks_dev, "line %d <- %03o\n", n, c & RBUF_CHAR);
    dks_fifo_put (n, c);
    return;
    }

/* Master clear: CSR, FIFO, window, interrupt state. */

static void dks_mclr (void)
{
    int n;

    dks_csr = 0;
    dks_rbuf = 0;
    dks_tsel = 0;
    dks_kaddr = 0;
    fifo_in = fifo_out = 0;
    for (n = 1; n <= DKS_LINES; ++n) {
        uint16 *w = &dks_lstat[(n - 1) / 12];
        int bit = (n - 1) % 12;

        if (dks_ldsc[n - 1].conn)
            *w |= (1 << bit);
        else
            *w &= ~(1 << bit);
        }
    CLR_INT (DKSRX);
    CLR_INT (DKSATN);
    return;
    }

/* Line state bookkeeping: LSTAT0/1 bitmaps and the LC flag. */

static void dks_line_state (int n, int connected)
{
    uint16 *w = &dks_lstat[(n - 1) / 12];
    int bit = (n - 1) % 12;
    uint16 was = *w;

    if (connected)
        *w |= (1 << bit);
    else
        *w &= ~(1 << bit);
    if (was != *w) {
        dks_csr |= DKS_CSR_LC;
        dks_update_atni ();
        sim_debug (DBG_PROTO, &dks_dev, "line %d %s\n",
                   n, connected? "connected": "disconnected");
        }
    return;
    }

/* IO dispatch routines. */

#define DKS_CSR_RD_MASK     (DKS_CSR_IE | DKS_CSR_RR | DKS_CSR_LC | DKS_CSR_ATN)

t_stat dks_rd (int32 *data, int32 PA, int32 access)
{
    int32 idx = ((PA - dks_dib.ba) >> 1) & 07;      /* register index */

    switch (idx) {

    case 00:                                        /* CSR */
        *data = dks_csr & DKS_CSR_RD_MASK;
        break;

    case 01:                                        /* RBUF: pop the FIFO */
        if (dks_fifo_cnt () > 0) {
            dks_rbuf = (dks_fifo[fifo_out & (DKS_FIFO_SIZE - 1)] & RBUF_M_LINE)
                        | RBUF_DV;
            ++fifo_out;
            sim_debug (DBG_DATA, &dks_dev, "RBUF <- line %d char %03o\n",
                       (dks_rbuf >> RBUF_V_LINE) & 037, dks_rbuf & RBUF_CHAR);
            }
        else
            dks_rbuf = 0;
        if (dks_fifo_cnt () == 0)                   /* drained? */
            dks_csr &= ~DKS_CSR_RR;
        dks_update_rxi ();
        *data = dks_rbuf;
        break;

    case 02:                                        /* TSEL */
        *data = dks_tsel;
        break;

    case 03:                                        /* TBUF: write only */
        *data = 0;
        break;

    case 04:                                        /* KADDR */
        *data = dks_kaddr;
        break;

    case 05:                                        /* KDATA: ОЗУ window */
        *data = e60_mem[cur_chan][dks_kaddr & KADDR_MASK];
        sim_debug (DBG_KADOPAM, &dks_dev, "KDATA read [%o]%05o <- %06o\n",
                   cur_chan, dks_kaddr & KADDR_MASK, *data);
        if (dks_kaddr & KADDR_AI)                   /* auto-increment */
            dks_kaddr = (uint16) (KADDR_AI | ((dks_kaddr + 1) & KADDR_MASK));
        break;

    case 06:                                        /* KCTL */
        *data = DKS_STATUS | (int32) (cur_chan & KCTL_M_CHAN);
        break;

    case 07:                                        /* LSTAT0/1 */
        if (PA & 1) {                               /* even words only */
            *data = 0;
            break;
            }
        if ((PA - dks_dib.ba) == DKS_LSTAT0)
            *data = dks_lstat[0];
        else                                        /* DKS_LSTAT1 */
            *data = dks_lstat[1];
        dks_csr &= ~DKS_CSR_LC;                         /* change seen */
        dks_update_atni ();
        break;
        }

    return SCPE_OK;
    }

t_stat dks_wr (int32 ldata, int32 PA, int32 access)
{
    int32 idx = ((PA - dks_dib.ba) >> 1) & 07;      /* register index */
    int32 data = ldata & 0177777;

    switch (idx) {

    case 00:                                        /* CSR */
        if (access == WRITEB)
            data = (PA & 1)?
                (dks_csr & 0377) | (data << 8):
                (dks_csr & ~0377) | data;
        if (data & DKS_CSR_MCLR) {                      /* master clear */
            dks_mclr ();
            break;
            }
        dks_csr = (dks_csr & ~DKS_CSR_RW) | (data & DKS_CSR_RW);
        dks_update_rxi ();
        dks_update_atni ();
        break;

    case 01:                                        /* RBUF: read only */
        break;

    case 02:                                        /* TSEL */
        if (access == WRITEB)
            data = (PA & 1)?
                (dks_tsel & 0377) | (data << 8):
                (dks_tsel & ~0377) | data;
        dks_tsel = (uint16) data;
        break;

    case 03:                                        /* TBUF: character out */
        if (dks_tsel >= 1 && dks_tsel <= DKS_LINES) {
            TMLN *lp = &dks_ldsc[dks_tsel - 1];
            int32 c = data & 0177;                  /* KOI-7: bit 7 ignored */

            if (lp->conn) {
                tmxr_putc_ln (lp, c);
                sim_debug (DBG_DATA, &dks_dev, "line %d -> %03o\n",
                           dks_tsel, c);
                }
            }
        break;

    case 04:                                        /* KADDR */
        if (access == WRITEB)
            data = (PA & 1)?
                (dks_kaddr & 0377) | (data << 8):
                (dks_kaddr & ~0377) | data;
        dks_kaddr = (uint16) data;
        break;

    case 05:                                        /* KDATA: ОЗУ window */
        if (access == WRITEB) {
            uint16 cur = e60_mem[cur_chan][dks_kaddr & KADDR_MASK];

            data = (PA & 1)?
                (cur & 0377) | (data << 8):
                (cur & ~0377) | data;
            }
        e60_mem[cur_chan][dks_kaddr & KADDR_MASK] = (uint16) data;
        sim_debug (DBG_KADOPAM, &dks_dev, "KDATA write [%o]%05o <- %06o\n",
                   cur_chan, dks_kaddr & KADDR_MASK, data);
        if ((dks_kaddr & KADDR_MASK) == DOORBELL && (data & DOORBELL_RING)) {
            sim_debug (DBG_IRQ, &dks_dev, "doorbell from БЭСМ-6\n");
            if (dks_engine == ENGINE_HOST) {        /* stage B1: firmware reacts */
                dks_csr |= DKS_CSR_ATN;
                dks_update_atni ();
                }
            else
                sim_activate (&dks_unit, 20);       /* like the reference */
            }
        if (dks_kaddr & KADDR_AI)                   /* auto-increment */
            dks_kaddr = (uint16) (KADDR_AI | ((dks_kaddr + 1) & KADDR_MASK));
        break;

    case 06:                                        /* KCTL */
        if (access == WRITEB && (PA & 1) == 0)      /* low byte: channel only */
            data = (cur_chan & ~KCTL_M_CHAN) | (data & KCTL_M_CHAN);
        cur_chan = data & KCTL_M_CHAN;
        if (data & KCTL_SIGNAL)
            sim_debug (DBG_RING, &dks_dev,
                       "signal to БЭСМ-6 (ПРП6 on its side)\n");
        break;

    case 07:                                        /* LSTAT0/1: read only */
        break;
        }

    return SCPE_OK;
    }

/* Interrupt acknowledge routines. */

int32 dks_rxinta (void)
{
    sim_debug (DBG_IRQ, &dks_dev, "DKSRX ack\n");
    CLR_INT (DKSRX);                                /* RBUF read refreshes it */
    return dks_dib.vec;
    }

int32 dks_atninta (void)
{
    sim_debug (DBG_IRQ, &dks_dev, "DKSATN ack\n");
    CLR_INT (DKSATN);
    dks_csr &= ~DKS_CSR_ATN;                            /* doorbell consumed */
    return dks_dib.vec + 4;                         /* LC stays until LSTAT read */
    }

/* Unit service: poll connections, input and output. */

t_stat dks_svc (UNIT *uptr)
{
    int32 ln, n, c;

    if (! (uptr->flags & UNIT_ATT))
        return SCPE_OK;
    while ((ln = tmxr_poll_conn (&dks_desc)) >= 0) {/* new connections */
        if (ln < dks_desc.lines)
            dks_line_state (ln + 1, TRUE);
        }
    for (n = 1; n <= dks_desc.lines; ++n) {         /* dropped connections */
        if (dks_ldsc[n - 1].conn == 0)
            dks_line_state (n, FALSE);
        }
    tmxr_poll_rx (&dks_desc);                       /* poll input */
    for (n = 1; n <= dks_desc.lines; ++n) {
        while ((c = tmxr_getc_ln (&dks_ldsc[n - 1])) != 0) {
            if (c & SCPE_BREAK)
                continue;
            dks_rx_char (n, c & 0377);
            }
        }
    tmxr_poll_tx (&dks_desc);                       /* poll output */
    sim_activate (uptr, dks_wait);
    return SCPE_OK;
    }

/* Reset, attach, detach. */

t_stat dks_reset (DEVICE *dptr)
{
    dks_desc.dptr = dptr;
    dks_desc.uptr = &dks_unit;
    memset (e60_mem, 0, sizeof (e60_mem));
    cur_chan = DKS_CHAN;
    dks_mclr ();
    sim_cancel (&dks_unit);
    if (dks_unit.flags & UNIT_ATT)
        sim_activate (&dks_unit, dks_wait);
    return auto_config (dptr->name, (dptr->flags & DEV_DIS)? 0: 1);
    }

t_stat dks_attach (UNIT *uptr, CONST char *cptr)
{
    t_stat r = tmxr_attach (&dks_desc, uptr, cptr);

    if (r != SCPE_OK)
        return r;
    sim_activate (uptr, dks_wait);
    return SCPE_OK;
    }

t_stat dks_detach (UNIT *uptr)
{
    t_stat r = tmxr_detach (&dks_desc, uptr);

    sim_cancel (uptr);
    return r;
    }

/* SET LINES processor: 1..24 telnet lines (the ОЗУ areas of all 24 lines
   always exist; extra lines simply never receive connections). */

t_stat dks_setnl (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    int32 newln, i, t;
    t_stat r;

    if (cptr == NULL)
        return SCPE_ARG;
    newln = (int32) get_uint (cptr, 10, DKS_LINES, &r);
    if ((r != SCPE_OK) || (newln == dks_desc.lines))
        return r;
    if (newln == 0)
        return SCPE_ARG;
    if (newln < dks_desc.lines) {
        for (i = newln, t = 0; i < dks_desc.lines; i++)
            t = t | dks_ldsc[i].conn;
        if (t && !get_yn ("This will disconnect users; proceed [N]?", FALSE))
            return SCPE_OK;
        for (i = newln; i < dks_desc.lines; i++) {
            if (dks_ldsc[i].conn) {
                tmxr_linemsg (&dks_ldsc[i],
                              "\r\nOperator disconnected line\r\n");
                tmxr_send_buffered_data (&dks_ldsc[i]);
                }
            tmxr_detach_ln (&dks_ldsc[i]);
            dks_line_state (i + 1, FALSE);
            }
        }
    dks_desc.lines = newln;
    return SCPE_OK;
    }

/* SET ENGINE / SET TESTOS processors. */

t_stat dks_set_engine (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    char gbuf[CBUFSIZE];

    if (cptr == NULL)
        return SCPE_ARG;
    cptr = get_glyph (cptr, gbuf, 0);
    if (strcmp (gbuf, "SIM") == 0)
        dks_engine = ENGINE_SIM;
    else if (strcmp (gbuf, "HOST") == 0)
        dks_engine = ENGINE_HOST;
    else
        return SCPE_ARG;
    return SCPE_OK;
    }

t_stat dks_show_engine (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "engine=%s", (dks_engine == ENGINE_SIM)? "SIM": "HOST");
    return SCPE_OK;
    }

t_stat dks_set_testos (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    char gbuf[CBUFSIZE];

    if (cptr == NULL)
        return SCPE_ARG;
    cptr = get_glyph (cptr, gbuf, 0);
    if (strcmp (gbuf, "OFF") == 0)
        dks_testos = TESTOS_OFF;
    else if (strcmp (gbuf, "ON") == 0)
        dks_testos = TESTOS_ON;
    else if (strcmp (gbuf, "PROMPT") == 0)
        dks_testos = TESTOS_PROMPT;
    else
        return SCPE_ARG;
    return SCPE_OK;
    }

t_stat dks_show_testos (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "testos=%s",
             (dks_testos == TESTOS_OFF)? "OFF":
             (dks_testos == TESTOS_ON)? "ON": "PROMPT");
    return SCPE_OK;
    }

/* SET LOG/NOLOG/SHOW LOG processors (per-line logging, like DZ). */

t_stat dks_set_log (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    t_stat r;
    char gbuf[CBUFSIZE];
    int32 ln;

    if (cptr == NULL)
        return SCPE_ARG;
    cptr = get_glyph (cptr, gbuf, '=');
    if ((cptr == NULL) || (*cptr == 0) || (gbuf[0] == 0))
        return SCPE_ARG;
    ln = (int32) get_uint (gbuf, 10, dks_desc.lines, &r);
    if ((r != SCPE_OK) || (ln >= dks_desc.lines))
        return SCPE_ARG;
    return tmxr_set_log (NULL, ln, cptr, desc);
    }

t_stat dks_set_nolog (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    t_stat r;
    int32 ln;

    if (cptr == NULL)
        return SCPE_ARG;
    ln = (int32) get_uint (cptr, 10, dks_desc.lines, &r);
    if ((r != SCPE_OK) || (ln >= dks_desc.lines))
        return SCPE_ARG;
    return tmxr_set_nolog (NULL, ln, NULL, desc);
    }

t_stat dks_show_log (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    int32 i;

    for (i = 0; i < dks_desc.lines; i++) {
        fprintf (st, "line %d: ", i);
        tmxr_show_log (st, NULL, i, desc);
        fprintf (st, "\n");
        }
    return SCPE_OK;
    }

t_stat dks_show_vec (FILE *st, UNIT *uptr, int32 arg, CONST void *desc)
{
    return show_vec (st, uptr, 2, desc);            /* RX + ATTN */
    }

/* Help and description. */

t_stat dks_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
fprintf (st, "ДКС Terminal Concentrator (Электроника-60)\n\n");
fprintf (st, "The ДКС is the display concentrator of the Диспак system: a micro-ЭВМ\n");
fprintf (st, "«Электроника-60» connecting up to %d terminals to the БЭСМ-6.  Each telnet\n", DKS_LINES);
fprintf (st, "connection is one terminal line (numbered 1..%d).\n\n", DKS_LINES);
fprintf (st, "The lines perform input and output through Telnet sessions connected\n");
fprintf (st, "to the ATTACH port:\n\n");
fprintf (st, "   sim> ATTACH DKS {interface:}port      set up listening port\n\n");
fprintf (st, "Other special ДКС commands:\n\n");
fprintf (st, "   sim> SHOW DKS CONNECTIONS            show current connections\n");
fprintf (st, "   sim> SHOW DKS STATISTICS             show statistics\n");
fprintf (st, "   sim> SET DKS DISCONNECT=line         disconnect the specified line\n");
fprintf (st, "   sim> SET DKS LINES=n                 set the number of lines (1..%d)\n", DKS_LINES);
fprintf (st, "   sim> SET DKS ENGINE=SIM|HOST         protocol engine: in C or in firmware\n");
fprintf (st, "   sim> SET DKS TESTOS=OFF|ON|PROMPT    OS-side stub (stage A)\n\n");
fprintf (st, "The firmware reaches the 32K-word ОЗУ of the Э-60 through the KADDR/KDATA\n");
fprintf (st, "window (bit 0100000 of KADDR is auto-increment); cell 077777 written with\n");
fprintf (st, "0400 is the doorbell from the БЭСМ-6.  Characters are KOI-7 (7 bits).\n\n");
dks_help_attach (st, dptr, uptr, flag, cptr);
return SCPE_OK;
    }

t_stat dks_help_attach (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
tmxr_attach_help (st, dptr, uptr, flag, cptr);
return SCPE_OK;
    }

const char *dks_description (DEVICE *dptr)
{
return "ДКС terminal concentrator (Электроника-60), 24 lines";
    }

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
#include "sim_sock.h"
#include "sim_tmxr.h"

#define DKS_FIFO_SIZE   512                             /* received data FIFO */

/* ОЗУ Э-60, one bank per КРК channel; only dks_chan is backed here. */
static uint16 e60_mem[4][32768];
static int32 dks_chan = DKS_CHAN;                       /* КРК channel of this Э-60 */
static uint32 cur_chan = DKS_CHAN;                      /* window channel */
static int dks_utf8 = 0;                /* ENCODING: 0 = raw КОИ-7Н1 */
static uint32 utf8_acc[DKS_LINES + 1];  /* UTF-8 input assembly */
static int utf8_need[DKS_LINES + 1];

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
static uint16 dks_kctl = 0;                             /* КАДОПАМ control shadow */

/* ===== КАДОПАМ bridge (stage B2) =====

   The bridge exposes the ОЗУ Э-60 to a БЭСМ-6 simulator over TCP, the
   same 8-byte frames КАДОПАМ exchanges internally (plan §8.1):

        request  D6 op chan addr_hi addr_lo val_hi val_lo 00
        response D7 op|0200 ...

   This side is the SERVER (SET DKS BRIDGE=port): the БЭСМ-6 connects
   with SET DKS REMOTE=host:port (BESM6/besm6_dks.c).  Frames are served
   from dks_svc, i.e. the PDP-11 CPU must be running (v1, no threads).
   One client at a time; a new connection replaces a stale one. */

#define BRIDGE_MAGIC_REQ    0xD6
#define BRIDGE_MAGIC_RSP    0xD7
#define BRIDGE_OP_READ      01
#define BRIDGE_OP_WRITE     02
#define BRIDGE_OP_ATTN      03
#define BRIDGE_OP_PING      04
#define BRIDGE_OP_BREAD     05          /* bulk read: header + 2*cnt bytes */
#define BRIDGE_OP_BWRITE    06          /* bulk write: header + payload */
#define BRIDGE_BULK_MAX     255         /* count is one byte */

static char dks_bridge_spec[CBUFSIZE] = "";             /* listening port */
static SOCKET dks_lsock = INVALID_SOCKET;               /* master socket */
static SOCKET dks_bsock = INVALID_SOCKET;               /* БЭСМ-6 client */
static uint8 dks_bfrm[8 + 2*BRIDGE_BULK_MAX];          /* inbound frame */
static int32 dks_bn = 0;                                /* bytes accumulated */
static int32 dks_bexp = 8;                              /* expected frame size */
static uint32 dks_nreads = 0, dks_nwrites = 0,          /* frame counters */
              dks_nattn = 0, dks_npings = 0, dks_nbad = 0,
              dks_nbulk = 0;

TMLN dks_ldsc[DKS_LINES];                               /* line descriptors */
TMXR dks_desc = { DKS_LINES, NULL, 0, dks_ldsc };

t_stat dks_svc (UNIT *uptr);
t_stat dks_reset (DEVICE *dptr);
t_stat dks_attach (UNIT *uptr, CONST char *cptr);
t_stat dks_detach (UNIT *uptr);
static int dks_line_can_input (int n);
static void dks_line_char (int n, int c);
t_stat dks_rd (int32 *data, int32 PA, int32 access);
t_stat dks_wr (int32 data, int32 PA, int32 access);
int32 dks_rxinta (void);
int32 dks_atninta (void);
t_stat dks_setnl (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_set_engine (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_engine (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_set_chan (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_chan (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_set_encoding (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_encoding (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_set_testos (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_testos (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_set_log (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_set_nolog (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_log (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
static void dks_update_atni (void);
t_stat dks_set_bridge (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_set_nobridge (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_bridge (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
static void dks_sig_besm6 (int sreq);
static void dks_bridge_poll (void);
static void testos_ring (void);
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
    { ORDATAD (KCTL,  dks_kctl,  16, "КАДОПАМ control (shadow: chan | SIGNAL pulse)") },
    { ORDATAD (BREAD, dks_nreads, 32, "bridge READ frames"), PV_LEFT },
    { ORDATAD (BWRITE,dks_nwrites,32, "bridge WRITE frames"), PV_LEFT },
    { ORDATAD (BATTN, dks_nattn, 32, "bridge ATTN frames sent"), PV_LEFT },
    { ORDATAD (BPING, dks_npings, 32, "bridge PING frames"), PV_LEFT },
    { ORDATAD (BBAD,  dks_nbad,  32, "bad bridge frames"), PV_LEFT },
    { ORDATAD (BBULK, dks_nbulk, 32, "bridge BULK frames (stage C)"), PV_LEFT },
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
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "CHAN", "CHAN=k",
        &dks_set_chan, &dks_show_chan, NULL,
        "The КРК channel of this Э-60 instance, 0-3 (stage C, default 2)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "ENCODING", "ENCODING=KOI7|UTF8",
        &dks_set_encoding, &dks_show_encoding, NULL,
        "Terminal encoding (stage C): raw КОИ-7Н1 or UTF-8 conversion" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "TESTOS", "TESTOS=OFF|ON|PROMPT",
        &dks_set_testos, &dks_show_testos, NULL,
        "OS-side stub for stage A: echo loop, prompt dialog, or off" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "BRIDGE", "BRIDGE=port",
        &dks_set_bridge, &dks_show_bridge, NULL,
        "КАДОПАМ bridge server: TCP port for the БЭСМ-6 (stage B2)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, NULL, "NOBRIDGE",
        &dks_set_nobridge, NULL, NULL, "Close the КАДОПАМ bridge" },
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
    { "BRIDGE",  DBG_BRIDGE,  "КАДОПАМ bridge frames" },
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

/* ===== Protocol engine (stage A: port of BESM6/besm6_dks.c) =====

   The engine is the Э-60 side of the СВЯЗЬ7 protocol, ported 1:1 from the
   reference (the ОС side): ring posts, terminal areas, system requests.
   In the SIM mode (default) the device runs it itself; TESTOS emulates the
   ОС side so the whole protocol closes in one process.  In the HOST mode
   (stage B1) the engine is the Э-60 firmware executed by the CPU, and the
   device only provides the hardware: the lines, the KADDR/KDATA window,
   the doorbell and the interrupts.

   Deviations from the reference are marked DEV: they all concern the
   transport (telnet lines instead of the БЭСМ-6 TTY clock), not the
   protocol: the signal to the БЭСМ-6 is a trace event here (the ПРП6
   interrupt is raised on the БЭСМ-6 side, by TESTOS locally or by the
   bridge in stage B2). */

typedef struct dks_line {
    int     up;                 /* the Э-60 sees the line as connected */
    int     want_connect;       /* connect request not yet sent */
    int     want_disconnect;
    int     term;               /* ОС terminal number, 0 if none */
    int     echo;
    int     len;                /* characters in the line being typed */
    char    buf[INBUF_BYTES];
    int     line_ready;         /* line finished, not yet passed to the ОС */
    int     pending;            /* 0, or the +2 command awaiting acknowledgement */
    int     out_done;           /* output printed, completion not yet posted */
    } DKS_LINE;

static DKS_LINE dks_line[DKS_LINES + 1];

static int os_polled = 0;       /* TESTOS: the ОС has read the channel status */
static int sys_busy = 0;        /* 0 none, 1 connect (await reply), 3 disconnect */
static int sys_line = 0;
static uint32 sys_src = 0;      /* абуфs source count when it was posted */
static uint32 bufh_recv = 0;    /* our receiver count in абуфh */
static uint32 bufh_bell = 0;    /* абуфh head as of the last doorbell */

static int testos_term_used[DKS_LINES + 1]; /* ОС terminal numbers in use */

static int dks_enabled (void)
{
    return ! (dks_dev.flags & DEV_DIS);
    }

/* The ОС listens only after it has read the channel status.  TESTOS
   emulates the ready ОС; with TESTOS=OFF the engine posts to the real
   БЭСМ-6 across the bridge (stage B2) regardless of readiness. */
static int os_ready (void)
{
    return os_polled || (dks_testos == TESTOS_OFF);
    }

static uint32 mem (int addr)
{
    return e60_mem[cur_chan][addr & 077777];
    }

static void set_mem (int addr, uint32 val)
{
    e60_mem[cur_chan][addr & 077777] = val & 0177777;
    }

/* Output helpers into a telnet line (DEV: the reference uses vt_send/
   vt_puts of besm6_tty.c).  Stage C, question B5: ENCODING=UTF8 spells
   the КОИ-7Н1 Cyrillic half (0140-0176) as UTF-8 for modern telnet
   clients; KOI7 (the default) passes the raw protocol bytes. */

/* КОИ-7Н1 bytes 0140..0176 (0x60-0x7E) -> Unicode (the mapping of
   besm6_tty.c); 0177 (0x7F) is the delimiter and stays unmapped. */
static const uint32 koi7_to_uni[31] = {
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413,
    0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412,
    0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427
    };

/* Unicode -> КОИ-7Н1, -1 when unmappable.  КОИ-7 has one case: upper
   and lower Cyrillic (0410-042F / 0430-044F) map to the same byte. */
static int uni_to_koi7 (uint32 val)
{
    int i;

    if (val <= '_')
        return (int) val;
    if (val >= 'a' && val <= 'z')
        return (int) (val + 'Z' - 'z');
    if (val >= 0x0430 && val <= 0x044F)         /* lower Cyrillic */
        val -= 0x20;
    for (i = 0; i < 31; ++i)
        if (koi7_to_uni[i] == val)
            return 0140 + i;                /* 0x60 + i */
    return -1;
}

static void dks_line_out (int n, int ch)
{
    TMLN *lp = &dks_ldsc[n - 1];
    uint32 uni;
    uint8 seq[2];

    if (! lp->conn)
        return;
    ch &= 0177;
    if (! dks_utf8 || ch < 0140 || ch > 0176) {     /* not КОИ-7Н1 Cyrillic */
        tmxr_putc_ln (lp, ch);
        return;
        }
    uni = koi7_to_uni[ch - 0140];
    seq[0] = (uint8) (0300 | (uni >> 6));           /* Cyrillic: 2 bytes */
    seq[1] = (uint8) (0200 | (uni & 077));
    tmxr_putc_ln (lp, seq[0]);
    tmxr_putc_ln (lp, seq[1]);
    }

static void dks_send_char (int n, uint32 sym)
{
    if (dks_ldsc[n - 1].conn)
        dks_line_out (n, (int) (sym & 0177));
    }

static void dks_send_str (int n, const char *s)
{
    TMLN *lp = &dks_ldsc[n - 1];

    if (lp->conn) {
        tmxr_linemsg (lp, s);
        tmxr_send_buffered_data (lp);
        }
    }

/* DEV: the reference raises ПРП6 on the БЭСМ-6 side (PRP |= DKS_PRP).
   Here the Э-60 signals through the device; the БЭСМ-6 sees the slot and
   its ПРП6 comes from TESTOS or the bridge.  A system request post also
   raises ПРП12 (stage C): the frame flag tells the БЭСМ-6 to set
   PRP_DKS_ATTN, which is what СВЯЗЬ7/Диспак wait for (нтерм7). */
static void dks_host_atten (int sreq)
{
    sim_debug (DBG_RING, &dks_dev, "signal to БЭСМ-6 (ПРП6 on its side)%s\n",
               sreq? " + ПРП12": "");
    dks_sig_besm6 (sreq);
    return;
    }

/* Post an abonent number into the ring towards the БЭСМ-6.
   sreq: the post is a system request (АБФОСН) -> ПРП12 on the БЭСМ-6.
   Returns 0 when the ring is full: the ОС has not caught up yet. */
static int e60_post (int abonent, int sreq)
{
    uint32 src = mem (A_BUFS) & RING_MASK;
    uint32 recv = mem (A_BUFS + 1) & RING_MASK;

    if (((src - recv) & RING_MASK) == RING_MASK)
        return 0;
    set_mem (A_BUFS + 2 + src, abonent);
    set_mem (A_BUFS, (src + 1) & RING_MASK);
    sim_debug (DBG_RING, &dks_dev, "post %d (slot %d)\n", abonent, src);
    dks_host_atten (sreq);
    return 1;
    }

static int find_line (int e60_line)
{
    return (e60_line >= 1 && e60_line <= DKS_LINES && dks_line[e60_line].up) ?
        e60_line : 0;
    }

/* Output array from the ОС: print it, then report completion. */
static void take_output (int n)
{
    int a = AREA(n);
    uint32 cmd = mem (a + AR_CMD_OUT);
    uint32 addr = mem (a + AR_OUTADDR) >> 1;
    uint32 len = mem (a + AR_OUTLEN);
    uint32 i;

    if (cmd != 4 && cmd != 5) {
        sim_debug (DBG_RING, &dks_dev, "line %d: entry without command %o\n",
                   n, cmd);
        return;
        }
    set_mem (a + AR_CMD_OUT, 0);
    sim_debug (DBG_RING, &dks_dev, "line %d: output cmd %o, %o words at %05o\n",
               n, cmd, len, addr);
    for (i = 0; i < len; ++i) {
        uint32 w = mem (addr + i);

        if ((w & 0377) == 0)
            break;
        dks_send_char (n, w & 0177);
        if ((w >> 8 & 0377) == 0)
            break;
        dks_send_char (n, w >> 8 & 0177);
        }
    if (dks_ldsc[n - 1].conn)                       /* push it out now */
        tmxr_send_buffered_data (&dks_ldsc[n - 1]);
    dks_line[n].out_done = 1;
    }

/* Ring from the ОС: take every entry posted up to the last doorbell.

   The ОС advances the ring head BEFORE writing the slot cell
   (observed with СВЯЗЬ7), and over the КАДОПАМ bridge the head update
   arrives one TCP frame ahead of the slot.  A drain that polls the
   live head can then run in between and read whatever the slot still
   held: 0, or - once the ring has wrapped - the STALE abonent of the
   previous cycle (observed: slot 0 still held 48 from the system
   reply, the drain consumed a phantom and the real output post was
   skipped for good - the terminal went silent after four exchanges).
   The doorbell completes a post, so the head is sampled AT the
   doorbell, exactly like the real firmware that sleeps until rung. */
static void drain_bufh (void)
{
    uint32 src = bufh_bell;

    while (bufh_recv != src) {
        int abonent = mem (A_BUFH + 2 + bufh_recv);
        int n;

        if (abonent == 0) {
            /* Belt and suspenders: a post is never complete without
               its doorbell, but a 0 slot still means "not landed". */
            sim_debug (DBG_RING, &dks_dev,
                       "ring slot %d: entry not posted yet\n",
                       (int) bufh_recv);
            break;
            }
        bufh_recv = (bufh_recv + 1) & RING_MASK;
        set_mem (A_BUFH + 1, bufh_recv);
        sim_debug (DBG_RING, &dks_dev, "got %d\n", abonent);
        n = find_line (abonent);
        if (n)
            take_output (n);
        }
    /* The reply to a connect request: абфосh+0 is written last. */
    if (sys_busy == 1 && mem (A_FOSH) == 2) {
        DKS_LINE *l = &dks_line[sys_line];
        int term = mem (A_FOSH + 1);
        int result = mem (A_FOSH + 2);

        set_mem (A_FOSH, 0);
        sys_busy = 0;
        sim_debug (DBG_RING, &dks_dev,
                   "line %d: connect reply term %d result %d\n",
                   sys_line, term, result);
        if (result == 0)
            l->term = term;
        else {
            dks_send_str (sys_line, result == 1 ?
                          "\r\nDKS: line busy\r\n" :
                          "\r\nDKS: no free terminal\r\n");
            l->up = 0;
            }
        }
    }

static void post_sys_request (int n, int code)
{
    int a = AREA(n);

    set_mem (A_FOSS, code);
    set_mem (A_FOSS + 1, code == 1 ? n : dks_line[n].term);
    set_mem (A_FOSS + 2, code == 1 ? a << 1 : 0);
    sys_src = mem (A_BUFS) & RING_MASK;
    if (! e60_post (ABONENT_OS, 1))       /* a system request: + ПРП12 */
        return;
    sys_busy = code;
    sys_line = n;
    sim_debug (DBG_RING, &dks_dev, "line %d: system request %d\n", n, code);
    }

static void start_line (int n)
{
    int a = AREA(n), i;

    for (i = 0; i < 8; ++i)
        set_mem (a + i, 0);
    set_mem (a + AR_INBUF, (a + AREA_INBUF) << 1);
    dks_line[n].len = 0;
    dks_line[n].line_ready = 0;
    dks_line[n].pending = 0;
    dks_line[n].out_done = 0;
    }

/* Is the ОС done with the last event of this line? */
static void check_ack (int n)
{
    int a = AREA(n);

    if (dks_line[n].pending == 4 && ! (mem (a + AR_STATUS0) & AR_FLAG))
        dks_line[n].pending = 0;
    else if (dks_line[n].pending == 5 && ! (mem (a + AR_STATUS1) & AR_FLAG))
        dks_line[n].pending = 0;
    }

static void post_line_event (int n, int cmd)
{
    int a = AREA(n);

    set_mem (a + AR_CMD_IN, cmd);
    if (cmd == 4)
        set_mem (a + AR_STATUS0, mem (a + AR_STATUS0) | AR_FLAG);
    if (! e60_post (dks_line[n].term, 0)) {
        set_mem (a + AR_CMD_IN, 0);
        return;
        }
    dks_line[n].pending = cmd;
    }

static void pass_input (int n)
{
    int a = AREA(n) + AREA_INBUF, i;
    DKS_LINE *l = &dks_line[n];

    for (i = 0; i <= l->len; i += 2) {
        uint32 lo = i < l->len ? (unsigned char) l->buf[i] : 0;
        uint32 hi = i + 1 < l->len ? (unsigned char) l->buf[i+1] : 0;

        set_mem (a + i/2, lo | hi << 8);
        }
    post_line_event (n, 4);
    if (l->pending == 4) {
        sim_debug (DBG_RING, &dks_dev, "line %d: input %d chars\n", n, l->len);
        l->len = 0;
        l->line_ready = 0;
        }
    }

/* Advance the Э-60 state machine; SIM engine only. */
static void dks_process (void)
{
    int n;

    if (! dks_enabled () || dks_engine != ENGINE_SIM)
        return;
    drain_bufh ();

    /* A disconnect gets no reply: wait until the ОС has read the ring. */
    if (sys_busy == 3) {
        uint32 posted = (mem (A_BUFS) - sys_src) & RING_MASK;
        uint32 taken = (mem (A_BUFS + 1) - sys_src) & RING_MASK;

        if (taken != 0 && taken <= posted) {
            sys_busy = 0;
            dks_line[sys_line].term = 0;
            }
        }
    for (n = 1; n <= DKS_LINES; ++n) {
        DKS_LINE *l = &dks_line[n];

        if (! sys_busy && l->want_disconnect) {
            l->want_disconnect = 0;
            if (l->term)
                post_sys_request (n, 3);
            l->up = 0;
            continue;
            }
        if (! sys_busy && l->want_connect && os_ready ()) {
            l->want_connect = 0;
            start_line (n);
            l->up = 1;
            post_sys_request (n, 1);
            if (! sys_busy) {
                l->up = 0;
                l->want_connect = 1;
                }
            continue;
            }
        if (! l->term)
            continue;
        check_ack (n);
        if (l->pending)
            continue;
        if (l->out_done) {
            post_line_event (n, 5);
            if (l->pending)
                l->out_done = 0;
            }
        else if (l->line_ready)
            pass_input (n);
        }
    }

/* Whether the line accepts another typed character now. */
static int dks_line_can_input (int n)
{
    return dks_enabled () && dks_line[n].term && ! dks_line[n].line_ready;
    }

/* A character typed on the line (KOI-7): local editing and echo.
   DEV: on Enter the reference echoes a bare newline; here CR LF keeps
   telnet clients happy. */
static void dks_line_char (int n, int c)
{
    DKS_LINE *l = &dks_line[n];

    switch (c) {

    case '\r':
    case '\n':
        if (l->echo)
            dks_send_str (n, "\r\n");
        l->line_ready = 1;
        break;

    case '\b':
    case 0177:
        if (l->len > 0) {
            --l->len;
            if (l->echo)
                dks_send_str (n, "\b \b");
            }
        break;

    default:
        if (l->len >= INBUF_BYTES - 1)
            break;
        l->buf[l->len++] = c;
        if (l->echo)
            dks_send_char (n, c);
        }
    return;
    }

/* ===== TESTOS: the ОС-side stub (stage A) =====

   Emulates just enough of СВЯЗЬ7 to close the protocol in one process:
   reads the channel status, takes the abonents out of абуфs (48 = system
   requests, 1..24 = terminal lines), allocates ОС terminal numbers,
   answers through абфосh and, for lines, produces an output array and
   rings the Э-60 (the doorbell). */

static int testos_alloc_term (void)
{
    int t;

    for (t = 1; t <= DKS_LINES; ++t) {
        if (! testos_term_used[t]) {
            testos_term_used[t] = 1;
            return t;
            }
        }
    return 0;
    }

static void testos_post_bufh (int abonent)
{
    uint32 src = mem (A_BUFH) & RING_MASK;

    set_mem (A_BUFH + 2 + src, abonent);
    set_mem (A_BUFH, (src + 1) & RING_MASK);
    sim_debug (DBG_RING, &dks_dev, "TESTOS: post %d to абуфh (slot %d)\n",
               abonent, src);
    }

/* The БЭСМ-6 rings the Э-60 (cell 077777 = 0400 through КАДОПАМ).
   With the engine in C the device services it directly; with the engine
   in the firmware (stage B1) the ring is the ATN interrupt. */
static void testos_ring (void)
{
    if (dks_engine == ENGINE_HOST) {
        dks_csr |= DKS_CSR_ATN;
        dks_update_atni ();
        }
    else {
        /* The TESTOS ОС completes its post with the doorbell, like the
           real СВЯЗЬ7: the cell write is what the firmware would see,
           the snapshot is what drain_bufh consumes up to. */
        set_mem (DOORBELL, DOORBELL_RING);
        bufh_bell = mem (A_BUFH) & RING_MASK;
        sim_activate (&dks_unit, 20);               /* like the reference */
        }
    }

/* Write a string as an output array and ring the Э-60 (DEV: ASCII text
   for now, see the B5 question about KOI-7/UTF-8). */
static void testos_output (int n, const char *str)
{
    int a = AREA(n);
    int i, len = strlen (str), words = 0;

    if (len > 2 * 01200)                        /* output array capacity */
        len = 2 * 01200;
    set_mem (a + AR_OUTADDR, (a + 8) << 1);
    for (i = 0; i < len; i += 2) {
        uint32 lo = str[i] & 0177;
        uint32 hi = (i + 1 < len)? (str[i + 1] & 0177): 0;

        set_mem (a + 8 + words++, lo | hi << 8);
        }
    set_mem (a + AR_OUTLEN, words);
    set_mem (a + AR_CMD_OUT, 4);
    set_mem (a + AR_STATUS1, mem (a + AR_STATUS1) | AR_FLAG);
    sim_debug (DBG_RING, &dks_dev, "TESTOS: %d bytes out to line %d\n",
               len, n);
    testos_post_bufh (n);
    testos_ring ();                                 /* the doorbell */
    }

/* TESTOS=ON: send the typed line back as an output array. */
static void testos_echo_back (int n)
{
    int a = AREA(n);
    uint32 inaddr = mem (a + AR_INBUF) >> 1;
    char buf[INBUF_BYTES + 1];
    int i, len = 0;

    for (i = 0; i < 01200; ++i) {
        uint32 w = mem (inaddr + i);

        if ((w & 0377) == 0)
            break;
        if (len < INBUF_BYTES)
            buf[len++] = w & 0177;
        if ((w >> 8 & 0377) == 0)
            break;
        if (len < INBUF_BYTES)
            buf[len++] = w >> 8 & 0177;
        }
    buf[len] = 0;
    sim_debug (DBG_RING, &dks_dev, "TESTOS: line %d input '%s'\n", n, buf);
    testos_output (n, buf);
    }

static void testos_process (void)
{
    uint32 src = mem (A_BUFS) & RING_MASK;
    uint32 recv = mem (A_BUFS + 1) & RING_MASK;
    int acted = 0;

    while (recv != src) {
        int abonent = mem (A_BUFS + 2 + recv);

        recv = (recv + 1) & RING_MASK;
        set_mem (A_BUFS + 1, recv);             /* free the slot */
        acted = 1;
        sim_debug (DBG_RING, &dks_dev, "TESTOS: take %d\n", abonent);
        if (abonent == ABONENT_OS) {
            int code = mem (A_FOSS);

            if (code == SYSREQ_CONNECT) {
                int ln = mem (A_FOSS + 1);
                int term = testos_alloc_term ();

                set_mem (A_FOSH, 2);
                set_mem (A_FOSH + 1, term);
                set_mem (A_FOSH + 2, term? 0: 2);
                sim_debug (DBG_RING, &dks_dev,
                           "TESTOS: connect line %d -> term %d result %d\n",
                           ln, term, term? 0: 2);
                if (term && dks_testos == TESTOS_PROMPT)
                    testos_output (ln, "\r\nE60 DKS ready\r\nDKS> ");
                }
            else if (code == SYSREQ_DISCONNECT) {
                int term = mem (A_FOSS + 1);

                if (term >= 1 && term <= DKS_LINES)
                    testos_term_used[term] = 0;
                acted = 1;
                sim_debug (DBG_RING, &dks_dev,
                           "TESTOS: disconnect term %d\n", term);
                }
            }
        else if (abonent >= 1 && abonent <= DKS_LINES) {
            int n = abonent;
            int a = AREA(n);
            uint32 cmd = mem (a + AR_CMD_IN);

            if (cmd == 4) {                     /* input ready */
                set_mem (a + AR_STATUS0,
                         mem (a + AR_STATUS0) & ~AR_FLAG);  /* acknowledge */
                if (dks_testos == TESTOS_ON)
                    testos_echo_back (n);
                else                            /* PROMPT */
                    testos_output (n, "\r\nDKS> ");
                }
            else if (cmd == 5) {                /* output done: acknowledge */
                set_mem (a + AR_STATUS1, mem (a + AR_STATUS1) & ~AR_FLAG);
                acted = 1;
                }
            }
        }
    /* The ОС rings адркс whenever it has news for the Э-60: a system
       reply (абфосh) or the consumption of the абуфs slots.  testos_output
       rings on its own (the output array).  With the engine in C (SIM)
       the ring just re-activates the unit; with the firmware (stage B1)
       it is the ATN interrupt the firmware lives on. */
    if (acted && dks_engine == ENGINE_HOST)
        testos_ring ();
    }

/* Received data FIFO.  The interrupt is raised while IE && RR. */

/* ===== КАДОПАМ bridge server (stage B2) ===== */

/* Close the bridge client and, when asked, the listening socket. */
static void dks_bridge_close (int all)
{
if (dks_bsock != INVALID_SOCKET) {
    sim_close_sock (dks_bsock);
    dks_bsock = INVALID_SOCKET;
    }
dks_bn = 0;
dks_bexp = 8;
if (all && dks_lsock != INVALID_SOCKET) {
    sim_close_sock (dks_lsock);
    dks_lsock = INVALID_SOCKET;
    dks_bridge_spec[0] = '\0';
    }
return;
}

/* The БЭСМ-6 is listening on the other end of the bridge.  Its readiness
   replaces the TESTOS os_polled artifact: with TESTOS=OFF the engine
   posts to the real ОС across the bridge. */
static int dks_bridge_up (void)
{
return (dks_bsock != INVALID_SOCKET);
}

/* Э-60 -> БЭСМ-6: "an entry waits in АБУФS" (ПРП6 on the БЭСМ-6 side).
   sreq (stage C): the entry is a system request -> the БЭСМ-6 also sets
   ПРП12 (PRP_DKS_ATTN). */
static void dks_sig_besm6 (int sreq)
{
uint8 f[8];

if (dks_bsock == INVALID_SOCKET)
    return;
f[0] = BRIDGE_MAGIC_REQ;
f[1] = BRIDGE_OP_ATTN;
f[2] = dks_chan;
f[3] = sreq? 1: 0;
f[4] = f[5] = f[6] = f[7] = 0;
if (sim_write_sock (dks_bsock, (const char *) f, 8) == 8) {
    ++dks_nattn;
    sim_debug (DBG_BRIDGE, &dks_dev, "bridge: -> ATTN%s\n",
               sreq? " +ПРП12": "");
    }
return;
}

/* One complete request frame from the БЭСМ-6: execute and respond.
   Bulk frames (05/06, stage C) carry cnt data words: 8 + 2*cnt bytes.
   Bulk data covers addresses 1..077776 only: cell 0 (channel select)
   and the doorbell 077777 keep their single-frame side effects. */
static void dks_bridge_frame (const uint8 *req)
{
uint8 rsp[8 + 2*BRIDGE_BULK_MAX];
int32 op = req[1], chan = req[2] & 3;               /* КРК: 4 канала */
int32 addr = (req[3] << 8) | req[4];
int32 val = (req[5] << 8) | req[6];

rsp[0] = BRIDGE_MAGIC_RSP;
rsp[1] = op | 0x80;
rsp[2] = chan;
rsp[3] = req[3];
rsp[4] = req[4];
rsp[5] = rsp[6] = rsp[7] = 0;
switch (op) {

case BRIDGE_OP_READ:                                /* КАДОПАМ 032 */
    ++dks_nreads;
    if (addr == 0)                                  /* channel status:
                                                       this instance's bit */
        val = 0400 << dks_chan;
    else if (addr == 2)                             /* номнпр */
        val = NOMNPR;
    else
        val = e60_mem[chan][addr];
    sim_debug (DBG_BRIDGE, &dks_dev,
               "bridge: READ [%o]%05o -> %06o\n", chan, addr, val);
    break;

case BRIDGE_OP_WRITE:                               /* КАДОПАМ 0132 */
    ++dks_nwrites;
    sim_debug (DBG_BRIDGE, &dks_dev,
               "bridge: WRITE [%o]%05o <- %06o\n", chan, addr, val);
    if (addr == 0) {                                /* channel select */
        cur_chan = val & 7;                         /* КАДОПАМ: cell 0 <- val */
        dks_kctl = (uint16) ((dks_kctl & ~KCTL_M_CHAN) | (val & 7));
        break;
        }
    e60_mem[chan][addr] = (uint16) val;
    if (chan == dks_chan && addr == DOORBELL) {     /* the БЭСМ-6 rings */
        if (val & ~DOORBELL_RING)
            sim_debug (DBG_RING, &dks_dev, "doorbell %06o\n", val);
        if (val & DOORBELL_RING) {
            sim_debug (DBG_IRQ, &dks_dev, "doorbell from БЭСМ-6\n");
            /* The head write of the post crossed before (TCP frame
               order); the post is complete - sample the head. */
            bufh_bell = mem (A_BUFH) & RING_MASK;
            if (dks_engine == ENGINE_HOST) {        /* firmware reacts */
                dks_csr |= DKS_CSR_ATN;
                dks_update_atni ();
                }
            else
                sim_activate (&dks_unit, 20);       /* like the reference */
            }
        }
    else if (chan == dks_chan &&
             addr >= AREA_BASE && addr < AREA (DKS_LINES + 1) &&
             (addr - AREA_BASE) % AREA_SIZE < 8)
        sim_debug (DBG_RING, &dks_dev, "line %d area+%d <- %06o\n",
                   (addr - AREA_BASE) / AREA_SIZE,
                   (addr - AREA_BASE) % AREA_SIZE, val);
    break;

case BRIDGE_OP_ATTN:                                /* Э-60 -> БЭСМ-6 only */
    sim_debug (DBG_BRIDGE, &dks_dev, "bridge: <- ATTN (ignored)\n");
    break;

case BRIDGE_OP_PING:
    ++dks_npings;
    sim_debug (DBG_BRIDGE, &dks_dev, "bridge: PING\n");
    break;

case BRIDGE_OP_BREAD: {                             /* bulk read (stage C) */
    int32 cnt = req[5], i;

    ++dks_nbulk;
    if (cnt == 0 || addr == 0 || addr + cnt > 077777) {
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: BULK_READ [%o]%05o x%d rejected\n",
                   chan, addr, cnt);
        cnt = 0;                                    /* error reply */
        }
    else {
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: BULK_READ [%o]%05o x%d\n", chan, addr, cnt);
        for (i = 0; i < cnt; ++i) {
            int32 w = e60_mem[chan][addr + i];

            rsp[8 + 2*i] = (w >> 8) & 0377;
            rsp[9 + 2*i] = w & 0377;
            }
        }
    rsp[0] = BRIDGE_MAGIC_RSP;
    rsp[1] = op | 0x80;
    rsp[2] = req[2];
    rsp[3] = req[3];
    rsp[4] = req[4];
    rsp[5] = cnt;
    rsp[6] = rsp[7] = 0;
    if (sim_write_sock (dks_bsock, (const char *) rsp, 8 + 2*cnt) < 0)
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: response write failed\n");
    return;
    }

case BRIDGE_OP_BWRITE: {                            /* bulk write (stage C) */
    int32 cnt = req[5], i;

    ++dks_nbulk;
    if (cnt == 0 || addr == 0 || addr + cnt > 077777) {
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: BULK_WRITE [%o]%05o x%d rejected\n",
                   chan, addr, cnt);
        cnt = 0;
        }
    else {
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: BULK_WRITE [%o]%05o x%d\n", chan, addr, cnt);
        for (i = 0; i < cnt; ++i) {
            int32 a = addr + i, w = (req[8 + 2*i] << 8) | req[9 + 2*i];

            e60_mem[chan][a] = (uint16) w;
            if (chan == dks_chan && a >= AREA_BASE &&
                a < AREA (DKS_LINES + 1) && (a - AREA_BASE) % AREA_SIZE < 8)
                sim_debug (DBG_RING, &dks_dev,
                           "line %d area+%d <- %06o\n",
                           (a - AREA_BASE) / AREA_SIZE,
                           (a - AREA_BASE) % AREA_SIZE, w);
            }
        }
    rsp[0] = BRIDGE_MAGIC_RSP;
    rsp[1] = op | 0x80;
    rsp[2] = req[2];
    rsp[3] = req[3];
    rsp[4] = req[4];
    rsp[5] = cnt;
    rsp[6] = rsp[7] = 0;
    if (sim_write_sock (dks_bsock, (const char *) rsp, 8) < 0)
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: response write failed\n");
    return;
    }

default:                                            /* unknown op */
    ++dks_nbad;
    sim_debug (DBG_BRIDGE, &dks_dev, "bridge: bad op %o\n", op);
    break;
    }
rsp[5] = (val >> 8) & 0377;
rsp[6] = val & 0377;
if (sim_write_sock (dks_bsock, (const char *) rsp, 8) != 8)
    sim_debug (DBG_BRIDGE, &dks_dev, "bridge: response write failed\n");
return;
}

/* Poll the bridge from dks_svc: accept, accumulate, serve. */
static void dks_bridge_poll (void)
{
int32 n;

if (dks_lsock != INVALID_SOCKET) {
    char *connectaddr = NULL;
    SOCKET newsock = sim_accept_conn (dks_lsock, &connectaddr);

    if (newsock != INVALID_SOCKET) {
        if (dks_bsock != INVALID_SOCKET) {          /* replace the stale one */
            sim_debug (DBG_BRIDGE, &dks_dev, "bridge: new client\n");
            dks_bridge_close (FALSE);
            }
        dks_bsock = newsock;
        dks_nreads = dks_nwrites = dks_nattn = 0;
        sim_debug (DBG_BRIDGE, &dks_dev, "bridge: client %s connected\n",
                   connectaddr? connectaddr: "?");
        }
    if (connectaddr)
        free (connectaddr);
    }
if (dks_bsock == INVALID_SOCKET)
    return;
while ((n = sim_read_sock (dks_bsock, (char *) dks_bfrm + dks_bn,
                           dks_bexp - dks_bn)) != 0) {
    if (n < 0) {                                    /* connection lost */
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: БЭСМ-6 disconnected\n");
        dks_bridge_close (FALSE);
        return;
        }
    dks_bn += n;
    if (dks_bexp == 8 && dks_bn >= 6 &&
        dks_bfrm[1] == BRIDGE_OP_BWRITE)
        dks_bexp = 8 + 2 * dks_bfrm[5];             /* + payload */
    else if (dks_bexp == 8 && dks_bn >= 6 &&
             dks_bfrm[1] == BRIDGE_OP_BREAD)
        dks_bexp = 8;                   /* the reply carries the data */
    if (dks_bn == dks_bexp) {
        if (dks_bfrm[0] == BRIDGE_MAGIC_REQ)
            dks_bridge_frame (dks_bfrm);
        else {
            ++dks_nbad;
            sim_debug (DBG_BRIDGE, &dks_dev,
                       "bridge: bad magic %02x, frame dropped\n", dks_bfrm[0]);
            }
        dks_bn = 0;
        dks_bexp = 8;
        }
    }
return;
}

/* SET/SHOW BRIDGE processors. */

t_stat dks_set_bridge (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
char gbuf[CBUFSIZE];

if (cptr == NULL || *cptr == 0)
    return SCPE_ARG;
if (dks_dev.flags & DEV_DIS)
    return SCPE_UDIS;
cptr = get_glyph (cptr, gbuf, 0);
if (*cptr != 0)
    return SCPE_2MARG;
dks_bridge_close (TRUE);                            /* drop the old one */
strncpy (dks_bridge_spec, gbuf, CBUFSIZE - 1);
/* REUSEADDR: the БЭСМ-6 reconnects when the Э-60 restarts. */
dks_lsock = sim_master_sock_ex (dks_bridge_spec, &r, SIM_SOCK_OPT_REUSEADDR);
if (dks_lsock == INVALID_SOCKET) {
    dks_bridge_spec[0] = '\0';
    return r? r: SCPE_OPENERR;
    }
sim_messagef (SCPE_OK, "Bridge listening on port %s\n", dks_bridge_spec);
return SCPE_OK;
}

t_stat dks_set_nobridge (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
dks_bridge_close (TRUE);
return SCPE_OK;
}

t_stat dks_show_bridge (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
if (dks_lsock == INVALID_SOCKET)
    fprintf (st, "bridge=off");
else
    fprintf (st, "bridge=%s%s", dks_bridge_spec,
             dks_bsock != INVALID_SOCKET? " (connected)": " (listening)");
return SCPE_OK;
}

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

/* A character from a terminal.  In the SIM mode the engine takes it
   directly (local echo and line editing, like the reference); in the HOST
   mode it goes to the hardware FIFO and raises the RX interrupt for the
   firmware. */

static void dks_rx_char (int n, int c)
{
    sim_debug (DBG_DATA, &dks_dev, "line %d <- %03o\n", n, c & 0377);
    if (dks_engine == ENGINE_SIM) {
        if (dks_line_can_input (n))
            dks_line_char (n, c & 0177);        /* KOI-7 */
        else
            sim_debug (DBG_DATA, &dks_dev,
                       "line %d: char %03o dropped, previous line pending\n",
                       n, c & 0377);
        }
    else
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

/* Line state bookkeeping: LSTAT0/1 bitmaps, the LC flag and the engine
   connect/disconnect requests. */

static void dks_line_state (int n, int connected)
{
    uint16 *w = &dks_lstat[(n - 1) / 12];
    int bit = (n - 1) % 12;
    uint16 was = *w;
    DKS_LINE *l = &dks_line[n];

    if (connected)
        *w |= (1 << bit);
    else {
        *w &= ~(1 << bit);
        utf8_need[n] = 0;               /* drop a partial UTF-8 sequence */
        }
    if (was != *w) {
        dks_csr |= DKS_CSR_LC;
        dks_update_atni ();
        sim_debug (DBG_PROTO, &dks_dev, "line %d %s\n",
                   n, connected? "connected": "disconnected");
        }
    if (connected) {
        l->want_disconnect = 0;
        l->want_connect = 1;
        l->echo = 1;
        dks_ldsc[n - 1].rcve = 1;               /* enable the line for input */
        if (dks_engine == ENGINE_SIM)           /* stage B1: the firmware greets */
            dks_send_str (n, "Connected to DKS\r\n");
        }
    else {
        l->want_connect = 0;
        if (l->up)
            l->want_disconnect = 1;
        }
    return;
    }

/* IO dispatch routines. */

#define DKS_CSR_RD_MASK     (DKS_CSR_IE | DKS_CSR_RR | DKS_CSR_LC | DKS_CSR_ATN)

t_stat dks_rd (int32 *data, int32 PA, int32 access)
{
    int32 idx = ((PA - dks_dib.ba) >> 1) & 017;      /* register index (0-8: LSTAT1 is 8) */

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
        *data = (0400 << dks_chan) | (int32) (cur_chan & KCTL_M_CHAN);
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
    int32 idx = ((PA - dks_dib.ba) >> 1) & 017;      /* register index (0-8: LSTAT1 is 8) */
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
                dks_line_out (dks_tsel, c);
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
        if ((dks_kaddr & KADDR_MASK) == DOORBELL) {
            if (data & ~DOORBELL_RING)              /* diagnostic trace */
                sim_debug (DBG_RING, &dks_dev, "doorbell %06o\n", data);
            if (data & DOORBELL_RING) {
                sim_debug (DBG_IRQ, &dks_dev, "doorbell from БЭСМ-6\n");
                /* The head of the post is already in the ОЗУ (the
                   store above) - the post is complete, sample it. */
                bufh_bell = mem (A_BUFH) & RING_MASK;
                if (dks_engine == ENGINE_HOST) {    /* stage B1: firmware reacts */
                    dks_csr |= DKS_CSR_ATN;
                    dks_update_atni ();
                    }
                else
                    sim_activate (&dks_unit, 20);   /* like the reference */
                }
            }
        if (dks_kaddr & KADDR_AI)                   /* auto-increment */
            dks_kaddr = (uint16) (KADDR_AI | ((dks_kaddr + 1) & KADDR_MASK));
        break;

    case 06:                                        /* KCTL */
        if (access == WRITEB) {
            uint16 cur = dks_kctl;

            data = (PA & 1)?
                (cur & 0377) | (data << 8):
                (cur & ~0377) | data;
            }
        dks_kctl = (uint16) (data & (KCTL_M_CHAN | KCTL_SIGNAL));
        cur_chan = data & KCTL_M_CHAN;
        if (data & KCTL_SIGNAL) {                   /* pulse: signal БЭСМ-6 */
            dks_kctl &= (uint16) ~KCTL_SIGNAL;      /* self-clearing */
            sim_debug (DBG_RING, &dks_dev,
                       "signal to БЭСМ-6 (ПРП6 on its side)\n");
            if (dks_testos != TESTOS_OFF)           /* the stub plays the ОС:
                                                       it must see the new
                                                       АБУФS entries now */
                sim_activate (&dks_unit, 20);
            dks_sig_besm6 (0);                      /* and/or the БЭСМ-6 */
            }
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

    if (uptr->flags & UNIT_ATT) {
        while ((ln = tmxr_poll_conn (&dks_desc)) >= 0) {/* new connections */
            if (ln < dks_desc.lines)
                dks_line_state (ln + 1, TRUE);
            }
        for (n = 1; n <= dks_desc.lines; ++n) {         /* dropped connections */
            if (dks_ldsc[n - 1].conn == 0 &&
                (dks_lstat[(n - 1) / 12] & (1 << ((n - 1) % 12))))
                dks_line_state (n, FALSE);
            }
        tmxr_poll_rx (&dks_desc);                       /* poll input */
        for (n = 1; n <= dks_desc.lines; ++n) {
            while ((c = tmxr_getc_ln (&dks_ldsc[n - 1])) != 0) {
                if (c & SCPE_BREAK)
                    continue;
                c &= 0377;
                if (dks_utf8) {                         /* stage C, B5 */
                    int k;

                    if (utf8_need[n]) {                 /* continuation */
                        if ((c & 0300) != 0200) {       /* broken: drop */
                            utf8_need[n] = 0;
                            continue;
                            }
                        utf8_acc[n] = (utf8_acc[n] << 6) | (c & 077);
                        if (--utf8_need[n])
                            continue;
                        }
                    else if (c < 0200) {                /* plain ASCII */
                        dks_rx_char (n, c);
                        continue;
                        }
                    else if ((c & 0340) == 0300) {      /* 2 bytes */
                        utf8_acc[n] = c & 037;
                        utf8_need[n] = 1;
                        continue;
                        }
                    else if ((c & 0360) == 0340) {      /* 3 bytes */
                        utf8_acc[n] = c & 017;
                        utf8_need[n] = 2;
                        continue;
                        }
                    else {                              /* 4 bytes */
                        utf8_acc[n] = c & 007;
                        utf8_need[n] = 3;
                        continue;
                        }
                    k = uni_to_koi7 (utf8_acc[n]);      /* complete */
                    if (k >= 0)
                        dks_rx_char (n, k);
                    continue;
                    }
                dks_rx_char (n, c);
                }
            }
        tmxr_poll_tx (&dks_desc);                       /* poll output */
        }
    /* The ОС-side stub runs with both engines: in HOST (stage B1) it is
       the only counterpart of the firmware. */
    if (dks_testos != TESTOS_OFF)
        testos_process ();
    if (dks_engine == ENGINE_SIM)                       /* stage A: engine in C */
        dks_process ();
    dks_bridge_poll ();                                 /* stage B2 */
    sim_activate (uptr, dks_wait);
    return SCPE_OK;
    }

/* Reset, attach, detach. */

t_stat dks_reset (DEVICE *dptr)
{
    int n;

    dks_desc.dptr = dptr;
    dks_desc.uptr = &dks_unit;
    memset (e60_mem, 0, sizeof (e60_mem));
    cur_chan = dks_chan;
    dks_kctl = dks_chan;
    memset (utf8_acc, 0, sizeof (utf8_acc));
    memset (utf8_need, 0, sizeof (utf8_need));
    sys_busy = 0;
    sys_line = 0;
    bufh_recv = 0;
    bufh_bell = 0;
    os_polled = (dks_testos != TESTOS_OFF);         /* TESTOS emulates the ОС */
    memset (testos_term_used, 0, sizeof (testos_term_used));
    dks_bridge_close (FALSE);                       /* drop the client, keep listening */
    for (n = 1; n <= DKS_LINES; ++n) {              /* keep attached lines up */
        int up = dks_line[n].up || dks_line[n].want_connect;

        memset (&dks_line[n], 0, sizeof (dks_line[n]));
        dks_line[n].echo = 1;
        dks_line[n].want_connect = up;
        }
    dks_mclr ();
    sim_cancel (&dks_unit);
    if ((dks_unit.flags & UNIT_ATT) || dks_lsock != INVALID_SOCKET)
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

/* SET CHAN=k: the КРК channel of this Э-60 instance (0-3).  Several
   Э-60 simulators (stage C multi-channel) each claim their own channel;
   the default is 2, the channel of the reference ДКС. */
t_stat dks_set_chan (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    t_stat r;
    int32 k;

    if (cptr == NULL || *cptr == 0)
        return SCPE_ARG;
    k = (int32) get_uint (cptr, 8, 3, &r);
    if (r != SCPE_OK)
        return r;
    dks_chan = k;
    cur_chan = k;
    return SCPE_OK;
    }

t_stat dks_show_chan (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "chan=%o", dks_chan);
    return SCPE_OK;
    }

/* SET ENCODING=KOI7|UTF8 (stage C, question B5): UTF8 converts the
   КОИ-7Н1 Cyrillic half to UTF-8 on output and back on input, for
   modern telnet clients.  The default KOI7 passes the raw protocol
   bytes (the behavior of the etalon). */
t_stat dks_set_encoding (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    char gbuf[CBUFSIZE];

    if (cptr == NULL)
        return SCPE_ARG;
    cptr = get_glyph (cptr, gbuf, 0);
    if (strcmp (gbuf, "KOI7") == 0)
        dks_utf8 = 0;
    else if (strcmp (gbuf, "UTF8") == 0)
        dks_utf8 = 1;
    else
        return SCPE_ARG;
    return SCPE_OK;
    }

t_stat dks_show_encoding (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "encoding=%s", dks_utf8? "UTF8": "KOI7");
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
fprintf (st, "   sim> SET DKS TESTOS=OFF|ON|PROMPT    OS-side stub (stage A)\n");
fprintf (st, "   sim> SET DKS BRIDGE=port             КАДОПАМ bridge server (stage B2)\n");
fprintf (st, "   sim> SET DKS NOBRIDGE                close the bridge\n");
fprintf (st, "   sim> SET DKS CHAN=k                  the КРК channel of this Э-60, 0-3 (stage C)\n");
fprintf (st, "   sim> SET DKS ENCODING=KOI7|UTF8      terminal encoding (stage C)\n\n");
fprintf (st, "The firmware reaches the 32K-word ОЗУ of the Э-60 through the KADDR/KDATA\n");
fprintf (st, "window (bit 0100000 of KADDR is auto-increment); cell 077777 written with\n");
fprintf (st, "0400 is the doorbell from the БЭСМ-6.  Writing KCTL with 0400|chan pulses\n");
fprintf (st, "the «signal БЭСМ-6» line (ПРП6 on its side, the bridge sends an ATTN\n");
fprintf (st, "frame).  Characters are КОИ-7Н1 (7 bits); ENCODING=UTF8 spells the\n");
fprintf (st, "Cyrillic half as UTF-8 for modern telnet clients.\n\n");
fprintf (st, "The bridge (stage B2) exposes the ОЗУ Э-60 to a БЭСМ-6 simulator over\n");
fprintf (st, "TCP: 8-byte frames, block transfers with the BULK_READ/BULK_WRITE\n");
fprintf (st, "opcodes 05/06 (stage C); see PDP11/pdp11_dks.md §«КАДОПАМ bridge».\n");
fprintf (st, "The БЭСМ-6 side connects with SET DKS REMOTE=host:port and may serve\n");
fprintf (st, "several Э-60 with SET DKS CHANS=n (stage C, КРК channels 2..2+n-1,\n");
fprintf (st, "ports p..p+n-1); a system request raises ПРП12 on the БЭСМ-6.\n\n");
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

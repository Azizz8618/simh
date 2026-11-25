/* e10025_chan.c: unified channel (Единый канал) model for Электроника-100-25
   Based on ИЧМ2.791.003 ТО2 ("Единый канал"): 56-line asynchronous bus,
   address/data/control phases, СА/СП handshake, 18-bit address space,
   BR7..BR4 arbitration and ЗНДК block transfer. Simh device/DIB mechanics are
   reused from PDP11/pdp11_io.c and pdp11_stddev.c (MIT license).

   Copyright (c) 1993-2022, Robert M Supnik, Mark Pizzolato and others
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
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "e10025_defs.h"

extern t_stat iopageR (int32 *data, uint32 addr, int32 access);
extern t_stat iopageW (int32 data, uint32 addr, int32 access);
extern uint16 *M;
extern int32 int_req[IPL_HLVL];
extern int32 int_vec[IPL_HLVL][32];
extern int32 (*int_ack[IPL_HLVL][32]) (void);
extern int32 ipl;
extern int32 PSW;

static t_stat chan_set_br_req (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat chan_set_br_vec (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_rd (int32 *data, int32 pa, int32 access);
static t_stat dma_wr (int32 data, int32 pa, int32 access);
static t_stat dma_go (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_set_dat (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_set_ap (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_set_wc (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_go (UNIT *uptr, int32 val, CONST char *cptr, void *desc);

/* Channel cycle phases, book 4 sections 6.2-6.6 (PDF pp.25-31).
   Real delays (150/75/75/75 ns, 15 us timeout) are abstracted as states. */
enum {
    CH_IDLE = 0, CH_ADDRESS = 1, CH_SA = 2, CH_WAIT_SACK = 3,
    CH_STROBE = 4, CH_RELEASE = 5, CH_CLEANUP = 6, CH_ERROR = 7
    };

#define CH_OP_IN      0                                 /* ВВОД */
#define CH_OP_OUT     1                                 /* ВЫВОД */
#define CH_BRS        0370                              /* BR request bits 7..4 */
#define BR_INT        (1u << 31)                        /* synthetic BR slot, never bit 1 */

static struct {
    int32 state;
    int32 addr;
    int32 data;
    int32 op;
    int32 cycles;
    int32 errors;
    int32 timeouts;
    int32 last_op;
    } chan;

static int32 br_req = 0;                                /* synthetic BR7..BR4 */
static int32 br_grant = 0;
static int32 br_vec = 0060;

/* Run one complete channel cycle. Returns data for input cycles. */
static int32 chan_run_cycle (int32 pa, int32 op, int32 data, int32 byte)
{
int32 result = 0, stat = SCPE_OK;

uint32 bank = ((uint32) pa) & ~((uint32) CH_ADDRMASK);
chan.cycles++;
chan.last_op = op;
chan.addr = pa & CH_ADDRMASK;
chan.data = data & DMASK;
chan.state = CH_ADDRESS;                                /* address + opcode on bus */
if ((bank != 0) && (bank != (uint32) CH_IOBANK)) {
    chan.state = CH_ERROR;
    chan.errors++;
    return 0;
    }
chan.state = CH_SA;                                     /* СА asserted */
chan.state = CH_WAIT_SACK;                              /* wait for СП */
if (chan.addr >= CH_IOBASE) {
    uint32 pa22 = ((uint32) chan.addr) | ((uint32) CH_IOBANK);
    if (op == CH_OP_IN) {
        stat = iopageR (&result, pa22, READ);
        if ((stat == SCPE_OK) && byte)
            result = (chan.addr & 1)? ((result >> 8) & 0377): (result & 0377);
        }
    else
        stat = iopageW (data & (byte? 0377: DMASK), pa22,
                        byte? WRITEB: WRITE);
    }
else if ((uint32) chan.addr < MEMSIZE) {
    int32 idx = chan.addr >> 1;
    if (byte) {
        if (op == CH_OP_IN)
            result = (chan.addr & 1) ? ((M[idx] >> 8) & 0377) : (M[idx] & 0377);
        else if (chan.addr & 1)
            M[idx] = (M[idx] & 0377) | ((data & 0377) << 8);
        else
            M[idx] = (M[idx] & 0177400) | (data & 0377);
        }
    else {
        if (op == CH_OP_IN)
            result = M[idx];
        else
            M[idx] = data & DMASK;
        }
    }
else
    stat = SCPE_NXM;
if (stat != SCPE_OK) {
    chan.timeouts++;
    chan.errors++;
    chan.state = CH_ERROR;
    return 0;
    }
chan.state = CH_STROBE;                                 /* strobe data, drop СА */
chan.state = CH_RELEASE;                                /* hold address 75 ns */
chan.state = CH_CLEANUP;                                /* passive drops СП */
chan.state = CH_IDLE;
chan.data = result & DMASK;
return result;
}

int32 chan_read_io (int32 pa, int32 byte)
{
return chan_run_cycle (pa, CH_OP_IN, 0, byte);
}

void chan_write_io (int32 pa, int32 data, int32 byte)
{
chan_run_cycle (pa, CH_OP_OUT, data, byte);
}

static t_stat br_inta4 (void), br_inta5 (void), br_inta6 (void), br_inta7 (void);

static int32 (*const br_inta_tab[8]) (void) = {
    NULL, NULL, NULL, NULL, &br_inta4, &br_inta5, &br_inta6, &br_inta7
    };

static t_stat br_ack (int32 level)
{
int_req[level] &= ~(int32) BR_INT;
br_req &= ~(1 << level);                                /* BR line released after grant */
return br_vec;
}

static t_stat br_inta4 (void) { return br_ack (4); }
static t_stat br_inta5 (void) { return br_ack (5); }
static t_stat br_inta6 (void) { return br_ack (6); }
static t_stat br_inta7 (void) { return br_ack (7); }

/* Level-only arbitration: the daisy chain inside a BR level, grant propagation
   and sub-priority order are not modelled; each level has one synthetic source.
   Requests above the real PSW ipl win. Slot 31 keeps BR clear of DMA [7][1],
   CLK [6][1], TTI [4][1] and PIRQ7 [7][0]. */
void br_arbitrate (void)
{
int32 level;

for (level = 4; level <= 7; level++)
    int_req[level] &= ~(int32) BR_INT;                  /* drop stale grants */
for (level = 7; level >= 4; level--) {
    if ((br_req & (1 << level)) && (level > ipl)) {
        br_grant = level;
        int_vec[level][31] = br_vec;
        int_ack[level][31] = br_inta_tab[level];
        int_req[level] |= (int32) BR_INT;
        break;
        }
    }
}

void chan_ack_interrupt (int32 vec)
{
int32 level, bit;
for (level = 0; level < IPL_HLVL; level++)
    for (bit = 0; bit < 32; bit++)
        if ((int_vec[level][bit] == vec) && (int_ack[level][bit] != NULL))
            int_ack[level][bit] ();
}

/* Channel control device: no bus registers, only simh-visible state. */
static UNIT chan_unit = { UDATA (NULL, 0, 0) };

static REG chan_reg[] = {
    { DRDATAD (STATE, chan.state, 8, "channel phase (0=IDLE, 7=ERROR)") },
    { DRDATAD (ADDR, chan.addr, 18, "last channel address") },
    { DRDATAD (DATA, chan.data, 16, "last channel data") },
    { DRDATAD (CYCLES, chan.cycles, 32, "channel cycles executed") },
    { DRDATAD (ERRORS, chan.errors, 32, "channel errors") },
    { DRDATAD (TIMEOUT, chan.timeouts, 32, "no-СП timeouts") },
    { DRDATAD (BRREQ, br_req, 8, "synthetic BR7..BR4 requests") },
    { DRDATAD (BRGRANT, br_grant, 8, "granted BR level") },
    { DRDATAD (BRVEC, br_vec, 16, "synthetic BR vector") },
    { NULL }
    };

static MTAB chan_mod[] = {
    { MTAB_XTD|MTAB_VDV, 0, "BRREQ", "BRREQ",
      &chan_set_br_req, NULL, NULL, "inject BR7..BR4 request" },
    { MTAB_XTD|MTAB_VDV, 0, "BRVEC", "BRVEC",
      &chan_set_br_vec, NULL, NULL, "synthetic BR vector" },
    { 0 }
    };

static t_stat chan_reset (DEVICE *dptr)
{
int32 level;

memset (&chan, 0, sizeof (chan));
br_req = br_grant = 0;
br_vec = 0060;
for (level = 4; level <= 7; level++)
    int_req[level] &= ~(int32) BR_INT;
return SCPE_OK;
}

DEVICE chan_dev = {
    "CHAN", &chan_unit, chan_reg, chan_mod,
    1, 10, 18, 0, 8, 8,
    NULL, NULL, &chan_reset,
    NULL, NULL, NULL, NULL,
    0, 0, NULL,
    NULL, NULL, NULL, NULL, NULL
    };

/* DMA (НДК) device. Register layout follows book 4 table 4, PDF p.44:
   N=РСУ, N+2=РД, N+4=РАП, N+6=РСС. */
static UNIT dma_unit = { UDATA (NULL, 0, 0) };
static int32 dma_busy = 0, dma_done = 0, dma_error = 0;
static int32 dma_csr = 0, dma_dat = 0, dma_ap = 0, dma_wc = 0;
static DIB dma_dib;

static t_stat dma_inta (void)
{
int_req[7] &= ~(1u << 1);                               /* DMA bit, not PIRQ7 or BR */
return dma_dib.vec;
}

static DIB dma_dib = {
    IOPAGEBASE + 017500, 010, &dma_rd, &dma_wr,
    1, 7*32+1, 0070, { &dma_inta }, 0, 0, NULL, NULL
    };

static t_stat dma_rd (int32 *data, int32 pa, int32 access)
{
int32 off = ((pa & IOPAGEMASK) - (dma_dib.ba & IOPAGEMASK)) / 2;
switch (off) {
case 0:                                                 /* РСУ */
    *data = (dma_csr & 0177076) | (dma_done ? 0200 : 0) |
            (dma_error ? 0100000 : 0) | (dma_busy ? 0100 : 0);
    break;
case 1: *data = dma_dat & DMASK; break;                  /* РД */
case 2: *data = dma_ap & DMASK; break;                   /* РАП */
case 3: *data = dma_wc & DMASK; break;                   /* РСС */
default: return SCPE_NXM;
    }
return SCPE_OK;
}

static t_stat dma_wr (int32 data, int32 pa, int32 access)
{
int32 off = ((pa & IOPAGEMASK) - (dma_dib.ba & IOPAGEMASK)) / 2;
switch (off) {
case 0:                                                 /* РСУ, bit0 = пуск */
    dma_csr = data & DMASK;
    if (data & 1)
        dma_go (&dma_unit, 0, NULL, NULL);
    break;
case 1: dma_dat = data & DMASK; break;
case 2: dma_ap = data & DMASK; break;
case 3: dma_wc = data & DMASK; break;
default: return SCPE_NXM;
    }
return SCPE_OK;
}

static t_stat dma_reset (DEVICE *dptr)
{
dma_busy = dma_done = dma_error = 0;
dma_csr = dma_dat = dma_ap = dma_wc = 0;
return SCPE_OK;
}

static REG dma_reg[] = {
    { HRDATAD (CSR, dma_csr, 16, "control/status register") },
    { HRDATAD (DAT, dma_dat, 16, "data register") },
    { HRDATAD (AP, dma_ap, 18, "current address register") },
    { HRDATAD (WC, dma_wc, 16, "word counter") },
    { FLDATAD (DONE, dma_done, 0, "transfer complete") },
    { NULL }
    };

static MTAB dma_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 010, "ADDRESS", "ADDRESS",
      &set_addr, &show_addr, NULL, "Channel address" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "VECTOR", "VECTOR",
      &set_vec, &show_vec, NULL, "Interrupt vector" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "DATA", "DATA",
      &dma_set_dat, NULL, NULL, "DMA data word" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "AP", "AP",
      &dma_set_ap, NULL, NULL, "DMA current address" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "WC", "WC",
      &dma_set_wc, NULL, NULL, "DMA word count" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "GO", "GO",
      &dma_go, NULL, NULL, "start DMA transfer" },
    { 0 }
    };

DEVICE dma_dev = {
    "DMA", &dma_unit, dma_reg, dma_mod,
    1, 10, 18, 2, 8, 8,
    NULL, NULL, &dma_reset,
    NULL, NULL, NULL,
    &dma_dib, DEV_UBUS, 0, NULL,
    NULL, NULL, NULL, NULL, NULL
    };

static t_stat dma_set_dat (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
if (cptr == NULL)
    return SCPE_ARG;
dma_dat = get_uint (cptr, DEV_RDX, DMASK, &r) & DMASK;
return r;
}

static t_stat dma_set_ap (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
if (cptr == NULL)
    return SCPE_ARG;
dma_ap = get_uint (cptr, DEV_RDX, CH_ADDRMASK, &r) & CH_ADDRMASK;
return r;
}

static t_stat dma_set_wc (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
if (cptr == NULL)
    return SCPE_ARG;
dma_wc = get_uint (cptr, DEV_RDX, DMASK, &r) & DMASK;
return r;
}

static t_stat dma_go (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
int32 before;
dma_error = dma_done = 0;
dma_busy = 1;
before = chan.errors;
while (dma_wc > 0) {
    chan_run_cycle (dma_ap, CH_OP_OUT, dma_dat, 0);
    if (chan.errors != before) {
        dma_error = 1;
        break;
        }
    dma_ap = (dma_ap + 2) & CH_ADDRMASK;
    dma_wc--;
    }
dma_busy = 0;
dma_done = 1;
dma_csr |= 0200;
if (dma_error)
    dma_csr |= 0100000;
/* Vector and ack come from dma_dib at [7][1]; leave PIRQ7 [7][0] alone. */
int_req[7] |= (1u << 1);
return SCPE_OK;
}

/* Expose channel phase to simh's BRREQ register writes. */
static t_stat chan_set_br_req (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
if (cptr == NULL)
    return SCPE_ARG;
br_req = get_uint (cptr, DEV_RDX, CH_BRS, &r) & CH_BRS;
br_grant = 0;                                           /* new injection resets the diagnostic */
br_arbitrate ();
return r;
}

static t_stat chan_set_br_vec (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
if (cptr == NULL)
    return SCPE_ARG;
br_vec = get_uint (cptr, DEV_RDX, DMASK, &r) & DMASK;
return r;
}

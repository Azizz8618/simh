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
extern DEVICE chan_dev;
static UNIT chan_unit;                                  /* defined below, used by chan_run_cycle */

static t_stat chan_set_br_req (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat chan_set_br_vec (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat chan_set_br_dev (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat chan_set_timing (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat chan_show_timing (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
static t_stat chan_set_nosp (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat chan_show_nosp (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
static t_stat chan_service (UNIT *uptr);
static t_stat dma_rd (int32 *data, int32 pa, int32 access);
static t_stat dma_wr (int32 data, int32 pa, int32 access);
static t_stat dma_go (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_set_dat (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_set_ap (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat dma_set_wc (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
static t_stat chan_set_cycle (UNIT *uptr, int32 val, CONST char *cptr, void *desc);

/* Channel cycle phases, book 4 sections 6.2-6.6 (PDF pp.25-31).
   The numbers 150/75/75/75 ns, 450 ns repeat, 525 ns full cycle and the
   15 us timeout are historical book-4 durations. They are logical values in
   this model: not host delays and not simulator clock ticks. */
enum {
    CH_IDLE = 0, CH_ADDRESS = 1, CH_SA = 2, CH_WAIT_SACK = 3,
    CH_STROBE = 4, CH_RELEASE = 5, CH_CLEANUP = 6, CH_ERROR = 7
    };

#define CH_OP_IN      00                                /* ВВОД, word input */
#define CH_OP_INP     01                                /* ВВОДП, journaled only */
#define CH_OP_OUT     010                               /* ВЫВОД, word output */
#define CH_OP_OUTB    011                               /* ВЫВОДБ, ADR00 byte output */
#define CH_BRS        0370                              /* BR request bits 7..4 */
#define BR_INT        (1u << 31)                        /* synthetic BR slot, never bit 1 */
#define CH_DEB_JOURNAL 1                                /* phase and opcode journal */
#define CH_JPHASES    8                                 /* seven phases plus ERROR */
#define CH_JCYCLES    8                                 /* last cycles kept in journal */

static struct {
    int32 state;
    int32 addr;
    int32 data;
    int32 op;
    int32 cycles;
    int32 errors;
    int32 timeouts;
    int32 last_op;
    int32 adr00;                                        /* ADR00 byte select */
    } chan;

/* Journal of the last cycles. The ВВОДП pair is recorded as diagnostic
   information only: no strict next-cycle check is enforced by default. */
static struct {
    int32 op;
    int32 addr;
    int32 adr00;
    int32 nphase;
    int32 phase[CH_JPHASES];
    int32 ok;
    int32 next_op;
    int32 next_addr;
    } chan_journal[CH_JCYCLES];
static int32 chan_jnext = 0;
static int32 chan_jused = 0;

/* Phase B timing mode. Logical is the default and keeps the synchronous
   deterministic cycle. Scaled uses a declared tick of 75 ns. */
static int32 chan_timing = CH_TIMING_LOGICAL;
static int32 chan_nosp = 0;
static int32 chan_event_is_timeout = 0;
static int32 chan_phase_step = 0;

int32 chan_timing_mode (void)
{
return chan_timing;
}

static DEBTAB chan_debug[] = {
    {"JOURNAL", CH_DEB_JOURNAL, "channel phase and opcode journal"},
    {0}
    };

/* Ordered BR source list for each level. ПК passes the chain from slot 0; the
   first requesting source blocks it (book 4, p.32 section 7.1). Slot 0 is the
   legacy source driven by BRREQ and BRVEC; BRDEV injects further slots. */
typedef struct {
    int32 slot;                                         /* chain position, 0 = nearest */
    int32 vec;                                          /* interrupt vector */
    int32 req;                                          /* request held on the BR line */
    } BR_SRC;

static BR_SRC br_src[IPL_HLVL][CH_BR_MAXSRC];           /* indexed by level 4..7 */
static int32 br_nsrc[IPL_HLVL];
static int32 br_active[IPL_HLVL];                       /* source currently holding ПК */
static int32 br_req = 0;                                /* synthetic BR7..BR4 */
static int32 br_grant = 0;
static int32 br_vec = 0060;

static void br_src_reset (void)
{
int32 level;

for (level = 4; level <= 7; level++) {
    br_nsrc[level] = 1;
    br_active[level] = -1;
    memset (&br_src[level][0], 0, sizeof (br_src[level][0]));
    br_src[level][0].slot = 0;
    br_src[level][0].vec = br_vec;
    }
}

static const char *chan_phase_name (int32 phase)
{
static const char *const names[8] = {
    "IDLE", "ADDRESS", "SA", "WAIT_SACK", "STROBE", "RELEASE", "CLEANUP", "ERROR"
    };
return ((phase >= 0) && (phase < 8))? names[phase]: "UNKNOWN";
}

/* Store one completed cycle and, when debugging is enabled, print it. */
static void chan_journal_record (int32 *phase, int32 nphase, int32 ok)
{
char phases[128] = "";
const char *pair = "--";
int32 cur = 0, i, j;
int32 newest = (chan_jnext + CH_JCYCLES - 1) % CH_JCYCLES;

if ((chan_jused > 0) && (chan_journal[newest].op == CH_OP_INP) &&
    (chan_journal[newest].next_op < 0)) {
    chan_journal[newest].next_op = chan.last_op;
    chan_journal[newest].next_addr = chan.addr;
    sim_debug (CH_DEB_JOURNAL, &chan_dev,
               "vbo_pair op=01 addr=%06o next=%02o@%06o "
               "(diagnostic only, not enforced)\n",
               chan_journal[newest].addr, chan_journal[newest].next_op,
               chan_journal[newest].next_addr);
    }
j = 0;
for (i = 0; i < nphase; i++) {
    const char *name = chan_phase_name (phase[i]);
    int32 len = strlen (name) + ((i > 0)? 1: 0);
    if ((j + len + 1) < (int32) sizeof (phases)) {
        if (i > 0)
            phases[j++] = ',';
        strcpy (&phases[j], name);
        j += strlen (name);
        }
    }
chan_journal[chan_jnext].op = chan.last_op;
chan_journal[chan_jnext].addr = chan.addr;
chan_journal[chan_jnext].adr00 = chan.adr00;
chan_journal[chan_jnext].nphase = nphase;
chan_journal[chan_jnext].ok = ok;
chan_journal[chan_jnext].next_op = -1;
chan_journal[chan_jnext].next_addr = 0;
for (i = 0; i < nphase; i++)
    chan_journal[chan_jnext].phase[i] = phase[i];
if (chan_journal[chan_jnext].op == CH_OP_INP)
    pair = "pending";                                   /* soft pair, never enforced */
chan_jnext = (chan_jnext + 1) % CH_JCYCLES;
if (chan_jused < CH_JCYCLES)
    chan_jused++;
cur = (chan_jnext + CH_JCYCLES - 1) % CH_JCYCLES;
sim_debug (CH_DEB_JOURNAL, &chan_dev,
           "cycle op=%02o addr=%06o adr00=%d phases=%s result=%s vbo_next=%s "
           "timing=%s scale_ns=%d historical_book4_ns=150/75/75/75 "
           "repeat=450 full=525 timeout_us=15000 "
           "(book-4 historical values, not host ns or simulator clocks)\n",
           chan_journal[cur].op, chan_journal[cur].addr,
           chan_journal[cur].adr00, phases, ok? "OK": "ERROR", pair,
           (chan_timing == CH_TIMING_SCALED)? "scaled": "logical", CH_SCALE_NS);
}

#define CH_PHASE(p) do { chan.state = (p); \
                         if (nphase < CH_JPHASES) phase[nphase++] = (p); \
                         } while (0)

/* Run one complete channel cycle. Returns data for input cycles. */
static int32 chan_run_cycle (int32 pa, int32 op, int32 data, int32 byte)
{
int32 result = 0, stat = SCPE_OK;
int32 phase[CH_JPHASES], nphase = 0;
int32 is_byte = byte || (op == CH_OP_OUTB);
int32 is_input = (op == CH_OP_IN) || (op == CH_OP_INP);
int32 scaled = (chan_timing == CH_TIMING_SCALED);
uint32 bank = ((uint32) pa) & ~((uint32) CH_ADDRMASK);

chan.cycles++;
chan.last_op = op;
chan.op = op;
chan.addr = pa & CH_ADDRMASK;
chan.adr00 = chan.addr & 1;
chan.data = data & DMASK;
CH_PHASE (CH_ADDRESS);                                  /* address + opcode on bus */
if ((bank != 0) && (bank != (uint32) CH_IOBANK)) {
    CH_PHASE (CH_ERROR);
    chan.errors++;
    chan_journal_record (phase, nphase, 0);
    return 0;
    }
CH_PHASE (CH_SA);                                       /* СА asserted */
if (scaled) {
    chan_event_is_timeout = 1;
    sim_activate_after (&chan_unit, CH_TIMEOUT_US);     /* 15 us, book 4 */
    }
CH_PHASE (CH_WAIT_SACK);                                /* wait for СП */
if (chan.addr >= CH_IOBASE) {
    uint32 pa22 = ((uint32) chan.addr) | ((uint32) CH_IOBANK);
    if (is_input) {
        stat = iopageR (&result, pa22, READ);
        if ((stat == SCPE_OK) && is_byte)
            result = chan.adr00? ((result >> 8) & 0377): (result & 0377);
        }
    else if (is_byte && chan.adr00 && (op == CH_OP_OUTB)) {
        int32 cur = 0;
        stat = iopageR (&cur, pa22 & ~1, READ);         /* merge high byte by АDR00 */
        if (stat == SCPE_OK) {
            cur = (cur & 0377) | ((data & 0377) << 8);
            stat = iopageW (cur, pa22 & ~1, WRITE);
            }
        }
    else
        stat = iopageW (data & (is_byte? 0377: DMASK), pa22,
                        is_byte? WRITEB: WRITE);
    }
else if ((uint32) chan.addr < MEMSIZE) {
    int32 idx = chan.addr >> 1;
    if (is_byte) {
        if (is_input)
            result = chan.adr00? ((M[idx] >> 8) & 0377): (M[idx] & 0377);
        else if (chan.adr00)
            M[idx] = (M[idx] & 0377) | ((data & 0377) << 8);
        else
            M[idx] = (M[idx] & 0177400) | (data & 0377);
        }
    else {
        if (is_input)
            result = M[idx];
        else
            M[idx] = data & DMASK;
        }
    }
else
    stat = SCPE_NXM;
if (stat != SCPE_OK) {
    if (scaled)                                         /* NXM is not a no-СП timeout */
        sim_cancel (&chan_unit);
    else
        chan.timeouts++;                                /* inherited NXM diagnostic */
    chan_event_is_timeout = 0;
    chan.errors++;
    CH_PHASE (CH_ERROR);
    chan_journal_record (phase, nphase, 0);
    return 0;
    }
if (scaled) {
    if (chan_nosp) {
        /* СП подавлен искусственно: ответа нет, событие таймаута остаётся. */
        chan.data = result & DMASK;
        CH_PHASE (CH_ERROR);
        chan.state = CH_WAIT_SACK;                      /* visible until timeout */
        chan_phase_step = 0;
        chan_journal_record (phase, nphase, 0);
        return result;
        }
    sim_cancel (&chan_unit);                            /* К СП Н arrived */
    chan_event_is_timeout = 0;
    }
CH_PHASE (CH_STROBE);                                   /* strobe data, drop СА */
CH_PHASE (CH_RELEASE);                                  /* hold address 75 ns */
CH_PHASE (CH_CLEANUP);                                  /* passive drops СП */
CH_PHASE (CH_IDLE);
chan.data = result & DMASK;
chan_journal_record (phase, nphase, 1);
if (scaled) {
    chan.state = CH_ADDRESS;
    chan_phase_step = 0;
    sim_cancel (&chan_unit);                            /* restart the phase chain */
    sim_activate (&chan_unit, 2);                       /* 150 ns, one tick = 75 ns */
    }
return result;
}

#undef CH_PHASE

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
int32 i, vec = br_vec;

if (br_active[level] < 0)
    return br_vec;                                      /* duplicate acknowledge */
for (i = 0; i < br_nsrc[level]; i++) {
    if (i == br_active[level]) {
        br_src[level][i].req = 0;                       /* grant released after ПВ */
        vec = br_src[level][i].vec;
        if (i == 0)
            br_req &= ~(1 << level);                    /* legacy BRREQ source */
        break;
        }
    }
br_active[level] = -1;
int_req[level] &= ~(int32) BR_INT;
return vec;
}

static t_stat br_inta4 (void) { return br_ack (4); }
static t_stat br_inta5 (void) { return br_ack (5); }
static t_stat br_inta6 (void) { return br_ack (6); }
static t_stat br_inta7 (void) { return br_ack (7); }

/* Grant propagation inside a BR level. ПК walks the ordered source list from
   the processor outwards; a source without a request passes it on, the first
   requesting source blocks it and takes the grant. Its acknowledge clears the
   request, so the next arbitration serves the following source. Requests above
   the real PSW ipl win. Slot 31 keeps BR clear of DMA [7][1], CLK [6][1],
   TTI [4][1] and PIRQ7 [7][0]. The list order is diagnostic only: book 4 gives
   the daisy-chain rule, not the historical device order on each line. */
void br_arbitrate (void)
{
int32 level, i;

for (level = 4; level <= 7; level++) {
    int_req[level] &= ~(int32) BR_INT;                  /* drop stale grants */
    br_active[level] = -1;
    }
for (level = 7; level >= 4; level--) {
    if (level <= ipl)
        continue;
    for (i = 0; i < br_nsrc[level]; i++) {
        if (!br_src[level][i].req)
            continue;                                   /* ПК passes this source */
        br_grant = level;
        br_active[level] = i;
        int_vec[level][31] = br_src[level][i].vec;
        int_ack[level][31] = br_inta_tab[level];
        int_req[level] |= (int32) BR_INT;
        return;
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

/* Scaled-mode event handler. The phase chain and the no-СП timeout share one
   unit but are never queued at the same time. */
static t_stat chan_service (UNIT *uptr)
{
static const int32 phase[] = { CH_SA, CH_WAIT_SACK, CH_STROBE,
                              CH_RELEASE, CH_CLEANUP, CH_IDLE };
static const int32 tick[6] = { 1, 1, 1, 1, 1, 1 };     /* 75 ns per tick */

if (chan_event_is_timeout) {
    chan_event_is_timeout = 0;
    chan.timeouts++;
    chan.state = CH_ERROR;
    chan_phase_step = 0;
    return SCPE_OK;
    }
if ((chan_timing == CH_TIMING_SCALED) && (chan_phase_step < 6)) {
    chan.state = phase[chan_phase_step];
    sim_activate (uptr, tick[chan_phase_step]);
    chan_phase_step++;
    }
return SCPE_OK;
}

/* Channel control device: no bus registers, only simh-visible state. */
static UNIT chan_unit = { UDATA (&chan_service, 0, 0) };

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
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "BRDEV", "BRDEV",
      &chan_set_br_dev, NULL, NULL, "inject BR source level:slot:vector (octal)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "CYCLE", "CYCLE",
      &chan_set_cycle, NULL, NULL, "cycle opcode:address[:data] (octal)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "TIMING", "TIMING={logical|scaled}",
      &chan_set_timing, &chan_show_timing, NULL, "channel timing mode" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "NOSP", "NOSP={0|1}",
      &chan_set_nosp, &chan_show_nosp, NULL, "suppress СП for timeout diagnostics" },
    { 0 }
    };

static t_stat chan_reset (DEVICE *dptr)
{
int32 level;

memset (&chan, 0, sizeof (chan));
memset (&chan_journal, 0, sizeof (chan_journal));
chan_jnext = chan_jused = 0;
sim_cancel (&chan_unit);
chan_event_is_timeout = chan_phase_step = 0;
br_req = br_grant = 0;
br_vec = 0060;
br_src_reset ();
for (level = 4; level <= 7; level++)
    int_req[level] &= ~(int32) BR_INT;
return SCPE_OK;
}

DEVICE chan_dev = {
    "CHAN", &chan_unit, chan_reg, chan_mod,
    1, 10, 18, 0, 8, 8,
    NULL, NULL, &chan_reset,
    NULL, NULL, NULL, NULL,
    DEV_DEBUG, 0, chan_debug,
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

/* Inject one channel cycle with a book-4 opcode (00/01/10/11). Address and
   data are octal; codes 10/11 use the optional data argument. */
static t_stat chan_set_cycle (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
int32 op = -1, addr = 0, data = 0, n;

if (cptr == NULL)
    return SCPE_ARG;
n = sscanf (cptr, "%o:%o:%o", &op, &addr, &data);
if ((n < 2) || ((op != CH_OP_IN) && (op != CH_OP_INP) &&
                (op != CH_OP_OUT) && (op != CH_OP_OUTB)))
    return sim_messagef (SCPE_ARG, "CHAN CYCLE expects opcode:address[:data] in octal (00/01/10/11)\n");
addr &= CH_ADDRMASK;
if (addr >= CH_IOBASE)
    addr |= (int32) CH_IOBANK;
chan_run_cycle (addr, op, data, (op == CH_OP_OUTB)? 1: 0);
return SCPE_OK;
}

/* Expose channel phase to simh's BRREQ register writes. BRREQ drives the
   legacy source in chain slot 0 on each level. */
static t_stat chan_set_br_req (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
int32 level;

if (cptr == NULL)
    return SCPE_ARG;
br_req = get_uint (cptr, DEV_RDX, CH_BRS, &r) & CH_BRS;
br_grant = 0;                                           /* new injection resets the diagnostic */
for (level = 4; level <= 7; level++)
    br_src[level][0].req = (br_req >> level) & 1;
br_arbitrate ();
return r;
}

static t_stat chan_set_br_vec (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
int32 level;

if (cptr == NULL)
    return SCPE_ARG;
br_vec = get_uint (cptr, DEV_RDX, DMASK, &r) & DMASK;
for (level = 4; level <= 7; level++)
    br_src[level][0].vec = br_vec;                      /* legacy source vector */
return r;
}

/* Diagnostic injection of one source on a BR daisy chain. Syntax:
   set CHAN BRDEV=<level>:<slot>:<vec>, all fields octal. <slot> is the chain
   position inside the level, 0 is closest to the processor. Injecting a slot
   again replaces its vector; the source list stays ordered by slot. */
static t_stat chan_set_br_dev (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
int32 level, slot, vec, i, n;

if (cptr == NULL)
    return SCPE_ARG;
n = sscanf (cptr, "%o:%o:%o", &level, &slot, &vec);
if ((n != 3) || (level < 4) || (level > 7) ||
    (slot < 0) || (slot >= CH_BR_MAXSRC) || (vec < 0) || (vec > DMASK))
    return sim_messagef (SCPE_ARG,
               "CHAN BRDEV expects level:slot:vector in octal (level 4..7, slot 0..7)\n");
for (i = 0; i < br_nsrc[level]; i++)
    if (br_src[level][i].slot == slot) {
        br_src[level][i].vec = vec;
        br_src[level][i].req = 1;                       /* injection holds the request */
        br_arbitrate ();
        return SCPE_OK;
        }
if (br_nsrc[level] >= CH_BR_MAXSRC)
    return sim_messagef (SCPE_ARG, "CHAN BRDEV source list is full for level %d\n", level);
for (i = br_nsrc[level]; i > 0; i--)                    /* keep the chain ordered */
    if (br_src[level][i - 1].slot > slot)
        br_src[level][i] = br_src[level][i - 1];
    else
        break;
br_src[level][i].slot = slot;
br_src[level][i].vec = vec;
br_src[level][i].req = 1;
br_nsrc[level]++;
br_arbitrate ();
return SCPE_OK;
}

/* Phase B modifiers. Logical is the default deterministic mode. */
static t_stat chan_set_timing (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
if (cptr == NULL)
    return SCPE_ARG;
if (strcasecmp (cptr, "logical") == 0)
    chan_timing = CH_TIMING_LOGICAL;
else if (strcasecmp (cptr, "scaled") == 0)
    chan_timing = CH_TIMING_SCALED;
else
    return sim_messagef (SCPE_ARG, "CHAN TIMING expects logical or scaled\n");
return SCPE_OK;
}

static t_stat chan_show_timing (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf (st, "TIMING=%s", (chan_timing == CH_TIMING_SCALED)? "scaled": "logical");
return SCPE_OK;
}

static t_stat chan_set_nosp (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
t_stat r;
if (cptr == NULL)
    return SCPE_ARG;
chan_nosp = get_uint (cptr, DEV_RDX, 1, &r) & 1;
return r;
}

static t_stat chan_show_nosp (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf (st, "NOSP=%d", chan_nosp);
return SCPE_OK;
}

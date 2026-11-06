/*
 * BESM-6 КАДОПАМ and the ДКС terminal concentrator
 *
 * Copyright (c) 2026, Leonid Broukhis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

 * Except as contained in this notice, the name of Leonid Broukhis shall not
 * be used in advertising or otherwise to promote the sale, use or other
 * dealings in this Software without prior written authorization from
 * Leonid Broukhis.
 */

/*
 * КАДОПАМ (канал программного доступа в оперативную память микро-ЭВМ) gives
 * the BESM-6 access to the RAM of the Электроника-60 machines of the ДКС:
 * instruction 032 reads a 16-bit word into bits 25-40 of the accumulator,
 * 0132 writes the low 16 bits.  Writing cell 0 selects the КРК channel;
 * reading it returns the channel status (bit 9+k = channel k present).
 *
 * One Э-60 is emulated: the OS's Э-60 index 2 on КРК channel 2, interrupting
 * on ПРП 6.  Only that index gets past the `ручной' check in СВЯЗЬ7.зпрос1.
 * The memory layout below is the one tabulated in svyaz7.be; see BESM6/ДКС.md
 * for the protocol, with references to the Диспак sources.
 *
 * Each telnet line set to "DKS" mode is an Э-60 line with local echo and line
 * editing.  A finished line is passed to the OS as one input array.
 */
#include "besm6_defs.h"
#include "sim_sock.h"
#include "sim_tmxr.h"

#define DKS_CHAN        2                       /* КРК channel of our Э-60 */
#define DKS_PRP         PRP_DKS_CHAN(DKS_CHAN)  /* its interrupt, ПРП 6 */
#define DKS_STATUS      (0400 << DKS_CHAN)      /* cell 0: channel present */

/* Э-60 index 2 layout (svyaz7.be tables абуфs, абуфh, абфосs, абфосh, адркс). */
#define A_BUFS          0140    /* ring Э-60 -> BESM-6: +0 source, +1 receiver, +2.. slots */
#define A_BUFH          0146    /* ring BESM-6 -> Э-60 */
#define A_FOSS          0154    /* system request: +0 code, +1 line/term, +2 area */
#define A_FOSH          0157    /* system reply: +0 = 2, +1 term no., +2 result */
#define RING_MASK       3       /* масотв for index 2 */
#define DOORBELL        077777  /* адркс */
#define DOORBELL_RING   0400    /* масабн */
#define ABONENT_OS      48      /* абонент 48 = system request/reply */

/* Terminal area, in words (svyaz7.be: sтрзпр, вводсs, прмсвs). */
#define AR_STATUS0      0       /* 020000 = input pending, cleared by the OS */
#define AR_STATUS1      1       /* 020000 = output active, cleared by the OS */
#define AR_CMD_IN       2       /* Э-60 -> OS: 4 input ready, 5 output done */
#define AR_INBUF        3       /* byte address of the input buffer */
#define AR_CMD_OUT      5       /* OS -> Э-60: 4 or 5 = take an array */
#define AR_OUTADDR      6       /* byte address of the output array */
#define AR_OUTLEN       7       /* its length in words */
#define AR_FLAG         020000

#define AREA_BASE       01000
#define AREA_SIZE       01400
#define AREA_INBUF      01210   /* after +8 and an 01200-word output array */
#define INBUF_BYTES     ((AREA_SIZE - AREA_INBUF) * 2 - 1)
#define AREA(n)         (AREA_BASE + (n) * AREA_SIZE)

#define DKS_LINES       24      /* TTY_MAX */

#define DBG_KADOPAM     001
#define DBG_RING        002
#define DBG_IRQ         004
#define DBG_BRIDGE      010

/* Э-60 RAM, one per КРК channel; only DKS_CHAN is backed by an Э-60. */
static uint16 e60_mem[4][32768];
static uint32 cur_chan;
static int os_polled;           /* КОНФУС has read the channel status */

/* ===== REMOTE mode (stage B2) =====

   With SET DKS REMOTE=host:port the ОЗУ of the Э-60 lives in another
   SIMH process (PDP11/pdp11_dks.c, SET DKS BRIDGE=port); КАДОПАМ
   becomes a TCP bridge.  Every 032/0132 is one 8-byte frame:

        request  D6 op chan addr_hi addr_lo val_hi val_lo 00
        response D7 op|0200 ...

   The local protocol engine (dks_process and friends) is disabled in
   this mode: the engine lives on the Э-60 side.  The БЭСМ-6 keeps only
   its own state: os_polled, МПРП and ПРП6 (the interrupt arrives as an
   ATTN frame from the Э-60).  Frames: READ 01, WRITE 02, ATTN 03,
   PING 04, BULK_READ 05, BULK_WRITE 06 (stage C).

   Bulk frames carry cnt one-byte words:  the request is 8 + 2*cnt
   bytes; BULK_READ answers 8 + 2*cnt, BULK_WRITE answers 8.  Bulk data
   covers addresses 1..077776 only: cell 0 (channel select) and the
   doorbell 077777 keep their single-frame side effects.  Consecutive
   ascending 0132 writes are coalesced into BULK_WRITE frames (up to
   BULK_MAX words); a read, cell 0, the doorbell or a non-adjacent
   write flushes the buffer first, so the program order over the
   bridge is preserved. */

#define BRIDGE_MAGIC_REQ    0xD6
#define BRIDGE_MAGIC_RSP    0xD7
#define BRIDGE_OP_READ      01
#define BRIDGE_OP_WRITE     02
#define BRIDGE_OP_ATTN      03
#define BRIDGE_OP_PING      04
#define BRIDGE_OP_BREAD     05
#define BRIDGE_OP_BWRITE    06
#define BULK_MAX            64          /* words per bulk frame (stage C) */

#define RS_DOWN         0       /* no socket, retry on the timer */
#define RS_CONN         1       /* connect() in progress */
#define RS_UP           2       /* established */

static int dks_remote;          /* REMOTE mode is on */
static char dks_remote_spec[CBUFSIZE] = "";     /* "host:port" of Э-60 #0 */
static int dks_nchan = 1;       /* CHANS: bridge clients, КРК channels 2+i */
static SOCKET rsocks[4] = { INVALID_SOCKET, INVALID_SOCKET,
                            INVALID_SOCKET, INVALID_SOCKET };
static int rst[4];              /* RS_DOWN/RS_CONN/RS_UP, per socket */
static time_t rlasttry[4];      /* last connect attempt, wall clock */
static uint8 rfrm[4][8];        /* inbound frame accumulators */
static int rfrm_n[4];
static uint32 n_rreads, n_rwrites, n_rattn, n_rerr;     /* counters */
static uint32 n_rbulk;          /* BULK_WRITE frames sent (stage C) */

/* Write coalescing (stage C): consecutive ascending 0132 writes to the
   same channel accumulate into one BULK_WRITE frame. */
static int bulk_cnt;            /* words pending, 0 = buffer empty */
static int bulk_addr;           /* address of the first pending word */
static int bulk_chan;           /* channel of the pending words */
static int32 bulk_buf[BULK_MAX];
static time_t bulk_last;        /* last timed flush attempt (safety net) */

static int dks_remote_connect (int idx, int force);
static void dks_remote_drop (int idx);
static int dks_remote_xact (int op, int addr, int32 *val);
static void dks_remote_poll (void);
static void dks_bulk_flush (void);
static int32 dks_chan_status (void);

/* Pending system request: 0 none, 1 connect (await reply), 3 disconnect. */
static int sys_busy;
static int sys_line;
static uint32 sys_src;          /* абуфs source count when it was posted */
static uint32 bufh_recv;        /* our receiver count in абуфh */

static struct dks_line {
    int     up;                 /* the Э-60 sees the line as connected */
    int     want_connect;       /* connect request not yet sent */
    int     want_disconnect;
    int     term;               /* OS terminal number k+1, 0 if none */
    int     echo;
    int     len;                /* characters in the line being typed */
    char    buf[INBUF_BYTES];
    int     line_ready;         /* line finished, not yet passed to the OS */
    int     pending;            /* 0, or the +2 command awaiting acknowledgement */
    int     out_done;           /* output printed, completion not yet posted */
} line[DKS_LINES + 1];

t_stat dks_svc (UNIT *u);
t_stat dks_reset (DEVICE *dptr);

UNIT dks_unit = { UDATA (dks_svc, 0, 0) };

/* SET/SHOW REMOTE processors (stage B2) forward declarations. */
t_stat dks_set_remote (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_set_local (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_remote (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat dks_set_chans (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat dks_show_chans (FILE *st, UNIT *uptr, int32 val, CONST void *desc);

MTAB dks_mtab[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "REMOTE", "REMOTE=host:port",
        &dks_set_remote, &dks_show_remote, NULL,
        "КАДОПАМ bridge client: connect to the Э-60 simulator (stage B2)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "CHANS", "CHANS=n",
        &dks_set_chans, &dks_show_chans, NULL,
        "Bridge clients 1-2: КРК channels 2+i, ports p+i (stage C)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, NULL, "LOCAL",
        &dks_set_local, NULL, NULL,
        "Turn the bridge off: the ОЗУ Э-60 is emulated locally again" },
    { 0 }
};

REG dks_reg[] = {
    { ORDATA (CHAN, cur_chan, 3) },
    { DRDATA (CHANS, dks_nchan, 8), PV_LEFT },
    { BRDATA (MEM, e60_mem[DKS_CHAN], 8, 16, 32768) },
    { DRDATA (BREAD, n_rreads, 32), PV_LEFT },
    { DRDATA (BWRITE, n_rwrites, 32), PV_LEFT },
    { DRDATA (BATTN, n_rattn, 32), PV_LEFT },
    { DRDATA (BBULK, n_rbulk, 32), PV_LEFT },
    { DRDATA (BERR, n_rerr, 32), PV_LEFT },
    { 0 }
};

DEBTAB dks_deb[] = {
    { "KADOPAM", DBG_KADOPAM, "every 032/0132 access" },
    { "RING",    DBG_RING,    "ring slots, system requests, area writes" },
    { "IRQ",     DBG_IRQ,     "interrupts raised" },
    { "BRIDGE",  DBG_BRIDGE,  "КАДОПАМ bridge frames" },
    { NULL, 0 }
};

DEVICE dks_dev = {
    "DKS", &dks_unit, dks_reg, dks_mtab,
    1, 8, 15, 1, 8, 16,
    NULL, NULL, &dks_reset, NULL, NULL, NULL,
    NULL, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, dks_deb
};

static int dks_enabled (void)
{
    return ! (dks_dev.flags & DEV_DIS);
}

/*
 * The OS listens only after КОНФУС has loaded СВЯЗЬ7, read the channel
 * status and unmasked our interrupt.
 */
static int os_ready (void)
{
    return os_polled && (MPRP & DKS_PRP);
}

static uint32 mem (int addr)
{
    int32 val;

    if (dks_remote) {
        dks_bulk_flush ();
        if (dks_remote_xact (BRIDGE_OP_READ, addr, &val) == 0)
            return val & 0177777;
        }
    return e60_mem[DKS_CHAN][addr & 077777];
}

static void set_mem (int addr, uint32 val)
{
    int32 rc;

    e60_mem[DKS_CHAN][addr & 077777] = val & 0177777;
    if (dks_remote) {
        dks_bulk_flush ();
        rc = (int32) (val & 0177777);
        dks_remote_xact (BRIDGE_OP_WRITE, addr, &rc);
    }
}

/*
 * Read by instruction 032: the result goes into bits 25-40 of the accumulator.
 * In REMOTE mode the read crosses the bridge (cell 0 included, so that a
 * multi-Э-60 configuration can report its live channel status).
 */
t_value dks_read (int addr)
{
    t_value val;
    int32 rv = 0;

    if (dks_remote)
        dks_bulk_flush ();              /* reads see all prior writes */
    if (addr == 0) {
        os_polled = 1;
        if (! dks_enabled ())
            val = 0;
        else if (dks_remote)
            val = (t_value) dks_chan_status ();   /* live clients, no RTT */
        else
            val = DKS_STATUS;
    }
    else if (addr == 2)
        val = 0;        /* номнпр: meaning unknown, 0 keeps рспец out */
    else if (dks_remote) {
        rv = 0;
        val = dks_remote_xact (BRIDGE_OP_READ, addr, &rv) ?
            0 : (t_value) rv;
    }
    else
        val = e60_mem[cur_chan & 3][addr];
    sim_debug (DBG_KADOPAM, &dks_dev, "032: [%o]%05o -> %06o\n",
               cur_chan, addr, (unsigned) val);
    return val;
}

/*
 * Write by instruction 0132: the low 16 bits of the accumulator.
 * In REMOTE mode the write crosses the bridge; the Э-60 detects the
 * doorbell (cell 077777 with 0400) on its side of the frame.
 */
void dks_write (int addr, t_value acc)
{
    uint32 val = (uint32) acc & 0177777;
    int32 wv = (int32) val;

    sim_debug (DBG_KADOPAM, &dks_dev, "0132: [%o]%05o <- %06o\n",
               cur_chan, addr, val);
    if (addr == 0) {
        cur_chan = val & 7;             /* routing decision, this side */
        if (dks_remote)                 /* the instance's shadow follows */
            dks_remote_xact (BRIDGE_OP_WRITE, 0, &wv);
        return;
    }
    if (dks_remote) {
        if (addr == DOORBELL) {         /* the firmware reacts: flush first */
            dks_bulk_flush ();
            dks_remote_xact (BRIDGE_OP_WRITE, addr, &wv);
            return;
            }
        /* Data write: coalesce ascending runs into BULK_WRITE frames. */
        if (bulk_cnt > 0 && (bulk_chan != (int) (cur_chan & 3) ||
                             addr != bulk_addr + bulk_cnt))
            dks_bulk_flush ();          /* not adjacent: flush */
        if (bulk_cnt == 0) {
            bulk_chan = cur_chan & 3;
            bulk_addr = addr;
            }
        bulk_buf[bulk_cnt++] = (int32) val;
        if (bulk_cnt == BULK_MAX)
            dks_bulk_flush ();
        return;
    }
    e60_mem[cur_chan & 3][addr] = val;
    if (! dks_enabled () || cur_chan != DKS_CHAN)
        return;
    if (addr == DOORBELL) {
        if (val & ~DOORBELL_RING)
            sim_debug (DBG_RING, &dks_dev, "doorbell %06o\n", val);
        if (val & DOORBELL_RING)
            sim_activate (&dks_unit, 20);
    } else if (addr >= AREA_BASE && addr < AREA(DKS_LINES + 1) &&
               (addr - AREA_BASE) % AREA_SIZE < 8) {
        sim_debug (DBG_RING, &dks_dev, "line %d area+%d <- %06o\n",
                   (addr - AREA_BASE) / AREA_SIZE,
                   (addr - AREA_BASE) % AREA_SIZE, val);
    }
}

/*
 * Post an abonent number into the ring towards the BESM-6.
 * Returns 0 when the ring is full: the OS has not caught up yet.
 */
static int e60_post (int abonent)
{
    uint32 src = mem (A_BUFS) & RING_MASK;
    uint32 recv = mem (A_BUFS + 1) & RING_MASK;

    if (((src - recv) & RING_MASK) == RING_MASK)
        return 0;
    set_mem (A_BUFS + 2 + src, abonent);
    set_mem (A_BUFS, (src + 1) & RING_MASK);
    sim_debug (DBG_RING, &dks_dev, "post %d (slot %d)\n", abonent, src);
    if (! (PRP & DKS_PRP))
        sim_debug (DBG_IRQ, &dks_dev, "ПРП 6\n");
    PRP |= DKS_PRP;
    return 1;
}

static int find_line (int e60_line)
{
    return (e60_line >= 1 && e60_line <= DKS_LINES && line[e60_line].up) ?
        e60_line : 0;
}

/*
 * Output array from the OS: print it, then report completion.
 */
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
        vt_send (n, w & 0177);
        if ((w >> 8 & 0377) == 0)
            break;
        vt_send (n, w >> 8 & 0177);
    }
    line[n].out_done = 1;
}

/*
 * Ring from the OS: take every entry posted since the last doorbell.
 */
static void drain_bufh (void)
{
    uint32 src = mem (A_BUFH) & RING_MASK;

    while (bufh_recv != src) {
        int abonent = mem (A_BUFH + 2 + bufh_recv);
        int n;

        if (abonent == 0) {
            /* The ОС advances the ring pointer before writing the slot
               cell (observed with СВЯЗЬ7).  This drain runs between
               guest instructions, so it can observe the advanced
               pointer with the slot write of the same notification not
               done yet; 0 is never a valid abonent - defer the entry
               to a later tick instead of losing it. */
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
        struct dks_line *l = &line[sys_line];
        int term = mem (A_FOSH + 1);
        int result = mem (A_FOSH + 2);

        set_mem (A_FOSH, 0);
        sys_busy = 0;
        sim_debug (DBG_RING, &dks_dev, "line %d: connect reply term %d result %d\n",
                   sys_line, term, result);
        if (result == 0) {
            l->term = term;
        } else {
            vt_puts (sys_line, result == 1 ? "\r\nDKS: line busy\r\n" :
                     "\r\nDKS: no free terminal\r\n");
            l->up = 0;
        }
    }
}

static void post_sys_request (int n, int code)
{
    int a = AREA(n);

    set_mem (A_FOSS, code);
    set_mem (A_FOSS + 1, code == 1 ? n : line[n].term);
    set_mem (A_FOSS + 2, code == 1 ? a << 1 : 0);
    sys_src = mem (A_BUFS) & RING_MASK;
    if (! e60_post (ABONENT_OS))
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
    line[n].len = 0;
    line[n].line_ready = 0;
    line[n].pending = 0;
    line[n].out_done = 0;
}

/*
 * Is the OS done with the last event of this line?
 */
static void check_ack (int n)
{
    int a = AREA(n);

    if (line[n].pending == 4 && ! (mem (a + AR_STATUS0) & AR_FLAG))
        line[n].pending = 0;
    else if (line[n].pending == 5 && ! (mem (a + AR_STATUS1) & AR_FLAG))
        line[n].pending = 0;
}

static void post_line_event (int n, int cmd)
{
    int a = AREA(n);

    set_mem (a + AR_CMD_IN, cmd);
    if (cmd == 4)
        set_mem (a + AR_STATUS0, mem (a + AR_STATUS0) | AR_FLAG);
    if (! e60_post (line[n].term)) {
        set_mem (a + AR_CMD_IN, 0);
        return;
    }
    line[n].pending = cmd;
}

static void pass_input (int n)
{
    int a = AREA(n) + AREA_INBUF, i;
    struct dks_line *l = &line[n];

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

/*
 * Advance the Э-60 state machine; called by the unit service and by the
 * terminal poll.  In REMOTE mode the engine lives on the Э-60 side; this
 * side only polls the bridge for ATTN frames (ПРП 6).
 */
static void dks_process (void)
{
    int n;

    if (! dks_enabled ())
        return;
    if (dks_remote) {
        dks_remote_poll ();
        /* Safety net: the protocol always flushes on reads, cell 0 and
           the doorbell; the timed flush bounds the latency if a run
           ever stalls with data buffered.  At most once a second. */
        if (bulk_cnt) {
            time_t now = time (NULL);

            if (now != bulk_last) {
                bulk_last = now;
                dks_bulk_flush ();
                }
            }
        return;
    }
    drain_bufh ();

    /* A disconnect gets no reply: wait until the OS has read the ring. */
    if (sys_busy == 3) {
        uint32 posted = (mem (A_BUFS) - sys_src) & RING_MASK;
        uint32 taken = (mem (A_BUFS + 1) - sys_src) & RING_MASK;

        if (taken != 0 && taken <= posted) {
            sys_busy = 0;
            line[sys_line].term = 0;
        }
    }
    for (n = 1; n <= DKS_LINES; ++n) {
        struct dks_line *l = &line[n];

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
        } else if (l->line_ready)
            pass_input (n);
    }
}

/*
 * ===== REMOTE bridge client (stage B2) =====
 */

/* Close socket idx (or all, idx < 0); the mode stays on, reconnection
   is by the timer. */
static void dks_remote_drop (int idx)
{
    int i, lo = (idx >= 0)? idx: 0, hi = (idx >= 0)? idx + 1: 4;

    for (i = lo; i < hi; ++i) {
        if (rsocks[i] != INVALID_SOCKET) {
            sim_close_sock (rsocks[i]);
            rsocks[i] = INVALID_SOCKET;
        }
        rfrm_n[i] = 0;
        if (rst[i] == RS_UP)
            sim_debug (DBG_BRIDGE, &dks_dev, "bridge down: %s\n",
                       dks_remote_spec);
        rst[i] = RS_DOWN;
    }
    bulk_cnt = 0;                   /* pending coalesced writes are lost */
}

/* The port of Э-60 number idx: the base spec with port+idx. */
static void dks_remote_spec_of (int idx, char *buf)
{
    char *colon;

    strcpy (buf, dks_remote_spec);
    colon = strrchr (buf, ':');
    if (colon != NULL && colon[1] != '\0')
        sprintf (colon + 1, "%d", atoi (colon + 1) + idx);
}

/* Try to establish connection idx; not more than once a second. */
static int dks_remote_connect (int idx, int force)
{
    char spec[CBUFSIZE];
    time_t now = time (NULL);

    if (rst[idx] == RS_UP)
        return 1;
    if (rsocks[idx] == INVALID_SOCKET) {
        if (! force && now == rlasttry[idx])
            return 0;
        rlasttry[idx] = now;
        dks_remote_spec_of (idx, spec);
        rsocks[idx] = sim_connect_sock_ex (NULL, spec, NULL, NULL, 0);
        if (rsocks[idx] == INVALID_SOCKET)
            return 0;
        rst[idx] = RS_CONN;
        sim_debug (DBG_BRIDGE, &dks_dev, "bridge: connecting to %s\n",
                   spec);
        return 0;
        }
    /* connect() in progress: writable = established, error = failed. */
    if (sim_check_conn (rsocks[idx], 0) == 1) {
        rst[idx] = RS_UP;
        dks_remote_spec_of (idx, spec);
        sim_debug (DBG_BRIDGE, &dks_dev, "bridge up: %s\n", spec);
        return 1;
        }
    if (sim_check_conn (rsocks[idx], 1) < 0)
        dks_remote_drop (idx);
    return 0;
}

/*
 * One inbound frame: ATTN raises ПРП6 locally, responses are consumed
 * by the transaction loop.  Returns 1 when a response was consumed and
 * stored into *val.
 */
static int dks_remote_frame (uint8 *f, int op, int addr, int32 *val,
                             int in_xact)
{
    if (f[0] == BRIDGE_MAGIC_REQ) {              /* Э-60 -> БЭСМ-6 */
        if (f[1] == BRIDGE_OP_ATTN) {
            static const char prp_name[4] = { 8, 7, 6, 5 };
            int chan = f[2] & 3;

            ++n_rattn;
            sim_debug (DBG_BRIDGE, &dks_dev,
                       "bridge: <- ATTN (ПРП %d)%s\n",
                       prp_name[chan], (f[3] & 1)? " + ПРП12": "");
            sim_debug (DBG_IRQ, &dks_dev, "ПРП %d\n", prp_name[chan]);
            PRP |= PRP_DKS_CHAN (chan);
            if (f[3] & 1) {          /* system request (stage C):
                                        СВЯЗЬ7/Диспак wait for ПРП12
                                        to run нтерм7 */
                sim_debug (DBG_IRQ, &dks_dev, "ПРП 12\n");
                PRP |= PRP_DKS_ATTN;
                }
            return 0;
            }
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: unexpected request op %o\n", f[1]);
        ++n_rerr;
        return 0;
        }
    if (f[0] != BRIDGE_MAGIC_RSP) {
        sim_debug (DBG_BRIDGE, &dks_dev,
                   "bridge: bad magic %02x\n", f[0]);
        dks_remote_drop (-1);
        ++n_rerr;
        return 0;
        }
    /* A response: in a transaction it must echo the request. */
    if (in_xact && f[1] == (uint8) (op | 0x80) &&
        f[3] == (uint8) (addr >> 8) && f[4] == (uint8) addr) {
        *val = (f[5] << 8) | f[6];
        return 1;
        }
    /* Late response of a timed-out transaction: drop it. */
    sim_debug (DBG_BRIDGE, &dks_dev, "bridge: stale response dropped\n");
    return 0;
}

/* Read and dispatch whatever inbound frames are pending, all clients. */
static void dks_remote_poll (void)
{
    int i, n;

    for (i = 0; i < dks_nchan; ++i) {
        if (rst[i] != RS_UP) {
            dks_remote_connect (i, FALSE);
            continue;
            }
        while ((n = sim_read_sock (rsocks[i],
                                   (char *) rfrm[i] + rfrm_n[i],
                                   8 - rfrm_n[i])) != 0) {
            if (n < 0) {
                dks_remote_drop (i);
                break;
                }
            rfrm_n[i] += n;
            if (rfrm_n[i] == 8) {
                dks_remote_frame (rfrm[i], 0, 0, NULL, FALSE);
                rfrm_n[i] = 0;
                }
            }
        }
}

/* Cell 0 in REMOTE mode: the live channels of our bridge clients. */
static int32 dks_chan_status (void)
{
    int32 val = 0;
    int i;

    for (i = 0; i < dks_nchan; ++i)
        if (rst[i] == RS_UP)
            val |= 0400 << (DKS_CHAN + i);
    return val;
}

/*
 * Send the coalesced writes as one BULK_WRITE frame (stage C) and wait
 * for the echo.  The buffer is dropped on any failure: the connection
 * is gone or stale anyway.  No-op when the buffer is empty.
 */
static void dks_bulk_flush (void)
{
    uint8 req[8], pay[2*BULK_MAX], *f;
    char spec[CBUFSIZE];
    SOCKET s;
    int *fn;
    int idx, i, have, n;

    if (bulk_cnt == 0 || ! dks_remote)
        return;
    bulk_last = time (NULL);
    idx = bulk_chan - DKS_CHAN;
    if (idx < 0 || idx >= dks_nchan || rst[idx] != RS_UP) {
        bulk_cnt = 0;                               /* nowhere to send */
        return;
        }
    s = rsocks[idx];
    f = rfrm[idx];
    fn = &rfrm_n[idx];
    dks_remote_spec_of (idx, spec);
    req[0] = BRIDGE_MAGIC_REQ;
    req[1] = BRIDGE_OP_BWRITE;
    req[2] = (uint8) bulk_chan;
    req[3] = (uint8) (bulk_addr >> 8);
    req[4] = (uint8) bulk_addr;
    req[5] = (uint8) bulk_cnt;
    req[6] = req[7] = 0;
    for (i = 0; i < bulk_cnt; ++i) {
        pay[2*i]     = (uint8) (bulk_buf[i] >> 8);
        pay[2*i + 1] = (uint8) bulk_buf[i];
        }
    sim_debug (DBG_BRIDGE, &dks_dev,
               "bridge: -> BULK_WRITE [%o]%05o x%d\n",
               bulk_chan, bulk_addr, bulk_cnt);
    for (have = 0; have < 8; ) {                    /* the header */
        i = sim_write_sock (s, (const char *) req + have, 8 - have);
        if (i <= 0) {
            dks_remote_drop (idx);
            ++n_rerr;
            return;
            }
        have += i;
        }
    for (have = 0; have < 2*bulk_cnt; ) {           /* the payload */
        n = sim_write_sock (s, (const char *) pay + have,
                            2*bulk_cnt - have);
        if (n <= 0) {
            dks_remote_drop (idx);
            ++n_rerr;
            return;
            }
        have += n;
        }
    for (i = 0; i < 100; ++i) {                     /* bounded wait */
        int spin;

        for (spin = 0; spin < 200; ++spin) {
            while ((n = sim_read_sock (s, (char *) f + *fn, 8 - *fn)) > 0) {
                *fn += n;
                if (*fn == 8) {
                    int match = (f[0] == BRIDGE_MAGIC_RSP &&
                                 f[1] == (uint8) (BRIDGE_OP_BWRITE | 0x80) &&
                                 f[3] == (uint8) (bulk_addr >> 8) &&
                                 f[4] == (uint8) bulk_addr);

                    *fn = 0;
                    if (match) {
                        ++n_rbulk;
                        bulk_cnt = 0;
                        return;
                        }
                    dks_remote_frame (f, 0, 0, NULL, FALSE);   /* ATTN etc. */
                    }
                }
            if (n < 0)
                break;
            }
        if (n < 0) {
            dks_remote_drop (idx);
            ++n_rerr;
            return;
            }
        sim_os_ms_sleep (1);
        }
    sim_debug (DBG_BRIDGE, &dks_dev,
               "bridge: BULK_WRITE response timeout (%s)\n", spec);
    ++n_rerr;
    bulk_cnt = 0;
}

/*
 * One КАДОПАМ transaction: send the request, wait for the response.
 * Returns 0 on success (*val = the word), -1 when the bridge is not
 * usable (the caller reads 0 / drops the write).  The BESM-6 never
 * blocks for long: at most a few milliseconds, then the transaction
 * fails with a rate-limited warning.
 */
static int dks_remote_xact (int op, int addr, int32 *val)
{
    uint8 req[8], *f;
    char spec[CBUFSIZE];
    SOCKET s;
    int *fn;
    int i, have, idx;

    if (! dks_remote)
        return -1;
    idx = ((int) cur_chan & 7) - DKS_CHAN;
    if (idx < 0 || idx >= dks_nchan) {
        static time_t last_warn = 0;
        time_t now = time (NULL);

        if (now != last_warn) {
            last_warn = now;
            sim_debug (DBG_BRIDGE, &dks_dev,
                       "bridge: channel %o has no Э-60, transaction lost\n",
                       cur_chan & 7);
            }
        ++n_rerr;
        return -1;
        }
    if (rst[idx] != RS_UP && ! dks_remote_connect (idx, FALSE)) {
        static time_t last_warn2 = 0;
        time_t now = time (NULL);

        if (now != last_warn2) {
            last_warn2 = now;
            dks_remote_spec_of (idx, spec);
            sim_debug (DBG_BRIDGE, &dks_dev,
                       "bridge: %s unavailable, transaction lost\n", spec);
            }
        ++n_rerr;
        return -1;
        }
    s = rsocks[idx];
    f = rfrm[idx];
    fn = &rfrm_n[idx];
    dks_remote_spec_of (idx, spec);
    req[0] = BRIDGE_MAGIC_REQ;
    req[1] = (uint8) op;
    req[2] = (uint8) (cur_chan & 7);
    req[3] = (uint8) (addr >> 8);
    req[4] = (uint8) addr;
    req[5] = (uint8) (*val >> 8);
    req[6] = (uint8) (*val & 0377);
    req[7] = 0;
    sim_debug (DBG_BRIDGE, &dks_dev, "bridge: -> op %o [%o]%05o val %06o\n",
               op, cur_chan & 7, addr, *val & 0177777);
    for (have = 0; have < 8; ) {                    /* send the request */
        i = sim_write_sock (s, (const char *) req + have, 8 - have);
        if (i < 0) {
            dks_remote_drop (idx);
            ++n_rerr;
            return -1;
            }
        have += i;
        }
    for (i = 0; i < 100; ++i) {                     /* bounded wait, ~100 ms */
        int n, spin;

        for (spin = 0; spin < 200; ++spin) {
            while ((n = sim_read_sock (s, (char *) f + *fn, 8 - *fn)) > 0) {
                *fn += n;
                if (*fn == 8) {
                    int32 v = 0;

                    *fn = 0;
                    if (dks_remote_frame (f, op, addr, &v, TRUE)) {
                        *val = v;
                        if (op == BRIDGE_OP_READ)
                            ++n_rreads;
                        else
                            ++n_rwrites;
                        return 0;
                        }
                    }
                }
            if (n < 0)
                break;
            }
        if (n < 0) {
            dks_remote_drop (idx);
            ++n_rerr;
            return -1;
            }
        sim_os_ms_sleep (1);        /* let the Э-60 process answer */
        }
    sim_debug (DBG_BRIDGE, &dks_dev,
               "bridge: response timeout (%s)\n", spec);
    ++n_rerr;
    return -1;
}

/* SET/SHOW REMOTE processors. */

/* Parse "host:port" (the host part may be empty for localhost). */
static int dks_remote_parse (const char *cptr)
{
    char *colon;

    strncpy (dks_remote_spec, cptr, CBUFSIZE - 1);
    dks_remote_spec[CBUFSIZE - 1] = '\0';
    colon = strrchr (dks_remote_spec, ':');
    if (colon == NULL || colon[1] == '\0')
        return -1;
    return 0;
}

t_stat dks_set_remote (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    if (cptr == NULL || *cptr == 0)
        return SCPE_ARG;
    if (dks_remote_parse (cptr))
        return SCPE_ARG;
    dks_remote_drop (-1);
    dks_remote = 1;
    return SCPE_OK;
}

/* SET CHANS=n (stage C): the number of bridge clients.  Client i serves
   КРК channel 2+i and connects to port p+i (p from the REMOTE spec).
   With CHANS=2 the Э-60 instances claim channels 2 and 3 (SET DKS
   CHAN=k on the PDP-11 side); the ОС sees the live channels in cell 0.
   КРК has 4 channels, but the base channel of the reference ДКС is 2,
   so ascending assignment covers 2 and 3 only. */
t_stat dks_set_chans (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    t_stat r;
    int32 n;

    if (cptr == NULL || *cptr == 0)
        return SCPE_ARG;
    n = (int32) get_uint (cptr, 10, 2, &r);
    if (r != SCPE_OK || n < 1)
        return SCPE_ARG;
    dks_remote_drop (-1);
    dks_nchan = n;
    return SCPE_OK;
}

t_stat dks_show_chans (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "chans=%d", dks_nchan);
    return SCPE_OK;
}

t_stat dks_set_local (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    dks_remote_drop (-1);
    dks_remote = 0;
    return SCPE_OK;
}

t_stat dks_show_remote (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    if (! dks_remote)
        fprintf (st, "remote=off");
    else
        fprintf (st, "remote=%s chans=%d%s", dks_remote_spec, dks_nchan,
                 rst[0] == RS_UP? " (up)":
                 rst[0] == RS_CONN? " (connecting)": " (down)");
    return SCPE_OK;
}

t_stat dks_svc (UNIT *u)
{
    dks_process ();
    return SCPE_OK;
}

t_stat dks_reset (DEVICE *dptr)
{
    int n;

    sim_cancel (&dks_unit);
    memset (e60_mem, 0, sizeof (e60_mem));
    cur_chan = 0;
    os_polled = 0;
    sys_busy = 0;
    bufh_recv = 0;
    dks_remote_drop (-1);           /* the bridge is re-established on demand */
    for (n = 1; n <= DKS_LINES; ++n) {
        int up = line[n].up || line[n].want_connect;
        memset (&line[n], 0, sizeof (line[n]));
        line[n].echo = 1;
        line[n].want_connect = up;
    }
    return SCPE_OK;
}

/*
 * Interface for besm6_tty.c.
 */
void dks_line_state (int n, int connected)
{
    struct dks_line *l = &line[n];

    if (connected) {
        l->want_disconnect = 0;
        l->want_connect = 1;
        l->echo = 1;
        vt_puts (n, "Connected to DKS\r\n");
    } else {
        l->want_connect = 0;
        if (l->up)
            l->want_disconnect = 1;
    }
}

/* Whether the line accepts another typed character now. */
int dks_line_can_input (int n)
{
    return dks_enabled () && line[n].term && ! line[n].line_ready;
}

/* Turns local echo on or off; for ТАЙНА, once its trigger is known. */
void e60_set_echo (int n, int on)
{
    line[n].echo = on;
}

/*
 * A character typed on the line (KOI-7): local editing and echo.
 */
void dks_line_char (int n, int c)
{
    struct dks_line *l = &line[n];

    switch (c) {
    case '\r':
    case '\n':
        if (l->echo)
            vt_send (n, '\n');
        l->line_ready = 1;
        break;
    case '\b':
    case 0177:
        if (l->len > 0) {
            --l->len;
            if (l->echo)
                vt_puts (n, "\b \b");
        }
        break;
    default:
        if (l->len >= INBUF_BYTES - 1)
            break;
        l->buf[l->len++] = c;
        if (l->echo)
            vt_send (n, c);
    }
}

/* Called on every terminal clock tick. */
void dks_poll (void)
{
    dks_process ();
}

int dks_busy (void)
{
    int n;

    if (! dks_enabled () || dks_remote)
        return 0;               /* REMOTE: nothing pending on this side */
    if (sys_busy)
        return 1;
    for (n = 1; n <= DKS_LINES; ++n)
        if ((line[n].want_connect && os_ready ()) || line[n].want_disconnect ||
            line[n].pending || line[n].out_done || line[n].line_ready)
            return 1;
    return 0;
}

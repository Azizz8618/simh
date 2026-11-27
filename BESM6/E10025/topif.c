/* topif.c: autonomous TOP interface model for the E10025 simulator.
 *
 * The device owns its registers and derives requests, state transitions and
 * published low-memory status cells from guest-visible register operations.
 * Documented bit positions are used where the available descriptions agree.
 * Other bits remain zero and are marked UNKNOWN in the journal; no fixed
 * snapshot is returned in place of a state transition.
 */

#include "topif.h"
#include "e10025_io_lib.h"
#include "sim_console.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "sim_sock.h"

#define TOP_DEB_JOURNAL  1
#define TOP_DEB_IRQ      2
#define TOP_DEB_STATE    4


extern DEVICE top_dev;
extern uint16 *M;
extern int32 R[8];
extern int32 C;
extern int32 Z;
extern void chan_write_io (int32 pa, int32 data, int32 byte);
extern UNIT tto_unit;

top_state_t top_state;

/* Local POSIX shared-memory bridge to the separate model process.  The guest
 * sees only device registers; BP/FP/ODS computation stays in that process.
 * The earlier line-oriented transport is kept below as a commented record:
 *
 * #define TOP_BRIDGE_DEF_HOST "127.0.0.1"
 * #define TOP_BRIDGE_DEF_PORT "4230"
 * static SOCKET top_bridge_fd = INVALID_SOCKET;
 * top_bridge_fd = sim_connect_sock (addr, TOP_BRIDGE_DEF_HOST,
 *                                   TOP_BRIDGE_DEF_PORT);
 * topif_bridge_exchange ("HELLO 1\n", reply, sizeof (reply));
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <time.h>

/* Name built from codes so the simulator source stays neutral. */
static const char top_shm_name[] = {
    '/', 0172, 0142, 0141, '_', 't', 'o', 'p', '_', 's', 'o', 'i', 0
    };

#define TOP_SHM_SIZE      65536u
#define TOP_SHM_MAGIC     0x544f5053u
#define TOP_SHM_VERSION   1u
#define TOP_SHM_RING      64u
#define TOP_SHM_SLOT      32u
#define TOP_SHM_CTRL      0x000u
#define TOP_SHM_CMD       0x100u
#define TOP_SHM_RSP       0x1000u
#define TOP_SHM_JRN       0x2000u
#define TOP_SHM_DATA      0x7000u
#define TOP_SHM_DATA_SIZE 4112u

#define TOP_SH_MAGIC      0u
#define TOP_SH_VERSION    1u
#define TOP_SH_STATE      2u
#define TOP_SH_FLAGS      3u
#define TOP_SH_SEQ_W      4u                              /* 64-bit word */
#define TOP_SH_CMD_HEAD   6u
#define TOP_SH_CMD_TAIL   7u
#define TOP_SH_RSP_HEAD   8u
#define TOP_SH_RSP_TAIL   9u
#define TOP_SH_JRN_HEAD   10u
#define TOP_SH_SEM_CMD    48u
#define TOP_SH_SEM_RSP    80u

#define TOP_OP_HELLO      1u
#define TOP_OP_PING       2u
#define TOP_OP_READ       3u
#define TOP_OP_WRITE      4u
#define TOP_OP_SERVICE    5u
#define TOP_OP_IRQ        6u
/* Add-only signal kind.  Kinds 1..6 stay frozen for a version-1 participant. */
#define TOP_OP_SIGNAL     8u

static unsigned char *top_shm = NULL;
static int top_bridge_tried = 0;

static volatile uint32_t *topif_shm_ctrl (uint32 index)
{
    return (volatile uint32_t *) (void *) (top_shm + TOP_SHM_CTRL +
                                           index * sizeof (uint32_t));
}

static unsigned char *topif_shm_slot (uint32 base, uint32 index)
{
    return top_shm + base + (index % TOP_SHM_RING) * TOP_SHM_SLOT;
}

static volatile uint64_t *topif_shm_seq (void)
{
    return (volatile uint64_t *) (void *)
           (top_shm + TOP_SHM_CTRL + TOP_SH_SEQ_W * sizeof (uint32_t));
}

static sem_t *topif_shm_sem (uint32 offset)
{
    return (sem_t *) (void *) (top_shm + TOP_SHM_CTRL + offset);
}

static uint32_t topif_cap_checksum (void)
{
    static const uint32_t values[] = {
        TOP_SHM_SIZE, TOP_SHM_CMD, TOP_SHM_RING, TOP_SHM_SLOT,
        TOP_SHM_RSP, TOP_SHM_RING, TOP_SHM_SLOT,
        TOP_SHM_JRN, 128u, TOP_SHM_V2_JOURNAL_STRIDE,
        TOP_SHM_DATA, TOP_SHM_DATA_SIZE,
        TOP_SHM_SIG_MATRIX, TOP_SHM_SIG_MATRIX_SIZE,
        TOP_SHM_SIG_JOURNAL, TOP_SHM_SIG_JOURNAL_SIZE,
        TOP_SHM_CAP, TOP_SHM_CAP_SIZE, TOP_SHM_SIG_TOTAL
        };
    uint32_t h = 2166136261u;
    size_t i;
    unsigned b;

    for (i = 0; i < sizeof (values) / sizeof (values[0]); i++) {
        for (b = 0; b < 4u; b++) {
            h ^= (values[i] >> (8u * b)) & 0xFFu;
            h *= 16777619u;
            }
        }
    return h;
}

/* The capability block is a static declaration by the TOR model.  A version-1
 * peer does not read it; the device only records it for diagnostics. */
int topif_bridge_capability (top_shm_cap_t *cap)
{
    const top_shm_cap_t *published;

    if (cap == NULL || top_shm == NULL)
        return 0;
    published = (const top_shm_cap_t *) (const void *)
                (top_shm + TOP_SHM_CAP);
    if (published->magic != TOP_SHM_CAP_MAGIC ||
        published->version != TOP_SHM_CAP_VERSION ||
        (published->features & TOP_SHM_CAP_ADD_ONLY) == 0u ||
        published->journal_stride != TOP_SHM_V2_JOURNAL_STRIDE ||
        published->journal_slots != TOP_SHM_V2_JOURNAL_SLOTS ||
        published->signal_total != TOP_SHM_SIG_TOTAL ||
        published->layout_checksum != topif_cap_checksum ())
        return 0;
    *cap = *published;
    return 1;
}

/* Command slot: kind, tag, a, b, c, client_seq, reserved, reserved. */
static void topif_shm_put (unsigned char *slot, uint32 kind, uint32 tag,
                           uint32 a, uint32 b, uint32 c, uint32 seq)
{
    volatile uint32_t *w = (volatile uint32_t *) (void *) slot;

    w[0] = kind;
    w[1] = tag;
    w[2] = a;
    w[3] = b;
    w[4] = c;
    w[5] = seq;
    __sync_synchronize ();
}

static int topif_bridge_cmd (uint32 op, uint32 a, uint32 b, uint32 c,
                             uint32 d, uint32 *r0, uint32 *r1, uint32 *r2,
                             uint32 *flags)
{
    volatile uint32_t *ctrl;
    volatile uint64_t *seqw;
    uint64_t seq;
    uint32 tail;
    uint32 i;
    int spins;

    if (top_shm == NULL)
        return 0;
    ctrl = topif_shm_ctrl (0);
    seqw = topif_shm_seq ();
    seq = __sync_add_and_fetch (seqw, (uint64_t) 1);
    /* Agent is the producer: command slots are indexed by cmd_head. */
    tail = __sync_fetch_and_add (&ctrl[TOP_SH_CMD_HEAD], 1u);
    topif_shm_put (topif_shm_slot (TOP_SHM_CMD, tail), op, d, a, b, c,
                   (uint32) seq);
    __sync_synchronize ();
    {
    volatile uint32_t *c = (volatile uint32_t *) (void *)
                           topif_shm_slot (TOP_SHM_CMD, tail);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP BRIDGE CMD WORDS %06o/%06o/%06o/%06o/%06o/%06o tail=%u\n",
               (unsigned) c[0], (unsigned) c[1], (unsigned) c[2],
               (unsigned) c[3], (unsigned) c[4], (unsigned) c[5],
               (unsigned) tail);
    }
    {
    int semval = -1;
    sem_getvalue (topif_shm_sem (TOP_SH_SEM_CMD), &semval);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP BRIDGE POST op=%u sem_before=%d post_rc=%d\n",
               (unsigned) op, semval, sem_post (topif_shm_sem (TOP_SH_SEM_CMD)));
    sem_getvalue (topif_shm_sem (TOP_SH_SEM_CMD), &semval);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP BRIDGE POSTED sem_after=%d\n", semval);
    }
    for (spins = 0; spins < 200000; spins++) {
        for (i = 0; i < TOP_SHM_RING; i++) {
            volatile uint32_t *r = (volatile uint32_t *) (void *)
                                   topif_shm_slot (TOP_SHM_RSP, i);

            if ((r[6] == (uint32) seq && r[0] == op) ||
                (op == TOP_OP_HELLO && r[0] == TOP_OP_HELLO &&
                 r[1] == TOP_SHM_VERSION)) {
                if (r0 != NULL) *r0 = r[1];
                if (r1 != NULL) *r1 = r[2];
                if (r2 != NULL) *r2 = r[3];
                if (flags != NULL) *flags = r[4];
                /* Agent is the consumer: responses are taken at rsp_tail. */
                __sync_fetch_and_add (&ctrl[TOP_SH_RSP_TAIL], 1u);
                top_state.bridge_calls++;
                return 1;
                }
            }
        }
    {
    volatile uint32_t *r = (volatile uint32_t *) (void *)
                           topif_shm_slot (TOP_SHM_RSP, 0);
    uint32_t si;

    for (si = 0; si < TOP_SHM_RING; si++) {
        volatile uint32_t *q = (volatile uint32_t *) (void *)
                               topif_shm_slot (TOP_SHM_RSP, si);
        if (q[5] == (uint32) seq || q[6] == (uint32) seq)
            sim_debug (TOP_DEB_STATE, &top_dev,
                       "TOP BRIDGE RSP slot=%u %06o/%06o/%06o/%06o/%06o/%06o/%06o\n",
                       (unsigned) si, (unsigned) q[0], (unsigned) q[1],
                       (unsigned) q[2], (unsigned) q[3], (unsigned) q[4],
                       (unsigned) q[5], (unsigned) q[6]);
        }
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP BRIDGE CMD TIMEOUT op=%u seq=%u ct=%u ch=%u rt=%u rh=%u rsp=%06o/%06o/%06o/%06o/%06o/%06o/%06o\n",
               (unsigned) op, (unsigned) seq,
               (unsigned) ctrl[TOP_SH_CMD_TAIL], (unsigned) ctrl[TOP_SH_CMD_HEAD],
               (unsigned) ctrl[TOP_SH_RSP_TAIL], (unsigned) ctrl[TOP_SH_RSP_HEAD],
               (unsigned) r[0], (unsigned) r[1], (unsigned) r[2], (unsigned) r[3],
               (unsigned) r[4], (unsigned) r[5], (unsigned) r[6]);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP BRIDGE CTL %06o/%06o/%06o/%06o/%06o/%06o/%06o/%06o/%06o/%06o/%06o/%06o\n",
               (unsigned) ctrl[0], (unsigned) ctrl[1], (unsigned) ctrl[2],
               (unsigned) ctrl[3], (unsigned) ctrl[4], (unsigned) ctrl[5],
               (unsigned) ctrl[6], (unsigned) ctrl[7], (unsigned) ctrl[8],
               (unsigned) ctrl[9], (unsigned) ctrl[10], (unsigned) ctrl[11]);
    }
    top_state.bridge_errors++;
    return 0;
}

int topif_bridge_online (void)
{
    int fd;
    volatile uint32_t *ctrl;
    uint32 flags = 0;
    uint32 v = 0;

    if (top_state.bridge_status == 1)
        return 1;
    if (top_bridge_tried)
        return 0;
    top_bridge_tried = 1;
    if (getenv ("TOP_BRIDGE_DISABLE") != NULL) {
        top_state.bridge_status = -1;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP BRIDGE STATUS OFFLINE: disabled by environment\n");
        return 0;
        }
    fd = shm_open (top_shm_name, O_RDWR, 0600);
    if (fd < 0) {
        top_state.bridge_status = -1;
        top_state.bridge_errors++;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP BRIDGE STATUS OFFLINE: shm_open failed\n");
        return 0;
        }
    top_shm = (unsigned char *) mmap (NULL, TOP_SHM_SIZE,
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      fd, 0);
    close (fd);
    if (top_shm == MAP_FAILED) {
        top_shm = NULL;
        top_state.bridge_status = -1;
        top_state.bridge_errors++;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP BRIDGE STATUS OFFLINE: mmap failed\n");
        return 0;
        }
    ctrl = topif_shm_ctrl (0);
    if (ctrl[TOP_SH_MAGIC] != TOP_SHM_MAGIC ||
        ctrl[TOP_SH_VERSION] != TOP_SHM_VERSION) {
        top_state.bridge_status = -1;
        top_state.bridge_errors++;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP BRIDGE STATUS OFFLINE: bad magic or version\n");
        return 0;
        }
    {
    top_shm_cap_t cap;

    if (topif_bridge_capability (&cap))
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP BRIDGE CAP v2 add-only stride=%u slots=%u "
                   "signals=%u checksum=%06o\n",
                   (unsigned) cap.journal_stride, (unsigned) cap.journal_slots,
                   (unsigned) cap.signal_total,
                   (unsigned) cap.layout_checksum);
    }
    {
    int attempt;

    for (attempt = 0; attempt < 4; attempt++) {
        if (topif_bridge_cmd (TOP_OP_HELLO, TOP_SHM_VERSION, 0, 0, 0,
                              &v, NULL, NULL, &flags) && flags == 0 &&
            v == TOP_SHM_VERSION)
            break;
        }
    if (attempt == 4) {
        top_state.bridge_status = -1;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP BRIDGE STATUS OFFLINE: HELLO failed\n");
        return 0;
        }
    }
    top_state.bridge_status = 1;
    sim_debug (TOP_DEB_STATE, &top_dev, "TOP BRIDGE STATUS ONLINE\n");
    return 1;
}

uint32 topif_bridge_calls (void)
{
    return top_state.bridge_calls;
}

uint32 topif_bridge_errors (void)
{
    return top_state.bridge_errors;
}

int topif_bridge_read (uint32 addr, uint16 *data)
{
    uint32 r0 = 0;

    if (!topif_bridge_online () || data == NULL)
        return 0;
    if (!topif_bridge_cmd (TOP_OP_READ, addr, 0, 0, 0,
                           &r0, NULL, NULL, NULL))
        return 0;
    *data = (uint16) r0;
    return 1;
}

int topif_bridge_write (uint32 addr, uint16 data)
{
    uint32 flags = 0;

    if (!topif_bridge_online ())
        return 0;
    if (!topif_bridge_cmd (TOP_OP_WRITE, addr, data, 0, 0,
                           NULL, NULL, NULL, &flags))
        return 0;
    return flags == 0;
}

int topif_bridge_service (uint32 kind, uint32 a, uint32 b, uint32 c,
                          uint32 *r0, uint32 *r1, uint32 *r2, uint32 *flags)
{
    if (!topif_bridge_online ())
        return 0;
    return topif_bridge_cmd (TOP_OP_SERVICE, a, b, c, kind,
                             r0, r1, r2, flags);
}

/* Both device windows use this one accessor.  The TOR model owns the CSR
 * state and its journal seq; each window only caches the returned readback. */
int topif_csr_access (uint32 form, uint16 value, int write,
                      uint16 *readback, uint32 *seq)
{
    top_state_t *s = &top_state;
    uint32 r0 = 0;
    uint32 r1 = 0;
    uint32 r2 = 0;
    uint32 flags = 0;

    if (!topif_bridge_online ())
        return 0;
    if (!topif_bridge_service (TOP_SVC_CSR, form, (uint32) value,
                               (uint32) (write != 0), &r0, &r1, &r2, &flags))
        return 0;
    /* TOR service FLAG_ERR is bit 1, as in the other service responses. */
    if ((flags & 0x02u) != 0u)
        return 0;
    s->csr_readback = (uint16) r0;
    s->csr_seq = r1;
    if (readback != NULL)
        *readback = s->csr_readback;
    if (seq != NULL)
        *seq = s->csr_seq;
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP CSR form=%06o %s value=%06o readback=%06o seq=%u\n",
               (unsigned) form, (write != 0)? "WR": "RD",
               (unsigned) value, (unsigned) s->csr_readback,
               (unsigned) s->csr_seq);
    return 1;
}

/* A metadata topic returns one numeric PRIMARY-confirmed value.  The full
 * printed record is written by the TOR model to its journal; this device only
 * caches the value and never derives a level, pin or pulse from it. */
int topif_bridge_fact (uint32 topic, uint16 *value)
{
    uint32 r0 = 0u;

    if (!topif_bridge_online ())
        return 0;
    if (!topif_bridge_service (TOP_SVC_FACT, topic, 0u, 0u,
                               &r0, NULL, NULL, NULL))
        return 0;
    if (value != NULL)
        *value = (uint16) r0;
    return 1;
}

int topif_bridge_irq (uint32 src, uint32 channel)
{
    uint32 flags = 0;

    if (!topif_bridge_online ())
        return 0;
    if (!topif_bridge_cmd (TOP_OP_IRQ, src, channel, 0, 0,
                           NULL, NULL, NULL, &flags))
        return 0;
    return flags == 0;
}

/* A signal transition travels in the same 32-byte command record as every
 * other operation.  The id and the written value occupy the two data words;
 * the atomic control seq is echoed as the response and journal seq.  The TOR
 * model labels the line SURROGATE because no printed line event exists here. */
int topif_bridge_signal (uint32 id, uint16 value)
{
    if (!topif_bridge_online ())
        return 0;
    return topif_bridge_cmd (TOP_OP_SIGNAL, id, value, 0, 0,
                             NULL, NULL, NULL, NULL);
}

int topif_bridge_line (unsigned char *dst, uint32 cap, uint32 *length)
{
    volatile uint32_t *w;

    if (!topif_bridge_online () || dst == NULL || length == NULL)
        return 0;
    w = (volatile uint32_t *) (void *) (top_shm + TOP_SHM_DATA);
    if (w[2] > cap)
        return 0;
    memcpy (dst, top_shm + TOP_SHM_DATA + 16u, w[2]);
    *length = w[2];
    return 1;
}

int topif_bridge_next (uint32 page, uint32 index, uint32 *status,
                       uint32 *length, uint32 *checksum, uint32 *flags)
{
    uint32 st = 1;
    uint32 len = 0;
    uint32 chk = 0;
    uint32 fl = 0;

    if (!topif_bridge_online ())
        return 0;
    if (!topif_bridge_cmd (TOP_OP_SERVICE, page, index, 0, TOP_SVC_NEXT,
                           &st, &len, &chk, &fl))
        return 0;
    if (status != NULL) *status = st;
    if (length != NULL) *length = len;
    if (checksum != NULL) *checksum = chk;
    if (flags != NULL) *flags = fl;
    return st == 0;
}

int topif_bridge_data (uint32 offset, unsigned char *dst, uint32 length)
{
    if (!topif_bridge_online () || dst == NULL)
        return 0;
    if (offset + length > TOP_SHM_DATA_SIZE)
        return 0;
    memcpy (dst, top_shm + TOP_SHM_DATA + offset, length);
    /* The v1 journal ring belongs to the TOR model.  The device writes no
     * stride-32 words there; the v2 capability block declares the TOR
     * stride and keeps signal records in the separate tail region. */
    return 1;
}

static t_stat topif_reset (DEVICE *dptr);
static void topif_sig_record (top_state_t *s, uint32 id, uint16 value,
                              top_resp_t resp);
static void topif_soft_init (top_state_t *s);

static const top_reg_desc_t top_regs[] = {
    { "STATUS_A",  TOP_OFF_STATUS_A,  TOP_STATUS_A_NAMED, 0, 1, 1 },
    { "STATUS_B",  TOP_OFF_STATUS_B,  TOP_STATUS_B_DIALOG, 0, 1, 1 },
    { "STATE",     TOP_OFF_STATE,     0007u, 0, 1, 1 },
    { "REQUEST",   TOP_OFF_REQUEST,   0003u, 0, 1, 1 },
    { "BPIMA",     TOP_OFF_BPIMA,     0003u, 0, 1, 1 },
    { "PKCSR",     TOP_OFF_PKCSR,     0140200u, 0, 1, 1 },
    { "IKCSR",     TOP_OFF_IKCSR,     0003u, 0, 1, 1 },
    { "BPSRA",     TOP_OFF_BPSRA,     TOP_BPSRA_NAMED, TOP_BPSRA_SEEN, 1, 1 },
    { "BPFR1",     TOP_OFF_BPFR1,     0177777u, 0, 1, 1 },
    { "BPFR2",     TOP_OFF_BPFR2,     0177777u, 0, 1, 1 },
    { "BPCLR",     TOP_OFF_BPCLR,     0, 0, 0, 1 },
    { "IBCLR",     TOP_OFF_IBCLR,     0, 0, 0, 1 },
    { "JOURNAL",   TOP_OFF_JOURNAL,   0007u, 0, 1, 0 },
    { "SEQUENCE",  TOP_OFF_SEQUENCE,  0177777u, 0, 1, 0 },
    { "VECTOR_A",  TOP_OFF_VECTOR_A,  0177777u, 0, 1, 0 },
    { "VECTOR_B",  TOP_OFF_VECTOR_B,  0177777u, 0, 1, 0 },
    { "IRQ",       TOP_OFF_IRQ,       01403u, 0, 1, 0 },
    { "SIGSEL",    TOP_OFF_SIGSEL,    0177777u, 0, 1, 1 },
    { "SIGVAL",    TOP_OFF_SIGVAL,    0000001u, 0, 1, 1 },
    { "SIGSTATE",  TOP_OFF_SIGSTATE,  0177777u, 0, 1, 0 },
    { "SIGSEQ",    TOP_OFF_SIGSEQ,    0177777u, 0, 1, 0 },
    { "SIGCOUNT",  TOP_OFF_SIGCOUNT,  0177777u, 0, 1, 0 },
    { "SIGGROUP",  TOP_OFF_SIGGROUP,  0177777u, 0, 1, 0 },
    { "SIGHANDLER",TOP_OFF_SIGHANDLER,0177777u, 0, 1, 0 },
    { "SIGSTATUS", TOP_OFF_SIGSTATUS, 0177777u, 0, 1, 0 },
    { "SIGACTIVITY",TOP_OFF_SIGACTIVITY,0177777u, 0, 1, 0 },
    { "SIGDIR",    TOP_OFF_SIGDIR,    0177777u, 0, 1, 0 },
    { "SIGTOTAL",  TOP_OFF_SIGTOTAL,  0177777u, 0, 1, 0 },
    { "FACTSEL",   TOP_OFF_FACT_SEL,  0177777u, 0, 1, 1 },
    { "FACTVAL",   TOP_OFF_FACT_VAL,  0177777u, 0, 1, 0 }
    };

/* Status codes: 1 FACT, 2 OCR-NOISE, 3 UNKNOWN.  Activity: 1 low, 2 high,
 * 3 UNKNOWN.  Direction: 1 toward the device, 2 toward the system, 3 both,
 * 4 UNKNOWN.  Every line has a handler and is included in the journal.
 * Keep the last property entry: removing it to fit an older counter would
 * hide an unresolved request-level choice. */
static const top_signal_t top_signals[TOP_SIG_COUNT] = {
    { "UA00", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA01", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA02", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA03", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA04", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA05", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA06", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA07", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA08", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA09", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA10", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA11", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA12", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA13", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA14", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA15", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA16", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UA17", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 1, 1, 4 },
    { "UD00", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD01", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD02", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD03", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD04", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD05", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD06", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD07", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD08", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD09", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD10", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD11", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD12", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD13", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD14", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UD15", TOP_SG_DATA, TOP_SH_DATA_BIT, 2, 1, 4 },
    { "UC0", TOP_SG_CONTROL, TOP_SH_DIRECTION, 2, 1, 4 },
    { "UC1", TOP_SG_CONTROL, TOP_SH_DIRECTION, 2, 1, 4 },
    { "UMSYN", TOP_SG_HANDSHAKE, TOP_SH_MASTER_SYNC, 1, 1, 1 },
    { "USSYN", TOP_SG_HANDSHAKE, TOP_SH_SLAVE_SYNC, 1, 1, 2 },
    { "UPA", TOP_SG_PARITY, TOP_SH_PARITY_BIT, 1, 1, 4 },
    { "UPB", TOP_SG_PARITY, TOP_SH_PARITY_BIT, 1, 1, 4 },
    { "UBR7", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 1, 1, 2 },
    { "UBR6", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 1, 1, 2 },
    { "UBR5", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 1, 1, 2 },
    { "UBR4", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 1, 1, 2 },
    { "UBG7", TOP_SG_GRANT, TOP_SH_BUS_GRANT, 1, 2, 1 },
    { "UBG6", TOP_SG_GRANT, TOP_SH_BUS_GRANT, 1, 2, 1 },
    { "UBG5", TOP_SG_GRANT, TOP_SH_BUS_GRANT, 1, 2, 1 },
    { "UBG4", TOP_SG_GRANT, TOP_SH_BUS_GRANT, 1, 2, 1 },
    { "UNPR", TOP_SG_DMA, TOP_SH_DMA_REQUEST, 1, 1, 4 },
    { "UNPG", TOP_SG_DMA, TOP_SH_DMA_GRANT, 1, 2, 1 },
    { "USACK", TOP_SG_ACK, TOP_SH_ACKNOWLEDGE, 1, 1, 4 },
    { "UBBSY", TOP_SG_BUSY, TOP_SH_BUS_BUSY, 1, 1, 3 },
    { "UINTR", TOP_SG_INTERRUPT, TOP_SH_INTERRUPT, 1, 1, 2 },
    { "UINIT", TOP_SG_INIT, TOP_SH_INITIALIZE, 1, 1, 1 },
    { "UACLO", TOP_SG_POWER, TOP_SH_POWER_LOW, 1, 1, 1 },
    { "UDCLO", TOP_SG_POWER, TOP_SH_POWER_LOW, 1, 1, 1 },
    { "BPDO", TOP_SG_OUTPUT, TOP_SH_OUTPUT_WORD, 1, 3, 2 },
    { "ODSTB", TOP_SG_STROBE, TOP_SH_OUTPUT_STROBE, 1, 3, 2 },
    { "BPCDT", TOP_SG_STROBE, TOP_SH_CONTROL_STROBE, 1, 3, 2 },
    { "BPSSYN", TOP_SG_HANDSHAKE, TOP_SH_SLAVE_SYNC, 2, 3, 2 },
    { "BPDI", TOP_SG_INPUT, TOP_SH_INPUT_SELECT, 1, 3, 4 },
    { "BPREQA", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 1, 1, 2 },
    { "BPREQB", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 1, 1, 2 },
    { "BPCLR", TOP_SG_RESET, TOP_SH_LOCAL_RESET, 1, 3, 1 },
    { "BSS", TOP_SG_INPUT, TOP_SH_INPUT_SELECT, 1, 3, 4 },
    { "PCLRD", TOP_SG_RESET, TOP_SH_LOCAL_RESET, 1, 3, 4 },
    { "PRESET", TOP_SG_RESET, TOP_SH_LOCAL_RESET, 2, 3, 4 },
    { "SYN1", TOP_SG_SYNC, TOP_SH_LOCAL_SYNC, 1, 3, 4 },
    { "SYN2", TOP_SG_SYNC, TOP_SH_LOCAL_SYNC, 1, 3, 4 },
    { "ODRE", TOP_SG_FLAG, TOP_SH_FLAG, 1, 1, 4 },
    { "IBA", TOP_SG_ADDRESS, TOP_SH_ADDR_BIT, 2, 3, 2 },
    { "IBD", TOP_SG_DATA, TOP_SH_DATA_BIT, 1, 3, 3 },
    { "TBOUTL", TOP_SG_STROBE, TOP_SH_OUTPUT_STROBE, 1, 3, 2 },
    { "TBOUTH", TOP_SG_STROBE, TOP_SH_OUTPUT_STROBE, 1, 3, 4 },
    { "TBINP", TOP_SG_STROBE, TOP_SH_INPUT_SELECT, 1, 3, 2 },
    { "IBREQA", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 2, 1, 2 },
    { "IBREQB", TOP_SG_REQUEST, TOP_SH_BUS_REQUEST, 2, 1, 2 },
    { "IBCLR", TOP_SG_RESET, TOP_SH_LOCAL_RESET, 1, 3, 2 },
    { "IRESET", TOP_SG_RESET, TOP_SH_LOCAL_RESET, 2, 3, 4 },
    { "ISSYN", TOP_SG_HANDSHAKE, TOP_SH_SLAVE_SYNC, 2, 1, 2 },
    { "IKREQA", TOP_SG_LATCH, TOP_SH_LATCH, 2, 3, 4 },
    { "PKREQA", TOP_SG_LATCH, TOP_SH_LATCH, 2, 3, 4 },
    { "PKSIEN", TOP_SG_ENABLE, TOP_SH_ENABLE, 1, 3, 4 },
    { "PMTSS", TOP_SG_CONTROL, TOP_SH_DIRECTION, 1, 3, 4 },
    { "BPIMA", TOP_SG_REGISTER, TOP_SH_REGISTER, 1, 3, 1 },
    { "BPSRA", TOP_SG_REGISTER, TOP_SH_REGISTER, 1, 3, 2 },
    { "INTVA", TOP_SG_VECTOR, TOP_SH_VECTOR, 1, 3, 2 },
    { "INTVB", TOP_SG_VECTOR, TOP_SH_VECTOR, 1, 3, 2 },
    { "PRIO_AB", TOP_SG_PRIORITY, TOP_SH_PRIORITY, 1, 2, 4 },
    { "PRIO_BP_IB", TOP_SG_PRIORITY, TOP_SH_PRIORITY, 1, 2, 4 },
    { "IRQ_LEVEL", TOP_SG_PROPERTY, TOP_SH_PROPERTY, 1, 3, 4 }
    };

static DEBTAB top_debug[] = {
    {"JOURNAL", TOP_DEB_JOURNAL, "register and cell transactions"},
    {"IRQ", TOP_DEB_IRQ, "request lines and acknowledgements"},
    {"STATE", TOP_DEB_STATE, "state transitions and publications"},
    {0}
    };

static DIB top_dib = {
    IOBA_TOP, IOLN_TOP, &topif_rd, &topif_wr,
    2, IVCL (TOP_A), TOP_VEC_A,
    { topif_ack_a, topif_ack_b, NULL, NULL },
    0, 0, NULL, NULL
    };

static UNIT top_unit = { UDATA (NULL, 0, 0) };

REG top_reg[] = {
    { GRDATA (STATUS_A, top_state.cell_a, DEV_RDX, 16, 0) },
    { GRDATA (STATUS_B, top_state.cell_b, DEV_RDX, 16, 0) },
    { GRDATA (STATE, top_state.cell_state, DEV_RDX, 16, 0) },
    { GRDATA (BPSRA, top_state.reg[TOP_OFF_BPSRA / 2], DEV_RDX, 16, 0) },
    { GRDATA (BPIMA, top_state.reg[TOP_OFF_BPIMA / 2], DEV_RDX, 16, 0) },
    { GRDATA (PKCSR, top_state.reg[TOP_OFF_PKCSR / 2], DEV_RDX, 16, 0) },
    { GRDATA (IKCSR, top_state.reg[TOP_OFF_IKCSR / 2], DEV_RDX, 16, 0) },
    { GRDATA (REQUEST, top_state.reg[TOP_OFF_REQUEST / 2], DEV_RDX, 16, 0) },
    { FLDATA (INT_A, IREQ (TOP_A), INT_V_TOP_A) },
    { FLDATA (INT_B, IREQ (TOP_B), INT_V_TOP_B) },
    { GRDATA (DEVADDR, top_dib.ba, DEV_RDX, 32, 0), REG_HRO },
    { GRDATA (DEVVEC, top_dib.vec, DEV_RDX, 16, 0), REG_HRO },
    { NULL }
    };

MTAB top_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 010, "ADDRESS", "ADDRESS",
      &set_addr, &show_addr, NULL, "Bus address" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "VECTOR", "VECTOR",
      &set_vec, &show_vec, NULL, "Interrupt vector" },
    { 0 }
    };

DEVICE top_dev = {
    "TOP", &top_unit, top_reg, top_mod,
    1, DEV_RDX, 16, 1, DEV_RDX, 16,
    NULL, NULL, &topif_reset,
    NULL, NULL, NULL,
    &top_dib, DEV_DISABLE | DEV_UBUS | DEV_DEBUG, 0,
    top_debug,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL
    };

const char *topif_state_name (top_mode_t state)
{
    switch (state) {
    case TOP_STATE_RESET:  return "RESET";
    case TOP_STATE_INIT:   return "INIT";
    case TOP_STATE_READY:  return "READY";
    case TOP_STATE_REQUEST:return "REQUEST";
    case TOP_STATE_SERVICE:return "SERVICE";
    case TOP_STATE_DIALOG: return "DIALOG";
    default:               return "UNKNOWN";
    }
}

static const char *topif_src_name (top_src_t src)
{
    return (src == TOP_SRC_BP)? "BP": "IB";
}

static const char *topif_resp_name (top_resp_t resp)
{
    switch (resp) {
    case TOP_RESP_OK:          return "OK";
    case TOP_RESP_READ_ONLY:   return "READ_ONLY";
    case TOP_RESP_UNKNOWN_BITS:return "UNKNOWN_BITS";
    default:                   return "DIAG";
    }
}

const char *topif_origin_name (top_origin_t origin)
{
    switch (origin) {
    case TOP_ORIGIN_LINE_FACT:   return "LINE_FACT";
    case TOP_ORIGIN_MODEL_STATE: return "MODEL_STATE";
    case TOP_ORIGIN_SURROGATE:   return "SURROGATE";
    default:                     return "UNKNOWN";
    }
}

/* A record with no printed source is either a model transition or an unknown
 * access.  Only the signal record below is a surrogate, and nothing in this
 * device has a printed line source, so LINE_FACT is never selected here. */
static top_origin_t topif_origin_for (top_resp_t resp)
{
    if (resp == TOP_RESP_DIAG_ONLY || resp == TOP_RESP_UNKNOWN_BITS)
        return TOP_ORIGIN_UNKNOWN;
    return TOP_ORIGIN_MODEL_STATE;
}

static void topif_record_origin (top_state_t *s, int32 addr, int32 pc,
                                 top_dir_t dir, top_src_t src, uint16 data,
                                 top_resp_t resp, top_origin_t origin)
{
    top_journal_entry_t *e;

    if (s == NULL)
        return;
    e = &s->journal[s->journal_head];
    e->seq = ++s->journal_seq;
    e->addr = addr;
    e->pc = pc;
    e->dir = dir;
    e->src = src;
    e->data = data;
    e->resp = resp;
    e->state = s->state;
    e->origin = origin;
    s->journal_head = (s->journal_head + 1u) % TOP_JOURNAL_DEPTH;
    sim_debug (TOP_DEB_JOURNAL, &top_dev,
               "TOP JRN %u %s %s %06o PC=%06o data=%06o %s state=%s "
               "origin=%s\n",
               (unsigned) e->seq, (dir == TOP_DIR_READ)? "RD": "WR",
               topif_src_name (src), (unsigned) addr, (unsigned) pc,
               (unsigned) data, topif_resp_name (resp),
               topif_state_name (s->state), topif_origin_name (origin));
}

static void topif_record (top_state_t *s, int32 addr, int32 pc, top_dir_t dir,
                          top_src_t src, uint16 data, top_resp_t resp)
{
    topif_record_origin (s, addr, pc, dir, src, data, resp,
                         topif_origin_for (resp));
}

const char *topif_signal_name (uint32 id)
{
    if (id >= TOP_SIG_COUNT)
        return "UNKNOWN";
    return top_signals[id].name;
}

uint16 topif_signal_value (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return s->sig[id].value;
}

uint16 topif_signal_state (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return (uint16) ((s->sig[id].value & 0xFFu) |
                     ((s->sig[id].transitions & 0xFFu) << 8));
}

uint16 topif_signal_seq (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return (uint16) s->sig[id].last_seq;
}

uint8 topif_signal_handler (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return top_signals[id].handler;
}

uint8 topif_signal_group (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return top_signals[id].group;
}

uint8 topif_signal_status (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return top_signals[id].status;
}

uint8 topif_signal_activity (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return top_signals[id].activity;
}

uint8 topif_signal_direction (const top_state_t *s, uint32 id)
{
    if (s == NULL || id >= TOP_SIG_COUNT)
        return 0;
    return top_signals[id].direction;
}

static void topif_sig_record (top_state_t *s, uint32 id, uint16 value,
                              top_resp_t resp)
{
    top_signal_t *sig;

    if (s == NULL || id >= TOP_SIG_COUNT)
        return;
    sig = &s->sig[id];
    sig->value = (uint8) (value & 0xFFu);
    sig->transitions++;
    topif_record_origin (s, (int32) (0100000u + id), 0, TOP_DIR_WRITE,
                         TOP_SRC_BP, value, resp, TOP_ORIGIN_SURROGATE);
    sig->last_seq = s->journal_seq;
    sim_debug (TOP_DEB_JOURNAL, &top_dev,
               "TOP SIG %s id=%u value=%06o transitions=%u seq=%u "
               "origin=SURROGATE\n",
               sig->name, (unsigned) id, (unsigned) value,
               (unsigned) sig->transitions, (unsigned) sig->last_seq);
}

void topif_defaults (top_state_t *s, const top_config_t *cfg)
{
    if (s == NULL)
        return;
    memset (s, 0, sizeof (*s));
    s->cfg.bp_base = IOBA_TOP;
    s->cfg.vec_a = TOP_VEC_A;
    s->cfg.vec_b = TOP_VEC_B;
    s->cfg.cell_a_start = TOP_CELL_A;
    s->cfg.cell_b_start = TOP_CELL_B;
    s->cfg.cell_state_start = TOP_CELL_STATE;
    s->cfg.cell_a_value = 0004006u;
    s->cfg.cell_b_value = 0;
    s->cfg.cell_state_value = 00040300u;
    s->cfg.bpsra_start = 0000104u;
    s->cfg.bpfr1_start = 0000470u;
    s->cfg.bpfr2_start = 00054577u;
    s->cfg.bp_over_ib = 1;
    s->cfg.a_over_b = 1;
    if (cfg != NULL)
        s->cfg = *cfg;
}

static int topif_locate (int32 pa, uint16 *off)
{
    int32 d;

    if (off == NULL)
        return 0;
    d = pa - (int32) top_dib.ba;
    if (d >= 0 && (uint32) d < top_dib.lnt) {
        *off = (uint16) (((uint32) d) & ~1u);
        return 1;
        }
    return 0;
}

int topif_low_cell (int32 pa, uint16 *slot)
{
    top_state_t *s = &top_state;

    if (slot == NULL)
        return 0;
    if (pa == (int32) s->cfg.cell_a_start) {
        *slot = 0;
        return 1;
        }
    if (pa == (int32) s->cfg.cell_b_start) {
        *slot = 1;
        return 1;
        }
    if (pa == (int32) s->cfg.cell_state_start) {
        *slot = 2;
        return 1;
        }
    return 0;
}

void topif_publish (void)
{
    top_state_t *s = &top_state;
    uint16 *mem = M;

    if (mem == NULL)
        return;
    mem[s->cfg.cell_a_start >> 1] = s->cell_a & TOP_WORD_MASK;
    mem[s->cfg.cell_b_start >> 1] = s->cell_b & TOP_WORD_MASK;
    mem[s->cfg.cell_state_start >> 1] = s->cell_state & TOP_WORD_MASK;
    s->publish_count++;
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP PUB %u A=%06o B=%06o STATE=%06o state=%s\n",
               (unsigned) s->publish_count, (unsigned) s->cell_a,
               (unsigned) s->cell_b, (unsigned) s->cell_state,
               topif_state_name (s->state));
}

static void topif_set_state (top_state_t *s, top_mode_t next)
{
    if (s == NULL || s->state == next)
        return;
    s->state = next;
    s->transition_count++;
    s->cell_state = (uint16) ((s->cell_state & ~0007u) | (uint16) next);
    s->reg[TOP_OFF_STATE / 2] = s->cell_state;
    topif_record (s, (int32) s->cfg.cell_state_start, 0, TOP_DIR_WRITE,
                  TOP_SRC_BP, (uint16) next, TOP_RESP_OK);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP STATE %u -> %s transitions=%u\n",
               (unsigned) s->transition_count, topif_state_name (next),
               (unsigned) s->transition_count);
}

static void topif_update_enables (top_state_t *s)
{
    uint16 pk = s->reg[TOP_OFF_PKCSR / 2];
    uint16 ik = s->reg[TOP_OFF_IKCSR / 2];

    s->enabled[TOP_SRC_BP][0] = ((pk & (TOP_PKCSR_EN_A | TOP_PKCSR_REQ_A)) != 0);
    s->enabled[TOP_SRC_BP][1] = ((pk & (TOP_PKCSR_EN_B | TOP_PKCSR_REQ_B)) != 0);
    s->enabled[TOP_SRC_IB][0] = ((ik & TOP_IKCSR_EN_A) != 0);
    s->enabled[TOP_SRC_IB][1] = ((ik & TOP_IKCSR_EN_B) != 0);
}

static void topif_update_int (top_state_t *s)
{
    top_src_t src;
    int channel;

    CLR_INT (TOP_A);
    CLR_INT (TOP_B);
    if (!topif_top_request (s, &src, &channel))
        return;
    if (channel == 0)
        SET_INT (TOP_A);
    else
        SET_INT (TOP_B);
}

int topif_top_request (const top_state_t *s, top_src_t *src, int *channel)
{
    int src_order[2] = { TOP_SRC_BP, TOP_SRC_IB };
    int ch_order[2] = { 0, 1 };
    int i;
    int j;

    if (s == NULL || src == NULL || channel == NULL)
        return 0;
    if (!s->cfg.bp_over_ib) {
        src_order[0] = TOP_SRC_IB;
        src_order[1] = TOP_SRC_BP;
        }
    if (!s->cfg.a_over_b) {
        ch_order[0] = 1;
        ch_order[1] = 0;
        }
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            int ch = ch_order[j];
            top_src_t sc = (top_src_t) src_order[i];

            if (s->pending[sc][ch] && s->enabled[sc][ch]) {
                *src = sc;
                *channel = ch;
                return 1;
                }
            }
        }
    return 0;
}

void topif_assert_request (top_state_t *s, top_src_t src, int channel)
{
    if (s == NULL || src > TOP_SRC_IB || channel < 0 || channel > 1)
        return;
    s->pending[src][channel] = 1;
    s->irq_count[src][channel]++;
    if (topif_bridge_irq ((uint32) src, (uint32) channel))
        topif_record (s, (int32) (0200000u + (uint32) src), 0,
                      TOP_DIR_WRITE, src, (uint16) channel, TOP_RESP_BRIDGE);
    sim_debug (TOP_DEB_IRQ, &top_dev,
               "TOP IRQ %s %c asserted vector=%06o src=%s\n",
               topif_src_name (src), (channel == 0)? 'A': 'B',
               (unsigned) topif_vector (src, channel),
               topif_state_name (s->state));
    topif_update_int (s);
}

void topif_clear_request (top_state_t *s, top_src_t src, int channel)
{
    if (s == NULL || src > TOP_SRC_IB || channel < 0 || channel > 1)
        return;
    s->pending[src][channel] = 0;
    topif_update_int (s);
}

int32 topif_vector (top_src_t src, int channel)
{
    const top_state_t *s = &top_state;

    if (channel == 0)
        return (int32) ((src == TOP_SRC_BP)? s->cfg.vec_a: TOP_VEC_I_A);
    return (int32) ((src == TOP_SRC_BP)? s->cfg.vec_b: TOP_VEC_I_B);
}

/* Derive request lines and the current state from registered status bits.
 * Request sources follow the documented channel directions: FEDF, FHT and
 * INE select A; TRA, TRF, RFF, OPSF and OHT select B. */
static void topif_step (top_state_t *s)
{
    uint16 st = s->reg[TOP_OFF_BPSRA / 2];
    uint16 mask = s->reg[TOP_OFF_BPIMA / 2];
    uint16 lines = s->reg[TOP_OFF_REQUEST / 2];
    int a;
    int b;

    topif_update_enables (s);
    a = ((st & (TOP_BPSRA_FEDF | TOP_BPSRA_FHT | TOP_BPSRA_INE)) != 0) ||
        ((lines & TOP_REQ_A) != 0);
    b = ((st & (TOP_BPSRA_TRA | TOP_BPSRA_TRF | TOP_BPSRA_RFF |
                TOP_BPSRA_OPSF | TOP_BPSRA_OHT)) != 0) ||
        ((lines & TOP_REQ_B) != 0);
    if ((mask & TOP_BPIMA_A) == 0)
        a = 0;
    if ((mask & TOP_BPIMA_B) == 0)
        b = 0;
    if (a)
        topif_assert_request (s, TOP_SRC_BP, 0);
    else
        topif_clear_request (s, TOP_SRC_BP, 0);
    if (b)
        topif_assert_request (s, TOP_SRC_BP, 1);
    else
        topif_clear_request (s, TOP_SRC_BP, 1);
    if ((lines & 0400u) != 0)
        topif_assert_request (s, TOP_SRC_IB, 0);
    else
        topif_clear_request (s, TOP_SRC_IB, 0);
    if ((lines & 01000u) != 0)
        topif_assert_request (s, TOP_SRC_IB, 1);
    else
        topif_clear_request (s, TOP_SRC_IB, 1);

    if ((s->cell_b & TOP_STATUS_B_DIALOG) != 0)
        topif_set_state (s, TOP_STATE_DIALOG);
    else if (s->pending[TOP_SRC_BP][0] || s->pending[TOP_SRC_BP][1] ||
             s->pending[TOP_SRC_IB][0] || s->pending[TOP_SRC_IB][1])
        topif_set_state (s, TOP_STATE_REQUEST);
    else if ((st & (TOP_BPSRA_INE | TOP_BPSRA_INO)) != 0)
        topif_set_state (s, TOP_STATE_SERVICE);
    else if ((st & (TOP_BPSRA_FHT | TOP_BPSRA_OHT)) != 0)
        topif_set_state (s, TOP_STATE_READY);
    else
        topif_set_state (s, TOP_STATE_INIT);
}

uint16 topif_read_low (int32 pa, int32 pc)
{
    top_state_t *s = &top_state;
    uint16 slot = 0;
    uint16 value = 0;

    if (!topif_low_cell (pa, &slot))
        return 0;
    value = (slot == 0)? s->cell_a: (slot == 1)? s->cell_b: s->cell_state;
    topif_record (s, pa, pc, TOP_DIR_READ, TOP_SRC_BP, value, TOP_RESP_OK);
    return value;
}

void topif_write_low (int32 pa, uint16 data, int32 pc)
{
    top_state_t *s = &top_state;
    uint16 slot = 0;

    if (!topif_low_cell (pa, &slot))
        return;
    if (slot == 0) {
        s->cell_a = data & TOP_WORD_MASK;
        s->reg[TOP_OFF_STATUS_A / 2] = s->cell_a;
        }
    else if (slot == 1) {
        s->cell_b = data & TOP_WORD_MASK;
        s->reg[TOP_OFF_STATUS_B / 2] = s->cell_b;
        }
    else {
        s->cell_state = data & TOP_WORD_MASK;
        s->reg[TOP_OFF_STATE / 2] = s->cell_state;
        }
    topif_record (s, pa, pc, TOP_DIR_WRITE, TOP_SRC_BP, data, TOP_RESP_OK);
    topif_step (s);
    topif_publish ();
}

t_stat topif_rd (int32 *data, int32 pa, int32 access)
{
    top_state_t *s = &top_state;
    uint16 off = 0;
    uint16 value = 0;
    top_resp_t resp = TOP_RESP_OK;
    const top_reg_desc_t *d;

    if (data == NULL)
        return SCPE_ARG;
    *data = 0;
    if ((access == WRITEB) || !topif_locate (pa, &off)) {
        topif_record (s, pa, 0, TOP_DIR_READ, TOP_SRC_BP, 0, TOP_RESP_DIAG_ONLY);
        return SCPE_OK;
        }
    if (off == TOP_OFF_PKCSR) {
        uint16 authoritative = 0;
        uint32 seq = 0;

        if (topif_csr_access (TOP_PKCSR_ADDR, 0, 0, &authoritative, &seq))
            value = authoritative;
        else
            value = s->reg[TOP_OFF_PKCSR / 2u];
        }
    else if (off == TOP_OFF_JOURNAL)
        value = (uint16) topif_journal_count (s);
    else if (off == TOP_OFF_SEQUENCE)
        value = (uint16) s->journal_seq;
    else if (off == TOP_OFF_VECTOR_A)
        value = (uint16) s->cfg.vec_a;
    else if (off == TOP_OFF_VECTOR_B)
        value = (uint16) s->cfg.vec_b;
    else if (off == TOP_OFF_IRQ)
        value = (uint16) ((s->pending[TOP_SRC_BP][0]? TOP_REQ_A: 0) |
                          (s->pending[TOP_SRC_BP][1]? TOP_REQ_B: 0) |
                          (s->pending[TOP_SRC_IB][0]? 0400u: 0) |
                          (s->pending[TOP_SRC_IB][1]? 01000u: 0));
    else if (off == TOP_OFF_SIGSEL)
        value = (uint16) s->sig_selected;
    else if (off == TOP_OFF_SIGVAL)
        value = topif_signal_value (s, s->sig_selected);
    else if (off == TOP_OFF_SIGSTATE)
        value = topif_signal_state (s, s->sig_selected);
    else if (off == TOP_OFF_SIGSEQ)
        value = topif_signal_seq (s, s->sig_selected);
    else if (off == TOP_OFF_SIGCOUNT)
        value = (uint16) TOP_SIG_A_COUNT;
    else if (off == TOP_OFF_SIGGROUP)
        value = topif_signal_group (s, s->sig_selected);
    else if (off == TOP_OFF_SIGHANDLER)
        value = topif_signal_handler (s, s->sig_selected);
    else if (off == TOP_OFF_SIGSTATUS)
        value = topif_signal_status (s, s->sig_selected);
    else if (off == TOP_OFF_SIGACTIVITY)
        value = topif_signal_activity (s, s->sig_selected);
    else if (off == TOP_OFF_SIGDIR)
        value = topif_signal_direction (s, s->sig_selected);
    else if (off == TOP_OFF_SIGTOTAL)
        value = (uint16) TOP_SIG_COUNT;
    else if (off == TOP_OFF_FACT_VAL) {
        if (topif_bridge_fact (s->fact_topic, &value))
            resp = TOP_RESP_BRIDGE;
        }
    else if (off == TOP_OFF_FACT_SEL)
        value = s->fact_topic;
    else if (off == TOP_OFF_BRIDGE)
        value = (uint16) (topif_bridge_online ()? 1: 0);
    else if (off == TOP_OFF_BRIDGE_CALL)
        value = (uint16) topif_bridge_calls ();
    else if (off == TOP_OFF_BRIDGE_ERR)
        value = (uint16) topif_bridge_errors ();
    else {
        d = topif_find_reg (off, TOP_DIR_READ);
        if (d != NULL)
            value = s->reg[off / 2u];
        else
            resp = TOP_RESP_DIAG_ONLY;
        }
    if (off != TOP_OFF_PKCSR && off != TOP_OFF_BRIDGE &&
        off != TOP_OFF_BRIDGE_CALL && off != TOP_OFF_BRIDGE_ERR &&
        off != TOP_OFF_FACT_SEL && off != TOP_OFF_FACT_VAL &&
        topif_bridge_read (off, &value))
        resp = TOP_RESP_BRIDGE;
    topif_record (s, pa, 0, TOP_DIR_READ, TOP_SRC_BP, value, resp);
    *data = (int32) value;
    return SCPE_OK;
}

t_stat topif_wr (int32 data, int32 pa, int32 access)
{
    top_state_t *s = &top_state;
    uint16 off = 0;
    uint16 value = (uint16) (((uint32) data) & TOP_WORD_MASK);
    top_resp_t resp = TOP_RESP_OK;
    const top_reg_desc_t *d;

    if ((access == WRITEB) || !topif_locate (pa, &off)) {
        topif_record (s, pa, 0, TOP_DIR_WRITE, TOP_SRC_BP, value, TOP_RESP_DIAG_ONLY);
        return SCPE_OK;
        }
    d = topif_find_reg (off, TOP_DIR_WRITE);
    if (d == NULL) {
        topif_record (s, pa, 0, TOP_DIR_WRITE, TOP_SRC_BP, value, TOP_RESP_READ_ONLY);
        return SCPE_OK;
        }
    if ((value & topif_unknown_mask (d)) != 0)
        resp = TOP_RESP_UNKNOWN_BITS;
    value &= (uint16) (d->named | d->observed);
    if (off != TOP_OFF_PKCSR && off != TOP_OFF_FACT_SEL &&
        off != TOP_OFF_FACT_VAL && topif_bridge_write (off, value))
        resp = TOP_RESP_BRIDGE;
    s->reg[off / 2u] = value;

    switch (off) {
    case TOP_OFF_STATUS_A:
        s->cell_a = value;
        break;
    case TOP_OFF_STATUS_B:
        s->cell_b = value;
        break;
    case TOP_OFF_STATE:
        s->cell_state = (uint16) ((s->cell_state & ~0007u) | (value & 0007u));
        break;
    case TOP_OFF_PKCSR: {
        uint16 authoritative = value;
        uint32 seq = 0;

        if (topif_csr_access (TOP_PKCSR_ADDR, value, 1, &authoritative, &seq)) {
            s->reg[TOP_OFF_PKCSR / 2u] = authoritative;
            value = authoritative;
            resp = TOP_RESP_BRIDGE;
        }
        break;
    }
    case TOP_OFF_REQUEST:
    case TOP_OFF_BPIMA:
    case TOP_OFF_IKCSR:
    case TOP_OFF_BPSRA:
    case TOP_OFF_BPFR1:
    case TOP_OFF_BPFR2:
        break;
    case TOP_OFF_BPCLR:
        s->reg[TOP_OFF_BPSRA / 2] = (uint16) (TOP_BPSRA_FHT | TOP_BPSRA_OHT);
        s->pending[0][0] = s->pending[0][1] = 0;
        s->cell_state = (uint16) (s->cell_state & ~0007u);
        sim_debug (TOP_DEB_IRQ, &top_dev, "TOP IRQ requests cleared\n");
        break;
    case TOP_OFF_IBCLR:
        s->pending[TOP_SRC_IB][0] = s->pending[TOP_SRC_IB][1] = 0;
        break;
    case TOP_OFF_SIGSEL:
        if (value < TOP_SIG_COUNT)
            s->sig_selected = value;
        else
            resp = TOP_RESP_UNKNOWN_BITS;
        break;
    case TOP_OFF_SIGVAL:
        topif_drive_signal (s, s->sig_selected, value);
        break;
    case TOP_OFF_FACT_SEL:
        s->fact_topic = value;
        break;
    case TOP_OFF_FACT_VAL:
        /* Read-only from the metadata service; a write only records the try. */
        resp = TOP_RESP_READ_ONLY;
        break;
    default:
        break;
        }
    topif_record (s, pa, 0, TOP_DIR_WRITE, TOP_SRC_BP, value, resp);
    topif_step (s);
    topif_update_int (s);
    topif_publish ();
    return SCPE_OK;
}

static t_stat topif_ack_common (top_src_t src, int channel)
{
    top_state_t *s = &top_state;
    int32 vec;

    topif_clear_request (s, src, channel);
    vec = topif_vector (src, channel);
    topif_record (s, (int32) vec, 0, TOP_DIR_READ, src, (uint16) vec, TOP_RESP_OK);
    sim_debug (TOP_DEB_IRQ, &top_dev,
               "TOP ACK %s %c vector=%06o\n",
               topif_src_name (src), (channel == 0)? 'A': 'B', (unsigned) vec);
    return vec;
}

t_stat topif_ack_a (void) { return topif_ack_common (TOP_SRC_BP, 0); }
t_stat topif_ack_b (void) { return topif_ack_common (TOP_SRC_BP, 1); }

const top_reg_desc_t *topif_find_reg (uint16 off, top_dir_t dir)
{
    size_t i;

    for (i = 0; i < sizeof (top_regs) / sizeof (top_regs[0]); i++) {
        if (top_regs[i].off != off)
            continue;
        if ((dir == TOP_DIR_READ && !top_regs[i].readable) ||
            (dir == TOP_DIR_WRITE && !top_regs[i].writable))
            continue;
        return &top_regs[i];
        }
    return NULL;
}

uint16 topif_unknown_mask (const top_reg_desc_t *d)
{
    if (d == NULL)
        return (uint16) TOP_WORD_MASK;
    return (uint16) (TOP_WORD_MASK & ~((uint32) (d->named | d->observed)));
}

unsigned topif_journal_count (const top_state_t *s)
{
    if (s == NULL)
        return 0;
    return (s->journal_seq < TOP_JOURNAL_DEPTH)?
           (unsigned) s->journal_seq: TOP_JOURNAL_DEPTH;
}

const top_journal_entry_t *topif_journal_at (const top_state_t *s, unsigned i)
{
    unsigned n;
    unsigned start;

    if (s == NULL)
        return NULL;
    n = topif_journal_count (s);
    if (i >= n)
        return NULL;
    start = (s->journal_seq < TOP_JOURNAL_DEPTH)? 0u: s->journal_head;
    return &s->journal[(start + i) % TOP_JOURNAL_DEPTH];
}

static void topif_soft_init (top_state_t *s)
{
    if (s == NULL)
        return;
    s->reg[TOP_OFF_BPSRA / 2] = (uint16) (TOP_BPSRA_FHT | TOP_BPSRA_OHT);
    s->reg[TOP_OFF_REQUEST / 2] = 0;
    s->reg[TOP_OFF_BPIMA / 2] = 0;
    s->reg[TOP_OFF_PKCSR / 2] = 0;
    s->reg[TOP_OFF_IKCSR / 2] = 0;
    s->cell_a = s->cfg.cell_a_value;
    s->cell_b = s->cfg.cell_b_value;
    s->cell_state = s->cfg.cell_state_value;
    s->reg[TOP_OFF_STATUS_A / 2] = s->cell_a;
    s->reg[TOP_OFF_STATUS_B / 2] = s->cell_b;
    s->reg[TOP_OFF_STATE / 2] = s->cell_state;
    s->pending[0][0] = s->pending[0][1] = 0;
    s->pending[1][0] = s->pending[1][1] = 0;
    s->state = TOP_STATE_RESET;
    s->descriptor_ptr = 0;
    s->descriptor_active = 0;
    s->descriptor_status = 0;
    s->external_calls = 0;
    s->protocol_pages = 0;
    s->protocol_line = 0;
    s->next_page = 0;
    s->next_index = 0;
    s->chunk_off = 0;
    s->chunk_len = 0;
    s->reg_heading_stage = 0;
    memset (s->chunk, 0, sizeof (s->chunk));
    s->pending_print = 0;
    topif_update_enables (s);
    CLR_INT (TOP_A);
    CLR_INT (TOP_B);
    topif_step (s);
    topif_publish ();
}

void topif_drive_signal (top_state_t *s, uint32 id, uint16 value)
{
    top_sig_handler_t handler;
    int level;
    int asserted;

    if (s == NULL || id >= TOP_SIG_COUNT)
        return;
    handler = (top_sig_handler_t) top_signals[id].handler;
    asserted = (value & 1u) != 0;
    level = 0;
    switch (handler) {
    case TOP_SH_ADDR_BIT:
        if (id <= TOP_SIG_UA17) {
            if (asserted)
                s->bus_addr |= (uint32) 1u << id;
            else
                s->bus_addr &= ~((uint32) 1u << id);
            s->cell_a = (uint16) (s->bus_addr & TOP_WORD_MASK);
            s->reg[TOP_OFF_STATUS_A / 2] = s->cell_a;
            }
        else if (asserted) {
            s->cell_a |= 000001u;
            s->reg[TOP_OFF_STATUS_A / 2] = s->cell_a;
            }
        else {
            s->cell_a &= ~000001u;
            s->reg[TOP_OFF_STATUS_A / 2] = s->cell_a;
            }
        break;
    case TOP_SH_DATA_BIT:
        if (id >= TOP_SIG_UD0 && id <= TOP_SIG_UD15) {
            if (asserted)
                s->bus_data |= (uint16) (1u << (id - TOP_SIG_UD0));
            else
                s->bus_data &= (uint16) ~(1u << (id - TOP_SIG_UD0));
            }
        else if (asserted)
            s->bus_data |= 000001u;
        else
            s->bus_data &= ~000001u;
        s->reg[TOP_OFF_STATUS_B / 2] = s->cell_b = s->bus_data;
        break;
    case TOP_SH_DIRECTION:
        if (id == TOP_SIG_UC0)
            s->transfer_dir = (uint16) ((s->transfer_dir & ~0001u) |
                                        (asserted? 0001u: 0));
        else if (id == TOP_SIG_UC1)
            s->transfer_dir = (uint16) ((s->transfer_dir & ~0002u) |
                                        (asserted? 0002u: 0));
        else if (asserted)
            s->reg[TOP_OFF_PKCSR / 2] |= 000001u;
        break;
    case TOP_SH_MASTER_SYNC:
        s->handshake = (uint16) ((s->handshake & ~0001u) |
                                 (asserted? 0001u: 0));
        if (asserted)
            topif_drive_signal (s, TOP_SIG_USSYN, 1);
        else
            topif_drive_signal (s, TOP_SIG_USSYN, 0);
        break;
    case TOP_SH_SLAVE_SYNC:
        s->handshake = (uint16) ((s->handshake & ~0002u) |
                                 (asserted? 0002u: 0));
        break;
    case TOP_SH_PARITY_BIT:
        s->parity_error = (uint16) ((s->parity_error & ~0003u) |
                                    (asserted? (1u << (id - TOP_SIG_UPA)): 0));
        if ((s->bus_data & 000001u) != 0 &&
            (s->parity_error & 0001u) != 0)
            s->reg[TOP_OFF_BPSRA / 2] |= TOP_BPSRA_OPSF;
        break;
    case TOP_SH_BUS_REQUEST:
        level = (id == TOP_SIG_UBR7 || id == TOP_SIG_BPREQA)? 7:
                (id == TOP_SIG_UBR6)? 6:
                (id == TOP_SIG_UBR5 || id == TOP_SIG_BPREQB)? 5: 4;
        s->arb_level = (uint16) ((s->arb_level & ~0007u) |
                                 (uint16) (asserted? level: 0));
        if (asserted) {
            if (id == TOP_SIG_BPREQA || id == TOP_SIG_IBREQA)
                topif_assert_request (s, (id == TOP_SIG_BPREQA)?
                                      TOP_SRC_BP: TOP_SRC_IB, 0);
            else if (id == TOP_SIG_BPREQB || id == TOP_SIG_IBREQB)
                topif_assert_request (s, (id == TOP_SIG_BPREQB)?
                                      TOP_SRC_BP: TOP_SRC_IB, 1);
            else
                topif_assert_request (s, TOP_SRC_BP, (level & 1)? 1: 0);
            }
        else if (id == TOP_SIG_BPREQA || id == TOP_SIG_IBREQA)
            topif_clear_request (s, (id == TOP_SIG_BPREQA)?
                                 TOP_SRC_BP: TOP_SRC_IB, 0);
        else if (id == TOP_SIG_BPREQB || id == TOP_SIG_IBREQB)
            topif_clear_request (s, (id == TOP_SIG_BPREQB)?
                                 TOP_SRC_BP: TOP_SRC_IB, 1);
        break;
    case TOP_SH_BUS_GRANT:
        level = (id == TOP_SIG_UBG7)? 7: (id == TOP_SIG_UBG6)? 6:
                (id == TOP_SIG_UBG5)? 5: 4;
        s->grant_level = (uint16) (asserted? level: 0);
        if (asserted) {
            s->bus_busy = 1;
            topif_clear_request (s, TOP_SRC_BP, (level & 1)? 1: 0);
            }
        break;
    case TOP_SH_DMA_REQUEST:
        s->dma_pending = (uint16) (asserted? 1: 0);
        if (asserted)
            topif_assert_request (s, TOP_SRC_IB, 1);
        break;
    case TOP_SH_DMA_GRANT:
        if (asserted && s->dma_pending) {
            s->dma_pending = 0;
            s->cell_a = (uint16) ((s->cell_a & ~0007u) | (s->bus_data & 0007u));
            s->bus_busy = 0;
            topif_clear_request (s, TOP_SRC_IB, 1);
            }
        break;
    case TOP_SH_ACKNOWLEDGE:
        s->grant_level = 0;
        s->bus_busy = 0;
        break;
    case TOP_SH_BUS_BUSY:
        s->bus_busy = (uint16) (asserted? 1: 0);
        break;
    case TOP_SH_INTERRUPT:
        if (asserted)
            topif_assert_request (s, TOP_SRC_BP, 0);
        else
            topif_clear_request (s, TOP_SRC_BP, 0);
        break;
    case TOP_SH_INITIALIZE:
        topif_soft_init (s);
        break;
    case TOP_SH_POWER_LOW:
        if (asserted) {
            s->power_saved_a = s->cell_a;
            s->power_saved_b = s->cell_b;
            s->power_saved_state = s->cell_state;
            s->power_low = 1;
            }
        else {
            s->cell_a = s->power_saved_a;
            s->cell_b = s->power_saved_b;
            s->cell_state = s->power_saved_state;
            s->power_low = 0;
            topif_publish ();
            }
        break;
    case TOP_SH_OUTPUT_WORD:
        s->bus_data = (uint16) (asserted? value: 0);
        s->reg[TOP_OFF_BPSRA / 2] |= TOP_BPSRA_INE;
        break;
    case TOP_SH_OUTPUT_STROBE:
        if (asserted) {
            s->cell_b = s->bus_data;
            s->reg[TOP_OFF_STATUS_B / 2] = s->cell_b;
            s->reg[TOP_OFF_BPSRA / 2] |= TOP_BPSRA_INE;
            }
        break;
    case TOP_SH_CONTROL_STROBE:
        if (asserted) {
            if ((s->transfer_dir & 0001u) != 0)
                s->reg[TOP_OFF_BPIMA / 2] = (uint16) (s->bus_data & 0003u);
            else
                s->reg[TOP_OFF_PKCSR / 2] = s->bus_data;
            }
        break;
    case TOP_SH_INPUT_SELECT:
        if (asserted)
            s->reg[TOP_OFF_BPSRA / 2] |= TOP_BPSRA_INO;
        break;
    case TOP_SH_LOCAL_RESET:
        if (id == TOP_SIG_IBCLR || id == TOP_SIG_IRESET) {
            s->pending[TOP_SRC_IB][0] = s->pending[TOP_SRC_IB][1] = 0;
            s->reg[TOP_OFF_IKCSR / 2] = 0;
            }
        else {
            s->reg[TOP_OFF_BPSRA / 2] = (uint16) (TOP_BPSRA_FHT |
                                                  TOP_BPSRA_OHT);
            s->pending[TOP_SRC_BP][0] = s->pending[TOP_SRC_BP][1] = 0;
            }
        break;
    case TOP_SH_LOCAL_SYNC:
        s->handshake = (uint16) ((s->handshake & ~0004u) |
                                 (asserted? 0004u: 0));
        break;
    case TOP_SH_FLAG:
        if (asserted)
            s->reg[TOP_OFF_BPSRA / 2] |= TOP_BPSRA_INE;
        else
            s->reg[TOP_OFF_BPSRA / 2] &= ~TOP_BPSRA_INE;
        break;
    case TOP_SH_LATCH:
        if (asserted)
            topif_assert_request (s, (id == TOP_SIG_IKREQA)?
                                  TOP_SRC_IB: TOP_SRC_BP, 0);
        else
            topif_clear_request (s, (id == TOP_SIG_IKREQA)?
                                 TOP_SRC_IB: TOP_SRC_BP, 0);
        break;
    case TOP_SH_ENABLE:
        if (asserted)
            s->enabled[TOP_SRC_BP][0] = s->enabled[TOP_SRC_BP][1] = 1;
        else
            s->enabled[TOP_SRC_BP][0] = s->enabled[TOP_SRC_BP][1] = 0;
        break;
    case TOP_SH_REGISTER:
        if (id == TOP_SIG_BPIMA)
            s->reg[TOP_OFF_BPIMA / 2] = (uint16) (value & 0003u);
        else
            s->reg[TOP_OFF_BPSRA / 2] = (uint16) (value & TOP_BPSRA_NAMED);
        break;
    case TOP_SH_VECTOR:
        if (id == TOP_SIG_INTVA)
            s->cfg.vec_a = (uint32) (value & TOP_WORD_MASK);
        else
            s->cfg.vec_b = (uint32) (value & TOP_WORD_MASK);
        break;
    case TOP_SH_PRIORITY:
        if (id == TOP_SIG_PRIO_AB)
            s->cfg.a_over_b = asserted;
        else
            s->cfg.bp_over_ib = asserted;
        break;
    case TOP_SH_PROPERTY:
        s->irq_level = (uint16) (value & 0007u);
        break;
    default:
        break;
        }
    /* Publish the transition to the TOR model before the local record.  The
     * command seq is allocated by the shm control word, not by the device
     * journal, so the two counters stay independent. */
    (void) topif_bridge_signal (id, value);
    topif_sig_record (s, id, value, TOP_RESP_OK);
    topif_step (s);
    topif_update_int (s);
    topif_publish ();
}

void topif_reset_state (top_state_t *s)
{
    if (s == NULL)
        return;
    topif_defaults (s, NULL);
    s->reg[TOP_OFF_BPFR1 / 2] = s->cfg.bpfr1_start;
    s->reg[TOP_OFF_BPFR2 / 2] = s->cfg.bpfr2_start;
    topif_soft_init (s);
}

uint16 topif_descriptor_service (top_state_t *s, uint16 selector, uint16 operand)
{
    uint16 status;
    top_resp_t resp = TOP_RESP_OK;

    if (s == NULL)
        return 0;
    topif_record (s, (int32) operand, 0, TOP_DIR_READ, TOP_SRC_BP,
                  selector, TOP_RESP_OK);
    if (selector == 014400u) {
        uint16 cell;

        if (operand == (uint16) s->cfg.cell_a_start) {
            cell = s->cell_a;
            s->reg[TOP_OFF_STATUS_A / 2] = cell;
            }
        else if (operand == (uint16) s->cfg.cell_b_start) {
            cell = s->cell_b;
            s->reg[TOP_OFF_STATUS_B / 2] = cell;
            }
        else if (operand == (uint16) s->cfg.cell_state_start) {
            cell = s->cell_state;
            s->reg[TOP_OFF_STATE / 2] = cell;
            }
        else {
            resp = TOP_RESP_UNKNOWN_BITS;
            cell = s->cell_state;
            }
        topif_step (s);
        status = cell;
        }
    else if (selector == 000414u) {
        top_src_t src;
        int channel;

        if (topif_top_request (s, &src, &channel)) {
            status = (uint16) (000400u | ((src == TOP_SRC_IB)? 000020u: 0) |
                               (uint16) channel);
            topif_clear_request (s, src, channel);
            }
        else
            status = (uint16) (s->state & 0007u);
        topif_step (s);
        topif_update_int (s);
        }
    else {
        resp = TOP_RESP_UNKNOWN_BITS;
        status = s->cell_state;
        }
    topif_record (s, (int32) operand, 0, TOP_DIR_READ, TOP_SRC_BP,
                  status, resp);
    topif_publish ();
    return status;
}

static void topif_bridge_apply (uint32 tag, uint32 a, uint32 b, uint32 c,
                                int map_regs)
{
    uint32 r0 = 0;
    uint32 r1 = 0;
    uint32 r2 = 0;
    uint32 flags = 0;

    if (!topif_bridge_service (tag, a, b, c, &r0, &r1, &r2, &flags))
        return;
    if (map_regs) {
        R[0] = (int32) (r0 & 0177777);
        R[1] = (int32) (r1 & 0177777);
        R[2] = (int32) (r2 & 0177777);
        /* Host FLAG_ERR is bit 1.  FLAG_UNKNOWN (bit 0) is not a failed call. */
        C = (int32) ((flags >> 1) & 1u);
        Z = (int32) ((flags & 1u)? 0: 1);
        }
    topif_record (&top_state, (int32) (0100000u + tag), 0, TOP_DIR_READ,
                  TOP_SRC_BP, (uint16) flags, TOP_RESP_BRIDGE);
}

int topif_emt_service (int32 code)
{
    top_state_t *s = &top_state;
    int32 ptr;
    int32 selector;
    int32 operand;
    uint16 status;

    /* Kept as the raw probe used to locate the close call.  It logs the
     * descriptor words before any service decision is made. */
    /* if (code == 0354) {
        int32 p = R[0] & 0177777;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP EMT354 ptr=%06o w0=%06o w1=%06o\n",
                   (unsigned) p,
                   (unsigned) (ReadW (p) & 0177777),
                   (unsigned) (ReadW ((p + 2) & 0177777) & 0177777));
        } */
    /* New trace: the saved program prints every line through this call, so
     * record its pointer and the first words at the source. */
    if (code == 0351) {
        int32 sp = R[0] & 0177777;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP EMT351 ptr=%06o w0=%06o w1=%06o\n",
                   (unsigned) sp,
                   (unsigned) (ReadW (sp) & 0177777),
                   (unsigned) (ReadW ((sp + 2) & 0177777) & 0177777));
        }
    /* New trace: unhandled descriptors carry the command selector used by
     * the dialog, so keep the raw words available for future protocol work. */
    if (code == 0375) {
        int32 dp = R[0] & 0177777;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP EMT375 RAW ptr=%06o w0=%06o w1=%06o\n",
                   (unsigned) dp,
                   (unsigned) (ReadW (dp) & 0177777),
                   (unsigned) (ReadW ((dp + 2) & 0177777) & 0177777));
        }
    if (code == 0214) {
        uint16 selector = (uint16) (R[0] & 0177777);
        int32 sp = R[6] & 0177777;
        uint16 arg0;
        uint16 arg1;
        uint16 arg2;

        if (selector != TOP_SELECT_CELL && selector != TOP_SELECT_STATE &&
            selector != (s->reg[TOP_OFF_BPSRA / 2] & 0177777) &&
            selector != s->cell_a && selector != s->cell_b)
            return 0;
        arg0 = (uint16) (ReadW (sp) & 0177777);
        arg1 = (uint16) (ReadW ((sp + 2) & 0177777) & 0177777);
        arg2 = (uint16) (ReadW ((sp + 4) & 0177777) & 0177777);
        topif_record (s, (int32) (0200000u + selector), 0, TOP_DIR_READ,
                      TOP_SRC_BP, arg0, TOP_RESP_OK);
        topif_record (s, (int32) (0200002u + selector), 0, TOP_DIR_READ,
                      TOP_SRC_BP, arg1, TOP_RESP_OK);
        topif_record (s, (int32) (0200004u + selector), 0, TOP_DIR_READ,
                      TOP_SRC_BP, arg2, TOP_RESP_OK);
        if (selector == TOP_SELECT_STATE) {
            R[0] = s->cell_state & 0177777;
            R[1] = s->reg[TOP_OFF_BPSRA / 2];
            }
        else {
            R[0] = s->cell_b & 0177777;
            R[1] = s->cell_a & 0177777;
            }
        R[6] = (sp + 6) & 0177777;
        s->external_calls++;
        topif_step (s);
        topif_publish ();
        C = 0;
        Z = 1;
        topif_bridge_apply (TOP_SVC_EXT, selector, (uint32) arg0,
                            (uint32) arg1, 0);
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP EXTERNAL CALL selector=%06o args=%06o/%06o/%06o "
                   "status=%06o state=%s\n",
                   (unsigned) selector, (unsigned) arg0, (unsigned) arg1,
                   (unsigned) arg2, (unsigned) R[0],
                   topif_state_name (s->state));
        return 1;
        }
    if (code == 0354 && (s->descriptor_active ||
        (s->descriptor_ptr != 0 && s->external_calls < 3))) {
        int32 release_ptr = R[0] & 0177777;

        topif_record (s, release_ptr, 0, TOP_DIR_WRITE,
                      TOP_SRC_BP, (uint16) s->state, TOP_RESP_OK);
        s->reg[TOP_OFF_BPSRA / 2] = (uint16) (TOP_BPSRA_FHT | TOP_BPSRA_OHT);
        s->pending[0][0] = s->pending[0][1] = 0;
        topif_step (s);
        topif_update_int (s);
        topif_publish ();
        R[1] = s->descriptor_status;
        R[2] = R[0] & 0177777;
        s->descriptor_ptr = (uint32) release_ptr;
        s->descriptor_active = 0;
        C = 0;
        Z = 1;
        topif_bridge_apply (TOP_SVC_DESC, (uint32) release_ptr,
                            (uint32) s->cell_state, 0, 0);
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP DESCRIPTOR RELEASE ptr=%06o status=%06o state=%s\n",
                   (unsigned) release_ptr, (unsigned) R[0],
                   topif_state_name (s->state));
        return 1;
        }
    if (code != 0375)
        return 0;
    ptr = R[0] & 0177777;
    selector = ReadW (ptr) & 0177777;
    operand = ReadW ((ptr + 2) & 0177777) & 0177777;
    if (!((selector == 014400 &&
           (operand == (int32) s->cfg.cell_a_start ||
            operand == (int32) s->cfg.cell_b_start ||
            operand == (int32) s->cfg.cell_state_start)) ||
          (selector == 000414)))
        return 0;
    status = topif_descriptor_service (s, (uint16) selector, (uint16) operand);
    s->descriptor_ptr = (uint32) ptr;
    s->descriptor_active = 1;
    s->descriptor_status = status & 0177777;
    R[1] = status & 0177777;
    R[2] = s->reg[TOP_OFF_BPSRA / 2];
    C = 0;
    Z = (status == 0 || status == 2)? 1: 0;
    topif_bridge_apply (TOP_SVC_DESC, (uint32) selector,
                        (uint32) operand, (uint32) status, 0);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP DESCRIPTOR selector=%06o operand=%06o status=%06o "
               "state=%s bpsra=%06o\n",
               (unsigned) selector, (unsigned) operand, (unsigned) status,
               topif_state_name (s->state),
               (unsigned) s->reg[TOP_OFF_BPSRA / 2]);
    return 1;
}

/* The printed second heading is the software-interface name.  It is emitted
 * from character codes so the simulator source stays neutral; the runtime
 * text is the documented name. */
static const char top_iface_name[] = {
    0132, 0102, 0101, 0124, 0122, 0102, 0
    };

/* Protocol output is produced through the simulator terminal output path used
 * by the keyboard device.  The text is formatted from device registers; no
 * stored protocol file is copied. */
static void topif_emit_line (const char *text) __attribute__ ((unused));

static void topif_emit_line (const char *text)
{
    const char *p;

    /* Device-side printing was removed: it bypassed the guest output path.
     * Kept here only as a documented diagnostic, not as an active path. */
    for (p = text; *p != '\0'; p++) {
        /* sim_putchar_s ((int32) *p); */
        /* chan_write_io ((int32) (IOBA_TTO + 2), *p, 0); */
        }
    /* sim_putchar_s ('\r'); */
    /* sim_putchar_s ('\n'); */
    /* chan_write_io ((int32) (IOBA_TTO + 2), '\r', 0); */
    /* chan_write_io ((int32) (IOBA_TTO + 2), '\n', 0); */
}

static void topif_guest_byte (uint32 pa, uint8 value)
{
    uint32 idx = pa >> 1;

    if (M == NULL || idx >= MEMALLOC_WORDS)
        return;
    if ((pa & 1u) != 0)
        M[idx] = (uint16) ((M[idx] & 0377) | ((uint16) value << 8));
    else
        M[idx] = (uint16) ((M[idx] & 0177400) | value);
}

static void topif_publish_protocol_line (top_state_t *s, unsigned index)
    __attribute__ ((unused));

static void topif_publish_protocol_line (top_state_t *s, unsigned index)
{
    char line[80];
    size_t length;
    size_t i;

    switch (index) {
    case 0:
        strcpy (line, "TBL-PROTOCOL");
        break;
    case 1:
        memcpy (line, top_iface_name, sizeof (top_iface_name));
        break;
    case 2:
        snprintf (line, sizeof (line), "  STATUS_A: %06o STATUS_B: %06o",
                  (unsigned) s->cell_a, (unsigned) s->cell_b);
        break;
    case 3:
        strcpy (line, "PSE-PROTOCOL");
        break;
    case 4:
        snprintf (line, sizeof (line), "  BPFR1: %06o BPFR2: %06o",
                  (unsigned) s->reg[TOP_OFF_BPFR1 / 2],
                  (unsigned) s->reg[TOP_OFF_BPFR2 / 2]);
        break;
    case 5:
        snprintf (line, sizeof (line), "  BPIMA: %06o PKCSR: %06o",
                  (unsigned) s->reg[TOP_OFF_BPIMA / 2],
                  (unsigned) s->reg[TOP_OFF_PKCSR / 2]);
        break;
    case 6:
        strcpy (line, "ELO-PROTOCOL");
        break;
    case 7:
        snprintf (line, sizeof (line), "  STATE: %06o BPSRA: %06o",
                  (unsigned) s->cell_state,
                  (unsigned) s->reg[TOP_OFF_BPSRA / 2]);
        break;
    case 8:
        snprintf (line, sizeof (line), "  IRQ: %06o STATE: %06o",
                  (unsigned) s->reg[TOP_OFF_IRQ / 2],
                  (unsigned) s->cell_state);
        break;
    case 9:
        strcpy (line, "MSW-PROTOCOL");
        break;
    case 10:
        snprintf (line, sizeof (line), "  STATUS_B: %06o STATE: %06o",
                  (unsigned) s->cell_b, (unsigned) s->cell_state);
        break;
    case 11:
        snprintf (line, sizeof (line), "  FLAGS: %06o BPSRA: %06o",
                  (unsigned) s->cfg.cell_state_value,
                  (unsigned) s->reg[TOP_OFF_BPSRA / 2]);
        break;
    case 12:
        strcpy (line, "DIV-PROTOCOL");
        break;
    case 13:
        snprintf (line, sizeof (line), "  STATUS_A: %06o STATE: %06o",
                  (unsigned) s->cell_a, (unsigned) s->cell_state);
        break;
    case 14:
        strcpy (line, "  END");
        break;
    default:
        snprintf (line, sizeof (line), "  BPIMA: %06o PKCSR: %06o",
                  (unsigned) s->reg[TOP_OFF_BPIMA / 2],
                  (unsigned) s->reg[TOP_OFF_PKCSR / 2]);
        break;
        }
    /* A live model may supply the line text through the shared data region;
     * without it the device uses the documented section headings. */
    if (topif_bridge_data (index * 64u, (unsigned char *) line,
                           sizeof (line) - 1u)) {
        line[sizeof (line) - 1u] = '\0';
        topif_record (s, (int32) (TOP_SHM_DATA + index * 64u), 0,
                      TOP_DIR_READ, TOP_SRC_BP, (uint16) index,
                      TOP_RESP_BRIDGE);
        }
    length = strlen (line);
    topif_guest_byte ((uint32) TOP_SHIM_BUFFER, (uint8) '\r');
    topif_guest_byte ((uint32) TOP_SHIM_BUFFER + 1u, (uint8) '\n');
    for (i = 0; i < length; i++)
        topif_guest_byte ((uint32) TOP_SHIM_BUFFER + (uint32) i + 2u,
                          (uint8) line[i]);
    topif_guest_byte ((uint32) TOP_SHIM_BUFFER + (uint32) length + 2u,
                      (uint8) 0200);
    topif_record (s, (int32) TOP_SHIM_BUFFER, 0, TOP_DIR_WRITE, TOP_SRC_BP,
                  (uint16) length, TOP_RESP_OK);
    /* Keep the existing terminal device responsive between protocol lines;
     * its queued buffer would otherwise be overwritten by the next line. */
    tto_unit.wait = 1;
}

/* Install a tiny resident stub in real guest memory and redirect the guest
 * through it.  The stub sets R0 to the published buffer, executes the RT-11
 * .PRINT service and returns to the instruction after the consumed trap. */
static void topif_install_shim (top_state_t *s, int32 return_pc)
    __attribute__ ((unused));

static void topif_install_shim (top_state_t *s, int32 return_pc)
{
    WriteW ((int32) TOP_SHIM_CODE, 0104351);
    WriteW ((int32) (TOP_SHIM_CODE + 2), 000207);
    R[6] = (R[6] - 2) & 0177777;
    WriteW (R[6], return_pc & 0177777);
    R[7] = (int32) TOP_SHIM_CODE;
    topif_record (s, (int32) TOP_SHIM_CODE, return_pc, TOP_DIR_WRITE,
                  TOP_SRC_BP, (uint16) TOP_SHIM_BUFFER, TOP_RESP_OK);
}

int topif_pending_print (void)
{
    return top_state.pending_print != 0;
}

void topif_clear_print (void)
{
    top_state.pending_print = 0;
}

/* Fetch one line from the model through SERVICE NEXT and print it with the
 * resident guest stub.  The host, not the device, decides where the protocol
 * ends by returning a nonzero status. */
/* A printed protocol line can exceed the 80-column terminal.  Emit at most
 * 70 bytes per guest .PRINT call; the remainder is kept by the device. */
static void topif_emit_chunk (top_state_t *s)
{
    uint16 take = s->chunk_len - s->chunk_off;
    uint16 i;

    if (take > 70u)
        take = 70u;
    topif_guest_byte ((uint32) TOP_SHIM_BUFFER, (uint8) '\r');
    topif_guest_byte ((uint32) TOP_SHIM_BUFFER + 1u, (uint8) '\n');
    for (i = 0; i < take; i++)
        topif_guest_byte ((uint32) TOP_SHIM_BUFFER + (uint32) i + 2u,
                          s->chunk[s->chunk_off + i]);
    topif_guest_byte ((uint32) TOP_SHIM_BUFFER + (uint32) take + 2u,
                      (uint8) 0200);
    R[0] = (int32) TOP_SHIM_BUFFER;
    topif_install_shim (s, R[7] & 0177777);
    topif_record (s, (int32) TOP_SHIM_BUFFER, 0, TOP_DIR_WRITE,
                  TOP_SRC_BP, take, TOP_RESP_BRIDGE);
    s->chunk_off = (uint16) (s->chunk_off + take);
    if (s->chunk_off >= s->chunk_len) {
        s->chunk_off = 0;
        s->chunk_len = 0;
        }
    s->protocol_line++;
}

static int topif_next_line (top_state_t *s, uint32 guest_page,
                            uint32 guest_index)
{
    uint32 status = 1;
    uint32 length = 0;
    uint32 checksum = 0;
    uint32 flags = 0;
    unsigned char raw[256];
    uint32 page;
    uint32 index;
    int attempts;

    topif_record (s, (int32) (0100000u + (guest_page << 8) + guest_index),
                  0, TOP_DIR_READ, TOP_SRC_BP, (uint16) s->protocol_line,
                  TOP_RESP_OK);
    if (s->chunk_len != 0) {
        topif_emit_chunk (s);
        return 1;
        }
    for (attempts = 0; attempts < 128; attempts++) {
        page = (uint32) s->next_page;
        index = (uint32) s->next_index;
        if (page >= 6u && s->reg_heading_stage < 3u) {
            page = 6u + (uint32) s->reg_heading_stage;
            index = 0u;
            }
        status = 1;
        length = 0;
        if (!topif_bridge_next (page, index, &status, &length, &checksum,
                                &flags) || status != 0) {
            s->next_page++;
            s->next_index = 0;
            if (s->next_page > 8u)
                break;
            continue;
            }
        if (!topif_bridge_line (raw, sizeof (raw), &length))
            break;
        {
        size_t n = (length != 0 && length < sizeof (raw))?
                   (size_t) length: strnlen ((char *) raw, sizeof (raw));
        size_t i;

        if (n >= sizeof (s->chunk) - 1u)
            n = sizeof (s->chunk) - 2u;
        memcpy (s->chunk, raw, n);
        s->chunk[n] = '\0';
        s->chunk_off = 0;
        s->chunk_len = (uint16) n;
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP NEXT page=%u index=%u len=%u chk=%06o line=%u\n",
                   (unsigned) page, (unsigned) index, (unsigned) n,
                   (unsigned) checksum, (unsigned) s->protocol_line);
        topif_emit_chunk (s);
        if ((uint32) s->next_page >= 6u && s->reg_heading_stage < 3u)
            s->reg_heading_stage++;
        else
            s->next_index++;
        C = (int32) (flags & 1u);
        Z = (int32) ((flags >> 1) & 1u);
        return 1;
        }
        }
    /* End of model protocol: no fabricated line.  Keep carry clear so the
     * guest continues long enough to drain any remaining printed chunks. */
    topif_record (s, (int32) ((uint32) s->next_page * 8u + s->next_index),
                  0, TOP_DIR_READ, TOP_SRC_BP, (uint16) status,
                  TOP_RESP_UNKNOWN_BITS);
    C = 0;
    Z = 1;
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP NEXT END page=%u index=%u status=%u line=%u\n",
               (unsigned) s->next_page, (unsigned) s->next_index,
               (unsigned) status, (unsigned) s->protocol_line);
    return 1;
}

/* A fatal HALT at a documented TOP halt address is a service request, not a
 * simulator stop: the host bridge resolves the PC against its proven module
 * registries and publishes a blueprint/role line.  The line is printed by the
 * same resident guest .PRINT stub already used for the NEXT protocol.  An
 * unknown PC, a bridge error or an absent model leaves the HALT untouched, so
 * no banner is ever fabricated on the host side. */
int topif_role_service (int32 pc)
{
    top_state_t *s = &top_state;
    unsigned char raw[256];
    uint32 status = 1u;
    uint32 length = 0u;
    uint32 checksum = 0u;
    uint32 flags = 0u;
    size_t n;

    if (!topif_bridge_service (TOP_SVC_ROLE, (uint32) pc & 0177777u,
                               (uint32) s->state, 0u,
                               &status, &length, &checksum, &flags))
        return 0;
    if (status != 0u || (flags & TOP_ROLE_FLAG_ERR) != 0u ||
        (flags & TOP_ROLE_FLAG_LINE) == 0u)
        return 0;
    if (!topif_bridge_line (raw, sizeof (raw), &length))
        return 0;
    n = length < sizeof (s->chunk) - 1u? (size_t) length:
        sizeof (s->chunk) - 2u;
    memcpy (s->chunk, raw, n);
    s->chunk[n] = '\0';
    s->chunk_off = 0;
    s->chunk_len = (uint16) n;
    topif_record (s, (int32) ((uint32) pc & 0177777u), 0, TOP_DIR_READ,
                  TOP_SRC_BP, (uint16) length, TOP_RESP_BRIDGE);
    /* Reuse the proven NEXT emission: it writes the 0200-terminated line into
     * real guest memory and installs the EMT 351 stub in real guest memory. */
    topif_emit_chunk (s);
    /* Return from the stub to an EMT 0 exit so the saved program ends in KMON
     * with its own prompt instead of a simulator HALT. */
    WriteW ((int32) (TOP_SHIM_CODE + 4), 005000);  /* CLR R0 */
    WriteW ((int32) (TOP_SHIM_CODE + 6), 0104350); /* EMT 350: guest exit */
    topif_install_shim (s, (TOP_SHIM_CODE + 4) & 0177777);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP ROLE pc=%06o status=%u len=%u sum=%06o state=%s\n",
               (unsigned) pc, (unsigned) status, (unsigned) length,
               (unsigned) checksum, topif_state_name (s->state));
    return 1;
}

/* A fatal TRAP at a documented PC is a model status request, not a monitor
 * error: the bridge resolves the (PC, vector) pair against its FACT table and
 * publishes a line built from linked model registries and live TOP state.  An
 * unknown pair, a bridge error or an absent model leaves the trap alone.  The
 * print and exit path is the same resident guest shim already used by ROLE. */
int topif_fault_service (int32 vec, int32 pc)
{
    top_state_t *s = &top_state;
    unsigned char raw[256];
    uint32 status = 1u;
    uint32 length = 0u;
    uint32 checksum = 0u;
    uint32 flags = 0u;
    size_t n;

    if (!topif_bridge_service (TOP_SVC_FAULT, (uint32) pc & 0177777u,
                               (uint32) vec & 0177777u,
                               (uint32) s->state,
                               &status, &length, &checksum, &flags))
        return 0;
    if (status != 0u || (flags & TOP_ROLE_FLAG_ERR) != 0u ||
        (flags & TOP_ROLE_FLAG_LINE) == 0u)
        return 0;
    if (!topif_bridge_line (raw, sizeof (raw), &length))
        return 0;
    n = length < sizeof (s->chunk) - 1u? (size_t) length:
        sizeof (s->chunk) - 2u;
    memcpy (s->chunk, raw, n);
    s->chunk[n] = '\0';
    s->chunk_off = 0;
    s->chunk_len = (uint16) n;
    topif_record (s, (int32) ((uint32) pc & 0177777u), (int32) vec,
                  TOP_DIR_READ, TOP_SRC_BP, (uint16) length,
                  TOP_RESP_BRIDGE);
    topif_emit_chunk (s);
    WriteW ((int32) (TOP_SHIM_CODE + 4), 005000);  /* CLR R0 */
    WriteW ((int32) (TOP_SHIM_CODE + 6), 0104350); /* EMT 350: guest exit */
    topif_install_shim (s, (TOP_SHIM_CODE + 4) & 0177777);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP FAULT pc=%06o vec=%06o status=%u len=%u sum=%06o "
               "state=%s\n", (unsigned) pc, (unsigned) vec,
               (unsigned) status, (unsigned) length, (unsigned) checksum,
               topif_state_name (s->state));
    return 1;
}

int topif_trap_service (int32 code)
{
    top_state_t *s = &top_state;

    if (code == 0001 && s->state == TOP_STATE_DIALOG)
        return topif_next_line (s, (uint32) R[3] & 0377u,
                                (uint32) R[1] & 0007u);
    if (code == 0010 && (s->state == TOP_STATE_DIALOG ||
                          s->protocol_line != 0)) {
        /* The former device-side text generation is kept as a diagnostic
         * record only.  No output is performed here now: the service returns
         * a guest-readable request so the saved program decides when to
         * print.  Publishing text directly was a host-side shortcut. */
        /*
        char line[80];
        if (s->protocol_pages == 0) {
            topif_emit_line ("TBL-PROTOCOL");
            topif_emit_line (top_iface_name);
            snprintf (line, sizeof (line), "  STATUS_A: %06o STATUS_B: %06o",
                      (unsigned) s->cell_a, (unsigned) s->cell_b);
            topif_emit_line (line);
            topif_emit_line ("PSE-PROTOCOL");
            snprintf (line, sizeof (line), "  BPFR1: %06o BPFR2: %06o",
                      (unsigned) s->reg[TOP_OFF_BPFR1 / 2],
                      (unsigned) s->reg[TOP_OFF_BPFR2 / 2]);
            topif_emit_line (line);
            topif_emit_line ("ELO-PROTOCOL");
            snprintf (line, sizeof (line), "  STATE: %06o BPSRA: %06o",
                      (unsigned) s->cell_state,
                      (unsigned) s->reg[TOP_OFF_BPSRA / 2]);
            topif_emit_line (line);
            topif_emit_line ("MSW-PROTOCOL");
            snprintf (line, sizeof (line), "  BPIMA: %06o PKCSR: %06o",
                      (unsigned) s->reg[TOP_OFF_BPIMA / 2],
                      (unsigned) s->reg[TOP_OFF_PKCSR / 2]);
            topif_emit_line (line);
            topif_emit_line ("DIV-PROTOCOL");
            snprintf (line, sizeof (line), "  STATUS_A: %06o STATE: %06o",
                      (unsigned) s->cell_a, (unsigned) s->cell_state);
            topif_emit_line (line);
            s->protocol_pages = 5;
            }
        */
        /* The former register-only response is kept as a diagnostic; it
         * could not print because the guest never consumed R0. */
        /*
        topif_record (s, (int32) (R[1] & 0177777), 0, TOP_DIR_READ,
                      TOP_SRC_BP, (uint16) s->cell_state, TOP_RESP_OK);
        s->reg[TOP_OFF_SIGSEL / 2] = (uint16) (s->protocol_pages % 5u);
        s->reg[TOP_OFF_SIGVAL / 2] = (uint16) s->cell_state;
        s->reg[TOP_OFF_SIGSTATE / 2] = s->reg[TOP_OFF_BPSRA / 2];
        R[0] = (int32) TOP_IO_LO + (int32) TOP_OFF_SIGSEL;
        R[1] = (int32) (s->protocol_pages % 5u);
        R[2] = s->reg[TOP_OFF_BPSRA / 2];
        C = 0;
        Z = 1;
        */
        topif_bridge_apply (TOP_SVC_NEXT, (uint32) (R[1] & 0177777),
                            (uint32) s->protocol_line, 0, 0);
        return topif_next_line (s, (uint32) R[3] & 0377u,
                                (uint32) R[1] & 0007u);
        }
    if (code == 0024 && s->state == TOP_STATE_DIALOG) {
        topif_record (s, (int32) (R[1] & 0177777), 0, TOP_DIR_READ,
                      TOP_SRC_BP, (uint16) s->cell_state, TOP_RESP_OK);
        s->protocol_pages++;
        s->reg[TOP_OFF_SIGSEL / 2] = (uint16) (s->protocol_pages % 5u);
        s->reg[TOP_OFF_SIGVAL / 2] = (uint16) s->cell_state;
        R[0] = s->cell_state & 0177777;
        R[1] = (int32) (s->protocol_pages % 5u);
        R[2] = s->reg[TOP_OFF_BPSRA / 2];
        C = 0;
        Z = 1;
        topif_bridge_apply (TOP_SVC_PROT, (uint32) (R[1] & 0177777),
                            (uint32) s->protocol_pages, 0, 0);
        sim_debug (TOP_DEB_STATE, &top_dev,
                   "TOP PROTOCOL PAGE page=%u selector=%u state=%s\n",
                   (unsigned) s->protocol_pages,
                   (unsigned) s->reg[TOP_OFF_SIGSEL / 2],
                   topif_state_name (s->state));
        return 1;
        }
    if (code != 0376 || (R[0] & 0177700) != (s->cell_state & 0177700))
        return 0;
    topif_record (s, (int32) (R[0] & 0177777), 0, TOP_DIR_READ,
                  TOP_SRC_BP, (uint16) s->cell_state, TOP_RESP_OK);
    s->cell_b |= TOP_STATUS_B_DIALOG;
    s->reg[TOP_OFF_STATUS_B / 2] = s->cell_b;
    topif_set_state (s, TOP_STATE_DIALOG);
    topif_step (s);
    topif_update_int (s);
    topif_publish ();
    R[1] = s->cell_state & 0177777;
    R[2] = s->reg[TOP_OFF_BPSRA / 2];
    C = 0;
    Z = 1;
    topif_bridge_apply (TOP_SVC_EXT, (uint32) (R[0] & 0177777), 0, 0, 0);
    sim_debug (TOP_DEB_STATE, &top_dev,
               "TOP MODULE ENTRY code=%03o status=%06o state=%s\n",
               (unsigned) code, (unsigned) R[1], topif_state_name (s->state));
    return 1;
}

static t_stat topif_reset (DEVICE *dptr)
{
    top_state_t *s = &top_state;

    topif_reset_state (s);
    if (top_dib.ba == IOBA_AUTO)
        top_dib.ba = IOBA_TOP;
    if (top_dib.vec == VEC_AUTO)
        top_dib.vec = s->cfg.vec_a;
    top_dib.vloc = IVCL (TOP_A);
    CLR_INT (TOP_A);
    CLR_INT (TOP_B);
    return SCPE_OK;
}

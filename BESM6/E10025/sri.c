/* sri.c: SRI (SR-BUS, BP and IB) interface model for E10025.
 *
 * The register windows, vectors and bit names printed in the documentation are
 * kept as facts.  Where sources disagree, or where only a snapshot exists,
 * bits stay UNKNOWN and no interrupt is synthesised from a register write.
 * The I-side names 330/334 of the KSE-BP channels A/B map to the P-side
 * vectors 320/324; KSE-BP interrupt control comes before KSE-IB in the
 * SR-BUS priority chain.
 */

#include "sri.h"
#include "topif.h"
#include "e10025_io_lib.h"
#include <string.h>

#define SRI_DEB_JOURNAL  1                              /* register transactions */
#define SRI_DEB_IRQ      2                              /* requests and acks */

#define IOLN_SRI_BP      0140
#define IOLN_SRI_IB      0440

extern DEVICE sri_dev;

sri_state_t sri_state;

/* Named bits are the printed BPSRA table.  The 0x2000 bit was seen in an error
 * snapshot but has no name.  PKCSR enable positions disagree between the
 * printed tables, so no enable bit is named here.
 *
 * STATUS_INJECTION and REQUEST_INJECTION are emulator-only surrogates for
 * software-driven emulation.  They are NOT hardware registers and their bit
 * codes are NOT a hardware register or bit layout; no bus timing is implied. */
static const sri_reg_desc_t bp_regs[] = {
    { "PKCSR",  SRI_OFF_PKCSR,   0x0000u, 0x8080u, 1, 1 },
    { "BPODR",  SRI_OFF_BPODR_W, 0x0000u, 0x0000u, 1, 1 },
    { "BPFR1",  SRI_OFF_BPFR1,   0x0000u, 0x0000u, 1, 1 },
    { "BPFR2",  SRI_OFF_BPFR2,   0x0000u, 0x0000u, 1, 1 },
    { "BPEXAR", SRI_OFF_BPEXAR,  0x0000u, 0x0000u, 1, 1 },
    { "BPIMA",  SRI_OFF_BPIMA,   0x0000u, 0x0000u, 1, 1 },
    { "BPTDR",  SRI_OFF_BPTDR,   0x0000u, 0x0000u, 1, 1 },
    { "BPMTS",  SRI_OFF_BPMTS,   0x0000u, 0x0000u, 1, 1 },
    { "BPODRR", SRI_OFF_BPODR_R, 0x0000u, 0x0000u, 1, 0 },
    { "BPSRA",  SRI_OFF_BPSRA,   0x0FFFu, 0x2000u, 1, 0 },
    { "STATUS_INJECTION",  SRI_OFF_STATUS_INJECT,  0x0000u, 0x0000u, 1, 1 },
    { "FMTTO",  SRI_OFF_FMTTO,   0x0000u, 0x0000u, 1, 0 },
    { "REQUEST_INJECTION", SRI_OFF_REQUEST_INJECT, 0x0000u, 0x0000u, 1, 1 },
    { "KXTO",   SRI_OFF_KXTO,    0x0000u, 0x0000u, 1, 0 },
    { "BPCLR",  SRI_OFF_BPCLR_W, 0x0000u, 0x0000u, 0, 1 },
    { "KYTO",   SRI_OFF_KYTO_R,  0x0000u, 0x0000u, 1, 0 }
    };

static const sri_reg_desc_t ib_regs[] = {
    { "IKCSR", SRI_OFF_IKCSR, 0x0000u, 0x0000u, 1, 1 },
    { "IBCLR", SRI_OFF_IBCLR, 0x0000u, 0x0000u, 0, 1 }
    };

static DEBTAB sri_debug[] = {
    {"JOURNAL", SRI_DEB_JOURNAL, "register window transactions"},
    {"IRQ", SRI_DEB_IRQ, "interrupt requests and acknowledgements"},
    {0}
    };

static t_stat sri_ack_bpa (void);
static t_stat sri_ack_bpb (void);
static t_stat sri_ack_iba (void);
static t_stat sri_ack_ibb (void);
static t_stat sri_reset (DEVICE *dptr);
static const char *sri_description (DEVICE *dptr);

static DIB sri_ib_dib;

static DIB sri_bp_dib = {
    IOBA_SRI_BP, IOLN_SRI_BP, &sri_rd, &sri_wr,
    2, IVCL (SRI_BPA), SRI_VEC_P_BP_A,
    { sri_ack_bpa, sri_ack_bpb, NULL, NULL },
    0, 0, &sri_ib_dib, NULL
    };

static DIB sri_ib_dib = {
    IOBA_SRI_IB, IOLN_SRI_IB, &sri_rd, &sri_wr,
    2, IVCL (SRI_IBA), SRI_VEC_I_BP_A,
    { sri_ack_iba, sri_ack_ibb, NULL, NULL },
    0, 0, NULL, NULL
    };

static UNIT sri_unit = { UDATA (NULL, 0, 0) };

/* The two windows are one device with two DIBs.  Only the first DIB is
 * assigned by auto_config; the second one is filled in by sri_reset(). */
REG sri_reg[] = {
    { GRDATA (PKCSR, sri_state.bp_reg[SRI_OFF_PKCSR / 2], DEV_RDX, 16, 0) },
    { GRDATA (BPSRA, sri_state.bp_reg[SRI_OFF_BPSRA / 2], DEV_RDX, 16, 0) },
    { GRDATA (BPODR, sri_state.bp_reg[SRI_OFF_BPODR_W / 2], DEV_RDX, 16, 0) },
    { GRDATA (IKCSR, sri_state.ib_reg[SRI_OFF_IKCSR / 2], DEV_RDX, 16, 0) },
    { FLDATA (INT_A, IREQ (SRI_BPA), INT_V_SRI_BPA) },
    { FLDATA (INT_B, IREQ (SRI_BPB), INT_V_SRI_BPB) },
    { FLDATA (IB_A,  IREQ (SRI_IBA), INT_V_SRI_IBA) },
    { FLDATA (IB_B,  IREQ (SRI_IBB), INT_V_SRI_IBB) },
    { GRDATA (DEVADDR, sri_bp_dib.ba, DEV_RDX, 32, 0), REG_HRO },
    { GRDATA (DEVVEC, sri_bp_dib.vec, DEV_RDX, 16, 0), REG_HRO },
    { NULL }
    };

MTAB sri_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 010, "ADDRESS", "ADDRESS",
      &set_addr, &show_addr, NULL, "Bus address" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "VECTOR", "VECTOR",
      &set_vec, &show_vec, NULL, "Interrupt vector" },
    { 0 }
    };

DEVICE sri_dev = {
    "SRI", &sri_unit, sri_reg, sri_mod,
    1, DEV_RDX, 16, 1, DEV_RDX, 16,
    NULL, NULL, &sri_reset,
    NULL, NULL, NULL,
    &sri_bp_dib, DEV_DISABLE | DEV_UBUS | DEV_DEBUG, 0,
    sri_debug,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    &sri_description
    };

static const char *sri_description (DEVICE *dptr)
{
    return "SRI SR-BUS/BP interface";
}

static const char *sri_src_name (sri_src_t src)
{
    return (src == SRI_SRC_BP)? "BP": "IB";
}

unsigned sri_request_level_fact (void)
{
    return SRI_REQUEST_LEVEL_FACT;
}

/* BPSRA is the one TOR-owned status register, not a PVS-owned slot.  The bridge
 * read reaches the ODS core status through live_bpsra(); while the bridge
 * object is absent the TOP device's own authoritative slot carries the same
 * FACT bits.  Both paths are caches only, as with PKCSR in this window. */
static uint16 sri_bpsra_fact (sri_state_t *s)
{
    uint16 bpsra = 0;

    if (s == NULL)
        return 0;
    if (topif_bridge_read (TOP_BPSRA_ADDR, &bpsra)) {
        sim_debug (SRI_DEB_JOURNAL, &sri_dev,
                   "SRI BPSRA FACT BRIDGE %06o\n", (unsigned) bpsra);
        }
    else {
        bpsra = top_state.reg[TOP_OFF_BPSRA / 2u];
        sim_debug (SRI_DEB_JOURNAL, &sri_dev,
                   "SRI BPSRA FACT MODEL %06o\n", (unsigned) bpsra);
        }
    s->bp_reg[SRI_OFF_BPSRA / 2u] = bpsra;
    return bpsra;
}

static const char *sri_resp_name (sri_resp_t resp)
{
    if (resp == SRI_RESP_OK)
        return "OK";
    if (resp == SRI_RESP_DIAG_ONLY)
        return "DIAG";
    return "UNKNOWN";
}

static int sri_locate (int32 pa, sri_src_t *src, uint16 *off)
{
    int32 d;

    if (src == NULL || off == NULL)
        return 0;
    d = pa - (int32) sri_bp_dib.ba;
    if (d >= 0 && (uint32) d < sri_bp_dib.lnt) {
        *src = SRI_SRC_BP;
        *off = (uint16) (((uint32) d) & ~1u);
        return 1;
        }
    d = pa - (int32) sri_ib_dib.ba;
    if (d >= 0 && (uint32) d < sri_ib_dib.lnt) {
        *src = SRI_SRC_IB;
        *off = (uint16) (((uint32) d) & ~1u);
        return 1;
        }
    return 0;
}

static void sri_record (sri_state_t *s, int32 addr, sri_dir_t dir,
                        sri_src_t src, uint16 data, sri_resp_t resp)
{
    sri_journal_entry_t *e;

    if (s == NULL)
        return;
    e = &s->journal[s->journal_head];
    e->seq = ++s->journal_seq;
    e->addr = addr;
    e->dir = dir;
    e->src = src;
    e->data = data;
    e->time = (int32) s->journal_seq;
    e->resp = resp;
    s->journal_head = (s->journal_head + 1u) % SRI_JOURNAL_DEPTH;
    sim_debug (SRI_DEB_JOURNAL, &sri_dev,
               "SRI JRN %u %s %s %06o data=%06o %s\n",
               (unsigned) e->seq, (dir == SRI_DIR_READ)? "RD": "WR",
               sri_src_name (src), (unsigned) addr, (unsigned) data,
               sri_resp_name (resp));
}

void sri_defaults (sri_state_t *s, const sri_config_t *cfg)
{
    if (s == NULL)
        return;
    memset (s, 0, sizeof (*s));

    s->cfg.bp_base = IOBA_SRI_BP;
    s->cfg.ib_base = IOBA_SRI_IB;
    s->cfg.bp_vec_a = SRI_VEC_P_BP_A;
    s->cfg.bp_vec_b = SRI_VEC_P_BP_B;
    s->cfg.ib_vec_a = SRI_VEC_I_BP_A;
    s->cfg.ib_vec_b = SRI_VEC_I_BP_B;
    s->cfg.bp_over_ib = 1;
    s->cfg.a_over_b = 1;
    s->cfg.bp_ipl = IPL_SRI_BP;
    s->cfg.ib_ipl = IPL_SRI_IB;
    /* Surrogate enable positions for the injection path only, not hardware. */
    s->cfg.pkcsr_ie_a = SRI_SURROGATE_IE_A;
    s->cfg.pkcsr_ie_b = SRI_SURROGATE_IE_B;
    s->cfg.no_autoinc_bpodr = 1;
    if (cfg != NULL)
        s->cfg = *cfg;
    /* Printed request level and priority order are FACTs.  No IPL line is
     * assigned and no pin or wire is claimed by this model. */
    sim_debug (SRI_DEB_JOURNAL, &sri_dev,
               "SRI FACT REQUEST_LEVEL=%u VEC_A=%06o VEC_B=%06o "
               "PRIO_A>B PRIO_BP>IB IPL=NONE PIN=NONE\n",
               (unsigned) SRI_REQUEST_LEVEL_FACT,
               (unsigned) s->cfg.bp_vec_a, (unsigned) s->cfg.bp_vec_b);
}

static void sri_update_int (sri_state_t *s)
{
    sri_src_t src;
    int channel;

    CLR_INT (SRI_BPA);
    CLR_INT (SRI_BPB);
    CLR_INT (SRI_IBA);
    CLR_INT (SRI_IBB);
    if (!sri_top_request (s, &src, &channel))
        return;
    if (src == SRI_SRC_BP) {
        if (channel == 0)
            SET_INT (SRI_BPA);
        else
            SET_INT (SRI_BPB);
        }
    else {
        if (channel == 0)
            SET_INT (SRI_IBA);
        else
            SET_INT (SRI_IBB);
        }
}

t_stat sri_rd (int32 *data, int32 pa, int32 access)
{
    sri_state_t *s = &sri_state;
    sri_src_t src = SRI_SRC_BP;
    uint16 off = 0;
    uint16 value = 0;
    sri_resp_t resp = SRI_RESP_OK;
    const sri_reg_desc_t *d;
    int found;

    if (data == NULL)
        return SCPE_ARG;
    *data = 0;
    found = sri_locate (pa, &src, &off);
    if ((access == WRITEB) || !found) {
        sri_record (s, pa, SRI_DIR_READ, src, 0, SRI_RESP_DIAG_ONLY);
        return SCPE_OK;
        }
    if (src == SRI_SRC_BP && off == SRI_OFF_PKCSR) {
        uint16 authoritative = 0;
        uint32 seq = 0;

        if (topif_csr_access (SRI_PKCSR_ADDR, 0, 0, &authoritative, &seq))
            value = authoritative;
        else
            value = s->bp_reg[off / 2u];
        }
    else if (src == SRI_SRC_BP && off == SRI_OFF_BPSRA) {
        value = sri_bpsra_fact (s);
        }
    else if (src == SRI_SRC_BP && off <= SRI_OFF_BTM15) {
        value = s->bp_reg[off / 2u];
        }
    else {
        d = sri_find_reg (src, off, SRI_DIR_READ);
        if (d != NULL) {
            uint16 slot = (uint16) (off / 2u);

            if (src == SRI_SRC_BP && off == SRI_OFF_BPODR_R)
                slot = (uint16) (SRI_OFF_BPODR_W / 2u);
            value = (src == SRI_SRC_BP)? s->bp_reg[slot]: s->ib_reg[slot];
            }
        else
            resp = SRI_RESP_DIAG_ONLY;
        }
    sri_record (s, pa, SRI_DIR_READ, src, value, resp);
    *data = (int32) value;
    return SCPE_OK;
}

t_stat sri_wr (int32 data, int32 pa, int32 access)
{
    sri_state_t *s = &sri_state;
    sri_src_t src = SRI_SRC_BP;
    uint16 off = 0;
    uint16 value = (uint16) (((uint32) data) & SRI_WORD_MASK);
    sri_resp_t resp = SRI_RESP_OK;
    const sri_reg_desc_t *d;
    int found;

    found = sri_locate (pa, &src, &off);
    /* Byte writes are recorded but never interpreted: the BP data register
     * explicitly forbids address auto-increment. */
    if ((access == WRITEB) || !found) {
        sri_record (s, pa, SRI_DIR_WRITE, src, value, SRI_RESP_DIAG_ONLY);
        return SCPE_OK;
        }
    if (src == SRI_SRC_BP && off == SRI_OFF_PKCSR) {
        uint16 authoritative = value;
        uint32 seq = 0;

        if (topif_csr_access (SRI_PKCSR_ADDR, value, 1, &authoritative, &seq))
            s->bp_reg[off / 2u] = authoritative;
        else
            s->bp_reg[off / 2u] = value;
        /* The surrogate enable latch is driven by the written word, not by a
         * CSR bit.  It stays separate from the one authoritative CSR owner. */
        s->enabled[0][0] = (s->cfg.pkcsr_ie_a != 0 &&
                            (value & s->cfg.pkcsr_ie_a) != 0);
        s->enabled[0][1] = (s->cfg.pkcsr_ie_b != 0 &&
                            (value & s->cfg.pkcsr_ie_b) != 0);
        }
    else if (src == SRI_SRC_BP && off <= SRI_OFF_BTM15) {
        s->bp_reg[off / 2u] = value;
        }
    else {
        d = sri_find_reg (src, off, SRI_DIR_WRITE);
        if (d != NULL) {
            if (src == SRI_SRC_BP)
                s->bp_reg[off / 2u] = value;
            else
                s->ib_reg[off / 2u] = value;
            if (src == SRI_SRC_BP && off == SRI_OFF_STATUS_INJECT) {
                /* Surrogate only: the written word replaces BPSRA and the
                 * printed TRA marker asserts each enabled BP channel.  This is
                 * NOT a hardware register effect or bit layout. */
                s->bp_reg[SRI_OFF_BPSRA / 2u] = value;
                if ((value & SRI_SURROGATE_STATUS_TRA) != 0) {
                    if (s->enabled[SRI_SRC_BP][0])
                        sri_assert_request (s, SRI_SRC_BP, 0);
                    if (s->enabled[SRI_SRC_BP][1])
                        sri_assert_request (s, SRI_SRC_BP, 1);
                    }
                }
            if (src == SRI_SRC_BP && off == SRI_OFF_REQUEST_INJECT) {
                /* Surrogate request decode from low bits, not a hardware map. */
                sri_src_t inj_src = (value & SRI_SURROGATE_SRC_IB)?
                                    SRI_SRC_IB: SRI_SRC_BP;
                int inj_ch = (value & SRI_SURROGATE_CH_B)? 1: 0;

                if (value != 0 && s->enabled[inj_src][inj_ch])
                    sri_assert_request (s, inj_src, inj_ch);
                }
            if (src == SRI_SRC_BP && off == SRI_OFF_BPCLR_W) {
                /* BPCLR drops the pending requests.  BPSRA keeps the TOR-owned
                 * FACT bits, refreshed through the same one-owner path. */
                (void) sri_bpsra_fact (s);
                s->pending[0][0] = 0;
                s->pending[0][1] = 0;
                sim_debug (SRI_DEB_IRQ, &sri_dev,
                           "SRI IRQ BP requests cleared by BPCLR\n");
                }
            }
        else
            resp = SRI_RESP_DIAG_ONLY;
        }
    sri_record (s, pa, SRI_DIR_WRITE, src, value, resp);
    sri_update_int (s);
    return SCPE_OK;
}

int32 sri_vector (sri_src_t src, int channel)
{
    const sri_state_t *s = &sri_state;

    if (src == SRI_SRC_BP)
        return (channel == 0)? (int32) s->cfg.bp_vec_a:
                               (int32) s->cfg.bp_vec_b;
    return (channel == 0)? (int32) s->cfg.ib_vec_a:
                           (int32) s->cfg.ib_vec_b;
}

/* The interrupt control of KSE-BP precedes that of KSE-IB.  Within BP,
 * channel A precedes channel B. */
int sri_top_request (const sri_state_t *s, sri_src_t *src, int *channel)
{
    int src_order[2] = { SRI_SRC_BP, SRI_SRC_IB };
    int ch_order[2] = { 0, 1 };
    int i;
    int j;

    if (s == NULL || src == NULL || channel == NULL)
        return 0;
    if (!s->cfg.bp_over_ib) {
        src_order[0] = SRI_SRC_IB;
        src_order[1] = SRI_SRC_BP;
        }
    if (!s->cfg.a_over_b) {
        ch_order[0] = 1;
        ch_order[1] = 0;
        }
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            int ch = ch_order[j];
            sri_src_t sc = (sri_src_t) src_order[i];

            if (s->pending[sc][ch] && s->enabled[sc][ch]) {
                *src = sc;
                *channel = ch;
                return 1;
                }
            }
        }
    return 0;
}

void sri_assert_request (sri_state_t *s, sri_src_t src, int channel)
{
    if (s == NULL || src > SRI_SRC_IB || channel < 0 || channel > 1)
        return;
    s->pending[src][channel] = 1;
    s->irq_count[src][channel]++;
    sim_debug (SRI_DEB_IRQ, &sri_dev,
               "SRI IRQ %s %c asserted, vector %06o\n",
               sri_src_name (src), (channel == 0)? 'A': 'B',
               (unsigned) sri_vector (src, channel));
    sri_update_int (s);
}

void sri_clear_request (sri_state_t *s, sri_src_t src, int channel)
{
    if (s == NULL || src > SRI_SRC_IB || channel < 0 || channel > 1)
        return;
    s->pending[src][channel] = 0;
    sri_update_int (s);
}

static t_stat sri_ack_common (sri_src_t src, int channel)
{
    sri_state_t *s = &sri_state;
    int32 vec;

    sri_clear_request (s, src, channel);
    vec = sri_vector (src, channel);
    if (src == SRI_SRC_BP) {
        sim_debug (SRI_DEB_IRQ, &sri_dev,
                   "SRI ACK BP%c I-vector %06o -> P-vector %06o\n",
                   (channel == 0)? 'A': 'B',
                   (unsigned) ((channel == 0)? SRI_VEC_I_BP_A:
                                               SRI_VEC_I_BP_B),
                   (unsigned) vec);
        }
    else
        sim_debug (SRI_DEB_IRQ, &sri_dev,
                   "SRI ACK IB%c vector %06o\n",
                   (channel == 0)? 'A': 'B', (unsigned) vec);
    return vec;
}

static t_stat sri_ack_bpa (void) { return sri_ack_common (SRI_SRC_BP, 0); }
static t_stat sri_ack_bpb (void) { return sri_ack_common (SRI_SRC_BP, 1); }
static t_stat sri_ack_iba (void) { return sri_ack_common (SRI_SRC_IB, 0); }
static t_stat sri_ack_ibb (void) { return sri_ack_common (SRI_SRC_IB, 1); }

t_stat sri_ack (sri_src_t src, int channel)
{
    if (src > SRI_SRC_IB || channel < 0 || channel > 1)
        return SCPE_ARG;
    return sri_ack_common (src, channel);
}

const sri_reg_desc_t *sri_find_reg (sri_src_t src, uint16 off, sri_dir_t dir)
{
    const sri_reg_desc_t *tab;
    size_t n;
    size_t i;

    if (src == SRI_SRC_BP) {
        tab = bp_regs;
        n = sizeof (bp_regs) / sizeof (bp_regs[0]);
        }
    else {
        tab = ib_regs;
        n = sizeof (ib_regs) / sizeof (ib_regs[0]);
        }
    for (i = 0; i < n; i++) {
        if (tab[i].off != off)
            continue;
        if (dir == SRI_DIR_READ && !tab[i].readable)
            continue;
        if (dir == SRI_DIR_WRITE && !tab[i].writable)
            continue;
        return &tab[i];
        }
    return NULL;
}

uint16 sri_unknown_mask (const sri_reg_desc_t *d)
{
    if (d == NULL)
        return (uint16) SRI_WORD_MASK;
    return (uint16) (SRI_WORD_MASK & ~((uint32) (d->named | d->observed)));
}

unsigned sri_journal_count (const sri_state_t *s)
{
    if (s == NULL)
        return 0;
    return (s->journal_seq < (uint32) SRI_JOURNAL_DEPTH)?
           (unsigned) s->journal_seq: SRI_JOURNAL_DEPTH;
}

const sri_journal_entry_t *sri_journal_at (const sri_state_t *s, unsigned i)
{
    unsigned n;
    unsigned start;

    if (s == NULL)
        return NULL;
    n = sri_journal_count (s);
    if (i >= n)
        return NULL;
    start = (s->journal_seq < (uint32) SRI_JOURNAL_DEPTH)?
            0u: s->journal_head;
    return &s->journal[(start + i) % SRI_JOURNAL_DEPTH];
}

static t_stat sri_reset (DEVICE *dptr)
{
    sri_state_t *s = &sri_state;

    sri_defaults (s, NULL);
    if (sri_bp_dib.ba == IOBA_AUTO)
        sri_bp_dib.ba = IOBA_SRI_BP;
    if (sri_bp_dib.vec == VEC_AUTO)
        sri_bp_dib.vec = s->cfg.bp_vec_a;
    sri_bp_dib.vloc = IVCL (SRI_BPA);
    if (sri_ib_dib.ba == IOBA_AUTO)
        sri_ib_dib.ba = IOBA_SRI_IB;
    if (sri_ib_dib.vec == VEC_AUTO)
        sri_ib_dib.vec = s->cfg.ib_vec_a;
    sri_ib_dib.vloc = IVCL (SRI_IBA);
    CLR_INT (SRI_BPA);
    CLR_INT (SRI_BPB);
    CLR_INT (SRI_IBA);
    CLR_INT (SRI_IBB);
    return SCPE_OK;
}

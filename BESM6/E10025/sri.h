/* sri.h: SRI interface model for the Электроника-100-25 simulator.
 *
 * The model keeps the documented BP and IB register windows and the four
 * interrupt channels as observable state.  Bit positions that are not settled
 * by the available documentation are deliberately left UNKNOWN instead of
 * being named or synthesised.
 */

#ifndef SRI_H
#define SRI_H

#include "e10025_defs.h"

/* Guest 16-bit register-window forms of the documented windows.  The physical
 * address is IOPAGEBASE with the low I/O-page bits of the guest address; these
 * constants are not physical bus addresses. */
#define SRI_BP_IO_LO     0166000u
#define SRI_BP_IO_HI     0166136u
#define SRI_IB_IO_LO     0164000u
#define SRI_IB_IO_HI     0164436u
/* PKCSR has two printable forms, 166100 here and 167012 in the TOP window.
 * Both are caches of one TOR-owned response with one readback and one seq. */
#define SRI_PKCSR_ADDR   0166100u
#define SRI_IKCSR_ADDR   0164400u
#define SRI_PKCSR_OFF    0100u
#define SRI_IKCSR_OFF    0400u

/* P-side vectors delivered to the processor.  The I-side names 330/334 of
 * KSE-BP channels A/B map onto these same two channels as 320/324. */
#define SRI_VEC_P_BP_A   0320u
#define SRI_VEC_P_BP_B   0324u
#define SRI_VEC_I_BP_A   0330u
#define SRI_VEC_I_BP_B   0334u

/* Emulator-only surrogate codes for software-driven request injection.
 * These codes are NOT a hardware register or bit layout.  The printed PKCSR
 * enable positions disagree, so two low-order positions are used only to let
 * a host or guest select a channel through the existing enable latch. */
#define SRI_SURROGATE_IE_A        0001u
#define SRI_SURROGATE_IE_B        0002u
#define SRI_SURROGATE_SRC_IB      0001u
#define SRI_SURROGATE_CH_B        0002u
#define SRI_SURROGATE_STATUS_TRA  0200u

/* Register offsets relative to each window base (octal). */
enum sri_bp_off {
    SRI_OFF_BTM0     = 0000,
    SRI_OFF_BTM15    = 0036,
    SRI_OFF_PKCSR    = 0100,
    SRI_OFF_BPODR_W  = 0102,
    SRI_OFF_BPFR1    = 0104,
    SRI_OFF_BPFR2    = 0106,
    SRI_OFF_BPEXAR   = 0110,
    SRI_OFF_BPIMA    = 0112,
    SRI_OFF_BPTDR    = 0114,
    SRI_OFF_BPMTS    = 0120,
    SRI_OFF_BPODR_R  = 0122,
    SRI_OFF_BPSRA    = 0124,
    /* Free BP-window slots used only by the emulator-only surrogate.  They are
     * NOT hardware registers and carry no hardware bit layout or timing. */
    SRI_OFF_STATUS_INJECT  = 0126,
    SRI_OFF_FMTTO    = 0130,
    SRI_OFF_REQUEST_INJECT = 0132,
    SRI_OFF_KXTO     = 0134,
    SRI_OFF_BPCLR_W  = 0136,
    SRI_OFF_KYTO_R   = 0136
    };

enum sri_ib_off {
    SRI_OFF_IKCSR    = 0400,
    SRI_OFF_IBCLR    = 0436
    };

#define SRI_WORD_MASK    0177777u
#define SRI_REG_SLOTS    0220u
#define SRI_JOURNAL_DEPTH 8u

typedef enum { SRI_DIR_READ = 0, SRI_DIR_WRITE = 1 } sri_dir_t;
typedef enum { SRI_SRC_BP = 0, SRI_SRC_IB = 1 } sri_src_t;
typedef enum {
    SRI_RESP_OK = 0,
    SRI_RESP_DIAG_ONLY = 1,
    SRI_RESP_UNKNOWN = 2
    } sri_resp_t;

typedef struct {
    uint32 bp_base;
    uint32 ib_base;
    uint16 bp_vec_a;
    uint16 bp_vec_b;
    uint16 ib_vec_a;
    uint16 ib_vec_b;
    int bp_over_ib;                             /* KSE-BP ahead of KSE-IB */
    int a_over_b;
    int bp_ipl;
    int ib_ipl;
    uint16 pkcsr_ie_a;                          /* surrogate enable, not hardware */
    uint16 pkcsr_ie_b;
    int no_autoinc_bpodr;
    } sri_config_t;

typedef struct {
    const char *name;
    uint16 off;
    uint16 named;
    uint16 observed;
    unsigned readable : 1;
    unsigned writable : 1;
    } sri_reg_desc_t;

typedef struct {
    uint32 seq;
    int32 addr;
    sri_dir_t dir;
    sri_src_t src;
    uint16 data;
    int32 time;
    sri_resp_t resp;
    } sri_journal_entry_t;

typedef struct {
    sri_config_t cfg;
    uint16 bp_reg[SRI_REG_SLOTS];
    uint16 ib_reg[SRI_REG_SLOTS];
    int pending[2][2];
    int enabled[2][2];
    uint32 irq_count[2][2];
    sri_journal_entry_t journal[SRI_JOURNAL_DEPTH];
    uint32 journal_seq;
    unsigned journal_head;
    } sri_state_t;

void sri_defaults (sri_state_t *s, const sri_config_t *cfg);
t_stat sri_rd (int32 *data, int32 pa, int32 access);
t_stat sri_wr (int32 data, int32 pa, int32 access);
t_stat sri_ack (sri_src_t src, int channel);
int32 sri_vector (sri_src_t src, int channel);
int sri_top_request (const sri_state_t *s, sri_src_t *src, int *channel);
void sri_assert_request (sri_state_t *s, sri_src_t src, int channel);
void sri_clear_request (sri_state_t *s, sri_src_t src, int channel);
const sri_reg_desc_t *sri_find_reg (sri_src_t src, uint16 off, sri_dir_t dir);
uint16 sri_unknown_mask (const sri_reg_desc_t *d);
unsigned sri_journal_count (const sri_state_t *s);
const sri_journal_entry_t *sri_journal_at (const sri_state_t *s, unsigned i);

extern sri_state_t sri_state;

#endif

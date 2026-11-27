/* topif.h: autonomous TOP interface model for the E10025 simulator.
 *
 * The device owns its status, request and state registers.  Guest accesses to
 * the register window are handled by the device through a DIB; the published
 * low-memory status cells are written by the device from that same state.
 * Bit positions that the available descriptions do not settle stay zero and
 * are reported as UNKNOWN; they are never invented as named hardware signals.
 */

#ifndef TOP_H
#define TOP_H

#include "e10025_defs.h"

/* Guest I/O-page form of the TOP register window.  The physical address is
 * IOPAGEBASE plus these low I/O-page bits. */
#define TOP_IO_LO        0167000u
#define TOP_IO_HI        0167176u
#define TOP_PKCSR_ADDR   0167012u
#define TOP_IKCSR_ADDR   0167014u
#define TOP_BPSRA_ADDR   0167016u

/* Low-memory cells published by the device from its authoritative state. */
#define TOP_CELL_A       0000340u
#define TOP_CELL_B       0000350u
#define TOP_CELL_STATE   0000356u

/* Vectors delivered by this device.  The documented BP channel numbers are
 * already registered by another interface, so this independent device uses a
 * free pair for delivery; the documented I-side names stay in the acknowledge
 * path as constants and are not claimed as device vectors. */
#define TOP_VEC_A        0340u
#define TOP_VEC_B        0344u
#define TOP_VEC_I_A      0330u
#define TOP_VEC_I_B      0334u

/* Register offsets inside the TOP window (octal). */
enum top_off {
    TOP_OFF_STATUS_A   = 0000,
    TOP_OFF_STATUS_B   = 0002,
    TOP_OFF_STATE      = 0004,
    TOP_OFF_REQUEST    = 0006,
    TOP_OFF_BPIMA      = 0010,
    TOP_OFF_PKCSR      = 0012,
    TOP_OFF_IKCSR      = 0014,
    TOP_OFF_BPSRA      = 0016,
    TOP_OFF_BPFR1      = 0020,
    TOP_OFF_BPFR2      = 0022,
    TOP_OFF_BPCLR      = 0024,
    TOP_OFF_IBCLR      = 0026,
    TOP_OFF_JOURNAL    = 0030,
    TOP_OFF_SEQUENCE   = 0032,
    TOP_OFF_VECTOR_A   = 0034,
    TOP_OFF_VECTOR_B   = 0036,
    TOP_OFF_IRQ        = 0040,
    TOP_OFF_SIGSEL     = 0100,
    TOP_OFF_SIGVAL     = 0102,
    TOP_OFF_SIGSTATE   = 0104,
    TOP_OFF_SIGSEQ     = 0106,
    TOP_OFF_SIGCOUNT   = 0110,
    TOP_OFF_SIGGROUP   = 0112,
    TOP_OFF_SIGHANDLER = 0114,
    TOP_OFF_SIGSTATUS  = 0116,
    TOP_OFF_SIGACTIVITY= 0120,
    TOP_OFF_SIGDIR     = 0122,
    TOP_OFF_SIGTOTAL   = 0124,
    TOP_OFF_BRIDGE     = 0126,
    TOP_OFF_BRIDGE_CALL= 0130,
    TOP_OFF_BRIDGE_ERR = 0132
    };

/* Documented status bits.  Bits not listed here are UNKNOWN and stay zero. */
#define TOP_BPSRA_KWF   0001u                             /* coordinate range */
#define TOP_BPSRA_INE   0002u                             /* input ready */
#define TOP_BPSRA_FHT   0004u                             /* figure halt */
#define TOP_BPSRA_INO   0010u                             /* output ready */
#define TOP_BPSRA_RFF   0020u
#define TOP_BPSRA_TRF   0040u
#define TOP_BPSRA_OHT   0100u                             /* data halt */
#define TOP_BPSRA_TRA   0200u                             /* transfer request */
#define TOP_BPSRA_OPSF  0400u
#define TOP_BPSRA_MRQF  01000u
#define TOP_BPSRA_FMHT  02000u
#define TOP_BPSRA_FEDF  04000u                            /* end of figure */
#define TOP_BPSRA_NAMED 07777u
#define TOP_BPSRA_SEEN  020000u                           /* observed, unnamed */

/* Request lines.  Channel A has priority over B; the BP source has priority
 * over the IB source in the shared interrupt chain. */
#define TOP_REQ_A       0001u
#define TOP_REQ_B       0002u

/* Printed request-status positions and channel-enable positions disagree
 * between sources.  Both documented sets are kept; none is presented as an
 * established hardware bit layout. */
#define TOP_PKCSR_REQ_A  0200u
#define TOP_PKCSR_REQ_B  0100000u
#define TOP_PKCSR_EN_A   010000u
#define TOP_PKCSR_EN_B   020000u
#define TOP_IKCSR_EN_A   0001u
#define TOP_IKCSR_EN_B   0002u

/* The mask register is documented as selecting a request channel from an
 * active status signal.  Its printed bit positions are UNKNOWN; the two low
 * positions are used only as a neutral channel selector for this model. */
#define TOP_BPIMA_A      0001u
#define TOP_BPIMA_B      0002u

/* STATUS_B bit 14 starts the interactive dialog on device startup. */
#define TOP_STATUS_B_DIALOG  040000u
#define TOP_STATUS_A_NAMED   0177777u

/* Service selectors returned by the descriptor path and consumed by the
 * external-call path.  They are device service codes, not hardware register
 * bits; a selector outside this set is left to the standard trap. */
#define TOP_SELECT_CELL  0u
#define TOP_SELECT_STATE 2u

/* Real guest memory used by the installed shim.  The code writes a short
 * RT-11 .PRINT stub here; it is guest memory, not a host-side shadow. */
#define TOP_SHIM_CODE    0150000u
#define TOP_SHIM_BUFFER  0150100u
#define TOP_PROTOCOL_LINES 15u
#define TOP_SVC_DESC    0x44455343u
#define TOP_SVC_EXT     0x45585420u
#define TOP_SVC_PROT    0x50524F54u
#define TOP_SVC_NEXT    0x4E455854u
#define TOP_SVC_ROLE    0x524F4C45u /* "ROLE": blueprint/role at a TOP halt */
#define TOP_SVC_FAULT   0x4641554Cu /* "FAULT": documented fatal TRAP PC */
#define TOP_ROLE_FLAG_ERR   0x02u
#define TOP_ROLE_FLAG_LINE  0x10u

#define TOP_WORD_MASK    0177777u
#define TOP_REG_SLOTS    0200u
#define TOP_JOURNAL_DEPTH 16u

/* The printed SR-BUS line list accounts for 56 logical conductors in the
 * A01-A16 ranges.  Each has an entry below.  Additional entries carry the
 * related interface, request and arbitration names from the other tables;
 * they use the same handler and journal path.  The last entry is the
 * IRQ_LEVEL property: 56 printed + 34 related + 1 property = 91 entries. */
#define TOP_SIG_A_COUNT  56u
#define TOP_SIG_COUNT    91u

typedef enum {
    TOP_SIG_UA0 = 0,  TOP_SIG_UA1,  TOP_SIG_UA2,  TOP_SIG_UA3,
    TOP_SIG_UA4,  TOP_SIG_UA5,  TOP_SIG_UA6,  TOP_SIG_UA7,
    TOP_SIG_UA8,  TOP_SIG_UA9,  TOP_SIG_UA10, TOP_SIG_UA11,
    TOP_SIG_UA12, TOP_SIG_UA13, TOP_SIG_UA14, TOP_SIG_UA15,
    TOP_SIG_UA16, TOP_SIG_UA17,
    TOP_SIG_UD0,  TOP_SIG_UD1,  TOP_SIG_UD2,  TOP_SIG_UD3,
    TOP_SIG_UD4,  TOP_SIG_UD5,  TOP_SIG_UD6,  TOP_SIG_UD7,
    TOP_SIG_UD8,  TOP_SIG_UD9,  TOP_SIG_UD10, TOP_SIG_UD11,
    TOP_SIG_UD12, TOP_SIG_UD13, TOP_SIG_UD14, TOP_SIG_UD15,
    TOP_SIG_UC0, TOP_SIG_UC1,
    TOP_SIG_UMSYN, TOP_SIG_USSYN, TOP_SIG_UPA, TOP_SIG_UPB,
    TOP_SIG_UBR7, TOP_SIG_UBR6, TOP_SIG_UBR5, TOP_SIG_UBR4,
    TOP_SIG_UBG7, TOP_SIG_UBG6, TOP_SIG_UBG5, TOP_SIG_UBG4,
    TOP_SIG_UNPR, TOP_SIG_UNPG, TOP_SIG_USACK, TOP_SIG_UBBSY,
    TOP_SIG_UINTR, TOP_SIG_UINIT, TOP_SIG_UACLO, TOP_SIG_UDCLO,
    TOP_SIG_BPDO, TOP_SIG_ODSTB, TOP_SIG_BPCDT, TOP_SIG_BPSSYN,
    TOP_SIG_BPDI, TOP_SIG_BPREQA, TOP_SIG_BPREQB, TOP_SIG_BPCLR,
    TOP_SIG_BSS, TOP_SIG_PCLRD, TOP_SIG_PRESET, TOP_SIG_SYN1,
    TOP_SIG_SYN2, TOP_SIG_ODRE, TOP_SIG_IBA, TOP_SIG_IBD,
    TOP_SIG_TBOUTL, TOP_SIG_TBOUTH, TOP_SIG_TBINP, TOP_SIG_IBREQA,
    TOP_SIG_IBREQB, TOP_SIG_IBCLR, TOP_SIG_IRESET, TOP_SIG_ISSYN,
    TOP_SIG_IKREQA, TOP_SIG_PKREQA, TOP_SIG_PKSIEN, TOP_SIG_PMTSS,
    TOP_SIG_BPIMA, TOP_SIG_BPSRA, TOP_SIG_INTVA, TOP_SIG_INTVB,
    TOP_SIG_PRIO_AB, TOP_SIG_PRIO_BP_IB, TOP_SIG_IRQ_LEVEL
    } top_signal_id_t;

typedef enum {
    TOP_SG_ADDRESS = 1, TOP_SG_DATA, TOP_SG_CONTROL, TOP_SG_HANDSHAKE,
    TOP_SG_PARITY, TOP_SG_REQUEST, TOP_SG_GRANT, TOP_SG_DMA,
    TOP_SG_ACK, TOP_SG_BUSY, TOP_SG_INTERRUPT, TOP_SG_INIT,
    TOP_SG_POWER, TOP_SG_OUTPUT, TOP_SG_STROBE, TOP_SG_INPUT,
    TOP_SG_RESET, TOP_SG_SYNC, TOP_SG_FLAG, TOP_SG_LATCH,
    TOP_SG_ENABLE, TOP_SG_REGISTER, TOP_SG_VECTOR, TOP_SG_PRIORITY,
    TOP_SG_PROPERTY
    } top_sig_group_t;

typedef enum {
    TOP_SH_ADDR_BIT = 1, TOP_SH_DATA_BIT, TOP_SH_DIRECTION,
    TOP_SH_MASTER_SYNC, TOP_SH_SLAVE_SYNC, TOP_SH_PARITY_BIT,
    TOP_SH_BUS_REQUEST, TOP_SH_BUS_GRANT, TOP_SH_DMA_REQUEST,
    TOP_SH_DMA_GRANT, TOP_SH_ACKNOWLEDGE, TOP_SH_BUS_BUSY,
    TOP_SH_INTERRUPT, TOP_SH_INITIALIZE, TOP_SH_POWER_LOW,
    TOP_SH_OUTPUT_WORD, TOP_SH_OUTPUT_STROBE, TOP_SH_CONTROL_STROBE,
    TOP_SH_INPUT_SELECT, TOP_SH_LOCAL_RESET, TOP_SH_LOCAL_SYNC,
    TOP_SH_FLAG, TOP_SH_LATCH, TOP_SH_ENABLE, TOP_SH_REGISTER,
    TOP_SH_VECTOR, TOP_SH_PRIORITY, TOP_SH_PROPERTY
    } top_sig_handler_t;

typedef enum { TOP_DIR_READ = 0, TOP_DIR_WRITE = 1 } top_dir_t;
typedef enum { TOP_SRC_BP = 0, TOP_SRC_IB = 1 } top_src_t;
typedef enum {
    TOP_RESP_OK = 0,
    TOP_RESP_READ_ONLY = 1,
    TOP_RESP_UNKNOWN_BITS = 2,
    TOP_RESP_DIAG_ONLY = 3,
    TOP_RESP_BRIDGE = 4
    } top_resp_t;
typedef enum {
    TOP_STATE_RESET = 0,
    TOP_STATE_INIT = 1,
    TOP_STATE_READY = 2,
    TOP_STATE_REQUEST = 3,
    TOP_STATE_SERVICE = 4,
    TOP_STATE_DIALOG = 5
    } top_mode_t;

typedef struct {
    uint32 bp_base;
    uint32 vec_a;
    uint32 vec_b;
    uint16 cell_a_start;
    uint16 cell_b_start;
    uint16 cell_state_start;
    uint16 cell_a_value;
    uint16 cell_b_value;
    uint16 cell_state_value;
    uint16 bpsra_start;
    uint16 bpfr1_start;
    uint16 bpfr2_start;
    int bp_over_ib;
    int a_over_b;
    } top_config_t;

typedef struct {
    const char *name;
    uint16 off;
    uint16 named;
    uint16 observed;
    unsigned readable : 1;
    unsigned writable : 1;
    } top_reg_desc_t;

typedef struct {
    uint32 seq;
    int32 addr;
    int32 pc;
    top_dir_t dir;
    top_src_t src;
    uint16 data;
    top_resp_t resp;
    top_mode_t state;
    } top_journal_entry_t;

typedef struct {
    const char *name;
    uint8 group;
    uint8 handler;
    uint8 status;
    uint8 activity;
    uint8 direction;
    uint8 value;
    uint16 transitions;
    uint32 last_seq;
    } top_signal_t;

typedef struct {
    top_config_t cfg;
    uint16 reg[TOP_REG_SLOTS];
    uint16 cell_a;
    uint16 cell_b;
    uint16 cell_state;
    int pending[2][2];
    int enabled[2][2];
    uint32 irq_count[2][2];
    uint32 transition_count;
    uint32 publish_count;
    top_signal_t sig[TOP_SIG_COUNT];
    uint32 sig_selected;
    uint32 bus_addr;
    uint16 bus_data;
    uint16 transfer_dir;
    uint16 handshake;
    uint16 arb_level;
    uint16 grant_level;
    uint16 dma_pending;
    uint16 bus_busy;
    uint16 power_low;
    uint16 power_saved_a;
    uint16 power_saved_b;
    uint16 power_saved_state;
    uint16 parity_error;
    uint16 irq_level;
    uint32 descriptor_ptr;
    uint16 descriptor_active;
    uint16 descriptor_status;
    uint16 external_calls;
    uint16 protocol_pages;
    uint16 protocol_line;
    uint16 next_page;
    uint16 next_index;
    uint16 chunk_off;
    uint16 chunk_len;
    uint16 reg_heading_stage;
    unsigned char chunk[256];
    uint16 pending_print;
    uint16 bridge_status;
    uint32 bridge_errors;
    uint32 bridge_calls;
    top_mode_t state;
    top_journal_entry_t journal[TOP_JOURNAL_DEPTH];
    uint32 journal_seq;
    unsigned journal_head;
    } top_state_t;

extern top_state_t top_state;
extern DEVICE top_dev;

void topif_defaults (top_state_t *s, const top_config_t *cfg);
void topif_reset_state (top_state_t *s);
int topif_low_cell (int32 pa, uint16 *slot);
uint16 topif_read_low (int32 pa, int32 pc);
void topif_write_low (int32 pa, uint16 data, int32 pc);
void topif_publish (void);
t_stat topif_rd (int32 *data, int32 pa, int32 access);
t_stat topif_wr (int32 data, int32 pa, int32 access);
t_stat topif_ack_a (void);
t_stat topif_ack_b (void);
int32 topif_vector (top_src_t src, int channel);
int topif_top_request (const top_state_t *s, top_src_t *src, int *channel);
void topif_assert_request (top_state_t *s, top_src_t src, int channel);
void topif_clear_request (top_state_t *s, top_src_t src, int channel);
const top_reg_desc_t *topif_find_reg (uint16 off, top_dir_t dir);
uint16 topif_unknown_mask (const top_reg_desc_t *d);
unsigned topif_journal_count (const top_state_t *s);
const top_journal_entry_t *topif_journal_at (const top_state_t *s, unsigned i);
const char *topif_state_name (top_mode_t state);
void topif_drive_signal (top_state_t *s, uint32 id, uint16 value);
uint16 topif_signal_value (const top_state_t *s, uint32 id);
uint16 topif_signal_state (const top_state_t *s, uint32 id);
uint16 topif_signal_seq (const top_state_t *s, uint32 id);
uint8 topif_signal_handler (const top_state_t *s, uint32 id);
uint8 topif_signal_group (const top_state_t *s, uint32 id);
uint8 topif_signal_status (const top_state_t *s, uint32 id);
uint8 topif_signal_activity (const top_state_t *s, uint32 id);
uint8 topif_signal_direction (const top_state_t *s, uint32 id);
const char *topif_signal_name (uint32 id);
int topif_emt_service (int32 code);
int topif_trap_service (int32 code);
int topif_role_service (int32 pc);
int topif_fault_service (int32 vec, int32 pc);
int topif_pending_print (void);
void topif_clear_print (void);
int topif_bridge_online (void);
uint32 topif_bridge_calls (void);
uint32 topif_bridge_errors (void);
int topif_bridge_read (uint32 addr, uint16 *data);
int topif_bridge_write (uint32 addr, uint16 data);
int topif_bridge_service (uint32 kind, uint32 a, uint32 b, uint32 c,
                          uint32 *r0, uint32 *r1, uint32 *r2, uint32 *flags);
int topif_bridge_irq (uint32 src, uint32 channel);
int topif_bridge_data (uint32 offset, unsigned char *dst, uint32 length);
int topif_bridge_line (unsigned char *dst, uint32 cap, uint32 *length);
int topif_bridge_next (uint32 page, uint32 index, uint32 *status,
                       uint32 *length, uint32 *checksum, uint32 *flags);
uint16 topif_descriptor_service (top_state_t *s, uint16 selector, uint16 operand);

#endif

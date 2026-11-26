/* e10025_bus_sig.h: named physical lines of the unified channel
   (Единый канал) of the Электроника-100-25.

   Source: ИЧМ2.791.003 ТО2, книга 4, печ. с. 13-17, таблица 2: 56 линий,
   активный уровень низкий у большинства сигналов. Дополнительные сигналы
   15BBB (Разр. Прр, Прр, ПЕ) не добавляют линий сверх этих 56.

   Copyright (c) 2026, Электроника-100-25 simulator contributors

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

#ifndef E10025_BUS_SIG_H_
#define E10025_BUS_SIG_H_

#include "e10025_defs.h"

/* Exactly 56 lines, in the order of book 4, table 2. */
#define BUS_SIG_NLINES      56
#define BUS_SIG_NADR        18
#define BUS_SIG_NDATA       16

/* Active physical level of a line. Н - low, B - high (book 4, table 1). */
enum {
    BUS_SIG_ACTIVE_LOW = 0,
    BUS_SIG_ACTIVE_HIGH = 1
    };

/* Indices are stable and match the phase driver below and the debug log. */
enum {
    BUS_SIG_ADR17 = 0, BUS_SIG_ADR16, BUS_SIG_ADR15, BUS_SIG_ADR14,
    BUS_SIG_ADR13, BUS_SIG_ADR12, BUS_SIG_ADR11, BUS_SIG_ADR10,
    BUS_SIG_ADR09, BUS_SIG_ADR08, BUS_SIG_ADR07, BUS_SIG_ADR06,
    BUS_SIG_ADR05, BUS_SIG_ADR04, BUS_SIG_ADR03, BUS_SIG_ADR02,
    BUS_SIG_ADR01, BUS_SIG_ADR00,

    BUS_SIG_D15, BUS_SIG_D14, BUS_SIG_D13, BUS_SIG_D12,
    BUS_SIG_D11, BUS_SIG_D10, BUS_SIG_D09, BUS_SIG_D08,
    BUS_SIG_D07, BUS_SIG_D06, BUS_SIG_D05, BUS_SIG_D04,
    BUS_SIG_D03, BUS_SIG_D02, BUS_SIG_D01, BUS_SIG_D00,

    BUS_SIG_VV1, BUS_SIG_VV0,
    BUS_SIG_SA, BUS_SIG_SP,
    BUS_SIG_RKCH1, BUS_SIG_RKCH2,
    BUS_SIG_ZNDK,
    BUS_SIG_ZK7, BUS_SIG_ZK6, BUS_SIG_ZK5, BUS_SIG_ZK4,
    BUS_SIG_PKND,
    BUS_SIG_PK7, BUS_SIG_PK6, BUS_SIG_PK5, BUS_SIG_PK4,
    BUS_SIG_PV, BUS_SIG_KZ, BUS_SIG_PRR, BUS_SIG_SVROS,
    BUS_SIG_MN_VAR, BUS_SIG_MN_CONST
    };

/* Mirror of the channel phase numbers in e10025_chan.c. The values are
   identical so chan_run_cycle can pass CH_* straight into bus_sig_phase(). */
enum {
    BUS_SIG_PH_IDLE = 0, BUS_SIG_PH_ADDRESS, BUS_SIG_PH_SA,
    BUS_SIG_PH_WAIT_SACK, BUS_SIG_PH_STROBE, BUS_SIG_PH_RELEASE,
    BUS_SIG_PH_CLEANUP, BUS_SIG_PH_ERROR
    };

typedef struct {
    const char *name;                                   /* book notation, UTF-8 */
    const char *code;                                   /* ASCII tag for logs */
    int active;                                         /* active physical level */
    int level;                                          /* current physical level */
    } BUS_SIG_LINE;

typedef struct {
    int registered;                                     /* set by bus_sig_init() */
    int last_phase;                                     /* last driven phase */
    BUS_SIG_LINE line[BUS_SIG_NLINES];
    } BUS_SIG_STATE;

extern BUS_SIG_STATE bus_sig;

void bus_sig_init (void);
int bus_sig_registered (void);
int bus_sig_count (void);
int bus_sig_index (const char *code_or_name);
const char *bus_sig_name (int idx);
const char *bus_sig_code (int idx);
int bus_sig_active (int idx);
int bus_sig_level (int idx);
int bus_sig_sample (int idx);
void bus_sig_set (int idx);
void bus_sig_clear (int idx);
void bus_sig_drive (int idx, int level);
void bus_sig_clear_all (void);
void bus_sig_clear_xfer (void);
void bus_sig_drive_addr (int32 addr);
void bus_sig_drive_data (int32 data);
void bus_sig_drive_op (int32 op);
void bus_sig_drive_br (int level, int req, int grant);
void bus_sig_drive_prr (int assert_prr);
void bus_sig_phase (int phase, int32 addr, int32 data, int32 op);
void bus_sig_format (char *buf, size_t len);
void bus_sig_dump (FILE *st);
const char *bus_sig_debug_text (void);            /* short summary only */
const char *bus_sig_debug_line (int idx);         /* one line, under 1 KB */

#endif

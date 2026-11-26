/* e10025_bus_sig.c: named physical lines of the unified channel

   Based on ИЧМ2.791.003 ТО2, книга 4, печ. с. 13-17, таблица 2. The 56 lines
   are an observation layer only: this module never touches guest memory, M[],
   int_req, DIB tables or device registers. Буква B denotes an active high line
   and Н denotes an active low line (с. 6, таблица 1).

   15BBB signals Разр. Прр and Прр are inputs of the local У9 module and are
   not separate bus lines. Its ПК, ЗК and ПРР map to lines already present in
   the book-4 table. Its ПЕ ("Подтверждение выбора") is close to ПВ, but the
   identity is not proven, so no alias is added here. [GAP]

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

#include "e10025_bus_sig.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

BUS_SIG_STATE bus_sig;

/* Book 4, table 2. Order must match the index enum in e10025_bus_sig.h. */
static const BUS_SIG_LINE sig_table[BUS_SIG_NLINES] = {
/* АДР17...АДР00, 18 lines, active low */
    {"АДР17", "ADR17", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР16", "ADR16", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР15", "ADR15", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР14", "ADR14", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР13", "ADR13", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР12", "ADR12", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР11", "ADR11", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР10", "ADR10", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР09", "ADR09", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР08", "ADR08", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР07", "ADR07", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР06", "ADR06", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР05", "ADR05", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР04", "ADR04", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР03", "ADR03", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР02", "ADR02", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР01", "ADR01", BUS_SIG_ACTIVE_LOW, 0},
    {"АДР00", "ADR00", BUS_SIG_ACTIVE_LOW, 0},
/* Д15...Д00, 16 lines, active low */
    {"Д15", "D15", BUS_SIG_ACTIVE_LOW, 0},
    {"Д14", "D14", BUS_SIG_ACTIVE_LOW, 0},
    {"Д13", "D13", BUS_SIG_ACTIVE_LOW, 0},
    {"Д12", "D12", BUS_SIG_ACTIVE_LOW, 0},
    {"Д11", "D11", BUS_SIG_ACTIVE_LOW, 0},
    {"Д10", "D10", BUS_SIG_ACTIVE_LOW, 0},
    {"Д09", "D09", BUS_SIG_ACTIVE_LOW, 0},
    {"Д08", "D08", BUS_SIG_ACTIVE_LOW, 0},
    {"Д07", "D07", BUS_SIG_ACTIVE_LOW, 0},
    {"Д06", "D06", BUS_SIG_ACTIVE_LOW, 0},
    {"Д05", "D05", BUS_SIG_ACTIVE_LOW, 0},
    {"Д04", "D04", BUS_SIG_ACTIVE_LOW, 0},
    {"Д03", "D03", BUS_SIG_ACTIVE_LOW, 0},
    {"Д02", "D02", BUS_SIG_ACTIVE_LOW, 0},
    {"Д01", "D01", BUS_SIG_ACTIVE_LOW, 0},
    {"Д00", "D00", BUS_SIG_ACTIVE_LOW, 0},
/* В/В1, В/В0: opcode relative to the active device */
    {"В/В1", "VV1", BUS_SIG_ACTIVE_LOW, 0},
    {"В/В0", "VV0", BUS_SIG_ACTIVE_LOW, 0},
/* Handshake: активный СА, пассивный СП */
    {"К СА Н", "SA", BUS_SIG_ACTIVE_LOW, 0},
    {"К СП Н", "SP", BUS_SIG_ACTIVE_LOW, 0},
/* Контроль чётности: РКЧ1 активен высоким, РКЧ2 низким */
    {"РКЧ1", "RKCH1", BUS_SIG_ACTIVE_HIGH, 0},
    {"К РКЧ2 Н", "RKCH2", BUS_SIG_ACTIVE_LOW, 0},
/* Запрос НДК и запросы уровней 7..4 */
    {"К ЗНДК Н", "ZNDK", BUS_SIG_ACTIVE_LOW, 0},
    {"К ЗК7 Н", "ZK7", BUS_SIG_ACTIVE_LOW, 0},
    {"К ЗК6 Н", "ZK6", BUS_SIG_ACTIVE_LOW, 0},
    {"К ЗК5 Н", "ZK5", BUS_SIG_ACTIVE_LOW, 0},
    {"К ЗК4 Н", "ZK4", BUS_SIG_ACTIVE_LOW, 0},
/* Разрешения: ПКНД и ПК7..ПК4 активны высоким */
    {"ПКНД", "PKND", BUS_SIG_ACTIVE_HIGH, 0},
    {"ПК7", "PK7", BUS_SIG_ACTIVE_HIGH, 0},
    {"ПК6", "PK6", BUS_SIG_ACTIVE_HIGH, 0},
    {"ПК5", "PK5", BUS_SIG_ACTIVE_HIGH, 0},
    {"ПК4", "PK4", BUS_SIG_ACTIVE_HIGH, 0},
/* Подтверждение выбора, канал занят, прерывание, служебные линии */
    {"К ПВ Н", "PV", BUS_SIG_ACTIVE_LOW, 0},
    {"К КЗ Н", "KZ", BUS_SIG_ACTIVE_LOW, 0},
    {"К ПРР Н", "PRR", BUS_SIG_ACTIVE_LOW, 0},
    {"К СВРОС Н", "SVROS", BUS_SIG_ACTIVE_LOW, 0},
    {"К ~МН Н", "MN_VAR", BUS_SIG_ACTIVE_LOW, 0},
    {"К =МН Н", "MN_CONST", BUS_SIG_ACTIVE_LOW, 0}
    };

static const char *const phase_names[8] = {
    "IDLE", "ADDRESS", "SA", "WAIT_SACK", "STROBE", "RELEASE", "CLEANUP", "ERROR"
    };

static const char *phase_name (int phase)
{
return ((phase >= 0) && (phase < 8))? phase_names[phase]: "UNKNOWN";
}

static void append_text (char *buf, size_t len, size_t *used,
                         const char *fmt, ...)
{
va_list ap;
int n;

if (*used >= len)
    return;
va_start (ap, fmt);
n = vsnprintf (buf + *used, len - *used, fmt, ap);
va_end (ap);
if (n < 0)
    return;
if ((size_t) n >= (len - *used))
    *used = len - 1;
else
    *used += (size_t) n;
}

void bus_sig_init (void)
{
int i;

for (i = 0; i < BUS_SIG_NLINES; i++) {
    if (sig_table[i].name == NULL)                      /* incomplete table */
        return;
    bus_sig.line[i].name = sig_table[i].name;
    bus_sig.line[i].code = sig_table[i].code;
    bus_sig.line[i].active = sig_table[i].active;
    bus_sig.line[i].level = sig_table[i].active? 0: 1;  /* inactive by default */
    }
bus_sig.last_phase = BUS_SIG_PH_IDLE;
bus_sig.registered = 1;
}

int bus_sig_registered (void)
{
return bus_sig.registered;
}

int bus_sig_count (void)
{
return BUS_SIG_NLINES;
}

int bus_sig_index (const char *code_or_name)
{
int i;

if (code_or_name == NULL)
    return -1;
for (i = 0; i < BUS_SIG_NLINES; i++) {
    if ((strcmp (code_or_name, bus_sig.line[i].code) == 0) ||
        (strcmp (code_or_name, bus_sig.line[i].name) == 0))
        return i;
    }
return -1;
}

const char *bus_sig_name (int idx)
{
return ((idx >= 0) && (idx < BUS_SIG_NLINES))? bus_sig.line[idx].name: "?";
}

const char *bus_sig_code (int idx)
{
return ((idx >= 0) && (idx < BUS_SIG_NLINES))? bus_sig.line[idx].code: "?";
}

int bus_sig_active (int idx)
{
return ((idx >= 0) && (idx < BUS_SIG_NLINES))? bus_sig.line[idx].active: -1;
}

int bus_sig_level (int idx)
{
return ((idx >= 0) && (idx < BUS_SIG_NLINES))? bus_sig.line[idx].level: -1;
}

int bus_sig_sample (int idx)
{
return ((idx >= 0) && (idx < BUS_SIG_NLINES) &&
        (bus_sig.line[idx].level == bus_sig.line[idx].active));
}

void bus_sig_drive (int idx, int level)
{
if ((idx >= 0) && (idx < BUS_SIG_NLINES))
    bus_sig.line[idx].level = level? 1: 0;
}

void bus_sig_set (int idx)
{
if ((idx >= 0) && (idx < BUS_SIG_NLINES))
    bus_sig.line[idx].level = bus_sig.line[idx].active;
}

void bus_sig_clear (int idx)
{
if ((idx >= 0) && (idx < BUS_SIG_NLINES))
    bus_sig.line[idx].level = bus_sig.line[idx].active? 0: 1;
}

void bus_sig_clear_all (void)
{
int i;

for (i = 0; i < BUS_SIG_NLINES; i++)
    bus_sig_clear (i);
}

/* Transfer group only: request, grant and service lines are owned by B3 and
   are deliberately left alone by the channel phase driver. */
void bus_sig_clear_xfer (void)
{
int i;

for (i = BUS_SIG_ADR17; i <= BUS_SIG_ADR00; i++)
    bus_sig_clear (i);
for (i = BUS_SIG_D15; i <= BUS_SIG_D00; i++)
    bus_sig_clear (i);
bus_sig_clear (BUS_SIG_VV1);
bus_sig_clear (BUS_SIG_VV0);
bus_sig_clear (BUS_SIG_SA);
bus_sig_clear (BUS_SIG_SP);
bus_sig_clear (BUS_SIG_ZNDK);
}

/* Low-active bit lines: a logical 1 drives the physical low level. */
static void drive_low_bit (int idx, int bit)
{
bus_sig_drive (idx, bit? 0: 1);
}

void bus_sig_drive_addr (int32 addr)
{
uint32 a = ((uint32) addr) & ((1u << BUS_SIG_NADR) - 1);
int i;

for (i = 0; i < BUS_SIG_NADR; i++)
    drive_low_bit (BUS_SIG_ADR17 + (BUS_SIG_NADR - 1 - i), (int) ((a >> i) & 1));
}

void bus_sig_drive_data (int32 data)
{
uint32 d = ((uint32) data) & ((1u << BUS_SIG_NDATA) - 1);
int i;

for (i = 0; i < BUS_SIG_NDATA; i++)
    drive_low_bit (BUS_SIG_D15 + (BUS_SIG_NDATA - 1 - i), (int) ((d >> i) & 1));
}

/* The CH_OP_* codes are octal 00/01/010/011: В/В1 is bit 3 and В/В0 is
   bit 0, matching the existing channel opcode register. */
void bus_sig_drive_op (int32 op)
{
drive_low_bit (BUS_SIG_VV1, (int) ((op >> 3) & 1));
drive_low_bit (BUS_SIG_VV0, (int) (op & 1));
}

void bus_sig_drive_br (int level, int req, int grant)
{
int i;

for (i = 0; i < 4; i++) {
    int lv = 7 - i;
    int is_level = (level == lv);

    drive_low_bit (BUS_SIG_ZK7 + i, is_level && req);    /* ЗК active low */
    bus_sig_drive (BUS_SIG_PK7 + i, is_level && grant);  /* ПК active high */
    }
}

void bus_sig_drive_prr (int assert_prr)
{
drive_low_bit (BUS_SIG_PRR, assert_prr);
}

/* Apply the book-4 phase to the visible lines. Address, opcode and output
   data appear on ADDRESS; СА is asserted on SA; СП is the passive answer on
   WAIT_SACK; input or output data is stable on STROBE; both handshake lines
   and the transfer group are released by CLEANUP. */
void bus_sig_phase (int phase, int32 addr, int32 data, int32 op)
{
bus_sig.last_phase = phase;
switch (phase) {
case BUS_SIG_PH_ADDRESS:
    bus_sig_clear_xfer ();
    bus_sig_drive_addr (addr);
    bus_sig_drive_op (op);
    if ((op == 010) || (op == 011))                     /* ВЫВОД, ВЫВОДБ */
        bus_sig_drive_data (data);
    break;
case BUS_SIG_PH_SA:
    bus_sig_set (BUS_SIG_SA);
    break;
case BUS_SIG_PH_WAIT_SACK:
    bus_sig_set (BUS_SIG_SP);
    break;
case BUS_SIG_PH_STROBE:
    bus_sig_set (BUS_SIG_SP);
    if ((op == 00) || (op == 01))                       /* ВВОД, ВВОДП */
        bus_sig_drive_data (data);                      /* answer from passive */
    break;                                              /* ВЫВОД data held since ADDRESS */
case BUS_SIG_PH_RELEASE:
    bus_sig_clear (BUS_SIG_SA);
    break;
case BUS_SIG_PH_CLEANUP:
    bus_sig_clear_xfer ();
    break;
case BUS_SIG_PH_ERROR:
    bus_sig_clear (BUS_SIG_SA);
    bus_sig_clear (BUS_SIG_SP);
    break;
case BUS_SIG_PH_IDLE:
default:
    bus_sig_clear_xfer ();
    break;
    }
}

/* sim_debug() formats into a 2048 byte stack buffer and retries a long
   result with an already consumed va_list. Keep every record below 1 KB. */
static void format_summary (char *buf, size_t len)
{
size_t used = 0;
int i, low = 0, high = 0;

if ((buf == NULL) || (len == 0))
    return;
for (i = 0; i < BUS_SIG_NLINES; i++) {
    if (bus_sig.line[i].active == BUS_SIG_ACTIVE_HIGH)
        high++;
    else
        low++;
    }
append_text (buf, len, &used,
             "SIG module=bus_sig registered=%d lines=%d active_low=%d active_high=%d phase=%s\n",
             bus_sig.registered, BUS_SIG_NLINES, low, high,
             phase_name (bus_sig.last_phase));
buf[used] = '\0';
}

static void format_line (int idx, char *buf, size_t len)
{
size_t used = 0;
const BUS_SIG_LINE *lp;

if ((buf == NULL) || (len == 0))
    return;
if ((idx < 0) || (idx >= BUS_SIG_NLINES)) {
    append_text (buf, len, &used, "SIG line=?? code=? name=? active=? level=-1 state=?\n");
    buf[used] = '\0';
    return;
    }
lp = &bus_sig.line[idx];
append_text (buf, len, &used,
             "SIG line=%02d code=%s name=%s active=%s level=%d state=%s\n",
             idx + 1, lp->code, lp->name,
             (lp->active == BUS_SIG_ACTIVE_HIGH)? "high": "low",
             lp->level, (lp->level == lp->active)? "active": "inactive");
buf[used] = '\0';
}

void bus_sig_format (char *buf, size_t len)
{
char part[1024];
size_t used = 0;
int i;

if ((buf == NULL) || (len == 0))
    return;
format_summary (part, sizeof (part));
append_text (buf, len, &used, "%s", part);
for (i = 0; i < BUS_SIG_NLINES; i++) {
    format_line (i, part, sizeof (part));
    append_text (buf, len, &used, "%s", part);
    }
buf[used] = '\0';
}

void bus_sig_dump (FILE *st)
{
char text[8192];

if (st == NULL)
    return;
bus_sig_format (text, sizeof (text));
fputs (text, st);
}

const char *bus_sig_debug_text (void)
{
static char text[256];

format_summary (text, sizeof (text));
return text;
}

const char *bus_sig_debug_line (int idx)
{
static char text[1024];

format_line (idx, text, sizeof (text));
return text;
}

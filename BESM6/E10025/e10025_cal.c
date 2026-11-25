/* e10025_cal.c: instruction timing modes for Электроника-100-25

   Two documented sources coexist here.  The 1998 article's opcode-delta file
   is applied as simulator ticks in "ticks" mode (the default), preserving the
   old CPU TICKS counter.  "historical" mode instead uses the microsecond
   durations of book 3, printed pp.109-112, tables 5-11, and schedules each
   duration on the SIMH clock with sim_timer_activate_after (sim_timer.h).
   "fast" applies neither.  The 75 ns and 150 ns figures are below one SIMH
   tick, so ticks are never claimed to be host nanoseconds.
*/

#include "e10025_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern DEVICE cpu_dev;

struct cal_entry {
    int32 opcode;
    int32 delta;
    };

static struct cal_entry cal_tab[E10025_CAL_MAX];
static uint32 cal_count = 0;
static char cal_path[CBUFSIZE] = "";
static int32 cal_mode = E10025_MODE_TICKS;
int32 e10025_memdisp = 0;
int32 cal_ticks = 0;

/* Book 3, printed p.109-110, tables 5 and 6: methods 0..7. */
static const double method_usec[8] = {
    0.00, 0.94, 1.00, 1.88, 1.00, 1.88, 1.60, 2.30
    };
static const int32 method_cyc[8] = { 0, 1, 1, 2, 1, 2, 2, 3 };
/* Table 6 parenthesis values for ADD/SUB/CMP/BIT/BIC/BIS with source method 0. */
static const double dst_bracket[8] = {
    0.00, 1.10, 1.10, 1.98, 1.10, 1.98, 1.90, 2.70
    };

/* Book 3, printed p.111, table 8 (MOV).  Index 0 is source method 0,
   index 1 is source methods 1..7.  The odd-byte columns already include
   the 0.54 us note, so they are used directly instead of adding it twice. */
static const double mov_word[2][8] = {
    { 1.00, 2.80, 2.80, 3.60, 2.80, 3.60, 3.20, 4.20 },
    { 1.60, 2.80, 2.80, 3.60, 2.80, 3.60, 3.54, 4.36 }
    };
static const double mov_odd[2][8] = {
    { 2.02, 3.00, 3.00, 3.70, 3.00, 3.70, 3.40, 4.40 },
    { 2.02, 3.00, 3.00, 3.70, 3.00, 3.70, 3.74, 4.66 }
    };
static const int32 mov_cyc[2][8] = {
    { 0, 2, 2, 3, 2, 3, 3, 4 },
    { 0, 2, 2, 3, 2, 3, 3, 4 }
    };

/* Book 3, printed p.111-112, table 9 (one-address).  Index 0 is method 0,
   index 1 is methods 1..7. */
static const double one_a[2] = { 1.20, 2.36 };    /* CLR COM NEG INC DEC ADC SBC TST ROL ASL SWAB */
static const double one_r[2] = { 1.40, 2.54 };    /* ROR ASR */
static const double one_s[2] = { 1.00, 2.18 };    /* SXT */
static const int32 one_cyc[2] = { 1, 2 };

/* Book 3, printed p.110, table 7 (execution). */
static const double exec_add[3] = { 1.20, 1.78, 2.18 };
static const double exec_sub[3] = { 1.20, 1.78, 2.36 };
static const int32 exec_cyc[3] = { 1, 1, 2 };

/* The event only marks elapsed simulated time.  It is rescheduled for every
   instruction in historical mode, so its service routine has no work. */
static t_stat cal_delay_svc (UNIT *uptr)
{
return SCPE_OK;
}

static UNIT cal_delay_unit = { UDATA (&cal_delay_svc, UNIT_IDLE, 0) };

/* Read-only parity of a byte effective address.  Getea in the CPU mutates
   auto-increment registers, so the timing hook must not call it. */
static int32 cal_odd_byte (int32 spec, int32 byte)
{
int32 mode = (spec >> 3) & 7;
int32 reg = spec & 7;
int32 incr = (byte && (reg < 6)) ? 1 : 2;
int32 ea;

if (!byte || (mode == 0))
    return 0;
switch (mode) {
case 1:                                             /* (Rn) */
case 2:                                             /* (Rn)+ */
    ea = R[reg];
    break;
case 3:                                             /* @(Rn)+ */
    ea = ReadW (R[reg]);
    break;
case 4:                                             /* -(Rn) */
    ea = (R[reg] - incr) & 0177777;
    break;
case 5:                                             /* @-(Rn) */
    ea = ReadW ((R[reg] - 2) & 0177777);
    break;
case 6:                                             /* X(Rn) */
    ea = (ReadW (PC) + R[reg]) & 0177777;
    break;
case 7:                                             /* @X(Rn) */
    ea = ReadW ((ReadW (PC) + R[reg]) & 0177777);
    break;
default:
    return 0;
    }
return ea & 1;
}

static int32 cal_branch_cond (int32 bop)
{
switch (bop) {
case 001: return 1;                                 /* BR */
case 002: return !Z;                                /* BNE */
case 003: return Z;                                 /* BEQ */
case 004: return !(N ^ V);                          /* BGE */
case 005: return (N ^ V);                           /* BLT */
case 006: return !Z && !(N ^ V);                    /* BGT */
case 007: return Z || (N ^ V);                      /* BLE */
case 0200: return !N;                               /* BPL */
case 0201: return N;                                /* BMI */
case 0202: return !C && !Z;                         /* BHI */
case 0203: return C || Z;                           /* BLOS */
case 0204: return !V;                               /* BVC */
case 0205: return V;                                /* BVS */
case 0206: return !C;                               /* BHIS */
case 0207: return C;                                /* BLO */
default: return -1;                                 /* not a branch */
    }
}

/* t = t_src + t_dst + t_exec; MOV uses t_src + t_exec, one-address uses
   t_dst + t_exec, branches and program control use t_exec only.  Values not
   present in tables 5-11 stay at zero and are named "GAP" in the debug log. */
static void cal_book_time (int32 IR, double *usec, int32 *cycles, const char **kind)
{
int32 opcode = (IR >> 12) & 017;
int32 byte = ((IR & 0100000) && (opcode != 016)) ? 1 : 0;
int32 srcspec = (IR >> 6) & 077;
int32 dstspec = IR & 077;
int32 srcm = (srcspec >> 3) & 7;
int32 dstm = (dstspec >> 3) & 7;
int32 src0 = (srcspec & 070) == 0;
int32 dst0 = (dstspec & 070) == 0;
int32 sub = (IR >> 6) & 077;
int32 idx, cond, reg, op;
double ts, td, te;
const double *base;

*usec = 0.0;
*cycles = 0;
*kind = "GAP";

if ((opcode >= 01 && opcode <= 06) || (opcode >= 011 && opcode <= 015) ||
    (opcode == 016)) {
    op = (opcode >= 011 && opcode <= 015)? (opcode & 07):
         ((opcode == 016)? 06: opcode);
    if (op == 01) {                                 /* MOV, table 8 */
        int32 s = src0? 0: 1;
        *usec = (byte && cal_odd_byte (dstspec, 1))? mov_odd[s][dstm]:
                                                    mov_word[s][dstm];
        *cycles = mov_cyc[s][dstm];
        *kind = "MOV";
        return;
        }
    ts = method_usec[srcm];
    td = method_usec[dstm];
    if (src0 && !dst0)
        td = dst_bracket[dstm];                     /* table 6, note 2 */
    if (byte) {
        if (cal_odd_byte (srcspec, 1))
            ts += E10025_ODD_US;
        if (cal_odd_byte (dstspec, 1))
            td += E10025_ODD_US;
        }
    idx = dst0? 0: (src0? 1: 2);
    te = (opcode == 016)? exec_sub[idx]: exec_add[idx];
    *usec = ts + td + te;
    *cycles = method_cyc[srcm] + method_cyc[dstm] + exec_cyc[idx];
    *kind = (opcode == 016)? "SUB":
            ((op == 02)? "CMP": ((op == 03)? "BIT": ((op == 04)? "BIC":
            ((op == 05)? "BIS": "ADD"))));
    return;
    }

if ((opcode == 0) || (opcode == 010)) {             /* one-address and opcode 0 */
    if ((sub >= 050 && sub <= 063) || (sub == 067) || (sub == 003)) {
        if ((sub == 060) || (sub == 062))           /* ROR, ASR */
            base = one_r;
        else if (sub == 067)                        /* SXT */
            base = one_s;
        else                                        /* CLR COM NEG INC DEC ADC SBC TST ROL ASL SWAB */
            base = one_a;
        idx = dst0? 0: 1;
        *usec = base[idx];
        if (byte && !dst0 && cal_odd_byte (dstspec, 1))
            *usec += E10025_ODD_US;
        *cycles = one_cyc[idx];
        *kind = "oneaddr";
        return;
        }
    if (sub == 011) {                               /* SOB, table 11 */
        reg = (srcspec >> 3) & 7;
        *usec = (R[reg] != 1)? 2.8: 2.5;
        *cycles = 1;
        *kind = "SOB";
        return;
        }
    cond = cal_branch_cond ((IR >> 8) & 0377);      /* table 11 */
    if (cond >= 0) {
        *usec = cond? 2.1: 1.7;
        *cycles = 1;
        *kind = "BR";
        return;
        }
    return;                                         /* HALT/WAIT/RTI/etc: no table value */
    }

if (opcode == 065) {                                /* MFPI, table 10 */
    *usec = 4.3;
    *cycles = 2;
    *kind = "MFPI";
    return;
    }
if (opcode == 066) {                                /* MTPI, table 10 */
    *usec = 4.2;
    *cycles = 2;
    *kind = "MTPI";
    return;
    }

if (opcode == 07) {
    int32 func = (IR >> 9) & 7;
    if (func == 4) {                                /* XOR, table 7 */
        idx = dst0? 0: 2;                           /* middle cell has no book value */
        *usec = (idx == 0)? 1.20: 2.18;
        *cycles = exec_cyc[idx];
        *kind = "XOR";
        return;
        }
    if (func == 7) {                                /* SOB encoded in opcode 7 */
        reg = (IR >> 6) & 7;
        *usec = (R[reg] != 1)? 2.8: 2.5;
        *cycles = 1;
        *kind = "SOB";
        return;
        }
    return;                                         /* MUL DIV ASH ASHC: [GAP] */
    }
return;
}

static t_bool cal_find (int32 opcode, int32 *delta)
{
uint32 i;

for (i = 0; i < cal_count; i++) {
    if (cal_tab[i].opcode == opcode) {
        *delta = cal_tab[i].delta;
        return TRUE;
        }
    }
return FALSE;
}

static t_stat cal_bad_line (int32 line_no, const char *path, int32 explicit)
{
cal_count = 0;
cal_ticks = 0;
if (explicit)
    cal_path[0] = 0;
return sim_messagef (SCPE_FMT, "bad calibration line %d in %s\n", (int) line_no, path);
}

t_stat e10025_cal_load (const char *fname)
{
char path[CBUFSIZE];
char line[CBUFSIZE];
FILE *f;
char *comment, *op, *dv, *end;
int32 opcode, delta, line_no = 0;
t_bool duplicate;
uint32 i;

cal_count = 0;
cal_ticks = 0;
if (fname != NULL) {
    if (*fname == 0)
        return SCPE_ARG;
    strncpy (cal_path, fname, CBUFSIZE - 1);
    cal_path[CBUFSIZE - 1] = 0;
    }
strncpy (path, cal_path[0]? cal_path: E10025_CALFILE_DEFAULT, CBUFSIZE - 1);
path[CBUFSIZE - 1] = 0;
if ((f = fopen (path, "r")) == NULL) {
    if (fname != NULL) {
        cal_path[0] = 0;
        return sim_messagef (SCPE_FMT, "cannot open calibration file %s\n", path);
        }
    return SCPE_OK;                                     /* no default file is normal */
    }
while (fgets (line, sizeof (line), f) != NULL) {
    line_no++;
    comment = strpbrk (line, "#;");
    if (comment != NULL)
        *comment = 0;
    if ((op = strtok (line, " \t\r\n")) == NULL)
        continue;                                       /* blank or comment line */
    dv = strtok (NULL, " \t\r\n");
    if (dv == NULL) {
        fclose (f);
        return cal_bad_line (line_no, path, fname != NULL);
        }
    opcode = (int32) strtol (op, &end, 8);               /* opcode is always octal */
    if ((*end != 0) || (opcode < 0) || (opcode > DMASK)) {
        fclose (f);
        return cal_bad_line (line_no, path, fname != NULL);
        }
    delta = (int32) strtol (dv, &end, 0);                /* octal with 0, else decimal */
    if ((*end != 0) || (strtok (NULL, " \t\r\n") != NULL)) {
        fclose (f);
        return cal_bad_line (line_no, path, fname != NULL);
        }
    duplicate = FALSE;
    for (i = 0; i < cal_count; i++) {
        if (cal_tab[i].opcode == opcode) {
            cal_tab[i].delta = delta;                    /* last entry wins */
            duplicate = TRUE;
            break;
            }
        }
    if (!duplicate) {
        if (cal_count >= E10025_CAL_MAX) {
            fclose (f);
            cal_count = 0;
            if (fname != NULL)
                cal_path[0] = 0;
            return sim_messagef (SCPE_INCOMP, "calibration table full in %s\n", path);
            }
        cal_tab[cal_count].opcode = opcode;
        cal_tab[cal_count].delta = delta;
        cal_count++;
        }
    }
fclose (f);
return SCPE_OK;
}

void e10025_cal_apply (int32 IR)
{
int32 delta, inst;
double usec, ips;
int32 cycles;
const char *kind;

if (cal_mode == E10025_MODE_TICKS) {
    if (!cal_find (IR & DMASK, &delta))
        return;
    cal_ticks += delta;
    sim_debug (E10025_DBG_CALIB, &cpu_dev, "calib: op %06o delta %d\n", IR & DMASK, delta);
    return;
    }
if (cal_mode != E10025_MODE_HIST)
    return;                                             /* fast mode: no ticks, no log */

cal_book_time (IR, &usec, &cycles, &kind);
if (usec <= 0.0)
    return;                                             /* [GAP] class: no delay */
if (e10025_memdisp && (cycles > 0))
    usec += E10025_MEMDISP_US * cycles;                 /* optional, book 3 p.109 */

ips = sim_timer_inst_per_sec ();
inst = (int32) (usec * ips / 1000000.0);
if (inst < 1)
    inst = 1;
cal_ticks += inst;
if (sim_is_running) {
    sim_cancel (&cal_delay_unit);                       /* one pending event at a time */
    sim_timer_activate_after (&cal_delay_unit, usec);   /* microseconds, sim_timer.h */
    sim_interval -= inst;                               /* consume documented simulated time */
    sim_debug (E10025_DBG_CALIB, &cpu_dev,
               "hist: op %06o %s %.2fus %dcy %dinst active %d\n",
               IR & DMASK, kind, usec, cycles, inst, (int) sim_is_active (&cal_delay_unit));
    }
else
    sim_debug (E10025_DBG_CALIB, &cpu_dev,
               "hist: op %06o %s %.2fus %dcy step\n",
               IR & DMASK, kind, usec, cycles);
}

t_stat e10025_cal_set_mode (const char *val)
{
if (val == NULL)
    return SCPE_ARG;
if (strcasecmp (val, "historical") == 0)
    cal_mode = E10025_MODE_HIST;
else if (strcasecmp (val, "ticks") == 0)
    cal_mode = E10025_MODE_TICKS;
else if (strcasecmp (val, "fast") == 0)
    cal_mode = E10025_MODE_FAST;
else
    return sim_messagef (SCPE_ARG, "unknown CPU mode %s, expected historical, ticks or fast\n", val);
sim_cancel (&cal_delay_unit);
cal_ticks = 0;
return SCPE_OK;
}

const char *e10025_cal_get_mode (void)
{
return (cal_mode == E10025_MODE_HIST)? "historical":
       ((cal_mode == E10025_MODE_TICKS)? "ticks": "fast");
}

t_stat e10025_cal_set_memdisp (const char *val)
{
if (val == NULL)
    return SCPE_ARG;
if ((strcasecmp (val, "on") == 0) || (strcasecmp (val, "true") == 0) ||
    (strcasecmp (val, "1") == 0))
    e10025_memdisp = 1;
else if ((strcasecmp (val, "off") == 0) || (strcasecmp (val, "false") == 0) ||
         (strcasecmp (val, "0") == 0))
    e10025_memdisp = 0;
else
    return sim_messagef (SCPE_ARG, "unknown MEMDISP value %s, expected on or off\n", val);
return SCPE_OK;
}

const char *e10025_cal_get_memdisp (void)
{
return e10025_memdisp? "on": "off";
}

t_stat e10025_cal_set_file (const char *fname)
{
if ((fname == NULL) || (*fname == 0))
    return SCPE_ARG;
return e10025_cal_load (fname);
}

const char *e10025_cal_get_file (void)
{
return cal_path[0]? cal_path: E10025_CALFILE_DEFAULT;
}

/* e10025_cpu.c: CPU simulator for Электроника-100-25
   Derived from the simh PDP-11 CPU structure (MIT license), with the opcode
   table checked against ИЧМ2.791.003 ТО1 ("Система команд"), PDF pages 11-131.
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
extern int32 calc_ints (int32 nipl, int32 trq);
extern int32 get_vector (int32 nipl);
extern int32 chan_read_io (int32 pa, int32 byte);
extern void chan_write_io (int32 pa, int32 data, int32 byte);
extern void br_arbitrate (void);
extern void dks_poll (void);
extern MTAB cpu_mod[];
int32 e10025_get_psw (void);
void e10025_put_psw (int32 val);
static int32 get_PSW (void);
static void put_PSW (int32 val);

uint16 *M = NULL;
int32 R[8] = { 0 };
int32 PSW = 0;
int32 saved_PC = 0;
int32 trap_req = 0;
int32 ipl = 0;
int32 int_req[IPL_HLVL] = { 0 };
int32 autcon_enb = 1;
int32 FPS = 0;
uint32 cpu_model = MOD_1140;

int32 N = 0, Z = 0, V = 0, C = 0, T = 0;
static int32 pm = 0, cm = 0;
static int32 wait_state = 0;
static t_stat stop_reason = SCPE_OK;

#define PC R[7]
#define SP R[6]
#define SIGN_W(v) (((v) >> 15) & 1)
#define SIGN_B(v) (((v) >> 7) & 1)

static const int32 trap_vec[TRAP_V_MAX] = {
    VEC_RED, VEC_ODD, VEC_NXM, VEC_MME, VEC_PAR, VEC_PRV,
    VEC_ILL, VEC_BPT, VEC_IOT, VEC_EMT, VEC_TRAP, VEC_TRC,
    VEC_YEL, VEC_PWRFL, VEC_FPE
    };

static const int32 trap_clear[TRAP_V_MAX] = {
    TRAP_RED, TRAP_ODD, TRAP_NXM, TRAP_MME, TRAP_PAR,
    TRAP_PRV, TRAP_ILL, TRAP_BPT, TRAP_IOT, TRAP_EMT,
    TRAP_TRAP, TRAP_TRC, TRAP_YEL, TRAP_PWRFL, TRAP_FPE
    };

/* Register display follows the channel addresses in book 4, chapter 8. */
REG cpu_reg[] = {
    { DRDATAD (PC, saved_PC, 16, "program counter") },
    { DRDATAD (PSW, PSW, 16, "processor status word") },
    { DRDATAD (R0, R[0], 16, "register 0") },
    { DRDATAD (R1, R[1], 16, "register 1") },
    { DRDATAD (R2, R[2], 16, "register 2") },
    { DRDATAD (R3, R[3], 16, "register 3") },
    { DRDATAD (R4, R[4], 16, "register 4") },
    { DRDATAD (R5, R[5], 16, "register 5") },
    { DRDATAD (SP, R[6], 16, "stack pointer") },
    { DRDATAD (TICKS, cal_ticks, 32, "opcode ticks or historical delay units") },
    { NULL }
    };

static DEBTAB cpu_debug[] = {
    { "CALIB", E10025_DBG_CALIB, "Opcode calibration tick lines" },
    { NULL, 0, NULL }
    };

/* Default 125K words (128000 decimal), the full 18-bit channel bank space.
   set CPU MEMSIZE=64K restores the old 64 KB compatibility configuration. */
UNIT cpu_unit = { UDATA (NULL, UNIT_FIX | UNIT_BINK, MEMSIZE125K) };

static t_stat cpu_reset (DEVICE *dptr);
static t_stat cpu_boot (int32 unitno, DEVICE *dptr);
t_stat cpu_ex (t_value *vptr, t_addr addr, UNIT *uptr, int32 sw);
t_stat cpu_dep (t_value val, t_addr addr, UNIT *uptr, int32 sw);

DEVICE cpu_dev = {
    "CPU", &cpu_unit, cpu_reg, cpu_mod,
    1, 10, 22, 2, 8, 16,
    &cpu_ex, &cpu_dep, &cpu_reset,
    &cpu_boot, NULL, NULL,
    NULL, DEV_DYNM | DEV_DEBUG, 0, cpu_debug,
    NULL, NULL, NULL, NULL, NULL
    };

/* Memory and channel access. The channel address is a byte address:
   АДР17..АДР16 select the RAM bank, АДР00 selects the byte. The linear
   word index therefore covers banks 0..3 when MEMSIZE is 124K. */
static int32 mem_index (int32 pa)
{
return (pa >> 1) & ((MEMSIZE >> 1) - 1);
}

static int32 mem_read_word (int32 pa)
{
return M[mem_index (pa)];
}

static void mem_write_word (int32 pa, int32 data)
{
M[mem_index (pa)] = data & DMASK;
}

static int32 mem_read_byte (int32 pa)
{
int32 w = M[mem_index (pa)];
return (pa & 1) ? ((w >> 8) & 0377) : (w & 0377);
}

static void mem_write_byte (int32 pa, int32 data)
{
int32 w = M[mem_index (pa)];
if (pa & 1)
    M[mem_index (pa)] = (w & 0377) | ((data & 0377) << 8);
else
    M[mem_index (pa)] = (w & 0177400) | (data & 0377);
}

/* Guest register addresses 0160000-0177777 live in the top of the 16-bit
   address space and select the 22-bit physical I/O page. */
static uint32 guest_io_pa (int32 pa)
{
return (uint32) IOPAGEBASE | (((uint32) pa) & IOPAGEMASK);
}

int32 ReadW (int32 pa)
{
pa &= VAMASK;
if (pa >= IO_VABASE)
    return chan_read_io (guest_io_pa (pa), 0) & DMASK;
if ((uint32) pa >= MEMSIZE)
    return 0;
return mem_read_word (pa);
}

void WriteW (int32 pa, int32 data)
{
pa &= VAMASK;
if (pa >= IO_VABASE) {
    chan_write_io (guest_io_pa (pa), data, 0);
    return;
    }
if ((uint32) pa < MEMSIZE)
    mem_write_word (pa, data);
}

static int32 ReadB (int32 pa)
{
pa &= VAMASK;
if (pa >= IO_VABASE)
    return chan_read_io (guest_io_pa (pa), 1) & 0377;
if ((uint32) pa >= MEMSIZE)
    return 0;
return mem_read_byte (pa);
}

static void WriteB (int32 pa, int32 data)
{
pa &= VAMASK;
if (pa >= IO_VABASE) {
    chan_write_io (guest_io_pa (pa), data, 1);
    return;
    }
if ((uint32) pa < MEMSIZE)
    mem_write_byte (pa, data);
}

/* PSW assembly. Bit positions: N3 Z2 V1 C0 T4, IPL 7-5, PM 13-12, CM 15-14. */
static int32 get_PSW (void)
{
return (C & 1) | ((V & 1) << 1) | ((Z & 1) << 2) | ((N & 1) << 3) |
       ((T & 1) << 4) | ((ipl & 7) << 5) | ((pm & 3) << 12) | ((cm & 3) << 14);
}

int32 e10025_get_psw (void)
{
return get_PSW ();
}

void e10025_put_psw (int32 val)
{
put_PSW (val);
}

static void put_PSW (int32 val)
{
PSW = val & 0177777;
C = PSW & 1;
V = (PSW >> 1) & 1;
Z = (PSW >> 2) & 1;
N = (PSW >> 3) & 1;
T = (PSW >> 4) & 1;
ipl = (PSW >> 5) & 7;
pm = (PSW >> 12) & 3;
cm = (PSW >> 14) & 3;
br_arbitrate ();                                        /* BR grants follow the new ipl */
}

/* Effective address for modes 1..7. Mode 0 is handled by get_src/set_dst. */
static int32 Getea (int32 spec, int32 byte)
{
int32 mode = (spec >> 3) & 7;
int32 reg = spec & 7;
int32 incr = (byte && (reg < 6)) ? 1 : 2;
int32 ea, x;

switch (mode) {
case 1:                                                 /* (Rn) */
    return R[reg];
case 2:                                                 /* (Rn)+, #n for PC */
    ea = R[reg];
    R[reg] = (R[reg] + incr) & 0177777;
    return ea;
case 3:                                                 /* @(Rn)+, @#A for PC */
    ea = R[reg];
    R[reg] = (R[reg] + 2) & 0177777;
    return ReadW (ea);
case 4:                                                 /* -(Rn) */
    R[reg] = (R[reg] - incr) & 0177777;
    return R[reg];
case 5:                                                 /* @-(Rn) */
    R[reg] = (R[reg] - 2) & 0177777;
    return ReadW (R[reg]);
case 6:                                                 /* X(Rn), A for PC */
    x = ReadW (PC);
    PC = (PC + 2) & 0177777;
    return (x + R[reg]) & 0177777;
case 7:                                                 /* @X(Rn), @A for PC */
    x = ReadW (PC);
    PC = (PC + 2) & 0177777;
    return ReadW ((x + R[reg]) & 0177777);
    }
return 0;
}

static int32 get_operand (int32 spec, int32 byte, int32 *ea)
{
int32 reg = spec & 7;
*ea = -1;
if ((spec & 070) == 0)
    return byte ? (R[reg] & 0377) : R[reg];
*ea = Getea (spec, byte);
return byte ? ReadB (*ea) : ReadW (*ea);
}

static void set_operand (int32 spec, int32 byte, int32 val, int32 ea)
{
int32 reg = spec & 7;
if ((spec & 070) == 0) {
    if (byte)
        R[reg] = (R[reg] & 0177400) | (val & 0377);
    else
        R[reg] = val & 0177777;
    return;
    }
if (byte)
    WriteB (ea, val);
else
    WriteW (ea, val);
}

static void put_result_flags (int32 res, int32 byte)
{
if (byte) {
    N = SIGN_B (res);
    Z = ((res & 0377) == 0);
    }
else {
    N = SIGN_W (res);
    Z = ((res & 0177777) == 0);
    }
}

static void add_flags (int32 a, int32 b, int32 res, int32 byte, int32 *cv)
{
int32 mask = byte ? 0377 : 0177777;
int32 sign = byte ? 0200 : 0100000;                      /* byte bit 7, word bit 15 */
*cv = ((~a ^ b) & (a ^ res) & sign) ? 1 : 0;
cv[1] = ((res & mask) < (a & mask)) ? 1 : 0;
}

static void sub_flags (int32 a, int32 b, int32 res, int32 byte, int32 *cv)
{
int32 mask = byte ? 0377 : 0177777;
add_flags (a, ((~b) + 1) & mask, res, byte, cv);
cv[1] = ((a & mask) < (b & mask)) ? 1 : 0;
}

static void set_trap (int32 t)
{
trap_req |= t;
}

static void push (int32 val)
{
SP = (SP - 2) & 0177777;
WriteW (SP, val);
}

static int32 pop (void)
{
int32 val = ReadW (SP);
SP = (SP + 2) & 0177777;
return val;
}

/* Process pending traps and interrupts before the next instruction. */
static void process_traps (void)
{
int32 vec = 0, i, t;

if (trap_req & TRAP_ALL) {
    t = trap_req & TRAP_ALL;
    for (i = 0; i < TRAP_V_MAX; i++) {
        if ((t >> i) & 1) {
            vec = trap_vec[i];
            trap_req &= ~trap_clear[i];
            break;
            }
        }
    }
else if (trap_req & TRAP_INT) {
    vec = get_vector (ipl);                             /* get_vector already calls ack */
    trap_req = calc_ints (ipl, 0);
    }
if (vec == 0)
    return;
push (get_PSW ());
push (PC);
saved_PC = ReadW (vec) & 0177777;
put_PSW (ReadW ((vec + 2) & 0177777));
PC = saved_PC;
wait_state = 0;
}

/* SIMH scans console addresses as hex by default. Book 4 writes them as
   octal, while the existing scripts use short decimal addresses. Long tokens
   made only of octal digits are therefore read as octal; everything else
   keeps the device default radix. */
static t_addr cpu_parse_addr (DEVICE *dptr, CONST char *cptr, CONST char **tptr)
{
CONST char *p;
size_t n = 0;

if ((dptr == NULL) || (cptr == NULL))
    return (t_addr) strtotv (cptr, tptr, 10);
if (dptr != &cpu_dev)
    return (t_addr) strtotv (cptr, tptr, dptr->aradix);
if ((cptr[0] == '0') && ((cptr[1] == 'x') || (cptr[1] == 'X')))
    return (t_addr) strtotv (cptr, tptr, 16);
for (p = cptr; (*p >= '0') && (*p <= '7'); p++)
    n++;
if ((n >= 6) && ((*p == '\0') || (*p == '-') || (*p == ':') || (*p == ',')))
    return (t_addr) strtotv (cptr, tptr, 8);
return (t_addr) strtotv (cptr, tptr, 10);
}

/* The console accepts the 22-bit physical form, the 16-bit guest form and
   the 6-digit book form. All three select the same physical I/O page. */
static uint32 console_pa (t_addr addr)
{
uint32 pa = (uint32) addr;
if (pa >= IOPAGEBASE)
    return pa;
if (pa >= IO_VABASE)
    return guest_io_pa ((int32) pa);
return pa;
}

/* The console understands three address forms. 0..0157777 is the guest RAM
   window, 0160000..0177777 is the Phase 2 guest register window, and larger
   addresses up to MEMSIZE are the 18-bit RAM banks of the 125K default.
   All registered I/O page addresses are above MEMSIZE, so they stay in the
   I/O page; the bottom of the page has no registered device. */
t_stat cpu_ex (t_value *vptr, t_addr addr, UNIT *uptr, int32 sw)
{
int32 iodata;
t_stat stat;
uint32 a = (uint32) addr;

if (vptr == NULL)
    return SCPE_ARG;
if (a < IO_VABASE) {
    if (a >= MEMSIZE)
        return SCPE_NXM;
    *vptr = ReadW ((int32) a) & DMASK;
    return SCPE_OK;
    }
if (a <= VAMASK) {
    stat = iopageR (&iodata, guest_io_pa ((int32) a), READC);
    *vptr = iodata;
    return stat;
    }
if (a < MEMSIZE) {
    *vptr = mem_read_word ((int32) a) & DMASK;
    return SCPE_OK;
    }
stat = iopageR (&iodata, console_pa (addr), READC);
*vptr = iodata;
return stat;
}

t_stat cpu_dep (t_value val, t_addr addr, UNIT *uptr, int32 sw)
{
uint32 a = (uint32) addr;

if (a < IO_VABASE) {
    if (a >= MEMSIZE)
        return SCPE_NXM;
    WriteW ((int32) a, val);
    return SCPE_OK;
    }
if (a <= VAMASK)
    return iopageW (val & DMASK, guest_io_pa ((int32) a), WRITEC);
if (a < MEMSIZE) {
    mem_write_word ((int32) a, val);
    return SCPE_OK;
    }
return iopageW (val & DMASK, console_pa (addr), WRITEC);
}

static t_stat cpu_reset (DEVICE *dptr)
{
sim_vm_parse_addr = cpu_parse_addr;
if (M == NULL) {
    M = (uint16 *) calloc (MEMALLOC_WORDS, sizeof (uint16));
    if (M == NULL)
        return SCPE_MEM;
    auto_config (NULL, 0);                              /* assign device addresses */
    }
memset (R, 0, sizeof (R));
N = Z = V = C = T = 0;
pm = cm = ipl = 0;
wait_state = 0;
trap_req = 0;
stop_reason = SCPE_OK;
put_PSW (0340);
saved_PC = 0;
PC = 0;
e10025_cal_load (NULL);                                 /* reload file at reset and startup */
return build_dib_tab ();                                /* console registers before first GO */
}

static t_stat cpu_boot (int32 unitno, DEVICE *dptr)
{
saved_PC = 0;
put_PSW (0340);
PC = 0;
return SCPE_OK;
}

/* Instruction execution. Returns non-zero when the instruction itself stops. */
static t_stat execute (int32 IR)
{
int32 opcode = (IR >> 12) & 017;
/* SUB's opcode already includes bit 15; it is a word-only instruction. */
int32 byte = ((IR & 0100000) && (opcode != 016)) ? 1 : 0;
int32 srcspec = (IR >> 6) & 077;
int32 dstspec = IR & 077;
int32 src, dst, ea, src_ea, res, cv[2], tmp;

if ((IR & 0177000) == 075000)                           /* FADD/FSUB/FMUL/FDIV */
    return e10025_fop ((IR >> 6) & 3, IR & 07);              /* 075x0R: op in bits 8..6 */

/* Byte forms of MOV/CMP/BIT/BIC/BIS occupy opcodes 011..015. */
if ((opcode >= 01 && opcode <= 06) || (opcode >= 011 && opcode <= 015)) {
    int32 op = (opcode >= 011)? (opcode & 07): opcode;
    src = get_operand (srcspec, byte, &src_ea);
    dst = get_operand (dstspec, byte, &ea);
    switch (op) {
    case 01:                                            /* MOV */
        res = src;
        set_operand (dstspec, byte, res, ea);
        put_result_flags (res, byte);
        V = 0;
        break;
    case 02:                                            /* CMP: src - dst */
        res = (src - dst) & (byte ? 0377 : 0177777);
        sub_flags (src, dst, res, byte, cv);
        V = cv[0];
        C = cv[1];
        put_result_flags (res, byte);
        break;
    case 03:                                            /* BIT */
        res = src & dst;
        put_result_flags (res, byte);
        V = 0;
        break;
    case 04:                                            /* BIC */
        res = dst & (~src) & (byte ? 0377 : 0177777);
        set_operand (dstspec, byte, res, ea);
        put_result_flags (res, byte);
        V = 0;
        break;
    case 05:                                            /* BIS */
        res = dst | src;
        set_operand (dstspec, byte, res, ea);
        put_result_flags (res, byte);
        V = 0;
        break;
    case 06:                                            /* ADD */
        res = (src + dst) & (byte ? 0377 : 0177777);
        add_flags (src, dst, res, byte, cv);
        V = cv[0];
        C = cv[1];
        set_operand (dstspec, byte, res, ea);
        put_result_flags (res, byte);
        break;
        }
    return SCPE_OK;
    }

if (opcode == 016) {                                    /* SUB: dst - src */
    src = get_operand (srcspec, byte, &src_ea);
    dst = get_operand (dstspec, byte, &ea);
    res = (dst - src) & (byte ? 0377 : 0177777);
    sub_flags (dst, src, res, byte, cv);
    V = cv[0];
    C = cv[1];
    set_operand (dstspec, byte, res, ea);
    put_result_flags (res, byte);
    return SCPE_OK;
    }

if (opcode == 0) {
    switch ((IR >> 6) & 077) {
    case 000:                                           /* HALT, WAIT, RTI, ... */
        switch (IR & 07) {
        case 0:                                         /* HALT */
            return SCPE_STOP;
        case 1:                                         /* WAIT */
            wait_state = 1;
            break;
        case 2:                                         /* RTI */
        case 6:                                         /* RTT */
            PC = pop ();
            put_PSW (pop ());
            if ((IR & 07) == 6)
                T = 0;
            break;
        case 3:                                         /* BRT */
            set_trap (TRAP_BPT);
            break;
        case 4:                                         /* IOT */
            set_trap (TRAP_IOT);
            break;
        case 5:                                         /* RESET */
            for (tmp = 0; tmp < IPL_HLVL; tmp++)
                int_req[tmp] = 0;
            trap_req &= ~TRAP_INT;
            break;
            }
        break;
    case 001:                                           /* JMP */
        if ((dstspec & 070) == 0) {
            set_trap (TRAP_ILL);
            break;
            }
        PC = Getea (dstspec, 0) & 0177777;
        break;
    case 002:                                           /* RTS or CC changes */
        if (IR >= 000240) {
            if (IR < 000260) {                          /* clear selected flags */
                if (IR & 010) N = 0;
                if (IR & 004) Z = 0;
                if (IR & 002) V = 0;
                if (IR & 001) C = 0;
                }
            else {                                      /* set selected flags */
                if (IR & 010) N = 1;
                if (IR & 004) Z = 1;
                if (IR & 002) V = 1;
                if (IR & 001) C = 1;
                }
            break;
            }
        tmp = dstspec & 07;                             /* RTS */
        PC = R[tmp];
        R[tmp] = pop ();
        break;
    case 003:                                           /* SWAB */
        dst = get_operand (dstspec, 0, &ea);
        tmp = ((dst & 0377) << 8) | ((dst >> 8) & 0377);
        set_operand (dstspec, 0, tmp, ea);
        N = (tmp >> 7) & 1;
        Z = ((tmp & 0377) == 0);
        V = C = 0;
        break;
    case 040:                                           /* JSR */
        tmp = dstspec & 07;
        dst = get_operand (dstspec, 0, &ea);
        push (R[tmp]);
        R[tmp] = PC;
        PC = dst & 0177777;
        break;
    case 011:                                           /* SOB */
        tmp = (srcspec >> 3) & 7;
        R[tmp] = (R[tmp] - 1) & 0177777;
        if (R[tmp] != 0)
            PC = (PC - 2 * (srcspec & 077)) & 0177777;
        break;
    case 025:                                           /* CLC/CLV/.../NOP */
        switch (IR & 0377) {
        case 0240:                                      /* NOP */
            break;
        case 0241: C = 0; break;
        case 0242: V = 0; break;
        case 0244: Z = 0; break;
        case 0250: N = 0; break;
        case 0261: C = 1; break;
        case 0262: V = 1; break;
        case 0264: Z = 1; break;
        case 0270: N = 1; break;
        case 0257: N = Z = V = C = 0; break;
        case 0277: N = Z = V = C = 1; break;
        case 0243: V = C = 0; break;
            }
        break;
    case 050: case 051: case 052: case 053: case 054:
    case 055: case 056: case 057: case 060: case 061:
    case 062: case 063: case 067:                       /* one-address word/byte */
        {
        int32 op = (IR >> 6) & 0177;
        int32 isbyte = byte;
        int32 mask = isbyte ? 0377 : 0177777;
        dst = get_operand (dstspec, isbyte, &ea);
        switch (op) {
        case 050:                                       /* CLR */
            res = 0;
            N = V = C = 0;
            Z = 1;
            break;
        case 051:                                       /* COM */
            res = (~dst) & mask;
            N = SIGN_W (res);
            Z = (res == 0);
            V = 0;
            C = 1;
            break;
        case 052:                                       /* INC */
            res = (dst + 1) & mask;
            put_result_flags (res, isbyte);
            V = (res == (isbyte ? 0200 : 0100000));
            break;
        case 053:                                       /* DEC */
            res = (dst - 1) & mask;
            put_result_flags (res, isbyte);
            V = (res == (isbyte ? 0177 : 077777));
            break;
        case 054:                                       /* NEG */
            res = (-dst) & mask;
            put_result_flags (res, isbyte);
            V = (res == (isbyte ? 0200 : 0100000));
            C = (res != 0);
            break;
        case 055:                                       /* ADC */
            res = (dst + C) & mask;
            add_flags (dst, C, res, isbyte, cv);
            V = cv[0];
            C = cv[1];
            put_result_flags (res, isbyte);
            break;
        case 056:                                       /* SBC */
            res = (dst - C) & mask;
            sub_flags (dst, C, res, isbyte, cv);
            V = cv[0];
            C = cv[1];
            put_result_flags (res, isbyte);
            break;
        case 057:                                       /* TST */
            put_result_flags (dst, isbyte);
            V = C = 0;
            return SCPE_OK;
        case 060:                                       /* ROR */
            tmp = C;
            C = dst & 1;
            res = ((dst >> 1) | (tmp << (isbyte ? 7 : 15))) & mask;
            put_result_flags (res, isbyte);
            V = C ^ N;
            break;
        case 061:                                       /* ROL */
            tmp = (dst >> (isbyte ? 7 : 15)) & 1;
            res = ((dst << 1) | C) & mask;
            C = tmp;
            put_result_flags (res, isbyte);
            V = C ^ N;
            break;
        case 062:                                       /* ASR */
            C = dst & 1;
            res = ((dst >> 1) | (dst & (isbyte ? 0200 : 0100000))) & mask;
            put_result_flags (res, isbyte);
            V = 0;
            break;
        case 063:                                       /* ASL */
            C = (dst >> (isbyte ? 7 : 15)) & 1;
            res = (dst << 1) & mask;
            put_result_flags (res, isbyte);
            V = C ^ N;
            break;
        case 067:                                       /* SXT */
            res = N ? mask : 0;
            Z = !N;
            V = 0;
            break;
        default:
            set_trap (TRAP_ILL);
            return SCPE_OK;
            }
        set_operand (dstspec, isbyte, res, ea);
        }
        break;
    default:                                            /* branches */
        {
        int32 bop = (IR >> 8) & 0377;
        int32 off = IR & 0377;
        int32 cond = 0;
        if (off & 0200)
            off |= ~0377;
        switch (bop) {
        case 001: cond = 1; break;                      /* BR */
        case 002: cond = !Z; break;                     /* BNE */
        case 003: cond = Z; break;                      /* BEQ */
        case 004: cond = !(N ^ V); break;               /* BGE */
        case 005: cond = (N ^ V); break;                /* BLT */
        case 006: cond = !Z && !(N ^ V); break;         /* BGT */
        case 007: cond = Z || (N ^ V); break;           /* BLE */
        case 0200: cond = !N; break;                    /* BPL */
        case 0201: cond = N; break;                     /* BMI */
        case 0202: cond = !C && !Z; break;              /* BHI */
        case 0203: cond = C || Z; break;                /* BLOS */
        case 0204: cond = !V; break;                    /* BVC */
        case 0205: cond = V; break;                     /* BVS */
        case 0206: cond = !C; break;                    /* BHIS */
        case 0207: cond = C; break;                     /* BLO */
        default:
            set_trap (TRAP_ILL);
            break;
            }
        if (cond)
            PC = (PC + 2 * off) & 0177777;
        }
        break;
        }
    return SCPE_OK;
    }

/* EMT and TRAP encode an 8-bit argument in the low byte. */
if ((IR & 0177400) == 0104000) {
    set_trap (TRAP_EMT);
    return SCPE_OK;
    }
if ((IR & 0177400) == 0104400) {
    set_trap (TRAP_TRAP);
    return SCPE_OK;
    }

/* Extended instruction set: MUL, DIV, ASH, ASHC, XOR. */
if (opcode == 07) {
    int32 func = (IR >> 9) & 7;
    int32 reg = (IR >> 6) & 7;
    src = get_operand (dstspec, 0, &ea);
    switch (func) {
    case 0:                                             /* MUL */
        {
        long long a = R[reg];
        long long b = src;
        long long prod;
        if (a & 0100000)
            a |= ~0177777;
        if (b & 0100000)
            b |= ~0177777;
        prod = a * b;
        R[reg] = (prod >> 16) & 0177777;
        R[(reg + 1) & 7] = prod & 0177777;
        N = (prod < 0);
        Z = (prod == 0);
        V = 0;
        C = (prod > 077777) || (prod < -0100000);
        }
        break;
    case 1:                                             /* DIV */
        {
        long long dividend = ((long long) R[reg] << 16) | R[(reg + 1) & 7];
        long long divisor = src;
        long long quot;
        if (divisor & 0100000)
            divisor |= ~0177777;
        if (divisor == 0) {
            V = C = 1;
            break;
            }
        quot = dividend / divisor;
        if ((quot > 077777) || (quot < -0100000)) {
            V = 1;
            break;
            }
        R[reg] = (quot >> 16) & 0177777;
        R[(reg + 1) & 7] = quot & 0177777;
        N = (quot < 0);
        Z = (quot == 0);
        V = C = 0;
        }
        break;
    case 2:                                             /* ASH */
        tmp = src & 077;
        if (tmp & 040)
            tmp |= ~077;
        if (tmp >= 0)
            res = (R[reg] << tmp) & 0177777;
        else
            res = R[reg] >> (-tmp);
        C = (tmp > 0) ? ((R[reg] >> (16 - tmp)) & 1) : 0;
        put_result_flags (res, 0);
        V = (tmp > 0) && (C != N);
        R[reg] = res;
        break;
    case 3:                                             /* ASHC */
        set_trap (TRAP_ILL);
        break;
    case 4:                                             /* XOR */
        dst = get_operand (dstspec, 0, &ea);
        res = dst ^ R[reg];
        set_operand (dstspec, 0, res, ea);
        put_result_flags (res, 0);
        V = 0;
        break;
    case 7:                                             /* SOB */
        R[reg] = (R[reg] - 1) & 0177777;
        if (R[reg] != 0)
            PC = (PC - 2 * (IR & 077)) & 0177777;
        break;
    default:
        set_trap (TRAP_ILL);
        break;
        }
    return SCPE_OK;
    }

set_trap (TRAP_ILL);
return SCPE_OK;
}

t_stat sim_instr (void)
{
int32 IR;

stop_reason = SCPE_OK;
if (build_dib_tab () != SCPE_OK)
    return SCPE_STOP;
br_arbitrate ();                                        /* rebuild cleared int_vec/int_ack */
while (stop_reason == SCPE_OK) {
    if (chan_timing_mode () == CH_TIMING_SCALED) {
        if (sim_interval <= 0) {                        /* scaled: service the event queue */
            stop_reason = sim_process_event ();
            continue;
            }
        }
    else {
        sim_interval = 1000;                            // host events are not used
        if (sim_interval <= 0) {
            sim_messagef (SCPE_OK, "DEBUG interval branch interval=%d\n", (int) sim_interval);
            stop_reason = sim_process_event ();
            continue;
            }
        }
    trap_req = calc_ints (ipl, trap_req);
    dks_poll ();
    if (trap_req)
        process_traps ();
    if (T)
        set_trap (TRAP_TRC);
    if (wait_state) {
        sim_idle (TMR_CLK, TRUE);
        if (trap_req == 0)
            continue;
        wait_state = 0;
        }
    IR = ReadW (PC);
    PC = (PC + 2) & 0177777;
    sim_interval--;
    /* ticks mode applies the 1998 opcode deltas as simulator ticks; historical
       mode schedules book-3 microseconds on the SIMH clock and consumes the
       documented simulated time; fast mode does neither. */
    e10025_cal_apply (IR);
    stop_reason = execute (IR);
    }
saved_PC = PC;
PSW = get_PSW ();
return stop_reason;
}

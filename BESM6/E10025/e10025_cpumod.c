/* e10025_cpumod.c: minimal CPU support and SYSTEM device for Электроника-100-25
   Borrowed structure from simh PDP-11 (pdp11_cpumod.c), MIT license.
   Only the parts required by the bus layer are implemented here.

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

extern int32 R[8];
extern int32 e10025_get_psw (void);
extern void e10025_put_psw (int32 val);

int32 uba_last = 0;
int32 stklim = 0;
int32 ub_map[UBM_LNT_LW] = { 0 };
t_bool cpu_bme = FALSE;
uint32 cpu_opt = BUS_U;                                /* UNIBUS arbitration */
uint32 cpu_type = 0;

static UNIT sys_unit = { UDATA (NULL, 0, 0) };
static REG sys_reg[] = { { NULL } };
static MTAB sys_mod[] = { { 0 } };

static t_stat sys_reset (DEVICE *dptr)
{
return SCPE_OK;
}

DEVICE sys_dev = {
    "SYSTEM", &sys_unit, sys_reg, sys_mod,
    1, 0, 0, 0, 0, 0,
    NULL, NULL, &sys_reset,
    NULL, NULL, NULL,
    NULL, 0, 0, NULL,
    NULL, NULL, NULL, NULL, NULL
    };

/* Speed modes and the opcode correction file from the 1998 article.
   Corrections are simulator ticks on instruction execution only. */
static t_stat cpu_set_mode (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
return e10025_cal_set_mode (cptr);
}

static t_stat cpu_show_mode (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf (st, "%s", e10025_cal_get_mode ());
return SCPE_OK;
}

static t_stat cpu_set_calfile (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
return e10025_cal_set_file (cptr);
}

static t_stat cpu_show_calfile (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
fprintf (st, "%s", e10025_cal_get_file ());
return SCPE_OK;
}

/* The machine starts with 125K words (128000 decimal), the largest config
   that fits the 128K-word channel bank space. 64K and 124K remain explicit
   compatibility options. The backing array is always 128K words so that
   АДР17..АДР16 can select any bank without out-of-range indexing. */
static t_stat cpu_set_memsize (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
uint32 newcap, copy, i;
uint16 *nM;

if ((uptr == NULL) || (cptr == NULL))
    return SCPE_ARG;
if (strcasecmp (cptr, "64K") == 0)
    newcap = MEMSIZE64K;
else if (strcasecmp (cptr, "124K") == 0)
    newcap = MEMSIZE124K;
else if (strcasecmp (cptr, "125K") == 0)
    newcap = MEMSIZE125K;
else
    return sim_messagef (SCPE_ARG, "CPU MEMSIZE expects 64K, 124K or 125K\n");
nM = (uint16 *) calloc (MEMALLOC_WORDS, sizeof (uint16));
if (nM == NULL)
    return SCPE_MEM;
if (M != NULL) {
    copy = (((uint32) uptr->capac) < newcap)? (uint32) uptr->capac: newcap;
    for (i = 0; i < copy; i = i + 2)
        nM[i >> 1] = M[i >> 1];
    free (M);
    }
M = nM;
uptr->capac = newcap;
return SCPE_OK;
}

static t_stat cpu_show_memsize (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
uint32 cap = (uint32) uptr->capac;
CONST char *name = (cap == MEMSIZE125K)? "125K words":
                   ((cap == MEMSIZE124K)? "124K words": "64K words");
fprintf (st, "MEMSIZE=%s", name);
return SCPE_OK;
}

MTAB cpu_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "MEMSIZE", "MEMSIZE={64K|124K|125K}",
      &cpu_set_memsize, &cpu_show_memsize, NULL, "Set RAM size (book 4, p.38)" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "MODE", "MODE={historical|fast}",
      &cpu_set_mode, &cpu_show_mode, NULL, "Set CPU speed mode" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR|MTAB_NC, 0, "CALFILE", "CALFILE=path",
      &cpu_set_calfile, &cpu_show_calfile, NULL, "Load opcode calibration file" },
    { 0 }
    };

/* CPU registers in the channel address space (book 4, figure 11, p.40).
   Book addresses 777700..777707 and 777774..777776 are mapped into simh's
   physical I/O page by adding IOPAGEBASE to the low 14 bits. */
static t_stat cpu_regs_rd (int32 *data, int32 pa, int32 access)
{
int32 idx = ((pa & IOPAGEMASK) - 017700) / 2;
if ((idx >= 0) && (idx < 8)) {
    *data = R[idx] & DMASK;
    return SCPE_OK;
    }
return SCPE_NXM;
}

static t_stat cpu_regs_wr (int32 data, int32 pa, int32 access)
{
int32 idx = ((pa & IOPAGEMASK) - 017700) / 2;
if ((idx >= 0) && (idx < 8)) {
    if (access == WRITEB) {
        int32 cur = R[idx] & DMASK;
        R[idx] = (pa & 1)? ((cur & 0377) | ((data & 0377) << 8)):
                           ((cur & 0177400) | (data & 0377));
        }
    else
        R[idx] = data & DMASK;
    return SCPE_OK;
    }
return SCPE_NXM;
}

static t_stat cpu_stk_rd (int32 *data, int32 pa, int32 access)
{
*data = stklim & DMASK;
return SCPE_OK;
}

static t_stat cpu_stk_wr (int32 data, int32 pa, int32 access)
{
if (access == WRITEB)
    stklim = (pa & 1)? ((stklim & 0377) | ((data & 0377) << 8)):
                       ((stklim & 0177400) | (data & 0377));
else
    stklim = data & DMASK;
return SCPE_OK;
}

static t_stat cpu_psw_rd (int32 *data, int32 pa, int32 access)
{
*data = e10025_get_psw () & DMASK;
return SCPE_OK;
}

static t_stat cpu_psw_wr (int32 data, int32 pa, int32 access)
{
if (access == WRITEB) {
    int32 cur = e10025_get_psw () & DMASK;
    data = (pa & 1)? ((cur & 0377) | ((data & 0377) << 8)):
                     ((cur & 0177400) | (data & 0377));
    }
e10025_put_psw (data);
return SCPE_OK;
}

static DIB cpu_regs_dib = {
    IOPAGEBASE + 017700, 020, &cpu_regs_rd, &cpu_regs_wr,
    0, 0, 0, { NULL }, 0, 0, NULL, NULL
    };

static DIB cpu_stk_dib = {
    IOPAGEBASE + 017774, 2, &cpu_stk_rd, &cpu_stk_wr,
    0, 0, 0, { NULL }, 0, 0, NULL, NULL
    };

static DIB cpu_psw_dib = {
    IOPAGEBASE + 017776, 2, &cpu_psw_rd, &cpu_psw_wr,
    0, 0, 0, { NULL }, 0, 0, NULL, NULL
    };

/* Book 4, figure 11: these addresses are named but their word formats are
   [GAP]. The model registers them with pass-through storage only; it invents
   no status flags, interrupts or translation behavior. Address 777546 is the
   timer and already owned by the existing CLK device at 017546; address
   777566 lies in the existing TTO register block 017564..017567. Neither is
   duplicated here. */
#define E10025_GAP_NREG 12
static const uint32 gap_ba[E10025_GAP_NREG] = {
    IOPAGEBASE + 017656,                                /* 777656 ГРАС */
    IOPAGEBASE + 017640,                                /* 777640 */
    IOPAGEBASE + 017616,                                /* 777616 БРИС */
    IOPAGEBASE + 017600,                                /* 777600 */
    IOPAGEBASE + 017576,                                /* 777576 */
    IOPAGEBASE + 017572,                                /* 777572 */
    IOPAGEBASE + 017570,                                /* 777570 пульт управления */
    IOPAGEBASE + 017550,                                /* 777550 */
    IOPAGEBASE + 012356,                                /* 772356 БРАС */
    IOPAGEBASE + 012340,                                /* 772340 */
    IOPAGEBASE + 012316,                                /* 772316 БРИС */
    IOPAGEBASE + 012300                                 /* 772300 */
    };
static int32 gap_store[E10025_GAP_NREG];

static int32 gap_index (int32 pa)
{
uint32 a = ((uint32) pa) & PAMASK;
int32 i;

for (i = 0; i < E10025_GAP_NREG; i++)
    if ((a & ~1u) == (gap_ba[i] & ~1u))
        return i;
return -1;
}

static t_stat gap_rd (int32 *data, int32 pa, int32 access)
{
int32 i = gap_index (pa);
if (i < 0)
    return SCPE_NXM;
*data = gap_store[i] & DMASK;
return SCPE_OK;
}

static t_stat gap_wr (int32 data, int32 pa, int32 access)
{
int32 i = gap_index (pa);
if (i < 0)
    return SCPE_NXM;
if (access == WRITEB) {
    int32 cur = gap_store[i] & DMASK;
    gap_store[i] = (pa & 1)? ((cur & 0377) | ((data & 0377) << 8)):
                            ((cur & 0177400) | (data & 0377));
    }
else
    gap_store[i] = data & DMASK;
return SCPE_OK;
}

static DIB gap_dib[E10025_GAP_NREG] = {
    { IOPAGEBASE + 017656, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 017640, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 017616, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 017600, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 017576, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 017572, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 017570, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 017550, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 012356, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 012340, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 012316, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL },
    { IOPAGEBASE + 012300, 2, &gap_rd, &gap_wr, 0, 0, 0, { NULL }, 0, 0, NULL, NULL }
    };

t_stat cpu_build_dib (void)
{
t_stat r;
int32 i;

cpu_regs_dib.next = &cpu_stk_dib;
cpu_stk_dib.next = &cpu_psw_dib;
cpu_psw_dib.next = &gap_dib[0];
for (i = 0; i < (E10025_GAP_NREG - 1); i++)
    gap_dib[i].next = &gap_dib[i + 1];
gap_dib[E10025_GAP_NREG - 1].next = NULL;
if ((r = build_ubus_tab (&cpu_dev, &cpu_regs_dib)))
    return r;
if ((r = build_ubus_tab (&cpu_dev, &cpu_stk_dib)))
    return r;
if ((r = build_ubus_tab (&cpu_dev, &cpu_psw_dib)))
    return r;
for (i = 0; i < E10025_GAP_NREG; i++)
    if ((r = build_ubus_tab (&cpu_dev, &gap_dib[i])))
        return r;
return SCPE_OK;
}

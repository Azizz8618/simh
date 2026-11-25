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

MTAB cpu_mod[] = {
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

t_stat cpu_build_dib (void)
{
t_stat r;
cpu_regs_dib.next = &cpu_stk_dib;
cpu_stk_dib.next = &cpu_psw_dib;
if ((r = build_ubus_tab (&cpu_dev, &cpu_regs_dib)))
    return r;
if ((r = build_ubus_tab (&cpu_dev, &cpu_stk_dib)))
    return r;
return build_ubus_tab (&cpu_dev, &cpu_psw_dib);
}

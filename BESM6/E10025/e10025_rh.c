/* e10025_rh.c: PDP-11 Massbus adapter simulator, ported to Electronica-100-25

   Copyright (c) 2005-2022, Robert M Supnik

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
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   Adapted from PDP11/pdp11_rh.c (RHA/RH11 Massbus adapter).  This file keeps
   the proven RH11 register, DMA and interrupt logic, but provides exactly one
   adapter named RH (logical name RHA) and does not depend on the removed
   Massbus branch of pdp11_io_lib.c.  The RH DIB is a normal channel DIB, so
   build_dib_tab() in e10025_io.c registers it without any shared-file edit.
   The device name RH makes `set RH enabled` unambiguous; the logical name
   RHA lets the existing auto_tab entry supply CSR 016700 and vector 0254.

   [GAP] Identity: the 1998 article names a 20 MB DP disk controller.  The
   RP04/05 geometry used here is a simh prototype only.  Exact identification
   of the historical DP controller with RP04/05 is not proven [G], because the
   historical DP register map is absent from the available sources.

   25-Jul-22    RMS     Removed OPT_RH11, changed adapter type test
   02-Sep-13    RMS     Added third Massbus adapter, debug printouts
   19-Mar-12    RMS     Fixed declaration of cpu_opt (Mark Pizzolato)
   02-Feb-08    RMS     Fixed DMA memory address limit test (John Dundas)
   17-May-07    RMS     Moved CS1 drive enable to devices
   21-Nov-05    RMS     Added enable/disable routine
   07-Jul-05    RMS     Removed extraneous externs

   WARNING: The interrupt logic of the RH11 is unusual and must be simulated
   with great precision.  The RH11 has an internal interrupt request flop,
   CSTB INTR, which is controlled as follows:

   - Writing IE and DONE simultaneously sets CSTB INTR
   - Controller clear, INIT, and interrupt acknowledge clear CSTB INTR
     (and also clear IE)
   - A transition of DONE from 0 to 1 sets CSTB INTR from IE

   The output of CSTB INTR is OR'd with the AND of RPCS1<SC,DONE,IE> to
   create the interrupt request signal.  Thus,

   - The DONE interrupt is edge sensitive, but the SC interrupt is
     level sensitive.
   - The DONE interrupt, once set, is not disabled if IE is cleared,
     but the SC interrupt is.
*/

#include "e10025_defs.h"

/* CS1 - base + 000 - control/status 1 */

#define CS1_OF          0
#define CS1_GO          CSR_GO                          /* go */
#define CS1_V_FNC       1                               /* function pos */
#define CS1_M_FNC       037                             /* function mask */
#define CS1_FNC         (CS1_M_FNC << CS1_V_FNC)
#define FNC_XFER        024                             /* >=? data xfr */
#define CS1_IE          CSR_IE                          /* int enable */
#define CS1_DONE        CSR_DONE                        /* ready */
#define CS1_V_UAE       8                               /* Unibus addr ext */
#define CS1_M_UAE       03
#define CS1_UAE         (CS1_M_UAE << CS1_V_UAE)
#define CS1_MCPE        0020000                         /* Mbus par err NI */
#define CS1_TRE         0040000                         /* transfer err */
#define CS1_SC          0100000                         /* special cond */
#define CS1_MBZ         0012000
#define CS1_DRV         (CS1_FNC | CS1_GO)
#define GET_FNC(x)      (((x) >> CS1_V_FNC) & CS1_M_FNC)

/* WC - base + 002 - word count */

#define WC_OF           1

/* BA - base + 004 - base address */

#define BA_OF           2
#define BA_MBZ          0000001                         /* must be zero */

/* CS2 - base + 010 - control/status 2 */

#define CS2_OF          3
#define CS2_V_UNIT      0                               /* unit pos */
#define CS2_M_UNIT      07                              /* unit mask */
#define CS2_UNIT        (CS2_M_UNIT << CS2_V_UNIT)
#define CS2_UAI         0000010                         /* addr inhibit */
#define CS2_PAT         0000020                         /* parity test NI */
#define CS2_CLR         0000040                         /* controller clear */
#define CS2_IR          0000100                         /* input ready */
#define CS2_OR          0000200                         /* output ready */
#define CS2_MDPE        0000400                         /* Mbus par err NI */
#define CS2_MXF         0001000                         /* missed xfer NI */
#define CS2_PGE         0002000                         /* program err */
#define CS2_NEM         0004000                         /* nx mem err */
#define CS2_NED         0010000                         /* nx drive err */
#define CS2_PE          0020000                         /* parity err NI */
#define CS2_WCE         0040000                         /* write check err */
#define CS2_DLT         0100000                         /* data late NI */
#define CS2_MBZ         (CS2_CLR)
#define CS2_RW          (CS2_UNIT | CS2_UAI | CS2_PAT | CS2_MXF | CS2_PE)
#define CS2_ERR         (CS2_MDPE | CS2_MXF | CS2_PGE | CS2_NEM | \
                         CS2_NED | CS2_PE | CS2_WCE | CS2_DLT)
#define GET_UNIT(x)     (((x) >> CS2_V_UNIT) & CS2_M_UNIT)

/* DB - base + 022 - data buffer */

#define DB_OF           4

/* BAE - base + 050/34 - bus address extension */

#define BAE_OF          5
#define AE_M_MAE        0                               /* addr ext pos */
#define AE_V_MAE        077                             /* addr ext mask */
#define AE_MBZ          0177700

/* CS3 - base + 052/36 - control/status 3 */

#define CS3_OF          6
#define CS3_APE         0100000                         /* addr perr - NI */
#define CS3_DPO         0040000                         /* data perr odd - NI */
#define CS3_DPE         0020000                         /* data perr even - NI */
#define CS3_WCO         0010000                         /* wchk err odd */
#define CS3_WCE         0004000                         /* wchk err even */
#define CS3_DBL         0002000                         /* dbl word xfer - NI */
#define CS3_IPCK        0000017                         /* wrong par - NI */
#define CS3_ERR         (CS3_APE|CS3_DPO|CS3_DPE|CS3_WCO|CS3_WCE)
#define CS3_MBZ         0001660
#define CS3_RW          (CS1_IE | CS3_IPCK)

#define MBA_OFSMASK     077                             /* max 32 reg */
#define INT             0000                            /* int reg flag */
#define EXT             0100                            /* ext reg flag */

/* The E10025 guest I/O window is a fixed physical bank.  RH11 addressing is
   used unconditionally; there is no Map_Addr() in this simulator. */
#define RH11            1
#define RH_IOLN         054                             /* RP register block */

/* RP register dispatch, supplied by e10025_rp.c */

extern t_stat rp_mbrd (int32 *data, int32 ofs, int32 drv);
extern t_stat rp_mbwr (int32 data, int32 ofs, int32 drv);
extern int32 rp_abort (void);

typedef struct {
    uint32 cs1;                                         /* ctrl/status 1 */
    uint32 wc;                                          /* word count */
    uint32 ba;                                          /* bus addr */
    uint32 cs2;                                         /* ctrl/status 2 */
    uint32 db;                                          /* data buffer */
    uint32 bae;                                         /* addr ext */
    uint32 cs3;                                         /* ctrl/status 3 */
    uint32 iff;                                         /* int flip flop */
    } MBACTX;

static MBACTX massbus[1];                               /* one adapter */

t_stat mba_reset (DEVICE *dptr);
t_stat mba_rd (int32 *val, int32 pa, int32 access);
t_stat mba_wr (int32 val, int32 pa, int32 access);
t_stat mba_show_num (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
t_stat mba_show_type (FILE *st, UNIT *uptr, int32 val, CONST void *desc);
int32 mba_inta (void);
void mba_set_int (uint32 mb);
void mba_clr_int (uint32 mb);
void mba_upd_cs1 (uint32 set, uint32 clr, uint32 mb);
void mba_set_cs2 (uint32 flg, uint32 mb);
int32 mba_map_pa (int32 pa, int32 *ofs);
t_stat rh_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *rh_description (DEVICE *dptr);
static void rh_install_rp (void);

/* Massbus register dispatches */

static t_stat (*mbregR[1])(int32 *dat, int32 ad, int32 md) = { NULL };
static t_stat (*mbregW[1])(int32 dat, int32 ad, int32 md) = { NULL };
static int32 (*mbabort[1])(void) = { NULL };

/* Unibus to register offset map */

static int32 mba_mapofs[(MBA_OFSMASK + 1) >> 1] = {
  INT|0,  INT|1,  INT|2,  EXT|5,  INT|3,  EXT|1,  EXT|2,  EXT|4,
  EXT|7,  INT|4,  EXT|3,  EXT|6,  EXT|8,  EXT|9,  EXT|10, EXT|11,
  EXT|12, EXT|13, EXT|14, EXT|15, EXT|16, EXT|17, EXT|18, EXT|19,
  EXT|20, EXT|21, EXT|22, EXT|23, EXT|24, EXT|25, EXT|26, EXT|27
  };

/* Massbus adapter data structures */

static DIB rh_dib = {
    IOBA_AUTO, RH_IOLN, &mba_rd, &mba_wr,
    1, IVCL (RHA), VEC_AUTO, { &mba_inta }
    };

static UNIT rh_unit = { UDATA (NULL, 0, 0) };

static REG rh_reg[] = {
    { ORDATAD (CS1, massbus[0].cs1, 16, "control/status register 1") },
    { ORDATAD (WC, massbus[0].wc, 16, "word count") },
    { ORDATAD (BA, massbus[0].ba, 16, "bus address") },
    { ORDATAD (CS2, massbus[0].cs2, 16, "control/status register 2") },
    { ORDATAD (DB, massbus[0].db, 16, "data buffer") },
    { ORDATAD (BAE, massbus[0].bae, 6, "bus address extension") },
    { ORDATAD (CS3, massbus[0].cs3, 16, "control/status register 3") },
    { FLDATAD (IFF, massbus[0].iff, 0, "transfer complete interrupt request flag") },
    { FLDATAD (INT, IREQ (RHA), INT_V_RHA, "interrupt pending flag (RP)") },
    { FLDATAD (SC, massbus[0].cs1, CSR_V_ERR, "special condition (CSR1<15>)") },
    { FLDATAD (DONE, massbus[0].cs1, CSR_V_DONE, "device done flag (CSR1<7>)") },
    { FLDATAD (IE, massbus[0].cs1, CSR_V_IE, "interrupt enable flag (CSR1<6>)") },
    { ORDATA (DEVADDR, rh_dib.ba, 32), REG_HRO },
    { ORDATA (DEVVEC, rh_dib.vec, 16), REG_HRO },
    { NULL }
    };

static MTAB rh_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0100, "ADDRESS", "ADDRESS",
      &set_addr, &show_addr, NULL, "Bus address" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "VECTOR", "VECTOR",
      &set_vec, &show_vec, NULL, "Interrupt vector" },
    { MTAB_XTD|MTAB_VDV, 0, "TYPE", NULL,
      NULL, &mba_show_type, NULL, "Massbus Adapter Type" },
    { 0 }
    };

DEVICE mba_dev[] = {
    {
    "RH", &rh_unit, rh_reg, rh_mod,
    1, 0, 0, 0, 0, 0,
    NULL, NULL, &mba_reset,
    NULL, NULL, NULL,
    &rh_dib, DEV_DISABLE | DEV_DEBUG | DEV_UBUS, 0,
    NULL, NULL, "RHA", &rh_help, NULL, NULL, &rh_description,
    NULL, NULL, NULL
    }
    };

/* Read Massbus adapter register */

t_stat mba_rd (int32 *val, int32 pa, int32 mode)
{
int32 ofs, dat, mb, drv;
t_stat r;

mb = mba_map_pa (pa, &ofs);                             /* get mb number */
if ((mb < 0) || (ofs < 0))                              /* valid? */
    return SCPE_NXM;
drv = GET_UNIT (massbus[mb].cs2);                       /* get drive */
mba_upd_cs1 (0, 0, mb);                                 /* update CS1 */

if (ofs & EXT) {                                        /* external? */
    if (!mbregR[mb])                                    /* device there? */
        return SCPE_NXM;
    r = mbregR[mb] (val, ofs & ~EXT, drv);              /* call device */
    if (r == MBE_NXD)                                   /* nx drive? */
        mba_set_cs2 (CS2_NED, mb);
    else if (r == MBE_NXR)                              /* nx reg? */
        return SCPE_NXM;
    return SCPE_OK;
    }

switch (ofs) {                                          /* case on reg */

    case CS1_OF:                                        /* CS1 */
        if (!mbregR[mb])                                /* nx device? */
            return SCPE_NXM;
        r = mbregR[mb] (&dat, ofs, drv);                /* get dev cs1 */
        if (r == MBE_NXD)                               /* nx drive? */
            mba_set_cs2 (CS2_NED, mb);
        *val = massbus[mb].cs1 | dat;
        break;

    case WC_OF:                                         /* WC */
        *val = massbus[mb].wc;
        break;

    case BA_OF:                                         /* BA */
        *val = massbus[mb].ba & ~BA_MBZ;
        break;

    case CS2_OF:                                        /* CS2 */
        *val = massbus[mb].cs2 = (massbus[mb].cs2 & ~CS2_MBZ) | CS2_IR | CS2_OR;
        break;

    case DB_OF:                                         /* DB */
        *val = massbus[mb].db;
        break;

    case BAE_OF:                                        /* BAE */
        *val = massbus[mb].bae = massbus[mb].bae & ~AE_MBZ;
        break;

    case CS3_OF:                                        /* CS3 */
        *val = massbus[mb].cs3 = (massbus[mb].cs3 & ~(CS1_IE | CS3_MBZ)) |
            (massbus[mb].cs1 & CS1_IE);
        break;

    default:                                            /* huh? */
        return SCPE_NXM;
        }

return SCPE_OK;
}

t_stat mba_wr (int32 val, int32 pa, int32 access)
{
int32 ofs, cs1f, drv, mb;
t_stat r;
t_bool cs1dt;

mb = mba_map_pa (pa, &ofs);                             /* get mb number */
if ((mb < 0) || (ofs < 0))                              /* valid? */
    return SCPE_NXM;
drv = GET_UNIT (massbus[mb].cs2);                       /* get drive */

if (ofs & EXT) {                                        /* external? */
    if (!mbregW[mb])                                    /* device there? */
        return SCPE_NXM;
    if ((access == WRITEB) && (pa & 1))                 /* byte writes */
        val = val << 8;                                 /* don't work */
    r = mbregW[mb] (val, ofs & ~EXT, drv);              /* write dev reg */
    if (r == MBE_NXD)                                   /* nx drive? */
        mba_set_cs2 (CS2_NED, mb);
    else if (r == MBE_NXR)                              /* nx reg? */
        return SCPE_NXM;
    mba_upd_cs1 (0, 0, mb);                             /* update status */
    return SCPE_OK;
    }

cs1f = 0;                                               /* no int on cs1 upd */
switch (ofs) {                                          /* case on reg */

    case CS1_OF:                                        /* CS1 */
        if (!mbregW[mb])                                /* device exist? */
            return SCPE_NXM;
        if ((access == WRITEB) && (pa & 1))
            val = val << 8;
        if (val & CS1_TRE) {                            /* error clear? */
            massbus[mb].cs1 &= ~CS1_TRE;                /* clr CS1<TRE> */
            massbus[mb].cs2 &= ~CS2_ERR;                /* clr CS2<15:8> */
            massbus[mb].cs3 &= ~CS3_ERR;                /* clr CS3<15:11> */
            }
        if ((access == WRITE) || (pa & 1)) {            /* hi byte write? */
            if (massbus[mb].cs1 & CS1_DONE)             /* done set? */
                massbus[mb].cs1 = (massbus[mb].cs1 & ~CS1_UAE) | (val & CS1_UAE);
            }
        if ((access == WRITE) || !(pa & 1)) {           /* lo byte write? */
            if ((val & CS1_DONE) && (val & CS1_IE))     /* to DONE+IE? */
                massbus[mb].iff = 1;                    /* set CSTB INTR */
            massbus[mb].cs1 = (massbus[mb].cs1 & ~CS1_IE) | (val & CS1_IE);
            cs1dt = (val & CS1_GO) && (GET_FNC (val) >= FNC_XFER);
            if (cs1dt && ((massbus[mb].cs1 & CS1_DONE) == 0))  /* dt, done clr? */
                mba_set_cs2 (CS2_PGE, mb);              /* prgm error */
            else {
                r = mbregW[mb] (val & 077, ofs, drv);   /* write dev CS1 */
                if (r == MBE_NXD)                       /* nx drive? */
                    mba_set_cs2 (CS2_NED, mb);
                else if (r == MBE_NXR)                  /* nx reg? */
                    return SCPE_NXM;
                else if (cs1dt && (r == SCPE_OK)) {     /* xfer, no err? */
                    massbus[mb].cs1 &= ~(CS1_TRE | CS1_MCPE | CS1_DONE);
                    massbus[mb].cs2 &= ~CS2_ERR;        /* clear errors */
                    massbus[mb].cs3 &= ~(CS3_ERR | CS3_DBL);
                    if (DEBUG_PRS (mba_dev[mb]))
                        fprintf (sim_deb, ">>RH STRT: cs1=%o, cs2=%o,ba=%o, wc=%o\n",
                            massbus[mb].cs1, massbus[mb].cs2, massbus[mb].ba, massbus[mb].wc);
                    }
                }
            }
        massbus[mb].cs3 = (massbus[mb].cs3 & ~CS1_IE) | /* update CS3 */
            (massbus[mb].cs1 & CS1_IE);
        massbus[mb].bae = (massbus[mb].bae & ~CS1_M_UAE) | /* update BAE */
            ((massbus[mb].cs1 >> CS1_V_UAE) & CS1_M_UAE);
        break;

    case WC_OF:                                         /* WC */
        if (access == WRITEB)
            val = (pa & 1)?
            (massbus[mb].wc & 0377) | (val << 8):
            (massbus[mb].wc & ~0377) | val;
        massbus[mb].wc = val;
        break;

    case BA_OF:                                         /* BA */
        if (access == WRITEB)
            val = (pa & 1)?
            (massbus[mb].ba & 0377) | (val << 8):
            (massbus[mb].ba & ~0377) | val;
        massbus[mb].ba = val & ~BA_MBZ;
        break;

    case CS2_OF:                                        /* CS2 */
        if ((access == WRITEB) && (pa & 1))
            val = val << 8;
        if (val & CS2_CLR)                              /* init? */
            mba_reset (&mba_dev[mb]);
        else {
            if ((val & ~massbus[mb].cs2) & (CS2_PE | CS2_MXF))
                cs1f = CS1_SC;                          /* diagn intr */
            if (access == WRITEB)                       /* merge val */
                val = (massbus[mb].cs2 & ((pa & 1)? 0377: 0177400)) | val;
            massbus[mb].cs2 = (massbus[mb].cs2 & ~CS2_RW) |
                (val & CS2_RW) | CS2_IR | CS2_OR;
            }
        break;

    case DB_OF:                                         /* DB */
        if (access == WRITEB)
            val = (pa & 1)?
            (massbus[mb].db & 0377) | (val << 8):
            (massbus[mb].db & ~0377) | val;
        massbus[mb].db = val;
        break;

    case BAE_OF:                                       /* BAE */
        if ((access == WRITEB) && (pa & 1))
            break;
        massbus[mb].bae = val & ~AE_MBZ;
        massbus[mb].cs1 = (massbus[mb].cs1 & ~CS1_UAE) | /* update CS1 */
            ((massbus[mb].bae << CS1_V_UAE) & CS1_UAE);
        break;

    case CS3_OF:                                        /* CS3 */
        if ((access == WRITEB) && (pa & 1))
            break;
        massbus[mb].cs3 = (massbus[mb].cs3 & ~CS3_RW) | (val & CS3_RW);
        massbus[mb].cs1 = (massbus[mb].cs1 & ~CS1_IE) | /* update CS1 */
            (massbus[mb].cs3 & CS1_IE);
        break;

    default:
        return SCPE_NXM;
        }

mba_upd_cs1 (cs1f, 0, mb);                              /* update status */
return SCPE_OK;
}

/* Massbus I/O routines

   mba_rdbufW    -       fetch word buffer from memory
   mba_wrbufW    -       store word buffer into memory
   mba_chbufW    -       compare word buffer with memory

   Returns number of bytes successfully transferred/checked
*/

int32 mba_rdbufW (uint32 mb, int32 bc, uint16 *buf)
{
int32 i, j, ba, mbc, pbc;
uint32 pa;

bc = bc & ~1;                                           /* bc even */
if (mb >= 1)                                            /* valid MBA? */
    return 0;
ba = (massbus[mb].bae << 16) | massbus[mb].ba;          /* get busaddr */
mbc = (0200000 - massbus[mb].wc) << 1;                  /* MB byte count */
if (bc > mbc)                                           /* use smaller */
    bc = mbc;
for (i = 0; i < bc; i = i + pbc) {                      /* loop by pages */
    pa = ba;                                            /* fixed E10025 map */
    if (!ADDR_IS_MEM (pa)) {                            /* NXM? */
        mba_set_cs2 (CS2_NEM, mb);                      /* set error */
        break;
        }
    pbc = UBM_PAGSIZE - UBM_GETOFF (pa);                /* left in page */
    if (pbc > (bc - i))                                 /* limit to rem xfr */
        pbc = bc - i;
    for (j = 0; j < pbc; j = j + 2) {                   /* loop by words */
        *buf++ = RdMemW (pa);                           /* fetch word */
        if (!(massbus[mb].cs2 & CS2_UAI)) {             /* if not inhb */
            ba = ba + 2;                                /* incr ba, pa */
            pa = pa + 2;
            }
        }
    }
massbus[mb].wc = (massbus[mb].wc + (bc >> 1)) & DMASK;   /* update wc */
massbus[mb].ba = ba & DMASK;                             /* update ba */
massbus[mb].bae = (ba >> 16) & ~AE_MBZ;                  /* upper 6b */
massbus[mb].cs1 = (massbus[mb].cs1 & ~ CS1_UAE) |         /* update CS1 */
    ((massbus[mb].bae << CS1_V_UAE) & CS1_UAE);
return i;
}

int32 mba_wrbufW (uint32 mb, int32 bc, const uint16 *buf)
{
int32 i, j, ba, mbc, pbc;
uint32 pa;

bc = bc & ~1;                                           /* bc even */
if (mb >= 1)                                            /* valid MBA? */
    return 0;
ba = (massbus[mb].bae << 16) | massbus[mb].ba;          /* get busaddr */
mbc = (0200000 - massbus[mb].wc) << 1;                  /* MB byte count */
if (bc > mbc)                                           /* use smaller */
    bc = mbc;
for (i = 0; i < bc; i = i + pbc) {                      /* loop by pages */
    pa = ba;                                            /* fixed E10025 map */
    if (!ADDR_IS_MEM (pa)) {                            /* NXM? */
        mba_set_cs2 (CS2_NEM, mb);                      /* set error */
        break;
        }
    pbc = UBM_PAGSIZE - UBM_GETOFF (pa);                /* left in page */
    if (pbc > (bc - i))                                 /* limit to rem xfr */
        pbc = bc - i;
    for (j = 0; j < pbc; j = j + 2) {                   /* loop by words */
        WrMemW (pa, *buf++);                            /* put word */
        if (!(massbus[mb].cs2 & CS2_UAI)) {             /* if not inhb */
            ba = ba + 2;                                /* incr ba, pa */
            pa = pa + 2;
            }
        }
    }
massbus[mb].wc = (massbus[mb].wc + (bc >> 1)) & DMASK;  /* update wc */
massbus[mb].ba = ba & DMASK;                             /* update ba */
massbus[mb].bae = (ba >> 16) & ~AE_MBZ;                  /* upper 6b */
massbus[mb].cs1 = (massbus[mb].cs1 & ~ CS1_UAE) |         /* update CS1 */
    ((massbus[mb].bae << CS1_V_UAE) & CS1_UAE);
return i;
}

int32 mba_chbufW (uint32 mb, int32 bc, uint16 *buf)
{
int32 i, j, ba, mbc, pbc;
uint32 pa;

bc = bc & ~1;                                           /* bc even */
if (mb >= 1)                                            /* valid MBA? */
    return 0;
ba = (massbus[mb].bae << 16) | massbus[mb].ba;          /* get busaddr */
mbc = (0200000 - massbus[mb].wc) << 1;                  /* MB byte count */
if (bc > mbc)                                           /* use smaller */
    bc = mbc;
for (i = 0; i < bc; i = i + pbc) {                      /* loop by pages */
    pa = ba;                                            /* fixed E10025 map */
    if (!ADDR_IS_MEM (pa)) {                            /* NXM? */
        mba_set_cs2 (CS2_NEM, mb);                      /* set error */
        break;
        }
    pbc = UBM_PAGSIZE - UBM_GETOFF (pa);                /* left in page */
    if (pbc > (bc - i))                                 /* limit to rem xfr */
        pbc = bc - i;
    for (j = 0; j < pbc; j = j + 2) {                   /* loop by words */
        massbus[mb].db = *buf++;                        /* get dev word */
        if (RdMemW (pa) != massbus[mb].db) {            /* miscompare? */
            mba_set_cs2 (CS2_WCE, mb);                  /* set error */
            massbus[mb].cs3 = massbus[mb].cs3 |         /* set even/odd */
                ((pa & 1)? CS3_WCO: CS3_WCE);
            break;
            }
        if (!(massbus[mb].cs2 & CS2_UAI)) {             /* if not inhb */
            ba = ba + 2;                                /* incr ba, pa */
            pa = pa + 2;
            }
        }
    }
massbus[mb].wc = (massbus[mb].wc + (bc >> 1)) & DMASK;  /* update wc */
massbus[mb].ba = ba & DMASK;                             /* update ba */
massbus[mb].bae = (ba >> 16) & ~AE_MBZ;                  /* upper 6b */
massbus[mb].cs1 = (massbus[mb].cs1 & ~ CS1_UAE) |         /* update CS1 */
    ((massbus[mb].bae << CS1_V_UAE) & CS1_UAE);
return i;
}

/* Device access, status, and interrupt routines */

void mba_set_don (uint32 mb)
{
mba_upd_cs1 (CS1_DONE, 0, mb);
if (DEBUG_PRS (mba_dev[mb]))
    fprintf (sim_deb, ">>RH DONE: cs1=%o, cs2=%o,ba=%o, wc=%o\n",
        massbus[mb].cs1, massbus[mb].cs2, massbus[mb].ba, massbus[mb].wc);
return;
}

void mba_upd_ata (uint32 mb, uint32 val)
{
if (val)
    mba_upd_cs1 (CS1_SC, 0, mb);
else mba_upd_cs1 (0, CS1_SC, mb);
return;
}

void mba_set_exc (uint32 mb)
{
mba_upd_cs1 (CS1_TRE | CS1_DONE, 0, mb);
return;
}

int32 mba_get_bc (uint32 mb)
{
if (mb >= 1)
    return 0;
return ((0200000 - massbus[mb].wc) << 1);
}

int32 mba_get_csr (uint32 mb)
{
if (mb >= 1)
    return 0;
return rh_dib.ba;
}

void mba_set_int (uint32 mb)
{
if (mb >= 1)
    return;
int_req[rh_dib.vloc >> 5] |= (1 << (rh_dib.vloc & 037));
return;
}

void mba_clr_int (uint32 mb)
{
if (mb >= 1)
    return;
int_req[rh_dib.vloc >> 5] &= ~(1 << (rh_dib.vloc & 037));
return;
}

void mba_upd_cs1 (uint32 set, uint32 clr, uint32 mb)
{
if (mb >= 1)
    return;
if ((set & ~massbus[mb].cs1) & CS1_DONE)                    /* DONE 0 to 1? */
    massbus[mb].iff = (massbus[mb].cs1 & CS1_IE)? 1: 0;     /* CSTB INTR <- IE */
massbus[mb].cs1 = (massbus[mb].cs1 & ~(clr | CS1_MCPE | CS1_MBZ | CS1_DRV)) | set;
if (massbus[mb].cs2 & CS2_ERR)
    massbus[mb].cs1 = massbus[mb].cs1 | CS1_TRE | CS1_SC;
else if (massbus[mb].cs1 & CS1_TRE)
    massbus[mb].cs1 = massbus[mb].cs1 | CS1_SC;
if (massbus[mb].iff ||
    ((massbus[mb].cs1 & CS1_SC) &&
     (massbus[mb].cs1 & CS1_DONE) &&
     (massbus[mb].cs1 & CS1_IE)))
    mba_set_int (mb);
else mba_clr_int (mb);
return;
}

void mba_set_cs2 (uint32 flag, uint32 mb)
{
if (mb >= 1)
    return;
massbus[mb].cs2 = massbus[mb].cs2 | flag;
mba_upd_cs1 (0, 0, mb);
return;
}

/* Interrupt acknowledge */

int32 mba_inta (void)
{
massbus[0].cs1 &= ~CS1_IE;                              /* clear int enable */
massbus[0].cs3 &= ~CS1_IE;                              /* in both registers */
massbus[0].iff = 0;                                     /* clear CSTB INTR */
return rh_dib.vec;                                      /* acknowledge */
}

/* Map physical address to Massbus number, offset */

int32 mba_map_pa (int32 pa, int32 *ofs)
{
int32 uo, ba, lnt;

ba = rh_dib.ba;
lnt = rh_dib.lnt;
if ((pa >= ba) &&                                       /* in range? */
    (pa < (ba + lnt))) {
    if (pa < (ba + (lnt - 4))) {                        /* not last two? */
        uo = ((pa - ba) & MBA_OFSMASK) >> 1;            /* get Unibus offset */
        *ofs = mba_mapofs[uo];                          /* map thru PROM */
        return 0;                                       /* return ctrl idx */
        }
    return -1;                                          /* RH11: done */
    }
return -1;
}

/* Reset Massbus adapter */

t_stat mba_reset (DEVICE *dptr)
{
uint32 mb;

mb = 0;                                                 /* one adapter */
(void) dptr;
rh_install_rp ();                                       /* RP drive dispatch */
massbus[mb].cs1 = CS1_DONE;
if ((sim_switches & SWMASK ('P')) !=0)
    massbus[mb].wc = 0;                             /* powerup only */
massbus[mb].ba = 0;
massbus[mb].cs2 = 0;
massbus[mb].db = 0;
massbus[mb].bae= 0;
massbus[mb].cs3 = 0;
massbus[mb].iff = 0;
mba_clr_int (mb);
if (mbabort[mb])
    mbabort[mb] ();
return build_dib_tab();
}

/* Enable/disable Massbus adapter.  The adapter and RP are registered as
   ordinary channel devices, so there is no MBA number to reassign here. */

void mba_set_enbdis (DEVICE *dptr)
{
(void) dptr;
return;
}

/* Show Massbus adapter number */

t_stat mba_show_num (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
(void) uptr;
(void) val;
(void) desc;
fprintf (st, "Massbus adapter 0 (RH)");
return SCPE_OK;
}

/* Show adapter type */

t_stat mba_show_type (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
(void) uptr;
(void) val;
(void) desc;
fprintf (st, "RH11 (E10025 port of simh RH11/RP04-05 prototype)");
return SCPE_OK;
}

/* Install the RP drive dispatch.  Called at reset; no shared io_lib branch
   is needed because this simulator has a single Massbus controller. */

static void rh_install_rp (void)
{
mbregR[0] = &rp_mbrd;
mbregW[0] = &rp_mbwr;
mbabort[0] = &rp_abort;
return;
}

t_stat rh_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
const char *const text =
/*567901234567890123456789012345678901234567890123456789012345678901234567890*/
" RH11 Massbus adapter (RH) for the Electronica-100-25 port\n"
"\n"
" The adapter uses the simh RH11 register and interrupt model with one RP\n"
" disk controller.  The guest CSR is 016700 and the vector is 0254, taken\n"
" from the existing RHA entry in auto_tab.  [GAP] The historical DP 20 MB\n"
" controller is not proven identical to RP04/05 [G].\n"
/*567901234567890123456789012345678901234567890123456789012345678901234567890*/
"\n"
"1$Set commands\n"
"1$Show commands\n"
"1$Registers\n";
return scp_help (st, dptr, uptr, flag, text, "");
}

const char *rh_description (DEVICE *dptr)
{
(void) dptr;
return "RH11 Massbus adapter (RP04/05 prototype, DP identity [G])";
}

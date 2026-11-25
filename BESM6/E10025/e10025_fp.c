/* e10025_fp.c: floating point operations for Электроника-100-25.
   Book 3 (ИЧМ2.791.003 ТО1), section 3.9, PDF pages 106-108: FADD/FSUB/
   FMUL/FDIV use R as a stack pointer, B=(R,R+2), A=(R+4,R+6), result goes
   to A and R is advanced by 4. Divide by zero leaves the stack untouched.
   The arithmetic helpers below are the single-precision helpers from
   simh PDP11/pdp11_fp.c (MIT license, copyright Robert M Supnik), which use
   the same excess-128, hidden-bit 32-bit format. Constants and exception
   machinery used only by the FP11 are omitted.

   Copyright (c) 1993-2023, Robert M Supnik

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
extern int32 N, Z, V, C;
extern int32 ReadW (int32 addr);
extern void WriteW (int32 data, int32 addr);

/* Single-precision format, 32b relative to fpac_t.h */
#define FP_V_SIGN       31
#define FP_V_EXP        23
#define FP_V_HB         23
#define FP_V_FROUND     (-1)
#define FP_M_EXP        0377
#define FP_SIGN         (1u << FP_V_SIGN)
#define FP_EXP          (FP_M_EXP << FP_V_EXP)
#define FP_HB           (1u << FP_V_HB)
#define FP_FRACH        ((1u << FP_V_HB) - 1)
#define FP_FRACL        0xFFFFFFFF
#define FP_BIAS         0200
#define FP_GUARD        3

#define GET_BIT(ir,n)   (((ir) >> (n)) & 1)
#define GET_SIGN(ir)    GET_BIT ((ir), FP_V_SIGN)
#define GET_EXP(ir)     (((ir) >> FP_V_EXP) & FP_M_EXP)

#define F_ADD(s2,s1,ds) \
                        ds.l = (s1.l + s2.l) & 0xFFFFFFFF; \
                        ds.h = (s1.h + s2.h + (ds.l < s2.l)) & 0xFFFFFFFF
#define F_SUB(s2,s1,ds) \
                        ds.h = (s1.h - s2.h - (s1.l < s2.l)) & 0xFFFFFFFF; \
                        ds.l = (s1.l - s2.l) & 0xFFFFFFFF
#define F_LT(x,y)       ((x.h < y.h) || ((x.h == y.h) && (x.l < y.l)))
#define F_LT_AP(x,y)    (((x->h & ~FP_SIGN) < (y->h & ~FP_SIGN)) || \
                        (((x->h & ~FP_SIGN) == (y->h & ~FP_SIGN)) && (x->l < y->l)))
#define F_GET_FRAC_P(sr,ds) \
                        ds.l = sr->l; \
                        ds.h = (sr->h & FP_FRACH) | FP_HB
#define F_LSH_1(ds)     ds.h = ((ds.h << 1) | ((ds.l >> 31) & 1)) & 0xFFFFFFFF; \
                        ds.l = (ds.l << 1) & 0xFFFFFFFF
#define F_RSH_1(ds)     ds.l = ((ds.l >> 1) & 0x7FFFFFFF) | ((ds.h & 1) << 31); \
                        ds.h = ((ds.h >> 1) & 0x7FFFFFFF)
#define F_LSH_K(sr,n,ds) \
                        ds.h = ((sr.h << (n)) | ((sr.l >> (32 - (n))) & and_mask[n])) \
                                & 0xFFFFFFFF; \
                        ds.l = (sr.l << (n)) & 0xFFFFFFFF
#define F_RSH_K(sr,n,ds) \
                        ds.l = (((sr.l >> (n)) & and_mask[32 - (n)]) | \
                                (sr.h << (32 - (n)))) & 0xFFFFFFFF; \
                        ds.h = ((sr.h >> (n)) & and_mask[32 - (n)]) & 0xFFFFFFFF
#define F_LSH_V(sr,n,ds) \
                        ds.h = (((n) >= 32)? (sr.l << ((n) - 32)): \
                                (sr.h << (n)) | ((sr.l >> (32 - (n))) & and_mask[n])) \
                                & 0xFFFFFFFF; \
                        ds.l = ((n) >= 32)? 0: (sr.l << (n)) & 0xFFFFFFFF
#define F_RSH_V(sr,n,ds) \
                        ds.l = (((n) >= 32)? (sr.h >> ((n) - 32)) & and_mask[64 - (n)]: \
                                ((sr.l >> (n)) & and_mask[32 - (n)]) | \
                                (sr.h << (32 - (n)))) & 0xFFFFFFFF; \
                        ds.h = ((n) >= 32)? 0: \
                                ((sr.h >> (n)) & and_mask[32 - (n)]) & 0xFFFFFFFF
#define F_LSH_GUARD(ds) F_LSH_K (ds, FP_GUARD, ds)
#define F_RSH_GUARD(ds) F_RSH_K (ds, FP_GUARD, ds)

static const uint32 and_mask[33] = { 0,
    0x1, 0x3, 0x7, 0xF,
    0x1F, 0x3F, 0x7F, 0xFF,
    0x1FF, 0x3FF, 0x7FF, 0xFFF,
    0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF,
    0x1FFFF, 0x3FFFF, 0x7FFFF, 0xFFFFF,
    0x1FFFFF, 0x3FFFFF, 0x7FFFFF, 0xFFFFFF,
    0x1FFFFFF, 0x3FFFFFF, 0x7FFFFFF, 0xFFFFFFF,
    0x1FFFFFFF, 0x3FFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF
    };

static fpac_t zero_fac = { 0, 0 };
static fpac_t fround_guard_fac = { 0, (1u << (FP_V_FROUND + FP_GUARD)) };

static int32 addfp11 (fpac_t *facp, fpac_t *fsrcp);
static int32 mulfp11 (fpac_t *facp, fpac_t *fsrcp);
static int32 divfp11 (fpac_t *facp, fpac_t *fsrcp);
static void frac_mulfp11 (fpac_t *f1p, fpac_t *f2p);
static int32 round_and_pack (fpac_t *facp, int32 exp, fpac_t *fracp);

static int32 round_and_pack (fpac_t *facp, int32 exp, fpac_t *fracp)
{
fpac_t frac = *fracp;

F_ADD (fround_guard_fac, frac, frac);
if (GET_BIT (frac.h, FP_V_HB + FP_GUARD + 1)) {
    F_RSH_1 (frac);
    exp = exp + 1;
    }
/* Diagnostic hook: printf ("DEBUG PACK exp=%d frac=%08x round=%08x\n",
                           exp, frac.h, fround_guard_fac.h); */
F_RSH_GUARD (frac);
facp->l = frac.l & FP_FRACL;
facp->h = (facp->h & FP_SIGN) | ((exp & FP_M_EXP) << FP_V_EXP) |
          (frac.h & FP_FRACH);
if ((exp > 0377) || (exp <= 0))                        /* underflow or overflow */
    *facp = zero_fac;                                   /* book 3: below 2^-128 -> 0 */
return 0;                                               /* V, C always cleared */
}

static int32 addfp11 (fpac_t *facp, fpac_t *fsrcp)
{
int32 facexp, fsrcexp, ediff;
fpac_t facfrac, fsrcfrac;

if (F_LT_AP (facp, fsrcp)) {
    facfrac = *facp;
    *facp = *fsrcp;
    *fsrcp = facfrac;
    }
facexp = GET_EXP (facp->h);
fsrcexp = GET_EXP (fsrcp->h);
if (facexp == 0) {
    *facp = fsrcexp? *fsrcp: zero_fac;
    return 0;
    }
if (fsrcexp == 0)
    return 0;
ediff = facexp - fsrcexp;
if (ediff >= 60)
    return 0;
F_GET_FRAC_P (facp, facfrac);
F_GET_FRAC_P (fsrcp, fsrcfrac);
F_LSH_GUARD (facfrac);
F_LSH_GUARD (fsrcfrac);
if (GET_SIGN (facp->h) != GET_SIGN (fsrcp->h)) {
    if (ediff)
        F_RSH_V (fsrcfrac, ediff, fsrcfrac);
    F_SUB (fsrcfrac, facfrac, facfrac);
    if ((facfrac.h | facfrac.l) == 0) {
        *facp = zero_fac;
        return 0;
        }
    if (ediff <= 1) {
        if ((facfrac.h & (0x00FFFFFF << FP_GUARD)) == 0) {
            F_LSH_K (facfrac, 24, facfrac);
            facexp = facexp - 24;
            }
        if ((facfrac.h & (0x00FFF000 << FP_GUARD)) == 0) {
            F_LSH_K (facfrac, 12, facfrac);
            facexp = facexp - 12;
            }
        if ((facfrac.h & (0x00FC0000 << FP_GUARD)) == 0) {
            F_LSH_K (facfrac, 6, facfrac);
            facexp = facexp - 6;
            }
        }
    while (GET_BIT (facfrac.h, FP_V_HB + FP_GUARD) == 0) {
        F_LSH_1 (facfrac);
        facexp = facexp - 1;
        }
    }
else {
    if (ediff)
        F_RSH_V (fsrcfrac, ediff, fsrcfrac);
    F_ADD (fsrcfrac, facfrac, facfrac);
    if (GET_BIT (facfrac.h, FP_V_HB + FP_GUARD + 1)) {
        F_RSH_1 (facfrac);
        facexp = facexp + 1;
        }
    }
return round_and_pack (facp, facexp, &facfrac);
}

static void frac_mulfp11 (fpac_t *f1p, fpac_t *f2p)
{
fpac_t result, mpy, mpc;
int32 i;

result = zero_fac;
mpy = *f1p;
mpc = *f2p;
F_LSH_GUARD (mpc);
if ((mpy.l | mpc.l) == 0) {
    for (i = 0; i < 24; i++) {
        if (mpy.h & 1)
            result.h = result.h + mpc.h;
        F_RSH_1 (result);
        mpy.h = mpy.h >> 1;
        }
    }
else {
    if (mpy.l != 0) {
        for (i = 0; i < 32; i++) {
            if (mpy.l & 1)
                F_ADD (mpc, result, result);
            F_RSH_1 (result);
            mpy.l = mpy.l >> 1;
            }
        }
    for (i = 0; i < 24; i++) {
        if (mpy.h & 1)
            F_ADD (mpc, result, result);
        F_RSH_1 (result);
        mpy.h = mpy.h >> 1;
        }
    }
*f1p = result;
}

static int32 mulfp11 (fpac_t *facp, fpac_t *fsrcp)
{
int32 facexp, fsrcexp;
fpac_t facfrac, fsrcfrac;

facexp = GET_EXP (facp->h);
fsrcexp = GET_EXP (fsrcp->h);
if ((facexp == 0) || (fsrcexp == 0)) {
    *facp = zero_fac;
    return 0;
    }
F_GET_FRAC_P (facp, facfrac);
F_GET_FRAC_P (fsrcp, fsrcfrac);
facexp = facexp + fsrcexp - FP_BIAS;
facp->h = facp->h ^ fsrcp->h;
frac_mulfp11 (&facfrac, &fsrcfrac);
if (GET_BIT (facfrac.h, FP_V_HB + FP_GUARD) == 0) {
    F_LSH_1 (facfrac);
    facexp = facexp - 1;
    }
return round_and_pack (facp, facexp, &facfrac);
}

static int32 divfp11 (fpac_t *facp, fpac_t *fsrcp)
{
int32 facexp, fsrcexp, i, count;
fpac_t facfrac, fsrcfrac, quo;

fsrcexp = GET_EXP (fsrcp->h);
facexp = GET_EXP (facp->h);
if (facexp == 0) {
    *facp = zero_fac;
    return 0;
    }
F_GET_FRAC_P (facp, facfrac);
F_GET_FRAC_P (fsrcp, fsrcfrac);
F_LSH_GUARD (facfrac);
F_LSH_GUARD (fsrcfrac);
facexp = facexp - fsrcexp + FP_BIAS + 1;
facp->h = facp->h ^ fsrcp->h;
count = FP_V_HB + FP_GUARD + 1;
quo = zero_fac;
for (i = count; (i > 0) && ((facfrac.h | facfrac.l) != 0); i--) {
    F_LSH_1 (quo);
    if (!F_LT (facfrac, fsrcfrac)) {
        F_SUB (fsrcfrac, facfrac, facfrac);
        quo.h = quo.h | 1;
        }
    F_LSH_1 (facfrac);
    }
if (i > 0)
    F_LSH_V (quo, i, quo);
if (GET_BIT (quo.h, FP_V_HB + FP_GUARD) == 0) {
    F_LSH_1 (quo);
    facexp = facexp - 1;
    }
return round_and_pack (facp, facexp, &quo);
}

/* Execute FADD/FSUB/FMUL/FDIV with stack pointer reg (book 3, R0..R7). */
int32 e10025_fop (int32 op, int32 reg)
{
int32 base = R[reg] & 0177777;
fpac_t b, a;

b.h = ((uint32) ReadW (base) << 16) | (uint32) ReadW ((base + 2) & 0177777);
b.l = 0;
a.h = ((uint32) ReadW ((base + 4) & 0177777) << 16) |
      (uint32) ReadW ((base + 6) & 0177777);
a.l = 0;
/* Diagnostic hook: printf ("DEBUG LOAD base=%o R5=%d w0=%o w2=%o w4=%o w6=%o b=%08x a=%08x\n",
                           base, R[5], ReadW (base), ReadW ((base + 2) & 0177777),
                           ReadW ((base + 4) & 0177777), ReadW ((base + 6) & 0177777), b.h, a.h); */
if (GET_SIGN (b.h) && (GET_EXP (b.h) == 0))
    b = zero_fac;                                       /* clean dirty zero */
if (GET_SIGN (a.h) && (GET_EXP (a.h) == 0))
    a = zero_fac;
N = Z = V = C = 0;
switch (op) {
case 0:                                                 /* FADD */
    addfp11 (&a, &b);
    break;
case 1:                                                 /* FSUB */
    if (b.h != 0)
        b.h = b.h ^ FP_SIGN;
    addfp11 (&a, &b);
    break;
case 2:                                                 /* FMUL */
    mulfp11 (&a, &b);
    break;
case 3:                                                 /* FDIV */
    if (b.h == 0)                                       /* divide by zero */
        return 0;                                       /* stack untouched, flags kept */
    divfp11 (&a, &b);
    break;
default:
    return 0;
    }
WriteW ((base + 4) & 0177777, (a.h >> 16) & 0177777);
WriteW ((base + 6) & 0177777, a.h & 0177777);
/* Diagnostic hook: printf ("DEBUG WRITE a=%08x hi=%o lo=%o rd=%o\n", a.h,
                           (a.h >> 16) & 0177777, a.h & 0177777,
                           ReadW ((base + 4) & 0177777)); */
R[reg] = (R[reg] + 4) & 0177777;                        /* pop B, keep A */
N = GET_SIGN (a.h);
Z = (GET_EXP (a.h) == 0);
V = C = 0;
/* Diagnostic hook: printf ("DEBUG FOP op=%d b=%08x a=%08x N=%d Z=%d\n", op, b.h, a.h, N, Z); */
return 0;
}

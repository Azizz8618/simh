/*
 * BESM-6 graph plotter: БЕНСОН / ГП НИТИ with real 18Р interrupts
 *
 * Copyright (c) 2026, Leonid Broukhis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Except as contained in this notice, the name of Leonid Broukhis shall not
 * be used in advertising or otherwise to promote the sale, use or other
 * dealings in this Software without prior written authorization from
 * Leonid Broukhis.
 */

/*
 * The line from the plotter is the 18th bit of ГРП, an INTERNAL interrupt.
 * It is enabled by ТВ67 and disabled by ТВ67А (ЭК 01215), enters through
 * 0500₈ (ВНУПР) and is dispatched via the guest cell обвнп1+30 = 04023₈
 * using M16 = 49 - r, that is 31 for r = 18.
 *
 * Sources, all from /home/azizz/Yandex.Disk/re-dispak/:
 *   disp70.be:222-225, 976-1008  entry 0500, ВНУПР, saving of the context
 *   disp70.lst:1532-1535, 1610   ОБВНП1 base 03765 and cell 04023
 *   dudka.be:120-131, 143-165 подмена вектора and сч E18; пв ТВ67(М15)
 *   dudka.be:249-294, 298-322    ВХОД, РЕГ '34' x6, РЕГ '240', D10155/56
 *
 * What is NOT proved: the format of the 16-bit БЕНСОН elements.  The decoder
 * in besm6_gp_decoder.c follows the [аналог NASA, БЕНСОН-501] convention.
 */
#include <stdlib.h>

#include "besm6_defs.h"
#include "besm6_gp.h"
#include "besm6_gp_decoder.h"

/* Tyurin 1985, ch. 2.4: ЭК 050 is the user-mode interface to the system.
 * The dpkg stand handles 01210/01211 inside e50() without a supervisor
 * check.  BESM6_NITI_USER opts into the same behaviour for GP subfunctions;
 * by default the guest operating system receives ЭК 050 normally. */
int
gp_user_compat_enabled (void)
{
        static int v = -1;
        if (v < 0)
                v = getenv ("BESM6_NITI_USER") != NULL;
        return v;
}

int
gp_extra_compat_addr (int addr)
{
        switch (addr & 07777) {
        case 01210: case 01211: case 01215: case 01216: case 01217:
                return 1;
        default:
                return 0;
        }
}

#define DBG_REG         001     /* register and ЭК accesses */
#define DBG_IRQ         002     /* 18Р entry and vector dispatch */
#define DBG_FRAME       004     /* 64-word frame hand-off */

/* Machine state of the plotter. */
static int      gp_enabled;             /* ТВ67 / ТВ67А, shadow of MGRP 18 */
static int      gp_ready;               /* mask words written through РЕГ '34' */
static int      gp_phase;               /* D10156 */
static unsigned gp_word_count;          /* D10155, words in the current page */
static unsigned gp_read_idx;            /* next word returned by РЕГ '240' */
static uint64_t gp_buf[GP_WORDS];       /* current 64-word frame */
static t_value  gp_config[16];          /* words accepted by РЕГ '34' */
static unsigned gp_config_count;

static uint32   gp_irqs;                /* 18Р entries taken */
static uint32   gp_frames;              /* frames handed to the decoder */
static uint32   gp_words;               /* data words accepted */
static uint32   gp_mask_loads;          /* six-word РЕГ '34' sequences */
static int      gp_plot_keep;           /* set gp plot=on */
static int      gp_finished;            /* benson_finish() already called */

/* Guest context preserved exactly in the order of ВНУПР. */
static uint32   gp_rk, gp_m17, gp_m16, gp_m15, gp_m20;
static t_value  gp_rmr;

t_stat gp_reset (DEVICE *dptr);
t_stat gp_attach (UNIT *uptr, CONST char *cptr);
t_stat gp_detach (UNIT *uptr);

UNIT gp_unit = { UDATA (NULL, UNIT_ATTABLE, 0) };

REG gp_reg[] = {
    { ORDATA (ENABLED, gp_enabled, 8) },
    { ORDATA (READY,   gp_ready,   8) },
    { ORDATA (PHASE,   gp_phase,   8) },
    { ORDATA (WCOUNT,  gp_word_count, 16) },
    { ORDATA (FRAMES,  gp_frames,  32) },
    { ORDATA (IRQS,    gp_irqs,    32) },
    { ORDATA (WORDS,   gp_words,   32) },
    { ORDATA (MASKS,   gp_mask_loads, 32) },
    { ORDATA (R48_RK,  gp_rk,      15) },
    { ORDATA (R48_M17, gp_m17,     15) },
    { ORDATA (R48_M16, gp_m16,     15) },
    { ORDATA (R48_M15, gp_m15,     15) },
    { ORDATA (R48_M20, gp_m20,     15) },
    { ORDATA (R48_RMR, gp_rmr,     48) },
    { BRDATA (BUF, gp_buf, 8, 48, GP_WORDS) },
    { 0 }
};

DEBTAB gp_deb[] = {
    { "REG",   DBG_REG,   "0172/0173, РЕГ 034/240 and ЭК 012xx accesses" },
    { "IRQ",   DBG_IRQ,   "18Р entry, saved context and vector dispatch" },
    { "FRAME", DBG_FRAME, "64-word frame passed to the decoder" },
    { NULL, 0 }
};

/* Flush the decoder once: rename calcomp.svg to benson.svg and drop the
 * intermediate calcomp.out.  SIMH has no device shutdown hook, so an atexit
 * handler registered on the first frame covers simulator exit as well. */
static void gp_finish_output (void)
{
    if (gp_frames != 0 && ! gp_finished) {
        gp_finished = 1;
        benson_finish ();
    }
}

static void gp_atexit (void)
{
    gp_finish_output ();
}

static t_stat gp_set_tv67 (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    gp_enabled = 1;
    MGRP |= MGRP_GP;
    return SCPE_OK;
}

static t_stat gp_set_tv67a (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    gp_enabled = 0;
    MGRP &= ~MGRP_GP;
    return SCPE_OK;
}

static t_stat gp_show_tv67 (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "%s", gp_enabled ? "on" : "off");
    return SCPE_OK;
}

static t_stat gp_set_plot (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    gp_plot_keep = 1;
    benson_set_keep (1);
    return SCPE_OK;
}

static t_stat gp_set_noplot (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    gp_plot_keep = 0;
    benson_set_keep (0);
    return SCPE_OK;
}

static t_stat gp_show_plot (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
    fprintf (st, "%s", gp_plot_keep ? "on" : "off");
    return SCPE_OK;
}

MTAB gp_mod[] = {
    { MTAB_XTD|MTAB_VDV, 0, "TV67", "TV67", &gp_set_tv67, &gp_show_tv67, NULL,
      "Enable ГРП bit 18 (internal interrupt from БЕНСОН)" },
    { MTAB_XTD|MTAB_VDV, 0, NULL, "TV67A", &gp_set_tv67a, NULL, NULL,
      "Disable ГРП bit 18" },
    { MTAB_XTD|MTAB_VDV, 0, "PLOT", "PLOT", &gp_set_plot, &gp_show_plot, NULL,
      "Keep benson.out along with benson.svg [аналог NASA, БЕНСОН-501]" },
    { MTAB_XTD|MTAB_VDV, 0, NULL, "NOPLOT", &gp_set_noplot, NULL, NULL,
      "Do not keep benson.out" },
    { 0 }
};

DEVICE gp_dev = {
    "GP", &gp_unit, gp_reg, gp_mod,
    1, 8, 5, 1, 8, 48,
    NULL, NULL, &gp_reset, NULL, &gp_attach, &gp_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, gp_deb
};

t_stat gp_reset (DEVICE *dptr)
{
    gp_finish_output ();
    gp_enabled = gp_ready = gp_phase = 0;
    gp_word_count = gp_read_idx = 0;
    gp_config_count = 0;
    gp_irqs = gp_frames = gp_words = gp_mask_loads = 0;
    gp_finished = 0;
    gp_rk = gp_m17 = gp_m16 = gp_m15 = gp_m20 = 0;
    gp_rmr = 0;
    memset (gp_buf, 0, sizeof (gp_buf));
    memset (gp_config, 0, sizeof (gp_config));
    GRP &= ~GRP_GP;
    MGRP &= ~MGRP_GP;
    return SCPE_OK;
}

t_stat gp_attach (UNIT *uptr, CONST char *cptr)
{
    return attach_unit (uptr, cptr);
}

t_stat gp_detach (UNIT *uptr)
{
    gp_finish_output ();
    return detach_unit (uptr);
}

/* Write the raw frame stream to the attached file, when there is one. */
static void gp_raw_write (const uint64_t *words, unsigned nwords)
{
    FILE *f = gp_unit.fileref;

    if (f != NULL && fwrite (words, sizeof (*words), nwords, f) != nwords)
        sim_debug (DBG_FRAME, &gp_dev, "gp0: short write\n");
}

/* Hand exactly one 64-word frame to the decoder. */
static void gp_deliver_frame (void)
{
    unsigned i;

    for (i = gp_word_count; i < GP_WORDS; ++i)
        gp_buf[i] = 0;
    gp_raw_write (gp_buf, GP_WORDS);
    if (gp_frames == 0)
        atexit (gp_atexit);
    ++gp_frames;
    sim_debug (DBG_FRAME, &gp_dev, "frame %u: %u words, decoder rc=%d\n",
               gp_frames, GP_WORDS, benson_frame (gp_buf, GP_WORDS));
    gp_word_count = 0;
    gp_read_idx = 0;
}

/* Raise the real internal interrupt line.  MGRP gates it in the CPU loop. */
static void gp_raise_irq (void)
{
    GRP |= GRP_GP;
}

void gp_save_context (void)
{
    /* ВНУПР: РК, И17, РМР, И16, И15, И20. */
    gp_rk  = RK;
    gp_m17 = M[017];
    gp_rmr = RMR;
    gp_m16 = M[016];
    gp_m15 = M[015];
    gp_m20 = M[020];
}

void gp_interrupt (void)
{
    t_value vector;

    gp_save_context ();
    op_int_1 ("БЕНСОН: ГРП разряд 18");
    GRP &= ~GRP_GP;                 /* ГАШРП1 для 18Р */
    ++gp_irqs;

    vector = mmu_load (GP_VECTOR_CELL) & BITS(15);
    if (vector != 0) {
        /* пб обвнп1-1(М16) with M16 = 49 - 18 = 31. */
        PC = vector;
        sim_debug (DBG_IRQ, &gp_dev, "18Р: PC <- M[04023] = %05o, "
                   "RK=%05o M17=%05o M16=%05o M15=%05o M20=%05o RMR=%08llo\n",
                   PC, gp_rk, gp_m17, gp_m16, gp_m15, gp_m20,
                   (unsigned long long) gp_rmr);
    } else {
        sim_debug (DBG_IRQ, &gp_dev, "18Р: PC <- 0500 (vector cell is zero), "
                   "RK=%05o M17=%05o M16=%05o M15=%05o M20=%05o\n",
                   gp_rk, gp_m17, gp_m16, gp_m15, gp_m20);
    }
}

/* РЕГ '34': six words in ВХОД, nine in the initial mask load.
 * rukava.be:404,536 says these are ПРП/ГРП masks, not device commands. */
void gp_config_write (t_value acc)
{
    gp_config[gp_config_count % 16] = acc;
    ++gp_config_count;
    if (gp_config_count % 6 == 0) {
        gp_ready = 1;
        ++gp_mask_loads;
    }
    sim_debug (DBG_REG, &gp_dev, "РЕГ '34' word %u = %016llo%s\n",
               gp_config_count, (unsigned long long) acc,
               gp_ready ? " (ready)" : "");
}

/* РЕГ '240': the guest reads the next word that arrived from БЕНСОН. */
t_value gp_data_read (void)
{
    t_value val = (t_value) gp_buf[gp_read_idx % GP_WORDS];

    ++gp_read_idx;
    gp_phase ^= 1;                  /* D10156 toggles between words */
    sim_debug (DBG_REG, &gp_dev, "РЕГ '240' word %u = %016llo\n",
               gp_read_idx, (unsigned long long) val);
    return val;
}

/* Short aliases for the CPU dispatcher. */
void gp_write (t_value acc)  { gp_data_write (acc); }
t_value gp_read (void)       { return gp_data_read (); }

/* One data word from the plotter side.  УВВ 0172/0173 is the only guest
 * address that can carry such a word in this simulator. */
void gp_data_write (t_value acc)
{
    gp_buf[gp_word_count % GP_WORDS] = (uint64_t) acc;
    ++gp_word_count;
    ++gp_words;
    gp_phase ^= 1;
    sim_debug (DBG_REG, &gp_dev, "УВВ 0172/0173 word %u = %016llo\n",
               gp_word_count, (unsigned long long) acc);
    gp_raise_irq ();
    if (gp_word_count >= GP_WORDS)
        gp_deliver_frame ();
}

/*
 * ЭК compatibility hooks (the numbers match the dispak stand: extra.c).
 *   01210, 01217  select the НИТИ scheme (argument in the low half)
 *   01211         accept a 64-word frame at memory[ACC & BITS(15)]
 *   01215         subfunction in the left half: 0 = ТВ67, 1 = ТВ67А
 *   01216         vector cell: left half 0 reads, 1 writes, 2 clears
 * The NASA opcode names are [аналог NASA, БЕНСОН-501], not NITI protocol.
 */
void gp_extra (int addr, t_value acc, t_value *acc_out)
{
    unsigned sub = (unsigned) ((acc >> 24) & BITS(24));
    unsigned arg = (unsigned) (acc & BITS(24));

    switch (addr) {
    case 01210:
    case 01217:
        gp_ready = 1;
        sim_debug (DBG_REG, &gp_dev, "ЭК %04o: НИТИ scheme %04o selected\n",
                   addr, arg);
        break;
    case 01211: {
        t_addr base = (t_addr) (acc & BITS(15));
        unsigned i;

        for (i = 0; i < GP_WORDS; ++i)
            gp_buf[i] = (uint64_t) mmu_load ((base + i) & BITS(15));
        gp_word_count = GP_WORDS;
        gp_read_idx = 0;
        gp_words += GP_WORDS;
        sim_debug (DBG_REG, &gp_dev,
                   "ЭК 01211: frame from M[%05o..%05o]\n",
                   base, (base + GP_WORDS - 1) & BITS(15));
        gp_raise_irq ();
        gp_deliver_frame ();
        break;
    }
    case 01215:
        switch (sub) {
        case 0:                 /* ТВ67 */
            gp_enabled = 1;
            MGRP |= MGRP_GP;
            sim_debug (DBG_IRQ, &gp_dev, "ЭК 01215: ТВ67, MGRP bit 18 set\n");
            break;
        case 1:                 /* ТВ67А */
            gp_enabled = 0;
            MGRP &= ~MGRP_GP;
            sim_debug (DBG_IRQ, &gp_dev, "ЭК 01215: ТВ67А, MGRP bit 18 clear\n");
            break;
        case 2:                 /* test hook: raise 18Р without data */
            gp_raise_irq ();
            sim_debug (DBG_IRQ, &gp_dev, "ЭК 01215: 18Р raised (test hook)\n");
            break;
        default:
            sim_debug (DBG_REG, &gp_dev, "ЭК 01215: unknown sub %u\n", sub);
            break;
        }
        break;
    case 01216:
        if (sub == 0) {
            if (acc_out != NULL)
                *acc_out = (t_value) (mmu_load (GP_VECTOR_CELL) & BITS(15));
            sim_debug (DBG_REG, &gp_dev, "ЭК 01216: vector = %05o\n", arg);
        } else if (sub == 1) {
            mmu_store (GP_VECTOR_CELL, (t_value) (arg & BITS(15)));
            sim_debug (DBG_REG, &gp_dev, "ЭК 01216: vector <- %05o\n", arg);
        } else if (sub == 2) {
            mmu_store (GP_VECTOR_CELL, 0);
            sim_debug (DBG_REG, &gp_dev, "ЭК 01216: vector cleared\n");
        }
        break;
    default:
        sim_debug (DBG_REG, &gp_dev, "GP: unknown ЭК %04o\n", addr);
        break;
    }
}

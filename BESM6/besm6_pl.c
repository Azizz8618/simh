/*
 * besm6_punch.c: BESM-6 punchtape output
 *
 * Copyright (c) 2020, Leonid Broukhis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * SERGE VAKULENKO OR LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.

 * Except as contained in this notice, the name of Leonid Broukhis or
 * Serge Vakulenko shall not be used in advertising or otherwise to promote
 * the sale, use or other dealings in this Software without prior written
 * authorization from Leonid Broukhis and Serge Vakulenko.
 */
#include "besm6_defs.h"
#include "besm6_rpg.h"
#include <stdlib.h>
#include <string.h>

t_stat pl_event (UNIT *u);

UNIT pl_unit [] = {
    { UDATA (pl_event, UNIT_SEQ+UNIT_ATTABLE, 0) },
    { UDATA (pl_event, UNIT_SEQ+UNIT_ATTABLE, 0) },
};

#define PL1_READY 04000
#define PL2_READY 02000

#define SET_RDY2(x)      do READY2 |= x; while (0)
#define CLR_RDY2(x)      do READY2 &= ~(x); while (0)
#define ISSET_RDY2(x)    ((READY2 & (x)) != 0)
#define ISCLR_RDY2(x)    ((READY2 & (x)) == 0)

#define PL_RATE         (int)(12.5*MSEC)

unsigned char PL[2];

t_stat pl_reset (DEVICE *dptr);
t_stat pl_attach (UNIT *uptr, CONST char *cptr);
t_stat pl_detach (UNIT *uptr);

static t_stat pl_set_rpg (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_enable (1);
    return SCPE_OK;
}

static t_stat pl_set_norpg (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_finish ();
    rpg_enable (0);
    return SCPE_OK;
}

static t_stat pl_show_rpg (FILE *st, UNIT *u, int32 v, CONST void *d)
{
    fprintf (st, "%s, %u УК, %u strokes, %s",
             rpg_is_enabled () ? "on" : "off", rpg_uk_count (),
             rpg_stroke_count (), rpg_jam () ? "jam" : "no jam");
    return SCPE_OK;
}

static t_stat pl_set_rpg_wide (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_set_wide (1);
    return SCPE_OK;
}

static t_stat pl_set_rpg_normal (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_set_wide (0);
    return SCPE_OK;
}

static t_stat pl_show_rpg_wide (FILE *st, UNIT *u, int32 v, CONST void *d)
{
    fprintf (st, "%s", rpg_is_wide () ? "68 cm" : "34 cm");
    return SCPE_OK;
}

static t_stat pl_set_rpg_6colour (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_set_sixcolor (1);
    return SCPE_OK;
}

static t_stat pl_set_rpg_3colour (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_set_sixcolor (0);
    return SCPE_OK;
}

static t_stat pl_show_rpg_6colour (FILE *st, UNIT *u, int32 v, CONST void *d)
{
    fprintf (st, "%s", rpg_is_sixcolor () ? "6 (hypothesis)" : "3");
    return SCPE_OK;
}

static t_stat pl_set_rpg_step (UNIT *u, int32 v, CONST char *cp, void *d)
{
    if (cp == NULL)
        return SCPE_ARG;
    if (strcmp (cp, "0.0625") == 0 || strcmp (cp, "625") == 0)
        rpg_set_fine_step (1);
    else if (strcmp (cp, "0.125") == 0 || strcmp (cp, "125") == 0)
        rpg_set_fine_step (0);
    else
        return SCPE_ARG;
    return SCPE_OK;
}

static t_stat pl_show_rpg_step (FILE *st, UNIT *u, int32 v, CONST void *d)
{
    fprintf (st, "%s mm", rpg_is_fine_step () ? "0.0625" : "0.125");
    return SCPE_OK;
}

static t_stat pl_set_rpg_autoproll (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_set_autoproll (1);
    return SCPE_OK;
}

static t_stat pl_set_rpg_noautoproll (UNIT *u, int32 v, CONST char *cp, void *d)
{
    rpg_set_autoproll (0);
    return SCPE_OK;
}

static t_stat pl_show_rpg_autoproll (FILE *st, UNIT *u, int32 v, CONST void *d)
{
    fprintf (st, "%s", rpg_has_autoproll () ? "on" : "off");
    return SCPE_OK;
}

/* Test hook: feed octal УК bytes straight into the decoder from an ini. */
static t_stat pl_set_rpg_uk (UNIT *u, int32 v, CONST char *cp, void *d)
{
    char *p, *end;

    if (! rpg_is_enabled () || cp == NULL)
        return SCPE_ARG;
    p = (char *) cp;
    while (*p == '"' || *p == '\'')
        p++;
    while (*p) {
        unsigned long b = strtoul (p, &end, 8);
        if (end == p || b > 0377)
            return SCPE_ARG;
        rpg_uk ((uint8_t) b);
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t' ||
               *p == '"' || *p == '\'')
            p++;
    }
    return SCPE_OK;
}

MTAB pl_mod[] = {
    /* MATCH_CMD() matches a typed prefix, so the short names come first. */
    { MTAB_XTD|MTAB_VUN, 0, "RPG", "RPG", &pl_set_rpg,
      &pl_show_rpg, NULL, "Route 0170/0171 bytes to the RПГ decoder" },
    { MTAB_XTD|MTAB_VUN, 0, NULL, "NORPG", &pl_set_norpg,
      NULL, NULL, "Send 0170/0171 bytes to the PL file again" },
    { MTAB_XTD|MTAB_VUN, 0, NULL, "RPG-WIDE", &pl_set_rpg_wide,
      &pl_show_rpg_wide, NULL, "RПГ: 68 cm roll (5440 steps)" },
    { MTAB_XTD|MTAB_VUN, 0, NULL, "RPG-NORMAL", &pl_set_rpg_normal,
      NULL, NULL, "RПГ: 34 cm roll (2720 steps)" },
    { MTAB_XTD|MTAB_VUN, 0, NULL, "RPG-6COLOR", &pl_set_rpg_6colour,
      &pl_show_rpg_6colour, NULL, "RПГ: six-colour mode (mapping is a hypothesis)" },
    { MTAB_XTD|MTAB_VUN, 0, NULL, "RPG-3COLOR", &pl_set_rpg_3colour,
      NULL, NULL, "RПГ: three-colour mode" },
    { MTAB_XTD|MTAB_VUN|MTAB_NC, 0, "RPG-STEP", "RPG-STEP",
      &pl_set_rpg_step, &pl_show_rpg_step, NULL,
      "RПГ: step 0.125 or 0.0625 mm (rpg-step=0.0625)" },
    { MTAB_XTD|MTAB_VUN, 0, NULL, "RPG-AUTOPROLL", &pl_set_rpg_autoproll,
      &pl_show_rpg_autoproll, NULL, "RПГ: auto paper feed at the end" },
    { MTAB_XTD|MTAB_VUN, 0, NULL, "RPG-NOAUTOPROLL",
      &pl_set_rpg_noautoproll, NULL, NULL, "RПГ: no auto paper feed" },
    { MTAB_XTD|MTAB_VUN|MTAB_QUOTE, 0, NULL, "RPG-UK", &pl_set_rpg_uk,
      NULL, NULL, "RПГ: feed octal УК bytes to the decoder (test hook)" },
    { 0 }
};

DEVICE pl_dev = {
    "PL", pl_unit, NULL, pl_mod,
    2, 8, 19, 1, 8, 50,
    NULL, NULL, &pl_reset, NULL, &pl_attach, &pl_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, rpg_deb
};

t_stat pl_reset (DEVICE *dptr)
{
    rpg_finish ();
    rpg_reset ();
    sim_cancel (&pl_unit[0]);
    sim_cancel (&pl_unit[1]);
    CLR_RDY2(PL1_READY | PL2_READY);
    if (pl_unit[0].flags & UNIT_ATT) {
        SET_RDY2(PL1_READY);
    }
    if (pl_unit[1].flags & UNIT_ATT) {
        SET_RDY2(PL2_READY);
    }
    if (pl_dev.dctrl)
        besm6_debug("reset READY2 := %08o", READY2);
    return SCPE_OK;
}

t_stat pl_attach (UNIT *u, CONST char *cptr)
{
    t_stat s;
    int num = u - pl_unit;
    s = attach_unit (u, cptr);
    if (s != SCPE_OK)
        return s;
    SET_RDY2(PL1_READY >> num);
    if (pl_dev.dctrl)
        besm6_debug("attach READY2 := %08o", READY2);
    return SCPE_OK;
}

t_stat pl_detach (UNIT *u)
{
    int num = u - pl_unit;
    if (num == 1 && rpg_is_enabled ())
        rpg_finish ();
    CLR_RDY2(PL1_READY >> num);
    if (pl_dev.dctrl)
        besm6_debug("detach READY2 := %08o", READY2);
    return detach_unit (u);
}

void pl_control (int num, uint32 cmd)
{
    UNIT *u = &pl_unit[num];
    FILE *f = u->fileref;

    if (! ISSET_RDY2(PL1_READY >> num)) {
        if (pl_dev.dctrl)
            besm6_debug("<<< PL80-%d not ready", num);
        return;
    }
    /* РПГ-1 is PL1 (0171), PRP 15, not the pl0 punchtape channel. */
    if (rpg_is_enabled () && num == 1)
        rpg_uk ((uint8_t) (cmd & 0xff));
    else
        putc(cmd & 0xff, f);
    PRP &= ~(PRP_PTAPE1_PUNCH >> num);
    CLR_RDY2(PL1_READY >> num);
    sim_activate_after(u, PL_RATE);
    if (pl_dev.dctrl) {
        besm6_debug("PL%d: punching %03o", num, cmd & 0xff);
        besm6_debug("punch READY2 := %08o", READY2);
    }
}

unsigned char unicode_to_gost (unsigned short val);

/*
 * The UPP code is the GOST 10859 code with odd parity.
 * UPP stood for "unit for preparation of punchards".
 */
static unsigned char unicode_to_upp (unsigned short ch) {
    unsigned char ret;
    ch = ret = unicode_to_gost (ch);
    ch = (ch & 0x55) + ((ch >> 1) & 0x55);
    ch = (ch & 0x33) + ((ch >> 2) & 0x33);
    ch = (ch & 0x0F) + ((ch >> 4) & 0x0F);
    return (ch & 1) ? ret : ret | 0x80;
}

t_stat pl_event (UNIT *u)
{
    int num = u - pl_unit;
    PRP |= PRP_PTAPE1_PUNCH >> num;
    SET_RDY2(PL1_READY >> num);
    if (pl_dev.dctrl) {
        besm6_debug("PL%d event, READY2 := %08o", num, READY2);
    }
    return SCPE_OK;
}

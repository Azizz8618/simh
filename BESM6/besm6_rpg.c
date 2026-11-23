/*
 * besm6_rpg.c: wide-roll six-colour RPLG (РПГ ВНИИЭФ) on the PL channel
 *
 * Historical parameters from BESM6/doc/ГП.md and gp1.be/gp4.be:
 *   step 0.125 mm (СМ = 80 steps/cm), fine step 0.0625 mm (ЦЕНАШ);
 *   roll width 34 cm = 2720 steps or 68 cm = 5440 steps (ШИРБУМ);
 *   pen byte 011 vvvvv: v5 sign of DY, v4 sign of DX, v2v1 pen 1..3,
 *   00 lifts the pen; command classes 0/4/7 carry no information.
 * GP1 actually emits the step bytes of ГП.md:246-253 (bit 8 set, step
 * bits 4..1, pen field in bits 7..5), so both encodings are accepted:
 * a 011 byte selects the frame decoder, otherwise the PL byte decoder is
 * used.  The exact six-colour bit mapping is not recovered from the
 * sources; the extra v3 bit of a frame pen byte is used as the upper
 * three-colour bank (1..3 -> 4..6).  That is a model hypothesis.
 * SVG colours 1 black, 2 red, 3 green, 4 blue, 5 magenta, 6 cyan are our
 * convention, not a historical property of the plotter.
 */
#include <stdlib.h>
#include <string.h>

#include "besm6_defs.h"
#include "besm6_rpg.h"

#define RPG_MM_NORMAL   340
#define RPG_MM_WIDE     680
#define RPG_STEPS_PER_MM        8
#define RPG_STEPS_PER_MM_FINE   16
#define RPG_AUTOPROLL_TAIL      1240    /* 15.5 cm of paper after the plot */

struct rpg_segment {
    long x1, y1, x2, y2;
    int colour;
};

static int      rpg_on;
static int      rpg_wide;
static int      rpg_sixcolour;
static int      rpg_fine;
static int      rpg_autoproll;
static int      rpg_jammed;
static long     rpg_x, rpg_y;
static long     rpg_max_x;
static int      rpg_pen;
static int      rpg_frame_mode;
static unsigned rpg_dx_lo, rpg_dx_hi, rpg_dy_lo, rpg_dy_hi;
static uint32   rpg_uk_total;
static uint32   rpg_pen_bytes;
static uint32   rpg_step_bytes;
static uint32   rpg_skipped;
static struct rpg_segment *rpg_segments;
static size_t   rpg_nsegments;
static size_t   rpg_segcap;
static int      rpg_finished;
static int      rpg_atexit_registered;

DEBTAB rpg_deb[] = {
    { "UK",   DBG_RPG_UK,   "УК bytes received and decoded" },
    { "GEOM", DBG_RPG_GEOM, "pen, steps and roll limits" },
    { "SVG",  DBG_RPG_SVG,  "rpg.svg output" },
    { NULL, 0, NULL }
};

static const char *rpg_colours[7] = {
    NULL, "black", "red", "green", "blue", "magenta", "cyan"
};

static long rpg_y_limit (void)
{
    long mm = rpg_wide ? RPG_MM_WIDE : RPG_MM_NORMAL;
    return mm * (rpg_fine ? RPG_STEPS_PER_MM_FINE : RPG_STEPS_PER_MM);
}

static double rpg_mm_per_step (void)
{
    return rpg_fine ? 0.0625 : 0.125;
}

static int rpg_sign (long v)
{
    return (v > 0) - (v < 0);
}

/* A stroke is extended while the pen keeps moving the same way. */
static void rpg_line (long dx, long dy)
{
    long nx, ny;
    struct rpg_segment *s;

    nx = rpg_x + dx;
    ny = rpg_y + dy;
    if (ny < 0 || ny > rpg_y_limit()) {
        rpg_jammed = 1;
        if (ny < 0)
            ny = 0;
        else
            ny = rpg_y_limit();
    }
    if (nx > rpg_max_x)
        rpg_max_x = nx;

    if (rpg_pen && (dx || dy)) {
        if (rpg_nsegments) {
            s = &rpg_segments[rpg_nsegments - 1];
            if (s->colour == rpg_pen && s->x2 == rpg_x && s->y2 == rpg_y &&
                rpg_sign(s->x2 - s->x1) == rpg_sign(dx) &&
                rpg_sign(s->y2 - s->y1) == rpg_sign(dy)) {
                s->x2 = nx;
                s->y2 = ny;
                rpg_x = nx;
                rpg_y = ny;
                return;
            }
        }
        if (rpg_nsegments == rpg_segcap) {
            size_t cap = rpg_segcap ? rpg_segcap * 2 : 256;
            struct rpg_segment *p = realloc (rpg_segments,
                                             cap * sizeof (*p));
            if (p == NULL) {
                rpg_jammed = 1;
                return;
            }
            rpg_segments = p;
            rpg_segcap = cap;
        }
        s = &rpg_segments[rpg_nsegments++];
        s->x1 = rpg_x;
        s->y1 = rpg_y;
        s->x2 = nx;
        s->y2 = ny;
        s->colour = rpg_pen;
        if (pl_dev.dctrl & DBG_RPG_GEOM)
            sim_debug (DBG_RPG_GEOM, &pl_dev,
                       "RPG stroke %u: (%ld,%ld)-(%ld,%ld) pen %d\n",
                       (unsigned) rpg_nsegments, s->x1, s->y1,
                       s->x2, s->y2, s->colour);
    }
    rpg_x = nx;
    rpg_y = ny;
}

/* 011 vvvvv: v5 is sign DY, v4 is sign DX, v3 extends the pen bank for
 * six colours (hypothesis), v2v1 is the pen number, 00 lifts the pen. */
static void rpg_frame_pen (unsigned v)
{
    long dx, dy;
    unsigned pen = v & 3;

    dx = rpg_dx_lo | (rpg_dx_hi << 5);
    dy = rpg_dy_lo | (rpg_dy_hi << 5);
    if (v & 0x08)                   /* v4: DX < 0 */
        dx = -dx;
    if (v & 0x10)                   /* v5: DY < 0 */
        dy = -dy;

    if (pen == 0)
        rpg_pen = 0;
    else if (rpg_sixcolour && (v & 0x04))
        rpg_pen = (int) (pen + 3);  /* unproven six-colour mapping */
    else
        rpg_pen = (int) pen;

    rpg_line (dx, dy);
    rpg_dx_lo = rpg_dx_hi = rpg_dy_lo = rpg_dy_hi = 0;
    rpg_pen_bytes++;
    if (pl_dev.dctrl & DBG_RPG_GEOM)
        sim_debug (DBG_RPG_GEOM, &pl_dev,
                   "RPG pen byte: pen=%d dX=%ld dY=%ld\n",
                   rpg_pen, dx, dy);
}

/* The byte GP1 puts on 0170: bit 8 is always one, bit 7 means pen up,
 * bits 6..5 select the pen, bit 4 is -Y, bit 3 +Y, bit 2 -X, bit 1 +X. */
static void rpg_step_byte (uint8_t b)
{
    unsigned pen = (b >> 5) & 3;
    long dx = 0, dy = 0;

    if ((b & 0x40) || pen == 0)
        rpg_pen = 0;
    else
        rpg_pen = (int) pen;        /* six-colour mapping here is unknown */

    if (b & 0x01) dx++;
    if (b & 0x02) dx--;
    if (b & 0x04) dy++;
    if (b & 0x08) dy--;
    rpg_line (dx, dy);
    rpg_step_bytes++;
}

void rpg_uk (uint8_t byte)
{
    unsigned cls = byte >> 5;
    unsigned v = byte & 0x1f;
    const char *what = "skip";

    rpg_uk_total++;
    /* Classes 1 and 2 (001/010) cannot be PL step bytes, which always
     * have bit 8 set, so they identify a frame before its pen byte. */
    if (cls == 1 || cls == 2 || cls == 3)
        rpg_frame_mode = 1;
    if (cls == 3) {
        rpg_frame_pen (v);
        what = "pen";
    } else if (rpg_frame_mode) {
        switch (cls) {
        case 1: rpg_dx_lo = v; what = "dX lo"; break;
        case 5: rpg_dx_hi = v; what = "dX hi"; break;
        case 2: rpg_dy_lo = v; rpg_dx_lo = rpg_dx_hi = 0;
                rpg_dy_hi = 0; what = "dY lo"; break;
        case 6: rpg_dy_hi = v; what = "dY hi"; break;
        default: rpg_skipped++; break;
        }
    } else if (byte & 0x80) {
        rpg_step_byte (byte);
        what = "step";
    } else {
        rpg_skipped++;
    }

    if (pl_dev.dctrl & DBG_RPG_UK)
        sim_debug (DBG_RPG_UK, &pl_dev,
                   "RPG UK %u: %03o class %u v %02o %s "
                   "(x=%ld y=%ld pen=%d)\n",
                   rpg_uk_total, (unsigned) byte, cls, v, what,
                   rpg_x, rpg_y, rpg_pen);
}

uint32 rpg_jam (void)
{
    return rpg_jammed ? 01 : 0;
}

static void rpg_write_svg (void)
{
    FILE *f;
    size_t i;
    long top = rpg_y_limit();
    double mm = rpg_mm_per_step();

    f = fopen ("rpg.svg", "w");
    if (f == NULL) {
        sim_printf ("RPG: cannot create rpg.svg\n");
        return;
    }
    fprintf (f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf (f, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
             "width=\"%gmm\" height=\"%gmm\" viewBox=\"0 0 %ld %ld\">\n",
             (double) (rpg_wide ? RPG_MM_WIDE : RPG_MM_NORMAL),
             (rpg_max_x * mm > 0 ? rpg_max_x * mm + 10 : 10),
             (rpg_max_x > 0 ? rpg_max_x + 80 : 80), top);
    fprintf (f, "<!-- step %g mm; roll width %d mm; six-colour %s -->\n",
             mm, rpg_wide ? RPG_MM_WIDE : RPG_MM_NORMAL,
             rpg_sixcolour ? "hypothesis" : "off");
    fprintf (f, "<rect width=\"100%%\" height=\"100%%\" fill=\"white\"/>\n");
    for (i = 0; i < rpg_nsegments; i++) {
        struct rpg_segment *s = &rpg_segments[i];
        const char *colour = (s->colour >= 1 && s->colour <= 6) ?
                             rpg_colours[s->colour] : "black";
        fprintf (f, "<path d=\"M %ld %ld L %ld %ld\" fill=\"none\" "
                 "stroke=\"%s\" stroke-width=\"0.3\"/>\n",
                 s->x1, top - s->y1, s->x2, top - s->y2, colour);
    }
    fprintf (f, "</svg>\n");
    fclose (f);
    if (pl_dev.dctrl & DBG_RPG_SVG)
        sim_debug (DBG_RPG_SVG, &pl_dev,
                   "RPG: wrote rpg.svg, %u УК, %u strokes\n",
                   rpg_uk_total, (unsigned) rpg_nsegments);
}

void rpg_finish (void)
{
    if (rpg_finished || rpg_uk_total == 0)
        return;
    rpg_finished = 1;
    if (rpg_autoproll) {
        rpg_pen = 0;
        rpg_line (RPG_AUTOPROLL_TAIL, 0);
    }
    rpg_write_svg ();
}

static void rpg_atexit (void)
{
    rpg_finish ();
}

void rpg_reset (void)
{
    rpg_finish ();
    rpg_on = rpg_wide = rpg_sixcolour = 0;
    rpg_fine = rpg_autoproll = rpg_jammed = 0;
    rpg_frame_mode = 0;
    rpg_x = rpg_y = rpg_max_x = 0;
    rpg_pen = 0;
    rpg_dx_lo = rpg_dx_hi = rpg_dy_lo = rpg_dy_hi = 0;
    rpg_uk_total = rpg_pen_bytes = rpg_step_bytes = rpg_skipped = 0;
    free (rpg_segments);
    rpg_segments = NULL;
    rpg_segcap = 0;
    rpg_nsegments = 0;
    rpg_finished = 0;
}

void rpg_enable (int on)
{
    rpg_on = on ? 1 : 0;
    if (rpg_on && ! rpg_atexit_registered) {
        atexit (rpg_atexit);
        rpg_atexit_registered = 1;
    }
}

void rpg_set_wide (int on)
{
    rpg_wide = on ? 1 : 0;
}

void rpg_set_sixcolor (int on)
{
    rpg_sixcolour = on ? 1 : 0;
}

void rpg_set_fine_step (int on)
{
    rpg_fine = on ? 1 : 0;
}

void rpg_set_autoproll (int on)
{
    rpg_autoproll = on ? 1 : 0;
}

void rpg_set_mode (int enable, int wide, int sixcolour, int fine,
                   int autoproll)
{
    rpg_enable (enable);
    rpg_set_wide (wide);
    rpg_set_sixcolor (sixcolour);
    rpg_set_fine_step (fine);
    rpg_set_autoproll (autoproll);
}

int rpg_is_enabled (void)     { return rpg_on; }
uint32 rpg_uk_count (void)    { return rpg_uk_total; }
uint32 rpg_stroke_count (void) { return (uint32) rpg_nsegments; }
int rpg_is_wide (void)        { return rpg_wide; }
int rpg_is_sixcolor (void)    { return rpg_sixcolour; }
int rpg_is_fine_step (void)   { return rpg_fine; }
int rpg_has_autoproll (void)  { return rpg_autoproll; }

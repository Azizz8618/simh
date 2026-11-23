/*
 * БЕНСОН-501 frame decoder for the new ГП device.
 *
 * Runtime strings are deliberately identical to the dispak analogue so the
 * golden C1-line output is reproduced byte for byte.  This file is written
 * from scratch: it implements the documented frame contract and command
 * table, it does not contain any code from dispak/dispak/extra.c.
 *
 * Status: [аналог NASA, БЕНСОН-501].  NASA TM-76182 describes a BENSON-FRANCE
 * 501 plotter attached to an Электроника-1001.  No 16-bit НИТИ command format
 * for that plotter has been found, so nothing here claims to reproduce a
 * historical БЭСМ-6 wire protocol.  The SVG is produced by the separate
 * MIT-licensed plotter.cpp renderer through its C bridge.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "besm6_gp_decoder.h"
#include "plotter_bridge.h"

#define BEN_LIMIT_X  5000
#define BEN_LIMIT_Y  7500
#define BEN_FRAME_WORDS 64

/* Table 2, p.7 of NASA TM-76182: external-device command codes. */
#define BENSON_Y_PLUS   06501
#define BENSON_X_PLUS   06502
#define BENSON_Y_MINUS  06504
#define BENSON_PEN_UP   06511
#define BENSON_PEN_DOWN 06512
#define BENSON_X_MINUS  06514

struct bench {
        int x, y;
        int pen_down;
        int step_x, step_y;
        int limit_x, limit_y;
        int steps;
};

static struct bench ben = { 0, 0, 0, 1, 1, BEN_LIMIT_X, BEN_LIMIT_Y, 0 };

static FILE *plot_file;
static FILE *benson_out;
static int keep = -1;
static int debug_flag = -1;

static int
benson_keep(void)
{
        if (keep < 0)
                keep = getenv("BESM6_DUBNA_PLOT_KEEP") != NULL;
        return keep;
}

static int
benson_debug(void)
{
        if (debug_flag < 0)
                debug_flag = getenv("BESM6_NITI_DEBUG") != NULL;
        return debug_flag;
}

void
benson_set_keep(int on)
{
        keep = on != 0;
}

void
benson_set_debug(int on)
{
        debug_flag = on != 0;
}

/*
 * The renderer lives in plotter_bridge.cpp (C++).  Weak no-op fallbacks keep
 * this C decoder linkable in builds that have not added those C++ sources:
 * a strong definition from the bridge always wins at link time.  No plotter
 * logic is duplicated here.
 */
__attribute__((weak)) void
dubna_plot_put(int device, unsigned char byte)
{
        (void) device;
        (void) byte;
}

__attribute__((weak)) void
dubna_plot_finish(int keep_raw)
{
        (void) keep_raw;
}

static void
benson_log(const char *fmt, ...)
{
        va_list ap;

        fputs("[аналог NASA, БЕНСОН-501] ", stderr);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
}

/* Low 24 bits of a FOREX INTEGER word, sign-extended from bit 23. */
static int
du_coord_value(uint64_t w)
{
        uint32_t v = (uint32_t)(w & 0xffffffu);

        if (v & 0x800000u)
                v |= 0xff000000u;
        return (int)(int32_t)v;
}

static void
bench_move(const char *dir, int dx, int dy)
{
        int nx = ben.x + dx * ben.step_x;
        int ny = ben.y + dy * ben.step_y;

        if (nx < 0 || nx > ben.limit_x) {
                benson_log("BENSON limit X (move %s)", dir);
                return;
        }
        if (ny < 0 || ny > ben.limit_y) {
                benson_log("BENSON limit Y (move %s)", dir);
                return;
        }
        ben.x = nx;
        ben.y = ny;
        ++ben.steps;
}

/* Internal conversion to the byte stream of the proven plotter.cpp backend. */
static unsigned char
benson_render_byte(unsigned code)
{
        switch (code) {
        case BENSON_X_PLUS:   return 02;
        case BENSON_X_MINUS:  return 01;
        case BENSON_Y_PLUS:   return 020;
        case BENSON_Y_MINUS:  return 040;
        case BENSON_PEN_UP:   return 04;
        case BENSON_PEN_DOWN: return 010;
        default:              return 0;
        }
}

static void
benson_emit(unsigned code, const char *name, int dx, int dy, int pen_event)
{
        if (pen_event)
                ben.pen_down = pen_event > 0;
        else
                bench_move(name, dx, dy);

        benson_log("BENSON command %03o (%s) X=%05d Y=%05d",
                   code, name, ben.x, ben.y);
        if (benson_keep()) {
                if (!benson_out)
                        benson_out = fopen("benson.out", "w");
                if (benson_out) {
                        fprintf(benson_out, "%03o %s X=%05d Y=%05d "
                                "[аналог NASA, БЕНСОН-501]\n",
                                code, name, ben.x, ben.y);
                        fflush(benson_out);
                }
        }
        dubna_plot_put(0, benson_render_byte(code));
}

/* One pen event, then all X, then all Y. */
static void
benson_step(int dx, int dy, int pen_event)
{
        if (pen_event < 0)
                benson_emit(BENSON_PEN_UP, "UP", 0, 0, -1);
        else if (pen_event > 0)
                benson_emit(BENSON_PEN_DOWN, "DOWN", 0, 0, 1);
        if (dx > 0)
                benson_emit(BENSON_X_PLUS, "+X", 1, 0, 0);
        else if (dx < 0)
                benson_emit(BENSON_X_MINUS, "-X", -1, 0, 0);
        if (dy > 0)
                benson_emit(BENSON_Y_PLUS, "+Y", 0, 1, 0);
        else if (dy < 0)
                benson_emit(BENSON_Y_MINUS, "-Y", 0, -1, 0);
}

int
benson_frame(const uint64_t words[64], unsigned nwords)
{
        unsigned i;
        int j, tx, ty, pen;

        if (!words || nwords < 3 || nwords > BEN_FRAME_WORDS) {
                benson_log("BENSON bad frame: words=%u", nwords);
                return -1;
        }
        if (!plot_file) {
                plot_file = fopen("plot.dat", "w");
                if (!plot_file) {
                        benson_log("cannot open plot.dat");
                        return -1;
                }
        }

        /* plot.dat is the byte-for-byte frame dump: 8 six-bit octal groups
         * per 48-bit word, 64 words per frame. */
        for (i = 0; i < nwords; ++i) {
                uint64_t v = words[i];

                for (j = 7; j >= 0; --j)
                        fprintf(plot_file, "%02o ", (unsigned)((v >> (j * 6)) & 077));
                fputc('\n', plot_file);
        }
        fflush(plot_file);

        tx = du_coord_value(words[0]);
        ty = du_coord_value(words[1]);
        pen = du_coord_value(words[2]) ? 1 : 0;
        if (tx < 0 || tx > ben.limit_x || ty < 0 || ty > ben.limit_y) {
                benson_log("BENSON coords out of range X=%d Y=%d", tx, ty);
                return -1;
        }
        for (i = 3; i < nwords; ++i)
                if (words[i] && benson_debug())
                        benson_log("BENSON nonzero pad word=%u", i);

        if (pen != ben.pen_down)
                benson_step(0, 0, pen ? 1 : -1);
        while (ben.x < tx)
                benson_step(1, 0, 0);
        while (ben.x > tx)
                benson_step(-1, 0, 0);
        while (ben.y < ty)
                benson_step(0, 1, 0);
        while (ben.y > ty)
                benson_step(0, -1, 0);
        benson_step(0, 0, -1);
        /* plotter.cpp flushes a path only when a move arrives with the pen
         * up.  Step out and back with the pen up so the stroke reaches the
         * SVG; the final position is restored. */
        benson_step(1, 0, 0);
        benson_step(-1, 0, 0);
        return 0;
}

void
benson_finish(void)
{
        if (benson_out) {
                fclose(benson_out);
                benson_out = NULL;
        }
        if (plot_file) {
                fclose(plot_file);
                plot_file = NULL;
        }
        benson_log("BENSON finish X=%d Y=%d pen=%s steps=%d",
                   ben.x, ben.y, ben.pen_down ? "DOWN" : "UP", ben.steps);
        dubna_plot_finish(0);
        if (rename("calcomp.svg", "benson.svg") != 0 && errno != ENOENT)
                benson_log("cannot rename calcomp.svg to benson.svg");
        remove("calcomp.out");
}

#ifdef BESM6_GP_DECODER_SELFTEST
/* Offline regression: two C1-line frames, (10,10) then (110,110).
 * The FOREX INTEGER tag 0xd00000 occupies the high 24 bits of every word. */
int
main(void)
{
        static const uint64_t tag = (uint64_t)064 << 42;
        uint64_t frame[64];
        unsigned i;

        benson_set_keep(1);
        benson_set_debug(0);
        for (i = 3; i < 64; ++i)
                frame[i] = tag;

        frame[0] = frame[1] = tag | 012;    /* X = Y = 10 */
        frame[2] = tag | 01;                /* pen down */
        if (benson_frame(frame, 64) != 0)
                return 1;
        frame[0] = frame[1] = tag | 0156;   /* X = Y = 110 */
        if (benson_frame(frame, 64) != 0)
                return 1;
        benson_finish();
        fprintf(stderr, "PASS: BENSON decoder self-test\n");
        return 0;
}
#endif /* BESM6_GP_DECODER_SELFTEST */

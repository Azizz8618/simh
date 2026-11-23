/*
 * Minimal C bridge to the proven Dubna plotter back end (plotter.cpp/.h).
 *
 * The back end itself is MIT code by Serge Vakulenko; see the license notice
 * in plotter.h.  This bridge only forwards bytes to the existing
 * calcomp_putch() / tektronix_putch() / watanabe_putch() / change_page() /
 * finish() methods.  No plotter logic is duplicated here.
 *
 * The bytes are supplied by an analogue decoder in extra.c, so every stream
 * fed through this bridge is [аналог], not a proven БЕНСОН/НИТИ protocol.
 */
#ifndef DUBNA_PLOTTER_BRIDGE_H
#define DUBNA_PLOTTER_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* device: 0 = Calcomp (default), 1 = Tektronix, 2 = Watanabe. */
void dubna_plot_put(int device, unsigned char byte);

/* Finish the current page and start a new one. */
void dubna_plot_page(void);

/* Save all pending output; keep_raw != 0 also writes the raw *.out file. */
void dubna_plot_finish(int keep_raw);

#ifdef __cplusplus
}
#endif

#endif /* DUBNA_PLOTTER_BRIDGE_H */

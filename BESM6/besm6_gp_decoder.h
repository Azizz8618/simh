/*
 * БЕНСОН-501 frame decoder for the new ГП device.
 *
 * This is an analogue of the NASA TM-76182 command set, not a proven
 * НИТИ/БЭСМ-6 protocol.  Every decoded line is marked
 * [аналог NASA, БЕНСОН-501].  The byte-level renderer lives in the separate
 * MIT-licensed plotter.cpp / plotter_bridge.cpp files.
 */
#ifndef BESM6_GP_DECODER_H
#define BESM6_GP_DECODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode one 64-word frame.  Returns 0 on success, nonzero on a bad frame. */
int benson_frame(const uint64_t words[64], unsigned nwords);

/* Write benson.out when on != 0.  BESM6_DUBNA_PLOT_KEEP is the default. */
void benson_set_keep(int on);

/* Debug logging of nonzero pad words and rejected coordinates. */
void benson_set_debug(int on);

/* Flush the renderer, produce benson.svg and remove intermediate files. */
void benson_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* BESM6_GP_DECODER_H */

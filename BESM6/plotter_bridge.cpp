/*
 * Minimal C bridge to the proven Dubna plotter back end.
 * See the MIT license notice in plotter.h for the copied back end.
 */
#include "plotter_bridge.h"

#include "plotter.h"

#define DUBNA_DEVICE_CALCOMP   0
#define DUBNA_DEVICE_TEKTRONIX 1
#define DUBNA_DEVICE_WATANABE  2

/* One process, one plotter: a dispak run handles one batch of tasks. */
static Plotter dubna_plotter;

extern "C" void
dubna_plot_put(int device, unsigned char byte)
{
        switch (device) {
        case DUBNA_DEVICE_TEKTRONIX:
                dubna_plotter.tektronix_putch((char) byte);
                break;
        case DUBNA_DEVICE_WATANABE:
                dubna_plotter.watanabe_putch((char) byte);
                break;
        case DUBNA_DEVICE_CALCOMP:
        default:
                dubna_plotter.calcomp_putch((char) byte);
                break;
        }
}

extern "C" void
dubna_plot_page(void)
{
        dubna_plotter.change_page(false);
}

extern "C" void
dubna_plot_finish(int keep_raw)
{
        dubna_plotter.finish(keep_raw != 0);
}

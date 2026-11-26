/* e10025_bus_time.c: виртуальная шкала времени канала в наносекундах книги 4.

   См. e10025_bus_time.h: это виртуальное время книги 4, а не наносекунды
   хоста, не часы SIMH и не утверждение точности 1998 года. Все метки
   вычисляются из фиксированных смещений, без чтения host-таймера.
*/
#include "e10025_bus_time.h"

#include <stdio.h>
#include <string.h>

static const char *const bt_names[BT_NPHASE] = {
    "ADDRESS", "SA", "WAIT_SACK", "STROBE", "RELEASE", "CLEANUP", "IDLE"
    };

/* Смещение фазы от начала цикла. 150 ns на переходе АДР -> СА, затем по
   75 ns. Сумма: 150 + 5*75 = 525 ns полного цикла; CLEANUP = 450 ns
   что соответствует интервалу повтора книги 4. */
static const uint32_t bt_offset_ns[BT_NPHASE] = {
    0,                                                  /* ADDRESS */
    BT_NS_ADDRESS,                                      /* SA, 150 */
    BT_NS_ADDRESS + BT_NS_STEP,                         /* WAIT_SACK, 225 */
    BT_NS_ADDRESS + 2 * BT_NS_STEP,                     /* STROBE, 300 */
    BT_NS_ADDRESS + 3 * BT_NS_STEP,                     /* RELEASE, 375 */
    BT_NS_ADDRESS + 4 * BT_NS_STEP,                     /* CLEANUP, 450 */
    BT_NS_ADDRESS + 5 * BT_NS_STEP                      /* IDLE, 525 */
    };

void bt_reset (BT_TIME *bt)
{
if (bt == NULL)
    return;
memset (bt, 0, sizeof (*bt));
bt->phase = -1;
}

void bt_cycle_start (BT_TIME *bt)
{
int i;

if (bt == NULL)
    return;
bt->now_ns = 0;
bt->phase = BT_PH_ADDRESS;
bt->started = 1;
for (i = 0; i < BT_NPHASE; i++)
    bt->t_ns[i] = 0;
}

uint32_t bt_advance (BT_TIME *bt, int phase)
{
if (bt == NULL)
    return 0;
if ((phase < 0) || (phase >= BT_NPHASE))
    return bt->now_ns;                                  /* ERROR не имеет метки книги 4 */
if (!bt->started)
    bt_cycle_start (bt);
bt->now_ns = bt_offset_ns[phase];
bt->t_ns[phase] = bt->now_ns;
bt->phase = phase;
if (phase == BT_PH_IDLE)
    bt->cycles++;                                       /* полный цикл завершён */
return bt->now_ns;
}

uint32_t bt_phase_ns (const BT_TIME *bt, int phase)
{
if ((bt == NULL) || (phase < 0) || (phase >= BT_NPHASE))
    return 0;
return bt->t_ns[phase];
}

uint32_t bt_delta_ns (const BT_TIME *bt, int from, int to)
{
if ((bt == NULL) || (from < 0) || (from >= BT_NPHASE) ||
    (to < 0) || (to >= BT_NPHASE) || (to < from))
    return 0;
return bt->t_ns[to] - bt->t_ns[from];
}

void bt_timeout (BT_TIME *bt)
{
if (bt == NULL)
    return;
bt->now_ns = BT_NS_TIMEOUT;                             /* 15000 book-ns, не host-время */
bt->timeouts++;
bt->started = 0;
}

const char *bt_phase_name (int phase)
{
if ((phase < 0) || (phase >= BT_NPHASE))
    return "ERROR";
return bt_names[phase];
}

size_t bt_debug_line (const BT_TIME *bt, char *buf, size_t len)
{
int n;

if ((buf == NULL) || (len == 0))
    return 0;
if (bt == NULL) {
    n = snprintf (buf, len, "t_ns=0 phase=ERROR cycles=0 timeouts=0");
    return (n > 0 && (size_t) n < len)? (size_t) n: len - 1;
    }
n = snprintf (buf, len, "t_ns=%u phase=%s cycles=%u timeouts=%u",
              bt->now_ns, bt_phase_name (bt->phase),
              bt->cycles, bt->timeouts);
return (n > 0 && (size_t) n < len)? (size_t) n: len - 1;
}

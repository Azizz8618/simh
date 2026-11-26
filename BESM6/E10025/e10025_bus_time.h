/* e10025_bus_time.h: виртуальные метки времени единого канала в наносекундах
   книги 4 (ИЧМ2.791.003, книга 4, разделы 6.2-6.6).

   ВАЖНО. Это виртуальное время книги 4. Это НЕ наносекунды хоста, НЕ часы
   SIMH и НЕ такты процессора. Никакой точности 1998 года модуль не заявляет:
   исторические длительности воспроизводятся как детерминированные числа.
   Здесь нет nanosleep, busy loop и обращений к host-таймеру. Объявленный
   масштаб совпадает с TIMING=scaled: 1 такт = 75 ns (BT_TICK_NS).

   Метка отсчитывается от начала цикла. Переход АДР -> СА занимает 150 ns,
   последующие переходы по 75 ns. Полный цикл до фазы IDLE равен 525 ns,
   метка CLEANUP равна 450 ns (повтор), ожидание без СП фиксируется на
   15000 ns. Порядок вызовов bt_advance не влияет на метку: каждая фаза
   имеет фиксированное виртуальное смещение от начала цикла.

   Интеграция в e10025_chan.c: завести static BT_TIME, вызывать bt_reset в
   chan_reset, bt_cycle_start в начале chan_run_cycle, bt_advance на каждой
   фазе CH_PHASE, bt_timeout в chan_timeout_service и bt_debug_line в
   debug-журнале. Точные строки интеграции в .localdocs/E10025-BUS-B2-EVIDENCE.txt.
*/
#ifndef E10025_BUS_TIME_H
#define E10025_BUS_TIME_H

#include <stddef.h>
#include <stdint.h>

#define BT_NPHASE       7                               /* семь фаз книги 4 */

/* Фазы единого канала. Числа совпадают с CH_IDLE..CH_CLEANUP в e10025_chan.c,
   но определены здесь независимо, чтобы модуль можно было проверять отдельно. */
enum {
    BT_PH_ADDRESS = 0,
    BT_PH_SA = 1,
    BT_PH_WAIT_SACK = 2,
    BT_PH_STROBE = 3,
    BT_PH_RELEASE = 4,
    BT_PH_CLEANUP = 5,
    BT_PH_IDLE = 6
    };

/* Исторические длительности книги 4 в наносекундах. Виртуальные значения,
   не host-время и не обещание точностных характеристик 1998 года. */
#define BT_NS_ADDRESS   150                             /* АДР -> СА */
#define BT_NS_STEP      75                              /* СА -> СП -> строб -> ... */
#define BT_NS_FULL      525                             /* полный цикл до IDLE */
#define BT_NS_REPEAT    450                             /* повтор, метка CLEANUP */
#define BT_NS_TIMEOUT   15000                           /* нет СП, 15 мкс */
#define BT_TICK_NS      75                              /* 1 такт = 75 ns */

typedef struct {
    uint32_t t_ns[BT_NPHASE];                           /* метки семи фаз */
    uint32_t now_ns;                                    /* текущая метка */
    uint32_t cycles;                                    /* завершённые циклы */
    uint32_t timeouts;                                  /* зафиксированные таймауты */
    int phase;                                          /* последняя фаза, -1 до старта */
    int started;                                        /* цикл начат */
    } BT_TIME;

void bt_reset (BT_TIME *bt);
void bt_cycle_start (BT_TIME *bt);
uint32_t bt_advance (BT_TIME *bt, int phase);
uint32_t bt_phase_ns (const BT_TIME *bt, int phase);
uint32_t bt_delta_ns (const BT_TIME *bt, int from, int to);
void bt_timeout (BT_TIME *bt);
const char *bt_phase_name (int phase);
size_t bt_debug_line (const BT_TIME *bt, char *buf, size_t len);

#endif

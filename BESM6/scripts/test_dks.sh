#!/bin/bash
# test_dks.sh — Тест ДКС (обновлено 2026-09-05 по итогам сессии)
# Цепочка: telnet 4203 → tty11 (DKS) → dks_register → kadopam_mem → ПРП6/12
# Целевой исход: ПРП6 → свядкс (032-чтение РКС≠0) → МПРП≠0 (cmd_033)
#              → E24 в ШГ → загруз (konfus.be:870) → подкачка СВЯЗЬ7
#
# Использование: ./scripts/test_dks.sh [--no-connect]
#   (--no-connect — не открывать telnet-клиент автоматически)

set -u
PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
DEBUG_LOG="$PROJECT_ROOT/debug.txt"
PID_FILE="/tmp/besm6.pid"
RUN_LOG="/tmp/besm6_run.log"
FIFO="/tmp/besm6_in"
PORT=4203

# 1. Проверка бинарника
[ -f "$PROJECT_ROOT/../BIN/besm6" ] || { echo "✗ Нет эмулятора. ./scripts/build.sh"; exit 1; }

# 2. Остановка ��режнего экземпляра (в т.ч. демона FIFO)
./scripts/stop_emulator.sh 2>/dev/null
pkill -f 'sleep infinity' 2>/dev/null
rm -f "$FIFO" "$DEBUG_LOG" "$RUN_LOG"

# 3. Запуск через FIFO (управляемый ввод, как в /tmp/dks_daemon.sh)
cd "$PROJECT_ROOT"
mkfifo "$FIFO"
setsid bash -c 'exec 3>/tmp/besm6_in; sleep infinity' < /dev/null > /dev/null 2>&1 &
setsid ../BIN/besm6 dispak.ini < "$FIFO" > "$RUN_LOG" 2>&1 &
echo $! > "$PID_FILE"
echo "✓ Эмулятор запущен, PID $(cat $PID_FILE), лог: $RUN_LOG"

# 4. Ожидание регистрации ДКС (не фиксированный sleep, а поллинг)
# ОС Диспак грузится 1-3 мин до первых 032-чтений: длинный таймаут,
# редкие проверки (sleep 10) — экономия токенов на пустых запросах.
echo -n "Ожидание «DKS: terminal registered»... "
for i in $(seq 1 60); do
    grep -q 'DKS: terminal registered' "$DEBUG_LOG" 2>/dev/null && break
    sleep 10
done
grep -q 'DKS: terminal registered' "$DEBUG_LOG" 2>/dev/null \
    && echo "✓" || echo "✗ (см. $RUN_LOG)"

# 5. Автоподключение telnet (если nc доступен и не указано --no-connect)
if [ "${1:-}" != "--no-connect" ] && command -v nc >/dev/null; then
    (sleep 2; printf 'A\r' ; sleep 2) | nc localhost $PORT > /dev/null 2>&1 &
    echo "✓ telnet-клиент отправлен на порт $PORT (tty11, DKS)"
fi

echo ""
echo "Проверка контрольных точек (1 раз): ./scripts/check_dks.sh"
echo "Наблюдение:                        ./scripts/monitor_dks.sh"
echo "Остановка:                         ./scripts/stop_emulator.sh"

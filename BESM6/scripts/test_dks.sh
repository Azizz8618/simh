#!/bin/bash
# test_dks.sh — Тестирование ДКС терминала
# Использование: ./scripts/test_dks.sh

set -e

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
DEBUG_LOG="$PROJECT_ROOT/debug.txt"
PID_FILE="/tmp/besm6.pid"

echo "=== Тестирование ДКС терминала ==="
echo ""

# Проверка существования эмулятора
if [ ! -f "$PROJECT_ROOT/../BIN/besm6" ]; then
    echo "✗ Эмулятор не найден. Запустите ./scripts/build.sh"
    exit 1
fi

# Остановка предыдущего экземпляра
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
        echo "Остановка предыдущего экземпляра (PID: $OLD_PID)..."
        kill "$OLD_PID" 2>/dev/null || true
        sleep 1
    fi
    rm -f "$PID_FILE"
fi

# Очистка старых логов
rm -f "$DEBUG_LOG"
rm -f "$PROJECT_ROOT/log.txt"
rm -f "$PROJECT_ROOT/output.txt"

cd "$PROJECT_ROOT"

echo "Запуск эмулятора..."
../BIN/besm6 dispak.ini &
BESM6_PID=$!
echo $BESM6_PID > "$PID_FILE"

echo "PID эмулятора: $BESM6_PID"
echo ""
echo "Ожидание загрузки ОС Диспак..."
sleep 5

echo ""
echo "=== Информация для подключения ==="
echo "Порт telnet:    4199"
echo "Команда:        telnet localhost 4199"
echo ""
echo "Мониторинг ДКС:"
echo "  tail -f $DEBUG_LOG | grep -iE '(DKS|KDP|terminal)'"
echo ""
echo "Остановка эмулятора:"
echo "  kill $BESM6_PID"
echo "  # или:"
echo "  kill \$(cat $PID_FILE)"
echo ""
echo "✓ Эмулятор запущен. PID сохранён в $PID_FILE"

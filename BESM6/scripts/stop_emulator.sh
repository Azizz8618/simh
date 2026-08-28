#!/bin/bash
# stop_emulator.sh — Остановка эмулятора БЭСМ-6
# Использование: ./scripts/stop_emulator.sh

PID_FILE="/tmp/besm6.pid"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        echo "Остановка эмулятора (PID: $PID)..."
        kill "$PID" 2>/dev/null
        sleep 1
        if kill -0 "$PID" 2>/dev/null; then
            echo "Принудительное завершение..."
            kill -9 "$PID" 2>/dev/null || true
        fi
        echo "✓ Эмулятор остановлен"
    else
        echo "Эмулятор не запущен (PID $PID не активен)"
    fi
    rm -f "$PID_FILE"
else
    echo "PID-файл не найден"
    # Попытка найти процесс по имени
    PIDS=$(pgrep -f "BIN/besm6 dispak.ini" 2>/dev/null)
    if [ -n "$PIDS" ]; then
        echo "Найдены процессы эмулятора: $PIDS"
        echo "Завершение..."
        kill $PIDS 2>/dev/null || true
        echo "✓ Процессы завершены"
    else
        echo "Запущенные эмуляторы не найдены"
    fi
fi

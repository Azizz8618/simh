#!/bin/bash
# stop_emulator.sh — Остановка эмулятора БЭСМ-6 + очистка логов (правило Л2)
#
# Использование: ./scripts/stop_emulator.sh [--keep-logs]
#   --keep-logs — не удалять логи (для анализа после остановки)

SESSION="besm6"
PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
OSZAGR="$PROJECT_ROOT/OSZAGR"
KEEP_LOGS=0

if [ "${1:-}" = "--keep-logs" ]; then
    KEEP_LOGS=1
fi

# --- 1. Остановка эмулятора ---
FOUND=0

# tmux-сессия
if tmux has-session -t "$SESSION" 2>/dev/null; then
    echo "Отправляю quit в tmux-сессию $SESSION..."
    tmux send-keys -t "$SESSION" "quit" Enter
    sleep 2
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    FOUND=1
fi

# Процессы besm6 (на всякий случай)
PIDS=$(pgrep -f 'BIN/besm6\|besm6.*dispak' 2>/dev/null)
if [ -n "$PIDS" ]; then
    echo "Завершаю процессы: $PIDS"
    kill $PIDS 2>/dev/null || true
    sleep 1
    kill -9 $PIDS 2>/dev/null || true
    FOUND=1
fi

if [ "$FOUND" -eq 1 ]; then
    echo "✓ Эмулятор остановлен"
else
    echo "Эмулятор не был запущен"
fi

# --- 2. Очистка логов (правило Л2) ---
if [ "$KEEP_LOGS" -eq 0 ]; then
    rm -f "$PROJECT_ROOT/debug.txt" "$PROJECT_ROOT/log.txt" \
          "$OSZAGR/debug.txt" "$OSZAGR/log.txt" \
          "$OSZAGR/logs/debug_"*.log \
          "$OSZAGR/1[0-9]"
    echo "✓ Логи очищены"
else
    echo "Логи сохранены (--keep-logs)"
fi

# --- 3. Проверка ---
RUNNING=$(pgrep -c -f besm6 2>/dev/null || echo 0)
if [ "$RUNNING" -gt 0 ]; then
    echo "⚠ ВНИМАНИЕ: $RUNNING процесс(ов) besm6 ещё работают!"
    pgrep -af besm6
else
    echo "✓ Процессов besm6 не осталось"
fi

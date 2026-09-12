#!/bin/bash
# === Сценарий выполнения теста Т2 (test_sis_k71.txt) ===
# Порядок: §10.5 ВВОД_АНАЛИЗ.md + direction КРК=1 ДО ввода
#
# Использование:
#   ./scripts/run_t2.sh            — полный прогон (ОС уже загружена)
#   ./scripts/run_t2.sh --fresh    — с нуля (kill + clean + boot + test)

set -e
BESM6_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OSZAGR="$BESM6_DIR/OSZAGR"
UVU="$BESM6_DIR/UVU"
TEST_FILE="$BESM6_DIR/VVOD/test_sis_k71.txt"
SESSION="besm6"
DIRECTION_TR1="100000000"   # ТР1: direction КРК=1 (бит 24)
TR4_VALUE="040000000"       # ТР4: Е24Р=1 (непрерывный режим)

log() { echo "[$(date +%H:%M:%S)] $*"; }

# --- Fresh start ---
if [[ "$1" == "--fresh" ]]; then
    log "Останавливаю эмулятор..."
    pkill -9 -f besm6 2>/dev/null || true
    sleep 2
    rm -rf "$OSZAGR/logs/" "$OSZAGR/debug.txt" "$OSZAGR/log.txt"
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    sleep 1

    log "Запускаю эмулятор..."
    tmux new-session -d -s "$SESSION" \
        "cd $OSZAGR && $BESM6_DIR/../BIN/besm6 dispak.ini"
    log "Жду загрузку ОС (40 сек)..."
    sleep 30
fi

# --- Проверка: ОС загружена? ---
if ! tmux has-session -t "$SESSION" 2>/dev/null; then
    log "Сессия $SESSION не найдена — запускаю эмулятор..."
    rm -rf "$OSZAGR/logs/" "$OSZAGR/debug.txt" "$OSZAGR/log.txt"
    tmux new-session -d -s "$SESSION" \
        "cd $OSZAGR && $BESM6_DIR/../BIN/besm6 dispak.ini"
    log "Жду загрузку ОС (40 сек)..."
    sleep 30
fi

# --- Step 0: Ctrl+G → sim> ---
log "Step 0: Останавливаю CPU (Ctrl+G)..."
tmux send-keys -t "$SESSION" C-g
sleep 2

# --- Цикл ввода: повторяем до появления В0 ---
MAX_RETRIES=3
for attempt in $(seq 1 $MAX_RETRIES); do
    log "--- Попытка ввода #$attempt ---"

    # Step 1: Подключение теста
    log "Step 1: Подключаю тест..."
    tmux send-keys -t "$SESSION" "do $UVU/test_kont_dks.ini" Enter
    sleep 15

    # Проверяем результат
    CAPTURE=$(tmux capture-pane -t "$SESSION" -p)

    if echo "$CAPTURE" | grep -q 'CБB0'; then
        log "CБB0 — ошибка ввода, повторяю..."
        tmux send-keys -t "$SESSION" Enter
		sleep 2
        tmux send-keys -t "$SESSION" Enter
		sleep 2
        tmux send-keys -t "$SESSION" C-g
        sleep 10
        continue
    fi

    if echo "$CAPTURE" | grep -q 'B07[0-9]-'; then
        log "B0 — тест принят!"
        break
    fi

    log "Неожиданный вывод, повторяю..."
    tmux send-keys -t "$SESSION" C-g
    sleep 10
done

# --- Step 4: ТР1 — direction КРК=1 ---
log "Step 4: ТР1 = $DIRECTION_TR1 (direction КРК=1)..."
tmux send-keys -t "$SESSION" C-g
sleep 2
#tmux send-keys -t "$SESSION" "d 1 $DIRECTION_TR1" Enter
sleep 1

# --- Step 5: ТР4 — Е24Р=1 ---
log "Step 5: ТР4 = $TR4_VALUE (Е24Р=1)..."
tmux send-keys -t "$SESSION" "d 4 $TR4_VALUE" Enter
sleep 1

# --- Step 6: go ---
log "Step 6: go..."
tmux send-keys -t "$SESSION" "go" Enter
# --- Результат ---
log "=== Состояние эмулятора ==="
tmux capture-pane -t "$SESSION" -p | tail -15
log "=== Лог ДКС (последние 10 строк) ==="
tail -10 "$OSZAGR/logs/debug_dks.log" 2>/dev/null || echo "(нет лога)"
log "=== Готово ==="
# --- Step 7: Ожидание + выброс из решения ---
log "Step 7: Жду 40 сек, затем ВЫБРОС..."
sleep 40
tmux send-keys -t "$SESSION" C-g
sleep 2
tmux send-keys -t "$SESSION" "d 1 140000000" Enter
sleep 1
tmux send-keys -t "$SESSION" "go" Enter
sleep 10

# --- Результат ---
log "=== Состояние эмулятора ==="
tmux capture-pane -t "$SESSION" -p | tail -15
log "=== Лог ДКС (последние 10 строк) ==="
tail -10 "$OSZAGR/logs/debug_dks.log" 2>/dev/null || echo "(нет лога)"
log "=== Готово ==="

#!/bin/bash
# === Сценарий снятия и анализа трассы ОС ===
# Используется после `set pult=NN` (NN>10 → dump_touched → файл NN в OSZAGR/)
# touched.pl расшифровывает дамп → список выполненных команд с адресами модулей
#
# Использование:
#   ./scripts/test_traces.sh                — pult=12, без fresh
#   ./scripts/test_traces.sh --fresh        — kill + clean + boot + trace
#   ./scripts/test_traces.sh --pult 13      — другой pult
#   ./scripts/test_traces.sh --wait 60      — ждать 60 сек перед снятием трассы
#   ./scripts/test_traces.sh --grep СВЯДКС  — после трассы grep по ключевому слову

set -e
BESM6_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OSZAGR="$BESM6_DIR/OSZAGR"
RE_DISPAK="$HOME/Yandex.Disk/re_dispak/re-dispak"
SESSION="besm6"

# --- Параметры по умолчанию ---
PULT_VAL="12"
WAIT_SEC=0
GREP_PATTERN=""

# --- Парсинг аргументов ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fresh)  FRESH=1; shift ;;
        --pult)   PULT_VAL="$2"; shift 2 ;;
        --wait)   WAIT_SEC="$2"; shift 2 ;;
        --grep)   GREP_PATTERN="$2"; shift 2 ;;
        *)        echo "Неизвестный аргумент: $1"; exit 1 ;;
    esac
done

DUMP_FILE="$OSZAGR/$PULT_VAL"
TRACE_FILE="/tmp/tr${PULT_VAL}.txt"

log() { echo "[$(date +%H:%M:%S)] $*"; }

# --- Fresh start ---
if [[ "${FRESH:-0}" == "1" ]]; then
    log "Останавливаю эмулятор..."
    pkill -9 -f besm6 2>/dev/null || true
    sleep 2
    rm -f "$OSZAGR/debug.txt" "$OSZAGR/log.txt" \
          "$OSZAGR/logs/debug_"*.log \
          "$OSZAGR/1[0-9]" \
          /tmp/besm6_in /tmp/besm6_run.log
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    sleep 1

    log "Запускаю эмулятор..."
    mkfifo /tmp/besm6_in 2>/dev/null || true
    tmux new-session -d -s "$SESSION" \
        "cd $OSZAGR && $BESM6_DIR/../BIN/besm6 dispak.ini"
    log "Жду загрузку ОС (30 сек)..."
    sleep 30
fi

# --- Проверка: ОС загружена? ---
if ! tmux has-session -t "$SESSION" 2>/dev/null; then
    log "Сессия $SESSION не найдена — запускаю эмулятор..."
    rm -f "$OSZAGR/debug.txt" "$OSZAGR/log.txt" \
          "$OSZAGR/logs/debug_"*.log \
          "$OSZAGR/1[0-9]"
    mkfifo /tmp/besm6_in 2>/dev/null || true
    tmux new-session -d -s "$SESSION" \
        "cd $OSZAGR && $BESM6_DIR/../BIN/besm6 dispak.ini"
    log "Жду загрузку ОС (30 сек)..."
    sleep 30
fi

# --- Step 0: Ctrl+G → sim> ---
log "Step 0: Останавливаю CPU (Ctrl+G)..."
tmux send-keys -t "$SESSION" C-g
sleep 2

# --- Step 1: set pult=NN → dump_touched ---
log "Step 1: set pult=$PULT_VAL → dump_touched..."
tmux send-keys -t "$SESSION" "set pult=$PULT_VAL" Enter
sleep 3

# Проверка: файл дампа создан?
if [ ! -f "$DUMP_FILE" ]; then
    log "ОШИБКА: файл дампа $DUMP_FILE не создан"
    log "Размеры файлов в OSZAGR:"
    ls -la "$OSZAGR"/1[0-9] 2>/dev/null || log "(нет дампов)"
    exit 1
fi
log "Дамп $DUMP_FILE создан ($(stat -c%s "$DUMP_FILE") байт)"

# --- Step 2: Ожидание (если нужно) ---
if [[ "$WAIT_SEC" -gt 0 ]]; then
    log "Step 2: Жду $WAIT_SEC сек перед снятием трассы..."
    sleep "$WAIT_SEC"
fi

# --- Step 3: touched.pl → расшифровка трассы ---
log "Step 3: Расшифровка трассы через touched.pl..."
if [ ! -x "$RE_DISPAK/touched.pl" ]; then
    log "ОШИБКА: touched.pl не найден или не исполняемый: $RE_DISPAK/touched.pl"
    exit 1
fi

cd "$RE_DISPAK" || exit 1
./touched.pl "$DUMP_FILE" > "$TRACE_FILE" 2>/dev/null

# --- Step 4: Проверки ---
if [ ! -s "$TRACE_FILE" ]; then
    log "ОШИБКА: файл трассы $TRACE_FILE пуст"
    exit 1
fi

TOTAL_COUNT=$(wc -l < "$TRACE_FILE")
UNKNOWN_COUNT=$(grep -c "unknown" "$TRACE_FILE" 2>/dev/null || echo 0)

log "=== Анализ трассы (pult=$PULT_VAL) ==="
log "  Всего строк: $TOTAL_COUNT"
log "  Неопознанных: $UNKNOWN_COUNT"

if [ "$UNKNOWN_COUNT" -gt 0 ]; then
    log "  Внимание: $UNKNOWN_COUNT неопознанных команд"
fi

# --- Step 5: Контрольные адреса (если есть --grep) ---
if [ -n "$GREP_PATTERN" ]; then
    MATCH_COUNT=$(grep -c "$GREP_PATTERN" "$TRACE_FILE" 2>/dev/null || echo 0)
    log "  Совпадений '$GREP_PATTERN': $MATCH_COUNT"
    if [ "$MATCH_COUNT" -gt 0 ]; then
        log "  --- Совпадения ---"
        grep "$GREP_PATTERN" "$TRACE_FILE" | head -20
        if [ "$MATCH_COUNT" -gt 20 ]; then
            log "  ... и ещё $((MATCH_COUNT - 20)) строк"
        fi
    fi
fi

# --- Step 6: Ключевые адреса СВЯЗЬ7 ---
log "=== Ключевые адреса ОС ==="
for sym in СВЯДКС НСТДКС ЗАГРУЗ ТСТДКС УСПРП8; do
    cnt=$(grep -c "$sym" "$TRACE_FILE" 2>/dev/null || echo 0)
    if [ "$cnt" -gt 0 ]; then
        log "  $sym: $cnt попаданий"
    fi
done

# --- Step 7: Первые/последние строки ---
log "=== Первые 5 строк трассы ==="
head -5 "$TRACE_FILE" | while read -r line; do log "  $line"; done
log "=== Последние 5 строк трассы ==="
tail -5 "$TRACE_FILE" | while read -r line; do log "  $line"; done

log "=== Трасса сохранена: $TRACE_FILE ==="
log "=== Готово ==="

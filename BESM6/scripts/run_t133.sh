#!/bin/bash
# === run_t133.sh ===
# Прогон теста t133_min_bemsh.txt (Э50 '133', СВЯЗЬ7)
# Маска 7703 0000 0000 0000: группа 03, направл. КРК=0, без Е24
# КАДОПАМ-обмен (7 команд кк 26) → запрос к РКС ДКС
#
# Использование:
#   ./scripts/run_t133.sh                   — прогон (ОС загружена)
#   ./scripts/run_t133.sh --fresh           — с нуля (kill + boot + test)
#   ./scripts/run_t133.sh --trace           — + снятие трассы после теста
#   ./scripts/run_t133.sh --trace --analyze — + трасса + анализ СВЯЗЬ7
#   ./scripts/run_t133.sh --check           — + проверка 7 точек ДКС
#   ./scripts/run_t133.sh --trace --check   — + трасса + проверка точек
#   ./scripts/run_t133.sh --acpu            — + анализ подвала АЦПУ
#   ./scripts/run_t133.sh --pult 13         — pult=13 вместо 12
#   ./scripts/run_t133.sh --grep СВЯДКС     — grep трассы
#   ./scripts/run_t133.sh --wait-exec 60    — 60 сек вместо 120

set -e
BESM6_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OSZAGR="$BESM6_DIR/OSZAGR"
UVU="$BESM6_DIR/UVU"
RE_DISPAK="$HOME/Yandex.Disk/re_dispak/re-dispak"
TEST_INI="$UVU/t133_min.dubna.ini"
SESSION="besm6"
WAIT_BOOT=30
WAIT_V0=60
WAIT_EXEC=120

FRESH=0; DO_TRACE=0; DO_ANALYZE=0; DO_CHECK=0; DO_ACPU=0
PULT_VAL="12"; GREP_PATTERN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fresh)       FRESH=1; shift ;;
        --trace)       DO_TRACE=1; shift ;;
        --analyze)     DO_ANALYZE=1; shift ;;
        --check)       DO_CHECK=1; shift ;;
        --acpu)        DO_ACPU=1; shift ;;
        --pult)        PULT_VAL="$2"; shift 2 ;;
        --grep)        GREP_PATTERN="$2"; shift 2 ;;
        --wait-exec)   WAIT_EXEC="$2"; shift 2 ;;
        --wait-boot)   WAIT_BOOT="$2"; shift 2 ;;
        *)             echo "Неизвестный аргумент: $1"; exit 1 ;;
    esac
done

DUMP_FILE="$OSZAGR/$PULT_VAL"
TRACE_FILE="/tmp/tr${PULT_VAL}.txt"

log() { echo "[$(date +%H:%M:%S)] $*"; }

# ─── Fresh start ───────────────────────────────────────────────
if [[ "$FRESH" -eq 1 ]]; then
    log "Останавливаю эмулятор..."
    pkill -9 -f besm6 2>/dev/null || true
    sleep 2
    rm -rf "$OSZAGR/logs/" "$OSZAGR/debug.txt" "$OSZAGR/log.txt"
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    sleep 1
    log "Запускаю эмулятор (dispak.ini)..."
    tmux new-session -d -s "$SESSION" \
        "cd $OSZAGR && $BESM6_DIR/../BIN/besm6 dispak.ini"
    log "Жду загрузку ОС (${WAIT_BOOT} сек)..."
    sleep "$WAIT_BOOT"
fi

if ! tmux has-session -t "$SESSION" 2>/dev/null; then
    log "Сессия $SESSION не найдена — запускаю эмулятор..."
    rm -rf "$OSZAGR/logs/" "$OSZAGR/debug.txt" "$OSZAGR/log.txt"
    tmux new-session -d -s "$SESSION" \
        "cd $OSZAGR && $BESM6_DIR/../BIN/besm6 dispak.ini"
    log "Жду загрузку ОС (${WAIT_BOOT} сек)..."
    sleep "$WAIT_BOOT"
fi

# ╔═════════════════════════════════════════════════════════════╗
# ║  ФАЗА 1: ПРОГОН ТЕСТА                                     ║
# ╚═════════════════════════════════════════════════════════════╝

log "Step 0: Останавливаю CPU (Ctrl+G)..."
tmux send-keys -t "$SESSION" C-g
sleep 2

log "Step 1: Подключаю перфокарты: do $TEST_INI"
tmux send-keys -t "$SESSION" "do $TEST_INI"
sleep 2
tmux send-keys -t "$SESSION" Enter
sleep 5

log "Ожидаю В0 (приём задания, ${WAIT_V0} сек)..."
V0_OK=0
for attempt in $(seq 1 $((WAIT_V0 / 5))); do
    CAPTURE=$(tmux capture-pane -t "$SESSION" -p)
    if echo "$CAPTURE" | grep -aqE 'B0[0-9]-|\u04120[0-9]-'; then
        log "  ✓ В0 получено — задание принято"
        V0_OK=1
        break
    fi
    sleep 5
done
if [[ "$V0_OK" -eq 0 ]]; then
    log "  ⚠ В0 не обнаружено за ${WAIT_V0} сек"
    tmux capture-pane -t "$SESSION" -p | tail -10
    log "  Продолжаю..."
fi

log "Step 2: go..."
tmux send-keys -t "$SESSION" "go" Enter
sleep 2

log "Жду исполнение теста (${WAIT_EXEC} сек)..."
sleep "$WAIT_EXEC"

log "Останавливаю CPU (Ctrl+G)..."
tmux send-keys -t "$SESSION" C-g
sleep 2

log "Выбрасываю задание из решения (ТР1 ← 140000000)..."
tmux send-keys -t "$SESSION" "d 1 140000000" Enter
sleep 1
tmux send-keys -t "$SESSION" "go" Enter
sleep 10

# ╔═════════════════════════════════════════════════════════════╗
# ║  ФАЗА 2: СБОР РЕЗУЛЬТАТОВ                                 ║
# ╚═════════════════════════════════════════════════════════════╝

log ""
log "══ РЕЗУЛЬТАТЫ ТЕСТА TMIN (Э50 '133', МАСКА 7703...) ══"
log ""

log "── Состояние терминала ──"
tmux capture-pane -t "$SESSION" -p | tail -20
log ""

log "── output.txt (последние 30 строк) ──"
tail -30 "$OSZAGR/output.txt" 2>/dev/null || echo "(нет output.txt)"
log ""

log "── debug_dks.log (последние 20 строк) ──"
tail -20 "$OSZAGR/logs/debug_dks.log" 2>/dev/null || echo "(нет debug_dks.log)"
log ""

log "── log.txt (хвост, без physobm) ──"
tail -30 "$OSZAGR/log.txt" 2>/dev/null | grep -av 'physobm' || echo "(нет log.txt)"
log ""

log "── Анализ: команды КК 26 (КАДОПАМ) ──"
if [[ -f "$OSZAGR/log.txt" ]]; then
    KDP=$(grep -ac 'kk26.*KDP\|KDP\|kadopam\|cmd_026' "$OSZAGR/log.txt" 2>/dev/null || echo 0)
    PRP=$(grep -ac 'PRP\|\u041f\u0420\u041f\|prp' "$OSZAGR/log.txt" 2>/dev/null || echo 0)
    MPRP=$(grep -ac 'MPRP\|\u041c\u041f\u0420\u041f\|mprp' "$OSZAGR/log.txt" 2>/dev/null || echo 0)
    log "  КАДОПАМ-операций (KDP/kadopam): $KDP"
    log "  ПРП (прерывания):               $PRP"
    log "  МПРП (маска прерываний):        $MPRP"
else
    log "  (нет log.txt для анализа)"
fi
log ""

# ╔═════════════════════════════════════════════════════════════╗
# ║  ФАЗА 2.5: АНАЛИЗ ПОДВАЛА АЦПУ (--acpu)                   ║
# ╚═════════════════════════════════════════════════════════════╝

if [[ "$DO_ACPU" -eq 1 ]]; then
    log "ФАЗА 2.5: АНАЛИЗ ПОДВАЛА АЦПУ (acpu_analyze.pl)"
    log ""

    ACPU_FILE="$BESM6_DIR/ACPU/output.txt"
    ACPU_ANALYZER="$RE_DISPAK/acpu_analyze.pl"

    if [ ! -s "$ACPU_FILE" ]; then
        log "  Файл АЦПУ не найден или пуст: $ACPU_FILE"
    elif [ ! -x "$ACPU_ANALYZER" ]; then
        log "  acpu_analyze.pl не найден: $ACPU_ANALYZER"
    else
        log "-- Сводка --"
        perl "$ACPU_ANALYZER" "$ACPU_FILE" --summary 2>/dev/null | while IFS= read -r line; do log "  $line"; done
        log ""
        log "-- Задания --"
        perl "$ACPU_ANALYZER" "$ACPU_FILE" --jobs 2>/dev/null | while IFS= read -r line; do log "  $line"; done
        log ""
        log "-- Ошибки --"
        perl "$ACPU_ANALYZER" "$ACPU_FILE" --errors 2>/dev/null | while IFS= read -r line; do log "  $line"; done
        log ""
        log "-- Ресурсы --"
        perl "$ACPU_ANALYZER" "$ACPU_FILE" --resources 2>/dev/null | while IFS= read -r line; do log "  $line"; done
        log ""
    fi
fi

# ╔═════════════════════════════════════════════════════════════╗
# ║  ФАЗА 3: СНЯТИЕ ТРАССЫ (--trace)                          ║
# ╚═════════════════════════════════════════════════════════════╝

if [[ "$DO_TRACE" -eq 1 ]]; then
    log "╔══════════════════════════════════════════════════════╗"
    log "║  ФАЗА 3: СНЯТИЕ ТРАССЫ (pult=$PULT_VAL)            ║"
    log "╚══════════════════════════════════════════════════════╝"
    log ""

    log "Step 3.1: Останавливаю CPU (Ctrl+G)..."
    tmux send-keys -t "$SESSION" C-g
    sleep 2

    log "Step 3.2: set pult=$PULT_VAL -> dump_touched..."
    tmux send-keys -t "$SESSION" "set pult=$PULT_VAL" Enter
    sleep 3

    if [ ! -f "$DUMP_FILE" ]; then
        log "  ⚠ Файл дампа $DUMP_FILE не создан"
        log "  Файлы в OSZAGR:"
        ls -la "$OSZAGR"/1[0-9] 2>/dev/null || log "  (нет дампов)"
    else
        DUMP_SIZE=$(stat -c%s "$DUMP_FILE" 2>/dev/null || echo 0)
        log "  ✓ Дамп: $DUMP_FILE ($DUMP_SIZE байт)"
    fi

    log "Step 3.3: Расшифровка трассы (touched.pl)..."
    if [ -x "$RE_DISPAK/touched.pl" ]; then
        cd "$RE_DISPAK" || true
        ./touched.pl "$DUMP_FILE" > "$TRACE_FILE" 2>/dev/null
        if [ -s "$TRACE_FILE" ]; then
            TOTAL_LINES=$(wc -l < "$TRACE_FILE")
            UNKNOWN_COUNT=$(grep -c 'unknown' "$TRACE_FILE" 2>/dev/null || echo 0)
            log "  ✓ Трасса: $TRACE_FILE ($TOTAL_LINES строк, $UNKNOWN_COUNT неопознанных)"
        else
            log "  ⚠ Файл трассы $TRACE_FILE пуст"
        fi
    else
        log "  ✗ touched.pl не найден: $RE_DISPAK/touched.pl"
    fi

    # Анализ трассы через trace_zones.sh (метки по листингам .lst)
    TRACE_ZONES="$BESM6_DIR/scripts/trace_zones.sh"
    if [[ -s "$TRACE_FILE" ]]; then
        log ""
        log "── Анализ трассы по зонам (trace_zones.sh) ──"
        ZONE_ARGS=("--unique")
        [[ -n "$GREP_PATTERN" ]] && ZONE_ARGS+=("--grep" "$GREP_PATTERN")
        if [[ -x "$TRACE_ZONES" ]]; then
            "$TRACE_ZONES" "$TRACE_FILE" "${ZONE_ARGS[@]}" 2>/dev/null | while IFS= read -r line; do log "  $line"; done
        else
            log "  ⚠ trace_zones.sh не найден: $TRACE_ZONES"
            log "  Fallback: grep по сырым адресам"
            grep -oP '\d{4}(?=\s\d{4}[LR])' "$TRACE_FILE" | sort | uniq -c | sort -rn | head -20
        fi
        log ""
    fi

    # Первые/последние строки трассы
    if [[ -s "$TRACE_FILE" ]]; then
        log "── Первые 10 строк трассы ──"
        head -10 "$TRACE_FILE" | while IFS= read -r line; do log "  $line"; done
        log ""
        log "── Последние 10 строк трассы ──"
        tail -10 "$TRACE_FILE" | while IFS= read -r line; do log "  $line"; done
        log ""
    fi

    log "Step 3.7: cont (продолжение выполнения)..."
    tmux send-keys -t "$SESSION" "cont" Enter
    sleep 2
fi
# ╔═════════════════════════════════════════════════════════════╗
# ║  ФАЗА 4: АНАЛИЗ СВЯЗЬ7 (--analyze)                        ║
# ╚═════════════════════════════════════════════════════════════╝

if [[ "$DO_ANALYZE" -eq 1 ]]; then
    log "╔══════════════════════════════════════════════════════╗"
    log "║  ФАЗА 4: АНАЛИЗ СВЯЗЬ7 (analyze_link7.pl)           ║"
    log "╚══════════════════════════════════════════════════════╝"
    log ""

    if [ ! -s "$TRACE_FILE" ]; then
        log "  ✗ Трасса $TRACE_FILE пуста — сначала выполните --trace"
    elif [ ! -x "$RE_DISPAK/analyze_link7.pl" ]; then
        log "  ✗ analyze_link7.pl не найден: $RE_DISPAK/analyze_link7.pl"
        log "  Альтернатива: disbesm_extract.sh + disbesm_verify.sh"
    else
        log "Запуск analyze_link7.pl..."
        cd "$RE_DISPAK" || true
        ./analyze_link7.pl 2>/dev/null
        log ""
    fi
fi

# ╔═════════════════════════════════════════════════════════════╗
# ║  ФАЗА 5: ПРОВЕРКА ТОЧЕК ДКС (--check)                     ║
# ╚═════════════════════════════════════════════════════════════╝

if [[ "$DO_CHECK" -eq 1 ]]; then
    log "╔══════════════════════════════════════════════════════╗"
    log "║  ФАЗА 5: ПРОВЕРКА ТОЧЕК ДКС (check_dks.sh)          ║"
    log "╚══════════════════════════════════════════════════════╝"
    log ""

    CHECK_ARGS=""
    [[ "$DO_TRACE" -eq 1 ]] && CHECK_ARGS="--trace"

    if [ -x "$BESM6_DIR/scripts/check_dks.sh" ]; then
        "$BESM6_DIR/scripts/check_dks.sh" $CHECK_ARGS
    else
        log "  ✗ check_dks.sh не найден: $BESM6_DIR/scripts/check_dks.sh"
    fi
    log ""
fi

log "══ Готово ══"

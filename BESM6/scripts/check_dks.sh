#!/bin/bash
# check_dks.sh — Проверка контрольных точек цепочки ДКС/СВЯЗЬ7
#
# Источник данных: logs/debug_dks.log (per-subsystem, основной),
#                  debug.txt (fallback).
#
# Алгоритм (по образцу Т2 — полная проверка цепочки):
#   1. Регистрация терминала (dks_register)
#   2. ПРП6/ПРП12 при регистрации
#   3. 032-чтение ОС (РКС/статус КРК)
#   4. Слово РКС содержит флаги направления
#   5. Запись МПРП (cmd_033, УСТПРП)
#   6. E24 в ШГ (запрос подкачки СВЯЗЬ7)
#   7. 132-запись ОС (буферы терминалов)
#
# Стыковка с трассами:
#   ./scripts/check_dks.sh            -- проверка (без трассы)
#   ./scripts/check_dks.sh --trace    -- при НЕпрохождении точки: снятие трассы
#
# Использование: ./scripts/check_dks.sh [--trace]

SESSION="besm6"
PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
OSZAGR="$PROJECT_ROOT/OSZAGR"
RE_DISPAK="$HOME/Yandex.Disk/re_dispak/re-dispak"
# Основной источник: per-subsystem лог
DKS_LOG="$OSZAGR/logs/debug_dks.log"
# Fallback
DEBUG_LOG="$PROJECT_ROOT/debug.txt"

TRACE_MODE=0
if [ "${1:-}" = "--trace" ]; then
    TRACE_MODE=1
fi

# --- Проверка наличия логов ---
if [ ! -f "$DKS_LOG" ] && [ ! -f "$DEBUG_LOG" ]; then
    echo "✗ Нет логов (ни $DKS_LOG, ни $DEBUG_LOG)"
    echo "  Запустите: ./scripts/test_dks.sh"
    exit 1
fi

# --- Функция: искать паттерн в логах ---
search_log() {
    local pat="$1"
    # Per-subsystem лог (основной)
    if [ -f "$DKS_LOG" ] && grep -aE "$pat" "$DKS_LOG" >/dev/null 2>&1; then
        return 0
    fi
    # Fallback debug.txt
    if [ -f "$DEBUG_LOG" ] && grep -aE "$pat" "$DEBUG_LOG" >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

# --- Функция: получить строку из лога ---
get_log_line() {
    local pat="$1"
    local src
    if [ -f "$DKS_LOG" ]; then
        src="$DKS_LOG"
    elif [ -f "$DEBUG_LOG" ]; then
        src="$DEBUG_LOG"
    else
        return 1
    fi
    grep -aE "$pat" "$src" 2>/dev/null | head -3
}

# --- 7 контрольных точек цепочки ДКС/СВЯЗЬ7 ---
POINTS=(
  'DKS: terminal [0-9]+ registered|Регистрация терминала в ДКС (dks_register)'
  'DKS: PRP=[0-7]+, MPRP|Постановка ПРП6/12 при регистрации'
  'KDP read|032-чтение ОС (РКС/статус КРК)'
  'слркс[0-9]+=0*[0-7]*[4-7][0-7][0-7]|Слово РКС содержит флаги направления'
  'MPRP=0*[1-7][0-7]*|Запись МПРП cmd_033 (УСТПРП)'
  'E24|Установка E24 в ШГ (запрос подкачки СВЯЗЬ7)'
  'KDP write|132-запись ОС (буферы терминалов)'
)

PASS=0; FAIL=0; FIRST_FAIL=""
for p in "${POINTS[@]}"; do
    pat="${p%%|*}"; desc="${p##*|}"
    if search_log "$pat"; then
        echo "✓ $desc"
        PASS=$((PASS+1))
    else
        echo "✗ $desc  [/$pat/]"
        FAIL=$((FAIL+1))
        [ -z "$FIRST_FAIL" ] && FIRST_FAIL="$pat"
    fi
done

echo ""
echo "Итог: $PASS/$((PASS+FAIL)) контрольных точек."
if [ $PASS -eq ${#POINTS[@]} ]; then
    echo "→ Все пройдены: СВЯЗЬ7 должна быть активна."
elif [ $FAIL -gt 0 ]; then
    echo "→ Первая непройденная точка: $FIRST_FAIL"
fi

# --- Дополнительная информация ---
echo ""
echo "--- Последние DKS-события ---"
if [ -f "$DKS_LOG" ]; then
    tail -10 "$DKS_LOG" 2>/dev/null | grep -aiE 'DKS|KDP|PRP|MPRP|E24' | tail -5
else
    tail -c 51200 "$DEBUG_LOG" 2>/dev/null | grep -aiE 'DKS|KDP|PRP|MPRP|E24' | tail -5
fi

# --- Снятие трассы при --trace и непройденных точках ---
if [ "$TRACE_MODE" -eq 1 ] && [ $FAIL -gt 0 ]; then
    echo ""
    echo "=== Снятие трассы (check_dks.sh --trace) ==="
    if ! tmux has-session -t "$SESSION" 2>/dev/null; then
        echo "✗ tmux-сессия $SESSION не найдена — эмулятор не запущен"
        exit 1
    fi

    PULT_VAL=12
    DUMP_FILE="$OSZAGR/$PULT_VAL"
    TRACE_FILE="/tmp/tr${PULT_VAL}.txt"

    echo "Step 1: Ctrl+G -> sim>"
    tmux send-keys -t "$SESSION" C-g
    sleep 2

    echo "Step 2: set pult=$PULT_VAL -> dump_touched"
    tmux send-keys -t "$SESSION" "set pult=$PULT_VAL" Enter
    sleep 3

    if [ ! -f "$DUMP_FILE" ]; then
        echo "✗ Файл дампа $DUMP_FILE не создан"
        exit 1
    fi
    echo "✓ Дамп: $DUMP_FILE ($(stat -c%s "$DUMP_FILE") байт)"

    echo "Step 3: touched.pl -> расшифровка трассы"
    if [ -x "$RE_DISPAK/touched.pl" ]; then
        cd "$RE_DISPAK" && ./touched.pl "$DUMP_FILE" > "$TRACE_FILE" 2>/dev/null
        if [ -s "$TRACE_FILE" ]; then
            TOTAL=$(wc -l < "$TRACE_FILE")
            echo "✓ Трасса: $TRACE_FILE ($TOTAL строк)"
            echo ""
            echo "--- Ключевые адреса СВЯЗЬ7 ---"
            for sym in СВЯДКС НСТДКС ЗАГРУЗ ТСТДКС УСПРП8; do
                cnt=$(grep -c "$sym" "$TRACE_FILE" 2>/dev/null || echo 0)
                [ "$cnt" -gt 0 ] && echo "  $sym: $cnt попаданий"
            done
            echo ""
            echo "--- Первые 10 строк ---"
            head -10 "$TRACE_FILE"
        else
            echo "✗ Трасса пуста"
        fi
    else
        echo "✗ touched.pl не найден: $RE_DISPAK/touched.pl"
    fi

    echo ""
    echo "Step 4: cont (продолжение выполнения)"
    tmux send-keys -t "$SESSION" "cont" Enter
    sleep 2
fi

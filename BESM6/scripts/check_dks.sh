#!/bin/bash
# check_dks.sh — ОДНОРАЗОВАЯ проверка всех контрольных точек ДКС
# (заменяет многократные tail/grep — главный источник издержек сессии)
# Использование: ./scripts/check_dks.sh [размер_хвоста_лога_в_КБ] [--wait [сек]]
#   --wait: перед проверкой ждать появления KDP-событий в логе (по умолчанию
#   до 5 мин, проверка раз в 20 с) — экономит токены на пустых запросах.
# ВАЖНО: debug.txt может быть >1 ГБ — читаем только хвост (правило T4)!

DEBUG_LOG=/home/azizz/Yandex.Disk/simh/BESM6/debug.txt
TAIL_KB=${1:-2000}
[ -f "$DEBUG_LOG" ] || { echo "✗ нет $DEBUG_LOG"; exit 1; }

# опция --wait [сек]: ждём, пока в логе появятся KDP/DKS-события
if [ "${2:-}" = "--wait" ] || [ "${1:-}" = "--wait" ]; then
    [ "${1:-}" = "--wait" ] && WAIT_MAX=${2:-300} || WAIT_MAX=${3:-300}
    echo "Ожидание KDP-событий (до ${WAIT_MAX}с, интервал 20с)..."
    for i in $(seq 1 $((WAIT_MAX / 20))); do
        grep -aq 'KDP\|DKS' "$DEBUG_LOG" 2>/dev/null && break
        sleep 20
    done
fi

# хвост лога во временный файл (grep по маленькому файлу вместо гигабайтов)
TAIL=/tmp/dks_tail.txt
tail -c $((TAIL_KB * 1024)) "$DEBUG_LOG" > "$TAIL"

# контрольные точки цепочки загрузки СВЯЗЬ7 (см. BESM6/AGENTS.md, 2026-09-05)
POINTS=(
  'DKS: terminal [0-9]+ registered|регистрация терминала в ДКС (dks_register)'
  'DKS: PRP=[0-7]+, MPRP|постановка ПРП6/12 при регистрации'
  'KDP read|032-чтение ОС (РКС/статус КРК)'
  'слркс0=[0-7]*[4-7][0-7][0-7]0|слово РКС содержит флаги направления (имитация Э-60)'
  'MPRP=0*[1-7][0-7]*|запись МПРП cmd_033 (УСТПРП битов 6/12)'
  'E24|установка E24 в ШГ (запрос подкачки СВЯЗЬ7)'
  'KDP write|132-запись ОС (буферы терминалов)'
)

PASS=0; FAIL=0
for p in "${POINTS[@]}"; do
    pat=${p%%|*}; desc=${p##*|}
    if grep -aE "$pat" "$TAIL" >/dev/null 2>&1; then
        echo "✓ $desc  [/$pat/]"; PASS=$((PASS+1))
    elif grep -aE "$pat" "$DEBUG_LOG" >/dev/null 2>&1; then
        echo "✓ $desc  [/$pat/] (в ранней части лога, вне хвоста)"; PASS=$((PASS+1))
    else
        echo "✗ $desc  [/$pat/]"; FAIL=$((FAIL+1))
    fi
done

echo ""
echo "Итог: $PASS/$((PASS+FAIL)) контрольных точек."
[ $PASS -eq ${#POINTS[@]} ] && echo "→ Все пройдены: СВЯЗЬ7 должна подкачаться (метка загруз)."
[ $FAIL -gt 0 ] && echo "→ Первая непройденная точка = место разрыва цепочки."

# волатильные данные — только хвост лога (правило T4)
echo ""
echo "--- Последние KDP-события ---"
grep -aiE 'KDP|DKS|PRP|MPRP|E24' "$TAIL" | tail -5

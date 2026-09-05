#!/bin/bash
# check_dks.sh — ОДНОРАЗОВАЯ проверка всех контрольных точек ДКС
# (заменяет многократные tail/grep — главный источник издержек сессии)
# Использование: ./scripts/check_dks.sh [размер_хвоста_лога_в_КБ]
# ВАЖНО: debug.txt может быть >1 ГБ — читаем только хвост (правило T4)!

DEBUG_LOG=/home/azizz/Yandex.Disk/simh/BESM6/debug.txt
TAIL_KB=${1:-2000}
[ -f "$DEBUG_LOG" ] || { echo "✗ нет $DEBUG_LOG"; exit 1; }

# хвост лога во временный файл (grep по маленькому файлу вместо 1.4 ГБ)
TAIL=/tmp/dks_tail.txt
tail -c $((TAIL_KB * 1024)) "$DEBUG_LOG" > "$TAIL"

# контрольные точки цепочки загрузки СВЯЗЬ7 (см. BESM6/AGENTS.md, 2026-09-05)
POINTS=(
  'DKS: terminal registered|регистрация терминала в ДКС'
  'KDP: генерация прерывания ПРП12|SREQ при подключении'
  'KDP read|032-чтение ОС (РКС/статус КРК)'
  'ПРП6|прерывание готовности приёма (вход в свядкс)'
  'MPRP=0*[1-7]|запись МПРП cmd_033 (УСТПРП битов 6/12)'
  'E24|установка E24 в ШГ (запрос подкачки СВЯЗЬ7)'
  'KDP write|132-запись ОС (буферы терминалов)'
)

PASS=0; FAIL=0
for p in "${POINTS[@]}"; do
    pat=${p%%|*}; desc=${p##*|}
    if grep -qiE "$pat" "$TAIL"; then
        echo "✓ $desc  [/$pat/]"; PASS=$((PASS+1))
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

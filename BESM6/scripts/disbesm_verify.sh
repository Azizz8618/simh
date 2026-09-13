#!/usr/bin/env bash
# disbesm_verify.sh — побайтовое сравнение двух дампов БЭСМ-6
# Эталон: verify.pl из проекта re-dispak
#
# disbesm_verify.sh <gold> <silver> [--disasm] [--disasm-path <path>]
#
# Файлы — 6 байт/слово (besmtool dump output, disbesm6 input)

set -euo pipefail

ZONE_WORDS=1024
WORD_SIZE=6
ZONE_BYTES=$((ZONE_WORDS * WORD_SIZE))
RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
CYAN='\033[1;36m'
NC='\033[0m'

DISASM=0
DISASM_PATH=""
GOLD=""
SILVER=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --disasm) DISASM=1; shift ;;
        --disasm-path) DISASM_PATH="$2"; shift 2 ;;
        --help|-h)
            echo "disbesm_verify.sh <gold> <silver> [--disasm] [--disasm-path <path>]"
            echo "  --disasm       при различии — дизассемблировать зону отличия"
            echo "  --disasm-path  путь к disbesm6 (по умолчанию: disbesm6)"
            exit 0 ;;
        *) if [[ -z "$GOLD" ]]; then GOLD="$1"
           elif [[ -z "$SILVER" ]]; then SILVER="$1"; fi; shift ;;
    esac
done

[[ -z "$GOLD" || -z "$SILVER" ]] && { echo "Ошибка: укажите два файла"; exit 1; }
[[ -f "$GOLD" ]] || { echo -e "${RED}Не найден: $GOLD${NC}"; exit 1; }
[[ -f "$SILVER" ]] || { echo -e "${RED}Не найден: $SILVER${NC}"; exit 1; }

# Если указан путь к disbesm6 — используем его
[[ -n "$DISASM_PATH" ]] && DISASM_CMD="$DISASM_PATH" || DISASM_CMD="disbesm6"

echo -e "${CYAN}=== disbesm_verify ===${NC}"
echo -e "Эталон:  ${YELLOW}$GOLD${NC}"
echo -e "Проверка: ${YELLOW}$SILVER${NC}"

GOLD_SIZE=$(stat --format='%s' "$GOLD")
SILVER_SIZE=$(stat --format='%s' "$SILVER")
MIN_SIZE=$((GOLD_SIZE < SILVER_SIZE ? GOLD_SIZE : SILVER_SIZE))
TOTAL_WORDS=$((MIN_SIZE / WORD_SIZE))
TOTAL_ZONES=$((MIN_SIZE / ZONE_BYTES))
REM_WORDS=$(( (MIN_SIZE % ZONE_BYTES) / WORD_SIZE ))

if [[ "$GOLD_SIZE" -ne "$SILVER_SIZE" ]]; then
    echo -e "${RED}Размеры различаются: gold=$GOLD_SIZE silver=$SILVER_SIZE${NC}"
fi
echo -e "Размер: $MIN_SIZE байт ($(printf '%o' $TOTAL_WORDS) слов, $(printf '%o' $TOTAL_ZONES)+$(printf '%o' $REM_WORDS) зон)"
echo ""

# Подсчёт различий
DIFF_COUNT=$(cmp -l "$GOLD" "$SILVER" 2>/dev/null | wc -l || true)

if [[ "$DIFF_COUNT" -eq 0 ]]; then
    echo -e "${GREEN}✓ Файлы идентичны ($(printf '%o' $TOTAL_WORDS) слов)${NC}"
    exit 0
fi

echo -e "${RED}✗ Различий: $DIFF_COUNT байт из $MIN_SIZE${NC}"
echo ""

# Первое различие
FIRST_LINE=$(cmp -b "$GOLD" "$SILVER" 2>/dev/null | head -1 || true)
FIRST_BYTE=$(echo "$FIRST_LINE" | sed -n 's/.*byte \([0-9]*\).*/\1/p')

if [[ -n "$FIRST_BYTE" ]]; then
    DIFF_ZONE=$((FIRST_BYTE / ZONE_BYTES))
    DIFF_WORD=$(( (FIRST_BYTE % ZONE_BYTES) / WORD_SIZE ))
    echo "Первое различие: байт $FIRST_BYTE → зона $(printf '%04o' $DIFF_ZONE), слово $(printf '%04o' $DIFF_WORD)"
    GWORD=$(xxd -s "$FIRST_BYTE" -l $WORD_SIZE -p "$GOLD")
    SWORD=$(xxd -s "$FIRST_BYTE" -l $WORD_SIZE -p "$SILVER")
    echo -e "  Эталон:   ${YELLOW}$GWORD${NC}"
    echo -e "  Проверка: ${RED}$SWORD${NC}"
    echo ""
fi

# Все различия (до 50)
if [[ "$DIFF_COUNT" -le 50 ]]; then
    echo -e "${CYAN}Все различия:${NC}"
    cmp -l "$GOLD" "$SILVER" 2>/dev/null | while read BOFF GB SB; do
        DZ=$((BOFF / ZONE_BYTES)); DW=$(( (BOFF % ZONE_BYTES) / WORD_SIZE ))
        printf "  %04o.%04o:  %02o → %02o\n" "$DZ" "$DW" "$GB" "$SB"
    done
else
    echo -e "${YELLOW}Первые 20 из $DIFF_COUNT:${NC}"
    cmp -l "$GOLD" "$SILVER" 2>/dev/null | head -20 | while read BOFF GB SB; do
        DZ=$((BOFF / ZONE_BYTES)); DW=$(( (BOFF % ZONE_BYTES) / WORD_SIZE ))
        printf "  %04o.%04o:  %02o → %02o\n" "$DZ" "$DW" "$GB" "$SB"
    done
    echo "  ..."
fi
echo ""

# Дизассемблирование при различиях
if [[ "$DISASM" -eq 1 && -n "$FIRST_BYTE" ]]; then
    DIFF_ZONE=$((FIRST_BYTE / ZONE_BYTES))
    ZONE_START=$((DIFF_ZONE * ZONE_BYTES))
    if [[ "$ZONE_START" -lt "$GOLD_SIZE" ]]; then
        echo -e "${CYAN}=== Disasm зоны $(printf '%04o' $DIFF_ZONE) ===${NC}"

        echo -e "${YELLOW}--- Эталон (gold) ---${NC}"
        dd if="$GOLD" bs="$WORD_SIZE" count="$ZONE_WORDS" skip=$((DIFF_ZONE * ZONE_WORDS)) 2>/dev/null > /tmp/_gold_zone.bin
        $DISASM_CMD -a0 -e0 /tmp/_gold_zone.bin 2>/dev/null || echo "($DISASM_CMD не найден)"
        echo ""

        echo -e "${YELLOW}--- Проверка (silver) ---${NC}"
        dd if="$SILVER" bs="$WORD_SIZE" count="$ZONE_WORDS" skip=$((DIFF_ZONE * ZONE_WORDS)) 2>/dev/null > /tmp/_silver_zone.bin
        $DISASM_CMD -a0 -e0 /tmp/_silver_zone.bin 2>/dev/null || echo "($DISASM_CMD не найден)"
        echo ""

        echo -e "${CYAN}--- Diff дизассемблирования ---${NC}"
        diff <($DISASM_CMD -a0 -e0 /tmp/_gold_zone.bin 2>/dev/null) <($DISASM_CMD -a0 -e0 /tmp/_silver_zone.bin 2>/dev/null) | head -60 || true
    fi
fi

exit $((DIFF_COUNT > 0 ? 1 : 0))

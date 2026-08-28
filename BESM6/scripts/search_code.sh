#!/bin/bash
# search_code.sh — Кэшированный поиск по коду
# Использование: ./scripts/search_code.sh <pattern> [file]
# Пример: ./scripts/search_code.sh "dks_poll" besm6_tty.c

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
CACHE_DIR="/tmp/besm6_search"

mkdir -p "$CACHE_DIR"

PATTERN="$1"
FILE="${2:-*.c}"

if [ -z "$PATTERN" ]; then
    echo "Использование: $0 <pattern> [file]"
    echo "Пример: $0 dks_poll besm6_tty.c"
    exit 1
fi

# Нормализованное имя для кэша
CACHE_NAME=$(echo "$PATTERN" | tr -c '[:alnum:]' '_')
CACHE_FILE="$CACHE_DIR/search_${CACHE_NAME}.txt"

echo "=== Поиск: '$PATTERN' в $FILE ==="
echo "Кэш: $CACHE_FILE"
echo ""

# Выполнение поиска с сохранением в кэш
cd "$PROJECT_ROOT"
grep -rn "$PATTERN" $FILE 2>/dev/null > "$CACHE_FILE"

if [ -s "$CACHE_FILE" ]; then
    echo "Найдено строк: $(wc -l < "$CACHE_FILE")"
    echo ""
    cat "$CACHE_FILE"
else
    echo "Не найдено"
fi

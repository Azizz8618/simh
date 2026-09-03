#!/bin/bash
# scripts/gen_code_map.sh — Автоматическая генерация карт кода
#
# Использование:
#   ./scripts/gen_code_map.sh <file.c>       — карта для одного файла
#   ./scripts/gen_code_map.sh --all          — все крупные файлы (LARGE_FILES)
#   ./scripts/gen_code_map.sh --check        — регенерировать только устаревшие
#   ./scripts/gen_code_map.sh --check <file> — проверить один файл
#
# Вызывается автоматически (правило M4 в .clinerules) перед работой с C-файлами.
# Скрипт извлекает: функции (с диапазонами строк), глобальные переменные,
# #define-константы, typedef/struct — всё из исходного кода.
# Результат: .cline/maps/<file>.map

set -e
cd "$(dirname "$0")/.."

MAPS_DIR=".cline/maps"
AWK_SCRIPT="scripts/gen_map.awk"
mkdir -p "$MAPS_DIR"

LARGE_FILES="besm6_cpu.c besm6_tty.c besm6_mmu.c besm6_disk.c besm6_defs.h"

gen_one() {
    local src="$1"
    if [ ! -f "$src" ]; then
        echo "✗ Файл не найден: $src" >&2
        return 1
    fi
    local base map
    base=$(basename "$src")
    map="$MAPS_DIR/${base}.map"

    awk -f "$AWK_SCRIPT" -v src="$base" "$src" > "$map"
    echo "✓ $base → $map ($(wc -l < "$map" | tr -d ' ') строк)"
}

case "${1:-}" in
    --all)
        echo "=== Генерация всех карт ==="
        for f in $LARGE_FILES; do gen_one "$f"; done
        echo "=== Готово: $(ls "$MAPS_DIR"/*.map 2>/dev/null | wc -l) карт ==="
        ;;
    --check)
        if [ -n "${2:-}" ]; then
            base=$(basename "$2")
            map="$MAPS_DIR/${base}.map"
            if [ ! -f "$map" ] || [ "$2" -nt "$map" ]; then
                echo "→ $base: регенерация..."
                gen_one "$2"
            else
                echo "✓ $base: актуальна"
            fi
        else
            echo "=== Проверка всех карт ==="
            regen=0
            for f in $LARGE_FILES; do
                map="$MAPS_DIR/${f}.map"
                if [ ! -f "$map" ] || [ "$f" -nt "$map" ]; then
                    echo "→ $f: регенерация..."
                    gen_one "$f"
                    regen=$((regen + 1))
                else
                    echo "✓ $f: актуальна"
                fi
            done
            echo "=== Готово: регенерировано $regen ==="
        fi
        ;;
    --help|"")
        echo "Использование:"
        echo "  $0 <file.c>       — карта для файла"
        echo "  $0 --all          — все крупные файлы"
        echo "  $0 --check        — регенерировать устаревшие"
        echo "  $0 --check <file> — проверить один файл"
        ;;
    *)
        gen_one "$1"
        ;;
esac

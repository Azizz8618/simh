#!/bin/bash
# scripts/gen_code_map.sh — Автоматическая генерация карт кода
#
# Использование:
#   ./scripts/gen_code_map.sh <file.c>       — карта для одного файла
#   ./scripts/gen_code_map.sh --all          — все крупные файлы (LARGE_FILES)
#   ./scripts/gen_code_map.sh --check        — регенерировать только устаревшие
#   ./scripts/gen_code_map.sh --check <file> — проверить один файл
#   ./scripts/gen_code_map.sh --auto         — авто-режим: C/C++/H-файлы проекта
#                                              > AUTO_MIN_LINES (по умолч. 500)
#
# Вызывается автоматически (правило M4 в .clinerules) перед работой с C-файлами.
# Скрипт извлекает: функции (с диапазонами строк), глобальные переменные,
# #define-константы, typedef/struct — всё из исхо��ного кода.
# Результат: .cline/maps/<file>.map
#
# Глобальная копия: ~/.cline/scripts/gen_code_map.sh — работает из ЛЮБОГО
# проекта (не только БЭСМ-6). Карты составляются для любых больших файлов
# автоматически (авто-обнаружение по размеру, список LARGE_FILES не обязателен).

set -e
# Определяем корень проекта: каталог, откуда вызван скрипт, либо git-корень
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Если скрипт лежит в <проект>/scripts — корень на уровень выше;
# если в ~/.cline/scripts — берём текущий каталог (запуск из проекта).
if [ "$SCRIPT_DIR" = "$HOME/.cline/scripts" ]; then
    PROJECT_DIR="$(pwd)"
else
    PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
fi
cd "$PROJECT_DIR"

MAPS_DIR=".cline/maps"
# awk-парсер: рядом со скриптом (проект или ~/.cline/scripts)
AWK_SCRIPT="$SCRIPT_DIR/gen_map.awk"
mkdir -p "$MAPS_DIR"

# Порог «крупного» файла для авто-режима (строк)
AUTO_MIN_LINES="${AUTO_MIN_LINES:-500}"

LARGE_FILES="besm6_cpu.c besm6_tty.c besm6_mmu.c besm6_disk.c besm6_defs.h"

# Авто-обнаружение крупных исходников (C/C++/заголовки) в дереве проекта.
# Исключаются: каталоги сборки, зависимостей, карты, бинарники.
auto_discover() {
    find . \( -name .git -o -name node_modules -o -name BIN -o -name build \
              -o -name .cline -o -name bin -o -name obj \) -prune -o \
        -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
                   -o -name '*.hpp' -o -name '*.hh' \) -print 2>/dev/null | \
    while read -r f; do
        [ "$(wc -l < "$f")" -ge "$AUTO_MIN_LINES" ] && printf '%s\n' "${f#./}"
    done
}

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
    --auto|--check-all)
        echo "=== Авто-проверка карт (файлы >= ${AUTO_MIN_LINES} строк) ==="
        regen=0
        while IFS= read -r f; do
            [ -z "$f" ] && continue
            base=$(basename "$f")
            map="$MAPS_DIR/${base}.map"
            if [ ! -f "$map" ] || [ "$f" -nt "$map" ]; then
                echo "→ $f: регенерация..."
                gen_one "$f"
                regen=$((regen + 1))
            fi
        done < <(auto_discover)
        echo "=== Готово: регенерирова��о $regen ==="
        ;;
    --all)
        echo "=== Генерация всех карт ==="
        for f in $LARGE_FILES; do gen_one "$f"; done
        echo "=== Готово: $(ls "$MAPS_DIR"/*.map 2>/dev/null | wc -l) карт ==="
        ;;
    --check)
        if [ -n "${2:-}" ]; then
            # Файл может быть путём; база — basename
            src="$2"
            base=$(basename "$src")
            map="$MAPS_DIR/${base}.map"
            if [ ! -f "$src" ] && [ -f "$base" ]; then src="$base"; fi
            if [ ! -f "$map" ] || [ "$src" -nt "$map" ]; then
                echo "→ $base: регенерация..."
                gen_one "$src"
            else
                echo "✓ $base: актуальна"
            fi
        else
            echo "=== Проверка всех карт (авто-обнаружение + LARGE_FILES) ==="
            regen=0
            # Авто-обнаруженные + фиксированный список (если файл существует)
            { auto_discover; for f in $LARGE_FILES; do [ -f "$f" ] && echo "$f"; done; } | sort -u | \
            while IFS= read -r f; do
                [ -z "$f" ] && continue
                base=$(basename "$f")
                map="$MAPS_DIR/${base}.map"
                if [ ! -f "$map" ] || [ "$f" -nt "$map" ]; then
                    echo "→ $f: регенерация..."
                    gen_one "$f"
                    regen=$((regen + 1))
                else
                    echo "✓ $f: актуальна"
                fi
            done
            echo "=== Готово ==="
        fi
        ;;
    --help|"")
        echo "Использование:"
        echo "  $0 <file.c>       — карта для файла"
        echo "  $0 --all          — все крупные файлы (LARGE_FILES)"
        echo "  $0 --check        — регенерировать устаревшие (авто-обнаружение)"
        echo "  $0 --check <file> — проверить один файл"
        echo "  $0 --auto         — авто-режим: все C/C++/H >= $AUTO_MIN_LINES строк"
        echo "Порог авто-режима настраивается: AUTO_MIN_LINES=<N> $0 --auto"
        ;;
    *)
        gen_one "$1"
        ;;
esac

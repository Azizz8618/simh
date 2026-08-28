#!/bin/bash
# monitor_dks.sh — Мониторинг отладки ДКС в реальном времени
# Использование: ./scripts/monitor_dks.sh [grep_pattern]

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
DEBUG_LOG="$PROJECT_ROOT/debug.txt"

if [ ! -f "$DEBUG_LOG" ]; then
    echo "✗ Лог отладки не найден: $DEBUG_LOG"
    echo "Запустите эмулятор: ./scripts/test_dks.sh"
    exit 1
fi

PATTERN="${1:-DKS|KDP|terminal|PRP}"

echo "=== Мониторинг ДКС ==="
echo "Файл: $DEBUG_LOG"
echo "Паттерн: $PATTERN"
echo ""
echo "Нажмите Ctrl+C для выхода"
echo ""

tail -f "$DEBUG_LOG" 2>/dev/null | grep --line-buffered -iE "$PATTERN"

#!/bin/bash
# monitor_dks.sh — Мониторинг логов ДКС в реальном времени
#
# Источник: logs/debug_dks.log (per-subsystem) с fallback на debug.txt
# Использование: ./scripts/monitor_dks.sh [grep_pattern]

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
OSZAGR="$PROJECT_ROOT/OSZAGR"
DKS_LOG="$OSZAGR/logs/debug_dks.log"
DEBUG_LOG="$PROJECT_ROOT/debug.txt"
PATTERN="${1:-DKS|KDP|terminal|PRP}"

if [ ! -f "$DKS_LOG" ] && [ ! -f "$DEBUG_LOG" ]; then
    echo "✗ Логи не найдены"
    echo "  Per-subsystem: $DKS_LOG"
    echo "  Debug:         $DEBUG_LOG"
    echo "  Запустите: ./scripts/test_dks.sh"
    exit 1
fi

echo "=== Мониторинг ДКС ==="
if [ -f "$DKS_LOG" ]; then
    echo "Файл: $DKS_LOG (per-subsystem)"
else
    echo "Файл: $DEBUG_LOG (fallback)"
fi

echo "Паттерн: $PATTERN"
echo "Ctrl+C для выхода"
echo ""

if [ -f "$DKS_LOG" ]; then
    tail -f "$DKS_LOG" 2>/dev/null | grep --line-buffered -iE "$PATTERN"
else
    tail -f "$DEBUG_LOG" 2>/dev/null | grep --line-buffered -iE "$PATTERN"
fi

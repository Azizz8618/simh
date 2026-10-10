#!/bin/bash
# build.sh — Быстрая сборка эмулятора БЭСМ-6
# Использование: ./scripts/build.sh [--clean] [--bg]
#
# ПРАВИЛА СБОРКИ (2026-09-16):
#   - Собирать ТОЛЬКО из каталога simh: make besm6
#   - make из BESM6/ использует другой Makefile (неполный!)
#   - Старый бинарник удалять перед сборкой

set -euo pipefail

SIMH_ROOT="/home/azizz/Yandex.Disk/simh"
BIN="$SIMH_ROOT/BIN/besm6"
BUILD_LOG="/tmp/besm6_build.log"
CLEAN=0
BG=0

for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --bg)    BG=1 ;;
    esac
done

cd "$SIMH_ROOT"

# Очистка
if [ "$CLEAN" -eq 1 ]; then
    echo "Очистка..."
    make clean > /dev/null 2>&1 || true
fi

# Удаляем старый бинарник — индикатор сборки
rm -f "$BIN"

echo "Сборка make besm6 из $SIMH_ROOT ..."
START=$(date +%s)

if [ "$BG" -eq 1 ]; then
    # Фоновый режим (для tmux)
    make besm6 > "$BUILD_LOG" 2>&1 &
    BUILD_PID=$!
    echo "PID=$BUILD_PID Лог=$BUILD_LOG"
    exit 0
fi

# Синхронный режим (ждём результат)
make besm6 > "$BUILD_LOG" 2>&1
RC=$?
END=$(date +%s)
DURATION=$((END - START))

if [ $RC -eq 0 ] && [ -x "$BIN" ]; then
    SIZE=$(ls -lh "$BIN" | awk '{print $5}')
    MTIME=$(stat -c '%y' "$BIN" | cut -d. -f1)
    echo "OK ${DURATION}с ${SIZE} $MTIME"
else
    echo "ОШИБКА (rc=$RC) ${DURATION}с"
    echo "--- Последние 20 строк лога ---"
    tail -20 "$BUILD_LOG"
    exit 1
fi

#!/bin/bash
# build.sh — Фоновая сборка эмулятора БЭСМ-6
# Использование: ./scripts/build.sh

set -e

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
BUILD_LOG="/tmp/besm6_build.log"

echo "=== Сборка эмулятора БЭСМ-6 ==="
echo "Лог: $BUILD_LOG"
echo ""

cd "$PROJECT_ROOT"

# Очистка и сборка: ТОЛЬКО из папки simh (make besm6)!
# make из папки BESM6 НЕ пересобирает эмулятор.
cd /home/azizz/Yandex.Disk/simh

# ОБЯЗАТЕЛЬНОЕ правило (2026-09-06): удалять старый бинарник перед сборкой.
# Отсутствие файла после сборки = надёжный индикатор незавершённой сборки.
rm -f /home/azizz/Yandex.Disk/simh/BIN/besm6

# Сборка в фоне
echo "Запуск сборки (make besm6 из simh, старый бинарник удалён)..."
make besm6 > "$BUILD_LOG" 2>&1 &
BUILD_PID=$!

echo "PID процесса: $BUILD_PID"
echo ""
echo "Проверка статуса:"
echo "  tail -f $BUILD_LOG"
echo ""
echo "Ожидание завершения (Ctrl+C для отмены)..."

# Ожидание завершения
wait $BUILD_PID 2>/dev/null

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Сборка успешно завершена"
    echo "Исполняемый файл: $PROJECT_ROOT/BIN/besm6"
    ls -lh "$PROJECT_ROOT/BIN/besm6" 2>/dev/null || true
else
    echo ""
    echo "✗ Ошибка сборки. Последние 30 строк лога:"
    tail -30 "$BUILD_LOG"
fi

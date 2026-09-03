#!/bin/bash
# Быстрая сборка BESM-6 эмулятора
# Использует кэширование объектных файлов для ускорения

cd /home/azizz/Yandex.Disk/simh/BESM6

echo "=== Сборка BESM-6 ==="
make -f Makefile.nopanel -j2 2>&1 | tail -5

if [ -x ../BIN/besm6 ]; then
    echo "✓ Бинарник готов: $(ls -lh ../BIN/besm6)"
    exit 0
else
    echo "✗ Ошибка сборки"
    exit 1
fi

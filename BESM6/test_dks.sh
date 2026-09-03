#!/bin/bash
# Оптимизированный тест DKS (КАДОПАМ) терминалов
# Одна команда для полного цикла: запуск -> тест -> результат

cd /home/azizz/Yandex.Disk/simh/BESM6

# Очистка
pkill -9 besm6 2>/dev/null
rm -f debug.txt output.txt

# Проверка бинарника
if [ ! -x ../BIN/besm6 ]; then
    echo "✗ Нет бинарника. Запустите ./build.sh сначала"
    exit 1
fi

echo "=== Запуск BESM-6 с DKS ==="
../BIN/besm6 /tmp/dks.ini > /tmp/dks_test.log 2>&1 &
BESM_PID=$!
sleep 3

# Проверка порта
if ss -tlnp | grep -q 4199; then
    echo "✓ Порт 4199 слушает"
else
    echo "✗ Порт не открыт"
    kill $BESM_PID 2>/dev/null
    exit 1
fi

# Подключение к tty17 (DKS)
echo "=== Подключение к DKS терминалу ==="
(echo ''; sleep 1) | nc localhost 4199 > /tmp/nc_out.log 2>&1 &
sleep 2

# Проверка результатов
echo "=== Результаты ==="
if [ -f debug.txt ]; then
    echo "--- Debug вывод ---"
    grep -E 'tty\d+:.*connection|DKS|kadopam_mem\[|PRP.*DKS' debug.txt 2>/dev/null | tail -15
    
    if grep -q "tty17.*connection\|DKS.*register\|kadopam_mem\[" debug.txt 2>/dev/null; then
        echo "✓ DKS терминал зарегистрирован"
    else
        echo "⚠ DKS регистрация не обнаружена"
        echo "--- Последние 20 строк debug ---"
        tail -20 debug.txt
    fi
else
    echo "✗ Нет debug файла"
fi

# Остановка
pkill -9 besm6 2>/dev/null
echo "=== Тест завершен ==="

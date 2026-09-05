#!/bin/bash
# scripts/test_traces.sh — скрипт тестирования генерации и проверки трасс

# Конфигурация
WORKDIR="$HOME/Yandex.Disk/simh/BESM6/OSZAGR"
RE_DISPAK="$HOME/Yandex.Disk/re_dispak/re-dispak"
PULT_VAL="12"
DUMP_FILE="$WORKDIR/$PULT_VAL"
TRACE_FILE="$WORKDIR/tr$PULT_VAL.txt"
SESSION="dispak"

echo "=== Шаг 2: Тестирование трассировки (pult=$PULT_VAL) ==="

# 1. Запуск эмулятора (если не запущен)
if ! screen -list | grep -q "$SESSION"; then
    echo "Запуск эмулятора..."
    cd "$WORKDIR" && screen -dmS "$SESSION" ../BIN/besm6 dispak.ini
    sleep 5
fi

# 2. Останов, генерация дампа
echo "Генерация дампа..."
screen -S "$SESSION" -X stuff $'\007'  # Ctrl+G
sleep 2
screen -S "$SESSION" -X stuff "set pult=$PULT_VAL"$'\r'
sleep 3
# Проверка файла дампа
if [ ! -f "$DUMP_FILE" ]; then
    echo "Ошибка: файл дампа $DUMP_FILE не создан"
    exit 1
fi
echo "Дамп $DUMP_FILE создан."

# 3. Обработка через touched.pl
echo "Генерация трассы в $TRACE_FILE..."
cd "$RE_DISPAK" || exit 1
# touched.pl требует наличия base.txt в текущем каталоге
./touched.pl "$DUMP_FILE" > "$TRACE_FILE"

# 4. Проверки
if [ ! -s "$TRACE_FILE" ]; then
    echo "Ошибка: файл трассы $TRACE_FILE пуст"
    exit 1
fi

UNKNOWN_COUNT=$(grep -c "unknown" "$TRACE_FILE")
TOTAL_COUNT=$(wc -l < "$TRACE_FILE")

echo "Анализ трассы:"
echo "  Всего строк: $TOTAL_COUNT"
echo "  Неопознанных: $UNKNOWN_COUNT"

if [ "$UNKNOWN_COUNT" -gt 0 ]; then
    echo "Внимание: найдено $UNKNOWN_COUNT неопознанных команд."
fi

echo "Тест завершён."

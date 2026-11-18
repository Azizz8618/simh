#!/bin/sh
# Повторяемая проверка этапа 2 порта СМ-2.
# Запуск из корня simh: sh HP2100/tests/run_sm2_tests.sh

set -eu
BIN=./BIN/hp2100
INI=HP2100/tests/sm2_stage2_test.ini
OUT=/tmp/sm2-stage2-test.out

[ -x "$BIN" ] || { echo "СТОП: нет $BIN" >&2; exit 1; }

"$BIN" < "$INI" > "$OUT" 2>&1

fail=0
check () {
    if grep -q "$1" "$OUT"; then
        echo "ОК   $2"
    else
        echo "ОШИБКА $2 (ожидалось: $1)" >&2
        fail=1
    fi
}

check '00103' 'СМ-2: ISZ пропускает 2 слова (P=00103)'
check '000000' 'СМ-2: ячейка 0200 увеличилась до нуля'
check '000101' 'СМ-2: JSB сохранил адрес возврата в РАВ'
check '00201' 'СМ-2: JSB перешёл на 0201'
check '00102' 'HP 2116: ISZ пропускает 1 слово (P=00102, без регрессии)'

if [ "$fail" = 0 ]; then
    echo 'ИТОГО: все проверки этапа 2 пройдены'
else
    echo 'ИТОГО: есть расхождения' >&2
    exit 1
fi

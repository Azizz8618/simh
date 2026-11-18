#!/bin/sh
# Повторяемая проверка этапа 2 порта СМ-2.
# Запуск из корня simh: sh HP2100/tests/run_sm2_tests.sh

set -eu
BIN=./BIN/hp2100
INI=HP2100/tests/sm2_stage2_test.ini
OUT=/tmp/sm2-stage2-test.out
OUT3=/tmp/sm2-stage3-test.out
OUT4=/tmp/sm2-stage4-test.out

[ -x "$BIN" ] || { echo "СТОП: нет $BIN" >&2; exit 1; }

"$BIN" < "$INI" > "$OUT" 2>&1
"$BIN" < HP2100/tests/sm2_stage3_test.ini > "$OUT3" 2>&1
"$BIN" < HP2100/tests/sm2_stage4_test.ini > "$OUT4" 2>&1

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
check3 () {
    if grep -q "$1" "$OUT3"; then
        echo "ОК   $2"
    else
        echo "ОШИБКА $2 (ожидалось: $1)" >&2
        fail=1
    fi
}

check3 '012345' 'СМ-2: РБ1 сместил адрес на 01000, LDA прочитала 012345'
check3 '00000001100' 'СМ-2: абсолютный адрес с РБ1 = 01100'
check3 '00000100100' 'СМ-2: РБ2 дал 17-разрядный адрес 0100100'

check4 () {
    if grep -q "$1" "$OUT4"; then
        echo "ОК   $2"
    else
        echo "ОШИБКА $2 (ожидалось: $1)" >&2
        fail=1
    fi
}

check4 '000017' 'СМ-2: MPY 3 x 5 = 017 в RA'
check4 '000122' 'СМ-2: LDX загрузил РИ1 (бит 0 автоиндекса сброшен)'
check4 '011111' 'СМ-2: DLD загрузил RA из первой ячейки'
check4 '022222' 'СМ-2: DLD загрузил RB из второй ячейки'

if [ "$fail" = 0 ]; then
    echo 'ИТОГО: все проверки этапов 2-4 пройдены'
else
    echo 'ИТОГО: есть расхождения' >&2
    exit 1
fi

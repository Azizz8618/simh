#!/usr/bin/env bash
# Phase P1: CR11/CD11 карточитатель. Тест проверяет только смоделированное
# устройство, production code не меняет. До регистрации CR в sim_devices[]
# и SIMDEV тест честно падает на "set CR" (устройство неизвестно).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "FAIL[sim]: simulator not built: $sim" >&2
  exit 1
  fi

fail () {
  echo "FAIL[$1]: $2" >&2
  exit 1
  }

check () {
  local out=$1 pat=$2 label=$3
  if ! printf '%s\n' "$out" | grep -Eq "$pat"; then
    echo "FAIL[$label]: expected $pat" >&2
    printf '%s\n' "$out" >&2
    exit 1
    fi
  }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Трёхкарточная колода для проверки attach и три отдельные карты для чтения.
# Цифры кодируются одинаково во всех таблицах перевода simh, поэтому
# ожидаемые слова в памяти не зависят от выбранной кодовой страницы.
printf '0123456789\nABCDEFGHIJ\n9876543210\n' > "$tmp/cards.txt"
printf '0123456789\n' > "$tmp/card1.txt"
printf 'ABCDEFGHIJ\n' > "$tmp/card2.txt"
printf '9876543210\n' > "$tmp/card3.txt"

# T0: устройство должно быть зарегистрировано и разрешаться командой SIMH.
# Это точка красного прогона: сейчас SIMH отвечает "Non-existent device: CR".
out=$(printf 'set CR enabled\nexit\n' | "$sim" 2>&1)
if printf '%s\n' "$out" | grep -q 'SIM-ERROR'; then
  fail cr_enable "set CR enabled не распознано: $out"
  fi

# T1: autoconfig использует уже существующую запись CR в auto_tab:
# CSR 017777160, вектор 0230, BR6 (INT_V_CR=5, IPL_CR=6).
out=$(printf 'set CR enabled\nshow devices\nexit\n' | "$sim" 2>&1)
check "$out" '^CR .*address=17777160-17777167, vector=230, BR6' 'T1 CR DIB'

# T2: колода из трёх карт подключается как обычный последовательный файл.
out=$(printf 'set CR enabled\nattach CR0 %s\nshow CR\nexit\n' "$tmp/cards.txt" | "$sim" 2>&1)
if printf '%s\n' "$out" | grep -q 'SIM-ERROR'; then
  fail attach "attach CR0 отклонён: $out"
  fi
check "$out" 'ASCII format' 'T2 ASCII deck format'

# T3: чтение карты. Гостевая программа пишет GO в CSR (guest 0177160),
# загружает CD11 счётчик слов (CSR+2) и адрес буфера (CSR+4), затем ждёт
# обнуления счётчика, то есть завершения передачи (DONE по CDCC).
# Вектор прерывания 0230 проверяется через DIB в T1.
program='
deposit 0 MOV #0177160,R0
deposit 4 MOV #0177162,R1
deposit 8 MOV #0177164,R2
deposit 12 MOV #001000,R3
deposit 16 MOV #0177660,@R1
deposit 20 MOV R3,@R2
deposit 22 MOV #000001,@R0
deposit 26 TST @R1
deposit 28 BNE 000026
deposit 30 HALT
'

read_card () {
  # $1 = файл карты, $2 = адрес приёмника (6 октальных цифр), $3 = метка
  printf '%s\n' \
    'set CPU BOOTEVENTS=on' \
    'set CR enabled' \
    'set CR CD11' \
    'set CR RATE=1200' \
    "attach CR0 $1" \
    'set CR RESET' \
    "$program" \
    "deposit 14 $2" \
    'go 0' \
    'examine CR CDCC' \
    'examine CR CRS' \
    'examine 001000' 'examine 001002' \
    'exit' | timeout 120 "$sim" 2>&1
  }

# T4: три карты из колоды дают завершённую передачу (CDCC=0) и ожидаемые
# 12-разрядные слова Холлерита в памяти по заданному адресу.
out=$(read_card "$tmp/card1.txt" 001000 card1)
check "$out" 'Simulation stopped' 'T4 card1 HALT'
check "$out" 'CDCC:.*000000' 'T4 card1 DONE'
check "$out" '512:.*001000' 'T4 card1 digit 0'
check "$out" '514:.*000400' 'T4 card1 digit 1'

out=$(read_card "$tmp/card2.txt" 001000 card2)
check "$out" 'Simulation stopped' 'T4 card2 HALT'
check "$out" 'CDCC:.*000000' 'T4 card2 DONE'
check "$out" '512:.*04400' 'T4 card2 letter A'
check "$out" '514:.*04200' 'T4 card2 letter B'

out=$(read_card "$tmp/card3.txt" 001000 card3)
check "$out" 'Simulation stopped' 'T4 card3 HALT'
check "$out" 'CDCC:.*000000' 'T4 card3 DONE'
check "$out" '512:.*00001' 'T4 card3 digit 9'
check "$out" '514:.*00002' 'T4 card3 digit 8'

echo 'PASS: CR card reader reads three cards, DONE, vector 0230, words in memory'

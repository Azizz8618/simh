#!/usr/bin/env bash
# Phase B2: виртуальные метки времени книги 4 для семи фаз единого канала.
# Метки не имеют отношения к наносекундам хоста или к часам SIMH.
# Скрипт намеренно красный, пока e10025_bus_time не зарегистрирован
# в e10025_chan.c и в debug-журнале нет строк t_ns=.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "FAIL: simulator not built: $sim" >&2
  exit 1
fi

debug_log=$(mktemp)
trap 'rm -f "$debug_log"' EXIT

run_sim_debug () {
  : > "$debug_log"
  printf '%s\n' "set debug $debug_log" 'set CHAN debug' "$@" 'exit' | "$sim" 2>&1
}

reg_val () {
  local out=$1 name=$2 nth=${3:-1}
  printf '%s\n' "$out" | sed -n "s/.*$name:[[:space:]]*//p" | sed -n "${nth}p"
}

expect_out () {
  local out=$1 pat=$2 label=$3
  if ! printf '%s\n' "$out" | grep -Eq "$pat"; then
    echo "FAIL: $label (expected $pat)" >&2
    printf '%s\n' "$out" >&2
    exit 1
  fi
}

expect_eq () {
  local got=$1 want=$2 label=$3
  if [ "$got" != "$want" ]; then
    echo "FAIL: $label (got '$got', expected '$want')" >&2
    exit 1
  fi
}

# 1. Один ВВОД: семь фаз, у каждой своя виртуальная метка. Пока модуль не
# подключён к журналу, меток нет и тест красный по самой причине B2.
out=$(run_sim_debug \
  'deposit 8192 012525' \
  'examine CHAN TIMEOUT' \
  'set CHAN CYCLE=00:020000' \
  'examine CHAN TIMEOUT')
printf '%s\n' "$out" | grep -E 'TIMEOUT:' >/dev/null

if ! grep -q 't_ns=' "$debug_log"; then
  echo 'FAIL: B2 red: no t_ns= in debug log, e10025_bus_time is not registered' >&2
  echo '      seven book-4 timestamps are expected after Phase B2 integration' >&2
  exit 1
fi

mapfile -t stamps < <(sed -n 's/.*t_ns=\([0-9][0-9]*\) phase=\([A-Z_]*\).*/\1 \2/p' \
  "$debug_log" | head -7)
expect_eq "${#stamps[@]}" 7 'one t_ns phase line for each of seven phases'

want_ns=(0 150 225 300 375 450 525)
want_ph=(ADDRESS SA WAIT_SACK STROBE RELEASE CLEANUP IDLE)
ns=()
for entry in "${stamps[@]}"; do
  ns+=("${entry%% *}")
done
for i in 0 1 2 3 4 5 6; do
  expect_eq "${ns[$i]}" "${want_ns[$i]}" "t_ns of ${want_ph[$i]}"
  if [ "${stamps[$i]}" != "${ns[$i]} ${want_ph[$i]}" ]; then
    echo "FAIL: phase name at ${want_ph[$i]} (got '${stamps[$i]}')" >&2
    exit 1
  fi
done

# 2. Точные разности между соседними фазами, полный цикл 525 и повтор 450.
want_delta=(150 75 75 75 75 75)
for i in 1 2 3 4 5 6; do
  delta=$(( ${ns[$i]} - ${ns[$((i - 1))]} ))
  expect_eq "$delta" "${want_delta[$((i - 1))]}" "delta into ${want_ph[$i]}"
done
expect_eq "${ns[6]}" 525 'full cycle is 525 book-ns'
expect_eq "${ns[5]}" 450 'repeat point CLEANUP is 450 book-ns'

# 3. Успешный обмен не увеличивает TIMEOUT.
t0=$(reg_val "$out" TIMEOUT 1)
t1=$(reg_val "$out" TIMEOUT 2)
expect_eq "$((10#$t0))" "$((10#$t1))" 'successful ВВОД does not grow TIMEOUT'

# 4. При подавленном СП таймаут фиксируется на 15000 book-ns, а счётчик
# TIMEOUT растёт ровно на единицу. Двойной цикл SOB даёт достаточно
# симулированных инструкций, host-время не используется.
out=$(run_sim_debug \
  'set CHAN TIMING=scaled' \
  'set CHAN NOSP=1' \
  'set CHAN CYCLE=00:020000' \
  'deposit 0 012701' 'deposit 2 0177777' 'deposit 4 077101' \
  'deposit 6 012701' 'deposit 8 0177777' 'deposit 10 077101' \
  'deposit 12 000000' 'deposit PC 0' 'cont' \
  'examine CHAN TIMEOUT' 'examine CHAN STATE' 'exit')
printf '%s\n' "$out" | grep -E 'TIMEOUT:|STATE:' >/dev/null
expect_out "$out" 'TIMEOUT:.*00000000001' 'NOSP=1 increments TIMEOUT once'
if ! grep -q 't_ns=15000' "$debug_log"; then
  echo 'FAIL: timeout is not stamped at 15000 book-ns' >&2
  exit 1
fi

echo 'PASS: E10025 book-4 virtual phase timestamps'
echo 'PASS: E10025 TIMEOUT unchanged on success'

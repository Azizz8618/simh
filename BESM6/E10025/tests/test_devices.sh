#!/usr/bin/env bash
# Минимальный набор периферии Электроника-100-25 и загрузка RT-11.
# Тест только проверяет текущую конфигурацию, production code не меняет.
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

out=$(printf '%s\n' 'show devices' 'exit' | "$sim" 2>&1)

# Ровно тот набор устройств, который заявлен как рабочий минимум.
for dev in CPU SYSTEM CLK TTI TTO CHAN DMA DKSE RK RL CR PTR PTP LPT RHA RP; do
  printf '%s\n' "$out" | grep -Eq "^${dev} " || fail "$dev" "device $dev missing from show devices"
  done

# Честная граница: эти контроллеры пока не реализованы и не должны появляться.
for dev in CD; do
  printf '%s\n' "$out" | grep -Eq "^${dev} " && fail "$dev" "unexpected device $dev in minimal set"
  done

# Ключевые адреса и векторы дисковых контроллеров.
printf '%s\n' "$out" | grep -q 'address=17777400-17777417, vector=220' || \
  fail rk 'expected RK05 at CSR 01777400, vector 0220'
printf '%s\n' "$out" | grep -q 'address=17774400-17774411, vector=160' || \
  fail rl 'expected RL02 at CSR 01774400, vector 0160'

# Память по умолчанию и отсутствие ошибок у отдельного show.
mem=$(printf '%s\n' 'show CPU MEMSIZE' 'exit' | "$sim" 2>&1)
printf '%s\n' "$mem" | grep -q 'MEMSIZE=124K words' || \
  fail memsize 'expected documented default MEMSIZE=124K words'

for dev in RK RL CHAN DMA DKSE; do
  d=$(printf '%s\n' "show $dev" 'exit' | "$sim" 2>&1)
  printf '%s\n' "$d" | grep -q 'SIM-ERROR' && fail "show-$dev" "show $dev returned SIM-ERROR"
  done

echo 'PASS: peripherals CPU/SYSTEM/CLK/TTI/TTO/CHAN/DMA/DKSE/RK/RL/CR/PTR/PTP/LPT/RHA/RP present'

#Системный тест: та же конфигурация должна загрузить RT-11 V4.
if ! timeout 90 bash "$here/tests/run_rt11_boot.sh"; then
  fail boot 'RT-11 V4 did not boot with the minimal peripheral set'
  fi
echo 'PASS: RT-11 V4 boots with the minimal peripheral set'

#!/usr/bin/env bash
# Переключатель типов дискового устройства: RP03, RP02, RP04 и RM03.
# Одновременно включается только одна пара контроллеров, поэтому адрес
# 016700 и вектор 0254 не конфликтуют.
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

run_sim () {
  printf '%s\n' "$@" 'exit' | "$sim" 2>&1
  }

out=$(run_sim 'show SYSTEM DISK')
printf '%s\n' "$out" | grep -q 'DISK=RP03' || fail default 'expected default DISK=RP03'

out=$(run_sim 'set SYSTEM DISK=RP04' 'show SYSTEM DISK' 'show devices')
printf '%s\n' "$out" | grep -q 'DISK=RP04' || fail rp04 'expected DISK=RP04'
printf '%s\n' "$out" | grep -q '^RR.*disabled' || fail rp04 'RR must be disabled for RP04'
printf '%s\n' "$out" | grep -q '^RHA .*address=' || fail rp04 'RHA must be enabled for RP04'
printf '%s\n' "$out" | grep -q '^RP .*Massbus' || fail rp04 'RP must be enabled for RP04'

out=$(run_sim 'set SYSTEM DISK=RP02' 'show SYSTEM DISK' 'show devices')
printf '%s\n' "$out" | grep -q 'DISK=RP02' || fail rp02 'expected DISK=RP02'
printf '%s\n' "$out" | grep -q '^RR .*RP11-C' || fail rp02 'RR must be enabled for RP02'
printf '%s\n' "$out" | grep -q '^RHA .*disabled' || fail rp02 'RHA must be disabled for RP02'

out=$(run_sim 'set SYSTEM DISK=RM03' 'show SYSTEM DISK' 'show devices')
printf '%s\n' "$out" | grep -q 'DISK=RM03' || fail rm03 'expected DISK=RM03'
printf '%s\n' "$out" | grep -q '^RR.*disabled' || fail rm03 'RR must be disabled for RM03'
printf '%s\n' "$out" | grep -q '^RHA .*address=' || fail rm03 'RHA must be enabled for RM03'

if printf '%s\n' "$out" | grep -q 'SIM-ERROR'; then
  fail conflicts 'disk switch reported SIM-ERROR'
  fi

echo 'PASS: disk switch RP03/RP02/RP04/RM03 keeps one controller active'

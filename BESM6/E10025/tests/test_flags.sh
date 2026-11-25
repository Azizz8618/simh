#!/usr/bin/env bash
# Flag verification for the Электроника-100-25 CPU.
# Covers ADD overflow V, SUB borrow C without false V, byte-form decoding
# (CMPB opcode 012) and the CCC/SEC/SEZ/CLZ flag-change instructions.
# PSW prints in decimal: 0224 is IPL 7 with all flags clear, N=8, Z=4, V=2, C=1.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "simulator not built: $sim" >&2
  exit 1
fi

check () {
  local out=$1 pat=$2 label=$3
  if ! printf '%s\n' "$out" | grep -Eq "$pat"; then
    echo "FAIL[$label]: expected $pat" >&2
    printf '%s\n' "$out" >&2
    exit 1
    fi
  }

run_case () {
  local label=$1 r0=$2 psw=$3
  shift 3
  local out
  out=$(printf '%s\n' "$@" 'examine R0' 'examine PSW' 'exit' | timeout 10 "$sim" 2>&1)
  check "$out" 'Simulation stopped' "$label stop"
  check "$out" "R0:.*${r0}$" "$label R0"
  check "$out" "PSW:.*${psw}$" "$label PSW"
  }

# 077777 + 1 = 0100000: signed overflow V, N set, no carry.
run_case add-word-overflow 32768 0234 \
  'deposit 0 000257' 'deposit 2 012700' 'deposit 4 077777' \
  'deposit 6 062700' 'deposit 8 1' 'deposit 10 000000' 'go'

# 0 - 1 = 0177777: borrow C and N, but no signed overflow.
run_case sub-word-underflow 65535 0233 \
  'deposit 0 000257' 'deposit 2 012700' 'deposit 4 0' \
  'deposit 6 0162700' 'deposit 8 1' 'deposit 10 000000' 'go'

# CMPB is opcode 012. Before the fix it trapped and PSW became 0.
run_case cmpb-equal 8 0228 \
  'deposit 0 000257' 'deposit 2 0112700' 'deposit 4 10' \
  'deposit 6 0122700' 'deposit 8 10' 'deposit 10 000000' 'go'

# Word CMP for regression of the same flag path.
run_case cmp-equal 8 0228 \
  'deposit 0 000257' 'deposit 2 012700' 'deposit 4 10' \
  'deposit 6 022700' 'deposit 8 10' 'deposit 10 000000' 'go'

run_case sec 0 0225 \
  'deposit 0 000257' 'deposit 2 000261' 'deposit 4 000000' 'go'

run_case sez-clz 0 0224 \
  'deposit 0 000257' 'deposit 2 000264' 'deposit 4 000244' 'deposit 6 000000' 'go'

echo 'PASS: E10025 CPU flags N/Z/V/C and byte-form decode'

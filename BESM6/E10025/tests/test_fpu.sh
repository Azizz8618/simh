#!/usr/bin/env bash
# FPU regression for the Электроника-100-25 simulator.
# Book 3, section 3.9 (PDF pages 106-108): FADD/FSUB/FMUL/FDIV use R as a
# stack pointer, B=(R,R+2), A=(R+4,R+6), result replaces A and R advances by 4.
# Divide by zero leaves the stack untouched. PDP-11 excess-128 floats:
# 1.0=040200, 2.0=040400, 3.0=040500, 6.0=040700 in the high word.
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

case_run () {
  local label=$1 op=$2 b=$3 a=$4 want_a=$5 want_r=$6
  local out
  out=$(printf '%s\n' \
    'deposit R5 1000' \
    "deposit 1000 $b" 'deposit 1002 0' \
    "deposit 1004 $a" 'deposit 1006 0' \
    "deposit 0 $op" 'deposit 2 000000' \
    'go' 'examine 1004' 'examine R5' 'examine PSW' 'exit' |
    timeout 10 "$sim" 2>&1)
  check "$out" 'Simulation stopped' "$label stop"
  check "$out" "1004:.*${want_a}\$" "$label result A"
  check "$out" "R5:.*${want_r}\$" "$label stack pointer"
  check "$out" 'PSW:.*00224' "$label flags"
  }

case_run fadd 075005 040200 040400 040500 01004       # 2.0 + 1.0 = 3.0
case_run fsub 075105 040200 040400 040200 01004       # 2.0 - 1.0 = 1.0
case_run fmul 075205 040500 040400 040700 01004       # 2.0 * 3.0 = 6.0
case_run fdiv 075305 040400 040700 040500 01004       # 6.0 / 2.0 = 3.0
case_run fdiv-zero 075305 000000 040400 040400 01000  # stack untouched

echo 'PASS: E10025 FPU FADD/FSUB/FMUL/FDIV and divide-by-zero stack rule'

#!/usr/bin/env bash
# Minimal CPU smoke test for the Электроника-100-25 simulator.
# Program: MOV #10,R0; ADD #10,R0; CMP R0,#20; BEQ taken; MOV #2,R1; HALT.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "simulator not built: $sim" >&2
  exit 1
fi
out=$(printf '%s\n' \
  'deposit 0 012700' 'deposit 2 010' \
  'deposit 4 062700' 'deposit 6 010' \
  'deposit 8 020027' 'deposit 10 020' \
  'deposit 12 001404' \
  'deposit 14 012701' 'deposit 16 1' 'deposit 18 000000' \
  'deposit 22 012701' 'deposit 24 2' 'deposit 26 000000' \
  'go' 'examine R0' 'examine R1' 'examine PSW' 'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E 'Simulation stopped|R0:|R1:|PSW:'
for expected in 'R0:.*00016' 'R1:.*00002'; do
  if ! printf '%s\n' "$out" | grep -Eq "$expected"; then
    echo "FAIL: expected $expected" >&2
    exit 1
    fi
  done
if ! printf '%s\n' "$out" | grep -q 'Simulation stopped'; then
  echo "FAIL: simulation did not stop" >&2
  exit 1
fi
echo 'PASS: E10025 CPU MOV/ADD/CMP/BEQ/BRANCH/HALT'

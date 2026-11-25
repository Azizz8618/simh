#!/usr/bin/env bash
# Calibration file and two speed modes from the 1998 article.
# Program is the MOV/ADD/CMP/BEQ/HALT sequence from tests/test_cpu.sh.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "simulator not built: $sim" >&2
  exit 1
fi
tmpdir=/tmp/.private/azizz/opencode/emvs
mkdir -p "$tmpdir"
cal=$(mktemp "$tmpdir/e10025-cal.XXXXXX")
dbg=$(mktemp "$tmpdir/e10025-cal-debug.XXXXXX")
trap 'rm -f "$cal" "$dbg"' EXIT

printf '%s\n' '012700 3' '062700 2' '001404 1' > "$cal"

program_commands () {
  printf '%s\n' \
    'deposit 0 012700' 'deposit 2 010' \
    'deposit 4 062700' 'deposit 6 010' \
    'deposit 8 020027' 'deposit 10 020' \
    'deposit 12 001404' \
    'deposit 14 012701' 'deposit 16 1' 'deposit 18 000000' \
    'deposit 22 012701' 'deposit 24 2' 'deposit 26 000000'
}

ticks_of () {
  printf '%s\n' "$1" | awk -F: '/TICKS:/ {gsub(/^[ \t]+/, "", $2); print ($2+0); exit}'
}

check_regs () {
  label=$1
  output=$2
  for expected in 'R0:.*00016' 'R1:.*00002'; do
    if ! printf '%s\n' "$output" | grep -Eq "$expected"; then
      echo "FAIL: $label expected $expected" >&2
      exit 1
      fi
    done
}

hist_out=$( {
  printf '%s\n' "set cpu calfile=$cal" 'set cpu mode=historical' \
                "set debug $dbg" 'set cpu debug'
  program_commands
  printf '%s\n' 'go' 'examine R0' 'examine R1' 'examine CPU TICKS' 'exit'
} | "$sim" 2>&1)

fast_out=$( {
  printf '%s\n' "set cpu calfile=$cal" 'set cpu mode=fast'
  program_commands
  printf '%s\n' 'go' 'examine R0' 'examine R1' 'examine CPU TICKS' 'exit'
} | "$sim" 2>&1)

hist_ticks=$(ticks_of "$hist_out")
fast_ticks=$(ticks_of "$fast_out")
if [ -z "$hist_ticks" ] || [ "$hist_ticks" -le 0 ]; then
  echo "FAIL: historical TICKS is not greater than 0: $hist_ticks" >&2
  exit 1
fi
if [ -z "$fast_ticks" ] || [ "$fast_ticks" -ne 0 ]; then
  echo "FAIL: fast TICKS is not 0: $fast_ticks" >&2
  exit 1
fi
for line in 'calib: op 012700 delta 3' \
            'calib: op 062700 delta 2' \
            'calib: op 001404 delta 1'; do
  if ! grep -Fq "$line" "$dbg"; then
    echo "FAIL: debug log missing $line" >&2
    exit 1
    fi
done

check_regs historical "$hist_out"
check_regs fast "$fast_out"
echo 'PASS: E10025 calibration historical/fast modes'

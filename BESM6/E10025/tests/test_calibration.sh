#!/usr/bin/env bash
# Three CPU speed modes: ticks (1998 opcode deltas), historical (book 3 us
# scheduled on the SIMH clock) and fast.  The program is the MOV/ADD/CMP/BEQ/
# HALT sequence from tests/test_cpu.sh.  Host wall-clock is not asserted: the
# deterministic proof is the scheduled event in the debug log and CPU TICKS.
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
hdbg=$(mktemp "$tmpdir/e10025-cal-book-debug.XXXXXX")
trap 'rm -f "$cal" "$dbg" "$hdbg"' EXIT

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

gap_commands () {
  printf '%s\n' \
    'deposit 0 012700' 'deposit 2 5' \
    'deposit 4 070027' 'deposit 6 3' \
    'deposit 8 071027' 'deposit 10 3' \
    'deposit 12 072027' 'deposit 14 1' \
    'deposit 16 073027' 'deposit 18 1' \
    'deposit 20 000000'
}

ticks_of () {
  printf '%s\n' "$1" | awk -F: '/TICKS:/ {gsub(/^[ \t]+/, "", $2); print ($2+0); exit}'
}

regs_of () {
  printf '%s\n' "$1" | grep -E 'R0:|R1:' | tr -d '\r'
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

# ticks mode is the default and keeps the old 1998 file behaviour.  The
# assertion strings below are deliberately unchanged from the old test.
hist_out=$( {
  printf '%s\n' "set cpu calfile=$cal" 'set cpu mode=ticks' \
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

# Default must be ticks, not historical or fast.
default_out=$( {
  printf '%s\n' 'show cpu mode'
  program_commands
  printf '%s\n' 'go' 'examine R0' 'examine R1' 'exit'
} | "$sim" 2>&1)
if ! printf '%s\n' "$default_out" | grep -Eq 'sim>[[:space:]]*ticks'; then
  echo 'FAIL: default CPU mode is not ticks' >&2
  exit 1
fi
check_regs default "$default_out"

# historical mode uses book-3 microseconds and schedules a SIMH clock event.
book_out=$( {
  printf '%s\n' 'set cpu mode=historical' "set debug $hdbg" 'set cpu debug'
  program_commands
  printf '%s\n' 'go' 'examine R0' 'examine R1' 'examine CPU TICKS' 'exit'
} | "$sim" 2>&1)
book_ticks=$(ticks_of "$book_out")
if [ -z "$book_ticks" ] || [ "$book_ticks" -le 0 ]; then
  echo "FAIL: historical book TICKS is not greater than 0: $book_ticks" >&2
  exit 1
fi
if ! grep -Eq 'hist: op [0-7]+ MOV .* active 1' "$hdbg"; then
  echo 'FAIL: historical mode did not schedule a clock event' >&2
  exit 1
fi
check_regs historical "$book_out"

# Functional results must be identical in all three modes.
if [ "$(regs_of "$hist_out")" != "$(regs_of "$book_out")" ] ||
   [ "$(regs_of "$fast_out")" != "$(regs_of "$book_out")" ]; then
  echo 'FAIL: R0/R1 differ between ticks, historical and fast modes' >&2
  exit 1
fi

# [GAP] classes (MUL, DIV, ASH, ASHC) get no delay but must not crash.
gap_fast=$( {
  printf '%s\n' 'set cpu mode=fast'
  gap_commands
  printf '%s\n' 'go' 'examine R0' 'examine R1' 'exit'
} | "$sim" 2>&1)
gap_book=$( {
  printf '%s\n' 'set cpu mode=historical'
  gap_commands
  printf '%s\n' 'go' 'examine R0' 'examine R1' 'exit'
} | "$sim" 2>&1)
if printf '%s\n' "$gap_book" | grep -Eq 'SIM-ERROR|SIM-FATAL|Segmentation'; then
  echo 'FAIL: historical mode crashed on GAP class instructions' >&2
  exit 1
fi
if [ "$(regs_of "$gap_fast")" != "$(regs_of "$gap_book")" ]; then
  echo 'FAIL: GAP class results differ between historical and fast modes' >&2
  exit 1
fi

# The optional memory dispatcher mode must be accepted and visible.
md_out=$( {
  printf '%s\n' 'set cpu memdisp=on' 'show cpu memdisp' \
                 'set cpu memdisp=off' 'show cpu memdisp' 'exit'
} | "$sim" 2>&1)
if ! printf '%s\n' "$md_out" | grep -Eq 'sim>[[:space:]]*on'; then
  echo 'FAIL: set cpu memdisp=on was not accepted' >&2
  exit 1
fi

echo 'PASS: E10025 calibration historical/fast modes'

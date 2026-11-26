#!/usr/bin/env bash
# Compact representative decode checks for book 3 on Электроника-100-25.
# Covers word MOV/ADD/CMP/BEQ, byte MOVB/CMPB, FADD through the FPU stack,
# the high-bit BPL branch, EMT vector dispatch and HALT. This is deliberately
# not a copy of test_cpu/test_flags/test_fpu: only one decode path per group.
# Deposited memory words and memory examines use octal (SIMH memory radix);
# register deposits, PSW and register examines use decimal (register radix).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "FAIL[sim]: simulator not built: $sim" >&2
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

# Word MOV #,R0; ADD #,R0; CMP R0,#; BEQ skips R1=1 and takes R1=2; HALT.
out=$(printf '%s\n' \
  'deposit 0 012700' 'deposit 2 010' \
  'deposit 4 062700' 'deposit 6 010' \
  'deposit 8 020027' 'deposit 10 020' \
  'deposit 12 001404' \
  'deposit 14 012701' 'deposit 16 1' 'deposit 18 000000' \
  'deposit 22 012701' 'deposit 24 2' 'deposit 26 000000' \
  'go' 'examine R0' 'examine R1' 'examine PSW' 'exit' | timeout 10 "$sim" 2>&1)
check "$out" 'Simulation stopped' 'word stop'
check "$out" 'R0:.*00016$' 'word R0'
check "$out" 'R1:.*00002$' 'word R1'
check "$out" 'PSW:.*00224$' 'word PSW'

# Byte forms: MOVB #10,R0 then CMPB #10,R0 sets Z. PSW 228 = 224 + Z(4).
out=$(printf '%s\n' \
  'deposit 0 112700' 'deposit 2 010' \
  'deposit 4 122700' 'deposit 6 010' \
  'deposit 10 000000' \
  'go' 'examine R0' 'examine PSW' 'exit' | timeout 10 "$sim" 2>&1)
check "$out" 'Simulation stopped' 'byte stop'
check "$out" 'R0:.*00008$' 'byte R0'
check "$out" 'PSW:.*00228$' 'byte PSW'

# FADD via R5 stack: B=1.0 at 1000, A=2.0 at 1004, result 3.0 replaces A.
out=$(printf '%s\n' \
  'deposit R5 1000' \
  'deposit 1000 040200' 'deposit 1002 0' \
  'deposit 1004 040400' 'deposit 1006 0' \
  'deposit 0 075005' 'deposit 2 000000' \
  'go' 'examine 1004' 'examine R5' 'examine PSW' 'exit' | timeout 10 "$sim" 2>&1)
check "$out" 'Simulation stopped' 'fadd stop'
check "$out" '1004:.*040500$' 'fadd result'
check "$out" 'R5:.*01004$' 'fadd stack pointer'
check "$out" 'PSW:.*00224$' 'fadd PSW'

# High-bit branch BPL (opcode 010 group): N=0 after MOV #1,R0, so branch
# at 4 with offset 4 lands on the R1=2 path, not the R1=1 path at 6.
out=$(printf '%s\n' \
  'deposit 0 012700' 'deposit 2 1' \
  'deposit 4 100004' \
  'deposit 6 012701' 'deposit 8 1' 'deposit 10 000000' \
  'deposit 14 012701' 'deposit 16 2' 'deposit 18 000000' \
  'go' 'examine R0' 'examine R1' 'examine PSW' 'exit' | timeout 10 "$sim" 2>&1)
check "$out" 'Simulation stopped' 'bpl stop'
check "$out" 'R0:.*00001$' 'bpl R0'
check "$out" 'R1:.*00002$' 'bpl R1'
check "$out" 'PSW:.*00224$' 'bpl PSW'

# EMT vector dispatch. Vector at octal 000030 = decimal 24 holds PC 0310
# octal (decimal 200, where HALT lives) and PSW 011 octal (decimal 9).
# process_traps pushes old PSW (octal 340) and PC (2) at SP-2 and SP-4.
out=$(printf '%s\n' \
  'deposit SP 1000' \
  'deposit 24 310' 'deposit 26 11' \
  'deposit 0 104355' 'deposit 2 000000' \
  'deposit 200 000000' \
  'go' 'examine PC' 'examine PSW' 'examine SP' \
  'examine 996' 'examine 998' 'exit' | timeout 10 "$sim" 2>&1)
check "$out" 'Simulation stopped' 'emt stop'
check "$out" 'PC:.*00202$' 'emt vector PC'
check "$out" 'PSW:.*00009$' 'emt vector PSW'
check "$out" 'SP:.*00996$' 'emt stack pointer'
check "$out" '996:.*000002$' 'emt saved PC'
check "$out" '998:.*000340$' 'emt saved PSW'

echo 'PASS: E10025 decode word MOV/ADD/CMP/BEQ, MOVB/CMPB, FADD, BPL, EMT vector, HALT'

#!/usr/bin/env bash
# Memory-size regression for Phase D (book 4, printed p.38).
# M1 default 125K words, M2 guest area, M3 18-bit RAM banks,
# M4 channel cycles, M5 I/O page unchanged, M6 back to 64K compatibility.
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

# M1: the machine starts with 125K words (128000 decimal), not 64K. The top
# word of that RAM is at octal 0763776, decimal 255998.
out=$(printf '%s\n' \
  'show CPU' \
  'show CPU MEMSIZE' \
  'deposit 0763776 012345' 'examine 0763776' \
  'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E 'MEMSIZE|255998'
check "$out" 'CPU.*MEMSIZE=125K words' 'M1 show CPU default'
check "$out" 'MEMSIZE=125K words' 'M1 show CPU MEMSIZE default'
check "$out" '255998:.*012345' 'M1 top word of 125K RAM'

# M2: the guest 16-bit RAM area and the Phase 2 register window are unchanged.
out=$(printf '%s\n' \
  'deposit 0157776 010101' 'examine 0157776' \
  'deposit 0177700 012345' 'examine 0177700' \
  'examine 0160000' \
  'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E '57342|65472|Address space'
check "$out" '57342:.*010101' 'M2 guest RAM at 0157776'
check "$out" '65472:.*012345' 'M2 guest register window unchanged'

# M3: 18-bit channel banks below and just below the I/O page still work.
out=$(printf '%s\n' \
  'deposit 0300000 012345' 'examine 0300000' \
  'deposit 0757776 067456' 'examine 0757776' \
  'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E '98304|253950'
check "$out" '98304:.*012345' 'M3 bank 1 at 0300000'
check "$out" '253950:.*067456' 'M3 top word at 0757776'

# M4: channel cycles use the same 18-bit RAM banks and do not fabricate errors.
out=$(printf '%s\n' \
  'set CHAN CYCLE=10:0300000:012345' \
  'set CHAN CYCLE=00:0300000' \
  'examine CHAN DATA' 'examine CHAN ERRORS' \
  'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E 'DATA:|ERRORS:'
check "$out" 'DATA:.*05349' 'M4 channel read of bank 1'
check "$out" 'ERRORS:.*00000000000' 'M4 no channel error'

# M5: the I/O page is unchanged by the larger RAM configuration.
out=$(printf '%s\n' \
  'deposit 777776 000340' 'examine 777776' \
  'examine 777700' 'examine 772356' 'examine 772300' \
  'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E '262142|262080|259310|259264|Address space'
check "$out" '262142:.*000340' 'M5 PSW at 777776'
check "$out" '262080:' 'M5 R0 at 777700'
check "$out" '259310:' 'M5 772356'
check "$out" '259264:' 'M5 772300'

# M6: 64K is still an explicit compatibility option with its old boundary.
out=$(printf '%s\n' \
  'set CPU MEMSIZE=64K' \
  'show CPU MEMSIZE' \
  'examine 0300000' \
  'examine 0763776' \
  'deposit 0157776 011111' 'examine 0157776' \
  'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E 'MEMSIZE|57342|Address space'
check "$out" 'MEMSIZE=64K words' 'M6 back to 64K'
check "$out" 'Address space exceeded' 'M6 bank 1 hidden again'
check "$out" '57342:.*011111' 'M6 old guest RAM boundary works'

echo 'PASS: E10025 memory default 125K words and 64K compatibility'

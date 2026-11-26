#!/usr/bin/env bash
# Phase P3 regression: PC11 paper tape reader and punch reused for E10025.
# PTR 017550/0264 reads five bytes into memory; PTP 017554/0270 writes the
# same bytes to a file. Vectors 0070/0074 belong to DMA in this model, so the
# PC11 pair uses 0264/0266. Both keep their BR4 slots and leave DMA/BR alone.
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

expect_out () {
  local out=$1 pat=$2 label=$3
  if ! printf '%s\n' "$out" | grep -Eq "$pat"; then
    printf '%s\n' "$out" >&2
    fail "$label" "expected $pat"
    fi
  }

expect_no_out () {
  local out=$1 pat=$2 label=$3
  if printf '%s\n' "$out" | grep -Eq "$pat"; then
    printf '%s\n' "$out" >&2
    fail "$label" "unexpected $pat"
    fi
  }

reg_val () {
  printf '%s\n' "$1" | sed -n "s/.*$2:[[:space:]]*//p" | sed -n "${3:-1}p"
  }

run_sim () {
  printf '%s\n' "$@" | "$sim" 2>&1
  }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tape="$work/tape.bin"
outfile="$work/ptp.bin"
printf '\001\002\003\004\005' > "$tape"
: > "$outfile"

# 1. Lead registration: devices must exist with the planned CSR and vector.
out=$(run_sim 'show PTR' 'show PTP' 'exit')
expect_no_out "$out" 'SIM-ERROR: Non-existent device' 'registration'
expect_out "$out" 'address=17777550-17777553, vector=264, BR4' \
  'PTR at CSR 0177550, vector 0264, BR4'
expect_out "$out" 'address=17777554-17777557, vector=270, BR4' \
  'PTP at CSR 0177554, vector 0270, BR4'

# 2. One guest program: PTR fills five bytes at 01200, then PTP writes them.
#    Guest I/O-page addresses are 0160000 + the CSR offset. The marker at
#    01100 captures CSR<7> DONE before the reader buffer read clears it.
#    Scaled channel timing is required so the 100-tick service events fire.
#    DMA keeps vector 0070; the PC11 pair uses 0264/0266.
prog=(
  'set CHAN TIMING=scaled'
  "attach PTR0 $tape"
  "attach PTP0 $outfile"
  'deposit 0 012700'  'deposit 2 001200'
  'deposit 4 012701'  'deposit 6 000005'
  'deposit 8 012737'  'deposit 10 000001' 'deposit 12 0177550'
  'deposit 14 032737' 'deposit 16 000200' 'deposit 18 0177550'
  'deposit 20 001774'
  'deposit 22 013737' 'deposit 24 0177550' 'deposit 26 001100'
  'deposit 28 113720' 'deposit 30 0177552'
  'deposit 32 005301' 'deposit 34 001362'
  'deposit 36 012700' 'deposit 38 001200'
  'deposit 40 012701' 'deposit 42 000005'
  'deposit 44 112037' 'deposit 46 0177556'
  'deposit 48 032737' 'deposit 50 000200' 'deposit 52 0177554'
  'deposit 54 001774'
  'deposit 56 005301' 'deposit 58 001370'
  'deposit 60 000000'
  'cont'
  'examine PC' 'examine 640' 'examine 642' 'examine 644'
  'examine 576' 'examine PTR POS' 'examine PTP CSR'
  'examine PTP BUF' 'examine PTP POS'
  'examine CHAN ERRORS' 'examine CHAN BRGRANT'
  'exit'
  )
out=$(run_sim "${prog[@]}")
expect_no_out "$out" 'SIM-ERROR' 'program run'
expect_out "$out" 'Simulation stopped, PC: 00062 \(HALT\)' 'program halt'
for pair in '640:.*001001' '642:.*002003' '644:.*000005' '576:.*000200'; do
  expect_out "$out" "$pair" 'reader bytes and DONE marker'
  done
expect_eq () {
  if [ "$1" != "$2" ]; then
    fail "$3" "got '$1', expected '$2'"
    fi
  }
expect_eq "$(reg_val "$out" POS 1)" '5' 'reader consumed five bytes'
expect_eq "$(reg_val "$out" CSR)" '000200' 'punch DONE latched'
expect_eq "$(reg_val "$out" BUF)" '005' 'punch last byte'
expect_eq "$(reg_val "$out" POS 2)" '5' 'punch wrote five bytes'
expect_eq "$(reg_val "$out" ERRORS)" '00000000000' 'channel errors unchanged'
expect_eq "$(reg_val "$out" BRGRANT)" '000' 'no BR grant disturbed'
bytes=$(od -An -tu1 "$outfile" | tr -s ' \n' ' ' | sed 's/^ //; s/ $//')
expect_eq "$bytes" '1 2 3 4 5' 'punch file contents'

# 3. Interrupt sources and DIB vectors. CPU vector delivery itself is covered
#    by test_channel.sh; here we verify both BR4 requests and adapted vectors.
out=$(run_sim 'deposit PTR INT 1' 'deposit PTP INT 1' \
  'examine PTR INT' 'examine PTP INT' \
  'examine PTR DEVVEC' 'examine PTP DEVVEC' 'exit')
expect_no_out "$out" 'SIM-ERROR' 'interrupt registers'
expect_out "$out" 'INT:.*1' 'PTR interrupt request'
expect_out "$out" 'INT:.*1' 'PTP interrupt request'
expect_out "$out" 'DEVVEC:.*000264' 'PTR DIB vector 0264'
expect_out "$out" 'DEVVEC:.*000270' 'PTP DIB vector 0270'

echo 'PASS: E10025 PTR reads five bytes with DONE and vector 0264'
echo 'PASS: E10025 PTP writes the same bytes with DONE and vector 0270'
echo 'PASS: E10025 paper tape interrupt sources keep BR4 slots and DIB vectors'

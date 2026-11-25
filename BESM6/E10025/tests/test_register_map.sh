#!/usr/bin/env bash
# Register-space regression for Phase 2 (book 4 address map).
# T1 console DIB (book/guest/physical forms), T2 guest access,
# T3 DMA into PSW register, T4 ADR00 byte select.
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

# T1: 22-bit physical, 6-digit book and 16-bit guest forms name the same
# register. simh prints memory addresses in decimal and data in octal.
out=$(printf '%s\n' \
  'deposit 017777700 012345' \
  'deposit 017777702 067456' \
  'deposit 017777776 000340' \
  'deposit 017777774 000340' \
  'examine 017777700' 'examine 017777702' \
  'examine 017777776' 'examine 017777774' \
  'examine 777700' 'examine 0177700' \
  'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E '012345|067456|000340|SIM-ERROR|Address space'
check "$out" '4194240:.*012345' 'T1 physical R0'
check "$out" '4194242:.*067456' 'T1 physical R1'
check "$out" '4194302:.*000340' 'T1 physical PSW'
check "$out" '4194300:.*000340' 'T1 physical STKLIM'
check "$out" '262080:.*012345' 'T1 book 777700'
check "$out" '65472:.*012345' 'T1 guest 0177700'

# T2: the guest program reads R0 through its 16-bit channel address and
# reads STKLIM back after writing it. Correct PDP-11 specifiers are
# 037 (@#A) and 027 (#n); the old audit listing used 077/037 by mistake.
out=$(printf '%s\n' \
  'deposit 0 012700' 'deposit 2 012345' \
  'deposit 4 013701' 'deposit 6 0177700' \
  'deposit 8 012737' 'deposit 10 000340' 'deposit 12 0177774' \
  'deposit 14 013702' 'deposit 16 0177774' \
  'deposit 18 000000' \
  'go' 'examine R1' 'examine R2' 'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E 'R1:|R2:|Simulation stopped'
check "$out" 'R1:.*05349' 'T2 guest R0 read'
check "$out" 'R2:.*00224' 'T2 guest STKLIM read'

# T3: DMA from book address 777776 reaches the PSW register.
out=$(printf '%s\n' \
  'deposit 0 000000' \
  'set DMA AP=777776' \
  'set DMA DATA=000340' \
  'set DMA WC=1' \
  'set DMA GO' \
  'go' 'examine CHAN ERRORS' 'examine 017777776' 'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E 'ERRORS:|4194302:|SIM-ERROR'
check "$out" 'ERRORS:.*00000000000' 'T3 DMA to PSW error count'
check "$out" '4194302:.*000340' 'T3 DMA to PSW value'

# T4: ADR00=1 selects the high byte. The initial word 075745 has high byte
# 0173 (decimal 123) and low byte 0345. Writing 0037 to the odd address
# leaves the low byte intact, so the word becomes 017745.
out=$(printf '%s\n' \
  'deposit 017777700 075745' \
  'deposit 0 0113701' 'deposit 2 0177701' \
  'deposit 4 0112737' 'deposit 6 0037' 'deposit 8 0177701' \
  'deposit 10 000000' \
  'go' 'examine R1' 'examine 017777700' 'exit' | "$sim" 2>&1)
printf '%s\n' "$out" | grep -E 'R1:|017745|00123|Simulation stopped'
check "$out" 'R1:.*00123' 'T4 high byte read'
check "$out" '4194240:.*017745' 'T4 high byte write'

echo 'PASS: E10025 register map T1-T4'

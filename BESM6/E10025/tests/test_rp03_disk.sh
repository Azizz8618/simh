#!/usr/bin/env bash
# RP03 (RP11/RP02/RP03) disk regression for E10025.
#
# [G] Proven simh PDP-11 source PDP11/pdp11_rr.c ported to e10025_rr.c.
# [GAP] The 1998 article's DP is plausibly the RT-11 driver name for
# RP11/RP02/RP03, but the exact 100-25 DP controller document is still missing.
#
# RED BY DESIGN until the lead registers RR in e10025_sys.c and Makefile and
# decides how RR coexists with RHA, which auto_tab also puts at 016700/0254:
# the current binary has no RR device, so step 1 fails with
# "Non-existent device: RR".  Do not report this test as green before then.
#
# After integration this test drives RR through the guest I/O page at
# guest VA 0176710..0176724 (RPDS..RPDA, DIB base 016700, four dummy words),
# writes one 256-word block, reads it back, checks DONE/INT and vector 0254,
# and verifies RK/RL/PTR/PTP/LPT CSR and vector assignments are unchanged.
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

run_sim () {
  printf '%s\n' "$@" | "$sim" 2>&1
  }

work=$(mktemp -d "${TMPDIR:-/tmp}/e10025-rp03.XXXXXX") || \
  fail 'workdir' 'cannot create work directory'
trap 'rm -f "$work/image.dsk" "$work/session.out"; rmdir "$work" 2>/dev/null' EXIT
image="$work/image.dsk"
# RP03 geometry from the proven simh drive table: 406 cyl * 20 surf *
# 10 sect * 512 bytes = 41574400 bytes.  Sparse, so the repo stays small.
truncate -s 41574400 "$image" || fail 'image' 'cannot create disk image'

# 1. Lead registration.  RR is DEV_DIS in the port, so enable it first.
#    The current binary must fail here with "Non-existent device: RR".
out=$(run_sim 'set RR enabled' 'show RR' 'show RR0' 'exit')
expect_no_out "$out" 'Non-existent device: RR' 'registration'
expect_no_out "$out" 'SIM-ERROR' 'registration errors'
expect_out "$out" 'RP11-C, address=17776700-17776737, vector=254, BR5' \
  'RR at DIB base 016700, vector 0254, BR5'
expect_out "$out" 'RP03' 'RP03 is the default drive type'

# 2. One guest program: WRITE one RP03 sector from guest 01000 through the
#    I/O page, then READ it back into guest 02000.  The -(R1) sequence and
#    TSTB (R1)/BPL polling are reused from the proven RP11 boot ROM.
#    R1 starts at 0176726 (RPM1), so CLR -(R1) walks RPDA, RPCA, RPBA, RPWC
#    and RPCS.  RPCS 0103 = WRITE(1)+GO+IE, 0105 = READ(2)+GO+IE.
#    RPWC 0177400 is -256 words, one 512-byte RP03 sector.
prog=(
  'set CHAN TIMING=scaled'
  'set RR enabled'
  "attach RR0 $image"
  # cpu_parse_addr treats fewer than six digits as decimal, so buffer
  # addresses are decimal 512 (= guest 001000) and 1024 (= guest 002000).
  'deposit 512 012345' 'deposit 514 067010'
  'deposit 516 000001' 'deposit 518 077777'
  'deposit 520 052525' 'deposit 522 000000'
  'deposit 524 017777' 'deposit 526 045454'
  'deposit 0 012701'  'deposit 2 0176726'
  'deposit 4 005041'  'deposit 6 005041'
  'deposit 8 012741'  'deposit 10 001000'
  'deposit 12 012741' 'deposit 14 0177400'
  'deposit 16 012741' 'deposit 18 000103'
  'deposit 20 0105711' 'deposit 22 0100376'
  'deposit 24 000000'
  'deposit 26 012701' 'deposit 28 0176726'
  'deposit 30 005041'  'deposit 32 005041'
  'deposit 34 012741'  'deposit 36 002000'
  'deposit 38 012741'  'deposit 40 0177400'
  'deposit 42 012741'  'deposit 44 000105'
  'deposit 46 0105711' 'deposit 48 0100376'
  'deposit 50 000000'
  'deposit PC 0' 'cont'
  'examine RR DONE' 'examine RR IE' 'examine RR INT'
  'deposit PC 26' 'cont'
  'examine 1024' 'examine 1026' 'examine 1028' 'examine 1030'
  'examine 1032' 'examine 1034' 'examine 1036' 'examine 1038'
  'examine RR DONE' 'examine RR IE' 'examine RR INT'
  'examine RR DEVVEC'
  'detach RR0'
  'exit'
  )
out=$(run_sim "${prog[@]}")
printf '%s\n' "$out" > "$work/session.out"
expect_no_out "$out" 'SIM-ERROR' 'guest run'

for pair in \
  '1024:.*012345' '1026:.*067010' '1028:.*000001' '1030:.*077777' \
  '1032:.*052525' '1034:.*000000' '1036:.*017777' '1038:.*045454'; do
  expect_out "$out" "$pair" 'read-back from RP03 sector 0'
  done
expect_out "$out" 'DONE:[[:space:]]+1' 'RR DONE after transfer'
expect_out "$out" 'IE:[[:space:]]+1' 'RR interrupt enable'
expect_out "$out" 'INT:[[:space:]]+1' 'RR BR5 interrupt request'
expect_out "$out" 'DEVVEC:[[:space:]]+000254' 'RR vector 0254'

# 3. The lead must still decide how RR and the RH/RP prototype coexist:
#    auto_tab currently gives both 016700 and vector 0254, so enabling RR
#    while RHA is enabled reports a vector conflict and stops the simulation.
#    This test intentionally does not disable RHA: that is a lead decision.

# 4. Other controllers keep their original CSR and vector assignments.
out=$(run_sim 'show RK' 'show RL' 'show PTR' 'show PTP' \
  'show LPT' 'exit')
expect_no_out "$out" 'SIM-ERROR' 'other devices'
for pair in \
  'address=17777400-17777417, vector=220' \
  'address=17774400-17774411, vector=160' \
  'address=17777550-17777553, vector=264' \
  'address=17777554-17777557, vector=270' \
  'address=17777514-17777517, vector=200'; do
  expect_out "$out" "$pair" 'unchanged CSR/vector'
  done

echo 'PASS: RR enabled, RP03 default, sparse image attached'
echo 'PASS: guest-page block write and read back through RP11 registers'
echo 'PASS: RR DONE, BR5 INT and vector 0254 work'
echo 'PASS: RK/RL/PTR/PTP/LPT CSR and vectors are undisturbed'

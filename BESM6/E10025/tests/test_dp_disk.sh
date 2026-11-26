#!/usr/bin/env bash
# Phase P5 regression: RH11 adapter and RP04/05-class DP disk for E10025.
#
# [GAP] The 1998 article names a 20 MB DP disk controller.  The simh RH11/RP
# register model is a prototype, not proof of exact DP identity [G]: the
# historical DP register map is not in the available sources.
#
# The RH adapter is a channel DIB at guest CSR 0176700 (RHA fixed CSR
# 016700, vector 0254, BR5).  RP has no Unibus DIB of its own; its dispatch
# is installed by e10025_rh.c.  This test drives both through the guest I/O
# page, checks DONE plus the RH interrupt, and verifies that RK/RL/DMA/PTR/
# PTP/LPT keep their existing addresses and vectors.
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

work=$(mktemp -d "${TMPDIR:-/tmp}/e10025-dp.XXXXXX") || \
  fail 'workdir' 'cannot create work directory'
trap 'rm -f "$work/image.dsk" "$work/session.out"; rmdir "$work" 2>/dev/null' EXIT
image="$work/image.dsk"
# RM03 geometry from the proven simh RP drive table: 823 cyl * 5 surf *
# 32 sect * 512 bytes.  Sparse, so no payload file is added to the repo.
truncate -s 67379200 "$image" || fail 'image' 'cannot create disk image'

# 1. Lead registration.  The current binary must fail here with
#    "Non-existent device: RH/RP"; after registration these commands work.
out=$(run_sim 'set RH enabled' 'show RH' 'show RP' 'exit')
expect_no_out "$out" 'Non-existent device: (RH|RP)' 'registration'
expect_no_out "$out" 'SIM-ERROR' 'registration errors'
expect_out "$out" 'address=17776700-17776753, vector=254, BR5' \
  'RH at CSR 0176700, vector 0254, BR5'

# 2. One guest program: WRITE one RP sector from 01000 through the I/O page,
#    then READ it back into 02000.  TSTB polls CS1<7> DONE, as in the proven
#    pdp11_rp.c boot ROM.  Scaled channel timing lets the service events fire.
#    CS1 value 0161 = WRITE(030<<1) + GO + IE; 0171 = READ(034<<1) + GO + IE.
#    WC 0177400 is -256 words, one 512-byte RP sector.
prog=(
  'set CHAN TIMING=scaled'
  "attach RP0 $image"
  # cpu_parse_addr treats fewer than six digits as decimal, so the source
  # buffer uses decimal 512 (= guest BA 001000 octal) and 514, 516, ...
  'deposit 512 012345' 'deposit 514 067010'
  'deposit 516 000001' 'deposit 518 077777'
  'deposit 520 052525' 'deposit 522 000000'
  'deposit 524 017777' 'deposit 526 045454'
  'deposit 0 012700'  'deposit 2 001000'
  'deposit 4 012701'  'deposit 6 0176700'
  'deposit 8 010061'  'deposit 10 000004'
  'deposit 12 012761' 'deposit 14 0177400' 'deposit 16 000002'
  'deposit 18 005061' 'deposit 20 000006'
  'deposit 22 005061' 'deposit 24 000034'
  'deposit 26 012711' 'deposit 28 000161'
  'deposit 30 0105711' 'deposit 32 0100376'
  'deposit 34 000000'
  'deposit 36 012700' 'deposit 38 002000'
  'deposit 40 010061' 'deposit 42 000004'
  'deposit 44 012761' 'deposit 46 0177400' 'deposit 48 000002'
  'deposit 50 005061' 'deposit 52 000006'
  'deposit 54 005061' 'deposit 56 000034'
  'deposit 58 012711' 'deposit 60 000171'
  'deposit 62 0105711' 'deposit 64 0100376'
  'deposit 66 000000'
  'deposit PC 0' 'cont'
  'examine RH DONE' 'examine RH IFF' 'examine RH INT'
  'deposit PC 36' 'cont'
  'examine 1024' 'examine 1026' 'examine 1028' 'examine 1030'
  'examine 1032' 'examine 1034' 'examine 1036' 'examine 1038'
  'examine RH DONE' 'examine RH DEVVEC' 'examine RH IE'
  'examine RH WC'
  'detach RP0'
  'exit'
  )
out=$(run_sim "${prog[@]}")
printf '%s\n' "$out" > "$work/session.out"
expect_no_out "$out" 'SIM-ERROR' 'guest run'

for pair in \
  '1024:.*012345' '1026:.*067010' '1028:.*000001' '1030:.*077777' \
  '1032:.*052525' '1034:.*000000' '1036:.*017777' '1038:.*045454'; do
  expect_out "$out" "$pair" 'read-back from RP sector 0'
  done
expect_out "$out" 'DONE:[[:space:]]+1' 'RH DONE after transfer'
expect_out "$out" 'DEVVEC:[[:space:]]+000254' 'RH vector 0254'
# IFF is the CSTB INTR flop set by the DONE transition while IE was set;
# its output raises the BR5 request in int_req.
expect_out "$out" 'IFF:[[:space:]]+1' 'RH CSTB interrupt flag'
expect_out "$out" 'INT:[[:space:]]+1' 'RH BR5 interrupt request'
expect_out "$out" 'IE:[[:space:]]+1' 'RH interrupt enable'

# 3. The other controllers keep their original CSR and vector assignments.
out=$(run_sim 'show RK' 'show RL' 'show DMA' 'show PTR' 'show PTP' \
  'show LPT' 'examine CHAN ERRORS' 'examine CHAN BRGRANT' 'exit')
expect_no_out "$out" 'SIM-ERROR' 'other devices'
for pair in \
  'address=17777400-17777417, vector=220' \
  'address=17774400-17774411, vector=160' \
  'address=17777500-17777507, vector=70' \
  'address=17777550-17777553, vector=264' \
  'address=17777554-17777557, vector=270' \
  'address=17777514-17777517, vector=200'; do
  expect_out "$out" "$pair" 'unchanged CSR/vector'
  done
expect_out "$out" 'ERRORS:[[:space:]]+00000000000' 'channel errors unchanged'
expect_out "$out" 'BRGRANT:[[:space:]]+000' 'no BR grant disturbed'

echo 'PASS: RH enabled, RP0 attached, guest-page block write and read back'
echo 'PASS: RH DONE, CSTB interrupt and vector 0254 work'
echo 'PASS: RK/RL/DMA/PTR/PTP/LPT CSR and vectors are undisturbed'

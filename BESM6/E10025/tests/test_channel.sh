#!/usr/bin/env bash
# Regression test for the unified channel model (book 4): DMA block transfer,
# vector delivery for DMA and BR7..BR4, IPL-based arbitration, no sticky BR,
# slot separation from CLK/TTI/PIRQ7 and the channel cycle/error counters.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "simulator not built: $sim" >&2
  exit 1
fi

run_sim () {
  printf '%s\n' "$@" | "$sim" 2>&1
}

expect_out () {
  local out=$1 pat=$2 label=$3
  if ! printf '%s\n' "$out" | grep -Eq "$pat"; then
    echo "FAIL: $label (expected $pat)" >&2
    printf '%s\n' "$out" >&2
    exit 1
  fi
}

expect_grep () {
  local file=$1 pat=$2 label=$3
  if ! grep -Eq "$pat" "$file"; then
    echo "FAIL: $label (expected $pat in $file)" >&2
    exit 1
  fi
}

expect_no_grep () {
  local file=$1 pat=$2 label=$3
  if grep -Eq "$pat" "$file"; then
    echo "FAIL: $label (unexpected $pat in $file)" >&2
    exit 1
  fi
}

# 1. Baseline data path and counters. No CHAN IPL override: arbitration now
# follows the PSW, not a synthetic static level.
out=$(run_sim \
  'set DMA ADDRESS=17777500' \
  'set DMA VECTOR=070' \
  'deposit 0 000000' \
  'set DMA DATA=12525' \
  'set DMA AP=020000' \
  'set DMA WC=3' \
  'set DMA GO' \
  'go' \
  'examine 8192' 'examine 8194' 'examine 8196' \
  'examine DMA CSR' 'examine CHAN CYCLES' 'examine CHAN ERRORS' \
  'exit')
printf '%s\n' "$out" | grep -E '8192:|8194:|8196:|CSR:|CYCLES:|ERRORS:|Simulation stopped'
for expected in \
  '8192:.*012525' '8194:.*012525' '8196:.*012525' \
  'CSR:.*0080' 'CYCLES:.*00000000003' 'ERRORS:.*00000000000'; do
  expect_out "$out" "$expected" 'DMA data/counter regression'
done

# RTI is used to lower the real PSW IPL: the console PSW register shadows the
# variable but does not call put_PSW, so it cannot change ipl before GO.
# Console addresses are decimal, deposit values are octal; set modifiers are
# octal. Octal 01600/01602/01000/01020/01040 become 896/898/512/528/544.
rti_setup=(
  'deposit 0 000002'
  'deposit 2 000000'
  'deposit SP 896'
  'deposit 896 000002'
  'deposit 898 000000'
)

# 2. DMA vector must reach the CPU. PC is printed in decimal by simh, so
# octal 01000 + HALT gives PC 00514.
out=$(run_sim \
  "${rti_setup[@]}" \
  'deposit 56 01000' 'deposit 58 000000' 'deposit 512 000000' \
  'set DMA VECTOR=070' \
  'set DMA AP=020000' 'set DMA WC=1' 'set DMA DATA=12525' 'set DMA GO' \
  'go' 'examine PC' 'examine DMA CSR' 'examine PSW' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:|CSR:|PSW:'
expect_out "$out" 'PC:.*00514' 'DMA vector delivery'
expect_out "$out" 'CSR:.*0080' 'DMA DONE latched after acknowledge'

# 3. BR4 vector and grant, then a second GO without a new BRREQ: no storm.
out=$(run_sim \
  "${rti_setup[@]}" \
  'deposit 76 01020' 'deposit 78 000000' 'deposit 528 000000' \
  'set CHAN BRVEC=0114' 'set CHAN BRREQ=0020' \
  'go' 'examine PC' 'examine CHAN BRGRANT' \
  'go' 'examine PC' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:|BRGRANT:'
expect_out "$out" 'PC:.*00530' 'BR4 vector delivery'
expect_out "$out" 'BRGRANT:.*004' 'BR4 grant level'
expect_out "$out" 'PC:.*00532' 'no interrupt storm after BR4 acknowledge'

# 4. IPL 5 blocks BR4 but not BR7 (UNIBUS rule).
out=$(run_sim \
  'deposit 0 000002' 'deposit 2 000000' \
  'deposit SP 896' 'deposit 896 000002' 'deposit 898 000240' \
  'set CHAN BRVEC=0114' 'set CHAN BRREQ=0020' \
  'go' 'examine PC' 'examine CHAN BRGRANT' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:|BRGRANT:'
expect_out "$out" 'PC:.*00004' 'BR4 blocked at IPL 5'
expect_out "$out" 'BRGRANT:.*000' 'no BR4 grant at IPL 5'

out=$(run_sim \
  'deposit 0 000002' 'deposit 2 000000' \
  'deposit SP 896' 'deposit 896 000002' 'deposit 898 000240' \
  'deposit 76 01020' 'deposit 78 000000' 'deposit 528 000000' \
  'set CHAN BRVEC=0114' 'set CHAN BRREQ=0200' \
  'go' 'examine PC' 'examine CHAN BRGRANT' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:|BRGRANT:'
expect_out "$out" 'PC:.*00530' 'BR7 vector delivery at IPL 5'
expect_out "$out" 'BRGRANT:.*007' 'BR7 grant level'

# 5. BR7 and DMA share level 7 but not a slot. DMA wins on the lower bit,
# BR7 is delivered on the next GO; neither overwrites the other's vector.
out=$(run_sim \
  "${rti_setup[@]}" \
  'deposit 56 01000' 'deposit 58 000000' 'deposit 512 000000' \
  'deposit 84 01040' 'deposit 86 000000' 'deposit 544 000000' \
  'set DMA VECTOR=070' 'set DMA AP=020000' 'set DMA WC=1' 'set DMA DATA=12525' 'set DMA GO' \
  'set CHAN BRVEC=0124' 'set CHAN BRREQ=0200' \
  'go' 'examine PC' 'examine CHAN BRGRANT' \
  'go' 'examine PC' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:|BRGRANT:'
expect_out "$out" 'PC:.*00514' 'DMA vector survives pending BR7'
expect_out "$out" 'BRGRANT:.*007' 'BR7 grant with DMA pending'
expect_out "$out" 'PC:.*00546' 'BR7 vector after DMA acknowledge'

# 6. CLK ([6][1], vector 0100) and TTI ([4][1], vector 0060) keep their slots
# while BR6 and BR4 are injected. CLK/TTI requests are raised directly through
# their INT registers; the event queue is not needed for them.
out=$(run_sim \
  "${rti_setup[@]}" \
  'deposit 64 01200' 'deposit 66 000000' 'deposit 640 000000' \
  'deposit 48 01240' 'deposit 50 000000' 'deposit 672 000000' \
  'deposit 92 01300' 'deposit 94 000000' 'deposit 704 000000' \
  'deposit CLK INT 1' 'deposit TTI INT 1' \
  'set CHAN BRVEC=0134' 'set CHAN BRREQ=0120' \
  'go' 'examine PC' \
  'go' 'examine PC' \
  'go' 'examine PC' \
  'go' 'examine PC' 'examine CHAN BRGRANT' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:|BRGRANT:'
mapfile -t pcs < <(printf '%s\n' "$out" | sed -n 's/^sim> PC:[[:space:]]*//p')
expect_out "${pcs[0]:-}" '^00642$' 'CLK vector with BR6 pending'
expect_out "${pcs[1]:-}" '^00706$' 'BR6 vector delivered'
expect_out "${pcs[2]:-}" '^00674$' 'TTI vector with BR4 pending'
expect_out "${pcs[3]:-}" '^00706$' 'BR4 vector delivered'
expect_out "$out" 'BRGRANT:.*004' 'BR4 grant after BR6'

# 7. Static guards for the slot layout and for the documented limitation.
# CLK owns [6][1], TTI owns [4][1], DMA owns [7][1] and PIRQ7 owns [7][0].
expect_grep "$here/e10025_chan.c" 'BR_INT' 'synthetic BR bit defined'
expect_grep "$here/e10025_chan.c" 'return br_vec' 'BR acknowledge returns vector'
expect_grep "$here/e10025_chan.c" 'return dma_dib.vec' 'DMA acknowledge returns vector'
expect_grep "$here/e10025_chan.c" 'int_vec\[level\]\[31\]' 'BR uses slot 31'
expect_grep "$here/e10025_chan.c" 'daisy' 'level-only arbitration documented'
expect_no_grep "$here/e10025_chan.c" 'int_vec\[level\]\[1\]' 'BR does not take CLK/TTI/DMA bit 1 slot'
expect_no_grep "$here/e10025_chan.c" 'int_vec\[7\]\[0\]' 'DMA does not overwrite PIRQ7 slot'
expect_grep "$here/e10025_cpu.c" 'br_arbitrate' 'arbitration recomputed from PSW'
expect_no_grep "$here/e10025_cpu.c" 'chan_ack_interrupt' 'no double acknowledge'

echo 'PASS: E10025 channel DMA/BR arbitration'

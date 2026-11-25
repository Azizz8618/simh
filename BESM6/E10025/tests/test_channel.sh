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

expect_fixed () {
  local out=$1 text=$2 label=$3
  if ! printf '%s\n' "$out" | grep -Fq -- "$text"; then
    echo "FAIL: $label (expected '$text')" >&2
    printf '%s\n' "$out" >&2
    exit 1
  fi
}

expect_eq () {
  local got=$1 want=$2 label=$3
  if [ "$got" != "$want" ]; then
    echo "FAIL: $label (got '$got', expected '$want')" >&2
    exit 1
  fi
}

expect_no_out () {
  local out=$1 pat=$2 label=$3
  if printf '%s\n' "$out" | grep -Eq "$pat"; then
    echo "FAIL: $label (unexpected $pat)" >&2
    printf '%s\n' "$out" >&2
    exit 1
  fi
}

# Phase/opcode journal (book 4, variant B). Durations printed in the journal
# are historical book-4 values, not host delays or simulator clock ticks.
debug_log=$(mktemp)
trap 'rm -f "$debug_log"' EXIT

run_sim_debug () {
  : > "$debug_log"
  printf '%s\n' "set debug $debug_log" 'set CHAN debug' "$@" 'exit' | "$sim" 2>&1
}

reg_val () {
  local out=$1 name=$2 nth=${3:-1}
  printf '%s\n' "$out" | sed -n "s/.*$name:[[:space:]]*//p" | sed -n "${nth}p"
}

journal_line () {
  grep "CHAN JOURNAL: cycle op=$1 " "$debug_log" | head -1
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

# 8. One ВВОД (opcode 00): exact phase order, cycle count +1, no error and no
# timeout increase. The journal is a logical record; its durations are the
# historical book-4 values and are not host nanoseconds or simulator ticks.
out=$(run_sim_debug \
  'deposit 8192 012525' \
  'examine CHAN CYCLES' 'examine CHAN ERRORS' 'examine CHAN TIMEOUT' \
  'set CHAN CYCLE=00:020000' \
  'examine CHAN CYCLES' 'examine CHAN ERRORS' 'examine CHAN TIMEOUT' \
  'examine CHAN DATA')
printf '%s\n' "$out" | grep -E 'CYCLES:|ERRORS:|TIMEOUT:|DATA:'
before=$(reg_val "$out" CYCLES 1)
after=$(reg_val "$out" CYCLES 2)
if [ $((10#$after)) -ne $((10#$before + 1)) ]; then
  echo "FAIL: ВВОД increments CYCLES by one ($before -> $after)" >&2
  exit 1
fi
expect_eq "$(reg_val "$out" ERRORS 2)" '00000000000' 'ВВОД leaves ERRORS unchanged'
expect_eq "$(reg_val "$out" TIMEOUT 2)" '00000000000' 'ВВОД leaves TIMEOUT unchanged'
line=$(journal_line 00)
printf '%s\n' "$line"
expect_out "$line" '^DBG\(.*\)> CHAN JOURNAL: cycle ' 'journal uses SIMH debug output'
expect_fixed "$line" 'op=00 addr=020000 adr00=0' 'ВВОД opcode, address and ADR00'
expect_fixed "$line" 'phases=ADDRESS,SA,WAIT_SACK,STROBE,RELEASE,CLEANUP,IDLE' \
  'exact ВВОД phase order'
expect_fixed "$line" 'result=OK' 'ВВОД has no ERROR phase'
expect_fixed "$line" 'historical_book4_ns=150/75/75/75 repeat=450 full=525 timeout_us=15000' \
  'book-4 durations are labeled historical'
expect_fixed "$line" 'not host ns or simulator clocks' 'durations are not host or simulator time'
if printf '%s\n' "$line" | grep -q 'ERROR'; then
  echo 'FAIL: ВВОД journal contains ERROR' >&2
  exit 1
fi

# 9. One ВЫВОД (opcode 10): same phase order, opcode 10 and the written word.
out=$(run_sim_debug \
  'deposit 8192 000000' \
  'set CHAN CYCLE=10:020000:012525' \
  'examine 8192' 'examine CHAN CYCLES' 'examine CHAN ERRORS')
printf '%s\n' "$out" | grep -E '8192:|CYCLES:|ERRORS:'
expect_out "$out" '8192:.*012525' 'ВЫВОД word stored in memory'
expect_eq "$(reg_val "$out" CYCLES 1)" '00000000001' 'ВЫВОД counts one cycle'
expect_eq "$(reg_val "$out" ERRORS 1)" '00000000000' 'ВЫВОД has no error'
line=$(journal_line 10)
printf '%s\n' "$line"
expect_fixed "$line" 'op=10 addr=020000 adr00=0' 'ВЫВОД opcode and address'
expect_fixed "$line" 'phases=ADDRESS,SA,WAIT_SACK,STROBE,RELEASE,CLEANUP,IDLE' \
  'exact ВЫВОД phase order'

# 10. Four book-4 codes and ADR00 byte selection for 11 (ВЫВОДБ). Both the
# memory path and the I/O-page path must select the low byte for an even
# address and the high byte for an odd address.
out=$(run_sim_debug \
  'deposit 8192 000000' \
  'set CHAN CYCLE=00:020000' \
  'set CHAN CYCLE=01:020000' \
  'set CHAN CYCLE=10:020000:012525' \
  'set CHAN CYCLE=11:020001:000011')
printf '%s\n' "$out" | grep -E 'CHAN JOURNAL' >/dev/null
for op in 00 01 10 11; do
  expect_fixed "$(journal_line "$op")" "op=$op " "book-4 opcode $op journaled"
done

out=$(run_sim_debug \
  'deposit 8192 000000' \
  'set CHAN CYCLE=11:020000:000011' 'examine 8192' \
  'set CHAN CYCLE=11:020001:012525' 'examine 8192')
printf '%s\n' "$out" | grep -E '8192:'
mapfile -t words < <(printf '%s\n' "$out" | sed -n 's/.*8192:[[:space:]]*//p')
expect_eq "${words[0]:-}" '000011' 'ВЫВОДБ even address changes low byte only'
expect_eq "${words[1]:-}" '052411' 'ВЫВОДБ odd address changes high byte only'

out=$(run_sim_debug \
  'set DMA DATA=000000' \
  'set CHAN CYCLE=11:017777502:000022' 'examine DMA DAT' \
  'set CHAN CYCLE=11:017777503:000033' 'examine DMA DAT')
printf '%s\n' "$out" | grep -E 'DAT:'
mapfile -t regs < <(printf '%s\n' "$out" | sed -n 's/.*DAT:[[:space:]]*//p')
expect_eq "${regs[0]:-}" '0012' 'ВЫВОДБ I/O page even address changes low byte'
expect_eq "${regs[1]:-}" '1B12' 'ВЫВОДБ I/O page odd address changes high byte'
expect_fixed "$(grep 'CHAN JOURNAL: cycle op=11 addr=777503' "$debug_log" | head -1)" \
  'adr00=1' 'ВЫВОДБ odd address sets ADR00'

# 11. ВВОДП (opcode 01) is journaled with its following code and address, but
# the next-cycle pair is not enforced by default: ERRORS must stay zero even
# when the following code is 00 at a different address.
out=$(run_sim_debug \
  'deposit 8192 000000' \
  'set CHAN CYCLE=01:020000' \
  'set CHAN CYCLE=10:020000:012525' \
  'set CHAN CYCLE=01:020002' \
  'set CHAN CYCLE=00:020004' \
  'examine CHAN ERRORS' 'examine CHAN TIMEOUT')
printf '%s\n' "$out" | grep -E 'ERRORS:|TIMEOUT:'
expect_fixed "$(journal_line 01)" 'vbo_next=pending' 'ВВОДП journal marks soft pair'
mapfile -t pairs < <(grep 'CHAN JOURNAL: vbo_pair' "$debug_log")
expect_fixed "${pairs[0]:-}" 'vbo_pair op=01 addr=020000 next=10@020000' \
  'next code after ВВОДП is 10 at the same address'
next_code=$(printf '%s\n' "${pairs[0]:-}" | sed -n 's/.*next=\([0-9][0-9]\)@.*/\1/p')
case "$next_code" in
  10|11) ;;
  *) echo "FAIL: next code after ВВОДП is $next_code, expected 10 or 11" >&2; exit 1 ;;
esac
expect_fixed "${pairs[1]:-}" 'vbo_pair op=01 addr=020002 next=00@020004' \
  'unpaired ВВОДП is journaled without enforcement'
expect_eq "$(reg_val "$out" ERRORS 1)" '00000000000' 'ВВОДП pair is not enforced'
expect_eq "$(reg_val "$out" TIMEOUT 1)" '00000000000' 'ВВОДП leaves TIMEOUT unchanged'

# 12. DMA: exactly WC cycles with opcode 10 and addresses AP, AP+2, ...
out=$(run_sim_debug \
  'set DMA AP=020000' 'set DMA WC=3' 'set DMA DATA=012525' 'set DMA GO' \
  'examine CHAN CYCLES' 'examine CHAN ERRORS' 'examine CHAN TIMEOUT')
printf '%s\n' "$out" | grep -E 'CYCLES:|ERRORS:|TIMEOUT:'
expect_out "$out" 'CYCLES:.*00000000003' 'DMA journal cycle count matches WC'
expect_out "$out" 'ERRORS:.*00000000000' 'DMA journal reports no errors'
expect_out "$out" 'TIMEOUT:.*00000000000' 'DMA leaves TIMEOUT unchanged'
mapfile -t addrs < <(grep 'CHAN JOURNAL: cycle op=10' "$debug_log" | \
  sed -n 's/.* addr=\([0-7]*\).*/\1/p')
expect_eq "${#addrs[@]}" 3 'DMA journal has exactly WC cycle lines'
expect_eq "${addrs[0]:-}" '020000' 'DMA first journal address is AP'
expect_eq "${addrs[1]:-}" '020002' 'DMA second journal address is AP+2'
expect_eq "${addrs[2]:-}" '020004' 'DMA third journal address is AP+4'
non10=$(grep 'CHAN JOURNAL: cycle op=' "$debug_log" | grep -vc 'op=10 ')
expect_eq "$non10" 0 'DMA journal contains only opcode 10'

# 13. Phase B modifiers are accepted. Logical is the default; scaled uses a
# declared scale of one simulator tick = 75 ns, not host nanoseconds.
out=$(run_sim \
  'set CHAN TIMING=scaled' 'show CHAN TIMING' \
  'set CHAN NOSP=1' 'show CHAN NOSP' \
  'set CHAN TIMING=logical' 'show CHAN TIMING' \
  'set CHAN NOSP=0' 'show CHAN NOSP' \
  'exit')
printf '%s\n' "$out" | grep -E 'TIMING=|NOSP='
expect_no_out "$out" 'SIM-ERROR' 'TIMING and NOSP modifiers are accepted'
expect_out "$out" 'TIMING=scaled' 'scaled timing accepted'
expect_out "$out" 'NOSP=1' 'NOSP=1 diagnostic accepted'
expect_out "$out" 'TIMING=logical' 'logical timing accepted'
expect_out "$out" 'NOSP=0' 'NOSP cleared'

# 14. One scaled ВВОД still gives the seven book-4 phases and a successful
# exchange leaves TIMEOUT unchanged, both at the console and after GO.
out=$(run_sim_debug \
  'set CHAN TIMING=scaled' \
  'deposit 8192 012525' \
  'examine CHAN TIMEOUT' \
  'set CHAN CYCLE=00:020000' \
  'examine CHAN CYCLES' 'examine CHAN TIMEOUT' 'examine CHAN STATE')
printf '%s\n' "$out" | grep -E 'CYCLES:|TIMEOUT:|STATE:'
line=$(journal_line 00)
printf '%s\n' "$line"
expect_fixed "$line" 'phases=ADDRESS,SA,WAIT_SACK,STROBE,RELEASE,CLEANUP,IDLE' \
  'scaled ВВОД keeps the seven book-4 phases'
expect_fixed "$line" 'timing=scaled scale_ns=75' 'scaled journal declares the tick scale'
expect_fixed "$line" 'historical_book4_ns=150/75/75/75 repeat=450 full=525 timeout_us=15000' \
  'scaled journal keeps the historical durations'
expect_eq "$(reg_val "$out" TIMEOUT 2)" '00000000000' 'scaled ВВОД leaves TIMEOUT unchanged'

out=$(run_sim \
  'set CHAN TIMING=scaled' \
  'set CHAN CYCLE=00:020000' \
  'deposit 0 000000' 'go' \
  'examine CHAN TIMEOUT' 'exit')
expect_out "$out" 'TIMEOUT:.*00000000000' 'successful scaled exchange has no timeout after GO'

# 15. NOSP=1 suppresses К СП Н: the separate 15 us event fires, TIMEOUT grows
# and STATE becomes ERROR. CONTINUE is used because GO resets the event queue.
# Two 65535-iteration SOB loops give enough simulated instructions for 15000 us.
out=$(run_sim \
  'set CHAN TIMING=scaled' \
  'set CHAN NOSP=1' \
  'set CHAN CYCLE=00:020000' \
  'deposit 0 012701' 'deposit 2 0177777' 'deposit 4 077101' \
  'deposit 6 012701' 'deposit 8 0177777' 'deposit 10 077101' \
  'deposit 12 000000' 'deposit PC 0' 'cont' \
  'examine CHAN TIMEOUT' 'examine CHAN STATE' 'exit')
printf '%s\n' "$out" | grep -E 'TIMEOUT:|STATE:'
expect_out "$out" 'TIMEOUT:.*00000000001' 'NOSP=1 increments TIMEOUT'
expect_out "$out" 'STATE:.*007' 'NOSP=1 leaves STATE=ERROR'

# 16. Logical mode still produces the book-4 journal with no timeout.
out=$(run_sim_debug \
  'set CHAN TIMING=logical' \
  'set CHAN CYCLE=00:020000' \
  'examine CHAN TIMEOUT')
expect_fixed "$(journal_line 00)" 'timing=logical scale_ns=75' \
  'logical mode keeps the declared scale in the journal'
expect_eq "$(reg_val "$out" TIMEOUT 1)" '00000000000' 'logical mode leaves TIMEOUT unchanged'

# 17. Phase C: BR daisy-chain inside one level. Diagnostic syntax:
#     set CHAN BRDEV=<level>:<slot>:<vec>
# All three fields are octal. <level> is BR4..BR7, <slot> is the position in
# the daisy chain inside that level (0 is closest to the processor and receives
# ПК first), <vec> is the interrupt vector of that source. BR keeps interrupt
# slot 31; CLK [6][1], TTI [4][1], DMA [7][1] and PIRQ7 [7][0] stay untouched.
out=$(run_sim \
  "${rti_setup[@]}" \
  'deposit 76 01020' 'deposit 78 000000' 'deposit 528 000000' \
  'deposit 84 01040' 'deposit 86 000000' 'deposit 544 000000' \
  'set CHAN BRVEC=0114' \
  'set CHAN BRDEV=4:0:0114' 'set CHAN BRDEV=4:1:0124' \
  'go' 'examine PC' 'examine CHAN BRGRANT' \
  'go' 'examine PC' 'examine CHAN BRGRANT' \
  'go' 'examine PC' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:|BRGRANT:'
expect_no_out "$out" 'SIM-ERROR' 'BRDEV diagnostic syntax is accepted'
mapfile -t brchain_pcs < <(printf '%s\n' "$out" | sed -n 's/^sim> PC:[[:space:]]*//p')
expect_eq "${brchain_pcs[0]:-}" '00530' 'BR daisy-chain: nearest BR4 source wins first'
expect_eq "$(reg_val "$out" BRGRANT 1)" '004' 'BR daisy-chain: first grant is level 4'
expect_eq "${brchain_pcs[1]:-}" '00546' 'BR daisy-chain: second source gets its own vector'
expect_eq "$(reg_val "$out" BRGRANT 2)" '004' 'BR daisy-chain: second grant is level 4'
expect_eq "${brchain_pcs[2]:-}" '00548' 'BR daisy-chain: no storm after both acknowledges'

# The daisy-chain injection must not take the CLK, TTI or DMA slots.
out=$(run_sim \
  "${rti_setup[@]}" \
  'deposit 64 01200' 'deposit 66 000000' 'deposit 640 000000' \
  'deposit 48 01240' 'deposit 50 000000' 'deposit 672 000000' \
  'deposit 56 01300' 'deposit 58 000000' 'deposit 704 000000' \
  'deposit 92 01340' 'deposit 94 000000' 'deposit 736 000000' \
  'deposit 100 01440' 'deposit 102 000000' 'deposit 800 000000' \
  'deposit CLK INT 1' 'deposit TTI INT 1' \
  'set DMA VECTOR=070' 'set DMA AP=020000' 'set DMA WC=1' 'set DMA DATA=12525' 'set DMA GO' \
  'set CHAN BRDEV=4:0:0134' 'set CHAN BRDEV=4:1:0144' \
  'go' 'examine PC' \
  'go' 'examine PC' \
  'go' 'examine PC' \
  'go' 'examine PC' \
  'go' 'examine PC' \
  'exit')
printf '%s\n' "$out" | grep -E 'Simulation stopped|PC:'
mapfile -t brchain_slots < <(printf '%s\n' "$out" | sed -n 's/^sim> PC:[[:space:]]*//p')
expect_eq "${brchain_slots[0]:-}" '00706' 'BR daisy-chain: DMA vector from [7][1]'
expect_eq "${brchain_slots[1]:-}" '00642' 'BR daisy-chain: CLK vector from [6][1]'
expect_eq "${brchain_slots[2]:-}" '00674' 'BR daisy-chain: TTI vector from [4][1]'
expect_eq "${brchain_slots[3]:-}" '00738' 'BR daisy-chain: first BR4 vector from slot 31'
expect_eq "${brchain_slots[4]:-}" '00802' 'BR daisy-chain: second BR4 vector from slot 31'
expect_grep "$here/e10025_chan.c" 'BRDEV' 'BRDEV diagnostic modifier registered'
expect_grep "$here/e10025_chan.c" 'br_src\[level\]\[i\].req' \
  'BR daisy-chain source list used by arbitration'

echo 'PASS: E10025 channel DMA/BR arbitration'
echo 'PASS: E10025 channel Phase B scaled timing and timeout'
echo 'PASS: E10025 channel BR daisy-chain'

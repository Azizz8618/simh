#!/usr/bin/env bash
# B1 regression for the named 56-line channel signal layer (book 4, pp.13-17).
# The layer is an observation only: this script checks that its debug trace
# follows the phase order, that active-low lines start inactive, and that the
# three legacy test_channel.sh PASS strings are preserved.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "simulator not built: $sim" >&2
  exit 1
fi

fail () {
  echo "FAIL: $*" >&2
  exit 1
}

need_line () {
  local dump=$1 pat=$2 label=$3
  if ! printf '%s\n' "$dump" | grep -Eq "$pat"; then
    echo "FAIL: $label (expected $pat)" >&2
    printf '%s\n' "$dump" >&2
    exit 1
  fi
}

# Static guards: the module exists, the count is 56, and the old PASS lines
# stay literally unchanged in test_channel.sh.
grep -Eq '#define[[:space:]]+BUS_SIG_NLINES[[:space:]]+56' \
  "$here/e10025_bus_sig.h" || fail 'BUS_SIG_NLINES is not 56'
grep -Fq 'void bus_sig_phase (' "$here/e10025_bus_sig.h" || \
  fail 'bus_sig_phase API missing'
grep -Fq 'const char *bus_sig_debug_text' "$here/e10025_bus_sig.h" || \
  fail 'debug formatting API missing'
grep -Fq 'SIG module=bus_sig' "$here/e10025_bus_sig.c" || \
  fail 'debug formatter not implemented'
pass_strings=(
  'PASS: E10025 channel DMA/BR arbitration'
  'PASS: E10025 channel Phase B scaled timing and timeout'
  'PASS: E10025 channel BR daisy-chain'
)
for s in "${pass_strings[@]}"; do
  grep -Fq -- "echo '$s'" "$here/tests/test_channel.sh" || \
    fail "legacy PASS string lost: $s"
done

dbg=$(mktemp)
sig=$(mktemp)
trap 'rm -f "$dbg" "$sig"' EXIT
: > "$dbg"

# One ВВОД from memory and one ВЫВОД with an injected word. Debug output of
# the SIG flag is the only interface used by this test.
out=$(printf '%s\n' \
  "set debug $dbg" \
  'set CHAN DEBUG=SIG' \
  'deposit 8192 012525' \
  'set CHAN CYCLE=00:020000' \
  'set CHAN CYCLE=010:020002:012525' \
  'exit' | "$sim" 2>&1)

if printf '%s\n' "$out" | grep -Fq "Invalid DEBUG option 'SIG'"; then
  fail "B1 red: CHAN DEBUG=SIG is not registered yet; signal module not integrated"
fi

sed -E 's/\r$//; s/^DBG\([0-9]+\)> //; s/^[^ ]+ SIG: //' "$dbg" | \
  grep '^SIG ' > "$sig" || \
  fail 'no SIG debug lines: bus_sig trace not registered in chan_run_cycle'

grep -Eq '^SIG module=bus_sig registered=1 lines=56 active_low=[0-9]+ active_high=[0-9]+ phase=' "$sig" || \
  fail 'SIG summary with registered=1 lines=56 missing'

phase_dump_nth () {
  awk -v want="$1" -v nth="${2:-1}" '
    /^SIG module=bus_sig/ {
      show = (index($0, "phase=" want " ") > 0) || ($0 ~ ("phase=" want "$"));
      if (show) { seen++; show = (seen == nth) }
      next
    }
    show && /^SIG line=/ { print }
  ' "$sig"
}

codes=$(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^code=/) print $i }' \
  "$sig" | sort -u | wc -l)
[ "$codes" -eq 56 ] || fail "expected 56 named lines, got $codes"

counts=$(awk '
  /^SIG module=bus_sig/ { if (n > 0) print n; n = 0; next }
  /^SIG line=/ { n++ }
  END { if (n > 0) print n }
' "$sig" | sort -u)
[ "$counts" = '56' ] || fail "each dump must show 56 lines, got: $counts"

idle=$(phase_dump_nth IDLE 1)
low_idle=$(printf '%s\n' "$idle" | grep -c 'active=low level=1 state=inactive')
[ "$low_idle" -ge 50 ] || fail "active-low default levels lost: $low_idle inactive"

addr_in=$(phase_dump_nth ADDRESS 1)
need_line "$addr_in" 'code=ADR13 .*active=low level=0 state=active' \
  'ADR13 active on ADDRESS phase (address 020000)'
need_line "$addr_in" 'code=VV1 .*active=low level=1 state=inactive' \
  'VV1 inactive for ВВОД'
need_line "$addr_in" 'code=VV0 .*active=low level=1 state=inactive' \
  'VV0 inactive for ВВОД'
need_line "$addr_in" 'code=SA .*state=inactive' \
  'СА not yet asserted on ADDRESS phase'
need_line "$(phase_dump_nth SA 1)" 'code=SA .*active=low level=0 state=active' \
  'СА active on SA phase'
need_line "$(phase_dump_nth WAIT_SACK 1)" \
  'code=SP .*active=low level=0 state=active' \
  'К СП Н active on WAIT_SACK phase'
need_line "$(phase_dump_nth STROBE 1)" \
  'code=D[0-9]+ .*active=low level=0 state=active' \
  'data lines active on STROBE phase (ВВОД result)'

cleanup=$(phase_dump_nth CLEANUP 1)
need_line "$cleanup" 'code=SA .*state=inactive' 'СА released on CLEANUP'
need_line "$cleanup" 'code=SP .*state=inactive' 'К СП Н released on CLEANUP'
need_line "$cleanup" 'code=ADR13 .*state=inactive' 'address released on CLEANUP'

addr_out=$(phase_dump_nth ADDRESS 2)
need_line "$addr_out" 'code=VV1 .*active=low level=0 state=active' \
  'VV1 active for ВЫВОД'
need_line "$addr_out" 'code=VV0 .*active=low level=1 state=inactive' \
  'VV0 inactive for ВЫВОД'
need_line "$addr_out" 'code=D12 .*active=low level=0 state=active' \
  'output data driven on ADDRESS phase'
need_line "$(phase_dump_nth STROBE 2)" \
  'code=D12 .*active=low level=0 state=active' \
  'output data stable on STROBE phase'

# The functional channel suite must keep its exact three PASS strings.
legacy=$(E10025="$sim" "$here/tests/test_channel.sh" 2>&1) || \
  fail "legacy test_channel.sh failed: $legacy"
for s in "${pass_strings[@]}"; do
  printf '%s\n' "$legacy" | grep -Fq -- "$s" || fail "legacy PASS string lost: $s"
done

echo 'PASS: E10025 bus signals 56 named lines'
echo 'PASS: E10025 bus signals phase trace'
echo 'PASS: E10025 bus signals preserve channel regressions'

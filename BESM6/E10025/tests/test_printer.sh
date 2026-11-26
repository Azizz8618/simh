#!/usr/bin/env bash
# LPT line printer regression for Электроника-100-25.
# Drives the real I/O page registers through a short HALT program so the
# printer service routine runs and the character reaches the attached file.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
sim=${E10025:-"$here/build/e10025"}
if [ ! -x "$sim" ]; then
  echo "FAIL[printer]: simulator not built: $sim" >&2
  exit 1
  fi

fail () {
  echo "FAIL[printer]: $1" >&2
  exit 1
  }

work=$(mktemp -d "${TMPDIR:-/tmp}/e10025-printer.XXXXXX") || \
  fail 'cannot create work directory'
trap 'rm -f "$work/print.txt" "$work/session.out"; rmdir "$work" 2>/dev/null' EXIT
cd "$work" || fail 'cannot enter work directory'

printf '%s\n' \
  'set CPU BOOTEVENTS=on' \
  'show LPT' \
  'examine LPT DEVVEC' \
  'examine LPT DEVADDR' \
  'attach LPT0 print.txt' \
  'deposit LPT TIME 1' \
  'deposit 0 0' \
  'deposit 177514 0100' \
  'deposit 177516 0110' \
  'deposit PC 0' 'go' \
  'deposit 177516 0145' \
  'deposit PC 0' 'go' \
  'deposit 177516 012' \
  'deposit PC 0' 'go' \
  'deposit PC 0' 'go' \
  'examine LPT CSR' \
  'examine LPT DONE' \
  'examine LPT INT' \
  'examine LPT POS' \
  'detach LPT0' \
  'exit' | "$sim" >session.out 2>&1
out=$(cat session.out)

# The registration step is what turns this from red into green.
printf '%s\n' "$out" | grep -q 'Non-existent device: LPT' && \
  fail 'show LPT not recognized: LPT is missing from sim_devices'
printf '%s\n' "$out" | grep -Eq 'sim> LPT +address=17777514-17777517, vector=200, BR4' || \
  fail 'show LPT does not report LPT at CSR 017514, vector 0200, BR4'
printf '%s\n' "$out" | grep -q '%SIM-ERROR' && \
  fail 'simulator reported an error while driving LPT'

printf '%s\n' "$out" | grep -Eq 'DEVVEC:[[:space:]]+000200' || \
  fail 'DEVVEC is not 000200'
printf '%s\n' "$out" | grep -Eq 'DEVADDR:[[:space:]]+00017777514' || \
  fail 'DEVADDR is not the LP11 CSR 01777514'
printf '%s\n' "$out" | grep -q 'LPT: creating new file' || \
  fail 'attach LPT0 print.txt did not create the output file'
printf '%s\n' "$out" | grep -Eq 'CSR:[[:space:]]+000300' || \
  fail 'CSR is not 000300 (DONE plus IE)'
printf '%s\n' "$out" | grep -Eq 'DONE:[[:space:]]+1' || \
  fail 'DONE flag was not set after printing'
printf '%s\n' "$out" | grep -Eq 'INT:[[:space:]]+1' || \
  fail 'interrupt request was not raised'
printf '%s\n' "$out" | grep -Eq 'POS:[[:space:]]+3' || \
  fail 'printer did not advance by three characters'

printf 'He\n' | cmp -s - print.txt || \
  fail 'output file does not contain the printed line He'

echo 'PASS: LPT prints He, DONE and IRQ set, CSR 017514 vector 0200'

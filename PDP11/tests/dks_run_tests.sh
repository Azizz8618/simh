#!/bin/sh
# dks_run_tests.sh - stage A acceptance tests of the DKS device.
#
# Usage: PDP11/tests/dks_run_tests.sh [path-to-BIN/pdp11] [port]
#
# Covers the test cases T1 (connect -> sysreq 1 -> АБФОСH reply),
# T2 (input HELLO -> output array), T6 (disconnect -> sysreq 3),
# T8 (doorbell with diagnostic bits), T12 (KOI-7 backspace) of the
# implementation plan, and the KADDR/KDATA window (writes, reads,
# auto-increment, doorbell) driven by a hand-assembled firmware.
#
# Requires: python3.

SIM=${1:-./BIN/pdp11}
PORT=${2:-4000}
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
:
PASS=0; FAIL=0

say() { printf '%s\n' "$*"; }
found() { # found <file> <pattern> -> prints 0 when found
    grep -c "$2" "$1" > /dev/null 2>&1 && echo 0 || echo 1
}
has() { # has <text> <pattern> -> prints 0 when found
    printf '%s' "$1" | grep -q "$2" && echo 0 || echo 1
}
check() { # check <name> <result 0|1>
    if [ "$2" = 0 ]; then PASS=$((PASS+1)); say "PASS: $1"
    else FAIL=$((FAIL+1)); say "FAIL: $1"; fi
}

run_sim() { # run_sim <ini> <logfile> - start the sim over a held-open pipe
    rm -f "$TMP/fifo"; mkfifo "$TMP/fifo"
    "$SIM" "$1" < "$TMP/fifo" > "$2" 2>&1 &
    SIMPID=$!
    exec 3>"$TMP/fifo"
    sleep 1.5
}

stop_sim() {
    printf '\005' >&3                          # Ctrl-E: stop the CPU
    printf 'detach dks\nquit\n' >&3
    sleep 1
    exec 3>&-
    kill $SIMPID 2>/dev/null
    wait $SIMPID 2>/dev/null
}

tclient() { # tclient <input> <seconds>
    printf '%b' "$1" | python3 "$DIR/dks_telnet_test.py" 127.0.0.1 "$PORT" "$2"
}

say "== T1/T2/T6: TESTOS=ON echo loop (dks_a_smoke.ini) =="
run_sim "$DIR/dks_a_smoke.ini" "$TMP/smoke.log"
OUT=$(tclient 'HELLO\n' 8)
check "T1 connect greeting"   "$(has "$OUT" 'Connected to DKS')"
check "T2 local echo"         "$(has "$OUT" 'HELLO')"
# T2: the OS echo - "HELLO" must appear twice (local echo + output array)
N=$(printf '%s' "$OUT" | grep -c "HELLO"); check "T2 OS echo back" $([ "$N" -ge 2 ] && echo 0 || echo 1)
tclient '' 4 > /dev/null                       # T6: client drops the line
sleep 1
stop_sim
check "T1 sysreq chain"       "$(found "$TMP/smoke.log" 'system request 1')"
check "T1 АБФОСH reply"       "$(found "$TMP/smoke.log" 'connect reply term')"
check "T1 ring post"          "$(found "$TMP/smoke.log" 'post 48 (slot 0)')"
check "T1 TESTOS reply"       "$(found "$TMP/smoke.log" 'TESTOS: connect line 1 -> term 1 result 0')"
check "T6 disconnect trace"   "$(found "$TMP/smoke.log" 'line 1 disconnected')"
check "T6 sysreq 3"           "$(found "$TMP/smoke.log" 'system request 3')"
check "T6 term freed"         "$(found "$TMP/smoke.log" 'TESTOS: disconnect term')"

say "== T12: KOI-7 backspace, TESTOS=PROMPT (dks_a_prompt.ini) =="
run_sim "$DIR/dks_a_prompt.ini" "$TMP/prompt.log"
BS=$(printf '\010')
OUT=$(tclient "AB${BS}C\n" 8)
check "T12 connect chain"     "$(has "$OUT" 'E60 DKS ready')"
check "T12 echo with erase"   "$(printf '%s' "$OUT" | grep -q "AB${BS} ${BS}C" && echo 0 || echo 1)"
check "T12 prompt reply"      "$(has "$OUT" 'DKS> ')"
stop_sim
check "T12 stored AC"         "$(found "$TMP/prompt.log" 'input 2 chars')"

say "== T8/window: firmware drives KADDR/KDATA (dks_firmware_test.ini) =="
"$SIM" "$DIR/dks_firmware_test.ini" < /dev/null > "$TMP/fw.log" 2>&1
check "window write"          "$(found "$TMP/fw.log" 'KDATA write \[2\]05000 <- 012525')"
check "window read (R0)"      "$(found "$TMP/fw.log" 'R0:.*012525')"
check "AI write +0"           "$(found "$TMP/fw.log" 'KDATA write \[2\]50000 <- 000001')"
check "AI write +1"           "$(found "$TMP/fw.log" 'KDATA write \[2\]50001 <- 000002')"
check "MEM examine (decimal)" "$(found "$TMP/fw.log" 'MEM\[20480\]:.*000001')"
check "doorbell ring"         "$(found "$TMP/fw.log" 'doorbell from')"

say "== T8: doorbell with extra diagnostic bits (dks_doorbell.ini) =="
"$SIM" "$DIR/dks_doorbell.ini" < /dev/null > "$TMP/bell.log" 2>&1
check "doorbell diagnostic"   "$(found "$TMP/bell.log" 'doorbell 000777')"
check "doorbell ring"         "$(found "$TMP/bell.log" 'doorbell from')"

say "== stock PDP11 test suite still passes =="
timeout 120 "$SIM" "$DIR/pdp11_test.ini" < /dev/null > "$TMP/stock.log" 2>&1
check "pdp11_test.ini"        "$(found "$TMP/stock.log" 'All KWV11 Tests Passed')"

say ""
say "passed: $PASS, failed: $FAIL"
[ $FAIL -eq 0 ]

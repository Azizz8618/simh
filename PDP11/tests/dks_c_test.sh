#!/bin/sh
# dks_c_test.sh - stage C acceptance: the BULK bridge opcodes (05/06)
# and the write coalescing on the БЭСМ-6 side.
#
# Usage: PDP11/tests/dks_c_test.sh [BIN/pdp11] [BIN/besm6] [port]
#        (BPORT=... overrides the bridge port, default 4140)
#
# Part 1: a raw socket drives the bridge directly (dks_c_bulk_test.py):
# PING, single WRITE + BULK_READ round trip, BULK_WRITE + read back,
# per-channel isolation, rejections (cnt=0, cell 0, doorbell overlap,
# overrun), the doorbell afterwards.
# Part 2: the ОС-заглушка (dks_b2_besm6.ini) talks to the SIM engine
# through the bridge; its АБФОСH reply (+1, +2) must go out as one
# coalesced BULK_WRITE frame, and a HELLO still echoes.
#
# Requires: python3.

P11=${1:-./BIN/pdp11}
B6=${2:-./BIN/besm6}
PORT=${3:-4000}
BPORT=${BPORT:-4140}                            # the КАДОПАМ bridge port
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
PASS=0; FAIL=0

say() { printf '%s\n' "$*"; }
has() { printf '%s' "$1" | grep -q "$2" && echo 0 || echo 1; }
check() {
    if [ "$2" = 0 ]; then PASS=$((PASS+1)); say "PASS: $1"
    else FAIL=$((FAIL+1)); say "FAIL: $1"; fi
}

# The INIs hardcode the telnet port 4000 and the bridge port 4140;
# substitute the requested ones into a temporary copy.
port_ini() { # port_ini <src> <dst>
    sed -e "s/^attach dks [0-9][0-9]*/attach dks $PORT/" \
        -e "s/^set dks bridge=[0-9][0-9]*/set dks bridge=$BPORT/" \
        -e "s/^set dks remote=[^ \t]*/set dks remote=localhost:$BPORT/" \
        "$1" > "$2"
}

# ---- part 1: the BULK opcodes over a raw socket ------------------------
say "== C: BULK opcodes over a raw socket (dks_c_bulk_test.py) =="
port_ini "$DIR/dks_b2_e60.ini" "$TMP/dks_b2_e60.port.ini"
rm -f "$TMP/fifo"; mkfifo "$TMP/fifo"
"$P11" "$TMP/dks_b2_e60.port.ini" < "$TMP/fifo" > "$TMP/e60.log" 2>&1 &
P11PID=$!
exec 3>"$TMP/fifo"
sleep 2

python3 "$DIR/dks_c_bulk_test.py" 127.0.0.1 "$BPORT" > "$TMP/bulk.log" 2>&1
PYRC=$?
grep -E "^(PASS|FAIL)" "$TMP/bulk.log"
check "C.1-C.6 raw bulk protocol" $([ "$PYRC" = 0 ] && echo 0 || echo 1)

# ---- part 2: coalescing with the ОС-заглушка ---------------------------
say ""
say "== C: write coalescing, the ОС-заглушка over the bridge =="
port_ini "$DIR/dks_b2_besm6.ini" "$TMP/dks_b2_besm6.port.ini"
"$B6" "$TMP/dks_b2_besm6.port.ini" > "$TMP/b6.log" 2>&1 &
B6PID=$!
sleep 4

OUT=$(printf 'HELLO\r\n' | python3 "$DIR/dks_telnet_test.py" 127.0.0.1 "$PORT" 10)

# ---- shutdown: the logs are block-buffered, flush them first -----------
printf '\005' >&3                                  # Ctrl-E: stop the CPU
printf 'detach dks\nquit\n' >&3
i=0
while [ $i -lt 20 ]; do
    kill -0 $P11PID 2>/dev/null || break
    sleep 0.5
    i=$((i+1))
    done
exec 3>&-
kill $P11PID $B6PID 2>/dev/null
wait $P11PID $B6PID 2>/dev/null
sleep 0.3

# the ОС-заглушка answers a connect with АБФОСH+1 and АБФОСH+2 back to
# back: two adjacent 0132 writes must leave as ONE BULK_WRITE frame
check "C.7 АБФОСH+1/+2 coalesced into one BULK_WRITE" \
      "$(has "$(cat "$TMP/e60.log")" 'BULK_WRITE \[2\]00160 x2')"
N=$(printf '%s' "$OUT" | grep -c HELLO)
check "C.8 HELLO echoed through the coalescing path" $([ "$N" -ge 2 ] && echo 0 || echo 1)
# the only rejections must be the six C.5 error cases of part 1
N=$(grep -c 'rejected' "$TMP/e60.log")
check "C.9 no rejected bulk frames from the ОС" $([ "$N" = 6 ] && echo 0 || echo 1)

say ""
say "passed: $PASS, failed: $FAIL"
rm -rf "$TMP"
[ "$FAIL" = 0 ]

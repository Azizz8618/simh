#!/bin/sh
# dks_b2_test.sh - stage B2 acceptance test: the КАДОПАМ bridge between
# two SIMH processes.  Starts the Э-60 (PDP-11) with the bridge server
# and the БЭСМ-6 with the ОС-заглушка (dks_b2_besm6.ini), then drives a
# telnet terminal against the Э-60.
#
# Usage: PDP11/tests/dks_b2_test.sh [BIN/pdp11] [BIN/besm6] [port]
#
# Covers plan §8.5 (bridge up, КАДОПАМ reads/writes, manual system
# request with the АБФОСH reply, input -> output array through the ОС,
# doorbell both ways).
#
# Requires: python3.

P11=${1:-./BIN/pdp11}
B6=${2:-./BIN/besm6}
PORT=${3:-4000}
BPORT=${BPORT:-4140}                            # the КАДОПАМ bridge port
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
PASS=0; FAIL=0

# The INIs hardcode the telnet port 4000 and the bridge port 4140;
# substitute the requested ones into a temporary copy.
port_ini() { # port_ini <src> <dst>
    sed -e "s/^attach dks [0-9][0-9]*/attach dks $PORT/" \
        -e "s/^set dks bridge=[0-9][0-9]*/set dks bridge=$BPORT/" \
        -e "s/^set dks remote=[^ \t]*/set dks remote=localhost:$BPORT/" \
        "$1" > "$2"
}

say() { printf '%s\n' "$*"; }
has() { printf '%s' "$1" | grep -q "$2" && echo 0 || echo 1; }
check() {
    if [ "$2" = 0 ]; then PASS=$((PASS+1)); say "PASS: $1"
    else FAIL=$((FAIL+1)); say "FAIL: $1"; fi
}

# ---- start the Э-60 over a held-open pipe -----------------------------
port_ini "$DIR/dks_b2_e60.ini" "$TMP/dks_b2_e60.port.ini"
rm -f "$TMP/fifo"; mkfifo "$TMP/fifo"
"$P11" "$TMP/dks_b2_e60.port.ini" < "$TMP/fifo" > "$TMP/e60.log" 2>&1 &
P11PID=$!
exec 3>"$TMP/fifo"
sleep 1.5

# ---- start the БЭСМ-6 with the ОС-заглушка ----------------------------
port_ini "$DIR/dks_b2_besm6.ini" "$TMP/dks_b2_besm6.port.ini"
"$B6" "$TMP/dks_b2_besm6.port.ini" > "$TMP/b6.log" 2>&1 &
B6PID=$!
sleep 4

OUT=$(printf 'HELLO\r\n' | python3 "$DIR/dks_telnet_test.py" 127.0.0.1 "$PORT" 10)
echo "$OUT" | head -5 > "$TMP/telnet.txt"

check "B2.1 bridge client accepted (Э-60 log)" \
      "$(has "$(cat "$TMP/e60.log")" 'bridge: client .* connected')"
check "B2.2 bridge up (БЭСМ-6 log)" \
      "$(has "$(cat "$TMP/b6.log")" 'bridge up')"
check "B2.3 channel status read 002000 over the bridge" \
      "$(has "$(cat "$TMP/b6.log")" '032: \[[0-2]\]00000 -> 002000')"
check "B2.4 telnet greeting" \
      "$(has "$(cat "$TMP/telnet.txt")" 'Connected to DKS')"
N=$(printf '%s' "$OUT" | grep -c HELLO)
check "B2.5 HELLO echoed locally and by the ОС" $([ "$N" -ge 2 ] && echo 0 || echo 1)
check "B2.6 connect reply (АБФОСH) reached the Э-60" \
      "$(has "$(cat "$TMP/e60.log")" 'connect reply term [1-9]')"
check "B2.7 ПРП6 by ATTN frame (БЭСМ-6 log)" \
      "$(has "$(cat "$TMP/b6.log")" 'ATTN .ПРП 6.')"
check "B2.8 doorbell delivered to the Э-60" \
      "$(has "$(cat "$TMP/e60.log")" 'doorbell from БЭСМ-6')"
check "B2.9 output array went through the ОС" \
      "$(has "$(cat "$TMP/e60.log")" 'output cmd 4')"

# ---- shutdown ---------------------------------------------------------
# Graceful: Ctrl-E stops the CPU, QUIT exits; the process must exit by
# itself so that the buffered stdout (the debug log) reaches the file.
printf '\005' >&3                                  # Ctrl-E: stop the CPU
printf 'detach dks\nquit\n' >&3
i=0
while [ $i -lt 20 ]; do                            # up to 10 s
    kill -0 $P11PID 2>/dev/null || break
    sleep 0.5
    i=$((i+1))
    done
exec 3>&-
kill $P11PID $B6PID 2>/dev/null                    # the Э-60 should be gone
wait $P11PID $B6PID 2>/dev/null
sleep 0.3                                          # tail of the flush

say ""
say "passed: $PASS, failed: $FAIL"
rm -rf "$TMP"
[ "$FAIL" = 0 ]

#!/bin/sh
# dks_b2_shm_test.sh - acceptance test of the SHMEM transport: the
# КАДОПАМ channel between two SIMH processes as a direct access to the
# shared ОЗУ Э-60 (POSIX shm), no network.  Starts the Э-60 with the
# published ОЗУ and the БЭСМ-6 with the ОС-заглушка
# (dks_b2_shm_besm6.ini), then drives a telnet terminal against the
# Э-60.
#
# Usage: PDP11/tests/dks_b2_shm_test.sh [BIN/pdp11] [BIN/besm6] [port]
#
# Covers the same exchange as dks_b2_test.sh, but every 032/0132 is a
# memory access and the two hardware signals (doorbell, ATTN/ПРП6)
# cross as control-block counters.
#
# Requires: python3, a Linux/POSIX host (shm_open).

P11=${1:-./BIN/pdp11}
B6=${2:-./BIN/besm6}
PORT=${3:-4000}
SHM=/dks_b2_t0                          # the segment the Э-60 publishes
DIR=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
PASS=0; FAIL=0

port_ini() { # port_ini <src> <dst>
    sed -e "s/^attach dks [0-9][0-9]*/attach dks $PORT/" "$1" > "$2"
}

say() { printf '%s\n' "$*"; }
# Grep the FILE directly: the sims free-run in SHMEM mode and the logs
# grow to hundreds of MB - never pass them as command arguments.
hasf() { grep -q -- "$2" "$1" && echo 0 || echo 1; }
hasnt() { grep -q -- "$2" "$1" && echo 1 || echo 0; }
check() {
    if [ "$2" = 0 ]; then PASS=$((PASS+1)); say "PASS: $1"
    else FAIL=$((FAIL+1)); say "FAIL: $1"; fi
}

# a stale segment from an earlier run would keep its counters; SCP
# uppercases the name, so the file in /dev/shm is uppercase
rm -f /dev/shm/DKS_B2_T0 /dev/shm/dks_b2_t0

# ---- start the Э-60 over a held-open pipe -----------------------------
port_ini "$DIR/dks_b2_shm_e60.ini" "$TMP/e60.port.ini"
rm -f "$TMP/fifo"; mkfifo "$TMP/fifo"
"$P11" "$TMP/e60.port.ini" < "$TMP/fifo" > "$TMP/e60.log" 2>&1 &
P11PID=$!
exec 3>"$TMP/fifo"
sleep 1.5

# ---- start the БЭСМ-6 with the ОС-заглушка ----------------------------
port_ini "$DIR/dks_b2_shm_besm6.ini" "$TMP/b6.port.ini"
"$B6" "$TMP/b6.port.ini" > "$TMP/b6.log" 2>&1 &
B6PID=$!
sleep 4

OUT=$(printf 'HELLO\r\n' | python3 "$DIR/dks_telnet_test.py" 127.0.0.1 "$PORT" 10)
echo "$OUT" | head -5 > "$TMP/telnet.txt"

# ---- shutdown: the logs are block-buffered, flush them first -----------
# SIGTERM stops the CPU of both sims; the PDP-11 then returns to the
# sim> prompt and reads the fifo - 'quit' makes it exit and flush.
# Neither sim reads stdin while the CPU runs, so the fifo is only
# written to here.  kill -9 is the last resort and loses the log tail.
kill $P11PID $B6PID 2>/dev/null
printf 'quit\n' >&3
i=0
while [ $i -lt 20 ]; do
    kill -0 $P11PID 2>/dev/null || kill -0 $B6PID 2>/dev/null || break
    sleep 0.5
    i=$((i+1))
    done
exec 3>&-
kill -9 $P11PID $B6PID 2>/dev/null                 # whatever is left
wait $P11PID $B6PID 2>/dev/null
sleep 0.3

check "B2S.1 ОЗУ published as shm (Э-60 log)" \
      "$(hasf "$TMP/e60.log" 'SHMEM: ОЗУ Э-60 published')"
check "B2S.2 shm mapped by the БЭСМ-6" \
      "$(hasf "$TMP/b6.log" 'shmem: mapped')"
check "B2S.3 channel status read 002000 (direct access)" \
      "$(hasf "$TMP/b6.log" '032: \[[0-2]\]00000 -> 002000')"
check "B2S.4 telnet greeting" \
      "$(hasf "$TMP/telnet.txt" 'Connected to DKS')"
N=$(printf '%s' "$OUT" | grep -c HELLO)
check "B2S.5 HELLO echoed locally and by the ОС" $([ "$N" -ge 2 ] && echo 0 || echo 1)
check "B2S.6 connect reply (АБФОСH) reached the Э-60" \
      "$(hasf "$TMP/e60.log" 'connect reply term [1-9]')"
check "B2S.7 ПРП6 by the ATTN counter (БЭСМ-6 log)" \
      "$(hasf "$TMP/b6.log" 'ATTN .ПРП 6.')"
check "B2S.8 doorbell delivered to the Э-60" \
      "$(hasf "$TMP/e60.log" 'doorbell from БЭСМ-6')"
check "B2S.9 output array went through the ОС" \
      "$(hasf "$TMP/e60.log" 'output cmd 4')"
check "B2S.10 no bridge socket in use" \
      "$(hasnt "$TMP/e60.log" 'Bridge listening')"

say ""
say "passed: $PASS, failed: $FAIL"
rm -rf "$TMP"
rm -f /dev/shm/DKS_B2_T0 /dev/shm/dks_b2_t0
[ "$FAIL" = 0 ]

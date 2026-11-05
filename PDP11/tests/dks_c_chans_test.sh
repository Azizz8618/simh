#!/bin/sh
# dks_c_chans_test.sh - stage C acceptance: multi-channel КРК (CHANS=2).
#
# Usage: PDP11/tests/dks_c_chans_test.sh [BIN/pdp11] [BIN/besm6] [port]
#        (BPORT=... overrides the bridge port, default 4140)
#
# Two Э-60 simulators: instance #1 on the default channel 2 (bridge port
# BPORT) and instance #2 on channel 3 (SET DKS CHAN=3, bridge port
# BPORT+1).  The БЭСМ-6 runs with SET DKS CHANS=2 and one REMOTE spec:
# client 0 connects to BPORT, client 1 to BPORT+1.  Checks:
#
#   CH.1 both bridge clients established
#   CH.2 cell 0 shows both live channels (0600 = 0400<<2 | 0400<<3)
#   CH.3 the ОС-заглушка still works through channel 2 (HELLO echoed)
#   CH.4 ПРП6 from the base channel and ПРП12 on the sysreq (unchanged)
#
# Requires: python3.

P11=${1:-./BIN/pdp11}
B6=${2:-./BIN/besm6}
PORT=${3:-4000}
BPORT=${BPORT:-4140}
BPORT2=$((BPORT + 1))
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
port_ini() { # port_ini <src> <dst> <bridge-port>
    sed -e "s/^attach dks [0-9][0-9]*/attach dks $PORT/" \
        -e "s/^set dks bridge=[0-9][0-9]*/set dks bridge=$3/" \
        -e "s/^set dks remote=[^ \t]*/set dks remote=localhost:$3/" \
        "$1" > "$2"
}

# ---- the two Э-60 instances -------------------------------------------
port_ini "$DIR/dks_b2_e60.ini" "$TMP/e60_ch2.ini" "$BPORT"
sed -e "s/^set dks bridge=[0-9][0-9]*/set dks bridge=$BPORT2/" \
    -e "s/^attach dks [0-9][0-9]*/attach dks $PORT/" \
    -e "/^go /i set dks chan=3" \
    "$DIR/dks_b2_e60.ini" > "$TMP/e60_ch3.ini"

rm -f "$TMP/fifo"; mkfifo "$TMP/fifo"
"$P11" "$TMP/e60_ch2.ini" < "$TMP/fifo" > "$TMP/e60_ch2.log" 2>&1 &
P11PID=$!
exec 3>"$TMP/fifo"
sleep 1.5

"$P11" "$TMP/e60_ch3.ini" > "$TMP/e60_ch3.log" 2>&1 &
P11PID2=$!
sleep 1.5

# ---- the БЭСМ-6 with two bridge clients --------------------------------
port_ini "$DIR/dks_b2_besm6.ini" "$TMP/b6.ini" "$BPORT"
sed -i "/^run /i set dks chans=2" "$TMP/b6.ini"
"$B6" "$TMP/b6.ini" > "$TMP/b6.log" 2>&1 &
B6PID=$!
sleep 5

OUT=$(printf 'HELLO\r\n' | python3 "$DIR/dks_telnet_test.py" 127.0.0.1 "$PORT" 10)

# ---- shutdown: flush the block-buffered logs first ----------------------
printf '\005' >&3
printf 'detach dks\nquit\n' >&3
i=0
while [ $i -lt 20 ]; do
    kill -0 $P11PID 2>/dev/null || break
    sleep 0.5
    i=$((i+1))
    done
exec 3>&-
kill $P11PID $P11PID2 $B6PID 2>/dev/null
wait $P11PID $P11PID2 $B6PID 2>/dev/null
sleep 0.3

check "CH.1 base bridge client established (channel 2)" \
      "$(has "$(cat "$TMP/b6.log")" "bridge up: .*$BPORT")"
check "CH.1b second bridge client established (channel 3)" \
      "$(has "$(cat "$TMP/b6.log")" "bridge up: .*$BPORT2")"
check "CH.2 cell 0 shows both live channels (06000)" \
      "$(has "$(cat "$TMP/b6.log")" '032: .[0-2].00000 -> 006000')"
N=$(printf '%s' "$OUT" | grep -c HELLO)
check "CH.3 HELLO echoed through channel 2" $([ "$N" -ge 2 ] && echo 0 || echo 1)
check "CH.4 sysreq ATTN raises ПРП12 (unchanged)" \
      "$(has "$(cat "$TMP/b6.log")" 'ATTN .ПРП 6. + ПРП12')"

say ""
say "passed: $PASS, failed: $FAIL"
rm -rf "$TMP"
[ "$FAIL" = 0 ]

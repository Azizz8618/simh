#!/bin/sh
# dks_b1_test.sh - stage B1 acceptance: the СВЯЗЬ7 engine as the Э-60
# firmware (PDP11/dks_fw.bin) executed by the CPU, SET DKS ENGINE=HOST.
#
# Usage: PDP11/tests/dks_b1_test.sh [BIN/pdp11] [BIN/besm6] [port]
#
# Part 1 (TESTOS=PROMPT): connect -> the term assignment chain driven by
# the firmware, the "E60 DKS ready" greeting as an output array, local
# echo, the "DKS> " prompt reply.
# Part 2 (TESTOS=ON): line editing by the firmware (backspace erases),
# the ОС echoes the resulting line back; disconnect and reconnect.
# Part 3 (TESTOS=OFF + BRIDGE): the firmware over the КАДОПАМ bridge
# with the БЭСМ-6 ОС-заглушка (dks_b2_besm6.ini) - the full protocol
# across two simulators.
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

run_sim() { # run_sim <ini> <logfile> - start the sim over a held-open pipe
    port_ini "$1" "$TMP/port.ini"
    rm -f "$TMP/fifo"; mkfifo "$TMP/fifo"
    "$P11" "$TMP/port.ini" < "$TMP/fifo" > "$2" 2>&1 &
    SIMPID=$!
    exec 3>"$TMP/fifo"
    sleep 1.5
}

stop_sim() { # Ctrl-E, detach, quit; wait for the buffered log to flush
    printf '\005' >&3
    printf 'detach dks\nquit\n' >&3
    i=0
    while [ $i -lt 20 ]; do
        kill -0 $SIMPID 2>/dev/null || break
        sleep 0.5
        i=$((i+1))
        done
    exec 3>&-
    kill $SIMPID 2>/dev/null
    wait $SIMPID 2>/dev/null
    sleep 0.3
}

tel() { # tel <stdin> - one telnet session, stdout = what came back
    printf '%b' "$1" | python3 "$DIR/dks_telnet_test.py" 127.0.0.1 "$PORT" 10
}

# ---- part 1: TESTOS=PROMPT, the firmware drives everything ------------
say "== B1: ENGINE=HOST + TESTOS=PROMPT (dks_b1_fw.ini) =="
run_sim "$DIR/dks_b1_fw.ini" "$TMP/fw.log"

OUT=$(tel 'HELLO\r\n')
echo "$OUT" > "$TMP/tel1.txt"

stop_sim                                        # flush the log first

check "B1.1 greeting through the firmware (output array)" \
      "$(has "$(cat "$TMP/tel1.txt")" 'DKS ready')"
check "B1.2 local echo by the firmware" \
      "$(has "$(cat "$TMP/tel1.txt")" 'DKS> HELLO')"
check "B1.3 prompt reply (input array -> cmd 4 -> prompt back)" \
      "$(printf '%s' "$OUT" | tr -d '\r\n\0' | grep -c 'DKS> HELLO.*DKS> ' > /dev/null 2>&1 && echo 0 || echo 1)"
check "B1.4 the ОС took the line from АБУФS" \
      "$(has "$(cat "$TMP/fw.log")" 'TESTOS: take 1')"
check "B1.5 connect reply (АБФОСH) consumed by the firmware" \
      "$(has "$(cat "$TMP/fw.log")" 'TESTOS: connect line 1 -> term [1-9]')"
check "B1.6 the firmware printed the output array" \
      "$(has "$(cat "$TMP/fw.log")" 'TESTOS: [0-9][0-9]* bytes out to line 1')"
N=$(printf '%s' "$OUT" | grep -c 'DKS> ')
check "B1.7 two prompts seen (greeting + reply)" $([ "$N" -ge 2 ] && echo 0 || echo 1)

# ---- part 2: TESTOS=ON, editing and reconnection -----------------------
say ""
say "== B1: ENGINE=HOST + TESTOS=ON: editing, disconnect, reconnect =="
sed -e 's/testos=prompt/testos=on/' "$DIR/dks_b1_fw.ini" > "$TMP/dks_b1_on.src"
port_ini "$TMP/dks_b1_on.src" "$TMP/dks_b1_echo.ini"
run_sim "$TMP/dks_b1_echo.ini" "$TMP/fw2.log"

OUT=$(tel 'AB\b\bCD\r\n')
E=$(printf '%s' "$OUT" | tr '\010' E | grep -c 'E E')
OUT2=$(tel 'AB\bC\r\n')
stop_sim                                        # flush fw2.log before the checks

check "B1.8 the firmware echoes the erase (BS SP BS)" $([ "$E" -ge 1 ] && echo 0 || echo 1)
N=$(printf '%s' "$OUT" | grep -c CD)
check "B1.9 the ОС received and echoed 'CD'" $([ "$N" -ge 2 ] && echo 0 || echo 1)
check "B1.10 reconnect after disconnect (term freed)" \
      "$(has "$(cat "$TMP/fw2.log")" 'TESTOS: disconnect term')"
N=$(printf '%s' "$OUT2" | grep -c AC)
check "B1.11 the second line is 'AC' after one erase" $([ "$N" -ge 1 ] && echo 0 || echo 1)

# ---- part 3: the firmware over the КАДОПАМ bridge (B1+B2) --------------
say ""
say "== B1+B2: ENGINE=HOST + BRIDGE=4140 + the БЭСМ-6 ОС-заглушка =="
run_sim "$DIR/dks_b2_e60_fw.ini" "$TMP/e60.log"
port_ini "$DIR/dks_b2_besm6.ini" "$TMP/dks_b2_besm6.port.ini"
"$B6" "$TMP/dks_b2_besm6.port.ini" > "$TMP/b6.log" 2>&1 &
B6PID=$!
sleep 4

OUT=$(tel 'HELLO\r\n')
echo "$OUT" | head -5 > "$TMP/tel3.txt"

# ---- shutdown: the logs are block-buffered, flush them first -----------
printf '\005' >&3
printf 'detach dks\nquit\n' >&3
i=0
while [ $i -lt 20 ]; do
    kill -0 $SIMPID 2>/dev/null || break
    sleep 0.5
    i=$((i+1))
    done
exec 3>&-
kill $SIMPID $B6PID 2>/dev/null
wait $SIMPID $B6PID 2>/dev/null
sleep 0.3

check "B1B2.1 bridge client accepted" \
      "$(has "$(cat "$TMP/e60.log")" 'bridge: client .* connected')"
check "B1B2.2 bridge up (БЭСМ-6 log)" \
      "$(has "$(cat "$TMP/b6.log")" 'bridge up')"
check "B1B2.3 channel status read 002000 over the bridge" \
      "$(has "$(cat "$TMP/b6.log")" '032: \[[0-2]\]00000 -> 002000')"
check "B1B2.4 connect reply reached the firmware" \
      "$(has "$(cat "$TMP/e60.log")" 'doorbell from БЭСМ-6')"
N=$(printf '%s' "$OUT" | grep -c HELLO)
check "B1B2.5 HELLO echoed locally and by the ОС" $([ "$N" -ge 2 ] && echo 0 || echo 1)
check "B1B2.6 ПРП6 by ATTN frame (БЭСМ-6 log)" \
      "$(has "$(cat "$TMP/b6.log")" 'ATTN .ПРП 6.')"
# the ОС-заглушка commands the output array: [AREA(1)+5] <- 4 = 02405.
# The command word may arrive as a coalesced BULK_WRITE (stage C), so
# check the area-write trace, which both single and bulk writes emit.
check "B1B2.7 output array commanded through the ОС" \
      "$(has "$(cat "$TMP/e60.log")" 'line 1 area+5 <- 000004')"

say ""
say "passed: $PASS, failed: $FAIL"
rm -rf "$TMP"
[ "$FAIL" = 0 ]

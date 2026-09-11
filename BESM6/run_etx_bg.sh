#!/bin/bash
# run_etx_bg.sh — fully detached emulator with FIFO + ETX test
FIFO=/tmp/besm6_in
DIR=/home/azizz/Yandex.Disk/simh/BESM6
PROG=/tmp/etx_progress.log
RESULT=/tmp/etx_result.txt
> "$PROG"
> "$RESULT"

echo "$(date) starting" >> "$PROG"

cd "$DIR"

# Step 1: FIFO writer (background, holds write end open)
tail -f /dev/null > "$FIFO" &
FPID=$!
echo "fifo_writer=$FPID" >> "$PROG"
disown $FPID

# Step 2: Emulator
/home/azizz/Yandex.Disk/simh/BIN/besm6 dispak.ini < "$FIFO" > /tmp/besm6_out.log 2>&1 &
BPID=$!
echo "besm6=$BPID" >> "$PROG"
disown $BPID

# Step 3: Wait for ЖДУ (max 90 min)
for i in $(seq 1 1080); do
    if ! kill -0 $BPID 2>/dev/null; then
        echo "$(date) DIED at iter=$i" >> "$PROG"
        echo "EMULATOR_DIED" > "$RESULT"
        kill $FPID 2>/dev/null
        exit 1
    fi
    if grep -qE 'ЖДУ|#/.*:/' log.txt 2>/dev/null; then
        echo "$(date) ЖДУ at iter=$i" >> "$PROG"
        echo "BOOT_OK" > "$RESULT"
        break
    fi
    if [ $((i % 60)) -eq 0 ]; then
        echo "$(date) iter=$i dbg=$(wc -c < debug.txt 2>/dev/null)B" >> "$PROG"
    fi
    sleep 5
done

# Step 4: Send ETX
if grep -q "BOOT_OK" "$RESULT" 2>/dev/null; then
    echo "$(date) sending ETX" >> "$PROG"
    printf '\x03' > "$FIFO"
    sleep 10
    
    echo "=== tty1.txt ===" >> "$RESULT"
    cat tty1.txt 2>/dev/null >> "$RESULT" || echo "(empty)" >> "$RESULT"
    echo "=== log tail ===" >> "$RESULT"
    tail -10 log.txt 2>/dev/null >> "$RESULT"
    echo "=== MPRP ===" >> "$RESULT"
    grep 'MPRP' debug.txt 2>/dev/null | sed 's/.*MPRP=/MPRP=/' | sort -u | head -5 >> "$RESULT"
    echo "=== sim_poll_kbd ===" >> "$RESULT"
    grep -c 'sim_poll_kbd' debug.txt 2>/dev/null >> "$RESULT"
else
    echo "$(date) boot timeout" >> "$PROG"
    echo "BOOT_TIMEOUT" >> "$RESULT"
fi

# Cleanup
kill $BPID $FPID 2>/dev/null
echo "$(date) done" >> "$PROG"
echo "DONE" >> "$RESULT"

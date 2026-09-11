#!/bin/bash
# Test ETX (0x03) on BESM-6 Dispak console
# Strategy: FIFO with persistent writer, SIMH "send" after Ctrl+G
BESM6=/home/azizz/Yandex.Disk/simh/BIN/besm6
DIR=/home/azizz/Yandex.Disk/simh/BESM6
FIFO=/tmp/besm6_in
LOG=$DIR/log.txt
DBG=$DIR/debug.txt
TTY1=$DIR/tty1.txt
RUNLOG=/tmp/besm6_run.log
RESULT=/tmp/etx_test_result.txt
PROGRESS=/tmp/etx_test_progress.txt

cd "$DIR"
> "$RESULT"
echo "starting at $(date)" > "$PROGRESS"

# Cleanup
pkill -9 -f besm6 2>/dev/null
pkill -9 -f 'tail.*/dev/null.*besm6' 2>/dev/null
sleep 0.5
rm -f "$FIFO" "$LOG" "$DBG" "$TTY1" "$RUNLOG"
mkfifo "$FIFO"

# Persistent writer: keeps FIFO open via fd 3
# All commands are sent through the same fd to avoid EOF
{
    exec 3>"$FIFO"
    echo "fifo_opened" >> "$PROGRESS"

    # Start emulator
    "$BESM6" dispak.ini < "$FIFO" > "$RUNLOG" 2>&1 &
    BESM_PID=$!
    echo "besm6_pid=$BESM_PID" >> "$PROGRESS"

    # Monitor boot progress
    echo "monitoring boot..." >> "$PROGRESS"
    for i in $(seq 1 240); do  # up to 20 min
        if ! kill -0 $BESM_PID 2>/dev/null; then
            echo "DIED at iter $i" >> "$PROGRESS"
            break
        fi
        # Check for ЖДУ in log.txt
        if grep -qE 'ЖДУ|#/.*:/' "$LOG" 2>/dev/null; then
            echo "ЖДУ DETECTED at iter $i ($(date))" >> "$PROGRESS"
            break
        fi
        # Periodic progress
        if [ $((i % 12)) -eq 0 ]; then
            DBG_SIZE=$(wc -c < "$DBG" 2>/dev/null || echo 0)
            echo "iter $i: debug=$DBG_SIZE bytes" >> "$PROGRESS"
        fi
        sleep 5
    done

    echo "boot phase done at $(date)" >> "$PROGRESS"
    echo "=== Log last 10 lines ===" >> "$RESULT"
    tail -10 "$LOG" >> "$RESULT" 2>/dev/null

    # Phase 2: Try sending ETX directly via FIFO while running
    echo "" >> "$RESULT"
    echo "=== Direct ETX via FIFO ===" >> "$RESULT"
    printf '\x03' >&3
    sleep 3
    echo "tty1.txt after direct ETX:" >> "$RESULT"
    cat "$TTY1" >> "$RESULT" 2>/dev/null || echo "(empty/missing)" >> "$RESULT"

    # Phase 3: Stop with Ctrl+G, use SIMH send command
    echo "" >> "$RESULT"
    echo "=== Ctrl+G then SIMH send ===" >> "$RESULT"
    printf '\007' >&3  # Ctrl+G = WRU
    sleep 2
    echo "send tty1 \"\003\"" >&3
    sleep 1
    echo "cont" >&3
    sleep 5

    echo "tty1.txt after SIMH send:" >> "$RESULT"
    cat "$TTY1" >> "$RESULT" 2>/dev/null || echo "(empty/missing)" >> "$RESULT"
    echo "" >> "$RESULT"
    echo "=== Final log tail ===" >> "$RESULT"
    tail -10 "$LOG" >> "$RESULT" 2>/dev/null
    echo "" >> "$RESULT"
    echo "=== MPRP unique values ===" >> "$RESULT"
    grep 'MPRP' "$DBG" 2>/dev/null | sed 's/.*MPRP=/MPRP=/' | sort -u | head -5 >> "$RESULT"
    echo "" >> "$RESULT"
    echo "=== DKS/KDP count ===" >> "$RESULT"
    grep -c 'KDP\|DKS.*registered\|terminal' "$DBG" 2>/dev/null >> "$RESULT"
    echo "" >> "$RESULT"

    # Cleanup
    exec 3>&-  # close fd 3
    kill $BESM_PID 2>/dev/null
    echo "DONE at $(date)" >> "$PROGRESS"
    echo "=== END ===" >> "$RESULT"
} &
SCRIPT_PID=$!
echo "script_pid=$SCRIPT_PID" > /tmp/etx_script_pid.txt
echo "Started. PID=$SCRIPT_PID"
echo "Monitor: tail -f $PROGRESS"
echo "Result:  cat $RESULT"

#!/bin/bash
# run_etx_test.sh — запуск besm6 через FIFO + мониторинг
cd /home/azizz/Yandex.Disk/simh/BESM6
FIFO=/tmp/besm6_in
rm -f "$FIFO"
mkfifo "$FIFO"

# Writer: keeps FIFO open
tail -f /dev/null > "$FIFO" &
FPID=$!
echo "fifo_writer=$FPID"

# Emulator
/home/azizz/Yandex.Disk/simh/BIN/besm6 dispak.ini < "$FIFO" &
BPID=$!
echo "besm6=$BPID"

# Wait for boot (poll log.txt for ЖДУ)
for i in $(seq 1 300); do
    if ! kill -0 $BPID 2>/dev/null; then
        echo "DIED at $i"
        kill $FPID 2>/dev/null
        exit 1
    fi
    if grep -qE 'ЖДУ|#/.*:/' log.txt 2>/dev/null; then
        echo "ЖДУ found at iter=$i"
        break
    fi
    if [ $((i % 12)) -eq 0 ]; then
        echo "iter=$i dbg=$(wc -c < debug.txt 2>/dev/null)B log=$(wc -l < log.txt 2>/dev/null)L"
    fi
    sleep 5
done

echo "Boot phase done. Sending ETX..."
printf '\x03' > "$FIFO"
sleep 5

echo "=== tty1.txt ==="
cat tty1.txt 2>/dev/null || echo "(empty)"
echo "=== log tail ==="
tail -5 log.txt 2>/dev/null
echo "=== MPRP ==="
grep 'MPRP' debug.txt 2>/dev/null | sed 's/.*MPRP=/MPRP=/' | sort -u | head -5

kill $BPID $FPID 2>/dev/null
echo "DONE"
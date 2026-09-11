#!/bin/bash
# t133_watch.sh — дозорный: ждёт ЖДУ ОС, подаёт t133, собирает результат
cd /home/azizz/Yandex.Disk/simh/BESM6
S=/tmp/t133_status.txt
echo "$(date '+%H:%M:%S') watcher started" > $S
# 1. Ждать инициализации ОС (маркеры старта Диспака в консоли), макс 90 мин
for i in $(seq 1 180); do
  grep -aq 'CM1П\|PEC:' log.txt 2>/dev/null && break
  sleep 30
done
if ! grep -aq 'CM1П\|PEC:' log.txt; then
  echo "$(date '+%H:%M:%S') FAIL: ОС не загрузилась за 90 мин" >> $S; exit 1
fi
echo "$(date '+%H:%M:%S') ОС загрузилась" >> $S
sleep 60
# 2. Подать do-файл t133 через FIFO (неблокирующе, читатель держит sleep infinity)
python3 - <<'EOF'
import os
f = os.open('/tmp/besm6_in', os.O_WRONLY | os.O_NONBLOCK)
os.write(f, b'do ../UVU/t133.dubna.ini\r')
os.close(f)
EOF
echo "$(date '+%H:%M:%S') do t133 подано" >> $S
# 3. Ждать В0 (подтверждение приёма задания), макс 30 мин
for i in $(seq 1 60); do
  grep -aq 'B075-1 419900' log.txt 2>/dev/null && break
  sleep 30
done
if grep -aq 'B075-1 419900' log.txt; then
  echo "$(date '+%H:%M:%S') В0 получено" >> $S
else
  echo "$(date '+%H:%M:%S') FAIL: В0 нет за 30 мин" >> $S; exit 1
fi
# 4. Дать заданию исполниться (цикл ввода+исполнения ~2-6 мин), собрать результат
sleep 360
echo "--- маркеры результата (output.txt) ---" >> $S
grep -a 'КОНЕЦ\|АВОСТ\|авост\|4199\|133\|ОТСТ' output.txt 2>/dev/null | head -15 >> $S
cp output.txt /tmp/t133_output_copy.txt 2>/dev/null
echo "--- хвост log.txt (не-физобм) ---" >> $S
grep -av 'physobm\|TRACE\|DBG' log.txt | grep -a . | tail -15 >> $S
echo "$(date '+%H:%M:%S') done" >> $S

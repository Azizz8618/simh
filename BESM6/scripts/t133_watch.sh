#!/bin/bash
# t133_watch.sh — дозорный: ждёт ЖДУ ОС, подаёт t133, собирает результат
# Управление эмулятором: tmux send-keys (БЕЗ FIFO)

SESSION="besm6"
PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
OSZAGR="$PROJECT_ROOT/OSZAGR"
LOG_FILE="$OSZAGR/log.txt"
cd "$PROJECT_ROOT"
S=/tmp/t133_status.txt
echo "$(date '+%H:%M:%S') watcher started" > $S

# 1. Ждать загрузки ОС (ЖДУ или PEC), макс 90 мин
for i in $(seq 1 180); do
  grep -aqE 'ЖДУ|PEC:|CM1' "$LOG_FILE" 2>/dev/null && break
  sleep 30
done
if ! grep -aqE 'ЖДУ|PEC:|CM1' "$LOG_FILE" 2>/dev/null; then
  echo "$(date '+%H:%M:%S') FAIL: ОС не загрузилась за 90 мин" >> $S; exit 1
fi
echo "$(date '+%H:%M:%S') ОС загрузилась" >> $S
sleep 60

# 2. Подать do-файл t133 через tmux send-keys
tmux send-keys -t "$SESSION" "do ../UVU/t133.dubna.ini" Enter
echo "$(date '+%H:%M:%S') do t133 подано" >> $S

# 3. Ждать В0 (подтверждение приёма задания), макс 30 мин
for i in $(seq 1 60); do
  grep -aq 'B075-1 419900' "$LOG_FILE" 2>/dev/null && break
  sleep 30
done
if grep -aq 'B075-1 419900' "$LOG_FILE" 2>/dev/null; then
  echo "$(date '+%H:%M:%S') В0 получено" >> $S
else
  echo "$(date '+%H:%M:%S') FAIL: В0 нет за 30 мин" >> $S; exit 1
fi

# 4. Дать заданию исполниться (~2-6 мин), собрать результат
sleep 360
echo "--- маркеры результата (output.txt) ---" >> $S
grep -a 'КОНЕЦ|АВОСТ|авост|4199|133|ОТСТ' output.txt 2>/dev/null | head -15 >> $S
cp output.txt /tmp/t133_output_copy.txt 2>/dev/null
echo "--- хвост log.txt ---" >> $S
tail -15 "$LOG_FILE" 2>/dev/null | grep -av 'physobm' >> $S
echo "$(date '+%H:%M:%S') done" >> $S

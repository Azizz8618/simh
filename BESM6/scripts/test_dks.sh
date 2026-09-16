#!/bin/bash
# test_dks.sh — Запуск эмулятора БЭСМ-6 с ДКС-линиями (ОС Диспак)
#
# Алгоритм:
#   1. Запуск через tmux (tmux send-keys для управления)
#   2. Ожидание загрузки ОС (logs/debug_dks.log или debug.txt)
#   3. Ожидание DKS-регистрации
#   4. Telnet-подключение к DKS-линии
#
# Управление (после запуска):
#   tmux send-keys -t besm6 'команда' Enter  -- команда эмулятору
#   tmux send-keys -t besm6 C-g              -- Ctrl+G (sim>)
#
# Стыковка с трассами:
#   ./scripts/test_dks.sh                  -> запуск + регистрация
#   ./scripts/check_dks.sh                 -> проверка точек цепочки
#   ./scripts/check_dks.sh --trace         -> проверка + снятие трассы
#   ./scripts/test_traces.sh --pult 12     -> снятие трассы
#   ./scripts/stop_emulator.sh             -> остановка + очистка
#
# Использование: ./scripts/test_dks.sh [--no-connect] [--wait SEC]

set -u
SESSION="besm6"
PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
BIN="$PROJECT_ROOT/../BIN/besm6"
OSZAGR="$PROJECT_ROOT/OSZAGR"
DKS_LOG="$OSZAGR/logs/debug_dks.log"
DEBUG_LOG="$PROJECT_ROOT/debug.txt"
PORT=4203

NO_CONNECT=0
WAIT_BOOT=30
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-connect) NO_CONNECT=1; shift ;;
        --wait)       WAIT_BOOT="$2"; shift 2 ;;
        *)            echo "Неизвестный аргумент: $1"; exit 1 ;;
    esac
done

# --- 0. Проверка бинарника ---
if [ ! -x "$BIN" ]; then
    echo "✗ Нет эмулятора: $BIN"; exit 1
fi

# --- 1. Остановка прежней сессии ---
echo "Останавливаю прежний эмулятор..."
tmux kill-session -t "$SESSION" 2>/dev/null || true
pkill -9 -f besm6 2>/dev/null || true
sleep 1

# --- 2. Очистка логов (правило Л1) ---
rm -f "$PROJECT_ROOT/debug.txt" "$PROJECT_ROOT/log.txt" \
      "$OSZAGR/debug.txt" "$OSZAGR/log.txt" \
      "$OSZAGR/logs/debug_"*.log \
      "$OSZAGR/1[0-9]"

# --- 3. Запуск через tmux ---
echo "Запускаю эмулятор в tmux-сессии..."
cd "$OSZAGR"
tmux new-session -d -s "$SESSION" \
    "cd $OSZAGR && $BIN dispak.ini"
echo "✓ tmux-сессия $SESSION создана"

# --- 4. Ожидание загрузки ОС ---
echo -n "Ожидание загрузки ОС (до ${WAIT_BOOT}с)... "
BOOTS=0
for i in $(seq 1 $((WAIT_BOOT / 10))); do
    if grep -aq 'ДКС\|DKS\|КРК\|СВЯЗЬ' "$DKS_LOG" 2>/dev/null; then
        BOOTS=1; break
    fi
    if grep -aq 'ДКС\|DKS\|КРК\|СВЯЗЬ' "$PROJECT_ROOT/debug.txt" 2>/dev/null; then
        BOOTS=1; break
    fi
    sleep 10
done
if [ "$BOOTS" -eq 1 ]; then
    echo "✓"
else
    echo "✗ (ОС не вышла на ДКС за ${WAIT_BOOT}с)"
    echo "  Проверьте: tmux attach -t $SESSION"
    exit 1
fi

# --- 5. Ожидание DKS-регистрации ---
echo -n "Ожидание «DKS: terminal registered»... "
REGED=0
for i in $(seq 1 30); do
    if grep -aq 'terminal.*registered\|DKS.*terminal' "$DKS_LOG" 2>/dev/null; then
        REGED=1; break
    fi
    if grep -aq 'terminal.*registered\|DKS.*terminal' "$PROJECT_ROOT/debug.txt" 2>/dev/null; then
        REGED=1; break
    fi
    sleep 10
done
if [ "$REGED" -eq 1 ]; then
    echo "✓"
    grep -aE 'terminal.*registered|DKS.*terminal' "$DKS_LOG" 2>/dev/null | tail -3
else
    echo "✗ (регистрация не произошла)"
fi

# --- 6. Telnet-подключение ---
if [ "$NO_CONNECT" -eq 0 ] && command -v nc >/dev/null 2>&1; then
    (sleep 2; printf 'A\r'; sleep 2) | nc localhost $PORT > /dev/null 2>&1 &
    echo "✓ telnet-клиент → порт $PORT (tty11, DKS)"
fi

echo ""
echo "Команды:"
echo "  Проверка точек:   ./scripts/check_dks.sh"
echo "  Проверка+трасса:  ./scripts/check_dks.sh --trace"
echo "  Трасса:            ./scripts/test_traces.sh --pult 12"
echo "  Наблюдение:       tail -f $DKS_LOG"
echo "  Остановка:        ./scripts/stop_emulator.sh"

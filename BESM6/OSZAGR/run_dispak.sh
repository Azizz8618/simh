#!/bin/bash
# run_dispak.sh — запуск эмулятора ОС ДИСПАК в screen/tmux

SESSION="dispak"
WORKDIR="$HOME/Yandex.Disk/simh/BESM6/OSZAGR"
EMULATOR="../../BIN/besm6"
CONFIG="dispak.ini"

# Порты терминалов (из dispak.ini)
TTY1_PORT=4202  # Тип 1 — операторская консоль

show_usage() {
    echo ""
    echo "  run_dispak.sh — управление эмулятором ОС ДИСПАК"
    echo ""
    echo "  Использование:"
    echo "    ./run_dispak.sh start   — запуск эмулятора"
    echo "    ./run_dispak.sh stop    — останов эмулятора"
    echo "    ./run_dispak.sh attach  — подключение к консоли"
    echo "    ./run_dispak.sh info    — информация о портах"
    echo ""
    echo "  Порты терминалов:"
    echo "    tty1 (оператор) — telnet localhost $TTY1_PORT"
    echo ""
    echo "  Управление в консоли:"
    echo "    Ctrl+G         — останов эмулятора"
    echo "    set pult=N     — трассировка (N >= 11)"
    echo "    quit           — выход из эмулятора"
    echo ""
}

start() {
    # Проверяем screen
    if screen -list 2>/dev/null | grep -q "$SESSION"; then
        echo "Сессия '$SESSION' уже запущена"
        echo "Для подключения: screen -r $SESSION"
        exit 1
    fi
    
    cd "$WORKDIR" || exit 1
    
    # Запуск в screen
    screen -dmS "$SESSION" -L -Logfile "$WORKDIR/screen.log" "$EMULATOR" "$CONFIG"
    sleep 1
    
    if screen -list 2>/dev/null | grep -q "$SESSION"; then
        echo "Эмулятор ОС ДИСПАК запущен"
        echo ""
        echo "  Подключение:"
        echo "    screen -r $SESSION"
        echo "    telnet localhost $TTY1_PORT"
        echo ""
        echo "  Управление:"
        echo "    Ctrl+G — останов"
        echo "    set pult=N (N>=11) — трасса"
        echo ""
        echo "  Лог: $WORKDIR/screen.log"
        echo ""
    else
        echo "Ошибка запуска эмулятора"
        exit 1
    fi
}

stop() {
    # Сначала пробуем через screen
    if screen -list 2>/dev/null | grep -q "$SESSION"; then
        screen -S "$SESSION" -X quit
        sleep 1
    fi
    # Если процессы остались — убиваем напрямую
    if pgrep -f "besm6.*dispak.ini" >/dev/null; then
        pkill -9 -f "besm6.*dispak.ini"
        sleep 1
    fi
    if pgrep -f "SCREEN.*dispak" >/dev/null; then
        pkill -9 -f "SCREEN.*dispak"
    fi
    echo "Сессия '$SESSION' остановлена"
}

attach() {
    screen -r "$SESSION"
}

info() {
    echo ""
    echo "  Состояние сессий screen:"
    screen -list 2>/dev/null || echo "  Нет активных сессий"
    echo ""
    echo "  Порты терминалов:"
    echo "    tty1 — telnet localhost $TTY1_PORT (операторская консоль)"
    echo ""
    if [ -f "$WORKDIR/screen.log" ]; then
        echo "  Последние строки лога:"
        tail -5 "$WORKDIR/screen.log"
        echo ""
    fi
}

case "${1:-}" in
    start)  start ;;
    stop)   stop ;;
    attach) attach ;;
    info)   info ;;
    *)      show_usage ;;
esac

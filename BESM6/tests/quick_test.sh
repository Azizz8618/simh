# Быстрый тест одного аспекта DKS
# Использование: ./quick_test.sh [параметр]
# Примеры:
#   ./quick_test.sh dks      - проверить установку DKS флага
#   ./quick_test.sh port     - проверить открытие порта
#   ./quick_test.sh connect  - проверить подключение

cd /home/azizz/Yandex.Disk/simh/BESM6
TEST="${1:-dks}"

case $TEST in
    dks)
        echo "set tty17 dks; show tty17" | timeout 2 ../BIN/besm6 2>&1 | grep -E 'DKS|MUX'
        ;;
    port)
        pkill -9 besm6; rm -f debug.txt
        ../BIN/besm6 /tmp/dks.ini &
        sleep 3 && ss -tlnp | grep 4199 && pkill -9 besm6
        ;;
    connect)
        pkill -9 besm6; rm -f debug.txt
        ../BIN/besm6 /tmp/dks.ini > /tmp/test.log 2>&1 &
        sleep 3
        (echo ''; sleep 1) | nc localhost 4199 &
        sleep 2
        grep -E 'tty.*connection|DKS' debug.txt 2>/dev/null | tail -5
        pkill -9 besm6
        ;;
    *)
        echo "Неизвестный тест: $TEST"
        echo "Доступно: dks, port, connect"
        exit 1
        ;;
esac

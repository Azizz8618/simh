#!/bin/bash
# run_test_dks.sh — Автоматизированный тест терминалов АС,ТТ через ДКС
#
# DKS-линия определяется ДИНАМИЧЕСКИ из INI-файла (парсинг "set ttyN dks").
# Telnet-клиент — scripts/dks_tty.py: одно постоянное подключение к целевой
# линии; шлёт ETX (гид ОС Диспак) сразу и каждые 15 сек, символ 'A' на t+6;
# весь вывод эмулятора пишет в RX-лог. По RX-логу проверяется реакция ОС.
#
# Проверяет:
#   1. Соответствие ГЕНС ⇄ INI (АС,ТТ:N-M → dks, не mux)
#   2. Загрузку ОС с генерированного диска 2053
#   3. DKS-регистрацию (dks_register → PRP_DKS_SREQ / ПРП12)
#   4. DKS-ввод символа (dks_poll → PRP_DKS_TERMREQ / ПРП7)
#   5. Реакцию ОС на ETX: приглашение вида «ЭВМ-3, Т-NNN» на терминале
#
# Использование: ./run_test_dks.sh [INI-файл]
#   по умолчанию: test_dks_term.ini

set -u

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
GENS_DIR="/home/azizz/Yandex.Disk/re_dispak/re-dispak/gens"
GENS_FILE="$GENS_DIR/gens_exp.b6"
BIN="$PROJECT_ROOT/../BIN/besm6"
PORT=4199
PASS=0
FAIL=0

# INI-файл — аргумент или по умолчанию
INI_FILE="${1:-$PROJECT_ROOT/test_dks_term.ini}"
INI_BASENAME="$(basename "$INI_FILE")"

# ДИНАМИЧЕСКИ определяем пути debug/log из самого INI
# (set console debug=..., set -n console log=...)
DEBUG_FILE=$(grep -oE 'console debug=[^ ]+' "$INI_FILE" | head -1 | cut -d= -f2-)
LOG_FILE=$(grep -oE 'console log=[^ ]+' "$INI_FILE" | head -1 | cut -d= -f2-)
DEBUG_FILE="${DEBUG_FILE:-/tmp/besm6_dks_${INI_BASENAME%.ini}_debug.txt}"
LOG_FILE="${LOG_FILE:-/tmp/besm6_dks_${INI_BASENAME%.ini}_log.txt}"
OUTPUT_FILE="/tmp/besm6_dks_${INI_BASENAME%.ini}_output.txt"
BOOT_LOG="/tmp/besm6_dks_${INI_BASENAME%.ini}_boot.log"
RX_LOG="/tmp/besm6_dks_${INI_BASENAME%.ini}_rx.log"
DKS_TTY="$PROJECT_ROOT/scripts/dks_tty.py"
DKS_TTY_LIFE=90      # срок жизни telnet-клиента (ETX повторяется каждые 15 сек)
OS_WAIT=14           # пауза после отправки ETX для проверки реакции ОС

cd "$PROJECT_ROOT"

# -------------------------------------------------------------------------
# Вспомогательные функции
# -------------------------------------------------------------------------
ok()   { echo "  ✓ $*"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $*"; FAIL=$((FAIL+1)); }
section() { echo ""; echo "=========================================="; echo "  $*"; echo "=========================================="; }

# -------------------------------------------------------------------------
# 0. Предварительные проверки
# -------------------------------------------------------------------------
section "0. ПРЕДВАРИТЕЛЬНЫЕ ПРОВЕРКИ (INI: $INI_BASENAME)"

if [ ! -x "$BIN" ]; then echo "✗ Нет бинарника: $BIN"; exit 1; fi
ok "Бинарник: $BIN"
if [ ! -f "$GENS_FILE" ]; then echo "✗ Нет ГЕНС-файла: $GENS_FILE"; exit 1; fi
ok "ГЕНС-файл: $GENS_FILE"
if [ ! -f "$INI_FILE" ]; then echo "✗ Нет INI: $INI_FILE"; exit 1; fi
ok "INI: $INI_FILE"

# -------------------------------------------------------------------------
# 1. ДИНАМИЧЕСКОЕ ОПРЕДЕЛЕНИЕ DKS-ЛИНИЙ из INI + проверка ГЕНС ⇄ INI
# -------------------------------------------------------------------------
section "1. СООТВЕТСТВИЕ ГЕНС ⇄ INI (DKS-линии определяются динамически)"

echo "--- Директивы ГЕНС (gens_exp.b6) ---"
grep -E '^(ТТ|ТЕРМ|TEPM)' "$GENS_FILE" || true

echo ""
echo "--- Парсинг INI: dks / mux / console ---"
# Все dks-линии (динамически)
DKS_LINES=$(grep -oE 'set tty[0-9]+ dks' "$INI_FILE" | grep -oE '[0-9]+' | sort -n)
DKS_COUNT=$(echo "$DKS_LINES" | grep -c '.' 2>/dev/null || echo 0)
echo "  DKS-линии в INI: $(echo $DKS_LINES | tr '\n' ' ') (всего $DKS_COUNT)"

# Занятые линии (console attach)
CONSOLE_LINES=$(grep -oE 'attach tty[0-9]+ console' "$INI_FILE" | grep -oE '[0-9]+' | sort -n)
echo "  Console-линии: $(echo $CONSOLE_LINES | tr '\n' ' ')"

# Проверка: mux на dks-линиях
MUX_LINES=$(grep -oE 'set tty[0-9]+ mux' "$INI_FILE" | grep -oE '[0-9]+' | sort -n)
echo "  MUX-линии: $(echo $MUX_LINES | tr '\n' ' ')"

if [ "$DKS_COUNT" -eq 0 ]; then
    fail "В INI нет ни одной dks-линии — тест невозможен"
    exit 1
fi
ok "Найдено DKS-линий: $DKS_COUNT"

# Выбираем первую (минимальный номер) DKS-линию для теста
DKS_TARGET=$(echo "$DKS_LINES" | head -1)
ok "Целевая DKS-линия для теста: tty$DKS_TARGET"

# Вычисляем количество telnet-подключений до достижения DKS_TARGET
# tty0 = faked busy (всегда), tty1..ttyN = console (заняты)
# tmxr_poll_conn возвращает первую свободную (conn==0) линию
# Первое telnet → первая свободная после всех занятых
# Считаем занятые линии с номером < DKS_TARGET
BUSY_BEFORE=0
for n in $CONSOLE_LINES; do
    if [ "$n" -lt "$DKS_TARGET" ]; then
        BUSY_BEFORE=$((BUSY_BEFORE+1))
    fi
done
# tty0 всегда занят (faked) → +1, но tty0 не входит в CONSOLE_LINES
BUSY_BEFORE=$((BUSY_BEFORE + 1))  # tty0 faked
# Количество telnet-подключений для достижения tty$DKS_TARGET.
# Свободных линий строго до DKS_TARGET = DKS_TARGET - BUSY_BEFORE.
# Они займут первые (DKS_TARGET - BUSY_BEFORE) подключений,
# затем +1 подключение попадёт на саму tty$DKS_TARGET.
NCONN=$((DKS_TARGET - BUSY_BEFORE + 1))
if [ "$NCONN" -lt 1 ]; then NCONN=1; fi
ok "Telnet-подключений для достижения tty$DKS_TARGET: $NCONN (пустышек: $((NCONN-1)), целевое: 1)"

# Проверка: нет mux на dks-линиях
MUX_ON_DKS=0
for d in $DKS_LINES; do
    for m in $MUX_LINES; do
        if [ "$d" = "$m" ]; then MUX_ON_DKS=$((MUX_ON_DKS+1)); fi
    done
done
if [ "$MUX_ON_DKS" -eq 0 ]; then
    ok "Нет MUX на DKS-линиях (mux⇔dks не конфликтуют)"
else
    fail "MUX стоит на $MUX_ON_DKS DKS-линиях (недопустимо — взаимоисключение)"
fi

# Проверка ГЕНС: ТЕРМ АС,ТТ присутствует
if grep -q 'ТЕРМ  АС,ТТ' "$GENS_FILE"; then
    ok "ГЕНС: ТЕРМ АС,ТТ (через ДКС) присутствует"
else
    fail "ГЕНС: нет ТЕРМ АС,ТТ"
fi
# Проверка: нет дублирующей ТЕРМ ТТ на тех же линиях
if grep -q 'ТЕРМ  ТТ:11-16' "$GENS_FILE"; then
    fail "ГЕНС: дублирующая ТЕРМ ТТ:11-16 (конфликт шкал)"
else
    ok "ГЕНС: нет дублирующей ТЕРМ ТТ:11-16"
fi

# -------------------------------------------------------------------------
# 2. Проверка диска 2053 (генерированный ГЕНС)
# -------------------------------------------------------------------------
section "2. ДИСК 2053 (ГЕНС gens_exp)"

DISK2053="/usr/local/share/besm6/2053"
if [ ! -f "$DISK2053" ]; then
    fail "Диск 2053 не найден: $DISK2053"
else
    ok "Диск 2053: $(stat -c '%s байт' "$DISK2053")"
    BACKUP=$(ls -t "$PROJECT_ROOT/scripts/.gens_backups/" 2>/dev/null | head -1)
    if [ -n "$BACKUP" ] && [ -f "$PROJECT_ROOT/scripts/.gens_backups/$BACKUP" ]; then
        MD5_NEW=$(md5sum "$DISK2053" | awk '{print $1}')
        MD5_OLD=$(md5sum "$PROJECT_ROOT/scripts/.gens_backups/$BACKUP" | awk '{print $1}')
        if [ "$MD5_NEW" != "$MD5_OLD" ]; then
            ok "Диск 2053 модифицирован ГЕНС (MD5: ${MD5_NEW:0:12}...)"
        else
            fail "Диск 2053 не изменён (совпадает с копией $BACKUP)"
        fi
    fi
fi

# -------------------------------------------------------------------------
# 3. Запуск эмулятора
# -------------------------------------------------------------------------
section "3. ЗАПУСК ЭМУЛЯТОРА ($INI_BASENAME)"

pkill -9 besm6 2>/dev/null || true
rm -f "$DEBUG_FILE" "$LOG_FILE" "$OUTPUT_FILE" "$BOOT_LOG"

echo "Запуск: $BIN $INI_FILE"
"$BIN" "$INI_FILE" > "$BOOT_LOG" 2>&1 &
BESM_PID=$!
echo "$BESM_PID" > /tmp/besm6.pid
ok "PID эмулятора: $BESM_PID"

echo "Ожидание загрузки ОС (5 сек)..."
sleep 5

if ! kill -0 "$BESM_PID" 2>/dev/null; then
    fail "Эмулятор завершился (крах?)"
    echo "--- Последние строки boot-лога ---"
    tail -20 "$BOOT_LOG"
    exit 1
fi
ok "Эмулятор работает"

if ss -tlnp 2>/dev/null | grep -q ":$PORT "; then
    ok "Порт $PORT слушает"
else
    fail "Порт $PORT не открыт"
    pkill -9 besm6 2>/dev/null
    exit 1
fi

# -------------------------------------------------------------------------
# 4. DKS-регистрация (telnet-клиент dks_tty.py → целевая tty$DKS_TARGET)
# -------------------------------------------------------------------------
section "4. DKS-РЕГИСТРАЦИЯ (telnet-клиент → целевая tty$DKS_TARGET)"

# Один постоянный клиент (scripts/dks_tty.py):
#   - сразу регистрирует DKS-линию (dks_register)
#   - t+3: ETX («гид» ОС Диспак), далее ETX каждые 15 сек
#   - t+6: символ 'A' (для dks_poll)
#   - живёт DKS_TTY_LIFE сек, весь вывод ОС пишет в RX_LOG
rm -f "$RX_LOG"
python3 "$DKS_TTY" "$PORT" "$RX_LOG" "$DKS_TTY_LIFE" > /tmp/besm6_dks_tty_client.log 2>&1 &
NC_PID=$!
# Клиент живёт заданное время — ограничиваем сверху на случай зависания
( sleep "$((DKS_TTY_LIFE + 10))"; kill "$NC_PID" 2>/dev/null ) &
WATCHDOG_PID=$!

echo "Ожидание подключения и регистрации (2 сек)..."
sleep 2

# Фактическая линия подключения — из последнего "*** ttyN: a new connection"
LANDED=$(grep -aoE 'tty[0-9]+: a new connection' "$DEBUG_FILE" 2>/dev/null \
         | tail -1 | grep -oE '[0-9]+')
if [ -z "$LANDED" ]; then
    fail "Не обнаружено ни одного нового telnet-подключения"
    LANDED=0
    grep -ai 'new connection' "$DEBUG_FILE" 2>/dev/null | tail -5 || true
else
    ok "Подключение обнаружено: tty$LANDED"
fi

if [ "$LANDED" -eq "$DKS_TARGET" ]; then
    ok "tty$LANDED — ожидаемая целевая DKS-линия"
else
    fail "Ожидалась tty$DKS_TARGET, получено tty$LANDED"
fi

if [ "$LANDED" -gt 0 ] && grep -q "set tty$LANDED dks" "$INI_FILE" 2>/dev/null; then
    ok "tty$LANDED — DKS-линия в INI"
else
    fail "tty$LANDED не помечена dks в INI"
fi

if [ -f "$DEBUG_FILE" ]; then
    echo "--- DKS-строки в debug ---"
    grep -a '>>> DKS' "$DEBUG_FILE" 2>/dev/null | tail -15 || echo "  (нет)"
else
    fail "Нет debug-файла: $DEBUG_FILE"
fi

if grep -aq "DKS: terminal $LANDED registered" "$DEBUG_FILE" 2>/dev/null; then
    ok "DKS: терминал $LANDED зарегистрирован (dks_register)"
    grep -a "DKS: terminal $LANDED registered" "$DEBUG_FILE" | tail -3
else
    fail "DKS: регистрация терминала $LANDED не обнаружена"
    grep -a 'DKS: terminal' "$DEBUG_FILE" 2>/dev/null | tail -10 || true
fi

if grep -aq '>>> DKS: PRP=' "$DEBUG_FILE" 2>/dev/null; then
    ok "DKS: PRP установлен (ПРП12 — SREQ, запрос S-терминала)"
    grep -a '>>> DKS: PRP=' "$DEBUG_FILE" | tail -3
else
    fail "DKS: PRP_DKS_SREQ не обнаружен"
fi

# -------------------------------------------------------------------------
# 5. DKS-ввод символа (dks_poll → PRP_DKS_TERMREQ / ПРП7)
# -------------------------------------------------------------------------
section "5. DKS-ВВОД СИМВОЛА (dks_poll → ПРП7)"

echo "Символ 'A' отправлен клиентом на t+6 сек; ожидание доставки (2 сек)..."
sleep 2

if grep -aq "DKS: char.*terminal $LANDED" "$DEBUG_FILE" 2>/dev/null; then
    ok "DKS: символ получен (dks_poll → tty$LANDED)"
    grep -a "DKS: char.*terminal $LANDED" "$DEBUG_FILE" | tail -3
else
    if grep -aq "DKS: char" "$DEBUG_FILE" 2>/dev/null; then
        ok "DKS: символ получен (dks_poll) — см. лог"
        grep -a "DKS: char" "$DEBUG_FILE" | tail -5
    else
        fail "DKS: символ не получен (dks_poll)"
    fi
fi

# -------------------------------------------------------------------------
# 6. РЕАКЦИЯ ОС НА ETX (гид Диспака: приглашение «ЭВМ-3, Т-NNN»)
# -------------------------------------------------------------------------
section "6. РЕАКЦИЯ ОС (ETX → гид → приглашение «ЭВМ-3, Т-NNN»)"

echo "Клиент послал ETX (t+3, повтор каждые 15 сек). Ожидание ответа ОС (${OS_WAIT} сек)..."
sleep "$OS_WAIT"

if grep -aq "DKS: char '?' (0003)" "$DEBUG_FILE" 2>/dev/null; then
    ok "ETX (0x03) доставлен в буфер S-терминала (kadopam_mem)"
else
    fail "ETX (0x03) не зафиксирован в debug"
fi

if [ -s "$RX_LOG" ]; then
    RX_TAIL=$(grep -av 'Connected to the\|Encoding is\|WRU\|Break to sim\|Type HYC\|^$' "$RX_LOG" 2>/dev/null | tail -5)
    if [ -n "$RX_TAIL" ]; then
        ok "Терминал получил данные от ОС:"
        echo "$RX_TAIL" | sed 's/^/    /'
    else
        fail "От ОС пришёл только telnet-баннер (данных нет)"
    fi
else
    fail "RX-лог пуст: $RX_LOG"
fi

if grep -aq 'ЭВМ-3, Т-' "$RX_LOG" 2>/dev/null; then
    ok "ПРИГЛАШЕНИЕ ОС: «ЭВМ-3, Т-…» (гид Диспака выполнен)"
    grep -a 'ЭВМ-3, Т-' "$RX_LOG" | head -2 | sed 's/^/    /'
elif grep -aq 'Т-0' "$RX_LOG" 2>/dev/null; then
    ok "Приглашение Т-0NN (вариант формата)"
    grep -a 'Т-0' "$RX_LOG" | head -2 | sed 's/^/    /'
else
    fail "Приглашение «ЭВМ-3, Т-NNN» не получено"
    echo "  Подсказка: ОС могла ещё грузиться; см. physobm в $LOG_FILE и"
    echo "  '>>> KDP read' в $DEBUG_FILE (опрос КРК = СВЯЗЬ7 активна)"
fi

# Что видно в RX-логе (для диагностики)
if [ -s "$RX_LOG" ]; then
    echo "--- Последние строки RX-лога ---"
    tail -c 600 "$RX_LOG" | cat -v
    echo ""
fi

# -------------------------------------------------------------------------
# 7. ИТОГИ
# -------------------------------------------------------------------------
section "7. ИТОГИ (INI: $INI_BASENAME, целевая tty$DKS_TARGET)"

echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "  ╔══════════════════════════════════════════╗"
    echo "  ║   ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ ✓                ║"
    echo "  ╚══════════════════════════════════════════╝"
    RC=0
else
    echo "  ╔══════════════════════════════════════════╗"
    echo "  ║   ОБНАРУЖЕНЫ ПРОБЛЕМЫ ($FAIL) ✗          ║"
    echo "  ╚══════════════════════════════════════════╝"
    RC=1
fi

# Остановка эмулятора, telnet-клиента и watchdog
pkill -9 besm6 2>/dev/null || true
kill "$NC_PID" 2>/dev/null || true
kill "$WATCHDOG_PID" 2>/dev/null || true
echo ""
echo "Эмулятор остановлен."

if [ -f "$DEBUG_FILE" ]; then
    echo ""
    echo "--- Последние 15 строк debug ---"
    tail -15 "$DEBUG_FILE" 2>/dev/null
fi

exit $RC
#!/bin/bash
# gens.sh — Работа с генератором системы ГЕНС (ОС Диспак)
#
# Трансляция нужной конфигурации ГЕНС пакетным эмулятором dispak.
# Изменения производятся на диске 2053, расположенном в каталоге,
# заданном переменной окружения BESM6_PATH.
#
# Конфигурации ГЕНС (.b6) находятся в:
#   ~/Yandex.Disk/re_dispak/re-dispak/gens/
#
# Использование:
#   ./scripts/gens.sh status            — проверить BESM6_PATH и диск 2053
#   ./scripts/gens.sh list              — список доступных конфигураций
#   ./scripts/gens.sh configs           — терминалы каждой конфигурации
#   ./scripts/gens.sh run <config>      — транслировать конфигурацию (перезапись 2053)
#   ./scripts/gens.sh backup            — резервная копия диска 2053
#   ./scripts/gens.sh restore [файл]    — восстановить диск 2053
#
# Проверка переменной BESM6_PATH выполняется командой besmtool list.

set -e

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
GENS_DIR="/home/azizz/Yandex.Disk/re_dispak/re-dispak/gens"
BACKUP_DIR="$PROJECT_ROOT/scripts/.gens_backups"
DISK_NUM=2053
LOG_FILE="/tmp/gens_run.log"

# -------------------------------------------------------------------------
# Вспомогательные функции
# -------------------------------------------------------------------------

die() {
    echo "✗ $*" >&2
    exit 1
}

# Проверка переменной BESM6_PATH и наличия диска 2053.
check_env() {
    if [ -z "$BESM6_PATH" ]; then
        die "BESM6_PATH не задана. Экспортируйте: export BESM6_PATH=/usr/local/share/besm6"
    fi
    if [ ! -d "$BESM6_PATH" ]; then
        die "Каталог BESM6_PATH не существует: $BESM6_PATH"
    fi
    DISK_FILE="$BESM6_PATH/$DISK_NUM"
    if [ ! -f "$DISK_FILE" ]; then
        die "Диск $DISK_NUM не найден в $BESM6_PATH"
    fi
}

# Проверка, что besmtool видит диск 2053 (как требует пользователь).
check_besmtool() {
    command -v besmtool >/dev/null 2>&1 || die "besmtool не найден в PATH"
    if ! besmtool list 2>/dev/null | awk '{print $1}' | grep -qx "$DISK_NUM"; then
        die "besmtool list не показывает диск $DISK_NUM. Проверьте BESM6_PATH=$BESM6_PATH"
    fi
}

# -------------------------------------------------------------------------
# status — проверить BESM6_PATH и диск 2053
# -------------------------------------------------------------------------
do_status() {
    echo "=== Проверка BESM6_PATH ==="
    if [ -z "$BESM6_PATH" ]; then
        echo "✗ BESM6_PATH не задана"
        exit 1
    fi
    echo "  BESM6_PATH = $BESM6_PATH"
    echo ""
    echo "=== besmtool list ==="
    besmtool list || die "besmtool list завершился с ошибкой"
    echo ""
    DISK_FILE="$BESM6_PATH/$DISK_NUM"
    if [ -f "$DISK_FILE" ]; then
        echo "✓ Диск $DISK_NUM: $(stat -c '%s байт, изменён %y' "$DISK_FILE")"
    else
        echo "✗ Диск $DISK_NUM не найден в $BESM6_PATH"
        exit 1
    fi
    echo ""
    echo "=== Каталог конфигураций ГЕНС ==="
    echo "  $GENS_DIR"
    if [ -d "$GENS_DIR" ]; then
        nconf=$(ls -1 "$GENS_DIR"/*.b6 2>/dev/null | wc -l)
        echo "✓ Найдено конфигураций: $nconf"
    else
        echo "✗ Каталог конфигураций не найден"
        exit 1
    fi
}

# -------------------------------------------------------------------------
# list — список доступных конфигураций
# -------------------------------------------------------------------------
do_list() {
    echo "=== Доступные конфигурации ГЕНС ==="
    echo "  Каталог: $GENS_DIR"
    echo ""
    if ! ls "$GENS_DIR"/*.b6 >/dev/null 2>&1; then
        echo "  Конфигурации не найдены"
        exit 1
    fi
    printf "  %-26s %s\n" "Файл" "Размер"
    for f in "$GENS_DIR"/*.b6; do
        printf "  %-26s %s\n" "$(basename "$f")" "$(stat -c '%s' "$f") байт"
    done
}

# -------------------------------------------------------------------------
# configs — терминалы каждой конфигурации
# -------------------------------------------------------------------------
do_configs() {
    echo "=== Терминалы в конфигурациях ГЕНС ==="
    echo ""
    for f in "$GENS_DIR"/*.b6; do
        echo "--- $(basename "$f") ---"
        grep -E '^(ТТ|ТЕРМ|TEPM|MЛЗAГ|МЛЗАГ|HPДИC|НРДИС|ОЗУ)' "$f" 2>/dev/null \
            | sed 's/^/  /' || echo "  (директивы не найдены)"
        echo ""
    done
}

# -------------------------------------------------------------------------
# backup — резервная копия диска 2053
# -------------------------------------------------------------------------
do_backup() {
    check_env
    mkdir -p "$BACKUP_DIR"
    local ts; ts=$(date +%Y%m%d_%H%M%S)
    local dst="$BACKUP_DIR/${DISK_NUM}_${ts}"
    cp -p "$DISK_FILE" "$dst"
    echo "✓ Резервная копия создана: $dst"
    echo "  Размер: $(stat -c '%s' "$dst") байт"
}

# -------------------------------------------------------------------------
# restore [файл] — восстановить диск 2053
# -------------------------------------------------------------------------
do_restore() {
    check_env
    local src="$1"
    if [ -z "$src" ]; then
        # По умолчанию — последняя резервная копия
        src=$(ls -1t "$BACKUP_DIR"/${DISK_NUM}_* 2>/dev/null | head -1)
        if [ -z "$src" ]; then
            die "Резервных копий в $BACKUP_DIR нет. Укажите файл: restore <файл>"
        fi
    fi
    if [ ! -f "$src" ]; then
        die "Файл не найден: $src"
    fi
    cp -p "$src" "$DISK_FILE"
    echo "✓ Диск $DISK_NUM восстановлен из: $src"
    echo "  Размер: $(stat -c '%s' "$DISK_FILE") байт"
}

# -------------------------------------------------------------------------
# run <config> — транслировать конфигурацию через dispak
# -------------------------------------------------------------------------
do_run() {
    local config="$1"
    [ -z "$config" ] && die "Укажите конфигурацию: run <config> (например, gens_exp)"

    # Дополнить расширением .b6 при необходимости
    local cfg_file
    if [[ "$config" == *.b6 ]]; then
        cfg_file="$GENS_DIR/$config"
    else
        cfg_file="$GENS_DIR/$config.b6"
    fi
    [ -f "$cfg_file" ] || die "Конфигурация не найдена: $cfg_file"

    echo "=== Трансляция конфигурации ГЕНС ==="
    echo "  Конфигурация: $(basename "$cfg_file")"
    echo "  BESM6_PATH:   $BESM6_PATH"
    echo "  Диск:         $DISK_NUM"
    echo ""

    # Предварительные проверки
    check_env
    check_besmtool

    # Резервная копия перед модификацией
    echo "=== Резервная копия диска $DISK_NUM ==="
    do_backup
    echo ""

    # Проверка эмулятора dispak
    command -v dispak >/dev/null 2>&1 || die "dispak не найден в PATH"

    # Трансляция: dispak использует BESM6_PATH (или --path) для поиска дисков.
    # Запуск из каталога конфигураций.
    echo "=== Запуск dispak ==="
    echo "  Лог: $LOG_FILE"
    echo ""
    (
        cd "$GENS_DIR"
        dispak --path="$BESM6_PATH" "$(basename "$cfg_file")"
    ) > "$LOG_FILE" 2>&1
    local rc=$?

    if [ $rc -eq 0 ]; then
        echo "✓ Трансляция завершена успешно (exit 0)"
        echo "  Диск $DISK_NUM обновлён: $(stat -c '%y' "$BESM6_PATH/$DISK_NUM")"
        echo ""
        echo "--- Последние строки вывода dispak ---"
        tail -20 "$LOG_FILE" 2>/dev/null || true
    else
        echo "✗ Трансляция завершилась с ошибкой (exit $rc)"
        echo ""
        echo "--- Последние 30 строк вывода dispak ---"
        tail -30 "$LOG_FILE" 2>/dev/null || true
        echo ""
        echo "  Диск мог быть частично изменён."
        echo "  Для отката: ./scripts/gens.sh restore"
        exit $rc
    fi
}

# -------------------------------------------------------------------------
# Маршрутизация подкоманд
# -------------------------------------------------------------------------
case "${1:-}" in
    status)   do_status ;;
    list)     do_list ;;
    configs)  do_configs ;;
    run)      shift; do_run "$@" ;;
    backup)   do_backup ;;
    restore)  shift; do_restore "$@" ;;
    "")
        echo "gens.sh — Работа с генератором системы ГЕНС (ОС Диспак)"
        echo ""
        echo "Использование: ./scripts/gens.sh <команда> [аргументы]"
        echo ""
        echo "Команды:"
        echo "  status            — проверить BESM6_PATH и диск 2053 (besmtool list)"
        echo "  list              — список доступных конфигураций"
        echo "  configs           — терминалы каждой конфигурации"
        echo "  run <config>      — транслировать конфигурацию (перезапись диска 2053)"
        echo "  backup            — резервная копия диска 2053"
        echo "  restore [файл]    — восстановить диск 2053 (по умолчанию — последняя копия)"
        echo ""
        echo "Примеры:"
        echo "  ./scripts/gens.sh status"
        echo "  ./scripts/gens.sh run gens_exp"
        echo "  ./scripts/gens.sh restore"
        ;;
    *)
        echo "Неизвестная команда: $1" >&2
        echo "См.: ./scripts/gens.sh" >&2
        exit 1
        ;;
esac
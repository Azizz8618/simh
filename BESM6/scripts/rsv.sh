#!/bin/bash
# rsv.sh — РЕЗЕРВНАЯ КОПИЯ КОНФИГУРАЦИИ (команда РЗВ)
#
# По COMMANDS.md §9.1:
#   1. Создать каталог с меткой времени в config_backup/
#   2. Скопировать файлы проекта (.clinerules, AGENTS.md, COMMANDS.md,
#      GENS_AGENTS.md, TEAM_AGENTS.md; README.md если есть)
#   3. Скопировать глобальный ~/.clinerules -> global_.clinerules
#   4. Скопировать глобальные каталоги (~/.cline/skills, ~/.cline/cache,
#      ~/.vscode) — без runtime-данных ~/.cline/data
#   5. Создать MANIFEST.txt с контрольными суммами
#
# По .clinerules: РЗВ — единственная команда, делающая git commit
#   (commit только staged config-файлов и скриптов).

set -e

PROJECT_ROOT="/home/azizz/Yandex.Disk/simh/BESM6"
BACKUP_ROOT="/home/azizz/Yandex.Disk/config_backup"
PROJECT_FILES=(.clinerules AGENTS.md COMMANDS.md GENS_AGENTS.md TEAM_AGENTS.md README.md)
COMMIT_MSG="Обновлены конфигурационные файлы и скрипты"

cd "$PROJECT_ROOT"

TS=$(date +%Y%m%d_%H%M%S)
BACKUP_DIR="$BACKUP_ROOT/BESM6_$TS"
mkdir -p "$BACKUP_DIR/global_.cline"

echo "=== РЕЗЕРВНАЯ КОПИЯ КОНФИГУРАЦИИ ==="
echo "  Каталог: $BACKUP_DIR"
echo ""

# 1. Файлы проекта
echo "--- Файлы проекта ---"
for f in "${PROJECT_FILES[@]}"; do
    if [ -f "$f" ]; then
        cp -p "$f" "$BACKUP_DIR/"
        echo "  ✓ $f"
    else
        echo "  — $f (отсутствует, пропущен)"
    fi
done

# 2. Глобальный файл
echo ""
echo "--- Глобальный файл ---"
cp -p "$HOME/.clinerules" "$BACKUP_DIR/global_.clinerules"
echo "  ✓ global_.clinerules"

# 3. Глобальные каталоги (только конфигурация, без runtime data)
echo ""
echo "--- Глобальные каталоги ---"
cp -rp "$HOME/.cline/skills" "$BACKUP_DIR/global_.cline/skills"
echo "  ✓ global_.cline/skills"
cp -rp "$HOME/.cline/cache" "$BACKUP_DIR/global_.cline/cache"
echo "  ✓ global_.cline/cache"
cp -rp "$HOME/.vscode" "$BACKUP_DIR/global_.vscode"
echo "  ✓ global_.vscode"

# 4. MANIFEST.txt с контрольными суммами
echo ""
echo "--- MANIFEST.txt ---"
(
    cd "$BACKUP_DIR"
    find . -type f ! -name MANIFEST.txt -print0 \
        | sort -z \
        | xargs -0 md5sum > MANIFEST.txt
)
nfiles=$(find "$BACKUP_DIR" -type f ! -name MANIFEST.txt | wc -l)
echo "  ✓ Контрольные суммы для $nfiles файлов"

echo ""
echo "✓ Резервная копия создана: $BACKUP_DIR"

# 5. git commit (только по РЗВ)
echo ""
echo "=== git commit ==="
git add scripts/*.sh .clinerules AGENTS.md TEAM_AGENTS.md GENS_AGENTS.md COMMANDS.md 2>/dev/null || true
if git diff --cached --quiet; then
    echo "  Нет изменений для коммита"
else
    git commit -m "$COMMIT_MSG" >/dev/null
    echo "  ✓ Коммит: $COMMIT_MSG"
    git log --oneline -1 | sed 's/^/  /'
fi

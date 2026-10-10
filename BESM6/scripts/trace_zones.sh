#!/bin/bash
# trace_zones.sh — Анализ трассы БЭСМ-6: сортировка, поиск зон/адресов
#
# Использование:
#   ./scripts/trace_zones.sh /tmp/tr12.txt                    — полный анализ
#   ./scripts/trace_zones.sh /tmp/tr12.txt --summary           — сводка по зонам
#   ./scripts/trace_zones.sh /tmp/tr12.txt --unique            — уникальные адреса
#   ./scripts/trace_zones.sh /tmp/tr12.txt --zone 0677         — только СВЯЗЬ7
#   ./scripts/trace_zones.sh /tmp/tr12.txt --grep ТРМДКС       — grep по метке
#   ./scripts/trace_zones.sh /tmp/tr12.txt --sort count        — сортировка по частоте

set -e
BESM6_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RE_DISPAK="$HOME/Yandex.Disk/re_dispak/re-dispak"
TRACE_ANALYZER="$RE_DISPAK/trace_analyze.pl"

TRACE_FILE=""
GREP_PATTERN=""
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --grep)
            GREP_PATTERN="$2"; shift 2
            ;;
        --summary|--unique)
            EXTRA_ARGS+=("$1"); shift
            ;;
        --zone|--sort|--labels-dir)
            EXTRA_ARGS+=("$1" "$2"); shift 2
            ;;
        -*)
            EXTRA_ARGS+=("$1"); shift
            ;;
        *)
            TRACE_FILE="$1"; shift
            ;;
    esac
done

if [[ -z "$TRACE_FILE" ]]; then
    echo "Использование: $0 <trace.txt> [--summary] [--unique] [--zone ZZZZ] [--grep МЕТКА] [--sort count]"
    exit 1
fi

if [[ ! -f "$TRACE_FILE" ]]; then
    echo "Ошибка: файл трассы не найден: $TRACE_FILE"
    exit 1
fi

# Запуск анализатора
if [[ -x "$TRACE_ANALYZER" ]]; then
    if [[ -n "$GREP_PATTERN" ]]; then
        # Режим grep: full output + grep по метке
        perl "$TRACE_ANALYZER" --labels-dir "$RE_DISPAK" "${EXTRA_ARGS[@]}" "$TRACE_FILE" 2>/dev/null
        echo ""
        echo "── grep '$GREP_PATTERN' по разрешённым меткам ──"
        perl "$TRACE_ANALYZER" --labels-dir "$RE_DISPAK" "${EXTRA_ARGS[@]}" "$TRACE_FILE" 2>/dev/null \
            | grep "$GREP_PATTERN" | head -30 || echo "  (нет совпадений)"
    else
        perl "$TRACE_ANALYZER" --labels-dir "$RE_DISPAK" "${EXTRA_ARGS[@]}" "$TRACE_FILE"
    fi
else
    echo "trace_analyze.pl не найден: $TRACE_ANALYZER"
    echo "Fallback: ручной анализ"
    echo ""
    echo "── Зоны в трассе ──"
    grep -oP '\d{4}(?=[ ]\d{4}[LR])' "$TRACE_FILE" | sort | uniq -c | sort -rn
    echo ""
    if [[ -n "$GREP_PATTERN" ]]; then
        echo "── grep '$GREP_PATTERN' ──"
        grep "$GREP_PATTERN" "$TRACE_FILE" | head -30
    fi
fi

# Навыки Cline для проекта ДИСПАК

## Структура

Все навыки хранятся **глобально** в `~/.cline/skills/` (каждый навык — отдельная папка с `SKILL.md`).
В локальной директории `.cline/skills/` — только симлинки на глобальные SKILL.md и проектный README.

## Установленные навыки

| Навык | Описание | Триггеры | Где хранится |
|-------|----------|----------|-------------|
| `code-map` | Сжатие C-файлов в карты кода | "карта кода", "обновить карты" | Симлинк → `~/.cline/skills/code-map/` |
| `disbesm6-disasm` | Дизассемблер БЭСМ-6 (disbesm6) | "дизассемблируй", "disbesm6", "дизассемблер", "disasm", "выкручивание" | Симлинк → `~/.cline/skills/disbesm6-disasm/` |
| `dispak-commands` | Полный справочник команд ОС ДИСПАК | "прокомментируй", "объясни команду", "исправь" | Симлинк → `~/.cline/skills/dispak-commands/` |
| `dispak-modules` | Структура модулей ОС ДИСПАК | вопросы о модулях | Симлинк → `~/.cline/skills/dispak-modules/` |
| `dispak-tracer` | Анализ трасс выполнения | "проанализируй трассу", "разбери tr" | Симлинк → `~/.cline/skills/dispak-tracer/` |
| `init-session` | Инициализация сессии (Шаг 0–6: определение проекта, чтение файлов, загрузка навыков, активация token-saver) | "init-session", "начало сессии", "инициализация" | Симлинк → `~/.cline/skills/init-session/` |
| `small-portions` | Анти-таймаут: дробление сложных задач | "дроби задачу", "сложная задача", "таймаут" | Симлинк → `~/.cline/skills/small-portions/` |
| `token-saver` | Экономия токенов + адаптивная температура | "экономь токены", "оптимизируй" | Симлинк → `~/.cline/skills/token-saver/` |
| `task-splitter` | Декомпозиция на сабагентов (автоматически по Д4) | "разбей на агентов", "декомпозиция", "сабагенты" | Симлинк → `~/.cline/skills/task-splitter/` |
| `bemsh-analyzer` | Анализ исходников и листингов МАКРО-БЕМШ (.be, .lst) | "анализируй листинг", "разбери .lst", "разбери .be", "автокод", "бемш" | Симлинк → `~/.cline/skills/bemsh-analyzer/` |
| `madlen-analyzer` | Анализ исходников и листингов МАДЛЕН (.mad, .lst) | "анализируй МАДЛЕН", "разбери .mad", "автокод", "мадлен" | Симлинк → `~/.cline/skills/madlen-analyzer/` |

## Использование

Навыки загружаются автоматически при совпадении запроса с `triggers`.

Принудительная активация через slash-команды:
```
/code-map
/disbesm6-disasm
/dispak-commands
/dispak-modules
/dispak-tracer
/token-saver
/small-portions
/init-session
/task-splitter
/bemsh-analyzer
/madlen-analyzer
```

## Структура файлов

```
~/.cline/skills/
├── bemsh-analyzer/
│   └── SKILL.md
├── madlen-analyzer/
│   └── SKILL.md
├── disbesm6-disasm/
│   └── SKILL.md
├── dispak-commands/
│   └── SKILL.md
├── dispak-modules/
│   └── SKILL.md
├── dispak-tracer/
│   └── SKILL.md
├── init-session/
│   └── SKILL.md
├── token-saver/
│   └── SKILL.md
├── code-map/
│   └── SKILL.md
├── small-portions/
│   └── SKILL.md
├── task-splitter/
│   └── SKILL.md
└── README.md
```

Локальный проект:
```
.cline/skills/
├── bemsh-analyzer.md -> ~/.cline/skills/bemsh-analyzer/SKILL.md
├── madlen-analyzer.md -> ~/.cline/skills/madlen-analyzer/SKILL.md
├── disbesm6-disasm.md -> ~/.cline/skills/disbesm6-disasm/SKILL.md
├── dispak-commands.md -> ~/.cline/skills/dispak-commands/SKILL.md
├── dispak-modules.md -> ~/.cline/skills/dispak-modules/SKILL.md
├── dispak-tracer.md -> ~/.cline/skills/dispak-tracer/SKILL.md
├── init-session.md -> ~/.cline/skills/init-session/SKILL.md
├── token-saver.md -> ~/.cline/skills/token-saver/SKILL.md
├── code-map.md -> ~/.cline/skills/code-map/SKILL.md
├── small-portions.md -> ~/.cline/skills/small-portions/SKILL.md
├── task-splitter.md -> ~/.cline/skills/task-splitter/SKILL.md
└── README.md (проектный документ)
```

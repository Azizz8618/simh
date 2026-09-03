# Навыки Cline для проекта ДИСПАК

## Установленные навыки

| Файл | Назначение |
|------|------------|
| `bemsh-analyzer.md` | Анализ листингов МАКРО-БЕМШ |
| `dispak-tracer.md` | Анализ трасс выполнения |
| `token-saver.md` | Экономия токенов |
| `dispak-commands.md` | Стандартные команды |
| `dispak-modules.md` | Структура модулей ОС |
| `code-map.md` | Сжатие C-файлов в структурные карты |

## Рекомендуемые MCP-серверы

### Для работы с файлами

```bash
# Filesystem MCP
npm install -g @modelcontextprotocol/server-filesystem

# Memory MCP (память между сессиями)
npm install -g @modelcontextprotocol/server-memory
```

### Для разработки

```bash
# Git MCP
npm install -g @modelcontextprotocol/server-git

# GitHub MCP
npm install -g @modelcontextprotocol/server-github

# PostgreSQL/SQLite (если нужны БД)
npm install -g @modelcontextprotocol/server-postgres
npm install -g @modelcontextprotocol/server-sqlite
```

### Для поиска

```bash
# Brave Search MCP
npm install -g @modelcontextprotocol/server-brave-search
```

## Настройка MCP

Добавьте в конфигурацию Cline (обычно `~/.cline/config.json` или через UI):

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/project"]
    },
    "memory": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-memory"]
    }
  }
}
```

## Использование

Навыки активируются автоматически при совпадении триггеров в запросах.

Для принудительной активации используйте фразы:
- "экономь токены" — для минимизации расхода
- "проанализируй трассу" — для разбора tr-файлов
- "анализируй листинг" — для разбора .lst файлов

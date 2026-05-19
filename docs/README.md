# OpenRouter MCP Server

MCP-сервер на C++/Qt для работы с OpenRouter API (и любым совместимым API).  
Реализован поверх `QMCPServer` — базового класса с поддержкой JSON-RPC 2.0 через stdin/stdout.

---

## Возможности

| Инструмент | Описание |
|---|---|
| `list_accounts` | Список настроенных аккаунтов (без API-ключей и путей) |
| `add_account` | Добавить аккаунт и сохранить в файл |
| `remove_account` | Удалить аккаунт |
| `get_paid_models` | Список платных моделей для указанного аккаунта |
| `get_free_models` | Список бесплатных моделей для указанного аккаунта |
| `send_chat_request` | Отправить chat-completion запрос к модели |

### Безопасность

- **API-ключи никогда не возвращаются** в ответах инструментов
- **Путь к файлу аккаунтов** не раскрывается модели
- В `list_accounts` возвращается только `id`, `name`, `description` и `provider` (хост без протокола и пути)

---

## Сборка

### Зависимости

- Qt 5.15+ или Qt 6.x (модули `Core`, `Network`)
- C++17 компилятор (GCC 8+, Clang 7+, MSVC 2019+)

### qmake

```bash
qmake openrouter_mcp.pro
make -j$(nproc)
# Бинарь: openrouter-mcp
```

### CMake (опционально)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## Запуск

```bash
# Файл аккаунтов рядом с бинарём (по умолчанию):
./openrouter-mcp

# Явное указание пути:
./openrouter-mcp --accounts /etc/openrouter/accounts.json

# Отладочный вывод в stderr:
./openrouter-mcp --debug

# Справка:
./openrouter-mcp --help
```

Если файл аккаунтов не найден — он создаётся автоматически с примером записи.

---

## Файл аккаунтов

Формат: JSON, поле `"accounts"` — массив объектов.

```json
{
    "_comment": "Keep this file secret!",
    "accounts": [
        {
            "id":          "or-main",
            "name":        "OpenRouter Main Account",
            "api_key":     "sk-or-v1-...",
            "base_url":    "https://openrouter.ai/api/v1",
            "description": "Primary account"
        }
    ]
}
```

| Поле | Обязательное | Описание |
|---|---|---|
| `id` | ✅ | Уникальный slug, используется в инструментах |
| `name` | ✅ | Человекочитаемое имя |
| `api_key` | ✅ | API-ключ провайдера |
| `base_url` | — | URL API (default: `https://openrouter.ai/api/v1`) |
| `description` | — | Произвольное описание |

---

## Подключение к Claude Desktop (mcp_servers.json)

```json
{
  "mcpServers": {
    "openrouter": {
      "command": "/path/to/openrouter-mcp",
      "args": ["--accounts", "/path/to/accounts.json"]
    }
  }
}
```

---

## Примеры вызовов инструментов

### Список аккаунтов
```json
{ "name": "list_accounts", "arguments": {} }
```

### Добавить аккаунт
```json
{
  "name": "add_account",
  "arguments": {
    "id": "groq-main",
    "name": "Groq Cloud",
    "api_key": "gsk_...",
    "base_url": "https://api.groq.com/openai/v1",
    "description": "Ultra-fast Llama inference"
  }
}
```

### Получить бесплатные модели
```json
{
  "name": "get_free_models",
  "arguments": { "account_id": "or-main" }
}
```

### Отправить запрос
```json
{
  "name": "send_chat_request",
  "arguments": {
    "account_id": "or-main",
    "model": "openai/gpt-4o-mini",
    "messages": [
      { "role": "user", "content": "Привет! Кто ты?" }
    ],
    "max_tokens": 512,
    "temperature": 0.7
  }
}
```

---

## Структура файлов

```
openrouter-mcp/
├── include/           # Заголовки (.h)
│   ├── QMCPServer.h
│   ├── McpLogger.h
│   ├── OpenRouterMCPServer.h
│   └── ServerConfig.h
├── src/               # Исходный код (.cpp)
│   ├── QMCPServer.cpp
│   ├── McpLogger.cpp
│   ├── main.cpp
│   └── OpenRouterMCPServer.cpp
├── docs/              # Документация
│   ├── README.md
│   └── accounts.json.example
├── openrouter_mcp.pro # qmake проект
├── .gitignore
└── LICENSE
```
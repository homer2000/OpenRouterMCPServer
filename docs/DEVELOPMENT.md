# Разработка (Development Guide)

## Начало работы

### 1. Клонирование репозитория
```bash
git clone <repository-url>
cd OpenrouterMCP
```

### 2. Создание аккаунта OpenRouter
1. Зарегистрируйтесь на [openrouter.ai](https://openrouter.ai)
2. Перейдите в раздел API Keys
3. Создайте новый ключ

### 3. Настройка аккаунта
```bash
cp docs/accounts.json.example accounts.json
# Отредактируйте accounts.json, добавив ваш API-ключ
```

## Сборка

### Требования
- Qt 5.15.2 GCC 64-bit (рекомендуется)
- Qt 6.5.1+ (также поддерживается)
- GCC 8+ или Clang 7+
- C++17 компилятор

### qmake (основной способ)
```bash
# Отладка
qmake openrouter_mcp.pro CONFIG+="debug"
make -j$(nproc)

# Релиз
qmake openrouter_mcp.pro CONFIG+="release"
make -j$(nproc)
```

### CMake (альтернативный способ)
```cmake
# CMakeLists.txt (дополнительно)
cmake_minimum_required(VERSION 3.16)
project(OpenRouterMCP)

set(CMAKE_CXX_STANDARD 17)

find_package(Qt6 REQUIRED COMPONENTS Core Network)
# или Qt5:
# find_package(Qt5 REQUIRED COMPONENTS Core Network)

add_executable(openrouter-mcp
    src/main.cpp
    src/OpenRouterMCPServer.cpp
    src/McpLogger.cpp
    src/QMCPServer.cpp
)

target_include_directories(openrouter-mcp PRIVATE include)
target_link_libraries(openrouter-mcp Qt6::Core Qt6::Network)
```

## Запуск

```bash
# С аккаунтами из файла
./openrouter-mcp --accounts accounts.json

# С отладкой
./openrouter-mcp --debug --accounts accounts.json

# С аргументами по умолчанию
./openrouter-mcp
```

## Добавление нового инструмента

### 1. Объявление в OpenRouterMCPServer.h
```cpp
// В классе OpenRouterMCPServer
void handleNewTool(const QString &id, const QVariantMap &args);
```

### 2. Реализация в OpenRouterMCPServer.cpp
```cpp
void OpenRouterMCPServer::handleNewTool(const QString &id, const QVariantMap &args)
{
    if (id == "new_tool") {
        // Ваш код
        sendResponse(createSuccessResult(...));
    }
}
```

### 3. Регистрация инструмента
```cpp
// В конструкторе или initTools()
registerTool("new_tool", "Описание инструмента");
```

## Стиль кода

- Используйте `camelCase` для переменных и методов
- Используйте `PascalCase` для классов
- Добавляйте комментарии к открытым методам
- Соблюдайте принцип: "Магия должна быть минимальной"

## Отладка

### Включение вывода отладочной информации
```cpp
// В main.cpp раскомментируйте:
server.setupLogger(
    QDir::tempPath() + "/openrouter-mcp-server.log",
    McpLogger::Level::Debug
);
```

### Просмотр логов
```bash
# Логи в temp-каталоге
cat /tmp/openrouter-mcp-server.log

# Или используйте stderr (--debug)
./openrouter-mcp --debug 2>&1 | tee debug.log
```

## Тестирование

### Ручное тестирование через Claude Desktop
```json
{
  "mcpServers": {
    "openrouter": {
      "command": "/путь/до/openrouter-mcp",
      "args": ["--accounts", "/путь/до/accounts.json"]
    }
  }
}
```

### Автоматическое тестирование (будущая разработка)
```bash
# unit tests (Qt Test)
./tests/openrouter_tests
```

## Прокси-сервер

### Настройка через конфигурационный файл
```json
{
    "proxy": {
        "enabled": true,
        "type": "http",
        "host": "127.0.0.1",
        "port": 8080,
        "username": "user",
        "password": "pass"
    }
}
```

### Переменные окружения
```bash
# Системные прокси (автоматически используются)
export HTTP_PROXY=http://127.0.0.1:8080
export HTTPS_PROXY=http://127.0.0.1:8080
```

## Полезные команды

```bash
# Очистка сборки
rm -rf build/*
rm -f Makefile *.pro.user

# Полный пересбор
qmake openrouter_mcp.pro
make clean
make -j$(nproc)

# Проверка утечек памяти (Valgrind)
valgrind --leak-check=full ./openrouter-mcp
```
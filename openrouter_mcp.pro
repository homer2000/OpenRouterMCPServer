QT       = core network
QT      -= gui

TARGET   = openrouter-mcp
TEMPLATE = app
CONFIG  += c++17

# Warn on deprecated Qt APIs
DEFINES += QT_DEPRECATED_WARNINGS

# Include directories
INCLUDEPATH += include

HEADERS += \
    include/QMCPServer.h \
    include/McpLogger.h \
    include/OpenRouterMCPServer.h \
    include/ServerConfig.h

SOURCES += \
    src/QMCPServer.cpp \
    src/McpLogger.cpp \
    src/main.cpp \
    src/OpenRouterMCPServer.cpp

# ── Оптимизация для релиза ────────────────────────────────────────────────────
CONFIG(release, debug|release) {
    DEFINES += QT_NO_DEBUG_OUTPUT
}

# ── Linux: статическая линковка стандартной библиотеки (опционально) ──────────
# unix:!macx: QMAKE_LFLAGS += -static-libstdc++ -static-libgcc
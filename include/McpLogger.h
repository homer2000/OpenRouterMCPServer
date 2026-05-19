#pragma once

#include <QString>
#include <QStringList>
#include <QFile>
#include <QMutex>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>

// ─────────────────────────────────────────────────────────────────────────────
// McpLogger
//
// Файловый логгер с:
//   • ротацией по размеру (defaultMaxBytes = 10 МБ, хранится до 3 архивов)
//   • уровнями: DEBUG < INFO < WARN < ERROR
//   • маскировкой секретов (API-ключи, Authorization-заголовки)
//   • дублированием в stderr (опционально)
//   • потокобезопасностью (QMutex)
//
// Использование:
//   McpLogger::instance().open("/var/log/openrouter-mcp/server.log");
//   McpLogger::instance().setLevel(McpLogger::Level::Debug);
//   MCP_LOG_INFO("Server started");
//   MCP_LOG_REQUEST(url, headers, body);
//   MCP_LOG_RESPONSE(url, httpCode, contentType, body);
// ─────────────────────────────────────────────────────────────────────────────

class McpLogger
{
public:
    enum class Level { Debug = 0, Info, Warn, Error };

    static McpLogger &instance();

    // Открыть лог-файл. Если filePath пустой — пишем только в stderr.
    bool open(const QString &filePath);
    void close();

    void setLevel(Level level)          { m_level = level; }
    void setEchoToStderr(bool on)       { m_echoStderr = on; }
    void setMaxFileBytes(qint64 bytes)  { m_maxBytes = bytes; }
    void setMaxBackups(int n)           { m_maxBackups = n; }

    // Базовое логирование
    void log(Level level, const QString &category, const QString &message);

    // Специализированные методы — маскируют секреты автоматически
    void logRequest (const QString &url,
                     const QString &method,
                     const QByteArray &body,
                     const QString &apiKeyHint = {});  // первые 8 символов ключа для идентификации

    void logResponse(const QString &url,
                     int            httpCode,
                     const QString &contentType,
                     const QByteArray &body);

    void logEvent   (const QString &event, const QJsonObject &details = {});

    // Удобные обёртки
    void debug(const QString &msg) { log(Level::Debug, "DEBUG", msg); }
    void info (const QString &msg) { log(Level::Info,  "INFO",  msg); }
    void warn (const QString &msg) { log(Level::Warn,  "WARN",  msg); }
    void error(const QString &msg) { log(Level::Error, "ERROR", msg); }

private:
    McpLogger() = default;
    ~McpLogger() { close(); }
    McpLogger(const McpLogger &) = delete;
    McpLogger &operator=(const McpLogger &) = delete;

    void        writeRaw(const QString &line);
    void        rotate();
    QString     timestamp() const;
    static QString maskSecrets(const QString &text);
    static QString maskSecrets(const QByteArray &data);
    static QString prettyJson(const QByteArray &data);
    static QString levelTag(Level l);

    QFile   m_file;
    QMutex  m_mutex;
    Level   m_level      = Level::Debug;
    bool    m_echoStderr = true;
    qint64  m_maxBytes   = 10LL * 1024 * 1024;   // 10 МБ
    int     m_maxBackups = 3;
    QString m_filePath;
};

// ─────────────────────────────────────────────────────────────────────────────
// Макросы для удобства
// ─────────────────────────────────────────────────────────────────────────────

#define MCP_LOG_DEBUG(msg)   McpLogger::instance().debug(msg)
#define MCP_LOG_INFO(msg)    McpLogger::instance().info(msg)
#define MCP_LOG_WARN(msg)    McpLogger::instance().warn(msg)
#define MCP_LOG_ERROR(msg)   McpLogger::instance().error(msg)

#define MCP_LOG_REQUEST(url, method, body, keyHint) \
    McpLogger::instance().logRequest(url, method, body, keyHint)

#define MCP_LOG_RESPONSE(url, code, ct, body) \
    McpLogger::instance().logResponse(url, code, ct, body)

#define MCP_LOG_EVENT(event, ...) \
    McpLogger::instance().logEvent(event, ##__VA_ARGS__)

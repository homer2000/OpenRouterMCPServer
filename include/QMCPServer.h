#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QThread>
#include <QMutex>
#include <QMap>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// Лог трафика MCP (входящие и исходящие сообщения JSON-RPC)
//
// true  — весь трафик пишется в <папка_бинаря>/mcp_traffic.log
// false — логирование полностью отключено (нулевой оверхед в продакшене)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr bool kEnableMCPTrafficLog = true;

// ─────────────────────────────────────────────────────────────────────────────
// Data structures
// ─────────────────────────────────────────────────────────────────────────────

struct MCPToolParam {
    QString name;
    QString type;
    QString description;
    bool    required = false;
};

struct MCPToolDefinition {
    QString             name;
    QString             description;
    QList<MCPToolParam> params;
    QString             outputType;
};

struct MCPToolResult {
    bool        isError = false;
    QString     text;
    QJsonObject structuredContent;

    static MCPToolResult ok(const QString &text) {
        MCPToolResult r; r.text = text; return r;
    }
    static MCPToolResult okJson(const QJsonObject &obj) {
        MCPToolResult r;
        r.structuredContent = obj;
        r.text = QString::fromUtf8(
            QJsonDocument(obj).toJson(QJsonDocument::Compact));
        return r;
    }
    static MCPToolResult error(const QString &msg) {
        MCPToolResult r; r.isError = true; r.text = msg; return r;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// StdinReaderThread
// ─────────────────────────────────────────────────────────────────────────────

class StdinReaderThread : public QThread
{
    Q_OBJECT
public:
    explicit StdinReaderThread(QObject *parent = nullptr) : QThread(parent) {}

signals:
    void lineReceived(const QByteArray &line);
    void eofReached();

protected:
    void run() override {
        char buf[65536];
        while (!isInterruptionRequested()) {
            if (!fgets(buf, sizeof(buf), stdin)) {
                emit eofReached();
                return;
            }
            QByteArray line = QByteArray(buf).trimmed();
            if (!line.isEmpty())
                emit lineReceived(line);
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// QMCPServer
// ─────────────────────────────────────────────────────────────────────────────

class QMCPServer : public QObject
{
    Q_OBJECT

public:
    explicit QMCPServer(const QString &serverName,
                        const QString &serverVersion,
                        QObject *parent = nullptr);
    ~QMCPServer() override;

    void start();
    void registerTool(const MCPToolDefinition &def);
    void setInstructions(const QString &instructions) { m_instructions = instructions; }
    void setDebug(bool on) { m_debug = on; }

    QString serverVersion() const { return m_serverVersion; }

    /**
     * Отправить результат инструмента из любого потока.
     * Используется подклассами для асинхронных инструментов:
     *   1. handleToolCallAsync() возвращает true и сохраняет id.
     *   2. Когда работа закончена — вызывается sendToolResult(id, result).
     * QMCPServer НЕ отправляет ответ сам если handleToolCallAsync вернул true.
     */
    void sendToolResult(const QJsonValue &id, const MCPToolResult &result);

signals:
    void clientInitialized();
    void toolCallReceived(const QString &toolName, const QJsonObject &args);
    void fatalError(const QString &message);

protected:
    /**
     * Синхронный обработчик инструмента.
     * Вызывается если handleToolCallAsync() вернул false (по умолчанию).
     */
    virtual MCPToolResult handleToolCall(const QString &name,
                                        const QJsonObject &args) = 0;

    /**
     * Асинхронный обработчик инструмента.
     * Переопределите для инструментов которые блокируют поток (sudo, процессы и т.п.).
     *
     * Если возвращает TRUE:
     *   - QMCPServer НЕ вызывает handleToolCall и НЕ отправляет ответ.
     *   - Подкласс обязан вызвать sendToolResult(id, result) когда готов.
     * Если возвращает FALSE (по умолчанию):
     *   - QMCPServer вызывает handleToolCall() синхронно и отправляет ответ.
     */
    virtual bool handleToolCallAsync(const QJsonValue  &/*id*/,
                                     const QString     &/*name*/,
                                     const QJsonObject &/*args*/) { return false; }

    virtual void onInitialized() {}

    void sendLog(const QString &message,
                 const QString &level = QStringLiteral("info"));
    void notifyToolListChanged();
    void dbg(const QString &msg) const;

private slots:
    void onLineReceived(const QByteArray &line);
    void onEof();

private:
    void dispatch(const QJsonObject &msg);
    void handleInitialize(const QJsonObject &msg);
    void handleToolsList(const QJsonObject &msg);
    void handleToolsCall(const QJsonObject &msg);
    void handlePing(const QJsonObject &msg);

    void sendResult(const QJsonValue &id, const QJsonValue &result);
    void sendError(const QJsonValue &id, int code, const QString &message);
    void sendNotification(const QString &method, const QJsonObject &params = {});
    void writeMessage(const QJsonObject &obj);

    QJsonObject buildInputSchema(const QList<MCPToolParam> &params) const;
    QJsonObject buildCapabilities() const;

    enum class State { Idle, Initializing, Ready };
    State   m_state   = State::Idle;
    bool    m_debug   = false;

    QString m_serverName;
    QString m_serverVersion;
    QString m_instructions;

    QMap<QString, MCPToolDefinition> m_tools;
    StdinReaderThread *m_reader = nullptr;
    QMutex m_writeMutex;

    // Traffic log
    QFile  m_logFile;
    QMutex m_logMutex;
    void   trafficLog(const char *direction, const QByteArray &data);

    static constexpr const char *kProtocolVersion = "2025-11-25";

public:
    // Debug log
    QFile  m_debugLogFile;
    QMutex m_debugLogMutex;
    void   debugLog(QString data);
};

#pragma once

#include "QMCPServer.h"
#include "McpLogger.h"
#include "ServerConfig.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QMap>

#include <functional>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// AccountEntry
// ─────────────────────────────────────────────────────────────────────────────

struct AccountEntry {
    QString id;
    QString name;
    QString model;
    QString apiKey;
    QString baseUrl;
    QString description;
};

// ─────────────────────────────────────────────────────────────────────────────
// HttpResult
// ─────────────────────────────────────────────────────────────────────────────

struct HttpResult {
    bool       ok   = false;
    int        code = 0;
    QByteArray body;
    QString    error;
    QString    contentType;
};

// ─────────────────────────────────────────────────────────────────────────────
// ResponseFormat
// ─────────────────────────────────────────────────────────────────────────────

enum class ResponseFormat {
    JsonChatCompletion,
    JsonError,
    JsonModels,
    JsonUnknown,
    Sse,
    Html,
    PlainText,
    Empty,
    Binary,
};

// ─────────────────────────────────────────────────────────────────────────────
// ParsedResponse
// ─────────────────────────────────────────────────────────────────────────────

struct ParsedResponse {
    ResponseFormat format = ResponseFormat::Empty;

    QString     content;
    QString     finishReason;
    QString     responseId;
    QString     model;
    QJsonObject usage;

    QString     apiErrorMessage;
    int         apiErrorCode = 0;
    QString     apiErrorType;

    QJsonArray  models;

    QString     rawText;
    QJsonObject rawJson;

    bool    ok = true;
    QString parseError;

    QString formatName() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// OpenRouterMCPServer
// ─────────────────────────────────────────────────────────────────────────────

class OpenRouterMCPServer : public QMCPServer
{
    Q_OBJECT

public:
    explicit OpenRouterMCPServer(const QString &accountsFilePath,
                                 const QString &configFilePath = {},
                                 QObject *parent = nullptr);
    ~OpenRouterMCPServer() override = default;

    // Настройка логгера до старта сервера
    // logFilePath — путь к файлу; пустой → только stderr
    // level       — минимальный уровень (Debug по умолчанию)
    void setupLogger(const QString &logFilePath,
                     McpLogger::Level level = McpLogger::Level::Debug);

protected:
    MCPToolResult handleToolCall(const QString     &name,
                                 const QJsonObject &args) override;

    bool handleToolCallAsync(const QJsonValue  &id,
                             const QString     &name,
                             const QJsonObject &args) override;

    void onInitialized() override;

private:
    // ── Конфигурация ──────────────────────────────────────────────────────────
    bool loadConfig();
    bool saveConfig() const;
    void ensureConfigFile();

    // ── Синхронные инструменты ────────────────────────────────────────────────
    MCPToolResult toolListAccounts();
    MCPToolResult toolAddAccount   (const QJsonObject &args);
    MCPToolResult toolRemoveAccount(const QJsonObject &args);

    // ── Асинхронные инструменты ───────────────────────────────────────────────
    bool toolGetPaidModels           (const QJsonValue &id, const QJsonObject &args);
    bool toolGetFreeModels           (const QJsonValue &id, const QJsonObject &args);
    bool toolSendChatRequest         (const QJsonValue &id, const QJsonObject &args);
    bool toolSendChatRequestWithFiles(const QJsonValue &id, const QJsonObject &args);

    // ── HTTP ──────────────────────────────────────────────────────────────────
    HttpResult httpGet (const QString &url, const QString &apiKey);
    HttpResult httpPost(const QString &url, const QString &apiKey,
                        const QByteArray &body);

    // ── Анализ ответа ─────────────────────────────────────────────────────────
    static ParsedResponse parseApiResponse(const HttpResult &http);
    static ParsedResponse assembleSseResponse(const QByteArray &body);
    static MCPToolResult  chatCompletionToResult(const ParsedResponse &pr);
    static MCPToolResult  responseErrorResult(const ParsedResponse &pr,
                                              const HttpResult     &http,
                                              const QString        &context = {});

    // ── Файлы ────────────────────────────────────────────────────────────────
    static QJsonArray buildFileContentBlocks(const QJsonArray &files,
                                             QString          &errorOut);
    static QString    mimeTypeFromPath(const QString &filePath);

    // ── Вспомогательное ──────────────────────────────────────────────────────
    QNetworkRequest makeRequest(const QString &url, const QString &apiKey) const;
    void            runAsync(std::function<void()> fn);

    // ── Аккаунты ─────────────────────────────────────────────────────────────
    bool          loadAccounts();
    bool          saveAccounts() const;
    void          ensureAccountsFile();
    AccountEntry *findAccount(const QString &id);

    static QJsonObject safeAccount(const AccountEntry &a);
    static bool        isFreeModel (const QJsonObject &m);
    static QJsonObject modelSummary(const QJsonObject &m);
    void registerAllTools();

    // Краткая подсказка по ключу для лога (первые 8 символов)
    static QString apiKeyHint(const QString &apiKey);

    // ── Поля ─────────────────────────────────────────────────────────────────
    QString                     m_accountsFilePath;
    QString                     m_configFilePath;
    ServerConfig                m_config;
    QMap<QString, AccountEntry> m_accounts;
};

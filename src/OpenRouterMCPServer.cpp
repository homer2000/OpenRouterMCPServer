#include "OpenRouterMCPServer.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QNetworkReply>
#include <QTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QFile>
#include <QMimeDatabase>
#include <QMimeType>
#include <QThread>

#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// Константы
// ─────────────────────────────────────────────────────────────────────────────

static constexpr const char *kServerName    = "openrouter-mcp";
static constexpr const char *kServerVersion = "1.0.0";
static constexpr int         kTimeoutMs     = 120'000;

static const QString kDefaultBaseUrl =
    QStringLiteral("https://openrouter.ai/api/v1");

static const QString kDefaultConfigName =
    QStringLiteral("server-config.json");

static constexpr qint64 kMaxFileSize = 20LL * 1024 * 1024;

// ─────────────────────────────────────────────────────────────────────────────
// ParsedResponse::formatName
// ─────────────────────────────────────────────────────────────────────────────

QString ParsedResponse::formatName() const
{
    switch (format) {
    case ResponseFormat::JsonChatCompletion: return "JSON chat-completion";
    case ResponseFormat::JsonError:          return "JSON API error";
    case ResponseFormat::JsonModels:         return "JSON models list";
    case ResponseFormat::JsonUnknown:        return "JSON (unknown schema)";
    case ResponseFormat::Sse:                return "SSE stream";
    case ResponseFormat::Html:               return "HTML";
    case ResponseFormat::PlainText:          return "plain text";
    case ResponseFormat::Empty:              return "empty body";
    case ResponseFormat::Binary:             return "binary data";
    }
    return "unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// Ctor
// ─────────────────────────────────────────────────────────────────────────────

OpenRouterMCPServer::OpenRouterMCPServer(const QString &accountsFilePath,
                                         const QString &configFilePath,
                                         QObject *parent)
    : QMCPServer(kServerName, kServerVersion, parent)
    , m_accountsFilePath(accountsFilePath)
{
    if (configFilePath.isEmpty()) {
        const QString dir = QCoreApplication::applicationDirPath();
        m_configFilePath  = dir + QDir::separator() + kDefaultConfigName;
    } else {
        m_configFilePath = configFilePath;
    }

    setInstructions(
        "OpenRouter MCP server. Manages AI accounts and sends requests.\n"
        "Each account has a fixed model configured — no model selection at call time.\n"
        "API keys and file paths are NEVER exposed."
    );

    ensureConfigFile();
    loadConfig();
    ensureAccountsFile();
    loadAccounts();
    registerAllTools();
}

// ─────────────────────────────────────────────────────────────────────────────
// setupLogger
// ─────────────────────────────────────────────────────────────────────────────

void OpenRouterMCPServer::setupLogger(const QString &logFilePath,
                                       McpLogger::Level level)
{
    McpLogger &log = McpLogger::instance();
    log.setLevel(level);
    log.setEchoToStderr(true);

    if (!logFilePath.isEmpty()) {
        if (!log.open(logFilePath)) {
            MCP_LOG_WARN(QString("Failed to open log file: %1 — logging to stderr only")
                             .arg(logFilePath));
        } else {
            MCP_LOG_INFO(QString("Log file: %1").arg(logFilePath));
        }
    }
}

void OpenRouterMCPServer::onInitialized()
{
    const QString proxyInfo = m_config.proxy.enabled
        ? QString("proxy=%1:%2").arg(m_config.proxy.host).arg(m_config.proxy.port)
        : QStringLiteral("no proxy");

    const QString msg = QString("OpenRouter MCP ready. Accounts: %1. Config: %2 (%3)")
                            .arg(m_accounts.size())
                            .arg(QFileInfo(m_configFilePath).fileName())
                            .arg(proxyInfo);

    MCP_LOG_INFO(msg);
    sendLog(msg);

    MCP_LOG_EVENT("server_initialized", QJsonObject {
        { "accounts",   int(m_accounts.size()) },
        { "configFile", QFileInfo(m_configFilePath).fileName() },
        { "proxy",      m_config.proxy.enabled },
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// apiKeyHint
// ─────────────────────────────────────────────────────────────────────────────

QString OpenRouterMCPServer::apiKeyHint(const QString &apiKey)
{
    if (apiKey.length() <= 8) return "***";
    return apiKey.left(4) + "..." + apiKey.right(4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Конфигурация
// ─────────────────────────────────────────────────────────────────────────────

void OpenRouterMCPServer::ensureConfigFile()
{
    if (QFileInfo::exists(m_configFilePath)) return;
    QDir().mkpath(QFileInfo(m_configFilePath).absolutePath());
    ServerConfig defaults;
    QFile f(m_configFilePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(QJsonDocument(defaults.toJson()).toJson(QJsonDocument::Indented));
}

bool OpenRouterMCPServer::loadConfig()
{
    QFile f(m_configFilePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        MCP_LOG_WARN(QString("Cannot open config: %1 — using defaults").arg(m_configFilePath));
        sendLog(QString("Cannot open config file: %1 — using defaults.").arg(m_configFilePath), "warning");
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError) {
        MCP_LOG_ERROR(QString("Config parse error: %1").arg(err.errorString()));
        sendLog(QString("Config parse error: %1 — using defaults.").arg(err.errorString()), "error");
        return false;
    }
    m_config = ServerConfig::fromJson(doc.object());
    MCP_LOG_INFO(QString("Config loaded: %1").arg(m_configFilePath));
    return true;
}

bool OpenRouterMCPServer::saveConfig() const
{
    const QString tmp = m_configFilePath + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(QJsonDocument(m_config.toJson()).toJson(QJsonDocument::Indented));
    f.close();
    QFile::remove(m_configFilePath);
    return QFile::rename(tmp, m_configFilePath);
}

// ─────────────────────────────────────────────────────────────────────────────
// runAsync
// ─────────────────────────────────────────────────────────────────────────────

void OpenRouterMCPServer::runAsync(std::function<void()> fn)
{
    std::thread([fn = std::move(fn)]() { fn(); }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
// makeRequest
// ─────────────────────────────────────────────────────────────────────────────

QNetworkRequest OpenRouterMCPServer::makeRequest(const QString &url,
                                                  const QString &apiKey) const
{
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    req.setRawHeader("HTTP-Referer", "https://github.com/openrouter-mcp");
    req.setRawHeader("X-Title",      "OpenRouter MCP Server");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}

// ─────────────────────────────────────────────────────────────────────────────
// httpGet
// ─────────────────────────────────────────────────────────────────────────────

HttpResult OpenRouterMCPServer::httpGet(const QString &url, const QString &apiKey)
{
    MCP_LOG_REQUEST(url, "GET", QByteArray{}, apiKeyHint(apiKey));

    QNetworkAccessManager nam;
    m_config.applyProxy(nam);
    QNetworkReply *reply = nam.get(makeRequest(url, apiKey));

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit,
                     Qt::DirectConnection);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit,
                     Qt::DirectConnection);
    timeout.start(kTimeoutMs);
    loop.exec();

    HttpResult result;
    result.code        = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();

    if (reply->error() != QNetworkReply::NoError && reply->bytesAvailable() == 0) {
        result.ok    = false;
        result.error = reply->errorString();
        MCP_LOG_ERROR(QString("GET %1 → network error: %2").arg(url).arg(result.error));
    } else {
        result.ok   = true;
        result.body = reply->readAll();
    }

    MCP_LOG_RESPONSE(url, result.code, result.contentType, result.body);
    reply->deleteLater();
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// httpPost
// ─────────────────────────────────────────────────────────────────────────────

HttpResult OpenRouterMCPServer::httpPost(const QString    &url,
                                          const QString    &apiKey,
                                          const QByteArray &body)
{
    MCP_LOG_REQUEST(url, "POST", body, apiKeyHint(apiKey));

    QNetworkAccessManager nam;
    m_config.applyProxy(nam);
    QNetworkReply *reply = nam.post(makeRequest(url, apiKey), body);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit,
                     Qt::DirectConnection);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&loop, reply]() {
        reply->abort();
        loop.quit();
    }, Qt::DirectConnection);
    timeout.start(kTimeoutMs);
    loop.exec();
    timeout.stop();

    HttpResult result;
    result.code        = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    result.body        = reply->readAll();

    if (reply->error() != QNetworkReply::NoError &&
        reply->error() != QNetworkReply::OperationCanceledError)
    {
        result.ok    = false;
        result.error = reply->errorString();
        MCP_LOG_ERROR(QString("POST %1 → network error (HTTP %2): %3")
                          .arg(url).arg(result.code).arg(result.error));
    } else {
        result.ok = true;
    }

    MCP_LOG_RESPONSE(url, result.code, result.contentType, result.body);
    reply->deleteLater();
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseApiResponse
// ─────────────────────────────────────────────────────────────────────────────

ParsedResponse OpenRouterMCPServer::parseApiResponse(const HttpResult &http)
{
    ParsedResponse pr;

    QByteArray body = http.body;
    if (body.startsWith("\xEF\xBB\xBF"))
        body = body.mid(3);

    const QByteArray trimmed = body.trimmed();

    if (trimmed.isEmpty()) {
        pr.format = ResponseFormat::Empty;
        if (!http.ok && !http.error.isEmpty()) {
            pr.ok         = false;
            pr.parseError = http.error;
        } else if (http.code >= 400) {
            pr.ok         = false;
            pr.parseError = QString("HTTP %1: empty response body").arg(http.code);
        }
        MCP_LOG_DEBUG(QString("parseApiResponse: Empty body, HTTP %1").arg(http.code));
        return pr;
    }

    const bool looksLikeSse = http.contentType.contains("text/event-stream") ||
                              trimmed.startsWith("data:");
    if (looksLikeSse) {
        MCP_LOG_DEBUG("parseApiResponse: detected SSE stream");
        pr = assembleSseResponse(body);
        return pr;
    }

    QJsonParseError jsonErr;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &jsonErr);

    if (jsonErr.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject root = doc.object();

        if (root.contains("choices")) {
            pr.format     = ResponseFormat::JsonChatCompletion;
            pr.responseId = root.value("id").toString();
            pr.model      = root.value("model").toString();
            pr.usage      = root.value("usage").toObject();

            const QJsonArray choices = root.value("choices").toArray();
            if (!choices.isEmpty()) {
                const QJsonObject choice = choices.first().toObject();
                const QJsonObject msgObj = choice.contains("message")
                    ? choice.value("message").toObject()
                    : choice.value("delta").toObject();
                pr.content      = msgObj.value("content").toString();
                pr.finishReason = choice.value("finish_reason").toString();
            }
            pr.ok = true;
            MCP_LOG_DEBUG(QString("parseApiResponse: JsonChatCompletion, model=%1, "
                                  "finish_reason=%2, content_len=%3")
                              .arg(pr.model)
                              .arg(pr.finishReason)
                              .arg(pr.content.length()));

        } else if (root.contains("error")) {
            pr.format = ResponseFormat::JsonError;
            pr.ok     = false;

            const QJsonValue errVal = root.value("error");
            if (errVal.isObject()) {
                const QJsonObject errObj = errVal.toObject();
                pr.apiErrorMessage = errObj.value("message").toString();
                pr.apiErrorType    = errObj.value("type").toString();
                const QJsonValue code = errObj.value("code");
                pr.apiErrorCode = code.isDouble()
                    ? code.toInt()
                    : code.toString().toInt();
            } else if (errVal.isString()) {
                pr.apiErrorMessage = errVal.toString();
            } else {
                pr.apiErrorMessage = QString::fromUtf8(
                    QJsonDocument(root).toJson(QJsonDocument::Compact));
            }
            if (pr.apiErrorMessage.isEmpty())
                pr.apiErrorMessage = QString::fromUtf8(
                    QJsonDocument(root).toJson(QJsonDocument::Compact));

            MCP_LOG_WARN(QString("parseApiResponse: JsonError — %1 (code=%2, type=%3)")
                             .arg(pr.apiErrorMessage)
                             .arg(pr.apiErrorCode)
                             .arg(pr.apiErrorType));

        } else if (root.contains("data") && root.value("data").isArray()) {
            pr.format = ResponseFormat::JsonModels;
            pr.models = root.value("data").toArray();
            pr.ok     = true;
            MCP_LOG_DEBUG(QString("parseApiResponse: JsonModels, count=%1").arg(pr.models.size()));

        } else {
            pr.format  = ResponseFormat::JsonUnknown;
            pr.rawJson  = root;
            pr.rawText  = QString::fromUtf8(trimmed.left(500));
            pr.ok       = http.code < 400;
            if (!pr.ok)
                pr.parseError = QString("HTTP %1: unrecognized JSON schema").arg(http.code);
            MCP_LOG_WARN(QString("parseApiResponse: JsonUnknown schema, HTTP %1, preview: %2")
                             .arg(http.code)
                             .arg(pr.rawText.left(200)));
        }

    } else {
        // Не-JSON
        const QByteArray preview = trimmed.left(5).toLower();

        if (preview.startsWith("<")) {
            pr.format  = ResponseFormat::Html;
            pr.ok      = false;
            pr.rawText = QString::fromUtf8(trimmed.left(1000));

            auto extractTag = [&](const QString &tag) -> QString {
                const QString html  = QString::fromUtf8(trimmed);
                const QString open  = "<" + tag;
                const QString close = "</" + tag + ">";
                int s = html.toLower().indexOf(open);
                if (s < 0) return {};
                s = html.indexOf('>', s);
                if (s < 0) return {};
                const int e = html.toLower().indexOf(close, s);
                if (e < 0) return {};
                return html.mid(s + 1, e - s - 1).trimmed();
            };

            const QString title = extractTag("title");
            const QString h1    = extractTag("h1");
            pr.parseError = QString("HTTP %1: server returned HTML").arg(http.code);
            if (!title.isEmpty()) pr.parseError += " — " + title;
            else if (!h1.isEmpty()) pr.parseError += " — " + h1;

            MCP_LOG_WARN(QString("parseApiResponse: HTML response — %1").arg(pr.parseError));

        } else {
            const QString text = QString::fromUtf8(trimmed);
            if (!text.isEmpty() && !text.contains(QChar::ReplacementCharacter)) {
                pr.format  = ResponseFormat::PlainText;
                pr.rawText = text;
                pr.ok      = http.code < 400;
                if (!pr.ok)
                    pr.parseError = QString("HTTP %1: %2").arg(http.code).arg(text.left(300));
                MCP_LOG_WARN(QString("parseApiResponse: PlainText, HTTP %1: %2")
                                 .arg(http.code).arg(text.left(200)));
            } else {
                pr.format     = ResponseFormat::Binary;
                pr.ok         = false;
                pr.rawText    = QString("HTTP %1: binary response (%2 bytes)")
                                    .arg(http.code).arg(trimmed.size());
                pr.parseError = pr.rawText;
                MCP_LOG_WARN(QString("parseApiResponse: Binary body, %1 bytes").arg(trimmed.size()));
            }
        }

        // Дополнительно логируем причину провала JSON-парсинга
        MCP_LOG_DEBUG(QString("parseApiResponse: JSON parse failed — %1 at offset %2")
                          .arg(jsonErr.errorString())
                          .arg(jsonErr.offset));
    }

    if (!http.ok && pr.ok) {
        if (pr.format != ResponseFormat::JsonError) {
            pr.ok = false;
            if (pr.parseError.isEmpty())
                pr.parseError = http.error;
        }
    }

    return pr;
}

// ─────────────────────────────────────────────────────────────────────────────
// assembleSseResponse
// ─────────────────────────────────────────────────────────────────────────────

ParsedResponse OpenRouterMCPServer::assembleSseResponse(const QByteArray &sseBody)
{
    ParsedResponse pr;
    pr.format = ResponseFormat::Sse;

    int chunkCount = 0;
    const QList<QByteArray> lines = sseBody.split('\n');

    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (!line.startsWith("data:")) continue;

        const QByteArray payload = line.mid(5).trimmed();
        if (payload == "[DONE]") {
            MCP_LOG_DEBUG("assembleSseResponse: received [DONE]");
            break;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            MCP_LOG_DEBUG(QString("assembleSseResponse: skip malformed chunk — %1")
                              .arg(err.errorString()));
            continue;
        }

        ++chunkCount;
        const QJsonObject chunk = doc.object();

        if (pr.responseId.isEmpty()) pr.responseId = chunk.value("id").toString();
        if (pr.model.isEmpty())      pr.model      = chunk.value("model").toString();
        if (chunk.contains("usage") && !chunk.value("usage").toObject().isEmpty())
            pr.usage = chunk.value("usage").toObject();

        if (chunk.contains("error")) {
            const QJsonValue errVal = chunk.value("error");
            pr.ok              = false;
            pr.format          = ResponseFormat::JsonError;
            pr.apiErrorMessage = errVal.isObject()
                ? errVal.toObject().value("message").toString()
                : errVal.toString();
            MCP_LOG_WARN(QString("assembleSseResponse: error chunk — %1")
                             .arg(pr.apiErrorMessage));
            return pr;
        }

        const QJsonArray choices = chunk.value("choices").toArray();
        if (!choices.isEmpty()) {
            const QJsonObject choice = choices.first().toObject();
            pr.content      += choice.value("delta").toObject().value("content").toString();
            const QJsonValue fr = choice.value("finish_reason");
            if (!fr.isNull() && fr.isString())
                pr.finishReason = fr.toString();
        }
    }

    MCP_LOG_DEBUG(QString("assembleSseResponse: done. chunks=%1, content_len=%2, "
                          "finish_reason=%3")
                      .arg(chunkCount)
                      .arg(pr.content.length())
                      .arg(pr.finishReason));

    if (pr.content.isEmpty() && pr.responseId.isEmpty()) {
        pr.ok         = false;
        pr.parseError = "SSE stream contained no valid data chunks";
        MCP_LOG_WARN("assembleSseResponse: " + pr.parseError);
    } else {
        pr.ok = true;
    }

    return pr;
}

// ─────────────────────────────────────────────────────────────────────────────
// chatCompletionToResult
// ─────────────────────────────────────────────────────────────────────────────

MCPToolResult OpenRouterMCPServer::chatCompletionToResult(const ParsedResponse &pr)
{
    return MCPToolResult::okJson(QJsonObject {
        { "model",         pr.model                                                },
        { "content",       pr.content                                              },
        { "finish_reason", pr.finishReason                                         },
        { "usage",         pr.usage.isEmpty() ? QJsonValue(QJsonValue::Null)
                                              : QJsonValue(pr.usage)               },
        { "id",            pr.responseId                                           },
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// responseErrorResult
// ─────────────────────────────────────────────────────────────────────────────

MCPToolResult OpenRouterMCPServer::responseErrorResult(const ParsedResponse &pr,
                                                        const HttpResult     &http,
                                                        const QString        &context)
{
    QString msg;
    switch (pr.format) {
    case ResponseFormat::JsonError:
        msg = pr.apiErrorMessage;
        if (pr.apiErrorCode != 0) msg += QString(" (code: %1)").arg(pr.apiErrorCode);
        if (!pr.apiErrorType.isEmpty()) msg += QString(" [%1]").arg(pr.apiErrorType);
        break;
    case ResponseFormat::Html:
    case ResponseFormat::PlainText:
        msg = pr.parseError.isEmpty() ? pr.rawText.left(300) : pr.parseError;
        break;
    case ResponseFormat::Empty:
        msg = pr.parseError.isEmpty()
            ? (http.error.isEmpty()
               ? QString("HTTP %1: empty response").arg(http.code)
               : http.error)
            : pr.parseError;
        break;
    case ResponseFormat::Sse:
        msg = pr.parseError.isEmpty() ? "SSE stream error" : pr.parseError;
        break;
    case ResponseFormat::JsonUnknown:
        msg = pr.parseError.isEmpty()
            ? QString("Unexpected response schema (HTTP %1): %2")
                  .arg(http.code)
                  .arg(QString::fromUtf8(
                      QJsonDocument(pr.rawJson).toJson(QJsonDocument::Compact).left(300)))
            : pr.parseError;
        break;
    case ResponseFormat::Binary:
        msg = pr.rawText;
        break;
    default:
        msg = pr.parseError.isEmpty()
            ? QString("HTTP %1: %2").arg(http.code).arg(pr.formatName())
            : pr.parseError;
        break;
    }

    if (!context.isEmpty()) msg = context + ": " + msg;

    MCP_LOG_ERROR(QString("Tool error [%1]: %2").arg(context).arg(msg));
    return MCPToolResult::error(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch
// ─────────────────────────────────────────────────────────────────────────────

MCPToolResult OpenRouterMCPServer::handleToolCall(const QString     &name,
                                                   const QJsonObject &args)
{
    MCP_LOG_DEBUG(QString("handleToolCall: %1").arg(name));
    if (name == "list_accounts")  return toolListAccounts();
    if (name == "add_account")    return toolAddAccount(args);
    if (name == "remove_account") return toolRemoveAccount(args);
    return MCPToolResult::error("Unknown tool: " + name);
}

bool OpenRouterMCPServer::handleToolCallAsync(const QJsonValue  &id,
                                               const QString     &name,
                                               const QJsonObject &args)
{
    if (name == "get_paid_models")              { MCP_LOG_DEBUG("handleToolCallAsync: " + name); return toolGetPaidModels(id, args); }
    if (name == "get_free_models")              { MCP_LOG_DEBUG("handleToolCallAsync: " + name); return toolGetFreeModels(id, args); }
    if (name == "send_chat_request")            { MCP_LOG_DEBUG("handleToolCallAsync: " + name); return toolSendChatRequest(id, args); }
    if (name == "send_chat_request_with_files") { MCP_LOG_DEBUG("handleToolCallAsync: " + name); return toolSendChatRequestWithFiles(id, args); }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Синхронные инструменты
// ─────────────────────────────────────────────────────────────────────────────

MCPToolResult OpenRouterMCPServer::toolListAccounts()
{
    MCP_LOG_EVENT("list_accounts", QJsonObject { { "count", int(m_accounts.size()) } });
    QJsonArray arr;
    for (const AccountEntry &a : qAsConst(m_accounts))
        arr.append(safeAccount(a));
    return MCPToolResult::okJson(QJsonObject { { "accounts", arr } });
}

MCPToolResult OpenRouterMCPServer::toolAddAccount(const QJsonObject &args)
{
    const QString id      = args.value("id").toString().trimmed();
    const QString name    = args.value("name").toString().trimmed();
    const QString model   = args.value("model").toString().trimmed();
    const QString apiKey  = args.value("api_key").toString().trimmed();
    QString       baseUrl = args.value("base_url").toString().trimmed();
    const QString desc    = args.value("description").toString().trimmed();

    if (id.isEmpty())     return MCPToolResult::error("'id' is required");
    if (name.isEmpty())   return MCPToolResult::error("'name' is required");
    if (model.isEmpty())  return MCPToolResult::error("'model' is required");
    if (apiKey.isEmpty()) return MCPToolResult::error("'api_key' is required");
    if (baseUrl.isEmpty()) baseUrl = kDefaultBaseUrl;

    if (m_accounts.contains(id)) {
        MCP_LOG_WARN(QString("add_account: '%1' already exists").arg(id));
        return MCPToolResult::error(
            QString("Account '%1' already exists. Remove it first.").arg(id));
    }

    AccountEntry e { id, name, model, apiKey, baseUrl, desc };
    m_accounts.insert(id, e);

    MCP_LOG_EVENT("account_added", QJsonObject {
        { "id", id }, { "model", model }, { "baseUrl", baseUrl }
    });

    if (!saveAccounts())
        return MCPToolResult::error("Added in memory but failed to save to file.");

    return MCPToolResult::okJson(QJsonObject {
        { "status",  "added"        },
        { "account", safeAccount(e) }
    });
}

MCPToolResult OpenRouterMCPServer::toolRemoveAccount(const QJsonObject &args)
{
    const QString id = args.value("id").toString().trimmed();
    if (id.isEmpty())             return MCPToolResult::error("'id' is required");
    if (!m_accounts.contains(id)) return MCPToolResult::error(
        QString("Account '%1' not found.").arg(id));

    m_accounts.remove(id);
    MCP_LOG_EVENT("account_removed", QJsonObject { { "id", id } });

    if (!saveAccounts())
        return MCPToolResult::error("Removed from memory but failed to save to file.");

    return MCPToolResult::okJson(QJsonObject { { "status", "removed" }, { "id", id } });
}

// ─────────────────────────────────────────────────────────────────────────────
// Асинхронные инструменты
// ─────────────────────────────────────────────────────────────────────────────

bool OpenRouterMCPServer::toolGetPaidModels(const QJsonValue  &id,
                                             const QJsonObject &args)
{
    const QString accountId = args.value("account_id").toString().trimmed();
    AccountEntry *account   = findAccount(accountId);
    if (!account) {
        sendToolResult(id, MCPToolResult::error(
            QString("Account '%1' not found.").arg(accountId)));
        return true;
    }

    const QString url    = (account->baseUrl.endsWith('/') ? account->baseUrl.left(account->baseUrl.length() - 1) : account->baseUrl) + "/models";
    const QString apiKey = account->apiKey;

    MCP_LOG_EVENT("get_paid_models", QJsonObject { { "account", accountId } });

    runAsync([this, id, url, apiKey, accountId]() {
        HttpResult     http = httpGet(url, apiKey);
        ParsedResponse pr   = parseApiResponse(http);

        if (!pr.ok || pr.format == ResponseFormat::JsonError) {
            sendToolResult(id, responseErrorResult(pr, http, "get_paid_models"));
            return;
        }
        if (pr.format != ResponseFormat::JsonModels) {
            sendToolResult(id, responseErrorResult(pr, http,
                QString("get_paid_models: unexpected format '%1'").arg(pr.formatName())));
            return;
        }

        QJsonArray filtered;
        for (const QJsonValue &v : pr.models) {
            const QJsonObject m = v.toObject();
            if (isFreeModel(m)) continue;
            filtered.append(modelSummary(m));
        }
        MCP_LOG_INFO(QString("get_paid_models: found %1 models for account '%2'")
                         .arg(filtered.size()).arg(accountId));
        sendToolResult(id, MCPToolResult::okJson(QJsonObject {
            { "total",  filtered.size() },
            { "models", filtered        }
        }));
    });
    return true;
}

bool OpenRouterMCPServer::toolGetFreeModels(const QJsonValue  &id,
                                             const QJsonObject &args)
{
    const QString accountId = args.value("account_id").toString().trimmed();
    AccountEntry *account   = findAccount(accountId);
    if (!account) {
        sendToolResult(id, MCPToolResult::error(
            QString("Account '%1' not found.").arg(accountId)));
        return true;
    }

    const QString url    = (account->baseUrl.endsWith('/') ? account->baseUrl.left(account->baseUrl.length() - 1) : account->baseUrl) + "/models";
    const QString apiKey = account->apiKey;

    MCP_LOG_EVENT("get_free_models", QJsonObject { { "account", accountId } });

    runAsync([this, id, url, apiKey, accountId]() {
        HttpResult     http = httpGet(url, apiKey);
        ParsedResponse pr   = parseApiResponse(http);

        if (!pr.ok || pr.format == ResponseFormat::JsonError) {
            sendToolResult(id, responseErrorResult(pr, http, "get_free_models"));
            return;
        }
        if (pr.format != ResponseFormat::JsonModels) {
            sendToolResult(id, responseErrorResult(pr, http,
                QString("get_free_models: unexpected format '%1'").arg(pr.formatName())));
            return;
        }

        QJsonArray filtered;
        for (const QJsonValue &v : pr.models) {
            const QJsonObject m = v.toObject();
            if (!isFreeModel(m)) continue;
            filtered.append(modelSummary(m));
        }
        MCP_LOG_INFO(QString("get_free_models: found %1 models for account '%2'")
                         .arg(filtered.size()).arg(accountId));
        sendToolResult(id, MCPToolResult::okJson(QJsonObject {
            { "total",  filtered.size() },
            { "models", filtered        }
        }));
    });
    return true;
}

bool OpenRouterMCPServer::toolSendChatRequest(const QJsonValue  &id,
                                               const QJsonObject &args)
{
    const QString    accountId   = args.value("account_id").toString().trimmed();
    const QJsonArray messages    = args.value("messages").toArray();
    const int        maxTokens   = args.value("max_tokens").toInt(4096);  // 4096 — разумный дефолт для reasoning-моделей
    const double     temperature = args.value("temperature").toDouble(0.7);

    if (messages.isEmpty()) {
        sendToolResult(id, MCPToolResult::error("'messages' must be a non-empty array"));
        return true;
    }
    AccountEntry *account = findAccount(accountId);
    if (!account) {
        sendToolResult(id, MCPToolResult::error(
            QString("Account '%1' not found.").arg(accountId)));
        return true;
    }

    const QString url    = (account->baseUrl.endsWith('/') ? account->baseUrl.left(account->baseUrl.length() - 1) : account->baseUrl) + "/chat/completions";
    const QString apiKey = account->apiKey;
    const QString model  = account->model;

    MCP_LOG_EVENT("send_chat_request", QJsonObject {
        { "account",     accountId          },
        { "model",       model              },
        { "messages",    int(messages.size()) },
        { "max_tokens",  maxTokens          },
        { "temperature", temperature        },
    });

    const QByteArray bodyBytes = QJsonDocument(QJsonObject {
        { "model",       model       },
        { "messages",    messages    },
        { "max_tokens",  maxTokens   },
        { "temperature", temperature },
        { "stream",      false       },
    }).toJson(QJsonDocument::Compact);

    runAsync([this, id, url, apiKey, bodyBytes, accountId, model, maxTokens]() {
        // ── Retry при пустом теле с HTTP 200 (поведение некоторых провайдеров) ──
        static constexpr int kMaxRetries   = 2;
        static constexpr int kRetryDelayMs = 3000;

        HttpResult     http;
        ParsedResponse pr;

        for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
            if (attempt > 0) {
                MCP_LOG_WARN(QString("send_chat_request: empty body on HTTP 200, "
                                     "retry %1/%2 for account '%3'")
                                 .arg(attempt).arg(kMaxRetries).arg(accountId));
                QThread::msleep(kRetryDelayMs);
            }

            http = httpPost(url, apiKey, bodyBytes);
            pr   = parseApiResponse(http);

            // Повторяем только если HTTP 200 + пустое тело
            if (pr.format == ResponseFormat::Empty && http.code == 200 && http.ok)
                continue;

            break;
        }

        if (!pr.ok) {
            sendToolResult(id, responseErrorResult(pr, http, "send_chat_request"));
            return;
        }
        if (pr.format != ResponseFormat::JsonChatCompletion &&
            pr.format != ResponseFormat::Sse)
        {
            sendToolResult(id, responseErrorResult(pr, http,
                QString("send_chat_request: unexpected format '%1'").arg(pr.formatName())));
            return;
        }

        // Предупреждение если ответ обрезан по лимиту токенов
        if (pr.finishReason == "length") {
            MCP_LOG_WARN(QString("send_chat_request: finish_reason=length — "
                                 "response was truncated at max_tokens=%1. "
                                 "Model: %2. Consider increasing max_tokens.")
                             .arg(maxTokens).arg(pr.model));
        }

        MCP_LOG_INFO(QString("send_chat_request: OK, model=%1, finish_reason=%2, content_len=%3")
                         .arg(pr.model).arg(pr.finishReason).arg(pr.content.length()));
        sendToolResult(id, chatCompletionToResult(pr));
    });
    return true;
}

bool OpenRouterMCPServer::toolSendChatRequestWithFiles(const QJsonValue  &id,
                                                        const QJsonObject &args)
{
    const QString    accountId   = args.value("account_id").toString().trimmed();
    const QString    prompt      = args.value("prompt").toString().trimmed();
    const QJsonArray files       = args.value("files").toArray();
    const QString    system      = args.value("system").toString().trimmed();
    const int        maxTokens   = args.value("max_tokens").toInt(4096);  // 4096 — разумный дефолт для reasoning-моделей
    const double     temperature = args.value("temperature").toDouble(0.7);

    if (accountId.isEmpty()) {
        sendToolResult(id, MCPToolResult::error("'account_id' is required"));
        return true;
    }
    if (prompt.isEmpty()) {
        sendToolResult(id, MCPToolResult::error("'prompt' is required"));
        return true;
    }
    if (files.isEmpty()) {
        sendToolResult(id, MCPToolResult::error(
            "'files' must be a non-empty array of file descriptors"));
        return true;
    }

    AccountEntry *account = findAccount(accountId);
    if (!account) {
        sendToolResult(id, MCPToolResult::error(
            QString("Account '%1' not found.").arg(accountId)));
        return true;
    }

    // Собираем мета-информацию о файлах для лога (без данных)
    QJsonArray fileMeta;
    for (const QJsonValue &fv : files) {
        const QJsonObject fd = fv.toObject();
        fileMeta.append(QJsonObject {
            { "type", fd.value("type") },
            { "path", fd.value("path").toString().isEmpty()
                          ? "(base64 inline)"
                          : fd.value("path").toString() },
        });
    }

    MCP_LOG_EVENT("send_chat_request_with_files", QJsonObject {
        { "account",     accountId          },
        { "model",       account->model     },
        { "files",       fileMeta           },
        { "prompt_len",  int(prompt.length()) },
        { "max_tokens",  maxTokens          },
        { "temperature", temperature        },
    });

    // Читаем файлы синхронно до runAsync
    QString    fileError;
    QJsonArray fileBlocks = buildFileContentBlocks(files, fileError);
    if (!fileError.isEmpty()) {
        MCP_LOG_ERROR(QString("send_chat_request_with_files: file error — %1").arg(fileError));
        sendToolResult(id, MCPToolResult::error(fileError));
        return true;
    }

    MCP_LOG_DEBUG(QString("send_chat_request_with_files: built %1 content blocks")
                      .arg(fileBlocks.size()));

    QJsonArray userContent = fileBlocks;
    userContent.append(QJsonObject { { "type", "text" }, { "text", prompt } });

    QJsonArray messages;
    if (!system.isEmpty())
        messages.append(QJsonObject { { "role", "system" }, { "content", system } });
    messages.append(QJsonObject { { "role", "user" }, { "content", userContent } });

    const QString url    = (account->baseUrl.endsWith('/') ? account->baseUrl.left(account->baseUrl.length() - 1) : account->baseUrl) + "/chat/completions";
    const QString apiKey = account->apiKey;
    const QString model  = account->model;

    const QByteArray bodyBytes = QJsonDocument(QJsonObject {
        { "model",       model       },
        { "messages",    messages    },
        { "max_tokens",  maxTokens   },
        { "temperature", temperature },
        { "stream",      false       },
    }).toJson(QJsonDocument::Compact);

    MCP_LOG_DEBUG(QString("send_chat_request_with_files: request body size = %1 bytes")
                      .arg(bodyBytes.size()));

    runAsync([this, id, url, apiKey, bodyBytes, model, maxTokens]() {
        // ── Retry при пустом теле с HTTP 200 ─────────────────────────────────
        static constexpr int kMaxRetries   = 2;
        static constexpr int kRetryDelayMs = 3000;

        HttpResult     http;
        ParsedResponse pr;

        for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
            if (attempt > 0) {
                MCP_LOG_WARN(QString("send_chat_request_with_files: empty body on HTTP 200, "
                                     "retry %1/%2").arg(attempt).arg(kMaxRetries));
                QThread::msleep(kRetryDelayMs);
            }

            http = httpPost(url, apiKey, bodyBytes);
            pr   = parseApiResponse(http);

            if (pr.format == ResponseFormat::Empty && http.code == 200 && http.ok)
                continue;

            break;
        }

        if (!pr.ok) {
            sendToolResult(id, responseErrorResult(pr, http,
                "send_chat_request_with_files"));
            return;
        }
        if (pr.format != ResponseFormat::JsonChatCompletion &&
            pr.format != ResponseFormat::Sse)
        {
            MCP_LOG_WARN(QString("send_chat_request_with_files: unexpected OK format '%1'")
                             .arg(pr.formatName()));
            sendToolResult(id, MCPToolResult::okJson(QJsonObject {
                { "warning",   QString("Unexpected response format: %1").arg(pr.formatName()) },
                { "model",     model },
                { "raw_text",  pr.rawText.isEmpty()
                                   ? QString::fromUtf8(
                                         QJsonDocument(pr.rawJson).toJson(QJsonDocument::Compact))
                                   : pr.rawText },
                { "http_code", http.code },
            }));
            return;
        }

        if (pr.finishReason == "length") {
            MCP_LOG_WARN(QString("send_chat_request_with_files: finish_reason=length — "
                                 "response was truncated at max_tokens=%1. "
                                 "Model: %2. Consider increasing max_tokens.")
                             .arg(maxTokens).arg(pr.model));
        }

        MCP_LOG_INFO(QString("send_chat_request_with_files: OK, model=%1, "
                             "finish_reason=%2, content_len=%3")
                         .arg(pr.model).arg(pr.finishReason).arg(pr.content.length()));
        sendToolResult(id, chatCompletionToResult(pr));
    });
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// mimeTypeFromPath
// ─────────────────────────────────────────────────────────────────────────────

QString OpenRouterMCPServer::mimeTypeFromPath(const QString &filePath)
{
    QMimeDatabase db;
    return db.mimeTypeForFile(filePath, QMimeDatabase::MatchExtension).name();
}

// ─────────────────────────────────────────────────────────────────────────────
// buildFileContentBlocks
// ─────────────────────────────────────────────────────────────────────────────

QJsonArray OpenRouterMCPServer::buildFileContentBlocks(const QJsonArray &files,
                                                        QString          &errorOut)
{
    QJsonArray blocks;

    for (const QJsonValue &fv : files) {
        if (!fv.isObject()) {
            errorOut = "Each element of 'files' must be an object";
            return {};
        }
        const QJsonObject fd = fv.toObject();

        const QString type      = fd.value("type").toString().trimmed().toLower();
        const QString path      = fd.value("path").toString().trimmed();
        const QString mediaType = fd.value("media_type").toString().trimmed();
        const QString dataB64   = fd.value("data").toString().trimmed();

        if (type.isEmpty()) {
            errorOut = "File descriptor missing required field 'type' (image|document|text)";
            return {};
        }

        QByteArray rawData;
        QString    resolvedMime;

        if (!dataB64.isEmpty()) {
            rawData      = QByteArray::fromBase64(dataB64.toUtf8());
            resolvedMime = mediaType;
            MCP_LOG_DEBUG(QString("buildFileContentBlocks: inline base64, type=%1, "
                                  "mime=%2, decoded=%3 bytes")
                              .arg(type).arg(resolvedMime).arg(rawData.size()));
        } else if (!path.isEmpty()) {
            QFileInfo fi(path);
            if (!fi.exists() || !fi.isFile()) {
                errorOut = QString("File not found: %1").arg(path);
                return {};
            }
            if (fi.size() > kMaxFileSize) {
                errorOut = QString("File too large (max %1 MB): %2")
                               .arg(kMaxFileSize / (1024 * 1024)).arg(path);
                return {};
            }
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                errorOut = QString("Cannot open file: %1").arg(path);
                return {};
            }
            rawData = f.readAll();
            f.close();
            resolvedMime = mediaType.isEmpty() ? mimeTypeFromPath(path) : mediaType;
            MCP_LOG_DEBUG(QString("buildFileContentBlocks: file=%1, mime=%2, size=%3 bytes")
                              .arg(path).arg(resolvedMime).arg(rawData.size()));
        } else {
            errorOut = "File descriptor must contain either 'path' or 'data'";
            return {};
        }

        if (type == "image") {
            if (resolvedMime.isEmpty()) resolvedMime = "image/png";
            const QString dataUri = QString("data:%1;base64,%2")
                                        .arg(resolvedMime)
                                        .arg(QString::fromLatin1(rawData.toBase64()));
            blocks.append(QJsonObject {
                { "type",      "image_url" },
                { "image_url", QJsonObject { { "url", dataUri } } }
            });
            MCP_LOG_DEBUG(QString("buildFileContentBlocks: → image_url block, "
                                  "dataUri length=%1").arg(dataUri.length()));

        } else if (type == "document" || type == "text") {
            const bool isText = resolvedMime.startsWith("text/") ||
                                resolvedMime == "application/json" ||
                                resolvedMime == "application/xml"  ||
                                type == "text";
            if (isText) {
                const QString textContent = QString::fromUtf8(rawData);
                blocks.append(QJsonObject {
                    { "type", "text" },
                    { "text", textContent }
                });
                MCP_LOG_DEBUG(QString("buildFileContentBlocks: → text block, %1 chars")
                                  .arg(textContent.length()));
            } else {
                if (resolvedMime.isEmpty()) resolvedMime = "application/octet-stream";
                const QString dataUri = QString("data:%1;base64,%2")
                                            .arg(resolvedMime)
                                            .arg(QString::fromLatin1(rawData.toBase64()));
                blocks.append(QJsonObject {
                    { "type", "text" },
                    { "text", QString("[File: %1]\n%2").arg(resolvedMime).arg(dataUri) }
                });
                MCP_LOG_DEBUG(QString("buildFileContentBlocks: → binary-as-text block, "
                                      "mime=%1, dataUri length=%2")
                                  .arg(resolvedMime).arg(dataUri.length()));
            }
        } else {
            errorOut = QString("Unknown file type '%1'. Use: image, document, text").arg(type);
            return {};
        }
    }

    return blocks;
}

// ─────────────────────────────────────────────────────────────────────────────
// registerAllTools
// ─────────────────────────────────────────────────────────────────────────────

void OpenRouterMCPServer::registerAllTools()
{
    registerTool({
        "list_accounts",
        "Returns configured AI accounts (id, name, model, description, provider). "
        "API keys and file paths are never included.",
        {},
        "object"
    });

    registerTool({
        "add_account",
        "Adds a new AI account. The model is fixed per account.",
        {
            { "id",          "string", "Unique slug, e.g. \"or-gpt4o\"",                      true  },
            { "name",        "string", "Human-readable name",                                  true  },
            { "model",       "string", "Fixed model for this account, e.g. openai/gpt-4o",    true  },
            { "api_key",     "string", "API key",                                              true  },
            { "base_url",    "string", "Base API URL (default: https://openrouter.ai/api/v1)", false },
            { "description", "string", "Optional description",                                 false },
        },
        "object"
    });

    registerTool({
        "remove_account",
        "Removes an account by id.",
        { { "id", "string", "Account id to remove", true } },
        "object"
    });

    registerTool({
        "get_paid_models",
        "Fetches the list of paid models from the account's provider.",
        { { "account_id", "string", "Account id to use for the request", true } },
        "object"
    });

    registerTool({
        "get_free_models",
        "Fetches the list of free models from the account's provider.",
        { { "account_id", "string", "Account id to use for the request", true } },
        "object"
    });

    registerTool({
        "send_chat_request",
        "Sends a chat-completion request. The model is determined by the account configuration.",
        {
            { "account_id",  "string",  "Account id to use",                                                               true  },
            { "messages",    "array",   "Array of {\"role\":\"user\"|\"assistant\"|\"system\",\"content\":\"...\"}",        true  },
            { "max_tokens",  "integer", "Maximum tokens in the response (default: 4096)",                                   false },
            { "temperature", "number",  "Sampling temperature 0.0–2.0 (default: 0.7)",                                     false },
        },
        "object"
    });

    registerTool({
        "send_chat_request_with_files",
        "Sends a chat-completion request with attached files (images, documents, text). "
        "Files are read from disk and encoded as base64 before sending.",
        {
            { "account_id",  "string",  "Account id to use",                                                                        true  },
            { "prompt",      "string",  "User text prompt to send alongside the files",                                             true  },
            { "files",       "array",   "Array of file descriptors: "
                                        "{\"type\":\"image\"|\"document\"|\"text\","
                                        "\"path\":\"/abs/path\"} or "
                                        "{\"type\":\"image\",\"media_type\":\"image/png\",\"data\":\"<base64>\"}",                   true  },
            { "system",      "string",  "Optional system message",                                                                  false },
            { "max_tokens",  "integer", "Maximum tokens in the response (default: 4096)",                                           false },
            { "temperature", "number",  "Sampling temperature 0.0–2.0 (default: 0.7)",                                             false },
        },
        "object"
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool OpenRouterMCPServer::isFreeModel(const QJsonObject &m)
{
    const QJsonObject pricing = m.value("pricing").toObject();
    const QJsonValue  prompt  = pricing.value("prompt");
    if (prompt.isString()) {
        const QString s = prompt.toString();
        return s == "0" || s == "0.0" || s == "0.00";
    }
    if (prompt.isDouble()) return prompt.toDouble() == 0.0;
    return false;
}

QJsonObject OpenRouterMCPServer::modelSummary(const QJsonObject &m)
{
    QJsonObject s;
    s["id"]             = m.value("id");
    s["name"]           = m.value("name");
    s["description"]    = m.value("description");
    s["context_length"] = m.value("context_length");
    s["pricing"]        = m.value("pricing");
    const QJsonObject arch = m.value("architecture").toObject();
    if (!arch.isEmpty()) s["architecture"] = arch;
    return s;
}

void OpenRouterMCPServer::ensureAccountsFile()
{
    if (QFileInfo::exists(m_accountsFilePath)) return;
    QDir().mkpath(QFileInfo(m_accountsFilePath).absolutePath());

    QJsonObject example {
        { "id",          "or-gpt4o-mini"                     },
        { "name",        "OpenRouter GPT-4o Mini"            },
        { "model",       "openai/gpt-4o-mini"                },
        { "api_key",     "sk-or-v1-YOUR_KEY_HERE"            },
        { "base_url",    kDefaultBaseUrl                      },
        { "description", "Cost-effective GPT-4o Mini via OR" },
    };
    QJsonObject root {
        { "_comment", "KEEP SECRET — contains API keys. Do NOT commit to VCS." },
        { "accounts", QJsonArray { example } }
    };
    QFile f(m_accountsFilePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool OpenRouterMCPServer::loadAccounts()
{
    m_accounts.clear();
    QFile f(m_accountsFilePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();

    if (err.error != QJsonParseError::NoError) {
        MCP_LOG_ERROR(QString("loadAccounts: parse error — %1").arg(err.errorString()));
        sendLog("Failed to parse accounts file: " + err.errorString(), "error");
        return false;
    }

    for (const QJsonValue &v : doc.object().value("accounts").toArray()) {
        const QJsonObject o = v.toObject();
        AccountEntry e;
        e.id          = o.value("id").toString().trimmed();
        e.name        = o.value("name").toString().trimmed();
        e.model       = o.value("model").toString().trimmed();
        e.apiKey      = o.value("api_key").toString().trimmed();
        e.baseUrl     = o.value("base_url").toString().trimmed();
        e.description = o.value("description").toString().trimmed();

        if (e.id.isEmpty() || e.apiKey.isEmpty() || e.model.isEmpty()) continue;
        if (e.baseUrl.isEmpty()) e.baseUrl = kDefaultBaseUrl;
        m_accounts.insert(e.id, e);
        MCP_LOG_DEBUG(QString("loadAccounts: loaded '%1' (model=%2)").arg(e.id).arg(e.model));
    }
    MCP_LOG_INFO(QString("loadAccounts: %1 account(s) loaded").arg(m_accounts.size()));
    return true;
}

bool OpenRouterMCPServer::saveAccounts() const
{
    QJsonArray arr;
    for (const AccountEntry &a : qAsConst(m_accounts)) {
        arr.append(QJsonObject {
            { "id",          a.id          },
            { "name",        a.name        },
            { "model",       a.model       },
            { "api_key",     a.apiKey      },
            { "base_url",    a.baseUrl     },
            { "description", a.description },
        });
    }
    QJsonObject root {
        { "_comment", "KEEP SECRET — contains API keys. Do NOT commit to VCS." },
        { "accounts", arr }
    };
    const QString tmp = m_accountsFilePath + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    QFile::remove(m_accountsFilePath);
    return QFile::rename(tmp, m_accountsFilePath);
}

AccountEntry *OpenRouterMCPServer::findAccount(const QString &id)
{
    auto it = m_accounts.find(id);
    return (it != m_accounts.end()) ? &it.value() : nullptr;
}

QJsonObject OpenRouterMCPServer::safeAccount(const AccountEntry &a)
{
    return QJsonObject {
        { "id",          a.id                   },
        { "name",        a.name                 },
        { "model",       a.model                },
        { "description", a.description          },
        { "provider",    QUrl(a.baseUrl).host() },
    };
}

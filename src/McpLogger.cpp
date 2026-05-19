#include "McpLogger.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QJsonArray>
#include <QTextStream>

#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// Паттерны для маскировки секретов
// ─────────────────────────────────────────────────────────────────────────────

// Ключи, значения которых надо скрыть в JSON-телах запросов
static const QStringList kSecretJsonKeys = {
    "api_key", "apiKey", "api-key", "key", "secret",
    "password", "token", "access_token",
};

// Префиксы Authorization-заголовков (маскируем после пробела)
static const QStringList kSecretHeaderPrefixes = {
    "authorization:", "x-api-key:", "api-key:",
};

// Сколько символов ключа показывать в маске (первые + звёздочки + последние)
static constexpr int kKeyShowPrefix = 4;
static constexpr int kKeyShowSuffix = 4;
static constexpr int kMaskMinLen    = 12;  // ключи короче — маскируем полностью

// Максимальный размер тела для логирования (остаток отрезается)
static constexpr int kBodyLogLimit = 8 * 1024;  // 8 КБ

// ─────────────────────────────────────────────────────────────────────────────
// instance
// ─────────────────────────────────────────────────────────────────────────────

McpLogger &McpLogger::instance()
{
    static McpLogger logger;
    return logger;
}

// ─────────────────────────────────────────────────────────────────────────────
// open / close
// ─────────────────────────────────────────────────────────────────────────────

bool McpLogger::open(const QString &filePath)
{
    QMutexLocker lock(&m_mutex);
    if (m_file.isOpen()) m_file.close();

    m_filePath = filePath;
    if (filePath.isEmpty()) return true;  // только stderr

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    m_file.setFileName(filePath);

    if (!m_file.open(QIODevice::Append | QIODevice::Text)) {
        std::cerr << "[McpLogger] Cannot open log file: "
                  << filePath.toStdString() << "\n";
        return false;
    }
    return true;
}

void McpLogger::close()
{
    QMutexLocker lock(&m_mutex);
    if (m_file.isOpen()) m_file.close();
}

// ─────────────────────────────────────────────────────────────────────────────
// timestamp
// ─────────────────────────────────────────────────────────────────────────────

QString McpLogger::timestamp() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
}

// ─────────────────────────────────────────────────────────────────────────────
// levelTag
// ─────────────────────────────────────────────────────────────────────────────

QString McpLogger::levelTag(Level l)
{
    switch (l) {
    case Level::Debug: return "DBG";
    case Level::Info:  return "INF";
    case Level::Warn:  return "WRN";
    case Level::Error: return "ERR";
    }
    return "???";
}

// ─────────────────────────────────────────────────────────────────────────────
// maskSecrets (QString)
//
// Заменяет значения секретных ключей в строке.
// Ищет паттерны вида:
//   "api_key": "sk-or-v1-abcdef..."  →  "api_key": "sk-o****cdef"
//   Authorization: Bearer sk-or-...  →  Authorization: Bearer sk-o****...
// ─────────────────────────────────────────────────────────────────────────────

QString McpLogger::maskSecrets(const QString &text)
{
    QString result = text;

    // ── JSON-поля ─────────────────────────────────────────────────────────────
    for (const QString &key : kSecretJsonKeys) {
        // Ищем: "key": "VALUE" или "key":"VALUE"
        const QStringList patterns = {
            QString(R"("%1"\s*:\s*")").arg(key),
        };
        Q_UNUSED(patterns);

        int pos = 0;
        while (true) {
            // Ищем ключ в кавычках
            const QString keyPattern = "\"" + key + "\"";
            const int keyPos = result.indexOf(keyPattern, pos, Qt::CaseInsensitive);
            if (keyPos < 0) break;

            // После ключа ищем : "
            int colon = result.indexOf(':', keyPos + keyPattern.length());
            if (colon < 0) break;
            int quote1 = result.indexOf('"', colon + 1);
            if (quote1 < 0) break;
            int quote2 = result.indexOf('"', quote1 + 1);
            if (quote2 < 0) break;

            const QString value = result.mid(quote1 + 1, quote2 - quote1 - 1);
            QString masked;
            if (value.length() >= kMaskMinLen) {
                masked = value.left(kKeyShowPrefix) +
                         QString(value.length() - kKeyShowPrefix - kKeyShowSuffix, '*') +
                         value.right(kKeyShowSuffix);
            } else {
                masked = QString(value.length(), '*');
            }

            result.replace(quote1 + 1, value.length(), masked);
            pos = quote1 + 1 + masked.length() + 1;
        }
    }

    // ── HTTP-заголовки ────────────────────────────────────────────────────────
    for (const QString &prefix : kSecretHeaderPrefixes) {
        int pos = 0;
        while (true) {
            const int idx = result.indexOf(prefix, pos, Qt::CaseInsensitive);
            if (idx < 0) break;

            // Ищем пробел после двоеточия (Bearer <token> или просто <token>)
            int valueStart = idx + prefix.length();
            while (valueStart < result.length() && result[valueStart] == ' ')
                ++valueStart;

            // Если "Bearer " — пропускаем это слово
            if (result.mid(valueStart, 7).compare(QStringLiteral("Bearer "), Qt::CaseInsensitive) == 0)
                valueStart += 7;

            // Конец значения — перевод строки или конец строки
            int valueEnd = result.indexOf('\n', valueStart);
            if (valueEnd < 0) valueEnd = result.length();
            // Убираем \r если есть
            int actualEnd = valueEnd;
            if (actualEnd > valueStart && result[actualEnd - 1] == '\r')
                --actualEnd;

            const QString value = result.mid(valueStart, actualEnd - valueStart).trimmed();
            QString masked;
            if (value.length() >= kMaskMinLen) {
                masked = value.left(kKeyShowPrefix) +
                         QString(value.length() - kKeyShowPrefix - kKeyShowSuffix, '*') +
                         value.right(kKeyShowSuffix);
            } else {
                masked = QString(value.length(), '*');
            }

            result.replace(valueStart, actualEnd - valueStart, masked);
            pos = valueStart + masked.length();
        }
    }

    return result;
}

QString McpLogger::maskSecrets(const QByteArray &data)
{
    return maskSecrets(QString::fromUtf8(data));
}

// ─────────────────────────────────────────────────────────────────────────────
// prettyJson — пытается красиво отформатировать JSON, иначе возвращает как есть
// ─────────────────────────────────────────────────────────────────────────────

QString McpLogger::prettyJson(const QByteArray &data)
{
    const QByteArray trimmed = data.trimmed();
    if (trimmed.isEmpty()) return "(empty)";

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
    if (err.error == QJsonParseError::NoError) {
        // Красивый JSON, но ограничиваем размер
        const QByteArray pretty = doc.toJson(QJsonDocument::Indented);
        if (pretty.size() > kBodyLogLimit) {
            return QString::fromUtf8(pretty.left(kBodyLogLimit)) +
                   QString("\n... [truncated, total %1 bytes]").arg(pretty.size());
        }
        return QString::fromUtf8(pretty);
    }

    // Не JSON — возвращаем как текст (обрезаем если большой)
    if (trimmed.size() > kBodyLogLimit) {
        return QString::fromUtf8(trimmed.left(kBodyLogLimit)) +
               QString("\n... [truncated, total %1 bytes]").arg(trimmed.size());
    }
    return QString::fromUtf8(trimmed);
}

// ─────────────────────────────────────────────────────────────────────────────
// rotate
// ─────────────────────────────────────────────────────────────────────────────

void McpLogger::rotate()
{
    // Вызывается под m_mutex
    if (!m_file.isOpen()) return;
    if (m_file.size() < m_maxBytes) return;

    m_file.close();

    // Сдвигаем архивы: .log.2 → .log.3, .log.1 → .log.2, .log → .log.1
    for (int i = m_maxBackups - 1; i >= 1; --i) {
        const QString older  = m_filePath + "." + QString::number(i + 1);
        const QString newer  = m_filePath + "." + QString::number(i);
        QFile::remove(older);
        QFile::rename(newer, older);
    }
    QFile::rename(m_filePath, m_filePath + ".1");

    m_file.setFileName(m_filePath);
    m_file.open(QIODevice::Append | QIODevice::Text);
}

// ─────────────────────────────────────────────────────────────────────────────
// writeRaw — пишет строку в файл и опционально в stderr
// ─────────────────────────────────────────────────────────────────────────────

void McpLogger::writeRaw(const QString &line)
{
    // Вызывается под m_mutex
    rotate();

    if (m_file.isOpen()) {
        QTextStream ts(&m_file);
#if QT_VERSION_MAJOR < 6
        ts.setCodec("UTF-8");
#endif
        ts << line << "\n";
        m_file.flush();
    }

    if (m_echoStderr) {
        std::cerr << line.toStdString() << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// log
// ─────────────────────────────────────────────────────────────────────────────

void McpLogger::log(Level level, const QString &category, const QString &message)
{
    if (level < m_level) return;

    const QString line = QString("[%1] [%2] %3")
                             .arg(timestamp())
                             .arg(levelTag(level))
                             .arg(message);

    QMutexLocker lock(&m_mutex);
    writeRaw(line);
    Q_UNUSED(category);
}

// ─────────────────────────────────────────────────────────────────────────────
// logRequest
// ─────────────────────────────────────────────────────────────────────────────

void McpLogger::logRequest(const QString    &url,
                            const QString    &method,
                            const QByteArray &body,
                            const QString    &apiKeyHint)
{
    if (Level::Debug < m_level) return;

    // Маскируем тело перед логированием
    const QString maskedBody = maskSecrets(prettyJson(body));

    QStringList lines;
    lines << "┌── REQUEST ──────────────────────────────────────────";
    lines << QString("│ %1 %2").arg(method).arg(url);
    if (!apiKeyHint.isEmpty())
        lines << QString("│ Key hint: %1").arg(apiKeyHint);
    lines << "│ Body:";
    for (const QString &bodyLine : maskedBody.split('\n'))
        lines << "│   " + bodyLine;
    lines << "└─────────────────────────────────────────────────────";

    const QString block = lines.join('\n');
    const QString line  = QString("[%1] [DBG] %2").arg(timestamp()).arg(block);

    QMutexLocker lock(&m_mutex);
    writeRaw(line);
}

// ─────────────────────────────────────────────────────────────────────────────
// logResponse
// ─────────────────────────────────────────────────────────────────────────────

void McpLogger::logResponse(const QString    &url,
                             int               httpCode,
                             const QString    &contentType,
                             const QByteArray &body)
{
    if (Level::Debug < m_level) return;

    const QString maskedBody = maskSecrets(prettyJson(body));

    // Эмодзи статуса для быстрого визуального сканирования лога
    const QString status = (httpCode >= 200 && httpCode < 300) ? "✓" :
                           (httpCode >= 400 && httpCode < 500) ? "✗ Client" :
                           (httpCode >= 500)                   ? "✗ Server" : "~";

    QStringList lines;
    lines << "┌── RESPONSE ─────────────────────────────────────────";
    lines << QString("│ HTTP %1 %2  [%3]").arg(httpCode).arg(status).arg(url);
    lines << QString("│ Content-Type: %1").arg(contentType.isEmpty() ? "(none)" : contentType);
    lines << QString("│ Body (%1 bytes):").arg(body.size());
    for (const QString &bodyLine : maskedBody.split('\n'))
        lines << "│   " + bodyLine;
    lines << "└─────────────────────────────────────────────────────";

    const QString block = lines.join('\n');
    const QString line  = QString("[%1] [DBG] %2").arg(timestamp()).arg(block);

    QMutexLocker lock(&m_mutex);
    writeRaw(line);
}

// ─────────────────────────────────────────────────────────────────────────────
// logEvent
// ─────────────────────────────────────────────────────────────────────────────

void McpLogger::logEvent(const QString &event, const QJsonObject &details)
{
    if (Level::Info < m_level) return;

    QString msg = "EVENT: " + event;
    if (!details.isEmpty()) {
        const QString detStr = QString::fromUtf8(
            QJsonDocument(details).toJson(QJsonDocument::Compact));
        msg += "  " + detStr;
    }

    const QString line = QString("[%1] [INF] %2").arg(timestamp()).arg(msg);

    QMutexLocker lock(&m_mutex);
    writeRaw(line);
}

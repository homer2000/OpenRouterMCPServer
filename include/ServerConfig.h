#pragma once

#include <QString>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig
// ─────────────────────────────────────────────────────────────────────────────

struct ProxyConfig {
    bool    enabled  = false;
    QString type     = "http";      // "http" | "socks5"
    QString host;
    quint16 port     = 8080;
    QString username;
    QString password;               // СЕКРЕТ — не логируется
};

// ─────────────────────────────────────────────────────────────────────────────
// ServerConfig
//
// Конфигурация самого MCP-сервера (не аккаунтов).
// Хранится в JSON-файле рядом с бинарём, если путь явно не указан.
//
// Формат файла:
// {
//   "_comment": "...",
//   "proxy": {
//     "enabled":  false,
//     "type":     "http",        // "http" или "socks5"
//     "host":     "127.0.0.1",
//     "port":     8080,
//     "username": "",
//     "password": ""
//   }
// }
// ─────────────────────────────────────────────────────────────────────────────

struct ServerConfig {
    ProxyConfig proxy;

    // ── JSON (де)сериализация ─────────────────────────────────────────────────

    static ServerConfig fromJson(const QJsonObject &root)
    {
        ServerConfig cfg;

        const QJsonObject p = root.value("proxy").toObject();
        cfg.proxy.enabled  = p.value("enabled").toBool(false);
        cfg.proxy.type     = p.value("type").toString("http").toLower();
        cfg.proxy.host     = p.value("host").toString();
        cfg.proxy.port     = static_cast<quint16>(p.value("port").toInt(8080));
        cfg.proxy.username = p.value("username").toString();
        cfg.proxy.password = p.value("password").toString();

        return cfg;
    }

    QJsonObject toJson() const
    {
        QJsonObject p {
            { "enabled",  proxy.enabled         },
            { "type",     proxy.type             },
            { "host",     proxy.host             },
            { "port",     static_cast<int>(proxy.port) },
            { "username", proxy.username         },
            { "password", proxy.password         },
        };
        return QJsonObject {
            { "_comment",
              "Server configuration. Password is stored in plain text — "
              "keep this file private." },
            { "proxy", p },
        };
    }

    // ── Применение прокси к QNetworkAccessManager ─────────────────────────────
    //
    // Вызывается в рабочем потоке сразу после создания локального NAM.

    void applyProxy(QNetworkAccessManager &nam) const
    {
        if (!proxy.enabled || proxy.host.isEmpty()) {
            nam.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            return;
        }

        QNetworkProxy::ProxyType proxyType =
            (proxy.type == "socks5")
                ? QNetworkProxy::Socks5Proxy
                : QNetworkProxy::HttpProxy;

        QNetworkProxy qproxy(proxyType, proxy.host, proxy.port,
                             proxy.username, proxy.password);
        nam.setProxy(qproxy);
    }
};

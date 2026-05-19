#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDir>

#include "OpenRouterMCPServer.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("openrouter-mcp");
    app.setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("OpenRouter MCP Server");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption accountsOpt(
        {"a", "accounts"},
        "Path to accounts JSON file.",
        "accounts-file"
    );
    QCommandLineOption configOpt(
        {"c", "config"},
        "Path to server config JSON file (default: next to binary).",
        "config-file"
    );

    parser.addOption(accountsOpt);
    parser.addOption(configOpt);
    parser.process(app);

    // Путь к accounts.json — если не передан, рядом с бинарём
    QString accountsFile = parser.value(accountsOpt);
    if (accountsFile.isEmpty())
        accountsFile = QCoreApplication::applicationDirPath()
                       + QDir::separator() + "accounts.json";

    // Путь к server-config.json — если не передан, конструктор сам подставит дефолт
    const QString configFile = parser.value(configOpt);

    OpenRouterMCPServer server(accountsFile, configFile);

    // Включаем логирование — путь к файлу и уровень детализации
    // server.setupLogger(
    //     QDir::tempPath() + "/openrouter-mcp-server.log",
    //     McpLogger::Level::Debug   // Debug = всё, включая тела запросов и ответов
    // );

    server.start();

    return app.exec();
}

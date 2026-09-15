#include "BridgeService.h"
#include "Bundle.h"
#include "BridgeRuntime.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("paper-bridge");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({{"api", "Run unprivileged API proxy"},
                       {"ensure-service", "Start or reuse the device systemd service and verify its health"},
                       {"runtime", "Installed AppLoad runtime directory; requires --ensure-service", "directory"},
                       {"device-service", "Run the managed device service on the application socket"},
                       {"idle-timeout", "Exit after this many seconds without a client; 0 disables", "seconds", "0"},
                       {"sandbox", "Use only the specified test directory", "directory"},
                       {"sandbox-unvalidated", "Simulate unavailable validation; requires --sandbox"},
                       {"socket", "Unix socket path", "path"},
                       {"request", "Single worker request: HTTP method,route,json", "json"}});
    parser.process(app);
    const bool sandbox = parser.isSet("sandbox"), proxy = parser.isSet("api");
    auto root = parser.value("sandbox");
    if (proxy)
        qputenv("PAPER_BRIDGE_SOCKET", "/run/paper-bridge/privileged.sock");
    try {
        if (parser.isSet("ensure-service")) {
            if (sandbox || proxy || parser.isSet("request") || parser.isSet("socket") ||
                parser.isSet("device-service") || !parser.isSet("runtime"))
                throw std::runtime_error("INVALID_SERVICE_START_ARGUMENTS");
#ifdef Q_OS_UNIX
            if (geteuid() != 0)
                throw std::runtime_error("DEVICE_SERVICE_REQUIRES_ROOT");
#endif
            paper::BridgeRuntimeConfig config;
            config.runtime = parser.value("runtime");
            const auto result = paper::ensureBridgeRuntime(config);
            QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
            return result.contains("error") ? 1 : 0;
        }
        if (parser.isSet("runtime") || (parser.isSet("device-service") &&
            (sandbox || proxy || parser.isSet("request") || parser.isSet("socket"))))
            throw std::runtime_error("INVALID_DEVICE_SERVICE_ARGUMENTS");
        bool timeoutValid = false;
        const auto idleTimeout = parser.value("idle-timeout").toInt(&timeoutValid);
        if (!timeoutValid || idleTimeout < 0 || idleTimeout > 86400)
            throw std::runtime_error("INVALID_IDLE_TIMEOUT");
        BridgeService service(sandbox ? root + "/state" : "/home/root/.local/share/paper-bridge",
                              sandbox ? root + "/store" : "/home/root/.local/share/remarkable/xochitl",
                              sandbox, proxy, parser.isSet("sandbox-unvalidated"));
        service.setIdleTimeout(idleTimeout);
        service.recover();
        if (parser.isSet("request")) {
            auto req = QJsonDocument::fromJson(parser.value("request").toUtf8()).object();
            auto result =
                service.handle(req["method"].toString(), req["route"].toString(), req["body"].toObject());
            QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
            return result.contains("error") ? 1 : 0;
        }
        auto socket = parser.value("socket");
        if (socket.isEmpty())
            socket = proxy || parser.isSet("device-service") ? "/run/paper-bridge/core.sock"
                                                            : "/run/paper-bridge/privileged.sock";
        if (!service.listen(socket)) {
            QTextStream(stderr) << "Bridge socket unavailable\n";
            return 1;
        }
        return app.exec();
    } catch (const std::exception &e) {
        if (parser.isSet("request") || parser.isSet("ensure-service"))
            QTextStream(stdout) << QJsonDocument(
                                       QJsonObject{{"error",
                                                    QJsonObject{{"code", QString::fromLatin1(e.what())},
                                                                {"message", QString::fromLatin1(e.what())}}}})
                                       .toJson(QJsonDocument::Compact)
                                << Qt::endl;
        else
            QTextStream(stderr) << "Bridge startup failed: " << e.what() << Qt::endl;
        return 1;
    }
}

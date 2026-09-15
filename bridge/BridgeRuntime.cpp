#include "BridgeRuntime.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

namespace paper {
namespace {
QJsonObject failure(const char *code, const QString &message) {
    return {{"error", QJsonObject{{"code", code}, {"message", message}, {"retryable", true}}}};
}
bool healthy(const QJsonObject &health) {
    return health["status"].toString() == "ok" && health["service"].toString() == "paper-bridge" &&
           health["protocolVersion"].toInt() == 1;
}
QJsonObject ready(const QJsonObject &health, const char *state) {
    return {{"status", state}, {"service", "paper-bridge"}, {"protocolVersion", 1},
            {"health", health}, {"message", "Paper Bridge est connecté."}};
}
}

QJsonObject probeBridgeRuntime(const QString &endpoint, int timeoutMs) {
    QElapsedTimer deadline;
    deadline.start();
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(timeoutMs))
        return {};
    socket.write("GET /v1/health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n"
                 "Connection: close\r\n\r\n{}");
    if (!socket.waitForBytesWritten(timeoutMs) && socket.bytesToWrite())
        return {};
    QByteArray response;
    while (deadline.elapsed() < timeoutMs) {
        response += socket.readAll();
        if (response.size() > 65536)
            return {};
        if (socket.state() == QLocalSocket::UnconnectedState)
            break;
        socket.waitForReadyRead(qMax(1, timeoutMs - int(deadline.elapsed())));
    }
    response += socket.readAll();
    if (response.size() > 65536 || !response.startsWith("HTTP/1.1 200 "))
        return {};
    const auto separator = response.indexOf("\r\n\r\n");
    if (separator < 0)
        return {};
    return QJsonDocument::fromJson(response.mid(separator + 4)).object();
}

QJsonObject ensureBridgeRuntime(const BridgeRuntimeConfig &config) {
    auto health = probeBridgeRuntime(config.endpoint);
    if (healthy(health))
        return ready(health, "already-running");
    const QFileInfo folder(config.runtime);
    const auto runtime = folder.canonicalFilePath();
    if (!folder.isDir() || runtime.isEmpty() || !QFileInfo(runtime + "/bridge-run").isExecutable() ||
        !QFileInfo(runtime + "/paper-bridge").isExecutable())
        return failure("PAPER_BRIDGE_RUNTIME_MISSING", "Le runtime Paper Bridge est incomplet. Réinstallez le paquet d’applications.");
    if (!QFileInfo(config.manager).isExecutable())
        return failure("PAPER_BRIDGE_SERVICE_MANAGER_MISSING", "Le gestionnaire de service systemd-run est absent de la tablette.");
    if (!QDir().mkpath(QFileInfo(config.lock).absolutePath()))
        return failure("PAPER_BRIDGE_START_LOCK", "Le verrou de démarrage Paper Bridge est inaccessible.");
    QLockFile lock(config.lock);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(3000)) {
        health = probeBridgeRuntime(config.endpoint);
        return healthy(health) ? ready(health, "already-running")
                               : failure("PAPER_BRIDGE_START_BUSY", "Paper Bridge est déjà en cours de démarrage. Réessayez dans quelques secondes.");
    }
    health = probeBridgeRuntime(config.endpoint);
    if (healthy(health))
        return ready(health, "already-running");
    // A different live protocol on this path must not be replaced or unlinked.
    QLocalSocket existing;
    existing.connectToServer(config.endpoint);
    if (existing.waitForConnected(150))
        return failure("PAPER_BRIDGE_ENDPOINT_CONFLICT", "Un autre service occupe le socket Paper Bridge.");

    QProcess manager;
    auto environment = QProcessEnvironment::systemEnvironment();
    // A starter called from Qt must never preload the AppLoad input/framebuffer shim.
    for (const auto &name : {"LD_PRELOAD", "LD_LIBRARY_PATH", "QTFB_KEY", "REPAPER_BRIDGE_STARTER"})
        environment.remove(name);
    manager.setProcessEnvironment(environment);
    manager.start(config.manager,
        {"--no-ask-password", "--quiet", "--collect", "--unit=" + config.unit,
         "--description=rePaper document bridge", "--service-type=exec",
         "--property=Restart=on-failure", "--property=RestartSec=1s",
         "--property=TimeoutStopSec=120s", "--property=KillMode=control-group",
         runtime + "/bridge-run"});
    const bool managerFinished = manager.waitForFinished(5000);
    if (!managerFinished) {
        manager.kill();
        manager.waitForFinished(1000);
    }
    // Another starter may already own the transient unit. Its actual health is authoritative.
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 5000) {
        health = probeBridgeRuntime(config.endpoint, 250);
        if (healthy(health))
            return ready(health, "started");
        QThread::msleep(50);
    }
    return failure("PAPER_BRIDGE_START_FAILED",
        "Paper Bridge n’a pas pu démarrer. Consultez le service repaper-paper-bridge.service pour connaître la cause.");
}
}

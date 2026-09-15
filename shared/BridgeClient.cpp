#include "BridgeClient.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtConcurrent>

namespace repaper {
BridgeClient::BridgeClient(QObject *parent) : QObject(parent) {}
QJsonObject BridgeClient::request(const QString &method, const QString &route, const QJsonObject &body) {
    QLocalSocket socket;
    socket.connectToServer(qEnvironmentVariable("PAPER_BRIDGE_SOCKET", "/run/paper-bridge/core.sock"));
    auto error = [](const char *code, const QString &message) {
        return QJsonObject{{"error", QJsonObject{{"code", code}, {"message", message}}}};
    };
    if (!socket.waitForConnected(2000)) {
        const auto starter = qEnvironmentVariable("REPAPER_BRIDGE_STARTER");
        if (starter.isEmpty())
            return error("PAPER_BRIDGE_UNAVAILABLE", "Paper Bridge n’est pas démarré.");
        const QFileInfo helper(starter);
        if (!helper.isAbsolute() || !helper.isFile() || !helper.isExecutable())
            return error("PAPER_BRIDGE_RUNTIME_MISSING", "Le lanceur Paper Bridge est absent du paquet d’applications.");
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove("LD_PRELOAD");
        environment.remove("QTFB_KEY");
        process.setProcessEnvironment(environment);
        process.start(helper.canonicalFilePath(), {});
        if (!process.waitForFinished(15000)) {
            process.kill();
            process.waitForFinished(1000);
            return error("PAPER_BRIDGE_START_TIMEOUT", "Le démarrage de Paper Bridge a pris trop de temps. Réessayez dans quelques secondes.");
        }
        const auto startup = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        if (startup.contains("error"))
            return startup;
        socket.abort();
        socket.connectToServer(qEnvironmentVariable("PAPER_BRIDGE_SOCKET", "/run/paper-bridge/core.sock"));
        if (!socket.waitForConnected(2000))
            return error("PAPER_BRIDGE_START_FAILED", "Paper Bridge n’a pas pu démarrer. Consultez le service repaper-paper-bridge.service.");
    }
    auto requestBody = body;
    if (method == "POST" && (route == "/v1/notebooks" || route.endsWith("/open"))) {
        bool validKey = false;
        const int key = qEnvironmentVariable("QTFB_KEY").toInt(&validKey);
        if (validKey && key >= 0)
            requestBody["callerQtfbKey"] = key;
    }
    const auto payload = QJsonDocument(requestBody).toJson(QJsonDocument::Compact);
    socket.write(method.toUtf8() + " " + route.toUtf8() +
                 " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
                 QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
    if (!socket.waitForBytesWritten(3000) && socket.bytesToWrite())
        return error("PAPER_BRIDGE_IO", "Envoi au Bridge impossible.");
    QByteArray response;
    while (socket.state() != QLocalSocket::UnconnectedState) {
        if (!socket.waitForReadyRead(120000) && socket.bytesAvailable() == 0)
            break;
        response += socket.readAll();
        if (response.size() > 8 * 1024 * 1024)
            return error("PAPER_BRIDGE_PROTOCOL", "Réponse trop volumineuse.");
    }
    response += socket.readAll();
    auto separator = response.indexOf("\r\n\r\n");
    if (separator < 0)
        return error("PAPER_BRIDGE_IO", "Réponse du Bridge interrompue.");
    auto doc = QJsonDocument::fromJson(response.mid(separator + 4));
    if (!doc.isObject())
        return error("PAPER_BRIDGE_PROTOCOL", "Réponse du Bridge invalide.");
    return doc.object();
}
void BridgeClient::run(const QString &method, const QString &route, QJsonObject body) {
    if (m_busy)
        return;
    m_busy = true;
    m_message = "Opération en cours…";
    emit changed();
    auto watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, route] {
        auto result = watcher->result();
        watcher->deleteLater();
        m_busy = false;
        if (result.contains("error")) {
            auto e = result["error"].toObject();
            m_message = e["message"].toString();
            emit failed(e["code"].toString(), m_message);
        } else {
            if (route == "/v1/capabilities")
                m_capabilities = result.toVariantMap();
            m_message = result["message"].toString("Terminé.");
            if (result.contains("documentId") && (route == "/v1/imports" || route == "/v1/notebooks"))
                emit imported(result["documentId"].toString());
        }
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([method, route, body] { return request(method, route, body); }));
}
void BridgeClient::probe() {
    run("GET", "/v1/capabilities");
}
void BridgeClient::importFile(const QString &path, const QString &title, const QString &key) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
        m_message = "Fichier absent ou trop volumineux (64 Mio maximum).";
        emit failed("PAPER_INPUT_INVALID", m_message);
        emit changed();
        return;
    }
    // A local absolute path is accepted only from trusted Unix peers and constrained by the server to app
    // export/cache roots.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    run("POST", "/v1/imports",
        {{"path", QFileInfo(file).absoluteFilePath()},
         {"displayName", title},
         {"idempotencyKey", key},
         {"sha256", QString::fromLatin1(hash.result().toHex())}});
}
void BridgeClient::createNotebook(const QString &title, const QString &key) {
    createNotebook(title, key, {});
}
void BridgeClient::createNotebook(const QString &title, const QString &key, const QJsonObject &agenda) {
    QJsonObject body{{"displayName", title}, {"idempotencyKey", key}};
    if (!agenda.isEmpty()) body["agenda"] = agenda;
    run("POST", "/v1/notebooks", body);
}
void BridgeClient::openDocument(const QString &id) {
    run("POST", "/v1/documents/" + id + "/open");
}
} // namespace repaper

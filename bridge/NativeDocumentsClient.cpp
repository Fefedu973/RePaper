#include "NativeDocumentsClient.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalSocket>

namespace paper {
namespace {
QJsonObject failure(const char *code, const QString &message) {
    return {{"error", QJsonObject{{"code", code}, {"message", message}, {"retryable", true}}}};
}
}
QJsonObject nativeDocumentsRequest(const QString &method, const QString &route,
                                  const QJsonObject &body, int timeoutMs) {
    const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (payload.size() > 60000 || route.contains('\r') || route.contains('\n') ||
        (method != "GET" && method != "POST"))
        return failure("NATIVE_DOCUMENT_REQUEST_INVALID", "La demande de document natif est invalide.");
    QLocalSocket socket;
    socket.connectToServer(qEnvironmentVariable("REPAPER_NATIVE_DOCUMENTS_SOCKET",
                                                "/run/repaper-appload/documents.sock"));
    if (!socket.waitForConnected(qMin(timeoutMs, 1000)))
        return failure("NATIVE_DOCUMENT_HOST_UNAVAILABLE",
            "Paper Bridge est connecté, mais le service de documents natifs d’AppLoad n’est pas disponible. Rechargez la version mise à jour d’AppLoad.");
    socket.write(method.toUtf8() + ' ' + route.toUtf8() +
                 " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
                 QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
    if (!socket.waitForBytesWritten(1000) && socket.bytesToWrite())
        return failure("NATIVE_DOCUMENT_HOST_IO", "La demande n’a pas pu être envoyée au service de documents natifs.");
    QElapsedTimer deadline;
    deadline.start();
    QByteArray response;
    while (deadline.elapsed() < timeoutMs) {
        response += socket.readAll();
        if (response.size() > 65536)
            return failure("NATIVE_DOCUMENT_HOST_PROTOCOL", "La réponse du service de documents natifs est trop volumineuse.");
        if (socket.state() == QLocalSocket::UnconnectedState)
            break;
        socket.waitForReadyRead(qMax(1, timeoutMs - int(deadline.elapsed())));
    }
    response += socket.readAll();
    const auto separator = response.indexOf("\r\n\r\n");
    if (response.size() > 65536 || separator < 0 || !response.startsWith("HTTP/1.1 "))
        return failure("NATIVE_DOCUMENT_HOST_IO", "La réponse du service de documents natifs a été interrompue. Vous pouvez réessayer la même opération.");
    const auto document = QJsonDocument::fromJson(response.mid(separator + 4));
    if (!document.isObject())
        return failure("NATIVE_DOCUMENT_HOST_PROTOCOL", "La réponse du service de documents natifs est invalide.");
    return document.object();
}
}

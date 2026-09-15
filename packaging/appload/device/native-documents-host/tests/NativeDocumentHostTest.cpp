#include "../NativeDocumentHost.h"
#include "TestProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
bool until(const std::function<bool()> &ready, int timeout = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QTest::qWait(1);
    }
    return ready();
}
QByteArray http(const QByteArray &method, const QByteArray &route, const QJsonObject &body = {}) {
    const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return method + ' ' + route + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
           + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes;
}
QString journalPath(const QString &state, const QString &key) {
    return state + '/' + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
}
QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeJson(const QString &path, const QJsonObject &value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(value).toJson()) > 0;
}
QString errorCode(const QJsonObject &value) { return value["error"].toObject()["code"].toString(); }
mode_t mode(const QString &path) {
    struct stat info{};
    return ::stat(path.toLocal8Bit().constData(), &info) == 0 ? info.st_mode & 0777 : 0;
}
struct Client {
    QLocalSocket socket;
    QByteArray response;
    bool send(const QString &path, const QByteArray &request) {
        socket.connectToServer(path);
        if (!socket.waitForConnected(1000)) return false;
        socket.write(request);
        socket.flush();
        return true;
    }
    bool complete(int timeout = 3000) {
        const bool done = until([this] { return socket.state() == QLocalSocket::UnconnectedState; }, timeout);
        response += socket.readAll();
        return done && response.startsWith("HTTP/1.1 ");
    }
    QJsonObject json() const { return QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object(); }
};
struct Fixture {
    QTemporaryDir directory;
    NativeDocumentHost host;
    QString state, socket;
    Fixture() {
        state = directory.path() + "/state";
        socket = directory.path() + "/socket/documents.sock";
        host.setStateDirectory(state);
        host.setSocketPath(socket);
        host.setEnabled(true);
        host.componentComplete();
    }
};
QJsonObject notebook(const QString &key = "meeting", const QString &title = "Meeting notes") {
    return {{"displayName", title}, {"idempotencyKey", key}};
}
QByteArray openRoute(const QString &id) { return "/v1/documents/" + id.toUtf8() + "/open"; }
QJsonObject processLine(QProcess &process) {
    if (!until([&] { return process.canReadLine(); })) return {};
    return QJsonDocument::fromJson(process.readLine()).object();
}
void printJson(const QJsonObject &value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
}

class NativeDocumentHostTest : public QObject {
    Q_OBJECT
private slots:
    void healthCapabilitiesAndPermissions() {
        Fixture fixture;
        Client health;
        QVERIFY(health.send(fixture.socket, http("GET", "/v1/health")));
        QVERIFY(health.complete());
        QCOMPARE(health.json()["service"].toString(), QString("repaper-native-documents"));
        QCOMPARE(health.json()["protocolVersion"].toInt(), 1);
        QCOMPARE(mode(fixture.state), mode_t(0700));
        QCOMPARE(mode(QFileInfo(fixture.socket).absolutePath()), mode_t(0700));
        QCOMPARE(mode(fixture.socket) & 0077, mode_t(0));
        QCOMPARE(mode(fixture.socket) & 0600, mode_t(0600));
        fixture.host.setEnabled(false);
        Client capabilities;
        QVERIFY(capabilities.send(fixture.socket, http("GET", "/v1/capabilities")));
        QVERIFY(capabilities.complete());
        QCOMPARE(capabilities.json()["document.createNotebook"].toString(), QString("unavailable"));
        Client create;
        QVERIFY(create.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(create.complete());
        QCOMPARE(errorCode(create.json()), QString("NATIVE_LIBRARY_NOT_READY"));
    }

    void reservationPrecedesNativeCallAndSynchronousConfirmation() {
        Fixture fixture;
        QJsonObject reservation;
        bool privateJournal = false;
        connect(&fixture.host, &NativeDocumentHost::createRequested, this,
            [&](const QString &request, const QString &id, const QString &page, const QString &title) {
                reservation = readJson(journalPath(fixture.state, "meeting"));
                privateJournal = mode(journalPath(fixture.state, "meeting")) == 0600;
                QCOMPARE(reservation["documentId"].toString(), id);
                QCOMPARE(reservation["pageId"].toString(), page);
                QCOMPARE(reservation["displayName"].toString(), title);
                fixture.host.created(request, id);
            });
        Client client;
        QVERIFY(client.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(client.complete());
        QCOMPARE(reservation["state"].toString(), QString("reserved"));
        QVERIFY(privateJournal);
        QVERIFY(!QUuid(reservation["documentId"].toString()).isNull());
        QVERIFY(!QUuid(reservation["pageId"].toString()).isNull());
        QCOMPARE(client.json()["documentId"], reservation["documentId"]);
        QCOMPARE(client.json()["status"].toString(), QString("succeeded"));
        QVERIFY(client.json()["nativeIndexVerified"].toBool());
        QCOMPARE(readJson(journalPath(fixture.state, "meeting"))["state"].toString(), QString("created"));
    }

    void responseLostRetainsIdentityAndOriginalTitle() {
        Fixture fixture;
        QSignalSpy created(&fixture.host, &NativeDocumentHost::createRequested);
        Client first;
        QVERIFY(first.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(until([&] { return created.size() == 1; }));
        const auto original = created.at(0);
        first.socket.abort();
        QCoreApplication::processEvents();
        fixture.host.created(original[0].toString(), original[1].toString());
        QCOMPARE(readJson(journalPath(fixture.state, "meeting"))["state"].toString(), QString("created"));
        Client retry;
        QVERIFY(retry.send(fixture.socket, http("POST", "/v1/notebooks", notebook("meeting", "Changed title"))));
        QVERIFY(until([&] { return created.size() == 2; }));
        QCOMPARE(created.at(1)[1], original[1]);
        QCOMPARE(created.at(1)[2], original[2]);
        QCOMPARE(created.at(1)[3], original[3]);
        fixture.host.created(created.at(1)[0].toString(), original[1].toString());
        QVERIFY(retry.complete());
        QCOMPARE(retry.json()["documentId"].toString(), original[1].toString());
    }

    void killedProcessRetainsDurableReservation() {
        QTemporaryDir directory;
        const auto state = directory.path() + "/state";
        const auto path = directory.path() + "/socket/documents.sock";
        QProcess first;
        startTestProcess(first, {"--fixture-host", state, path, "hold"});
        QVERIFY(first.waitForStarted());
        QCOMPARE(processLine(first)["ready"].toBool(), true);
        Client initial;
        QVERIFY(initial.send(path, http("POST", "/v1/notebooks", notebook())));
        const auto reserved = processLine(first);
        QVERIFY(!reserved["documentId"].toString().isEmpty());
        QCOMPARE(readJson(journalPath(state, "meeting"))["state"].toString(), QString("reserved"));
        first.kill();
        QVERIFY(first.waitForFinished());
        QProcess second;
        startTestProcess(second, {"--fixture-host", state, path, "confirm"});
        QVERIFY(second.waitForStarted());
        QCOMPARE(processLine(second)["ready"].toBool(), true);
        Client retry;
        QVERIFY(retry.send(path, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(retry.complete());
        const auto recovered = processLine(second);
        QCOMPARE(recovered["documentId"], reserved["documentId"]);
        QCOMPARE(recovered["pageId"], reserved["pageId"]);
        QCOMPARE(retry.json()["documentId"], reserved["documentId"]);
        second.kill();
        QVERIFY(second.waitForFinished());
    }

    void timeoutRetryIgnoresLateConfirmation() {
        Fixture fixture;
        QSignalSpy created(&fixture.host, &NativeDocumentHost::createRequested);
        Client first;
        QVERIFY(first.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(until([&] { return created.size() == 1; }));
        const auto old = created.at(0);
        QVERIFY(first.complete(17000));
        QCOMPARE(errorCode(first.json()), QString("NATIVE_TIMEOUT"));
        QCOMPARE(readJson(journalPath(fixture.state, "meeting"))["state"].toString(), QString("reserved"));
        Client retry;
        QVERIFY(retry.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(until([&] { return created.size() == 2; }));
        QCOMPARE(created.at(1)[1], old[1]);
        QVERIFY(created.at(1)[0] != old[0]);
        fixture.host.created(old[0].toString(), old[1].toString());
        QCoreApplication::processEvents();
        QCOMPARE(retry.socket.state(), QLocalSocket::ConnectedState);
        fixture.host.created(created.at(1)[0].toString(), old[1].toString());
        QVERIFY(retry.complete());
        QCOMPARE(retry.json()["status"].toString(), QString("succeeded"));
    }

    void duplicateInFlightAndLibrarySessionLoss() {
        Fixture fixture;
        QSignalSpy created(&fixture.host, &NativeDocumentHost::createRequested);
        Client first;
        QVERIFY(first.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(until([&] { return created.size() == 1; }));
        Client duplicate;
        QVERIFY(duplicate.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(duplicate.complete());
        QCOMPARE(errorCode(duplicate.json()), QString("NATIVE_BUSY"));
        QCOMPARE(created.size(), 1);
        fixture.host.setEnabled(false);
        QVERIFY(first.complete());
        QCOMPARE(errorCode(first.json()), QString("NATIVE_LIBRARY_NOT_READY"));
        fixture.host.created(created.at(0)[0].toString(), created.at(0)[1].toString());
        QCOMPARE(readJson(journalPath(fixture.state, "meeting"))["state"].toString(), QString("reserved"));
        fixture.host.setEnabled(true);
        Client retry;
        QVERIFY(retry.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(until([&] { return created.size() == 2; }));
        QCOMPARE(created.at(1)[1], created.at(0)[1]);
        fixture.host.created(created.at(1)[0].toString(), created.at(1)[1].toString());
        QVERIFY(retry.complete());
        QCOMPARE(retry.json()["status"].toString(), QString("succeeded"));
    }

    void mismatchedCallbacksCannotConfirmOrDismiss() {
        Fixture fixture;
        QSignalSpy created(&fixture.host, &NativeDocumentHost::createRequested);
        QSignalSpy opened(&fixture.host, &NativeDocumentHost::openRequested);
        QSignalSpy dismissed(&fixture.host, &NativeDocumentHost::dismissAppRequested);
        Client create;
        QVERIFY(create.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(until([&] { return created.size() == 1; }));
        fixture.host.created(created.at(0)[0].toString(), QUuid::createUuid().toString());
        QVERIFY(create.complete());
        QCOMPARE(errorCode(create.json()), QString("NATIVE_ID_MISMATCH"));
        const auto id = created.at(0)[1].toString();
        Client wrong;
        QVERIFY(wrong.send(fixture.socket, http("POST", openRoute(id), {{"callerQtfbKey", 7}})));
        QVERIFY(until([&] { return opened.size() == 1; }));
        fixture.host.opened(opened.at(0)[0].toString(), QUuid::createUuid().toString());
        QVERIFY(wrong.complete());
        QCOMPARE(errorCode(wrong.json()), QString("NATIVE_ID_MISMATCH"));
        Client wrongKind;
        QVERIFY(wrongKind.send(fixture.socket, http("POST", openRoute(id), {{"callerQtfbKey", 7}})));
        QVERIFY(until([&] { return opened.size() == 2; }));
        fixture.host.created(opened.at(1)[0].toString(), id);
        QVERIFY(wrongKind.complete());
        QCOMPARE(errorCode(wrongKind.json()), QString("NATIVE_ID_MISMATCH"));
        QTest::qWait(200);
        QCOMPARE(dismissed.size(), 0);
        Client valid;
        QVERIFY(valid.send(fixture.socket, http("POST", openRoute(id), {{"callerQtfbKey", 42}})));
        QVERIFY(until([&] { return opened.size() == 3; }));
        QCOMPARE(dismissed.size(), 0);
        fixture.host.opened(opened.at(2)[0].toString(), id);
        QVERIFY(valid.complete());
        QCOMPARE(valid.json()["status"].toString(), QString("opened"));
        QVERIFY(until([&] { return dismissed.size() == 1; }));
        QCOMPARE(dismissed.at(0)[0].toInt(), 42);
    }

    void malformedRequests_data() {
        QTest::addColumn<QByteArray>("request");
        QTest::addColumn<QString>("expected");
        QTest::newRow("missing length") << QByteArray("GET /v1/health HTTP/1.1\r\nHost: local\r\n\r\n{}") << "INVALID_REQUEST";
        QTest::newRow("duplicate length") << QByteArray("GET /v1/health HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}") << "INVALID_REQUEST";
        QTest::newRow("negative length") << QByteArray("GET /v1/health HTTP/1.1\r\nContent-Length: -2\r\n\r\n{}") << "INVALID_REQUEST";
        QTest::newRow("chunked") << QByteArray("GET /v1/health HTTP/1.1\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n{}") << "INVALID_REQUEST";
        QTest::newRow("too large") << QByteArray("GET /v1/health HTTP/1.1\r\nContent-Length: 65537\r\n\r\n") << "INVALID_REQUEST";
        QTest::newRow("array") << QByteArray("GET /v1/health HTTP/1.1\r\nContent-Length: 2\r\n\r\n[]") << "INVALID_REQUEST";
        QTest::newRow("json syntax") << QByteArray("GET /v1/health HTTP/1.1\r\nContent-Length: 2\r\n\r\n}{") << "INVALID_REQUEST";
        QTest::newRow("pipelined") << http("GET", "/v1/health") + http("GET", "/v1/health") << "INVALID_REQUEST";
        QTest::newRow("large header") << QByteArray("GET /v1/health HTTP/1.1\r\nX: ") + QByteArray(8200, 'a') + "\r\nContent-Length: 2\r\n\r\n{}" << "INVALID_REQUEST";
        QTest::newRow("unknown method") << http("DELETE", "/v1/notebooks") << "NOT_FOUND";
        QTest::newRow("invalid UUID") << http("POST", "/v1/documents/not-a-uuid/open") << "INVALID_DOCUMENT_ID";
        QTest::newRow("invalid notebook") << http("POST", "/v1/notebooks", notebook("meeting", "  ")) << "INVALID_NOTEBOOK";
        QTest::newRow("fractional caller") << http("POST", "/v1/notebooks", {{"callerQtfbKey", 1.5}}) << "INVALID_CALLER";
        QTest::newRow("string caller") << http("POST", "/v1/notebooks", {{"callerQtfbKey", "1"}}) << "INVALID_CALLER";
        QTest::newRow("negative caller") << http("POST", "/v1/notebooks", {{"callerQtfbKey", -1}}) << "INVALID_CALLER";
    }
    void malformedRequests() {
        QFETCH(QByteArray, request);
        QFETCH(QString, expected);
        Fixture fixture;
        QSignalSpy created(&fixture.host, &NativeDocumentHost::createRequested);
        QSignalSpy opened(&fixture.host, &NativeDocumentHost::openRequested);
        Client client;
        QVERIFY(client.send(fixture.socket, request));
        QVERIFY(client.complete());
        QCOMPARE(errorCode(client.json()), expected);
        QCOMPARE(created.size(), 0);
        QCOMPARE(opened.size(), 0);
    }

    void brokenSymlinkAndCorruptedJournalsFailClosed() {
        Fixture fixture;
        QSignalSpy created(&fixture.host, &NativeDocumentHost::createRequested);
        const auto path = journalPath(fixture.state, "meeting");
        const auto target = fixture.directory.path() + "/outside.json";
        QVERIFY(::symlink(target.toLocal8Bit().constData(), path.toLocal8Bit().constData()) == 0);
        Client link;
        QVERIFY(link.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
        QVERIFY(link.complete());
        QCOMPARE(errorCode(link.json()), QString("NATIVE_JOURNAL_INVALID"));
        QVERIFY(!QFile::exists(target));
        QVERIFY(QFile::remove(path));
        QJsonObject valid{{"version", 1}, {"keyHash", QFileInfo(path).baseName()}, {"state", "reserved"},
            {"documentId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"pageId", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"displayName", "Meeting notes"}};
        for (const auto &field : {"version", "keyHash", "state", "documentId", "pageId", "displayName"}) {
            auto corrupt = valid;
            corrupt.remove(field);
            QVERIFY(writeJson(path, corrupt));
            Client client;
            QVERIFY(client.send(fixture.socket, http("POST", "/v1/notebooks", notebook())));
            QVERIFY(client.complete());
            QCOMPARE(errorCode(client.json()), QString("NATIVE_JOURNAL_INVALID"));
        }
        QCOMPARE(created.size(), 0);
    }

    void secondHostCannotStealSocketOrJournal() {
        Fixture first;
        NativeDocumentHost second;
        second.setStateDirectory(first.state);
        second.setSocketPath(first.directory.path() + "/other/documents.sock");
        second.setEnabled(true);
        second.componentComplete();
        QVERIFY(!QFileInfo::exists(second.socketPath()));
        Client health;
        QVERIFY(health.send(first.socket, http("GET", "/v1/health")));
        QVERIFY(health.complete());
        QCOMPARE(health.json()["status"].toString(), QString("ok"));
    }

    void peerCredentialsRejectAnotherUid() {
        if (::geteuid() != 0) QSKIP("Cross-UID peer test requires a root test process.");
        Fixture fixture;
        // Expose only this temporary socket to exercise SO_PEERCRED independently of filesystem permissions.
        QVERIFY(::chmod(fixture.directory.path().toLocal8Bit().constData(), 0755) == 0);
        QVERIFY(::chmod(QFileInfo(fixture.socket).absolutePath().toLocal8Bit().constData(), 0755) == 0);
        QVERIFY(::chmod(fixture.socket.toLocal8Bit().constData(), 0777) == 0);
        QProcess peer;
        startTestProcess(peer, {"--foreign-peer", fixture.socket});
        QVERIFY(peer.waitForStarted());
        QVERIFY(until([&] { return peer.state() == QProcess::NotRunning; }, 5000));
        QCOMPARE(peer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(peer.exitCode(), 0);
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 5 && args[1] == "--fixture-host") {
        NativeDocumentHost host;
        host.setStateDirectory(args[2]);
        host.setSocketPath(args[3]);
        host.setEnabled(true);
        QObject::connect(&host, &NativeDocumentHost::createRequested, &host,
            [&](const QString &request, const QString &id, const QString &page, const QString &title) {
                if (args[4] == "confirm") host.created(request, id);
                printJson({{"documentId", id}, {"pageId", page}, {"displayName", title}});
            });
        host.componentComplete();
        printJson({{"ready", true}});
        return app.exec();
    }
    if (args.size() == 3 && args[1] == "--foreign-peer") {
        if (::setgid(65534) || ::setuid(65534)) return 4;
        const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (descriptor < 0) return 5;
        struct sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto path = args[2].toLocal8Bit();
        if (path.size() >= int(sizeof address.sun_path)) return 6;
        std::memcpy(address.sun_path, path.constData(), size_t(path.size() + 1));
        if (::connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof address)) return 7;
        const auto request = http("GET", "/v1/health");
        ::send(descriptor, request.constData(), size_t(request.size()), MSG_NOSIGNAL);
        const timeval timeout{2, 0};
        ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        char response[4096];
        const auto received = ::recv(descriptor, response, sizeof response, 0);
        ::close(descriptor);
        return received <= 0 ? 0 : 8;
    }
    NativeDocumentHostTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "NativeDocumentHostTest.moc"

#include "../NativeDocumentHost.h"
#include "TestProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <sys/stat.h>

namespace {
bool until(const std::function<bool()> &ready, int timeout = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) { QCoreApplication::processEvents(); QTest::qWait(1); }
    return ready();
}
QString hash(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray http(const QByteArray &method, const QByteArray &route, const QJsonObject &value = {}) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    return method + ' ' + route + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
        + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes;
}
bool writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
QJsonObject readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeJson(const QString &path, const QJsonObject &value) {
    return writeBytes(path, QJsonDocument(value).toJson());
}
QString error(const QJsonObject &value) { return value["error"].toObject()["code"].toString(); }
mode_t permissions(const QString &path) {
    struct stat value{};
    return ::stat(path.toLocal8Bit().constData(), &value) == 0 ? value.st_mode & 0777 : 0;
}
struct Client {
    QLocalSocket socket;
    QByteArray response;
    bool send(const QString &path, const QByteArray &bytes) {
        socket.connectToServer(path);
        if (!socket.waitForConnected(1000)) return false;
        socket.write(bytes); socket.flush(); return true;
    }
    bool complete() {
        const bool done = until([&] { return socket.state() == QLocalSocket::UnconnectedState; });
        response += socket.readAll();
        return done && response.startsWith("HTTP/1.1 ");
    }
    QJsonObject json() const { return QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object(); }
};
struct Fixture {
    QTemporaryDir directory;
    QString state, socketPath, inputRoot, metadataRoot, source;
    QByteArray pdf = "%PDF-1.7\nfixture PDF data\n%%EOF\n";
    std::unique_ptr<NativeDocumentHost> host;
    Fixture() {
        state = directory.path() + "/state";
        socketPath = directory.path() + "/socket/documents.sock";
        inputRoot = directory.path() + "/bridge/native-imports";
        metadataRoot = directory.path() + "/native-index";
        source = inputRoot + "/prepared.pdf";
        QDir().mkpath(inputRoot);
        QDir().mkpath(metadataRoot);
        writeBytes(source, pdf);
        restart();
    }
    void restart() {
        host.reset();
        host = std::make_unique<NativeDocumentHost>();
        host->setStateDirectory(state); host->setSocketPath(socketPath);
        host->setInputRoot(inputRoot); host->setMetadataRoot(metadataRoot);
        host->setEnabled(true); host->setImportsEnabled(true); host->componentComplete();
    }
    QJsonObject body(const QString &key = "moodle-document") const {
        return {{"path", source}, {"sha256", hash(pdf)}, {"displayName", "Support de cours"}, {"idempotencyKey", key}};
    }
    QString journal(const QString &key = "moodle-document") const { return state + "/imports/" + hash(key.toUtf8()) + ".json"; }
};
void finishSuccessfully(Fixture &fixture, const QString &keyHash, const QString &id) {
    if (!fixture.host->identifyImport(keyHash, id)) return;
    fixture.host->completeImport(keyHash, id);
}
void printJson(const QJsonObject &value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fputc('\n', stdout); std::fflush(stdout);
}
QJsonObject processLine(QProcess &process) {
    if (!until([&] { return process.canReadLine(); })) return {};
    return QJsonDocument::fromJson(process.readLine()).object();
}
}

class NativeImportHostTest : public QObject {
    Q_OBJECT
private slots:
    void capabilitiesFollowBothReadinessFlags() {
        Fixture f;
        for (bool enabled : {false, true}) for (bool imports : {false, true}) {
            f.host->setEnabled(enabled); f.host->setImportsEnabled(imports);
            Client client;
            QVERIFY(client.send(f.socketPath, http("GET", "/v1/capabilities")));
            QVERIFY(client.complete());
            QCOMPARE(client.json()["document.import.pdf"].toString(), enabled && imports ? QString("available") : QString("unavailable"));
            QCOMPARE(client.json()["document.import.image"], client.json()["document.import.pdf"]);
            QCOMPARE(client.json()["document.createNotebook"].toString(), enabled ? QString("available") : QString("unavailable"));
        }
        f.host->setImportsEnabled(false);
        Client denied;
        QVERIFY(denied.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(denied.complete());
        QCOMPARE(error(denied.json()), QString("NATIVE_IMPORT_NOT_READY"));
    }
    void durableSnapshotDispatchIdentityAndCompletion() {
        Fixture f;
        QString snapshot, key, id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        connect(f.host.get(), &NativeDocumentHost::importRequested, this,
            [&](const QString &keyHash, const QString &url, const QString &token, const QString &documentId,
                const QString &displayName, bool dispatched, bool completed) {
                key = keyHash; snapshot = QUrl(url).toLocalFile();
                QVERIFY(!dispatched && !completed && documentId.isEmpty());
                QCOMPARE(displayName, QString("Support de cours"));
                QCOMPARE(QFileInfo(snapshot).fileName(), token + ".pdf");
                QCOMPARE(QFileInfo(snapshot).absolutePath(), f.state + "/imports/" + key);
                QCOMPARE(permissions(QFileInfo(snapshot).absolutePath()), mode_t(0700));
                QCOMPARE(permissions(snapshot), mode_t(0600));
                QCOMPARE(permissions(f.journal()), mode_t(0600));
                QCOMPARE(readJson(f.journal())["state"].toString(), QString("prepared"));
                QFile copied(snapshot); QVERIFY(copied.open(QIODevice::ReadOnly)); QCOMPARE(copied.readAll(), f.pdf);
                QVERIFY(f.host->dispatchImport(key));
                QCOMPARE(readJson(f.journal())["state"].toString(), QString("dispatched"));
                QVERIFY(f.host->identifyImport(key, id));
                QCOMPARE(readJson(f.journal())["state"].toString(), QString("identified"));
                QCOMPARE(readJson(f.journal())["documentId"].toString(), id);
                QVERIFY(QFileInfo::exists(snapshot));
                f.host->completeImport(key, id);
            });
        Client client;
        QVERIFY(client.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(client.complete());
        QCOMPARE(client.json()["documentId"].toString(), id);
        QCOMPARE(client.json()["status"].toString(), QString("succeeded"));
        QVERIFY(client.json()["nativeIndexVerified"].toBool());
        QCOMPARE(client.json()["message"].toString(), QString("Document importé."));
        QCOMPARE(readJson(f.journal())["state"].toString(), QString("completed"));
        QVERIFY(!QFileInfo::exists(snapshot));
        QVERIFY(QFileInfo::exists(f.source));
        QVERIFY(f.host->identifyImport(key, id));
        QCOMPARE(readJson(f.journal())["state"].toString(), QString("completed"));
    }
    void invalidInput_data() {
        QTest::addColumn<QString>("mutation"); QTest::addColumn<QString>("code");
        for (const auto &value : {"missing-path", "relative", "missing-hash", "uppercase-hash", "missing-key", "missing-title", "nul-path"})
            QTest::newRow(value) << QString(value) << QString("INVALID_IMPORT");
        for (const auto &value : {"outside", "prefix-collision", "traversal", "symlink", "parent-symlink", "fifo", "directory"})
            QTest::newRow(value) << QString(value) << QString("INVALID_IMPORT_PATH");
        QTest::newRow("hash-mismatch") << QString("hash-mismatch") << QString("IMPORT_HASH_MISMATCH");
        QTest::newRow("invalid-pdf") << QString("invalid-pdf") << QString("INVALID_PDF");
        QTest::newRow("empty") << QString("empty") << QString("INVALID_PDF");
        QTest::newRow("oversized") << QString("oversized") << QString("IMPORT_TOO_LARGE");
    }
    void invalidInput() {
        QFETCH(QString, mutation); QFETCH(QString, code);
        Fixture f;
        QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        auto body = f.body();
        if (mutation == "missing-path") body.remove("path");
        if (mutation == "relative") body["path"] = "prepared.pdf";
        if (mutation == "missing-hash") body.remove("sha256");
        if (mutation == "uppercase-hash") body["sha256"] = hash(f.pdf).toUpper();
        if (mutation == "missing-key") body.remove("idempotencyKey");
        if (mutation == "missing-title") body.remove("displayName");
        if (mutation == "nul-path") body["path"] = f.source + QChar::Null;
        if (mutation == "outside" || mutation == "prefix-collision") {
            const auto other = mutation == "outside" ? f.directory.path() + "/outside.pdf" : f.inputRoot + "-other/prepared.pdf";
            QVERIFY(QDir().mkpath(QFileInfo(other).absolutePath())); QVERIFY(writeBytes(other, f.pdf)); body["path"] = other;
        }
        if (mutation == "traversal") body["path"] = f.inputRoot + "/../native-imports/prepared.pdf";
        if (mutation == "symlink") { const auto link = f.inputRoot + "/link.pdf"; QVERIFY(QFile::link(f.source, link)); body["path"] = link; }
        if (mutation == "parent-symlink") {
            QVERIFY(QDir().mkpath(f.inputRoot + "/real")); QVERIFY(writeBytes(f.inputRoot + "/real/prepared.pdf", f.pdf));
            QVERIFY(QFile::link(f.inputRoot + "/real", f.inputRoot + "/link")); body["path"] = f.inputRoot + "/link/prepared.pdf";
        }
        if (mutation == "fifo") { const auto fifo = f.inputRoot + "/pipe"; QVERIFY(::mkfifo(fifo.toLocal8Bit().constData(), 0600) == 0); body["path"] = fifo; }
        if (mutation == "directory") body["path"] = f.inputRoot;
        if (mutation == "hash-mismatch") body["sha256"] = hash("other content");
        if (mutation == "invalid-pdf") { QVERIFY(writeBytes(f.source, "not a pdf")); body["sha256"] = hash("not a pdf"); }
        if (mutation == "empty") { QVERIFY(writeBytes(f.source, {})); body["sha256"] = hash({}); }
        if (mutation == "oversized") { QFile file(f.source); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.resize(64 * 1024 * 1024 + 1)); }
        Client client;
        QVERIFY(client.send(f.socketPath, http("POST", "/v1/imports", body)));
        QVERIFY(client.complete());
        QCOMPARE(error(client.json()), code);
        QCOMPARE(requests.size(), 0);
        QVERIFY(!QFileInfo::exists(f.journal()));
    }
    void maximumSizeStreamsSuccessfully() {
        Fixture f;
        QFile file(f.source); QVERIFY(file.open(QIODevice::WriteOnly));
        QCryptographicHash digest(QCryptographicHash::Sha256);
        QByteArray block(65536, ' '); block.replace(0, 9, "%PDF-1.7\n");
        for (int i = 0; i < 1024; ++i) { QCOMPARE(file.write(block), qint64(block.size())); digest.addData(block); block.fill(' '); }
        file.close();
        QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        auto body = f.body(); body["sha256"] = QString::fromLatin1(digest.result().toHex());
        Client client; QVERIFY(client.send(f.socketPath, http("POST", "/v1/imports", body)));
        QVERIFY(until([&] { return requests.size() == 1; }));
        QCOMPARE(QFileInfo(QUrl(requests[0][1].toString()).toLocalFile()).size(), qint64(64 * 1024 * 1024));
        f.host->failImport(requests[0][0].toString(), "NATIVE_FOLDER_UNAVAILABLE", "No folder");
        QVERIFY(client.complete());
    }
    void inflightConflictAndNoDuplicateDispatch() {
        Fixture f;
        QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        Client duplicate; QVERIFY(duplicate.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(duplicate.complete()); QCOMPARE(error(duplicate.json()), QString("NATIVE_BUSY"));
        auto changed = f.body(); changed["sha256"] = hash("different PDF");
        Client conflict; QVERIFY(conflict.send(f.socketPath, http("POST", "/v1/imports", changed)));
        QVERIFY(conflict.complete()); QCOMPARE(error(conflict.json()), QString("IDEMPOTENCY_CONFLICT"));
        changed = f.body(); changed["displayName"] = "Different title";
        Client titleConflict; QVERIFY(titleConflict.send(f.socketPath, http("POST", "/v1/imports", changed)));
        QVERIFY(titleConflict.complete()); QCOMPARE(error(titleConflict.json()), QString("IDEMPOTENCY_CONFLICT"));
        const auto key = requests[0][0].toString();
        QVERIFY(f.host->dispatchImport(key));
        QVERIFY(!f.host->dispatchImport(key));
        QVERIFY(first.complete()); QCOMPARE(error(first.json()), QString("NATIVE_IMPORT_UNCERTAIN"));
        QCOMPARE(requests.size(), 1);
    }
    void timeoutKeepsJobForLateCallback() {
        Fixture f;
        QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        const auto key = requests[0][0].toString(), snapshot = QUrl(requests[0][1].toString()).toLocalFile();
        QVERIFY(f.host->dispatchImport(key));
        bool expired = false;
        for (auto timer : f.host->findChildren<QTimer *>()) if (timer->interval() == 90000) {
            timer->setInterval(1); timer->start(); expired = true;
        }
        QVERIFY(expired); QVERIFY(first.complete()); QCOMPARE(error(first.json()), QString("NATIVE_TIMEOUT"));
        QVERIFY(QFileInfo::exists(snapshot));
        Client retry; QVERIFY(retry.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(retry.complete()); QCOMPARE(error(retry.json()), QString("NATIVE_IMPORT_UNCERTAIN"));
        QCOMPARE(requests.size(), 1);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        finishSuccessfully(f, key, id);
        QCOMPARE(readJson(f.journal())["state"].toString(), QString("completed"));
        QVERIFY(!QFileInfo::exists(snapshot));
        auto body = f.body(); body["path"] = f.inputRoot + "/removed.pdf";
        Client complete; QVERIFY(complete.send(f.socketPath, http("POST", "/v1/imports", body)));
        QVERIFY(until([&] { return requests.size() == 2; }));
        QCOMPARE(requests[1][3].toString(), id); QCOMPARE(requests[1][4].toString(), QString("Support de cours"));
        QVERIFY(requests[1][5].toBool() && requests[1][6].toBool());
        f.host->completeImport(key, id); QVERIFY(complete.complete()); QCOMPARE(complete.json()["documentId"].toString(), id);
    }
    void disconnectedWaiterDoesNotLoseNativeResult() {
        Fixture f;
        QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        const auto key = requests[0][0].toString(); QVERIFY(f.host->dispatchImport(key));
        first.socket.abort(); QCoreApplication::processEvents();
        finishSuccessfully(f, key, QUuid::createUuid().toString(QUuid::WithoutBraces));
        QCOMPARE(readJson(f.journal())["state"].toString(), QString("completed"));
    }
    void readinessLossRejectsWaitersAndLateCallbacks_data() {
        QTest::addColumn<bool>("importsOnly");
        QTest::newRow("library") << false; QTest::newRow("imports") << true;
    }
    void readinessLossRejectsWaitersAndLateCallbacks() {
        QFETCH(bool, importsOnly);
        Fixture f;
        QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        const auto key = requests[0][0].toString(); QVERIFY(f.host->dispatchImport(key));
        if (importsOnly) f.host->setImportsEnabled(false); else f.host->setEnabled(false);
        QVERIFY(first.complete());
        QCOMPARE(error(first.json()), importsOnly ? QString("NATIVE_IMPORT_NOT_READY") : QString("NATIVE_LIBRARY_NOT_READY"));
        const auto before = readJson(f.journal());
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(!f.host->identifyImport(key, id)); f.host->completeImport(key, id);
        f.host->failImport(key, "NATIVE_IMPORT_FAILED", "late failure");
        QCOMPARE(readJson(f.journal()), before);
    }
    void confirmedFailureCanRetryButUncertaintyCannot_data() {
        QTest::addColumn<QString>("code"); QTest::addColumn<bool>("dispatched"); QTest::addColumn<bool>("canRetry");
        QTest::newRow("pre-call") << QString("NATIVE_FOLDER_UNAVAILABLE") << false << true;
        QTest::newRow("native-failed") << QString("NATIVE_IMPORT_FAILED") << true << true;
        QTest::newRow("index-uncertain") << QString("NATIVE_IMPORT_UNCERTAIN") << true << false;
    }
    void confirmedFailureCanRetryButUncertaintyCannot() {
        QFETCH(QString, code); QFETCH(bool, dispatched); QFETCH(bool, canRetry);
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; })); const auto key = requests[0][0].toString();
        if (dispatched) QVERIFY(f.host->dispatchImport(key));
        f.host->failImport(key, code, "Failure"); QVERIFY(first.complete());
        QVERIFY(first.json()["error"].toObject()["retryable"].toBool());
        QVERIFY(QFileInfo::exists(QUrl(requests[0][1].toString()).toLocalFile()));
        Client retry; QVERIFY(retry.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        if (canRetry) {
            QVERIFY(until([&] { return requests.size() == 2; })); QVERIFY(!requests[1][5].toBool());
            QVERIFY(f.host->dispatchImport(key)); finishSuccessfully(f, key, QUuid::createUuid().toString(QUuid::WithoutBraces));
            QVERIFY(retry.complete()); QCOMPARE(retry.json()["status"].toString(), QString("succeeded"));
        } else {
            QVERIFY(retry.complete()); QCOMPARE(error(retry.json()), QString("NATIVE_IMPORT_UNCERTAIN")); QCOMPARE(requests.size(), 1);
        }
    }
    void restartDispatchedRecoversOnlyUniqueReservedMetadata_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("found");
        QTest::newRow("token") << QString("token") << true;
        QTest::newRow("pdf-suffix") << QString("pdf-suffix") << true;
        for (const auto &value : {"absent", "wrong-name", "duplicate", "symlink", "bad-id", "malformed", "deleted", "trash"})
            QTest::newRow(value) << QString(value) << false;
    }
    void restartDispatchedRecoversOnlyUniqueReservedMetadata() {
        QFETCH(QString, kind); QFETCH(bool, found);
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        const auto key = requests[0][0].toString(), token = requests[0][2].toString();
        QVERIFY(f.host->dispatchImport(key)); first.socket.abort();
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString name = kind == "wrong-name" ? "Renamed by user" : token + (kind == "pdf-suffix" ? ".pdf" : "");
        const auto metadata = f.metadataRoot + '/' + (kind == "bad-id" ? "not-a-uuid" : id) + ".metadata";
        if (kind != "absent") {
            if (kind == "malformed") QVERIFY(writeBytes(metadata, "{broken"));
            else if (kind == "symlink") {
                const auto outside = f.directory.path() + "/outside.metadata";
                QVERIFY(writeJson(outside, {{"visibleName", name}})); QVERIFY(QFile::link(outside, metadata));
            } else QVERIFY(writeJson(metadata, {{"visibleName", name}, {"deleted", kind == "deleted"},
                                                {"parent", kind == "trash" ? "trash" : ""}}));
        }
        if (kind == "duplicate") QVERIFY(writeJson(f.metadataRoot + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".metadata", {{"visibleName", token}}));
        f.restart(); QSignalSpy recovered(f.host.get(), &NativeDocumentHost::importRequested);
        Client retry; QVERIFY(retry.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        if (found) {
            QVERIFY(until([&] { return recovered.size() == 1; }));
            QCOMPARE(recovered[0][3].toString(), id); QVERIFY(recovered[0][5].toBool()); QVERIFY(!recovered[0][6].toBool());
            finishSuccessfully(f, key, id); QVERIFY(retry.complete()); QCOMPARE(retry.json()["documentId"].toString(), id);
            QCOMPARE(readJson(metadata)["visibleName"].toString(), name); // Host never writes native metadata.
        } else {
            QVERIFY(retry.complete()); QCOMPARE(error(retry.json()), QString("NATIVE_IMPORT_UNCERTAIN")); QCOMPARE(recovered.size(), 0);
        }
    }
    void identifiedRestartPreservesIdWithoutMetadataToken() {
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; })); const auto key = requests[0][0].toString();
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(f.host->dispatchImport(key)); QVERIFY(f.host->identifyImport(key, id)); first.socket.abort();
        f.restart(); QSignalSpy recovered(f.host.get(), &NativeDocumentHost::importRequested);
        Client retry; QVERIFY(retry.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return recovered.size() == 1; })); QCOMPARE(recovered[0][3].toString(), id);
        QVERIFY(recovered[0][5].toBool()); finishSuccessfully(f, key, id); QVERIFY(retry.complete());
    }
    void journalTamperingIsRejected_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &value : {"malformed", "wrong-key", "wrong-state", "bad-token", "bad-id", "symlink", "staging-symlink"})
            QTest::newRow(value) << QString(value);
    }
    void journalTamperingIsRejected() {
        QFETCH(QString, kind);
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; })); first.socket.abort();
        const auto snapshot = QUrl(requests[0][1].toString()).toLocalFile();
        auto journal = readJson(f.journal()); f.host.reset();
        if (kind == "malformed") QVERIFY(writeBytes(f.journal(), "{broken"));
        else if (kind == "symlink") {
            const auto outside = f.directory.path() + "/outside.json"; QVERIFY(writeJson(outside, journal));
            QVERIFY(QFile::remove(f.journal())); QVERIFY(QFile::link(outside, f.journal()));
        } else if (kind == "staging-symlink") {
            QVERIFY(QFile::remove(snapshot)); QVERIFY(QFile::link(f.source, snapshot));
        } else {
            if (kind == "wrong-key") journal["keyHash"] = hash("other");
            if (kind == "wrong-state") journal["state"] = "unknown";
            if (kind == "bad-token") journal["token"] = "../../native/document";
            if (kind == "bad-id") { journal["state"] = "identified"; journal["documentId"] = "bad"; }
            QVERIFY(writeJson(f.journal(), journal));
        }
        f.restart(); QSignalSpy recovered(f.host.get(), &NativeDocumentHost::importRequested);
        Client retry; QVERIFY(retry.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(retry.complete()); QCOMPARE(error(retry.json()), QString("NATIVE_JOURNAL_INVALID")); QCOMPARE(recovered.size(), 0);
    }
    void fourActiveImportsRemainBoundedAfterDisconnect() {
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        for (int i = 0; i < 4; ++i) {
            Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body(QString::number(i)))));
            QVERIFY(until([&] { return requests.size() == i + 1; }));
            QVERIFY(f.host->dispatchImport(requests[i][0].toString())); first.socket.abort(); QCoreApplication::processEvents();
        }
        Client fifth; QVERIFY(fifth.send(f.socketPath, http("POST", "/v1/imports", f.body("fifth"))));
        QVERIFY(fifth.complete()); QCOMPARE(error(fifth.json()), QString("NATIVE_BUSY")); QCOMPARE(requests.size(), 4);
        f.host->failImport(requests[0][0].toString(), "NATIVE_IMPORT_FAILED", "confirmed failure");
        Client next; QVERIFY(next.send(f.socketPath, http("POST", "/v1/imports", f.body("fifth"))));
        QVERIFY(until([&] { return requests.size() == 5; }));
    }
    void preparedRestartCanDispatchOnce() {
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        const auto key = requests[0][0].toString(), token = requests[0][2].toString();
        first.socket.abort(); f.restart();
        QSignalSpy recovered(f.host.get(), &NativeDocumentHost::importRequested);
        Client retry; QVERIFY(retry.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return recovered.size() == 1; }));
        QCOMPARE(recovered[0][2].toString(), token); QVERIFY(!recovered[0][5].toBool());
        QVERIFY(f.host->dispatchImport(key));
        finishSuccessfully(f, key, QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(retry.complete()); QCOMPARE(retry.json()["status"].toString(), QString("succeeded"));
    }
    void journalFailurePreventsNativeDispatch() {
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        const auto key = requests[0][0].toString(), snapshot = QUrl(requests[0][1].toString()).toLocalFile();
        QVERIFY(QFile::remove(f.journal())); QVERIFY(QDir().mkdir(f.journal()));
        QVERIFY(!f.host->dispatchImport(key));
        QVERIFY(first.complete()); QCOMPARE(error(first.json()), QString("NATIVE_JOURNAL_WRITE"));
        QVERIFY(QFileInfo::exists(snapshot));
    }
    void nativeIdMustBeDurableBeforeCompletion() {
        Fixture f; QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(until([&] { return requests.size() == 1; }));
        const auto key = requests[0][0].toString(), snapshot = QUrl(requests[0][1].toString()).toLocalFile();
        QVERIFY(f.host->dispatchImport(key));
        f.host->completeImport(key, QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(first.complete()); QCOMPARE(error(first.json()), QString("NATIVE_ID_MISMATCH"));
        QCOMPARE(readJson(f.journal())["state"].toString(), QString("dispatched"));
        QVERIFY(QFileInfo::exists(snapshot));
    }
    void killedProcessLeavesDispatchedJournalWithoutSecondImport() {
        Fixture f; f.host.reset();
        QProcess process;
        startTestProcess(process, {"--hold-import", f.state, f.socketPath, f.inputRoot, f.metadataRoot});
        QVERIFY(process.waitForStarted()); QCOMPARE(processLine(process)["ready"].toBool(), true);
        Client first; QVERIFY(first.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        const auto event = processLine(process); QVERIFY(event["dispatched"].toBool());
        QCOMPARE(readJson(f.journal())["state"].toString(), QString("dispatched"));
        process.kill(); QVERIFY(process.waitForFinished()); first.socket.abort();
        f.restart(); QSignalSpy requests(f.host.get(), &NativeDocumentHost::importRequested);
        Client retry; QVERIFY(retry.send(f.socketPath, http("POST", "/v1/imports", f.body())));
        QVERIFY(retry.complete()); QCOMPARE(error(retry.json()), QString("NATIVE_IMPORT_UNCERTAIN")); QCOMPARE(requests.size(), 0);
        QVERIFY(QFileInfo::exists(QUrl(event["url"].toString()).toLocalFile()));
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 6 && args[1] == "--hold-import") {
        NativeDocumentHost host;
        host.setStateDirectory(args[2]); host.setSocketPath(args[3]);
        host.setInputRoot(args[4]); host.setMetadataRoot(args[5]);
        host.setEnabled(true); host.setImportsEnabled(true);
        QObject::connect(&host, &NativeDocumentHost::importRequested, &host,
            [&](const QString &key, const QString &url, const QString &token, const QString &, const QString &, bool, bool) {
                const bool dispatched = host.dispatchImport(key);
                printJson({{"key", key}, {"url", url}, {"token", token}, {"dispatched", dispatched}});
            });
        host.componentComplete(); printJson({{"ready", true}}); return app.exec();
    }
    NativeImportHostTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "NativeImportHostTest.moc"

#include "NativeDocumentHost.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

class NativeDocumentsAdapterTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<QQmlApplicationEngine> engine;
    NativeDocumentHost *adapter = nullptr;
    QString socketPath;
    QObject *object(const char *name) { return adapter->property(name).value<QObject *>(); }
    QJsonObject request(const QString &method, const QString &route, const QJsonObject &body = {}) {
        QLocalSocket socket;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        timer.setInterval(3000);
        QByteArray response;
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::readyRead, &loop, [&] { response += socket.readAll(); });
        connect(&socket, &QLocalSocket::connected, &loop, [&] {
            const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
            socket.write(method.toUtf8() + ' ' + route.toUtf8()
                         + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                         + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
        });
        socket.connectToServer(socketPath);
        timer.start();
        loop.exec();
        response += socket.readAll();
        const int separator = response.indexOf("\r\n\r\n");
        if (separator < 0) return {{"testFailure", "missing HTTP response"}};
        return QJsonDocument::fromJson(response.mid(separator + 4)).object();
    }
private slots:
    void initTestCase() {
        qmlRegisterType<NativeDocumentHost>("net.asivery.AppLoad", 1, 0, "NativeDocumentHost");
        qmlRegisterType(QUrl("qrc:/appload/qml/RePaperNativeDocuments.qml"), "net.asivery.AppLoad", 1, 0,
                        "RePaperNativeDocuments");
    }
    void init() {
        directory = std::make_unique<QTemporaryDir>();
        QVERIFY(directory->isValid());
        socketPath = directory->filePath("run/documents.sock");
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty("testSocketPath", socketPath);
        engine->rootContext()->setContextProperty("testStateDirectory", directory->filePath("state"));
        engine->load(QUrl("qrc:/native-documents-tests/AdapterFixture.qml"));
        QVERIFY(!engine->rootObjects().isEmpty());
        adapter = qobject_cast<NativeDocumentHost *>(engine->rootObjects().first());
        QVERIFY(adapter);
        QVERIFY(adapter->enabled());
    }
    void cleanup() { engine.reset(); directory.reset(); adapter = nullptr; }
    void createsWithReservedIdsAndRetryDoesNotReformatExistingNote() {
        const auto result = request("POST", "/v1/notebooks", {{"displayName", "Notes de cours"}, {"idempotencyKey", "event:one"}});
        QVERIFY2(!result.contains("error"), QJsonDocument(result).toJson().constData());
        const auto id = result.value("documentId").toString();
        QVERIFY(!QUuid(id).isNull());
        QCOMPARE(result.value("status").toString(), QString("succeeded"));
        QVERIFY(result.value("nativeIndexVerified").toBool());
        auto controller = object("fakeLibraryController");
        QVERIFY(controller);
        QCOMPARE(controller->property("createCalls").toInt(), 1);
        QVERIFY(!QUuid(controller->property("requestedPage").toString()).isNull());
        QCOMPARE(controller->property("orientationCalls").toInt(), 1);
        QCOMPARE(controller->property("coverCalls").toInt(), 1);
        QCOMPARE(object("fakeDocumentController")->property("templateName").toString(), QString("Blank"));
        const auto repeated = request("POST", "/v1/notebooks", {{"displayName", "Titre modifié"}, {"idempotencyKey", "event:one"}});
        QCOMPARE(repeated.value("documentId").toString(), id);
        QCOMPARE(controller->property("createCalls").toInt(), 1);
        QCOMPARE(controller->property("orientationCalls").toInt(), 1);
        QCOMPARE(object("fakeDocumentController")->property("templateCalls").toInt(), 1);
    }
    void reportsOpenOnlyAfterTheRequestedDocumentIsLoaded() {
        const auto created = request("POST", "/v1/notebooks", {{"displayName", "Séance"}, {"idempotencyKey", "event:two"}});
        const QString id = created.value("documentId").toString();
        QVERIFY(!id.isEmpty());
        QSignalSpy dismiss(adapter, &NativeDocumentHost::dismissAppRequested);
        const auto opened = request("POST", "/v1/documents/" + id + "/open", {{"callerQtfbKey", 4567}});
        QCOMPARE(opened.value("status").toString(), QString("opened"));
        QVERIFY(!opened.contains("documentId"));
        QCOMPARE(object("fakeNavigator")->property("lastRoute").toString(), QString("legacydevice/window/main"));
        QCOMPARE(object("fakeNavigator")->property("lastId").toString(), id);
        QTRY_COMPARE(dismiss.size(), 1);
        QCOMPARE(dismiss.first().first().toInt(), 4567);
    }
    void unknownDocumentAndRejectedCreationNeverReportSuccess() {
        const QString absent = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto opened = request("POST", "/v1/documents/" + absent + "/open");
        QCOMPARE(opened.value("error").toObject().value("code").toString(), QString("NATIVE_DOCUMENT_MISSING"));
        QCOMPARE(object("fakeNavigator")->property("openCalls").toInt(), 0);
        object("fakeLibraryController")->setProperty("rejectCreation", true);
        const auto created = request("POST", "/v1/notebooks", {{"displayName", "Notes"}, {"idempotencyKey", "event:reject"}});
        QCOMPARE(created.value("error").toObject().value("code").toString(), QString("NATIVE_CREATE_NOT_CONFIRMED"));
        QVERIFY(!created.contains("documentId"));
    }
    void unavailableLibraryDoesNotInvokeNativeMethods() {
        object("fakeLibrary")->setProperty("isReady", false);
        QTRY_VERIFY(!adapter->enabled());
        const auto capabilities = request("GET", "/v1/capabilities");
        QCOMPARE(capabilities.value("document.createNotebook").toString(), QString("unavailable"));
        const auto result = request("POST", "/v1/notebooks", {{"displayName", "Notes"}, {"idempotencyKey", "event:disabled"}});
        QCOMPARE(result.value("error").toObject().value("code").toString(), QString("NATIVE_LIBRARY_NOT_READY"));
        QCOMPARE(object("fakeLibraryController")->property("createCalls").toInt(), 0);
    }
};
QTEST_MAIN(NativeDocumentsAdapterTest)
#include "NativeDocumentsAdapterTest.moc"

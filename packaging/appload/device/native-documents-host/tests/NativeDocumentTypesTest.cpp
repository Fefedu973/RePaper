#include "NativeDocumentHost.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QJSEngine>
#include <QJSValue>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

// These separate metatypes reproduce the firmware's QML-facing signatures.
// They do not implement or load Xochitl's storage code. The real firmware's
// notebook window proves the two individual conversions by assigning its
// EntityId argument to a string property before calling createDocument(entry::Id,...).
namespace xofm::libs::navigation {
class EntityId {
    Q_GADGET
    Q_CLASSINFO("QML.Element", "entityId")
public:
    QString text;
};
}
namespace entry {
class Id {
    Q_GADGET
public:
    QString text;
    Q_INVOKABLE QString toString() const { return text; }
};
}
Q_DECLARE_METATYPE(xofm::libs::navigation::EntityId)
Q_DECLARE_METATYPE(entry::Id)

class TypedDocument : public QObject {
    Q_OBJECT
    Q_PROPERTY(entry::Id id READ id CONSTANT)
    Q_PROPERTY(int lastOpenedPage READ lastOpenedPage CONSTANT)
public:
    entry::Id key;
    explicit TypedDocument(const entry::Id &id, QObject *parent) : QObject(parent), key(id) {}
    entry::Id id() const { return key; }
    int lastOpenedPage() const { return 0; }
};

class TypedLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isReady READ isReady CONSTANT)
public:
    QHash<QString, TypedDocument *> documents;
    bool isReady() const { return true; }
    Q_INVOKABLE QObject *entryForId(entry::Id id) { return documents.value(id.text, nullptr); }
};

class TypedExplorer : public QObject {
    Q_OBJECT
    Q_PROPERTY(xofm::libs::navigation::EntityId currentFolderId READ currentFolderId CONSTANT)
public:
    xofm::libs::navigation::EntityId currentFolderId() const { return {"root"}; }
};

class TypedLibraryController : public QObject {
    Q_OBJECT
public:
    explicit TypedLibraryController(TypedLibrary *library) : library(library) {}
    TypedLibrary *library;
    int calls = 0, orientationCalls = 0, coverCalls = 0;
    entry::Id receivedParent, receivedDocument;
    QString receivedPage;
    // Same types and defaulted overloads as ferrari 3.28.0.169 metadata.
    Q_INVOKABLE entry::Id createDocument(entry::Id currentDirectoryId, QString visibleName,
                                          entry::Id uuid = {}, QString uuid_page = {}) {
        Q_UNUSED(visibleName)
        ++calls;
        receivedParent = currentDirectoryId;
        if (uuid.text.isEmpty()) uuid.text = QUuid::createUuid().toString(QUuid::WithoutBraces);
        receivedDocument = uuid;
        receivedPage = uuid_page;
        library->documents[uuid.text] = new TypedDocument(uuid, library);
        return uuid;
    }
    Q_INVOKABLE void setOrientation(entry::Id id, Qt::Orientation orientation) {
        Q_UNUSED(id)
        if (orientation == Qt::Vertical) ++orientationCalls;
    }
    Q_INVOKABLE void setCoverPageNumber(entry::Id id, int page) {
        Q_UNUSED(id)
        if (page == -1) ++coverCalls;
    }
};

class TypedDocumentController : public QObject {
    Q_OBJECT
public:
    int templateCalls = 0;
    Q_INVOKABLE void setTemplateForPage(entry::Id id, int page, QString templateName, QJSValue paperSize) {
        Q_UNUSED(id)
        Q_UNUSED(page)
        if (templateName == "Blank" && !paperSize.isUndefined()) ++templateCalls;
    }
};

class NativeDocumentTypesTest : public QObject {
    Q_OBJECT
    const QString uuid = "466e5445-f12c-45a3-a5d2-f43c59ae4c4a";
    const QString pageUuid = "6b8c2fcb-bd60-4acb-aa86-deec9436a205";
    QJsonObject request(const QString &socketPath, const QJsonObject &body) {
        QLocalSocket socket;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(3000);
        QByteArray response;
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::readyRead, &loop, [&] { response += socket.readAll(); });
        connect(&socket, &QLocalSocket::connected, &loop, [&] {
            const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
            socket.write("POST /v1/notebooks HTTP/1.1\r\nHost: local\r\nContent-Length: "
                         + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
        });
        socket.connectToServer(socketPath);
        timeout.start();
        loop.exec();
        response += socket.readAll();
        const auto offset = response.indexOf("\r\n\r\n");
        return offset < 0 ? QJsonObject{{"testFailure", "No HTTP response"}}
                          : QJsonDocument::fromJson(response.mid(offset + 4)).object();
    }
private slots:
    void initTestCase() {
        qRegisterMetaType<xofm::libs::navigation::EntityId>();
        qRegisterMetaType<entry::Id>();
        QVERIFY((QMetaType::registerConverter<xofm::libs::navigation::EntityId, QString>(
            [](const xofm::libs::navigation::EntityId &id) { return id.text; })));
        QVERIFY((QMetaType::registerConverter<QString, entry::Id>(
            [](const QString &text) { return entry::Id{text}; })));
        QVERIFY((QMetaType::registerConverter<entry::Id, QString>(
            [](const entry::Id &id) { return id.text; })));
        qmlRegisterType<NativeDocumentHost>("net.asivery.AppLoad", 1, 0, "NativeDocumentHost");
        qmlRegisterType(QUrl("qrc:/appload/qml/RePaperNativeDocuments.qml"), "net.asivery.AppLoad", 1, 0,
                        "RePaperNativeDocuments");
    }

    void baselineEntityIdArgumentFailsButStringNormalizationSucceeds() {
        TypedLibrary library;
        TypedExplorer explorer;
        TypedLibraryController controller(&library);
        QJSEngine engine;
        QJSEngine::setObjectOwnership(&explorer, QJSEngine::CppOwnership);
        QJSEngine::setObjectOwnership(&controller, QJSEngine::CppOwnership);
        engine.globalObject().setProperty("explorer", engine.newQObject(&explorer));
        engine.globalObject().setProperty("controller", engine.newQObject(&controller));
        engine.globalObject().setProperty("documentId", uuid);
        engine.globalObject().setProperty("pageId", pageUuid);
        const auto direct = engine.evaluate("controller.createDocument(explorer.currentFolderId, 'Notes', documentId, pageId)");
        QVERIFY2(direct.isError(), "The old call must reproduce a real Qt metatype-conversion failure.");
        qInfo().noquote() << "Baseline direct EntityId -> entry::Id:" << direct.toString();
        QCOMPARE(controller.calls, 0);
        const auto normalized = engine.evaluate("controller.createDocument(explorer.currentFolderId.toString(), 'Notes', documentId, pageId)");
        QVERIFY2(!normalized.isError(), qPrintable(normalized.toString()));
        QCOMPARE(controller.calls, 1);
        QCOMPARE(controller.receivedParent.text, QString("root"));
        QCOMPARE(controller.receivedDocument.text, uuid);
        QCOMPARE(controller.receivedPage, pageUuid);
        qInfo().noquote() << "Normalized EntityId -> QString -> entry::Id: succeeded";
    }

    void firmwareTwoArgumentStringPropertyFlowAlsoSucceeds() {
        TypedLibrary library;
        TypedExplorer explorer;
        TypedLibraryController controller(&library);
        QJSEngine engine;
        QJSEngine::setObjectOwnership(&explorer, QJSEngine::CppOwnership);
        QJSEngine::setObjectOwnership(&controller, QJSEngine::CppOwnership);
        engine.globalObject().setProperty("explorer", engine.newQObject(&explorer));
        engine.globalObject().setProperty("controller", engine.newQObject(&controller));
        const auto result = engine.evaluate("controller.createDocument(explorer.currentFolderId.toString(), 'Notes')");
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QCOMPARE(controller.calls, 1);
        QCOMPARE(controller.receivedParent.text, QString("root"));
    }

    void shippedAdapterAcceptsTypedFolderAndRetainsNotebookIdentity() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TypedLibrary library;
        TypedExplorer explorer;
        TypedLibraryController controller(&library);
        TypedDocumentController documentController;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("typedLibrary", &library);
        engine.rootContext()->setContextProperty("typedExplorer", &explorer);
        engine.rootContext()->setContextProperty("typedLibraryController", &controller);
        engine.rootContext()->setContextProperty("typedDocumentController", &documentController);
        const auto socketPath = directory.filePath("run/documents.sock");
        engine.rootContext()->setContextProperty("testSocketPath", socketPath);
        engine.rootContext()->setContextProperty("testStateDirectory", directory.filePath("state"));
        engine.load(QUrl("qrc:/native-documents-tests/NativeTypesFixture.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *adapter = qobject_cast<NativeDocumentHost *>(engine.rootObjects().first());
        QVERIFY(adapter && adapter->enabled());
        const QJsonObject body{{"displayName", "Typed native note"}, {"idempotencyKey", "event:typed"}};
        const auto result = request(socketPath, body);
        QVERIFY2(result["status"] == "succeeded", QJsonDocument(result).toJson().constData());
        QCOMPARE(controller.calls, 1);
        QCOMPARE(controller.receivedParent.text, QString("root"));
        QCOMPARE(controller.orientationCalls, 1);
        QCOMPARE(controller.coverCalls, 1);
        QCOMPARE(documentController.templateCalls, 1);
        const auto repeated = request(socketPath, body);
        QCOMPARE(repeated["documentId"], result["documentId"]);
        QCOMPARE(controller.calls, 1);
        QCOMPARE(documentController.templateCalls, 1);
    }
};
QTEST_MAIN(NativeDocumentTypesTest)
#include "NativeDocumentTypesTest.moc"

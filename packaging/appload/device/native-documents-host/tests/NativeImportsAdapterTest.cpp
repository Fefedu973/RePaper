#include "NativeDocumentHost.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJSEngine>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <functional>

// Reproduce the firmware's distinct metatypes and public importer signature.
// This is a typed protocol fixture, not Xochitl's actual implementation.
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

class ImportDocument : public QObject {
    Q_OBJECT
    Q_PROPERTY(entry::Id id READ id CONSTANT)
    Q_PROPERTY(QString visibleName READ visibleName WRITE setVisibleName)
public:
    ImportDocument(QString id, QString name, QObject *parent)
        : QObject(parent), key{std::move(id)}, name(std::move(name)) {}
    entry::Id key;
    QString name;
    entry::Id id() const { return key; }
    QString visibleName() const { return name; }
    void setVisibleName(const QString &value) { name = value; }
};

class ImportLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isReady READ isReady CONSTANT)
public:
    QHash<QString, ImportDocument *> documents;
    bool isReady() const { return true; }
    Q_INVOKABLE QObject *entryForId(entry::Id id) { return documents.value(id.text, nullptr); }
    ImportDocument *add(const QString &id, const QString &name) {
        auto *document = new ImportDocument(id, name, this);
        documents[id] = document;
        return document;
    }
signals:
    void entryImported(const QString &visibleName, entry::Id id);
};

class ImportExplorer : public QObject {
    Q_OBJECT
    Q_PROPERTY(xofm::libs::navigation::EntityId currentFolderId READ currentFolderId CONSTANT)
public:
    xofm::libs::navigation::EntityId currentFolderId() const { return {"root"}; }
};

class ImportLibraryController : public QObject {
    Q_OBJECT
public:
    explicit ImportLibraryController(ImportLibrary *library) : library(library) {}
    ImportLibrary *library;
    int renameCalls = 0;
    QString renamedId;
    bool allowRename = true;
    Q_INVOKABLE bool setVisibleName(entry::Id id, const QString &name) {
        ++renameCalls;
        renamedId = id.text;
        const auto document = library->documents.value(id.text);
        if (!allowRename || !document) return false;
        document->name = name;
        return true;
    }
    Q_INVOKABLE entry::Id createDocument(entry::Id, const QString &) { return {}; }
};

class TypedImporter : public QObject {
    Q_OBJECT
public:
    int calls = 0;
    QList<QUrl> urls;
    entry::Id folder;
    std::function<void()> onImport;
    Q_INVOKABLE bool importFromUrls(QList<QUrl> sourceUrls, entry::Id parentId) {
        ++calls;
        urls = sourceUrls;
        folder = parentId;
        if (onImport) onImport();
        return true;
    }
signals:
    void failed(QUrl sourceUrl);
    void finished(QUrl sourceUrl);
};

// The production QML remains unchanged; only this test registration supplies
// temporary roots through the existing C++-only injection points.
class ImportFixtureHost : public NativeDocumentHost {
    Q_OBJECT
public:
    inline static QString inputRoot, metadataRoot;
    using NativeDocumentHost::NativeDocumentHost;
    void componentComplete() override {
        setInputRoot(inputRoot);
        setMetadataRoot(metadataRoot);
        NativeDocumentHost::componentComplete();
    }
};

class NativeImportsAdapterTest : public QObject {
    Q_OBJECT
    const QString documentId = "36201668-71a0-4e91-aac4-d5669e344252";
    const QString decoyId = "1e9dc85d-4f02-4e6a-a4b6-dc5d29fe8302";
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<ImportLibrary> library;
    std::unique_ptr<ImportLibraryController> controller;
    std::unique_ptr<ImportExplorer> explorer;
    std::unique_ptr<TypedImporter> importer;
    std::unique_ptr<QQmlApplicationEngine> engine;
    NativeDocumentHost *host = nullptr;
    QString socketPath;

    void loadEngine() {
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty("typedLibrary", library.get());
        engine->rootContext()->setContextProperty("typedLibraryController", controller.get());
        engine->rootContext()->setContextProperty("typedExplorer", explorer.get());
        engine->rootContext()->setContextProperty("typedImporter", importer.get());
        engine->rootContext()->setContextProperty("testSocketPath", socketPath);
        engine->rootContext()->setContextProperty("testStateDirectory", directory->filePath("state"));
        engine->load(QUrl("qrc:/native-documents-tests/ImportTypesFixture.qml"));
        QVERIFY(!engine->rootObjects().isEmpty());
        host = qobject_cast<NativeDocumentHost *>(engine->rootObjects().first());
        QVERIFY(host && host->enabled() && host->importsEnabled());
    }
    QJsonObject request(const QJsonObject &body, int timeoutMs = 3000) {
        QLocalSocket socket;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(timeoutMs);
        QByteArray response;
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::readyRead, &loop, [&] { response += socket.readAll(); });
        connect(&socket, &QLocalSocket::connected, &loop, [&] {
            const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
            socket.write("POST /v1/imports HTTP/1.1\r\nHost: local\r\nContent-Length: "
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
    QJsonObject body(const QString &key = "moodle:typed") {
        const QByteArray bytes = "%PDF-1.4\n% typed importer protocol fixture\n";
        const auto path = directory->filePath("inputs/prepared.pdf");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return {};
        return {{"path", path}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
                {"displayName", "Cours importé"}, {"idempotencyKey", key}};
    }
    QString token() const { return QFileInfo(importer->urls.first().toLocalFile()).completeBaseName(); }
    void writeMetadata(const QString &id, const QString &name) {
        QFile metadata(directory->filePath("metadata/" + id + ".metadata"));
        QVERIFY(metadata.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(QJsonObject{{"visibleName", name}, {"type", "DocumentType"}}).toJson();
        QCOMPARE(metadata.write(bytes), qint64(bytes.size()));
    }
    bool verifySuccess(const QJsonObject &result) {
        const bool success = result["status"] == "succeeded" && result["documentId"].toString() == documentId
            && result["nativeIndexVerified"].toBool();
        if (!success) qWarning().noquote() << QJsonDocument(result).toJson();
        return success;
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
        qmlRegisterType<ImportFixtureHost>("net.asivery.AppLoad", 1, 0, "NativeDocumentHost");
        qmlRegisterType(QUrl("qrc:/appload/qml/RePaperNativeDocuments.qml"), "net.asivery.AppLoad", 1, 0,
                        "RePaperNativeDocuments");
    }
    void init() {
        directory = std::make_unique<QTemporaryDir>();
        QVERIFY(directory->isValid());
        QVERIFY(QDir().mkpath(directory->filePath("inputs")));
        QVERIFY(QDir().mkpath(directory->filePath("metadata")));
        ImportFixtureHost::inputRoot = directory->filePath("inputs");
        ImportFixtureHost::metadataRoot = directory->filePath("metadata");
        socketPath = directory->filePath("run/documents.sock");
        library = std::make_unique<ImportLibrary>();
        controller = std::make_unique<ImportLibraryController>(library.get());
        explorer = std::make_unique<ImportExplorer>();
        importer = std::make_unique<TypedImporter>();
        loadEngine();
        QVERIFY(host);
    }
    void cleanup() {
        engine.reset(); host = nullptr;
        importer.reset(); explorer.reset(); controller.reset(); library.reset(); directory.reset();
    }
    void rawEntityIdFailsAndNormalizedUrlListSucceeds() {
        QJSEngine js;
        QJSEngine::setObjectOwnership(explorer.get(), QJSEngine::CppOwnership);
        QJSEngine::setObjectOwnership(importer.get(), QJSEngine::CppOwnership);
        js.globalObject().setProperty("explorer", js.newQObject(explorer.get()));
        js.globalObject().setProperty("importer", js.newQObject(importer.get()));
        const auto raw = js.evaluate("importer.importFromUrls(['file:///private/test.pdf'], explorer.currentFolderId)");
        QVERIFY(raw.isError());
        QCOMPARE(importer->calls, 0);
        const auto normalized = js.evaluate("importer.importFromUrls(['file:///private/test.pdf'], explorer.currentFolderId.toString())");
        QVERIFY2(!normalized.isError(), qPrintable(normalized.toString()));
        QCOMPARE(importer->calls, 1);
        QCOMPARE(importer->folder.text, QString("root"));
        QCOMPARE(importer->urls, QList<QUrl>{QUrl("file:///private/test.pdf")});
    }
    void typedImportAndCompletedRetryPreserveDocumentAndUserRename() {
        importer->onImport = [this] {
            library->add(documentId, token());
            emit library->entryImported(token(), {documentId});
        };
        const auto source = body();
        QVERIFY(verifySuccess(request(source)));
        QCOMPARE(importer->calls, 1);
        QCOMPARE(importer->urls.size(), 1);
        QVERIFY(importer->urls.first().isLocalFile());
        QCOMPARE(importer->folder.text, QString("root"));
        QCOMPARE(controller->renamedId, documentId);
        QCOMPARE(controller->renameCalls, 1);
        QCOMPARE(library->documents[documentId]->name, QString("Cours importé"));
        library->documents[documentId]->name = "Titre personnel";
        QVERIFY(verifySuccess(request(source)));
        QCOMPARE(importer->calls, 1);
        QCOMPARE(controller->renameCalls, 1);
        QCOMPARE(library->documents[documentId]->name, QString("Titre personnel"));
    }
    void unrelatedSignalsAreIgnoredAndDelayedNativeIndexIsRequired() {
        importer->onImport = [this] {
            library->add(decoyId, "Autre document");
            emit library->entryImported("other-" + token(), {decoyId});
            emit importer->failed(QUrl::fromLocalFile("/unrelated/" + token() + ".pdf"));
            emit importer->finished(QUrl::fromLocalFile("/unrelated/" + token() + ".pdf"));
            emit library->entryImported(token() + ".pdf", {documentId});
            QTimer::singleShot(250, library.get(), [this] { library->add(documentId, token() + ".pdf"); });
        };
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(verifySuccess(request(body())));
        QVERIFY(elapsed.elapsed() >= 200);
        QCOMPARE(importer->calls, 1);
        QCOMPARE(controller->renameCalls, 1);
        QCOMPARE(controller->renamedId, documentId);
        QCOMPARE(library->documents[decoyId]->name, QString("Autre document"));
    }
    void finishedSignalReconcilesMetadataWhenEntrySignalWasMissed() {
        importer->onImport = [this] {
            QTimer::singleShot(40, library.get(), [this] { writeMetadata(documentId, token()); });
            QTimer::singleShot(150, library.get(), [this] { library->add(documentId, token()); });
            emit importer->finished(importer->urls.first());
        };
        QVERIFY(verifySuccess(request(body())));
        QCOMPARE(importer->calls, 1);
        QCOMPARE(controller->renameCalls, 1);
    }
    void lostResponseAndHostRestartRecoverWithoutAnotherImport() {
        const auto source = body();
        QVERIFY(request(source, 70).contains("testFailure"));
        QCOMPARE(importer->calls, 1);
        const auto originalUrl = importer->urls.first();
        const auto originalToken = token();
        engine.reset(); host = nullptr;
        writeMetadata(documentId, originalToken);
        library->add(documentId, originalToken);
        loadEngine();
        QVERIFY(verifySuccess(request(source)));
        QCOMPARE(importer->calls, 1);
        QCOMPARE(importer->urls.first(), originalUrl);
        QCOMPARE(controller->renameCalls, 1);
    }
    void uncertainRestartDoesNotResubmitOrClaimUnrelatedMetadata() {
        const auto source = body();
        QVERIFY(request(source, 70).contains("testFailure"));
        QCOMPARE(importer->calls, 1);
        const auto originalToken = token();
        engine.reset(); host = nullptr;
        writeMetadata(decoyId, "prefix-" + originalToken);
        library->add(decoyId, "prefix-" + originalToken);
        loadEngine();
        const auto result = request(source);
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_IMPORT_UNCERTAIN"));
        QVERIFY(!result.contains("documentId"));
        QCOMPARE(importer->calls, 1);
        QCOMPARE(controller->renameCalls, 0);
    }
    void correlatedFailureAllowsOneExplicitRetry() {
        importer->onImport = [this] { emit importer->failed(importer->urls.first()); };
        const auto source = body();
        const auto failed = request(source);
        QCOMPARE(failed["error"].toObject()["code"].toString(), QString("NATIVE_IMPORT_FAILED"));
        QCOMPARE(importer->calls, 1);
        importer->onImport = [this] {
            library->add(documentId, token());
            emit library->entryImported(token(), {documentId});
        };
        QVERIFY(verifySuccess(request(source)));
        QCOMPARE(importer->calls, 2);
        QCOMPARE(controller->renameCalls, 1);
    }
    void renameFailureRetainsNativeIdentityForRetry() {
        controller->allowRename = false;
        importer->onImport = [this] {
            library->add(documentId, token());
            emit library->entryImported(token(), {documentId});
        };
        const auto source = body();
        const auto failed = request(source);
        QCOMPARE(failed["error"].toObject()["code"].toString(), QString("NATIVE_IMPORT_NAME_FAILED"));
        controller->allowRename = true;
        QVERIFY(verifySuccess(request(source)));
        QCOMPARE(importer->calls, 1);
        QCOMPARE(controller->renameCalls, 2);
    }
};
QTEST_MAIN(NativeImportsAdapterTest)
#include "NativeImportsAdapterTest.moc"

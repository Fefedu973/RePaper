#include "NativeDocumentHost.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QJSEngine>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

// Reproduce the distinct firmware metatypes, including QList<entry::Id>.
// Fixtures deliberately implement storage only in memory: no native files or
// Xochitl services are opened by these tests.
namespace entry {
class Id {
    Q_GADGET
public:
    QString text;
    Q_INVOKABLE QString toString() const { return text; }
};
}
namespace xofm::libs::navigation {
class EntityId {
    Q_GADGET
    Q_CLASSINFO("QML.Element", "entityId")
public:
    QString text;
};
}
Q_DECLARE_METATYPE(entry::Id)
Q_DECLARE_METATYPE(QList<entry::Id>)
Q_DECLARE_METATYPE(xofm::libs::navigation::EntityId)

class FolderEntry : public QObject {
    Q_OBJECT
    Q_PROPERTY(entry::Id id READ id CONSTANT)
    Q_PROPERTY(entry::Id parentId READ parentId)
    Q_PROPERTY(int lastOpenedPage READ lastOpenedPage CONSTANT)
    Q_PROPERTY(bool isTrashed READ isTrashed)
    Q_PROPERTY(QString visibleName READ visibleName)
    Q_PROPERTY(Qt::Orientation orientation READ orientation)
public:
    explicit FolderEntry(QString key, QString parent, QObject *owner)
        : QObject(owner), key(std::move(key)), parent(std::move(parent)) {}
    QString key, parent, name, templateName = "Blank";
    Qt::Orientation pageOrientation = Qt::Vertical;
    QByteArray userInk;
    bool trashed = false;
    entry::Id id() const { return {key}; }
    entry::Id parentId() const { return {parent}; }
    int lastOpenedPage() const { return 0; }
    bool isTrashed() const { return trashed; }
    QString visibleName() const { return name; }
    Qt::Orientation orientation() const { return pageOrientation; }
    Q_INVOKABLE QString templateForPage(int) const { return templateName; }
};

class FolderLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isReady READ isReady CONSTANT)
public:
    // This UUID was verified in the firmware initializer. Production code must
    // obtain it through meetingNotesId(), not duplicate the literal.
    const QString meetingId = "94db1b5c-7599-5db2-8052-83077bc87711";
    QHash<QString, FolderEntry *> entries;
    int meetingCalls = 0, folderCreations = 0;
    bool missingFolderResult = false, emptyFolderResult = false, trashedFolder = false;
    bool isReady() const { return true; }
    Q_INVOKABLE entry::Id meetingNotesId() {
        ++meetingCalls;
        if (emptyFolderResult) return {};
        if (!entries.contains(meetingId) && !missingFolderResult) {
            auto *folder = new FolderEntry(meetingId, "root", this);
            folder->name = "Meeting Notes";
            folder->trashed = trashedFolder;
            entries.insert(meetingId, folder);
            ++folderCreations;
        }
        return {meetingId};
    }
    Q_INVOKABLE QObject *entryForId(entry::Id id) { return entries.value(id.text); }
    Q_INVOKABLE entry::Id parentIdForId(entry::Id id) {
        const auto *entry = entries.value(id.text);
        return {entry ? entry->parent : QString()};
    }
};

class FolderExplorer : public QObject {
    Q_OBJECT
    Q_PROPERTY(xofm::libs::navigation::EntityId currentFolderId READ currentFolderId CONSTANT)
public:
    xofm::libs::navigation::EntityId currentFolderId() const { return {"unrelated-current-folder"}; }
};

class FolderLibraryController : public QObject {
    Q_OBJECT
public:
    explicit FolderLibraryController(FolderLibrary *library) : library(library) {}
    FolderLibrary *library;
    int createCalls = 0, moveCalls = 0, orientationCalls = 0, coverCalls = 0;
    bool rejectMove = false, ignoreMove = false;
    QString lastParent, lastCreated, lastPage;
    QList<entry::Id> lastMoved;
    Q_INVOKABLE entry::Id createDocument(entry::Id parent, QString title, entry::Id uuid = {}, QString page = {}) {
        ++createCalls;
        lastParent = parent.text;
        lastCreated = uuid.text;
        lastPage = page;
        auto *document = new FolderEntry(uuid.text, parent.text, library);
        document->name = title;
        library->entries.insert(uuid.text, document);
        return uuid;
    }
    Q_INVOKABLE bool moveEntries(QList<entry::Id> ids, entry::Id folder) {
        ++moveCalls;
        lastMoved = ids;
        if (rejectMove) return false;
        if (!ignoreMove) for (const auto &id : ids) {
            auto *entry = library->entries.value(id.text);
            if (!entry) return false;
            entry->parent = folder.text;
        }
        return true;
    }
    Q_INVOKABLE bool setOrientation(entry::Id id, Qt::Orientation orientation) {
        ++orientationCalls;
        if (auto *entry = library->entries.value(id.text)) entry->pageOrientation = orientation;
        return true;
    }
    Q_INVOKABLE bool setCoverPageNumber(entry::Id, int) { ++coverCalls; return true; }
};

class FolderDocumentController : public QObject {
    Q_OBJECT
public:
    FolderLibrary *library = nullptr;
    int templateCalls = 0;
    QString lastTemplate;
    Q_INVOKABLE void setTemplateForPage(entry::Id id, int, QString name, QJSValue) {
        ++templateCalls;
        lastTemplate = name;
        if (library) if (auto *entry = library->entries.value(id.text)) entry->templateName = name;
    }
};

class NativeAgendaFolderTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<QQmlApplicationEngine> engine;
    std::unique_ptr<FolderLibrary> library;
    std::unique_ptr<FolderLibraryController> controller;
    FolderExplorer explorer;
    FolderDocumentController documentController;
    QString socketPath;
    NativeDocumentHost *adapter = nullptr;

    QJsonObject agenda() const {
        return {{"schemaVersion", 1}, {"kind", "event"}, {"date", "2026-09-07"},
            {"timeZone", "Europe/Paris"}, {"event", QJsonObject{{"id", "source:event"},
                {"title", "Cours de mathématiques"}, {"subject", "Mathématiques"},
                {"start", "2026-09-07T09:00:00+02:00"}, {"end", "2026-09-07T10:30:00+02:00"},
                {"timeZone", "Europe/Paris"}, {"allDay", false}}}};
    }
    QJsonObject body(const QString &key, bool withAgenda = true) const {
        QJsonObject result{{"displayName", "Notes de cours"}, {"idempotencyKey", "reagenda:event:" + key}};
        if (withAgenda) {
            auto context = agenda();
            auto event = context["event"].toObject();
            event["id"] = key;
            context["event"] = event;
            result["agenda"] = context;
        }
        return result;
    }
    QJsonObject request(const QString &route, const QJsonObject &body) {
        QLocalSocket socket;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QByteArray response;
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::readyRead, &loop, [&] { response += socket.readAll(); });
        connect(&socket, &QLocalSocket::connected, &loop, [&] {
            const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
            socket.write("POST " + route.toUtf8() + " HTTP/1.1\r\nHost: local\r\nContent-Length: "
                + QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
        });
        socket.connectToServer(socketPath);
        timer.start(3000);
        loop.exec();
        response += socket.readAll();
        const int separator = response.indexOf("\r\n\r\n");
        return separator < 0 ? QJsonObject{{"testFailure", "No HTTP response"}}
            : QJsonDocument::fromJson(response.mid(separator + 4)).object();
    }
    void startAdapter() {
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty("typedLibrary", library.get());
        engine->rootContext()->setContextProperty("typedLibraryController", controller.get());
        engine->rootContext()->setContextProperty("typedDocumentController", &documentController);
        engine->rootContext()->setContextProperty("typedExplorer", &explorer);
        engine->rootContext()->setContextProperty("testSocketPath", socketPath);
        engine->rootContext()->setContextProperty("testStateDirectory", directory->filePath("state"));
        engine->load(QUrl("qrc:/native-documents-tests/FolderTypesFixture.qml"));
        QVERIFY(!engine->rootObjects().isEmpty());
        adapter = qobject_cast<NativeDocumentHost *>(engine->rootObjects().first());
        QVERIFY(adapter && adapter->enabled());
    }
private slots:
    void initTestCase() {
        qRegisterMetaType<entry::Id>();
        qRegisterMetaType<QList<entry::Id>>();
        qRegisterMetaType<xofm::libs::navigation::EntityId>();
        QVERIFY((QMetaType::registerConverter<QString, entry::Id>([](const QString &s) { return entry::Id{s}; })));
        QVERIFY((QMetaType::registerConverter<entry::Id, QString>([](const entry::Id &id) { return id.text; })));
        QVERIFY((QMetaType::registerConverter<xofm::libs::navigation::EntityId, QString>(
            [](const xofm::libs::navigation::EntityId &id) { return id.text; })));
        qmlRegisterType<NativeDocumentHost>("net.asivery.AppLoad", 1, 0, "NativeDocumentHost");
        qmlRegisterType(QUrl("qrc:/appload/qml/RePaperNativeDocuments.qml"), "net.asivery.AppLoad", 1, 0,
            "RePaperNativeDocuments");
    }
    void init() {
        directory = std::make_unique<QTemporaryDir>();
        QVERIFY(directory->isValid());
        socketPath = directory->filePath("run/documents.sock");
        library = std::make_unique<FolderLibrary>();
        controller = std::make_unique<FolderLibraryController>(library.get());
        documentController.library = library.get();
        documentController.templateCalls = 0;
        documentController.lastTemplate.clear();
        startAdapter();
    }
    void cleanup() {
        engine.reset(); adapter = nullptr;
        controller.reset(); library.reset(); directory.reset();
    }
    void typedJavascriptArrayConvertsToNativeIdList() {
        QJSEngine js;
        QJSEngine::setObjectOwnership(controller.get(), QJSEngine::CppOwnership);
        js.globalObject().setProperty("controller", js.newQObject(controller.get()));
        const QString id = "466e5445-f12c-45a3-a5d2-f43c59ae4c4a";
        library->entries[id] = new FolderEntry(id, "root", library.get());
        QJSEngine::setObjectOwnership(library->entries[id], QJSEngine::CppOwnership);
        js.globalObject().setProperty("noteEntry", js.newQObject(library->entries[id]));
        js.globalObject().setProperty("note", id);
        js.globalObject().setProperty("folder", library->meetingId);
        const auto baseline = js.evaluate("controller.moveEntries([note], folder)");
        qInfo() << "String-array baseline" << baseline.toString() << "received elements" << controller->lastMoved.size()
                << "first value" << (controller->lastMoved.isEmpty() ? QString() : controller->lastMoved.first().text);
        library->entries[id]->parent = "root";
        const auto result = js.evaluate("controller.moveEntries([noteEntry.id], folder)");
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QVERIFY(result.toBool());
        QCOMPARE(controller->lastMoved.size(), 1);
        QCOMPARE(controller->lastMoved.first().text, id);
        QCOMPARE(library->entries[id]->parent, library->meetingId);
    }
    void createsAgendaInsideStableNativeFolder() {
        const auto result = request("/v1/notebooks", body("agenda:new"));
        QVERIFY2(result["status"] == "succeeded", QJsonDocument(result).toJson().constData());
        QCOMPARE(controller->lastParent, library->meetingId);
        QCOMPARE(library->folderCreations, 1);
        QCOMPARE(controller->createCalls, 1);
        QCOMPARE(controller->moveCalls, 0);
        QVERIFY(!QUuid(controller->lastPage).isNull());
        auto *note = library->entries.value(result["documentId"].toString());
        QVERIFY(note);
        QCOMPARE(note->templateName, QString("P Day"));
        QCOMPARE(note->pageOrientation, Qt::Vertical);
        const auto second = request("/v1/notebooks", body("agenda:another"));
        QCOMPARE(second["status"].toString(), QString("succeeded"));
        QCOMPARE(library->folderCreations, 1);
        QCOMPARE(controller->createCalls, 2);
    }
    void retryAndRestartRetainUuidAndUserContent() {
        const auto result = request("/v1/notebooks", body("agenda:retry"));
        QVERIFY2(result["status"] == "succeeded", QJsonDocument(result).toJson().constData());
        const QString id = result["documentId"].toString();
        auto *entry = library->entries.value(id);
        QVERIFY(entry);
        entry->userInk = "user-owned-strokes-and-annotations";
        entry->name = "Renamed by user";
        entry->templateName = "User selected template";
        entry->pageOrientation = Qt::Vertical;
        const int templates = documentController.templateCalls;
        const int orientations = controller->orientationCalls;
        const int covers = controller->coverCalls;
        engine.reset(); adapter = nullptr;
        startAdapter();
        const auto repeated = request("/v1/notebooks", body("agenda:retry"));
        QCOMPARE(repeated["documentId"].toString(), id);
        QCOMPARE(controller->createCalls, 1);
        QCOMPARE(library->folderCreations, 1);
        QCOMPARE(documentController.templateCalls, templates);
        QCOMPARE(controller->orientationCalls, orientations);
        QCOMPARE(controller->coverCalls, covers);
        QCOMPARE(entry->userInk, QByteArray("user-owned-strokes-and-annotations"));
        QCOMPARE(entry->name, QString("Renamed by user"));
        QCOMPARE(entry->templateName, QString("User selected template"));
        QCOMPARE(entry->pageOrientation, Qt::Vertical);
    }
    void legacyNoteMovesOnceWithoutReformatting() {
        const auto legacy = request("/v1/notebooks", body("agenda:legacy", false));
        QVERIFY2(legacy["status"] == "succeeded", QJsonDocument(legacy).toJson().constData());
        const QString id = legacy["documentId"].toString();
        auto *entry = library->entries.value(id);
        QVERIFY(entry);
        entry->userInk = "legacy-user-ink";
        const int templates = documentController.templateCalls;
        const auto adopted = request("/v1/notebooks", body("agenda:legacy"));
        QCOMPARE(adopted["documentId"].toString(), id);
        QCOMPARE(entry->parent, library->meetingId);
        QCOMPARE(controller->moveCalls, 1);
        QCOMPARE(controller->lastMoved.size(), 1);
        QCOMPARE(controller->lastMoved.first().text, id);
        QCOMPARE(controller->createCalls, 1);
        QCOMPARE(documentController.templateCalls, templates);
        QCOMPARE(entry->userInk, QByteArray("legacy-user-ink"));
        QCOMPARE(request("/v1/notebooks", body("agenda:legacy"))["documentId"].toString(), id);
        QCOMPARE(controller->moveCalls, 1);
    }
    void failedOrUnconfirmedMoveDoesNotClaimSuccess_data() {
        QTest::addColumn<bool>("reject");
        QTest::newRow("native-rejects") << true;
        QTest::newRow("native-no-parent-change") << false;
    }
    void failedOrUnconfirmedMoveDoesNotClaimSuccess() {
        QFETCH(bool, reject);
        const auto legacy = request("/v1/notebooks", body("agenda:move-fail", false));
        const QString id = legacy["documentId"].toString();
        auto *entry = library->entries.value(id);
        QVERIFY(entry);
        entry->userInk = "keep-ink";
        controller->rejectMove = reject; controller->ignoreMove = !reject;
        const auto result = request("/v1/notebooks", body("agenda:move-fail"));
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_AGENDA_MOVE_FAILED"));
        QVERIFY(result["status"] != "succeeded");
        QCOMPARE(controller->moveCalls, 1);
        QCOMPARE(entry->parent, QString("unrelated-current-folder"));
        QCOMPARE(entry->userInk, QByteArray("keep-ink"));
        QCOMPARE(controller->createCalls, 1);
    }
    void deletedNativeFolderIsRecreatedThroughNativeApi() {
        const auto first = request("/v1/notebooks", body("agenda:folder-deleted"));
        QVERIFY2(first["status"] == "succeeded", QJsonDocument(first).toJson().constData());
        delete library->entries.take(library->meetingId);
        const auto second = request("/v1/notebooks", body("agenda:new-after-folder-deletion"));
        QCOMPARE(second["status"].toString(), QString("succeeded"));
        QCOMPARE(library->folderCreations, 2);
        QCOMPARE(controller->lastParent, library->meetingId);
    }
    void unavailableFolderNeverCreatesNotebook_data() {
        QTest::addColumn<int>("failure");
        QTest::newRow("empty-id") << 0;
        QTest::newRow("entry-missing") << 1;
        QTest::newRow("trashed-folder") << 2;
    }
    void unavailableFolderNeverCreatesNotebook() {
        QFETCH(int, failure);
        library->emptyFolderResult = failure == 0;
        library->missingFolderResult = failure == 1;
        library->trashedFolder = failure == 2;
        const auto result = request("/v1/notebooks", body("agenda:folder-unavailable"));
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_AGENDA_FOLDER_UNAVAILABLE"));
        QCOMPARE(controller->createCalls, 0);
    }
    void trashedLegacyNoteIsNotMovedOrResurrected() {
        const auto legacy = request("/v1/notebooks", body("agenda:trashed", false));
        auto *entry = library->entries.value(legacy["documentId"].toString());
        QVERIFY(entry);
        entry->trashed = true;
        entry->parent = "trash";
        entry->userInk = "trashed-user-content";
        const auto result = request("/v1/notebooks", body("agenda:trashed"));
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_AGENDA_NOTE_TRASHED"));
        QCOMPARE(controller->moveCalls, 0);
        QCOMPARE(controller->createCalls, 1);
        QCOMPARE(entry->parent, QString("trash"));
        QCOMPARE(entry->userInk, QByteArray("trashed-user-content"));
    }
    void ordinaryNotebookKeepsCurrentFolderBehavior() {
        const auto result = request("/v1/notebooks", body("ordinary-note", false));
        QVERIFY2(result["status"] == "succeeded", QJsonDocument(result).toJson().constData());
        QCOMPARE(controller->lastParent, QString("unrelated-current-folder"));
        QCOMPARE(library->meetingCalls, 0);
        QCOMPARE(documentController.lastTemplate, QString("Blank"));
    }
};
QTEST_MAIN(NativeAgendaFolderTest)
#include "NativeAgendaFolderTest.moc"

#include "NativeDocumentHost.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>

class FakeAgendaPage : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
public:
    bool active = false, autoPrepared = true, autoFinished = true, finishSuccess = true;
    bool synchronous = false, allowPrepare = true, allowCommit = true;
    int prepareCalls = 0, commitCalls = 0, cancelCalls = 0, nativeInserts = 0;
    QString requestId, state = "empty", hash = QString(64, 'a'), failureCode = "SYNTHETIC_NATIVE_FAILURE";
    QString journalPath, phaseAtCommit, hashAtCommit;
    QVariantMap context, fields;
    QSizeF pageSize;
    QPointer<QObject> controller;
    std::function<void()> afterPrepare, beforeFinished;
    bool busy() const { return active; }
    QString reason() const { return "Échec synthétique du préremplissage"; }
    Q_INVOKABLE bool prepare(const QString &request, QObject *sceneController, const QVariantMap &pageContext,
                             const QVariantMap &pageFields, const QSizeF &size) {
        ++prepareCalls;
        if (!allowPrepare || active) return false;
        active = true; requestId = request; controller = sceneController;
        context = pageContext; fields = pageFields; pageSize = size;
        emit changed();
        if (afterPrepare) afterPrepare();
        if (autoPrepared) {
            if (synchronous) deliverPrepared();
            else QTimer::singleShot(0, this, [this, request] { if (active && requestId == request) deliverPrepared(); });
        }
        return true;
    }
    Q_INVOKABLE bool commit(const QString &request) {
        ++commitCalls;
        QFile file(journalPath);
        if (file.open(QIODevice::ReadOnly)) {
            const auto ledger = QJsonDocument::fromJson(file.readAll()).object();
            phaseAtCommit = ledger["agendaPageState"].toString();
            hashAtCommit = ledger["agendaPlanHash"].toString();
        }
        if (!allowCommit || !active || request != requestId) return false;
        if (state == "empty") ++nativeInserts;
        if (autoFinished) {
            if (synchronous) deliverFinished();
            else QTimer::singleShot(0, this, [this, request] { if (active && requestId == request) deliverFinished(); });
        }
        return true;
    }
    Q_INVOKABLE void cancel(const QString &request) {
        if (request != requestId) return;
        ++cancelCalls; active = false; emit changed();
    }
    void deliverPrepared() {
        emit prepared(requestId, {{"state", state}, {"planHash", hash},
            {"displayedTitle", fields["title"]}, {"strokeCount", state == "empty" ? 25 : 0}});
    }
    void deliverFinished() {
        const auto request = requestId;
        if (beforeFinished) beforeFinished();
        active = false; emit changed();
        emit finished(request, finishSuccess, {{"state", state == "empty" ? "inserted" : "already-present"},
            {"planHash", hash}, {"code", finishSuccess ? "" : failureCode}});
    }
signals:
    void prepared(const QString &request, const QVariantMap &receipt);
    void finished(const QString &request, bool success, const QVariantMap &receipt);
    void changed();
};

class NativeAgendaInitializationTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<FakeAgendaPage> helper;
    std::unique_ptr<QQmlApplicationEngine> engine;
    NativeDocumentHost *host = nullptr;
    QString socketPath;
    const QString key = "reagenda:event:mycpe:course-17";
    QObject *object(const char *name) const { return host->property(name).value<QObject *>(); }
    QJsonObject agenda() const {
        return {{"schemaVersion", 1}, {"kind", "event"}, {"date", "2026-09-07"}, {"timeZone", "Europe/Paris"},
            {"event", QJsonObject{{"id", "mycpe:course-17"}, {"title", "Travaux dirigés"}, {"subject", "Électronique"},
                {"start", "2026-09-07T09:00:00+02:00"}, {"end", "2026-09-07T10:30:00+02:00"},
                {"timeZone", "Europe/Paris"}, {"allDay", false}}}};
    }
    QJsonObject request(const QString &route, const QJsonObject &body, int timeoutMs = 3000) {
        QLocalSocket socket;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true); timeout.setInterval(timeoutMs);
        QByteArray response;
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
        connect(&socket, &QLocalSocket::readyRead, &loop, [&] { response += socket.readAll(); });
        connect(&socket, &QLocalSocket::connected, &loop, [&] {
            const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
            socket.write("POST " + route.toUtf8() + " HTTP/1.1\r\nHost: local\r\nContent-Length: "
                         + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
        });
        socket.connectToServer(socketPath); timeout.start(); loop.exec();
        response += socket.readAll();
        const auto separator = response.indexOf("\r\n\r\n");
        return separator < 0 ? QJsonObject{{"testFailure", "No HTTP response"}}
                            : QJsonDocument::fromJson(response.mid(separator + 4)).object();
    }
    QJsonObject create(bool withAgenda = true) {
        QJsonObject body{{"displayName", "Travaux dirigés"}, {"idempotencyKey", key}};
        if (withAgenda) body["agenda"] = agenda();
        return request("/v1/notebooks", body);
    }
    QJsonObject open(const QString &id, int timeoutMs = 3000) {
        return request("/v1/documents/" + id + "/open", {{"callerQtfbKey", 2468}}, timeoutMs);
    }
    QString ledgerPath() const {
        return directory->filePath("state/" + QString::fromLatin1(
            QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json");
    }
    QJsonObject ledger() const {
        QFile file(ledgerPath());
        if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    void loadEngine() {
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty("testSocketPath", socketPath);
        engine->rootContext()->setContextProperty("testStateDirectory", directory->filePath("state"));
        engine->rootContext()->setContextProperty("testAgendaPage", helper.get());
        engine->load(QUrl("qrc:/native-documents-tests/AgendaInitializationFixture.qml"));
        QVERIFY(!engine->rootObjects().isEmpty());
        host = qobject_cast<NativeDocumentHost *>(engine->rootObjects().first());
        QVERIFY(host && host->enabled());
        // Preserve the production timeout algorithm, with faster polling in
        // tests that intentionally never make a native document/page ready.
        auto *timer = object("openConfirmation");
        QVERIFY(timer); timer->setProperty("interval", 5);
    }
    bool succeeded(const QJsonObject &value, const char *status) const {
        const bool ok = value["status"].toString() == QString::fromLatin1(status);
        if (!ok) qWarning().noquote() << QJsonDocument(value).toJson();
        return ok;
    }
    void blockJournal() {
        QVERIFY(QFile::rename(ledgerPath(), ledgerPath() + ".saved"));
        QVERIFY(QDir().mkdir(ledgerPath()));
    }
private slots:
    void initTestCase() {
        qmlRegisterType<NativeDocumentHost>("net.asivery.AppLoad", 1, 0, "NativeDocumentHost");
        qmlRegisterType(QUrl("qrc:/appload/qml/RePaperNativeDocuments.qml"), "net.asivery.AppLoad", 1, 0,
                        "RePaperNativeDocuments");
    }
    void init() {
        directory = std::make_unique<QTemporaryDir>(); QVERIFY(directory->isValid());
        socketPath = directory->filePath("run/documents.sock");
        helper = std::make_unique<FakeAgendaPage>(); helper->journalPath = ledgerPath();
        loadEngine(); QVERIFY(host);
    }
    void cleanup() { engine.reset(); host = nullptr; helper.reset(); directory.reset(); }
    void freshPageUsesNativeHeaderAndDurableDispatchBeforeOneInsert_data() {
        QTest::addColumn<bool>("synchronous");
        QTest::newRow("queued-signals") << false;
        QTest::newRow("synchronous-signals") << true;
    }
    void freshPageUsesNativeHeaderAndDurableDispatchBeforeOneInsert() {
        QFETCH(bool, synchronous); helper->synchronous = synchronous;
        object("fakeAgendaHeader")->setProperty("synchronous", synchronous);
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        const auto id = created["documentId"].toString();
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("pending"));
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        QVERIFY(succeeded(open(id), "opened"));
        QCOMPARE(helper->prepareCalls, 1); QCOMPARE(helper->commitCalls, 1); QCOMPARE(helper->nativeInserts, 1);
        QCOMPARE(helper->phaseAtCommit, QString("dispatched")); QCOMPARE(helper->hashAtCommit, helper->hash);
        QCOMPARE(helper->context["documentId"].toString(), id);
        QCOMPARE(helper->context["pageId"].toString(), ledger()["pageId"].toString());
        QCOMPARE(helper->context["layer"].toString(), QString("layer-1"));
        QVERIFY(helper->controller); QVERIFY(helper->pageSize.isValid());
        QCOMPARE(helper->fields["title"].toString(), QString("Travaux dirigés"));
        QCOMPARE(helper->fields["day"].toString(), QString("Lundi"));
        QCOMPARE(helper->fields["date"].toString(), QString("07 / 09 / 2026"));
        QCOMPARE(helper->fields["time"].toString(), QString("09:00 – 10:30"));
        QCOMPARE(helper->fields["headerPrepared"].metaType().id(), int(QMetaType::Bool));
        QVERIFY(helper->fields["headerPrepared"].toBool());
        QCOMPARE(helper->fields["headerPlanHash"].toString(), object("fakeAgendaHeader")->property("hash").toString());
        QCOMPARE(object("fakeAgendaHeader")->property("prepareCalls").toInt(), 2);
        QCOMPARE(object("fakeAgendaHeader")->property("commitCalls").toInt(), 1);
        QCOMPARE(object("fakeAgendaHeader")->property("nativeInserts").toInt(), 1);
        QCOMPARE(object("fakeAgendaHeader")->property("day").toString(), helper->fields["day"].toString());
        QCOMPARE(object("fakeAgendaHeader")->property("date").toString(), helper->fields["date"].toString());
        QCOMPARE(object("fakeAgendaHeader")->property("time").toString(), helper->fields["time"].toString());
        QCOMPARE(object("fakeLibrary")->property("loadCalls").toInt(), 0);
        QCOMPARE(object("fakeDocumentController")->property("customTemplateCalls").toInt(), 0);
        QCOMPARE(object("fakeDocumentController")->property("unloadedTemplateCalls").toInt(), 0);
        QCOMPARE(host->property("callTrace").toString(), QString("|header-prepare|header-prepared|header-cancel|header-prepare|header-prepared|header-commit|header-finished"));
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("completed"));
        QCOMPARE(ledger()["agendaPlanHash"].toString(), helper->hash);
        QTRY_COMPARE(dismissed.size(), 1); QCOMPARE(dismissed.first().first().toInt(), 2468);
        QVERIFY(!helper->busy());
    }
    void alreadyPresentPageCommitsReceiptWithoutAnotherInsert() {
        helper->state = "already-present";
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        QVERIFY(succeeded(open(created["documentId"].toString()), "opened"));
        QCOMPARE(helper->commitCalls, 1); QCOMPARE(helper->nativeInserts, 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("completed"));
        QVERIFY(!helper->busy());
    }
    void completedAndLegacyNotesOpenWithoutReformatOrPreparation_data() {
        QTest::addColumn<bool>("legacy"); QTest::newRow("completed") << false; QTest::newRow("legacy") << true;
    }
    void completedAndLegacyNotesOpenWithoutReformatOrPreparation() {
        QFETCH(bool, legacy);
        auto created = create(!legacy); QVERIFY(succeeded(created, "succeeded"));
        const auto id = created["documentId"].toString();
        if (!legacy) QVERIFY(succeeded(open(id), "opened"));
        const int prepares = helper->prepareCalls, inserts = helper->nativeInserts;
        const int orientations = object("fakeLibraryController")->property("orientationCalls").toInt();
        const int templates = object("fakeDocumentController")->property("templateCalls").toInt();
        created = create(); QVERIFY(succeeded(created, "succeeded")); QCOMPARE(created["documentId"].toString(), id);
        QVERIFY(succeeded(open(id), "opened"));
        QCOMPARE(helper->prepareCalls, prepares); QCOMPARE(helper->nativeInserts, inserts);
        QCOMPARE(object("fakeLibraryController")->property("orientationCalls").toInt(), orientations);
        QCOMPARE(object("fakeDocumentController")->property("templateCalls").toInt(), templates);
        QCOMPARE(ledger()["agendaPageState"].toString(), legacy ? QString("legacy") : QString("completed"));
    }
    void dispatchJournalFailurePreventsCommit() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        helper->afterPrepare = [this] { blockJournal(); };
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        const auto result = open(created["documentId"].toString());
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_JOURNAL_WRITE"));
        QCOMPARE(helper->commitCalls, 0); QCOMPARE(helper->nativeInserts, 0); QCOMPARE(dismissed.size(), 0);
        QVERIFY(!helper->busy());
    }
    void completionJournalFailureDoesNotReportOpened() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        helper->beforeFinished = [this] { blockJournal(); };
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        const auto result = open(created["documentId"].toString());
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_JOURNAL_WRITE"));
        QCOMPARE(helper->nativeInserts, 1); QCOMPARE(dismissed.size(), 0); QVERIFY(!helper->busy());
    }
    void loadingViewDelaysPreparation() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        object("fakeView")->setProperty("isLoading", true);
        QTimer::singleShot(120, host, [this] { QCOMPARE(helper->prepareCalls, 0); object("fakeView")->setProperty("isLoading", false); });
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(succeeded(open(created["documentId"].toString()), "opened"));
        QVERIFY(elapsed.elapsed() >= 100); QCOMPARE(helper->nativeInserts, 1);
    }
    void headerInspectionDelaysTextAndHeaderCompletionDelaysOpening_data() {
        QTest::addColumn<bool>("inspection");
        QTest::newRow("inspection") << true; QTest::newRow("native-header-commit") << false;
    }
    void headerInspectionDelaysTextAndHeaderCompletionDelaysOpening() {
        QFETCH(bool, inspection);
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        object("fakeAgendaHeader")->setProperty(inspection ? "autoPrepared" : "autoFinished", false);
        QTimer::singleShot(120, host, [this, inspection] {
            QCOMPARE(helper->nativeInserts, inspection ? 0 : 1);
            QCOMPARE(ledger()["agendaPageState"].toString(), inspection ? QString("pending") : QString("dispatched"));
            auto *header = object("fakeAgendaHeader");
            header->setProperty(inspection ? "autoPrepared" : "autoFinished", true);
            QVERIFY(QMetaObject::invokeMethod(header, inspection ? "deliverPrepared" : "deliverFinished"));
        });
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(succeeded(open(created["documentId"].toString()), "opened"));
        QVERIFY(elapsed.elapsed() >= 100); QCOMPARE(helper->prepareCalls, 1);
        QCOMPARE(object("fakeLibrary")->property("loadCalls").toInt(), 0);
        QCOMPARE(object("fakeDocumentController")->property("customTemplateCalls").toInt(), 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("completed"));
    }
    void headerFailuresNeverPrepareOrCommitNativeText_data() {
        QTest::addColumn<QString>("failure"); QTest::addColumn<QString>("code");
        QTest::newRow("prepare-refused") << QString("prepare") << QString("SYNTHETIC_HEADER_FAILURE");
        QTest::newRow("inspection-failed") << QString("inspect") << QString("SYNTHETIC_HEADER_FAILURE");
        QTest::newRow("inspection-timeout") << QString("timeout") << QString("NATIVE_OPEN_NOT_CONFIRMED");
    }
    void headerFailuresNeverPrepareOrCommitNativeText() {
        QFETCH(QString, failure); QFETCH(QString, code);
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        object("fakeAgendaHeader")->setProperty(failure == "prepare" ? "allowPrepare"
            : failure == "inspect" ? "failInspection" : "autoPrepared", failure == "inspect");
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        const auto result = open(created["documentId"].toString());
        QCOMPARE(result["error"].toObject()["code"].toString(), code);
        QCOMPARE(helper->prepareCalls, 0); QCOMPARE(helper->commitCalls, 0); QCOMPARE(helper->nativeInserts, 0);
        QCOMPARE(object("fakeAgendaHeader")->property("nativeInserts").toInt(), 0);
        QCOMPARE(object("fakeLibrary")->property("loadCalls").toInt(), 0);
        QCOMPARE(dismissed.size(), 0); QCOMPARE(ledger()["agendaPageState"].toString(), QString("pending"));
    }
    void failedHeaderCommitKeepsDispatchAndRetryPreservesTitle() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        object("fakeAgendaHeader")->setProperty("finishSuccess", false);
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        QVERIFY(open(created["documentId"].toString()).contains("error"));
        QCOMPARE(dismissed.size(), 0); QCOMPARE(ledger()["agendaPageState"].toString(), QString("dispatched"));
        QCOMPARE(helper->nativeInserts, 1);
        helper->state = "already-present";
        object("fakeAgendaHeader")->setProperty("finishSuccess", true);
        object("fakeAgendaHeader")->setProperty("state", "already-present");
        QVERIFY(succeeded(open(created["documentId"].toString()), "opened"));
        QCOMPARE(helper->nativeInserts, 1); QCOMPARE(object("fakeAgendaHeader")->property("nativeInserts").toInt(), 1);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("completed"));
    }
    void refusedHeaderInsertionResumesExistingTitleWithoutCreatingNotebook_data() {
        QTest::addColumn<bool>("restart");
        QTest::newRow("same-host") << false;
        QTest::newRow("after-host-restart") << true;
    }
    void refusedHeaderInsertionResumesExistingTitleWithoutCreatingNotebook() {
        QFETCH(bool, restart);
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        const auto id = created["documentId"].toString(), page = ledger()["pageId"].toString();
        // Reproduce the observed boundary: the title is confirmed, but the
        // native insertion guard rejects header ink before any stroke exists.
        object("fakeAgendaHeader")->setProperty("allowCommit", false);
        QVERIFY(open(id).contains("error"));
        QCOMPARE(helper->nativeInserts, 1);
        QCOMPARE(object("fakeAgendaHeader")->property("nativeInserts").toInt(), 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("dispatched"));
        if (restart) {
            engine.reset(); host = nullptr; helper.reset();
            helper = std::make_unique<FakeAgendaPage>(); helper->journalPath = ledgerPath();
            loadEngine(); QVERIFY(host);
            QVERIFY(QMetaObject::invokeMethod(host, "restoreDocument", Q_ARG(QVariant, id), Q_ARG(QVariant, page)));
        }
        helper->state = "already-present";
        object("fakeAgendaHeader")->setProperty("allowCommit", true);
        const int creates = object("fakeLibraryController")->property("createCalls").toInt();
        const int titles = helper->nativeInserts;
        const auto repeated = create(); QVERIFY(succeeded(repeated, "succeeded"));
        QCOMPARE(repeated["documentId"].toString(), id);
        QCOMPARE(ledger()["pageId"].toString(), page);
        QVERIFY(succeeded(open(id), "opened"));
        QCOMPARE(object("fakeLibraryController")->property("createCalls").toInt(), creates);
        QCOMPARE(helper->nativeInserts, titles);
        QCOMPARE(object("fakeAgendaHeader")->property("nativeInserts").toInt(), 1);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("completed"));
        QVERIFY(succeeded(create(), "succeeded"));
        QVERIFY(succeeded(open(id), "opened"));
        QCOMPARE(object("fakeLibraryController")->property("createCalls").toInt(), creates);
        QCOMPARE(helper->nativeInserts, titles);
        QCOMPARE(object("fakeAgendaHeader")->property("nativeInserts").toInt(), 1);
    }
    void pageChangeAndDisableCancelPendingPreparation_data() {
        QTest::addColumn<QString>("mutation");
        QTest::newRow("view-page-change") << QString("view");
        QTest::newRow("controller-page-change") << QString("controller");
        QTest::newRow("template-change") << QString("template");
        QTest::newRow("disable") << QString("disable");
    }
    void pageChangeAndDisableCancelPendingPreparation() {
        QFETCH(QString, mutation);
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        helper->autoPrepared = false;
        helper->afterPrepare = [this, mutation] {
            QTimer::singleShot(0, host, [this, mutation] {
                if (mutation == "disable") object("fakeLibrary")->setProperty("isReady", false);
                else if (mutation == "controller") object("fakeSceneController")->setProperty("pageId", "fc084b72-b559-4255-adbb-bf1f2d44a854");
                else if (mutation == "template") {
                    const auto document = helper->context["documentId"];
                    QVERIFY(QMetaObject::invokeMethod(object("fakeDocumentController"), "setTemplateForPage",
                        Q_ARG(QVariant, document), Q_ARG(QVariant, 0), Q_ARG(QVariant, QString("Blank")),
                        Q_ARG(QVariant, QSizeF(1620, 2160))));
                }
                else object("fakeView")->setProperty("currentPageId", "fc084b72-b559-4255-adbb-bf1f2d44a854");
            });
        };
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        const auto result = open(created["documentId"].toString());
        QVERIFY(result.contains("error")); QCOMPARE(helper->commitCalls, 0); QCOMPARE(helper->nativeInserts, 0);
        QVERIFY(helper->cancelCalls > 0); QVERIFY(!helper->busy());
        helper->deliverPrepared(); helper->deliverFinished(); QCoreApplication::processEvents();
        QCOMPARE(helper->commitCalls, 0); QCOMPARE(dismissed.size(), 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("pending"));
    }
    void preparationTimeoutCancelsAndIgnoresLateCallbacks() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        helper->autoPrepared = false;
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        const auto result = open(created["documentId"].toString());
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_OPEN_NOT_CONFIRMED"));
        QVERIFY(helper->cancelCalls > 0); QVERIFY(!helper->busy());
        helper->deliverPrepared(); helper->deliverFinished(); QCoreApplication::processEvents();
        QCOMPARE(helper->commitCalls, 0); QCOMPARE(helper->nativeInserts, 0); QCOMPARE(dismissed.size(), 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("pending"));
    }
    void nativeTemplateOrOrientationChangePreventsPreparation_data() {
        QTest::addColumn<bool>("orientation");
        QTest::newRow("orientation") << true; QTest::newRow("template") << false;
    }
    void nativeTemplateOrOrientationChangePreventsPreparation() {
        QFETCH(bool, orientation);
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        const auto id = created["documentId"].toString();
        if (orientation) {
            QVERIFY(QMetaObject::invokeMethod(object("fakeLibraryController"), "setOrientation",
                Q_ARG(QVariant, id), Q_ARG(QVariant, int(Qt::Horizontal))));
        } else {
            QVERIFY(QMetaObject::invokeMethod(object("fakeDocumentController"), "setTemplateForPage",
                Q_ARG(QVariant, id), Q_ARG(QVariant, 0), Q_ARG(QVariant, QString("Blank")), Q_ARG(QVariant, QSizeF(1620,2160))));
        }
        const auto result = open(id);
        QCOMPARE(result["error"].toObject()["code"].toString(), QString("NATIVE_AGENDA_TEMPLATE_CHANGED"));
        QCOMPARE(helper->prepareCalls, 0); QCOMPARE(helper->nativeInserts, 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("pending"));
    }
    void nativeFailureRetainsDispatchedPlanForRetry() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        helper->finishSuccess = false;
        QSignalSpy dismissed(host, &NativeDocumentHost::dismissAppRequested);
        const auto result = open(created["documentId"].toString());
        QVERIFY(result.contains("error")); QCOMPARE(dismissed.size(), 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("dispatched"));
        helper->finishSuccess = true; helper->state = "already-present";
        QVERIFY(succeeded(open(created["documentId"].toString()), "opened"));
        QCOMPARE(helper->commitCalls, 2); QCOMPARE(helper->nativeInserts, 1);
    }
    void retryWithDifferentPlanCannotCommitAgain() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        helper->finishSuccess = false;
        QVERIFY(open(created["documentId"].toString()).contains("error"));
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("dispatched"));
        const auto originalHash = helper->hash;
        helper->finishSuccess = true; helper->state = "already-present"; helper->hash = QString(64, 'b');
        const auto retry = open(created["documentId"].toString());
        QCOMPARE(retry["error"].toObject()["code"].toString(), QString("NATIVE_AGENDA_PLAN_MISMATCH"));
        QCOMPARE(helper->commitCalls, 1); QCOMPARE(helper->nativeInserts, 1);
        QCOMPARE(ledger()["agendaPlanHash"].toString(), originalHash);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("dispatched"));
    }
    void interruptedCommitRecoversAfterHostRestartWithoutAnotherInsert() {
        const auto created = create(); QVERIFY(succeeded(created, "succeeded"));
        const auto id = created["documentId"].toString();
        const auto page = ledger()["pageId"].toString();
        helper->autoFinished = false;
        QVERIFY(open(id, 70).contains("testFailure"));
        QCOMPARE(helper->commitCalls, 1); QCOMPARE(helper->nativeInserts, 1);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("dispatched"));
        engine.reset(); host = nullptr; helper.reset();
        helper = std::make_unique<FakeAgendaPage>(); helper->journalPath = ledgerPath(); helper->state = "already-present";
        loadEngine(); QVERIFY(host);
        QVERIFY(QMetaObject::invokeMethod(host, "restoreDocument", Q_ARG(QVariant, id), Q_ARG(QVariant, page)));
        const auto repeated = create(); QVERIFY(succeeded(repeated, "succeeded")); QCOMPARE(repeated["documentId"].toString(), id);
        QVERIFY(succeeded(open(id), "opened"));
        QCOMPARE(helper->commitCalls, 1); QCOMPARE(helper->nativeInserts, 0);
        QCOMPARE(ledger()["agendaPageState"].toString(), QString("completed"));
    }
};
QTEST_MAIN(NativeAgendaInitializationTest)
#include "NativeAgendaInitializationTest.moc"

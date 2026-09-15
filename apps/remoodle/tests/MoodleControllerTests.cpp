#include "KeyboardController.h"
#include "MoodleController.h"
#include "MoodleProtocol.h"
#include "MoodleRelayClient.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QInputMethodEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>
#include <cstring>

namespace {
const QString Site = "https://school.example/moodle";
const QString SecondSite = "https://another-school.example/campus";
const QString TokenA(32, 'a'), TokenB(32, 'b'); // Synthetic test tokens only.

QJsonDocument siteInfo(int user) {
    return QJsonDocument(
        QJsonObject{{"userid", user},
                    {"sitename", "École de test"},
                    {"functions", QJsonArray{QJsonObject{{"name", "core_enrol_get_users_courses"}},
                                             QJsonObject{{"name", "core_course_get_contents"}}}}});
}
QJsonDocument courseList() {
    return QJsonDocument(
        QJsonArray{QJsonObject{{"id", 11}, {"fullname", "Mathématiques"}, {"shortname", "MATH"}}});
}
QJsonDocument courseContents(const QString &base) {
    QJsonArray files;
    for (const auto &name : QStringList{"a.pdf", "b.pdf"})
        files.append(
            QJsonObject{{"type", "file"},
                        {"filename", name},
                        {"fileurl", base + "/webservice/pluginfile.php/42/" + name + "?token=secret-in-url"},
                        {"mimetype", "application/pdf"},
                        {"filesize", 0},
                        {"timemodified", 100},
                        {"privatetoken", "must-not-be-persisted"}});
    return QJsonDocument(QJsonArray{QJsonObject{
        {"id", 21},
        {"name", "Section"},
        {"summary", "<b>Description</b>"},
        {"modules", QJsonArray{QJsonObject{
                        {"id", 31}, {"name", "Ressources"}, {"modname", "folder"}, {"contents", files}}}}}});
}

// This fixture intercepts requests entirely in memory. It does not install a CA,
// alter SSL configuration, open a listening port, or bypass production TLS checks.
class FakeReply : public QNetworkReply {
  public:
    FakeReply(const QNetworkRequest &request, QNetworkAccessManager::Operation operation, QObject *parent)
        : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override {
        ++abortCount;
        setError(OperationCanceledError, "Synthetic cancellation");
    }
    bool isSequential() const override {
        return true;
    }
    qint64 bytesAvailable() const override {
        return pending.size() + QNetworkReply::bytesAvailable();
    }
    void progress(qint64 received, qint64 total) {
        emit downloadProgress(received, total);
    }
    void finish(const QByteArray &body, int status = 200, bool announceData = true) {
        if (isFinished())
            return;
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        pending += body;
        if (announceData && !pending.isEmpty())
            emit readyRead();
        setFinished(true);
        emit finished();
    }
    int abortCount = 0;

  protected:
    qint64 readData(char *destination, qint64 maximum) override {
        const auto amount = qMin(maximum, qint64(pending.size()));
        if (amount == 0)
            return isFinished() ? -1 : 0;
        std::memcpy(destination, pending.constData(), size_t(amount));
        pending.remove(0, int(amount));
        return amount;
    }

  private:
    QByteArray pending;
};
class FakeMoodle : public QNetworkAccessManager {
  public:
    struct Recorded {
        QNetworkRequest request;
        Operation operation;
        QUrlQuery form;
    };
    QVector<Recorded> requests;
    QVector<QPointer<FakeReply>> replies;
    bool holdAuthentication = false;
    bool rejectAuthentication = false;
    bool rejectContents = false;
    bool holdQr = false;
    QString qrError;
    QJsonArray qrRequest;
    int count(const QString &function) const {
        int count = 0;
        for (const auto &entry : requests)
            if (entry.form.queryItemValue("wsfunction") == function)
                ++count;
        return count;
    }

  protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoing) override {
        const auto body = outgoing ? outgoing->readAll() : QByteArray();
        QUrlQuery form(QString::fromUtf8(body));
        requests.append({request, operation, form});
        auto reply = new FakeReply(request, operation, this);
        replies.append(reply);
        if (request.url().path().endsWith("/lib/ajax/service-nologin.php")) {
            qrRequest = QJsonDocument::fromJson(body).array();
            if (holdQr)
                return reply;
            QJsonObject response =
                qrError.isEmpty()
                    ? QJsonObject{{"error", false},
                                  {"data", QJsonObject{{"token", TokenA},
                                                       {"privatetoken", "synthetic-private-never-saved"},
                                                       {"warnings", QJsonArray{}}}}}
                    : QJsonObject{{"error", true}, {"exception", QJsonObject{{"errorcode", qrError}}}};
            QTimer::singleShot(0, reply, [reply, response] {
                reply->finish(QJsonDocument(QJsonArray{response}).toJson(QJsonDocument::Compact));
            });
            return reply;
        }
        if (operation == GetOperation)
            return reply; // Tests drive download events precisely.
        const auto function = form.queryItemValue("wsfunction");
        if (holdAuthentication && function == "core_webservice_get_site_info")
            return reply;
        QJsonDocument response;
        if (function == "core_webservice_get_site_info") {
            response = rejectAuthentication
                           ? QJsonDocument(QJsonObject{{"exception", "webservice_access_exception"},
                                                       {"errorcode", "invalidtoken"}})
                           : siteInfo(form.queryItemValue("wstoken") == TokenB ? 2 : 1);
        } else if (function == "core_enrol_get_users_courses")
            response = courseList();
        else if (function == "core_course_get_contents") {
            auto base = request.url().toString();
            base.chop(QString("/webservice/rest/server.php").size());
            response = rejectContents ? QJsonDocument(QJsonObject{{"exception", "moodle_exception"},
                                                                  {"errorcode", "accessexception"}})
                                      : courseContents(base);
        } else
            response = QJsonDocument(QJsonObject{{"exception", "unexpected_test_call"}});
        QTimer::singleShot(0, reply,
                           [reply, response] { reply->finish(response.toJson(QJsonDocument::Compact)); });
        return reply;
    }
};
class FakeImportBridge : public QLocalServer {
  public:
    QList<QJsonObject> imports;
    FakeImportBridge() {
        connect(this, &QLocalServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const auto separator = buffer->indexOf("\r\n\r\n");
                    if (separator < 0) return;
                    int length = 0;
                    for (const auto &line : buffer->left(separator).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer->size() - separator - 4 < length) return;
                    imports << QJsonDocument::fromJson(buffer->mid(separator + 4, length)).object();
                    const QByteArray body = "{\"documentId\":\"synthetic-ppt-document\",\"message\":\"Import terminé\"}";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromServer();
                });
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
};
} // namespace

class MoodleControllerTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    int profile = 0;
    void login(MoodleController &controller, const QString &base = Site, const QString &token = TokenA) {
        QSignalSpy done(&controller, &MoodleController::loginFinished);
        controller.login(base, token);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 2000);
        QVERIFY(done.first().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QVERIFY(controller.loggedIn());
    }
    void browse(MoodleController &controller) {
        QVERIFY(!controller.items().isEmpty());
        controller.openItem(controller.items().first().toMap());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QVERIFY(!controller.items().isEmpty());
        QCOMPARE(controller.items().first().toMap().value("kind").toString(), QString("section"));
        controller.openItem(controller.items().first().toMap());
        QVERIFY(!controller.items().isEmpty());
        controller.openItem(controller.items().first().toMap());
        QCOMPARE(controller.items().size(), 2);
    }
  private slots:
    void initTestCase() {
        repaper::registerKeyboardTypes();
        QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        QCoreApplication::setOrganizationName("RePaperTest");
    }
    void init() {
        QCoreApplication::setApplicationName("moodle-controller-" + QString::number(++profile));
    }
    void cleanup() {
        qunsetenv("REPAPER_OFFICE_CONVERTER");
        qunsetenv("REPAPER_CONVERTER_TEST_LOG");
        qunsetenv("PAPER_BRIDGE_SOCKET");
    }
    void powerPointCatalogKeepsTheSameOriginRequirement() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        QVariantList files;
        for (const auto &name : QStringList{"cours.PPTX", "ancien.ppt", "texte.docx"})
            files << QVariantMap{{"type", "file"}, {"filename", name}, {"mimetype", "application/octet-stream"},
                                 {"fileurl", Site + "/webservice/pluginfile.php/" + name}};
        files << QVariantMap{{"type", "file"}, {"filename", "externe.pptx"},
            {"mimetype", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
            {"fileurl", "https://external.example/file.pptx"}};
        controller.openItem({{"kind", "module"}, {"id", 40}, {"name", "Documents"}, {"contents", files}});
        const auto rows = controller.items();
        QCOMPARE(rows.size(), 4);
        QVERIFY(rows[0].toMap()["compatible"].toBool());
        QVERIFY(rows[1].toMap()["compatible"].toBool());
        QVERIFY(rows[0].toMap()["convertsToPdf"].toBool());
        QVERIFY(!rows[2].toMap()["compatible"].toBool());
        QVERIFY(!rows[3].toMap()["compatible"].toBool());
    }
    void powerPointDownloadConvertsOnceAndKeepsTheOriginalIdentity() {
        qputenv("REPAPER_OFFICE_CONVERTER", QByteArray(OFFICE_FIXTURE));
        const auto conversionLog = directory.filePath("controller-conversion.jsonl");
        QFile::remove(conversionLog);
        qputenv("REPAPER_CONVERTER_TEST_LOG", conversionLog.toUtf8());
        FakeImportBridge bridge;
        const auto socketName = directory.filePath("ppt-bridge.sock");
        QVERIFY(bridge.listen(socketName));
        qputenv("PAPER_BRIDGE_SOCKET", socketName.toUtf8());
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        controller.openItem({{"kind", "module"}, {"id", 40}, {"name", "Documents"},
            {"contents", QVariantList{QVariantMap{{"type", "file"}, {"filename", "cours.pptx"},
                {"mimetype", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
                {"fileurl", Site + "/webservice/pluginfile.php/cours.pptx"}, {"timemodified", 200}}}}});
        const auto resource = controller.items().first().toMap();
        controller.importResource(resource);
        QVERIFY(controller.busy());
        QFile presentation(QFINDTESTDATA("fixtures/circuits.pptx"));
        QVERIFY(presentation.open(QIODevice::ReadOnly));
        network.replies.last()->finish(presentation.readAll());
        QTRY_COMPARE_WITH_TIMEOUT(bridge.imports.size(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        const auto first = bridge.imports.first();
        const auto account = QString::fromLatin1(QCryptographicHash::hash((Site + "|1").toUtf8(), QCryptographicHash::Sha256).toHex());
        QCOMPARE(first["idempotencyKey"].toString(), account + ":" + resource["id"].toString() + ":" + resource["revision"].toString());
        QCOMPARE(first["displayName"].toString(), QString("cours.pdf"));
        QFile cache(first["path"].toString());
        QVERIFY(cache.open(QIODevice::ReadOnly));
        const auto pdf = cache.readAll();
        cache.close();
        QVERIFY(pdf.startsWith("%PDF-"));
        QCOMPARE(first["sha256"].toString(), QString::fromLatin1(QCryptographicHash::hash(pdf, QCryptographicHash::Sha256).toHex()));
        QVERIFY(controller.items().first().toMap()["cached"].toBool());
        QVERIFY(controller.items().first().toMap()["imported"].toBool());
        const auto downloads = network.requests.size();
        controller.importResource(resource);
        QTRY_COMPARE_WITH_TIMEOUT(bridge.imports.size(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(network.requests.size(), downloads);
        QCOMPARE(bridge.imports[1], first);
        QFile log(conversionLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count('\n'), 1);
    }
    void officialQrExchangeIsDirectAndCookieFree() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        controller.prepareLogin(Site);
        auto relay = controller.findChild<MoodleRelayClient *>();
        QVERIFY(relay);
        QSignalSpy done(&controller, &MoodleController::loginFinished);
        relay->qrReceived(1, QString(32, 'c'));
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(done.first().first().toBool());
        QTRY_VERIFY(!controller.busy());
        QVERIFY(controller.loggedIn());
        QCOMPARE(network.qrRequest.size(), 1);
        const auto request = network.qrRequest.first().toObject();
        QCOMPARE(request["methodname"].toString(), QString("tool_mobile_get_tokens_for_qr_login"));
        QCOMPARE(request["args"].toObject(), (QJsonObject{{"userid", 1}, {"qrloginkey", QString(32, 'c')}}));
        const auto http = network.requests.first().request;
        QCOMPARE(http.url(), QUrl(Site + "/lib/ajax/service-nologin.php"));
        QVERIFY(http.rawHeader("User-Agent").contains("MoodleMobile"));
        QVERIFY(http.rawHeader("Cookie").isEmpty());
        QVERIFY(http.rawHeader("Authorization").isEmpty());
        QCOMPARE(http.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(),
                 int(QNetworkRequest::Manual));
        QCOMPARE(http.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(),
                 int(QNetworkRequest::Manual));
        QCOMPARE(http.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::ManualRedirectPolicy));
        const auto vault = repaper::SecretStore().get("account");
        QVERIFY(!vault.contains("synthetic-private"));
        QVERIFY(!vault.contains(QString(32, 'c').toUtf8()));
    }
    void officialQrErrorsPreservePreviousAccount_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("message");
        QTest::newRow("different network") << QString("ipmismatch") << QString("même réseau");
        QTest::newRow("expired") << QString("expiredkey") << QString("expiré");
        QTest::newRow("consumed") << QString("invalidkey") << QString("expiré");
        QTest::newRow("app required") << QString("apprequired") << QString("refuse");
    }
    void officialQrErrorsPreservePreviousAccount() {
        QFETCH(QString, code);
        QFETCH(QString, message);
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        network.qrError = code;
        controller.prepareLogin(SecondSite);
        QSignalSpy done(&controller, &MoodleController::loginFinished);
        controller.findChild<MoodleRelayClient *>()->qrReceived(2, QString(32, 'c'));
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(!done.first().first().toBool());
        QVERIFY(controller.message().contains(message));
        QVERIFY(!controller.busy());
        QVERIFY(controller.loggedIn());
        QCOMPARE(controller.baseUrl(), Site);
        QCOMPARE(network.count("core_webservice_get_site_info"), 1);
    }
    void officialQrWrongIdentityRejected() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        controller.prepareLogin(Site);
        QSignalSpy done(&controller, &MoodleController::loginFinished);
        controller.findChild<MoodleRelayClient *>()->qrReceived(2, QString(32, 'c'));
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(!done.first().first().toBool());
        QVERIFY(!controller.loggedIn());
        QVERIFY(controller.message().contains("ne correspond pas"));
    }
    void officialQrCancellationIgnoresLateResult() {
        FakeMoodle network;
        network.holdQr = true;
        MoodleController controller(nullptr, &network);
        controller.prepareLogin(Site);
        QSignalSpy done(&controller, &MoodleController::loginFinished);
        controller.findChild<MoodleRelayClient *>()->qrReceived(1, QString(32, 'c'));
        QVERIFY(controller.busy());
        auto reply = network.replies.last();
        controller.cancel();
        QVERIFY(!controller.busy());
        QVERIFY(reply->abortCount > 0);
        reply->finish(
            QJsonDocument(QJsonArray{QJsonObject{{"error", false}, {"data", QJsonObject{{"token", TokenA}}}}})
                .toJson());
        QCoreApplication::processEvents();
        QVERIFY(!controller.loggedIn());
        QCOMPARE(network.count("core_webservice_get_site_info"), 0);
        QCOMPARE(done.count(), 0);
    }
    void realLoginLeavesDemoAndFetchesContents() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        controller.loadDemo();
        QVERIFY(controller.demo());
        login(controller);
        QVERIFY(!controller.demo());
        browse(controller);
        QCOMPARE(network.count("core_course_get_contents"), 1);
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(network.count("core_course_get_contents"), 2);
        QCOMPARE(controller.items().first().toMap().value("kind").toString(), QString("section"));
        for (const auto &entry : network.requests) {
            QCOMPARE(entry.request.url().scheme(), QString("https"));
            QCOMPARE(entry.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                     int(QNetworkRequest::ManualRedirectPolicy));
            QVERIFY(entry.request.transferTimeout() > 0);
            QVERIFY(entry.request.sslConfiguration().peerVerifyMode() != QSslSocket::VerifyNone);
        }
    }
    void pendingLoginFieldSurvivesUnrelatedChanges() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("moodle", &controller);
        engine.load(QUrl("qrc:/remoodle/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->setProperty("settingsOpen", true);
        auto field = window->findChild<QQuickItem *>("loginBaseField");
        QVERIFY(field);
        field->forceActiveFocus();
        QTest::qWait(20);
        QMetaObject::invokeMethod(field, "selectAll");
        QInputMethodEvent commit;
        commit.setCommitString(SecondSite);
        QCoreApplication::sendEvent(field, &commit);
        QCOMPARE(field->property("text").toString(), SecondSite);
        QCOMPARE(controller.loginBaseUrl(), SecondSite);
        controller.prepareLogin(SecondSite);
        controller.setSearch("unrelated update");
        QCOMPARE(field->property("text").toString(), SecondSite);
        QCOMPARE(controller.baseUrl(), Site);
        QCOMPARE(QUrl(controller.launchLink()).host(), QString("another-school.example"));
        const auto generated = controller.launchLink();
        QVERIFY(!generated.isEmpty());
        controller.setLoginBaseUrl(Site);
        QVERIFY(controller.launchLink().isEmpty());
    }
    void fileCacheIsAccountScopedAndSnapshotsAreRedacted() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        browse(controller);
        const auto resource = controller.items().first().toMap();
        QVERIFY(!resource.value("cached").toBool());
        const auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        const auto accountA = QString::fromLatin1(
            QCryptographicHash::hash((Site + "|1").toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto revision = resource.value("revision").toString();
        QFile cached(root + "/files/" + accountA + "-" + revision + ".download");
        QVERIFY(cached.open(QIODevice::WriteOnly));
        cached.write("%PDF-1.4\naccount A only\n");
        cached.close();
        QFile legacy(root + "/files/" + revision + ".download");
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        legacy.write("%PDF-1.4\nlegacy unscoped bytes\n");
        legacy.close();
        QVERIFY(controller.items().first().toMap().value("cached").toBool());
        const QString connection = "snapshot-review-" + QString::number(profile);
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(root + "/cache.sqlite");
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("SELECT data FROM snapshots"));
            while (query.next()) {
                const auto data = query.value(0).toByteArray();
                QVERIFY(!data.contains("secret-in-url"));
                QVERIFY(!data.contains("must-not-be-persisted"));
                QVERIFY(!data.contains("privatetoken"));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        login(controller, Site, TokenB);
        browse(controller);
        QCOMPARE(controller.items().first().toMap().value("revision").toString(), revision);
        QVERIFY(!controller.items().first().toMap().value("cached").toBool());
        QVERIFY(QFile::exists(cached.fileName()));
    }
    void staleDownloadCannotChangeNewProgressOrCancellation() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        browse(controller);
        const auto rows = controller.items();
        controller.importResource(rows[0].toMap());
        QVERIFY(controller.busy());
        auto first = network.replies.last();
        QVERIFY(first);
        first->progress(10, 100);
        QCOMPARE(controller.progress(), .1);
        controller.cancel();
        QCOMPARE(first->abortCount, 1);
        controller.importResource(rows[1].toMap());
        QVERIFY(controller.busy());
        auto second = network.replies.last();
        QVERIFY(second && second != first);
        second->progress(2, 100);
        QCOMPARE(controller.progress(), .02);
        first->progress(99, 100);
        QCOMPARE(controller.progress(), .02);
        first->finish("%PDF-1.4\nlate cancelled content\n");
        QVERIFY(controller.busy());
        QCOMPARE(controller.progress(), .02);
        controller.cancel();
        QCOMPARE(second->abortCount, 1);
        second->finish("%PDF-1.4\nother cancelled content\n");
        QVERIFY(!controller.busy());
        const QDir files(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/files");
        QCOMPARE(files.entryList({"*.download"}, QDir::Files).size(), 0);
    }
    void cancelledAndRejectedLoginPreservePreviousAccount() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        network.holdAuthentication = true;
        controller.prepareLogin(SecondSite);
        QSignalSpy done(&controller, &MoodleController::loginFinished);
        controller.login(SecondSite, TokenB);
        auto pending = network.replies.last();
        QVERIFY(controller.busy());
        controller.cancel();
        QCOMPARE(done.count(), 1);
        QVERIFY(!done.first().first().toBool());
        QCOMPARE(controller.baseUrl(), Site);
        QVERIFY(controller.loggedIn());
        pending->finish(siteInfo(2).toJson());
        QCOMPARE(controller.baseUrl(), Site);
        network.holdAuthentication = false;
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(network.requests.last().form.queryItemValue("wstoken"), TokenA);
        QCOMPARE(network.requests.last().request.url().host(), QString("school.example"));
        network.rejectAuthentication = true;
        controller.login(SecondSite, TokenB);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 2000);
        QVERIFY(!done.last().first().toBool());
        QCOMPARE(controller.baseUrl(), Site);
        repaper::SecretStore vault;
        const auto saved = QJsonDocument::fromJson(vault.get("account")).object();
        QCOMPARE(saved.value("base").toString(), Site);
        QCOMPARE(saved.value("token").toString(), TokenA);
        QVERIFY(controller.message().contains("expiré"));
    }
    void accessFailureKeepsCachedCourse() {
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        login(controller);
        browse(controller);
        network.rejectContents = true;
        controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QVERIFY(controller.offline());
        QVERIFY(!controller.items().isEmpty());
        QCOMPARE(controller.items().first().toMap().value("kind").toString(), QString("section"));
        QCOMPARE(controller.items().first().toMap().value("name").toString(), QString("Section"));
    }
    void invalidPortalOverrideIsExplicit() {
        const auto previous = qgetenv("REPAPER_MOODLE_PORTAL_URL");
        qputenv("REPAPER_MOODLE_PORTAL_URL", "http://insecure.example");
        FakeMoodle network;
        MoodleController controller(nullptr, &network);
        controller.prepareLogin(Site);
        controller.openLogin(Site);
        QVERIFY(!controller.waitingForBrowser());
        QVERIFY(controller.loginCode().isEmpty());
        QVERIFY(controller.message().contains("portail", Qt::CaseInsensitive));
        QCOMPARE(network.requests.size(), 0);
        if (previous.isNull())
            qunsetenv("REPAPER_MOODLE_PORTAL_URL");
        else
            qputenv("REPAPER_MOODLE_PORTAL_URL", previous);
    }
};
QTEST_MAIN(MoodleControllerTests)
#include "MoodleControllerTests.moc"

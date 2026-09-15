#include "AgendaController.h"
#include "SecretStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>
#include <cstring>

class DeferredReply : public QNetworkReply {
    QByteArray payload;
    qint64 position = 0;
    bool completed = false;
public:
    bool abortRequested = false;
    explicit DeferredReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent) {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override {
        // A queued response can still arrive after cancellation. Tests choose its order.
        abortRequested = true;
    }
    void finish(const QByteArray &bytes, int status = 200) {
        if (completed) return;
        completed = true; payload = bytes;
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (status >= 400) setError(QNetworkReply::ContentAccessDenied, "Synthetic response");
        setFinished(true);
        emit readyRead(); emit finished();
    }
    qint64 bytesAvailable() const override { return payload.size() - position + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 size) override {
        const auto count = qMin(size, payload.size() - position);
        if (!count) return -1;
        std::memcpy(data, payload.constData() + position, static_cast<size_t>(count));
        position += count; return count;
    }
};

class CalendarNetwork : public QNetworkAccessManager {
public:
    QList<QPointer<DeferredReply>> replies;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        auto reply = new DeferredReply(request, this);
        replies.append(reply);
        return reply;
    }
};

class AgendaControllerTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> temporary;
    QByteArray priorDataHome;
    bool priorTestMode = false;
    void seedCpe() {
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QVERIFY(QDir().mkpath(directory));
        QFile cache(directory + "/agenda-cache.json");
        QVERIFY(cache.open(QIODevice::WriteOnly));
        cache.write(QJsonDocument(QJsonObject{{"version", 1}, {"sources", QJsonArray{
            QJsonObject{{"id", "mycpe"}, {"kind", "cpe"}, {"label", "Synthetic CPE"},
                        {"events", QJsonArray{}}, {"cachedWeeks", QJsonArray{}}}}}}).toJson());
        repaper::SecretStore secrets;
        QVERIFY(secrets.put("cpe-token", "synthetic-only-token"));
        QVERIFY(secrets.put("cpe-configuration", "{\"visibilite\":{\"est_visible_mon_planning\":true}}"));
    }
    static QByteArray event(const QString &date, const QString &title) {
        return QJsonDocument(QJsonArray{QJsonObject{{"id", title}, {"date_debut", date + "T09:00:00"},
            {"date_fin", date + "T10:00:00"}, {"libelle", title}, {"favori", QJsonObject{{"f3", title}}}}}).toJson();
    }
    static QString firstDate(const DeferredReply *reply) {
        return QUrlQuery(reply->url()).queryItemValue("date_debut");
    }
private slots:
    void init() {
        priorDataHome = qgetenv("XDG_DATA_HOME"); priorTestMode = QStandardPaths::isTestModeEnabled();
        temporary = std::make_unique<QTemporaryDir>(); QVERIFY(temporary->isValid());
        qputenv("XDG_DATA_HOME", temporary->path().toUtf8()); QStandardPaths::setTestModeEnabled(false);
        QCoreApplication::setOrganizationName("RePaperTests");
        QCoreApplication::setApplicationName("reagenda-controller");
    }
    void cleanup() {
        if (priorDataHome.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", priorDataHome);
        QStandardPaths::setTestModeEnabled(priorTestMode); temporary.reset();
    }
    void navigationDebouncesAndFetchesTheFinalWeek() {
        seedCpe(); CalendarNetwork network; AgendaController agenda(nullptr, &network);
        agenda.selectDate("2026-09-07"); agenda.shift(1); agenda.shift(1); agenda.shift(1);
        QCOMPARE(network.replies.size(), 0);
        QTRY_COMPARE(network.replies.size(), 1);
        QCOMPARE(firstDate(network.replies[0]), QString("2026-09-28"));
        network.replies[0]->finish("[]");
        QTRY_VERIFY(!agenda.busy());
        agenda.shift(-1); QTRY_COMPARE(network.replies.size(), 2);
        QCOMPARE(firstDate(network.replies[1]), QString("2026-09-21"));
        network.replies[1]->finish("[]");
    }
    void cancelledWeekResponseCannotReplaceCurrentDataOrMessage() {
        seedCpe(); CalendarNetwork network; AgendaController agenda(nullptr, &network);
        agenda.selectDate("2026-09-07"); QTRY_COMPARE(network.replies.size(), 1);
        auto old = network.replies[0]; agenda.shift(1);
        QVERIFY(old->abortRequested); QTRY_COMPARE(network.replies.size(), 2);
        network.replies[1]->finish(event("2026-09-14", "Current week"));
        const auto before = agenda.days(); const auto message = agenda.message();
        old->finish(event("2026-09-07", "Stale week"));
        QCOMPARE(agenda.days(), before); QCOMPARE(agenda.message(), message);
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QFile file(directory + "/agenda-cache.json"); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto cached = QJsonDocument::fromJson(file.readAll()).object()["sources"].toArray()[0].toObject();
        QCOMPARE(cached["windowFirst"].toString(), QString("2026-09-14"));
        QCOMPARE(cached["cachedWeeks"].toArray(), QJsonArray{"2026-09-14"});
    }
    void staleUnauthorizedReplyCannotDisconnectCurrentAccount_data() {
        QTest::addColumn<bool>("currentReplyFirst");
        QTest::newRow("stale-401-during-current-refresh") << false;
        QTest::newRow("stale-401-after-current-success") << true;
    }
    void staleUnauthorizedReplyCannotDisconnectCurrentAccount() {
        QFETCH(bool, currentReplyFirst);
        seedCpe(); CalendarNetwork network; AgendaController agenda(nullptr, &network);
        agenda.selectDate("2026-09-07"); QTRY_COMPARE(network.replies.size(), 1);
        auto old = network.replies[0]; agenda.shift(1); QTRY_COMPARE(network.replies.size(), 2);
        QVERIFY(old->abortRequested);
        auto current = network.replies[1];
        QCOMPARE(firstDate(current), QString("2026-09-14"));
        QCOMPARE(current->request().rawHeader("Authorization"), QByteArray("Bearer synthetic-only-token"));
        const auto currentEvents = event("2026-09-14", "Current week");
        if (currentReplyFirst)
            current->finish(currentEvents);

        repaper::SecretStore secrets;
        const auto token = secrets.get("cpe-token");
        const auto configuration = secrets.get("cpe-configuration");
        QVERIFY(!token.isEmpty()); QVERIFY(!configuration.isEmpty());
        const auto days = agenda.days(); const auto sources = agenda.sources();
        const auto message = agenda.message();
        QCOMPARE(message, currentReplyFirst ? QString() : QString("Actualisation des calendriers…"));

        old->finish("{}", 401);
        QVERIFY(agenda.cpeConnected());
        QVERIFY(!current->abortRequested);
        QCOMPARE(secrets.get("cpe-token"), token);
        QCOMPARE(secrets.get("cpe-configuration"), configuration);
        QCOMPARE(agenda.days(), days); QCOMPARE(agenda.sources(), sources);
        QCOMPARE(agenda.message(), message);

        if (!currentReplyFirst)
            current->finish(currentEvents);
        QTRY_VERIFY(!agenda.busy());
        QVERIFY(agenda.cpeConnected());
        const auto events = agenda.days().first().toMap()["events"].toList();
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().toMap()["title"].toString(), QString("Current week"));
        QCOMPARE(agenda.message(), QString()); // Successful refresh leaves no ordinary status message.
    }
    void sameWeekViewsDeduplicateAndRecentWeeksUseCache() {
        seedCpe(); CalendarNetwork network; AgendaController agenda(nullptr, &network);
        agenda.selectDate("2026-09-07"); QTRY_COMPARE(network.replies.size(), 1);
        agenda.setView("day"); agenda.selectDate("2026-09-08");
        QTest::qWait(350); QCOMPARE(network.replies.size(), 1); QVERIFY(!network.replies[0]->abortRequested);
        network.replies[0]->finish("[]");
        agenda.setView("week"); agenda.shift(1); QTRY_COMPARE(network.replies.size(), 2);
        network.replies[1]->finish("[]");
        agenda.shift(-1); QTest::qWait(350); QCOMPARE(network.replies.size(), 2);
        agenda.refresh(); QCOMPARE(network.replies.size(), 3); // Explicit refresh bypasses the freshness window.
        network.replies[2]->finish("[]");
    }
    void disconnectCancelsOutstandingCalendarAndPreservesOfflineState() {
        seedCpe(); CalendarNetwork network; AgendaController agenda(nullptr, &network);
        agenda.selectDate("2026-09-07"); QTRY_COMPARE(network.replies.size(), 1);
        auto reply = network.replies[0]; agenda.disconnectCpe(); QVERIFY(reply->abortRequested);
        reply->finish(event("2026-09-07", "Late account data"));
        QVERIFY(!agenda.cpeConnected()); QVERIFY(agenda.days().first().toMap()["events"].toList().isEmpty());
    }
    void noteRequestRetainsEventIdentityAndPreventsRepeatedClicks() {
        CalendarNetwork network; AgendaController agenda(nullptr, &network);
        QStringList received; QJsonObject context; int calls = 0;
        agenda.setNoteHandler([&](const QString &date, const QString &id, const QString &title, const QJsonObject &value) {
            ++calls; received = {date, id, title}; context = value; return true;
        });
        const QVariantMap snapshot{{"id", "mycpe:event-123"}, {"title", "Cours de test"}, {"subject", "Analyse"},
            {"start", "2026-09-14T09:00:00+02:00"}, {"end", "2026-09-14T11:00:00+02:00"},
            {"timeZone", "Europe/Paris"}, {"allDay", false}};
        QSignalSpy notes(&agenda, &AgendaController::noteRequested);
        agenda.requestNote("2026-09-14", "mycpe:event-123", "Cours de test", snapshot);
        QVERIFY(agenda.noteBusy()); QCOMPARE(calls, 1); QCOMPARE(notes.size(), 1);
        QCOMPARE(received, QStringList({"2026-09-14", "mycpe:event-123", "Cours de test"}));
        QCOMPARE(context["event"].toObject()["subject"].toString(), QString("Analyse"));
        QCOMPARE(context["event"].toObject()["start"].toString(), QString("2026-09-14T09:00:00+02:00"));
        QCOMPARE(context["timeZone"].toString(), QString("Europe/Paris"));
        agenda.requestNote("2026-09-14", "mycpe:event-123", "Cours de test", snapshot); QCOMPARE(calls, 1);
        agenda.reportBridgeError("Synthetic native opening error");
        QVERIFY(!agenda.noteBusy()); QCOMPARE(agenda.noteMessage(), QString("Synthetic native opening error"));
        agenda.requestNote("2026-09-14", "mycpe:event-123", "Cours de test", snapshot); QCOMPARE(calls, 2);
    }
    void openEventSnapshotSurvivesCalendarRefresh() {
        seedCpe(); CalendarNetwork network; AgendaController agenda(nullptr, &network);
        agenda.selectDate("2026-09-07"); QTRY_COMPARE(network.replies.size(), 1);
        const QJsonObject original{{"id", "stable-course-id"}, {"date_debut", "2026-09-07T09:00:00"},
            {"date_fin", "2026-09-07T10:30:00"},
            {"favori", QJsonObject{{"f2", "Travaux dirigés | Salle A"}, {"f3", "Électronique"}}}};
        network.replies.last()->finish(QJsonDocument(QJsonArray{original}).toJson());
        QTRY_VERIFY(!agenda.busy());
        const auto snapshot = agenda.days().first().toMap()["events"].toList().first().toMap();
        QCOMPARE(snapshot["title"].toString(), QString("Travaux dirigés"));
        QCOMPARE(snapshot["subject"].toString(), QString("Électronique"));
        auto changed = original;
        changed["date_debut"] = "2026-09-07T14:00:00";
        changed["date_fin"] = "2026-09-07T15:00:00";
        changed["matiere"] = "Titre actualisé";
        agenda.refresh(); QCOMPARE(network.replies.size(), 2);
        network.replies.last()->finish(QJsonDocument(QJsonArray{changed}).toJson());
        QTRY_VERIFY(!agenda.busy());
        QJsonObject context; QString title;
        agenda.setNoteHandler([&](const QString &, const QString &, const QString &name, const QJsonObject &value) {
            title = name; context = value; return true;
        });
        agenda.requestNote(snapshot["date"].toString(), snapshot["id"].toString(), snapshot["title"].toString(), snapshot);
        QCOMPARE(title, QString("Travaux dirigés"));
        QCOMPARE(context["event"].toObject()["start"].toString(), QString("2026-09-07T09:00:00+02:00"));
        QCOMPARE(context["event"].toObject()["subject"].toString(), QString("Électronique"));
        QCOMPARE(context["event"].toObject()["id"].toString(), QString("mycpe:stable-course-id"));
    }
    void dayContextHasNoInventedEventOrTimeAndMissingEventDoesNotCreate() {
        CalendarNetwork network; AgendaController agenda(nullptr, &network);
        QJsonObject context; int calls = 0;
        agenda.setNoteHandler([&](const QString &, const QString &, const QString &, const QJsonObject &value) {
            context = value; ++calls; return true;
        });
        agenda.requestNote("2026-10-25", "", "Notes du 2026-10-25");
        QCOMPARE(context, (QJsonObject{{"schemaVersion", 1}, {"kind", "day"},
            {"date", "2026-10-25"}, {"timeZone", "Europe/Paris"}}));
        agenda.reportNoteProgress("", false);
        agenda.requestNote("2026-10-25", "missing-event", "Événement disparu");
        QCOMPARE(calls, 1); QVERIFY(!agenda.noteBusy());
        QVERIFY(agenda.noteMessage().contains("plus disponibles"));
    }
    void dstAndAllDaySnapshotsKeepTheirCalendarSemantics() {
        CalendarNetwork network; AgendaController agenda(nullptr, &network);
        QJsonObject context;
        agenda.setNoteHandler([&](const QString &, const QString &, const QString &, const QJsonObject &value) {
            context = value; return true;
        });
        const auto parsed = ReAgenda::parseIcs("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:dst-note\r\n"
            "SUMMARY:Veille\r\nDTSTART;TZID=Europe/Paris:20261025T013000\r\nDTEND;TZID=Europe/Paris:20261025T033000\r\n"
            "END:VEVENT\r\nEND:VCALENDAR\r\n", "ical", QDate(2026,10,25), QDate(2026,10,25));
        QCOMPARE(parsed.events.size(), 1);
        auto snapshot = parsed.events.first().toVariant();
        agenda.requestNote("2026-10-25", snapshot["id"].toString(), snapshot["title"].toString(), snapshot);
        const auto event = context["event"].toObject();
        QCOMPARE(event["start"].toString(), QString("2026-10-25T01:30:00+02:00"));
        QCOMPARE(event["end"].toString(), QString("2026-10-25T03:30:00+01:00"));
        QCOMPARE(event["subject"].toString(), QString("Veille"));
        QVERIFY(!event["allDay"].toBool());
        agenda.reportNoteProgress("", false);
        const auto allDay = ReAgenda::parseIcs("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:day-note\r\n"
            "SUMMARY:Journée de cours\r\nDTSTART;VALUE=DATE:20261025\r\nDTEND;VALUE=DATE:20261026\r\n"
            "END:VEVENT\r\nEND:VCALENDAR\r\n", "ical", QDate(2026,10,25), QDate(2026,10,25));
        QCOMPARE(allDay.events.size(), 1);
        snapshot = allDay.events.first().toVariant();
        agenda.requestNote("2026-10-25", snapshot["id"].toString(), snapshot["title"].toString(), snapshot);
        QVERIFY(context["event"].toObject()["allDay"].toBool());
        QCOMPARE(context["event"].toObject()["end"].toString(), QString("2026-10-26T00:00:00+01:00"));
    }
};
QTEST_GUILESS_MAIN(AgendaControllerTest)
#include "AgendaControllerTest.moc"

#include "AgendaNoteContext.h"
#include <QTest>

class AgendaNoteContextTest : public QObject {
    Q_OBJECT
    QJsonObject context() const {
        return {{"schemaVersion", 1}, {"kind", "event"}, {"date", "2026-09-07"}, {"timeZone", "Europe/Paris"},
            {"event", QJsonObject{{"id", "source:event"}, {"title", "Cours de mathématiques – salle 2"},
                {"subject", "Mathématiques"}, {"start", "2026-09-07T09:00:00+02:00"},
                {"end", "2026-09-07T10:30:00+02:00"}, {"timeZone", "Europe/Paris"}, {"allDay", false}}}};
    }
private slots:
    void eventTitleAndFrenchHeader() {
        QJsonObject normalized;
        QVERIFY(AgendaNoteContext::normalize(context(), &normalized));
        const auto header = AgendaNoteContext::fields(normalized, "Fallback");
        QCOMPARE(header["title"].toString(), QString("Cours de mathématiques – salle 2"));
        QCOMPARE(header["day"].toString(), QString("Lundi"));
        QCOMPARE(header["date"].toString(), QString("07 / 09 / 2026"));
        QCOMPARE(header["time"].toString(), QString("09:00 – 10:30"));
    }
    void daylightSavingAndUtcInputs() {
        auto input = context(); auto event = input["event"].toObject();
        event["start"] = "2026-10-25T00:30:00Z";
        event["end"] = "2026-10-25T02:30:00Z";
        event.remove("subject"); input["event"] = event;
        QJsonObject normalized; QVERIFY(AgendaNoteContext::normalize(input, &normalized));
        const auto header = AgendaNoteContext::fields(normalized, "Fallback");
        QCOMPARE(header["day"].toString(), QString("Dimanche"));
        QCOMPARE(header["time"].toString(), QString("02:30 – 03:30"));
        QCOMPARE(header["title"], event["title"]);
    }
    void allDayAndDayNoteDoNotInventAnHour() {
        auto input = context(); auto event = input["event"].toObject();
        event["allDay"] = true; input["event"] = event;
        QJsonObject normalized; QVERIFY(AgendaNoteContext::normalize(input, &normalized));
        QCOMPARE(AgendaNoteContext::fields(normalized, "Fallback")["time"].toString(), QString("Toute la journée"));
        input["kind"] = "day"; input.remove("event");
        QVERIFY(AgendaNoteContext::normalize(input, &normalized));
        const auto header = AgendaNoteContext::fields(normalized, "Notes du jour");
        QVERIFY(header["time"].toString().isEmpty());
        QCOMPARE(header["title"].toString(), QString("Notes du jour"));
    }
    void overnightHasUnambiguousEndDate() {
        auto input = context(); auto event = input["event"].toObject();
        event["start"] = "2026-09-07T23:00:00+02:00";
        event["end"] = "2026-09-08T01:00:00+02:00"; input["event"] = event;
        QJsonObject normalized; QVERIFY(AgendaNoteContext::normalize(input, &normalized));
        QCOMPARE(AgendaNoteContext::fields(normalized, "Fallback")["time"].toString(), QString("23:00 – 08/09 01:00"));
    }
    void rejectsAmbiguousOrMalformedValues() {
        QJsonObject normalized;
        QVERIFY(!AgendaNoteContext::normalize(QJsonValue(), &normalized));
        auto input = context(); input["schemaVersion"] = 2;
        QVERIFY(!AgendaNoteContext::normalize(input, &normalized));
        input = context(); input["date"] = "2026-02-30";
        QVERIFY(!AgendaNoteContext::normalize(input, &normalized));
        input = context(); input["timeZone"] = "Missing/Zone";
        QVERIFY(!AgendaNoteContext::normalize(input, &normalized));
        input = context(); auto event = input["event"].toObject();
        event["start"] = "2026-09-07T09:00:00"; input["event"] = event;
        QVERIFY(!AgendaNoteContext::normalize(input, &normalized));
        event = context()["event"].toObject(); event["end"] = "2026-09-06T10:00:00+02:00";
        input["event"] = event; QVERIFY(!AgendaNoteContext::normalize(input, &normalized));
        event = context()["event"].toObject(); event["subject"] = QString(513, QChar('x'));
        input["event"] = event; QVERIFY(!AgendaNoteContext::normalize(input, &normalized));
    }
};
QTEST_GUILESS_MAIN(AgendaNoteContextTest)
#include "AgendaNoteContextTest.moc"

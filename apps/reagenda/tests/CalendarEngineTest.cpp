#include "CalendarEngine.h"
#include <QJsonObject>
#include <QtTest>

using namespace ReAgenda;
class CalendarEngineTest : public QObject {
    Q_OBJECT
    static QByteArray calendar(const QByteArray &events) {
        return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + events + "END:VCALENDAR\r\n";
    }
    static QByteArray event(const QByteArray &properties) {
        return "BEGIN:VEVENT\r\n" + properties + "END:VEVENT\r\n";
    }
  private slots:
    void floatingDatesUseParis() {
        const auto date = parseDateTime("2026-09-01T10:00:00", QTimeZone("Europe/Paris"));
        QCOMPARE(date.time(), QTime(10, 0));
        QCOMPARE(date.offsetFromUtc(), 7200);
        QCOMPARE(date.toUTC().time(), QTime(8, 0));
        QCOMPARE(parseDateTime("2026-12-01T10:00:00", QTimeZone("Europe/Paris")).offsetFromUtc(), 3600);
    }
    void explicitOffsetsArePreserved() {
        QCOMPARE(parseDateTime("2026-09-01T10:00:00Z", QTimeZone("Europe/Paris")).time(), QTime(12, 0));
        QCOMPARE(parseDateTime("20260901T100000Z", QTimeZone("Europe/Paris")).time(), QTime(12, 0));
        QCOMPARE(parseDateTime("2026-09-01T10:00:00+03:00", QTimeZone("Europe/Paris")).time(), QTime(9, 0));
    }
    void cpeNormalizesRealContract() {
        const QJsonArray items{QJsonObject{{"id", 12},
                                           {"date_debut", "2026-09-01T10:00:00"},
                                           {"date_fin", "2026-09-01T12:00:00"},
                                           {"matiere", "Maths"},
                                           {"ressource", "Amphi A"},
                                           {"salle", "Autre"},
                                           {"statut_intervention", "Séance annulée"}},
                               QJsonObject{{"is_break", true}}, QJsonObject{{"is_empty", true}},
                               QJsonObject{{"date_debut", "invalid"}, {"date_fin", "2026-09-01T12:00:00"}}};
        const auto result = parseCpe(items, "cpe", QDate(2026, 8, 31), QDate(2026, 9, 6));
        QCOMPARE(result.events.size(), 1);
        QCOMPARE(result.events[0].id, QString("cpe:12"));
        QCOMPARE(result.events[0].location, QString("Amphi A"));
        QCOMPARE(result.events[0].status, QString("CANCELLED"));
        QCOMPARE(result.events[0].start.toUTC().time(), QTime(8, 0));
        QVERIFY(!result.warnings.isEmpty());
    }
    void cpeMissingIdsRemainStableAcrossOrdering() {
        const QJsonObject a{
            {"date_debut", "2026-09-01T10:00:00"}, {"date_fin", "2026-09-01T12:00:00"}, {"matiere", "Maths"}};
        const QJsonObject b{{"date_debut", "2026-09-01T13:00:00"},
                            {"date_fin", "2026-09-01T14:00:00"},
                            {"matiere", "Électronique"}};
        const auto first = parseCpe({a, b}, "cpe", QDate(2026, 9, 1), QDate(2026, 9, 1));
        const auto second = parseCpe({b, a}, "cpe", QDate(2026, 9, 1), QDate(2026, 9, 1));
        QCOMPARE(first.events[0].id, second.events[1].id);
        QVERIFY(first.events[0].id != first.events[1].id);
    }
    void cpeDisplayFields_data() {
        QTest::addColumn<QJsonObject>("fields");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("room");
        QTest::addColumn<QString>("teacher");
        QTest::newRow("subject-with-room-only-label")
            << QJsonObject{{"matiere", QJsonValue::Null}, {"type_activite", QJsonValue::Null},
                           {"ressource", ""}, {"intervenants", QJsonValue::Null},
                           {"favori", QJsonObject{{"f2", " | A101 "}, {"f3", " Analyse des circuits "},
                                                 {"f4", " Enseignant test "}, {"f5", "Cours  "}}}}
            << QString("Analyse des circuits") << QString("A101") << QString("Enseignant test");
        QTest::newRow("event-label-before-generic-subject")
            << QJsonObject{{"favori", QJsonObject{{"f2", "Présentation du semestre | Amphi A"},
                                                 {"f3", "Pédagogique"}, {"f5", "Rentrée"}}}}
            << QString("Présentation du semestre") << QString("Amphi A") << QString();
        QTest::newRow("multiline-label-and-placeholder-subject")
            << QJsonObject{{"favori", QJsonObject{{"f2", "Atelier\r\nPrésence obligatoire  | Amphi B "},
                                                 {"f3", "_"}, {"f5", "Présentation"}}}}
            << QString("Atelier\r\nPrésence obligatoire") << QString("Amphi B") << QString();
        QTest::newRow("ordinary-fields-take-precedence")
            << QJsonObject{{"matiere", " Mathématiques "}, {"ressource", " B202 "},
                           {"intervenants", " Enseignant principal "},
                           {"favori", QJsonObject{{"f2", "Autre | A101"}, {"f3", "Autre matière"},
                                                 {"f4", "Autre enseignant"}}}}
            << QString("Mathématiques") << QString("B202") << QString("Enseignant principal");
        QTest::newRow("activity-without-subject")
            << QJsonObject{{"favori", QJsonObject{{"f2", " | "}, {"f3", " _ "}, {"f5", " Atelier "}}}}
            << QString("Atelier") << QString() << QString();
        QTest::newRow("event-label-without-room")
            << QJsonObject{{"favori", QJsonObject{{"f2", " Réunion "}, {"f3", "_"}}}}
            << QString("Réunion") << QString() << QString();
        QTest::newRow("invalid-display-values")
            << QJsonObject{{"favori", QJsonObject{{"f2", 42}, {"f3", QJsonObject{}}, {"f5", QJsonValue::Null}}}}
            << QString("Cours") << QString() << QString();
    }
    void cpeDisplayFields() {
        QFETCH(QJsonObject, fields);
        QFETCH(QString, title);
        QFETCH(QString, room);
        QFETCH(QString, teacher);
        fields["id"] = 123;
        fields["date_debut"] = "2026-09-01T10:00:00";
        fields["date_fin"] = "2026-09-01T12:00:00";
        const auto result = parseCpe({fields}, "cpe", QDate(2026, 9, 1), QDate(2026, 9, 1));
        QCOMPARE(result.events.size(), 1);
        const auto &event = result.events.first();
        QCOMPARE(event.id, QString("cpe:123"));
        QCOMPARE(event.title, title);
        QCOMPARE(event.location, room);
        QCOMPARE(event.teacher, teacher);
        QCOMPARE(event.toVariant().value("title").toString(), title);
    }
    void allDayEndIsExclusive() {
        const auto bytes = calendar(event("UID:holiday\r\nDTSTART;VALUE=DATE:20260901\r\nDTEND;VALUE=DATE:"
                                          "20260903\r\nSUMMARY:Vacances\r\n"));
        const auto yes = parseIcs(bytes, "ics", QDate(2026, 9, 2), QDate(2026, 9, 2));
        QCOMPARE(yes.events.size(), 1);
        QVERIFY(yes.events[0].allDay);
        QCOMPARE(parseIcs(bytes, "ics", QDate(2026, 9, 3), QDate(2026, 9, 3)).events.size(), 0);
    }
    void unfoldsEscapesAndIgnoresAlarms() {
        const auto bytes =
            calendar(event("UID:folded\r\nDTSTART:20260901T100000\r\nDTEND:20260901T110000\r\nSUMMARY:"
                           "Longue\r\n  séance\r\nDESCRIPTION:Ligne 1\\nLigne 2\\, texte\\; "
                           "fin\r\nBEGIN:VALARM\r\nDESCRIPTION:Ne remplace pas\r\nEND:VALARM\r\n"));
        const auto result = parseIcs(bytes, "ics", QDate(2026, 9, 1), QDate(2026, 9, 1));
        QCOMPARE(result.events.size(), 1);
        QCOMPARE(result.events[0].title, QString("Longue séance"));
        QCOMPARE(result.events[0].description, QString("Ligne 1\nLigne 2, texte; fin"));
    }
    void weeklyRecurrencePreservesWallClockAcrossDst() {
        const auto bytes =
            calendar(event("UID:dst\r\nDTSTART;TZID=Europe/Paris:20260323T090000\r\nDTEND;TZID=Europe/"
                           "Paris:20260323T100000\r\nRRULE:FREQ=WEEKLY;COUNT=3;BYDAY=MO\r\n"));
        const auto result = parseIcs(bytes, "ics", QDate(2026, 3, 23), QDate(2026, 4, 6));
        QCOMPARE(result.events.size(), 3);
        for (const auto &e : result.events)
            QCOMPARE(e.start.time(), QTime(9, 0));
        QCOMPARE(result.events[0].start.offsetFromUtc(), 3600);
        QCOMPARE(result.events[1].start.offsetFromUtc(), 7200);
    }
    void excludesAndMovesOccurrencesWithoutDuplicates() {
        const auto bytes = calendar(
            event("UID:repeat\r\nDTSTART;TZID=Europe/Paris:20260901T090000\r\nDTEND;TZID=Europe/"
                  "Paris:20260901T100000\r\nRRULE:FREQ=DAILY;COUNT=4\r\nEXDATE;TZID=Europe/"
                  "Paris:20260902T090000\r\n") +
            event("UID:repeat\r\nRECURRENCE-ID;TZID=Europe/Paris:20260903T090000\r\nDTSTART;TZID=Europe/"
                  "Paris:20260903T140000\r\nDTEND;TZID=Europe/Paris:20260903T150000\r\nSUMMARY:Déplacé\r\n"));
        const auto result = parseIcs(bytes, "ics", QDate(2026, 9, 1), QDate(2026, 9, 4));
        QCOMPARE(result.events.size(), 3);
        QCOMPARE(result.events[1].start.time(), QTime(14, 0));
        QVERIFY(result.events[1].id.contains("2026-09-03T07:00:00"));
    }
    void cancellationWithoutStartRemovesInstance() {
        const auto bytes =
            calendar(event("UID:cancel\r\nDTSTART:20260901T090000\r\nDTEND:20260901T100000\r\nRRULE:FREQ="
                           "DAILY;COUNT=2\r\n") +
                     event("UID:cancel\r\nRECURRENCE-ID:20260902T090000\r\nSTATUS:CANCELLED\r\n"));
        QCOMPARE(parseIcs(bytes, "ics", QDate(2026, 9, 1), QDate(2026, 9, 3)).events.size(), 1);
    }
    void monthlyOrdinalAndNegativeMonthday() {
        const auto ordinal = calendar(event("UID:nth\r\nDTSTART:20260908T090000\r\nDURATION:PT1H\r\nRRULE:"
                                            "FREQ=MONTHLY;BYDAY=2TU;COUNT=3\r\n"));
        const auto result = parseIcs(ordinal, "ics", QDate(2026, 9, 1), QDate(2026, 12, 1));
        QCOMPARE(result.events.size(), 3);
        QCOMPARE(result.events[1].start.date(), QDate(2026, 10, 13));
        QCOMPARE(result.events[2].start.date(), QDate(2026, 11, 10));
        const auto last = calendar(
            event("UID:last\r\nDTSTART;VALUE=DATE:20260930\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=-1;COUNT=3\r\n"));
        QCOMPARE(parseIcs(last, "ics", QDate(2026, 9, 1), QDate(2026, 12, 1)).events[1].start.date(),
                 QDate(2026, 10, 31));
    }
    void rdatesAndUntil() {
        const auto bytes = calendar(event("UID:rdates\r\nDTSTART:20260901T090000\r\nDURATION:PT1H\r\nRRULE:"
                                          "FREQ=DAILY;UNTIL=20260903T070000Z\r\nRDATE:20260910T090000\r\n"));
        const auto result = parseIcs(bytes, "ics", QDate(2026, 9, 1), QDate(2026, 9, 10));
        QCOMPARE(result.events.size(), 4);
        QCOMPARE(result.events.last().start.date(), QDate(2026, 9, 10));
    }
    void unsupportedRuleIsVisible() {
        const auto bytes = calendar(event("UID:unsupported\r\nDTSTART:20260901T090000\r\nDURATION:"
                                          "PT1H\r\nRRULE:FREQ=MINUTELY;COUNT=100\r\n"));
        const auto result = parseIcs(bytes, "ics", QDate(2026, 9, 1), QDate(2026, 9, 3));
        QCOMPARE(result.events.size(), 1);
        QVERIFY(!result.warnings.isEmpty());
    }
    void unknownTimeZoneIsNotSilentlyUtc() {
        const auto bytes =
            calendar(event("UID:badzone\r\nDTSTART;TZID=Unknown/Place:20260901T090000\r\nDURATION:PT1H\r\n"));
        const auto result = parseIcs(bytes, "ics", QDate(2026, 9, 1), QDate(2026, 9, 3));
        QCOMPARE(result.events.size(), 0);
        QVERIFY(!result.warnings.isEmpty());
    }
    void movedInstanceCanEnterWindow() {
        const auto bytes = calendar(
            event("UID:moved\r\nDTSTART:20260901T090000\r\nDURATION:PT1H\r\nRRULE:FREQ=DAILY;COUNT=2\r\n") +
            event("UID:moved\r\nRECURRENCE-ID:20260902T090000\r\nDTSTART:20260910T090000\r\nDURATION:"
                  "PT1H\r\n"));
        const auto result = parseIcs(bytes, "ics", QDate(2026, 9, 10), QDate(2026, 9, 10));
        QCOMPARE(result.events.size(), 1);
    }
    void windowsAndInputAreBounded() {
        QVERIFY(!parseIcs("bad", "ics", QDate(2026, 1, 1), QDate(2026, 1, 2)).warnings.isEmpty());
        QVERIFY(!parseIcs(calendar({}), "ics", QDate(2026, 1, 1), QDate(2028, 1, 1)).warnings.isEmpty());
    }
};
QTEST_GUILESS_MAIN(CalendarEngineTest)
#include "CalendarEngineTest.moc"

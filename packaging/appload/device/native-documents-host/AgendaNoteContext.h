#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

namespace AgendaNoteContext {
inline bool shortText(const QJsonValue &value, int limit, bool required = true) {
    if (!value.isString()) return !required && value.isUndefined();
    const auto text = value.toString();
    return text.size() <= limit && !text.contains(QChar::Null) && (!required || !text.trimmed().isEmpty());
}
inline QDateTime timestamp(const QJsonValue &value) {
    if (!value.isString() || value.toString().size() > 40) return {};
    static const QRegularExpression offset("(?:Z|[+-][0-9]{2}:[0-9]{2})$");
    if (!offset.match(value.toString()).hasMatch()) return {};
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}
inline bool normalize(const QJsonValue &value, QJsonObject *result) {
    if (!value.isObject()) return false;
    const auto input = value.toObject();
    const auto dateText = input["date"].toString();
    const auto date = QDate::fromString(dateText, Qt::ISODate);
    const auto kind = input["kind"].toString();
    if (input["schemaVersion"].toDouble() != 1 || !date.isValid() || date.toString(Qt::ISODate) != dateText
        || (kind != "day" && kind != "event") || !shortText(input["timeZone"], 128)) return false;
    const QTimeZone zone(input["timeZone"].toString().toUtf8());
    if (!zone.isValid()) return false;
    QJsonObject output{{"schemaVersion", 1}, {"kind", kind}, {"date", dateText},
                       {"timeZone", input["timeZone"]}};
    if (kind == "event") {
        if (!input["event"].isObject()) return false;
        const auto event = input["event"].toObject();
        if (!shortText(event["id"], 1024) || !shortText(event["title"], 512)
            || !shortText(event["subject"], 512, false) || !event["allDay"].isBool()
            || !shortText(event["timeZone"], 128) || event["timeZone"].toString() != input["timeZone"].toString()) return false;
        const auto start = timestamp(event["start"]), end = timestamp(event["end"]);
        if (!start.isValid() || !end.isValid() || end < start) return false;
        QJsonObject normalized{{"id", event["id"]}, {"title", event["title"].toString().simplified()},
            {"subject", event["subject"].toString().simplified()}, {"start", event["start"]},
            {"end", event["end"]}, {"timeZone", event["timeZone"]}, {"allDay", event["allDay"]}};
        output["event"] = normalized;
    } else if (input.contains("event")) return false;
    *result = output;
    return true;
}

inline QJsonObject fields(const QJsonObject &agenda, const QString &notebookTitle) {
    const auto event = agenda["event"].toObject();
    const QTimeZone zone(agenda["timeZone"].toString().toUtf8());
    const auto start = timestamp(event["start"]).toTimeZone(zone);
    const auto end = timestamp(event["end"]).toTimeZone(zone);
    const QDate date = agenda["kind"] == "event" ? start.date()
                                                 : QDate::fromString(agenda["date"].toString(), Qt::ISODate);
    const QLocale french(QLocale::French, QLocale::France);
    QString day = french.dayName(date.dayOfWeek(), QLocale::LongFormat);
    if (!day.isEmpty()) day[0] = day[0].toUpper();
    QString time;
    if (!event.isEmpty()) {
        if (event["allDay"].toBool()) time = QStringLiteral("Toute la journée");
        else {
            time = start.time().toString("HH:mm");
            if (end != start) time += QStringLiteral(" – ")
                + (start.date() == end.date() ? end.time().toString("HH:mm") : end.toString("dd/MM HH:mm"));
        }
    }
    QString title = event["title"].toString();
    if (title.isEmpty()) title = event["subject"].toString();
    if (title.isEmpty()) title = notebookTitle.simplified();
    return {{"title", title}, {"day", day}, {"date", date.toString("dd / MM / yyyy")},
            {"time", time}, {"isoDate", date.toString(Qt::ISODate)}};
}
}

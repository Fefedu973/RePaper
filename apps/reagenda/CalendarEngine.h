#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>
#include <QVariantList>
#include <QVector>

namespace ReAgenda {
struct Event {
    QString id;
    QString source;
    QString title;
    QString subject;
    QString description;
    QString location;
    QString teacher;
    QString status;
    QDateTime start;
    QDateTime end;
    bool allDay = false;
    QVariantMap toVariant() const;
};
struct CalendarResult {
    QVector<Event> events;
    QStringList warnings;
};
QDateTime parseDateTime(const QString &value, const QTimeZone &zone);
CalendarResult parseIcs(const QByteArray &bytes, const QString &source, const QDate &first, const QDate &last,
                        const QTimeZone &zone = QTimeZone("Europe/Paris"));
CalendarResult parseCpe(const QJsonArray &items, const QString &source, const QDate &first,
                        const QDate &last);
QString stableId(const QStringList &parts);
} // namespace ReAgenda

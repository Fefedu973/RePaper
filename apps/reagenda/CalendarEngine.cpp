#include "CalendarEngine.h"
#include <QCryptographicHash>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace ReAgenda {
QString stableId(const QStringList &parts) {
    QByteArray packed;
    for (const QString &part : parts) {
        const QByteArray value = part.toUtf8();
        packed += QByteArray::number(value.size()) + ':' + value;
    }
    return QString::fromLatin1(QCryptographicHash::hash(packed, QCryptographicHash::Sha256).toHex());
}
QVariantMap Event::toVariant() const {
    return {{"id", id},
            {"source", source},
            {"title", title},
            {"subject", subject.isEmpty() ? title : subject},
            {"description", description},
            {"location", location},
            {"teacher", teacher},
            {"status", status},
            {"start", start.toString(Qt::ISODate)},
            {"end", end.toString(Qt::ISODate)},
            {"timeZone", QString::fromUtf8(start.timeZone().id())},
            {"date", start.date().toString(Qt::ISODate)},
            {"time", allDay ? QStringLiteral("Toute la journée")
                            : start.toString("HH:mm") + QStringLiteral(" – ") + end.toString("HH:mm")},
            {"allDay", allDay},
            {"cancelled", status == "CANCELLED"}};
}
QDateTime parseDateTime(const QString &value, const QTimeZone &zone) {
    const QString text = value.trimmed();
    if (text.isEmpty())
        return {};
    const bool utc = text.endsWith('Z');
    QDateTime parsed;
    if (text.contains('-'))
        parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    else if (text.size() == 8)
        return QDateTime(QDate::fromString(text, "yyyyMMdd"), QTime(0, 0), zone);
    else
        parsed = QDateTime::fromString(utc ? text.chopped(1) : text, "yyyyMMdd'T'HHmmss");
    if (!parsed.isValid())
        return {};
    const bool offset = QRegularExpression("[+-]\\d{2}:?\\d{2}$").match(text).hasMatch();
    if (utc)
        parsed.setTimeSpec(Qt::UTC);
    else if (!offset)
        parsed = QDateTime(parsed.date(), parsed.time(), zone);
    return parsed.isValid() ? parsed.toTimeZone(zone) : QDateTime();
}
static bool intersects(const Event &event, const QDate &first, const QDate &last) {
    return event.start.date() <= last && event.end > QDateTime(first, QTime(0, 0), event.start.timeZone());
}
CalendarResult parseCpe(const QJsonArray &items, const QString &source, const QDate &first,
                        const QDate &last) {
    CalendarResult result;
    const QTimeZone paris("Europe/Paris");
    QSet<QString> seen;
    for (const auto &item : items) {
        if (!item.isObject()) {
            result.warnings << "Événement CPE invalide ignoré.";
            continue;
        }
        const auto raw = item.toObject();
        if (raw.value("is_break").toBool() || raw.value("is_empty").toBool())
            continue;
        Event e;
        e.source = source;
        e.start = parseDateTime(raw.value("date_debut").toString(), paris);
        e.end = parseDateTime(raw.value("date_fin").toString(), paris);
        if (!e.start.isValid() || !e.end.isValid() || e.end <= e.start) {
            result.warnings << "Une séance CPE contient des dates invalides.";
            continue;
        }
        // Some My CPE accounts receive display fields only in `favori`:
        // f2 = event label | room, f3 = subject, f4 = teachers, f5 = activity.
        // Keep the ordinary fields authoritative when both forms are present.
        const auto display = raw.value("favori").toObject();
        const QString labelAndRoom = display.value("f2").toString();
        const int separator = labelAndRoom.lastIndexOf('|');
        const QString eventLabel =
            (separator < 0 ? labelAndRoom : labelAndRoom.left(separator)).trimmed();
        QString subject = display.value("f3").toString().trimmed();
        if (subject == "_") // Observed placeholder for events without a subject.
            subject.clear();
        e.subject = raw.value("matiere").toString().trimmed();
        if (e.subject.isEmpty())
            e.subject = subject;
        e.title = raw.value("matiere").toString().trimmed();
        if (e.title.isEmpty())
            e.title = eventLabel;
        if (e.title.isEmpty())
            e.title = subject;
        if (e.title.isEmpty())
            e.title = raw.value("type_activite").toString().trimmed();
        if (e.title.isEmpty())
            e.title = display.value("f5").toString().trimmed();
        if (e.title.isEmpty())
            e.title = "Cours";
        e.description = raw.value("description").toString();
        e.teacher = raw.value("intervenants").toString().trimmed();
        if (e.teacher.isEmpty())
            e.teacher = display.value("f4").toString().trimmed();
        e.location = raw.value("ressource").toString().trimmed();
        if (e.location.isEmpty())
            e.location = raw.value("salle").toString().trimmed();
        if (e.location.isEmpty() && separator >= 0)
            e.location = labelAndRoom.mid(separator + 1).trimmed();
        e.status = raw.value("statut_intervention").toString();
        const QString status = e.status.normalized(QString::NormalizationForm_D)
                                   .remove(QRegularExpression("[\\x{0300}-\\x{036f}]"))
                                   .toLower();
        if (QRegularExpression("annul|cancel|supprim").match(status).hasMatch())
            e.status = "CANCELLED";
        const auto id = raw.value("id");
        e.id =
            source + ':' +
            (id.isString() || id.isDouble() ? id.toVariant().toString()
                                            : stableId({e.start.toString(Qt::ISODate),
                                                        e.end.toString(Qt::ISODate), e.title, e.location}));
        if (intersects(e, first, last) && !seen.contains(e.id)) {
            result.events << e;
            seen.insert(e.id);
        }
    }
    return result;
}

namespace {
struct Property {
    QString value;
    QMap<QString, QString> params;
};
using Component = QMap<QString, QVector<Property>>;
QString unescape(QString text) {
    QString result;
    bool escaped = false;
    for (QChar ch : text) {
        if (escaped) {
            result += (ch == 'n' || ch == 'N') ? QChar('\n') : ch;
            escaped = false;
        } else if (ch == '\\')
            escaped = true;
        else
            result += ch;
    }
    if (escaped)
        result += '\\';
    return result;
}
Property property(const Component &c, const QString &name) {
    return c.value(name).value(0);
}
QDateTime dateProperty(const Property &p, const QTimeZone &fallback) {
    const QString tz = p.params.value("TZID");
    const QTimeZone zone = tz.isEmpty() ? fallback : QTimeZone(tz.toUtf8());
    if (!zone.isValid())
        return {};
    return parseDateTime(p.value, zone);
}
QString recurrenceKey(const QDateTime &date) {
    return date.toUTC().toString(Qt::ISODateWithMs);
}
Event makeEvent(const Component &c, const QString &source, const QTimeZone &zone) {
    Event event;
    event.id = property(c, "UID").value;
    event.source = source;
    event.title = unescape(property(c, "SUMMARY").value);
    if (event.title.isEmpty())
        event.title = "Événement";
    event.description = unescape(property(c, "DESCRIPTION").value);
    event.location = unescape(property(c, "LOCATION").value);
    event.status = property(c, "STATUS").value.toUpper();
    const Property begin = property(c, "DTSTART");
    event.allDay = begin.params.value("VALUE") == "DATE" || begin.value.size() == 8;
    event.start = dateProperty(begin, zone);
    event.end = dateProperty(property(c, "DTEND"), event.start.timeZone());
    if (!event.end.isValid()) {
        const QString duration = property(c, "DURATION").value;
        const auto match =
            QRegularExpression("^P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$")
                .match(duration);
        if (match.hasMatch()) {
            const qint64 days = match.captured(1).toLongLong() * 7 + match.captured(2).toLongLong();
            const qint64 seconds = match.captured(3).toLongLong() * 3600 +
                                   match.captured(4).toLongLong() * 60 + match.captured(5).toLongLong();
            event.end =
                days <= 3660 && seconds <= 864000 ? event.start.addDays(days).addSecs(seconds) : QDateTime();
        } else if (duration.isEmpty())
            event.end = event.allDay ? event.start.addDays(1) : event.start;
    }
    if (event.id.isEmpty())
        event.id = stableId({event.title, event.start.toString(Qt::ISODate), event.location});
    return event;
}
bool matchesDay(const QDate &date, const QString &rule) {
    static const QStringList names{"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    const auto match = QRegularExpression("^([+-]?\\d{1,2})?(MO|TU|WE|TH|FR|SA|SU)$").match(rule);
    if (!match.hasMatch() || names.indexOf(match.captured(2)) + 1 != date.dayOfWeek())
        return false;
    if (match.captured(1).isEmpty())
        return true;
    const int nth = match.captured(1).toInt();
    return nth > 0 ? (date.day() - 1) / 7 + 1 == nth : -((date.daysInMonth() - date.day()) / 7 + 1) == nth;
}
bool matchesRule(const QDate &date, const QDate &origin, const QMap<QString, QString> &rule, int interval) {
    const QString freq = rule.value("FREQ");
    const int days = origin.daysTo(date);
    const int months = (date.year() - origin.year()) * 12 + date.month() - origin.month();
    if (freq == "DAILY" && days % interval)
        return false;
    if (freq == "WEEKLY") {
        const QDate week = origin.addDays(1 - origin.dayOfWeek());
        if ((week.daysTo(date) / 7) % interval)
            return false;
        if (!rule.contains("BYDAY") && date.dayOfWeek() != origin.dayOfWeek())
            return false;
    }
    if (freq == "MONTHLY" && months % interval)
        return false;
    if (freq == "YEARLY" && (date.year() - origin.year()) % interval)
        return false;
    if (freq == "YEARLY" && !rule.contains("BYMONTH") && date.month() != origin.month())
        return false;
    if ((freq == "MONTHLY" || freq == "YEARLY") && !rule.contains("BYDAY") && !rule.contains("BYMONTHDAY") &&
        date.day() != origin.day())
        return false;
    if (rule.contains("BYMONTH") && !rule.value("BYMONTH").split(',').contains(QString::number(date.month())))
        return false;
    if (rule.contains("BYMONTHDAY")) {
        bool hit = false;
        for (const QString &part : rule.value("BYMONTHDAY").split(',')) {
            const int day = part.toInt();
            hit |= day > 0 ? date.day() == day : date.day() == date.daysInMonth() + day + 1;
        }
        if (!hit)
            return false;
    }
    if (rule.contains("BYDAY")) {
        bool hit = false;
        for (const QString &part : rule.value("BYDAY").split(','))
            hit |= matchesDay(date, part);
        if (!hit)
            return false;
    }
    return true;
}
} // namespace

CalendarResult parseIcs(const QByteArray &bytes, const QString &source, const QDate &first, const QDate &last,
                        const QTimeZone &zone) {
    CalendarResult result;
    if (!first.isValid() || !last.isValid() || first > last || first.daysTo(last) > 400 ||
        bytes.size() > 8 * 1024 * 1024 || !zone.isValid()) {
        result.warnings << "Calendrier ou fenêtre de lecture invalide (400 jours maximum).";
        return result;
    }
    QString input = QString::fromUtf8(bytes);
    input.replace("\r\n", "\n");
    input.replace(QRegularExpression("\n[ \\t]"), "");
    if (!input.contains("BEGIN:VCALENDAR")) {
        result.warnings << "Le fichier n'est pas un calendrier ICS.";
        return result;
    }
    QVector<Component> components;
    Component current;
    bool inside = false;
    int nested = 0;
    for (const QString &line : input.split('\n')) {
        if (line == "BEGIN:VEVENT") {
            current.clear();
            inside = true;
            nested = 0;
            continue;
        }
        if (line == "END:VEVENT" && inside) {
            components << current;
            inside = false;
            if (components.size() > 10000)
                break;
            continue;
        }
        if (!inside)
            continue;
        if (line.startsWith("BEGIN:")) {
            ++nested;
            continue;
        }
        if (line.startsWith("END:")) {
            --nested;
            continue;
        }
        if (nested)
            continue;
        const int colon = line.indexOf(':');
        if (colon < 1)
            continue;
        const auto fields = line.left(colon).split(';');
        Property p;
        p.value = line.mid(colon + 1);
        for (int i = 1; i < fields.size(); ++i) {
            const int equals = fields[i].indexOf('=');
            if (equals > 0)
                p.params.insert(fields[i].left(equals).toUpper(), fields[i].mid(equals + 1).remove('"'));
        }
        current[fields.first().toUpper()] << p;
    }
    QMap<QString, QMap<QString, Component>> exceptions;
    for (const auto &c : components) {
        if (!c.contains("RECURRENCE-ID"))
            continue;
        const auto p = property(c, "RECURRENCE-ID");
        if (p.params.contains("RANGE")) {
            result.warnings
                << "Exception ICS RANGE non prise en charge ; seule cette occurrence est appliquée.";
        }
        exceptions[property(c, "UID").value].insert(recurrenceKey(dateProperty(p, zone)), c);
    }
    QSet<QString> added;
    int expansionBudget = 200000;
    auto append = [&](Event event, const QDateTime &original) {
        if (!event.allDay) {
            event.start = event.start.toTimeZone(zone);
            event.end = event.end.toTimeZone(zone);
        }
        event.id = source + ':' + event.id + ':' + recurrenceKey(original);
        if (result.events.size() >= 20000) {
            result.warnings << "Calendrier volumineux : affichage limité à 20 000 événements.";
            return;
        }
        if (!added.contains(event.id) && intersects(event, first, last)) {
            result.events << event;
            added.insert(event.id);
        }
    };
    for (const auto &c : components) {
        if (c.contains("RECURRENCE-ID"))
            continue;
        Event base = makeEvent(c, source, zone);
        if (!base.start.isValid() || !base.end.isValid() || base.end < base.start) {
            result.warnings << "Événement ICS avec date ou fuseau invalide ignoré.";
            continue;
        }
        QSet<QString> excluded;
        for (const auto &p : c.value("EXDATE"))
            for (const QString &value : p.value.split(',')) {
                Property single = p;
                single.value = value;
                excluded.insert(recurrenceKey(dateProperty(single, base.start.timeZone())));
            }
        auto occurrence = [&](const QDateTime &start) {
            const QString key = recurrenceKey(start);
            if (excluded.contains(key))
                return;
            const auto changes = exceptions.value(base.id);
            if (changes.contains(key)) {
                const Component changed = changes.value(key);
                Event event = makeEvent(changed, source, base.start.timeZone());
                if (event.status == "CANCELLED")
                    return;
                if (event.start.isValid() && event.end.isValid())
                    append(event, start);
                return;
            }
            Event event = base;
            event.start = start;
            event.end = base.allDay ? start.addDays(base.start.date().daysTo(base.end.date()))
                                    : start.addSecs(base.start.secsTo(base.end));
            append(event, start);
        };
        const QString recurrence = property(c, "RRULE").value;
        occurrence(base.start);
        if (!recurrence.isEmpty()) {
            QMap<QString, QString> rule;
            for (const QString &part : recurrence.toUpper().split(';')) {
                const int eq = part.indexOf('=');
                if (eq > 0)
                    rule.insert(part.left(eq), part.mid(eq + 1));
            }
            const QSet<QString> supported{"FREQ",  "INTERVAL",   "COUNT",   "UNTIL",
                                          "BYDAY", "BYMONTHDAY", "BYMONTH", "WKST"};
            bool valid = QStringList{"DAILY", "WEEKLY", "MONTHLY", "YEARLY"}.contains(rule.value("FREQ"));
            for (auto it = rule.cbegin(); it != rule.cend(); ++it)
                valid &= supported.contains(it.key());
            valid &= !rule.contains("WKST") || rule.value("WKST") == "MO";
            if ((rule.value("FREQ") == "WEEKLY" || rule.value("FREQ") == "DAILY") &&
                rule.value("BYDAY").contains(QRegularExpression("[0-9]")))
                valid = false;
            if (rule.value("FREQ") == "YEARLY" && rule.contains("BYDAY") && !rule.contains("BYMONTH"))
                valid = false;
            const int interval = rule.value("INTERVAL", "1").toInt();
            const int count = rule.value("COUNT", "10000").toInt();
            valid &= interval >= 1 && interval <= 366 && count >= 1;
            if (!valid) {
                result.warnings
                    << "Une règle ICS n'est pas prise en charge ; seule sa première séance est visible.";
            } else {
                const QDateTime until = parseDateTime(rule.value("UNTIL"), base.start.timeZone());
                int emitted = 1;
                const QDate limit = std::min(last, base.start.date().addDays(36600));
                if (last > limit)
                    result.warnings << "Récurrence trop ancienne : expansion limitée à 100 ans.";
                for (QDate date = base.start.date().addDays(1);
                     date <= limit && emitted < count && emitted < 10000; date = date.addDays(1)) {
                    if (--expansionBudget < 0) {
                        result.warnings
                            << "Calendrier complexe : limite d'expansion des récurrences atteinte.";
                        break;
                    }
                    if (!matchesRule(date, base.start.date(), rule, interval))
                        continue;
                    const QDateTime start(date, base.start.time(), base.start.timeZone());
                    if (!start.isValid())
                        continue;
                    if (until.isValid() && start > until)
                        break;
                    ++emitted;
                    occurrence(start);
                }
            }
        }
        for (const auto &p : c.value("RDATE"))
            for (const QString &value : p.value.split(',')) {
                Property single = p;
                single.value = value;
                const auto start = dateProperty(single, base.start.timeZone());
                if (start.isValid())
                    occurrence(start);
            }
    }
    // Detached instances may be supplied without the master (e.g. a bounded CalDAV response).
    for (const auto &c : components) {
        if (!c.contains("RECURRENCE-ID"))
            continue;
        Event event = makeEvent(c, source, zone);
        if (event.status == "CANCELLED" || !event.start.isValid() || !event.end.isValid())
            continue;
        const auto original = dateProperty(property(c, "RECURRENCE-ID"), event.start.timeZone());
        if (original.isValid())
            append(event, original);
    }
    std::sort(result.events.begin(), result.events.end(),
              [](const Event &a, const Event &b) { return a.start < b.start; });
    result.warnings.removeDuplicates();
    return result;
}
} // namespace ReAgenda

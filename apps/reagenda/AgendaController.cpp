#include "AgendaController.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <memory>

namespace {
constexpr int MaxResponse = 8 * 1024 * 1024;
const QString CpeBase = "https://mycpe.cpe.fr/mobile/";
const QString CpeId = "mycpe";
QString decimal(const QJsonValue &value) {
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QLocale(QLocale::French).toString(value.toDouble(), 'g', 8);
    return QStringLiteral("Non disponible");
}
} // namespace

AgendaController::AgendaController(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_network(network ? network : &m_ownedNetwork) {
    m_refreshClock.start();
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(250);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] { refreshCalendars(false); });
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(directory);
    QFile::setPermissions(directory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    m_statePath = directory + "/agenda-cache.json";
    QFile file(m_statePath);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 24 * 1024 * 1024) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        m_sources = object.value("sources").toArray();
        m_grades = object.value("grades").toArray();
        m_absences = object.value("absences").toObject();
    }
    m_cpeToken = m_secrets.get("cpe-token");
    m_configuration = QJsonDocument::fromJson(m_secrets.get("cpe-configuration")).object();
    rebuild();
    if (!m_sources.isEmpty()) {
        setMessage({});
        m_refreshTimer.start();
    }
    else
        setMessage("Ajoutez un calendrier pour commencer.");
}
void AgendaController::setMessage(const QString &message) {
    m_message = message;
    emit messageChanged();
}
QPair<QDate, QDate> AgendaController::range() const {
    if (m_view == "day")
        return {m_date, m_date};
    QDate start = m_view == "month" ? QDate(m_date.year(), m_date.month(), 1) : m_date;
    start = start.addDays(1 - start.dayOfWeek());
    return {start, start.addDays(m_view == "month" ? 41 : 6)};
}
QString AgendaController::periodLabel() const {
    const QLocale locale(QLocale::French);
    if (m_view == "month")
        return locale.toString(m_date, "MMMM yyyy");
    if (m_view == "day")
        return locale.toString(m_date, "dddd d MMMM yyyy");
    const auto dates = range();
    return locale.toString(dates.first, "d MMM") + " – " + locale.toString(dates.second, "d MMM yyyy");
}
QVariantList AgendaController::days() const {
    QVariantList result;
    const auto dates = range();
    const QLocale locale(QLocale::French);
    for (QDate date = dates.first; date <= dates.second; date = date.addDays(1)) {
        QVariantList items;
        bool uncached = false;
        const auto cpe = source(CpeId);
        if (!cpe.isEmpty())
            uncached = !cpe.value("cachedWeeks")
                            .toArray()
                            .contains(date.addDays(1 - date.dayOfWeek()).toString(Qt::ISODate));
        const QDateTime midnight(date, QTime(0, 0), QTimeZone("Europe/Paris"));
        for (const auto &event : m_events) {
            if (event.start < midnight.addDays(1) &&
                (event.end > midnight || (event.start == event.end && event.start >= midnight)))
                items << event.toVariant();
        }
        result << QVariantMap{{"date", date.toString(Qt::ISODate)},
                              {"dayNumber", date.day()},
                              {"label", locale.toString(date, m_view == "day" ? "dddd d MMMM" : "ddd d")},
                              {"today", date == QDate::currentDate()},
                              {"inMonth", date.month() == m_date.month()},
                              {"events", items},
                              {"uncached", uncached}};
    }
    return result;
}
QVariantList AgendaController::sources() const {
    QVariantList result;
    for (const auto &value : m_sources) {
        const auto s = value.toObject();
        result << QVariantMap{{"id", s.value("id").toString()},
                              {"label", s.value("label").toString()},
                              {"kind", s.value("kind").toString()},
                              {"updated", s.value("updated").toString()},
                              {"error", s.value("error").toString()},
                              {"needsLogin", s.value("kind") == "cpe" && m_cpeToken.isEmpty()}};
    }
    return result;
}
void AgendaController::setView(const QString &view) {
    if ((view != "day" && view != "week" && view != "month") || view == m_view)
        return;
    const auto previous = range();
    m_view = view;
    rebuild();
    scheduleRefresh(previous);
}
void AgendaController::selectDate(const QString &date) {
    const auto parsed = QDate::fromString(date, Qt::ISODate);
    if (parsed.isValid() && parsed != m_date) {
        const auto previous = range();
        m_date = parsed;
        rebuild();
        scheduleRefresh(previous);
    }
}
void AgendaController::shift(int direction) {
    if (direction != -1 && direction != 1)
        return;
    const auto previous = range();
    m_date = m_view == "month" ? m_date.addMonths(direction)
                               : m_date.addDays(direction * (m_view == "week" ? 7 : 1));
    rebuild();
    scheduleRefresh(previous);
}
void AgendaController::today() {
    selectDate(QDate::currentDate().toString(Qt::ISODate));
}
void AgendaController::scheduleRefresh(const QPair<QDate, QDate> &previousRange) {
    if (previousRange == range())
        return;
    // Invalidate immediately, before the debounce: a late response must not commit
    // or display an error for the period the user has already left.
    const auto currentRange = range();
    const auto weekStart = [](QDate date) { return date.addDays(1 - date.dayOfWeek()); };
    if (weekStart(previousRange.first) != weekStart(currentRange.first) ||
        weekStart(previousRange.second) != weekStart(currentRange.second))
        cancelRefresh(CpeId);
    m_refreshTimer.start();
}
void AgendaController::cancelRefresh(const QString &id) {
    ++m_refreshGenerations[id];
    m_refreshWindows.remove(id);
    const auto reply = m_calendarReplies.take(id);
    if (reply)
        reply->abort();
}
void AgendaController::finishRefresh(const QString &id, quint64 generation) {
    if (m_refreshGenerations.value(id) != generation)
        return;
    m_refreshWindows.remove(id);
    m_calendarReplies.remove(id);
}
bool AgendaController::recentlyRefreshed(const QString &key) const {
    return m_lastRefresh.contains(key) && m_refreshClock.elapsed() - m_lastRefresh.value(key) < 30000;
}
QJsonObject AgendaController::source(const QString &id) const {
    for (const auto &value : m_sources)
        if (value.toObject().value("id") == id)
            return value.toObject();
    return {};
}
void AgendaController::putSource(const QJsonObject &item) {
    for (int i = 0; i < m_sources.size(); ++i)
        if (m_sources[i].toObject().value("id") == item.value("id")) {
            m_sources[i] = item;
            emit sourcesChanged();
            return;
        }
    m_sources << item;
    emit sourcesChanged();
}
void AgendaController::sourceError(const QString &id, const QString &message) {
    auto s = source(id);
    if (s.isEmpty())
        return;
    s["error"] = message;
    putSource(s);
    setMessage(message);
}
void AgendaController::save() {
    QSaveFile file(m_statePath);
    m_storageError.clear();
    if (!file.open(QIODevice::WriteOnly)) {
        m_storageError = "Impossible d'enregistrer le cache sur cet appareil.";
        emit messageChanged();
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 1},
                                                      {"sources", m_sources},
                                                      {"grades", m_grades},
                                                      {"absences", m_absences}})
                                .toJson(QJsonDocument::Compact);
    if (data.size() > 24 * 1024 * 1024) {
        m_storageError = "Cache non enregistré : limite totale de 24 Mio atteinte. Retirez un calendrier.";
        emit messageChanged();
        return;
    }
    if (file.write(data) != data.size() || !file.commit())
        m_storageError =
            "Échec d'enregistrement du cache : les derniers changements ne sont pas sauvegardés.";
    emit messageChanged();
}
void AgendaController::rebuild() {
    m_events.clear();
    const auto dates = range();
    QStringList warnings;
    for (const auto &value : m_sources) {
        const auto s = value.toObject();
        const QString id = s.value("id").toString();
        ReAgenda::CalendarResult result;
        if (s.value("kind") == "cpe")
            result = ReAgenda::parseCpe(s.value("events").toArray(), id, dates.first, dates.second);
        else
            result = ReAgenda::parseIcs(QByteArray::fromBase64(s.value("ics").toString().toLatin1()), id,
                                        dates.first, dates.second);
        m_events += result.events;
        warnings += result.warnings;
    }
    std::sort(m_events.begin(), m_events.end(),
              [](const auto &a, const auto &b) { return a.start < b.start; });
    warnings.removeDuplicates();
    m_parseWarnings = warnings;
    emit messageChanged();
    emit calendarChanged();
}
QNetworkReply *AgendaController::request(const QUrl &url, const QByteArray &body, const QByteArray &token,
                               Completion done) {
    if (url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        done({}, 0, "Une adresse HTTPS sans identifiants intégrés est requise.");
        return nullptr;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json, text/calendar;q=0.9");
    request.setRawHeader("User-Agent", "reAgenda/0.1");
    if (!token.isEmpty()) {
        if (url.host() != "mycpe.cpe.fr" || url.port(443) != 443 || !url.path().startsWith("/mobile/")) {
            done({}, 0, "Origine CPE refusée.");
            return nullptr;
        }
        request.setRawHeader("Authorization", "Bearer " + token);
    }
    if (!body.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = body.isEmpty() ? m_network->get(request) : m_network->post(request, body);
    ++m_pending;
    emit busyChanged();
    auto data = std::make_shared<QByteArray>();
    auto failure = std::make_shared<QString>();
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    timer->start(30000);
    connect(timer, &QTimer::timeout, reply, [reply, failure] {
        *failure = "Le service n'a pas répondu dans le délai de 30 secondes.";
        reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, reply, [reply, data, failure] {
        if (data->size() + reply->bytesAvailable() > MaxResponse) {
            *failure = "La réponse dépasse la limite de 8 Mio.";
            reply->abort();
        } else
            *data += reply->readAll();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, timer, data, failure, done = std::move(done)] {
                timer->stop();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (failure->isEmpty()) {
                    if (status == 401 || status == 403)
                        *failure = "Session refusée ou expirée. Reconnectez votre compte.";
                    else if (status >= 300 && status < 400)
                        *failure = "Le service demande une redirection. Utilisez l'adresse HTTPS finale.";
                    else if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
                        *failure = "Impossible de récupérer les données du service.";
                    else if (data->size() + reply->bytesAvailable() > MaxResponse)
                        *failure = "La réponse dépasse la limite de 8 Mio.";
                    else
                        *data += reply->readAll();
                }
                --m_pending;
                emit busyChanged();
                reply->deleteLater();
                done(*data, status, *failure);
            });
    return reply;
}
void AgendaController::addIcs(const QString &label, const QString &location) {
    if (m_sources.size() >= 12) {
        setMessage("La limite de 12 calendriers est atteinte.");
        return;
    }
    const QString input = location.trimmed();
    const QUrl url(input);
    QJsonObject s{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                  {"label", label.trimmed().isEmpty() ? "Calendrier ICS" : label.trimmed()},
                  {"kind", "ics"}};
    if (url.scheme() == "https") {
        if (!url.userInfo().isEmpty()) {
            setMessage("Utilisez une URL HTTPS sans identifiants dans l'adresse.");
            return;
        }
        if (!m_secrets.put("ics-url-" + s.value("id").toString(), input.toUtf8())) {
            setMessage("Impossible de protéger l'adresse du calendrier sur cet appareil.");
            return;
        }
        s["remote"] = true;
        putSource(s);
        refreshSource(s.value("id").toString());
    } else {
        const QString path = url.isLocalFile() ? url.toLocalFile() : input;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > MaxResponse) {
            setMessage("Fichier ICS inaccessible ou supérieur à 8 Mio.");
            return;
        }
        const QByteArray bytes = file.readAll();
        if (!bytes.contains("BEGIN:VCALENDAR")) {
            setMessage("Ce fichier n'est pas un calendrier ICS.");
            return;
        }
        s["ics"] = QString::fromLatin1(bytes.toBase64());
        s["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        putSource(s);
        save();
        rebuild();
        setMessage("Calendrier ajouté.");
    }
}
void AgendaController::removeSource(const QString &id) {
    cancelRefresh(id);
    if (id == CpeId)
        disconnectCpe();
    m_secrets.remove("ics-url-" + id);
    if (id == CpeId) {
        m_grades = {};
        m_absences = {};
        emit schoolDataChanged();
    }
    for (int i = m_sources.size() - 1; i >= 0; --i)
        if (m_sources[i].toObject().value("id") == id)
            m_sources.removeAt(i);
    save();
    rebuild();
    emit sourcesChanged();
}
void AgendaController::refresh() {
    m_refreshTimer.stop();
    refreshCalendars(true);
}
void AgendaController::refreshCalendars(bool force) {
    if (!m_sources.isEmpty())
        setMessage("Actualisation des calendriers…");
    const auto currentSources = m_sources;
    for (const auto &value : currentSources)
        refreshSource(value.toObject().value("id").toString(), force);
    if (!busy() && m_message == "Actualisation des calendriers…")
        setMessage({});
}
void AgendaController::refreshSource(const QString &id, bool force) {
    const auto s = source(id);
    if (s.value("kind") == "cpe") {
        refreshCpe(id, force);
        return;
    }
    const QUrl url(QString::fromUtf8(m_secrets.get("ics-url-" + id)));
    if (url.isEmpty())
        return;
    if (m_refreshWindows.contains(id) || (!force && recentlyRefreshed(id)))
        return;
    const quint64 generation = ++m_refreshGenerations[id];
    m_refreshWindows[id] = id;
    m_calendarReplies[id] = request(url, {}, {}, [this, id, generation](const QByteArray &bytes, int, const QString &error) {
        if (source(id).isEmpty() || generation != m_refreshGenerations.value(id))
            return;
        finishRefresh(id, generation);
        if (!error.isEmpty()) {
            sourceError(id, error);
            return;
        }
        if (!bytes.contains("BEGIN:VCALENDAR")) {
            sourceError(id, "La réponse n'est pas un calendrier ICS.");
            return;
        }
        auto updated = source(id);
        updated["ics"] = QString::fromLatin1(bytes.toBase64());
        updated["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        updated.remove("error");
        m_lastRefresh[id] = m_refreshClock.elapsed();
        putSource(updated);
        save();
        rebuild();
        setMessage({});
    });
}
void AgendaController::connectCpe(const QString &username, const QString &password) {
    if (busy())
        return;
    if (username.trimmed().isEmpty() || password.isEmpty()) {
        setMessage("Saisissez votre identifiant et votre mot de passe CPE.");
        return;
    }
    const int generation = ++m_cpeGeneration;
    const QByteArray body = QJsonDocument(QJsonObject{{"login", username.trimmed()}, {"password", password}})
                                .toJson(QJsonDocument::Compact);
    request(QUrl(CpeBase + "login"), body, {},
            [this, generation](const QByteArray &bytes, int, const QString &error) {
                if (generation != m_cpeGeneration)
                    return;
                if (!error.isEmpty()) {
                    setMessage(error);
                    return;
                }
                const QByteArray token =
                    QJsonDocument::fromJson(bytes).object().value("normal").toString().trimmed().toUtf8();
                if (token.isEmpty() || token.size() > 16384 || token.contains('\r') || token.contains('\n')) {
                    setMessage("Réponse de connexion CPE invalide.");
                    return;
                }
                request(
                    QUrl(CpeBase + "configuration"), {}, token,
                    [this, token, generation](const QByteArray &config, int, const QString &failure) {
                        if (generation != m_cpeGeneration)
                            return;
                        if (!failure.isEmpty()) {
                            setMessage(failure);
                            return;
                        }
                        const auto document = QJsonDocument::fromJson(config);
                        if (!document.isObject()) {
                            setMessage("Configuration CPE invalide.");
                            return;
                        }
                        const auto nextConfiguration = document.object();
                        const QString pupil = nextConfiguration.value("individu")
                                                  .toObject()
                                                  .value("individu_id")
                                                  .toVariant()
                                                  .toString();
                        const auto previous = source(CpeId);
                        if (previous.value("pupil").toString() != pupil || pupil.isEmpty()) {
                            for (int i = m_sources.size() - 1; i >= 0; --i)
                                if (m_sources[i].toObject().value("id") == CpeId)
                                    m_sources.removeAt(i);
                            m_grades = {};
                            m_absences = {};
                        }
                        m_cpeToken = token;
                        m_configuration = nextConfiguration;
                        const bool saved =
                            m_secrets.put("cpe-token", token) && m_secrets.put("cpe-configuration", config);
                        if (!saved) {
                            m_secrets.remove("cpe-token");
                            m_secrets.remove("cpe-configuration");
                        }
                        auto s = source(CpeId);
                        s["id"] = CpeId;
                        s["kind"] = "cpe";
                        s["label"] = "My CPE Lyon";
                        s["pupil"] = pupil;
                        s.remove("error");
                        putSource(s);
                        save();
                        rebuild();
                        setMessage(
                            saved ? "CPE connecté. Jeton protégé sur l'appareil ; mot de passe non conservé."
                                  : "CPE connecté pour cette session ; stockage du jeton indisponible.");
                        refreshCpe(CpeId, true);
                        refreshSchoolDetails();
                    });
            });
}
void AgendaController::disconnectCpe() {
    ++m_cpeGeneration;
    cancelRefresh(CpeId);
    m_cpeToken.fill('\0');
    m_cpeToken.clear();
    m_configuration = {};
    const bool tokenRemoved = m_secrets.remove("cpe-token");
    m_secrets.remove("cpe-configuration");
    emit sourcesChanged();
    setMessage(tokenRemoved
                   ? "Session CPE déconnectée. Le calendrier en cache reste disponible."
                   : "La session en mémoire est fermée, mais la suppression du jeton enregistré a échoué.");
}
void AgendaController::refreshCpe(const QString &id, bool force) {
    if (m_cpeToken.isEmpty()) {
        if (force)
            sourceError(id, "Reconnectez CPE pour actualiser.");
        return;
    }
    if (m_configuration.value("visibilite").toObject().value("est_visible_mon_planning") == false) {
        sourceError(id, "Le planning n'est pas activé pour ce compte CPE.");
        return;
    }
    const auto dates = range();
    const auto first = dates.first.addDays(1 - dates.first.dayOfWeek());
    const auto last = dates.second.addDays(7 - dates.second.dayOfWeek());
    const auto key = id + ':' + QString::number(m_cpeGeneration) + ':' + first.toString(Qt::ISODate) + ':' + last.toString(Qt::ISODate);
    if (m_refreshWindows.value(id) == key || (!force && recentlyRefreshed(key)))
        return;
    cancelRefresh(id);
    m_refreshWindows[id] = key;
    cpeWeek(id, first, last, first, {}, m_refreshGenerations.value(id));
}
void AgendaController::cpeWeek(const QString &id, const QDate &week, const QDate &last, const QDate &first,
                               QJsonArray collected, quint64 refreshGeneration) {
    QUrl url(CpeBase + "mon_planning");
    QUrlQuery query;
    query.addQueryItem("date_debut", week.toString(Qt::ISODate));
    query.addQueryItem("date_fin", week.addDays(6).toString(Qt::ISODate));
    url.setQuery(query);
    const int generation = m_cpeGeneration;
    m_calendarReplies[id] = request(url, {}, m_cpeToken,
            [this, id, week, last, first, collected, generation, refreshGeneration](const QByteArray &bytes, int status,
                                                                 const QString &error) mutable {
                if (generation != m_cpeGeneration || source(id).isEmpty() ||
                    refreshGeneration != m_refreshGenerations.value(id))
                    return;
                if (!error.isEmpty()) {
                    finishRefresh(id, refreshGeneration);
                    if (status == 401 || status == 403)
                        disconnectCpe();
                    sourceError(id, error);
                    return;
                }
                const auto document = QJsonDocument::fromJson(bytes);
                if (!bytes.trimmed().isEmpty() && !document.isArray()) {
                    finishRefresh(id, refreshGeneration);
                    sourceError(id, "Planning CPE invalide.");
                    return;
                }
                for (const auto &value : document.array()) {
                    if (!value.isObject()) {
                        finishRefresh(id, refreshGeneration);
                        sourceError(id, "Planning CPE invalide ; le cache est conservé.");
                        return;
                    }
                    collected << value;
                }
                if (collected.size() > 20000) {
                    finishRefresh(id, refreshGeneration);
                    sourceError(id, "Planning CPE trop volumineux.");
                    return;
                }
                if (week.addDays(7) <= last) {
                    cpeWeek(id, week.addDays(7), last, first, collected, refreshGeneration);
                    return;
                }
                auto s = source(id);
                // Keep cached weeks outside the refreshed window; never erase them after a failed request.
                QJsonArray combined;
                const auto oldest = QDate::currentDate().addYears(-1);
                const auto newest = QDate::currentDate().addYears(2);
                for (const auto &value : s.value("events").toArray()) {
                    const auto date = ReAgenda::parseDateTime(value.toObject().value("date_debut").toString(),
                                                              QTimeZone("Europe/Paris"))
                                          .date();
                    if (date.isValid() && (date < first || date > last) && date >= oldest && date <= newest)
                        combined << value;
                }
                for (const auto &value : collected)
                    combined << value;
                s["events"] = combined;
                QJsonArray weeks;
                for (const auto &value : s.value("cachedWeeks").toArray()) {
                    const auto date = QDate::fromString(value.toString(), Qt::ISODate);
                    if (date >= oldest && date <= newest && !weeks.contains(value))
                        weeks << value;
                }
                for (QDate date = first; date <= last; date = date.addDays(7))
                    if (!weeks.contains(date.toString(Qt::ISODate)))
                        weeks << date.toString(Qt::ISODate);
                s["cachedWeeks"] = weeks;
                s["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                s["windowFirst"] = first.toString(Qt::ISODate);
                s["windowLast"] = last.toString(Qt::ISODate);
                s.remove("error");
                m_lastRefresh[m_refreshWindows.value(id)] = m_refreshClock.elapsed();
                finishRefresh(id, refreshGeneration);
                putSource(s);
                save();
                rebuild();
                setMessage({});
            });
}
void AgendaController::refreshSchoolDetails() {
    const auto visibility = m_configuration.value("visibilite").toObject();
    const int generation = m_cpeGeneration;
    for (const QString &route : {QString("mes_notes"), QString("mes_absences")}) {
        const QString flag = route == "mes_notes" ? "est_visible_mes_notes" : "est_visible_mes_absences";
        if (visibility.value(flag) == false)
            continue;
        request(QUrl(CpeBase + route), {}, m_cpeToken,
                [this, route, generation](const QByteArray &bytes, int status, const QString &error) {
                    if (generation != m_cpeGeneration)
                        return;
                    if (!error.isEmpty()) {
                        if (status == 401 || status == 403)
                            disconnectCpe();
                        setMessage(error);
                        return;
                    }
                    const auto document = QJsonDocument::fromJson(bytes);
                    if (route == "mes_notes" &&
                        (document.isArray() || (status == 204 && bytes.trimmed().isEmpty())))
                        m_grades = document.array();
                    else if (route == "mes_absences" && document.isObject()) {
                        const auto object = document.object();
                        if (!object.value("absences").isArray() && !object.value("absences").isNull()) {
                            setMessage("Réponse d'absences CPE invalide.");
                            return;
                        }
                        m_absences = object;
                    } else {
                        setMessage("Données scolaires CPE invalides.");
                        return;
                    }
                    save();
                    emit schoolDataChanged();
                });
    }
}
QVariantList AgendaController::grades() const {
    QVariantList result;
    for (const auto &value : m_grades) {
        const auto course = value.toObject();
        const auto registration = course.value("inscription_cours").toObject();
        QStringList lines;
        if (!registration.value("moyenne").isUndefined())
            lines << "Moyenne du service : " + decimal(registration.value("moyenne"));
        if (!registration.value("nombre_credits_obtenus").isUndefined())
            lines << "Crédits : " + decimal(registration.value("nombre_credits_obtenus")) + " / " +
                         decimal(registration.value("nombre_credits_potentiels"));
        if (registration.value("est_validee").isBool())
            lines << (registration.value("est_validee").toBool() ? "Validé" : "Non validé");
        for (const auto &examValue : course.value("epreuves").toArray()) {
            const auto exam = examValue.toObject();
            const QString score = exam.value("est_absent").toBool()      ? "Absent"
                                  : exam.value("est_non_noter").toBool() ? "Non noté"
                                                                         : decimal(exam.value("note"));
            lines << exam.value("libelle").toString("Épreuve") + " : " + score;
        }
        result << QVariantMap{
            {"title", course.value("cours_libelle").toString(course.value("cours_code").toString("Cours"))},
            {"details", lines.join('\n')}};
    }
    return result;
}
QVariantList AgendaController::absences() const {
    QVariantList result;
    for (const auto &value : m_absences.value("absences").toArray()) {
        const auto absence = value.toObject();
        const auto event = absence.value("evenement").toObject();
        const auto reason = absence.value("motif_absence").toObject();
        const auto date =
            ReAgenda::parseDateTime(event.value("date_debut").toString(), QTimeZone("Europe/Paris"));
        const QString justified = reason.value("est_excuser").isBool()
                                      ? (reason.value("est_excuser").toBool() ? "Justifiée" : "Non justifiée")
                                      : "Justification inconnue";
        result << QVariantMap{{"title", event.value("libelle_construit").toString("Absence")},
                              {"details", date.toString("dd/MM/yyyy HH:mm") + " · " + justified + "\n" +
                                              reason.value("libelle").toString()}};
    }
    return result;
}
void AgendaController::requestNote(const QString &date, const QString &eventId, const QString &title,
                                   const QVariantMap &eventSnapshot) {
    if (!QDate::fromString(date, Qt::ISODate).isValid() || m_noteBusy)
        return;
    reportNoteProgress("Ouverture des notes de cet événement…", true);
    emit noteRequested(date, eventId, title);
    if (!m_noteHandler) {
        reportBridgeError("Le service Paper Bridge doit être installé pour ouvrir une note native.");
        return;
    }
    QJsonObject context{{"schemaVersion", 1}, {"kind", eventId.isEmpty() ? "day" : "event"},
                        {"date", date}, {"timeZone", "Europe/Paris"}};
    QString noteTitle = title;
    if (!eventId.isEmpty()) {
        auto snapshot = eventSnapshot;
        // The event dialog supplies the event that was shown when it opened.
        // Background refreshes must not replace its title or timing at click.
        if (snapshot.isEmpty())
            for (const auto &event : m_events)
                if (event.id == eventId) { snapshot = event.toVariant(); break; }
        if (snapshot.value("id").toString() != eventId || snapshot.value("start").toString().isEmpty()
                || snapshot.value("end").toString().isEmpty() || snapshot.value("timeZone").toString().isEmpty()) {
            reportBridgeError("Les détails de cet événement ne sont plus disponibles. Rouvrez-le dans le calendrier.");
            return;
        }
        noteTitle = snapshot.value("title", title).toString();
        const auto zone = snapshot.value("timeZone").toString();
        auto subject = snapshot.value("subject").toString().trimmed();
        if (subject.isEmpty()) subject = noteTitle;
        context["timeZone"] = zone;
        context["event"] = QJsonObject{{"id", eventId}, {"title", noteTitle}, {"subject", subject},
            {"start", snapshot.value("start").toString()}, {"end", snapshot.value("end").toString()},
            {"timeZone", zone}, {"allDay", snapshot.value("allDay").toBool()}};
    }
    if (!m_noteHandler(date, eventId, noteTitle, context))
        reportBridgeError("Une autre opération sur les notes est en cours. Réessayez après sa fin.");
}
void AgendaController::reportNoteProgress(const QString &message, bool busy) {
    m_noteMessage = message;
    m_noteBusy = busy;
    emit noteStateChanged();
}

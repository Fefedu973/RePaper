#include "MoodleController.h"
#include "MoodleProtocol.h"
#include "MoodleRelayClient.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QNetworkRequest>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <memory>

namespace {
QJsonValue safeSnapshotValue(const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray out;
        for (const auto &entry : value.toArray())
            out.append(safeSnapshotValue(entry));
        return out;
    }
    if (value.isObject()) {
        static const QSet<QString> allowed{"id",       "fullname",    "shortname",    "name",
                                           "summary",  "description", "modules",      "modname",
                                           "contents", "type",        "filename",     "fileurl",
                                           "mimetype", "filesize",    "timemodified", "contenthash"};
        QJsonObject out;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!allowed.contains(it.key()))
                continue;
            if (it.key() == "fileurl")
                out[it.key()] =
                    moodle::publicFileUrl(QUrl(it.value().toString())).toString(QUrl::FullyEncoded);
            else if (it.key() == "summary" || it.key() == "description")
                out[it.key()] = moodle::plainText(it.value().toString());
            else
                out[it.key()] = safeSnapshotValue(it.value());
        }
        return out;
    }
    return value;
}
} // namespace
MoodleController::MoodleController(QObject *parent, QNetworkAccessManager *network,
                                   QNetworkAccessManager *relayNetwork)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this)) {
    auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir + "/files");
    m_db = QSqlDatabase::addDatabase("QSQLITE", "moodle-" + QUuid::createUuid().toString());
    m_db.setDatabaseName(dir + "/cache.sqlite");
    if (m_db.open()) {
        QSqlQuery q(m_db);
        q.exec("PRAGMA journal_mode=WAL");
        q.exec("CREATE TABLE IF NOT EXISTS snapshots(account TEXT,key TEXT,data BLOB,updated INTEGER,PRIMARY "
               "KEY(account,key))");
        q.exec("CREATE TABLE IF NOT EXISTS imports(key TEXT PRIMARY KEY,document_id TEXT)");
    } else
        m_message = "Cache local indisponible : vérifiez l’espace disque.";
    auto credentials = QJsonDocument::fromJson(m_vault.get("account")).object();
    if (!credentials.isEmpty()) {
        m_base = credentials["base"].toString();
        m_token = credentials["token"].toString();
        m_user = credentials["user"].toString();
        showCourses(false);
    }
    m_loginBase = m_base;
    m_relay = new MoodleRelayClient(this, relayNetwork);
    connect(m_relay, &MoodleRelayClient::changed, this, [this] {
        if (waitingForBrowser())
            m_message = m_relay->message();
        emit changed();
    });
    connect(m_relay, &MoodleRelayClient::failed, this, [this](const QString &message) {
        m_authFlow = "error";
        m_passport.clear();
        m_message = message;
        emit changed();
    });
    connect(m_relay, &MoodleRelayClient::qrReceived, this, [this](int userId, const QString &key) {
        if (waitingForBrowser())
            exchangeQr(userId, key);
    });
    connect(&m_bridge, &repaper::BridgeClient::changed, this, [this] {
        m_message = m_bridge.message();
        emit changed();
    });
    connect(&m_bridge, &repaper::BridgeClient::imported, this, [this](const QString &id) {
        QSqlQuery q(m_db);
        q.prepare("INSERT OR REPLACE INTO imports(key,document_id) VALUES(?,?)");
        q.addBindValue(m_importKey);
        q.addBindValue(id);
        q.exec();
        emit changed();
    });
    connect(&m_powerPoint, &PowerPointConverter::converted, this, [this](const QString &path) {
        const auto item = m_convertingItem;
        m_convertingItem.clear();
        m_busy = false;
        importCachedResource(item, path);
    });
    connect(&m_powerPoint, &PowerPointConverter::failed, this, [this](const QString &message) {
        m_convertingItem.clear();
        m_busy = false;
        m_message = message;
        emit changed();
    });
}
MoodleController::~MoodleController() {
    if (m_handoff) {
        auto process = m_handoff.data();
        m_handoff = nullptr;
        process->terminate();
        if (!process->waitForFinished(2000))
            process->kill();
    }
    const auto connection = m_db.connectionName();
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}
QString MoodleController::account() const {
    return QString::fromLatin1(
        QCryptographicHash::hash((m_base + "|" + m_user).toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString MoodleController::launchLink() const {
    return m_passport.isEmpty() ? QString()
                                : moodle::launchUrl(m_loginBase, m_passport).toString(QUrl::FullyEncoded);
}
void MoodleController::setLoginBaseUrl(const QString &value) {
    if (m_loginBase == value)
        return;
    if (m_authenticating || m_qrAuthenticating)
        cancel();
    else
        cancelLogin();
    m_loginBase = value;
    m_passport.clear();
    emit loginBaseUrlChanged();
    emit changed();
}
void MoodleController::setSearch(const QString &value) {
    m_search = value;
    emit changed();
}
QString MoodleController::cacheFile(const QVariantMap &item) const {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/files/" + account() +
           "-" + item["revision"].toString() + ".download";
}
QVariantList MoodleController::items() const {
    QVariantList out;
    for (auto value : m_rows) {
        auto row = value.toMap();
        if (!m_search.isEmpty() && !row["name"].toString().contains(m_search, Qt::CaseInsensitive))
            continue;
        if (row["kind"] == "file") {
            row["cached"] = QFileInfo::exists(cacheFile(row));
            QSqlQuery q(m_db);
            q.prepare("SELECT document_id FROM imports WHERE key=?");
            q.addBindValue(account() + ":" + row["id"].toString() + ":" + row["revision"].toString());
            q.exec();
            if (q.next())
                row["imported"] = true;
        }
        out << row;
    }
    return out;
}
void MoodleController::fail(const QString &message) {
    if (m_qrAuthenticating) {
        m_qrAuthenticating = false;
        m_authFlow = "error";
        emit loginFinished(false);
    }
    m_expectedQrUserId = 0;
    m_busy = false;
    m_message = message;
    m_offline = true;
    m_reply = nullptr;
    if (m_authenticating) {
        if (m_authFlow == "verifying")
            m_authFlow = "error";
        m_base = m_previousBase;
        m_token = m_previousToken;
        m_user = m_previousUser;
        m_authenticating = false;
        emit loginFinished(false);
    }
    emit changed();
}
void MoodleController::prepareLogin(const QString &base) {
    cancelLogin();
    auto clean = moodle::normalizeBase(base);
    if (clean.isEmpty()) {
        setLoginBaseUrl(base);
        m_passport.clear();
        m_message = "Indiquez une adresse Moodle HTTPS sans paramètres.";
        emit changed();
        return;
    }
    setLoginBaseUrl(clean);
    m_passport = QUuid::createUuid().toString(QUuid::Id128);
    m_loginAttemptId = QString::fromLatin1(
        QCryptographicHash::hash((clean + m_passport).toUtf8(), QCryptographicHash::Md5).toHex());
    m_authFlow = "waiting";
    const auto attempt = m_loginAttemptId;
    QTimer::singleShot(600000, this, [this, attempt] {
        if (waitingForBrowser() && m_loginAttemptId == attempt) {
            cancelLogin();
            m_message = "La demande de connexion a expiré. Recommencez depuis cet écran.";
            emit changed();
        }
    });
    emit changed();
}
bool MoodleController::pcIntegration() const {
    const QFileInfo helper(qEnvironmentVariable("REPAPER_PC_HANDOFF_HELPER"));
    return qEnvironmentVariable("REPAPER_PC_EMULATOR") == "1" && helper.isAbsolute() && helper.isFile();
}
bool MoodleController::handoffBusy() const {
    return !m_handoff.isNull();
}
QString MoodleController::loginCode() const {
    return m_relay->code();
}
QString MoodleController::verificationUrl() const {
    return m_relay->verificationUrl();
}
QString MoodleController::loginQr() const {
    return m_relay->qrImage();
}
QString MoodleController::portalUrl() const {
    return moodle::normalizeBase(qEnvironmentVariable(
        "REPAPER_MOODLE_PORTAL_URL", "https://repaper-moodle-connect.fefe-du-973.chatgpt.site"));
}
void MoodleController::openLogin(const QString &base) {
    if (busy())
        return;
    prepareLogin(base);
    if (m_passport.isEmpty())
        return;
    m_relay->start(portalUrl(), m_loginBase, m_passport);
}
void MoodleController::openPortal() {
    if (!pcIntegration() || handoffBusy() || verificationUrl().isEmpty())
        return;
    pcHandoff("open", QJsonDocument(QJsonObject{{"url", verificationUrl()}}).toJson(QJsonDocument::Compact));
}
void MoodleController::cancelLogin() {
    if (m_relay)
        m_relay->cancel();
    if (m_handoff) {
        auto process = m_handoff.data();
        m_handoff = nullptr;
        process->terminate();
        QTimer::singleShot(2000, process, [process] {
            if (process->state() != QProcess::NotRunning)
                process->kill();
        });
    }
    m_passport.clear();
    if (m_authFlow == "waiting")
        m_authFlow = "cancelled";
    emit changed();
}
void MoodleController::pcHandoff(const QString &operation, const QByteArray &payload) {
    auto process = new QProcess(this);
    m_handoff = process;
    auto output = std::make_shared<QByteArray>();
    auto finish = [this, process, output](bool success) {
        if (m_handoff != process) {
            process->deleteLater();
            return;
        }
        m_handoff = nullptr;
        if (!success || !QJsonDocument::fromJson(*output).object()["ok"].toBool())
            m_message = "Le navigateur Windows ne peut pas être ouvert. Ouvrez le site indiqué sur votre PC "
                        "et saisissez le code affiché.";
        output->fill('\0');
        output->clear();
        process->deleteLater();
        emit changed();
    };
    connect(process, &QProcess::started, this, [process, payload] {
        process->write(payload);
        process->closeWriteChannel();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [process, output] {
        *output += process->readAllStandardOutput();
        if (output->size() > 2048)
            process->kill();
    });
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [finish](int code, QProcess::ExitStatus status) {
                finish(code == 0 && status == QProcess::NormalExit);
            });
    QTimer::singleShot(20000, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
    process->start("python3", {qEnvironmentVariable("REPAPER_PC_HANDOFF_HELPER"), operation});
    emit changed();
}
void MoodleController::exchangeQr(int userId, const QString &key) {
    if (busy() || userId <= 0 || !QRegularExpression("^[a-fA-F0-9]{32}$").match(key).hasMatch())
        return;
    const auto base = moodle::normalizeBase(m_loginBase);
    if (base.isEmpty())
        return;
    m_qrAuthenticating = true;
    m_authFlow = "verifying";
    m_passport.clear();
    m_busy = true;
    m_message = "Vérification du QR auprès de votre Moodle…";
    emit changed();
    QNetworkRequest request(QUrl(base + "/lib/ajax/service-nologin.php"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", "reMoodle/0.1 (MoodleMobile compatible)");
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(20000);
    const QJsonArray payload{QJsonObject{{"index", 0},
                                         {"methodname", "tool_mobile_get_tokens_for_qr_login"},
                                         {"args", QJsonObject{{"userid", userId}, {"qrloginkey", key}}}}};
    auto reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply = reply;
    const auto epoch = m_epoch;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        *bytes += reply->readAll();
        if (bytes->size() > 65536)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, epoch, base, userId] {
        *bytes += reply->readAll();
        reply->deleteLater();
        if (epoch != m_epoch)
            return;
        if (m_reply == reply)
            m_reply = nullptr;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (bytes->size() > 65536 || reply->error() != QNetworkReply::NoError || status != 200) {
            fail("L’échange du QR a été interrompu. Affichez un nouveau QR Moodle et réessayez.");
            return;
        }
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(*bytes, &parse);
        bytes->fill('\0');
        bytes->clear();
        if (parse.error != QJsonParseError::NoError || !document.isArray() || document.array().size() != 1) {
            fail("Réponse QR Moodle invalide. Affichez un nouveau QR.");
            return;
        }
        const auto entry = document.array().first().toObject();
        if (!entry["error"].isBool()) {
            fail("Réponse QR Moodle invalide. Affichez un nouveau QR.");
            return;
        }
        if (entry["error"].toBool()) {
            const auto code = entry["exception"].toObject()["errorcode"].toString();
            fail(code == "ipmismatch"
                     ? "Moodle exige le même réseau : connectez cette tablette et l’appareil ayant affiché "
                       "le QR au même Wi-Fi, puis générez un nouveau QR."
                 : code == "expiredkey" || code == "invalidkey"
                     ? "Le QR Moodle a expiré ou a déjà été utilisé. Affichez un nouveau QR depuis votre "
                       "profil."
                 : code == "qrcodedisabled" ? "La connexion par QR n’est pas activée par votre établissement."
                 : code == "apprequired"
                     ? "Ce Moodle refuse la connexion QR à ce client."
                     : "Moodle a refusé ce QR. Vérifiez votre compte et générez un nouveau QR.");
            return;
        }
        const auto token = entry["data"].toObject()["token"].toString();
        if (!QRegularExpression("^[a-fA-F0-9]{32}$").match(token).hasMatch()) {
            fail("Moodle n’a pas renvoyé un accès valide pour ce QR.");
            return;
        }
        // The optional private browser token is deliberately ignored.
        m_qrAuthenticating = false;
        m_busy = false;
        m_expectedQrUserId = userId;
        login(base, token);
    });
}
void MoodleController::login(const QString &base, const QString &handoff) {
    if (busy())
        return;
    auto clean = moodle::normalizeBase(base);
    const auto expected =
        m_passport.isEmpty()
            ? QString()
            : QString::fromLatin1(
                  QCryptographicHash::hash((clean + m_passport).toUtf8(), QCryptographicHash::Md5).toHex());
    auto parsed = moodle::token(handoff, expected);
    if (clean.isEmpty() || parsed.isEmpty()) {
        m_message = "Adresse HTTPS ou lien Moodle invalide. Utilisez le lien issu de cette connexion.";
        emit changed();
        emit loginFinished(false);
        return;
    }
    m_previousBase = m_base;
    m_previousToken = m_token;
    m_previousUser = m_user;
    m_authenticating = true;
    m_base = clean;
    m_token = parsed;
    m_busy = true;
    m_message = "Vérification du compte…";
    emit changed();
    call("core_webservice_get_site_info", {}, [this](QJsonDocument doc) {
        auto site = doc.object();
        if (site["userid"].toInt() <= 0) {
            fail("Moodle n’a pas renvoyé de compte valide.");
            return;
        }
        if (m_expectedQrUserId > 0 && site["userid"].toInt() != m_expectedQrUserId) {
            fail("Le compte renvoyé ne correspond pas au QR Moodle choisi.");
            return;
        }
        m_expectedQrUserId = 0;
        QStringList functions;
        for (auto f : site["functions"].toArray())
            functions << f.toObject()["name"].toString();
        if (!functions.contains("core_enrol_get_users_courses") ||
            !functions.contains("core_course_get_contents")) {
            fail("Le service mobile de ce Moodle ne permet pas de parcourir les cours.");
            return;
        }
        m_user = QString::number(site["userid"].toInt());
        m_name = moodle::plainText(site["sitename"].toString());
        if (!m_vault.put("account",
                         QJsonDocument(QJsonObject{{"base", m_base}, {"token", m_token}, {"user", m_user}})
                             .toJson(QJsonDocument::Compact))) {
            fail("Impossible d’enregistrer le compte dans le coffre chiffré.");
            return;
        }
        m_authenticating = false;
        if (m_authFlow == "verifying")
            m_authFlow = "connected";
        m_demo = false;
        setLoginBaseUrl(m_base);
        m_passport.clear();
        m_stack.clear();
        m_rows.clear();
        m_busy = false;
        m_offline = false;
        emit loginFinished(true);
        showCourses(true);
    });
}
void MoodleController::logout() {
    cancel();
    m_vault.remove("account");
    m_token.clear();
    m_user.clear();
    m_demo = false;
    m_rows.clear();
    m_stack.clear();
    m_passport.clear();
    m_message = "Compte déconnecté. Les documents importés restent dans votre bibliothèque.";
    emit changed();
}
void MoodleController::cancel() {
    m_qrAuthenticating = false;
    m_expectedQrUserId = 0;
    cancelLogin();
    if (m_authFlow == "verifying")
        m_authFlow = "cancelled";
    ++m_epoch;
    m_powerPoint.cancel();
    m_convertingItem.clear();
    if (m_reply)
        m_reply->abort();
    m_reply = nullptr;
    m_busy = false;
    if (m_authenticating) {
        m_base = m_previousBase;
        m_token = m_previousToken;
        m_user = m_previousUser;
        m_authenticating = false;
        emit loginFinished(false);
    }
    m_message = "Opération annulée.";
    emit changed();
}
void MoodleController::call(const QString &function, const QMap<QString, QString> &params, Callback callback,
                            int attempt) {
    const int epoch = m_epoch;
    QUrlQuery form;
    form.addQueryItem("wstoken", m_token);
    form.addQueryItem("wsfunction", function);
    form.addQueryItem("moodlewsrestformat", "json");
    for (auto it = params.begin(); it != params.end(); ++it)
        form.addQueryItem(it.key(), it.value());
    QNetworkRequest request(QUrl(m_base + "/webservice/rest/server.php"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(20000);
    auto reply = m_network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    m_reply = reply;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        *bytes += reply->readAll();
        if (bytes->size() > 8 * 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, bytes, callback, function, params, attempt, epoch] {
                *bytes += reply->readAll();
                reply->deleteLater();
                if (epoch != m_epoch)
                    return;
                if (m_reply == reply)
                    m_reply = nullptr;
                int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (bytes->size() > 8 * 1024 * 1024) {
                    fail("Les métadonnées de ce cours dépassent la limite locale.");
                    return;
                }
                if ((status == 429 || status >= 500 || status == 0) && attempt < 2) {
                    m_message = "Réseau indisponible, nouvelle tentative…";
                    emit changed();
                    QTimer::singleShot((1 << attempt) * 1000 + QRandomGenerator::global()->bounded(300), this,
                                       [this, function, params, callback, attempt, epoch] {
                                           if (epoch == m_epoch)
                                               call(function, params, callback, attempt + 1);
                                       });
                    return;
                }
                if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                    fail(status == 401 || status == 403
                             ? "Accès Moodle refusé. Reconnectez votre compte."
                             : "Connexion Moodle impossible. Réessayez dans un instant.");
                    return;
                }
                QJsonParseError parse;
                auto doc = QJsonDocument::fromJson(*bytes, &parse);
                if (parse.error != QJsonParseError::NoError || (!doc.isArray() && !doc.isObject())) {
                    fail("Réponse Moodle invalide. Réessayez dans un instant.");
                    return;
                }
                if (doc.isObject() && doc.object().contains("exception")) {
                    auto code = doc.object()["errorcode"].toString();
                    fail(code == "invalidtoken" ? "Votre accès Moodle a expiré. Reconnectez-vous."
                         : code == "accessexception"
                             ? "Cette ressource ou fonction mobile n’est pas autorisée."
                             : "Moodle n’a pas pu effectuer cette opération.");
                    return;
                }
                m_busy = false;
                m_offline = false;
                m_message.clear();
                callback(doc);
                emit changed();
            });
}
void MoodleController::saveSnapshot(const QString &key, const QJsonDocument &doc) {
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO snapshots(account,key,data,updated) VALUES(?,?,?,?)");
    q.addBindValue(account());
    q.addBindValue(key);
    q.addBindValue(QJsonDocument(safeSnapshotValue(doc.array()).toArray()).toJson(QJsonDocument::Compact));
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!q.exec())
        m_message = "Le cache n’a pas pu être mis à jour.";
}
QJsonDocument MoodleController::snapshot(const QString &key) const {
    QSqlQuery q(m_db);
    q.prepare("SELECT data FROM snapshots WHERE account=? AND key=?");
    q.addBindValue(account());
    q.addBindValue(key);
    q.exec();
    return q.next() ? QJsonDocument::fromJson(q.value(0).toByteArray()) : QJsonDocument();
}
QVariantList MoodleController::courses(const QJsonArray &array) const {
    QVariantList rows;
    for (auto value : array) {
        auto c = value.toObject();
        if (c["id"].toInt() <= 0)
            continue;
        rows << QVariantMap{{"kind", "course"},
                            {"id", c["id"].toInt()},
                            {"name", moodle::plainText(c["fullname"].toString())},
                            {"detail", moodle::plainText(c["shortname"].toString())}};
    }
    return rows;
}
QVariantList MoodleController::sections(const QJsonArray &array) const {
    QVariantList rows;
    for (auto value : array) {
        auto section = value.toObject();
        rows << QVariantMap{{"kind", "section"},
                            {"id", section["id"].toInt()},
                            {"name", moodle::plainText(section["name"].toString("Section"))},
                            {"detail", moodle::plainText(section["summary"].toString())},
                            {"modules", section["modules"].toArray().toVariantList()}};
    }
    return rows;
}
void MoodleController::setRows(const QVariantList &rows, const QString &title, bool push) {
    if (push)
        m_stack.append({m_title, m_rows});
    m_rows = rows;
    m_title = title;
    m_search.clear();
    emit changed();
}
void MoodleController::showCourses(bool update) {
    m_course = 0;
    m_stack.clear();
    auto cached = snapshot("courses");
    setRows(courses(cached.array()), "Mes cours");
    if (!update) {
        m_message = cached.isNull() ? "Actualisez pour charger vos cours." : QString{};
        emit changed();
        return;
    }
    m_busy = true;
    emit changed();
    call("core_enrol_get_users_courses", {{"userid", m_user}}, [this](QJsonDocument doc) {
        if (!doc.isArray()) {
            fail("Impossible de charger la liste des cours.");
            return;
        }
        saveSnapshot("courses", doc);
        setRows(courses(doc.array()), "Mes cours");
    });
}
void MoodleController::showSections(int course, const QString &name, bool update) {
    m_course = course;
    auto key = "course:" + QString::number(course);
    auto cached = snapshot(key);
    setRows(sections(cached.array()), name, true);
    if (!update)
        return;
    m_busy = true;
    emit changed();
    call("core_course_get_contents", {{"courseid", QString::number(course)}},
         [this, key, name](QJsonDocument doc) {
             if (!doc.isArray()) {
                 fail("Impossible de charger ce cours.");
                 return;
             }
             saveSnapshot(key, doc);
             setRows(sections(doc.array()), name);
         });
}
void MoodleController::refresh() {
    if (busy() || !loggedIn())
        return;
    if (m_demo) {
        m_message = "Simulation locale : données fictives, aucun appel Moodle.";
        emit changed();
        return;
    }
    if (m_course == 0)
        showCourses(true);
    else {
        auto name = m_stack.isEmpty() ? m_title : m_stack.value(1).first;
        if (m_stack.size() > 1) {
            name = m_stack[1].first;
            m_stack = m_stack.mid(0, 1);
        }
        auto title = m_title;
        if (!m_stack.isEmpty()) {
            m_title = m_stack[0].first;
            m_rows = m_stack[0].second;
            m_stack.clear();
        }
        showSections(m_course, name.isEmpty() ? title : name, true);
    }
}
void MoodleController::back() {
    if (busy())
        cancel();
    if (m_stack.isEmpty())
        return;
    auto previous = m_stack.takeLast();
    m_title = previous.first;
    m_rows = previous.second;
    m_search.clear();
    if (m_stack.isEmpty())
        m_course = 0;
    emit changed();
}
void MoodleController::openItem(const QVariantMap &item) {
    if (busy())
        return;
    auto kind = item["kind"].toString();
    if (kind == "course") {
        showSections(item["id"].toInt(), item["name"].toString(), !m_demo);
        return;
    }
    if (kind == "file") {
        importResource(item);
        return;
    }
    QVariantList rows;
    if (kind == "section") {
        for (auto value : item["modules"].toList()) {
            auto module = value.toMap();
            rows << QVariantMap{{"kind", "module"},
                                {"id", module["id"]},
                                {"name", moodle::plainText(module["name"].toString())},
                                {"detail", module["modname"]},
                                {"description", moodle::plainText(module["description"].toString())},
                                {"contents", module["contents"]}};
        }
    } else if (kind == "module") {
        int index = 0;
        for (auto value : item["contents"].toList()) {
            auto file = QJsonObject::fromVariantMap(value.toMap());
            auto url = moodle::publicFileUrl(QUrl(file["fileurl"].toString()));
            const auto mimetype = file["mimetype"].toString();
            const bool powerPoint = !PowerPointConverter::extension(file["filename"].toString(), mimetype).isEmpty();
            bool compatible = (mimetype == "application/pdf" || mimetype.startsWith("image/") || powerPoint) &&
                              moodle::sameOrigin(url, QUrl(m_base));
            rows << QVariantMap{
                {"kind", file["type"].toString() == "file" ? "file" : "unsupported"},
                {"id", moodle::resourceId(account(), m_course, item["id"].toInt(), index++, url)},
                {"revision", moodle::revision(file)},
                {"name", file["filename"].toString()},
                {"url", url.toString(QUrl::FullyEncoded)},
                {"mime", mimetype},
                {"size", file["filesize"].toVariant()},
                {"compatible", compatible},
                {"convertsToPdf", powerPoint},
                {"detail", compatible ? QString(powerPoint ? "%1 Ko · Importer en PDF" : "%1 Ko · Toucher pour importer")
                                            .arg(file["filesize"].toDouble() / 1024, 0, 'f', 0)
                                      : "Ce type d’activité n’est pas importable"}};
        }
        if (rows.isEmpty())
            m_message = item["description"].toString().isEmpty()
                            ? "Cette activité ne contient pas de fichier importable."
                            : item["description"].toString();
    } else
        return;
    setRows(rows, item["name"].toString(), true);
}
void MoodleController::loadDemo() {
    m_demo = true;
    m_base = "https://demo.invalid";
    m_token = "demo-only";
    m_user = "0";
    m_passport.clear();
    QJsonArray list{QJsonObject{{"id", 1},
                                {"fullname", "Électronique · circuits et filtres"},
                                {"shortname", "Cours de démonstration"}},
                    QJsonObject{{"id", 2},
                                {"fullname", "Mathématiques · analyse"},
                                {"shortname", "Cours de démonstration"}}};
    saveSnapshot("courses", QJsonDocument(list));
    QJsonObject file{{"type", "file"},
                     {"filename", "Exemple de fiche.pdf"},
                     {"fileurl", "https://demo.invalid/webservice/pluginfile.php/1/example.pdf"},
                     {"mimetype", "application/pdf"},
                     {"filesize", 0},
                     {"timemodified", 1}};
    for (int c = 1; c <= 2; ++c) {
        auto resource = QVariantMap{{"revision", moodle::revision(file)}};
        auto path = cacheFile(resource);
        {
            QPdfWriter writer(path);
            writer.setPageSize(QPageSize(QPageSize::A4));
            QPainter painter(&writer);
            painter.setFont(QFont("Sans", 28));
            painter.drawText(QRect(800, 800, writer.width() - 1600, 1500), Qt::TextWordWrap,
                             "RePaper — document de démonstration\n\nCe PDF fictif permet de tester le "
                             "parcours Moodle → bibliothèque locale de l’émulateur.");
        }
        QJsonArray content{
            QJsonObject{{"id", 1},
                        {"name", "Chapitre 1 · Fondamentaux"},
                        {"summary", "Ressources fictives pour les essais sur PC."},
                        {"modules", QJsonArray{QJsonObject{{"id", 10},
                                                           {"name", "Support de cours"},
                                                           {"modname", "resource"},
                                                           {"contents", QJsonArray{file}}},
                                               QJsonObject{{"id", 11},
                                                           {"name", "Travaux pratiques"},
                                                           {"modname", "folder"},
                                                           {"contents", QJsonArray{file}}}}}},
            QJsonObject{{"id", 2}, {"name", "Chapitre 2 · Applications"}, {"modules", QJsonArray{}}}};
        saveSnapshot("course:" + QString::number(c), QJsonDocument(content));
    }
    showCourses(false);
    m_message = "Simulation locale · cours fictifs · aucun compte requis";
    emit changed();
}
void MoodleController::importResource(const QVariantMap &item) {
    if (busy())
        return;
    if (!item["compatible"].toBool()) {
        m_message = "Ce format ou cette adresse n’est pas pris en charge pour l’import.";
        emit changed();
        return;
    }
    m_importKey = account() + ":" + item["id"].toString() + ":" + item["revision"].toString();
    const auto path = cacheFile(item);
    if (QFileInfo::exists(path)) {
        importCachedResource(item, path);
        return;
    }
    auto url = QUrl(item["url"].toString());
    if (!moodle::sameOrigin(url, QUrl(m_base)) || item["size"].toLongLong() > 64 * 1024 * 1024) {
        fail("Ressource refusée : adresse externe ou fichier de plus de 64 Mio.");
        return;
    }
    QUrlQuery query(url);
    query.addQueryItem("token", m_token);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    auto output = std::make_shared<QSaveFile>(path);
    if (!output->open(QIODevice::WriteOnly)) {
        fail("Le cache de fichiers est plein ou inaccessible.");
        return;
    }
    m_busy = true;
    m_progress = 0;
    m_message = "Téléchargement…";
    emit changed();
    auto reply = m_network->get(request);
    m_reply = reply;
    auto total = std::make_shared<qint64>(0);
    auto bad = std::make_shared<bool>(false);
    const auto epoch = m_epoch;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, output, total, bad, epoch] {
        auto bytes = reply->readAll();
        if (epoch != m_epoch)
            return;
        *total += bytes.size();
        if (*total > 64 * 1024 * 1024 || output->write(bytes) != bytes.size()) {
            *bad = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply, epoch](qint64 a, qint64 b) {
        if (epoch != m_epoch || m_reply != reply)
            return;
        m_progress = b > 0 ? qBound(0.0, double(a) / b, 1.0) : 0;
        emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, output, total, bad, epoch, item, path] {
        reply->deleteLater();
        if (epoch != m_epoch) {
            output->cancelWriting();
            return;
        }
        if (m_reply == reply)
            m_reply = nullptr;
        m_busy = false;
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto trailing = reply->readAll();
        *total += trailing.size();
        if (*total > 64 * 1024 * 1024 || output->write(trailing) != trailing.size())
            *bad = true;
        if (*bad || reply->error() != QNetworkReply::NoError || status != 200 || *total == 0 ||
            (item["size"].toLongLong() > 0 && *total != item["size"].toLongLong())) {
            output->cancelWriting();
            fail("Téléchargement interrompu ou ressource modifiée. Réessayez après actualisation.");
            return;
        }
        if (!output->commit()) {
            fail("Impossible d’enregistrer le fichier téléchargé.");
            return;
        }
        importCachedResource(item, path);
    });
}
void MoodleController::importCachedResource(const QVariantMap &item, const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail("Le fichier téléchargé ne peut pas être relu. Vérifiez le stockage local.");
        return;
    }
    const auto header = file.peek(512);
    const auto mime = QMimeDatabase().mimeTypeForData(header).name();
    const auto suffix = PowerPointConverter::extension(item["name"].toString(), item["mime"].toString());
    file.close();
    if (PowerPointConverter::matchesContent(header, suffix)) {
        m_busy = true;
        m_progress = 0;
        m_message = "Conversion PowerPoint en PDF…";
        m_convertingItem = item;
        emit changed();
        m_powerPoint.convert(path, suffix, path);
        return;
    }
    if (!(header.startsWith("%PDF-") || mime.startsWith("image/"))) {
        QFile::remove(path);
        fail("Moodle a renvoyé un contenu inattendu (connexion expirée ou fichier incompatible).");
        return;
    }
    // Native imports are independent copies; converting does not change the
    // original resource/revision key used for cache lookup and deduplication.
    QDir files(QFileInfo(path).absolutePath());
    const auto list = files.entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);
    qint64 size = 0;
    for (const auto &entry : list)
        size += entry.size();
    for (const auto &entry : list) {
        if (size <= 256 * 1024 * 1024)
            break;
        if (entry.absoluteFilePath() != path && QFile::remove(entry.absoluteFilePath()))
            size -= entry.size();
    }
    m_offline = false;
    const auto title = suffix.isEmpty() ? item["name"].toString()
                                       : QFileInfo(item["name"].toString()).completeBaseName() + ".pdf";
    m_bridge.importFile(path, title, m_importKey);
    emit changed();
}

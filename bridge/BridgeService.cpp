#include "BridgeService.h"
#include "BridgeClient.h"
#include "NativeDocumentsClient.h"
#include "NativeImport.h"
#include "Bundle.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QTimeZone>
#include <QUuid>
#include <memory>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace {
QJsonObject error(const QString &code, const QString &message = {}) {
    return {
        {"error",
         QJsonObject{{"code", code}, {"message", message.isEmpty() ? code : message}, {"retryable", false}}}};
}
void must(bool value, const char *code) {
    if (!value)
        throw std::runtime_error(code);
}
QString uid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QByteArray json(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
QString firmware() {
    QFile f("/etc/os-release");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    auto match = QRegularExpression("IMG_VERSION=\"([^\"]+)\"").match(QString::fromUtf8(f.readAll()));
    return match.captured(1);
}
bool compatible() {
    QFile f("/sys/devices/soc0/machine");
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return f.readAll().contains("Ferrari") && firmware() == "3.28.0.169";
}
bool agendaContextValid(const QJsonValue &value) {
    if (!value.isObject()) return false;
    const auto context = value.toObject();
    if (json(context).size() > 8192 || !context["schemaVersion"].isDouble()
            || context["schemaVersion"].toDouble() != 1) return false;
    const auto bounded = [](const QJsonValue &value, int maximum) {
        return value.isString() && !value.toString().trimmed().isEmpty()
            && value.toString().size() <= maximum && !value.toString().contains(QChar::Null);
    };
    const auto date = context["date"].toString();
    if (date.size() != 10 || QDate::fromString(date, Qt::ISODate).toString(Qt::ISODate) != date
            || !bounded(context["timeZone"], 128)) return false;
    const QTimeZone zone(context["timeZone"].toString().toUtf8());
    if (!zone.isValid()) return false;
    if (context["kind"] == "day") return !context.contains("event");
    if (context["kind"] != "event" || !context["event"].isObject()) return false;
    const auto event = context["event"].toObject();
    if (!bounded(event["id"], 512) || !bounded(event["title"], 512)
            || !bounded(event["subject"], 512) || !event["allDay"].isBool()
            || event["timeZone"] != context["timeZone"]
            || !bounded(event["start"], 40) || !bounded(event["end"], 40)) return false;
    static const QRegularExpression explicitOffset("(?:Z|[+-][0-9]{2}:[0-9]{2})$");
    const auto startText = event["start"].toString(), endText = event["end"].toString();
    const auto start = QDateTime::fromString(startText, Qt::ISODateWithMs);
    const auto end = QDateTime::fromString(endText, Qt::ISODateWithMs);
    return explicitOffset.match(startText).hasMatch() && explicitOffset.match(endText).hasMatch()
        && start.isValid() && end.isValid() && end >= start
        && zone.offsetFromUtc(start) == start.offsetFromUtc() && zone.offsetFromUtc(end) == end.offsetFromUtc();
}
} // namespace
BridgeService::BridgeService(QString data, QString store, bool sandbox, bool proxy, bool sandboxUnvalidated,
                             QObject *parent, QString importInputRoot)
    : QObject(parent), m_state(std::move(data)), m_store(std::move(store)), m_sandbox(sandbox),
      m_proxy(proxy), m_sandboxUnvalidated(sandboxUnvalidated), m_instanceLock(m_state + "/service.lock") {
    // The executable supplies no override; private test entry points can inject
    // their input root without adding a device CLI/environment bypass.
    m_importInputRoot = importInputRoot.isEmpty() ? "/home/root/.local/share/RePaper" : std::move(importInputRoot);
    must(!sandboxUnvalidated || sandbox, "SANDBOX_REQUIRED");
    if (m_proxy)
        return;
    QDir().mkpath(m_state);
    QFile::setPermissions(m_state, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // One worker owns this journal and its recovery state, including across socket names.
    m_instanceLock.setStaleLockTime(0);
    must(m_instanceLock.tryLock(0), "BRIDGE_ALREADY_RUNNING");
    if (m_sandbox)
        QDir().mkpath(m_store);
    m_db = QSqlDatabase::addDatabase("QSQLITE", "bridge-" + uid());
    m_db.setDatabaseName(m_state + "/operations.sqlite");
    must(m_db.open(), "JOURNAL_UNAVAILABLE");
    QSqlQuery q(m_db);
    must(q.exec("PRAGMA journal_mode=WAL"), "JOURNAL_WAL_FAILED");
    must(q.exec("PRAGMA synchronous=FULL"), "JOURNAL_SYNC_FAILED");
    must(q.exec("CREATE TABLE IF NOT EXISTS operations(id TEXT PRIMARY KEY, idempotency_key TEXT NOT NULL "
                "UNIQUE, request_hash TEXT NOT NULL, document_id TEXT NOT NULL, state TEXT NOT NULL, "
                "result_json TEXT, updated_at INTEGER NOT NULL)"),
         "JOURNAL_SCHEMA_FAILED");
}
bool BridgeService::listen(const QString &path) {
    QLocalSocket existing;
    existing.connectToServer(path);
    if (existing.waitForConnected(100))
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLocalServer::removeServer(path);
    m_server.setSocketOptions(QLocalServer::UserAccessOption | QLocalServer::GroupAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (auto socket = m_server.nextPendingConnection()) {
#ifdef Q_OS_UNIX
            struct ucred cred {};
            socklen_t len = sizeof(cred);
            auto user = getpwnam("paperbridge");
            if (getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 ||
                (cred.uid != 0 && cred.uid != getuid() && (!user || cred.uid != user->pw_uid))) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
#endif
            ++m_activeConnections;
            m_idleTimer.stop();
            connect(socket, &QLocalSocket::disconnected, this, [this] {
                --m_activeConnections;
                if (m_idleTimeout && !m_activeConnections)
                    m_idleTimer.start(m_idleTimeout * 1000);
            });
            auto buffer = std::make_shared<QByteArray>();
            auto done = std::make_shared<bool>(false);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(130000, socket, [socket] { socket->abort(); });
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer, done] {
                if (*done)
                    return;
                *buffer += socket->readAll();
                if (buffer->size() > 65536) {
                    *done = true;
                    socket->abort();
                    return;
                }
                int sep = buffer->indexOf("\r\n\r\n");
                if (sep < 0)
                    return;
                auto header = buffer->left(sep);
                const auto lines = header.split('\n');
                auto first = lines.value(0).trimmed().split(' ');
                qint64 length = -1;
                int lengthHeaders = 0;
                bool invalid = false;
                for (auto line : lines) {
                    line = line.trimmed();
                    if (line.toLower().startsWith("content-length:")) {
                        bool ok = false;
                        length = line.mid(15).trimmed().toLongLong(&ok);
                        invalid |= !ok;
                        ++lengthHeaders;
                    }
                    if (line.toLower().startsWith("transfer-encoding:"))
                        invalid = true;
                }
                if (invalid || lengthHeaders != 1 || length < 0 || length > 60000 || first.size() != 3) {
                    *done = true;
                    socket->abort();
                    return;
                }
                if (buffer->size() < sep + 4 + length)
                    return;
                *done = true;
                QJsonParseError parse;
                auto doc = QJsonDocument::fromJson(buffer->mid(sep + 4, length), &parse);
                QJsonObject response;
                if (parse.error != QJsonParseError::NoError || !doc.isObject())
                    response = error("INVALID_JSON");
                else
                    try {
                        response =
                            handle(QString::fromUtf8(first[0]), QString::fromUtf8(first[1]), doc.object());
                    } catch (const std::exception &e) {
                        response = error(QString::fromLatin1(e.what()));
                    }
                auto payload = json(response);
                socket->write(QByteArray("HTTP/1.1 ") +
                              (response.contains("error") ? "400 Bad Request" : "200 OK") +
                              "\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
                socket->disconnectFromServer();
            });
        }
    });
    const bool listening = m_server.listen(path);
    if (listening && m_idleTimeout)
        m_idleTimer.start(m_idleTimeout * 1000);
    return listening;
}
void BridgeService::setIdleTimeout(int seconds) {
    m_idleTimeout = seconds;
    m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, [] { QCoreApplication::quit(); });
}
bool BridgeService::adapterValidated() const {
    if (m_sandbox)
        return !m_sandboxUnvalidated;
    if (!compatible())
        return false;
    const auto attestation = readJson(m_state + "/adapter-validation.json");
    // An exact version string alone is not evidence that destructive-format operations were tested.
    return attestation["model"].toString() == "ferrari" && attestation["firmware"].toString() == firmware() &&
           attestation["importRoundtripValidated"].toBool() && attestation["rollbackValidated"].toBool();
}
QJsonObject BridgeService::capabilities() const {
    const bool supported = adapterValidated();
    const auto status = supported ? "experimental" : "unavailable";
    const auto native = m_sandbox ? QJsonObject{} : paper::nativeDocumentsRequest("GET", "/v1/capabilities", {}, 1000);
    const auto nativeStatus = [&native](const char *key) {
        const auto value = native[key].toString();
        return value == "available" || value == "experimental" ? value : QString("unavailable");
    };
    const auto notebookStatus = m_sandbox ? QString(status) : nativeStatus("document.createNotebook");
    const auto openStatus = m_sandbox ? QString("unavailable") : nativeStatus("document.open");
    const auto pdfStatus = m_sandbox ? QString(status) : nativeStatus("document.import.pdf");
    const auto imageStatus = m_sandbox ? QString(status) : nativeStatus("document.import.image");
    const bool nativeNotes = notebookStatus != "unavailable";
    return {{"protocolVersion", 1},
            {"service", "paper-bridge"},
            {"connection", "connected"},
            {"document.list", "available"},
            {"operation.read", "available"},
            {"firmware", m_sandbox ? "sandbox" : firmware()},
            {"adapter", supported ? "ferrari-3.28.0.169" : "unknown"},
            {"document.import.pdf", pdfStatus},
            {"document.import.image", imageStatus},
            {"document.import.scene", m_sandbox ? QString(status) : QString("unavailable")},
            {"document.createNotebook", notebookStatus},
            {"document.import.epub", "unavailable"},
            {"document.import.rmdoc", "unavailable"},
            {"document.open", openStatus},
            {"stencil.activePage", "unavailable"},
            {"message",
             m_sandbox ? (supported ? "Paper Bridge connecté. Imports et carnets disponibles dans la bibliothèque de test."
                                    : "Paper Bridge connecté. L’adaptateur de la bibliothèque de test est désactivé.")
             : pdfStatus != "unavailable" ? "Paper Bridge connecté. Import natif des PDF/images et notes selon les capacités d’AppLoad."
             : nativeNotes ? "Paper Bridge connecté. Notes natives disponibles ; le service d’import natif n’est pas disponible."
                           : "Paper Bridge connecté. Le service de documents natifs d’AppLoad n’est pas disponible."}};
}
QJsonObject BridgeService::nativeImport(const QJsonObject &request) {
    QJsonObject prepared;
    try {
        prepared = paper::prepareNativeImport(request, m_importInputRoot, m_state + "/native-imports");
    } catch (const std::exception &failure) {
        const auto code = QString::fromLatin1(failure.what());
        const auto message = code == "IDEMPOTENCY_CONFLICT"
            ? "Cette demande d’import correspond déjà à un autre contenu ou titre."
            : code == "INPUT_PATH_NOT_ALLOWED" ? "Le fichier téléchargé n’est pas disponible dans le dossier autorisé de l’application."
            : code == "INPUT_HASH_MISMATCH" ? "Le fichier a changé depuis son téléchargement. Téléchargez-le de nouveau."
            : code == "INVALID_OR_ENCRYPTED_PDF" || code == "PDF_PAGE_COUNT_INVALID" ? "Ce PDF est invalide, chiffré ou contient trop de pages."
            : code == "UNSUPPORTED_CONTENT" || code == "IMAGE_DECODE_FAILED" ? "Ce fichier n’est pas un PDF ou une image pris en charge."
            : code == "INPUT_SIZE_INVALID" ? "Le fichier est vide ou dépasse la limite de 64 Mio."
            : "La préparation du fichier pour reMarkable a échoué. Le téléchargement local est conservé.";
        return error(code, message);
    }
    const auto result = paper::nativeDocumentsRequest("POST", "/v1/imports", prepared, 100000);
    if (!result.contains("error") && (QUuid(result["documentId"].toString()).isNull() ||
        result["status"].toString() != "succeeded" || !result["nativeIndexVerified"].toBool()))
        return error("NATIVE_DOCUMENT_UNCONFIRMED", "Le service natif n’a pas confirmé l’import. Réessayez le même fichier pour retrouver son résultat.");
    return result;
}
bool BridgeService::serviceActive(const QString &name) const {
    if (m_sandbox)
        return false;
    QProcess p;
    p.start("/bin/systemctl", {"is-active", "--quiet", name});
    return p.waitForFinished(10000) && p.exitCode() == 0;
}
void BridgeService::service(const QString &action, const QString &name) const {
    if (m_sandbox)
        return;
    must((name == "xochitl" || name == "rm-sync") && (action == "start" || action == "stop"),
         "INVALID_SERVICE_ACTION");
    QProcess p;
    p.start("/bin/systemctl", {action, name});
    must(p.waitForFinished(30000) && p.exitCode() == 0, "SERVICE_ACTION_FAILED");
    must(action == "start" ? serviceActive(name) : !serviceActive(name), "SERVICE_STATE_UNVERIFIED");
}
void BridgeService::state(const QString &operation, const QString &value, const QJsonObject &result) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE operations SET state=?,result_json=?,updated_at=? WHERE id=?");
    q.addBindValue(value);
    q.addBindValue(QString::fromUtf8(json(result)));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(operation);
    must(q.exec(), "JOURNAL_UPDATE_FAILED");
}
void BridgeService::rollback(const QJsonObject &manifest) {
    const auto id = manifest["documentId"].toString();
    must(!QUuid(id).isNull(), "INVALID_ROLLBACK_ID");
    for (auto item : manifest["entries"].toArray()) {
        auto name = item.toString();
        must(name == id || name == id + ".pdf" || name == id + ".content" || name == id + ".metadata" ||
                 name == id + ".pagedata",
             "INVALID_ROLLBACK_ENTRY");
        auto path = m_store + "/" + name;
        QFileInfo f(path);
        if (!f.exists())
            continue;
        must(!f.isSymLink(), "ROLLBACK_SYMLINK");
        if (f.isDir())
            must(QDir(path).removeRecursively(), "ROLLBACK_FAILED");
        else
            must(QFile::remove(path), "ROLLBACK_FAILED");
    }
    paper::syncDirectory(m_store);
}
void BridgeService::recover() {
    if (m_proxy)
        return;
    QLockFile lock(m_state + "/write.lock");
    must(lock.tryLock(1000), "BRIDGE_LOCKED");
    QSqlQuery q(m_db);
    q.exec("SELECT id,state FROM operations WHERE state NOT IN ('succeeded','failed','rolled_back')");
    QList<QPair<QString, QString>> pending;
    while (q.next())
        pending.append({q.value(0).toString(), q.value(1).toString()});
    if (!pending.isEmpty() && !adapterValidated()) {
        // Keep the transaction phase: a committed import must never become rollback-eligible after
        // revalidation.
        for (const auto &entry : pending)
            state(entry.first, entry.second,
                  error("PAPER_FIRMWARE_UNSUPPORTED", "Récupération suspendue : adaptateur non validé pour "
                                                      "ce firmware. Le stockage documentaire est conservé."));
        return;
    }
    for (auto pair : pending) {
        // Older suspended journals did not retain the original phase. Never infer permission to delete from
        // them.
        if (pair.second == "recovery_requires_review") {
            state(
                pair.first, pair.second,
                error("RECOVERY_REQUIRES_REVIEW",
                      "État transactionnel antérieur inconnu : le document est conservé pour vérification."));
            continue;
        }
        auto manifest = readJson(m_state + "/" + pair.first + ".json");
        if (manifest.isEmpty()) {
            state(pair.first, "failed", error("INTERRUPTED_BEFORE_COMMIT"));
            continue;
        }
        if (pair.second == "verifying") {
            // All files were committed before services restarted. Preserve possible user edits after a crash.
            if (QFile::exists(m_store + "/" + manifest["documentId"].toString() + ".metadata"))
                state(pair.first, "succeeded",
                      {{"documentId", manifest["documentId"]},
                       {"message", "Import récupéré après interruption."}});
            else
                state(pair.first, "failed", error("RECOVERY_REQUIRES_REVIEW"));
        } else {
            service("stop", "xochitl");
            service("stop", "rm-sync");
            rollback(manifest);
            state(pair.first, "rolled_back", error("IMPORT_INTERRUPTED"));
        }
        if (manifest["syncWasActive"].toBool())
            service("start", "rm-sync");
        if (manifest["xochitlWasActive"].toBool())
            service("start", "xochitl");
    }
}
QJsonObject BridgeService::import(const QJsonObject &request, bool notebook) {
    if (!adapterValidated())
        return error("PAPER_FIRMWARE_UNSUPPORTED",
                     notebook ? "La création de carnets par l’adaptateur de test n’est pas activée."
                              : "Paper Bridge est connecté, mais l’import de fichiers par cet adaptateur n’est pas activé sur cette version de la tablette. Le fichier téléchargé reste disponible dans reMoodle.");
    auto key = request["idempotencyKey"].toString(), title = request["displayName"].toString();
    must(!key.isEmpty() && key.size() <= 512 && !title.trimmed().isEmpty() && title.size() <= 250,
         "INVALID_IMPORT_REQUEST");
    auto expectedHash = request["sha256"].toString();
    QByteArray signature = json({{"kind", notebook ? "notebook" : "import"},
                                 {"sha256", expectedHash},
                                 {"title", notebook ? QString() : title}});
    auto requestHash = QCryptographicHash::hash(signature, QCryptographicHash::Sha256).toHex();
    QSqlQuery lookup(m_db);
    lookup.prepare("SELECT request_hash,state,result_json FROM operations WHERE idempotency_key=?");
    lookup.addBindValue(key);
    must(lookup.exec(), "JOURNAL_READ_FAILED");
    if (lookup.next()) {
        if (lookup.value(0).toByteArray() != requestHash)
            return error("IDEMPOTENCY_CONFLICT", "Cette clé d’import correspond à un contenu différent.");
        auto previous = QJsonDocument::fromJson(lookup.value(2).toByteArray()).object();
        if (previous.isEmpty())
            return error("IMPORT_IN_PROGRESS");
        return previous;
    }
    QLockFile lock(m_state + "/write.lock");
    must(lock.tryLock(1000), "BRIDGE_BUSY");
    const auto operation = uid(), document = uid();
    QString source;
    if (!notebook) {
        auto path = request["path"].toString();
        QFileInfo info(path);
        const auto canonical = info.canonicalFilePath();
        const auto prefix = m_sandbox ? QFileInfo(m_state + "/inputs").absoluteFilePath() + "/"
                                      : QString("/home/root/.local/share/RePaper/");
        must(!canonical.isEmpty() && canonical.startsWith(prefix) && canonical == info.absoluteFilePath() &&
                 !info.isSymLink() && info.isFile(),
             "INPUT_PATH_NOT_ALLOWED");
        QFile file(canonical);
        must(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= 64 * 1024 * 1024,
             "INPUT_SIZE_INVALID");
        const auto bytes = file.readAll();
        must(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == expectedHash.toLatin1(),
             "INPUT_HASH_MISMATCH");
        source = m_state + "/input-" + operation +
                 (path.endsWith(".paper-scene.json") ? ".paper-scene.json" : ".blob");
        paper::writeDurable(source, bytes);
    }
    QSqlQuery insert(m_db);
    insert.prepare("INSERT INTO operations(id,idempotency_key,request_hash,document_id,state,updated_at) "
                   "VALUES(?,?,?,?,?,?)");
    insert.addBindValue(operation);
    insert.addBindValue(key);
    insert.addBindValue(requestHash);
    insert.addBindValue(document);
    insert.addBindValue("staging");
    insert.addBindValue(QDateTime::currentMSecsSinceEpoch());
    must(insert.exec(), "JOURNAL_INSERT_FAILED");
    QJsonObject manifest;
    bool stopped = false, committed = false;
    paper::Bundle bundle;
    try {
        bundle = paper::prepareBundle(m_store + "/.repaper-staging", document, title, source);
        QJsonArray entries;
        for (const auto &entry : bundle.entries) {
            must(!QFileInfo::exists(m_store + "/" + entry), "DOCUMENT_COLLISION");
            entries.append(entry);
        }
        manifest = {{"documentId", document},
                    {"entries", entries},
                    {"xochitlWasActive", serviceActive("xochitl")},
                    {"syncWasActive", serviceActive("rm-sync")}};
        paper::writeDurable(m_state + "/" + operation + ".json", json(manifest));
        state(operation, "stopping");
        stopped = true;
        service("stop", "xochitl");
        service("stop", "rm-sync");
        state(operation, "installing");
        int installed = 0;
        for (const auto &entry : bundle.entries) {
            must(QDir().rename(bundle.staging + "/" + entry, m_store + "/" + entry), "BUNDLE_INSTALL_FAILED");
            ++installed;
            if (m_sandbox && qEnvironmentVariable("PAPER_BRIDGE_FAULT") == "after_first_file" &&
                installed == 1)
                throw std::runtime_error("INJECTED_FAILURE");
        }
        paper::syncDirectory(m_store);
        state(operation, "verifying");
        committed = true;
        if (manifest["syncWasActive"].toBool())
            service("start", "rm-sync");
        if (manifest["xochitlWasActive"].toBool())
            service("start", "xochitl");
        must(readJson(m_store + "/" + document + ".metadata")["visibleName"].toString() == title.trimmed(),
             "BUNDLE_VERIFICATION_FAILED");
        auto result = QJsonObject{{"operationId", operation},
                                  {"documentId", document},
                                  {"status", "succeeded"},
                                  {"nativeIndexVerified", false},
                                  {"message", m_sandbox
                                      ? "Document enregistré dans la bibliothèque de test sur ce PC."
                                      : "Document importé. Retrouvez-le dans la bibliothèque reMarkable "
                                        "à la fermeture de l’application."}};
        state(operation, "succeeded", result);
        if (!source.isEmpty())
            QFile::remove(source);
        QDir(bundle.staging).removeRecursively();
        return result;
    } catch (const std::exception &e) {
        auto result = error(QString::fromLatin1(e.what()),
                            "L’import n’a pas abouti. Votre document d’origine est conservé.");
        if (stopped && !committed) {
            try {
                service("stop", "xochitl");
                service("stop", "rm-sync");
                rollback(manifest);
                state(operation, "rolled_back", result);
            } catch (...) {
                state(operation, "recovery_required", result);
            }
        } else
            state(operation, committed ? "verifying" : "failed", result);
        if (stopped) {
            try {
                if (manifest["syncWasActive"].toBool())
                    service("start", "rm-sync");
                if (manifest["xochitlWasActive"].toBool())
                    service("start", "xochitl");
            } catch (...) {
            }
        }
        if (!source.isEmpty())
            QFile::remove(source);
        if (!committed)
            QDir(m_store + "/.repaper-staging/" + document).removeRecursively();
        return result;
    }
}
QJsonObject BridgeService::handle(const QString &method, const QString &route, const QJsonObject &body) {
    if (m_proxy)
        return repaper::BridgeClient::request(method, route, body);
    if (method == "GET" && route == "/v1/health")
        return {{"status", "ok"}, {"service", "paper-bridge"}, {"protocolVersion", 1},
                {"mode", m_sandbox ? "sandbox" : "device"}};
    if (method == "GET" && route == "/v1/capabilities")
        return capabilities();
    if (method == "GET" && route == "/v1/documents") {
        QJsonArray documents;
        for (const auto &name : QDir(m_store).entryList({"*.metadata"}, QDir::Files, QDir::Time)) {
            if (documents.size() >= 500)
                break;
            auto meta = readJson(m_store + "/" + name);
            if (meta["deleted"].toBool())
                continue;
            documents.append(QJsonObject{{"documentId", name.chopped(9)},
                                         {"displayName", meta["visibleName"]},
                                         {"type", meta["type"]}});
        }
        return {{"documents", documents}};
    }
    if (method == "POST" && route == "/v1/imports")
        return m_sandbox ? import(body, false) : nativeImport(body);
    if (method == "POST" && route == "/v1/notebooks") {
        if (body.contains("agenda") && !agendaContextValid(body["agenda"]))
            return error("INVALID_AGENDA_CONTEXT", "Le contexte de la note d’agenda est invalide ou incomplet. Rouvrez l’événement pour réessayer.");
        if (m_sandbox)
            return import(body, true);
        const auto title = body["displayName"].toString(), key = body["idempotencyKey"].toString();
        if (title.trimmed().isEmpty() || title.size() > 250 || key.isEmpty() || key.size() > 512)
            return error("INVALID_NOTE_REQUEST", "Le titre ou l’identifiant de la note est invalide.");
        // AppLoad reserves and journals the native UUID before creating through Xochitl.
        // Never fall back to the filesystem writer after an uncertain native response.
        const auto result = paper::nativeDocumentsRequest(method, route, body);
        if (!result.contains("error") && (QUuid(result["documentId"].toString()).isNull() ||
            result["status"].toString() != "succeeded" || !result["nativeIndexVerified"].toBool()))
            return error("NATIVE_DOCUMENT_UNCONFIRMED", "Le service natif n’a pas confirmé la création de la note. Réessayez la même note pour la retrouver.");
        return result;
    }
    if (method == "POST" && route.startsWith("/v1/documents/") && route.endsWith("/open")) {
        if (!m_sandbox) {
            const auto id = route.mid(14, route.size() - 19);
            if (QUuid(id).isNull())
                return error("INVALID_DOCUMENT_ID", "L’identifiant du document à ouvrir est invalide.");
            const auto result = paper::nativeDocumentsRequest(method, route, body);
            if (!result.contains("error") && result["status"].toString() != "opened")
                return error("NATIVE_DOCUMENT_UNCONFIRMED", "Le service natif n’a pas confirmé l’ouverture de la note.");
            return result;
        }
        return error("DOCUMENT_OPEN_UNAVAILABLE",
                     m_sandbox ? "Le document est dans la bibliothèque de test PC. L’ouverture Xochitl "
                                 "n’est pas simulée."
                               : "Ouvrez ce document dans la bibliothèque reMarkable après avoir fermé "
                                 "l’application.");
    }
    if (method == "GET" && route.startsWith("/v1/operations/")) {
        QSqlQuery q(m_db);
        q.prepare("SELECT id,document_id,state,result_json FROM operations WHERE id=?");
        q.addBindValue(route.mid(15));
        q.exec();
        if (q.next())
            return {{"operationId", q.value(0).toString()},
                    {"documentId", q.value(1).toString()},
                    {"state", q.value(2).toString()},
                    {"result", QJsonDocument::fromJson(q.value(3).toByteArray()).object()}};
    }
    return error("NOT_FOUND");
}

#include "NativeDocumentHost.h"
#include "AgendaNoteContext.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QUrl>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qsizetype MaxRequest = 65536;
constexpr qint64 MaxImport = 64 * 1024 * 1024;
QString canonicalUuid(const QString &value) {
    static const QRegularExpression pattern("^[{]?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}[}]?$");
    if (!pattern.match(value).hasMatch() || QUuid(value).isNull()) return {};
    return QUuid(value).toString(QUuid::WithoutBraces);
}
bool privateDirectory(const QString &path) {
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isAbsolute()) return false;
    if (!QDir().mkpath(path)) return false;
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}
bool hexHash(const QString &value) {
    static const QRegularExpression pattern("^[0-9a-f]{64}$");
    return pattern.match(value).hasMatch();
}
bool validImportToken(const QString &value) {
    return value.startsWith("rePaper-") && value.mid(8) == canonicalUuid(value.mid(8))
        && !value.mid(8).isEmpty();
}
bool durableDirectory(const QString &path) {
    const int directory = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return false;
    const bool result = fsync(directory) == 0;
    close(directory);
    return result;
}
bool ownedImportDirectory(const QString &path) {
    if (!privateDirectory(path)) return false;
    return QFileInfo(path).canonicalFilePath() == QDir::cleanPath(path)
        && durableDirectory(path) && durableDirectory(QFileInfo(path).absolutePath());
}
int openBoundedInput(const QString &root, const QString &path) {
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanPath = QDir::cleanPath(path);
    if (!QFileInfo(path).isAbsolute() || cleanPath != path
        || !cleanPath.startsWith(cleanRoot + '/')
        || QFileInfo(root).canonicalFilePath() != cleanRoot
        || !QFileInfo(path).canonicalFilePath().startsWith(cleanRoot + '/')) return -1;
    const auto parts = cleanPath.mid(cleanRoot.size() + 1).split('/');
    int descriptor = ::open(cleanRoot.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return -1;
    for (int index = 0; index < parts.size(); ++index) {
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (index + 1 < parts.size() ? O_DIRECTORY : O_NONBLOCK);
        const int next = openat(descriptor, parts[index].toLocal8Bit().constData(), flags);
        close(descriptor);
        if (next < 0) return -1;
        descriptor = next;
    }
    return descriptor;
}
}

NativeDocumentHost::NativeDocumentHost(QObject *parent) : QObject(parent) {
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (m_server.hasPendingConnections()) {
            auto socket = m_server.nextPendingConnection();
            struct ucred credential{};
            socklen_t size = sizeof credential;
            if (m_connections.size() >= 8 || getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED,
                                                       &credential, &size) != 0 || credential.uid != geteuid()) {
                socket->abort(); socket->deleteLater(); continue;
            }
            socket->setReadBufferSize(MaxRequest + 8192);
            m_connections.insert(socket, {});
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(socket); });
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                m_connections.remove(socket);
                for (const auto &request : m_pending.keys())
                    if (m_pending[request].importing && m_pending[request].socket == socket)
                        finish(request, failure("NATIVE_TIMEOUT", "La connexion d’import a été interrompue.", true));
                socket->deleteLater();
            });
            QTimer::singleShot(15000, socket, [this, socket] {
                if (m_connections.contains(socket) && !m_connections[socket].handled) socket->disconnectFromServer();
            });
        }
    });
}

NativeDocumentHost::~NativeDocumentHost() {
    for (auto socket : m_connections.keys()) {
        disconnect(socket, nullptr, this, nullptr);
        socket->abort();
    }
    m_server.close();
}

void NativeDocumentHost::componentComplete() {
    m_complete = true;
    start();
}

void NativeDocumentHost::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        for (const auto &request : m_pending.keys())
            finish(request, failure("NATIVE_LIBRARY_NOT_READY", "La bibliothèque reMarkable n’est plus prête. Réessayez.", true));
    }
    if (m_complete) start();
    emit enabledChanged();
}

void NativeDocumentHost::setImportsEnabled(bool enabled) {
    if (m_importsEnabled == enabled) return;
    m_importsEnabled = enabled;
    if (!enabled)
        for (const auto &request : m_pending.keys())
            if (m_pending[request].importing)
                finish(request, failure("NATIVE_IMPORT_NOT_READY", "L’import reMarkable n’est plus prêt. Réessayez.", true));
    emit importsEnabledChanged();
}

void NativeDocumentHost::start() {
    if (m_server.isListening()) return;
    if (!privateDirectory(m_stateDirectory) || !privateDirectory(QFileInfo(m_socketPath).absolutePath())) return;
    m_lock = std::make_unique<QLockFile>(m_stateDirectory + "/host.lock");
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) { m_lock.reset(); return; }
    // The state lock excludes another instance; removing a dead socket is safe.
    QLocalSocket existing;
    existing.connectToServer(m_socketPath);
    if (existing.waitForConnected(100)) { m_lock.reset(); return; }
    QLocalServer::removeServer(m_socketPath);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(m_socketPath)) m_lock.reset();
}

QJsonObject NativeDocumentHost::failure(const QString &code, const QString &message, bool retryable) const {
    return {{"error", QJsonObject{{"code", code}, {"message", message}, {"retryable", retryable}}}};
}

void NativeDocumentHost::respond(QLocalSocket *socket, const QJsonObject &body) {
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return;
    const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QByteArray status = body.contains("error") ? "400 Bad Request" : "200 OK";
    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
                  + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
    socket->flush();
    socket->disconnectFromServer();
}

void NativeDocumentHost::read(QLocalSocket *socket) {
    if (!m_connections.contains(socket)) return;
    auto &connection = m_connections[socket];
    if (connection.handled) return;
    connection.bytes += socket->readAll();
    auto reject = [this, socket, &connection] {
        connection.handled = true;
        respond(socket, failure("INVALID_REQUEST", "Requête native invalide."));
    };
    if (connection.bytes.size() > MaxRequest + 8192) { reject(); return; }
    if (connection.headerEnd < 0) {
        connection.headerEnd = connection.bytes.indexOf("\r\n\r\n");
        if (connection.headerEnd < 0) { if (connection.bytes.size() > 8192) reject(); return; }
        if (connection.headerEnd > 8192) { reject(); return; }
        const auto lines = connection.bytes.left(connection.headerEnd).split('\n');
        int lengths = 0;
        for (const auto &raw : lines.mid(1)) {
            const auto line = raw.trimmed();
            const int colon = line.indexOf(':');
            if (colon < 1) { reject(); return; }
            const auto name = line.left(colon).toLower();
            if (name == "transfer-encoding") { reject(); return; }
            if (name == "content-length") {
                bool valid = false;
                connection.expected = line.mid(colon + 1).trimmed().toLongLong(&valid);
                if (!valid || connection.expected < 0 || connection.expected > MaxRequest || ++lengths != 1) { reject(); return; }
            }
        }
        if (lengths != 1) { reject(); return; }
    }
    const auto bodyOffset = connection.headerEnd + 4;
    if (connection.bytes.size() < bodyOffset + connection.expected) return;
    if (connection.bytes.size() != bodyOffset + connection.expected) { reject(); return; }
    const auto firstLine = connection.bytes.left(connection.bytes.indexOf("\r\n")).split(' ');
    if (firstLine.size() != 3 || firstLine[2] != "HTTP/1.1") { reject(); return; }
    QJsonParseError error;
    const auto data = QJsonDocument::fromJson(connection.bytes.mid(bodyOffset), &error);
    if (error.error != QJsonParseError::NoError || !data.isObject()) { reject(); return; }
    connection.handled = true;
    connection.bytes.clear();
    dispatch(socket, QString::fromLatin1(firstLine[0]), QString::fromLatin1(firstLine[1]), data.object());
}

bool NativeDocumentHost::writeJournal(const QString &path, const QJsonObject &journal) {
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile())
        || QFileInfo(info.absolutePath()).canonicalFilePath() != QDir::cleanPath(info.absolutePath())) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto bytes = QJsonDocument(journal).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.flush() || fsync(file.handle()) != 0 || !file.commit()) return false;
    const int directory = ::open(QFileInfo(path).absolutePath().toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return false;
    const bool durable = fsync(directory) == 0;
    close(directory);
    return durable;
}

void NativeDocumentHost::dispatch(QLocalSocket *socket, const QString &method, const QString &route, const QJsonObject &body) {
    if (method == "GET" && route == "/v1/health") {
        respond(socket, {{"status", "ok"}, {"service", "repaper-native-documents"}, {"protocolVersion", 1}}); return;
    }
    if (method == "GET" && route == "/v1/capabilities") {
        const QString value = m_enabled ? "available" : "unavailable";
        const QString imports = m_enabled && m_importsEnabled ? "available" : "unavailable";
        respond(socket, {{"document.createNotebook", value}, {"document.open", value},
                         {"document.import.pdf", imports}, {"document.import.image", imports}}); return;
    }
    if (!m_enabled) { respond(socket, failure("NATIVE_LIBRARY_NOT_READY", "La bibliothèque reMarkable n’est pas encore prête.", true)); return; }
    if (method == "POST" && route == "/v1/imports") { requestImport(socket, body); return; }
    if (m_pending.size() >= 4) { respond(socket, failure("NATIVE_BUSY", "Une opération native est déjà en cours.", true)); return; }
    Pending operation;
    operation.socket = socket;
    if (body.contains("callerQtfbKey")) {
        const auto value = body.value("callerQtfbKey").toDouble(-1);
        if (value < 0 || value > 2147483647 || value != qint64(value)) {
            respond(socket, failure("INVALID_CALLER", "Identifiant d’application invalide.")); return;
        }
        operation.callerKey = int(value);
    }
    if (method == "POST" && route == "/v1/notebooks") {
        const auto title = body.value("displayName").toString().trimmed();
        const auto key = body.value("idempotencyKey").toString();
        if (title.isEmpty() || title.size() > 512 || key.isEmpty() || key.size() > 4096) {
            respond(socket, failure("INVALID_NOTEBOOK", "Le titre ou l’identifiant du carnet est invalide.")); return;
        }
        QJsonObject agenda;
        if (body.contains("agenda")
            && (!AgendaNoteContext::normalize(body["agenda"], &agenda)
                || (agenda["kind"] == "event" && key != "reagenda:event:" + agenda["event"].toObject()["id"].toString())
                || (agenda["kind"] == "day" && key != "reagenda:day:" + agenda["date"].toString()))) {
            respond(socket, failure("INVALID_AGENDA_CONTEXT", "Le contexte de la note d’agenda est invalide.")); return;
        }
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
        operation.journalPath = m_stateDirectory + "/" + hash + ".json";
        for (const auto &pending : m_pending)
            if (pending.journalPath == operation.journalPath) {
                respond(socket, failure("NATIVE_BUSY", "La création de ce carnet est en cours.", true)); return;
            }
        const QFileInfo journalInfo(operation.journalPath);
        if (journalInfo.isSymLink() || (journalInfo.exists() && !journalInfo.isFile())) {
            respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le lien local du carnet est invalide.")); return;
        }
        if (journalInfo.exists()) {
            QFile file(operation.journalPath);
            if (file.size() > MaxRequest || !file.open(QIODevice::ReadOnly)) {
                respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le lien local du carnet ne peut pas être lu.")); return;
            }
            operation.journal = QJsonDocument::fromJson(file.readAll()).object();
            const QString journalTitle = operation.journal["displayName"].toString();
            const QString journalState = operation.journal["state"].toString();
            if (operation.journal["version"].toDouble() != 1 || operation.journal["keyHash"].toString() != hash
                || canonicalUuid(operation.journal["documentId"].toString()).isEmpty()
                || canonicalUuid(operation.journal["pageId"].toString()).isEmpty()
                || journalTitle.trimmed().isEmpty() || journalTitle.size() > 512
                || (journalState != "reserved" && journalState != "created")) {
                respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le lien local du carnet est invalide.")); return;
            }
            operation.journal["documentId"] = canonicalUuid(operation.journal["documentId"].toString());
            operation.journal["pageId"] = canonicalUuid(operation.journal["pageId"].toString());
            if (operation.journal.contains("agenda")) {
                QJsonObject stored;
                const auto pageState = operation.journal["agendaPageState"].toString();
                if (!AgendaNoteContext::normalize(operation.journal["agenda"], &stored)
                    || (pageState != "pending" && pageState != "dispatched" && pageState != "completed" && pageState != "legacy")
                    || ((pageState == "dispatched" || pageState == "completed")
                        && !hexHash(operation.journal["agendaPlanHash"].toString()))) {
                    respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le contexte enregistré du carnet est invalide.")); return;
                }
                operation.journal["agenda"] = stored;
            } else if (!agenda.isEmpty()) {
                // Existing notebooks can be moved into the native calendar
                // folder, but their page layout and handwriting remain intact.
                operation.journal["agenda"] = agenda;
                operation.journal["agendaPageState"] = "legacy";
                if (!writeJournal(operation.journalPath, operation.journal)) {
                    respond(socket, failure("NATIVE_JOURNAL_WRITE", "Le classement du carnet ne peut pas être enregistré.")); return;
                }
            }
        } else {
            operation.journal = {{"version", 1}, {"keyHash", hash}, {"state", "reserved"},
                {"documentId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"pageId", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"displayName", title}};
            if (!agenda.isEmpty()) {
                operation.journal["agenda"] = agenda;
                operation.journal["agendaPageState"] = "pending";
            }
            if (!writeJournal(operation.journalPath, operation.journal)) {
                respond(socket, failure("NATIVE_JOURNAL_WRITE", "Le lien du carnet ne peut pas être enregistré.")); return;
            }
        }
        operation.documentId = operation.journal["documentId"].toString();
        m_noteJournalPaths.insert(operation.documentId, operation.journalPath);
    } else if (method == "POST" && route.startsWith("/v1/documents/") && route.endsWith("/open")) {
        operation.documentId = canonicalUuid(route.mid(14, route.size() - 14 - 5));
        if (operation.documentId.isEmpty()) { respond(socket, failure("INVALID_DOCUMENT_ID", "Identifiant du document invalide.")); return; }
        operation.opening = true;
        const auto journalPath = m_noteJournalPaths.value(operation.documentId);
        if (!journalPath.isEmpty() && !QFileInfo(journalPath).isSymLink()) {
            QFile file(journalPath);
            if (file.size() <= MaxRequest && file.open(QIODevice::ReadOnly)) {
                const auto journal = QJsonDocument::fromJson(file.readAll()).object();
                if (journal["documentId"].toString() == operation.documentId) {
                    operation.journalPath = journalPath;
                    operation.journal = journal;
                }
            }
        }
    } else { respond(socket, failure("NOT_FOUND", "Opération native inconnue.")); return; }
    const QString request = QUuid::createUuid().toString(QUuid::WithoutBraces);
    operation.timeout = new QTimer(this);
    operation.timeout->setSingleShot(true);
    operation.timeout->setInterval(15000);
    connect(operation.timeout, &QTimer::timeout, this, [this, request] {
        finish(request, failure("NATIVE_TIMEOUT", "La bibliothèque reMarkable n’a pas confirmé l’opération. Réessayez.", true));
    });
    m_pending.insert(request, operation);
    operation.timeout->start();
    if (operation.opening) emit openRequested(request, operation.documentId);
    else emit createRequested(request, operation.documentId, operation.journal["pageId"].toString(), operation.journal["displayName"].toString());
}

QVariantMap NativeDocumentHost::agendaContext(const QString &request) const {
    const auto found = m_pending.constFind(request);
    if (found == m_pending.constEnd() || found->importing) return {};
    QJsonObject agenda;
    if (!AgendaNoteContext::normalize(found->journal["agenda"], &agenda)) return {};
    const auto state = found->journal["agendaPageState"].toString("legacy");
    return QJsonObject{{"agenda", agenda},
        {"fields", AgendaNoteContext::fields(agenda, found->journal["displayName"].toString())},
        {"initializePage", state == "pending" || state == "dispatched"}, {"pageState", state},
        {"planHash", found->journal["agendaPlanHash"]},
        {"documentId", found->documentId}, {"pageId", found->journal["pageId"]}}.toVariantMap();
}

bool NativeDocumentHost::beginAgendaPage(const QString &request, const QString &planHash) {
    const auto found = m_pending.find(request);
    if (found == m_pending.end()) return false;
    const auto reject = [this, &request](const char *code, const QString &message) {
        finish(request, failure(code, message, true));
        return false;
    };
    if (!m_enabled || !found->opening || found->importing || found->journalPath.isEmpty() || !hexHash(planHash)
        || found->journal["state"].toString() != "created" || agendaContext(request).isEmpty())
        return reject("NATIVE_AGENDA_NOT_READY", "Le préremplissage de cette page n’est pas disponible.");
    const auto state = found->journal["agendaPageState"].toString();
    if (state == "dispatched") {
        if (found->journal["agendaPlanHash"].toString() == planHash) return true;
        return reject("NATIVE_AGENDA_PLAN_MISMATCH", "Cette page attend la confirmation d’un autre préremplissage.");
    }
    if (state != "pending") return reject("NATIVE_AGENDA_ALREADY_INITIALIZED", "Cette page a déjà été préparée ; son contenu est conservé.");
    auto journal = found->journal;
    journal["agendaPageState"] = "dispatched";
    journal["agendaPlanHash"] = planHash;
    if (!writeJournal(found->journalPath, journal))
        return reject("NATIVE_JOURNAL_WRITE", "Le préremplissage n’a pas pu être enregistré avant son envoi.");
    found->journal = journal;
    return true;
}

bool NativeDocumentHost::completeAgendaPage(const QString &request, const QString &planHash) {
    const auto found = m_pending.find(request);
    if (found == m_pending.end()) return false;
    if (!m_enabled || !found->opening || found->importing || found->journalPath.isEmpty()
        || found->journal["agendaPageState"].toString() != "dispatched"
        || !hexHash(planHash) || found->journal["agendaPlanHash"].toString() != planHash) {
        finish(request, failure("NATIVE_AGENDA_CONFIRMATION_MISMATCH", "La page d’agenda n’a pas confirmé le préremplissage attendu.", true));
        return false;
    }
    auto journal = found->journal;
    journal["agendaPageState"] = "completed";
    if (!writeJournal(found->journalPath, journal)) {
        finish(request, failure("NATIVE_JOURNAL_WRITE", "La page est préremplie, mais sa confirmation n’a pas pu être conservée. Réessayez.", true));
        return false;
    }
    found->journal = journal;
    return true;
}

void NativeDocumentHost::finish(const QString &request, const QJsonObject &body) {
    if (!m_pending.contains(request)) return;
    const auto operation = m_pending.take(request);
    operation.timeout->stop();
    operation.timeout->deleteLater();
    respond(operation.socket, body);
}

void NativeDocumentHost::created(const QString &request, const QString &id) {
    if (!m_pending.contains(request)) return;
    auto &operation = m_pending[request];
    if (operation.opening || operation.importing || canonicalUuid(id) != operation.documentId) {
        finish(request, failure("NATIVE_ID_MISMATCH", "La bibliothèque a retourné un autre identifiant de carnet.")); return;
    }
    operation.journal["state"] = "created";
    if (!writeJournal(operation.journalPath, operation.journal)) {
        finish(request, failure("NATIVE_JOURNAL_WRITE", "Le carnet existe, mais son lien n’a pas pu être confirmé. Réessayez.", true)); return;
    }
    const QString documentId = operation.documentId;
    finish(request, {{"documentId", documentId}, {"status", "succeeded"}, {"nativeIndexVerified", true}, {"message", "Carnet prêt."}});
}

void NativeDocumentHost::opened(const QString &request, const QString &id) {
    if (!m_pending.contains(request)) return;
    const auto operation = m_pending[request];
    if (!operation.opening || canonicalUuid(id) != operation.documentId) {
        finish(request, failure("NATIVE_ID_MISMATCH", "Le document demandé n’a pas été ouvert.")); return;
    }
    finish(request, {{"status", "opened"}, {"message", "Carnet ouvert dans reMarkable."}});
    if (operation.callerKey >= 0)
        QTimer::singleShot(150, this, [this, key = operation.callerKey] { emit dismissAppRequested(key); });
}

void NativeDocumentHost::rejected(const QString &request, const QString &code, const QString &message) {
    finish(request, failure(code.left(80), message.left(512)));
}

bool NativeDocumentHost::snapshotImport(const QString &path, const QString &sha256,
                                        const QString &destination, QString *code) {
    const int descriptor = openBoundedInput(m_inputRoot, path);
    if (descriptor < 0) { *code = "INVALID_IMPORT_PATH"; return false; }
    struct stat info{};
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(descriptor); *code = "INVALID_IMPORT_PATH"; return false;
    }
    if (info.st_size > MaxImport) { close(descriptor); *code = "IMPORT_TOO_LARGE"; return false; }
    QFile input;
    if (!input.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        close(descriptor); *code = "INVALID_IMPORT_PATH"; return false;
    }
    const QFileInfo target(destination);
    if (target.isSymLink() || (target.exists() && !target.isFile())
        || !ownedImportDirectory(target.absolutePath())) {
        *code = "NATIVE_JOURNAL_INVALID"; return false;
    }
    QSaveFile snapshot(destination);
    if (!snapshot.open(QIODevice::WriteOnly)
        || !snapshot.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        *code = "NATIVE_JOURNAL_WRITE"; return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!input.atEnd()) {
        const auto chunk = input.read(65536);
        if (chunk.isEmpty() && input.error() != QFileDevice::NoError) { *code = "INVALID_IMPORT_PATH"; return false; }
        if (total == 0 && !chunk.startsWith("%PDF-")) { *code = "INVALID_PDF"; return false; }
        total += chunk.size();
        if (total > MaxImport) { *code = "IMPORT_TOO_LARGE"; return false; }
        hash.addData(chunk);
        if (snapshot.write(chunk) != chunk.size()) { *code = "NATIVE_JOURNAL_WRITE"; return false; }
    }
    if (total < 5) { *code = "INVALID_PDF"; return false; }
    if (QString::fromLatin1(hash.result().toHex()) != sha256) { *code = "IMPORT_HASH_MISMATCH"; return false; }
    if (!snapshot.flush() || fsync(snapshot.handle()) != 0 || !snapshot.commit() || !durableDirectory(target.absolutePath())) {
        *code = "NATIVE_JOURNAL_WRITE"; return false;
    }
    return true;
}

void NativeDocumentHost::requestImport(QLocalSocket *socket, const QJsonObject &body) {
    if (!m_importsEnabled) {
        respond(socket, failure("NATIVE_IMPORT_NOT_READY", "L’import reMarkable n’est pas encore prêt.", true)); return;
    }
    const QString path = body["path"].toString();
    const QString sha256 = body["sha256"].toString();
    const QString title = body["displayName"].toString().trimmed();
    const QString key = body["idempotencyKey"].toString();
    if (path.isEmpty() || path.size() > 4096 || path.contains(QChar::Null) || !QFileInfo(path).isAbsolute()
        || !hexHash(sha256) || title.isEmpty() || title.size() > 512 || title.contains(QChar::Null)
        || key.isEmpty() || key.size() > 4096) {
        respond(socket, failure("INVALID_IMPORT", "Le document préparé ou son identifiant est invalide.")); return;
    }
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
    if (m_importJobs.contains(hash) && (m_importJobs[hash].journal["sha256"].toString() != sha256
        || m_importJobs[hash].journal["displayName"].toString() != title)) {
        respond(socket, failure("IDEMPOTENCY_CONFLICT", "Cet identifiant d’import désigne un autre contenu ou titre.")); return;
    }
    for (const auto &pending : m_pending)
        if (pending.importing && pending.importKey == hash) {
            respond(socket, failure("NATIVE_BUSY", "L’import de ce document est en cours.", true)); return;
        }
    if (m_pending.size() >= 4) {
        respond(socket, failure("NATIVE_BUSY", "Des opérations natives sont déjà en cours.", true)); return;
    }
    const QString importsRoot = m_stateDirectory + "/imports";
    if (!ownedImportDirectory(importsRoot) || !durableDirectory(QFileInfo(m_stateDirectory).absolutePath())) {
        respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le dossier privé d’import est invalide.")); return;
    }
    if (!m_importJobs.contains(hash)) {
        ImportJob job;
        job.journalPath = importsRoot + '/' + hash + ".json";
        const QFileInfo info(job.journalPath);
        if (info.isSymLink() || (info.exists() && !info.isFile())) {
            respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le suivi local d’import est invalide.")); return;
        }
        if (info.exists()) {
            QFile file(job.journalPath);
            if (file.size() > MaxRequest || !file.open(QIODevice::ReadOnly)) {
                respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le suivi local d’import ne peut pas être lu.")); return;
            }
            job.journal = QJsonDocument::fromJson(file.readAll()).object();
            const QString state = job.journal["state"].toString();
            const QString id = job.journal["documentId"].toString();
            const QString storedTitle = job.journal["displayName"].toString();
            const bool hasId = state == "identified" || state == "completed";
            if (job.journal["version"].toDouble() != 1 || job.journal["kind"].toString() != "import"
                || job.journal["keyHash"].toString() != hash || !hexHash(job.journal["sha256"].toString())
                || !validImportToken(job.journal["token"].toString())
                || storedTitle.trimmed().isEmpty() || storedTitle.size() > 512 || storedTitle.contains(QChar::Null)
                || (state != "prepared" && state != "dispatched" && state != "identified" && state != "completed" && state != "failed")
                || (hasId ? canonicalUuid(id).isEmpty() : !id.isEmpty())) {
                respond(socket, failure("NATIVE_JOURNAL_INVALID", "Le suivi local d’import est invalide.")); return;
            }
            if (hasId) job.journal["documentId"] = canonicalUuid(id);
        } else {
            job.journal = {{"version", 1}, {"kind", "import"}, {"keyHash", hash}, {"sha256", sha256},
                {"displayName", title}, {"token", "rePaper-" + QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"state", "prepared"}, {"documentId", ""}};
        }
        job.sourcePath = importsRoot + '/' + hash + '/' + job.journal["token"].toString() + ".pdf";
        m_importJobs.insert(hash, job);
    }
    auto &job = m_importJobs[hash];
    if (job.journal["sha256"].toString() != sha256 || job.journal["displayName"].toString() != title) {
        respond(socket, failure("IDEMPOTENCY_CONFLICT", "Cet identifiant d’import désigne un autre contenu ou titre.")); return;
    }
    QString state = job.journal["state"].toString();
    if (state == "prepared" || state == "failed") {
        int active = 0;
        for (const auto &existing : m_importJobs) if (existing.awaitingNative) ++active;
        if (!job.awaitingNative && active >= 4) {
            respond(socket, failure("NATIVE_BUSY", "Quatre imports natifs sont déjà en cours.", true)); return;
        }
        QString code;
        if (!snapshotImport(path, sha256, job.sourcePath, &code)) {
            if (!QFileInfo::exists(job.journalPath)) m_importJobs.remove(hash);
            respond(socket, failure(code, "Le PDF préparé n’a pas pu être vérifié et réservé.")); return;
        }
        auto journal = job.journal;
        journal["state"] = "prepared";
        if (!writeJournal(job.journalPath, journal)) {
            respond(socket, failure("NATIVE_JOURNAL_WRITE", "L’import ne peut pas être enregistré.")); return;
        }
        job.journal = journal;
        state = "prepared";
    }
    QString id = job.journal["documentId"].toString();
    if (state == "dispatched" && id.isEmpty()) {
        id = findImportedDocument(hash);
        if (id.isEmpty()) {
            respond(socket, failure("NATIVE_IMPORT_UNCERTAIN",
                "L’import a été envoyé à reMarkable, mais son résultat n’est pas encore confirmé. Aucun doublon n’a été créé.", true));
            return;
        }
    }
    Pending operation;
    operation.socket = socket;
    operation.importing = true;
    operation.importKey = hash;
    operation.timeout = new QTimer(this);
    operation.timeout->setSingleShot(true);
    operation.timeout->setInterval(90000);
    const QString request = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connect(operation.timeout, &QTimer::timeout, this, [this, request] {
        finish(request, failure("NATIVE_TIMEOUT", "reMarkable n’a pas encore confirmé l’import. Son suivi est conservé.", true));
    });
    m_pending.insert(request, operation);
    operation.timeout->start();
    if (state != "completed") job.awaitingNative = true;
    emit importRequested(hash, QUrl::fromLocalFile(job.sourcePath).toString(), job.journal["token"].toString(),
                         id, job.journal["displayName"].toString(), state != "prepared", state == "completed");
}

void NativeDocumentHost::finishImport(const QString &keyHash, const QJsonObject &body) {
    for (const auto &request : m_pending.keys())
        if (m_pending[request].importing && m_pending[request].importKey == keyHash)
            finish(request, body);
}

bool NativeDocumentHost::dispatchImport(const QString &keyHash) {
    if (!m_enabled || !m_importsEnabled || !m_importJobs.contains(keyHash)) {
        finishImport(keyHash, failure("NATIVE_IMPORT_NOT_READY", "L’import natif n’est plus prêt.", true)); return false;
    }
    auto &job = m_importJobs[keyHash];
    if (job.journal["state"].toString() != "prepared") {
        finishImport(keyHash, failure("NATIVE_IMPORT_UNCERTAIN", "Cet import a déjà été envoyé à reMarkable.", true)); return false;
    }
    auto journal = job.journal;
    journal["state"] = "dispatched";
    if (!writeJournal(job.journalPath, journal)) {
        finishImport(keyHash, failure("NATIVE_JOURNAL_WRITE", "L’envoi de l’import ne peut pas être enregistré.", true)); return false;
    }
    job.journal = journal;
    job.awaitingNative = true;
    return true;
}

bool NativeDocumentHost::identifyImport(const QString &keyHash, const QString &documentId) {
    if (!m_enabled || !m_importsEnabled || !m_importJobs.contains(keyHash)) {
        finishImport(keyHash, failure("NATIVE_IMPORT_NOT_READY", "L’import natif n’est plus prêt.", true)); return false;
    }
    auto &job = m_importJobs[keyHash];
    const QString id = canonicalUuid(documentId);
    const QString stored = job.journal["documentId"].toString();
    const QString state = job.journal["state"].toString();
    if (id.isEmpty() || (!stored.isEmpty() && stored != id)
        || (state != "dispatched" && state != "identified" && state != "completed")) {
        finishImport(keyHash, failure("NATIVE_ID_MISMATCH", "L’identifiant du document importé est incohérent.")); return false;
    }
    if (state == "completed" || state == "identified") return true;
    auto journal = job.journal;
    journal["documentId"] = id;
    journal["state"] = "identified";
    if (!writeJournal(job.journalPath, journal)) {
        finishImport(keyHash, failure("NATIVE_JOURNAL_WRITE", "Le document existe, mais son identifiant n’a pas pu être conservé.", true)); return false;
    }
    job.journal = journal;
    return true;
}

void NativeDocumentHost::completeImport(const QString &keyHash, const QString &documentId) {
    if (!m_enabled || !m_importsEnabled || !m_importJobs.contains(keyHash)) return;
    auto &job = m_importJobs[keyHash];
    const QString id = canonicalUuid(documentId);
    const QString state = job.journal["state"].toString();
    if (id.isEmpty() || id != job.journal["documentId"].toString()
        || (state != "identified" && state != "completed")) {
        finishImport(keyHash, failure("NATIVE_ID_MISMATCH", "L’import n’a pas été identifié durablement.")); return;
    }
    if (state != "completed") {
        auto journal = job.journal;
        journal["state"] = "completed";
        if (!writeJournal(job.journalPath, journal)) {
            finishImport(keyHash, failure("NATIVE_JOURNAL_WRITE", "Le document est importé, mais sa confirmation n’a pas pu être conservée.", true)); return;
        }
        job.journal = journal;
    }
    job.awaitingNative = false;
    // Only our own staging file is removed, after the completed journal is durable.
    if (!QFileInfo(job.sourcePath).isSymLink()
        && QFileInfo(QFileInfo(job.sourcePath).absolutePath()).canonicalFilePath()
            == QDir::cleanPath(QFileInfo(job.sourcePath).absolutePath()))
        QFile::remove(job.sourcePath);
    finishImport(keyHash, {{"documentId", id}, {"status", "succeeded"},
                          {"nativeIndexVerified", true}, {"message", "Document importé."}});
}

void NativeDocumentHost::failImport(const QString &keyHash, const QString &code, const QString &message) {
    if (!m_enabled || !m_importsEnabled || !m_importJobs.contains(keyHash)) return;
    auto &job = m_importJobs[keyHash];
    const QString state = job.journal["state"].toString();
    // Only a pre-call rejection or a correlated native failed(url) permits a
    // fresh native call. An index delay/exception after dispatch is uncertain.
    if (state == "prepared" || (state == "dispatched" && code == "NATIVE_IMPORT_FAILED")) {
        auto journal = job.journal;
        journal["state"] = "failed";
        if (!writeJournal(job.journalPath, journal)) {
            finishImport(keyHash, failure("NATIVE_JOURNAL_WRITE", "L’échec de l’import ne peut pas être enregistré.", true)); return;
        }
        job.journal = journal;
        job.awaitingNative = false;
    }
    finishImport(keyHash, failure(code.left(80), message.left(512), true));
}

QString NativeDocumentHost::findImportedDocument(const QString &keyHash) const {
    if (!m_importJobs.contains(keyHash)) return {};
    const QString token = m_importJobs[keyHash].journal["token"].toString();
    if (!validImportToken(token) || QFileInfo(m_metadataRoot).canonicalFilePath() != QDir::cleanPath(m_metadataRoot)) return {};
    QString found;
    QDirIterator entries(m_metadataRoot, {"*.metadata"}, QDir::Files | QDir::NoSymLinks);
    while (entries.hasNext()) {
        entries.next();
        const QFileInfo info = entries.fileInfo();
        const QString id = canonicalUuid(info.completeBaseName());
        if (id.isEmpty() || info.size() > MaxRequest) continue;
        const int descriptor = openBoundedInput(m_metadataRoot, info.absoluteFilePath());
        if (descriptor < 0) continue;
        struct stat attributes{};
        if (fstat(descriptor, &attributes) || !S_ISREG(attributes.st_mode) || attributes.st_size > MaxRequest) {
            close(descriptor); continue;
        }
        QFile file;
        if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { close(descriptor); continue; }
        const auto value = QJsonDocument::fromJson(file.read(MaxRequest + 1)).object();
        if (value["deleted"].toBool() || value["parent"].toString() == "trash") continue;
        const QString name = value["visibleName"].toString();
        if (name != token && name != token + ".pdf") continue;
        if (!found.isEmpty() && found != id) return {};
        found = id;
    }
    return found;
}

#include "ChatController.h"
#include "CoreProcess.h"
#include "Credential.h"
#include "SecretStore.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

namespace rmchat {
namespace {
const QString entry = QStringLiteral("chatgpt-web/credential-v1");
constexpr qint64 maxPdf = 64 * 1024 * 1024;
bool validId(const QString &id) {
    static const QRegularExpression expression("^[A-Za-z0-9_-]{1,128}$");
    return expression.match(id).hasMatch();
}
QString safeMessage(const QString &kind) {
    static const QMap<QString, QString> messages{
        {"AUTH_REQUIRED", "Importez votre session ChatGPT pour vous connecter."},
        {"AUTH_INVALID", "La session a été refusée. Réimportez une session ChatGPT valide."},
        {"AUTH_UNCONFIRMED", "Le service n’a pas confirmé la connexion."},
        {"SESSION_REIMPORT_REQUIRED", "La session a changé. Réimportez-la depuis votre navigateur."},
        {"SESSION_PERSISTENCE_REQUIRED", "Ce service ne permet pas de conserver cette session."},
        {"WEB_AUTH_REQUIRED", "Une vérification dans le navigateur ChatGPT est nécessaire."},
        {"UPSTREAM_FORBIDDEN", "ChatGPT a refusé la requête (HTTP 403)."},
        {"UNEXPECTED_HTML", "ChatGPT a renvoyé une page Web inattendue."},
        {"RATE_LIMITED", "ChatGPT limite les requêtes. Réessayez plus tard."},
        {"NETWORK_ERROR", "ChatGPT est momentanément injoignable."},
        {"CORE_UNAVAILABLE", "Le service local rmchat-core est indisponible."},
        {"IPC_ERROR", "La connexion au service local a été interrompue."},
        {"IPC_PROTOCOL_ERROR", "La réponse du service local est invalide."},
        {"INVALID_CREDENTIAL", "Choisissez un fichier de session JSON valide de 32 Kio maximum."},
        {"VAULT_ERROR", "Le coffre est inaccessible. La connexion a été arrêtée."},
        {"BUSY", "Une autre instance utilise déjà le coffre rmChat."},
        {"FILE_REJECTED", "Choisissez un PDF valide de 64 Mio maximum (4 fichiers au plus)."},
        {"FILE_CHANGED", "Le PDF sélectionné a changé. Sélectionnez-le à nouveau."},
        {"UNSUPPORTED", "Cette opération n’est pas disponible avec ce service."},
        {"UNSUPPORTED_CREDENTIAL_KIND", "Ce type de session n’est pas pris en charge."},
        {"RESPONSE_TOO_LARGE", "Cette conversation est trop volumineuse."},
        {"NOT_FOUND", "Cette conversation est introuvable."},
        {"INVALID_PARAMS", "Vérifiez le modèle, le message et la conversation sélectionnés."},
        {"MODEL_UNAVAILABLE", "Ce modèle n’est pas disponible pour votre compte. Actualisez la liste des modèles."},
        {"CANCELLED", "Opération annulée. Vérifiez l’historique avant de renouveler un envoi."},
        {"OUTCOME_UNKNOWN", "L’envoi n’est pas confirmé. Vérifiez l’historique avant de renvoyer."},
        {"DUPLICATE_REQUEST", "Cet envoi a déjà été reçu. Vérifiez l’historique."},
        {"UPSTREAM_ERROR", "ChatGPT a renvoyé une réponse inutilisable."}};
    return messages.value(kind, messages.value("UPSTREAM_ERROR"));
}
QString safeKind(const QString &kind) {
    return safeMessage(kind) != safeMessage("UPSTREAM_ERROR") ? kind : QStringLiteral("UPSTREAM_ERROR");
}
QVariantMap displayMessage(const QJsonObject &source) {
    return {{"id", source.value("id").toString()}, {"role", source.value("role").toString()},
            {"text", source.value("text").toString()}, {"status", QString()}};
}
bool validMessage(const QJsonObject &source) {
    return validId(source.value("id").toString()) && source.value("text").isString() &&
        QStringList{"user", "assistant", "system", "tool"}.contains(source.value("role").toString());
}
QString localPath(const QUrl &url) {
    return url.isLocalFile() ? url.toLocalFile() : QString();
}
}

class ChatWorker final : public QObject {
    Q_OBJECT
public:
    ChatWorker(QString corePath, QString vaultDirectory, std::shared_ptr<std::atomic_bool> cancel)
        : m_corePath(std::move(corePath)), m_vaultDirectory(std::move(vaultDirectory)), m_cancel(std::move(cancel)) {}
    void initialize(bool preview) {
        m_preview = preview;
        clearRemote();
        m_state.insert("credentialStored", false);
        if (preview) {
            status("Aperçu local — aucune connexion ouverte.");
        } else if (!QDir().mkpath(m_vaultDirectory) ||
                   !QFile::setPermissions(m_vaultDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            error("VAULT_ERROR");
        } else {
            m_lock = std::make_unique<QLockFile>(m_vaultDirectory + "/probe.lock");
            if (!m_lock->tryLock(0)) {
                error("BUSY");
                m_lock.reset();
            } else {
                m_vault = std::make_unique<repaper::SecretStore>(m_vaultDirectory);
                auto saved = m_vault->get(entry);
                m_state.insert("credentialStored", !saved.isEmpty());
                saved.fill('\0');
                if (!m_vault->error().isEmpty()) error("VAULT_ERROR");
                else status(m_state.value("credentialStored").toBool()
                    ? "Session enregistrée. Connectez-vous pour la vérifier."
                    : "Importez votre session ChatGPT pour commencer.");
            }
        }
        publish();
        emit finished(false, {});
    }
    void execute(const QString &operation, const QVariantMap &arguments) {
        if (m_preview) { emit finished(false, {}); return; }
        if (!m_vault) { error("BUSY"); publish(); emit finished(false, {}); return; }
        bool sent = false;
        if (operation == "logout") logout();
        else if (m_cancel->load()) error("CANCELLED");
        else if (operation == "connect" || operation == "import") authenticate(operation == "import" ? arguments.value("path").toString() : QString());
        else if (operation == "addPdf") addPdf(arguments.value("path").toString());
        else if (operation == "removePdf") removePdf(arguments.value("id").toString());
        else if (operation == "new") {
            m_state.insert("conversationId", QString()); m_state.insert("conversationTitle", QString());
            m_state.insert("messages", QVariantList{}); m_state.insert("streamingText", QString());
            m_messageCursor.clear(); m_parent.clear(); m_state.insert("hasMoreMessages", false);
            status("Nouvelle conversation.");
        } else if (!m_state.value("connected").toBool()) error("AUTH_REQUIRED");
        else if (operation == "models") loadModels();
        else if (operation == "list") list(arguments.value("more").toBool());
        else if (operation == "get") get(arguments.value("id").toString(), arguments.value("more").toBool());
        else if (operation == "send") sent = send(arguments.value("text").toString(), arguments.value("model").toString());
        publish();
        emit finished(sent, sent ? arguments.value("text").toString() : QString(), operation == "logout");
    }
    void shutdown() {
        if (m_core) m_core->stop();
        m_core.reset();
        m_files.clear();
        m_uploadRoot.reset();
        m_vault.reset();
        m_lock.reset();
    }
signals:
    void snapshot(const QVariantMap &state);
    void finished(bool sent, const QString &sentDraft, bool loggedOut = false);
private:
    struct Pdf { QString id, path, name, sha; qint64 size; };
    void publish() {
        QVariantList files;
        for (const auto &file : m_files) files.append(QVariantMap{{"id", file.id}, {"name", file.name}, {"size", file.size}});
        m_state.insert("attachments", files);
        emit snapshot(m_state);
    }
    void status(const QString &message) {
        m_state.insert("statusMessage", message); m_state.insert("errorKind", QString());
        m_state.insert("errorDetails", QString());
    }
    void error(const QString &kind) {
        const auto safe = safeKind(kind);
        m_state.insert("statusMessage", safeMessage(safe)); m_state.insert("errorKind", safe);
        m_state.insert("errorDetails", QString());
    }
    void requestError(const QString &method, const QJsonObject &data) {
        const auto kind = safeKind(data.value("kind").toString());
        error(kind);
        const auto stage = data.value("stage").toString();
        const QMap<QString, QString> stages{{"validation", "Vérification du modèle"},
            {"prepare", "Préparation de l’envoi"}, {"submit", "Transmission du message"},
            {"stream", "Réception de la réponse"}};
        QStringList details;
        if (method == "chat.send" && stages.contains(stage)) details << stages.value(stage);
        const auto code = data.value("httpStatus");
        if (code.isDouble() && code.toDouble() == code.toInt() && code.toInt() >= 400 && code.toInt() <= 599)
            details << QString("HTTP %1").arg(code.toInt());
        if (method == "chat.send") details << "Votre message reste dans le brouillon.";
        m_state.insert("errorDetails", details.join(" · "));
    }
    void clearRemote() {
        for (const auto &name : {"accountLabel", "conversationId", "conversationTitle", "streamingText", "modelStatus", "defaultModelId"}) m_state.insert(name, QString());
        for (const auto &name : {"models", "conversations", "messages"}) m_state.insert(name, QVariantList{});
        for (const auto &name : {"connected", "hasMoreConversations", "hasMoreMessages"}) m_state.insert(name, false);
        m_methods = {}; m_listCursor.clear(); m_messageCursor.clear(); m_parent.clear();
    }
    bool uploadRoot() {
        if (m_uploadRoot) return true;
        m_uploadRoot = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/rmchat-files-XXXXXX");
        if (!m_uploadRoot->isValid() || !QFile::setPermissions(m_uploadRoot->path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            m_uploadRoot.reset(); error("FILE_REJECTED"); return false;
        }
        return true;
    }
    bool call(const QString &method, const QJsonObject &parameters, QJsonObject *result,
              int timeout = 120000, CoreProcess::Progress progress = {}) {
        if (method != "auth.import" && !m_methods.contains(method)) { error("UNSUPPORTED"); return false; }
        if (m_cancel->load()) { error("CANCELLED"); return false; }
        auto response = m_core->call(method, parameters, timeout, std::move(progress));
        if (response.isEmpty()) {
            m_state.insert("connected", false);
            error(m_cancel->load() ? "CANCELLED" : method == "chat.send" ? "OUTCOME_UNKNOWN" : "IPC_ERROR");
            return false;
        }
        if (response.contains("error")) {
            const auto data = response.value("error").toObject().value("data").toObject();
            const auto kind = safeKind(data.value("kind").toString());
            requestError(method, data);
            if (QStringList{"AUTH_REQUIRED", "AUTH_INVALID", "SESSION_REIMPORT_REQUIRED"}.contains(kind)) {
                m_state.insert("connected", false); m_core->stop();
            }
            return false;
        }
        if (!response.value("result").isObject()) { error("IPC_PROTOCOL_ERROR"); return false; }
        *result = response.value("result").toObject();
        // credentialUpdate is valid only at the authentication boundary.
        if (method != "auth.import" && result->contains("credentialUpdate")) {
            result->remove("credentialUpdate"); m_state.insert("connected", false);
            m_core->stop(); error("IPC_PROTOCOL_ERROR"); return false;
        }
        return true;
    }
    void authenticate(const QString &path) {
        QByteArray input;
        if (path.isEmpty()) {
            input = m_vault->get(entry);
            if (!m_vault->error().isEmpty()) { input.fill('\0'); error("VAULT_ERROR"); return; }
        }
        else {
            QFile file(path);
            if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) || file.size() > credentialLimit) { error("INVALID_CREDENTIAL"); return; }
            input = file.read(credentialLimit + 1);
        }
        QString ignored;
        auto candidate = normalizeCredential(input, &ignored);
        input.fill('\0');
        if (candidate.isEmpty()) { error(path.isEmpty() ? "AUTH_REQUIRED" : "INVALID_CREDENTIAL"); return; }
        const auto credentialKind = QJsonDocument::fromJson(candidate).object().value("kind").toString();
        if (m_core) m_core->stop();
        clearRemote();
        if (!uploadRoot()) { candidate.fill('\0'); return; }
        m_core = std::make_unique<CoreProcess>();
        m_core->setCancellationCheck([cancel = m_cancel] { return cancel->load(); });
        if (!m_core->start(m_corePath, {m_uploadRoot->path()})) { candidate.fill('\0'); error("CORE_UNAVAILABLE"); return; }
        QJsonObject auth;
        const bool accepted = call("auth.import", {{"credential", QJsonDocument::fromJson(candidate).object()}, {"persistSession", true}}, &auth);
        if (!accepted || !auth.value("authenticated").isBool() || !auth.value("authenticated").toBool()) {
            candidate.fill('\0'); auth = {}; m_core->stop();
            if (accepted) error("AUTH_UNCONFIRMED");
            return;
        }
        if (auth.contains("credentialUpdate")) {
            const auto update = auth.take("credentialUpdate").toObject();
            if (credentialKind != "session_token" || update.size() != 4 || update.value("version").toDouble(-1) != 1 ||
                update.value("provider").toString() != "chatgpt-web" || update.value("kind").toString() != "session_token") {
                candidate.fill('\0'); auth = {}; m_core->stop(); error("INVALID_CREDENTIAL"); return;
            }
            auto updateBytes = QJsonDocument(update).toJson(QJsonDocument::Compact);
            auto updated = normalizeCredential(updateBytes, &ignored);
            updateBytes.fill('\0');
            if (updated.isEmpty()) { candidate.fill('\0'); auth = {}; m_core->stop(); error("INVALID_CREDENTIAL"); return; }
            candidate.fill('\0'); candidate = updated; updated.fill('\0');
        } else if (credentialKind == "session_token") {
            candidate.fill('\0'); auth = {}; m_core->stop(); error("SESSION_PERSISTENCE_REQUIRED"); return;
        }
        // Commit before reading account data or issuing any further network request.
        const bool saved = m_vault->put(entry, candidate);
        candidate.fill('\0');
        if (!saved) { auth = {}; m_core->stop(); error("VAULT_ERROR"); return; }
        // A successful import may switch accounts, including when identity is unavailable.
        // Keep the draft text, but require an explicit re-selection of its local files.
        for (const auto &pdf : m_files) QFile::remove(pdf.path);
        m_files.clear();
        m_state.insert("credentialStored", true);
        m_state.insert("connected", true);
        m_methods = auth.value("capabilities").toObject().value("methods").toArray();
        const auto account = auth.value("account").toObject();
        m_state.insert("accountLabel", account.value("displayName").toString());
        auth = {};
        status("Connecté à ChatGPT.");
        publish();
        if (!loadModels()) return;
        list(false);
    }
    bool loadModels() {
        QJsonObject models;
        if (!call("models.list", {}, &models)) return false;
        if (!models.value("items").isArray() || models.value("items").toArray().size() > 1000) { error("IPC_PROTOCOL_ERROR"); return false; }
        QVariantList items;
        QSet<QString> seen;
        QHash<QString, int> nameCounts;
        for (const auto &value : models.value("items").toArray()) {
            const auto item = value.toObject();
            const auto id = item.value("id").toString();
            const auto name = item.value("name").toString().trimmed();
            if (id.isEmpty() || id.size() > 128 || !item.value("name").isString() || name.isEmpty() || name.size() > 256) { error("IPC_PROTOCOL_ERROR"); return false; }
            if (seen.contains(id)) continue;
            seen.insert(id);
            items.append(QVariantMap{{"id", id}, {"name", name}});
            ++nameCounts[name];
        }
        for (auto &value : items) {
            auto item = value.toMap();
            if (nameCounts.value(item.value("name").toString()) > 1)
                item.insert("name", item.value("name").toString() + " · " + item.value("id").toString());
            value = item;
        }
        m_state.insert("models", items);
        m_state.insert("defaultModelId", models.value("defaultModelId").toString());
        m_state.insert("modelStatus", items.isEmpty() ? "Aucun modèle accessible n’a été renvoyé pour ce compte." : QString());
        status(items.isEmpty() ? "Aucun modèle accessible pour envoyer un message." : "Modèles disponibles à jour.");
        return true;
    }
    void list(bool more) {
        if (more && m_listCursor.isEmpty()) return;
        QJsonObject result;
        if (!call("conversations.list", {{"cursor", more ? QJsonValue(m_listCursor) : QJsonValue::Null}, {"limit", 20}}, &result)) return;
        if (!result.value("items").isArray() || result.value("items").toArray().size() > 20 ||
            (!result.value("nextCursor").isNull() && !result.value("nextCursor").isString())) { error("IPC_PROTOCOL_ERROR"); return; }
        auto items = more ? m_state.value("conversations").toList() : QVariantList{};
        for (const auto &value : result.value("items").toArray()) {
            const auto item = value.toObject();
            if (!validId(item.value("id").toString()) || !item.value("title").isString()) { error("IPC_PROTOCOL_ERROR"); return; }
            const QVariantMap display{{"id", item.value("id").toString()}, {"title", item.value("title").toString()}, {"updatedAt", item.value("updatedAt").toString()}};
            bool found = false;
            for (auto &existing : items) if (existing.toMap().value("id") == display.value("id")) { existing = display; found = true; break; }
            if (!found) items.append(display);
        }
        m_listCursor = result.value("nextCursor").toString();
        m_state.insert("conversations", items); m_state.insert("hasMoreConversations", !m_listCursor.isEmpty());
        status("Conversations à jour.");
    }
    bool get(const QString &id, bool more) {
        if (!validId(id) || (more && (id != m_state.value("conversationId").toString() || m_messageCursor.isEmpty()))) { error("INVALID_PARAMS"); return false; }
        QJsonObject result;
        if (!call("conversations.get", {{"conversationId", id}, {"cursor", more ? QJsonValue(m_messageCursor) : QJsonValue::Null}, {"limit", 50}}, &result)) return false;
        const auto parent = result.value("continuationParentId").toString();
        if (result.value("id").toString() != id || !result.value("title").isString() || !result.value("messages").isArray() ||
            result.value("messages").toArray().size() > 50 || (!parent.isEmpty() && !validId(parent)) ||
            (!result.value("nextCursor").isNull() && !result.value("nextCursor").isString())) { error("IPC_PROTOCOL_ERROR"); return false; }
        auto items = more ? m_state.value("messages").toList() : QVariantList{};
        for (const auto &value : result.value("messages").toArray()) {
            if (!validMessage(value.toObject())) { error("IPC_PROTOCOL_ERROR"); return false; }
            items.append(displayMessage(value.toObject()));
        }
        m_state.insert("messages", items); m_state.insert("conversationId", id); m_state.insert("conversationTitle", result.value("title").toString());
        m_state.insert("streamingText", QString()); m_messageCursor = result.value("nextCursor").toString(); m_parent = parent;
        m_state.insert("hasMoreMessages", !m_messageCursor.isEmpty()); status("Conversation ouverte.");
        return true;
    }
    void addPdf(const QString &path) {
        QFile source(path);
        const QFileInfo info(path);
        if (m_files.size() >= 4 || !info.isFile() || !source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > maxPdf || source.peek(5) != "%PDF-" || !uploadRoot()) { error("FILE_REJECTED"); return; }
        Pdf pdf;
        pdf.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pdf.path = m_uploadRoot->filePath(pdf.id + ".pdf"); pdf.name = info.fileName(); pdf.size = 0;
        QFile snapshot(pdf.path);
        if (!snapshot.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !snapshot.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) { snapshot.close(); QFile::remove(pdf.path); error("FILE_REJECTED"); return; }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        bool ok = true;
        while (!source.atEnd()) {
            const auto chunk = source.read(64 * 1024);
            if (chunk.isEmpty() || pdf.size + chunk.size() > maxPdf || m_cancel->load() || snapshot.write(chunk) != chunk.size()) { ok = false; break; }
            hash.addData(chunk); pdf.size += chunk.size();
        }
        ok = ok && snapshot.flush() && pdf.size == source.size(); snapshot.close();
        if (!ok) { QFile::remove(pdf.path); error(m_cancel->load() ? "CANCELLED" : "FILE_REJECTED"); return; }
        pdf.sha = QString::fromLatin1(hash.result().toHex()); m_files.append(pdf); status("PDF joint au brouillon.");
    }
    void removePdf(const QString &id) {
        for (qsizetype i = 0; i < m_files.size(); ++i) if (m_files.at(i).id == id) { QFile::remove(m_files.at(i).path); m_files.removeAt(i); break; }
        status("Pièces jointes mises à jour.");
    }
    bool send(const QString &text, const QString &model) {
        bool found = false;
        for (const auto &value : m_state.value("models").toList()) if (value.toMap().value("id").toString() == model) found = true;
        const auto conversation = m_state.value("conversationId").toString();
        if (!found || text.trimmed().isEmpty() || text.toUtf8().size() > 128 * 1024 || (!conversation.isEmpty() && m_parent.isEmpty())) { error("INVALID_PARAMS"); return false; }
        // Cursors are tied to the active branch. Finish reading that branch before
        // advancing it, so older messages cannot be appended after the new reply.
        QSet<QString> visited;
        while (!m_messageCursor.isEmpty()) {
            if (visited.contains(m_messageCursor)) { error("IPC_PROTOCOL_ERROR"); return false; }
            if (visited.size() >= 200) { error("RESPONSE_TOO_LARGE"); return false; }
            visited.insert(m_messageCursor);
            status("Chargement de la suite des messages avant l’envoi…"); publish();
            if (!get(conversation, true)) return false;
        }
        QJsonArray attachments;
        for (const auto &pdf : m_files) {
            QJsonObject uploaded;
            if (!call("files.upload", {{"path", pdf.path}, {"filename", pdf.name}, {"mimeType", "application/pdf"}, {"sha256", pdf.sha}}, &uploaded, 180000)) return false;
            const auto ref = uploaded.value("attachmentRef");
            if (!ref.isString() || ref.toString().isEmpty()) { error("IPC_PROTOCOL_ERROR"); return false; }
            attachments.append(ref);
        }
        const auto userId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_state.insert("streamingText", QString()); status("Réponse en cours…"); publish();
        QJsonObject result;
        int lastSequence = 0;
        const auto progress = [this, &lastSequence](const QJsonObject &event) {
            const int sequence = event.value("sequence").toInt();
            if (sequence <= lastSequence || !event.value("text").isString()) return;
            lastSequence = sequence; m_state.insert("streamingText", event.value("text").toString()); publish();
        };
        if (!call("chat.send", {{"conversationId", conversation.isEmpty() ? QJsonValue::Null : QJsonValue(conversation)},
            {"parentMessageId", m_parent.isEmpty() ? QJsonValue::Null : QJsonValue(m_parent)}, {"clientMessageId", userId},
            {"modelId", model}, {"text", text}, {"attachments", attachments}}, &result, 300000, progress)) return false;
        const auto returnedId = result.value("conversationId").toString(), parent = result.value("continuationParentId").toString();
        const auto response = result.value("message").toObject();
        if (!validId(returnedId) || (!conversation.isEmpty() && returnedId != conversation) || !validId(parent) || !validMessage(response)) { error("OUTCOME_UNKNOWN"); return false; }
        auto messages = m_state.value("messages").toList();
        messages.append(QVariantMap{{"id", userId}, {"role", "user"}, {"text", text}, {"status", QString()}});
        messages.append(displayMessage(response)); m_state.insert("messages", messages); m_state.insert("conversationId", returnedId);
        m_parent = parent; m_messageCursor.clear(); m_state.insert("hasMoreMessages", false); m_state.insert("streamingText", QString());
        for (const auto &pdf : m_files) QFile::remove(pdf.path);
        m_files.clear(); status("Réponse reçue.");
        return true;
    }
    void logout() {
        if (m_core) m_core->stop(); m_core.reset();
        const bool removed = m_vault->remove(entry);
        clearRemote(); m_files.clear(); m_uploadRoot.reset();
        if (removed) { m_state.insert("credentialStored", false); status("Session supprimée de cet appareil."); }
        else error("VAULT_ERROR");
    }
    QString m_corePath, m_vaultDirectory;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::unique_ptr<CoreProcess> m_core;
    std::unique_ptr<repaper::SecretStore> m_vault;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QTemporaryDir> m_uploadRoot;
    QList<Pdf> m_files;
    QVariantMap m_state;
    QJsonArray m_methods;
    QString m_listCursor, m_messageCursor, m_parent;
    bool m_preview = false;
};

ChatController::ChatController(QString corePath, QString vaultDirectory, bool preview, QObject *parent)
    : QObject(parent), m_cancel(std::make_shared<std::atomic_bool>(false)), m_preview(preview) {
    if (corePath.isEmpty()) corePath = qEnvironmentVariable("RMCHAT_CORE_PATH");
    if (corePath.isEmpty()) {
        corePath = QCoreApplication::applicationDirPath() + "/rmchat-core";
#ifdef Q_OS_WIN
        corePath += ".exe";
#endif
    }
    if (vaultDirectory.isEmpty()) vaultDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/vault";
    m_worker = new ChatWorker(corePath, vaultDirectory, m_cancel);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &ChatWorker::snapshot, this, [this](const QVariantMap &state) {
        m_state = state;
        bool selectedPresent = false;
        for (const auto &value : models()) if (value.toMap().value("id").toString() == m_selectedModel) selectedPresent = true;
        if (!selectedPresent) {
            m_selectedModel.clear();
            const auto preferred = state.value("defaultModelId").toString();
            for (const auto &value : models()) if (value.toMap().value("id").toString() == preferred) m_selectedModel = preferred;
            if (m_selectedModel.isEmpty() && !models().isEmpty()) m_selectedModel = models().first().toMap().value("id").toString();
        }
        emit changed();
    });
    connect(m_worker, &ChatWorker::finished, this, [this](bool sent, const QString &sentDraft, bool loggedOut) {
        if (sent && m_draft == sentDraft) { m_draft.clear(); emit draftChanged(); }
        if (m_logoutQueued && !loggedOut) return;
        m_logoutQueued = false;
        m_busy = false; emit changed();
    });
    m_thread.start();
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, preview] { worker->initialize(preview); }, Qt::QueuedConnection);
}
ChatController::~ChatController() {
    m_cancel->store(true);
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
    m_thread.quit(); m_thread.wait();
}
bool ChatController::canSend() const {
    return !m_preview && connected() && !m_busy && !m_selectedModel.isEmpty() && !m_draft.trimmed().isEmpty() && m_draft.toUtf8().size() <= 128 * 1024;
}
void ChatController::setDraft(const QString &value) {
    if (value == m_draft) return;
    m_draft = value; emit draftChanged(); emit changed();
}
void ChatController::setSelectedModelId(const QString &id) {
    if (m_busy || id == m_selectedModel) return;
    for (const auto &value : models()) if (value.toMap().value("id").toString() == id) { m_selectedModel = id; emit changed(); return; }
}
bool ChatController::begin() {
    if (m_busy || m_preview) return false;
    m_cancel->store(false); m_busy = true; emit changed(); return true;
}
void ChatController::submit(const QString &operation, const QVariantMap &arguments) {
    if (!begin()) return;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, operation, arguments] { worker->execute(operation, arguments); }, Qt::QueuedConnection);
}
void ChatController::connectSaved() { submit("connect"); }
void ChatController::importSessionFile(const QUrl &url) {
    if (localPath(url).isEmpty()) { if (!m_busy && !m_preview) { m_state.insert("errorKind", "INVALID_CREDENTIAL"); m_state.insert("statusMessage", safeMessage("INVALID_CREDENTIAL")); emit changed(); } return; }
    submit("import", {{"path", localPath(url)}});
}
void ChatController::refreshConversations() { submit("list", {{"more", false}}); }
void ChatController::refreshModels() { submit("models"); }
void ChatController::loadMoreConversations() { if (hasMoreConversations()) submit("list", {{"more", true}}); }
void ChatController::openConversation(const QString &id) { submit("get", {{"id", id}, {"more", false}}); }
void ChatController::loadMoreMessages() { if (hasMoreMessages()) submit("get", {{"id", conversationId()}, {"more", true}}); }
void ChatController::newConversation() { submit("new"); }
void ChatController::send() { if (canSend()) submit("send", {{"text", m_draft}, {"model", m_selectedModel}}); }
void ChatController::addPdf(const QUrl &url) { submit("addPdf", {{"path", localPath(url)}}); }
void ChatController::removeAttachment(const QString &id) { submit("removePdf", {{"id", id}}); }
void ChatController::cancel() { if (m_busy) m_cancel->store(true); }
void ChatController::logout() {
    if (m_preview) return;
    if (m_busy) {
        if (m_logoutQueued) return;
        m_logoutQueued = true;
        m_cancel->store(true);
        // The worker processes logout after the interrupted operation, retaining the lock.
        QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->execute("logout", {}); }, Qt::QueuedConnection);
    } else submit("logout");
}
}
#include "ChatController.moc"

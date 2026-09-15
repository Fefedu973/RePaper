#include "CoreProcess.h"
#include "Credential.h"
#include "SecretStore.h"
#include <QCommandLineParser>
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
#include <QTextStream>
#include <QUuid>
#include <cmath>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
const QString vaultEntry = QStringLiteral("chatgpt-web/credential-v1");
void output(const QJsonObject &object) {
    QTextStream stream(stdout);
    stream << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
    stream.flush();
}
int failure(const QString &message, const QString &kind = QStringLiteral("LOCAL_ERROR")) {
    output(QJsonObject{{"error", QJsonObject{{"message", message},
                                            {"data", QJsonObject{{"kind", kind}, {"retryable", false}}}}}});
    return 1;
}
QByteArray readInput(qsizetype limit) {
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return {};
    QByteArray bytes;
    while (bytes.size() <= limit) {
        auto chunk = input.read(qMin<qsizetype>(8192, limit + 1 - bytes.size()));
        if (chunk.isEmpty())
            break;
        bytes += chunk;
    }
    return bytes;
}
bool hasMethod(const QJsonObject &auth, const QString &method) {
    return auth.value("capabilities").toObject().value("methods").toArray().contains(method);
}

// check is a diagnostic boundary: neither upstream messages nor arbitrary JSON
// fields may reach stdout, even when the core returns a malformed response.
QJsonObject checkError(const QJsonObject &error) {
    const auto data = error.value("data").toObject();
    const auto kind = data.value("kind").toString();
    const QStringList kinds{"INVALID_PARAMS", "AUTH_REQUIRED", "AUTH_INVALID", "UNSUPPORTED_CREDENTIAL_KIND",
        "SESSION_REIMPORT_REQUIRED", "SESSION_PERSISTENCE_REQUIRED", "WEB_AUTH_REQUIRED", "UPSTREAM_FORBIDDEN", "UNEXPECTED_HTML",
        "RATE_LIMITED", "NETWORK_ERROR", "UPSTREAM_ERROR", "CANCELLED", "BUSY", "UNSUPPORTED",
        "RESPONSE_TOO_LARGE", "NOT_FOUND", "OUTCOME_UNKNOWN", "DUPLICATE_REQUEST"};
    QJsonObject safe{{"kind", kinds.contains(kind) ? kind : QStringLiteral("UPSTREAM_ERROR")},
                     {"retryable", data.value("retryable").isBool() && data.value("retryable").toBool()}};
    const auto status = data.value("httpStatus");
    const int code = status.toInt(-1);
    if (status.isDouble() && status.toDouble() == code && code >= 100 && code <= 599)
        safe.insert("httpStatus", code);
    const auto contentType = data.value("contentType").toString();
    if (QStringList{"application/json", "text/html", "application/xhtml+xml", "text/event-stream",
                    "unknown", "other"}.contains(contentType))
        safe.insert("contentType", contentType);
    if (data.value("challenge").isBool()) safe.insert("challenge", data.value("challenge"));
    return safe;
}
int inspectionFailure(const QString &step, const QJsonObject &error, bool persisted = false) {
    output({{"inspection", QJsonObject{{"ok", false}, {"credentialPersisted", persisted},
        {"step", step}, {"error", error}}}});
    return 1;
}
int inspectionFailure(const QString &step, const QString &kind, bool persisted = false) {
    return inspectionFailure(step, QJsonObject{{"kind", kind}, {"retryable", false}}, persisted);
}
// Repeat the core's schema projection at the stdout boundary. Unexpected fields,
// auth metadata, remote diagnostics and progress never enter this report.
QJsonObject inspectionSchema(const QJsonObject &source, int depth = 0, bool top = false) {
    QJsonObject safe;
    const auto keyName = top ? QStringLiteral("topLevelKeys") : QStringLiteral("keys");
    const QRegularExpression keyPattern("^[A-Za-z0-9_-]{1,64}$");
    const QRegularExpression idPattern("^[A-Za-z0-9_.:/-]{1,128}$");
    QJsonArray keys;
    const auto sourceKeys = source.value(keyName).toArray();
    if (sourceKeys.size() <= 128) {
        for (const auto &key : sourceKeys)
            if (key.isString() && keyPattern.match(key.toString()).hasMatch()) keys.append(key);
    }
    safe.insert(keyName, keys);
    for (const auto &key : keys) {
        const auto value = source.value(key.toString());
        if (value.isBool()) safe.insert(key.toString(), value);
    }
    for (const auto &key : QStringList{"slug", "category", "default_model", "default_model_slug",
             "code_interpreter_model", "browsing_model", "plugins_model", "dalle_model", "subscription_level"}) {
        const auto value = source.value(key);
        if (value.isString() && idPattern.match(value.toString()).hasMatch()) safe.insert(key, value);
    }
    for (const auto &key : QStringList{"title", "human_category_name", "human_category_short_name"}) {
        const auto value = source.value(key);
        const auto text = value.toString();
        if (value.isString() && text.toUtf8().size() <= 256 && !text.contains(QChar(0)) &&
            !text.contains('\r') && !text.contains('\n')) safe.insert(key, value);
    }
    for (const auto &key : QStringList{"model_ids", "model_slugs", "available_models"}) {
        const auto value = source.value(key);
        if (!value.isArray() || value.toArray().size() > 256) continue;
        QJsonArray ids;
        for (const auto &id : value.toArray())
            if (id.isString() && idPattern.match(id.toString()).hasMatch()) ids.append(id);
        safe.insert(key, ids);
    }
    for (const auto &key : QStringList{"can_use", "is_available", "is_enabled", "enabled", "is_user_selectable",
             "is_visible", "hidden", "disabled", "available", "is_default", "is_selected", "remaining",
             "remaining_messages", "limit", "max_requests", "reset_after", "requires_upgrade", "is_entitled", "has_access"}) {
        const auto value = source.value(key);
        if (value.isBool() || (value.isDouble() && std::isfinite(value.toDouble()) &&
            value.toDouble() >= 0 && value.toDouble() <= 1e9)) safe.insert(key, value);
    }
    for (const auto &key : QStringList{"availability", "status"}) {
        const auto value = source.value(key);
        if (value.isString() && QStringList{"available", "unavailable", "enabled", "disabled", "hidden",
            "limited", "blocked", "allowed", "denied", "restricted"}.contains(value.toString())) safe.insert(key, value);
    }
    if (depth < 3) {
        for (const auto &key : QStringList{"capabilities", "product_features", "entitlements", "availability", "access"})
            if (source.value(key).isObject()) safe.insert(key, inspectionSchema(source.value(key).toObject(), depth + 1));
    }
    return safe;
}
int inspectModels(const QString &corePath, const QString &vaultDirectory, bool fromStdin) {
    // Qt can otherwise print paths from failed lock-file cleanup. This command
    // reports local failures using the fixed diagnostic object below instead.
    struct QuietDiagnostics {
        QtMessageHandler previous = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});
        ~QuietDiagnostics() { qInstallMessageHandler(previous); }
    } quietDiagnostics;
    QByteArray candidate;
    struct ClearCandidate {
        QByteArray &bytes;
        ~ClearCandidate() { bytes.fill('\0'); }
    } clearCandidate{candidate};
    if (fromStdin) {
#ifdef Q_OS_UNIX
        if (isatty(STDIN_FILENO)) return inspectionFailure("input", "STDIN_REQUIRED");
#endif
        auto input = readInput(rmchat::credentialLimit);
        candidate = rmchat::normalizeCredential(input, nullptr);
        input.fill('\0');
        if (candidate.isEmpty()) return inspectionFailure("input", "INVALID_CREDENTIAL");
    }
    if (!QFileInfo(vaultDirectory).isDir()) {
        if (QFileInfo::exists(vaultDirectory)) return inspectionFailure("vault", "VAULT_ERROR");
        if (!fromStdin) return inspectionFailure("vault", "AUTH_REQUIRED");
        // Only the private directory is prepared here. The entry and device key
        // are written after auth.import confirms this candidate.
        repaper::SecretStore prepare(vaultDirectory);
        if (!QFileInfo(vaultDirectory).isDir()) return inspectionFailure("vault", "VAULT_ERROR");
    }
    QLockFile lock(vaultDirectory + "/probe.lock");
    if (!lock.tryLock(0)) return inspectionFailure("vault", "BUSY");
    repaper::SecretStore vault(vaultDirectory);
    if (!fromStdin) {
        auto stored = vault.get(vaultEntry);
        if (!vault.error().isEmpty()) return inspectionFailure("vault", "VAULT_ERROR");
        if (stored.isEmpty()) return inspectionFailure("vault", "AUTH_REQUIRED");
        candidate = rmchat::normalizeCredential(stored, nullptr);
        stored.fill('\0');
        if (candidate.isEmpty()) return inspectionFailure("vault", "INVALID_CREDENTIAL");
    }
    const auto credential = QJsonDocument::fromJson(candidate).object();
    const bool session = credential.value("kind").toString() == "session_token";
    rmchat::CoreProcess core;
    if (!core.start(corePath)) {
        candidate.fill('\0');
        return inspectionFailure("core", "CORE_UNAVAILABLE");
    }
    QJsonObject params{{"credential", credential}};
    if (session) params.insert("persistSession", true);
    const auto auth = core.call("auth.import", params);
    if (auth.isEmpty()) return inspectionFailure("auth.import", "IPC_ERROR");
    if (auth.contains("error")) return inspectionFailure("auth.import", checkError(auth.value("error").toObject()));
    auto authenticated = auth.value("result").toObject();
    if (!authenticated.value("authenticated").isBool() || !authenticated.value("authenticated").toBool())
        return inspectionFailure("auth.import", "AUTH_UNCONFIRMED");
    const bool hasUpdate = authenticated.contains("credentialUpdate");
    const auto update = authenticated.take("credentialUpdate");
    bool persisted = false;
    if (session) {
        auto renewed = update.isObject()
            ? rmchat::normalizeCredential(QJsonDocument(update.toObject()).toJson(QJsonDocument::Compact), nullptr)
            : QByteArray{};
        if (renewed.isEmpty() || QJsonDocument::fromJson(renewed).object().value("kind").toString() != "session_token") {
            renewed.fill('\0');
            return inspectionFailure("auth.import", "INVALID_CREDENTIAL_UPDATE");
        }
        candidate.fill('\0'); candidate = renewed;
        renewed.fill('\0');
    } else if (hasUpdate) return inspectionFailure("auth.import", "INVALID_CREDENTIAL_UPDATE");
    if (session || fromStdin) {
        // Commit the freshly confirmed access credential or canonical rotated
        // session before inspecting models, using this same authenticated core.
        persisted = vault.put(vaultEntry, candidate);
        if (!persisted) return inspectionFailure("vault", session ? "SESSION_PERSISTENCE_REQUIRED" : "VAULT_ERROR");
    }
    candidate.fill('\0');
    if (!hasMethod(authenticated, "models.inspect"))
        return inspectionFailure("models.inspect", "UNSUPPORTED_OPERATION", persisted);
    const auto response = core.call("models.inspect", {});
    if (response.isEmpty()) return inspectionFailure("models.inspect", "IPC_ERROR", persisted);
    if (response.contains("error"))
        return inspectionFailure("models.inspect", checkError(response.value("error").toObject()), persisted);
    const auto source = response.value("result").toObject();
    if (!response.value("result").isObject() || !source.value("topLevelKeys").isArray() ||
        !source.value("models").isArray() || !source.value("categories").isArray())
        return inspectionFailure("models.inspect", "IPC_PROTOCOL_ERROR", persisted);
    if (QJsonDocument(source).toJson(QJsonDocument::Compact).size() > 128 * 1024)
        return inspectionFailure("models.inspect", "RESPONSE_TOO_LARGE", persisted);
    auto schema = inspectionSchema(source, 0, true);
    for (const auto &key : QStringList{"models", "categories"}) {
        const auto items = source.value(key).toArray();
        if (items.size() > 1024) return inspectionFailure("models.inspect", "RESPONSE_TOO_LARGE", persisted);
        QJsonArray projected;
        for (const auto &item : items) {
            if (!item.isObject() || !item.toObject().value("keys").isArray())
                return inspectionFailure("models.inspect", "IPC_PROTOCOL_ERROR", persisted);
            projected.append(inspectionSchema(item.toObject()));
        }
        schema.insert(key, projected);
    }
    output({{"inspection", QJsonObject{{"ok", true}, {"credentialPersisted", persisted}, {"schema", schema}}}});
    return 0;
}
class CheckReport {
public:
    explicit CheckReport(bool fromStdin) : m_fromStdin(fromStdin) {}
    void passed(const QString &step, int count = -1) {
        QJsonObject value{{"step", step}, {"ok", true}};
        if (count >= 0) value.insert("count", count);
        m_steps.append(value);
    }
    void noConversation() { m_steps.append(QJsonObject{{"step", "conversations.get"}, {"skipped", true}}); }
    void conversationRead() { m_conversationRead = true; }
    int failed(const QString &step, const QString &kind) {
        return failed(step, QJsonObject{{"kind", kind}, {"retryable", false}});
    }
    int failed(const QString &step, const QJsonObject &safeError) {
        m_steps.append(QJsonObject{{"step", step}, {"ok", false}, {"error", safeError}});
        return finish(false);
    }
    int finish(bool ok) const {
        output(QJsonObject{{"check", QJsonObject{{"ok", ok}, {"readOnly", true},
            {"credentialSource", m_fromStdin ? "stdin" : "vault"}, {"credentialPersisted", false},
            {"conversationRead", m_conversationRead}, {"steps", m_steps}}}});
        return ok ? 0 : 1;
    }
private:
    bool m_fromStdin;
    bool m_conversationRead = false;
    QJsonArray m_steps;
};
int check(const QString &corePath, const QString &vaultDirectory, bool fromStdin) {
    CheckReport report(fromStdin);
    QByteArray candidate;
    std::unique_ptr<repaper::SecretStore> vault;
    std::unique_ptr<QLockFile> lock;
    QString credentialError;
    if (fromStdin) {
#ifdef Q_OS_UNIX
        if (isatty(STDIN_FILENO)) return report.failed("input", "STDIN_REQUIRED");
#endif
        auto input = readInput(rmchat::credentialLimit);
        candidate = rmchat::normalizeCredential(input, &credentialError);
        input.fill('\0');
        if (candidate.isEmpty()) return report.failed("input", "INVALID_CREDENTIAL");
    } else {
        if (!QFileInfo(vaultDirectory).isDir())
            return report.failed("vault", QFileInfo::exists(vaultDirectory) ? "VAULT_ERROR" : "AUTH_REQUIRED");
        lock = std::make_unique<QLockFile>(vaultDirectory + "/probe.lock");
        if (!lock->tryLock(0)) return report.failed("vault", "BUSY");
        vault = std::make_unique<repaper::SecretStore>(vaultDirectory);
        auto stored = vault->get(vaultEntry);
        if (!vault->error().isEmpty()) return report.failed("vault", "VAULT_ERROR");
        if (stored.isEmpty()) return report.failed("vault", "AUTH_REQUIRED");
        candidate = rmchat::normalizeCredential(stored, &credentialError);
        stored.fill('\0');
        if (candidate.isEmpty()) return report.failed("vault", "INVALID_CREDENTIAL");
    }
    if (QJsonDocument::fromJson(candidate).object().value("kind").toString() == "session_token") {
        candidate.fill('\0');
        return report.failed(fromStdin ? "input" : "vault", "SESSION_PERSISTENCE_REQUIRED");
    }
    rmchat::CoreProcess core;
    if (!core.start(corePath)) {
        candidate.fill('\0');
        return report.failed("core", "CORE_UNAVAILABLE");
    }
    auto auth = core.call("auth.import", {{"credential", QJsonDocument::fromJson(candidate).object()}});
    candidate.fill('\0');
    if (auth.isEmpty()) return report.failed("auth.import", "IPC_ERROR");
    if (auth.contains("error")) return report.failed("auth.import", checkError(auth.value("error").toObject()));
    if (!auth.value("result").isObject() || !auth.value("result").toObject().value("authenticated").isBool() ||
        !auth.value("result").toObject().value("authenticated").toBool())
        return report.failed("auth.import", "AUTH_UNCONFIRMED");
    report.passed("auth.import");
    const auto capabilities = auth.value("result").toObject();

    const auto call = [&core, &report, &capabilities](const QString &method, const QJsonObject &params,
                                                    QJsonObject *result) {
        if (!hasMethod(capabilities, method)) {
            report.failed(method, "UNSUPPORTED_OPERATION");
            return false;
        }
        // Deliberately no progress callback: check never prints remote content.
        const auto response = core.call(method, params);
        if (response.isEmpty()) {
            report.failed(method, "IPC_ERROR");
            return false;
        }
        if (response.contains("error")) {
            report.failed(method, checkError(response.value("error").toObject()));
            return false;
        }
        if (!response.value("result").isObject()) {
            report.failed(method, "IPC_PROTOCOL_ERROR");
            return false;
        }
        *result = response.value("result").toObject();
        return true;
    };
    QJsonObject models;
    if (!call("models.list", {}, &models)) return 1;
    if (!models.value("items").isArray()) return report.failed("models.list", "IPC_PROTOCOL_ERROR");
    report.passed("models.list", models.value("items").toArray().size());

    QJsonObject conversations;
    if (!call("conversations.list", {{"cursor", QJsonValue::Null}, {"limit", 5}}, &conversations)) return 1;
    if (!conversations.value("items").isArray() || conversations.value("items").toArray().size() > 5)
        return report.failed("conversations.list", "IPC_PROTOCOL_ERROR");
    const auto items = conversations.value("items").toArray();
    report.passed("conversations.list", items.size());
    if (items.isEmpty()) {
        report.noConversation();
        return report.finish(true);
    }
    const auto id = items.first().toObject().value("id").toString();
    if (!QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(id).hasMatch())
        return report.failed("conversations.get", "IPC_PROTOCOL_ERROR");
    QJsonObject conversation;
    if (!call("conversations.get", {{"conversationId", id}, {"cursor", QJsonValue::Null}, {"limit", 5}}, &conversation))
        return 1;
    if (conversation.value("id").toString() != id || !conversation.value("messages").isArray() ||
        conversation.value("messages").toArray().size() > 5)
        return report.failed("conversations.get", "IPC_PROTOCOL_ERROR");
    report.passed("conversations.get", conversation.value("messages").toArray().size());
    report.conversationRead();
    return report.finish(true);
}
}
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("RePaper");
    app.setApplicationName("rmchat");
    app.setApplicationVersion("0.1.0-phase0");
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Prototype PC ChatGPT Web. Session JSON sur stdin pour import ; texte sur stdin pour send.\n"
        "Un core est lancé pour chaque commande et arrêté à la sortie. Aucun token dans les arguments."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"core", "Chemin absolu de rmchat-core (défaut : voisin du probe).", "path"});
    parser.addOption({"vault-dir", "Répertoire privé du coffre local.", "path"});
    parser.addOption({"import-stdin", "JSON sur stdin : check vérifie sans enregistrer ; inspect-models importe puis inspecte dans le même core."});
    parser.addOption({"cursor", "Curseur opaque renvoyé par list/conversation.", "cursor"});
    parser.addOption({"limit", "Nombre de résultats, de 1 à 50.", "count", "20"});
    parser.addOption({"model", "Identifiant de modèle renvoyé par models (send).", "id"});
    parser.addOption({"conversation", "Conversation existante à poursuivre (send).", "id"});
    parser.addOption({"parent", "continuationParentId de la conversation (send).", "id"});
    parser.addOption({"pdf", "PDF à transférer dans le même core avant send ; répétable, maximum 4.", "path"});
    parser.addPositionalArgument("command", "import | status | check | inspect-models | models | list | conversation ID | send | upload PATH | logout");
    parser.addPositionalArgument("argument", "Identifiant de conversation ou chemin du PDF.", "[argument]");
    if (app.arguments().contains("check") || app.arguments().contains("inspect-models")) {
        if (!parser.parse(app.arguments())) {
            if (app.arguments().contains("inspect-models")) return inspectionFailure("input", "INVALID_PARAMS");
            return CheckReport(app.arguments().contains("--import-stdin")).failed("input", "INVALID_PARAMS");
        }
        if (parser.isSet("help")) parser.showHelp(0);
        if (parser.isSet("version")) parser.showVersion();
    } else parser.process(app);
    const auto positional = parser.positionalArguments();
    if (positional.isEmpty())
        parser.showHelp(1);
    const auto command = positional.at(0);
    if (command == "inspect-models") {
        if (positional.size() != 1 || parser.isSet("cursor") || parser.isSet("limit") ||
            parser.isSet("model") || parser.isSet("conversation") || parser.isSet("parent") || parser.isSet("pdf"))
            return inspectionFailure("input", "INVALID_PARAMS");
        const auto corePath = parser.isSet("core") ? parser.value("core") :
            QCoreApplication::applicationDirPath() + "/rmchat-core";
        const auto vaultDirectory = parser.isSet("vault-dir") ? QDir(parser.value("vault-dir")).absolutePath() :
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/vault";
        return inspectModels(corePath, vaultDirectory, parser.isSet("import-stdin"));
    }
    const QStringList commands{"import", "status", "check", "models", "list", "conversation", "send", "upload", "logout"};
    if (command == "check") {
        const bool fromStdin = parser.isSet("import-stdin");
        if (positional.size() != 1 || parser.isSet("cursor") || parser.isSet("limit") ||
            parser.isSet("model") || parser.isSet("conversation") || parser.isSet("parent") || parser.isSet("pdf"))
            return CheckReport(fromStdin).failed("input", "INVALID_PARAMS");
        const auto corePath = parser.isSet("core") ? parser.value("core") :
            QCoreApplication::applicationDirPath() + "/rmchat-core";
        const auto vaultDirectory = parser.isSet("vault-dir") ? QDir(parser.value("vault-dir")).absolutePath() :
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/vault";
        return check(corePath, vaultDirectory, fromStdin);
    }
    if (parser.isSet("import-stdin"))
        return failure(QStringLiteral("--import-stdin est réservé à check et inspect-models."));
    const bool requiresArgument = command == "conversation" || command == "upload";
    if (!commands.contains(command) || positional.size() != (requiresArgument ? 2 : 1))
        return failure(QStringLiteral("Commande ou nombre d’arguments invalide."));
    bool validLimit = false;
    const int limit = parser.value("limit").toInt(&validLimit);
    if (!validLimit || limit < 1 || limit > 50)
        return failure(QStringLiteral("La limite doit être comprise entre 1 et 50."));
    if (command == "send" && (parser.value("model").isEmpty() ||
        parser.isSet("conversation") != parser.isSet("parent") ||
        (parser.isSet("conversation") && (parser.value("conversation").isEmpty() || parser.value("parent").isEmpty()))))
        return failure(QStringLiteral("send exige --model ; pour reprendre, fournir --conversation et --parent."));
    auto pdfPaths = parser.values("pdf");
    if ((command != "send" && !pdfPaths.isEmpty()) || pdfPaths.size() > 4)
        return failure(QStringLiteral("--pdf est réservé à send, avec quatre fichiers maximum."));
    if (command == "upload")
        pdfPaths.append(positional.at(1));
    QStringList uploadRoots;
    for (auto &path : pdfPaths) {
        const QFileInfo file(path);
        if (!file.isFile() || file.isSymLink() || file.size() <= 0 || file.size() > 64 * 1024 * 1024)
            return failure(QStringLiteral("PDF régulier requis, de 1 octet à 64 Mio."));
        path = file.canonicalFilePath();
        if (path.isEmpty())
            return failure(QStringLiteral("PDF inaccessible."));
        const auto root = QFileInfo(path).absolutePath();
        if (!uploadRoots.contains(root))
            uploadRoots.append(root);
    }
    QByteArray input;
    if (command == "import" || command == "send") {
#ifdef Q_OS_UNIX
        if (command == "import" && isatty(STDIN_FILENO))
            return failure(QStringLiteral("Pour import, fournir le JSON par stdin redirigé, sans le coller dans le terminal ni dans le chat."));
#endif
        input = readInput(command == "import" ? rmchat::credentialLimit : 128 * 1024);
        if (command == "send" && (input.isEmpty() || input.size() > 128 * 1024 ||
                                 QString::fromUtf8(input).toUtf8() != input))
            return failure(QStringLiteral("Texte UTF-8 requis sur stdin, limité à 128 Kio."));
    }
    QString credentialError;
    auto candidate = command == "import" ? rmchat::normalizeCredential(input, &credentialError) : QByteArray{};
    if (command == "import") {
        input.fill('\0');
        input.clear();
        if (candidate.isEmpty())
            return failure(credentialError, "INVALID_CREDENTIAL");
    }
    const auto vaultDirectory = parser.isSet("vault-dir") ? QDir(parser.value("vault-dir")).absolutePath() :
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/vault";
    repaper::SecretStore vault(vaultDirectory);
    QLockFile lock(vaultDirectory + "/probe.lock");
    if (!lock.tryLock(0))
        return failure(QStringLiteral("Une autre commande utilise le coffre, ou celui-ci est inaccessible."), "BUSY");
    if (command == "logout") {
        if (!vault.remove(vaultEntry))
            return failure(QStringLiteral("Impossible d’effacer la session locale."));
        output(QJsonObject{{"result", QJsonObject{{"authenticated", false}}}});
        return 0;
    }
    if (command != "import") {
        candidate = vault.get(vaultEntry);
        if (!vault.error().isEmpty())
            return failure(vault.error());
        if (!candidate.isEmpty()) {
            candidate = rmchat::normalizeCredential(candidate, &credentialError);
            if (candidate.isEmpty())
                return failure(QStringLiteral("La session du coffre est invalide ; importer une nouvelle session."));
        }
    }
    const auto corePath = parser.isSet("core") ? parser.value("core") :
        QCoreApplication::applicationDirPath() + "/rmchat-core";
    rmchat::CoreProcess core;
    if (!core.start(corePath, uploadRoots))
        return failure(core.error());
    const auto call = [&core](const QString &method, const QJsonObject &params = {}, int timeout = 120000) {
        return core.call(method, params, timeout, [](const QJsonObject &event) {
            output(QJsonObject{{"jsonrpc", "2.0"}, {"method", "chat.progress"}, {"params", event}});
        });
    };
    if (command == "status") {
        auto response = call("auth.status");
        if (response.isEmpty())
            return failure(core.error());
        if (response.contains("result")) {
            auto status = response.value("result").toObject();
            status.insert("credentialStored", !candidate.isEmpty());
            status.insert("sessionVerifiedThisRun", false);
            // Local metadata only: never infer a kind from a remote response or
            // authenticate the stored value merely to identify its schema.
            status.remove("credentialKind");
            const auto kind = QJsonDocument::fromJson(candidate).object().value("kind").toString();
            if (kind == "access_token" || kind == "session_token") status.insert("credentialKind", kind);
            response.insert("result", status);
        }
        candidate.fill('\0');
        output(response);
        return response.contains("error") ? 1 : 0;
    }
    if (candidate.isEmpty())
        return failure(QStringLiteral("Importer une session avant cette commande."), "AUTH_REQUIRED");
    const auto originalCredential = QJsonDocument::fromJson(candidate).object();
    const bool sessionCredential = originalCredential.value("kind").toString() == "session_token";
    QJsonObject importParams{{"credential", originalCredential}};
    if (sessionCredential) importParams.insert("persistSession", true);
    auto auth = call("auth.import", importParams);
    if (auth.isEmpty()) {
        candidate.fill('\0');
        return failure(core.error());
    }
    if (auth.contains("error")) {
        candidate.fill('\0');
        output(auth);
        return 1;
    }
    if (!auth.value("result").toObject().value("authenticated").toBool()) {
        candidate.fill('\0');
        return failure(QStringLiteral("Le core n’a pas confirmé la session."));
    }
    // credentialUpdate is confidential IPC between our two owned processes.
    // Remove it before any diagnostic output, and commit before other requests.
    auto publicAuth = auth.value("result").toObject();
    const bool hasUpdate = publicAuth.contains("credentialUpdate");
    const auto update = publicAuth.take("credentialUpdate");
    auth.insert("result", publicAuth);
    if (sessionCredential) {
        auto renewed = update.isObject()
            ? rmchat::normalizeCredential(QJsonDocument(update.toObject()).toJson(QJsonDocument::Compact), &credentialError)
            : QByteArray{};
        if (renewed.isEmpty() || QJsonDocument::fromJson(renewed).object().value("kind").toString() != "session_token") {
            candidate.fill('\0'); renewed.fill('\0');
            call("auth.logout", {}, 5000); core.stop();
            return failure(QStringLiteral("Le core n’a pas fourni de session renouvelée enregistrable."));
        }
        candidate.fill('\0'); candidate = renewed; renewed.fill('\0');
    } else if (hasUpdate) {
        candidate.fill('\0'); call("auth.logout", {}, 5000); core.stop();
        return failure(QStringLiteral("Mise à jour de session inattendue refusée."));
    }
    if (command == "import" || sessionCredential) {
        const bool saved = vault.put(vaultEntry, candidate);
        candidate.fill('\0');
        if (!saved) {
            call("auth.logout", {}, 5000);
            core.stop();
            return failure(QStringLiteral("Session vérifiée mais sauvegarde impossible ; connexion arrêtée. Un cookie renouvelé devra être réimporté depuis le navigateur."));
        }
        if (command == "import") { output(auth); return 0; }
    }
    candidate.fill('\0');
    QString method;
    QJsonObject params;
    if (command == "models")
        method = "models.list";
    else if (command == "list" || command == "conversation") {
        method = command == "list" ? "conversations.list" : "conversations.get";
        params = {{"cursor", parser.isSet("cursor") ? QJsonValue(parser.value("cursor")) : QJsonValue::Null}, {"limit", limit}};
        if (command == "conversation")
            params.insert("conversationId", positional.at(1));
    } else if (command == "send") {
        method = "chat.send";
        params = {{"conversationId", parser.isSet("conversation") ? QJsonValue(parser.value("conversation")) : QJsonValue::Null},
                  {"parentMessageId", parser.isSet("parent") ? QJsonValue(parser.value("parent")) : QJsonValue::Null},
                  {"clientMessageId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                  {"modelId", parser.value("model")}, {"text", QString::fromUtf8(input)}};
    } else
        method = "files.upload";
    const auto authResult = auth.value("result").toObject();
    if (!hasMethod(authResult, method) || (!pdfPaths.isEmpty() && !hasMethod(authResult, "files.upload")))
        return failure(QStringLiteral("Cette opération n’est pas disponible dans le core actuel."), "UNSUPPORTED_OPERATION");
    QJsonArray attachments;
    QJsonObject uploadResponse;
    for (const auto &path : pdfPaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !file.peek(5).startsWith("%PDF-"))
            return failure(QStringLiteral("Le fichier ne contient pas un en-tête PDF valide."));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            return failure(QStringLiteral("Impossible de calculer l’empreinte du PDF."));
        uploadResponse = call("files.upload", {{"path", path}, {"filename", QFileInfo(path).fileName()},
                                               {"mimeType", "application/pdf"}, {"sha256", QString::fromLatin1(hash.result().toHex())}}, 180000);
        if (uploadResponse.isEmpty())
            return failure(core.error());
        if (uploadResponse.contains("error")) {
            output(uploadResponse);
            return 1;
        }
        const auto reference = uploadResponse.value("result").toObject().value("attachmentRef");
        if (!reference.isString() || reference.toString().isEmpty())
            return failure(QStringLiteral("Référence PDF absente de la réponse du core."));
        attachments.append(reference);
    }
    if (command == "upload") {
        output(uploadResponse);
        return 0;
    }
    if (command == "send")
        params.insert("attachments", attachments);
    const auto response = call(method, params, command == "send" ? 300000 : 120000);
    if (response.isEmpty())
        return failure(core.error());
    output(response);
    return response.contains("error") ? 1 : 0;
}

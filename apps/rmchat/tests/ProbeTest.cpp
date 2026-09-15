#include "CoreProcess.h"
#include "Credential.h"
#include "SecretStore.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <memory>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif

namespace {
const QByteArray secretA("synthetic-rmchat-secret-alpha");
const QByteArray secretB("synthetic-rmchat-secret-beta");
const QByteArray secretC("synthetic-rmchat-secret-rotated-gamma");
const QString vaultEntry = QStringLiteral("chatgpt-web/credential-v1");
constexpr qsizetype frameLimit = 1024 * 1024;

QByteArray credential(const QByteArray &value = secretA, const QString &kind = "access_token") {
    return QJsonDocument(QJsonObject{{"version", 1}, {"provider", "chatgpt-web"},
                                     {"kind", kind}, {"value", QString::fromLatin1(value)}})
        .toJson(QJsonDocument::Compact);
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QJsonObject readObject(const QString &path) {
    return QJsonDocument::fromJson(readFile(path)).object();
}
class Environment {
public:
    void set(const QByteArray &name, const QByteArray &value) {
        if (!previous.contains(name)) previous.insert(name, qgetenv(name));
        qputenv(name.constData(), value);
    }
    ~Environment() {
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (it.value().isNull()) qunsetenv(it.key().constData());
            else qputenv(it.key().constData(), it.value());
        }
    }
private:
    QMap<QByteArray, QByteArray> previous;
};

QJsonObject capabilities() {
    return {{"protocolVersion", 1}, {"provider", "chatgpt-web"},
            {"credentialKinds", QJsonArray{"access_token", "session_token"}},
            {"methods", QJsonArray{"auth.import", "auth.status", "auth.logout", "models.list",
                                    "conversations.list", "conversations.get", "chat.send",
                                    "files.upload", "request.cancel"}}};
}
QJsonObject authStatus(bool authenticated) {
    return {{"authenticated", authenticated}, {"account", QJsonValue::Null},
            {"capabilities", capabilities()}};
}
QByteArray response(const QJsonValue &id, const QJsonObject &result) {
    return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}})
               .toJson(QJsonDocument::Compact) + '\n';
}
struct FakeState {
    bool authenticated = false;
    QJsonObject record;
    QJsonArray methods;
};

// The test executable doubles as an IPC-only child. It never accesses the network.
int fakeCore(QCoreApplication &app, const QStringList &arguments) {
    const int index = arguments.indexOf("--socket");
    if (index < 0 || index + 1 == arguments.size()) return 60;
    const QString mode = qEnvironmentVariable("RMCHAT_TEST_FAKE_MODE", "valid");
    const QString recordPath = qEnvironmentVariable("RMCHAT_TEST_RECORD");
    auto state = std::make_shared<FakeState>();
    state->record = {{"pid", double(QCoreApplication::applicationPid())},
                     {"preloadPresent", !qEnvironmentVariableIsEmpty("LD_PRELOAD")},
                     {"qtfbPresent", !qEnvironmentVariableIsEmpty("QTFB_KEY") ||
                                         !qEnvironmentVariableIsEmpty("QTFB_TEST_VALUE")},
                     {"credentialInArguments", arguments.join(' ').contains(QString::fromLatin1(secretA)) ||
                                                   arguments.join(' ').contains(QString::fromLatin1(secretB))},
                     {"hasUploadRoot", arguments.contains("--upload-root")}};
    auto record = [state, recordPath] {
        state->record["methods"] = state->methods;
        if (!recordPath.isEmpty()) writeFile(recordPath, QJsonDocument(state->record).toJson());
    };
    record();
    if (mode == "check-startup-fail") return 62;
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(arguments.at(index + 1))) return 61;
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        auto socket = server.nextPendingConnection();
        auto pending = std::make_shared<QByteArray>();
        QObject::connect(socket, &QLocalSocket::disconnected, &app, &QCoreApplication::quit);
        QObject::connect(socket, &QLocalSocket::readyRead, &app,
                         [&, socket, pending, state, mode, record] {
            *pending += socket->readAll();
            for (;;) {
                const auto newline = pending->indexOf('\n');
                if (newline < 0) return;
                const auto request = QJsonDocument::fromJson(pending->left(newline)).object();
                pending->remove(0, newline + 1);
                const auto id = request.value("id");
                const auto method = request.value("method").toString();
                const auto params = request.value("params").toObject();
                state->methods.append(method);
                record();
                if (mode.startsWith("check") || mode.startsWith("inspect")) {
                    // Matching progress with private content must also be suppressed by check.
                    socket->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", "chat.progress"},
                        {"params", QJsonObject{{"requestId", id}, {"sequence", 1}, {"text", QString::fromLatin1(secretA)}}}})
                        .toJson(QJsonDocument::Compact) + '\n');
                    if (mode == "check-malformed-ipc") {
                        socket->write("{invalid:" + secretB + '\n');
                        continue;
                    }
                    const QString failedMethod = mode == "inspect-refuse-auth" ? "auth.import"
                        : mode == "inspect-refuse" ? "models.inspect"
                        : mode == "check-refuse-auth" || mode == "check-invalid-diagnostics" ? "auth.import"
                        : mode == "check-refuse-models" ? "models.list"
                        : mode == "check-refuse-list" ? "conversations.list"
                        : mode == "check-refuse-conversation" ? "conversations.get" : QString();
                    if (method == failedMethod) {
                        QJsonObject data{{"kind", "UPSTREAM_FORBIDDEN"}, {"retryable", false}, {"httpStatus", 403},
                            {"contentType", "application/json"}, {"challenge", false},
                            {"headers", QJsonObject{{"cookie", QString::fromLatin1(secretA)}}},
                            {"conversationId", "private-conversation-A"}, {"rawBody", QString::fromLatin1(secretB)}};
                        if (mode == "check-invalid-diagnostics") {
                            data["kind"] = QString::fromLatin1(secretA);
                            data["retryable"] = "true";
                            data["httpStatus"] = "403";
                            data["contentType"] = "application/json; private=" + QString::fromLatin1(secretB);
                            data["challenge"] = "false";
                        }
                        socket->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                            {"error", QJsonObject{{"code", -32000}, {"message", QString::fromLatin1(secretA)}, {"data", data}}}})
                            .toJson(QJsonDocument::Compact) + '\n');
                        continue;
                    }
                }
                if (mode == "timeout") continue;
                if (mode == "eof") { socket->disconnectFromServer(); return; }
                if (mode == "malformed") { socket->write("{broken\n"); continue; }
                if (mode == "oversize") { socket->write(QByteArray(frameLimit + 1, 'x')); continue; }
                if (mode == "wrong-id") { socket->write(response("ui:unrelated", {})); continue; }
                if (mode == "wrong-version") {
                    socket->write("{\"jsonrpc\":\"1.0\",\"id\":\"ui:1\",\"result\":{}}\n");
                    continue;
                }
                if (mode == "both-result-error") {
                    socket->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                        {"result", QJsonObject{}}, {"error", QJsonObject{}}}).toJson(QJsonDocument::Compact) + '\n');
                    continue;
                }
                QJsonObject result;
                if (method == "auth.import") {
                    const auto candidate = params.value("credential").toObject();
                    const auto value = candidate.value("value").toString().toUtf8();
                    state->record["credentialReceivedOverIpc"] = value == secretA || value == secretB;
                    state->record["credentialWasCandidate"] = value == secretB;
                    // Deliberately noisy synthetic child output must never reach the probe's output.
                    QFile out, err;
                    out.open(stdout, QIODevice::WriteOnly); err.open(stderr, QIODevice::WriteOnly);
                    out.write(value + '\n'); out.flush(); err.write(value + '\n'); err.flush();
                    record();
                    if (mode == "reject-auth") {
                        socket->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                            {"error", QJsonObject{{"code", -32000}, {"message", "Candidat refusé."},
                                {"data", QJsonObject{{"kind", "AUTH_INVALID"}, {"retryable", false}}}}}})
                            .toJson(QJsonDocument::Compact) + '\n');
                        continue;
                    }
                    if (mode == "block-vault" || mode == "rotate-block-vault" || mode == "inspect-block-vault") {
                        const QString directory = qEnvironmentVariable("RMCHAT_TEST_BLOCK_VAULT");
                        const bool moved = !directory.isEmpty() && QDir().rename(directory, directory + ".held");
                        const bool blocked = moved && writeFile(directory, "fixture blocks directory lookup");
                        state->record["vaultBlocked"] = blocked;
                        record();
                    }
                    state->authenticated = mode != "unconfirmed-auth" && mode != "inspect-unconfirmed-auth";
                    result = authStatus(state->authenticated);
                    if (mode.startsWith("rotate") && mode != "rotate-missing") {
                        state->record["persistSession"] = params.value("persistSession").toBool();
                        result["credentialUpdate"] = QJsonDocument::fromJson(credential(secretB,
                            mode == "rotate-invalid-kind" ? "access_token" : "session_token")).object();
                        record();
                    }
                    if (mode.startsWith("check")) {
                        result["account"] = QJsonObject{{"id", "private-account-id"}, {"displayName", "private-account-name"}};
                        result["unexpectedCredential"] = QString::fromLatin1(secretB);
                    }
                    if (mode.startsWith("inspect")) {
                        auto caps = result.value("capabilities").toObject();
                        auto methods = caps.value("methods").toArray();
                        if (mode != "inspect-no-capability") methods.append("models.inspect");
                        caps["methods"] = methods;
                        result["capabilities"] = caps;
                        result["account"] = QJsonObject{{"id", "private-account"}, {"email", "private-mail@example.invalid"}};
                        result["unexpectedCredential"] = QString::fromLatin1(secretA);
                        state->record["persistSession"] = params.value("persistSession").toBool();
                        if ((candidate.value("kind") == "session_token" && mode != "inspect-missing-update") || mode == "inspect-unexpected-update")
                            result["credentialUpdate"] = QJsonDocument::fromJson(credential(mode == "inspect-stdin-session" ? secretC : secretB,
                                mode == "inspect-invalid-update" ? "access_token" : "session_token")).object();
                        record();
                    }
                } else if (method == "auth.status") {
                    result = authStatus(state->authenticated);
                    if (mode == "status-untrusted-kind") result["credentialKind"] = QString::fromLatin1(secretB);
                }
                else if (method == "auth.logout") { state->authenticated = false; result = {{"authenticated", false}}; }
                else if (method == "models.list") {
                    result = {{"items", QJsonArray{QJsonObject{{"id", "fixture-model"}, {"name", "Modèle de test"}}}}, {"defaultModelId", QJsonValue::Null}};
                    if (mode == "rotate-session") {
                        repaper::SecretStore saved(qEnvironmentVariable("RMCHAT_TEST_BLOCK_VAULT"));
                        state->record["rotationSavedBeforeModels"] = saved.get(vaultEntry) == credential(secretB, "session_token");
                        record();
                    }
                }
                else if (method == "models.inspect") {
                    repaper::SecretStore saved(qEnvironmentVariable("RMCHAT_TEST_BLOCK_VAULT"));
                    const auto persistedCredential = saved.get(vaultEntry);
                    state->record["rotationSavedBeforeInspection"] = persistedCredential == credential(mode == "inspect-stdin-session" ? secretC : secretB, "session_token");
                    state->record["accessSavedBeforeInspection"] = persistedCredential == credential(secretB);
                    state->record["inspectionParamsEmpty"] = params.isEmpty();
                    record();
                    result = {{"topLevelKeys", QJsonArray{"models", "categories", "default_model"}},
                        {"default_model", "fixture:model.v2"},
                        {"models", QJsonArray{QJsonObject{{"keys", QJsonArray{"slug", "title", "is_user_selectable", "access", "fixture_new_flag"}},
                            {"slug", "fixture:model.v2"}, {"title", "Fixture model"}, {"is_user_selectable", true},
                            {"fixture_new_flag", false},
                            {"access", QJsonObject{{"keys", QJsonArray{"has_access"}}, {"has_access", true},
                                {"credential", QString::fromLatin1(secretB)}}},
                            {"credential", QString::fromLatin1(secretA)}}}},
                        {"categories", QJsonArray{QJsonObject{{"keys", QJsonArray{"category", "default_model"}},
                            {"category", "fixture-category"}, {"default_model", "fixture:model.v2"}}}},
                        {"account", QJsonObject{{"email", "private-mail@example.invalid"}}},
                        {"rawBody", QString::fromLatin1(secretB)}};
                    if (mode == "inspect-invalid-schema") result["models"] = "private-malformed";
                    if (mode == "inspect-large-schema") result["rawBody"] = QString(128 * 1024, 'x');
                }
                else if (method == "conversations.list") {
                    result = {{"items", QJsonArray{}}, {"nextCursor", QJsonValue::Null}};
                    state->record["listLimit"] = params.value("limit");
                    state->record["listFirstPage"] = params.value("cursor").isNull();
                    if (mode.startsWith("check") && mode != "check-empty") {
                        QJsonArray items;
                        for (int i = 0; i < (mode == "check-list-too-many" ? 6 : 2); ++i)
                            items.append(QJsonObject{{"id", i == 0 ? "private-conversation-A" : "private-conversation-B"},
                                {"title", "private-conversation-title"}, {"updatedAt", "private-time"}});
                        if (mode == "check-invalid-id") items[0] = QJsonObject{{"id", 42}, {"title", "private-title"}};
                        result["items"] = items;
                    }
                    record();
                } else if (method == "conversations.get") {
                    state->record["getLimit"] = params.value("limit");
                    state->record["getFirstPage"] = params.value("cursor").isNull();
                    state->record["readFirstConversation"] = params.value("conversationId") == "private-conversation-A";
                    record();
                    result = {{"id", params.value("conversationId")}, {"title", "private-conversation-title"},
                        {"messages", QJsonArray{QJsonObject{{"id", "private-user-id"}, {"text", "private-question"}},
                            QJsonObject{{"id", "private-assistant-id"}, {"text", "private-answer"}}}},
                        {"continuationParentId", "private-parent-id"}, {"nextCursor", "private-cursor"}};
                }
                else if (method == "files.upload") {
                    const auto bytes = readFile(params.value("path").toString());
                    state->record["uploadHashMatches"] = params.value("sha256").toString().toLatin1() ==
                        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
                    state->record["uploadMimeMatches"] = params.value("mimeType") == "application/pdf";
                    record();
                    result = {{"attachmentRef", "fixture-attachment"}, {"filename", params.value("filename")},
                              {"mimeType", "application/pdf"}, {"size", bytes.size()}};
                } else if (method == "chat.send") {
                    state->record["sendHadAttachment"] = params.value("attachments").toArray().contains("fixture-attachment");
                    state->record["sendTextBytes"] = params.value("text").toString().toUtf8().size();
                    state->record["sendModel"] = params.value("modelId");
                    state->record["sendHadClientId"] = !QUuid(params.value("clientMessageId").toString()).isNull();
                    record();
                    for (const auto &requestId : {QString("ui:unrelated"), id.toString()}) {
                        socket->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", "chat.progress"},
                            {"params", QJsonObject{{"requestId", requestId}, {"sequence", 1},
                                {"conversationId", QJsonValue::Null}, {"messageId", QJsonValue::Null}, {"text", "Réponse partielle"}}}})
                            .toJson(QJsonDocument::Compact) + '\n');
                    }
                    result = {{"conversationId", "fixture-conversation"},
                              {"message", QJsonObject{{"id", "fixture-assistant"}, {"parentId", "fixture-user"},
                                  {"role", "assistant"}, {"text", "Réponse terminée"}, {"attachments", QJsonArray{}}}},
                              {"continuationParentId", "fixture-assistant"}};
                }
                if (mode == "check-models-missing" && method == "models.list") result.remove("items");
                if (mode == "check-list-not-array" && method == "conversations.list") result["items"] = "private-invalid-list";
                if (mode == "check-messages-not-array" && method == "conversations.get") result["messages"] = "private-invalid-messages";
                if (mode == "check-wrong-conversation" && method == "conversations.get") result["id"] = "private-other-conversation";
                const auto bytes = response(id, result);
                if (mode == "split") {
                    socket->write(bytes.left(7));
                    QTimer::singleShot(20, socket, [socket, bytes] { socket->write(bytes.mid(7)); });
                } else socket->write(bytes);
                socket->flush();
            }
        });
    });
    return app.exec();
}

struct ProcessResult { bool finished = false; int exitCode = -1; QByteArray output, error; };
ProcessResult probe(const QString &vault, const QStringList &command, const QByteArray &input = {},
                    const QString &mode = "valid", const QString &record = {}, const QString &blockVault = {}) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("RMCHAT_TEST_FAKE_MODE", mode);
    environment.insert("RMCHAT_TEST_RECORD", record);
    environment.insert("RMCHAT_TEST_BLOCK_VAULT", blockVault);
    process.setProcessEnvironment(environment);
    const auto program = QCoreApplication::applicationDirPath() + "/rmchat-probe";
    QStringList arguments{"--core", QCoreApplication::applicationFilePath(), "--vault-dir", vault};
    arguments.append(command);
    process.start(program, arguments);
    ProcessResult result;
    if (!process.waitForStarted(5000)) return result;
    process.write(input);
    process.closeWriteChannel();
    result.finished = process.waitForFinished(15000);
    if (!result.finished) { process.kill(); process.waitForFinished(1000); }
    result.exitCode = process.exitCode();
    result.output = process.readAllStandardOutput();
    result.error = process.readAllStandardError();
    return result;
}
bool processExists(qint64 pid) {
#ifdef Q_OS_UNIX
    return pid > 0 && (::kill(pid_t(pid), 0) == 0 || errno == EPERM);
#else
    Q_UNUSED(pid);
    return false;
#endif
}
} // namespace

class ProbeTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(QFileInfo(QCoreApplication::applicationDirPath() + "/rmchat-probe").isExecutable());
    }
    void strictCredentialAndSessionConversion() {
        QString error;
        QCOMPARE(rmchat::normalizeCredential(credential(), &error), credential());
        QVERIFY(error.isEmpty());
        QCOMPARE(rmchat::normalizeCredential(credential(secretB, "session_token"), nullptr), credential(secretB, "session_token"));
        const auto source = QJsonDocument(QJsonObject{{"accessToken", QString::fromLatin1(secretA)},
            {"user", QJsonObject{{"email", "fixture@example.invalid"}, {"id", "private-extra"}}},
            {"expires", "2099-01-01T00:00:00Z"}, {"refreshToken", "never-store-this-extra"}}).toJson();
        const auto normalized = rmchat::normalizeCredential(source, &error);
        QCOMPARE(normalized, credential());
        QVERIFY(!normalized.contains("fixture@example.invalid"));
        QVERIFY(!normalized.contains("private-extra"));
        QVERIFY(!normalized.contains("never-store-this-extra"));
    }
    void inspectionPersistsSessionAndOnlyReadsModelSchema() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        repaper::SecretStore vault(vaultPath);
        QVERIFY(vault.put(vaultEntry, credential(secretA, "session_token")));
        const auto inspected = probe(vaultPath, {"inspect-models"}, {}, "inspect", recordPath, vaultPath);
        QVERIFY(inspected.finished); QCOMPARE(inspected.exitCode, 0);
        QCOMPARE(inspected.output.trimmed().split('\n').size(), 1); QVERIFY(inspected.error.isEmpty());
        const auto report = QJsonDocument::fromJson(inspected.output).object().value("inspection").toObject();
        QVERIFY(report.value("ok").toBool()); QVERIFY(report.value("credentialPersisted").toBool());
        const auto schema = report.value("schema").toObject();
        QCOMPARE(schema.value("default_model").toString(), QString("fixture:model.v2"));
        QCOMPARE(schema.value("models").toArray().size(), 1);
        const auto flag = schema.value("models").toArray().first().toObject().value("fixture_new_flag");
        QVERIFY(flag.isBool()); QVERIFY(!flag.toBool());
        QVERIFY(schema.value("models").toArray().first().toObject().value("access").toObject().value("has_access").toBool());
        QCOMPARE(schema.value("categories").toArray().size(), 1);
        const auto record = readObject(recordPath);
        QCOMPARE(record.value("methods").toArray(), (QJsonArray{"auth.import", "models.inspect"}));
        QVERIFY(record.value("persistSession").toBool()); QVERIFY(record.value("rotationSavedBeforeInspection").toBool());
        QVERIFY(record.value("inspectionParamsEmpty").toBool());
        QCOMPARE(vault.get(vaultEntry), credential(secretB, "session_token"));
        for (const auto &marker : {secretA, secretB, QByteArray("private-"), QByteArray("rawBody"),
                                  QByteArray("credentialUpdate"), QByteArray("chat.progress")}) {
            QVERIFY(!inspected.output.contains(marker)); QVERIFY(!inspected.error.contains(marker));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(qint64(record.value("pid").toDouble())), 2000);
    }
    void inspectionImportsStdinInOneCore_data() {
        QTest::addColumn<bool>("session"); QTest::addColumn<bool>("existingVault");
        QTest::newRow("fresh-browser-access") << false << true;
        QTest::newRow("fresh-rotating-session") << true << true;
        QTest::newRow("first-browser-import") << false << false;
        QTest::newRow("first-session-import") << true << false;
    }
    void inspectionImportsStdinInOneCore() {
        QFETCH(bool, session); QFETCH(bool, existingVault);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        if (existingVault) {
            repaper::SecretStore previous(vaultPath); QVERIFY(previous.put(vaultEntry, credential()));
        }
        const auto input = session ? credential(secretB, "session_token") : QJsonDocument(QJsonObject{
            {"accessToken", QString::fromLatin1(secretB)}, {"user", QJsonObject{{"email", "private-mail@example.invalid"}}},
            {"expires", "2099-01-01T00:00:00Z"}, {"refreshToken", "private-refresh-extra"}}).toJson();
        const auto inspected = probe(vaultPath, {"inspect-models", "--import-stdin"}, input,
            session ? "inspect-stdin-session" : "inspect", recordPath, vaultPath);
        QVERIFY(inspected.finished); QCOMPARE(inspected.exitCode, 0);
        QCOMPARE(inspected.output.trimmed().split('\n').size(), 1); QVERIFY(inspected.error.isEmpty());
        const auto report = QJsonDocument::fromJson(inspected.output).object().value("inspection").toObject();
        QVERIFY(report.value("ok").toBool()); QVERIFY(report.value("credentialPersisted").toBool());
        const auto record = readObject(recordPath);
        QCOMPARE(record.value("methods").toArray(), (QJsonArray{"auth.import", "models.inspect"}));
        QVERIFY(record.value("credentialWasCandidate").toBool());
        QVERIFY(!record.value("credentialInArguments").toBool());
        QCOMPARE(record.value("persistSession").toBool(), session);
        QVERIFY(record.value(session ? "rotationSavedBeforeInspection" : "accessSavedBeforeInspection").toBool());
        QVERIFY(record.value("inspectionParamsEmpty").toBool());
        repaper::SecretStore vault(vaultPath);
        QCOMPARE(vault.get(vaultEntry), session ? credential(secretC, "session_token") : credential(secretB));
        for (const auto &marker : {secretA, secretB, secretC, QByteArray("private-"), QByteArray("account"),
                                  QByteArray("credentialUpdate"), QByteArray("chat.progress"), QByteArray("unexpectedCredential")}) {
            QVERIFY(!inspected.output.contains(marker)); QVERIFY(!inspected.error.contains(marker));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(qint64(record.value("pid").toDouble())), 2000);
    }
    void inspectionStdinPreservesVaultUntilConfirmed_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("session");
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("persisted"); QTest::addColumn<bool>("calledInspection");
        QTest::newRow("access-auth-refused") << QString("inspect-refuse-auth") << false << QString("UPSTREAM_FORBIDDEN") << false << false;
        QTest::newRow("session-auth-refused") << QString("inspect-refuse-auth") << true << QString("UPSTREAM_FORBIDDEN") << false << false;
        QTest::newRow("unconfirmed") << QString("inspect-unconfirmed-auth") << false << QString("AUTH_UNCONFIRMED") << false << false;
        QTest::newRow("missing-rotation") << QString("inspect-missing-update") << true << QString("INVALID_CREDENTIAL_UPDATE") << false << false;
        QTest::newRow("invalid-rotation") << QString("inspect-invalid-update") << true << QString("INVALID_CREDENTIAL_UPDATE") << false << false;
        QTest::newRow("unexpected-rotation") << QString("inspect-unexpected-update") << false << QString("INVALID_CREDENTIAL_UPDATE") << false << false;
        QTest::newRow("access-save-failed") << QString("inspect-block-vault") << false << QString("VAULT_ERROR") << false << false;
        QTest::newRow("session-save-failed") << QString("inspect-block-vault") << true << QString("SESSION_PERSISTENCE_REQUIRED") << false << false;
        QTest::newRow("access-models-refused") << QString("inspect-refuse") << false << QString("UPSTREAM_FORBIDDEN") << true << true;
        QTest::newRow("session-models-refused") << QString("inspect-refuse") << true << QString("UPSTREAM_FORBIDDEN") << true << true;
        QTest::newRow("inspection-disabled") << QString("inspect-no-capability") << false << QString("UNSUPPORTED_OPERATION") << true << false;
    }
    void inspectionStdinPreservesVaultUntilConfirmed() {
        QFETCH(QString, mode); QFETCH(bool, session); QFETCH(QString, kind); QFETCH(bool, persisted); QFETCH(bool, calledInspection);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        repaper::SecretStore vault(vaultPath); QVERIFY(vault.put(vaultEntry, credential()));
        const auto secretFile = QDir(vaultPath).entryList({"*.secret"}, QDir::Files).first();
        const auto previous = readFile(QDir(vaultPath).filePath(secretFile));
        const auto inspected = probe(vaultPath, {"inspect-models", "--import-stdin"},
            credential(secretB, session ? "session_token" : "access_token"), mode, recordPath, vaultPath);
        QVERIFY(inspected.finished); QCOMPARE(inspected.exitCode, 1);
        QCOMPARE(inspected.output.trimmed().split('\n').size(), 1); QVERIFY(inspected.error.isEmpty());
        const auto report = QJsonDocument::fromJson(inspected.output).object().value("inspection").toObject();
        QVERIFY(!report.value("ok").toBool()); QCOMPARE(report.value("credentialPersisted").toBool(), persisted);
        QCOMPARE(report.value("error").toObject().value("kind").toString(), kind);
        const auto record = readObject(recordPath);
        QCOMPARE(record.value("methods").toArray(), calledInspection ? QJsonArray({"auth.import", "models.inspect"}) : QJsonArray({"auth.import"}));
        const auto preservedDirectory = mode == "inspect-block-vault" ? vaultPath + ".held" : vaultPath;
        repaper::SecretStore preserved(preservedDirectory);
        QCOMPARE(preserved.get(vaultEntry), persisted ? credential(secretB, session ? "session_token" : "access_token") : credential());
        if (!persisted) QCOMPARE(readFile(QDir(preservedDirectory).filePath(secretFile)), previous);
        for (const auto &marker : {secretA, secretB, secretC, QByteArray("private-"), QByteArray("credentialUpdate"), QByteArray("chat.progress")}) {
            QVERIFY(!inspected.output.contains(marker)); QVERIFY(!inspected.error.contains(marker));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(qint64(record.value("pid").toDouble())), 2000);
    }
    void inspectionRejectsInvalidStdinBeforeCore_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("empty") << QByteArray{};
        QTest::newRow("raw-secret") << secretB;
        QTest::newRow("too-large") << QByteArray(rmchat::credentialLimit + 1, 'x');
        QTest::newRow("invalid-json") << QByteArray("{\"accessToken\":");
    }
    void inspectionRejectsInvalidStdinBeforeCore() {
        QFETCH(QByteArray, input);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        const auto inspected = probe(vaultPath, {"inspect-models", "--import-stdin"}, input, "inspect", recordPath, vaultPath);
        QVERIFY(inspected.finished); QCOMPARE(inspected.exitCode, 1); QVERIFY(inspected.error.isEmpty());
        const auto report = QJsonDocument::fromJson(inspected.output).object().value("inspection").toObject();
        QCOMPARE(report.value("step").toString(), QString("input"));
        QCOMPARE(report.value("error").toObject().value("kind").toString(), QString("INVALID_CREDENTIAL"));
        QVERIFY(!QFileInfo::exists(recordPath)); QVERIFY(!QFileInfo::exists(vaultPath));
        QVERIFY(!inspected.output.contains(secretB));
    }
    void inspectionStopsWithoutAdditionalRpc_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("calledInspection");
        QTest::newRow("capability-disabled") << QString("inspect-no-capability") << QString("UNSUPPORTED_OPERATION") << false;
        QTest::newRow("auth-refused") << QString("inspect-refuse-auth") << QString("UPSTREAM_FORBIDDEN") << false;
        QTest::newRow("missing-update") << QString("inspect-missing-update") << QString("INVALID_CREDENTIAL_UPDATE") << false;
        QTest::newRow("invalid-update") << QString("inspect-invalid-update") << QString("INVALID_CREDENTIAL_UPDATE") << false;
        QTest::newRow("persistence-failed") << QString("inspect-block-vault") << QString("SESSION_PERSISTENCE_REQUIRED") << false;
        QTest::newRow("models-refused") << QString("inspect-refuse") << QString("UPSTREAM_FORBIDDEN") << true;
        QTest::newRow("schema-invalid") << QString("inspect-invalid-schema") << QString("IPC_PROTOCOL_ERROR") << true;
        QTest::newRow("schema-too-large") << QString("inspect-large-schema") << QString("RESPONSE_TOO_LARGE") << true;
    }
    void inspectionStopsWithoutAdditionalRpc() {
        QFETCH(QString, mode); QFETCH(QString, kind); QFETCH(bool, calledInspection);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        repaper::SecretStore vault(vaultPath);
        QVERIFY(vault.put(vaultEntry, credential(secretA, "session_token")));
        const auto inspected = probe(vaultPath, {"inspect-models"}, {}, mode, recordPath, vaultPath);
        QVERIFY(inspected.finished); QCOMPARE(inspected.exitCode, 1);
        QCOMPARE(inspected.output.trimmed().split('\n').size(), 1); QVERIFY(inspected.error.isEmpty());
        const auto report = QJsonDocument::fromJson(inspected.output).object().value("inspection").toObject();
        QVERIFY(!report.value("ok").toBool());
        QCOMPARE(report.value("error").toObject().value("kind").toString(), kind);
        QCOMPARE(readObject(recordPath).value("methods").toArray(), calledInspection
            ? QJsonArray({"auth.import", "models.inspect"}) : QJsonArray({"auth.import"}));
        for (const auto &marker : {secretA, secretB, QByteArray("private-"), QByteArray("rawBody"), QByteArray("headers")}) {
            QVERIFY(!inspected.output.contains(marker)); QVERIFY(!inspected.error.contains(marker));
        }
    }
    void inspectionRejectsArgumentsBeforeCore_data() {
        QTest::addColumn<QStringList>("command");
        QTest::newRow("stdin-history-cursor") << QStringList{"inspect-models", "--import-stdin", "--cursor", QString::fromLatin1(secretA)};
        QTest::newRow("history-cursor") << QStringList{"inspect-models", "--cursor", QString::fromLatin1(secretA)};
        QTest::newRow("unknown-arg") << QStringList{"inspect-models", "--" + QString::fromLatin1(secretA)};
        QTest::newRow("positional-secret") << QStringList{"inspect-models", QString::fromLatin1(secretA)};
    }
    void inspectionRejectsArgumentsBeforeCore() {
        QFETCH(QStringList, command);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto recordPath = directory.filePath("record.json");
        const auto inspected = probe(directory.filePath("vault"), command, {}, "inspect", recordPath);
        QVERIFY(inspected.finished); QCOMPARE(inspected.exitCode, 1);
        QVERIFY(inspected.error.isEmpty()); QVERIFY(!inspected.output.contains(secretA));
        QVERIFY(!QFileInfo::exists(recordPath)); QVERIFY(!QFileInfo::exists(directory.filePath("vault")));
        const auto report = QJsonDocument::fromJson(inspected.output).object().value("inspection").toObject();
        QCOMPARE(report.value("error").toObject().value("kind").toString(), QString("INVALID_PARAMS"));
    }
    void rotatedSessionStaysPrivateAndPersistsBeforeNextRequest() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        repaper::SecretStore vault(vaultPath);
        QVERIFY(vault.put(vaultEntry, credential(secretA, "session_token")));
        const auto result = probe(vaultPath, {"models"}, {}, "rotate-session", recordPath, vaultPath);
        QVERIFY(result.finished); QCOMPARE(result.exitCode, 0);
        QCOMPARE(vault.get(vaultEntry), credential(secretB, "session_token"));
        const auto record = readObject(recordPath);
        QVERIFY(record.value("persistSession").toBool());
        QVERIFY(record.value("rotationSavedBeforeModels").toBool());
        QVERIFY(!result.output.contains(secretA)); QVERIFY(!result.output.contains(secretB));
        QVERIFY(!result.output.contains("credentialUpdate"));
        QVERIFY(!result.error.contains(secretA)); QVERIFY(!result.error.contains(secretB));
    }
    void invalidRotationDoesNotReplaceVault_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("missing") << QString("rotate-missing");
        QTest::newRow("wrong-kind") << QString("rotate-invalid-kind");
    }
    void invalidRotationDoesNotReplaceVault() {
        QFETCH(QString, mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        repaper::SecretStore vault(vaultPath); QVERIFY(vault.put(vaultEntry, credential()));
        const auto result = probe(vaultPath, {"import"}, credential(secretA, "session_token"), mode, recordPath);
        QVERIFY(result.finished); QCOMPARE(result.exitCode, 1);
        QCOMPARE(vault.get(vaultEntry), credential());
        QVERIFY(readObject(recordPath).value("methods").toArray().contains("auth.logout"));
        QVERIFY(!result.output.contains(secretA)); QVERIFY(!result.output.contains(secretB));
    }
    void checkDoesNotConsumeRotatingCookie() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto recordPath = directory.filePath("record.json");
        const auto result = probe(directory.filePath("unused-vault"), {"check", "--import-stdin"},
                                  credential(secretA, "session_token"), "rotate-session", recordPath);
        QVERIFY(result.finished); QCOMPARE(result.exitCode, 1);
        QVERIFY(result.output.contains("SESSION_PERSISTENCE_REQUIRED"));
        QVERIFY(!QFileInfo::exists(recordPath));
        QVERIFY(!QFileInfo::exists(directory.filePath("unused-vault")));
        QVERIFY(!result.output.contains(secretA));
    }
    void cancellationClosesOwnedCore() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Environment environment;
        environment.set("RMCHAT_TEST_FAKE_MODE", "timeout");
        environment.set("RMCHAT_TEST_RECORD", directory.filePath("record.json").toUtf8());
        rmchat::CoreProcess core;
        QVERIFY(core.start(QCoreApplication::applicationFilePath()));
        QElapsedTimer timer; timer.start();
        core.setCancellationCheck([&] { return timer.elapsed() > 100; });
        QVERIFY(core.call("auth.status", {}, 10000).isEmpty());
        QVERIFY(timer.elapsed() < 4000);
        QVERIFY(core.error().contains("interrompue"));
        const auto pid = qint64(readObject(directory.filePath("record.json")).value("pid").toDouble());
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(pid), 2000);
    }
    void malformedCredential_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("raw-token") << secretA;
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("broken-json") << QByteArray("{broken");
        QTest::newRow("oversized") << QByteArray(rmchat::credentialLimit + 1, 'x');
        auto base = QJsonDocument::fromJson(credential()).object();
        const QList<QPair<QString, QJsonValue>> changes{{"version", 2}, {"version", 1.5}, {"version", "1"},
            {"provider", "moodle"}, {"kind", "refresh_token"}, {"value", QJsonValue::Null},
            {"value", ""}, {"value", " leading"}, {"value", "trailing "}, {"value", "two words"},
            {"value", "line\nbreak"}, {"value", QString("n") + QChar(0) + "ul"}, {"value", "é-token"}};
        int row = 0;
        for (const auto &change : changes) {
            auto value = base; value[change.first] = change.second;
            QTest::newRow(qPrintable(QString("invalid-field-%1").arg(++row))) << QJsonDocument(value).toJson();
        }
        auto extra = base; extra["extra"] = "not-allowed";
        QTest::newRow("extra-field") << QJsonDocument(extra).toJson();
        auto missing = base; missing.remove("value");
        QTest::newRow("missing-value") << QJsonDocument(missing).toJson();
        QTest::newRow("cookie-separator-semicolon") << credential("session;other=x", "session_token");
        QTest::newRow("cookie-separator-comma") << credential("session,other=x", "session_token");
    }
    void malformedCredential() {
        QFETCH(QByteArray, input);
        QString error;
        QVERIFY(rmchat::normalizeCredential(input, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains(QString::fromLatin1(secretA)));
        QVERIFY(!error.contains(QString::fromLatin1(secretB)));
    }
    void credentialByteBoundary() {
        const auto overhead = credential({}).size();
        const auto exact = credential(QByteArray(rmchat::credentialLimit - overhead, 'a'));
        QCOMPARE(exact.size(), rmchat::credentialLimit);
        QCOMPARE(rmchat::normalizeCredential(exact, nullptr), exact);
        QVERIFY(rmchat::normalizeCredential(exact + ' ', nullptr).isEmpty());
    }
    void vaultNameBindingAndRejectedWrite() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        repaper::SecretStore vault(directory.path());
        QVERIFY(vault.put(vaultEntry, credential()));
        auto files = QDir(directory.path()).entryList({"*.secret"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        const auto first = readFile(directory.filePath(files.first()));
        QVERIFY(!first.contains(secretA));
        QVERIFY(!vault.put(vaultEntry, QByteArray(65537, 'x')));
        QCOMPARE(readFile(directory.filePath(files.first())), first);
        QCOMPARE(vault.get(vaultEntry), credential());
        const QString otherName = "other-provider/credential-v1";
        const QString copiedName = QString::fromLatin1(QCryptographicHash::hash(otherName.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".secret";
        QVERIFY(writeFile(directory.filePath(copiedName), first));
        QVERIFY(vault.get(otherName).isEmpty());
        QVERIFY(!vault.error().isEmpty());
    }
    void fragmentedFramesAndOwnedChild() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Environment environment;
        environment.set("RMCHAT_TEST_FAKE_MODE", "split");
        environment.set("RMCHAT_TEST_RECORD", directory.filePath("record.json").toUtf8());
        environment.set("LD_PRELOAD", "/fixture-do-not-load.so");
        environment.set("QTFB_KEY", "123");
        environment.set("QTFB_TEST_VALUE", "fixture");
        rmchat::CoreProcess core;
        QVERIFY2(core.start(QCoreApplication::applicationFilePath()), qPrintable(core.error()));
        auto result = core.call("auth.status", {}, 5000);
        QCOMPARE(result.value("id").toString(), QString("ui:1"));
        QVERIFY(result.value("result").isObject());
        result = core.call("auth.status", {}, 5000);
        QCOMPARE(result.value("id").toString(), QString("ui:2"));
        const auto record = readObject(directory.filePath("record.json"));
        QVERIFY(!record.value("preloadPresent").toBool());
        QVERIFY(!record.value("qtfbPresent").toBool());
        QVERIFY(!record.value("credentialInArguments").toBool());
        const auto pid = qint64(record.value("pid").toDouble());
        core.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(pid), 2000);
    }
    void malformedPeer_data() {
        QTest::addColumn<QString>("mode");
        for (const char *mode : {"malformed", "oversize", "wrong-id", "wrong-version", "both-result-error", "eof", "timeout"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void malformedPeer() {
        QFETCH(QString, mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Environment environment;
        environment.set("RMCHAT_TEST_FAKE_MODE", mode.toUtf8());
        environment.set("RMCHAT_TEST_RECORD", directory.filePath("record.json").toUtf8());
        rmchat::CoreProcess core;
        QVERIFY2(core.start(QCoreApplication::applicationFilePath()), qPrintable(core.error()));
        const auto pid = qint64(readObject(directory.filePath("record.json")).value("pid").toDouble());
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(core.call("auth.status", {}, mode == "timeout" ? 80 : 2000).isEmpty());
        QVERIFY(!core.error().isEmpty());
        QVERIFY(elapsed.elapsed() < 4000);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(pid), 2000);
    }
    void progressBelongsToRequest() {
        Environment environment; environment.set("RMCHAT_TEST_FAKE_MODE", "valid");
        rmchat::CoreProcess core;
        QVERIFY(core.start(QCoreApplication::applicationFilePath()));
        QList<QJsonObject> progress;
        auto result = core.call("chat.send", {}, 5000, [&progress](const QJsonObject &event) { progress.append(event); });
        QCOMPARE(progress.size(), 1);
        QCOMPARE(progress.first().value("requestId").toString(), QString("ui:1"));
        QCOMPARE(result.value("result").toObject().value("conversationId").toString(), QString("fixture-conversation"));
    }
    void cliRejectsCandidateWithoutReplacingVault_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("explicit-rejection") << QString("reject-auth");
        QTest::newRow("unconfirmed-success") << QString("unconfirmed-auth");
    }
    void cliRejectsCandidateWithoutReplacingVault() {
        QFETCH(QString, mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        auto first = probe(vaultPath, {"import"}, credential());
        QVERIFY(first.finished); QCOMPARE(first.exitCode, 0);
        repaper::SecretStore vault(vaultPath);
        QCOMPARE(vault.get(vaultEntry), credential());
        const auto files = QDir(vaultPath).entryList({"*.secret"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        const auto cipherPath = QDir(vaultPath).filePath(files.first());
        const auto previous = readFile(cipherPath);
        const auto recordPath = directory.filePath("record.json");
        const auto rejected = probe(vaultPath, {"import"}, credential(secretB), mode, recordPath);
        QVERIFY(rejected.finished); QVERIFY(rejected.exitCode != 0);
        QCOMPARE(readFile(cipherPath), previous);
        QCOMPARE(vault.get(vaultEntry), credential());
        for (const auto &bytes : {first.output, first.error, rejected.output, rejected.error}) {
            QVERIFY(!bytes.contains(secretA)); QVERIFY(!bytes.contains(secretB));
        }
        const auto record = readObject(recordPath);
        QVERIFY(record.value("credentialReceivedOverIpc").toBool());
        QVERIFY(!record.value("credentialInArguments").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(qint64(record.value("pid").toDouble())), 2000);
    }
    void cliPersistenceFailureCancelsProvisionalCore() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        auto first = probe(vaultPath, {"import"}, credential());
        QVERIFY(first.finished); QCOMPARE(first.exitCode, 0);
        const auto recordPath = directory.filePath("record.json");
        auto failed = probe(vaultPath, {"import"}, credential(secretB), "block-vault", recordPath, vaultPath);
        QVERIFY(failed.finished); QVERIFY(failed.exitCode != 0);
        const auto record = readObject(recordPath);
        QVERIFY(record.value("vaultBlocked").toBool());
        QCOMPARE(record.value("methods").toArray(), (QJsonArray{"auth.import", "auth.logout"}));
        repaper::SecretStore preserved(vaultPath + ".held");
        QCOMPARE(preserved.get(vaultEntry), credential());
        QVERIFY(!failed.output.contains(secretB)); QVERIFY(!failed.error.contains(secretB));
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(qint64(record.value("pid").toDouble())), 2000);
    }
    void cliStatusDoesNotVerifyAndNetworkCommandDoes() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        auto imported = probe(vaultPath, {"import"}, credential());
        QVERIFY(imported.finished); QCOMPARE(imported.exitCode, 0);
        const auto recordPath = directory.filePath("record.json");
        auto status = probe(vaultPath, {"status"}, {}, "reject-auth", recordPath);
        QVERIFY(status.finished); QCOMPARE(status.exitCode, 0);
        const auto result = QJsonDocument::fromJson(status.output).object().value("result").toObject();
        QVERIFY(result.value("credentialStored").toBool());
        QVERIFY(!result.value("sessionVerifiedThisRun").toBool());
        QVERIFY(!result.value("authenticated").toBool());
        QCOMPARE(readObject(recordPath).value("methods").toArray(), QJsonArray{"auth.status"});
        auto models = probe(vaultPath, {"models"}, {}, "valid", recordPath);
        QVERIFY(models.finished); QCOMPARE(models.exitCode, 0);
        QCOMPARE(readObject(recordPath).value("methods").toArray(), (QJsonArray{"auth.import", "models.list"}));
        QVERIFY(!models.output.contains(secretA)); QVERIFY(!models.error.contains(secretA));
    }
    void cliStatusReportsOnlyValidatedLocalKind_data() {
        QTest::addColumn<QByteArray>("stored"); QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("valid");
        QTest::newRow("missing") << QByteArray{} << QString{} << true;
        QTest::newRow("access-token") << credential() << QString("access_token") << true;
        QTest::newRow("session-token") << credential(secretA, "session_token") << QString("session_token") << true;
        auto invalid = QJsonDocument::fromJson(credential()).object(); invalid["kind"] = QString::fromLatin1(secretB);
        QTest::newRow("invalid-kind") << QJsonDocument(invalid).toJson(QJsonDocument::Compact) << QString{} << false;
    }
    void cliStatusReportsOnlyValidatedLocalKind() {
        QFETCH(QByteArray, stored); QFETCH(QString, kind); QFETCH(bool, valid);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        const auto recordPath = directory.filePath("record.json");
        repaper::SecretStore vault(vaultPath);
        QByteArray encrypted;
        QString secretPath;
        if (!stored.isEmpty()) {
            QVERIFY(vault.put(vaultEntry, stored));
            secretPath = QDir(vaultPath).filePath(QDir(vaultPath).entryList({"*.secret"}, QDir::Files).first());
            encrypted = readFile(secretPath);
        }
        const auto status = probe(vaultPath, {"status"}, {}, "status-untrusted-kind", recordPath);
        QVERIFY(status.finished); QCOMPARE(status.exitCode, valid ? 0 : 1);
        QCOMPARE(status.output.trimmed().split('\n').size(), 1); QVERIFY(status.error.isEmpty());
        for (const auto &marker : {secretA, secretB, secretC}) QVERIFY(!status.output.contains(marker));
        if (valid) {
            const auto result = QJsonDocument::fromJson(status.output).object().value("result").toObject();
            QCOMPARE(result.value("credentialStored").toBool(), !stored.isEmpty());
            QVERIFY(!result.value("sessionVerifiedThisRun").toBool()); QVERIFY(!result.value("authenticated").toBool());
            if (kind.isEmpty()) QVERIFY(result.value("credentialKind").isUndefined());
            else QCOMPARE(result.value("credentialKind").toString(), kind);
            const auto record = readObject(recordPath);
            QCOMPARE(record.value("methods").toArray(), (QJsonArray{"auth.status"}));
            QVERIFY(!record.value("credentialReceivedOverIpc").toBool());
            QVERIFY(!record.value("credentialInArguments").toBool());
        } else QVERIFY(!QFileInfo::exists(recordPath));
        if (!secretPath.isEmpty()) QCOMPARE(readFile(secretPath), encrypted);
        QCOMPARE(vault.get(vaultEntry), stored);
    }
    void cliLockAndLocalLogout() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        repaper::SecretStore vault(vaultPath); QVERIFY(vault.put(vaultEntry, credential()));
        const auto recordPath = directory.filePath("record.json");
        QLockFile lock(vaultPath + "/probe.lock"); QVERIFY(lock.tryLock(0));
        auto busy = probe(vaultPath, {"logout"}, {}, "valid", recordPath);
        QVERIFY(busy.finished); QVERIFY(busy.exitCode != 0);
        QCOMPARE(QJsonDocument::fromJson(busy.output).object().value("error").toObject().value("data").toObject().value("kind").toString(), QString("BUSY"));
        QCOMPARE(vault.get(vaultEntry), credential());
        QVERIFY(!QFileInfo::exists(recordPath));
        lock.unlock();
        auto removed = probe(vaultPath, {"logout"}, {}, "valid", recordPath);
        QVERIFY(removed.finished); QCOMPARE(removed.exitCode, 0);
        QVERIFY(vault.get(vaultEntry).isEmpty());
        QVERIFY(!QFileInfo::exists(recordPath));
    }
    void cliInvalidInputNeverStartsCore_data() {
        QTest::addColumn<QStringList>("command"); QTest::addColumn<QByteArray>("input");
        QTest::newRow("credential-limit") << QStringList{"import"} << QByteArray(rmchat::credentialLimit + 1, 'x');
        QTest::newRow("credential-raw") << QStringList{"import"} << secretA;
        QTest::newRow("send-limit") << QStringList{"send", "--model", "fixture-model"} << QByteArray(128 * 1024 + 1, 'x');
        QTest::newRow("send-invalid-utf8") << QStringList{"send", "--model", "fixture-model"} << QByteArray("\xff", 1);
        QTest::newRow("send-missing-parent") << QStringList{"send", "--model", "fixture-model", "--conversation", "fixture-conversation"} << QByteArray("Question");
    }
    void cliInvalidInputNeverStartsCore() {
        QFETCH(QStringList, command); QFETCH(QByteArray, input);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto recordPath = directory.filePath("record.json");
        auto rejected = probe(directory.filePath("vault"), command, input, "valid", recordPath);
        QVERIFY(rejected.finished); QVERIFY(rejected.exitCode != 0);
        QVERIFY(!QFileInfo::exists(recordPath));
        QVERIFY(!rejected.output.contains(secretA)); QVERIFY(!rejected.error.contains(secretA));
    }
    void cliUploadsAndSendsInSameCore() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        repaper::SecretStore vault(vaultPath); QVERIFY(vault.put(vaultEntry, credential()));
        const auto pdf = directory.filePath("fixture.pdf");
        QVERIFY(writeFile(pdf, "%PDF-1.4\nfixture-only\n%%EOF\n"));
        const auto recordPath = directory.filePath("record.json");
        const QByteArray input = QString("Question du cours\nDeuxième ligne").toUtf8();
        const auto sent = probe(vaultPath, {"send", "--model", "fixture-model", "--pdf", pdf}, input, "valid", recordPath);
        QVERIFY(sent.finished); QCOMPARE(sent.exitCode, 0);
        const auto record = readObject(recordPath);
        QCOMPARE(record.value("methods").toArray(), (QJsonArray{"auth.import", "files.upload", "chat.send"}));
        QVERIFY(record.value("hasUploadRoot").toBool()); QVERIFY(record.value("uploadHashMatches").toBool());
        QVERIFY(record.value("uploadMimeMatches").toBool()); QVERIFY(record.value("sendHadAttachment").toBool());
        QVERIFY(record.value("sendHadClientId").toBool());
        QCOMPARE(record.value("sendTextBytes").toInt(), input.size());
        QCOMPARE(record.value("sendModel").toString(), QString("fixture-model"));
        const auto lines = sent.output.trimmed().split('\n');
        QCOMPARE(lines.size(), 2);
        QCOMPARE(QJsonDocument::fromJson(lines.first()).object().value("method").toString(), QString("chat.progress"));
        QVERIFY(QJsonDocument::fromJson(lines.last()).object().value("result").isObject());
        QVERIFY(!sent.output.contains(secretA)); QVERIFY(!sent.error.contains(secretA));
    }
    void checkUsesOneCoreAndNeverRewritesVault_data() {
        QTest::addColumn<bool>("fromStdin");
        QTest::newRow("stored-credential") << false;
        QTest::newRow("stdin-session-without-saving") << true;
    }
    void checkUsesOneCoreAndNeverRewritesVault() {
        QFETCH(bool, fromStdin);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("vault");
        repaper::SecretStore vault(vaultPath); QVERIFY(vault.put(vaultEntry, credential()));
        const auto secretFile = QDir(vaultPath).entryList({"*.secret"}, QDir::Files).first();
        const auto previous = readFile(QDir(vaultPath).filePath(secretFile));
        const auto recordPath = directory.filePath("record.json");
        QStringList command{"check"};
        QByteArray input;
        if (fromStdin) {
            command << "--import-stdin";
            input = QJsonDocument(QJsonObject{{"accessToken", QString::fromLatin1(secretB)},
                {"user", QJsonObject{{"email", "private-mail@example.invalid"}}}}).toJson();
        }
        const auto checked = probe(vaultPath, command, input, "check", recordPath);
        QVERIFY(checked.finished); QCOMPARE(checked.exitCode, 0);
        QCOMPARE(checked.output.trimmed().split('\n').size(), 1);
        QVERIFY(checked.error.isEmpty());
        const auto top = QJsonDocument::fromJson(checked.output).object();
        QCOMPARE(top.keys(), QStringList{"check"});
        const auto result = top.value("check").toObject();
        QVERIFY(result.value("ok").toBool()); QVERIFY(result.value("readOnly").toBool());
        QVERIFY(result.value("conversationRead").toBool()); QVERIFY(!result.value("credentialPersisted").toBool());
        QCOMPARE(result.value("credentialSource").toString(), fromStdin ? QString("stdin") : QString("vault"));
        QCOMPARE(result.value("steps").toArray(), (QJsonArray{
            QJsonObject{{"step", "auth.import"}, {"ok", true}},
            QJsonObject{{"step", "models.list"}, {"ok", true}, {"count", 1}},
            QJsonObject{{"step", "conversations.list"}, {"ok", true}, {"count", 2}},
            QJsonObject{{"step", "conversations.get"}, {"ok", true}, {"count", 2}}}));
        const auto record = readObject(recordPath);
        QCOMPARE(record.value("methods").toArray(), (QJsonArray{"auth.import", "models.list", "conversations.list", "conversations.get"}));
        QCOMPARE(record.value("credentialWasCandidate").toBool(), fromStdin);
        QCOMPARE(record.value("listLimit").toInt(), 5); QCOMPARE(record.value("getLimit").toInt(), 5);
        QVERIFY(record.value("listFirstPage").toBool()); QVERIFY(record.value("getFirstPage").toBool());
        QVERIFY(record.value("readFirstConversation").toBool());
        QCOMPARE(readFile(QDir(vaultPath).filePath(secretFile)), previous);
        QCOMPARE(vault.get(vaultEntry), credential());
        for (const auto &marker : {secretA, secretB, QByteArray("private-"), QByteArray("fixture-model"), QByteArray("ui:")})
            QVERIFY(!checked.output.contains(marker));
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(qint64(record.value("pid").toDouble())), 2000);
    }
    void checkStdinDoesNotCreateVaultAndEmptyListSkipsRead() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto vaultPath = directory.filePath("never-created-vault");
        const auto recordPath = directory.filePath("record.json");
        const auto checked = probe(vaultPath, {"check", "--import-stdin"}, credential(), "check-empty", recordPath);
        QVERIFY(checked.finished); QCOMPARE(checked.exitCode, 0);
        QVERIFY(!QFileInfo::exists(vaultPath));
        const auto result = QJsonDocument::fromJson(checked.output).object().value("check").toObject();
        QVERIFY(result.value("ok").toBool()); QVERIFY(!result.value("conversationRead").toBool());
        QCOMPARE(result.value("steps").toArray().last().toObject(), (QJsonObject{{"step", "conversations.get"}, {"skipped", true}}));
        QCOMPARE(readObject(recordPath).value("methods").toArray(), (QJsonArray{"auth.import", "models.list", "conversations.list"}));
    }
    void checkStopsAtFirstRefusal_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QStringList>("expectedMethods");
        QTest::newRow("auth") << QString("check-refuse-auth") << QStringList{"auth.import"};
        QTest::newRow("models") << QString("check-refuse-models") << QStringList{"auth.import", "models.list"};
        QTest::newRow("list") << QString("check-refuse-list") << QStringList{"auth.import", "models.list", "conversations.list"};
        QTest::newRow("conversation") << QString("check-refuse-conversation") << QStringList{"auth.import", "models.list", "conversations.list", "conversations.get"};
    }
    void checkStopsAtFirstRefusal() {
        QFETCH(QString, mode); QFETCH(QStringList, expectedMethods);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto recordPath = directory.filePath("record.json");
        const auto checked = probe(directory.filePath("vault"), {"check", "--import-stdin"}, credential(), mode, recordPath);
        QVERIFY(checked.finished); QVERIFY(checked.exitCode != 0);
        QCOMPARE(checked.output.trimmed().split('\n').size(), 1); QVERIFY(checked.error.isEmpty());
        const auto result = QJsonDocument::fromJson(checked.output).object().value("check").toObject();
        QVERIFY(!result.value("ok").toBool());
        const auto steps = result.value("steps").toArray();
        QCOMPARE(steps.size(), expectedMethods.size());
        const auto failed = steps.last().toObject();
        QCOMPARE(failed.value("step").toString(), expectedMethods.last());
        QCOMPARE(failed.value("error").toObject(), (QJsonObject{{"kind", "UPSTREAM_FORBIDDEN"}, {"retryable", false},
            {"httpStatus", 403}, {"contentType", "application/json"}, {"challenge", false}}));
        QCOMPARE(readObject(recordPath).value("methods").toArray(), QJsonArray::fromStringList(expectedMethods));
        QVERIFY(!checked.output.contains(secretA)); QVERIFY(!checked.output.contains(secretB));
        QVERIFY(!checked.output.contains("private-")); QVERIFY(!checked.output.contains("headers"));
        QVERIFY(!QFileInfo::exists(directory.filePath("vault")));
    }
    void checkRejectsMalformedResults_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("stage");
        QTest::newRow("missing-models") << QString("check-models-missing") << QString("models.list");
        QTest::newRow("invalid-list") << QString("check-list-not-array") << QString("conversations.list");
        QTest::newRow("too-many-conversations") << QString("check-list-too-many") << QString("conversations.list");
        QTest::newRow("invalid-conversation-id") << QString("check-invalid-id") << QString("conversations.get");
        QTest::newRow("invalid-messages") << QString("check-messages-not-array") << QString("conversations.get");
        QTest::newRow("wrong-conversation") << QString("check-wrong-conversation") << QString("conversations.get");
    }
    void checkRejectsMalformedResults() {
        QFETCH(QString, mode); QFETCH(QString, stage);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto recordPath = directory.filePath("record.json");
        const auto checked = probe(directory.filePath("vault"), {"check", "--import-stdin"}, credential(), mode, recordPath);
        QVERIFY(checked.finished); QVERIFY(checked.exitCode != 0);
        const auto result = QJsonDocument::fromJson(checked.output).object().value("check").toObject();
        QVERIFY(!result.value("ok").toBool());
        const auto failed = result.value("steps").toArray().last().toObject();
        QCOMPARE(failed.value("step").toString(), stage);
        QCOMPARE(failed.value("error").toObject().value("kind").toString(), QString("IPC_PROTOCOL_ERROR"));
        const auto methods = readObject(recordPath).value("methods").toArray();
        if (mode == "check-invalid-id") QVERIFY(!methods.contains("conversations.get"));
        else QCOMPARE(methods.last().toString(), stage);
        QVERIFY(!checked.output.contains("private-")); QVERIFY(!checked.output.contains(secretA));
    }
    void checkDiscardsUntrustedDiagnostics() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto checked = probe(directory.filePath("vault"), {"check", "--import-stdin"}, credential(), "check-invalid-diagnostics");
        QVERIFY(checked.finished); QVERIFY(checked.exitCode != 0);
        const auto result = QJsonDocument::fromJson(checked.output).object().value("check").toObject();
        QCOMPARE(result.value("steps").toArray().first().toObject().value("error").toObject(),
            (QJsonObject{{"kind", "UPSTREAM_ERROR"}, {"retryable", false}}));
        QVERIFY(!checked.output.contains(secretA)); QVERIFY(!checked.output.contains(secretB));
    }
    void checkLocalErrorsHaveSafeFinalObject_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QStringList>("command");
        QTest::addColumn<QByteArray>("input"); QTest::addColumn<QString>("stage");
        QTest::newRow("invalid-stdin") << QString("valid") << QStringList{"check", "--import-stdin"} << secretA << QString("input");
        QTest::newRow("missing-vault") << QString("valid") << QStringList{"check"} << QByteArray{} << QString("vault");
        QTest::newRow("invalid-option") << QString("valid") << QStringList{"check", "--limit", "50"} << QByteArray{} << QString("input");
        QTest::newRow("unknown-option") << QString("valid") << QStringList{"check", "--not-a-real-option"} << QByteArray{} << QString("input");
        QTest::newRow("missing-option-value") << QString("valid") << QStringList{"check", "--core"} << QByteArray{} << QString("input");
        QTest::newRow("startup-failed") << QString("check-startup-fail") << QStringList{"check", "--import-stdin"} << credential() << QString("core");
        QTest::newRow("invalid-ipc") << QString("check-malformed-ipc") << QStringList{"check", "--import-stdin"} << credential() << QString("auth.import");
    }
    void checkLocalErrorsHaveSafeFinalObject() {
        QFETCH(QString, mode); QFETCH(QStringList, command); QFETCH(QByteArray, input); QFETCH(QString, stage);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto checked = probe(directory.filePath("vault"), command, input, mode);
        QVERIFY(checked.finished); QVERIFY(checked.exitCode != 0);
        QCOMPARE(checked.output.trimmed().split('\n').size(), 1); QVERIFY(checked.error.isEmpty());
        const auto top = QJsonDocument::fromJson(checked.output).object();
        QCOMPARE(top.keys(), QStringList{"check"});
        const auto result = top.value("check").toObject();
        QVERIFY(!result.value("ok").toBool());
        QCOMPARE(result.value("steps").toArray().first().toObject().value("step").toString(), stage);
        QVERIFY(!checked.output.contains(secretA)); QVERIFY(!checked.output.contains(secretB));
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--socket")) return fakeCore(app, app.arguments());
    ProbeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ProbeTest.moc"

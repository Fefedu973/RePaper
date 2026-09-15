#include "ChatController.h"
#include "Credential.h"
#include "SecretStore.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>
#ifdef RMCHAT_TEST_UI
#include "KeyboardController.h"
#include "LocalFiles.h"
#include "RichMessage.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QPainter>
#endif

namespace {
const QString entry = "chatgpt-web/credential-v1";
const QString privateSecret = "synthetic-controller-private-session";
const QString rotatedSecret = "synthetic-controller-rotated-session";
QByteArray credential(const QString &value = privateSecret, const QString &kind = "access_token") {
    return QJsonDocument(QJsonObject{{"version", 1}, {"provider", "chatgpt-web"}, {"kind", kind}, {"value", value}}).toJson(QJsonDocument::Compact);
}
bool writeFile(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
QByteArray readFile(const QString &path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QJsonObject record(const QString &path) { return QJsonDocument::fromJson(readFile(path)).object(); }
QJsonObject message(const QString &id, const QString &role, const QString &text) {
    return {{"id", id}, {"role", role}, {"text", text}, {"parentId", QJsonValue::Null}, {"attachments", QJsonArray{}}};
}
QJsonObject capabilities() {
    return {{"protocolVersion", 1}, {"provider", "chatgpt-web"}, {"credentialKinds", QJsonArray{"access_token", "session_token"}},
        {"methods", QJsonArray{"auth.import", "models.list", "conversations.list", "conversations.get", "chat.send", "files.upload", "request.cancel", "auth.logout"}}};
}
int fakeCore(QCoreApplication &app) {
    const auto args = app.arguments();
    const auto socketPath = args.value(args.indexOf("--socket") + 1);
    const auto uploadRoot = args.value(args.indexOf("--upload-root") + 1);
    const auto mode = qEnvironmentVariable("RMCHAT_CONTROLLER_FAKE_MODE");
    const auto recordPath = qEnvironmentVariable("RMCHAT_CONTROLLER_FAKE_RECORD");
    QJsonArray methods;
    QJsonObject metadata{{"pid", double(app.applicationPid())}, {"starts", record(recordPath).value("starts").toInt() + 1}};
    writeFile(recordPath, QJsonDocument(metadata).toJson());
    QLocalServer server;
    if (!server.listen(socketPath)) return 2;
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        auto *socket = server.nextPendingConnection();
        auto pending = std::make_shared<QByteArray>();
        QObject::connect(socket, &QLocalSocket::disconnected, &app, [&] {
            metadata.insert("stopped", true); writeFile(recordPath, QJsonDocument(metadata).toJson()); app.quit();
        });
        QObject::connect(socket, &QLocalSocket::readyRead, &app, [&, socket, pending] {
            *pending += socket->readAll();
            while (pending->contains('\n')) {
                const auto offset = pending->indexOf('\n');
                const auto request = QJsonDocument::fromJson(pending->left(offset)).object(); pending->remove(0, offset + 1);
                const auto method = request.value("method").toString();
                const auto params = request.value("params").toObject();
                const auto id = request.value("id");
                methods.append(method); metadata.insert("methods", methods);
                QJsonObject result;
                if (method == "auth.import") {
                    metadata.insert("persistSession", params.value("persistSession").toBool());
                    metadata.insert("candidateMatched", params.value("credential").toObject().value("value").toString() == privateSecret);
                    result = {{"authenticated", true}, {"account", QJsonObject{{"id", "fixture-account"}, {"displayName", "Compte fictif de test"}}}, {"capabilities", capabilities()},
                        {"privateExtra", privateSecret}};
                    if (mode == "rotation" || mode == "bad-rotation" || mode == "vault-fail") {
                        result.insert("credentialUpdate", mode == "bad-rotation" ? QJsonObject{{"accessToken", rotatedSecret}} : QJsonDocument::fromJson(credential(rotatedSecret, "session_token")).object());
                    }
                    if (mode == "vault-fail") {
                        const auto vault = qEnvironmentVariable("RMCHAT_CONTROLLER_FAKE_VAULT");
                        QDir().rename(vault, vault + ".held");
                        writeFile(vault, "blocked");
                    }
                } else if (method == "models.list") {
                    metadata.insert("modelReads", metadata.value("modelReads").toInt() + 1);
                    if (mode == "rotation") {
                        repaper::SecretStore vault(qEnvironmentVariable("RMCHAT_CONTROLLER_FAKE_VAULT"));
                        auto saved = vault.get(entry);
                        metadata.insert("rotationSavedBeforeModels", QJsonDocument::fromJson(saved).object().value("value").toString() == rotatedSecret);
                        saved.fill('\0');
                    }
                    result = {{"items", QJsonArray{QJsonObject{{"id", "real-service-model"}, {"name", "Modèle fictif"}, {"privateExtra", privateSecret}}}}, {"defaultModelId", "real-service-model"}};
                    if (mode == "models-empty") result.insert("items", QJsonArray{});
                    if (mode == "models-duplicates" && metadata.value("modelReads").toInt() == 1) {
                        result.insert("items", QJsonArray{
                            QJsonObject{{"id", "real-service-model"}, {"name", "Modèle fictif"}},
                            QJsonObject{{"id", "real-service-model"}, {"name", "Même modèle répété"}},
                            QJsonObject{{"id", "service.variant-v2"}, {"name", "Modèle fictif"}}});
                        result.insert("defaultModelId", "service.variant-v2");
                    }
                    if (mode == "malformed-models") result.insert("items", "bad");
                    if (mode == "unexpected-rotation") result.insert("credentialUpdate", QJsonDocument::fromJson(credential(rotatedSecret)).object());
                } else if (method == "conversations.list") {
                    const bool more = params.value("cursor").toString() == "list-page-2";
                    metadata.insert("listLimit", params.value("limit"));
                    metadata.insert("listUsedCursor", more);
                    result = {{"items", QJsonArray{QJsonObject{{"id", more ? "conversation-2" : "conversation-1"},
                        {"title", more ? "Deuxième conversation fictive" : "Conversation fictive"}, {"updatedAt", QJsonValue::Null}, {"privateExtra", privateSecret}}}},
                        {"nextCursor", more ? QJsonValue::Null : QJsonValue("list-page-2")}};
                } else if (method == "conversations.get") {
                    const bool more = params.value("cursor").toString() == "messages-page-2";
                    metadata.insert("getIdMatched", params.value("conversationId").toString() == "conversation-1");
                    metadata.insert("getUsedCursor", more);
                    result = {{"id", params.value("conversationId")}, {"title", "Conversation fictive"},
                        {"messages", QJsonArray{more ? message("answer-1", "assistant", "Réponse existante") : message("user-1", "user", "Message existant")}},
                        {"continuationParentId", "answer-1"}, {"nextCursor", more ? QJsonValue::Null : QJsonValue("messages-page-2")}};
                    if (mode == "rich") {
                        const auto richText = QString::fromUtf8("## Une réponse structurée\n\nDu **gras**, de l’*italique* et une formule $E=mc^2$.\n\n"
                            "$$\\frac{1}{\\sqrt{2\\pi}}\\int_{-\\infty}^{+\\infty} e^{-x^2/2}\\,dx=1$$\n\n"
                            "| Élément | Valeur |\n|---|---|\n| Exemple | 42 |\n\n"
                            "> Une citation\n\n- Une liste\n- Un second élément\n\n```cpp\nauto valeur = 42; // $ reste dans le code\n```\n\n")
                            + QString("Un paragraphe assez long pour vérifier le retour à la ligne et le défilement de la conversation.\n\n").repeated(90);
                        result.insert("messages", QJsonArray{message("user-1", "user", "Présente un exemple de Markdown et de LaTeX."), message("answer-1", "assistant", richText)});
                        result.insert("nextCursor", QJsonValue::Null);
                    }
                } else if (method == "files.upload") {
                    const auto path = params.value("path").toString(); const auto content = readFile(path);
                    metadata.insert("uploadUnderRoot", QFileInfo(path).absolutePath() == uploadRoot);
                    metadata.insert("uploadSnapshotUnchanged", content == "%PDF-1.4\nOriginal snapshot\n%%EOF\n");
                    metadata.insert("uploadHashMatched", params.value("sha256").toString() == QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()));
                    result = {{"attachmentRef", "opaque-upload-fixture"}};
                } else if (method == "chat.send") {
                    metadata.insert("sendModelId", params.value("modelId"));
                    metadata.insert("sendUsesRealModel", params.value("modelId").toString() == "real-service-model");
                    metadata.insert("sendParentMatched", params.value("parentMessageId").toString() == "answer-1");
                    metadata.insert("sendAttachmentMatched", params.value("attachments").toArray().contains("opaque-upload-fixture"));
                    metadata.insert("sendTextMatched", params.value("text").toString() == "Mon brouillon");
                    const auto emitProgress = [socket, id](int sequence, const QString &text) {
                        socket->write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", "chat.progress"}, {"params", QJsonObject{
                            {"requestId", id}, {"sequence", sequence}, {"text", text}, {"conversationId", "conversation-1"}, {"privateExtra", privateSecret}}}}).toJson(QJsonDocument::Compact) + '\n'); socket->flush();
                    };
                    if (!mode.startsWith("send-http403")) {
                        emitProgress(1, "Réponse");
                        QTimer::singleShot(70, socket, [emitProgress] { emitProgress(2, "Réponse en direct"); });
                    }
                    result = {{"conversationId", "conversation-1"}, {"message", message("answer-2", "assistant", "Réponse finale — données fictives de test")}, {"continuationParentId", "answer-2"}};
                }
                writeFile(recordPath, QJsonDocument(metadata).toJson());
                if (mode == "hang" && method == "chat.send") continue;
                QJsonObject response{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
                const bool refuse = (mode == "refuse" && method == "auth.import") || (mode == "offline" && method == "models.list") ||
                    (mode == "send-refuse" && method == "chat.send") || (mode == "page-refuse" && method == "conversations.get" && !params.value("cursor").toString().isEmpty());
                if (refuse) {
                    response.remove("result"); response.insert("error", QJsonObject{{"code", -32000}, {"message", privateSecret},
                        {"data", QJsonObject{{"kind", mode == "offline" || mode == "page-refuse" ? "NETWORK_ERROR" : "AUTH_INVALID"}, {"retryable", false}, {"credentialUpdate", privateSecret}}}});
                }
                if (mode.startsWith("send-http403") && method == "chat.send") {
                    response.remove("result");
                    response.insert("error", QJsonObject{{"code", -32000}, {"message", privateSecret},
                        {"data", QJsonObject{{"kind", "UPSTREAM_FORBIDDEN"}, {"httpStatus", 403},
                            {"stage", mode == "send-http403-untrusted" ? privateSecret : QString("prepare")},
                            {"headers", privateSecret}, {"credentialUpdate", privateSecret}}}});
                }
                const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
                if (method == "chat.send" || (method == "auth.import" && mode == "slow-auth")) {
                    QTimer::singleShot(160, socket, [socket, bytes] { socket->write(bytes); socket->flush(); });
                } else { socket->write(bytes); socket->flush(); }
            }
        });
    });
    return app.exec();
}
class Fixture {
public:
    QTemporaryDir temp;
    QString vault = temp.filePath("vault"), input = temp.filePath("session.json"), log = temp.filePath("metadata.json");
    Fixture(const QString &mode = {}, const QString &kind = "access_token") {
        qputenv("RMCHAT_CONTROLLER_FAKE_MODE", mode.toUtf8());
        qputenv("RMCHAT_CONTROLLER_FAKE_RECORD", log.toUtf8());
        qputenv("RMCHAT_CONTROLLER_FAKE_VAULT", vault.toUtf8());
        writeFile(input, credential(privateSecret, kind));
    }
    ~Fixture() { qunsetenv("RMCHAT_CONTROLLER_FAKE_MODE"); qunsetenv("RMCHAT_CONTROLLER_FAKE_RECORD"); qunsetenv("RMCHAT_CONTROLLER_FAKE_VAULT"); }
    QString core() const { return QCoreApplication::applicationFilePath(); }
    QByteArray publicState(const rmchat::ChatController &controller) const {
        QJsonObject state;
        const auto *meta = controller.metaObject();
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const auto property = meta->property(i); state.insert(property.name(), QJsonValue::fromVariant(property.read(&controller)));
        }
        return QJsonDocument(state).toJson();
    }
};
#ifdef RMCHAT_TEST_UI
bool saveWindow(QQuickWindow *window, const QString &path) {
    auto rendered = window->grabWindow();
    if (rendered.isNull()) {
        const auto grab = window->contentItem()->grabToImage();
        if (!grab) return false;
        QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
        if (!ready.wait(2000) || grab->image().isNull()) return false;
        rendered = grab->image();
    }
    auto background = window->property("color").value<QColor>();
    if (!background.isValid() || background.alpha() != 255) background = QColor("#f8f7f3");
    QImage opaque(rendered.size(), QImage::Format_ARGB32_Premultiplied); opaque.fill(background);
    QPainter painter(&opaque); painter.drawImage(0, 0, rendered);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver); painter.fillRect(opaque.rect(), background); painter.end();
    return opaque.pixelColor(0, 0).alpha() == 255 && opaque.save(path);
}
#endif
}

class ChatControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void startupIsLocalAndPreviewNeverTouchesVault() {
        Fixture f;
        rmchat::ChatController controller(f.core(), f.vault);
        QTRY_VERIFY(!controller.busy());
        QVERIFY(!controller.connected()); QVERIFY(!controller.credentialStored()); QVERIFY(!QFileInfo::exists(f.log));
        controller.setDraft("Un brouillon local");
        QCOMPARE(controller.draft(), QString("Un brouillon local"));
        const auto previewVault = f.temp.filePath("preview-vault");
        rmchat::ChatController preview(f.core(), previewVault, true);
        QTRY_VERIFY(!preview.busy());
        preview.importSessionFile(QUrl::fromLocalFile(f.input)); preview.connectSaved(); preview.logout();
        QVERIFY(!QFileInfo::exists(previewVault)); QVERIFY(!QFileInfo::exists(f.log));
    }
    void importUsesOneCoreAndRealDataWithPagination() {
        Fixture f;
        rmchat::ChatController c(f.core(), f.vault);
        QTRY_VERIFY(!c.busy()); c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QVERIFY(c.connected()); QVERIFY(c.credentialStored());
        QCOMPARE(c.accountLabel(), QString("Compte fictif de test"));
        QCOMPARE(c.models().size(), 1); QCOMPARE(c.selectedModelId(), QString("real-service-model"));
        QCOMPARE(c.conversations().size(), 1); QVERIFY(c.hasMoreConversations());
        c.loadMoreConversations(); QTRY_VERIFY(!c.busy()); QCOMPARE(c.conversations().size(), 2); QVERIFY(!c.hasMoreConversations());
        c.openConversation("conversation-1"); QTRY_VERIFY(!c.busy()); QCOMPARE(c.messages().size(), 1); QVERIFY(c.hasMoreMessages());
        c.loadMoreMessages(); QTRY_VERIFY(!c.busy()); QCOMPARE(c.messages().size(), 2); QVERIFY(!c.hasMoreMessages());
        QCOMPARE(record(f.log).value("starts").toInt(), 1);
        QCOMPARE(record(f.log).value("methods").toArray(), (QJsonArray{"auth.import", "models.list", "conversations.list", "conversations.list", "conversations.get", "conversations.get"}));
        QVERIFY(record(f.log).value("persistSession").toBool()); QVERIFY(record(f.log).value("candidateMatched").toBool());
        QVERIFY(record(f.log).value("getIdMatched").toBool()); QVERIFY(record(f.log).value("getUsedCursor").toBool());
        QVERIFY(!f.publicState(c).contains(privateSecret.toUtf8()));
    }
    void streamingAndPdfUsePersistentCoreAndSnapshot() {
        Fixture f;
        rmchat::ChatController c(f.core(), f.vault);
        QTRY_VERIFY(!c.busy()); c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        c.openConversation("conversation-1"); QTRY_VERIFY(!c.busy());
        const auto pdf = f.temp.filePath("document.pdf");
        QVERIFY(writeFile(pdf, "%PDF-1.4\nOriginal snapshot\n%%EOF\n"));
        c.addPdf(QUrl::fromLocalFile(pdf)); QTRY_VERIFY(!c.busy()); QCOMPARE(c.attachments().size(), 1);
        QVERIFY(writeFile(pdf, "%PDF-1.4\nChanged source after selection\n%%EOF\n"));
        QStringList snapshots;
        connect(&c, &rmchat::ChatController::changed, &c, [&] { if (!c.streamingText().isEmpty()) snapshots.append(c.streamingText()); });
        c.setDraft("Mon brouillon"); QVERIFY(c.canSend()); c.send(); QVERIFY(c.busy());
        QTRY_VERIFY(!c.busy()); QVERIFY(snapshots.contains("Réponse")); QVERIFY(snapshots.contains("Réponse en direct"));
        QCOMPARE(c.messages().last().toMap().value("text").toString(), QString("Réponse finale — données fictives de test"));
        QVERIFY(c.draft().isEmpty()); QVERIFY(c.attachments().isEmpty()); QVERIFY(c.streamingText().isEmpty());
        const auto metadata = record(f.log);
        QCOMPARE(metadata.value("starts").toInt(), 1);
        for (const auto &field : {"uploadUnderRoot", "uploadSnapshotUnchanged", "uploadHashMatched", "sendUsesRealModel", "sendParentMatched", "sendAttachmentMatched", "sendTextMatched"}) QVERIFY2(metadata.value(field).toBool(), field);
        QVERIFY(!f.publicState(c).contains(privateSecret.toUtf8()));
    }
    void sendingFinishesPaginationBeforeAdvancingBranch() {
        Fixture f; rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        c.openConversation("conversation-1"); QTRY_VERIFY(!c.busy()); QVERIFY(c.hasMoreMessages()); QCOMPARE(c.messages().size(), 1);
        c.setDraft("Mon brouillon"); c.send(); QTRY_VERIFY(!c.busy());
        QVERIFY(!c.hasMoreMessages()); QCOMPARE(c.messages().size(), 4);
        QCOMPARE(c.messages().at(0).toMap().value("text").toString(), QString("Message existant"));
        QCOMPARE(c.messages().at(1).toMap().value("text").toString(), QString("Réponse existante"));
        QCOMPARE(c.messages().at(2).toMap().value("text").toString(), QString("Mon brouillon"));
        QCOMPARE(c.messages().at(3).toMap().value("text").toString(), QString("Réponse finale — données fictives de test"));
        QCOMPARE(record(f.log).value("methods").toArray(), (QJsonArray{"auth.import", "models.list", "conversations.list", "conversations.get", "conversations.get", "chat.send"}));
        const auto before = record(f.log).value("methods"); c.loadMoreMessages(); QCOMPARE(record(f.log).value("methods"), before);
    }
    void failedPaginationPreventsSendAndPreservesDraft() {
        Fixture f("page-refuse"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        c.openConversation("conversation-1"); QTRY_VERIFY(!c.busy());
        c.setDraft("Mon brouillon"); c.send(); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.errorKind(), QString("NETWORK_ERROR")); QCOMPARE(c.draft(), QString("Mon brouillon")); QCOMPARE(c.messages().size(), 1);
        QVERIFY(!record(f.log).value("methods").toArray().contains("chat.send"));
    }
    void rotationIsPersistedBeforeModelsAndNeverPublished() {
        Fixture f("rotation", "session_token");
        rmchat::ChatController c(f.core(), f.vault);
        QByteArray snapshots;
        connect(&c, &rmchat::ChatController::changed, &c, [&] { snapshots += f.publicState(c); });
        QTRY_VERIFY(!c.busy()); c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QVERIFY(c.connected()); QVERIFY(record(f.log).value("rotationSavedBeforeModels").toBool());
        QVERIFY(!snapshots.contains(privateSecret.toUtf8())); QVERIFY(!snapshots.contains(rotatedSecret.toUtf8())); QVERIFY(!snapshots.contains("credentialUpdate"));
        repaper::SecretStore vault(f.vault); auto saved = vault.get(entry);
        QCOMPARE(QJsonDocument::fromJson(saved).object().value("value").toString(), rotatedSecret); saved.fill('\0');
    }
    void rejectedAuthenticationPreservesDraftAndOldCredential() {
        Fixture f("refuse"); repaper::SecretStore vault(f.vault); QVERIFY(vault.put(entry, credential("old-synthetic-secret")));
        const auto secretFiles = QDir(f.vault).entryList({"*.secret"}, QDir::Files); QCOMPARE(secretFiles.size(), 1);
        const auto encrypted = readFile(f.vault + "/" + secretFiles.first());
        rmchat::ChatController c(f.core(), f.vault);
        QTRY_VERIFY(!c.busy()); c.setDraft("Ne pas perdre ce brouillon");
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QVERIFY(!c.connected()); QCOMPARE(c.errorKind(), QString("AUTH_INVALID")); QCOMPARE(c.draft(), QString("Ne pas perdre ce brouillon"));
        QCOMPARE(readFile(f.vault + "/" + secretFiles.first()), encrypted);
        QCOMPARE(record(f.log).value("methods").toArray(), (QJsonArray{"auth.import"})); QVERIFY(!f.publicState(c).contains(privateSecret.toUtf8()));
    }
    void sessionTokenRequiresCredentialUpdate() {
        Fixture f({}, "session_token"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QVERIFY(!c.connected()); QVERIFY(!c.credentialStored()); QCOMPARE(c.errorKind(), QString("SESSION_PERSISTENCE_REQUIRED"));
        QCOMPARE(record(f.log).value("methods").toArray(), (QJsonArray{"auth.import"}));
    }
    void refusedSendKeepsDraftAndPdf() {
        Fixture f("send-refuse"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        const auto pdf = f.temp.filePath("document.pdf"); QVERIFY(writeFile(pdf, "%PDF-1.4\nOriginal snapshot\n%%EOF\n"));
        c.addPdf(QUrl::fromLocalFile(pdf)); QTRY_VERIFY(!c.busy()); c.setDraft("Mon brouillon"); c.send(); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.errorKind(), QString("AUTH_INVALID")); QCOMPARE(c.draft(), QString("Mon brouillon")); QCOMPARE(c.attachments().size(), 1);
        QVERIFY(c.messages().isEmpty()); QVERIFY(!c.connected()); QVERIFY(!f.publicState(c).contains(privateSecret.toUtf8()));
    }
    void availableModelsAreUniqueAndSelectionFollowsExplicitRefresh() {
        Fixture f("models-duplicates"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.models().size(), 2); QCOMPARE(c.selectedModelId(), QString("service.variant-v2"));
        QVERIFY(c.models().at(0).toMap().value("name") != c.models().at(1).toMap().value("name"));
        QVERIFY(c.models().at(1).toMap().value("name").toString().contains("service.variant-v2"));
        c.setDraft("Mon brouillon"); QVERIFY(c.canSend()); c.send(); QTRY_VERIFY(!c.busy());
        QCOMPARE(record(f.log).value("sendModelId").toString(), QString("service.variant-v2"));
        QCOMPARE(c.selectedModelId(), QString("service.variant-v2"));
        c.setDraft("Brouillon suivant"); c.refreshModels(); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.models().size(), 1); QCOMPARE(c.selectedModelId(), QString("real-service-model"));
        QCOMPARE(c.draft(), QString("Brouillon suivant")); QVERIFY(c.canSend());
    }
    void emptyModelCatalogKeepsHistoryButPreventsSending() {
        Fixture f("models-empty"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QVERIFY(c.connected()); QVERIFY(!c.conversations().isEmpty()); QVERIFY(c.models().isEmpty());
        QVERIFY(c.selectedModelId().isEmpty()); QVERIFY(!c.modelStatus().isEmpty());
        c.setDraft("Un brouillon conservé"); QVERIFY(!c.canSend()); c.send();
        QVERIFY(!record(f.log).value("methods").toArray().contains("chat.send"));
    }
    void forbiddenSendShowsSafeStageAndNeverResends_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("knownStage");
        QTest::newRow("known-stage") << QString("send-http403") << true;
        QTest::newRow("untrusted-stage") << QString("send-http403-untrusted") << false;
    }
    void forbiddenSendShowsSafeStageAndNeverResends() {
        QFETCH(QString, mode); QFETCH(bool, knownStage); Fixture f(mode);
        rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        c.setDraft("Mon brouillon"); c.send(); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.errorKind(), QString("UPSTREAM_FORBIDDEN")); QVERIFY(c.errorDetails().contains("HTTP 403"));
        QCOMPARE(c.errorDetails().contains("Préparation"), knownStage);
        QCOMPARE(c.draft(), QString("Mon brouillon")); QVERIFY(c.messages().isEmpty()); QVERIFY(c.streamingText().isEmpty());
        QVERIFY(c.connected()); QVERIFY(!f.publicState(c).contains(privateSecret.toUtf8()));
        const auto calls = record(f.log).value("methods").toArray();
        QCOMPARE(calls, (QJsonArray{"auth.import", "models.list", "conversations.list", "chat.send"}));
        QTest::qWait(100); QCOMPARE(record(f.log).value("methods").toArray(), calls);
    }
    void failuresAreSafeAndDoNotInventData_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("expected");
        QTest::newRow("offline") << QString("offline") << QString("NETWORK_ERROR");
        QTest::newRow("bad-models") << QString("malformed-models") << QString("IPC_PROTOCOL_ERROR");
        QTest::newRow("unexpected-rotation") << QString("unexpected-rotation") << QString("IPC_PROTOCOL_ERROR");
        QTest::newRow("bad-rotation") << QString("bad-rotation") << QString("INVALID_CREDENTIAL");
        QTest::newRow("access-to-session-rotation") << QString("rotation") << QString("INVALID_CREDENTIAL");
    }
    void failuresAreSafeAndDoNotInventData() {
        QFETCH(QString, mode); QFETCH(QString, expected); Fixture f(mode);
        rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy()); c.setDraft("Ne pas perdre ce brouillon");
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.errorKind(), expected); QVERIFY(c.models().isEmpty()); QVERIFY(c.conversations().isEmpty()); QVERIFY(c.messages().isEmpty());
        QCOMPARE(c.draft(), QString("Ne pas perdre ce brouillon")); QVERIFY(!f.publicState(c).contains(privateSecret.toUtf8())); QVERIFY(!f.publicState(c).contains(rotatedSecret.toUtf8()));
    }
    void vaultFailureStopsBeforeModelsAndPreservesOldVault() {
        Fixture f("vault-fail", "session_token");
        repaper::SecretStore before(f.vault); QVERIFY(before.put(entry, credential("old-synthetic-secret")));
        rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy()); c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QVERIFY(!c.connected()); QCOMPARE(c.errorKind(), QString("VAULT_ERROR"));
        QCOMPARE(record(f.log).value("methods").toArray(), (QJsonArray{"auth.import"}));
        repaper::SecretStore held(f.vault + ".held"); auto saved = held.get(entry);
        QCOMPARE(QJsonDocument::fromJson(saved).object().value("value").toString(), QString("old-synthetic-secret")); saved.fill('\0');
        QVERIFY(QFile::remove(f.vault)); QVERIFY(QDir().rename(f.vault + ".held", f.vault));
    }
    void guiRemainsResponsiveDuringAuthentication() {
        Fixture f("slow-auth"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        bool timerWhileBusy = false;
        c.importSessionFile(QUrl::fromLocalFile(f.input));
        QTimer::singleShot(40, &c, [&] { timerWhileBusy = c.busy(); });
        QTRY_VERIFY(!c.busy()); QVERIFY(timerWhileBusy); QVERIFY(c.connected());
    }
    void successfulAccountImportClearsAttachmentsButKeepsDraft() {
        Fixture f; rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        const auto pdf = f.temp.filePath("document.pdf"); QVERIFY(writeFile(pdf, "%PDF-1.4\nOriginal snapshot\n%%EOF\n"));
        c.addPdf(QUrl::fromLocalFile(pdf)); QTRY_VERIFY(!c.busy()); QCOMPARE(c.attachments().size(), 1);
        c.setDraft("Brouillon conservé"); c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QVERIFY(c.connected()); QVERIFY(c.attachments().isEmpty()); QCOMPARE(c.draft(), QString("Brouillon conservé"));
    }
    void editedDraftIsPreservedAfterEarlierSendFinishes() {
        Fixture f; rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy()); c.setDraft("Mon brouillon"); c.send();
        c.setDraft("Brouillon suivant"); QTRY_VERIFY(!c.busy()); QCOMPARE(c.draft(), QString("Brouillon suivant"));
    }
    void cancellationAndLogoutAreLocalAndKeepDraft() {
        Fixture f("hang"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy()); c.setDraft("Mon brouillon"); c.send();
        QTRY_VERIFY(!c.streamingText().isEmpty()); c.cancel(); QTRY_VERIFY_WITH_TIMEOUT(!c.busy(), 5000);
        QCOMPARE(c.errorKind(), QString("CANCELLED")); QCOMPARE(c.draft(), QString("Mon brouillon"));
        c.logout(); QTRY_VERIFY(!c.busy()); QVERIFY(!c.connected()); QVERIFY(!c.credentialStored()); QVERIFY(c.messages().isEmpty());
        repaper::SecretStore vault(f.vault); QVERIFY(vault.get(entry).isEmpty());
    }
    void logoutWhileBusyWaitsForCoreAndRemovesCredential() {
        Fixture f("hang"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy()); c.setDraft("Mon brouillon"); c.send();
        QTRY_VERIFY(!c.streamingText().isEmpty()); c.logout(); QTRY_VERIFY_WITH_TIMEOUT(!c.busy(), 5000);
        QVERIFY(!c.connected()); QVERIFY(!c.credentialStored()); QVERIFY(c.streamingText().isEmpty());
        repaper::SecretStore vault(f.vault); QVERIFY(vault.get(entry).isEmpty());
    }
    void closingStopsChildAndReleasesVault() {
        Fixture f;
        {
            rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
            c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy()); QVERIFY(c.connected());
        }
        QTRY_VERIFY(record(f.log).value("stopped").toBool());
        QLockFile lock(f.vault + "/probe.lock"); QVERIFY(lock.tryLock(0));
    }
    void savedCredentialRequiresExplicitConnectionAndLockIsShared() {
        Fixture f; repaper::SecretStore vault(f.vault); QVERIFY(vault.put(entry, credential()));
        rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy()); QVERIFY(c.credentialStored()); QVERIFY(!c.connected()); QVERIFY(!QFileInfo::exists(f.log));
        QLockFile competing(f.vault + "/probe.lock"); QVERIFY(!competing.tryLock(0));
        c.connectSaved(); QTRY_VERIFY(!c.busy()); QVERIFY(c.connected());
    }
    void invalidInputNeverStartsCore_data() {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("not-json") << QByteArray("invalid");
        QTest::newRow("oversize") << QByteArray(32769, 'x');
        QTest::newRow("wrong-envelope") << QByteArray("{\"version\":2,\"provider\":\"chatgpt-web\",\"kind\":\"access_token\",\"value\":\"x\"}");
    }
    void invalidInputNeverStartsCore() {
        QFETCH(QByteArray, input); Fixture f; QVERIFY(writeFile(f.input, input));
        rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy()); c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.errorKind(), QString("INVALID_CREDENTIAL")); QVERIFY(!QFileInfo::exists(f.log));
    }
    void unavailableCoreFailsWithoutFakeData() {
        Fixture f; rmchat::ChatController c(f.temp.filePath("no-core"), f.vault); QTRY_VERIFY(!c.busy());
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy()); QCOMPARE(c.errorKind(), QString("CORE_UNAVAILABLE"));
        QVERIFY(!c.connected()); QVERIFY(c.models().isEmpty()); QVERIFY(c.conversations().isEmpty());
    }
    void pdfCountAndHeaderAreBounded() {
        Fixture f; rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        const auto pdf = f.temp.filePath("document.pdf"); QVERIFY(writeFile(pdf, "Not PDF"));
        c.addPdf(QUrl::fromLocalFile(pdf)); QTRY_VERIFY(!c.busy()); QCOMPARE(c.errorKind(), QString("FILE_REJECTED")); QVERIFY(c.attachments().isEmpty());
        QVERIFY(writeFile(pdf, "%PDF-1.4\nOriginal snapshot\n%%EOF\n"));
        for (int i = 0; i < 4; ++i) { c.addPdf(QUrl::fromLocalFile(pdf)); QTRY_VERIFY(!c.busy()); }
        QCOMPARE(c.attachments().size(), 4); c.addPdf(QUrl::fromLocalFile(pdf)); QTRY_VERIFY(!c.busy());
        QCOMPARE(c.attachments().size(), 4); QCOMPARE(c.errorKind(), QString("FILE_REJECTED"));
        c.removeAttachment(c.attachments().first().toMap().value("id").toString()); QTRY_VERIFY(!c.busy()); QCOMPARE(c.attachments().size(), 3);
        QVERIFY(!QFileInfo::exists(f.log));
    }
#ifdef RMCHAT_TEST_UI
    void nativeRichConversationWrapsWithoutLayoutLoops() {
        Fixture f("rich"); rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        rmchat::LocalFiles files;
        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) { warnings.append(errors); });
        engine.rootContext()->setContextProperty("chat", &c);
        engine.rootContext()->setContextProperty("localFiles", &files);
        engine.rootContext()->setContextProperty("previewPage", QString());
        engine.load(QUrl("qrc:/rmchat/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy());
        c.openConversation("conversation-1"); QTRY_VERIFY(!c.busy());
        auto richItems = [window] {
            QList<rmchat::RichMessage *> found;
            std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
                if (auto *rich = qobject_cast<rmchat::RichMessage *>(item); rich && !rich->text().isEmpty()) found.append(rich);
                for (auto *child : item->childItems()) visit(child);
            };
            visit(window->contentItem()); return found;
        };
        QTRY_COMPARE(richItems().size(), 2);
        auto *answer = richItems().last();
        QTRY_VERIFY(answer->hasMath()); QTRY_COMPARE(answer->mathErrorCount(), 0);
        QTRY_VERIFY(answer->contentHeight() > 1000);
        QVERIFY(answer->width() < window->width());
        const auto screenshot = qEnvironmentVariable("RMCHAT_RICH_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(saveWindow(window, screenshot));
        window->resize(760, 1020); window->setProperty("showHistory", false);
        QTest::qWait(200);
        for (auto *rich : richItems()) {
            QVERIFY(rich->width() > 0 && rich->width() < window->width());
            QCOMPARE(rich->height(), rich->contentHeight());
        }
        if (!screenshot.isEmpty()) QVERIFY(saveWindow(window, QFileInfo(screenshot).absolutePath() + "/rich-narrow.png"));
        QStringList details; for (const auto &warning : warnings) details.append(warning.toString());
        QVERIFY2(warnings.isEmpty(), qPrintable(details.join('\n')));
    }
    void nativeQmlUsesControllerAndStreamsWithoutWarnings() {
        Fixture f; rmchat::ChatController c(f.core(), f.vault); QTRY_VERIFY(!c.busy());
        rmchat::LocalFiles files; files.openFolder(QUrl::fromLocalFile(f.temp.path()));
        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &errors) { warnings.append(errors); });
        engine.rootContext()->setContextProperty("chat", &c);
        engine.rootContext()->setContextProperty("localFiles", &files);
        engine.rootContext()->setContextProperty("previewPage", QString());
        engine.load(QUrl("qrc:/rmchat/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        c.importSessionFile(QUrl::fromLocalFile(f.input)); QTRY_VERIFY(!c.busy()); QVERIFY(c.connected());
        c.openConversation("conversation-1"); QTRY_VERIFY(!c.busy());
        auto *composer = window->findChild<QQuickItem *>("composer"); QVERIFY(composer);
        QVERIFY(composer->setProperty("text", "Mon brouillon")); QTRY_COMPARE(c.draft(), QString("Mon brouillon"));
        auto *send = window->findChild<QQuickItem *>("sendButton"); QVERIFY(send); QTRY_VERIFY(send->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(send, "clicked")); QTRY_VERIFY(c.busy()); QTRY_VERIFY(!c.busy());
        QTRY_COMPARE(c.messages().last().toMap().value("text").toString(), QString("Réponse finale — données fictives de test"));
        QTRY_COMPARE(composer->property("text").toString(), QString());
        QTest::qWait(100);
        const auto screenshot = qEnvironmentVariable("RMCHAT_CONTROLLER_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY2(window->isVisible(), "Native window is hidden");
            QVERIFY2(window->isExposed(), "Native window is not exposed");
            QVERIFY(saveWindow(window, screenshot));
        }
        window->requestActivate(); composer->forceActiveFocus();
        auto *keyboard = window->findChild<repaper::KeyboardController *>("textKeyboard"); QVERIFY(keyboard);
        QTRY_VERIFY(keyboard->fallbackVisible());
        keyboard->insertText("Texte fictif saisi au clavier"); QTRY_COMPARE(c.draft(), QString("Texte fictif saisi au clavier"));
        if (!screenshot.isEmpty()) QVERIFY(saveWindow(window, QFileInfo(screenshot).absolutePath() + "/keyboard.png"));
        keyboard->dismiss(); QTRY_VERIFY(!keyboard->fallbackVisible());
        QObject *addPdf = nullptr;
        for (auto *object : window->findChildren<QObject *>()) if (object->property("text").toString() == "+ PDF" && object->metaObject()->indexOfSignal("clicked()") >= 0) { addPdf = object; break; }
        QVERIFY(addPdf); QVERIFY(QMetaObject::invokeMethod(addPdf, "clicked"));
        const auto folder = f.temp.filePath("fichiers-fictifs"); QVERIFY(QDir().mkpath(folder));
        QVERIFY(writeFile(folder + "/exemple-fictif.pdf", "%PDF-1.4\nOriginal snapshot\n%%EOF\n"));
        files.openFolder(QUrl::fromLocalFile(folder)); QCOMPARE(files.mode(), QString("pdf")); QTRY_COMPARE(files.entries().size(), 1);
        auto *picker = window->findChild<QObject *>("loginDialog"); QVERIFY(picker); QTRY_VERIFY(picker->property("opened").toBool());
        if (!screenshot.isEmpty()) QVERIFY(saveWindow(window, QFileInfo(screenshot).absolutePath() + "/picker.png"));
        QVERIFY(QMetaObject::invokeMethod(picker, "close"));
        QStringList details; for (const auto &warning : warnings) details.append(warning.toString());
        QVERIFY2(warnings.isEmpty(), qPrintable(details.join('\n')));
    }
#endif
};
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) if (QByteArray(argv[i]) == "--socket") {
        QCoreApplication app(argc, argv); return fakeCore(app);
    }
#ifdef RMCHAT_TEST_UI
    QGuiApplication app(argc, argv); QQuickStyle::setStyle("Basic"); repaper::registerKeyboardTypes(); rmchat::registerRichTypes();
#else
    QCoreApplication app(argc, argv);
#endif
    app.setOrganizationName("RePaperTests"); app.setApplicationName("rmchat-controller-tests");
    ChatControllerTest test; return QTest::qExec(&test, argc, argv);
}
#include "ChatControllerTest.moc"

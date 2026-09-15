#pragma once
#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>

namespace rmchat {
class ChatWorker;
// Only display data crosses this object's QML boundary. Credentials stay in ChatWorker.
class ChatController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool credentialStored READ credentialStored NOTIFY changed)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY changed)
    Q_PROPERTY(QString message READ statusMessage NOTIFY changed)
    Q_PROPERTY(QString errorKind READ errorKind NOTIFY changed)
    Q_PROPERTY(QString errorDetails READ errorDetails NOTIFY changed)
    Q_PROPERTY(QString modelStatus READ modelStatus NOTIFY changed)
    Q_PROPERTY(QString accountLabel READ accountLabel NOTIFY changed)
    Q_PROPERTY(QVariantList models READ models NOTIFY changed)
    Q_PROPERTY(QString selectedModelId READ selectedModelId WRITE setSelectedModelId NOTIFY changed)
    Q_PROPERTY(QVariantList conversations READ conversations NOTIFY changed)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY changed)
    Q_PROPERTY(QString conversationId READ conversationId NOTIFY changed)
    Q_PROPERTY(QString conversationTitle READ conversationTitle NOTIFY changed)
    Q_PROPERTY(QString draft READ draft WRITE setDraft NOTIFY draftChanged)
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY changed)
    Q_PROPERTY(bool hasMoreConversations READ hasMoreConversations NOTIFY changed)
    Q_PROPERTY(bool hasMoreMessages READ hasMoreMessages NOTIFY changed)
    Q_PROPERTY(QString streamingText READ streamingText NOTIFY changed)
    Q_PROPERTY(bool canSend READ canSend NOTIFY changed)
    Q_PROPERTY(bool readOnlyPreview READ readOnlyPreview CONSTANT)
public:
    explicit ChatController(QString corePath = {}, QString vaultDirectory = {},
                            bool readOnlyPreview = false, QObject *parent = nullptr);
    ~ChatController() override;
    bool connected() const { return m_state.value("connected").toBool(); }
    bool busy() const { return m_busy; }
    bool credentialStored() const { return m_state.value("credentialStored").toBool(); }
    QString statusMessage() const { return m_state.value("statusMessage").toString(); }
    QString errorKind() const { return m_state.value("errorKind").toString(); }
    QString errorDetails() const { return m_state.value("errorDetails").toString(); }
    QString modelStatus() const { return m_state.value("modelStatus").toString(); }
    QString accountLabel() const { return m_state.value("accountLabel").toString(); }
    QVariantList models() const { return m_state.value("models").toList(); }
    QString selectedModelId() const { return m_selectedModel; }
    void setSelectedModelId(const QString &id);
    QVariantList conversations() const { return m_state.value("conversations").toList(); }
    QVariantList messages() const { return m_state.value("messages").toList(); }
    QString conversationId() const { return m_state.value("conversationId").toString(); }
    QString conversationTitle() const { return m_state.value("conversationTitle").toString(); }
    QString draft() const { return m_draft; }
    void setDraft(const QString &value);
    QVariantList attachments() const { return m_state.value("attachments").toList(); }
    bool hasMoreConversations() const { return m_state.value("hasMoreConversations").toBool(); }
    bool hasMoreMessages() const { return m_state.value("hasMoreMessages").toBool(); }
    QString streamingText() const { return m_state.value("streamingText").toString(); }
    bool canSend() const;
    bool readOnlyPreview() const { return m_preview; }
    Q_INVOKABLE void connectSaved();
    Q_INVOKABLE void importSessionFile(const QUrl &url);
    Q_INVOKABLE void refreshConversations();
    Q_INVOKABLE void refreshModels();
    Q_INVOKABLE void loadMoreConversations();
    Q_INVOKABLE void openConversation(const QString &id);
    Q_INVOKABLE void loadMoreMessages();
    Q_INVOKABLE void newConversation();
    Q_INVOKABLE void send();
    Q_INVOKABLE void addPdf(const QUrl &url);
    Q_INVOKABLE void removeAttachment(const QString &id);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void logout();
signals:
    void changed();
    void draftChanged();
private:
    bool begin();
    void submit(const QString &operation, const QVariantMap &arguments = {});
    QThread m_thread;
    ChatWorker *m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QVariantMap m_state;
    QString m_draft, m_selectedModel;
    bool m_busy = true;
    bool m_logoutQueued = false;
    const bool m_preview;
};
}

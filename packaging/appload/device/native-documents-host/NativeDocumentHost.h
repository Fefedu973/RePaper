#pragma once
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <QTimer>
#include <memory>

class NativeDocumentHost : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool importsEnabled READ importsEnabled WRITE setImportsEnabled NOTIFY importsEnabledChanged)
    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath)
    Q_PROPERTY(QString stateDirectory READ stateDirectory WRITE setStateDirectory)
public:
    explicit NativeDocumentHost(QObject *parent = nullptr);
    ~NativeDocumentHost() override;
    void classBegin() override {}
    void componentComplete() override;
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    bool importsEnabled() const { return m_importsEnabled; }
    void setImportsEnabled(bool enabled);
    QString socketPath() const { return m_socketPath; }
    void setSocketPath(const QString &path) { if (!m_complete) m_socketPath = path; }
    QString stateDirectory() const { return m_stateDirectory; }
    void setStateDirectory(const QString &path) { if (!m_complete) m_stateDirectory = path; }
    void setInputRoot(const QString &path) { if (!m_complete) m_inputRoot = path; }
    void setMetadataRoot(const QString &path) { if (!m_complete) m_metadataRoot = path; }
    Q_INVOKABLE QVariantMap agendaContext(const QString &request) const;
    Q_INVOKABLE bool beginAgendaPage(const QString &request, const QString &planHash);
    Q_INVOKABLE bool completeAgendaPage(const QString &request, const QString &planHash);
    Q_INVOKABLE bool dispatchImport(const QString &keyHash);
    Q_INVOKABLE bool identifyImport(const QString &keyHash, const QString &documentId);
    Q_INVOKABLE void completeImport(const QString &keyHash, const QString &documentId);
    Q_INVOKABLE void failImport(const QString &keyHash, const QString &code, const QString &message);
    Q_INVOKABLE QString findImportedDocument(const QString &keyHash) const;
    Q_INVOKABLE void created(const QString &request, const QString &documentId);
    Q_INVOKABLE void opened(const QString &request, const QString &documentId);
    Q_INVOKABLE void rejected(const QString &request, const QString &code, const QString &message);
signals:
    void enabledChanged();
    void importsEnabledChanged();
    void importRequested(const QString &keyHash, const QString &sourceUrl, const QString &token,
                         const QString &documentId, const QString &displayName, bool dispatched, bool completed);
    void createRequested(const QString &request, const QString &documentId,
                         const QString &pageId, const QString &displayName);
    void openRequested(const QString &request, const QString &documentId);
    void dismissAppRequested(int framebufferKey);
private:
    struct Connection { QByteArray bytes; qint64 expected = -1; qsizetype headerEnd = -1; bool handled = false; };
    struct Pending {
        QPointer<QLocalSocket> socket;
        QString documentId, journalPath;
        QJsonObject journal;
        int callerKey = -1;
        bool opening = false;
        bool importing = false;
        QString importKey;
        QTimer *timeout = nullptr;
    };
    struct ImportJob {
        QString journalPath, sourcePath;
        QJsonObject journal;
        bool awaitingNative = false;
    };
    void start();
    void read(QLocalSocket *socket);
    void dispatch(QLocalSocket *socket, const QString &method, const QString &route, const QJsonObject &body);
    void requestImport(QLocalSocket *socket, const QJsonObject &body);
    bool snapshotImport(const QString &path, const QString &sha256, const QString &destination, QString *code);
    void finishImport(const QString &keyHash, const QJsonObject &body);
    void respond(QLocalSocket *socket, const QJsonObject &body);
    void finish(const QString &request, const QJsonObject &body);
    bool writeJournal(const QString &path, const QJsonObject &journal);
    QJsonObject failure(const QString &code, const QString &message, bool retryable = false) const;
    QString m_socketPath = "/run/repaper-appload/documents.sock";
    QString m_stateDirectory = "/home/root/.local/share/RePaper/native-documents";
    QString m_inputRoot = "/home/root/.local/share/paper-bridge/native-imports";
    QString m_metadataRoot = "/home/root/.local/share/remarkable/xochitl";
    QLocalServer m_server;
    std::unique_ptr<QLockFile> m_lock;
    QHash<QLocalSocket *, Connection> m_connections;
    QHash<QString, Pending> m_pending;
    QHash<QString, QString> m_noteJournalPaths;
    QHash<QString, ImportJob> m_importJobs;
    bool m_enabled = false, m_complete = false;
    bool m_importsEnabled = false;
};

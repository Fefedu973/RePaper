#pragma once
#include "BridgeClient.h"
#include "PowerPointConverter.h"
#include "SecretStore.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSqlDatabase>
#include <QVariantList>
#include <functional>
class QProcess;
class MoodleRelayClient;
class MoodleController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool offline READ offline NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString baseUrl READ baseUrl NOTIFY changed)
    Q_PROPERTY(QString loginBaseUrl READ loginBaseUrl WRITE setLoginBaseUrl NOTIFY loginBaseUrlChanged)
    Q_PROPERTY(QString launchLink READ launchLink NOTIFY changed)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY changed)
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(bool demo READ demo NOTIFY changed)
    Q_PROPERTY(bool pcIntegration READ pcIntegration CONSTANT)
    Q_PROPERTY(bool handoffBusy READ handoffBusy NOTIFY changed)
    Q_PROPERTY(bool waitingForBrowser READ waitingForBrowser NOTIFY changed)
    Q_PROPERTY(QString loginCode READ loginCode NOTIFY changed)
    Q_PROPERTY(QString verificationUrl READ verificationUrl NOTIFY changed)
    Q_PROPERTY(QString loginQr READ loginQr NOTIFY changed)
    Q_PROPERTY(QString portalUrl READ portalUrl CONSTANT)
  public:
    // An injected manager must outlive the controller. Production uses an ordinary
    // QNetworkAccessManager with Qt's unmodified TLS verification policy.
    explicit MoodleController(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr,
                              QNetworkAccessManager *relayNetwork = nullptr);
    ~MoodleController() override;
    bool loggedIn() const {
        return !m_token.isEmpty();
    }
    bool busy() const {
        return m_busy || m_bridge.busy();
    }
    bool offline() const {
        return m_offline;
    }
    QString message() const {
        return m_message;
    }
    QString title() const {
        return m_title;
    }
    QString baseUrl() const {
        return m_base;
    }
    QString loginBaseUrl() const {
        return m_loginBase;
    }
    void setLoginBaseUrl(const QString &value);
    QString launchLink() const;
    QString search() const {
        return m_search;
    }
    void setSearch(const QString &value);
    bool demo() const {
        return m_demo;
    }
    void loadDemo();
    bool pcIntegration() const;
    bool handoffBusy() const;
    bool waitingForBrowser() const {
        return m_authFlow == "waiting";
    }
    QString loginCode() const;
    QString verificationUrl() const;
    QString loginQr() const;
    QString portalUrl() const;
    Q_INVOKABLE void openPortal();
    Q_INVOKABLE void cancelLogin();
    QVariantList items() const;
    bool canGoBack() const {
        return !m_stack.isEmpty();
    }
    double progress() const {
        return m_progress;
    }
    Q_INVOKABLE void prepareLogin(const QString &base);
    Q_INVOKABLE void openLogin(const QString &base);
    Q_INVOKABLE void login(const QString &base, const QString &handoff);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void openItem(const QVariantMap &item);
    Q_INVOKABLE void back();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void importResource(const QVariantMap &item);
  signals:
    void changed();
    void loginFinished(bool success);
    void loginBaseUrlChanged();

  private:
    using Callback = std::function<void(QJsonDocument)>;
    void call(const QString &function, const QMap<QString, QString> &params, Callback callback,
              int attempt = 0);
    void showCourses(bool refresh);
    void showSections(int course, const QString &name, bool refresh);
    void setRows(const QVariantList &items, const QString &title, bool push = false);
    void saveSnapshot(const QString &key, const QJsonDocument &doc);
    QJsonDocument snapshot(const QString &key) const;
    QVariantList courses(const QJsonArray &array) const;
    QVariantList sections(const QJsonArray &array) const;
    QString account() const;
    QString cacheFile(const QVariantMap &item) const;
    void importCachedResource(const QVariantMap &item, const QString &path);
    void fail(const QString &message);
    void pcHandoff(const QString &operation, const QByteArray &payload);
    void exchangeQr(int userId, const QString &key);
    QString m_base = "https://e-campus.cpe.fr", m_token, m_user, m_name, m_title = "Mes cours", m_search,
            m_message, m_passport, m_importKey;
    QString m_previousBase, m_previousToken, m_previousUser, m_loginBase;
    bool m_busy = false, m_offline = false, m_authenticating = false, m_demo = false;
    double m_progress = 0;
    int m_epoch = 0, m_course = 0;
    QVariantList m_rows;
    QList<QPair<QString, QVariantList>> m_stack;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QSqlDatabase m_db;
    repaper::SecretStore m_vault;
    repaper::BridgeClient m_bridge;
    PowerPointConverter m_powerPoint;
    QVariantMap m_convertingItem;
    QPointer<QProcess> m_handoff;
    QString m_authFlow = "idle", m_loginAttemptId;
    MoodleRelayClient *m_relay = nullptr;
    bool m_qrAuthenticating = false;
    int m_expectedQrUserId = 0;
};

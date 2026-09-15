#pragma once
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <openssl/evp.h>

class MoodleRelayClient : public QObject {
    Q_OBJECT
  public:
    explicit MoodleRelayClient(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~MoodleRelayClient() override;
    void start(const QString &portal, const QString &moodleBase, const QString &passport);
    void cancel();
    bool active() const {
        return m_active;
    }
    QString code() const {
        return m_code;
    }
    QString verificationUrl() const {
        return m_verificationUrl;
    }
    QString qrImage() const {
        return m_qrImage;
    }
    QString message() const {
        return m_message;
    }
    QString publicKey() const {
        return m_publicKey;
    }
    QString expectedSiteId() const {
        return m_expected;
    }
    void poll();
  signals:
    void changed();
    void qrReceived(int userId, const QString &key);
    void failed(const QString &message);

  private:
    using Callback = std::function<void(int, const QJsonObject &)>;
    void request(const QString &method, const QString &path, const QJsonObject &body, bool authenticated,
                 Callback callback);
    void error(const QString &message);
    void forget();
    bool generateKey();
    QJsonObject decrypt(const QString &ciphertext);
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_poll;
    QDeadlineTimer m_deadline;
    EVP_PKEY *m_key = nullptr;
    QString m_portal, m_session, m_secret, m_code, m_verificationUrl, m_qrImage, m_expected, m_publicKey,
        m_message;
    bool m_active = false;
    int m_epoch = 0;
};

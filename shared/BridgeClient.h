#pragma once
#include <QJsonObject>
#include <QObject>
#include <QVariantMap>

namespace repaper {
class BridgeClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantMap capabilities READ capabilities NOTIFY changed)
  public:
    explicit BridgeClient(QObject *parent = nullptr);
    bool busy() const {
        return m_busy;
    }
    QString message() const {
        return m_message;
    }
    QVariantMap capabilities() const {
        return m_capabilities;
    }
    Q_INVOKABLE void probe();
    Q_INVOKABLE void importFile(const QString &path, const QString &title, const QString &idempotencyKey);
    Q_INVOKABLE void createNotebook(const QString &title, const QString &idempotencyKey);
    void createNotebook(const QString &title, const QString &idempotencyKey, const QJsonObject &agenda);
    Q_INVOKABLE void openDocument(const QString &documentId);
    static QJsonObject request(const QString &method, const QString &route, const QJsonObject &body = {});
  signals:
    void changed();
    void imported(const QString &documentId);
    void failed(const QString &code, const QString &message);

  private:
    void run(const QString &method, const QString &route, QJsonObject body = {});
    bool m_busy = false;
    QString m_message;
    QVariantMap m_capabilities;
};
} // namespace repaper

#pragma once
#include <QJsonObject>
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QSqlDatabase>
#include <QTimer>
class BridgeService : public QObject {
    Q_OBJECT
  public:
    BridgeService(QString state, QString store, bool sandbox, bool proxy, bool sandboxUnvalidated = false,
                  QObject *parent = nullptr, QString importInputRoot = {});
    bool listen(const QString &socket);
    void recover();
    void setIdleTimeout(int seconds);
    QJsonObject handle(const QString &method, const QString &route, const QJsonObject &body);

  private:
    QJsonObject capabilities() const;
    bool adapterValidated() const;
    QJsonObject import(const QJsonObject &request, bool notebook);
    QJsonObject nativeImport(const QJsonObject &request);
    void state(const QString &operation, const QString &value, const QJsonObject &result = {});
    bool serviceActive(const QString &name) const;
    void service(const QString &action, const QString &name) const;
    void rollback(const QJsonObject &manifest);
    QString m_state, m_store, m_importInputRoot;
    bool m_sandbox = false, m_proxy = false, m_sandboxUnvalidated = false;
    QLockFile m_instanceLock;
    QTimer m_idleTimer;
    int m_idleTimeout = 0;
    int m_activeConnections = 0;
    QLocalServer m_server;
    QSqlDatabase m_db;
};

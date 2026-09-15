#pragma once

#include <QJsonObject>
#include <QLocalServer>
#include <QPointer>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class QLocalSocket;

// Geometry telemetry for an explicitly opted-in AppLoad framebuffer.
// This item never takes input focus or handles pointer events.
class NativeViewportHost : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int framebufferID READ framebufferID WRITE setFramebufferID NOTIFY framebufferIDChanged)
    Q_PROPERTY(QQuickItem *framebufferItem READ framebufferItem WRITE setFramebufferItem NOTIFY framebufferItemChanged)
    Q_PROPERTY(QString socketPath READ socketPath NOTIFY socketPathChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
public:
    explicit NativeViewportHost(QQuickItem *parent = nullptr);
    ~NativeViewportHost() override;

    int framebufferID() const { return m_key; }
    void setFramebufferID(int key);
    QQuickItem *framebufferItem() const { return m_framebuffer.data(); }
    void setFramebufferItem(QQuickItem *item);
    QString socketPath() const { return m_path; }
    bool connected() const { return m_ready; }

signals:
    void framebufferIDChanged();
    void framebufferItemChanged();
    void socketPathChanged();
    void connectedChanged();

private slots:
    void scheduleGeometry();

private:
    void openServer();
    void closeServer();
    void acceptConnection();
    void readFrames();
    void sendGeometry();
    void dropPeer();
    bool send(const QJsonObject &frame);

    QLocalServer m_server;
    std::unique_ptr<QTemporaryDir> m_directory;
    QPointer<QLocalSocket> m_socket;
    QPointer<QQuickItem> m_framebuffer;
    QList<QMetaObject::Connection> m_geometryConnections;
    QTimer m_geometryTimer;
    QByteArray m_buffer;
    QString m_path;
    int m_key = -1;
    qint64 m_sequence = 0;
    QSizeF m_lastSize;
    int m_lastRotation = -1;
    bool m_ready = false;
};

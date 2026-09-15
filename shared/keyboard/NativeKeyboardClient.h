#pragma once
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QTimer>

namespace repaper {
// Connects a focused application field to a Qt input-method item in Xochitl.
// This transport never implements, or claims to implement, the native keyboard.
class NativeKeyboardClient : public QObject {
    Q_OBJECT
public:
    explicit NativeKeyboardClient(QObject *parent = nullptr);
    ~NativeKeyboardClient() override;
    void setEndpoint(const QString &path, int framebufferKey);
    void focus(QQuickWindow *window, QQuickItem *target);
    void blur();
    bool available() const { return m_available; }
    bool waiting() const { return m_waiting; }
    bool visible() const { return m_available && m_visible; }
    QRectF keyboardRectangle() const { return m_keyboardRectangle; }
signals:
    void changed();
private slots:
    void scheduleState();
    void sendState();
private:
    void send(const QJsonObject &message);
    void receive();
    void handle(const QJsonObject &message);
    void fail();
    void clearTarget();
    QJsonObject state(const QString &type) const;
    QString m_path, m_session;
    int m_key = -1;
    QLocalSocket m_socket;
    QByteArray m_buffer;
    QTimer m_timeout, m_stateTimer;
    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem> m_target;
    QList<QMetaObject::Connection> m_connections;
    bool m_ready = false, m_available = false, m_waiting = false, m_visible = false;
    QRectF m_keyboardRectangle;
    qint64 m_ack = 0;
};
}

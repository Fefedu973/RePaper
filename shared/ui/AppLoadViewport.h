#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QMatrix4x4>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRectF>
#include <QTimer>
#include <QTransform>

class AppLoadViewport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(qreal width READ width NOTIFY changed)
    Q_PROPERTY(qreal height READ height NOTIFY changed)
    Q_PROPERTY(QMatrix4x4 contentTransform READ contentTransform NOTIFY changed)
public:
    explicit AppLoadViewport(QQuickWindow *window = nullptr, QObject *parent = nullptr);
    ~AppLoadViewport() override;
    void setWindow(QQuickWindow *window);
    void setEndpoint(const QString &path, int framebufferKey);

    bool available() const;
    qreal width() const;
    qreal height() const;
    QMatrix4x4 contentTransform() const;
    Q_INVOKABLE QRectF mapRectFromWindow(const QRectF &rectangle) const;

signals:
    void changed();

private:
    void connectEndpoint();
    void receive();
    void handle(const QJsonObject &frame);
    void fail();
    QTransform contentToWindow() const;

    QPointer<QQuickWindow> m_window;
    QList<QMetaObject::Connection> m_windowConnections;
    QLocalSocket m_socket;
    QTimer m_retryTimer;
    QTimer m_timeout;
    QByteArray m_buffer;
    QString m_path;
    int m_key = -1;
    int m_retryDelay = 200;
    qint64 m_sequence = 0;
    qreal m_hostWidth = 0;
    qreal m_hostHeight = 0;
    int m_rotation = 0;
    bool m_receivedGeometry = false;
    bool m_resetting = false;
};

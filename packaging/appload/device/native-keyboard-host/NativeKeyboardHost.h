#pragma once

#include <QJsonObject>
#include <QLocalServer>
#include <QPointer>
#include <QQuickItem>
#include <QTemporaryDir>
#include <memory>

class FBController;
class QInputMethodEvent;
class QKeyEvent;
class QLocalSocket;

// A focus item inside the host Qt Quick window. It delegates the actual input
// panel to that process's installed QInputMethod; it does not draw a keyboard.
class NativeKeyboardHost : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int framebufferID READ framebufferID WRITE setFramebufferID NOTIFY framebufferIDChanged)
    Q_PROPERTY(QQuickItem *framebufferItem READ framebufferItem WRITE setFramebufferItem NOTIFY framebufferItemChanged)
    Q_PROPERTY(QString socketPath READ socketPath NOTIFY socketPathChanged)
public:
    explicit NativeKeyboardHost(QQuickItem *parent = nullptr);
    ~NativeKeyboardHost() override;

    int framebufferID() const { return m_key; }
    void setFramebufferID(int key);
    QQuickItem *framebufferItem() const { return m_framebuffer.data(); }
    void setFramebufferItem(QQuickItem *item);
    QString socketPath() const { return m_path; }
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    Q_INVOKABLE QVariant inputMethodQuery(Qt::InputMethodQuery query, const QVariant &argument) const;

signals:
    void framebufferIDChanged();
    void framebufferItemChanged();
    void socketPathChanged();

protected:
    void inputMethodEvent(QInputMethodEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    friend class NativeKeyboardHostTests;
    struct State {
        QString text;
        int cursor = 0;
        int anchor = 0;
        int hints = 0;
        int maximumLength = 2147483647;
        int echoMode = 0;
        QRectF cursorRect;
        QRectF anchorRect;
        QSizeF clientSize;
    };
    void openServer();
    void closeServer();
    void acceptConnection();
    void readFrames();
    void processFrame(const QJsonObject &frame);
    bool parseState(const QJsonObject &frame, State *state) const;
    bool send(QJsonObject frame);
    void reject(const char *code);
    void endSession(bool restoreFocus);
    void activateSession();
    void releaseFocus(bool restoreFocus);
    bool ownsInputFocus() const;
    void sendStatus();
    void reconnectGeometry();
    void geometryChanged();
    QTransform clientToFramebufferItem(bool *ok = nullptr) const;
    QRectF clientRectToHost(const QRectF &rect) const;
    QRectF keyboardRectForClient(const QRectF &windowRectangle) const;
    void forwardKey(QKeyEvent *event, bool pressed);
    void applyInputToShadow(const QInputMethodEvent *event);
    void applyKeyToShadow(const QKeyEvent *event);

    QLocalServer m_server;
    std::unique_ptr<QTemporaryDir> m_directory;
    QPointer<QLocalSocket> m_socket;
    QPointer<QQuickItem> m_framebuffer;
    QPointer<QQuickItem> m_previousFocus;
    QList<QMetaObject::Connection> m_geometryConnections;
    QByteArray m_buffer;
    QString m_path;
    QString m_session;
    State m_state;
    int m_key = -1;
    qint64 m_sequence = 0;
    bool m_ready = false;
    bool m_rejecting = false;
    bool m_closing = false;
    bool m_wantsFocus = false;
};

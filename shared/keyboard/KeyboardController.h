#pragma once
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

namespace repaper {
class ClipboardBridge;
class NativeKeyboardClient;
// Public Qt input-method bridge. A visible Qt input panel is not evidence that
// the proprietary Xochitl keyboard is available to an external application.
class KeyboardController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(QQuickItem* target READ target NOTIFY changed)
    Q_PROPERTY(bool fallbackVisible READ fallbackVisible NOTIFY changed)
    Q_PROPERTY(bool platformVisible READ platformVisible NOTIFY changed)
    Q_PROPERTY(qreal platformHeight READ platformHeight NOTIFY changed)
    Q_PROPERTY(QRectF platformRectangle READ platformRectangle NOTIFY changed)
public:
    explicit KeyboardController(QObject *parent=nullptr);
    QQuickWindow *window() const {return m_window;}
    void setWindow(QQuickWindow *window);
    QQuickItem *target() const {return m_target;}
    bool fallbackVisible() const;
    bool platformVisible() const;
    qreal platformHeight() const;
    QRectF platformRectangle() const;
    Q_INVOKABLE void insertText(const QString &text);
    Q_INVOKABLE void backspace();
    Q_INVOKABLE void enter();
    Q_INVOKABLE void moveCursor(int direction);
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void refreshGeometry();
    // Dependency injection also allows a future verified platform adapter.
    void setClipboardBridge(ClipboardBridge *clipboard);
signals:
    void windowChanged();
    void changed();
    void pasteFailed();
private:
    bool eventFilter(QObject *object,QEvent *event) override;
private slots:
    void updateFocus();
private:
    bool editable(QQuickItem *item) const;
    void sendKey(int key);
    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem> m_target;
    QTimer m_fallbackTimer;
    QList<QMetaObject::Connection> m_targetConnections;
    bool m_waiting=false;
    QPointer<ClipboardBridge> m_clipboard;
    NativeKeyboardClient *m_native = nullptr;
    quint64 m_focusEpoch=0,m_pasteEpoch=0;
    QPointer<QQuickItem> m_pasteTarget;
    int m_pasteCursor=0,m_pasteAnchor=0,m_pasteEnd=0;
    QString m_pasteText;
    bool m_pastePending=false;
};
void registerKeyboardTypes();
}

#pragma once
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

class QQuickItem;
class QQuickWindow;
class QPointingDevice;

// Observes the window before Quick dispatches touch to MouseArea. It never
// accepts an event or changes pointer grabs; native navigation keeps ownership.
class NativeTouchGuard final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool blockedSequence READ blockedSequence NOTIFY blockedSequenceChanged)
    Q_PROPERTY(bool penCaptureAllowed READ penCaptureAllowed NOTIFY penCaptureAllowedChanged)
public:
    explicit NativeTouchGuard(QObject *parent = nullptr);
    ~NativeTouchGuard() override;
    void attach(QQuickItem *view);
    bool acceptsBegin() const;
    bool blockedSequence() const { return m_blocked; }
    bool penCaptureAllowed() const { return !m_eraser; }
    void confirmNativePenTip();

signals:
    void cancelRequested();
    void blockedSequenceChanged();
    void penCaptureAllowedChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setWindow(QQuickWindow *window);
    void block(bool alwaysCancel = false);
    void setBlocked(bool blocked);
    void resetAfterDispatch();
    int activeContactCount() const;

    QPointer<QQuickItem> m_view;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_viewWindowChanged;
    QMetaObject::Connection m_viewDestroyed;
    QMetaObject::Connection m_windowDestroyed;
    QHash<const QPointingDevice *, QSet<int>> m_contacts;
    QTimer m_reset;
    bool m_blocked = false;
    bool m_eraser = false;
};

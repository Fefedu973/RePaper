#pragma once

#include <QGuiApplication>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTabletEvent>
#include <QWindow>

namespace repaper {

// AppLoad forwards real tablet events to a LinuxFB application, but the legacy
// mouse handlers used by its controls need an explicit mouse delivery path.
// Opt-in is restricted to the app process; the original pressure-bearing tablet
// transport and the host's input handling remain separate.
class TabletMouseAdapter final : public QObject
{
public:
    explicit TabletMouseAdapter(QObject *parent = nullptr) : QObject(parent)
    {
        if (qGuiApp)
            qGuiApp->installEventFilter(this);
    }

    ~TabletMouseAdapter() override
    {
        cancelGesture();
        if (qGuiApp)
            qGuiApp->removeEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_window && ((watched == m_window
                         && (event->type() == QEvent::Hide || event->type() == QEvent::Close
                             || event->type() == QEvent::WindowDeactivate))
                        || (watched == qGuiApp && event->type() == QEvent::ApplicationDeactivate))) {
            // A control can hide its window from its own pressed handler,
            // before Qt Quick has installed the exclusive grab. Cancel as soon
            // as that mouse delivery unwinds so the new grab is cleared too.
            if (m_sendingMouse)
                m_cancelPending = true;
            else
                cancelGesture();
            return false;
        }

        if (m_sendingMouse)
            return false;

        auto *window = qobject_cast<QWindow *>(watched);
        if (!window || (event->type() != QEvent::TabletPress
                        && event->type() != QEvent::TabletMove
                        && event->type() != QEvent::TabletRelease))
            return false;

        auto *tablet = static_cast<QTabletEvent *>(event);
        if (event->type() == QEvent::TabletPress) {
            cancelGesture();
            m_window = window;
            m_device = tablet->pointingDevice();
            m_destroyed = connect(window, &QObject::destroyed, this, [this]() { clearGesture(); });
        }

        // A hover, stray release, or another pen must not create a partial
        // mouse gesture. Consume it to prevent a second Qt synthesis path.
        event->accept();
        if (!m_window || tablet->pointingDevice() != m_device)
            return true;

        QPointer<QWindow> target = m_window;
        const QPointF global = tablet->globalPosition();
        const QPointF local = target == window
            ? tablet->position() : global - QPointF(target->mapToGlobal(QPoint(0, 0)));
        const bool finalPositionChanged = global != m_lastGlobal;
        m_lastGlobal = global;
        m_modifiers = tablet->modifiers();
        m_timestamp = tablet->timestamp();

        QEvent::Type type = QEvent::MouseMove;
        Qt::MouseButton button = Qt::NoButton;
        Qt::MouseButtons buttons = Qt::LeftButton;
        if (event->type() == QEvent::TabletPress) {
            type = QEvent::MouseButtonPress;
            button = Qt::LeftButton;
        } else if (event->type() == QEvent::TabletRelease) {
            type = QEvent::MouseButtonRelease;
            button = Qt::LeftButton;
            buttons = Qt::NoButton;
            clearGesture();
            // Controls cache their pressed/inside state on movement. A pen can
            // leave the surface and release without an intervening move packet.
            if (finalPositionChanged)
                sendMouse(target, QEvent::MouseMove, local, global,
                          Qt::NoButton, Qt::LeftButton, tablet->modifiers(), tablet->timestamp());
        }
        sendMouse(target, type, local, global, button, buttons,
                  tablet->modifiers(), tablet->timestamp());
        return true;
    }

private:
    void sendMouse(QPointer<QWindow> window, QEvent::Type type, const QPointF &local,
                   const QPointF &global, Qt::MouseButton button, Qt::MouseButtons buttons,
                   Qt::KeyboardModifiers modifiers, ulong timestamp)
    {
        if (!window)
            return;
        QMouseEvent mouse(type, local, local, global, button, buttons, modifiers,
                          Qt::MouseEventSynthesizedByApplication,
                          QPointingDevice::primaryPointingDevice());
        mouse.setTimestamp(timestamp);
        const bool wasSending = m_sendingMouse;
        m_sendingMouse = true;
        QCoreApplication::sendEvent(window, &mouse);
        m_sendingMouse = wasSending;
        if (!wasSending && m_cancelPending) {
            m_cancelPending = false;
            cancelGesture();
        }
    }

    void clearGesture()
    {
        disconnect(m_destroyed);
        m_destroyed = {};
        m_window.clear();
        m_device = nullptr;
        m_cancelPending = false;
    }

    void cancelGesture()
    {
        QPointer<QWindow> window = m_window;
        const auto modifiers = m_modifiers;
        const auto timestamp = m_timestamp;
        clearGesture();
        if (!window)
            return;
        // Cancel the exclusive grab first. Releasing a cached pressed Button
        // directly at an outside position can still trigger its clicked signal.
        if (auto *quick = qobject_cast<QQuickWindow *>(window.data())) {
            if (auto *grabber = quick->mouseGrabberItem())
                grabber->ungrabMouse();
        }
        if (!window)
            return;
        QEvent ungrab(QEvent::UngrabMouse);
        QCoreApplication::sendEvent(window, &ungrab);
        if (!window)
            return;
        const QPointF outside(-1, -1);
        const QPointF global = QPointF(window->mapToGlobal(QPoint(0, 0))) + outside;
        sendMouse(window, QEvent::MouseButtonRelease, outside, global,
                  Qt::LeftButton, Qt::NoButton, modifiers, timestamp);
    }

    QPointer<QWindow> m_window;
    const QPointingDevice *m_device = nullptr;
    QMetaObject::Connection m_destroyed;
    QPointF m_lastGlobal;
    Qt::KeyboardModifiers m_modifiers = Qt::NoModifier;
    ulong m_timestamp = 0;
    bool m_sendingMouse = false;
    bool m_cancelPending = false;
};

inline TabletMouseAdapter *installTabletMouseAdapter()
{
    if (!qGuiApp || qgetenv("REPAPER_APPLOAD_TABLET_INPUT") != QByteArrayLiteral("1"))
        return nullptr;
    static QPointer<TabletMouseAdapter> adapter;
    if (!adapter) {
        adapter = new TabletMouseAdapter(qGuiApp);
        adapter->setObjectName(QStringLiteral("repaper.tabletMouseAdapter"));
    }
    return adapter;
}

} // namespace repaper

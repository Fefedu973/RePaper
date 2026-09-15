#include "NativeTouchGuard.h"
#include <QEvent>
#include <QCoreApplication>
#include <QEventPoint>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QTouchEvent>
#include <QPointerEvent>

NativeTouchGuard::NativeTouchGuard(QObject *parent) : QObject(parent) {
    // Proximity events are addressed to the application rather than a window.
    if (qApp) qApp->installEventFilter(this);
    m_reset.setSingleShot(true);
    m_reset.setInterval(0);
    connect(&m_reset, &QTimer::timeout, this, [this] {
        if (activeContactCount() == 0) setBlocked(false);
    });
}

NativeTouchGuard::~NativeTouchGuard() {
    if (qApp) qApp->removeEventFilter(this);
}

void NativeTouchGuard::attach(QQuickItem *view) {
    if (m_view == view) return;
    disconnect(m_viewWindowChanged);
    disconnect(m_viewDestroyed);
    m_view = view;
    if (view) {
        m_viewWindowChanged = connect(view, &QQuickItem::windowChanged, this,
                                     &NativeTouchGuard::setWindow);
        m_viewDestroyed = connect(view, &QObject::destroyed, this, [this] {
            setWindow(nullptr);
        });
    }
    setWindow(view ? view->window() : nullptr);
}

bool NativeTouchGuard::acceptsBegin() const {
    return m_window && !m_blocked && !m_eraser && activeContactCount() < 2;
}

void NativeTouchGuard::confirmNativePenTip() {
    // The pre-routing kernel classifier can see a tip before any Qt event can
    // arrive at a surface that was hidden by eraser proximity.
    if (!m_eraser) return;
    m_eraser = false;
    emit penCaptureAllowedChanged();
}

void NativeTouchGuard::setWindow(QQuickWindow *window) {
    if (window && window->thread() != thread()) window = nullptr;
    if (m_window == window) return;
    const bool hadWindow = !m_window.isNull();
    disconnect(m_windowDestroyed);
    m_reset.stop();
    m_contacts.clear();
    m_window = window;
    setBlocked(false);
    if (hadWindow) emit cancelRequested();
    if (window) {
        m_windowDestroyed = connect(window, &QObject::destroyed, this, [this] {
            m_reset.stop();
            m_contacts.clear();
            m_window.clear();
            setBlocked(false);
            emit cancelRequested();
        });
    }
}

int NativeTouchGuard::activeContactCount() const {
    int count = 0;
    for (auto it = m_contacts.cbegin(); it != m_contacts.cend(); ++it)
        count += it.value().size();
    return count;
}

void NativeTouchGuard::setBlocked(bool blocked) {
    if (m_blocked == blocked) return;
    m_blocked = blocked;
    emit blockedSequenceChanged();
}

void NativeTouchGuard::block(bool alwaysCancel) {
    const bool wasBlocked = m_blocked;
    setBlocked(true);
    if (!wasBlocked || alwaysCancel) emit cancelRequested();
}

void NativeTouchGuard::resetAfterDispatch() {
    // Keep the latch through this entire TouchEnd/Cancel dispatch, including
    // its synthesized MouseArea release. Clearing the latch never restarts the
    // canceled editor gesture; NativeScene::pointerEnd must remain a no-op then.
    if (activeContactCount() == 0) m_reset.start();
}

bool NativeTouchGuard::eventFilter(QObject *watched, QEvent *event) {
    const bool proximity = event->type() == QEvent::TabletEnterProximity
        || event->type() == QEvent::TabletLeaveProximity;
    if (m_window && (watched == m_window || watched == m_view || (proximity && watched == qApp))
        && event->isPointerEvent()) {
        const auto *pointer = static_cast<QPointerEvent *>(event);
        const auto type = pointer->pointingDevice()->pointerType();
        const bool eraser = type == QPointingDevice::PointerType::Eraser;
        // A stale release/leave cannot re-enable a surface canceled by the eraser.
        const bool freshPen = type == QPointingDevice::PointerType::Pen
            && event->type() != QEvent::TabletRelease && event->type() != QEvent::MouseButtonRelease
            && event->type() != QEvent::TabletLeaveProximity && event->type() != QEvent::HoverLeave;
        if ((eraser || freshPen) && m_eraser != eraser) {
            m_eraser = eraser;
            QPointer<NativeTouchGuard> alive(this);
            emit penCaptureAllowedChanged();
            if (!alive) return false;
            if (eraser) emit cancelRequested();
            if (!alive) return false;
        }
    }
    if (watched != m_window) return false;
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        const auto *touch = static_cast<const QTouchEvent *>(event);
        m_reset.stop();
        if (event->type() == QEvent::TouchBegin) {
            m_contacts.remove(touch->pointingDevice());
            if (activeContactCount() == 0) setBlocked(false);
        }
        auto &contacts = m_contacts[touch->pointingDevice()];
        for (const auto &point : touch->points()) {
            if (point.state() == QEventPoint::Released) contacts.remove(point.id());
            else contacts.insert(point.id());
        }
        // Also reject a batch containing one release and a second press, even
        // if their delivery leaves only one currently active contact.
        if (touch->points().size() > 1 || activeContactCount() > 1) block();
        if (event->type() == QEvent::TouchEnd)
            m_contacts.remove(touch->pointingDevice());
        resetAfterDispatch();
        break;
    }
    case QEvent::TouchCancel:
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
    case QEvent::Close:
        m_contacts.clear();
        block(true);
        resetAfterDispatch();
        break;
    default:
        break;
    }
    // In particular, do not call accept(), ignore(), setAccepted(), or any grab
    // API. Both normal and canceled touch streams still reach native handlers.
    return false;
}

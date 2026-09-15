#include "NativeTouchRelay.h"
#include <QCoreApplication>
#include <QQuickWindow>
#include <QScopedValueRollback>
#include <QTouchEvent>
#include <QWindow>

namespace {
class TouchEndpoint final : public QWindow {
protected:
    bool event(QEvent *event) override {
        // TouchArea's event filter is the only handler. A false result means it
        // is still considering candidates and the original stream may continue.
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
            || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel)
            return false;
        return QWindow::event(event);
    }
};
}

NativeTouchRelay::NativeTouchRelay(QObject *parent)
    : QObject(parent), m_endpoint(std::make_unique<TouchEndpoint>()) {}

NativeTouchRelay::~NativeTouchRelay() {
    cancel();
    if (m_window) m_window->removeEventFilter(this);
}

QObject *NativeTouchRelay::eventTarget() const { return m_endpoint.get(); }

void NativeTouchRelay::setSurface(QQuickItem *surface) {
    if (m_surface == surface) return;
    cancel();
    if (m_surface) disconnect(m_surface, nullptr, this, nullptr);
    m_surface = surface;
    if (surface) {
        connect(surface, &QQuickItem::windowChanged, this, &NativeTouchRelay::setWindow);
        connect(surface, &QObject::destroyed, this, [this] { setWindow(nullptr); });
    }
    setWindow(surface ? surface->window() : nullptr);
    emit surfaceChanged();
}

void NativeTouchRelay::setRecognizer(QObject *recognizer) {
    if (m_recognizer == recognizer) return;
    cancel();
    m_recognizer = recognizer;
    emit recognizerChanged();
}

void NativeTouchRelay::setExcludedItems(const QVariantList &items) {
    if (m_excludedItems == items) return;
    m_excludedItems = items;
    emit excludedItemsChanged();
}

void NativeTouchRelay::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    if (!enabled) cancel();
    m_enabled = enabled;
    emit enabledChanged();
}

void NativeTouchRelay::setWindow(QQuickWindow *window) {
    if (m_window == window) return;
    cancel();
    if (m_window) m_window->removeEventFilter(this);
    m_window = window;
    if (window) window->installEventFilter(this);
}

bool NativeTouchRelay::acceptsOrigin(const QPointF &position) const {
    if (!m_enabled || !m_surface || !m_recognizer || !m_surface->isEnabled()
        || !m_surface->isVisible() || !m_surface->contains(m_surface->mapFromScene(position))) return false;
    for (const auto &value : m_excludedItems) {
        const auto *item = qobject_cast<QQuickItem *>(value.value<QObject *>());
        if (item && item->isVisible() && item->isEnabled()
            && item->contains(item->mapFromScene(position))) return false;
    }
    return true;
}

void NativeTouchRelay::cancel() {
    const bool hadSequence = m_sequence;
    m_sequence = false;
    m_adopted = false;
    if (hadSequence && m_recognizer) QMetaObject::invokeMethod(m_recognizer, "cancelSequence");
    if (hadSequence) emit cancelRequested();
}

bool NativeTouchRelay::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_window || m_deliveringCancel) return false;
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto *touch = static_cast<QTouchEvent *>(event);
        if (event->type() == QEvent::TouchBegin) {
            cancel();
            m_sequence = !touch->points().isEmpty() && acceptsOrigin(touch->points().first().scenePosition());
        }
        if (!m_sequence) return false;
        // QWindow is intentional: the exact firmware TouchArea.target filter
        // maps each scene position into its own item space for window targets.
        // Its ordinary candidate/adoption and palm/pen policies remain native.
        const std::unique_ptr<QTouchEvent> copy(touch->clone());
        const bool handled = QCoreApplication::sendEvent(m_endpoint.get(), copy.get());
        if (handled && !m_adopted) {
            m_adopted = true;
            emit cancelRequested();
            // Native TouchArea sends TouchCancel to its target when it adopts.
            // Our endpoint absorbed that notification; mirror it to the actual
            // Quick window so its synthesized mouse/touch grab state is reset
            // even though we will consume the rest of this native sequence.
            if (m_window) {
                QScopedValueRollback<bool> cancelGuard(m_deliveringCancel, true);
                QTouchEvent canceled(QEvent::TouchCancel, touch->pointingDevice(), touch->modifiers());
                QCoreApplication::sendEvent(m_window, &canceled);
            }
            if (m_surface) m_surface->ungrabMouse();
        }
        const bool consume = m_adopted;
        if (event->type() == QEvent::TouchEnd) {
            m_sequence = false;
            m_adopted = false;
        }
        if (consume) event->accept();
        return consume;
    }
    case QEvent::TouchCancel:
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
    case QEvent::Close:
        cancel();
        break;
    default:
        break;
    }
    return false;
}

#include "NativeSceneObserver.h"
#include <QMetaMethod>
#include <QMetaProperty>
#include <QThread>

namespace {
QString controllerPage(QObject *controller) {
    if (!controller || controller->thread() != QThread::currentThread()) return {};
    const int property = controller->metaObject()->indexOfProperty("pageId");
    if (property < 0) return {};
    const auto meta = controller->metaObject()->property(property);
    if (meta.metaType() != QMetaType::fromType<QString>()) return {};
    return meta.read(controller).toString();
}
}

NativeSceneObserver::NativeSceneObserver(QObject *controller, QObject *parent)
    : QObject(parent), m_controller(controller), m_pageId(controllerPage(controller)) {
    m_reason = m_pageId.isEmpty() ? QStringLiteral("page-unavailable") : QStringLiteral("not-attached");
}

NativeSceneObserver::~NativeSceneObserver() { disconnectScene(); }

bool NativeSceneObserver::samePage() const {
    return !m_pageId.isEmpty() && m_pageId.size() <= 128 && m_controller
        && thread() == QThread::currentThread() && controllerPage(m_controller) == m_pageId;
}

bool NativeSceneObserver::ready() const {
    return m_ready && !m_detached && m_scene && samePage();
}

bool NativeSceneObserver::observesIdentity(quintptr sceneIdentity) const {
    return sceneIdentity && ready() && reinterpret_cast<quintptr>(m_scene.data()) == sceneIdentity;
}

bool NativeSceneObserver::tryAttach() {
    if (m_detached) return false;
    if (!samePage()) {
        m_detached = true;
        disconnectScene();
        m_reason = QStringLiteral("page-or-controller-changed");
        emit readinessChanged();
        return false;
    }
    // Revalidate the existing Page's actual Scene even when the pageId has not
    // changed (a native reload can replace its Scene). The bounded acquisition
    // keeps an unchanged binding, or reconnects to the new guarded sender.
    QString reason;
    const bool attached = RePaperNative::attachExistingSceneObserver(m_controller, m_pageId, this, &reason);
    if (!attached) disconnectScene();
    m_reason = attached ? QString{} : reason;
    emit readinessChanged();
    return attached && ready();
}

bool NativeSceneObserver::connectScene(QObject *scene) {
    if (!scene || m_detached || !m_controller || thread() != QThread::currentThread()) return false;
    if (m_scene == scene && m_ready && m_changeConnection && m_destroyConnection) return true;
    disconnectScene();
    const auto *nativeMeta = scene->metaObject();
    const int signalIndex = nativeMeta->indexOfSignal("changed(QRectF)");
    const int slotIndex = metaObject()->indexOfSlot("sceneChanged(QRectF)");
    if (signalIndex < 0 || slotIndex < 0) return false;
    m_scene = scene;
    m_changeConnection = QObject::connect(scene, nativeMeta->method(signalIndex),
        this, metaObject()->method(slotIndex), Qt::QueuedConnection);
    m_destroyConnection = QObject::connect(scene, &QObject::destroyed,
        this, &NativeSceneObserver::sceneDestroyed, Qt::QueuedConnection);
    m_ready = bool(m_changeConnection) && bool(m_destroyConnection);
    if (!m_ready) disconnectScene();
    return m_ready;
}

void NativeSceneObserver::disconnectScene() {
    QObject::disconnect(m_changeConnection);
    QObject::disconnect(m_destroyConnection);
    m_changeConnection = {};
    m_destroyConnection = {};
    m_scene.clear();
    m_ready = false;
}

void NativeSceneObserver::detach() {
    m_detached = true;
    disconnectScene();
    m_reason = QStringLiteral("detached");
    emit readinessChanged();
}

void NativeSceneObserver::sceneChanged(const QRectF &) {
    // No Scene access occurs here. This slot is always delivered on our GUI
    // thread, after the native worker has emitted the dirty-region signal.
    if (ready() && sender() == m_scene.data()) emit contentChanged();
}

void NativeSceneObserver::sceneDestroyed() {
    // A queued destruction of the previous sender may arrive after a retry
    // attached a new Scene for the same page. Its QPointer is already null;
    // never invalidate a newly attached live sender because of that event.
    if (!m_scene) detach();
}

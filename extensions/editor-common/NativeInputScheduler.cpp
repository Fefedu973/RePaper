#include "NativeInputScheduler.h"
#include <QMouseEvent>
#include <QQuickWindow>
#include <QCoreApplication>
#include <QThread>
#include <cmath>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__linux__)
#include <QDir>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

struct NativePenContactState {
    std::mutex mutex;
    std::condition_variable delivered;
    bool active = true;
    bool captureEnabled = false;
    quint64 requested = 0, completed = 0;
    NativeInputScheduler::NativeToolProbe probe;
    std::function<void(NativeInputScheduler::NativeTool)> apply; // GUI thread only.
};

// Xochitl's Digitizer emits PenInput.penDownChanged directly on its input
// thread BEFORE it locks and chooses the stroke region. All later Qt events
// carry one fixed Pen device, including the eraser. Classify and publish the
// region here so the first eraser point already follows Xochitl's native path.
// One relay per native PenInput outlives page hosts. It retains only weak
// subscriptions, so page changes cannot accumulate receivers or host closures.
class NativePenContactRelay final : public QObject {
    Q_OBJECT
public:
    explicit NativePenContactRelay(QObject *input) : QObject(qApp), m_input(input) {
        setObjectName(QStringLiteral("repaperNativePenContactRelay"));
        m_connection = connect(input, SIGNAL(penDownChanged(bool)), this,
                               SLOT(penDownChanged(bool)), Qt::DirectConnection);
    }
    QObject *input() const { return m_input; } // GUI thread only.
    bool connected() const { return bool(m_connection); }
    void subscribe(const std::shared_ptr<NativePenContactState> &state) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_states.erase(std::remove_if(m_states.begin(), m_states.end(),
            [](const auto &entry) { return entry.expired(); }), m_states.end());
        m_states.push_back(state);
    }
public slots:
    void penDownChanged(bool down) {
        if (!down) return; // Native routing/release is still pending at false.
        std::vector<std::shared_ptr<NativePenContactState>> states;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto &entry : m_states) if (auto state = entry.lock()) states.push_back(std::move(state));
        }
        for (const auto &state : states) deliverContact(state);
    }
private:
    static void deliverContact(const std::shared_ptr<NativePenContactState> &state) {
        quint64 ticket;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->active || !state->captureEnabled) return;
            ticket = ++state->requested;
        }
        const auto tool = state->probe();
        const auto deliver = [state, ticket, tool] {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->active) return;
            }
            state->apply(tool);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = qMax(state->completed, ticket);
            }
            state->delivered.notify_all();
        };
        if (QThread::currentThread() == qApp->thread()) deliver();
        else {
            if (!QMetaObject::invokeMethod(qApp, deliver, Qt::QueuedConnection)) return;
            std::unique_lock<std::mutex> lock(state->mutex);
            // Unlike BlockingQueuedConnection, detach/destruction/aboutToQuit
            // explicitly wake this wait without requiring another GUI event.
            state->delivered.wait(lock, [&] { return !state->active || state->completed >= ticket; });
        }
    }
    QPointer<QObject> m_input;
    QMetaObject::Connection m_connection;
    std::mutex m_mutex;
    std::vector<std::weak_ptr<NativePenContactState>> m_states;
};

namespace {
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__linux__)
NativeInputScheduler::NativeTool readNativeTool() {
    using Tool = NativeInputScheduler::NativeTool;
    constexpr unsigned bitsPerWord = sizeof(unsigned long) * 8;
    const auto set = [](const unsigned long *bits, unsigned key) {
        return (bits[key / bitsPerWord] & (1UL << (key % bitsPerWord))) != 0;
    };
    Tool result = Tool::Unknown;
    int devices = 0;
    const auto paths = QDir(QStringLiteral("/dev/input")).entryList({QStringLiteral("event*")}, QDir::System | QDir::Files);
    for (const auto &path : paths) {
        const auto name = (QStringLiteral("/dev/input/") + path).toLocal8Bit();
        const int fd = ::open(name.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long capabilities[(KEY_MAX / bitsPerWord) + 1] = {};
        unsigned long keys[(KEY_MAX / bitsPerWord) + 1] = {};
        const bool digitizer = ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(capabilities)), capabilities) >= 0
            && set(capabilities, BTN_TOOL_PEN) && set(capabilities, BTN_TOOL_RUBBER);
        if (digitizer) {
            ++devices;
            if (::ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
                if (set(keys, BTN_TOOL_RUBBER)) result = Tool::Eraser;
                else if (set(keys, BTN_TOOL_PEN) && set(keys, BTN_TOUCH)) result = Tool::Pen;
                else result = Tool::Unknown;
            } else result = Tool::Unknown;
        }
        ::close(fd);
    }
    // Ambiguous or unavailable classification always yields native ownership.
    // This reads current kernel state, without consuming/grabbing native input.
    return devices == 1 ? result : Tool::Unknown;
}
#endif
}

NativeInputScheduler::NativeInputScheduler(QObject *parent) : QObject(parent) {
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__linux__)
    m_nativeToolProbe = readNativeTool;
    m_nativePenAllowed = false;
#endif
    connect(&m_pointerGuard, &NativeTouchGuard::penCaptureAllowedChanged, this, &NativeInputScheduler::captureChanged);
    connect(&m_pointerGuard, &NativeTouchGuard::cancelRequested, this, [this] {
        if(!m_enabled&&!m_active)return;
        cancel();emit cancelRequested();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &NativeInputScheduler::disconnectNativePenInput);
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(FrameIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &NativeInputScheduler::flush);
    m_pending.reserve(MaximumBatchSize);
}

NativeInputScheduler::~NativeInputScheduler() {
    disconnectNativePenInput();
    if (m_target) m_target->removeEventFilter(this);
}

void NativeInputScheduler::setTarget(QQuickItem *target) {
    if (m_target == target) return;
    cancel();
    if (m_target) {
        m_target->removeEventFilter(this);
        disconnect(m_target, nullptr, this, nullptr);
    }
    m_target = target;
    m_nativeRegionPublished=false;
    m_pointerGuard.attach(target);
    if (target) {
        target->installEventFilter(this);
        connect(target, &QQuickItem::windowChanged, this, &NativeInputScheduler::cancel);
        connect(target, &QObject::destroyed, this, &NativeInputScheduler::cancel);
    }
    emit targetChanged();
}

void NativeInputScheduler::setCoordinateItem(QQuickItem *item) {
    if (m_coordinateItem == item) return;
    cancel();
    m_coordinateItem = item;
    emit coordinateItemChanged();
}

void NativeInputScheduler::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) cancel();
    if (m_nativeContact) {
        std::lock_guard<std::mutex> lock(m_nativeContact->mutex);
        m_nativeContact->captureEnabled = enabled;
    }
    QPointer<NativeInputScheduler> alive(this);
    if (!enabled && m_nativeToolProbe) applyNativeContact(NativeTool::Unknown);
    if (!alive) return;
    emit enabledChanged();
}

void NativeInputScheduler::begin() {
    cancel();
    m_active = m_enabled && penCaptureAllowed() && m_target && m_coordinateItem;
}

void NativeInputScheduler::queueMove(qreal x, qreal y) {
    if (!m_active || !m_enabled || !penCaptureAllowed() || !m_target || !m_coordinateItem
        || !std::isfinite(x) || !std::isfinite(y)) return;
    m_pending.append(QVariantMap{{"x", x}, {"y", y}});
    // Never retain an unbounded event backlog or throw away a freehand corner.
    if (m_pending.size() >= MaximumBatchSize) flush();
    else if (!m_timer.isActive()) m_timer.start();
}

void NativeInputScheduler::flush() {
    m_timer.stop();
    if (!m_active || m_pending.isEmpty()) return;
    const QVariantList batch = std::move(m_pending);
    m_pending = {};
    m_pending.reserve(MaximumBatchSize);
    emit moveBatch(batch);
}

void NativeInputScheduler::finish() {
    // The exact release point follows this synchronous batch in pointerEnd.
    flush();
    m_active = false;
}

void NativeInputScheduler::cancel() {
    m_timer.stop();
    m_pending.clear();
    m_active = false;
}

bool NativeInputScheduler::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_target || !m_enabled) return false;
    if (!penCaptureAllowed()) { cancel(); return false; }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const auto type = mouse->pointingDevice()->pointerType();
        if (type != QPointingDevice::PointerType::Pen && type != QPointingDevice::PointerType::Eraser
            && (mouse->source() == Qt::MouseEventSynthesizedByQt || mouse->source() == Qt::MouseEventSynthesizedBySystem)) {
            // Reject Qt's finger-to-mouse fallback before MouseArea can grab it.
            // The original QTouchEvent and native recognizer are never rerouted.
            event->ignore();
            return true;
        }
    }
    if (event->type() == QEvent::MouseMove && m_active && m_coordinateItem) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const QPointF point = m_target->mapToItem(m_coordinateItem, mouse->position());
        queueMove(point.x(), point.y());
        // Stop per-sample MouseArea positionChanged/QML state work, while keeping
        // its native press/release/grab lifecycle intact. No pressed-state gate:
        // PenInputBlocker forwards native moves as ordinary QMouseEvents.
        event->accept();
        return true;
    }
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::TouchCancel
        || event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate)
        cancel();
    return false;
}

void NativeInputScheduler::captureChanged() {
    QPointer<NativeInputScheduler> alive(this);
    if (m_enabled&&!penCaptureAllowed()) { cancel(); emit cancelRequested(); }
    if (!alive) return;
    emit penCaptureAllowedChanged();
}

void NativeInputScheduler::disconnectNativePenInput() {
    disconnect(m_nativeDestroyedConnection);
    m_publishedSurfaceManager.clear();m_nativeRegionPublished=false;
    if (!m_nativeContact) return;
    {
        std::lock_guard<std::mutex> lock(m_nativeContact->mutex);
        m_nativeContact->active = false;
    }
    m_nativeContact->delivered.notify_all();
    m_nativeContact.reset();
}

void NativeInputScheduler::setNativePenInput(QObject *input) {
    if (m_nativePenInput == input) return;
    disconnectNativePenInput();
    m_nativePenInput = input;
    QPointer<NativeInputScheduler> alive(this);
    connectNativePenInput();
    if (!alive) return;
    emit nativePenInputChanged();
}

void NativeInputScheduler::setNativeToolProbe(NativeToolProbe probe) {
    disconnectNativePenInput();
    m_nativeToolProbe = std::move(probe);
    connectNativePenInput();
}

void NativeInputScheduler::connectNativePenInput() {
    if (!m_nativeToolProbe) {
        m_nativePenAllowed = true;
        captureChanged();
        return;
    }
    // Start closed until a positively identified tip reaches the native gate.
    m_nativePenAllowed = false;
    QPointer<NativeInputScheduler> alive(this);
    captureChanged();
    if (!alive) return;
    if (!m_nativePenInput) return;
    if (m_nativePenInput->metaObject()->indexOfSignal("penDownChanged(bool)") < 0) return;
    m_nativeContact = std::make_shared<NativePenContactState>();
    m_nativeContact->probe = m_nativeToolProbe;
    m_nativeContact->captureEnabled = m_enabled;
    m_nativeContact->apply = [this](NativeTool tool) { applyNativeContact(tool); };
    NativePenContactRelay *relay = nullptr;
    const auto relays = qApp->findChildren<NativePenContactRelay *>(QStringLiteral("repaperNativePenContactRelay"), Qt::FindDirectChildrenOnly);
    for (auto *existing : relays) if (existing->input() == m_nativePenInput) { relay = existing; break; }
    if (!relay) relay = new NativePenContactRelay(m_nativePenInput);
    if (!relay->connected()) { disconnectNativePenInput(); return; }
    relay->subscribe(m_nativeContact);
    m_nativeDestroyedConnection = connect(m_nativePenInput, &QObject::destroyed, this, [this] {
        disconnectNativePenInput();
        m_nativePenAllowed = false;
        captureChanged();
    });
}

void NativeInputScheduler::applyNativeContact(NativeTool tool) {
    QPointer<NativeInputScheduler> alive(this);
    QPointer<QObject> manager = m_nativePenInput ? m_nativePenInput->property("surfaceManager").value<QObject *>() : nullptr;
    const bool regionContract = manager && manager->metaObject()->indexOfMethod("updateRegions()") >= 0;
    const bool allowed = m_enabled && tool == NativeTool::Pen && regionContract;
    const bool captureBefore=penCaptureAllowed();
    const bool routingChanged=m_nativePenAllowed!=allowed;
    if (allowed) m_pointerGuard.confirmNativePenTip();
    if (!alive) return;
    if (m_nativePenAllowed != allowed) {
        m_nativePenAllowed = allowed;
        captureChanged();
    }
    if (!alive) return;
    // Publish OUR routing changes before Digitizer chooses the first point.
    // Unchanged consecutive pen contacts must not rebuild Xochitl's complete
    // item/surface tree. Native-owned geometry changes retain its own lifecycle.
    const bool publish=!m_nativeRegionPublished||m_publishedSurfaceManager!=manager
        ||routingChanged||captureBefore!=penCaptureAllowed();
    if(regionContract&&publish) {
        const bool updated=QMetaObject::invokeMethod(manager,"updateRegions",Qt::DirectConnection);
        if(!alive)return;
        if(updated){m_publishedSurfaceManager=manager;m_nativeRegionPublished=true;}
        else {
            m_nativeRegionPublished=false;m_nativePenAllowed=false;
            captureChanged();
        }
    }
}

#include "NativeInputScheduler.moc"

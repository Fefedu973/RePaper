#pragma once
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QVariantList>
#include "NativeTouchGuard.h"
#include <functional>
#include <memory>

struct NativePenContactState;

// Batch forwarded digitizer mouse moves before crossing into QML and rebuilding
// editor state. A short timer bounds latency; a full queue flushes without loss.
class NativeInputScheduler : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickItem* target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(QQuickItem* coordinateItem READ coordinateItem WRITE setCoordinateItem NOTIFY coordinateItemChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QObject* nativePenInput READ nativePenInput WRITE setNativePenInput NOTIFY nativePenInputChanged)
    Q_PROPERTY(bool penCaptureAllowed READ penCaptureAllowed NOTIFY penCaptureAllowedChanged)
public:
    enum class NativeTool { Unknown, Pen, Eraser };
    using NativeToolProbe = std::function<NativeTool()>;
    explicit NativeInputScheduler(QObject *parent = nullptr);
    ~NativeInputScheduler() override;
    QQuickItem *target() const { return m_target; }
    QQuickItem *coordinateItem() const { return m_coordinateItem; }
    bool enabled() const { return m_enabled; }
    QObject *nativePenInput() const { return m_nativePenInput; }
    bool penCaptureAllowed() const { return m_nativePenAllowed && m_pointerGuard.penCaptureAllowed(); }
    void setTarget(QQuickItem *target);
    void setCoordinateItem(QQuickItem *item);
    void setEnabled(bool enabled);
    void setNativePenInput(QObject *input);
    // Injectable read-only device classifier; the production probe uses EVIOCGKEY.
    void setNativeToolProbe(NativeToolProbe probe);
    Q_INVOKABLE void begin();
    Q_INVOKABLE void queueMove(qreal x, qreal y);
    Q_INVOKABLE void finish();
    Q_INVOKABLE void flush();
    Q_INVOKABLE void cancel();
    static constexpr int MaximumBatchSize = 256;
    static constexpr int FrameIntervalMs = 16;
signals:
    void targetChanged();
    void coordinateItemChanged();
    void enabledChanged();
    void nativePenInputChanged();
    void penCaptureAllowedChanged();
    void cancelRequested();
    void moveBatch(const QVariantList &points);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void disconnectNativePenInput();
    void connectNativePenInput();
    void applyNativeContact(NativeTool tool);
    void captureChanged();
    NativeTouchGuard m_pointerGuard;
    QPointer<QObject> m_nativePenInput;
    QPointer<QObject> m_publishedSurfaceManager;
    bool m_nativeRegionPublished=false;
    QMetaObject::Connection m_nativeDestroyedConnection;
    std::shared_ptr<NativePenContactState> m_nativeContact;
    NativeToolProbe m_nativeToolProbe;
    QPointer<QQuickItem> m_target;
    QPointer<QQuickItem> m_coordinateItem;
    QTimer m_timer;
    QVariantList m_pending;
    bool m_enabled = true;
    bool m_active = false;
    bool m_nativePenAllowed = true;
};

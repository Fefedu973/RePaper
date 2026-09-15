#pragma once
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QVariantList>
#include <memory>

class QQuickWindow;
class QWindow;

// Let the existing firmware TouchArea arbitrate canvas touches even when our
// MouseArea is above it. The endpoint is never shown or backed by a native window.
class NativeTouchRelay : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickItem* surface READ surface WRITE setSurface NOTIFY surfaceChanged)
    Q_PROPERTY(QObject* recognizer READ recognizer WRITE setRecognizer NOTIFY recognizerChanged)
    Q_PROPERTY(QObject* eventTarget READ eventTarget CONSTANT)
    Q_PROPERTY(QVariantList excludedItems READ excludedItems WRITE setExcludedItems NOTIFY excludedItemsChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
public:
    explicit NativeTouchRelay(QObject *parent = nullptr);
    ~NativeTouchRelay() override;
    QQuickItem *surface() const { return m_surface; }
    QObject *recognizer() const { return m_recognizer; }
    QObject *eventTarget() const;
    QVariantList excludedItems() const { return m_excludedItems; }
    bool enabled() const { return m_enabled; }
    void setSurface(QQuickItem *surface);
    void setRecognizer(QObject *recognizer);
    void setExcludedItems(const QVariantList &items);
    void setEnabled(bool enabled);
    Q_INVOKABLE void cancel();
signals:
    void surfaceChanged();
    void recognizerChanged();
    void excludedItemsChanged();
    void enabledChanged();
    void cancelRequested();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void setWindow(QQuickWindow *window);
    bool acceptsOrigin(const QPointF &scenePosition) const;
    QPointer<QQuickItem> m_surface;
    QPointer<QObject> m_recognizer;
    QPointer<QQuickWindow> m_window;
    std::unique_ptr<QWindow> m_endpoint;
    QVariantList m_excludedItems;
    bool m_enabled = false;
    bool m_sequence = false;
    bool m_adopted = false;
    bool m_deliveringCancel = false;
};

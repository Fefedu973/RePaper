#pragma once
#include "NativePreviewFrame.h"
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVariantList>

// Temporary view-space ink only. Geometry follows the visible ink rectangle:
// native PenInputSurfaceManager classifies the entire ItemHasContents bounds.
class NativePreviewItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QVariantList strokes READ strokes WRITE setStrokes NOTIFY strokesChanged)
    Q_PROPERTY(QVariant previewFrame READ previewFrame WRITE setPreviewFrame NOTIFY strokesChanged)
    Q_PROPERTY(bool hasStrokes READ hasStrokes NOTIFY strokesChanged)
    Q_PROPERTY(QRectF contentBounds READ contentBounds NOTIFY contentBoundsChanged)
    Q_PROPERTY(QRectF viewportRect READ viewportRect WRITE setViewportRect NOTIFY viewportRectChanged)
public:
    explicit NativePreviewItem(QQuickItem *parent = nullptr);
    QVariantList strokes() const { return m_legacyRequested; }
    QVariant previewFrame() const { return QVariant::fromValue(m_requestedFrame); }
    bool hasStrokes() const { return !m_contentBounds.isEmpty(); }
    QRectF contentBounds() const { return m_contentBounds; }
    QRectF viewportRect() const { return m_viewportRect; }
    void setViewportRect(const QRectF &viewport);
    void setStrokes(const QVariantList &strokes);
    void setPreviewFrame(const QVariant &frame);
    void paint(QPainter *painter) override;
signals:
    void strokesChanged();
    void contentBoundsChanged();
    void viewportRectChanged();
    // Previous/new ink union in host view coordinates, not a display receipt.
    // QQuickPaintedItem::update itself receives only local backing-store pixels.
    void frameRequested(QRect dirty);
private:
    void scheduleFrame(bool empty);
    void flushFrame();
    void applyFrame(const RePaperNative::PreviewFrame &frame);
    QVariantList m_legacyRequested;
    RePaperNative::PreviewFrame m_requestedFrame, m_frame;
    QRectF m_contentBounds, m_viewportRect;
    QTimer m_frameTimer;
    bool m_framePending = false, m_requestIsTyped = true;
};

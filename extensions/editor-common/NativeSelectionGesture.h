#pragma once
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVector>

// Pure interaction geometry for an existing native selection. No scene calls,
// object identities, document storage, or incremental mutation are performed.
class NativeSelectionGesture {
public:
    enum class Handle { None, Move, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left, Rotate };
    static constexpr qreal MinimumDimension=1;
    static constexpr qreal CoordinateLimit=1000000;
    static constexpr qreal RotationHandleOffsetPixels=36;

    // Points are in paper coordinates; viewScale converts the screen-space hit
    // radius to paper units. Handle order is clockwise, starting at top-left.
    static QVector<QPointF> handlePoints(const QRectF &rect);
    static QPointF rotationHandlePoint(const QRectF &rect,qreal viewScale);
    static QString handleName(Handle handle);
    static Handle hitTest(const QRectF &rect,QPointF point,qreal viewScale,qreal radiusPixels=14);

    // A null pageBounds means the global finite-coordinate limits only.
    // An explicit bound must fully contain the starting selection.
    bool begin(const QRectF &rect,QPointF point,qreal viewScale,
               const QRectF &pageBounds={},qreal radiusPixels=14);
    bool update(QPointF point);
    // Finish retains the final preview for the caller's ONE native operation.
    // An invalid final point cancels, restoring the original rectangle.
    bool finish(QPointF point);
    void cancel();

    bool active() const { return m_active; }
    bool canLockAspectRatio() const;
    bool lockAspectRatio();
    bool aspectRatioLocked() const { return m_aspectRatioLocked; }
    Handle handle() const { return m_handle; }
    QRectF startRect() const { return m_startRect; }
    QRectF previewRect() const { return m_previewRect; }
    QTransform previewTransform() const;
    QPointF anchor() const { return m_anchor; }
    qreal rotationAngleDegrees() const { return m_rotationAngleDegrees; }
    qreal scaleX() const;
    qreal scaleY() const;
    QPointF delta() const { return m_handle==Handle::Rotate?QPointF():m_previewRect.topLeft()-m_startRect.topLeft(); }
private:
    bool m_active=false,m_aspectRatioLocked=false;
    Handle m_handle=Handle::None;
    QRectF m_startRect,m_previewRect,m_limits;
    QPointF m_startPoint,m_anchor;
    qreal m_rotationAngleDegrees=0;
};

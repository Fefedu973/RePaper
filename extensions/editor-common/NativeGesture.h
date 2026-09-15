#pragma once
#include "DrawingModel.h"
#include "AlignmentGuide.h"
#include <QVariantMap>

// Only the temporary gesture. There is deliberately no second document store.
class NativeGesture {
public:
    QString tool="line", style="solid", arrowDirection="end", symbolId;
    QVariantMap stencilParameters;
    qreal width=3;
    QColor color=Qt::black;
    bool horizontalFirst=true;
    int symbolQuarterTurns=0;
    bool voltageArrow=false,voltageArrowReversed=false,voltageArrowOtherSide=false;
    qreal alignmentTolerance=0;
    qreal stencilTapTolerance=4;
    repaper::drawing::Document wireDocument;
    repaper::drawing::Attachment wireStart,wireEnd;
    bool active() const { return m_active; }
    bool canLockAspectRatio() const;
    bool lockAspectRatio();
    bool aspectRatioLocked() const { return m_aspectRatioLocked; }
    bool begin(QPointF point);
    bool move(QPointF point);
    bool moveBatch(const QVector<QPointF> &points);
    bool finish(QPointF point, repaper::drawing::Item *result);
    QVariantMap inputSummary(QPointF release) const;
    void cancel();
    const QVector<PaperDrawing::Stroke> &preview() const { return m_item.strokes; }
    const QVector<repaper::drawing::AlignmentGuide> &alignmentGuides() const { return m_alignmentGuides; }
    const repaper::drawing::StencilPlacementResult &stencilPlacement() const { return m_stencilPlacement; }
private:
    bool rebuild(QPointF point);
    bool m_active=false,m_aspectRatioLocked=false,m_wireRoutePending=false;
    QPointF m_start,m_lastMove;
    int m_moveSamples=0,m_wireDeferredMoveSamples=0;
    qreal m_maxDistance=0;
    repaper::drawing::Item m_item;
    QVector<repaper::drawing::AlignmentGuide> m_alignmentGuides;
    repaper::drawing::StencilPlacementResult m_stencilPlacement;
    bool m_stencilPlacementInitialized=false;
};

#pragma once
#include "DrawingModel.h"
#include "AlignmentGuide.h"
#include <QVariantMap>

// Parametric, pen-only editing geometry. It owns no native IDs and performs no
// scene mutation. The caller commits the final model once at pen release.
class NativeObjectGesture {
public:
    repaper::drawing::Document alignmentDocument;
    qreal alignmentTolerance=0;
    const QVector<repaper::drawing::AlignmentGuide> &alignmentGuides() const { return m_alignmentGuides; }
    struct Control { QPointF point; QString kind; int index = -1; };
    static bool supported(const repaper::drawing::Item &item);
    static QVector<QPointF> outline(const repaper::drawing::Item &item);
    static QVector<Control> controls(const repaper::drawing::Item &item);
    static QPointF rotationPoint(const repaper::drawing::Item &item,qreal viewScale);
    static bool transformGeometry(repaper::drawing::Item &item,const QTransform &transform);
    static bool resizeBox(repaper::drawing::Item &item,qreal width,qreal height);
    bool begin(const repaper::drawing::Item &item,QPointF point,qreal viewScale);
    bool update(QPointF point);
    bool finish(QPointF point);
    void cancel();
    bool active() const { return m_active; }
    bool canLockAspectRatio() const;
    bool lockAspectRatio();
    bool aspectRatioLocked() const { return m_aspectRatioLocked; }
    QString kind() const { return m_control.kind; }
    int endpointIndex() const { return m_control.kind=="endpoint"?m_control.index:-1; }
    const repaper::drawing::Item &preview() const { return m_preview; }
    QVariantMap affineChange() const;
    QPointF endpointCandidate(QPointF pointer) const;
    QPointF pointerForEndpoint(QPointF endpoint) const;
private:
    repaper::drawing::Item m_original,m_preview;
    Control m_control;
    QPointF m_start,m_anchor,m_delta;
    qreal m_angle=0;
    bool m_active=false,m_aspectRatioLocked=false;
    QVector<repaper::drawing::AlignmentGuide> m_alignmentGuides;
};

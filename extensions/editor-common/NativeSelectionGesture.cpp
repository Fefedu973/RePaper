#include "NativeSelectionGesture.h"
#include <QLineF>
#include <cmath>
#include <limits>

namespace {
using Handle=NativeSelectionGesture::Handle;
bool finite(QPointF point) {
    return std::isfinite(point.x())&&std::isfinite(point.y())
        &&std::abs(point.x())<=NativeSelectionGesture::CoordinateLimit
        &&std::abs(point.y())<=NativeSelectionGesture::CoordinateLimit;
}
bool validRect(const QRectF &rect) {
    return finite(rect.topLeft())&&finite(rect.bottomRight())
        &&rect.width()>=NativeSelectionGesture::MinimumDimension
        &&rect.height()>=NativeSelectionGesture::MinimumDimension;
}
bool left(Handle h){return h==Handle::TopLeft||h==Handle::Left||h==Handle::BottomLeft;}
bool right(Handle h){return h==Handle::TopRight||h==Handle::Right||h==Handle::BottomRight;}
bool top(Handle h){return h==Handle::TopLeft||h==Handle::Top||h==Handle::TopRight;}
bool bottom(Handle h){return h==Handle::BottomLeft||h==Handle::Bottom||h==Handle::BottomRight;}
QTransform rotation(QPointF anchor,qreal angle) {
    QTransform transform;
    transform.translate(anchor.x(),anchor.y());
    transform.rotate(angle);
    transform.translate(-anchor.x(),-anchor.y());
    return transform;
}
}

QVector<QPointF> NativeSelectionGesture::handlePoints(const QRectF &rect) {
    if(!validRect(rect))return {};
    const auto center=rect.center();
    return {rect.topLeft(),{center.x(),rect.top()},rect.topRight(),{rect.right(),center.y()},
        rect.bottomRight(),{center.x(),rect.bottom()},rect.bottomLeft(),{rect.left(),center.y()}};
}
QPointF NativeSelectionGesture::rotationHandlePoint(const QRectF &rect,qreal viewScale) {
    if(!validRect(rect)||!std::isfinite(viewScale)||viewScale<=0)return {};
    return {rect.center().x(),rect.top()-RotationHandleOffsetPixels/viewScale};
}
QString NativeSelectionGesture::handleName(Handle handle) {
    switch(handle){
    case Handle::TopLeft:return "top-left";
    case Handle::Top:return "top";
    case Handle::TopRight:return "top-right";
    case Handle::Right:return "right";
    case Handle::BottomRight:return "bottom-right";
    case Handle::Bottom:return "bottom";
    case Handle::BottomLeft:return "bottom-left";
    case Handle::Left:return "left";
    case Handle::Move:return "move";
    case Handle::Rotate:return "rotate";
    case Handle::None:return {};
    }
    return {};
}
NativeSelectionGesture::Handle NativeSelectionGesture::hitTest(const QRectF &rect,QPointF point,
                                                               qreal viewScale,qreal radiusPixels) {
    if(!validRect(rect)||!finite(point)||!std::isfinite(viewScale)||viewScale<=0
        ||!std::isfinite(radiusPixels)||radiusPixels<=0||radiusPixels>512)return Handle::None;
    const qreal radius=radiusPixels/viewScale;
    if(!std::isfinite(radius))return Handle::None;
    const auto points=handlePoints(rect);
    const Handle handles[]={Handle::TopLeft,Handle::Top,Handle::TopRight,Handle::Right,
        Handle::BottomRight,Handle::Bottom,Handle::BottomLeft,Handle::Left};
    qreal nearest=std::numeric_limits<qreal>::infinity();Handle result=Handle::None;
    for(qsizetype i=0;i<points.size();++i){
        const qreal distance=QLineF(point,points[i]).length();
        if(distance<=radius&&distance<nearest){nearest=distance;result=handles[i];}
    }
    const auto rotationPoint=rotationHandlePoint(rect,viewScale);
    const qreal rotationDistance=QLineF(point,rotationPoint).length();
    if(finite(rotationPoint)&&rotationDistance<=radius&&rotationDistance<nearest)result=Handle::Rotate;
    if(result!=Handle::None)return result;
    return rect.contains(point)?Handle::Move:Handle::None;
}
bool NativeSelectionGesture::begin(const QRectF &rect,QPointF point,qreal viewScale,
                                   const QRectF &pageBounds,qreal radiusPixels) {
    cancel();
    const auto handle=hitTest(rect,point,viewScale,radiusPixels);
    if(handle==Handle::None)return false;
    const QRectF global(-CoordinateLimit,-CoordinateLimit,2*CoordinateLimit,2*CoordinateLimit);
    QRectF limits=global;
    if(!pageBounds.isNull()){
        if(!validRect(pageBounds))return false;
        limits=pageBounds.intersected(global);
    }
    if(!limits.contains(rect))return false;
    m_startRect=rect;m_previewRect=rect;m_startPoint=point;m_limits=limits;m_handle=handle;
    m_anchor={left(handle)?rect.right():(right(handle)?rect.left():rect.center().x()),
              top(handle)?rect.bottom():(bottom(handle)?rect.top():rect.center().y())};
    m_active=true;return true;
}
bool NativeSelectionGesture::canLockAspectRatio() const {
    return m_active&&(left(m_handle)||right(m_handle)||top(m_handle)||bottom(m_handle));
}
bool NativeSelectionGesture::lockAspectRatio() {
    if(!canLockAspectRatio())return false;
    m_aspectRatioLocked=true;return true;
}
bool NativeSelectionGesture::update(QPointF point) {
    if(!m_active||!finite(point))return false;
    const QPointF change=point-m_startPoint;
    if(m_handle==Handle::Rotate){
        const auto initial=m_startPoint-m_anchor,current=point-m_anchor;
        // Crossing the center has no defined angle. Retain the last preview
        // until the pointer moves away, so the gesture does not jump or cancel.
        if(QLineF(point,m_anchor).length()<0.000001)return true;
        constexpr qreal RadiansToDegrees=180.0/3.14159265358979323846;
        const qreal angle=std::remainder((std::atan2(current.y(),current.x())-
            std::atan2(initial.y(),initial.x()))*RadiansToDegrees,360.0);
        const auto rect=rotation(m_anchor,angle).mapRect(m_startRect);
        // A rotation cannot be clamped independently along either axis. Keep
        // the most recent valid orientation when its bounds reach the limit.
        if(validRect(rect)&&m_limits.contains(rect)){
            m_rotationAngleDegrees=angle;m_previewRect=rect;
        }
    }else if(m_handle==Handle::Move){
        const qreal dx=qBound(m_limits.left()-m_startRect.left(),change.x(),m_limits.right()-m_startRect.right());
        const qreal dy=qBound(m_limits.top()-m_startRect.top(),change.y(),m_limits.bottom()-m_startRect.bottom());
        m_previewRect=m_startRect.translated(dx,dy);
    }else if(m_aspectRatioLocked){
        const qreal width=m_startRect.width(),height=m_startRect.height();
        const qreal sx=1+(left(m_handle)?-change.x():change.x())/width;
        const qreal sy=1+(top(m_handle)?-change.y():change.y())/height;
        qreal scale=!(left(m_handle)||right(m_handle))?sy:!(top(m_handle)||bottom(m_handle))?sx:
            (std::abs(sx-1)>=std::abs(sy-1)?sx:sy);
        qreal maxScale=std::numeric_limits<qreal>::infinity();
        const qreal toLeft=m_anchor.x()-m_startRect.left(),toRight=m_startRect.right()-m_anchor.x();
        const qreal toTop=m_anchor.y()-m_startRect.top(),toBottom=m_startRect.bottom()-m_anchor.y();
        if(toLeft>0)maxScale=qMin(maxScale,(m_anchor.x()-m_limits.left())/toLeft);
        if(toRight>0)maxScale=qMin(maxScale,(m_limits.right()-m_anchor.x())/toRight);
        if(toTop>0)maxScale=qMin(maxScale,(m_anchor.y()-m_limits.top())/toTop);
        if(toBottom>0)maxScale=qMin(maxScale,(m_limits.bottom()-m_anchor.y())/toBottom);
        const qreal minScale=qMin(qreal(1),std::nextafter(qMax(MinimumDimension/width,MinimumDimension/height),
                                                        std::numeric_limits<qreal>::infinity()));
        scale=qBound(minScale,scale,maxScale);
        // Side handles scale around the opposite edge's midpoint, so their
        // perpendicular dimension grows symmetrically with one uniform scale.
        m_previewRect=QRectF(m_anchor-QPointF(toLeft*scale,toTop*scale),QSizeF(width*scale,height*scale));
    }else{
        qreal x0=m_startRect.left(),x1=m_startRect.right(),y0=m_startRect.top(),y1=m_startRect.bottom();
        // Each affected edge is independent, with the opposite edge fixed.
        // Crossing is clamped to one paper unit; never mirror a selection.
        if(left(m_handle))x0=qBound(m_limits.left(),x0+change.x(),x1-MinimumDimension);
        if(right(m_handle))x1=qBound(x0+MinimumDimension,x1+change.x(),m_limits.right());
        if(top(m_handle))y0=qBound(m_limits.top(),y0+change.y(),y1-MinimumDimension);
        if(bottom(m_handle))y1=qBound(y0+MinimumDimension,y1+change.y(),m_limits.bottom());
        m_previewRect=QRectF(QPointF(x0,y0),QPointF(x1,y1));
    }
    return validRect(m_previewRect);
}
bool NativeSelectionGesture::finish(QPointF point) {
    if(!update(point)){cancel();return false;}
    m_active=false;return true;
}
void NativeSelectionGesture::cancel() {
    m_active=false;m_aspectRatioLocked=false;m_handle=Handle::None;m_previewRect=m_startRect;m_rotationAngleDegrees=0;
}
qreal NativeSelectionGesture::scaleX() const {
    if(m_handle==Handle::Rotate)return 1;
    return m_startRect.width()>0?m_previewRect.width()/m_startRect.width():1;
}
qreal NativeSelectionGesture::scaleY() const {
    if(m_handle==Handle::Rotate)return 1;
    return m_startRect.height()>0?m_previewRect.height()/m_startRect.height():1;
}
QTransform NativeSelectionGesture::previewTransform() const {
    if(m_handle==Handle::Rotate)return rotation(m_anchor,m_rotationAngleDegrees);
    const qreal sx=scaleX(),sy=scaleY();
    return QTransform(sx,0,0,sy,m_previewRect.left()-m_startRect.left()*sx,
                      m_previewRect.top()-m_startRect.top()*sy);
}

#include "NativeGesture.h"
#include "NativeArrowStroke.h"
#include <QLineF>
#include <cmath>
using namespace repaper::drawing;
namespace {
bool bounded(QPointF p) { return std::isfinite(p.x())&&std::isfinite(p.y())&&std::abs(p.x())<20000&&std::abs(p.y())<20000; }
constexpr qsizetype MaximumSourcePoints = 20000; // DrawingModel::rebuild's existing limit
}
bool NativeGesture::begin(QPointF point) {
    cancel();
    if(!bounded(point)||!color.isValid()||color.alpha()!=255||!QStringList{"pen","line","arrow","wire","rectangle","ellipse","symbol"}.contains(tool))return false;
    m_active=true;m_start=point;m_lastMove=point;m_moveSamples=0;m_maxDistance=0;
    m_item=Item{};m_item.kind=tool;m_item.style=style;
    m_item.width=width;m_item.arrowDirection=arrowDirection;m_item.horizontalFirst=horizontalFirst;m_item.symbolId=symbolId;
    m_item.stencilParameters=tool=="symbol"?stencilParameters:QVariantMap{};
    m_item.voltageArrow=tool=="symbol"&&voltageArrow&&PaperDrawing::supportsVoltageArrow(symbolId);
    const bool voltageSupported=tool=="symbol"&&PaperDrawing::supportsVoltageArrow(symbolId);
    m_item.voltageArrowReversed=voltageSupported&&voltageArrowReversed;m_item.voltageArrowOtherSide=voltageSupported&&voltageArrowOtherSide;
    if(tool=="pen")m_item.sourcePoints={point};
    rebuild(point);return true;
}
bool NativeGesture::canLockAspectRatio() const {
    return m_active&&(tool=="rectangle"||tool=="ellipse"||tool=="symbol");
}
bool NativeGesture::lockAspectRatio() {
    if(!canLockAspectRatio())return false;
    m_aspectRatioLocked=true;return true;
}
bool NativeGesture::rebuild(QPointF point) {
    if(!m_active||!bounded(point))return false;
    m_wireRoutePending=false;
    m_alignmentGuides.clear();
    if(tool=="wire"){
        // A snapped endpoint can still equal the first press, or briefly enter
        // an obstacle's clearance band. Neither condition ends a pen gesture.
        // Build separately so an unroutable sample cannot erase the last valid
        // preview or the preferred route used by the next sample.
        auto candidate=m_item;
        candidate.sourcePoints={m_start,point};
        candidate.startAttachment=wireStart;candidate.endAttachment=wireEnd;
        if(!repaper::drawing::routeWire(candidate,wireDocument)){
            m_wireRoutePending=true;return false;
        }
        if(!withinBudget({candidate},false))return false;
        repaper::drawing::setForegroundColor(candidate,color);
        candidate.strokes=RePaperNative::wholeNativeArrowStrokes(candidate);
        m_item=std::move(candidate);return true;
    }
    const bool stencilTap=tool=="symbol"&&QLineF(m_start,point).length()<stencilTapTolerance
        &&m_maxDistance<stencilTapTolerance;
    if(tool=="pen") {
        if(m_item.sourcePoints.isEmpty()||QLineF(m_item.sourcePoints.constLast(),point).length()>0.3){
            if(m_item.sourcePoints.size()>=MaximumSourcePoints)return false;
            // Release our previous derived alias before appending. External
            // snapshots still retain their own immutable implicit-shared data.
            if(m_item.style=="solid")m_item.strokes.clear();
            m_item.sourcePoints.append(point);
        }
    } else if(tool=="rectangle"||tool=="ellipse"||tool=="symbol") {
        const bool vertical=tool=="symbol"&&symbolQuarterTurns%2;
        if(stencilTap)point=m_start+(vertical?QPointF(80,120):QPointF(120,80));
        if(m_aspectRatioLocked){
            // Catalogue coordinates use the same 120 x 80 nominal box as a tap.
            const qreal ratio=tool=="symbol"?(vertical?1/1.5:1.5):1;
            const auto delta=point-m_start;
            const qreal sx=delta.x()<0?-1:1,sy=delta.y()<0?-1:1;
            qreal height=qMax(std::abs(delta.x())/ratio,std::abs(delta.y()));
            const qreal maxWidth=qMin(qreal(20000),20000-sx*m_start.x());
            const qreal maxHeight=qMin(qreal(20000),20000-sy*m_start.y());
            height=qMin(height,qMin(maxWidth/ratio,maxHeight));
            point=m_start+QPointF(sx*height*ratio,sy*height);
        }
        const QRectF box=QRectF(m_start,point).normalized();
        if(box.width()<1||box.height()<1){m_item.strokes.clear();return true;}
        m_item.sourcePoints=vertical?PaperDrawing::Polyline{box.topRight(),box.bottomRight(),box.topLeft()}
                                   :PaperDrawing::Polyline{box.topLeft(),box.topRight(),box.bottomLeft()};
    } else m_item.sourcePoints={m_start,point};
    if(tool=="pen"&&m_item.style=="solid") {
        // Source samples were checked at begin/moveBatch/release. A solid pen
        // has no generated geometry: rebuilding it used to rescan its entire
        // history for length, coordinates and budget at every input frame.
        if(!std::isfinite(m_item.width)||m_item.width<=0||m_item.width>100
            ||m_item.sourcePoints.size()>MaximumSourcePoints){m_item.strokes.clear();return false;}
        m_item.strokes={{m_item.sourcePoints,m_item.width,color}};
        return true;
    }
    if(!repaper::drawing::rebuild(m_item)){m_item.strokes.clear();return false;}
    if(tool=="symbol"){
        if(!m_stencilPlacementInitialized){
            m_stencilPlacement=placeStencilNearPointer(m_item,m_start,wireDocument,alignmentTolerance);
            m_stencilPlacementInitialized=true;
        }
        QPointF delta;
        if(m_stencilPlacement.snapped){
            const int port=m_item.portIds.indexOf(m_stencilPlacement.localPortId);
            if(port<0||port>=m_item.anchors.size()){m_item.strokes.clear();return false;}
            // Once the first press chooses a port, resizing keeps that exact
            // port on its target; alignment must not pull it away afterwards.
            delta=m_stencilPlacement.targetPoint-m_item.anchors[port];
        }else if(stencilTap)delta=m_start-defaultStencilAnchor(m_item);
        if(!delta.isNull()){
            for(auto &p:m_item.sourcePoints)p+=delta;
            if(!repaper::drawing::rebuild(m_item)){m_item.strokes.clear();return false;}
        }
    }
    if(tool=="symbol"&&alignmentTolerance>0&&!m_stencilPlacement.snapped){
        const auto aligned=repaper::drawing::alignTranslation(m_item,wireDocument,{},alignmentTolerance);
        m_alignmentGuides=aligned.guides;
        if(!aligned.delta.isNull()){
            for(auto &p:m_item.sourcePoints)p+=aligned.delta;
            if(!repaper::drawing::rebuild(m_item)){m_item.strokes.clear();return false;}
        }
    }
    if(!withinBudget({m_item},false)){m_item.strokes.clear();return false;}
    repaper::drawing::setForegroundColor(m_item,color);
    m_item.strokes=RePaperNative::wholeNativeArrowStrokes(m_item);
    return true;
}
bool NativeGesture::move(QPointF point) {
    return moveBatch({point});
}
bool NativeGesture::moveBatch(const QVector<QPointF> &points) {
    if(!m_active||points.isEmpty()||points.size()>256)return false;
    for(const auto &point:points)if(!bounded(point))return false;
    // Keep freehand corners, but regenerate its derived geometry only once per
    // delivered frame. Parametric tools need only the latest endpoint.
    if(tool=="pen"){
        QVector<QPointF> additions;additions.reserve(points.size());
        auto last=m_item.sourcePoints.constLast();
        for(const auto &point:points)if(QLineF(last,point).length()>0.3){additions.append(point);last=point;}
        // Count before mutation so a budget failure preserves the last frame.
        if(additions.size()>MaximumSourcePoints-m_item.sourcePoints.size())return false;
        if(!additions.isEmpty()){
            if(m_item.style=="solid")m_item.strokes.clear();
            m_item.sourcePoints+=additions;
        }
    }
    for(const auto &point:points)m_maxDistance=qMax(m_maxDistance,QLineF(m_start,point).length());
    if(!rebuild(points.last())&&!m_wireRoutePending)return false;
    if(m_wireRoutePending)m_wireDeferredMoveSamples=qMin(m_wireDeferredMoveSamples+int(points.size()),65535);
    m_moveSamples=qMin(m_moveSamples+int(points.size()),65535);m_lastMove=points.last();return true;
}
QVariantMap NativeGesture::inputSummary(QPointF release) const {
    // Bounded aggregate diagnostics only: no positions, stroke path or document
    // content. The final/last sample distances expose a stale release event.
    const auto distance=[](QPointF a,QPointF b){return qBound(0,qRound(QLineF(a,b).length()),1000000);};
    QVariantMap result{{"moveSamples",m_moveSamples},{"maxDistance",qRound(m_maxDistance)},
        {"releaseDistance",distance(m_start,release)},
        {"lastMoveDistance",distance(m_start,m_lastMove)},
        {"releaseToLastMoveDistance",distance(m_lastMove,release)}};
    if(tool=="wire"){
        result.insert("wireDeferredMoveSamples",m_wireDeferredMoveSamples);
        result.insert("wireRoutePending",m_wireRoutePending);
    }
    return result;
}
bool NativeGesture::finish(QPointF point,Item *result) {
    if(!rebuild(point)||m_item.strokes.isEmpty()){cancel();return false;}
    *result=m_item;m_active=false;m_item=Item{};m_alignmentGuides.clear();m_stencilPlacement={};m_stencilPlacementInitialized=false;return true;
}
void NativeGesture::cancel(){m_active=false;m_aspectRatioLocked=false;m_wireRoutePending=false;m_wireDeferredMoveSamples=0;m_item=Item{};m_alignmentGuides.clear();m_stencilPlacement={};m_stencilPlacementInitialized=false;}

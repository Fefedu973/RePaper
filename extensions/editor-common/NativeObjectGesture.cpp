#include "NativeObjectGesture.h"
#include "NativeArrowStroke.h"
#include <QPolygonF>
#include <limits>

namespace {
using namespace repaper::drawing;
bool finite(QPointF point){return std::isfinite(point.x())&&std::isfinite(point.y())&&std::abs(point.x())<=20000&&std::abs(point.y())<=20000;}
QPointF normalized(QPointF point){const auto length=std::hypot(point.x(),point.y());return length>1e-9?point/length:QPointF(1,0);}
QPointF perpendicular(QPointF point){return {-point.y(),point.x()};}
QPointF center(const Item &item){return hasBox(item)?mapBox(item,.5,.5):(item.sourcePoints[0]+item.sourcePoints[1])*.5;}
QPointF wireBendPoint(const Item &item){
    const auto x=normalized(item.wireAxis),y=perpendicular(x),a=item.sourcePoints[0],d=item.sourcePoints[1]-a;
    const qreal dx=QPointF::dotProduct(d,x),dy=QPointF::dotProduct(d,y);
    if(!item.wireHasBend&&std::abs(item.wireBend)<1e-9)
        return a+(item.horizontalFirst?x*dx:y*dy);
    return a+x*(dx*.5+(item.horizontalFirst?item.wireBend:0))+y*(dy*.5+(item.horizontalFirst?0:item.wireBend));
}
}
bool NativeObjectGesture::supported(const Item &item){return hasEndpoints(item)||hasBox(item);}
QVector<QPointF> NativeObjectGesture::outline(const Item &item){
    if(hasBox(item))return boxCorners(item);
    if(!hasEndpoints(item))return {};
    if(item.kind=="wire"){
        auto x=normalized(item.wireAxis),y=perpendicular(x),origin=item.sourcePoints[0];
        qreal left=0,right=0,top=0,bottom=0;
        for(const auto &stroke:item.strokes)for(auto point:stroke.points){
            const auto d=point-origin;const auto px=QPointF::dotProduct(d,x),py=QPointF::dotProduct(d,y);
            left=qMin(left,px);right=qMax(right,px);top=qMin(top,py);bottom=qMax(bottom,py);
        }
        if(right-left<1){left-=.5;right+=.5;}if(bottom-top<1){top-=.5;bottom+=.5;}
        return {origin+x*left+y*top,origin+x*right+y*top,origin+x*right+y*bottom,origin+x*left+y*bottom};
    }
    const auto a=item.sourcePoints[0],b=item.sourcePoints[1],x=normalized(b-a),y=perpendicular(x);
    qreal half=qMax(qreal(1),item.width*.5),left=0,right=QLineF(a,b).length();
    for(const auto &stroke:item.strokes)for(auto point:stroke.points){
        half=qMax(half,std::abs(QPointF::dotProduct(point-a,y)));
        const auto along=QPointF::dotProduct(point-a,x);left=qMin(left,along);right=qMax(right,along);
    }
    return {a+x*left-y*half,a+x*right-y*half,a+x*right+y*half,a+x*left+y*half};
}
QVector<NativeObjectGesture::Control> NativeObjectGesture::controls(const Item &item){
    QVector<Control> result;
    if(hasEndpoints(item)){
        result={{item.sourcePoints[0],"endpoint",0},{item.sourcePoints[1],"endpoint",1}};
        if(item.kind=="wire"){
            if(item.wireRouteMode=="legacy")result.append({wireBendPoint(item),"bend",0});
            else {
                const auto path=wirePoints(item);
                for(int i=1;i+2<path.size();++i)result.append({(path[i]+path[i+1])*.5,"segment",i});
            }
        }
    }else if(hasBox(item)){
        const auto points=boxCorners(item);
        for(int i=0;i<4;++i){result.append({points[i],"resize",i*2});result.append({(points[i]+points[(i+1)%4])*.5,"resize",i*2+1});}
    }
    return result;
}
QPointF NativeObjectGesture::rotationPoint(const Item &item,qreal scale){
    const auto points=outline(item);if(points.size()!=4||!std::isfinite(scale)||scale<=0)return {};
    const auto outward=normalized(points[0]-points[3]);
    return (points[0]+points[1])*.5+outward*(36/scale);
}
bool NativeObjectGesture::transformGeometry(Item &item,const QTransform &transform){
    const auto head=item.headSize,pattern=item.patternScale,radius=item.cornerRadius,width=item.width;
    if(!repaper::drawing::transform(item,transform))return false;
    item.headSize=head;item.patternScale=pattern;item.cornerRadius=radius;item.width=width;
    return RePaperNative::rebuildNativeObject(item);
}
bool NativeObjectGesture::resizeBox(Item &item,qreal width,qreal height){
    if(!hasBox(item)||!std::isfinite(width)||!std::isfinite(height)||width<1||height<1||width>10000||height>10000)return false;
    item.sourcePoints[1]=item.sourcePoints[0]+normalized(item.sourcePoints[1]-item.sourcePoints[0])*width;
    item.sourcePoints[2]=item.sourcePoints[0]+normalized(item.sourcePoints[2]-item.sourcePoints[0])*height;
    return RePaperNative::rebuildNativeObject(item);
}
bool NativeObjectGesture::begin(const Item &item,QPointF point,qreal scale){
    cancel();
    if(!supported(item)||!finite(point)||!std::isfinite(scale)||scale<=0)return false;
    const qreal radius=16/scale; qreal nearest=std::numeric_limits<qreal>::infinity();Control control;
    for(const auto &candidate:controls(item)){
        const qreal distance=QLineF(point,candidate.point).length();
        if(distance<=radius&&distance<nearest){control=candidate;nearest=distance;}
    }
    const auto rotation=rotationPoint(item,scale);
    if(QLineF(point,rotation).length()<=radius&&QLineF(point,rotation).length()<nearest)control={rotation,"rotate",0};
    if(control.kind.isEmpty()){
        const QPolygonF polygon(outline(item));
        bool inside=polygon.containsPoint(point,Qt::OddEvenFill);
        // Thin paths still have a usable move target between their end handles.
        if(!inside&&hasEndpoints(item))for(const auto &stroke:item.strokes)for(int i=1;i<stroke.points.size();++i){
            const auto a=stroke.points[i-1],segment=stroke.points[i]-a;const auto n=QPointF::dotProduct(segment,segment);
            if(n>1e-9&&QLineF(point,a+segment*std::clamp(QPointF::dotProduct(point-a,segment)/n,qreal(0),qreal(1))).length()<=radius*.6)inside=true;
        }
        if(!inside)return false;
        control={point,"move",0};
    }
    m_original=m_preview=item;m_control=control;m_start=point;m_anchor=center(item);m_active=true;return true;
}
bool NativeObjectGesture::canLockAspectRatio() const {
    return m_active&&m_control.kind=="resize"&&hasBox(m_original)
        &&boxWidth(m_original)>=1&&boxHeight(m_original)>=1;
}
bool NativeObjectGesture::lockAspectRatio() {
    if(!canLockAspectRatio())return false;
    m_aspectRatioLocked=true;return true;
}
bool NativeObjectGesture::update(QPointF point){
    if(!m_active||!finite(point))return false;
    auto candidate=m_original;const auto delta=point-m_start;
    if(m_control.kind=="endpoint"){
        candidate.sourcePoints[m_control.index]+=delta;
        if(QLineF(candidate.sourcePoints[0],candidate.sourcePoints[1]).length()<1)return true;
        if(m_control.index==0)candidate.startAttachment={};else candidate.endAttachment={};
    }else if(m_control.kind=="bend"){
        const auto axis=normalized(m_original.wireAxis),direction=m_original.horizontalFirst?axis:perpendicular(axis);
        const auto moved=m_control.point+delta;
        candidate.wireBend=QPointF::dotProduct(moved-center(candidate),direction);candidate.wireHasBend=true;
        candidate.wireRouteMode="legacy";candidate.wireRoute.clear();
    }else if(m_control.kind=="segment"){
        if(!moveWireSegment(candidate,m_control.index,m_control.point+delta))return true;
    }else if(m_control.kind=="resize"){
        const auto origin=m_original.sourcePoints[0],x=normalized(m_original.sourcePoints[1]-origin),y=normalized(m_original.sourcePoints[2]-origin);
        const auto width=boxWidth(m_original),height=boxHeight(m_original);
        const auto dx=QPointF::dotProduct(delta,x),dy=QPointF::dotProduct(delta,y);const int i=m_control.index;
        qreal left=0,right=width,top=0,bottom=height;
        const bool moveLeft=i==0||i==6||i==7,moveRight=i==2||i==3||i==4;
        const bool moveTop=i==0||i==1||i==2,moveBottom=i==4||i==5||i==6;
        if(m_aspectRatioLocked){
            const qreal sx=1+(moveLeft?-dx:dx)/width,sy=1+(moveTop?-dy:dy)/height;
            qreal scale=!(moveLeft||moveRight)?sy:!(moveTop||moveBottom)?sx:
                (std::abs(sx-1)>=std::abs(sy-1)?sx:sy);
            const qreal ax=moveLeft?width:moveRight?0:width*.5;
            const qreal ay=moveTop?height:moveBottom?0:height*.5;
            const auto anchor=origin+x*ax+y*ay;
            qreal maxScale=qMin(20000/width,20000/height);
            // One scale must satisfy every rotated corner's coordinate bounds.
            for(const auto corner:boxCorners(m_original)){
                const auto offset=corner-anchor;
                for(const auto component:{QPointF(anchor.x(),offset.x()),QPointF(anchor.y(),offset.y())}){
                    if(component.y()>0)maxScale=qMin(maxScale,(20000-component.x())/component.y());
                    if(component.y()<0)maxScale=qMin(maxScale,(-20000-component.x())/component.y());
                }
            }
            // Leave numerical headroom when a rotated dimension reaches one.
            const qreal minScale=(1+1e-9)/qMin(width,height);
            if(maxScale<minScale)return true;
            scale=qBound(minScale,scale,maxScale);
            left=ax*(1-scale);right=left+width*scale;
            top=ay*(1-scale);bottom=top+height*scale;
        }else{
            if(moveLeft)left=qMin(dx,width-1);
            if(moveRight)right=qMax(qreal(1),width+dx);
            if(moveTop)top=qMin(dy,height-1);
            if(moveBottom)bottom=qMax(qreal(1),height+dy);
        }
        candidate.sourcePoints={origin+x*left+y*top,origin+x*right+y*top,origin+x*left+y*bottom};
    }else if(m_control.kind=="move"){
        QPointF movement=delta;m_alignmentGuides.clear();
        if(alignmentTolerance>0&&QLineF({},delta).length()>.1){
            const auto aligned=alignTranslation(m_original,alignmentDocument,delta,alignmentTolerance);
            movement=aligned.delta;m_alignmentGuides=aligned.guides;
        }
        QTransform transform;transform.translate(movement.x(),movement.y());
        if(!transformGeometry(candidate,transform))return true;m_delta=movement;
    }else if(m_control.kind=="rotate"){
        const auto initial=m_start-m_anchor,current=point-m_anchor;
        if(QLineF(point,m_anchor).length()<1e-6)return true;
        const auto angle=std::remainder((std::atan2(current.y(),current.x())-std::atan2(initial.y(),initial.x()))*180/3.14159265358979323846,360.);
        QTransform transform;transform.translate(m_anchor.x(),m_anchor.y());transform.rotate(angle);transform.translate(-m_anchor.x(),-m_anchor.y());
        if(!transformGeometry(candidate,transform))return true;m_angle=angle;
    }
    if(!RePaperNative::rebuildNativeObject(candidate)||!withinBudget({candidate},false)||candidate.strokes.size()>128)return true;
    m_preview=std::move(candidate);return true;
}
bool NativeObjectGesture::finish(QPointF point){if(!update(point)){cancel();return false;}m_active=false;return true;}
void NativeObjectGesture::cancel(){m_active=false;m_aspectRatioLocked=false;m_control={};m_delta={};m_angle=0;m_alignmentGuides.clear();}
QVariantMap NativeObjectGesture::affineChange() const {
    if(m_control.kind=="move")return {{"kind","move"},{"delta",m_delta}};
    if(m_control.kind=="rotate")return {{"kind","rotate"},{"anchor",m_anchor},{"angle",m_angle}};
    return {};
}
QPointF NativeObjectGesture::endpointCandidate(QPointF pointer) const {
    return m_control.kind=="endpoint"?m_original.sourcePoints[m_control.index]+pointer-m_start:pointer;
}
QPointF NativeObjectGesture::pointerForEndpoint(QPointF endpoint) const {
    return m_control.kind=="endpoint"?endpoint+m_start-m_original.sourcePoints[m_control.index]:endpoint;
}

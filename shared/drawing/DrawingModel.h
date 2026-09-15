#pragma once

// Parametric document data and rendering, independent of QQuickItem and Xochitl.
// These UUIDs belong to RePaper; they are never claimed to be native scene IDs.
#include "Geometry.h"
#include <QHash>
#include <QLineF>
#include <QSet>
#include <QStringList>
#include <QTransform>
#include <QUuid>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <functional>

namespace repaper::drawing {
using namespace PaperDrawing;
struct Attachment {
    QString objectId, portId;
    // A junction on a wire is stored by normalized distance along its route.
    // Ordinary named ports retain -1, including another wire's start/end.
    qreal wirePosition=-1;
    bool empty() const { return objectId.isEmpty(); }
};
struct Item {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVector<Stroke> strokes;
    QVector<QPointF> anchors;
    QStringList portIds;
    QString kind="legacy", style="solid", arrowDirection="end", symbolId;
    QVariantMap stencilParameters;
    bool voltageArrow=false, voltageArrowReversed=false, voltageArrowOtherSide=false;
    Polyline sourcePoints;
    qreal width=3, headSize=26, patternScale=1, cornerRadius=0, wireBend=0;
    bool horizontalFirst=true;
    bool wireHasBend=false;
    QPointF wireAxis{1,0}; // Local orthogonal routing axis, retained on rotation.
    Polyline wireRoute;
    QString wireRouteMode="auto"; // auto, manual waypoints, or legacy bend parameters.
    Attachment startAttachment, endAttachment;
};
using Document = QVector<Item>;
struct StencilPlacementResult {
    QPointF delta;
    QString localPortId;
    Attachment targetAttachment;
    QPointF targetPoint;
    bool snapped=false;
};

inline bool isBackgroundStroke(const Item &item, qsizetype index) {
    // A lone stroke is also used as a foreground-color seed before rebuilding.
    return index==0 && item.strokes.size()>1 && item.kind=="symbol"
        && isConfigurableStencil(item.symbolId)
        && item.stencilParameters.value("opaqueBackground",true).toBool();
}
inline QColor foregroundColor(const Item &item) {
    QColor color;
    for(qsizetype i=0;i<item.strokes.size();++i)if(!isBackgroundStroke(item,i)) {
        const auto &next=item.strokes[i].color;
        if(!next.isValid() || (color.isValid()&&color!=next))return {};
        color=next;
    }
    return item.strokes.isEmpty()?QColor(Qt::black):color;
}
inline void setForegroundColor(Item &item,const QColor &color) {
    if(item.strokes.isEmpty())item.strokes.append({{},item.width,color});
    else for(qsizetype i=0;i<item.strokes.size();++i)
        if(!isBackgroundStroke(item,i))item.strokes[i].color=color;
}

inline bool hasEndpoints(const Item &item) {
    return QStringList{"line","wire","arrow"}.contains(item.kind) && item.sourcePoints.size()==2;
}
inline bool hasBox(const Item &item) {
    return (item.kind=="rectangle" || item.kind=="ellipse" || (item.kind=="symbol"&&!item.symbolId.isEmpty())) && item.sourcePoints.size()==3;
}
inline bool hasPath(const Item &item) {
    return hasEndpoints(item) || hasBox(item) || (item.kind=="pen"&&!item.sourcePoints.isEmpty());
}
inline qreal boxWidth(const Item &item) { return hasBox(item)?QLineF(item.sourcePoints[0],item.sourcePoints[1]).length():0; }
inline qreal boxHeight(const Item &item) { return hasBox(item)?QLineF(item.sourcePoints[0],item.sourcePoints[2]).length():0; }
inline QPointF mapBox(const Item &item, qreal x, qreal y) {
    return item.sourcePoints[0]+(item.sourcePoints[1]-item.sourcePoints[0])*x+(item.sourcePoints[2]-item.sourcePoints[0])*y;
}
inline QVector<QPointF> boxCorners(const Item &item) {
    if(!hasBox(item))return {};
    return {mapBox(item,0,0),mapBox(item,1,0),mapBox(item,1,1),mapBox(item,0,1)};
}
inline void ensurePorts(Item &item) {
    if(item.portIds.size()==item.anchors.size())return;
    item.portIds.clear();
    for(int i=0;i<item.anchors.size();++i)item.portIds.append(QString("legacy-%1").arg(i+1));
}
inline Polyline legacyWirePoints(const Item &item) {
    if(item.kind!="wire"||!hasEndpoints(item))return {};
    const qreal length=std::hypot(item.wireAxis.x(),item.wireAxis.y());
    if(!std::isfinite(length)||length<1e-9)return {};
    const auto origin=item.sourcePoints[0],axisX=item.wireAxis/length,axisY=QPointF(-axisX.y(),axisX.x());
    const auto difference=item.sourcePoints[1]-origin;
    const QPointF a,b(QPointF::dotProduct(difference,axisX),QPointF::dotProduct(difference,axisY));
    Polyline points;
    if(!item.wireHasBend&&std::abs(item.wireBend)<1e-9)points=orthogonalRoute(a,b,item.horizontalFirst);
    else if(item.horizontalFirst){const qreal bend=b.x()/2+item.wireBend;points={a,{bend,0},{bend,b.y()},b};}
    else {const qreal bend=b.y()/2+item.wireBend;points={a,{0,bend},{b.x(),bend},b};}
    for(auto &p:points)p=origin+axisX*p.x()+axisY*p.y();
    return points;
}
// Undashed points are kept separately from rendering so junctions and handles
// are unaffected by a wire's line style.
inline Polyline wirePoints(const Item &item) {
    if(item.wireRoute.isEmpty()||item.wireRouteMode=="legacy")return legacyWirePoints(item);
    if(item.kind!="wire"||!hasEndpoints(item)||item.wireRoute.size()<2||item.wireRoute.size()>1024)return {};
    const qreal length=std::hypot(item.wireAxis.x(),item.wireAxis.y());
    if(!std::isfinite(length)||length<1e-9)return {};
    const QPointF ax=item.wireAxis/length,ay(-ax.y(),ax.x());
    auto local=[&](QPointF p){return QPointF(QPointF::dotProduct(p,ax),QPointF::dotProduct(p,ay));};
    Polyline original;original.reserve(item.wireRoute.size());
    for(auto p:item.wireRoute) {
        if(!std::isfinite(p.x())||!std::isfinite(p.y())||std::abs(p.x())>20000||std::abs(p.y())>20000)return {};
        original.append(local(p));
    }
    for(int i=1;i<original.size();++i) {
        const auto delta=original[i]-original[i-1];
        if(std::abs(delta.x())>1e-6&&std::abs(delta.y())>1e-6)return {};
    }
    const auto start=local(item.sourcePoints[0]),end=local(item.sourcePoints[1]);
    if(QLineF(start,original.first()).length()<1e-7&&QLineF(end,original.last()).length()<1e-7)return item.wireRoute;
    // Preserve edited inner segments when a connected component moves. Only
    // short orthogonal connectors at either end need to change.
    if(original.size()==2) {
        auto result=orthogonalRoute(start,end,std::abs(original[1].x()-original[0].x())>1e-7);
        for(auto &p:result)p=ax*p.x()+ay*p.y();return result;
    }
    const bool firstHorizontal=std::abs(original[1].x()-original[0].x())>1e-7;
    const bool lastHorizontal=std::abs(original.last().x()-original[original.size()-2].x())>1e-7;
    Polyline points=orthogonalRoute(start,original[1],firstHorizontal);
    for(int i=2;i<original.size()-1;++i)points.append(original[i]);
    const auto tail=orthogonalRoute(original[original.size()-2],end,lastHorizontal);
    for(int i=1;i<tail.size();++i)points.append(tail[i]);
    Polyline result;
    for(auto p:points)if(result.isEmpty()||QLineF(result.last(),ax*p.x()+ay*p.y()).length()>1e-7)result.append(ax*p.x()+ay*p.y());
    return result;
}
inline bool rebuild(Item &item) {
    if(!hasPath(item))return false;
    const bool voltageOptions=item.voltageArrow||item.voltageArrowReversed||item.voltageArrowOtherSide;
    if(voltageOptions&&item.kind!="symbol")return false;
    if(item.sourcePoints.size()>20000)return false;
    for(auto p:item.sourcePoints)if(!std::isfinite(p.x())||!std::isfinite(p.y())||std::abs(p.x())>20000||std::abs(p.y())>20000)return false;
    if(!std::isfinite(item.width)||item.width<=0||item.width>100 || !std::isfinite(item.patternScale)||item.patternScale<0.01||item.patternScale>1000)return false;
    const bool configurable=item.kind=="symbol"&&isConfigurableStencil(item.symbolId);
    const QColor color=configurable?foregroundColor(item):(item.strokes.isEmpty()?QColor(Qt::black):item.strokes.first().color);
    if(configurable&&!color.isValid())return false;
    Polyline points;
    if(hasBox(item)) {
        const qreal w=boxWidth(item),h=boxHeight(item);
        if(w<1||h<1||w>20000||h>20000)return false;
        const auto ax=(item.sourcePoints[1]-item.sourcePoints[0])/w,ay=(item.sourcePoints[2]-item.sourcePoints[0])/h;
        if(std::abs(QPointF::dotProduct(ax,ay))>1e-6)return false;
        if(item.kind=="symbol") {
            Symbol selected;
            QVariantMap parameters;
            if(configurable) {
                if(!normalizeStencilParameters(item.symbolId,item.stencilParameters,&parameters))return false;
                selected=configuredStencil(item.symbolId,parameters);
            } else for(const auto &symbol:electronicsCatalogue())if(symbol.id==item.symbolId){selected=symbol;break;}
            if(!selected.strokes.isEmpty()) {
                if(voltageOptions&&!supportsVoltageArrow(selected))return false;
                auto strokes=selected.strokes;
                qreal localWidth=120,localHeight=80;
                if(item.symbolId=="square-root") {
                    strokes={{squareRoot(QRectF(0,0,w,h)),2.3,color}};
                    localWidth=w;localHeight=h;
                }
                if(item.voltageArrow) {
                    const auto annotation=voltageArrowStrokes(selected,item.voltageArrowReversed,item.voltageArrowOtherSide);
                    if(annotation.isEmpty())return false;
                    strokes+=annotation;
                }
                for(auto &stroke:strokes){
                    stroke.width=configurable?std::max(qreal(.5),item.width*(stroke.width/2.3)):item.width;
                    if(!std::isfinite(stroke.width)||stroke.width>100)return false;
                    stroke.color=color;for(auto &p:stroke.points)p=mapBox(item,p.x()/localWidth,p.y()/localHeight);
                }
                if(configurable&&parameters.value("opaqueBackground").toBool()) {
                    // One round-capped stroke masks earlier ink. The dense
                    // serpentine remains a single explicitly owned native item.
                    const qreal thickness=std::min(qreal(24),std::min(w,h));
                    const qreal along=std::max(w,h),across=std::min(w,h);
                    const qreal inset=thickness/2,span=across-thickness;
                    const int bands=1+int(std::ceil(span/(thickness*.65)));
                    const qreal spacing=bands>1?span/(bands-1):0;
                    // Match the native sampler's 8-unit spacing before any
                    // allocation. Large valid boxes must fail without stalling.
                    const qreal sampled=1+bands*std::max(qreal(1),std::ceil((along-thickness)/8))
                        +(bands-1)*std::max(qreal(1),std::ceil(spacing/8));
                    if(!std::isfinite(sampled)||sampled>200000||bands>100000)return false;
                    Polyline mask;mask.reserve(bands*2);
                    for(int row=0;row<bands;++row) {
                        const qreal cross=inset+row*spacing;
                        const qreal first=row%2?along-inset:inset,second=row%2?inset:along-inset;
                        if(w>=h){mask.append(mapBox(item,first/w,cross/h));mask.append(mapBox(item,second/w,cross/h));}
                        else {mask.append(mapBox(item,cross/w,first/h));mask.append(mapBox(item,cross/w,second/h));}
                    }
                    strokes.prepend({mask,thickness,Qt::white});
                }
                if(configurable) {
                    // Account for foreground sampling as well as the mask.
                    qreal samples=0;
                    for(const auto &stroke:strokes){
                        if(stroke.points.size()<2)return false;
                        ++samples;
                        for(qsizetype i=1;i<stroke.points.size();++i)
                            samples+=std::max(qreal(1),std::ceil(QLineF(stroke.points[i-1],stroke.points[i]).length()/8));
                        if(!std::isfinite(samples)||samples>200000)return false;
                    }
                    item.stencilParameters=parameters;
                }
                item.strokes=std::move(strokes);
                item.anchors.clear();for(auto p:selected.anchors)item.anchors.append(mapBox(item,p.x()/120.,p.y()/80.));
                item.portIds=selected.portIds;ensurePorts(item);return true;
            }
            return false;
        }
        if(!std::isfinite(item.cornerRadius)||item.cornerRadius<0)return false;
        item.cornerRadius=std::min(item.cornerRadius,std::min(w,h)/2);
        points=item.kind=="ellipse"?ellipse(QRectF(0,0,w,h)):roundedRectangle(QRectF(0,0,w,h),item.cornerRadius);
        for(auto &p:points)p=mapBox(item,p.x()/w,p.y()/h);
        item.anchors={mapBox(item,0.5,0),mapBox(item,1,0.5),mapBox(item,0.5,1),mapBox(item,0,0.5)};
        item.portIds={"north","east","south","west"};
    } else if(item.kind=="wire") {
        if(!std::isfinite(item.wireBend)||std::abs(item.wireBend)>20000)return false;
        const qreal axisLength=std::hypot(item.wireAxis.x(),item.wireAxis.y());
        if(!std::isfinite(axisLength)||axisLength<1e-9)return false;
        item.wireAxis/=axisLength;
        if(item.wireRouteMode!="auto"&&item.wireRouteMode!="manual"&&item.wireRouteMode!="legacy")return false;
        points=wirePoints(item);if(points.isEmpty())return false;
    } else points=item.sourcePoints;
    QVector<qreal> pattern;
    if(item.style=="dashed")pattern={24*item.patternScale,16*item.patternScale};
    else if(item.style=="dotted")pattern={0.2*item.patternScale,11*item.patternScale};
    else if(item.style!="solid")return false;
    qreal length=0;for(int p=1;p<points.size();++p)length+=QLineF(points[p-1],points[p]).length();
    const qreal estimate=pattern.isEmpty()?points.size():points.size()+2*std::ceil(length/(pattern[0]+pattern[1]))+10;
    if(!std::isfinite(estimate)||estimate>250000)return false;
    QVector<Stroke> strokes;
    for(const auto &part:dashByArcLength(points,pattern))strokes.append({part,item.width,color});
    if(item.kind=="arrow") {
        if(!std::isfinite(item.headSize)||item.headSize<0.01||item.headSize>5000)return false;
        if(item.arrowDirection=="end"||item.arrowDirection=="both")strokes+=arrowHead(item.sourcePoints[0],item.sourcePoints[1],item.headSize,item.width);
        if(item.arrowDirection=="start"||item.arrowDirection=="both")strokes+=arrowHead(item.sourcePoints[1],item.sourcePoints[0],item.headSize,item.width);
    }
    for(auto &stroke:strokes)stroke.color=color;
    item.strokes=strokes;
    if(hasEndpoints(item)){item.anchors=item.sourcePoints;item.portIds={"start","end"};}
    return !strokes.isEmpty();
}
inline bool transform(Item &item,const QTransform &t) {
    for(auto &stroke:item.strokes)for(auto &p:stroke.points)p=t.map(p);
    for(auto &p:item.anchors)p=t.map(p);
    for(auto &p:item.sourcePoints)p=t.map(p);
    for(auto &p:item.wireRoute)p=t.map(p);
    const auto axis=t.map(QPointF(1,0))-t.map(QPointF());
    const qreal scale=std::hypot(axis.x(),axis.y());
    if(hasPath(item)) {
        item.headSize*=scale;item.patternScale*=scale;item.cornerRadius*=scale;item.wireBend*=scale;
        if(item.headSize<0.01||item.headSize>5000||item.patternScale<0.01||item.patternScale>1000)return false;
        if(item.kind=="wire")item.wireAxis=t.map(item.wireAxis)-t.map(QPointF());
        return rebuild(item);
    }
    return true;
}
inline bool setWireRoute(Item &item,const Polyline &points) {
    if(item.kind!="wire"||!hasEndpoints(item)||points.size()<2||points.size()>1024
            ||QLineF(points.first(),item.sourcePoints.first()).length()>1e-7
            ||QLineF(points.last(),item.sourcePoints.last()).length()>1e-7)return false;
    auto candidate=item;candidate.wireRoute=points;candidate.wireRouteMode="manual";
    candidate.wireHasBend=false;candidate.wireBend=0;
    if(!rebuild(candidate))return false;
    item=std::move(candidate);return true;
}
// Segment indices address points[index] -> points[index + 1]. End segments are
// excluded so manual editing never disconnects a port or moves an endpoint.
inline bool moveWireSegment(Item &item,int index,QPointF position) {
    auto points=wirePoints(item);
    if(index<1||index+2>=points.size()||!std::isfinite(position.x())||!std::isfinite(position.y()))return false;
    const qreal length=std::hypot(item.wireAxis.x(),item.wireAxis.y());if(length<1e-9)return false;
    const QPointF ax=item.wireAxis/length,ay(-ax.y(),ax.x());
    const auto segment=points[index+1]-points[index];
    const QPointF normal=std::abs(QPointF::dotProduct(segment,ax))>1e-7?ay:ax;
    const auto movement=normal*QPointF::dotProduct(position-(points[index]+points[index+1])/2,normal);
    points[index]+=movement;points[index+1]+=movement;
    return setWireRoute(item,points);
}
inline bool portPosition(const Document &doc,const Attachment &ref,QPointF *position) {
    if(ref.empty())return false;
    for(const auto &item:doc)if(item.id==ref.objectId) {
        if(item.kind=="wire"&&ref.portId=="route") {
            if(!std::isfinite(ref.wirePosition)||ref.wirePosition<0||ref.wirePosition>1)return false;
            const auto points=wirePoints(item);if(points.size()<2)return false;
            qreal length=0;for(int i=1;i<points.size();++i)length+=QLineF(points[i-1],points[i]).length();
            qreal distance=length*ref.wirePosition;
            for(int i=1;i<points.size();++i) {
                const qreal segment=QLineF(points[i-1],points[i]).length();
                if(segment>1e-9&&(distance<=segment||i==points.size()-1)) {
                    *position=points[i-1]+(points[i]-points[i-1])*std::clamp(distance/segment,qreal(0),qreal(1));return true;
                }
                distance-=segment;
            }
            *position=points.first();return true;
        }
        if(ref.wirePosition!=-1)return false;
        const int index=item.portIds.indexOf(ref.portId);
        if(index<0||index>=item.anchors.size())return false;
        *position=item.anchors[index];return true;
    }
    return false;
}
inline QPointF cardinalDirection(QPointF value) {
    if(!std::isfinite(value.x())||!std::isfinite(value.y())||std::hypot(value.x(),value.y())<1e-9)return {};
    if(std::abs(value.x())>=std::abs(value.y()))return {value.x()<0?-1.:1.,0};
    return {0,value.y()<0?-1.:1.};
}
inline QPointF portDirection(const Document &doc,const Attachment &ref) {
    if(ref.empty()||ref.portId=="route")return {};
    for(const auto &item:doc)if(item.id==ref.objectId) {
        const int index=item.portIds.indexOf(ref.portId);
        if(index<0||index>=item.anchors.size()||item.symbolId=="node")return {};
        if(hasBox(item)) {
            const qreal w=boxWidth(item),h=boxHeight(item);if(w<1||h<1)return {};
            const QPointF ax=(item.sourcePoints[1]-item.sourcePoints[0])/w,ay=(item.sourcePoints[2]-item.sourcePoints[0])/h;
            const auto p=item.anchors[index]-item.sourcePoints[0];
            const qreal x=QPointF::dotProduct(p,ax)/w,y=QPointF::dotProduct(p,ay)/h;
            const qreal distances[]={std::abs(x),std::abs(1-x),std::abs(y),std::abs(1-y)};
            const QPointF directions[]={-ax,ax,-ay,ay};int side=0;
            for(int i=1;i<4;++i)if(distances[i]<distances[side]-1e-7)side=i;
            return cardinalDirection(directions[side]);
        }
        if(item.kind=="wire") {
            const auto points=wirePoints(item);if(points.size()<2)return {};
            return cardinalDirection(index==0?points.first()-points[1]:points.last()-points[points.size()-2]);
        }
        return {};
    }
    return {};
}
// Logical anchors use the already rebuilt world geometry. Voltage annotations
// deliberately do not shift a component's insertion anchor.
inline QPointF defaultStencilAnchor(const Item &item) {
    if(item.kind=="symbol") {
        if(item.symbolId=="square-root") {
            for(const auto &stroke:item.strokes)if(!stroke.points.isEmpty())return stroke.points.first();
        }
        if(isConfigurableStencil(item.symbolId)&&hasBox(item))return item.sourcePoints.first();
        QString port;
        if(item.symbolId=="opamp")port="output";
        else if(item.symbolId=="npn"||item.symbolId=="pnp")port="base";
        else if(item.symbolId=="mosfet")port="gate";
        else if(item.anchors.size()==1&&item.portIds.size()==1)port=item.portIds.first();
        const int index=item.portIds.indexOf(port);
        if(!port.isEmpty()&&index>=0&&index<item.anchors.size())return item.anchors[index];
    }
    if(hasBox(item))return mapBox(item,.5,.5);
    if(!item.sourcePoints.isEmpty())return item.sourcePoints.first();
    return bounds(item.strokes).center();
}
inline bool hasStencilConnectionPorts(const Item &item) {
    return item.kind=="symbol"&&item.symbolId!="square-root"&&!isConfigurableStencil(item.symbolId)
        &&!item.anchors.isEmpty()&&item.anchors.size()==item.portIds.size();
}
// Unit vector leaving a box port, retaining an observed non-cardinal rotation.
inline QPointF stencilPortOutwardDirection(const Item &item,int index) {
    if(!hasBox(item)||index<0||index>=item.anchors.size())return {};
    const qreal w=boxWidth(item),h=boxHeight(item);if(w<1||h<1)return {};
    const QPointF ax=(item.sourcePoints[1]-item.sourcePoints[0])/w,ay=(item.sourcePoints[2]-item.sourcePoints[0])/h;
    const auto p=item.anchors[index]-item.sourcePoints[0];
    const qreal x=QPointF::dotProduct(p,ax)/w,y=QPointF::dotProduct(p,ay)/h;
    const qreal distances[]={std::abs(x),std::abs(1-x),std::abs(y),std::abs(1-y)};
    const QPointF directions[]={-ax,ax,-ay,ay};int side=0;
    for(int i=1;i<4;++i)if(distances[i]<distances[side]-1e-7)side=i;
    return directions[side];
}
inline StencilPlacementResult placeStencilNearPointer(const Item &item,QPointF pointer,const Document &doc,qreal tolerance) {
    StencilPlacementResult result;result.targetPoint=pointer;
    const auto finite=[](QPointF p){return std::isfinite(p.x())&&std::isfinite(p.y());};
    const auto anchor=defaultStencilAnchor(item);
    if(!finite(pointer)||!finite(anchor))return result;
    result.delta=pointer-anchor;
    if(!hasStencilConnectionPorts(item)||!std::isfinite(tolerance)||tolerance<=0||tolerance>10000||doc.size()>3000)return result;
    qreal bestDistance=tolerance*tolerance;int bestPriority=10;
    const auto unit=[](QPointF v){const qreal length=std::hypot(v.x(),v.y());return std::isfinite(length)&&length>1e-9?v/length:QPointF();};
    auto consider=[&](QPointF target,const Attachment &attachment,QPointF bodyDirection,int priority) {
        if(!finite(target))return;
        const auto difference=target-pointer;const qreal distance=QPointF::dotProduct(difference,difference);
        if(distance>tolerance*tolerance||distance>bestDistance+1e-12)return;
        if(result.snapped&&std::abs(distance-bestDistance)<=1e-12&&priority>=bestPriority)return;
        int chosen=-1;qreal bestAlignment=-1;
        for(int index=0;index<item.anchors.size();++index) {
            if(!finite(item.anchors[index]))continue;
            const auto outward=stencilPortOutwardDirection(item,index);
            const qreal alignment=bodyDirection.isNull()?1:-QPointF::dotProduct(outward,bodyDirection);
            if(!outward.isNull()&&alignment>=.999&&alignment>bestAlignment+1e-9){chosen=index;bestAlignment=alignment;}
        }
        if(chosen<0)return;
        result.delta=target-item.anchors[chosen];result.localPortId=item.portIds[chosen];
        result.targetAttachment=attachment;result.targetPoint=target;result.snapped=true;
        bestDistance=distance;bestPriority=priority;
    };
    // Prefer nearby terminals over line projections, matching wire insertion.
    for(const auto &target:doc)if(target.id!=item.id&&!target.id.isEmpty()) {
        if(target.kind=="wire"&&hasEndpoints(target)) {
            const auto route=wirePoints(target);if(route.size()<2)continue;
            for(int end=0;end<2;++end) {
                const auto point=end==0?route.first():route.last();QPointF direction;
                for(int offset=1;offset<route.size();++offset) {
                    direction=unit(point-route[end==0?offset:route.size()-1-offset]);
                    if(!direction.isNull())break;
                }
                const auto &existing=end==0?target.startAttachment:target.endAttachment;
                consider(point,{target.id,end==0?QString("start"):QString("end")},direction,existing.empty()?0:2);
            }
        } else if(hasStencilConnectionPorts(target)) {
            for(int index=0;index<target.anchors.size();++index)
                consider(target.anchors[index],{target.id,target.portIds[index]},stencilPortOutwardDirection(target,index),1);
        }
    }
    if(result.snapped)return result;
    for(const auto &target:doc)if(target.id!=item.id&&!target.id.isEmpty()&&target.kind=="wire") {
        const auto route=wirePoints(target);qreal total=0,walked=0;
        for(int i=1;i<route.size();++i)total+=QLineF(route[i-1],route[i]).length();
        if(!std::isfinite(total)||total<1e-9)continue;
        for(int i=1;i<route.size();++i) {
            const auto segment=route[i]-route[i-1];const qreal length=QLineF(route[i-1],route[i]).length();
            if(length<1e-9)continue;
            const qreal fraction=std::clamp(QPointF::dotProduct(pointer-route[i-1],segment)/(length*length),qreal(0),qreal(1));
            const auto projected=route[i-1]+segment*fraction;
            // An incompatible endpoint must not be disguised as a T junction.
            if((i>1||fraction>1e-9)&&(i<route.size()-1||fraction<1-1e-9)) {
                const auto tangent=segment/length;const QPointF normal(-tangent.y(),tangent.x());
                const qreal side=QPointF::dotProduct(pointer-projected,normal);
                const Attachment attachment{target.id,"route",(walked+fraction*length)/total};
                if(side>=-1e-9)consider(projected,attachment,normal,3);
                if(side<=1e-9)consider(projected,attachment,-normal,3);
            }
            walked+=length;
        }
    }
    return result;
}
// Only metadata changes: each chosen endpoint must already coincide with the
// new port. Geometry, routing, existing connections and unrelated wires remain
// untouched. A placement restricts this to its explicitly chosen free endpoint.
inline int attachCoincidentWireEndpoints(Document &doc,const Item &stencil,const StencilPlacementResult *placement=nullptr) {
    if(!hasStencilConnectionPorts(stencil)||stencil.id.isEmpty()||doc.size()>3000)return 0;
    if(placement&&(!placement->snapped||placement->targetAttachment.empty()||placement->localPortId.isEmpty()||
        placement->targetAttachment.wirePosition!=-1||
        (placement->targetAttachment.portId!="start"&&placement->targetAttachment.portId!="end")))return 0;
    struct Candidate {int wire,end,port;};QVector<Candidate> candidates;
    for(int index=0;index<doc.size();++index) {
        const auto &wire=doc[index];if(wire.id==stencil.id||wire.kind!="wire"||!hasEndpoints(wire))continue;
        if(placement&&wire.id!=placement->targetAttachment.objectId)continue;
        for(int end=0;end<2;++end) {
            const auto &existing=end==0?wire.startAttachment:wire.endAttachment;if(!existing.empty())continue;
            if(!std::isfinite(wire.sourcePoints[end].x())||!std::isfinite(wire.sourcePoints[end].y()))continue;
            if(placement&&placement->targetAttachment.portId!=(end==0?"start":"end"))continue;
            int matched=-1;
            for(int port=0;port<stencil.anchors.size();++port) {
                if(placement&&stencil.portIds[port]!=placement->localPortId)continue;
                if(!std::isfinite(stencil.anchors[port].x())||!std::isfinite(stencil.anchors[port].y()))continue;
                if(QLineF(wire.sourcePoints[end],stencil.anchors[port]).length()>1e-5)continue;
                if(matched>=0){matched=-2;break;}matched=port;
            }
            if(matched>=0)candidates.append({index,end,matched});
        }
    }
    int count=0;
    for(const auto &candidate:candidates) {
        if(!placement&&std::count_if(candidates.cbegin(),candidates.cend(),[&](const auto &other){return other.port==candidate.port;})!=1)continue;
        auto &wire=doc[candidate.wire];auto &attachment=candidate.end==0?wire.startAttachment:wire.endAttachment;
        attachment={stencil.id,stencil.portIds[candidate.port]};++count;
    }
    return count;
}
inline bool resolveWireAttachments(Item &item,const Document &doc) {
    if(item.kind!="wire"||!hasEndpoints(item))return false;
    Attachment *refs[]={&item.startAttachment,&item.endAttachment};
    for(int end=0;end<2;++end)if(!refs[end]->empty()) {
        QPointF p;if(portPosition(doc,*refs[end],&p))item.sourcePoints[end]=p;else *refs[end]={};
    }
    return true;
}
inline bool routeWire(Item &item,const Document &doc,const QSet<QString> *routedWires=nullptr) {
    if(!resolveWireAttachments(item,doc))return false;
    if(item.wireRouteMode=="auto"&&(item.wireHasBend||std::abs(item.wireBend)>1e-9))item.wireRouteMode="legacy";
    if(item.wireRouteMode=="auto") {
        OrthogonalRouteOptions options;options.horizontalFirst=item.horizontalFirst;
        options.startDirection=portDirection(doc,item.startAttachment);options.endDirection=portDirection(doc,item.endAttachment);
        options.preferredRoute=item.wireRoute;options.clearance=std::max(qreal(12),item.width*2);
        for(const auto &obstacle:doc)if(obstacle.id!=item.id&&obstacle.kind=="symbol") {
            const auto corners=boxCorners(obstacle);
            if(corners.isEmpty()) {const auto box=bounds(obstacle.strokes);if(!box.isEmpty())options.obstacles.append(box);}
            else {
                qreal left=corners[0].x(),right=left,top=corners[0].y(),bottom=top;
                for(auto p:corners){left=std::min(left,p.x());right=std::max(right,p.x());top=std::min(top,p.y());bottom=std::max(bottom,p.y());}
                options.obstacles.append(QRectF(QPointF(left,top),QPointF(right,bottom)));
            }
        }
        const auto sameAttachment=[](const Attachment &a,const Attachment &b) {
            return !a.empty()&&a.objectId==b.objectId&&a.portId==b.portId&&std::abs(a.wirePosition-b.wirePosition)<1e-9;
        };
        for(const auto &other:doc)if(other.id!=item.id&&other.kind=="wire") {
            // Earlier automatic routes keep their lanes. Later routes will be
            // recomputed against them; treating their stale geometry as fixed
            // can otherwise make a copied wire block an existing port lead.
            if(routedWires&&other.wireRouteMode=="auto"&&!other.wireHasBend&&
                std::abs(other.wireBend)<1e-9&&!routedWires->contains(other.id))continue;
            const auto points=wirePoints(other);if(points.size()<2)continue;
            QVector<QPointF> junctions;
            auto addJunction=[&](QPointF p) {
                for(auto present:junctions)if(QLineF(p,present).length()<1e-6)return;
                junctions.append(p);
            };
            const Attachment mine[]={item.startAttachment,item.endAttachment};
            const Attachment theirs[]={other.startAttachment,other.endAttachment};
            for(int end=0;end<2;++end)if(!mine[end].empty()) {
                if(mine[end].objectId==other.id)addJunction(item.sourcePoints[end]);
                for(const auto &ref:theirs)if(sameAttachment(mine[end],ref))addJunction(item.sourcePoints[end]);
            }
            for(int end=0;end<2;++end)if(theirs[end].objectId==item.id)addJunction(other.sourcePoints[end]);
            for(int i=1;i<points.size();++i) {
                auto a=points[i-1],b=points[i];if(QLineF(a,b).length()<1e-7)continue;
                if(std::abs(a.x()-b.x())<1e-7)b.setX(a.x());
                else if(std::abs(a.y()-b.y())<1e-7)b.setY(a.y());
                else continue; // A diagonal manual segment cannot overlap an orthogonal route.
                OccupiedRouteSegment segment{a,b,{}};
                for(auto point:junctions) {
                    const qreal length=QLineF(a,point).length()+QLineF(point,b).length();
                    if(std::abs(length-QLineF(a,b).length())<1e-6)segment.sharedJunctions.append(point);
                }
                options.occupiedSegments.append(std::move(segment));
            }
        }
        item.wireRoute=orthogonalRoute(item.sourcePoints[0],item.sourcePoints[1],options);
        if(item.wireRoute.isEmpty())return false;
        item.wireAxis={1,0};
    }
    if(!rebuild(item))return false;
    item.wireRoute=wirePoints(item);
    return !item.wireRoute.isEmpty();
}
// Targets are processed before their dependent wires. A cycle rejects the
// proposed graph; a missing target detaches without moving its last endpoint.
inline bool resolveDocumentWires(Document &doc,bool automatic) {
    QHash<QString,int> indices;
    for(int i=0;i<doc.size();++i){if(indices.contains(doc[i].id))return false;indices.insert(doc[i].id,i);}
    QVector<int> state(doc.size());
    QSet<QString> routedWires;
    std::function<bool(int)> visit=[&](int index) {
        if(doc[index].kind!="wire"||state[index]==2)return true;
        if(state[index]==1)return false;
        state[index]=1;
        for(const auto &ref:{doc[index].startAttachment,doc[index].endAttachment})if(!ref.empty()&&indices.contains(ref.objectId)) {
            if(!visit(indices.value(ref.objectId)))return false;
        }
        auto &item=doc[index];
        if(automatic){if(!routeWire(item,doc,&routedWires))return false;routedWires.insert(item.id);}
        else {
            if(!resolveWireAttachments(item,doc)||!rebuild(item))return false;
            item.wireRoute=wirePoints(item);
        }
        state[index]=2;return true;
    };
    for(int i=0;i<doc.size();++i)if(!visit(i))return false;
    return true;
}
inline bool reroute(Document &doc) { return resolveDocumentWires(doc,true); }
inline bool resolveAttachments(Document &doc) { return resolveDocumentWires(doc,false); }
inline bool attachmentWouldCycle(const Document &doc,const QString &source,const QString &target) {
    if(source.isEmpty())return false;
    QStringList pending{target};QSet<QString> seen;
    while(!pending.isEmpty()) {
        const auto id=pending.takeLast();if(id==source)return true;
        if(seen.contains(id))continue;seen.insert(id);
        for(const auto &item:doc)if(item.id==id&&item.kind=="wire") {
            if(!item.startAttachment.empty())pending.append(item.startAttachment.objectId);
            if(!item.endAttachment.empty())pending.append(item.endAttachment.objectId);
            break;
        }
    }
    return false;
}
inline Attachment nearestPort(const Document &doc,QPointF &p,qreal radius,const QString &excluding={}) {
    Attachment result;qreal distance=radius;
    for(const auto &item:doc)if(item.id!=excluding&&!attachmentWouldCycle(doc,excluding,item.id)) {
        for(int i=0;i<item.anchors.size()&&i<item.portIds.size();++i) {
            const qreal d=QLineF(p,item.anchors[i]).length();
            if(d<distance){distance=d;result={item.id,item.portIds[i]};}
        }
        if(item.kind=="wire") {
            const auto points=wirePoints(item);qreal total=0,walked=0;
            for(int i=1;i<points.size();++i)total+=QLineF(points[i-1],points[i]).length();
            if(total<1e-9)continue;
            for(int i=1;i<points.size();++i) {
                const auto delta=points[i]-points[i-1];const qreal squared=QPointF::dotProduct(delta,delta);
                if(squared<1e-12)continue;
                const qreal fraction=std::clamp(QPointF::dotProduct(p-points[i-1],delta)/squared,qreal(0),qreal(1));
                const auto projected=points[i-1]+delta*fraction;const qreal d=QLineF(p,projected).length(),length=std::sqrt(squared);
                if(d<distance){distance=d;result={item.id,"route",(walked+fraction*length)/total};}
                walked+=length;
            }
        }
    }
    QPointF resolved;if(portPosition(doc,result,&resolved))p=resolved;
    return result;
}
inline bool withinBudget(const Document &doc,bool enforcePage=true) {
    if(doc.size()>3000)return false;
    qint64 rendered=0,source=0;
    for(const auto &item:doc) {
        source+=item.sourcePoints.size()+item.anchors.size()+item.wireRoute.size();
        for(const auto &stroke:item.strokes) {
            rendered+=stroke.points.size();
            for(auto p:stroke.points)if(!std::isfinite(p.x())||!std::isfinite(p.y())||(enforcePage&&(p.x()<0||p.y()<0||p.x()>PageWidth||p.y()>PageHeight)))return false;
        }
    }
    return rendered<=250000&&source<=256000;
}
inline Document copies(const Document &doc,const QStringList &ids,QPointF offset) {
    Document result;QHash<QString,QString> mapping;
    for(const auto &item:doc)if(ids.contains(item.id)){auto copy=item;copy.id=QUuid::createUuid().toString(QUuid::WithoutBraces);mapping.insert(item.id,copy.id);result.append(copy);}
    QTransform t;t.translate(offset.x(),offset.y());
    for(auto &copy:result) {
        if(!transform(copy,t))return {};
        for(auto ref:{&copy.startAttachment,&copy.endAttachment}) {
            if(mapping.contains(ref->objectId))ref->objectId=mapping.value(ref->objectId);else *ref={};
        }
    }
    if(!reroute(result))return {};
    return result;
}
}

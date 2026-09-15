#pragma once

#include "DrawingModel.h"
#include <QLineF>
#include <limits>

namespace repaper::drawing {
struct AlignmentGuide {
    QLineF line;
    QString targetId;
    bool operator==(const AlignmentGuide &other) const { return line==other.line&&targetId==other.targetId; }
};
struct AlignmentResult {
    // Total translation, including the caller's proposed movement.
    QPointF delta;
    QVector<AlignmentGuide> guides;
    bool snappedX=false, snappedY=false;
};

namespace alignment_detail {
struct Feature { QPointF point; int rank=0; };
struct Features { QRectF box; QVector<Feature> x,y; };
inline bool finite(QPointF p) { return std::isfinite(p.x())&&std::isfinite(p.y()); }
inline Features features(const Item &item,QPointF translation={}) {
    Features result;
    if(!hasBox(item)||!finite(translation))return result;
    const auto corners=boxCorners(item);if(corners.size()!=4)return result;
    qreal left=std::numeric_limits<qreal>::infinity(),top=left,right=-left,bottom=-left;
    for(auto p:corners) {
        if(!finite(p))return {};
        p+=translation;left=std::min(left,p.x());right=std::max(right,p.x());top=std::min(top,p.y());bottom=std::max(bottom,p.y());
    }
    if(right-left<1e-7||bottom-top<1e-7)return {};
    result.box=QRectF(QPointF(left,top),QPointF(right,bottom));
    const auto center=result.box.center();
    // Ports have priority over centers and edges only when corrections tie.
    for(auto p:item.anchors)if(finite(p)) {result.x.append({p+translation,0});result.y.append({p+translation,0});}
    result.x.append({center,1});result.y.append({center,1});
    result.x.append({{left,center.y()},2});result.x.append({{right,center.y()},2});
    result.y.append({{center.x(),top},2});result.y.append({{center.x(),bottom},2});
    return result;
}
struct Match {
    bool present=false;
    qreal correction=0,distance=0,gap=0;
    int rank=0;
    QPointF moving,target;
    QString targetId;
};
inline void consider(Match &best,const Feature &moving,const Feature &target,const QString &id,
                     bool x,qreal tolerance,qreal gap) {
    const qreal correction=x?target.point.x()-moving.point.x():target.point.y()-moving.point.y();
    const qreal distance=std::abs(correction);if(distance>tolerance)return;
    const int rank=moving.rank==target.rank?moving.rank:3;
    bool better=!best.present||distance<best.distance-1e-7;
    if(best.present&&std::abs(distance-best.distance)<=1e-7) {
        better=rank<best.rank||(rank==best.rank&&(gap<best.gap-1e-7
            ||(std::abs(gap-best.gap)<=1e-7&&id<best.targetId)));
    }
    if(better)best={true,correction,distance,gap,rank,moving.point,target.point,id};
}
}

// All values, including tolerance and maximumGap, are in document coordinates.
// The caller converts a small screen-distance tolerance through its viewport
// scale. No grid is used and neither the document nor its rendered ink changes.
inline AlignmentResult alignTranslation(const Item &moving,const Document &document,QPointF proposedDelta,
                                        qreal tolerance,qreal maximumGap=400) {
    using namespace alignment_detail;
    AlignmentResult result;
    if(!finite(proposedDelta))return result;
    result.delta=proposedDelta;
    if(!std::isfinite(tolerance)||tolerance<0||!std::isfinite(maximumGap)||maximumGap<0)return result;
    const auto source=features(moving,proposedDelta);if(source.box.isEmpty())return result;
    Match x,y;
    for(const auto &item:document)if(item.id!=moving.id) {
        const auto target=features(item);if(target.box.isEmpty())continue;
        const qreal gapX=std::max({qreal(0),source.box.left()-target.box.right(),target.box.left()-source.box.right()});
        const qreal gapY=std::max({qreal(0),source.box.top()-target.box.bottom(),target.box.top()-source.box.bottom()});
        const qreal gap=std::hypot(gapX,gapY);if(gap>maximumGap)continue;
        for(const auto &a:source.x)for(const auto &b:target.x)consider(x,a,b,item.id,true,tolerance,gap);
        for(const auto &a:source.y)for(const auto &b:target.y)consider(y,a,b,item.id,false,tolerance,gap);
    }
    const QPointF correction(x.present?x.correction:0,y.present?y.correction:0);
    result.delta+=correction;result.snappedX=x.present;result.snappedY=y.present;
    const qreal extension=std::max(qreal(8),2*tolerance);
    if(x.present) {
        const auto p=x.moving+correction;
        result.guides.append({QLineF(x.target.x(),std::min(x.target.y(),p.y())-extension,
                                   x.target.x(),std::max(x.target.y(),p.y())+extension),x.targetId});
    }
    if(y.present) {
        const auto p=y.moving+correction;
        result.guides.append({QLineF(std::min(y.target.x(),p.x())-extension,y.target.y(),
                                   std::max(y.target.x(),p.x())+extension,y.target.y()),y.targetId});
    }
    return result;
}
}

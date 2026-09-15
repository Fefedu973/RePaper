#include "NativeObjectGesture.h"
#include <QtTest>
#include <limits>
using namespace repaper::drawing;
namespace {
Item box(const QString &kind="rectangle"){
    Item item;item.kind=kind;item.symbolId="resistor-iec";item.sourcePoints={{200,200},{440,200},{200,360}};item.width=3;
    rebuild(item);QTransform rotation;rotation.translate(320,280);rotation.rotate(37);rotation.translate(-320,-280);transform(item,rotation);return item;
}
bool close(QPointF a,QPointF b){return QLineF(a,b).length()<.001;}
}
class NativeObjectGestureTest : public QObject {
    Q_OBJECT
private slots:
    void moveAlignmentIsTransientAndDoesNotMoveOnTap(){
        Item moving;moving.id="moving";moving.kind="symbol";moving.symbolId="resistor-iec";
        moving.sourcePoints={{204,300},{324,300},{204,380}};QVERIFY(rebuild(moving));
        Item target=moving;target.id="target";QTransform shift;shift.translate(-4,-200);QVERIFY(transform(target,shift));
        NativeObjectGesture gesture;gesture.alignmentDocument={moving,target};gesture.alignmentTolerance=8;
        const QPointF press(264,340);QVERIFY(gesture.begin(moving,press,1));QCOMPARE(gesture.kind(),QString("move"));
        QVERIFY(gesture.update(press));QCOMPARE(gesture.preview().sourcePoints,moving.sourcePoints);
        QVERIFY(gesture.alignmentGuides().isEmpty());
        QVERIFY(gesture.update(press+QPointF(1,8)));QVERIFY(!gesture.alignmentGuides().isEmpty());
        QCOMPARE(gesture.preview().sourcePoints[0].x(),qreal(200));
        QCOMPARE(gesture.preview().strokes.size(),moving.strokes.size());
        gesture.cancel();QVERIFY(gesture.alignmentGuides().isEmpty());
    }
    void aspectLockedRotatedBoxesUseEveryOppositeAnchor(){
        const QVector<QPointF> localChanges{{-120,-40},{30,-80},{120,-40},{120,30},{120,40},{30,80},{-120,40},{-120,30}};
        for(const auto &kind:QStringList{"rectangle","ellipse","symbol"}){
            const auto item=box(kind);const auto origin=item.sourcePoints[0];
            const auto x=(item.sourcePoints[1]-origin)/240,y=(item.sourcePoints[2]-origin)/160;
            const auto controls=NativeObjectGesture::controls(item);const auto corners=boxCorners(item);
            for(int i=0;i<8;++i){
                NativeObjectGesture gesture;QVERIFY(!gesture.lockAspectRatio());
                const auto press=controls[i].point+x*2+y;
                QVERIFY(gesture.begin(item,press,1));QCOMPARE(gesture.kind(),QString("resize"));
                QVERIFY(gesture.canLockAspectRatio());QVERIFY(gesture.lockAspectRatio());QVERIFY(gesture.lockAspectRatio());
                const auto point=press+x*localChanges[i].x()+y*localChanges[i].y();
                QVERIFY(gesture.update(point));QVERIFY(gesture.update(point));QVERIFY(gesture.finish(point));
                const auto result=gesture.preview();const auto resized=boxCorners(result);
                const auto anchor=controls[(i+4)%8].point;
                QVERIFY(qAbs(boxWidth(result)-360)<.001);QVERIFY(qAbs(boxHeight(result)-240)<.001);
                for(int n=0;n<4;++n)QVERIFY(close(resized[n],anchor+(corners[n]-anchor)*1.5));
                QCOMPARE(result.width,item.width);for(const auto &stroke:result.strokes)QCOMPARE(stroke.width,item.width);
                QVERIFY(gesture.affineChange().isEmpty());QVERIFY(!gesture.active());QVERIFY(gesture.aspectRatioLocked());
                QVERIFY(!gesture.canLockAspectRatio());
                QVERIFY(gesture.begin(item,press,1));QVERIFY(!gesture.aspectRatioLocked());
            }
        }
    }
    void aspectLockedCrossingClampsWithoutFlippingOrChangingRatio(){
        const auto item=box();const auto origin=item.sourcePoints[0];
        const auto x=(item.sourcePoints[1]-origin)/240,y=(item.sourcePoints[2]-origin)/160;
        const auto controls=NativeObjectGesture::controls(item);
        const QVector<QPointF> localChanges{{1000,1000},{0,1000},{-1000,1000},{-1000,0},{-1000,-1000},{0,-1000},{1000,-1000},{1000,0}};
        for(int i=0;i<8;++i){
            NativeObjectGesture gesture;QVERIFY(gesture.begin(item,controls[i].point,1));QVERIFY(gesture.lockAspectRatio());
            QVERIFY(gesture.update(controls[i].point+x*localChanges[i].x()+y*localChanges[i].y()));
            const auto result=gesture.preview();
            QVERIFY(qAbs(boxWidth(result)-1.5)<.001);QVERIFY(qAbs(boxHeight(result)-1)<.001);
            QVERIFY(boxWidth(result)>=1);QVERIFY(boxHeight(result)>=1);
            QVERIFY(close((result.sourcePoints[1]-result.sourcePoints[0])/boxWidth(result),x));
            QVERIFY(close((result.sourcePoints[2]-result.sourcePoints[0])/boxHeight(result),y));
            QVERIFY(close(NativeObjectGesture::controls(result)[(i+4)%8].point,controls[(i+4)%8].point));
            QVERIFY(gesture.update(controls[i].point));
            for(int n=0;n<3;++n)QVERIFY(close(gesture.preview().sourcePoints[n],item.sourcePoints[n]));
        }
    }
    void aspectLockPreservesCurrentDimensionsOfAnAlreadyStretchedSymbol(){
        auto item=box("symbol");QVERIFY(NativeObjectGesture::resizeBox(item,300,120));
        const auto origin=item.sourcePoints[0],x=(item.sourcePoints[1]-origin)/300,y=(item.sourcePoints[2]-origin)/120;
        const auto corner=boxCorners(item)[2];NativeObjectGesture gesture;
        QVERIFY(gesture.begin(item,corner,1));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(gesture.update(corner-x*100-y*10));
        QVERIFY(qAbs(boxWidth(gesture.preview())-200)<.001);QVERIFY(qAbs(boxHeight(gesture.preview())-80)<.001);
        QVERIFY(gesture.finish(corner+x*60+y*20));const auto result=gesture.preview();
        QVERIFY(qAbs(boxWidth(result)-360)<.001);QVERIFY(qAbs(boxHeight(result)-144)<.001);
        QVERIFY(close(result.sourcePoints[0],origin));
        QVERIFY(close(result.anchors.first(),mapBox(result,0,.5)));QVERIFY(close(result.anchors.last(),mapBox(result,1,.5)));
        for(const auto &stroke:result.strokes)QCOMPARE(stroke.width,item.width);
    }
    void aspectLockedResizeUsesCommonScaleAtCoordinateBounds(){
        auto item=box();QTransform translate;translate.translate(19200,0);QVERIFY(transform(item,translate));
        const auto controls=NativeObjectGesture::controls(item);const auto origin=item.sourcePoints[0];
        const auto x=(item.sourcePoints[1]-origin)/240,y=(item.sourcePoints[2]-origin)/160;
        NativeObjectGesture gesture;QVERIFY(gesture.begin(item,controls[4].point,1));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(gesture.finish({19999,10000}));const auto result=gesture.preview();
        QVERIFY(boxWidth(result)>boxWidth(item));QVERIFY(qAbs(boxWidth(result)/boxHeight(result)-1.5)<.000001);
        QVERIFY(close(result.sourcePoints[0],origin));
        for(const auto corner:boxCorners(result)){QVERIFY(qAbs(corner.x())<=20000.000001);QVERIFY(qAbs(corner.y())<=20000.000001);}
        QVERIFY(close((result.sourcePoints[1]-origin)/boxWidth(result),x));
        QVERIFY(close((result.sourcePoints[2]-origin)/boxHeight(result),y));
    }
    void aspectLockIsLimitedToBoxResizeAndResetsOnCancel(){
        const auto item=box();NativeObjectGesture gesture;
        for(const auto point:{mapBox(item,.5,.5),NativeObjectGesture::rotationPoint(item,1)}){
            QVERIFY(gesture.begin(item,point,1));QVERIFY(!gesture.canLockAspectRatio());QVERIFY(!gesture.lockAspectRatio());
        }
        for(const auto &kind:QStringList{"line","arrow","wire"}){
            Item path;path.kind=kind;path.sourcePoints={{100,100},{400,300}};QVERIFY(rebuild(path));
            for(const auto control:NativeObjectGesture::controls(path)){
                QVERIFY(gesture.begin(path,control.point,1));QVERIFY(!gesture.lockAspectRatio());
            }
        }
        QVERIFY(gesture.begin(item,item.sourcePoints[0],1));QVERIFY(gesture.lockAspectRatio());gesture.cancel();
        QVERIFY(!gesture.aspectRatioLocked());
        QVERIFY(gesture.begin(item,item.sourcePoints[0],1));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(!gesture.finish({std::numeric_limits<qreal>::infinity(),0}));QVERIFY(!gesture.aspectRatioLocked());
    }
    void rotatedBoxResizesInItsOwnAxesWithoutChangingLineWidth(){
        const auto item=box();const auto origin=item.sourcePoints[0],x=(item.sourcePoints[1]-origin)/240,y=(item.sourcePoints[2]-origin)/160;
        NativeObjectGesture gesture;const auto corner=boxCorners(item)[2];
        QVERIFY(gesture.begin(item,corner,1));QCOMPARE(gesture.kind(),QString("resize"));
        QVERIFY(gesture.finish(corner+x*80+y*50));const auto result=gesture.preview();
        QVERIFY(close(result.sourcePoints[0],origin));QVERIFY(qAbs(boxWidth(result)-320)<.001);QVERIFY(qAbs(boxHeight(result)-210)<.001);
        QVERIFY(close((result.sourcePoints[1]-origin)/320,x));QVERIFY(close((result.sourcePoints[2]-origin)/210,y));
        for(const auto &stroke:result.strokes)QCOMPARE(stroke.width,item.width);
        QVERIFY(gesture.affineChange().isEmpty());
    }
    void symbolPortsAndEveryStrokeFollowLocalResize(){
        auto item=box("symbol");QVERIFY(item.strokes.size()>1);const auto originalWidth=item.width;const auto strokeCount=item.strokes.size();
        QVERIFY(NativeObjectGesture::resizeBox(item,360,240));QCOMPARE(item.strokes.size(),strokeCount);
        for(const auto &stroke:item.strokes)QCOMPARE(stroke.width,originalWidth);
        QVERIFY(close(item.anchors.first(),mapBox(item,0,.5)));QVERIFY(close(item.anchors.last(),mapBox(item,1,.5)));
    }
    void diagonalArrowEndpointKeepsItsOtherEndHeadSizeAndThickness(){
        Item item;item.kind="arrow";item.sourcePoints={{100,100},{300,260}};item.width=2;item.headSize=32;item.arrowDirection="both";QVERIFY(rebuild(item));
        NativeObjectGesture gesture;const auto press=item.sourcePoints[1]+QPointF(3,2);
        QVERIFY(gesture.begin(item,press,1));QCOMPARE(gesture.kind(),QString("endpoint"));
        QVERIFY(gesture.finish(press+QPointF(-60,180)));const auto result=gesture.preview();
        QCOMPARE(result.sourcePoints[0],item.sourcePoints[0]);QCOMPARE(result.sourcePoints[1],QPointF(240,440));
        QCOMPARE(result.headSize,item.headSize);QCOMPARE(result.arrowDirection,item.arrowDirection);
        for(const auto &stroke:result.strokes)QCOMPARE(stroke.width,item.width);
        QCOMPARE(NativeObjectGesture::controls(result).size(),2);
    }
    void rotatedWireEndpointsAndBendRetainPerpendicularSegments(){
        Item item;item.kind="wire";item.wireRouteMode="legacy";item.sourcePoints={{100,100},{400,300}};item.width=2;QVERIFY(rebuild(item));
        QTransform rotate;rotate.rotate(31);QVERIFY(transform(item,rotate));
        NativeObjectGesture gesture;QVERIFY(gesture.begin(item,item.sourcePoints[1],1));
        QVERIFY(gesture.finish(item.sourcePoints[1]+QPointF(100,80)));item=gesture.preview();
        QCOMPARE(item.width,qreal(2));QVERIFY(close(item.wireAxis,rotate.map(QPointF(1,0))));
        auto controls=NativeObjectGesture::controls(item);QCOMPARE(controls.size(),3);QCOMPARE(controls.last().kind,QString("bend"));
        QVERIFY(gesture.begin(item,controls.last().point,1));
        const auto axis=item.wireAxis,mid=(item.sourcePoints[0]+item.sourcePoints[1])*.5;
        // Passing through zero bend must retain the three-segment route.
        QVERIFY(gesture.finish(mid));item=gesture.preview();QVERIFY(item.wireHasBend);QVERIFY(qAbs(item.wireBend)<.001);
        QCOMPARE(item.strokes.first().points.size(),4);
        const auto points=item.strokes.first().points;
        for(int i=1;i<points.size();++i){
            const auto direction=points[i]-points[i-1];
            QVERIFY(qAbs(QPointF::dotProduct(direction,axis))<.001||qAbs(direction.x()*axis.y()-direction.y()*axis.x())<.001);
        }
        for(const auto &stroke:item.strokes)QCOMPARE(stroke.width,qreal(2));
    }
    void endpointSnappingAccountsForPressOffset(){
        Item item;item.kind="line";item.sourcePoints={{100,100},{400,300}};QVERIFY(rebuild(item));
        NativeObjectGesture gesture;QVERIFY(gesture.begin(item,{403,302},1));
        QCOMPARE(gesture.endpointCandidate({503,402}),QPointF(500,400));
        const auto snapped=QPointF(510,410);QVERIFY(gesture.finish(gesture.pointerForEndpoint(snapped)));
        QCOMPARE(gesture.preview().sourcePoints[1],snapped);
    }
    void scaleKeepsHeadAndPatternParametersWhileGrowingGeometry(){
        Item item;item.kind="arrow";item.style="dashed";item.sourcePoints={{100,100},{400,300}};QVERIFY(rebuild(item));
        const auto original=item;QTransform scale;scale.scale(2,2);QVERIFY(NativeObjectGesture::transformGeometry(item,scale));
        QCOMPARE(item.width,original.width);QCOMPARE(item.headSize,original.headSize);QCOMPARE(item.patternScale,original.patternScale);
        QCOMPARE(item.sourcePoints[1],QPointF(800,600));
    }
    void invalidReleaseCancelsWithoutReplacingTheModel(){
        const auto item=box();NativeObjectGesture gesture;QVERIFY(gesture.begin(item,item.sourcePoints[0],1));
        QVERIFY(!gesture.finish({std::numeric_limits<qreal>::infinity(),0}));QVERIFY(!gesture.active());
        QVERIFY(gesture.affineChange().isEmpty());
    }
    void routedInteriorSegmentMovesWithoutLosingPortsOrOtherWaypoints(){
        Item item;item.kind="wire";item.sourcePoints={{100,100},{600,400}};
        item.wireRouteMode="manual";item.wireRoute={{100,100},{200,100},{200,250},{450,250},{450,400},{600,400}};
        item.startAttachment={"source","out"};item.endAttachment={"target","in"};QVERIFY(rebuild(item));
        const auto controls=NativeObjectGesture::controls(item);
        const auto segment=std::find_if(controls.cbegin(),controls.cend(),[](const auto &control){return control.kind=="segment"&&control.index==2;});
        QVERIFY(segment!=controls.cend());NativeObjectGesture gesture;
        QVERIFY(gesture.begin(item,segment->point,1));QCOMPARE(gesture.kind(),QString("segment"));
        QVERIFY(gesture.finish(segment->point+QPointF(60,70)));const auto result=gesture.preview();
        QCOMPARE(result.wireRouteMode,QString("manual"));QCOMPARE(result.sourcePoints,item.sourcePoints);
        QCOMPARE(result.startAttachment.objectId,QString("source"));QCOMPARE(result.endAttachment.objectId,QString("target"));
        const auto path=wirePoints(result);QCOMPARE(path.size(),6);QCOMPARE(path[2],QPointF(200,320));QCOMPARE(path[3],QPointF(450,320));
        QCOMPARE(path.first(),item.sourcePoints.first());QCOMPARE(path.last(),item.sourcePoints.last());
        for(int i=1;i<path.size();++i)QVERIFY(qAbs(path[i].x()-path[i-1].x())<.001||qAbs(path[i].y()-path[i-1].y())<.001);
    }
};
QTEST_GUILESS_MAIN(NativeObjectGestureTest)
#include "NativeObjectGestureTest.moc"

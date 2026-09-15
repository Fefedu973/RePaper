#include "NativeSelectionGesture.h"
#include <QtTest>
#include <limits>

using Handle=NativeSelectionGesture::Handle;
class NativeSelectionGestureTest : public QObject {
    Q_OBJECT
private slots:
    void aspectLockUsesUniformScaleAroundEveryOppositeAnchor(){
        const QRectF initial(50,70,200,100);const auto points=NativeSelectionGesture::handlePoints(initial);
        const QVector<QPointF> changes{{-100,-20},{30,-50},{100,-20},{100,30},{100,20},{30,50},{-100,20},{-100,30}};
        for(int i=0;i<8;++i){
            NativeSelectionGesture gesture;QVERIFY(!gesture.lockAspectRatio());
            const auto press=points[i]+QPointF(2,1);
            QVERIFY(gesture.begin(initial,press,1));QVERIFY(gesture.canLockAspectRatio());
            const auto anchor=points[(i+4)%8];QCOMPARE(gesture.anchor(),anchor);
            QVERIFY(gesture.lockAspectRatio());QVERIFY(gesture.lockAspectRatio());
            const auto target=press+changes[i];
            QVERIFY(gesture.update(target));QVERIFY(gesture.update(target));
            QCOMPARE(gesture.scaleX(),qreal(1.5));QCOMPARE(gesture.scaleY(),qreal(1.5));
            QCOMPARE(gesture.previewTransform().map(anchor),anchor);
            QCOMPARE(gesture.previewTransform().mapRect(initial),gesture.previewRect());
            QCOMPARE(gesture.previewRect().topLeft(),anchor+(initial.topLeft()-anchor)*1.5);
            QVERIFY(gesture.update(press));QCOMPARE(gesture.previewRect(),initial);
            QVERIFY(gesture.finish(target));QVERIFY(!gesture.active());QVERIFY(gesture.aspectRatioLocked());
            QVERIFY(!gesture.canLockAspectRatio());
            QVERIFY(gesture.begin(initial,press,1));QVERIFY(!gesture.aspectRatioLocked());
        }
    }
    void aspectLockedCrossingClampsBothDimensionsWithoutMirroring(){
        const QRectF initial(50,70,200,100);const auto points=NativeSelectionGesture::handlePoints(initial);
        const QVector<QPointF> changes{{1000,1000},{0,1000},{-1000,1000},{-1000,0},{-1000,-1000},{0,-1000},{1000,-1000},{1000,0}};
        for(int i=0;i<8;++i){
            NativeSelectionGesture gesture;QVERIFY(gesture.begin(initial,points[i],1));QVERIFY(gesture.lockAspectRatio());
            QVERIFY(gesture.update(points[i]+changes[i]));
            QCOMPARE(gesture.previewRect().size(),QSizeF(2,1));
            QCOMPARE(gesture.scaleX(),qreal(.01));QCOMPARE(gesture.scaleY(),qreal(.01));
            QCOMPARE(gesture.previewTransform().map(gesture.anchor()),gesture.anchor());
        }
    }
    void aspectLockClampsUniformlyToPageBoundsForEveryHandle(){
        const QRectF initial(50,70,200,100),page(0,0,400,300);
        const auto points=NativeSelectionGesture::handlePoints(initial);
        const QVector<QPointF> changes{{-1000,-1000},{0,-1000},{1000,-1000},{1000,0},{1000,1000},{0,1000},{-1000,1000},{-1000,0}};
        const QVector<qreal> expectedScales{1.25,1.5,1.7,1.75,1.75,1.5,1.25,1.25};
        for(int i=0;i<8;++i){
            NativeSelectionGesture gesture;QVERIFY(gesture.begin(initial,points[i],1,page));QVERIFY(gesture.lockAspectRatio());
            QVERIFY(gesture.finish(points[i]+changes[i]));
            QVERIFY(page.contains(gesture.previewRect()));
            QCOMPARE(gesture.scaleX(),expectedScales[i]);QCOMPARE(gesture.scaleY(),expectedScales[i]);
            QCOMPARE(gesture.previewTransform().map(gesture.anchor()),gesture.anchor());
        }
    }
    void fractionalDimensionsCanReachMinimumSize(){
        const QRectF initial(-111.1,-222.2,600.6,333.33);NativeSelectionGesture gesture;
        QVERIFY(gesture.begin(initial,initial.bottomRight(),1));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(gesture.finish(initial.topLeft()-QPointF(10,10)));
        QVERIFY(gesture.previewRect().width()>=1);QVERIFY(gesture.previewRect().height()>=1);
        QVERIFY(qAbs(gesture.previewRect().height()-1)<.000001);
        QVERIFY(qAbs(gesture.scaleX()-gesture.scaleY())<.000001);
        QCOMPARE(gesture.previewRect().topLeft(),initial.topLeft());
    }
    void aspectLockRespectsGlobalBoundsAndEligibility(){
        const QRectF initial(-100,-50,200,100);NativeSelectionGesture gesture;
        QVERIFY(gesture.begin(initial,initial.bottomRight(),1));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(gesture.update({1000000,1000000}));
        QCOMPARE(gesture.previewRect(),QRectF(-100,-50,1000100,500050));
        QCOMPARE(gesture.scaleX(),gesture.scaleY());
        gesture.cancel();QVERIFY(!gesture.aspectRatioLocked());QCOMPARE(gesture.previewRect(),initial);
        for(const auto point:{initial.center(),NativeSelectionGesture::rotationHandlePoint(initial,1)}){
            QVERIFY(gesture.begin(initial,point,1));QVERIFY(!gesture.canLockAspectRatio());QVERIFY(!gesture.lockAspectRatio());
        }
        QVERIFY(gesture.begin(initial,initial.bottomRight(),1));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(!gesture.finish({std::numeric_limits<qreal>::quiet_NaN(),0}));
        QVERIFY(!gesture.aspectRatioLocked());QCOMPARE(gesture.previewRect(),initial);
        QVERIFY(gesture.previewTransform().isIdentity());
    }
    void cornersResizeAxesIndependentlyFromTheOppositeAnchor() {
        const QRectF initial(50,70,200,100);
        NativeSelectionGesture gesture;
        QVERIFY(gesture.begin(initial,initial.bottomRight(),1));
        QCOMPARE(gesture.handle(),Handle::BottomRight);QCOMPARE(gesture.anchor(),initial.topLeft());
        QVERIFY(gesture.update({350,120}));
        QCOMPARE(gesture.previewRect(),QRectF(50,70,300,50));
        QCOMPARE(gesture.scaleX(),qreal(1.5));QCOMPARE(gesture.scaleY(),qreal(0.5));
        QCOMPARE(gesture.previewTransform().mapRect(initial),gesture.previewRect());
        QCOMPARE(gesture.previewTransform().map(initial.topLeft()),initial.topLeft());
        QVERIFY(gesture.finish({350,120}));QVERIFY(!gesture.active());
        QCOMPARE(gesture.previewRect(),QRectF(50,70,300,50));
    }
    void everyHandleMovesOnlyItsEdges_data() {
        QTest::addColumn<int>("index");QTest::addColumn<QRectF>("expected");
        QTest::newRow("top-left")<<0<<QRectF(70,60,180,110);
        QTest::newRow("top")<<1<<QRectF(50,60,200,110);
        QTest::newRow("top-right")<<2<<QRectF(50,60,220,110);
        QTest::newRow("right")<<3<<QRectF(50,70,220,100);
        QTest::newRow("bottom-right")<<4<<QRectF(50,70,220,90);
        QTest::newRow("bottom")<<5<<QRectF(50,70,200,90);
        QTest::newRow("bottom-left")<<6<<QRectF(70,70,180,90);
        QTest::newRow("left")<<7<<QRectF(70,70,180,100);
    }
    void everyHandleMovesOnlyItsEdges() {
        QFETCH(int,index);QFETCH(QRectF,expected);
        const QRectF initial(50,70,200,100);const auto point=NativeSelectionGesture::handlePoints(initial).at(index);
        NativeSelectionGesture gesture;QVERIFY(gesture.begin(initial,point,1));
        const auto anchor=gesture.anchor();
        QVERIFY(gesture.update(point+QPointF(20,-10)));
        QCOMPARE(gesture.previewRect(),expected);
        QCOMPARE(gesture.previewTransform().map(anchor),anchor);
    }
    void repeatedUpdatesAreAbsoluteAndPreservePointerOffset() {
        const QRectF initial(0,0,200,100);NativeSelectionGesture gesture;
        QVERIFY(gesture.begin(initial,{204,103},1));
        QVERIFY(gesture.update({304,53}));QCOMPARE(gesture.previewRect(),QRectF(0,0,300,50));
        for(int i=0;i<100;++i)QVERIFY(gesture.update({304,53}));
        QCOMPARE(gesture.previewRect(),QRectF(0,0,300,50));
        QVERIFY(gesture.update({204,103}));QCOMPARE(gesture.previewRect(),initial);
        QVERIFY(gesture.previewTransform().isIdentity());
    }
    void handleRadiusIsMeasuredInScreenPixels() {
        const QRectF rect(0,0,200,100);
        QCOMPARE(NativeSelectionGesture::hitTest(rect,{207,100},2),Handle::BottomRight);
        QCOMPARE(NativeSelectionGesture::hitTest(rect,{208,100},2),Handle::None);
        QCOMPARE(NativeSelectionGesture::hitTest(rect,{220,100},0.5),Handle::BottomRight);
        QCOMPARE(NativeSelectionGesture::hitTest(rect,{230,100},0.5),Handle::None);
        QCOMPARE(NativeSelectionGesture::hitTest(rect,rect.center(),1),Handle::Move);
        QCOMPARE(NativeSelectionGesture::handlePoints(rect).size(),8);
    }
    void rotationHandleMaintainsScreenOffsetAndHitRadius() {
        const QRectF rect(50,70,200,100);
        for(const qreal scale:{0.5,1.0,2.0}){
            const auto point=NativeSelectionGesture::rotationHandlePoint(rect,scale);
            QCOMPARE(point.x(),rect.center().x());
            QCOMPARE((rect.top()-point.y())*scale,NativeSelectionGesture::RotationHandleOffsetPixels);
            QCOMPARE(NativeSelectionGesture::hitTest(rect,point,scale),Handle::Rotate);
            QCOMPARE(NativeSelectionGesture::hitTest(rect,point+QPointF(14/scale,0),scale),Handle::Rotate);
            QCOMPARE(NativeSelectionGesture::hitTest(rect,point+QPointF(15/scale,0),scale),Handle::None);
            QCOMPARE(NativeSelectionGesture::hitTest(rect,{rect.center().x(),rect.top()},scale),Handle::Top);
        }
        QCOMPARE(NativeSelectionGesture::handleName(Handle::Rotate),QString("rotate"));
    }
    void freeRotationPreservesCenterAndDistancesAtNonRightAngles() {
        const QRectF initial(50,70,200,100);const auto center=initial.center();
        const auto start=NativeSelectionGesture::rotationHandlePoint(initial,1);
        // Beginning slightly off the visual handle must not cause a jump.
        const auto press=start+QPointF(4,3);
        NativeSelectionGesture gesture;QVERIFY(gesture.begin(initial,press,1));
        QCOMPARE(gesture.handle(),Handle::Rotate);QCOMPARE(gesture.anchor(),center);
        const auto pointAt=[&](qreal angle){
            QTransform transform;transform.translate(center.x(),center.y());
            transform.rotate(angle);transform.translate(-center.x(),-center.y());
            return transform.map(press);
        };
        for(const qreal angle:{37.0,-63.5,179.0,-179.0,0.0}){
            QVERIFY(gesture.update(pointAt(angle)));
            QVERIFY(qAbs(gesture.rotationAngleDegrees()-angle)<0.000001);
            const auto transform=gesture.previewTransform();
            QVERIFY(QLineF(transform.map(center),center).length()<0.000001);
            QVERIFY(qAbs(QLineF(transform.map(initial.topLeft()),transform.map(initial.topRight())).length()-initial.width())<0.000001);
            QCOMPARE(gesture.previewRect(),transform.mapRect(initial));
            QCOMPARE(gesture.scaleX(),qreal(1));QCOMPARE(gesture.scaleY(),qreal(1));
            QCOMPARE(gesture.delta(),QPointF());
        }
        QVERIFY(gesture.finish(pointAt(37)));QVERIFY(!gesture.active());
        QVERIFY(qAbs(gesture.rotationAngleDegrees()-37)<0.000001);
        QCOMPARE(gesture.handle(),Handle::Rotate);
    }
    void rotationThroughCenterAndOutsidePageKeepsLastValidOrientation() {
        const QRectF initial(20,40,160,80);const auto center=initial.center();
        NativeSelectionGesture gesture;
        const auto start=NativeSelectionGesture::rotationHandlePoint(initial,1);
        QVERIFY(gesture.begin(initial,start,1,QRectF(0,0,200,160)));
        QVERIFY(gesture.update(center));QCOMPARE(gesture.rotationAngleDegrees(),qreal(0));
        QVERIFY(gesture.update(center+QPointF(0,100)));
        QVERIFY(qAbs(qAbs(gesture.rotationAngleDegrees())-180)<0.000001);
        const auto validAngle=gesture.rotationAngleDegrees();
        // At 45 degrees the rotated rectangle exceeds the vertical page bounds.
        QVERIFY(gesture.update(center+QPointF(100,-100)));
        QCOMPARE(gesture.rotationAngleDegrees(),validAngle);
        QVERIFY(QRectF(0,0,200,160).contains(gesture.previewRect()));
        gesture.cancel();QCOMPARE(gesture.rotationAngleDegrees(),qreal(0));
        QCOMPARE(gesture.previewRect(),initial);QVERIFY(gesture.previewTransform().isIdentity());
        QVERIFY(gesture.begin(initial,start,1));QVERIFY(gesture.update(center+QPointF(100,0)));
        QVERIFY(!gesture.finish({std::numeric_limits<qreal>::quiet_NaN(),0}));
        QCOMPARE(gesture.rotationAngleDegrees(),qreal(0));QVERIFY(gesture.previewTransform().isIdentity());
    }
    void movingIsClampedToBoundsWithoutResizing() {
        const QRectF initial(50,70,200,100);NativeSelectionGesture gesture;
        QVERIFY(gesture.begin(initial,initial.center(),1,QRectF(0,0,400,300)));
        QCOMPARE(gesture.handle(),Handle::Move);
        QVERIFY(gesture.update({900,900}));QCOMPARE(gesture.previewRect(),QRectF(200,200,200,100));
        QVERIFY(gesture.update({-900,-900}));QCOMPARE(gesture.previewRect(),QRectF(0,0,200,100));
        QCOMPARE(gesture.scaleX(),qreal(1));QCOMPARE(gesture.scaleY(),qreal(1));
        QCOMPARE(gesture.previewTransform().mapRect(initial),gesture.previewRect());
    }
    void crossingNeverMirrorsAndBoundsLimitResize() {
        const QRectF initial(50,70,200,100);NativeSelectionGesture gesture;
        QVERIFY(gesture.begin(initial,initial.topLeft(),1,QRectF(0,0,400,300)));
        QVERIFY(gesture.update({900,900}));QCOMPARE(gesture.previewRect(),QRectF(249,169,1,1));
        QVERIFY(gesture.scaleX()>0);QVERIFY(gesture.scaleY()>0);
        QVERIFY(gesture.update({-900,-900}));QCOMPARE(gesture.previewRect(),QRectF(0,0,250,170));
        QCOMPARE(gesture.previewTransform().map(initial.bottomRight()),initial.bottomRight());
    }
    void cancelAndInvalidReleaseRollbackExactly() {
        const QRectF initial(-100,-70,200,100);NativeSelectionGesture gesture;
        QVERIFY(gesture.begin(initial,initial.bottomRight(),1));QVERIFY(gesture.update({500,700}));
        gesture.cancel();QVERIFY(!gesture.active());QCOMPARE(gesture.previewRect(),initial);
        QVERIFY(gesture.previewTransform().isIdentity());QVERIFY(!gesture.finish({500,700}));
        QVERIFY(gesture.begin(initial,initial.bottomRight(),1));QVERIFY(gesture.update({500,700}));
        QVERIFY(!gesture.finish({std::numeric_limits<qreal>::quiet_NaN(),0}));
        QCOMPARE(gesture.previewRect(),initial);QVERIFY(gesture.previewTransform().isIdentity());
    }
    void invalidContextsCannotStartOrEscapeCoordinateLimits() {
        NativeSelectionGesture gesture;const QRectF rect(0,0,200,100);
        QVERIFY(!gesture.begin(rect,rect.center(),0));
        QVERIFY(!gesture.begin(rect,{1000001,0},1));
        QVERIFY(!gesture.begin(QRectF(0,0,-200,100),{0,0},1));
        QVERIFY(!gesture.begin(rect,rect.center(),1,QRectF(0,0,100,100)));
        QVERIFY(!gesture.begin(rect,rect.center(),1,QRectF(0,0,400,0)));
        QVERIFY(gesture.begin(rect,rect.bottomRight(),1));
        QVERIFY(gesture.update({1000000,1000000}));
        QCOMPARE(gesture.previewRect(),QRectF(0,0,1000000,1000000));
        QVERIFY(!gesture.update({1000001,0}));
        QVERIFY(!gesture.finish({1000001,0}));QCOMPARE(gesture.previewRect(),rect);
    }
};
QTEST_APPLESS_MAIN(NativeSelectionGestureTest)
#include "NativeSelectionGestureTest.moc"

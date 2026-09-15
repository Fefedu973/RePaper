#include "NativeGesture.h"
#include <QtTest>
#include <limits>
class NativeGestureTest:public QObject {
    Q_OBJECT
private slots:
    void solidFreehandKeepsExactModelGeometryWithoutRebuildingSharedSamples(){
        NativeGesture gesture;gesture.tool="pen";gesture.color=QColor("#136aca");gesture.width=7;
        QVERIFY(gesture.begin({0,0}));
        for(int batch=0;batch<64;++batch){
            QVector<QPointF> points;points.reserve(128);
            for(int i=1;i<=128;++i){const int n=batch*128+i;points.append({qreal(n%100),qreal(n/100)});}
            QVERIFY(gesture.moveBatch(points));
        }
        const auto previous=gesture.preview().first().points;
        const auto last=previous.last();
        QVERIFY(gesture.moveBatch(QVector<QPointF>(256,last)));
        QCOMPARE(gesture.preview().first().points.constData(),previous.constData());
        repaper::drawing::Item item;QVERIFY(gesture.finish(last,&item));
        QCOMPARE(item.sourcePoints,previous);QCOMPARE(item.strokes.first().points,previous);
        auto reference=item;reference.strokes={};QVERIFY(repaper::drawing::rebuild(reference));
        repaper::drawing::setForegroundColor(reference,gesture.color);
        QCOMPARE(item.strokes.size(),reference.strokes.size());
        QCOMPARE(item.strokes.first().points,reference.strokes.first().points);
        QCOMPARE(item.strokes.first().width,reference.strokes.first().width);
        QCOMPARE(item.strokes.first().color,reference.strokes.first().color);
    }
    void freehandBudgetFailureDoesNotAppendHalfOfABatch(){
        NativeGesture gesture;gesture.tool="pen";QVERIFY(gesture.begin({0,0}));
        for(int i=1;i<20000;){
            QVector<QPointF> points;
            for(int end=qMin(i+256,20000);i<end;++i)points.append({qreal(i%100),qreal(i/100)});
            QVERIFY(gesture.moveBatch(points));
        }
        const auto previous=gesture.preview().first().points;
        QCOMPARE(previous.size(),qsizetype(20000));
        QVERIFY(!gesture.moveBatch({{500,500},{600,600}}));
        QCOMPARE(gesture.preview().first().points,previous);
        repaper::drawing::Item item;QVERIFY(gesture.finish(previous.last(),&item));
        QCOMPARE(item.sourcePoints,previous);
    }
    void wireAcceptsSamplesThatRemainSnappedToTheStartingPort(){
        using namespace repaper::drawing;
        Item target;target.kind="wire";target.sourcePoints={{100,300},{300,300}};QVERIFY(routeWire(target,{}));
        NativeGesture gesture;gesture.tool="wire";gesture.wireDocument={target};
        gesture.wireStart={target.id,"end"};
        QVERIFY(gesture.begin({300,300}));QVERIFY(gesture.active());
        // The native snapper returns the same terminal while the pen remains
        // inside its 14-view-pixel radius. These are valid input samples.
        gesture.wireEnd={target.id,"end"};
        QVERIFY(gesture.moveBatch({{300,300},{300,300}}));
        QVERIFY(gesture.active());QVERIFY(gesture.preview().isEmpty());
        QCOMPARE(gesture.inputSummary({300,300}).value("wireDeferredMoveSamples").toInt(),2);
        QVERIFY(gesture.inputSummary({300,300}).value("wireRoutePending").toBool());
        gesture.wireEnd={};
        QVERIFY(gesture.move({450,460}));QVERIFY(!gesture.preview().isEmpty());
        QVERIFY(!gesture.inputSummary({450,460}).value("wireRoutePending").toBool());
        Item result;QVERIFY(gesture.finish({500,460},&result));
        QCOMPARE(result.startAttachment.objectId,target.id);QCOMPARE(result.startAttachment.portId,QString("end"));
        QCOMPARE(result.sourcePoints.first(),QPointF(300,300));QCOMPARE(result.sourcePoints.last(),QPointF(500,460));
    }
    void wireKeepsItsLastValidPreviewAcrossATemporarilyBlockedEndpoint(){
        using namespace repaper::drawing;
        Item obstacle;obstacle.kind="symbol";obstacle.symbolId="resistor-iec";
        obstacle.sourcePoints={{200,200},{440,200},{200,360}};QVERIFY(rebuild(obstacle));
        const QPointF start(100,100),valid(500,100),blocked(320,194),recovered(500,460);
        Item probe;probe.kind="wire";probe.sourcePoints={start,blocked};
        QVERIFY(!routeWire(probe,{obstacle})); // Endpoint lies in the clearance band, outside the component itself.
        NativeGesture gesture;gesture.tool="wire";gesture.wireDocument={obstacle};
        QVERIFY(gesture.begin(start));QVERIFY(gesture.move(valid));
        const auto previous=gesture.preview();QVERIFY(!previous.isEmpty());
        QVERIFY(gesture.move(blocked));QVERIFY(gesture.active());
        QCOMPARE(gesture.preview().size(),previous.size());
        for(int i=0;i<previous.size();++i)QCOMPARE(gesture.preview()[i].points,previous[i].points);
        QVERIFY(gesture.move(recovered));
        Item result;QVERIFY(gesture.finish(recovered,&result));
        QCOMPARE(result.sourcePoints.first(),start);QCOMPARE(result.sourcePoints.last(),recovered);
        QVERIFY(!result.strokes.isEmpty());
        QVERIFY(gesture.begin(start));QVERIFY(gesture.move(valid));QVERIFY(gesture.move(blocked));
        Item untouched;untouched.id="unmodified-output";
        QVERIFY(!gesture.finish(blocked,&untouched));
        QCOMPARE(untouched.id,QString("unmodified-output"));QVERIFY(untouched.strokes.isEmpty());
        QVERIFY(!gesture.active());QVERIFY(gesture.preview().isEmpty());
    }
    void pendingWireStillRejectsMalformedSamplesAndClearsOnCancel(){
        NativeGesture gesture;gesture.tool="wire";QVERIFY(gesture.begin({100,100}));
        QVERIFY(gesture.move({100,100}));
        QVERIFY(!gesture.moveBatch({{200,100},{std::numeric_limits<qreal>::quiet_NaN(),100}}));
        QVERIFY(gesture.preview().isEmpty());
        gesture.cancel();QVERIFY(!gesture.active());
        QVERIFY(gesture.begin({500,500}));
        QCOMPARE(gesture.inputSummary({500,500}).value("wireDeferredMoveSamples").toInt(),0);
        QVERIFY(gesture.move({700,600}));
        repaper::drawing::Item result;QVERIFY(gesture.finish({700,600},&result));
        QCOMPARE(result.sourcePoints,PaperDrawing::Polyline({{500,500},{700,600}}));
    }
    void tapUsesLogicalAnchorAndKeepsPreviewAtThePress(){
        using namespace repaper::drawing;
        for(const auto &id:QStringList{"resistor-iec","capacitor","voltage-generator-simple","voltage-source","opamp","npn","ground","square-root","table"}){
            NativeGesture gesture;gesture.tool="symbol";gesture.symbolId=id;
            gesture.stencilParameters=PaperDrawing::stencilDefaults(id);
            const QPointF press(500,400);QVERIFY(gesture.begin(press));
            const auto preview=gesture.preview();QVERIFY(!preview.isEmpty());
            QVERIFY(gesture.move(press+QPointF(1,1)));
            QCOMPARE(gesture.preview().first().points,preview.first().points);
            Item item;QVERIFY(gesture.finish(press+QPointF(2,1),&item));
            QCOMPARE(defaultStencilAnchor(item),press);
            QCOMPARE(item.strokes.first().points,preview.first().points);
            QVERIFY(!gesture.stencilPlacement().snapped);
        }
    }
    void voltageDecorationDoesNotMoveThePlacementAnchor(){
        using namespace repaper::drawing;
        for(int vertical=0;vertical<2;++vertical){
            NativeGesture gesture;gesture.tool="symbol";gesture.symbolId="resistor-iec";
            gesture.symbolQuarterTurns=vertical;gesture.voltageArrow=true;
            gesture.voltageArrowOtherSide=true;gesture.voltageArrowReversed=true;
            const QPointF press(500,400);QVERIFY(gesture.begin(press));
            Item item;QVERIFY(gesture.finish(press,&item));
            QCOMPARE((item.anchors[0]+item.anchors[1])/2,press);
        }
    }
    void firstPressSnapsPortAndSizingKeepsThatPortFixed(){
        using namespace repaper::drawing;
        Item target;target.kind="symbol";target.symbolId="resistor-iec";
        target.sourcePoints={{200,100},{320,100},{200,180}};QVERIFY(rebuild(target));
        NativeGesture gesture;gesture.tool="symbol";gesture.symbolId="capacitor";
        gesture.wireDocument={target};gesture.alignmentTolerance=8;
        const QPointF press=target.anchors[1]+QPointF(3,2);
        QVERIFY(gesture.begin(press));QVERIFY(gesture.stencilPlacement().snapped);
        QCOMPARE(gesture.stencilPlacement().targetPoint,target.anchors[1]);
        const auto port=gesture.stencilPlacement().localPortId;
        QVERIFY(gesture.move(press+QPointF(240,160)));
        QCOMPARE(gesture.stencilPlacement().localPortId,port);
        Item item;QVERIFY(gesture.finish(press+QPointF(240,160),&item));
        const int index=item.portIds.indexOf(port);QVERIFY(index>=0);
        QCOMPARE(item.anchors[index],target.anchors[1]);
        QCOMPARE(boxWidth(item),qreal(240));QCOMPARE(boxHeight(item),qreal(160));
        QVERIFY(gesture.alignmentGuides().isEmpty());QVERIFY(!gesture.stencilPlacement().snapped);
        QVERIFY(gesture.begin({900,800}));QVERIFY(!gesture.stencilPlacement().snapped);
        gesture.cancel();QVERIFY(gesture.preview().isEmpty());
    }
    void nearbyWireEndpointSnapsOnFirstPressWithoutWaitingForMovement(){
        using namespace repaper::drawing;
        Item wire;wire.kind="wire";wire.sourcePoints={{100,300},{300,300}};QVERIFY(routeWire(wire,{}));
        for(const auto &target:wire.sourcePoints){
            NativeGesture gesture;gesture.tool="symbol";gesture.symbolId="resistor-iec";
            gesture.wireDocument={wire};gesture.alignmentTolerance=8;
            QVERIFY(gesture.begin(target+QPointF(2,2)));QVERIFY(gesture.stencilPlacement().snapped);
            const auto local=gesture.stencilPlacement().localPortId;
            Item item;QVERIFY(gesture.finish(target+QPointF(2,2),&item));
            QCOMPARE(item.anchors[item.portIds.indexOf(local)],target);
            const auto center=defaultStencilAnchor(item);
            QVERIFY(target.x()==100?center.x()<100:center.x()>300);
        }
    }
    void verticalTapAndDragKeepPortsVoltageAndAspect(){
        NativeGesture gesture;gesture.tool="symbol";gesture.symbolId="resistor-iec";
        gesture.symbolQuarterTurns=1;gesture.voltageArrow=true;gesture.voltageArrowReversed=true;
        gesture.color=QColor("#d90707");
        QVERIFY(gesture.begin({100,100}));
        repaper::drawing::Item item;QVERIFY(gesture.finish({100,100},&item));
        QCOMPARE(item.anchors.size(),2);QCOMPARE(item.anchors[0].x(),item.anchors[1].x());
        QVERIFY(item.anchors[1].y()>item.anchors[0].y());
        QVERIFY(item.voltageArrow);QVERIFY(item.voltageArrowReversed);
        QVERIFY(item.strokes.size()>2);for(const auto &stroke:item.strokes)QCOMPARE(stroke.color,gesture.color);
        QVERIFY(gesture.begin({100,100}));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(gesture.finish({150,280},&item));
        QCOMPARE(repaper::drawing::boxWidth(item),qreal(180));
        QCOMPARE(repaper::drawing::boxHeight(item),qreal(120));
    }
    void voltagePreferencesDoNotBreakOtherTools(){
        NativeGesture gesture;gesture.voltageArrow=true;gesture.voltageArrowReversed=true;gesture.voltageArrowOtherSide=true;
        for(const auto &tool:QStringList{"rectangle","ellipse","line","wire","symbol"}){
            gesture.tool=tool;gesture.symbolId="square-root";
            QVERIFY(gesture.begin({100,100}));repaper::drawing::Item item;
            QVERIFY(gesture.finish({280,220},&item));
            QVERIFY(!item.voltageArrow);QVERIFY(!item.voltageArrowReversed);QVERIFY(!item.voltageArrowOtherSide);
        }
    }
    void placementGuidesSnapWithoutAddingInk(){
        repaper::drawing::Item target;target.id="target";target.kind="symbol";target.symbolId="resistor-iec";
        target.sourcePoints={{200,100},{320,100},{200,180}};QVERIFY(repaper::drawing::rebuild(target));
        NativeGesture gesture;gesture.tool="symbol";gesture.symbolId="resistor-iec";
        gesture.wireDocument={target};gesture.alignmentTolerance=8;
        QVERIFY(gesture.begin({264,300}));QVERIFY(!gesture.alignmentGuides().isEmpty());
        repaper::drawing::Item item;QVERIFY(gesture.finish({264,300},&item));
        QCOMPARE(item.sourcePoints[0].x(),qreal(200));QCOMPARE(item.strokes.size(),target.strokes.size());
        QVERIFY(gesture.alignmentGuides().isEmpty());
        QVERIFY(gesture.begin({264,300}));gesture.cancel();QVERIFY(gesture.alignmentGuides().isEmpty());
    }
    void configuredStencilsPreserveParametersAndWhiteBackgroundThroughGesture() {
        for (const auto &id:QStringList{"table","graph","bode"}) {
            NativeGesture gesture; gesture.tool="symbol"; gesture.symbolId=id;
            gesture.stencilParameters=PaperDrawing::stencilDefaults(id);
            gesture.color=QColor("#136aca"); gesture.width=3;
            if(id=="table")gesture.stencilParameters.insert("rows",7);
            if(id=="graph")gesture.stencilParameters.insert("curveType","second-order");
            if(id=="bode")gesture.stencilParameters.insert("curve",true);
            QVERIFY(gesture.begin({100,100})); QVERIFY(gesture.move({700,500}));
            QCOMPARE(gesture.preview().first().color,QColor(Qt::white));
            repaper::drawing::Item item; QVERIFY(gesture.finish({700,500},&item));
            QCOMPARE(item.stencilParameters,gesture.stencilParameters);
            QCOMPARE(repaper::drawing::foregroundColor(item),gesture.color);
            QCOMPARE(item.strokes.first().color,QColor(Qt::white));
            QVERIFY(item.strokes.size()>2);
            for(qsizetype i=1;i<item.strokes.size();++i)QCOMPARE(item.strokes[i].color,gesture.color);
            QCOMPARE(repaper::drawing::boxWidth(item),qreal(600));
            QCOMPARE(repaper::drawing::boxHeight(item),qreal(400));
        }
    }
    void aspectLockedCreationKeepsQuadrantsAndNominalRatio(){
        const QPointF start(500,500);
        for(const auto &tool:QStringList{"rectangle","ellipse","symbol"})for(const int sx:{-1,1})for(const int sy:{-1,1}){
            NativeGesture gesture;gesture.tool=tool;gesture.symbolId="resistor-iec";gesture.width=7;
            QVERIFY(!gesture.canLockAspectRatio());QVERIFY(!gesture.lockAspectRatio());
            QVERIFY(gesture.begin(start));QVERIFY(gesture.canLockAspectRatio());
            QVERIFY(gesture.lockAspectRatio());QVERIFY(gesture.lockAspectRatio());QVERIFY(gesture.aspectRatioLocked());
            // The dominant axis can change during one gesture; release is final.
            QVERIFY(gesture.move(start+QPointF(sx*60,sy*140)));
            QVERIFY(gesture.moveBatch({start+QPointF(sx*100,sy*100),start+QPointF(sx*160,sy*70)}));
            repaper::drawing::Item result;
            QVERIFY(gesture.finish(start+QPointF(sx*180,sy*80),&result));
            const qreal height=tool=="symbol"?120:180;
            QCOMPARE(repaper::drawing::boxWidth(result),qreal(180));
            QCOMPARE(repaper::drawing::boxHeight(result),height);
            QCOMPARE(result.sourcePoints[0],start+QPointF(sx<0?-180:0,sy<0?-height:0));
            QCOMPARE(result.width,qreal(7));for(const auto &stroke:result.strokes)QCOMPARE(stroke.width,qreal(7));
            QVERIFY(!gesture.active());QVERIFY(gesture.aspectRatioLocked());QVERIFY(!gesture.canLockAspectRatio());
            QVERIFY(gesture.begin(start));QVERIFY(!gesture.aspectRatioLocked());
            QVERIFY(gesture.lockAspectRatio());
            QVERIFY(gesture.finish(start+QPointF(sx*40,sy*160),&result));
            QCOMPARE(repaper::drawing::boxWidth(result),tool=="symbol"?qreal(240):qreal(160));
            QCOMPARE(repaper::drawing::boxHeight(result),qreal(160));
        }
    }
    void aspectLockKeepsSymbolTapAndCancelsCleanly(){
        NativeGesture gesture;gesture.tool="symbol";gesture.symbolId="resistor-iec";
        QVERIFY(gesture.begin({100,100}));QVERIFY(gesture.lockAspectRatio());
        repaper::drawing::Item result;QVERIFY(gesture.finish({100,100},&result));
        QCOMPARE(repaper::drawing::boxWidth(result),qreal(120));QCOMPARE(repaper::drawing::boxHeight(result),qreal(80));
        gesture.cancel();QVERIFY(!gesture.aspectRatioLocked());
        for(const auto &tool:QStringList{"pen","line","arrow","wire"}){
            gesture.tool=tool;QVERIFY(gesture.begin({100,100}));
            QVERIFY(!gesture.canLockAspectRatio());QVERIFY(!gesture.lockAspectRatio());QVERIFY(!gesture.aspectRatioLocked());
        }
        gesture.tool="rectangle";QVERIFY(gesture.begin({100,100}));QVERIFY(gesture.lockAspectRatio());
        QVERIFY(!gesture.finish({std::numeric_limits<qreal>::infinity(),0},&result));
        QVERIFY(!gesture.aspectRatioLocked());QVERIFY(gesture.preview().isEmpty());
    }
    void aspectLockedCreationClampsBothDimensionsTogether(){
        NativeGesture gesture;gesture.tool="rectangle";
        QVERIFY(gesture.begin({19900,100}));QVERIFY(gesture.lockAspectRatio());
        repaper::drawing::Item result;QVERIFY(gesture.finish({19950,1000},&result));
        QCOMPARE(repaper::drawing::boxWidth(result),qreal(100));QCOMPARE(repaper::drawing::boxHeight(result),qreal(100));
        QCOMPARE(result.sourcePoints[1],QPointF(20000,100));
    }
    void batchedFreehandKeepsCornersAndExactRelease(){
        NativeGesture gesture;gesture.tool="pen";
        QVERIFY(gesture.begin({10,10}));
        const QVector<QPointF> corners{{80,10},{80,70},{20,70},{20,25}};
        QVERIFY(gesture.moveBatch(corners));
        QCOMPARE(gesture.preview().first().points,PaperDrawing::Polyline({{10,10},{80,10},{80,70},{20,70},{20,25}}));
        repaper::drawing::Item item;QVERIFY(gesture.finish({30,25},&item));
        QCOMPARE(item.sourcePoints.last(),QPointF(30,25));
        QCOMPARE(item.sourcePoints.size(),6);
    }
    void batchedShapeUsesFinalEndpointButCountsAllSamples(){
        NativeGesture gesture;gesture.tool="arrow";
        QVERIFY(gesture.begin({10,10}));
        QVERIFY(gesture.moveBatch({{300,10},{100,100},{100,30}}));
        const auto summary=gesture.inputSummary({100,30});
        QCOMPARE(summary.value("moveSamples").toInt(),3);
        QCOMPARE(summary.value("maxDistance").toInt(),290);
        repaper::drawing::Item item;QVERIFY(gesture.finish({110,40},&item));
        QCOMPARE(item.sourcePoints,PaperDrawing::Polyline({{10,10},{110,40}}));
    }
    void malformedBatchDoesNotAppendPartialFreehand(){
        NativeGesture gesture;gesture.tool="pen";QVERIFY(gesture.begin({10,10}));
        QVERIFY(!gesture.moveBatch({{20,20},{std::numeric_limits<qreal>::quiet_NaN(),30}}));
        repaper::drawing::Item item;QVERIFY(gesture.finish({30,30},&item));
        QCOMPARE(item.sourcePoints,PaperDrawing::Polyline({{10,10},{30,30}}));
    }
    void chosenColorReachesAllPreviewAndCommittedParts(){
        for(const auto &tool:QStringList{"arrow","rectangle","symbol"}){
            NativeGesture gesture;gesture.tool=tool;gesture.style="dashed";
            gesture.symbolId="resistor-iec";gesture.color=QColor("#d90707");
            QVERIFY(gesture.begin({100,100}));QVERIFY(gesture.move({400,300}));
            for(const auto &stroke:gesture.preview())QCOMPARE(stroke.color,gesture.color);
            repaper::drawing::Item item;QVERIFY(gesture.finish({500,400},&item));
            QVERIFY(!item.strokes.isEmpty());for(const auto &stroke:item.strokes)QCOMPARE(stroke.color,gesture.color);
        }
        NativeGesture invalid;invalid.color=QColor("#00000000");QVERIFY(!invalid.begin({100,100}));
    }
    void inputDiagnosticsExposeMissingMovesWithoutStoringPositions(){
        NativeGesture gesture;gesture.tool="arrow";
        QVERIFY(gesture.begin({100,200}));
        QVERIFY(gesture.move({200,200}));QVERIFY(gesture.move({400,200}));
        const auto summary=gesture.inputSummary({101,200});
        QCOMPARE(summary.value("moveSamples").toInt(),2);
        QCOMPARE(summary.value("maxDistance").toInt(),300);
        QCOMPARE(summary.value("releaseDistance").toInt(),1);
        QCOMPARE(summary.value("lastMoveDistance").toInt(),300);
        QCOMPARE(summary.value("releaseToLastMoveDistance").toInt(),299);
        QCOMPARE(summary.size(),5);
        gesture.cancel();QVERIFY(!gesture.move({900,900}));
        QVERIFY(gesture.begin({0,0}));QCOMPARE(gesture.inputSummary({0,0}).value("moveSamples").toInt(),0);
    }
    void cancelLeavesNoCommittedOrPreviewGeometry(){
        NativeGesture gesture;gesture.tool="arrow";
        QVERIFY(gesture.begin({300,400}));QVERIFY(gesture.move({700,500}));QVERIFY(!gesture.preview().isEmpty());
        gesture.cancel();QVERIFY(!gesture.active());QVERIFY(gesture.preview().isEmpty());
        repaper::drawing::Item result;QVERIFY(!gesture.finish({700,500},&result));
    }
    void arrowMovesEndpointAndCommitsOnlyFinalGeometry(){
        NativeGesture gesture;gesture.tool="arrow";gesture.style="dashed";
        QVERIFY(gesture.begin({100,100}));
        for(int i=1;i<=200;++i)QVERIFY(gesture.move({qreal(100+i),qreal(100+i/2)}));
        repaper::drawing::Item result;QVERIFY(gesture.finish({900,700},&result));
        QCOMPARE(result.kind,QString("arrow"));QCOMPARE(result.sourcePoints,PaperDrawing::Polyline({{100,100},{900,700}}));
        QVERIFY(result.strokes.size()>2);QVERIFY(!gesture.active());QVERIFY(gesture.preview().isEmpty());
    }
    void symbolPlacementAndMorphableBoxUseSharedModel(){
        NativeGesture gesture;gesture.tool="symbol";gesture.symbolId="resistor-iec";
        QVERIFY(gesture.begin({-120,40}));repaper::drawing::Item symbol;QVERIFY(gesture.finish({-120,40},&symbol));
        QCOMPARE(symbol.symbolId,QString("resistor-iec"));QVERIFY(!symbol.portIds.isEmpty());
        gesture.tool="rectangle";QVERIFY(gesture.begin({10,20}));repaper::drawing::Item rectangle;
        QVERIFY(gesture.finish({410,220},&rectangle));QCOMPARE(repaper::drawing::boxWidth(rectangle),qreal(400));
        QCOMPARE(repaper::drawing::boxHeight(rectangle),qreal(200));
    }
    void rejectsInvalidInputWithoutAStroke(){
        NativeGesture gesture;QVERIFY(!gesture.begin({std::numeric_limits<qreal>::infinity(),1}));
        QVERIFY(gesture.preview().isEmpty());gesture.tool="rectangle";QVERIFY(gesture.begin({10,20}));
        repaper::drawing::Item result;QVERIFY(!gesture.finish({10,20},&result));QVERIFY(gesture.preview().isEmpty());
    }
};
QTEST_MAIN(NativeGestureTest)
#include "NativeGestureTest.moc"

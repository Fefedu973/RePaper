#include "NativeScene.h"
#include "NativeStrokeSampling.h"
#include "NativeArrowStroke.h"
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>

using namespace RePaperNative;
class WorkflowAccess : public NativeObjectAccess {
    Q_OBJECT
public:
    bool pending=false;
    QString request;
    NativeObjectSnapshot snapshot;
    NativeObjectSnapshot rollbackBaseline,rollbackExpected;
    QVector<quint64> requestedIds,insertIds;
    int inspections=0,selections=0,cancels=0,rollbacks=0,replacements=0;
    std::function<bool()> backgroundPending;
    bool inspect(QObject*,const QVariantMap&) override {if(pending)return false;pending=true;request="inspect";++inspections;return true;}
    bool insert(QObject*,const QVariantMap&,const QVariant&,QPointF) override {
        if(pending)return false;pending=true;request="insert";return true;
    }
    bool select(QObject*,const QVariantMap&,const NativeObjectSnapshot &baseline,const QVector<quint64> &ids) override {
        if(pending)return false;pending=true;request="select";snapshot=baseline;requestedIds=ids;++selections;return true;
    }
    bool replace(QObject*,const QVariantMap&,const NativeObjectSnapshot &baseline,const QVector<quint64> &ids,const QVariant&,QPointF) override {
        if(pending)return false;pending=true;request="replace";snapshot=baseline;requestedIds=ids;++replacements;return true;
    }
    bool cancelPropertySession(QObject*,const QVariantMap&,const NativeObjectSnapshot &baseline,const NativeObjectSnapshot &expected) override {
        if(pending)return false;pending=true;request="cancel-properties";rollbackBaseline=baseline;rollbackExpected=expected;++rollbacks;return true;
    }
    bool busy() const override{return pending;}
    QString reason() const override{return "test-job-failed";}
    NativeObjectSnapshot result() const override{return snapshot;}
    QVector<quint64> insertedIds() const override{return insertIds;}
    void cancel() override{pending=false;++cancels;}
    void complete(const NativeObjectSnapshot &value,bool ok=true){
        snapshot=value;pending=false;emit finished(ok);
        if(backgroundPending)QVERIFY(QTest::qWaitFor([&]{return !backgroundPending();},5000));
    }
    void completeSelection(){auto result=snapshot;result.selectedIds=requestedIds;result.nativeSelectionExact=true;result.fingerprint+="-selected";complete(result);}
};
class WorkflowColor : public NativeSelectionColor {
public:
    bool pending=false;
    NativeHistorySnapshot before,after;
    QColor color;
    bool dispatch(QObject*,int,const QColor &chosen,const NativeHistorySnapshot &expected={}) override {
        if(pending)return false;pending=true;color=chosen;before=expected;return true;
    }
    bool busy() const override{return pending;}
    QString reason() const override{return {};}
    NativeHistorySnapshot beforeHistory() const override{return before;}
    NativeHistorySnapshot afterHistory() const override{return after;}
    void cancel() override{pending=false;before={};after={};}
    void complete(const NativeHistorySnapshot &result,bool success=true){after=result;pending=false;emit finished(success,QString());}
};
class WorkflowHost : public QObject {
    Q_OBJECT
public:
    QVariantMap context{{"documentId","document-a"},{"pageId","page-a"},{"layer",0},{"working",false},{"queueSize",0}};
    int transforms=0,regions=0,clears=0,flushes=0;
    int historyCalls=0,selectionCancels=0;
    bool nativeSelectionPresent=false,allowHistory=true;
    QString lastHistoryAction;
    std::function<void()> flushMoves;
    std::function<void()> historyDispatch;
    QVariantMap lastTransform;
    Q_INVOKABLE QVariant readState(){return context;}
    Q_INVOKABLE QVariant coordinateMappingAvailable(){return true;}
    Q_INVOKABLE QVariant viewToPaper(QVariant p){return p;}
    Q_INVOKABLE QVariant paperToView(QVariant p){return p;}
    Q_INVOKABLE QVariant viewScale(){return 1.;}
    Q_INVOKABLE QVariant activateCustomTool(){return true;}
    Q_INVOKABLE QVariant flushGestureInput(){++flushes;if(flushMoves)flushMoves();return true;}
    Q_INVOKABLE QVariant transformCustomSelection(QVariant change){++transforms;lastTransform=change.toMap();return true;}
    Q_INVOKABLE QVariant selectCustomRegion(QVariant){++regions;return true;}
    Q_INVOKABLE QVariant clearCustomSelection(){++clears;return true;}
    Q_INVOKABLE QVariant selectionAction(QVariant action,QVariant){
        lastHistoryAction=action.toString();
        if(!allowHistory)return false;
        // Exact SceneController::undo first cancels a live native selection;
        // it only dispatches a history command once that selection is gone.
        if(nativeSelectionPresent){++selectionCancels;return true;}
        ++historyCalls;if(historyDispatch)historyDispatch();return true;
    }
    Q_INVOKABLE QVariant presentCommittedPreview(QVariant){return false;}
};
class WorkflowScene : public NativeScene {
public:
    using NativeScene::NativeScene;
    QVector<PaperDrawing::Stroke> preparedStrokes;
    QVariant nativeItems(const QVector<PaperDrawing::Stroke> &strokes) override {preparedStrokes=strokes;return QVariantList{strokes.size()};}
};
namespace {
NativeObjectSnapshot page(){NativeObjectSnapshot s;s.documentId="document-a";s.pageId="page-a";s.layer=0;s.layerId=0x100000000000a;s.complete=s.pendingEditIdentity=s.nativeSelectionExact=true;s.fingerprint="page-initial";s.history.valid=s.history.appendIsolated=true;s.history.historyIdentity=0x1000;return s;}
void historyAppend(NativeObjectSnapshot &snapshot,quint64 id){NativeHistoryCommand command;command.identity=id;command.control=id+0x10;command.type=0x2000;command.cannotMerge=true;snapshot.history.undo.append(command);snapshot.history.redo.clear();}
repaper::drawing::Item resistor(){repaper::drawing::Item item;item.kind="symbol";item.symbolId="resistor-iec";item.sourcePoints={{200,200},{440,200},{200,360}};repaper::drawing::rebuild(item);return item;}
QVector<quint64> add(NativeObjectSnapshot &snapshot,const repaper::drawing::Item &item,quint64 start=100){
    QVector<quint64> ids;
    for(const auto &stroke:item.strokes){NativeObjectLine line;line.id=0x1000000000000+start++;line.parentId=snapshot.layerId;line.lineageId=line.id;line.tool=19;
        line.version=QByteArray::number(line.id);line.contentVersion=line.version;line.maximumPointWidth=quint16(qRound(stroke.width*4));line.uniformPointWidth=true;line.stroke=stroke;line.stroke.points=sampleStrokeForNativeSelection(stroke.points).points;
        ids.append(line.id);snapshot.lines.append(line);}
    return ids;
}
NativeObjectSnapshot moved(NativeObjectSnapshot value,const QTransform &transform,quint64 offset=1000){
    for(auto &line:value.lines){line.id+=offset;line.version+="-changed";for(auto &point:line.stroke.points)point=transform.map(point);}
    value.selectedIds.clear();value.nativeSelectionExact=false;value.fingerprint+="-changed";return value;
}
}
class NativeObjectWorkflowTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QObject controller;
    QQuickWindow window;
    QQuickItem view;
    WorkflowHost host;
    WorkflowAccess access;
    WorkflowColor colorAccess;
    QScopedPointer<NativeScene> scene;
    void beginSelection(const NativeObjectSnapshot &snapshot){QVERIFY(scene->m_objectBindings->observe(snapshot));QVERIFY(scene->loadObjectSelection(snapshot,snapshot.selectedIds));scene->m_objectCacheDirty=false;}
private slots:
    void init(){
        host.context={{"documentId","document-a"},{"pageId","page-a"},{"layer",0},{"working",false},{"queueSize",0},{"canUndo",true},{"canRedo",true}};
        host.transforms=host.regions=host.clears=host.flushes=0;host.flushMoves={};access.pending=false;access.inspections=access.selections=access.rollbacks=0;access.requestedIds.clear();
        access.rollbackBaseline={};access.rollbackExpected={};access.replacements=0;colorAccess.cancel();
        host.historyCalls=host.selectionCancels=0;host.nativeSelectionPresent=false;host.allowHistory=true;
        host.lastHistoryAction.clear();host.historyDispatch={};
        controller.setProperty("pendingEdit",QTransform());controller.setProperty("selectionItemCount",0);
        view.setParentItem(window.contentItem());
        scene.reset(new WorkflowScene(nullptr,&access,directory.path()+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces),&colorAccess));
        access.backgroundPending=[this]{return scene&&scene->m_bindingsPending;};
        scene->attach(&controller,&view,&host);scene->m_target=scene->m_lineType=scene->m_coordinates=scene->m_nativeToolActive=true;scene->m_gesture.tool="select";
    }
    void cleanup(){scene.reset();}
    void backgroundModelsHoldInputGateAndDiscardPreviousPageReceipt(){
        scene->refresh();QVERIFY(access.pending);
        access.backgroundPending={};
        access.complete(page());
        QVERIFY(scene->m_bindingsPending);
        QVERIFY(scene->selectionOperationPending());
        QVERIFY(!scene->pointerBegin(20,20,"pen"));
        host.context["pageId"]="page-b";scene->refresh();
        QVERIFY(!scene->m_bindingsPending);
        QTest::qWait(30);
        QVERIFY(scene->m_objectSnapshot.pageId!="page-a");
        QVERIFY(!scene->m_objectBindings->ready());
    }
    void acceptedInsertionBindingSurvivesImmediateSceneDestruction(){
        auto snapshot=page();auto item=resistor();
        access.insertIds=add(snapshot,item);snapshot.fingerprint="inserted-before-close";
        scene->m_insertingItem=item;scene->m_objectContext=host.context;scene->m_objectPhase=NativeScene::ObjectPhase::Insert;
        const auto bindingsDirectory=directory.path()+"/close-test";
        scene->m_objectBindings=std::make_unique<NativeObjectBindings>(bindingsDirectory);
        access.backgroundPending={};access.complete(snapshot);
        QVERIFY(scene->m_bindingsPending);scene.reset();
        NativeObjectBindings reloaded(bindingsDirectory);
        QVERIFY(reloaded.observe(snapshot));QCOMPARE(reloaded.activeObjectCount(),1);
    }
    void nativeChangeDuringBackgroundModelsRequiresAnotherWorkerReceipt(){
        scene->refresh();const int inspections=access.inspections;access.backgroundPending={};
        access.complete(page());QVERIFY(scene->m_bindingsPending);
        scene->nativeContentChanged();
        QTRY_VERIFY(access.pending);
        QCOMPARE(access.inspections,inspections+1);QVERIFY(scene->m_objectCacheDirty);
        QVERIFY(scene->selectionOperationPending());QVERIFY(!scene->m_objectSelectionVerified);
        auto current=page();current.fingerprint="after-native-change";
        access.backgroundPending=[this]{return scene->m_bindingsPending;};access.complete(current);
        QVERIFY(!scene->m_objectCacheDirty);QCOMPARE(scene->m_objectSnapshot.fingerprint,current.fingerprint);
    }
    void insertionRereadDoesNotRegisterTwiceOrLoseInsertedIds(){
        auto snapshot=page();auto item=resistor();access.insertIds=add(snapshot,item);
        snapshot.fingerprint="inserted-with-concurrent-wakeup";
        scene->m_insertingItem=item;scene->m_objectContext=host.context;scene->m_objectPhase=NativeScene::ObjectPhase::Insert;
        access.backgroundPending={};access.complete(snapshot);scene->nativeContentChanged();
        QTRY_VERIFY(access.pending);QVERIFY(scene->m_bindingsRegistrationComplete);
        access.insertIds.clear();
        access.backgroundPending=[this]{return scene->m_bindingsPending;};access.complete(snapshot);
        QVERIFY(scene->m_evidence.value("nativeInsertedObjectBound").toBool());
        QCOMPARE(scene->m_objectBindings->activeObjectCount(),1);
        QVERIFY(!scene->m_bindingsRegistrationComplete);QVERIFY(!scene->m_objectCacheDirty);
    }
    void cleanPageRefreshAndToolChangesDoNotInspectAgain(){
        scene->refresh();access.complete(page());
        const int inspections=access.inspections;
        QSignalSpy changes(scene.data(),&NativeScene::changed);
        for(int i=0;i<100;++i)scene->refresh();
        QCOMPARE(access.inspections,inspections);QCOMPARE(changes.count(),0);
        for(int i=0;i<100;++i){
            QVERIFY(scene->action("tool",i%2?"line":"rectangle"));
            scene->setNativeToolActive(false);scene->setNativeToolActive(true);
        }
        QCOMPARE(access.inspections,inspections);QVERIFY(!access.pending);
        scene->refresh(true); // native tool activation can clear an empty selection
        QCOMPARE(access.inspections,inspections);
    }
    void nativeNavigationAndQueuedContentLeaveInactiveEditorQuiet(){
        scene->refresh();access.complete(page());scene->setNativeToolActive(false);
        const int inspections=access.inspections;
        QSignalSpy changes(scene.data(),&NativeScene::changed);
        QSignalSpy previews(scene.data(),&NativeScene::previewChanged);
        for(int i=0;i<100;++i){scene->nativeViewTransformChanged();scene->refresh();scene->nativeContentChanged();}
        QCOMPARE(changes.count(),0);QCOMPARE(previews.count(),0);QCOMPARE(access.inspections,inspections);
        QVERIFY(scene->m_objectCacheDirty);
        scene->setNativeToolActive(true);
        QCOMPARE(access.inspections,inspections+1);QVERIFY(access.pending);
    }
    void previewPacketIsSharedUntilGeometryOrProjectionChanges(){
        scene->m_gesture.tool="pen";
        QVERIFY(scene->pointerBegin(10,10,"pen"));QVERIFY(scene->pointerMove(40,60));
        const auto first=scene->previewFrame();QVERIFY(!first.isEmpty());
        QCOMPARE(scene->previewFrame(),first);
        QSignalSpy changes(scene.data(),&NativeScene::changed);
        QSignalSpy previews(scene.data(),&NativeScene::previewChanged);
        QVERIFY(scene->pointerMove(70,90));
        QCOMPARE(changes.count(),0);QCOMPARE(previews.count(),1);
        QVERIFY(scene->previewFrame()!=first);
        scene->pointerCancel();QVERIFY(scene->previewFrame().isEmpty());
        scene->m_pendingStrokes={{{{1,2},{3,4}},2,Qt::black}};
        scene->nativeViewTransformChanged();const auto committed=scene->previewFrame();
        QVERIFY(!committed.isEmpty());scene->nativeViewTransformChanged();
        QVERIFY(scene->previewFrame()!=committed);QCOMPARE(changes.count(),0);
    }
    void selectedVoltageOptionsReplaceGroupedInkAndUndoRestoresPreviousArrow(){
        auto current=page();auto item=resistor();current.selectedIds=add(current,item);
        QVERIFY(scene->m_objectBindings->registerInserted(item,current,current.selectedIds));beginSelection(current);
        QVERIFY(scene->state().value("selectionSupportsVoltage").toBool());
        NativeObjectSnapshot previous;
        const QStringList actions{"voltageArrow","voltageReversed","voltageOtherSide","voltageArrow"};
        for(int step=0;step<actions.size();++step){
            previous=current;
            QVERIFY(scene->action(actions[step],step!=3));
            QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::ReplaceRead);
            const auto expected=scene->m_replacingItem;
            QCOMPARE(expected.id,item.id);QCOMPARE(expected.anchors,item.anchors);
            QCOMPARE(expected.voltageArrow,step!=3);
            QCOMPARE(expected.voltageArrowReversed,step>=1);
            QCOMPARE(expected.voltageArrowOtherSide,step>=2);
            access.complete(current);QCOMPARE(access.request,QString("replace"));
            auto after=page();after.history=current.history;historyAppend(after,200+step);
            after.selectedIds=add(after,expected,5000+1000*step);after.fingerprint="voltage-"+QByteArray::number(step);
            access.insertIds=after.selectedIds;access.complete(after);current=after;
            QCOMPARE(access.replacements,step+1);QVERIFY(!scene->selectionOperationPending());
            QCOMPARE(scene->m_objectBindings->activeObjectCount(),1);
            QCOMPARE(scene->state().value("selectedVoltageArrow").toBool(),step!=3);
        }
        QVERIFY(scene->action("undo"));access.complete(current);QVERIFY(access.requestedIds.isEmpty());
        access.completeSelection();QCOMPARE(host.historyCalls,1);QCOMPARE(host.selectionCancels,0);
        auto restored=previous;restored.selectedIds.clear();restored.history.redo={current.history.undo.last()};
        access.complete(restored);QVERIFY(!scene->selectionOperationPending());
        repaper::drawing::Item restoredModel;QVERIFY(scene->m_objectBindings->selectedModel(previous.selectedIds,&restoredModel));
        QVERIFY(restoredModel.voltageArrow);QVERIFY(restoredModel.voltageArrowReversed);QVERIFY(restoredModel.voltageArrowOtherSide);
        QCOMPARE(restoredModel.anchors,item.anchors);
    }
    void stationaryHoldCreatesProportionalInkAndResetsAfterRelease_data(){
        QTest::addColumn<QString>("tool");QTest::addColumn<qreal>("ratio");
        QTest::newRow("square")<<QString("rectangle")<<qreal(1);
        QTest::newRow("circle")<<QString("ellipse")<<qreal(1);
        QTest::newRow("resistor")<<QString("symbol")<<qreal(1.5);
    }
    void stationaryHoldCreatesProportionalInkAndResetsAfterRelease(){
        QFETCH(QString,tool);QFETCH(qreal,ratio);
        scene->m_gesture.tool=tool;scene->m_gesture.symbolId="resistor-iec";
        QVERIFY(scene->pointerBegin(100,100,"pen"));
        QVERIFY(scene->state().value("aspectRatioHoldPending").toBool());
        QVERIFY(!scene->state().value("aspectRatioLocked").toBool());
        QVERIFY(scene->pointerMove(104,103)); // Normal pen jitter stays eligible.
        scene->checkAspectHold(scene->m_aspectClock.elapsed()+1000);
        QCOMPARE(host.flushes,1);QVERIFY(scene->state().value("aspectRatioLocked").toBool());
        QCOMPARE(scene->state().value("aspectRatioLockAnchor").toPointF(),QPointF(100,100));
        QVERIFY(scene->pointerMove(280,160));QVERIFY(scene->pointerEnd(310,170));
        QCOMPARE(access.request,QString("insert"));
        const auto item=scene->m_insertingItem;
        QVERIFY(qAbs(repaper::drawing::boxWidth(item)/repaper::drawing::boxHeight(item)-ratio)<.001);
        QVERIFY(!scene->state().value("aspectRatioLocked").toBool());QVERIFY(!scene->m_aspectTimer.isActive());
        auto confirmed=page();access.insertIds=add(confirmed,item);access.complete(confirmed);
        QVERIFY(!scene->creationOperationPending());QCOMPARE(scene->m_objectBindings->activeObjectCount(),1);
        QVERIFY(scene->pointerBegin(500,100,"pen"));QVERIFY(scene->pointerMove(710,170));QVERIFY(scene->pointerEnd(710,170));
        QVERIFY(qAbs(repaper::drawing::boxWidth(scene->m_insertingItem)/repaper::drawing::boxHeight(scene->m_insertingItem)-3)<.001);
    }
    void earlyMovementPermanentlyCancelsHoldIncludingQueuedReturn_data(){
        QTest::addColumn<bool>("queued");
        QTest::newRow("already-delivered")<<false;QTest::newRow("pending-at-deadline")<<true;
    }
    void earlyMovementPermanentlyCancelsHoldIncludingQueuedReturn(){
        QFETCH(bool,queued);scene->m_gesture.tool="rectangle";
        QVERIFY(scene->pointerBegin(100,100,"pen"));
        const auto move=[&]{QVERIFY(scene->pointerMoveBatch({QPointF(109,100),QPointF(100,100)}));};
        if(queued)host.flushMoves=move;else move();
        scene->checkAspectHold(scene->m_aspectClock.elapsed()+1000);
        QVERIFY(!scene->state().value("aspectRatioLocked").toBool());
        QVERIFY(!scene->state().value("aspectRatioHoldPending").toBool());QVERIFY(!scene->m_aspectTimer.isActive());
        QCOMPARE(host.flushes,queued?1:0);
        scene->checkAspectHold(scene->m_aspectClock.elapsed()+3000); // Returning and waiting cannot re-arm.
        QVERIFY(scene->pointerEnd(310,170));
        QVERIFY(qAbs(repaper::drawing::boxWidth(scene->m_insertingItem)/repaper::drawing::boxHeight(scene->m_insertingItem)-3)<.001);
    }
    void contextChangesCancelWaitingAndLockedHolds_data(){
        QTest::addColumn<QString>("event");QTest::addColumn<bool>("locked");
        for(const auto &event:{QString("tool"),QString("page"),QString("touch"),QString("capture")})
            for(bool locked:{false,true})QTest::newRow(qPrintable(event+(locked?"-locked":"-waiting")))<<event<<locked;
    }
    void contextChangesCancelWaitingAndLockedHolds(){
        QFETCH(QString,event);QFETCH(bool,locked);scene->m_gesture.tool="ellipse";
        QVERIFY(scene->pointerBegin(100,100,"pen"));
        if(locked){scene->checkAspectHold(scene->m_aspectClock.elapsed()+1000);QVERIFY(scene->aspectRatioLocked());}
        if(event=="tool")QVERIFY(scene->action("tool",QString("rectangle")));
        else if(event=="page"){host.context.insert("pageId","page-b");scene->refresh();}
        else if(event=="touch")scene->m_touchGuard->cancelRequested();
        else scene->setNativeToolActive(false);
        QVERIFY(!scene->state().value("aspectRatioLocked").toBool());
        QVERIFY(!scene->state().value("aspectRatioHoldPending").toBool());QVERIFY(!scene->m_aspectTimer.isActive());
        QVERIFY(!scene->m_gesture.active());scene->checkAspectHold(scene->m_aspectClock.elapsed()+3000);
        QVERIFY(!scene->aspectRatioLocked());QCOMPARE(host.transforms,0);
        QVERIFY(static_cast<WorkflowScene*>(scene.data())->preparedStrokes.isEmpty());
        QVERIFY(!access.pending||access.request=="inspect"); // Context refresh may read the new page.
    }
    void unsupportedGesturesNeverArmAspectHold(){
        for(const auto &tool:{QString("pen"),QString("line"),QString("arrow"),QString("wire"),QString("select")}){
            scene->m_gesture.tool=tool;QVERIFY(scene->pointerBegin(100,100,"pen"));
            QVERIFY(!scene->state().value("aspectRatioHoldPending").toBool());
            scene->checkAspectHold(scene->m_aspectClock.elapsed()+1000);QVERIFY(!scene->aspectRatioLocked());scene->pointerCancel();
        }
        scene->m_gesture.tool="rectangle";QVERIFY(!scene->pointerBegin(100,100,"touch"));
        QVERIFY(!scene->state().value("aspectRatioHoldPending").toBool());QCOMPARE(host.flushes,0);
    }
    void heldRotatedHandlePersistsCurrentRatioThroughReplacement(){
        auto s=page();auto item=resistor();QVERIFY(NativeObjectGesture::resizeBox(item,320,100));
        QTransform rotation;rotation.rotate(32);QVERIFY(repaper::drawing::transform(item,rotation));
        s.selectedIds=add(s,item);QVERIFY(scene->m_objectBindings->registerInserted(item,s,s.selectedIds));beginSelection(s);
        const auto controls=NativeObjectGesture::controls(scene->m_objectModel);
        const auto press=controls[4].point;const auto release=press+QPointF(120,70);
        QVERIFY(scene->pointerBegin(press.x(),press.y(),"pen"));QVERIFY(scene->m_objectGesture.kind()=="resize");
        scene->checkAspectHold(scene->m_aspectClock.elapsed()+1000);QVERIFY(scene->aspectRatioLocked());
        QVERIFY(scene->pointerMove(release.x(),release.y()));QVERIFY(scene->pointerEnd(release.x(),release.y()));
        QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::ReplaceRead);QVERIFY(!scene->aspectRatioLocked());
        const auto expected=scene->m_replacingItem;
        QVERIFY(qAbs(repaper::drawing::boxWidth(expected)/repaper::drawing::boxHeight(expected)-3.2)<.001);
        QCOMPARE(expected.width,item.width);access.complete(s);QCOMPARE(access.request,QString("replace"));
        auto after=page();after.selectedIds=add(after,expected,5000);after.fingerprint="held-resize";
        access.insertIds=after.selectedIds;access.complete(after);QVERIFY(!scene->selectionOperationPending());
        QVERIFY(qAbs(repaper::drawing::boxWidth(scene->m_objectModel)/repaper::drawing::boxHeight(scene->m_objectModel)-3.2)<.001);
        QCOMPARE(scene->m_objectModel.width,item.width);
    }
    void heldOrdinaryInkHandleDispatchesUniformNativeScale(){
        auto s=page();s.selectedIds=add(s,resistor());beginSelection(s);QVERIFY(!scene->m_hasObjectModel);
        const auto press=scene->m_selectedRect.bottomRight();
        QVERIFY(scene->pointerBegin(press.x(),press.y(),"pen"));QVERIFY(scene->m_selectionGesture.active());
        scene->checkAspectHold(scene->m_aspectClock.elapsed()+1000);QVERIFY(scene->aspectRatioLocked());
        QVERIFY(scene->pointerEnd(press.x()+120,press.y()+30));QVERIFY(!scene->aspectRatioLocked());
        QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::AffineRead);
        access.complete(s);QCOMPARE(host.transforms,1);QCOMPARE(host.lastTransform.value("kind").toString(),QString("scale"));
        QVERIFY(qAbs(host.lastTransform.value("sx").toDouble()-host.lastTransform.value("sy").toDouble())<.001);
        QVERIFY(host.lastTransform.value("sx").toDouble()>1);
    }
    void touchingOneResistorStrokeSelectsAllExactMembers(){
        auto s=page();const auto item=resistor();const auto ids=add(s,item);
        QVERIFY(ids.size()>1);QVERIFY(scene->m_objectBindings->registerInserted(item,s,ids));
        s.selectedIds={ids.first()};
        QVERIFY(scene->pointerBegin(100,100,"pen"));QVERIFY(scene->pointerEnd(100,100));
        QCOMPARE(host.regions,1);QVERIFY(access.pending);QVERIFY(scene->selectionOperationPending());
        access.complete(s);QCOMPARE(access.request,QString("select"));QCOMPARE(access.requestedIds,ids);
        QVERIFY(scene->selectionOperationPending());access.completeSelection();
        QCOMPARE(scene->m_selectionCount,ids.size());QVERIFY(!scene->selectionOperationPending());
        QVERIFY(scene->state().value("nativeSelectionInkOverlay").toBool());
        QCOMPARE(scene->previewStrokes().size(),ids.size()+1);
    }
    void twoNativeInkRotationsRepairStaleNativeSelectionBeforeUnlocking(){
        auto s=page();auto item=resistor();item.kind="legacy";item.sourcePoints.clear();const auto ids=add(s,item);s.selectedIds=ids;
        QVERIFY(scene->m_objectBindings->registerInserted(item,s,ids));beginSelection(s);
        for(int turn=0;turn<2;++turn){
            const auto anchor=scene->m_selectedRect.center();QTransform rotation;rotation.translate(anchor.x(),anchor.y());rotation.rotate(90);rotation.translate(-anchor.x(),-anchor.y());
            QVERIFY(scene->action("rotate"));QCOMPARE(host.transforms,turn);QVERIFY(access.pending);
            access.complete(s);QCOMPARE(host.transforms,turn+1);QVERIFY(access.pending);
            s=moved(s,rotation); // Same persisted lineage, newly allocated IDs; native records still name old IDs.
            access.complete(s);QCOMPARE(access.request,QString("select"));QCOMPARE(access.requestedIds.size(),ids.size());
            for(const auto &line:s.lines)QVERIFY(access.requestedIds.contains(line.id));
            QVERIFY(scene->selectionOperationPending());QVERIFY(!scene->pointerBegin(50,50,"pen"));
            access.completeSelection();s=access.snapshot;
            QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->m_objectSelectionVerified);
            QCOMPARE(scene->m_selectionCount,ids.size());QVERIFY(scene->m_objectBindings->expandSelection({s.lines.last().id}).ids.size()==ids.size());
        }
    }
    void mixedNativeSelectionIsNormalizedBeforeHidingSelectedTiles(){
        auto s=page();s.selectedIds=add(s,resistor());s.nativeSelectionExact=false;
        QVERIFY(scene->inspectObjects(NativeScene::ObjectPhase::Refresh));access.complete(s);
        QCOMPARE(access.request,QString("select"));QVERIFY(!scene->selectionInkOverlay());
        QCOMPARE(access.requestedIds,s.selectedIds);access.completeSelection();
        QVERIFY(scene->selectionInkOverlay());QVERIFY(scene->m_objectSnapshot.nativeSelectionExact);
    }
    void selectingAnotherObjectRetainsTheFirstPress(){
        auto s=page();s.selectedIds=add(s,resistor());beginSelection(s);
        QVERIFY(scene->pointerBegin(800,800,"pen"));QCOMPARE(host.clears,1);QVERIFY(scene->m_regionSelecting);
        QVERIFY(scene->pointerEnd(800,800));QCOMPARE(host.regions,1);QVERIFY(access.pending);
        s.selectedIds.clear();access.complete(s);QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->m_selectedStrokes.isEmpty());
    }
    void readFailureAfterToolChangeSchedulesAndCompletesCleanup(){
        auto s=page();s.selectedIds=add(s,resistor());beginSelection(s);
        QVERIFY(scene->inspectObjects(NativeScene::ObjectPhase::Refresh));
        QVERIFY(scene->action("tool","wire"));access.complete(s,false);
        QVERIFY(scene->selectionOperationPending());
        QTRY_COMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::CleanupRead);
        access.complete(s);access.completeSelection();
        QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->m_objectFaulted);
    }
    void duplicateKeepsOriginalAndCopiedMembershipSeparate(){
        auto s=page();const auto item=resistor();const auto ids=add(s,item);s.selectedIds=ids;
        QVERIFY(scene->m_objectBindings->registerInserted(item,s,ids));beginSelection(s);
        QVERIFY(scene->action("duplicate"));access.complete(s);
        auto after=s;after.selectedIds.clear();
        for(auto line:s.lines){line.id+=1000;line.lineageId=line.id;line.version+="-copy";
            for(auto &point:line.stroke.points)point+=QPointF(24,24);
            after.selectedIds.append(line.id);after.lines.append(line);}
        after.fingerprint="after-copy";access.complete(after);QCOMPARE(access.request,QString("select"));access.completeSelection();
        QVERIFY(!scene->selectionOperationPending());QCOMPARE(scene->m_objectBindings->activeObjectCount(),2);
        const auto oldSelection=scene->m_objectBindings->expandSelection({ids.first()});QCOMPARE(oldSelection.ids.size(),ids.size());
        for(auto id:oldSelection.ids)QVERIFY(ids.contains(id));
        const auto newSelection=scene->m_objectBindings->expandSelection({after.selectedIds.first()});QCOMPARE(newSelection.ids.size(),ids.size());
        for(auto id:newSelection.ids)QVERIFY(after.selectedIds.contains(id));
    }
    void chosenColorReachesNativeCreationAndConfirmedSelection(){
        scene->m_gesture.tool="line";
        QVERIFY(scene->action("color",QString("#d90707")));
        QCOMPARE(scene->state().value("lineColor").toString(),QString("#d90707"));
        QVERIFY(scene->pointerBegin(100,100,"pen"));QVERIFY(scene->pointerMove(300,250));
        QCOMPARE(scene->previewStrokes().first().toMap().value("color").toString(),QString("#ffd90707"));
        QVERIFY(scene->pointerEnd(320,260));QCOMPARE(access.request,QString("insert"));
        const auto &prepared=static_cast<WorkflowScene*>(scene.data())->preparedStrokes;
        QVERIFY(!prepared.isEmpty());for(const auto &stroke:prepared)QCOMPARE(stroke.color,QColor("#d90707"));
        auto confirmed=page();access.insertIds=add(confirmed,scene->m_insertingItem);access.complete(confirmed);
        QVERIFY(!scene->creationOperationPending());QVERIFY(scene->m_pendingStrokes.isEmpty());
        QCOMPARE(scene->m_objectBindings->activeObjectCount(),1);
        confirmed.selectedIds=access.insertIds;scene->m_gesture.tool="select";beginSelection(confirmed);
        QCOMPARE(scene->state().value("selectedLineColor").toString(),QString("#d90707"));
    }
    void colorPickerWaitsForNativePendingTransform(){
        auto selected=page();selected.selectedIds=add(selected,resistor());beginSelection(selected);
        QVERIFY(scene->state().value("selectionCanChangeColor").toBool());
        QTransform pending;pending.rotate(30);controller.setProperty("pendingEdit",pending);
        QVERIFY(!scene->state().value("selectionCanChangeColor").toBool());
        QVERIFY(!scene->action("color",QString("#0062cc")));QVERIFY(scene->status().contains("transformation"));
        QCOMPARE(access.inspections,0);QVERIFY(!access.pending);
        controller.setProperty("pendingEdit",QTransform());
        QVERIFY(scene->state().value("selectionCanChangeColor").toBool());
    }
    void rejectedInsertionRemainsVisibleInStatusAfterCleanup(){
        scene->m_gesture.tool="line";
        scene->m_objectPhase=NativeScene::ObjectPhase::Insert;scene->m_objectContext=scene->m_insertionContext=host.context;
        scene->m_pendingStrokes=resistor().strokes;
        scene->failObjectOperation("native-insertion-history-unavailable");
        QVERIFY(scene->m_pendingStrokes.isEmpty());QVERIFY(scene->status().contains("n’a pas été ajouté"));
        scene->refresh();QCOMPARE(access.request,QString("inspect"));access.complete(page());access.completeSelection();
        QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->m_objectFaulted);
        QVERIFY(scene->status().contains("n’a pas été ajouté"));
        QVERIFY(scene->pointerBegin(100,100,"pen"));QVERIFY(!scene->status().contains("n’a pas été ajouté"));
    }
    void attributedInsertionRegistersMembershipBeforePreviewRetires(){
        auto s=page();const auto item=resistor();const auto ids=add(s,item);
        scene->m_objectPhase=NativeScene::ObjectPhase::Insert;scene->m_objectContext=scene->m_insertionContext=host.context;
        scene->m_gesture.tool="symbol";scene->m_pendingStrokes=item.strokes;scene->m_insertingItem=item;access.insertIds=ids;
        access.complete(s);QVERIFY(!scene->creationOperationPending());QVERIFY(scene->m_pendingStrokes.isEmpty());
        QVERIFY(scene->m_selectedStrokes.isEmpty());QCOMPARE(scene->m_selectionCount,0);QVERIFY(!scene->selectionInkOverlay());
        QCOMPARE(scene->m_objectBindings->activeObjectCount(),1);
        QCOMPARE(scene->m_objectBindings->expandSelection({ids.last()}).ids.size(),ids.size());
        QVERIFY(scene->evidence().value("nativeInsertionIdsObserved").toBool());
    }
    void changedBaselineCannotTransformDifferentInk(){
        auto s=page();s.selectedIds=add(s,resistor());beginSelection(s);
        QVERIFY(scene->action("rotate"));s.lines.first().stroke.points.first()+=QPointF(50,0);s.fingerprint="external-change";
        access.complete(s);QCOMPARE(host.transforms,0);QVERIFY(!scene->m_objectSelectionVerified);QVERIFY(scene->selectionOperationPending());
        QVERIFY(!scene->state().value("hasSelection").toBool());QVERIFY(!scene->state().value("selectionCanChangeColor").toBool());
        scene->refresh();access.complete(s);access.completeSelection();QVERIFY(!scene->selectionOperationPending());
    }
    void toolChangeKeepsLockThroughReselectionAndCleanup(){
        auto s=page();s.selectedIds=add(s,resistor());beginSelection(s);
        QVERIFY(scene->transformSelection({{"kind","move"},{"delta",QPointF(30,20)}}));access.complete(s);
        QVERIFY(scene->action("tool","wire"));QVERIFY(scene->selectionOperationPending());
        QTransform translation;translation.translate(30,20);s=moved(s,translation);access.complete(s);access.completeSelection();
        QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::CleanupRead);QVERIFY(scene->selectionOperationPending());
        access.complete(access.snapshot);QCOMPARE(access.request,QString("select"));QVERIFY(access.requestedIds.isEmpty());
        access.completeSelection();QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->m_selectedStrokes.isEmpty());
    }
    void staleCompletionAfterPageChangeCannotBindAnotherPage(){
        auto s=page();s.selectedIds=add(s,resistor());beginSelection(s);
        QVERIFY(scene->action("rotate"));host.context.insert("pageId","page-b");scene->refresh();
        access.complete(s);QVERIFY(scene->m_selectedStrokes.isEmpty());QCOMPARE(host.transforms,0);
        QVERIFY(scene->m_objectSnapshot.documentId.isEmpty());QVERIFY(!scene->m_objectSelectionVerified);
    }
    void failedAffineReceiptKeepsLockUntilExactCleanup(){
        auto s=page();s.selectedIds=add(s,resistor());beginSelection(s);
        QVERIFY(scene->transformSelection({{"kind","move"},{"delta",QPointF(30,20)}}));access.complete(s);
        QTransform wrong;wrong.translate(300,20);access.complete(moved(s,wrong));
        QVERIFY(scene->m_objectFaulted);QVERIFY(scene->selectionOperationPending());QVERIFY(!scene->action("undo"));
        scene->refresh();QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::CleanupRead);
        auto actual=access.snapshot;access.complete(actual);QVERIFY(access.requestedIds.isEmpty());access.completeSelection();
        QVERIFY(!scene->m_objectFaulted);QVERIFY(!scene->selectionOperationPending());
    }
    void wireGestureUsesCachedPortAtPressAndRelease(){
        auto s=page();const auto item=resistor();const auto ids=add(s,item);QVERIFY(scene->m_objectBindings->registerInserted(item,s,ids));
        scene->m_objectCacheDirty=false;
        scene->m_gesture.tool="wire";const auto port=item.anchors.first();
        QVERIFY(scene->pointerBegin(port.x()+3,port.y()+2,"pen"));QVERIFY(scene->m_wireSnap.snapped);
        QVERIFY(scene->pointerMove(port.x()+100,port.y()+100));
        repaper::drawing::Item result;QVERIFY(scene->m_gesture.finish(scene->snappedWirePoint(item.anchors.last()+QPointF(2,3)),&result));
        QCOMPARE(result.sourcePoints.first(),port);QCOMPARE(result.sourcePoints.last(),item.anchors.last());QCOMPARE(access.inspections,0);
    }
    void wireStartsAfterSeveralSmallMovesInsideItsSnappedTerminal_data(){
        QTest::addColumn<QString>("targetKind");
        QTest::newRow("wire-end")<<QString("wire-end");
        QTest::newRow("wire-junction")<<QString("wire-junction");
        QTest::newRow("component-port")<<QString("component-port");
    }
    void wireStartsAfterSeveralSmallMovesInsideItsSnappedTerminal(){
        QFETCH(QString,targetKind);
        using namespace repaper::drawing;
        Item target;
        if(targetKind=="component-port")target=resistor();
        else {target.kind="wire";target.sourcePoints={{200,400},{500,400}};QVERIFY(routeWire(target,{}));}
        const bool junction=targetKind=="wire-junction";
        const auto start=junction?QPointF(350,400):target.anchors.last();
        const auto expectedPort=junction?QString("route"):target.portIds.last();
        auto before=page();const auto targetIds=add(before,target);
        QVERIFY(scene->m_objectBindings->registerInserted(target,before,targetIds));
        scene->m_objectCacheDirty=false;
        scene->m_gesture.tool="wire";scene->m_objectSnapshot=before;
        QVERIFY(scene->pointerBegin(start.x(),start.y()+2,"pen"));QVERIFY(scene->m_wireSnap.snapped);
        QVERIFY(scene->pointerMove(start.x(),start.y()+3));QVERIFY(scene->m_gesture.active());
        QVERIFY(scene->pointerMoveBatch({start+QPointF(0,4),start+QPointF(0,5)}));
        QVERIFY(scene->state().value("wireSnapActive").toBool());
        QCOMPARE(scene->state().value("wireSnapPoint").toPointF(),start);
        QVERIFY(scene->pointerMove(700,600));QVERIFY(!scene->previewStrokes().isEmpty());
        QVERIFY(scene->pointerEnd(710,610));QCOMPARE(access.request,QString("insert"));
        const auto item=scene->m_insertingItem;
        QCOMPARE(item.sourcePoints.first(),start);QCOMPARE(item.sourcePoints.last(),QPointF(710,610));
        QCOMPARE(item.startAttachment.objectId,target.id);QCOMPARE(item.startAttachment.portId,expectedPort);
        if(junction)QVERIFY(qAbs(item.startAttachment.wirePosition-.5)<1e-9);
        QVERIFY(item.endAttachment.empty());
    }
    void wirePreviewSurvivesCrossingAnObstacleClearanceBand(){
        using namespace repaper::drawing;
        const auto obstacle=resistor();auto before=page();const auto obstacleIds=add(before,obstacle);
        QVERIFY(scene->m_objectBindings->registerInserted(obstacle,before,obstacleIds));
        scene->m_gesture.tool="wire";scene->m_objectSnapshot=before;
        QVERIFY(scene->pointerBegin(100,100,"pen"));QVERIFY(scene->pointerMove(500,100));
        const auto previous=scene->previewStrokes();QVERIFY(!previous.isEmpty());
        QVERIFY(scene->pointerMove(320,194));QVERIFY(scene->m_gesture.active());
        QCOMPARE(scene->previewStrokes(),previous);
        QVERIFY(scene->pointerMove(500,460));QVERIFY(scene->pointerEnd(500,460));
        QCOMPARE(access.request,QString("insert"));
        QCOMPARE(scene->m_insertingItem.sourcePoints,PaperDrawing::Polyline({{100,100},{500,460}}));
        QVERIFY(scene->m_insertingItem.startAttachment.empty());QVERIFY(scene->m_insertingItem.endAttachment.empty());
    }
    void wireReleasedWithoutAValidRouteDoesNotCommitThePreviousPreview(){
        using namespace repaper::drawing;
        const auto obstacle=resistor();auto before=page();const auto obstacleIds=add(before,obstacle);
        QVERIFY(scene->m_objectBindings->registerInserted(obstacle,before,obstacleIds));
        scene->m_gesture.tool="wire";scene->m_objectSnapshot=before;
        QVERIFY(scene->pointerBegin(100,100,"pen"));QVERIFY(scene->pointerMove(500,100));
        QVERIFY(!scene->previewStrokes().isEmpty());QVERIFY(scene->pointerMove(320,194));
        QVERIFY(!scene->pointerEnd(320,194));
        QVERIFY(!scene->m_gesture.active());QVERIFY(scene->previewStrokes().isEmpty());
        QVERIFY(!scene->creationOperationPending());QVERIFY(!access.pending);
        QVERIFY(static_cast<WorkflowScene*>(scene.data())->preparedStrokes.isEmpty());
        QCOMPARE(scene->m_objectBindings->activeObjectCount(),1);
        QVERIFY(scene->pointerBegin(100,100,"pen"));QVERIFY(scene->pointerMove(100,100));
        QVERIFY(!scene->pointerEnd(100,100));QVERIFY(scene->previewStrokes().isEmpty());
        QVERIFY(!access.pending);
    }
    void stencilFirstPressShowsSnapAndKeepsTheFreeWireConnectionAfterInsertion(){
        using namespace repaper::drawing;
        Item wire;wire.kind="wire";wire.sourcePoints={{200,400},{500,400}};QVERIFY(routeWire(wire,{}));
        auto baseline=page();const auto ids=add(baseline,wire);
        QVERIFY(scene->m_objectBindings->registerInserted(wire,baseline,ids));
        scene->m_gesture.tool="symbol";scene->m_gesture.symbolId="resistor-iec";
        QVERIFY(scene->pointerBegin(503,402,"pen"));
        QVERIFY(scene->state().value("wireSnapActive").toBool());
        QCOMPARE(scene->state().value("wireSnapPoint").toPointF(),QPointF(500,400));
        const auto preview=scene->m_gesture.preview();QVERIFY(!preview.isEmpty());
        QVERIFY(scene->pointerEnd(503,402));QCOMPARE(access.request,QString("insert"));
        const auto created=scene->m_insertingItem;QVERIFY(scene->m_insertingPlacement.snapped);
        QCOMPARE(created.anchors.first(),QPointF(500,400));
        QCOMPARE(created.strokes.first().points,preview.first().points);
        auto confirmed=baseline;access.insertIds=add(confirmed,created,1000);historyAppend(confirmed,9000);
        confirmed.fingerprint="stencil-snapped-insert";access.complete(confirmed);
        QVERIFY(!scene->creationOperationPending());QVERIFY(!scene->state().value("wireSnapActive").toBool());
        QVERIFY(!scene->m_insertingPlacement.snapped);
        auto models=scene->m_objectBindings->documentModels();QCOMPARE(models.size(),2);
        auto found=std::find_if(models.begin(),models.end(),[&](const auto &entry){return entry.id==wire.id;});
        QVERIFY(found!=models.end());QCOMPARE(found->endAttachment.objectId,created.id);
        QCOMPARE(found->endAttachment.portId,created.portIds.first());
        for(auto &entry:models)if(entry.id==created.id){QTransform shift;shift.translate(60,80);QVERIFY(transform(entry,shift));}
        QVERIFY(reroute(models));
        found=std::find_if(models.begin(),models.end(),[&](const auto &entry){return entry.id==wire.id;});
        QCOMPARE(found->sourcePoints.last(),QPointF(560,480));
    }
    void fingersAndUnidentifiedMouseEventsNeverBeginADrawing(){
        scene->m_gesture.tool="line";
        QVERIFY(!scene->pointerBegin(100,100,"touch"));QVERIFY(!scene->pointerBegin(100,100,"mouse"));
        QVERIFY(!scene->m_gesture.active());QVERIFY(!scene->pointerEnd(300,300));QCOMPARE(access.inspections,0);
        QVERIFY(scene->pointerBegin(100,100,"pen"));QVERIFY(scene->m_gesture.active());
    }
    void idleNativeNavigationDoesNotRebuildEditorState(){
        scene->m_gesture.tool="line";QSignalSpy changes(scene.data(),&NativeScene::changed);
        for(int i=0;i<200;++i)scene->pointerCancel();
        QCOMPARE(changes.count(),0);QCOMPARE(access.inspections,0);QCOMPARE(access.selections,0);
    }
    void touchingAnEndpointWithoutMovingCreatesNoNativeEdit(){
        auto s=page();repaper::drawing::Item item;item.kind="arrow";item.sourcePoints={{100,100},{400,300}};
        QVERIFY(rebuildNativeObject(item));s.selectedIds=add(s,item);QVERIFY(scene->m_objectBindings->registerInserted(item,s,s.selectedIds));beginSelection(s);
        QVERIFY(scene->pointerBegin(400,300,"pen"));QVERIFY(scene->pointerEnd(400,300));
        QVERIFY(!access.pending);QCOMPARE(access.inspections,0);QCOMPARE(host.transforms,0);
    }
    void endpointEditWaitsForExactReplacementAndPreservesTheOtherInk(){
        auto s=page();repaper::drawing::Item item;item.kind="arrow";item.sourcePoints={{100,100},{400,300}};
        item.width=2;QVERIFY(RePaperNative::rebuildNativeObject(item));const auto ids=add(s,item);s.selectedIds=ids;
        const auto other=add(s,resistor(),500);QVERIFY(scene->m_objectBindings->registerInserted(item,s,ids));beginSelection(s);
        QVERIFY(scene->m_hasObjectModel);QVERIFY(scene->state().value("selectionHasEndpoints").toBool());
        QCOMPARE(scene->state().value("selectionHandlePoints").toList().size(),2);
        QVERIFY(scene->pointerBegin(400,300,"pen"));QVERIFY(scene->pointerMove(460,390));QVERIFY(!access.pending);
        QCOMPARE(scene->m_objectGesture.preview().width,qreal(2));
        QVERIFY(scene->pointerEnd(480,410));QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::ReplaceRead);
        QVERIFY(scene->selectionOperationPending());QVERIFY(!scene->pointerBegin(100,100,"pen"));
        access.complete(s);QCOMPARE(access.request,QString("replace"));QCOMPARE(access.requestedIds,ids);
        auto after=s;
        after.lines.erase(std::remove_if(after.lines.begin(),after.lines.end(),[&](const auto &line){return ids.contains(line.id);}),after.lines.end());
        const auto model=scene->m_replacingItem;after.selectedIds=add(after,model,5000);after.fingerprint="after-endpoint";
        access.insertIds=after.selectedIds;access.complete(after);
        QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->m_hasObjectModel);QVERIFY(scene->selectionInkOverlay());
        QCOMPARE(scene->m_objectModel.sourcePoints[0],QPointF(100,100));QCOMPARE(scene->m_objectModel.sourcePoints[1],QPointF(480,410));
        QCOMPARE(scene->m_objectModel.width,qreal(2));QCOMPARE(host.transforms,0);
        for(auto id:other)QVERIFY(!scene->m_objectSnapshot.selectedIds.contains(id));
    }
    void editedEndpointsAndWireBendsAllowTheNextDrawing_data(){
        QTest::addColumn<QString>("kind");
        QTest::newRow("arrow")<<QString("arrow");
        QTest::newRow("orthogonal-wire")<<QString("wire");
    }
    void editedEndpointsAndWireBendsAllowTheNextDrawing(){
        QFETCH(QString,kind);
        auto current=page();repaper::drawing::Item item;item.kind=kind;item.sourcePoints={{100,100},{400,300}};
        QVERIFY(rebuildNativeObject(item));current.selectedIds=add(current,item);
        QVERIFY(scene->m_objectBindings->registerInserted(item,current,current.selectedIds));beginSelection(current);
        for(int edit=0;edit<2;++edit){
            const auto controls=NativeObjectGesture::controls(scene->m_objectModel);
            const auto press=kind=="wire"&&edit==1?controls.last().point:scene->m_objectModel.sourcePoints.last();
            const auto release=press+QPointF(80,40);
            QVERIFY(scene->pointerBegin(press.x(),press.y(),"pen"));QVERIFY(scene->pointerMove(release.x(),release.y()));
            QVERIFY(scene->pointerEnd(release.x(),release.y()));
            const auto expected=scene->m_replacingItem;
            access.complete(current);QCOMPARE(access.request,QString("replace"));
            auto after=page();after.selectedIds=add(after,expected,5000+1000*edit);after.fingerprint="edited-"+QByteArray::number(edit);
            access.insertIds=after.selectedIds;access.complete(after);current=after;
            QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->m_hasObjectModel);
            QCOMPARE(scene->m_objectModel.sourcePoints,expected.sourcePoints);
            QVERIFY(qAbs(scene->m_objectModel.wireBend-expected.wireBend)<.001);
            QCOMPARE(scene->m_objectModel.width,item.width);
        }
        // The completed edit must also allow leaving Selection and immediately
        // committing another shape, while preserving the modified object's ink.
        QVERIFY(scene->action("tool",QString("rectangle")));QCOMPARE(access.request,QString("inspect"));
        access.complete(current);access.completeSelection();current=access.snapshot;
        QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->pointerBegin(700,500,"pen"));
        QVERIFY(scene->pointerMove(900,700));QVERIFY(scene->pointerEnd(900,700));QCOMPARE(access.request,QString("insert"));
        const auto editedLines=current.lines;
        access.insertIds=add(current,scene->m_insertingItem,20000);current.fingerprint="next-drawing";
        access.complete(current);QVERIFY(!scene->creationOperationPending());QVERIFY(scene->m_pendingStrokes.isEmpty());
        QCOMPARE(scene->m_objectBindings->activeObjectCount(),2);
        for(const auto &line:editedLines){
            const auto found=std::find_if(scene->m_objectSnapshot.lines.cbegin(),scene->m_objectSnapshot.lines.cend(),[&](const auto &entry){return entry.id==line.id;});
            QVERIFY(found!=scene->m_objectSnapshot.lines.cend());QCOMPARE(found->version,line.version);QCOMPARE(found->stroke.points,line.stroke.points);
        }
    }
    void rotatedSymbolNumericResizeUsesItsLocalDimensions(){
        auto s=page();auto item=resistor();QTransform rotation;rotation.rotate(32);QVERIFY(repaper::drawing::transform(item,rotation));
        s.selectedIds=add(s,item);QVERIFY(scene->m_objectBindings->registerInserted(item,s,s.selectedIds));beginSelection(s);
        QVERIFY(scene->m_hasObjectModel);
        QVERIFY(qAbs(scene->state().value("selectedShapeWidth").toDouble()-240)<.01);
        QVERIFY(qAbs(scene->state().value("selectedShapeHeight").toDouble()-160)<.01);
        QVERIFY(scene->resizeSelection(360,220));QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::ReplaceRead);
        QVERIFY(qAbs(repaper::drawing::boxWidth(scene->m_replacingItem)-360)<.01);
        QVERIFY(qAbs(repaper::drawing::boxHeight(scene->m_replacingItem)-220)<.01);
        QCOMPARE(scene->m_replacingItem.width,item.width);QCOMPARE(host.transforms,0);
    }
    void changedSemanticBaselineRejectsReplacementAndRepairsSelection(){
        auto s=page();auto item=resistor();s.selectedIds=add(s,item);
        QVERIFY(scene->m_objectBindings->registerInserted(item,s,s.selectedIds));beginSelection(s);
        QVERIFY(scene->resizeSelection(360,200));
        s.lines.first().version="other-edit";s.fingerprint="other-edit";access.complete(s);
        QVERIFY(scene->m_objectFaulted);QVERIFY(scene->selectionOperationPending());QVERIFY(access.request!="replace");
        scene->refresh();access.complete(s);access.completeSelection();QVERIFY(!scene->selectionOperationPending());
    }
    void propertyUndoAndRedoWaitForNativeDeselectionAndOneHistoryCommand(){
        auto before=page();auto item=resistor();before.selectedIds=add(before,item);
        QVERIFY(scene->m_objectBindings->registerInserted(item,before,before.selectedIds));beginSelection(before);
        QVERIFY(scene->action("width",qreal(5)));const auto edited=scene->m_replacingItem;
        access.complete(before);QCOMPARE(access.request,QString("replace"));
        auto after=page();after.selectedIds=add(after,edited,5000);after.fingerprint="property-edited";
        access.insertIds=after.selectedIds;access.complete(after);QCOMPARE(scene->m_objectModel.width,qreal(5));
        for(int step=0;step<2;++step){
            const auto action=step==0?QString("undo"):QString("redo");
            const auto current=step==0?after:before;
            auto restored=step==0?before:after;
            if(step)beginSelection(current);
            host.nativeSelectionPresent=true;controller.setProperty("selectionItemCount",current.selectedIds.size());
            QVERIFY(scene->action(action));
            QCOMPARE(host.historyCalls,step);QCOMPARE(host.selectionCancels,0);
            QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::CleanupRead);
            QVERIFY(!scene->state().value("hasSelection").toBool());QVERIFY(!scene->selectionInkOverlay());
            QVERIFY(!scene->action(action));QVERIFY(!scene->pointerBegin(800,800,"pen"));
            scene->refresh();scene->nativeAreaSelected(0,{});scene->nativeContentChanged();
            QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::CleanupRead);
            access.complete(current);QCOMPARE(access.request,QString("select"));QVERIFY(access.requestedIds.isEmpty());
            QCOMPARE(access.snapshot.lines.size(),current.lines.size());
            for(qsizetype i=0;i<current.lines.size();++i)QCOMPARE(access.snapshot.lines[i].version,current.lines[i].version);
            const auto inspectionsBeforeDispatch=access.inspections;
            host.historyDispatch=[&]{
                QVERIFY(scene->selectionOperationPending());QVERIFY(!scene->selectionInkOverlay());
                // aboutToUndo and selectionCleared can refresh synchronously
                // before the controller queues the actual history callback.
                scene->refresh();scene->nativeAreaSelected(0,{});scene->nativeContentChanged();
                QCOMPARE(access.inspections,inspectionsBeforeDispatch);QVERIFY(!access.pending);
            };
            host.nativeSelectionPresent=false;controller.setProperty("selectionItemCount",0);access.completeSelection();
            QCOMPARE(host.historyCalls,step+1);QCOMPARE(host.selectionCancels,0);QCOMPARE(host.lastHistoryAction,action);
            QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::HistoryObserve);
            QVERIFY(scene->selectionOperationPending());QVERIFY(!scene->state().value("canUndo").toBool());
            QVERIFY(!scene->state().value("canRedo").toBool());QVERIFY(!scene->action("undo"));
            restored.selectedIds.clear();restored.fingerprint+="-history";access.complete(restored);
            QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->m_selectedStrokes.isEmpty());QVERIFY(!scene->m_objectFaulted);
            QVERIFY(scene->state().value("canUndo").toBool());QVERIFY(scene->state().value("canRedo").toBool());
            repaper::drawing::Item revision;
            QVERIFY(scene->m_objectBindings->selectedModel(step==0?before.selectedIds:after.selectedIds,&revision));
            QCOMPARE(revision.width,step==0?item.width:edited.width);
            QCoreApplication::processEvents();QVERIFY(!access.pending);QCOMPARE(host.historyCalls,step+1);
        }
        scene->m_gesture.tool="rectangle";QVERIFY(scene->pointerBegin(700,500,"pen"));
        QVERIFY(scene->pointerEnd(900,700));QCOMPARE(access.request,QString("insert"));
    }
    void historyCleanupFailureNeverReplaysTheQueuedUndo(){
        auto current=page();current.selectedIds=add(current,resistor());beginSelection(current);
        QVERIFY(scene->action("undo"));access.complete(current,false);
        QVERIFY(scene->m_pendingHistoryAction.isEmpty());QCOMPARE(host.historyCalls,0);
        scene->refresh();access.complete(current);access.completeSelection();
        QVERIFY(!scene->selectionOperationPending());QCOMPARE(host.historyCalls,0);
        QCoreApplication::processEvents();QCOMPARE(host.historyCalls,0);
    }
    void pageChangeDuringHistoryCleanupDropsTheQueuedUndo(){
        auto current=page();current.selectedIds=add(current,resistor());beginSelection(current);
        QVERIFY(scene->action("undo"));host.context.insert("pageId","page-b");scene->refresh();
        access.complete(current);QCOMPARE(host.historyCalls,0);QVERIFY(scene->m_pendingHistoryAction.isEmpty());
        QVERIFY(scene->m_selectedStrokes.isEmpty());QVERIFY(!scene->m_objectSelectionVerified);
    }
    void historyWithoutSelectionGuardsReentrantRefreshUntilDispatchReturns(){
        host.historyDispatch=[&]{scene->refresh();scene->nativeAreaSelected(0,{});scene->nativeContentChanged();QCOMPARE(access.inspections,0);};
        QVERIFY(scene->action("undo"));QCOMPARE(host.historyCalls,1);QCOMPARE(access.inspections,1);
        QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::HistoryObserve);
        scene->setNativeToolActive(false);QVERIFY(scene->captureEnabled());
        access.complete(page());QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->captureEnabled());
    }
    void rejectedHistoryDispatchReleasesItsLockAndDoesNotRetry(){
        host.allowHistory=false;
        QVERIFY(!scene->action("undo"));QVERIFY(!scene->selectionOperationPending());QVERIFY(!access.pending);
        QCOMPARE(host.historyCalls,0);QCoreApplication::processEvents();QCOMPARE(host.historyCalls,0);
    }
    void propertiesCancelRestoresEverySessionEditIncludingColorWithFreshNativeIds(){
        auto baseline=page();const auto item=resistor();baseline.selectedIds=add(baseline,item);
        add(baseline,resistor(),800);historyAppend(baseline,10); // Existing content/history belongs to the page, before this popup.
        QVERIFY(scene->m_objectBindings->registerInserted(item,baseline,baseline.selectedIds));beginSelection(baseline);
        QVERIFY(scene->beginPropertiesSession());QVERIFY(scene->state().value("propertiesSessionActive").toBool());
        auto current=baseline;
        for(int edit=0;edit<2;++edit){
            QVERIFY(scene->action("width",qreal(5+edit)));const auto model=scene->m_replacingItem;
            QVERIFY(!scene->acceptPropertiesSession());QVERIFY(!scene->cancelPropertiesSession());
            access.complete(current);QCOMPARE(access.request,QString("replace"));
            auto after=current;const auto oldIds=current.selectedIds;
            after.lines.erase(std::remove_if(after.lines.begin(),after.lines.end(),[&](const auto &line){return oldIds.contains(line.id);}),after.lines.end());
            after.selectedIds=add(after,model,5000+edit*1000);after.fingerprint="session-edit-"+QByteArray::number(edit);historyAppend(after,20+edit);
            access.insertIds=after.selectedIds;access.complete(after);current=after;
            QVERIFY(scene->state().value("propertiesSessionCanCancel").toBool());QCOMPARE(scene->m_propertiesCommandCount,edit+1);
        }
        QVERIFY(scene->action("color",QString("#d90707")));
        QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::PropertiesColorRead);access.complete(current);
        QVERIFY(colorAccess.pending);QVERIFY(sameNativeHistory(colorAccess.before,current.history));
        auto recolored=current;
        for(auto &line:recolored.lines)if(current.selectedIds.contains(line.id)){
            line.id+=10000;line.version+="-red";line.contentVersion+="-red";line.stroke.color=QColor("#d90707");
        }
        for(auto &id:recolored.selectedIds)id+=10000;
        recolored.fingerprint="session-red";historyAppend(recolored,30);
        // A color helper can emit statusChanged after clearing its own busy
        // flag but before delivering its completion. The popup stays locked.
        colorAccess.pending=false;colorAccess.statusChanged();QVERIFY(scene->selectionOperationPending());
        QVERIFY(!scene->acceptPropertiesSession());QVERIFY(!scene->cancelPropertiesSession());
        colorAccess.complete(recolored.history);QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::PropertiesColorObserve);
        access.complete(recolored);current=recolored;
        QCOMPARE(scene->m_propertiesCommandCount,3);QVERIFY(scene->state().value("propertiesSessionCanCancel").toBool());
        QVERIFY(!scene->action("undo")); // The popup owns its changes until Valider or Annuler.
        QVERIFY(scene->cancelPropertiesSession());QVERIFY(scene->m_propertiesCancelPending);
        QVERIFY(!scene->selectionInkOverlay());QVERIFY(!scene->pointerBegin(700,500,"pen"));
        access.complete(current);QCOMPARE(access.request,QString("cancel-properties"));QCOMPARE(access.rollbacks,1);
        QVERIFY(sameNativeObjectContent(access.rollbackBaseline,baseline));QVERIFY(sameNativeHistory(access.rollbackExpected.history,current.history));
        QCOMPARE(access.rollbackExpected.history.undo.size()-access.rollbackBaseline.history.undo.size(),3);
        QVERIFY(!scene->acceptPropertiesSession());QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(0));
        auto restored=baseline;restored.selectedIds.clear();restored.fingerprint="cancel-restored";
        for(auto &line:restored.lines){line.id+=20000;line.version+="-undo-origin";}
        for(qsizetype i=current.history.undo.size();i>baseline.history.undo.size();--i)restored.history.redo.append(current.history.undo[i-1]);
        access.complete(restored);
        QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->m_propertiesSessionActive);QVERIFY(!scene->m_propertiesCancelPending);
        QVERIFY(scene->m_selectedStrokes.isEmpty());QVERIFY(!scene->m_objectFaulted);QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(1));
        QCOMPARE(host.historyCalls,0);QCOMPARE(host.selectionCancels,0);QCOMPARE(access.rollbacks,1);
        QVERIFY(nativeObjectContentRestored(baseline,scene->m_objectSnapshot));QVERIFY(!sameNativeObjectContent(baseline,scene->m_objectSnapshot));
        QCoreApplication::processEvents();QVERIFY(!access.pending);QCOMPARE(access.rollbacks,1);
    }
    void configurableStencilPropertiesCancelRestoresParametersBackgroundAndColor(){
        using namespace repaper::drawing;
        auto item=resistor();item.symbolId="table";item.stencilParameters={{"rows",4},{"columns",6},{"opaqueBackground",true}};
        setForegroundColor(item,QColor(Qt::blue));QVERIFY(rebuildNativeObject(item));
        auto baseline=page();baseline.selectedIds=add(baseline,item);historyAppend(baseline,10);
        QVERIFY(scene->m_objectBindings->registerInserted(item,baseline,baseline.selectedIds));beginSelection(baseline);
        QVERIFY(scene->state().value("selectionCanChangeWidth").toBool());
        QVERIFY(!scene->state().value("selectionCanChangeStyle").toBool());
        QVERIFY(!scene->action("style",QString("dashed")));
        QVERIFY(scene->beginPropertiesSession());auto current=baseline;
        for(int edit=0;edit<3;++edit){
            if(edit==0)QVERIFY(scene->action("stencilParameters",QVariantMap{{"rows",7},{"opaqueBackground",false}}));
            else if(edit==1)QVERIFY(scene->action("color",QString("#d90707")));
            else QVERIFY(scene->action("stencilParameters",QVariantMap{{"opaqueBackground",true}}));
            QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::ReplaceRead);QVERIFY(!colorAccess.pending);
            const auto model=scene->m_replacingItem;
            QCOMPARE(model.stencilParameters.value("rows").toInt(),7);
            QCOMPARE(model.stencilParameters.value("columns").toInt(),6);
            QCOMPARE(model.stencilParameters.value("opaqueBackground").toBool(),edit==2);
            QCOMPARE(foregroundColor(model),edit==0?QColor(Qt::blue):QColor("#d90707"));
            QCOMPARE(isBackgroundStroke(model,0),edit==2);
            if(edit==2)QCOMPARE(model.strokes.first().color,QColor(Qt::white));
            QVERIFY(!scene->acceptPropertiesSession());QVERIFY(!scene->cancelPropertiesSession());
            access.complete(current);QCOMPARE(access.request,QString("replace"));
            const auto prepared=static_cast<WorkflowScene*>(scene.data())->preparedStrokes;
            QCOMPARE(prepared.size(),model.strokes.size());
            for(qsizetype i=0;i<prepared.size();++i)QCOMPARE(prepared[i].color,model.strokes[i].color);
            auto after=current;const auto oldIds=current.selectedIds;
            after.lines.erase(std::remove_if(after.lines.begin(),after.lines.end(),[&](const auto &line){return oldIds.contains(line.id);}),after.lines.end());
            after.selectedIds=add(after,model,5000+edit*1000);after.fingerprint="stencil-edit-"+QByteArray::number(edit);historyAppend(after,20+edit);
            access.insertIds=after.selectedIds;access.complete(after);current=after;
            QCOMPARE(scene->m_objectModel.stencilParameters,model.stencilParameters);
            QCOMPARE(foregroundColor(scene->m_objectModel),foregroundColor(model));
            QCOMPARE(scene->m_propertiesCommandCount,edit+1);QVERIFY(scene->state().value("propertiesSessionCanCancel").toBool());
        }
        QVERIFY(scene->cancelPropertiesSession());access.complete(current);
        QCOMPARE(access.request,QString("cancel-properties"));QCOMPARE(access.rollbacks,1);
        QVERIFY(sameNativeObjectContent(access.rollbackBaseline,baseline));QVERIFY(sameNativeHistory(access.rollbackExpected.history,current.history));
        QCOMPARE(access.rollbackExpected.history.undo.size()-access.rollbackBaseline.history.undo.size(),3);
        auto restored=baseline;restored.selectedIds.clear();restored.fingerprint="stencil-cancel-restored";QVector<quint64> restoredIds;
        for(auto &line:restored.lines){line.id+=20000;line.version+="-undo-origin";restoredIds.append(line.id);}
        for(qsizetype i=current.history.undo.size();i>baseline.history.undo.size();--i)restored.history.redo.append(current.history.undo[i-1]);
        access.complete(restored);
        QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->m_propertiesSessionActive);QVERIFY(!scene->m_objectFaulted);
        QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(1));QVERIFY(nativeObjectContentRestored(baseline,scene->m_objectSnapshot));
        Item original;QVERIFY(scene->m_objectBindings->selectedModel(restoredIds,&original));
        QCOMPARE(original.stencilParameters,item.stencilParameters);QCOMPARE(foregroundColor(original),QColor(Qt::blue));
        QVERIFY(isBackgroundStroke(original,0));QCOMPARE(original.strokes.first().color,QColor(Qt::white));
        QCOMPARE(host.historyCalls,0);QCOMPARE(access.rollbacks,1);
    }
    void propertiesCancelAlsoRestoresDuplicateAndRemoval(){
        auto baseline=page();const auto item=resistor();baseline.selectedIds=add(baseline,item);historyAppend(baseline,10);
        QVERIFY(scene->m_objectBindings->registerInserted(item,baseline,baseline.selectedIds));beginSelection(baseline);
        QVERIFY(scene->beginPropertiesSession());QVERIFY(scene->action("duplicate"));access.complete(baseline);
        auto copied=baseline;copied.selectedIds.clear();
        for(auto line:baseline.lines){line.id+=1000;line.lineageId=line.id;line.version+="-copy";line.contentVersion+="-copy";
            for(auto &point:line.stroke.points)point+=QPointF(24,24);
            copied.selectedIds.append(line.id);copied.lines.append(line);}
        copied.fingerprint="session-copy";historyAppend(copied,20);access.complete(copied);access.completeSelection();copied=access.snapshot;
        QCOMPARE(scene->m_propertiesCommandCount,1);
        QVERIFY(scene->action("remove"));access.complete(copied);
        auto removed=baseline;removed.selectedIds.clear();removed.fingerprint="session-removed";removed.history=copied.history;historyAppend(removed,30);
        access.complete(removed);access.completeSelection();removed=access.snapshot;
        QVERIFY(scene->m_selectedStrokes.isEmpty());QCOMPARE(scene->m_propertiesCommandCount,2);
        QVERIFY(scene->state().value("propertiesSessionCanCancel").toBool());QVERIFY(scene->cancelPropertiesSession());access.complete(removed);
        QCOMPARE(access.request,QString("cancel-properties"));
        auto restored=baseline;restored.selectedIds.clear();restored.history.redo={removed.history.undo[2],removed.history.undo[1]};
        access.complete(restored);QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(1));QVERIFY(!scene->m_propertiesSessionActive);
    }
    void propertiesCancelRestoresOrdinaryInkAfterRotationAndColor(){
        auto baseline=page();baseline.selectedIds=add(baseline,resistor());historyAppend(baseline,10);beginSelection(baseline);
        QVERIFY(!scene->m_hasObjectModel);QVERIFY(scene->beginPropertiesSession());
        const auto center=scene->m_selectedRect.center();QTransform rotation;rotation.translate(center.x(),center.y());rotation.rotate(90);rotation.translate(-center.x(),-center.y());
        QVERIFY(scene->action("rotate"));access.complete(baseline);
        auto rotated=moved(baseline,rotation);historyAppend(rotated,20);access.complete(rotated);access.completeSelection();rotated=access.snapshot;
        QCOMPARE(scene->m_propertiesCommandCount,1);QVERIFY(!scene->m_hasObjectModel);
        QVERIFY(scene->action("color",QString("#136aca")));access.complete(rotated);
        auto colored=rotated;
        for(auto &line:colored.lines){line.id+=1000;line.version+="-blue";line.contentVersion+="-blue";line.stroke.color=QColor("#136aca");}
        for(auto &id:colored.selectedIds)id+=1000;
        colored.fingerprint="ordinary-blue";historyAppend(colored,30);colorAccess.complete(colored.history);access.complete(colored);
        QCOMPARE(scene->m_propertiesCommandCount,2);QVERIFY(scene->cancelPropertiesSession());access.complete(colored);
        QCOMPARE(access.request,QString("cancel-properties"));QVERIFY(sameNativeObjectContent(access.rollbackBaseline,baseline));
        auto restored=baseline;restored.selectedIds.clear();restored.history.redo={colored.history.undo[2],colored.history.undo[1]};
        for(auto &line:restored.lines){line.id+=10000;line.version+="-undo";}
        access.complete(restored);QVERIFY(!scene->selectionOperationPending());QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(1));
    }
    void unchangedPropertiesCancelClearsSelectionAndPreservesEarlierRedo(){
        auto baseline=page();baseline.selectedIds=add(baseline,resistor());historyAppend(baseline,10);
        auto command=baseline.history.undo.takeLast();baseline.history.redo.append(command);beginSelection(baseline);
        QVERIFY(scene->beginPropertiesSession());QVERIFY(scene->cancelPropertiesSession());access.complete(baseline);
        QCOMPARE(access.request,QString("cancel-properties"));QVERIFY(sameNativeHistory(access.rollbackBaseline.history,access.rollbackExpected.history));
        auto restored=baseline;restored.selectedIds.clear();access.complete(restored);
        QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(1));QVERIFY(sameNativeHistory(scene->m_objectSnapshot.history,baseline.history));
        QVERIFY(!scene->m_propertiesSessionActive);QCOMPARE(host.historyCalls,0);
    }
    void acceptingPropertiesKeepsChangesAndNextPopupGetsANewBaseline(){
        auto baseline=page();auto item=resistor();baseline.selectedIds=add(baseline,item);beginSelection(baseline);
        QVERIFY(scene->m_objectBindings->registerInserted(item,baseline,baseline.selectedIds));beginSelection(baseline);
        QVERIFY(scene->beginPropertiesSession());QVERIFY(scene->action("width",qreal(8)));const auto model=scene->m_replacingItem;
        access.complete(baseline);auto changed=page();changed.selectedIds=add(changed,model,5000);changed.fingerprint="accepted";historyAppend(changed,20);
        access.insertIds=changed.selectedIds;access.complete(changed);QVERIFY(scene->acceptPropertiesSession());
        QCOMPARE(scene->m_objectModel.width,qreal(8));QCOMPARE(access.rollbacks,0);QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(0));
        QVERIFY(scene->beginPropertiesSession());QVERIFY(sameNativeObjectContent(scene->m_propertiesBaseline,changed));
        QCOMPARE(scene->m_propertiesCommandCount,0);QVERIFY(scene->acceptPropertiesSession());QVERIFY(scene->acceptPropertiesSession());
    }
    void unrelatedHistoryOrInkPreventsPropertiesCancelWithoutUndo_data(){
        QTest::addColumn<bool>("historyChanged");QTest::newRow("unowned-history")<<true;QTest::newRow("unrecorded-content")<<false;
    }
    void unrelatedHistoryOrInkPreventsPropertiesCancelWithoutUndo(){
        QFETCH(bool,historyChanged);auto baseline=page();baseline.selectedIds=add(baseline,resistor());beginSelection(baseline);
        QVERIFY(scene->beginPropertiesSession());QVERIFY(scene->cancelPropertiesSession());
        auto external=baseline;if(historyChanged)historyAppend(external,99);else external.lines[0].version+="-external";
        access.complete(external);QCOMPARE(access.rollbacks,0);QCOMPARE(host.historyCalls,0);
        QVERIFY(!scene->selectionOperationPending());QVERIFY(scene->m_propertiesSessionActive);
        QVERIFY(!scene->state().value("propertiesSessionCanCancel").toBool());QVERIFY(!scene->state().value("propertiesSessionCanEdit").toBool());
        QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(0));QVERIFY(scene->acceptPropertiesSession());
    }
    void propertiesCancelFailureOrPageChangeNeverReportsSuccess_data(){
        QTest::addColumn<bool>("pageChanged");QTest::newRow("worker-failure")<<false;QTest::newRow("page-changed")<<true;
    }
    void propertiesCancelFailureOrPageChangeNeverReportsSuccess(){
        QFETCH(bool,pageChanged);auto baseline=page();baseline.selectedIds=add(baseline,resistor());beginSelection(baseline);
        QVERIFY(scene->beginPropertiesSession());QVERIFY(scene->cancelPropertiesSession());access.complete(baseline);
        QCOMPARE(access.rollbacks,1);
        if(pageChanged){host.context.insert("pageId","page-b");scene->refresh();access.complete(baseline);QVERIFY(!scene->m_propertiesSessionActive);}
        else {access.complete(baseline,false);scene->refresh();access.complete(baseline);access.completeSelection();QVERIFY(scene->m_propertiesSessionActive);QVERIFY(!scene->m_propertiesSessionValid);}
        QCOMPARE(scene->m_propertiesCancelGeneration,qulonglong(0));QCoreApplication::processEvents();QCOMPARE(access.rollbacks,1);
    }
    void replacingObjectAfterToolChangeClearsItsExactNewSelection(){
        auto s=page();const auto item=resistor();s.selectedIds=add(s,item);
        QVERIFY(scene->m_objectBindings->registerInserted(item,s,s.selectedIds));beginSelection(s);
        QVERIFY(scene->resizeSelection(360,200));access.complete(s);const auto replacement=scene->m_replacingItem;
        QVERIFY(scene->action("tool","wire"));QVERIFY(scene->selectionOperationPending());
        auto after=page();after.selectedIds=add(after,replacement,5000);after.fingerprint="replacement";access.insertIds=after.selectedIds;
        access.complete(after);QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::CleanupRead);
        access.complete(after);QVERIFY(access.requestedIds.isEmpty());access.completeSelection();
        QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->m_hasObjectModel);QVERIFY(scene->m_selectedStrokes.isEmpty());
    }
    void connectedComponentEditIsOneReplacementWithBranchAndExactFocus_data(){
        QTest::addColumn<QString>("operation");
        QTest::newRow("drag")<<QString("move");QTest::newRow("rotate")<<QString("rotate");
        QTest::newRow("resize")<<QString("resize");QTest::newRow("two-components")<<QString("group");
    }
    void connectedComponentEditIsOneReplacementWithBranchAndExactFocus(){
        QFETCH(QString,operation);using namespace repaper::drawing;
        auto left=resistor(),right=resistor();QTransform translate;translate.translate(650,280);QVERIFY(transform(right,translate));
        Item wire;wire.kind="wire";wire.sourcePoints={left.anchors.last(),right.anchors.first()};
        wire.startAttachment={left.id,left.portIds.last()};wire.endAttachment={right.id,right.portIds.first()};
        Document circuit{left,right,wire};QVERIFY(reroute(circuit));wire=circuit.last();
        Item branch;branch.kind="wire";branch.startAttachment={wire.id,"route",.45};
        QPointF junction;QVERIFY(portPosition(circuit,branch.startAttachment,&junction));branch.sourcePoints={junction,{720,900}};
        circuit.append(branch);QVERIFY(reroute(circuit));branch=circuit.last();
        auto before=page();QVector<QVector<quint64>> originalGroups;
        for(int i=0;i<circuit.size();++i)originalGroups.append(add(before,circuit[i],100+i*100));
        for(int i=0;i<circuit.size();++i)QVERIFY(scene->m_objectBindings->registerInserted(circuit[i],before,originalGroups[i]));
        before.selectedIds=originalGroups[0];if(operation=="group")before.selectedIds+=originalGroups[1];
        beginSelection(before);const auto oldWire=wire.strokes;
        if(operation=="resize")QVERIFY(scene->resizeSelection(310,180));
        else if(operation=="rotate")QVERIFY(scene->transformSelection({{"kind","rotate"},{"anchor",mapBox(left,.5,.5)},{"angle",90.}}));
        else QVERIFY(scene->transformSelection({{"kind","move"},{"delta",QPointF(50,130)}}));
        QCOMPARE(access.request,QString("inspect"));QCOMPARE(access.replacements,0);
        access.complete(before);QCOMPARE(access.request,QString("select"));
        for(auto id:originalGroups[0])QVERIFY(access.requestedIds.contains(id));
        for(auto id:originalGroups[2])QVERIFY(access.requestedIds.contains(id));
        if(operation!="group")for(auto id:originalGroups[1])QVERIFY(!access.requestedIds.contains(id));
        access.completeSelection();QCOMPARE(access.request,QString("replace"));QCOMPARE(access.replacements,1);
        const auto replacementModels=scene->m_replacingDocument;QVERIFY(replacementModels.size()>=3);
        const auto focusCount=operation=="group"?2:1;
        auto after=before;const auto removed=access.requestedIds;
        after.lines.erase(std::remove_if(after.lines.begin(),after.lines.end(),[&](const auto &line){return removed.contains(line.id);}),after.lines.end());
        after.selectedIds.clear();QVector<quint64> expectedFocus;
        for(int i=0;i<replacementModels.size();++i){const auto ids=add(after,replacementModels[i],5000+i*100);after.selectedIds+=ids;if(i<focusCount)expectedFocus+=ids;}
        after.fingerprint="connected-edit";historyAppend(after,0x8888);access.insertIds=after.selectedIds;
        access.complete(after);QCOMPARE(access.request,QString("select"));QCOMPARE(access.replacements,1);
        access.completeSelection();QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->m_objectFaulted);
        auto actualSelection=scene->m_objectSnapshot.selectedIds;std::sort(actualSelection.begin(),actualSelection.end());std::sort(expectedFocus.begin(),expectedFocus.end());
        QCOMPARE(actualSelection,expectedFocus);
        const auto actual=scene->m_objectBindings->documentModels();QCOMPARE(actual.size(),4);
        const auto byId=[&](const QString &id){for(const auto &model:actual)if(model.id==id)return model;return Item{};};
        const auto movedLeft=byId(left.id),newWire=byId(wire.id),newBranch=byId(branch.id);
        QCOMPARE(newWire.startAttachment.objectId,left.id);QCOMPARE(newWire.endAttachment.objectId,right.id);
        QVERIFY(QLineF(newWire.sourcePoints[0],movedLeft.anchors.last()).length()<.01);
        QPointF actualJunction;QVERIFY(portPosition(actual,newBranch.startAttachment,&actualJunction));
        QVERIFY(QLineF(newBranch.sourcePoints[0],actualJunction).length()<.01);
        QCOMPARE(newBranch.startAttachment.objectId,wire.id);QVERIFY(newWire.strokes[0].points!=oldWire[0].points);
        QCOMPARE(scene->m_objectSnapshot.history.undo.size(),before.history.undo.size()+1);
    }
    void newWireRemembersBothNativePortsAndShowsAnOrthogonalPreview(){
        using namespace repaper::drawing;auto left=resistor(),right=resistor();QTransform t;t.translate(650,280);QVERIFY(transform(right,t));
        auto before=page();const auto a=add(before,left),b=add(before,right,500);
        QVERIFY(scene->m_objectBindings->registerInserted(left,before,a));QVERIFY(scene->m_objectBindings->registerInserted(right,before,b));
        scene->m_objectCacheDirty=false;
        scene->m_gesture.tool="wire";scene->m_objectSnapshot=before;
        const auto start=left.anchors.last(),end=right.anchors.first();
        QVERIFY(scene->pointerBegin(start.x()+2,start.y()+2,"pen"));
        QVERIFY(scene->pointerMove(end.x()-2,end.y()-2));QVERIFY(scene->pointerEnd(end.x(),end.y()));
        QCOMPARE(access.request,QString("insert"));const auto wire=scene->m_insertingItem;
        QCOMPARE(wire.startAttachment.objectId,left.id);QCOMPARE(wire.endAttachment.objectId,right.id);
        QCOMPARE(wire.sourcePoints.first(),start);QCOMPARE(wire.sourcePoints.last(),end);
        const auto path=wirePoints(wire);QVERIFY(path.size()>=4);
        for(int i=1;i<path.size();++i)QVERIFY(qAbs(path[i].x()-path[i-1].x())<.001||qAbs(path[i].y()-path[i-1].y())<.001);
    }
    void connectedSelectionRefusesChangedPageOrDependentInk_data(){
        QTest::addColumn<bool>("changePage");QTest::newRow("page-changed")<<true;QTest::newRow("dependent-wire-changed")<<false;
    }
    void connectedSelectionRefusesChangedPageOrDependentInk(){
        QFETCH(bool,changePage);using namespace repaper::drawing;
        const auto component=resistor();Item wire;wire.kind="wire";wire.sourcePoints={component.anchors.last(),{850,560}};
        wire.startAttachment={component.id,component.portIds.last()};QVERIFY(routeWire(wire,{component}));
        auto before=page();const auto componentIds=add(before,component),wireIds=add(before,wire,500);
        QVERIFY(scene->m_objectBindings->registerInserted(component,before,componentIds));QVERIFY(scene->m_objectBindings->registerInserted(wire,before,wireIds));
        before.selectedIds=componentIds;beginSelection(before);
        QVERIFY(scene->transformSelection({{"kind","move"},{"delta",QPointF(80,130)}}));access.complete(before);
        QCOMPARE(scene->m_objectPhase,NativeScene::ObjectPhase::ReplaceSelect);QCOMPARE(access.replacements,0);
        if(changePage){host.context.insert("pageId","page-b");scene->refresh();access.completeSelection();}
        else {
            auto changed=access.snapshot;changed.selectedIds=access.requestedIds;changed.fingerprint="external-dependent-change";
            for(auto &line:changed.lines)if(wireIds.contains(line.id)){line.version+="-changed";line.contentVersion=line.version;line.stroke.points[1]+=QPointF(8,8);break;}
            access.complete(changed);
        }
        QCOMPARE(access.replacements,0);QCOMPARE(host.transforms,0);
    }
};
QTEST_MAIN(NativeObjectWorkflowTest)
#include "NativeObjectWorkflowTest.moc"

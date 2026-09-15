#include "NativeScene.h"
#include <QQuickItem>
#include <QTransform>
#include <QtTest>

// Exercise the actual NativeScene operation lifecycle without native firmware.
// Empty removal receipts are observable without interpreting private Line data;
// nonempty receipts deliberately cannot match this host and must stay locked.
class LifecycleHost : public QObject {
    Q_OBJECT
public:
    QVariantMap context{{"documentId","document-a"},{"pageId","page-a"},{"layer",0},
        {"working",false},{"queueSize",0},{"canUndo",true},{"canRedo",true}};
    int clearCalls=0,activateCalls=0,undoCalls=0;
    int presentationCalls=0;
    qulonglong presentationToken=0;
    bool allowPresentation=true;
    int coordinateCalls=0;
    QTransform viewMapping;
    bool allowClear=true;
    Q_INVOKABLE QVariant coordinateMappingAvailable(){return true;}
    Q_INVOKABLE QVariant readState(){return context;}
    Q_INVOKABLE QVariant cloneSelection(){return {};}
    Q_INVOKABLE QVariant clearCustomSelection(){++clearCalls;return allowClear;}
    Q_INVOKABLE QVariant activateCustomTool(){++activateCalls;return true;}
    Q_INVOKABLE QVariant paperToView(QVariant point){return point;}
    Q_INVOKABLE QVariant viewToPaper(QVariant point){++coordinateCalls;return viewMapping.map(point.toPointF());}
    Q_INVOKABLE QVariant viewScale(){return 1.;}
    Q_INVOKABLE QVariant selectionAction(QVariant,QVariant){++undoCalls;return true;}
    Q_INVOKABLE QVariant presentCommittedPreview(QVariant token){++presentationCalls;presentationToken=token.toULongLong();return allowPresentation;}
};

class NativeSelectionLifecycleTest : public QObject {
    Q_OBJECT
    QObject controller;
    QQuickItem view;
    LifecycleHost host;
    QScopedPointer<NativeScene> scene;
    void pending(const QString &kind="remove"){
        PaperDrawing::Stroke stroke;stroke.width=3;stroke.color=Qt::black;stroke.points={{10,10},{30,30}};
        scene->m_selectedStrokes={stroke};scene->m_selectedRect={8.5,8.5,23,23};scene->m_selectionCount=1;
        scene->m_selectionContext=host.context;
        scene->m_selectionWaiting=true;scene->m_selectionAwaitContent=true;scene->m_selectionSignal=false;
        const QVariantMap change=kind=="remove"?QVariantMap{{"kind","remove"}}:
            QVariantMap{{"kind","move"},{"delta",QPointF(10,20)}};
        QVERIFY(scene->m_affineReceipt.begin(scene->m_selectedStrokes,change));
    }
    void poll(int count){
        for(int i=0;i<count;++i){scene->m_selectionTimer.stop();scene->checkSelectionObservation();}
    }
private slots:
    void init(){
        host.context={{"documentId","document-a"},{"pageId","page-a"},{"layer",0},
            {"working",false},{"queueSize",0},{"canUndo",true},{"canRedo",true}};
        host.clearCalls=host.activateCalls=host.undoCalls=0;host.allowClear=true;
        host.presentationCalls=0;host.presentationToken=0;host.allowPresentation=true;
        host.coordinateCalls=0;host.viewMapping=QTransform();
        controller.setProperty("pendingEdit",QTransform());controller.setProperty("selectionItemCount",0);
        controller.setProperty("pageId","page-a");
        scene.reset(new NativeScene);
        scene->attach(&controller,&view,&host);
        scene->m_target=scene->m_lineType=scene->m_coordinates=scene->m_nativeToolActive=true;
        scene->m_gesture.tool="select";
    }
    void cleanup(){scene.reset();}
    void penBatchUpdatesBindingsOnceAndMapsThePageOnce(){
        scene->m_gesture.tool="pen";QVERIFY(scene->m_gesture.begin({20,20}));
        host.viewMapping=QTransform(2,0,0,3,10,20);
        QSignalSpy changed(scene.data(),&NativeScene::changed);
        QSignalSpy previewChanged(scene.data(),&NativeScene::previewChanged);
        const QVariantList samples{QPointF(10,10),QVariantMap{{"x",30.0},{"y",10.0}},QPointF(30,30),QPointF(10,30)};
        QVERIFY(scene->pointerMoveBatch(samples));
        QCOMPARE(changed.size(),0);QCOMPARE(previewChanged.size(),1);QCOMPARE(host.coordinateCalls,3);
        QCOMPARE(scene->m_gesture.preview().first().points,PaperDrawing::Polyline({{20,20},{30,50},{70,50},{70,110},{30,110}}));
        scene->pointerCancel();QVERIFY(!scene->pointerMoveBatch(samples));
        QVERIFY(scene->m_gesture.preview().isEmpty());
    }
    void committedPreviewWaitsForMatchingFrameAndKeepsInputLocked(){
        scene->m_gesture.tool="arrow";
        scene->m_pendingStrokes={{{{10,10},{100,100}},3,Qt::black}};
        scene->m_insertionContext=host.context;
        scene->beginPreviewHandover();
        QCOMPARE(host.presentationCalls,1);QVERIFY(scene->creationOperationPending());
        QCOMPARE(scene->previewStrokes().size(),1);
        QVERIFY(!scene->pointerBegin(50,50,"pen"));QVERIFY(!scene->action("undo"));
        scene->nativePreviewPresented(host.presentationToken+1);
        QVERIFY(scene->creationOperationPending());
        scene->setNativeToolActive(false);QVERIFY(scene->captureEnabled());
        scene->nativePreviewPresented(host.presentationToken);
        QVERIFY(!scene->creationOperationPending());QVERIFY(!scene->captureEnabled());
        QVERIFY(scene->previewStrokes().isEmpty());
    }
    void changingToolsKeepsCommittedPreviewUntilItsFrame(){
        scene->m_gesture.tool="arrow";
        scene->m_pendingStrokes={{{{10,10},{100,100}},3,Qt::black}};
        scene->m_insertionContext=host.context;scene->beginPreviewHandover();
        QVERIFY(scene->action("tool","select"));
        QCOMPARE(scene->previewStrokes().size(),1);
        scene->nativePreviewPresented(host.presentationToken);
        QVERIFY(scene->previewStrokes().isEmpty());
    }
    void pageChangeInvalidatesAQueuedPresentationCallback(){
        scene->m_pendingStrokes={{{{10,10},{100,100}},3,Qt::black}};
        scene->m_insertionContext=host.context;scene->beginPreviewHandover();
        const auto oldToken=host.presentationToken;
        host.context.insert("pageId","page-b");scene->refresh();
        QVERIFY(!scene->creationOperationPending());QVERIFY(scene->m_pendingStrokes.isEmpty());
        scene->m_pendingStrokes={{{{20,20},{200,200}},3,Qt::black}};
        scene->m_insertionContext=host.context;scene->beginPreviewHandover();
        scene->nativePreviewPresented(oldToken);QVERIFY(scene->creationOperationPending());
        scene->nativePreviewPresented(host.presentationToken);QVERIFY(!scene->creationOperationPending());
    }
    void unavailableOrStalledWindowCannotKeepCompletedInsertionLocked(){
        scene->m_insertionContext=host.context;host.allowPresentation=false;
        scene->m_pendingStrokes={{{{10,10},{100,100}},3,Qt::black}};
        scene->beginPreviewHandover();QVERIFY(!scene->creationOperationPending());
        host.allowPresentation=true;
        scene->m_pendingStrokes={{{{10,10},{100,100}},3,Qt::black}};
        scene->beginPreviewHandover();QVERIFY(scene->creationOperationPending());
        QVERIFY(QMetaObject::invokeMethod(&scene->m_previewHandoverTimer,"timeout"));
        QVERIFY(!scene->creationOperationPending());QVERIFY(scene->m_pendingStrokes.isEmpty());
        QVERIFY(scene->evidence().value("previewHandoverFrameTimedOut").toBool());
    }
    void rotationPreviewBoundsFollowActualInkAndIdleDoesNotOverpaint(){
        scene->m_selectedStrokes={{{{0,0},{100,100}},3,Qt::black}};
        scene->m_selectedRect={0,0,100,100};scene->m_selectionContext=host.context;
        scene->m_selectionCount=1;scene->m_selectionMaximumPointWidth=12;
        QCOMPARE(scene->previewStrokes().size(),1); // Selection border only, no bold duplicate ink.
        QVERIFY(scene->m_selectionGesture.begin(scene->m_selectedRect,{50,-36},1));
        const qreal offset=86/std::sqrt(2.0);
        QVERIFY(scene->m_selectionGesture.update({50+offset,50-offset}));
        scene->updateSelectionPreviewBounds();
        const auto state=scene->state();
        QVERIFY(std::abs(state.value("selectedShapeWidth").toDouble()-1)<1e-6);
        QVERIFY(std::abs(state.value("selectedShapeHeight").toDouble()-100*std::sqrt(2.0))<1e-6);
        QCOMPARE(scene->previewStrokes().size(),2);
        QCOMPARE(scene->previewStrokes().first().toMap().value("width").toDouble(),3.0);
    }
    void scaledPreviewUsesNativeThicknessFactor(){
        scene->m_selectedStrokes={{{{0,0},{100,100}},3,Qt::black}};
        scene->m_selectedRect={0,0,100,100};scene->m_selectionContext=host.context;
        scene->m_selectionCount=1;scene->m_selectionMaximumPointWidth=12;
        QVERIFY(scene->m_selectionGesture.begin(scene->m_selectedRect,{100,100},1));
        QVERIFY(scene->m_selectionGesture.update({400,100}));scene->updateSelectionPreviewBounds();
        QCOMPARE(scene->previewStrokes().first().toMap().value("width").toDouble(),6.0);
        QCOMPARE(scene->state().value("selectedShapeWidth").toDouble(),400.0);
    }
    void toolChangePreservesReceiptAndBlocksMutations(){
        pending();
        QVERIFY(scene->action("tool","line"));
        QVERIFY(scene->m_selectionWaiting);QVERIFY(scene->m_selectionAwaitContent);
        QVERIFY(scene->m_affineReceipt.valid());QCOMPARE(host.clearCalls,0);
        QVERIFY(scene->captureEnabled());QVERIFY(!scene->state().value("hasSelection").toBool());
        QVERIFY(!scene->state().value("canUndo").toBool());QVERIFY(!scene->state().value("canRedo").toBool());
        QVERIFY(scene->previewStrokes().isEmpty());
        QVERIFY(!scene->pointerBegin(50,50,"pen"));QVERIFY(!scene->action("undo"));
        scene->nativeAreaSelected(0,{1,2,3,4});
        QVERIFY(!scene->m_selectionSignal);QVERIFY(scene->m_affineReceipt.valid());
        scene->refresh();QCOMPARE(scene->m_selectedStrokes.size(),1);
        scene->nativeContentChanged();
        QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->m_affineReceipt.valid());
        QCOMPARE(host.clearCalls,1);QVERIFY(scene->m_selectedStrokes.isEmpty());
        QVERIFY(scene->action("undo"));QCOMPARE(host.undoCalls,1);
    }
    void leavingNativeToolKeepsInputCapturedUntilReceipt(){
        pending();scene->setNativeToolActive(false);
        QVERIFY(scene->captureEnabled());QVERIFY(!scene->m_nativeToolActive);
        QVERIFY(scene->previewStrokes().isEmpty());QCOMPARE(host.clearCalls,0);
        scene->setNativeToolActive(true);QCOMPARE(host.activateCalls,0);
        scene->setNativeToolActive(false);scene->nativeContentChanged();
        QVERIFY(!scene->captureEnabled());QVERIFY(!scene->selectionOperationPending());
        QCOMPARE(host.clearCalls,1);
    }
    void returningToSelectDoesNotReviveDismissedOverlay(){
        pending("move");
        QVERIFY(scene->action("tool","line"));QVERIFY(scene->action("tool","select"));
        QVERIFY(!scene->state().value("hasSelection").toBool());
        QVERIFY(scene->previewStrokes().isEmpty());QVERIFY(scene->selectionOperationPending());
        scene->nativeContentChanged();
        QVERIFY(scene->selectionOperationPending());QVERIFY(scene->m_affineReceipt.valid());QCOMPARE(host.clearCalls,0);
    }
    void idleSpinnerAndTimeoutCannotCompleteAffine(){
        pending();scene->setNativeToolActive(false);poll(110);
        QVERIFY(scene->selectionOperationPending());QVERIFY(scene->m_affineReceipt.valid());
        QCOMPARE(host.clearCalls,0);QVERIFY(scene->captureEnabled());
        scene->nativeContentChanged();
        QVERIFY(!scene->selectionOperationPending());QCOMPARE(host.clearCalls,1);
    }
    void regionSelectionWaitsForLateCallbackAfterToolChange(){
        pending();scene->m_affineReceipt.clear();scene->m_selectionAwaitContent=false;
        scene->m_selectedStrokes.clear();scene->m_selectionCount=0;
        QVERIFY(scene->action("tool","line"));poll(110);
        QVERIFY(scene->selectionOperationPending());QCOMPARE(host.clearCalls,0);
        scene->nativeAreaSelected(1,{});QVERIFY(scene->selectionOperationPending());
        scene->nativeAreaSelected(0,{10,10,20,20});
        QVERIFY(!scene->selectionOperationPending());QCOMPARE(host.clearCalls,1);
    }
    void cleanupFailureKeepsLockAfterReceipt(){
        pending();host.allowClear=false;scene->setNativeToolActive(false);
        scene->nativeContentChanged();
        QVERIFY(!scene->m_selectionWaiting);QVERIFY(!scene->m_affineReceipt.valid());
        QVERIFY(scene->m_selectionCleanupPending);QVERIFY(scene->captureEnabled());
        QVERIFY(!scene->action("undo"));
        host.allowClear=true;poll(1);
        QVERIFY(!scene->selectionOperationPending());QVERIFY(!scene->captureEnabled());
    }
    void pageChangeDropsOldWorkWithoutNativeCleanup(){
        pending();scene->setNativeToolActive(false);host.context.insert("pageId","page-b");
        scene->refresh();
        QVERIFY(!scene->selectionOperationPending());QCOMPARE(host.clearCalls,0);
        scene->nativeContentChanged();scene->nativeAreaSelected(0,{});
        QCOMPARE(host.clearCalls,0);QVERIFY(!scene->selectionOperationPending());
    }
    void reattachDropsOldReceiptAndDestructionCallbacks(){
        auto oldController=new QObject;
        scene->attach(oldController,&view,&host);
        scene->m_target=scene->m_lineType=scene->m_coordinates=scene->m_nativeToolActive=true;
        pending();scene->setNativeToolActive(false);
        host.context.insert("pageId","page-b");
        scene->attach(&controller,&view,&host);
        delete oldController;
        QCOMPARE(scene->m_controller.data(),&controller);QVERIFY(!scene->selectionOperationPending());
        QCOMPARE(host.clearCalls,0);
    }
};
QTEST_MAIN(NativeSelectionLifecycleTest)
#include "NativeSelectionLifecycleTest.moc"

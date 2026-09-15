#include "NativeInputScheduler.h"
#include <QMouseEvent>
#include <QHoverEvent>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest>
#include <atomic>
#include <thread>

class SchedulerPenInput : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* surfaceManager MEMBER surfaceManager)
public:
    QObject *surfaceManager = nullptr;
signals:
    void penDownChanged(bool down);
};
class SchedulerSurfaceManager : public QObject {
    Q_OBJECT
public:
    QQuickItem *surface = nullptr;
    std::atomic_bool captures{false};
    int updates = 0;
    Q_INVOKABLE void updateRegions() { ++updates; captures = surface && surface->isVisible(); }
};
// Also makes assertion failures safely detach a pending input-thread request.
struct PendingSchedulerContact {
    QPointer<NativeInputScheduler> owner;
    std::thread worker;
    ~PendingSchedulerContact() { if (owner) owner->setNativePenInput(nullptr); if (worker.joinable()) worker.join(); }
};
class SchedulerObservedItem : public QQuickItem {
public:
    int presses=0;
    bool event(QEvent *event) override {
        if(event->type()==QEvent::MouseButtonPress){++presses;return false;}
        return QQuickItem::event(event);
    }
};

class NativeInputSchedulerTest : public QObject {
    Q_OBJECT
private slots:
    void repeatedTipContactsDoNotRebuildGlobalSurfaceTree() {
        using Tool=NativeInputScheduler::NativeTool;
        QQuickItem coordinates,surface(&coordinates);NativeInputScheduler scheduler;
        SchedulerPenInput input;SchedulerSurfaceManager manager;
        input.surfaceManager=&manager;manager.surface=&surface;
        Tool tool=Tool::Pen;
        scheduler.setTarget(&surface);scheduler.setCoordinateItem(&coordinates);
        connect(&scheduler,&NativeInputScheduler::penCaptureAllowedChanged,&surface,[&]{surface.setVisible(scheduler.penCaptureAllowed());});
        scheduler.setNativeToolProbe([&]{return tool;});scheduler.setNativePenInput(&input);
        for(int stroke=0;stroke<100;++stroke){emit input.penDownChanged(true);emit input.penDownChanged(false);}
        QCOMPARE(manager.updates,1);QVERIFY(manager.captures.load());
        tool=Tool::Eraser;emit input.penDownChanged(true);
        QCOMPARE(manager.updates,2);QVERIFY(!manager.captures.load());
        tool=Tool::Pen;emit input.penDownChanged(true);
        QCOMPARE(manager.updates,3);QVERIFY(manager.captures.load());
        scheduler.setEnabled(false);
        QCOMPARE(manager.updates,4);QVERIFY(!manager.captures.load());
        for(int stroke=0;stroke<100;++stroke)emit input.penDownChanged(true);
        QCOMPARE(manager.updates,4);
        scheduler.setEnabled(true);emit input.penDownChanged(true);
        QCOMPARE(manager.updates,5);QVERIFY(manager.captures.load());
    }
    void tabletMouseSynthesisIsPreservedWhileFingerFallbackIsRejected() {
        QQuickItem coordinates;SchedulerObservedItem surface;NativeInputScheduler scheduler;
        scheduler.setTarget(&surface);scheduler.setCoordinateItem(&coordinates);
        QPointingDevice tip("tip",4,QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen,QInputDevice::Capability::Position,1,1);
        QPointingDevice finger("finger",2,QInputDevice::DeviceType::TouchScreen,
            QPointingDevice::PointerType::Finger,QInputDevice::Capability::Position,10,0);
        for(auto source:{Qt::MouseEventSynthesizedByQt,Qt::MouseEventSynthesizedBySystem}) {
            QMouseEvent pen(QEvent::MouseButtonPress,{20,20},{20,20},{20,20},Qt::RightButton,Qt::RightButton,Qt::NoModifier,source,&tip);
            pen.setAccepted(false);const int before=surface.presses;QCoreApplication::sendEvent(&surface,&pen);
            QCOMPARE(surface.presses,before+1);QVERIFY(!pen.isAccepted());
            QMouseEvent touch(QEvent::MouseButtonPress,{20,20},{20,20},{20,20},Qt::LeftButton,Qt::LeftButton,Qt::NoModifier,source,&finger);
            QCoreApplication::sendEvent(&surface,&touch);QCOMPARE(surface.presses,before+1);QVERIFY(!touch.isAccepted());
        }
    }
    void pageRebindingReusesNativeReceiver() {
        SchedulerPenInput input;SchedulerSurfaceManager manager;input.surfaceManager=&manager;
        const auto count=[] { return qApp->findChildren<QObject*>("repaperNativePenContactRelay",Qt::FindDirectChildrenOnly).size(); };
        const int before=count();
        for(int i=0;i<20;++i) {
            NativeInputScheduler scheduler;scheduler.setNativeToolProbe([]{return NativeInputScheduler::NativeTool::Pen;});
            scheduler.setNativePenInput(&input);emit input.penDownChanged(true);QVERIFY(scheduler.penCaptureAllowed());
            QCOMPARE(count(),before+1);
        }
        emit input.penDownChanged(true);QCOMPARE(count(),before+1);
    }
    void nativeCancellationCanDestroySchedulerDuringGuiDelivery() {
        SchedulerPenInput input;SchedulerSurfaceManager manager;input.surfaceManager=&manager;
        auto *scheduler=new NativeInputScheduler;QPointer<NativeInputScheduler> alive=scheduler;
        std::atomic<NativeInputScheduler::NativeTool> tool{NativeInputScheduler::NativeTool::Pen};
        scheduler->setNativeToolProbe([&]{return tool.load();});scheduler->setNativePenInput(&input);
        emit input.penDownChanged(true);QVERIFY(scheduler->penCaptureAllowed());
        connect(scheduler,&NativeInputScheduler::cancelRequested,qApp,[&]{delete scheduler;});
        tool=NativeInputScheduler::NativeTool::Eraser;std::atomic_bool done{false};
        PendingSchedulerContact pending{scheduler,std::thread([&]{emit input.penDownChanged(true);done=true;})};
        QTRY_VERIFY_WITH_TIMEOUT(done.load(),1000);pending.worker.join();QVERIFY(alive.isNull());
    }
    void nativeTipConfirmationRestoresSurfaceAfterTypedEraserProximity() {
        QQuickWindow window;QQuickItem coordinates(window.contentItem()),surface(&coordinates);
        NativeInputScheduler scheduler;SchedulerPenInput input;SchedulerSurfaceManager manager;
        manager.surface=&surface;input.surfaceManager=&manager;
        scheduler.setTarget(&surface);scheduler.setCoordinateItem(&coordinates);
        connect(&scheduler,&NativeInputScheduler::penCaptureAllowedChanged,&surface,[&]{surface.setVisible(scheduler.penCaptureAllowed());});
        scheduler.setNativeToolProbe([]{return NativeInputScheduler::NativeTool::Pen;});scheduler.setNativePenInput(&input);
        emit input.penDownChanged(true);QVERIFY(scheduler.penCaptureAllowed());
        QPointingDevice eraser("eraser",8,QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Eraser,QInputDevice::Capability::Position,1,1);
        QHoverEvent hover(QEvent::HoverMove,{20,30},{19,29},Qt::NoModifier,&eraser);
        QCoreApplication::sendEvent(&window,&hover);QVERIFY(!scheduler.penCaptureAllowed());
        emit input.penDownChanged(true);QVERIFY(scheduler.penCaptureAllowed());QVERIFY(manager.captures.load());
    }
    void missingNativeContractsStayClosedAndDisabledInputNeverWaitsForGui() {
        SchedulerPenInput input;NativeInputScheduler scheduler;QObject wrongManager;
        int probes=0;scheduler.setNativeToolProbe([&]{++probes;return NativeInputScheduler::NativeTool::Pen;});
        scheduler.setNativePenInput(&input);emit input.penDownChanged(true);QVERIFY(!scheduler.penCaptureAllowed());
        input.surfaceManager=&wrongManager;emit input.penDownChanged(true);QVERIFY(!scheduler.penCaptureAllowed());
        SchedulerSurfaceManager manager;input.surfaceManager=&manager;
        scheduler.setEnabled(false);const int before=probes;std::atomic_bool done{false};
        PendingSchedulerContact pending{&scheduler,std::thread([&]{emit input.penDownChanged(true);done=true;})};
        for(int i=0;i<200&&!done.load();++i)QThread::msleep(1);
        QVERIFY(done.load());pending.worker.join();pending.owner.clear();QCOMPARE(probes,before);
        scheduler.setEnabled(true);emit input.penDownChanged(true);QVERIFY(scheduler.penCaptureAllowed());
        QObject missingSignal;scheduler.setNativePenInput(&missingSignal);QVERIFY(!scheduler.penCaptureAllowed());
    }
    void nativeClassificationUpdatesRegionBeforeFirstPointAndSurvivesRelease() {
        using Tool = NativeInputScheduler::NativeTool;
        QQuickItem coordinates, surface(&coordinates); NativeInputScheduler scheduler;
        SchedulerPenInput input; SchedulerSurfaceManager manager; manager.surface=&surface; input.surfaceManager=&manager;
        std::atomic<Tool> tool{Tool::Eraser};
        scheduler.setTarget(&surface); scheduler.setCoordinateItem(&coordinates);
        connect(&scheduler,&NativeInputScheduler::penCaptureAllowedChanged,&surface,[&] { surface.setVisible(scheduler.penCaptureAllowed()); });
        scheduler.setNativeToolProbe([&] { return tool.load(); }); scheduler.setNativePenInput(&input);
        QVERIFY(!surface.isVisible());
        const auto contact = [&](Tool value, bool expectedCapture) {
            tool=value; std::atomic_bool done{false}, routed{!expectedCapture};
            PendingSchedulerContact pending{&scheduler, std::thread([&] { emit input.penDownChanged(true); routed=manager.captures.load(); done=true; })};
            QTRY_VERIFY_WITH_TIMEOUT(done.load(),1000);
            pending.worker.join(); pending.owner.clear();
            QCOMPARE(routed.load(),expectedCapture); QCOMPARE(surface.isVisible(),expectedCapture);
        };
        contact(Tool::Eraser,false); QCOMPARE(manager.updates,1);
        emit input.penDownChanged(false); QVERIFY(!scheduler.penCaptureAllowed()); QCOMPARE(manager.updates,1);
        contact(Tool::Pen,true);
        QSignalSpy batches(&scheduler,&NativeInputScheduler::moveBatch);
        scheduler.begin(); scheduler.queueMove(12,13);
        contact(Tool::Eraser,false); scheduler.finish(); QCOMPARE(batches.count(),0);
        // The native Qt device remains Pen even throughout an eraser stroke.
        QPointingDevice fixedPen("firmware pen",4,QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen,QInputDevice::Capability::Position,1,1);
        QMouseEvent stale(QEvent::MouseMove,{20,20},{20,20},Qt::NoButton,Qt::LeftButton,Qt::NoModifier,&fixedPen);
        QCoreApplication::sendEvent(&surface,&stale); QVERIFY(!scheduler.penCaptureAllowed());
        contact(Tool::Pen,true); scheduler.begin(); scheduler.queueMove(20,21); scheduler.finish(); QCOMPARE(batches.count(),1);
        contact(Tool::Unknown,false); scheduler.begin(); scheduler.queueMove(30,31); scheduler.finish(); QCOMPARE(batches.count(),1);
    }
    void pendingContactIsReleasedByDestructionWithoutProcessingGuiCallback() {
        SchedulerPenInput input; std::atomic_bool probed{false},done{false};
        auto *scheduler=new NativeInputScheduler;
        scheduler->setNativeToolProbe([&] { probed=true; return NativeInputScheduler::NativeTool::Eraser; });
        scheduler->setNativePenInput(&input);
        PendingSchedulerContact pending{scheduler,std::thread([&] { emit input.penDownChanged(true); done=true; })};
        for(int i=0;i<200&&!probed.load();++i)QThread::msleep(1);
        QVERIFY(probed.load()); QVERIFY(!done.load());
        delete scheduler;
        for(int i=0;i<200&&!done.load();++i)QThread::msleep(1);
        QVERIFY(done.load()); pending.worker.join();
        QCoreApplication::processEvents(); // The queued callback must be harmless.
    }
    void replacingNativeInputInvalidatesQueuedOldContact() {
        SchedulerPenInput first,second; NativeInputScheduler scheduler;
        SchedulerSurfaceManager manager; second.surfaceManager=&manager;
        std::atomic_bool probed{false},done{false};
        scheduler.setNativeToolProbe([&] { probed=true; return NativeInputScheduler::NativeTool::Pen; });
        scheduler.setNativePenInput(&first);
        PendingSchedulerContact pending{&scheduler,std::thread([&] { emit first.penDownChanged(true); done=true; })};
        for(int i=0;i<200&&!probed.load();++i)QThread::msleep(1);
        QVERIFY(probed.load()); scheduler.setNativePenInput(&second);
        for(int i=0;i<200&&!done.load();++i)QThread::msleep(1);
        QVERIFY(done.load()); pending.worker.join(); pending.owner.clear();
        QCoreApplication::processEvents(); QVERIFY(!scheduler.penCaptureAllowed());
        emit second.penDownChanged(true); QVERIFY(scheduler.penCaptureAllowed());
    }
    void floodBatchesPreserveEverySampleAndFlushBeforeRelease() {
        QQuickItem coordinates, surface(&coordinates);NativeInputScheduler scheduler;
        scheduler.setTarget(&surface);scheduler.setCoordinateItem(&coordinates);scheduler.begin();
        QSignalSpy batches(&scheduler,&NativeInputScheduler::moveBatch);
        for(int i=0;i<815;++i)scheduler.queueMove(i,i%7);
        QCOMPARE(batches.count(),3);
        scheduler.finish();QCOMPARE(batches.count(),4);
        int count=0;
        for(const auto &emission:batches) {
            const auto points=emission.first().toList();QVERIFY(points.size()<=256);
            for(const auto &point:points) {
                const auto p=point.toMap();QCOMPARE(p.value("x").toInt(),count);QCOMPARE(p.value("y").toInt(),count%7);++count;
            }
        }
        QCOMPARE(count,815);QTest::qWait(30);QCOMPARE(batches.count(),4);
    }
    void forwardedMovesMapCoordinatesWithoutDependingOnMouseAreaPressed() {
        QQuickItem coordinates, surface(&coordinates);surface.setPosition({30,40});surface.setScale(2);
        surface.setTransformOrigin(QQuickItem::TopLeft);
        NativeInputScheduler scheduler;scheduler.setTarget(&surface);scheduler.setCoordinateItem(&coordinates);scheduler.begin();
        QSignalSpy batches(&scheduler,&NativeInputScheduler::moveBatch);
        QMouseEvent move(QEvent::MouseMove,QPointF(20,25),QPointF(20,25),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        move.setAccepted(false);QCoreApplication::sendEvent(&surface,&move);QVERIFY(move.isAccepted());
        scheduler.finish();QCOMPARE(batches.count(),1);
        const auto point=batches.first().first().toList().first().toMap();
        QCOMPARE(point.value("x").toReal(),qreal(70));QCOMPARE(point.value("y").toReal(),qreal(90));
    }
    void shortQueuePublishesDuringContactAndCancellationDropsQueuedWork() {
        QQuickItem coordinates,surface(&coordinates);NativeInputScheduler scheduler;
        scheduler.setTarget(&surface);scheduler.setCoordinateItem(&coordinates);scheduler.begin();
        QSignalSpy batches(&scheduler,&NativeInputScheduler::moveBatch);
        scheduler.queueMove(10,20);QTRY_COMPARE_WITH_TIMEOUT(batches.count(),1,150);
        scheduler.queueMove(30,40);scheduler.cancel();QTest::qWait(30);QCOMPARE(batches.count(),1);
        scheduler.finish();QCOMPARE(batches.count(),1);
        scheduler.begin();scheduler.queueMove(50,60);scheduler.setEnabled(false);
        QTest::qWait(30);QCOMPARE(batches.count(),1);
        scheduler.setEnabled(true);scheduler.queueMove(70,80);scheduler.finish();QCOMPARE(batches.count(),1);
    }
};
QTEST_MAIN(NativeInputSchedulerTest)
#include "NativeInputSchedulerTest.moc"

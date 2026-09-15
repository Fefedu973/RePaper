#include "NativeTouchGuard.h"
#include <QCoreApplication>
#include <QEventPoint>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTouchEvent>
#include <QTabletEvent>
#include <QHoverEvent>
#include <functional>

namespace {
class ObservedWindow final : public QQuickWindow {
public:
    std::function<void(QEvent *)> delivered;
protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
            || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel
            || event->type() == QEvent::WindowDeactivate || event->isPointerEvent()) {
            if (delivered) delivered(event);
            return false;
        }
        return QQuickWindow::event(event);
    }
};
QEventPoint contact(int id, QEventPoint::State state) {
    return QEventPoint(id, state, QPointF(20 + id * 10, 30), QPointF(20 + id * 10, 30));
}
}

class NativeTouchGuardTest final : public QObject {
    Q_OBJECT
    QPointingDevice device{"screen", 1, QInputDevice::DeviceType::TouchScreen,
        QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0};
    void send(ObservedWindow &window, QEvent::Type type, const QList<QEventPoint> &points,
              bool accepted = true) {
        QTouchEvent event(type, &device, Qt::NoModifier, points);
        event.setAccepted(accepted);
        QCoreApplication::sendEvent(&window, &event);
        QCOMPARE(event.isAccepted(), accepted);
    }
private slots:
    void eraserHoverCancelsWithoutConsumingAndOnlyFreshPenRestoresCapture() {
        ObservedWindow window; QQuickItem view(window.contentItem()); NativeTouchGuard guard;
        guard.attach(&view); QSignalSpy canceled(&guard, &NativeTouchGuard::cancelRequested);
        QPointingDevice eraser("eraser", 8, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Eraser, QInputDevice::Capability::Position, 1, 1);
        QPointingDevice pen("tip", 4, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position, 1, 1);
        int delivered = 0;
        window.delivered = [&](QEvent *event) { ++delivered; QVERIFY(!guard.acceptsBegin()); QVERIFY(!event->isAccepted()); };
        QHoverEvent hover(QEvent::HoverMove, QPointF(20, 30), QPointF(19, 29), Qt::NoModifier, &eraser);
        hover.setAccepted(false); QCoreApplication::sendEvent(&window, &hover);
        QVERIFY(!hover.isAccepted()); QCOMPARE(canceled.count(), 1);
        for (auto type : {QEvent::TabletPress, QEvent::TabletRelease}) {
            QTabletEvent event(type, &eraser, {20,30}, {20,30}, 0.6, 0,0,0,0,0, Qt::NoModifier, Qt::LeftButton, Qt::LeftButton);
            event.setAccepted(false); QCoreApplication::sendEvent(&window, &event); QVERIFY(!event.isAccepted());
        }
        QTabletEvent stale(QEvent::TabletRelease, &pen, {20,30}, {20,30}, 0,0,0,0,0,0, Qt::NoModifier, Qt::LeftButton, Qt::NoButton);
        stale.setAccepted(false); QCoreApplication::sendEvent(&window, &stale);
        QCOMPARE(delivered, 4); QVERIFY(!guard.penCaptureAllowed());
        window.delivered = [&](QEvent *) { QVERIFY(guard.penCaptureAllowed()); };
        // Barrel-button use is still the pen tip, never inferred as an eraser.
        QTabletEvent tip(QEvent::TabletPress, &pen, {20,30}, {20,30}, 0.6,0,0,0,0,0, Qt::NoModifier, Qt::RightButton, Qt::RightButton);
        QCoreApplication::sendEvent(&window, &tip); QVERIFY(guard.acceptsBegin());
    }
    void applicationProximityClassifiesEraserBeforeContact() {
        ObservedWindow window; QQuickItem view(window.contentItem()); NativeTouchGuard guard; guard.attach(&view);
        QPointingDevice eraser("eraser", 8, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Eraser, QInputDevice::Capability::Position, 1, 1);
        for (auto type : {QEvent::TabletEnterProximity, QEvent::TabletLeaveProximity}) {
            QTabletEvent event(type, &eraser, {20,30}, {20,30}, 0,0,0,0,0,0, Qt::NoModifier, Qt::NoButton, Qt::NoButton);
            QCoreApplication::sendEvent(qApp, &event); QVERIFY(!guard.penCaptureAllowed());
        }
    }
    void oneFingerDoesNotCancelOrConsume() {
        ObservedWindow window; QQuickItem view(window.contentItem()); NativeTouchGuard guard;
        guard.attach(&view); QSignalSpy canceled(&guard, &NativeTouchGuard::cancelRequested);
        int delivered = 0;
        window.delivered = [&](QEvent *) { ++delivered; QVERIFY(guard.acceptsBegin()); };
        send(window, QEvent::TouchBegin, {contact(1, QEventPoint::Pressed)}, false);
        send(window, QEvent::TouchUpdate, {contact(1, QEventPoint::Updated)});
        send(window, QEvent::TouchEnd, {contact(1, QEventPoint::Released)}, false);
        QCOMPARE(delivered, 3); QCOMPARE(canceled.count(), 0);
    }
    void secondFingerCancelsBeforeReleaseAndDoesNotRestartGesture() {
        ObservedWindow window; QQuickItem view(window.contentItem()); NativeTouchGuard guard;
        guard.attach(&view); QSignalSpy canceled(&guard, &NativeTouchGuard::cancelRequested);
        bool gestureActive = true; int commits = 0, delivered = 0;
        connect(&guard, &NativeTouchGuard::cancelRequested, this, [&] { gestureActive = false; });
        send(window, QEvent::TouchBegin, {contact(1, QEventPoint::Pressed)});
        window.delivered = [&](QEvent *event) {
            ++delivered; QVERIFY(guard.blockedSequence()); QVERIFY(!guard.acceptsBegin());
            if (event->type() == QEvent::TouchEnd && gestureActive) ++commits;
        };
        send(window, QEvent::TouchUpdate, {contact(1, QEventPoint::Stationary), contact(2, QEventPoint::Pressed)}, false);
        QCOMPARE(canceled.count(), 1);
        send(window, QEvent::TouchUpdate, {contact(1, QEventPoint::Released), contact(2, QEventPoint::Stationary)});
        send(window, QEvent::TouchEnd, {contact(2, QEventPoint::Released)});
        QCOMPARE(delivered, 3); QCOMPARE(commits, 0); QVERIFY(guard.blockedSequence());
        QCoreApplication::processEvents();
        QVERIFY(guard.acceptsBegin()); QVERIFY(!gestureActive);
        // A late synthesized mouse release still cannot commit a canceled gesture.
        if (gestureActive) ++commits;
        QCOMPARE(commits, 0);
    }
    void secondPressAndFirstReleaseInSameBatchCancel() {
        ObservedWindow window; QQuickItem view(window.contentItem()); NativeTouchGuard guard;
        guard.attach(&view); QSignalSpy canceled(&guard, &NativeTouchGuard::cancelRequested);
        send(window, QEvent::TouchBegin, {contact(1, QEventPoint::Pressed)});
        send(window, QEvent::TouchUpdate, {contact(1, QEventPoint::Released), contact(2, QEventPoint::Pressed)});
        QCOMPARE(canceled.count(), 1); QVERIFY(!guard.acceptsBegin());
    }
    void freshBeginSupersedesQueuedReset() {
        ObservedWindow window; QQuickItem view(window.contentItem()); NativeTouchGuard guard;
        guard.attach(&view);
        send(window, QEvent::TouchBegin, {contact(1, QEventPoint::Pressed), contact(2, QEventPoint::Pressed)});
        send(window, QEvent::TouchEnd, {contact(1, QEventPoint::Released), contact(2, QEventPoint::Released)});
        send(window, QEvent::TouchBegin, {contact(3, QEventPoint::Pressed), contact(4, QEventPoint::Pressed)});
        QCoreApplication::processEvents();
        QVERIFY(!guard.acceptsBegin());
        send(window, QEvent::TouchEnd, {contact(3, QEventPoint::Released), contact(4, QEventPoint::Released)});
        send(window, QEvent::TouchBegin, {contact(5, QEventPoint::Pressed)});
        QVERIFY(guard.acceptsBegin());
    }
    void cancelAndWindowLossAreDeliveredAfterCancellation() {
        ObservedWindow window; QQuickItem view(window.contentItem()); NativeTouchGuard guard;
        guard.attach(&view); QSignalSpy canceled(&guard, &NativeTouchGuard::cancelRequested);
        send(window, QEvent::TouchBegin, {contact(1, QEventPoint::Pressed)});
        window.delivered = [&](QEvent *) { QVERIFY(canceled.count() > 0); QVERIFY(!guard.acceptsBegin()); };
        send(window, QEvent::TouchCancel, {});
        QEvent deactivate(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&window, &deactivate);
        QCOMPARE(canceled.count(), 2);
    }
    void windowReplacementAndDetachRemoveOldFilter() {
        ObservedWindow first, second; QQuickItem view(first.contentItem()); NativeTouchGuard guard;
        guard.attach(&view); QSignalSpy canceled(&guard, &NativeTouchGuard::cancelRequested);
        view.setParentItem(second.contentItem());
        QVERIFY(canceled.count() >= 1); canceled.clear();
        send(first, QEvent::TouchBegin, {contact(1, QEventPoint::Pressed), contact(2, QEventPoint::Pressed)});
        QCOMPARE(canceled.count(), 0);
        send(second, QEvent::TouchBegin, {contact(1, QEventPoint::Pressed), contact(2, QEventPoint::Pressed)});
        QCOMPARE(canceled.count(), 1);
        guard.attach(nullptr); QVERIFY(!guard.acceptsBegin()); canceled.clear();
        send(second, QEvent::TouchBegin, {contact(3, QEventPoint::Pressed), contact(4, QEventPoint::Pressed)});
        QCOMPARE(canceled.count(), 0);
    }
};
QTEST_MAIN(NativeTouchGuardTest)
#include "NativeTouchGuardTest.moc"

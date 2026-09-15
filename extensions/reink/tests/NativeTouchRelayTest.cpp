#include "NativeTouchRelay.h"
#include <QQuickWindow>
#include <QPointingDevice>
#include <QSignalSpy>
#include <QTouchEvent>
#include <QtTest>

class RelayWindow : public QQuickWindow {
public:
    int delivered = 0;
protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
            || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {
            ++delivered;
            return false;
        }
        return QQuickWindow::event(event);
    }
};

// Reproduce only the attested public TouchArea.target filter contract. Native
// gesture thresholds, pen proximity and actual navigation remain device QA.
class RelayRecognizer : public QObject {
    Q_OBJECT
public:
    int received = 0, cancels = 0;
    bool adopt = false, sawWindow = false;
    QList<QPointF> positions;
    Q_INVOKABLE void cancelSequence() { ++cancels; adopt = false; }
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::TouchBegin && event->type() != QEvent::TouchUpdate
            && event->type() != QEvent::TouchEnd) return false;
        sawWindow = qobject_cast<QWindow *>(watched);
        const auto *touch = static_cast<QTouchEvent *>(event);
        ++received;
        for (const auto &point : touch->points()) positions.append(point.scenePosition());
        event->accept(); // Must not mutate the original while considering.
        return adopt;
    }
};

class NativeTouchRelayTest : public QObject {
    Q_OBJECT
    QPointingDevice device{"touch", 5, QInputDevice::DeviceType::TouchScreen,
        QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 10, 0};
    void send(RelayWindow &window, QEvent::Type type, QEventPoint::State state,
              QPointF position, bool expectAccepted = false) {
        QTouchEvent event(type, &device, Qt::NoModifier,
                          {QEventPoint(1, state, position, position)});
        event.setAccepted(false);
        QCoreApplication::sendEvent(&window, &event);
        QCOMPARE(event.isAccepted(), expectAccepted);
    }
    void setup(RelayWindow &window, QQuickItem &surface, NativeTouchRelay &relay,
               RelayRecognizer &recognizer) {
        surface.setParentItem(window.contentItem());
        surface.setPosition({30,40});surface.setSize({400,500});
        relay.setSurface(&surface);relay.setRecognizer(&recognizer);
        relay.eventTarget()->installEventFilter(&recognizer);relay.setEnabled(true);
    }
private slots:
    void nativeCandidatesPreserveFingerInputThenAdoptionOwnsTheRemainder() {
        RelayWindow window;QQuickItem surface;NativeTouchRelay relay;RelayRecognizer native;
        setup(window,surface,relay,native);QSignalSpy canceled(&relay,&NativeTouchRelay::cancelRequested);
        send(window,QEvent::TouchBegin,QEventPoint::Pressed,{80,90});
        send(window,QEvent::TouchUpdate,QEventPoint::Updated,{120,110});
        QCOMPARE(native.received,2);QCOMPARE(window.delivered,2);QCOMPARE(canceled.count(),0);
        QVERIFY(native.sawWindow);QVERIFY(!qobject_cast<QWindow*>(relay.eventTarget())->handle());
        QCOMPARE(native.positions,QList<QPointF>({{80,90},{120,110}}));
        native.adopt=true;
        send(window,QEvent::TouchUpdate,QEventPoint::Updated,{160,180},true);
        QCOMPARE(canceled.count(),1);QCOMPARE(window.delivered,3);QCOMPARE(native.cancels,0);
        // Adoption remains latched even when TouchEnd retires the native filter.
        native.adopt=false;
        send(window,QEvent::TouchEnd,QEventPoint::Released,{170,190},true);
        QCOMPARE(native.received,4);QCOMPARE(window.delivered,3);QCOMPARE(canceled.count(),1);
        send(window,QEvent::TouchBegin,QEventPoint::Pressed,{180,190});
        QCOMPARE(window.delivered,4);
        send(window,QEvent::TouchEnd,QEventPoint::Released,{180,190});
    }
    void outsideAndInspectorOriginsNeverEnterTheNativeRelay() {
        RelayWindow window;QQuickItem surface;NativeTouchRelay relay;RelayRecognizer native;
        setup(window,surface,relay,native);
        QQuickItem inspector(window.contentItem());inspector.setPosition({200,100});inspector.setSize({100,100});
        relay.setExcludedItems({QVariant::fromValue<QObject*>(&inspector)});
        for (const QPointF origin : {QPointF(10,90),QPointF(220,130)}) {
            send(window,QEvent::TouchBegin,QEventPoint::Pressed,origin);
            send(window,QEvent::TouchUpdate,QEventPoint::Updated,{80,90});
            send(window,QEvent::TouchEnd,QEventPoint::Released,{80,90});
        }
        QCOMPARE(native.received,0);QCOMPARE(window.delivered,6);
        inspector.setVisible(false);
        send(window,QEvent::TouchBegin,QEventPoint::Pressed,{220,130});
        QCOMPARE(native.received,1);
        send(window,QEvent::TouchEnd,QEventPoint::Released,{220,130});
    }
    void toolPageAndWindowLossCancelCandidatesWithoutReplayingLaterEvents() {
        RelayWindow window;QQuickItem surface;NativeTouchRelay relay;RelayRecognizer native;
        setup(window,surface,relay,native);QSignalSpy canceled(&relay,&NativeTouchRelay::cancelRequested);
        send(window,QEvent::TouchBegin,QEventPoint::Pressed,{80,90});
        relay.setEnabled(false);QCOMPARE(native.cancels,1);QCOMPARE(canceled.count(),1);
        send(window,QEvent::TouchEnd,QEventPoint::Released,{80,90});QCOMPARE(native.received,1);
        relay.setEnabled(true);
        send(window,QEvent::TouchBegin,QEventPoint::Pressed,{80,90});
        surface.setParentItem(nullptr);QCOMPARE(native.cancels,2);QCOMPARE(canceled.count(),2);
        send(window,QEvent::TouchEnd,QEventPoint::Released,{80,90});QCOMPARE(native.received,2);
        relay.setSurface(nullptr);
    }
};
QTEST_MAIN(NativeTouchRelayTest)
#include "NativeTouchRelayTest.moc"

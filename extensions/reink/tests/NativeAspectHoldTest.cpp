#include "NativeAspectHold.h"
#include <QtTest>
#include <limits>

class NativeAspectHoldTest final : public QObject {
    Q_OBJECT
private slots:
    void inactiveUntilBeginAndTimerLocksExactlyOnce() {
        NativeAspectHold hold;
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Off);
        hold.move({100, 200}, 1000);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Off);
        QVERIFY(!hold.advance(2000));
        QVERIFY(hold.begin({100, 200}, 4000));
        QVERIFY(hold.waiting());
        QVERIFY(!hold.advance(4000));
        QVERIFY(!hold.advance(4999));
        QVERIFY(hold.advance(5000));
        QVERIFY(hold.locked());
        QVERIFY(!hold.advance(5000));
        QVERIFY(!hold.advance(8000));
        hold.move({-5000, 8000}, 8000);
        QVERIFY(hold.locked());
    }

    void jitterUsesViewSpaceRadiusFromPress() {
        NativeAspectHold hold;
        QVERIFY(hold.begin({400, 300}, 0));
        hold.move({408, 300}, 100);
        hold.move({392, 300}, 200);
        hold.move({400, 308}, 300);
        hold.move({400, 292}, 400);
        hold.move({404, 306}, 999);
        QVERIFY(hold.waiting());
        QVERIFY(hold.advance(1000));
        QVERIFY(hold.locked());
    }

    void radialExcursionCancelsWhenOutAndBackBatchFlushesAtDeadline() {
        NativeAspectHold hold;
        QVERIFY(hold.begin({100, 200}, 0));
        // Both points were buffered until the timer flushed them. Each axis of
        // the first point is < 8, but its radius is > 8.
        hold.move({106, 206}, 1000);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        hold.move({100, 200}, 1000);
        QVERIFY(!hold.advance(1000));
        hold.move({100, 200}, 5000);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
    }

    void movementImmediatelyBeforeDeadlinePermanentlyCancels() {
        NativeAspectHold hold;
        QVERIFY(hold.begin({0, 0}, 10));
        hold.move({8.001, 0}, 1009);
        QVERIFY(!hold.advance(1010));
        QVERIFY(!hold.advance(90000));
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
    }

    void movementAtOrAfterDeadlineCancelsUntilTimerActuallyLocks_data() {
        QTest::addColumn<qint64>("moveTime");
        QTest::newRow("at deadline") << qint64(1000);
        QTest::newRow("delayed timer") << qint64(1200);
    }
    void movementAtOrAfterDeadlineCancelsUntilTimerActuallyLocks() {
        QFETCH(qint64, moveTime);
        NativeAspectHold hold;
        QVERIFY(hold.begin({20, 30}, 0));
        hold.move({24, 33}, 999);
        hold.move({200, 300}, moveTime);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(!hold.advance(moveTime));
        QVERIFY(!hold.locked());
    }

    void stationaryMovementDoesNotActivateLockByItself() {
        NativeAspectHold hold;
        QVERIFY(hold.begin({20, 30}, 0));
        hold.move({20, 30}, 1200);
        QVERIFY(hold.waiting());
        QVERIFY(!hold.locked());
        QVERIFY(hold.advance(1200));
        hold.move({200, 300}, 1200);
        QVERIFY(hold.locked());
    }

    void cancelResetAndNewPressStartIndependentAttempts() {
        NativeAspectHold hold;
        QVERIFY(hold.begin({1, 2}, 0));
        hold.cancel();
        QVERIFY(!hold.advance(1000));
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(hold.begin({3, 4}, 2000));
        QVERIFY(hold.advance(3000));
        hold.cancel();
        QVERIFY(!hold.locked());
        hold.reset();
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Off);
        QVERIFY(!hold.advance(9000));
        QVERIFY(hold.begin({5, 6}, 0)); // A new gesture may use a restarted clock.
        QVERIFY(!hold.advance(999));
        QVERIFY(hold.advance(1000));
        QVERIFY(hold.begin({7, 8}, 2000)); // A fresh press also clears Locked.
        QVERIFY(hold.waiting());
        QVERIFY(!hold.advance(2999));
        QVERIFY(hold.advance(3000));
    }

    void invalidCoordinatesCancel_data() {
        QTest::addColumn<QPointF>("point");
        const qreal nan = std::numeric_limits<qreal>::quiet_NaN();
        const qreal inf = std::numeric_limits<qreal>::infinity();
        QTest::newRow("nan x") << QPointF(nan, 0);
        QTest::newRow("nan y") << QPointF(0, nan);
        QTest::newRow("positive infinity") << QPointF(inf, 0);
        QTest::newRow("negative infinity") << QPointF(0, -inf);
    }
    void invalidCoordinatesCancel() {
        QFETCH(QPointF, point);
        NativeAspectHold hold;
        QVERIFY(!hold.begin(point, 0));
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(!hold.advance(1000));
        QVERIFY(hold.begin({0, 0}, 0));
        hold.move(point, 1000); // Invalid evidence cannot lock.
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(hold.begin({0, 0}, 0));
        QVERIFY(hold.advance(1000));
        hold.move(point, 1001);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
    }

    void negativeAndRegressingTimesInvalidateAttempt() {
        NativeAspectHold hold;
        QVERIFY(!hold.begin({0, 0}, -1));
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(hold.begin({0, 0}, 10));
        QVERIFY(!hold.advance(-1));
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(hold.begin({0, 0}, 10));
        hold.move({0, 0}, 9);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(hold.begin({0, 0}, 10));
        hold.move({0, 0}, 100);
        QVERIFY(!hold.advance(99));
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(!hold.advance(1010));
        QVERIFY(hold.begin({0, 0}, 10));
        QVERIFY(hold.advance(1010));
        hold.move({0, 0}, 1009);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
    }

    void largeClockValuesDoNotOverflowDeadline() {
        NativeAspectHold hold;
        const qint64 maximum = std::numeric_limits<qint64>::max();
        QVERIFY(hold.begin({0, 0}, maximum - 1000));
        QVERIFY(!hold.advance(maximum - 1));
        QVERIFY(hold.advance(maximum));
        QVERIFY(!hold.advance(maximum));
        QVERIFY(hold.begin({0, 0}, maximum - 999));
        QVERIFY(!hold.advance(maximum));
        QVERIFY(hold.waiting());
        QVERIFY(hold.begin({0, 0}, 0));
        QVERIFY(hold.advance(maximum));
        QVERIFY(hold.locked());
    }

    void extremeFiniteDistanceCannotWrapIntoJitterRadius() {
        NativeAspectHold hold;
        const qreal maximum = std::numeric_limits<qreal>::max();
        QVERIFY(hold.begin({maximum, maximum}, 0));
        hold.move({-maximum, -maximum}, 1);
        QCOMPARE(hold.phase(), NativeAspectHold::Phase::Cancelled);
        QVERIFY(!hold.advance(1000));
    }
};
QTEST_APPLESS_MAIN(NativeAspectHoldTest)
#include "NativeAspectHoldTest.moc"

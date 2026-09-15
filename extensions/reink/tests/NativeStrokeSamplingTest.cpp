#include "NativeStrokeSampling.h"
#include <QLineF>
#include <QRectF>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace RePaperNative;
namespace {
bool containsSample(const QRectF &rect, const QVector<QPointF> &points) {
    return std::any_of(points.begin(), points.end(),
                       [&rect](const QPointF &point) { return rect.contains(point); });
}
}

class NativeStrokeSamplingTest : public QObject {
    Q_OBJECT
private slots:
    void alreadySampledLargePathSharesItsImmutableStorage() {
        QVector<QPointF> path;path.reserve(NativeStrokePointBudget);
        for(qsizetype i=0;i<NativeStrokePointBudget;++i)path.append({qreal(i),0});
        const auto sampled=sampleStrokeForNativeSelection(path);
        QVERIFY(sampled.valid());QCOMPARE(sampled.points.size(),path.size());
        QCOMPARE(sampled.points.constData(),path.constData());
        auto edited=sampled.points;edited[0]={500,500};
        QCOMPARE(path.first(),QPointF(0,0)); // sharing retains copy-on-write isolation
    }
    void interpolatedPathRetainsExactFloatingPointSourceVertices() {
        const QVector<QPointF> path{{-999999.875,123.456789},{0.00000000001,-0.0},{123.00000000003,70.125}};
        const auto sampled=sampleStrokeForNativeSelection(path);
        QVERIFY(sampled.valid());
        QVERIFY(sampled.points.contains(path[1]));
        const auto middle=sampled.points.indexOf(path[1]);
        QVERIFY(middle>0);
        QVERIFY(std::signbit(sampled.points[middle].y()));
        QCOMPARE(sampled.points.first(),path.first());QCOMPARE(sampled.points.last(),path.last());
    }
    void longLineMidpointSelection_data() {
        QTest::addColumn<QPointF>("start");
        QTest::addColumn<QPointF>("end");
        QTest::newRow("horizontal") << QPointF(100, 300) << QPointF(1100, 300);
        QTest::newRow("vertical") << QPointF(400, 100) << QPointF(400, 1100);
        QTest::newRow("diagonal") << QPointF(-400, -300) << QPointF(400, 300);
        QTest::newRow("reverse-diagonal") << QPointF(400, -300) << QPointF(-400, 300);
    }
    void longLineMidpointSelection() {
        QFETCH(QPointF, start);
        QFETCH(QPointF, end);
        const QVector<QPointF> path{start, end};
        const auto midpoint = (start + end) / 2;
        const QRectF selection(midpoint - QPointF(5, 5), QSizeF(10, 10));
        QVERIFY(!containsSample(selection, path));
        const auto sampled = sampleStrokeForNativeSelection(path);
        QVERIFY(sampled.valid());
        QVERIFY(containsSample(selection, sampled.points));
        QCOMPARE(sampled.points.first(), start);
        QCOMPARE(sampled.points.last(), end);
        QCOMPARE(sampled.points.size(), qsizetype(126));
        for (qsizetype i = 1; i < sampled.points.size(); ++i) {
            QVERIFY(QLineF(sampled.points[i - 1], sampled.points[i]).length()
                    <= NativeStrokeSampleSpacing + 1e-9);
            const auto relative = sampled.points[i] - start;
            const auto segment = end - start;
            QVERIFY(std::abs(relative.x() * segment.y() - relative.y() * segment.x()) < 1e-7);
        }
        QVERIFY(!containsSample(selection.translated(2000, 2000), sampled.points));
    }
    void sourceVerticesShortSegmentsAndRepeatedPointsArePreserved() {
        const QVector<QPointF> path{{0, 0}, {8, 0}, {8, 0}, {8, 1}, {25, 1}};
        const auto sampled = sampleStrokeForNativeSelection(path);
        QVERIFY(sampled.valid());
        QCOMPARE(sampled.points.size(), qsizetype(7));
        QCOMPARE(sampled.points.mid(0, 4), path.mid(0, 4));
        QCOMPARE(sampled.points.last(), path.last());
        for (qsizetype i = 1; i < sampled.points.size(); ++i)
            QVERIFY(QLineF(sampled.points[i - 1], sampled.points[i]).length() <= 8.0);
        const QVector<QPointF> dot{{5, 5}, {5, 5}};
        QCOMPARE(sampleStrokeForNativeSelection(dot).points, dot);
    }
    void nonfiniteAndOutOfRangeCoordinatesNeverReturnPartialPaths() {
        const auto nan = std::numeric_limits<qreal>::quiet_NaN();
        const auto infinity = std::numeric_limits<qreal>::infinity();
        for (const auto invalid : {QPointF(nan, 0), QPointF(0, nan), QPointF(infinity, 0),
                                  QPointF(0, -infinity), QPointF(1000001, 0), QPointF(0, -1000001)}) {
            for (const auto path : {QVector<QPointF>{invalid, {0, 0}},
                                    QVector<QPointF>{{0, 0}, {8, 0}, invalid}}) {
                const auto sampled = sampleStrokeForNativeSelection(path);
                QVERIFY(!sampled.valid());
                QCOMPARE(sampled.error, StrokeSamplingError::InvalidCoordinates);
                QVERIFY(sampled.points.isEmpty());
            }
        }
        QVERIFY(sampleStrokeForNativeSelection({{1000000, -1000000}, {999999, -999999}}).valid());
    }
    void inputAndInterpolatedBudgetsAreBounded() {
        for (const auto path : {QVector<QPointF>{}, QVector<QPointF>{{0, 0}},
                                QVector<QPointF>(NativeStrokePointBudget + 1, {0, 0})}) {
            const auto sampled = sampleStrokeForNativeSelection(path);
            QCOMPARE(sampled.error, StrokeSamplingError::InvalidGeometryOrBudget);
            QVERIFY(sampled.points.isEmpty());
        }
        const QVector<QPointF> path{{0, 0}, {16, 0}};
        for (const auto budget : {qsizetype(-1), qsizetype(0), qsizetype(1), NativeStrokePointBudget + 1})
            QCOMPARE(sampleStrokeForNativeSelection(path, budget).error,
                     StrokeSamplingError::InvalidGeometryOrBudget);
        const auto tooSmall = sampleStrokeForNativeSelection(path, 2);
        QCOMPARE(tooSmall.error, StrokeSamplingError::PointBudgetExceeded);
        QVERIFY(tooSmall.points.isEmpty());
        const auto exact = sampleStrokeForNativeSelection(path, 3);
        QVERIFY(exact.valid());
        QCOMPARE(exact.points.size(), qsizetype(3));

        // One long segment can exhaust the batch limit even with only two
        // source vertices. Equality at the hard limit is accepted.
        const QVector<QPointF> atLimit{{-799996, 0}, {799996, 0}};
        const auto full = sampleStrokeForNativeSelection(atLimit);
        QVERIFY(full.valid());
        QCOMPARE(full.points.size(), NativeStrokePointBudget);
        const auto overLimit = sampleStrokeForNativeSelection({{-800000, 0}, {800000, 0}});
        QCOMPARE(overLimit.error, StrokeSamplingError::PointBudgetExceeded);
        QVERIFY(overLimit.points.isEmpty());
    }
    void remainingBudgetAppliesAcrossStrokes() {
        qsizetype remaining = 6;
        const QVector<QPointF> path{{0, 0}, {16, 0}};
        const auto first = sampleStrokeForNativeSelection(path, remaining);
        QVERIFY(first.valid());
        remaining -= first.points.size();
        const auto second = sampleStrokeForNativeSelection(path, remaining);
        QVERIFY(second.valid());
        remaining -= second.points.size();
        QCOMPARE(remaining, qsizetype(0));
        const auto third = sampleStrokeForNativeSelection(path, remaining);
        QVERIFY(!third.valid());
        QVERIFY(third.points.isEmpty());
    }
};
QTEST_APPLESS_MAIN(NativeStrokeSamplingTest)
#include "NativeStrokeSamplingTest.moc"

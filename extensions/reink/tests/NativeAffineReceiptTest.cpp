#include "NativeAffineReceipt.h"
#include "NativeSelectionStyle.h"
#include <QtTest>
#include <algorithm>
#include <limits>

namespace {
using Strokes = QVector<PaperDrawing::Stroke>;
PaperDrawing::Stroke line(qreal x, qreal y = 0) { return {{{x,y},{x+10,y+5}},3,Qt::black}; }
QVariantMap move(QPointF delta) { return {{"kind","move"},{"delta",delta}}; }
}
class NativeAffineReceiptTest : public QObject {
    Q_OBJECT
private slots:
    void delayedOldGeometryFailsAndNewGeometryMatchesInAnyOrder() {
        const Strokes baseline{line(0),line(30)}; NativeAffineReceipt receipt;
        QVERIFY(receipt.begin(baseline,move({40,-20}))); QVERIFY(receipt.valid()); QVERIFY(!receipt.isNoop());
        QVERIFY(!receipt.matches(baseline,2));
        Strokes observed{line(70,-20),line(40,-20)};
        observed[0].width = 99; observed[0].color = Qt::red; // style is not part of this evidence
        QVERIFY(receipt.matches(observed,2)); QVERIFY(!receipt.matches(observed,1));
        QCOMPARE(receipt.expectedBounds(),QRectF(40,-20,40,5));
    }
    void independentScaleAndQuarterTurnUseSignedPositions() {
        NativeAffineReceipt receipt; const Strokes baseline{line(10,20)};
        QVERIFY(receipt.begin(baseline,{{"kind","scale"},{"anchor",QPointF(10,20)},{"sx",2.0},{"sy",0.5}}));
        Strokes scaled{{{{10,20},{30,22.5}},3,Qt::black}};
        QVERIFY(receipt.matches(scaled,1)); QVERIFY(!receipt.matches(baseline,1));
        QVERIFY(receipt.begin(baseline,{{"kind","rotate"},{"anchor",QPointF(0,0)},{"angle",90}}));
        Strokes rotated{{{{-20,10},{-25,20}},3,Qt::black}};
        QVERIFY(receipt.matches(rotated,1));
        for(auto &point:rotated[0].points) point.setX(-point.x());
        QVERIFY(!receipt.matches(rotated,1));
    }
    void nativeWidthScalesByAreaRootWithQuarterUnitQuantization() {
        using namespace RePaperNative;
        QCOMPARE(scaledNativeStrokeWidth(3,2,8),qreal(12));
        QCOMPARE(scaledNativeStrokeWidth(3,2,0.5),qreal(3));
        QCOMPARE(scaledNativeStrokeWidth(0.25,1.5,1.5),qreal(0.5));
        const qreal below = std::nextafter(1.5f,0.0f);
        const qreal above = std::nextafter(1.5f,2.0f);
        QCOMPARE(scaledNativeStrokeWidth(0.25,below,below),qreal(0.25));
        QCOMPARE(scaledNativeStrokeWidth(0.25,above,above),qreal(0.5));
        QCOMPARE(scaledNativeStrokeWidth(0.25,0.5,0.5),qreal(0.25));
        QCOMPARE(scaledNativeStrokeWidth(0.25,0.25,0.25),qreal(0));
        QCOMPARE(scaledNativeStrokeWidth(0,5,7),qreal(0));
        NativeAffineReceipt receipt;
        QVERIFY(receipt.begin({line(0)},{{"kind","scale"},{"anchor",QPointF()},{"sx",2},{"sy",8}}));
        QCOMPARE(receipt.expected()[0].width,qreal(12));
        QVERIFY(receipt.begin({line(0)},{{"kind","rotate"},{"anchor",QPointF()},{"angle",37}}));
        QCOMPARE(receipt.expected()[0].width,qreal(3));
        QVERIFY(receipt.begin({line(0)},move({10,0})));
        QCOMPARE(receipt.expected()[0].width,qreal(3));
    }
    void nativeWidthRejectsUint16OverflowAndInvalidFactors() {
        using namespace RePaperNative;
        QVERIFY(nativeScaleWidthFits(65535,1,1));
        QCOMPARE(scaledNativeStrokeWidth(65535.0/4,1,1),qreal(65535.0/4));
        const qreal boundary = 65535.5/65535.0;
        QVERIFY(!nativeScaleWidthFits(65535,boundary,boundary));
        QVERIFY(std::isnan(scaledNativeStrokeWidth(65535.0/4,boundary,boundary)));
        QVERIFY(nativeScaleWidthFits(65535,std::nextafter(float(boundary),0.0f),
                                      std::nextafter(float(boundary),0.0f)));
        QVERIFY(!nativeScaleWidthFits(16,10000,10000));
        QVERIFY(!nativeScaleWidthFits(12,-1,1));
        QVERIFY(!nativeScaleWidthFits(12,0,1));
        QVERIFY(!nativeScaleWidthFits(12,std::numeric_limits<qreal>::infinity(),1));
        QVERIFY(std::isnan(scaledNativeStrokeWidth(-1,1,1)));
        QVERIFY(std::isnan(scaledNativeStrokeWidth(16384,1,1)));
        QVERIFY(std::isnan(scaledNativeStrokeWidth(0.3,1,1)));
        QVERIFY(std::isnan(scaledNativeStrokeWidth(std::numeric_limits<qreal>::quiet_NaN(),1,1)));
        NativeAffineReceipt receipt;
        QVERIFY(!receipt.begin({line(0)},{{"kind","scale"},{"anchor",QPointF()},{"sx",10000},{"sy",10000}}));
        QVERIFY(!receipt.valid());QVERIFY(receipt.expected().isEmpty());
    }
    void duplicateExpectsOnlyNewOffsetSelectionAndRemoveExpectsEmpty() {
        NativeAffineReceipt receipt; const Strokes baseline{line(0)};
        QVERIFY(receipt.begin(baseline,{{"kind","duplicate"}}));
        QCOMPARE(receipt.kind(),QString("duplicate")); QVERIFY(!receipt.isNoop());
        QVERIFY(receipt.matches({line(24,24)},1)); QVERIFY(!receipt.matches({line(0),line(24,24)},2));
        QVERIFY(receipt.begin(baseline,{{"kind","remove"}}));
        QCOMPARE(receipt.expectedSelectionCount(),0); QVERIFY(receipt.matches({},0));
        QVERIFY(!receipt.matches({},1)); QVERIFY(!receipt.matches(baseline,1));
    }
    void arbitraryRotationMatchesGeometryAroundItsAnchor() {
        NativeAffineReceipt receipt;const Strokes baseline{line(10,20)};
        const QPointF anchor(3,-8);
        for(const qreal angle:{-179.5,-45.0,22.5,135.0,359.0}){
            QVERIFY(receipt.begin(baseline,{{"kind","rotate"},{"anchor",anchor},{"angle",angle}}));
            QTransform transform;transform.translate(anchor.x(),anchor.y());transform.rotate(angle);transform.translate(-anchor.x(),-anchor.y());
            auto expected=baseline;for(auto &point:expected[0].points)point=transform.map(point);
            QVERIFY(receipt.matches(expected,1));QVERIFY(!receipt.matches(baseline,1));
        }
    }
    void ambiguousCandidatesRequireBijectionAndPreserveMultiplicity() {
        NativeAffineReceipt receipt;
        // Expected x:0,0.2. First path matches both observed candidates; second
        // matches only 0.1. A greedy matcher would consume the wrong candidate.
        QVERIFY(receipt.begin({line(-10),line(-9.8)},move({10,0})));
        QVERIFY(receipt.matches({line(0.1),line(-0.1)},2));
        QVERIFY(receipt.begin({line(0),line(0),line(20)},move({10,0})));
        QVERIFY(receipt.matches({line(30),line(10),line(10)},3));
        QVERIFY(!receipt.matches({line(30),line(30),line(10)},3));
    }
    void toleranceDoesNotAdmitDifferentPathsOrOldSubToleranceMoves() {
        NativeAffineReceipt receipt;
        QVERIFY(receipt.begin({line(0)},move({0.125,0}))); QVERIFY(receipt.isNoop());
        QVERIFY(receipt.begin({line(0)},move({0.126,0}))); QVERIFY(!receipt.isNoop());
        QVERIFY(!receipt.matches({line(0)},1));
        QVERIFY(receipt.begin({line(0)},move({10,0})));
        auto expected=receipt.expected(); expected[0].points[0].setX(10.125); QVERIFY(receipt.matches(expected,1));
        expected[0].points[0].setX(10.126); QVERIFY(!receipt.matches(expected,1));
        expected=receipt.expected(); std::reverse(expected[0].points.begin(),expected[0].points.end());
        QVERIFY(!receipt.matches(expected,1));
    }
    void invalidInputsClearPreviousReceiptAndEnforceBudgets() {
        NativeAffineReceipt receipt; QVERIFY(receipt.begin({line(0)},move({10,0})));
        QVERIFY(!receipt.begin({},move({1,0}))); QVERIFY(!receipt.valid()); QVERIFY(!receipt.error().isEmpty());
        QVERIFY(!receipt.matches({},0)); QVERIFY(!receipt.begin({line(0)},{{"kind","move"}}));
        QVERIFY(!receipt.begin({line(0)},move({std::numeric_limits<qreal>::infinity(),0})));
        QVERIFY(!receipt.begin({line(999990)},move({20,0})));
        QVERIFY(!receipt.begin(Strokes(129,line(0)),move({1,0})));
        auto longLine=line(0); longLine.points.fill({1,1},200001);
        QVERIFY(!receipt.begin({longLine},move({1,0})));
        longLine.points.resize(200000); QVERIFY(receipt.begin({longLine},move({1,0})));
        auto invalid=receipt.expected(); invalid[0].points[10].setY(std::numeric_limits<qreal>::quiet_NaN());
        QVERIFY(!receipt.matches(invalid,1));
        QVERIFY(!receipt.begin({line(0)},{{"kind","scale"},{"anchor",QPointF()},{"sx",-1},{"sy",1}}));
        QVERIFY(!receipt.begin({line(0)},{{"kind","rotate"},{"anchor",QPointF()},{"angle",361}}));
        QVERIFY(!receipt.begin({line(0)},{{"kind","unknown"}}));
    }
    void noopsAndDegenerateBoundsRemainExplicit() {
        NativeAffineReceipt receipt; const Strokes baseline{line(0)};
        QVERIFY(receipt.begin(baseline,move({0,0}))); QVERIFY(receipt.isNoop());
        QVERIFY(receipt.begin(baseline,{{"kind","scale"},{"anchor",QPointF(30,50)},{"sx",1},{"sy",1}}));
        QVERIFY(receipt.isNoop()); receipt.clear(); QVERIFY(!receipt.valid()); QVERIFY(receipt.kind().isEmpty());
        Strokes axes{{{{0,0},{10,0}},3,Qt::black}, {{{30,20},{30,40}},3,Qt::black}};
        QVERIFY(receipt.begin(axes,move({10,10})));
        QCOMPARE(receipt.expectedBounds(),QRectF(10,10,30,40));
    }
};
QTEST_MAIN(NativeAffineReceiptTest)
#include "NativeAffineReceiptTest.moc"

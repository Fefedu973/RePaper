#include "NativeArrowStroke.h"
#include "NativeGesture.h"
#include "NativeStrokeSampling.h"
#include "DrawingModel.h"
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QtTest>
#include <algorithm>

using namespace repaper::drawing;
using namespace RePaperNative;
namespace {
using Edge = QPair<QPointF, QPointF>;
QVector<Edge> edges(const QVector<Stroke> &strokes) {
    QVector<Edge> result;
    for (const auto &stroke : strokes)
        for (qsizetype i = 1; i < stroke.points.size(); ++i)
            result.append({stroke.points[i - 1], stroke.points[i]});
    return result;
}
bool sameEdge(const Edge &a, const Edge &b) {
    return a == b || (a.first == b.second && a.second == b.first);
}
bool sameVisibleEdges(const QVector<Stroke> &a, const QVector<Stroke> &b) {
    const auto first = edges(a), second = edges(b);
    const auto covered = [](const QVector<Edge> &source, const QVector<Edge> &target) {
        return std::all_of(source.cbegin(), source.cend(), [&](const auto &edge) {
            return std::any_of(target.cbegin(), target.cend(), [&](const auto &other) {
                return sameEdge(edge, other);
            });
        });
    };
    return covered(first, second) && covered(second, first);
}
bool identical(const QVector<Stroke> &a, const QVector<Stroke> &b) {
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i)
        if (a[i].points != b[i].points || a[i].width != b[i].width || a[i].color != b[i].color)
            return false;
    return true;
}
Item arrow(const QString &direction = "end", const QString &style = "solid") {
    Item item;
    item.kind = "arrow"; item.style = style; item.arrowDirection = direction;
    item.sourcePoints = {{100, 100}, {600, 400}};
    item.width = 3; item.headSize = 26;
    if (!rebuild(item)) return {};
    return item;
}
QImage render(const QVector<Stroke> &strokes) {
    QImage image(750, 550, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    for (const auto &stroke : strokes) {
        painter.setPen(QPen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(stroke.points.constData(), stroke.points.size());
    }
    return image;
}
bool sameRasterWithinOnePixel(const QImage &a, const QImage &b) {
    if (a.size() != b.size()) return false;
    const auto covered = [](const QImage &source, const QImage &target) {
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
            if (source.pixel(x, y) == target.pixel(x, y)) continue;
            bool nearby = false;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                if (target.rect().contains(x + dx, y + dy)
                    && source.pixel(x, y) == target.pixel(x + dx, y + dy)) nearby = true;
            if (!nearby) return false;
        }
        return true;
    };
    return covered(a, b) && covered(b, a);
}
QVector<Stroke> mapped(QVector<Stroke> strokes, const QTransform &transform) {
    for (auto &stroke : strokes) for (auto &point : stroke.points) point = transform.map(point);
    return strokes;
}
}

class NativeArrowStrokeTest : public QObject {
    Q_OBJECT
private slots:
    void completeArrowUsesOnlyOriginalEdges_data() {
        QTest::addColumn<QString>("direction");
        QTest::addColumn<QPointF>("end");
        QTest::addColumn<QColor>("color");
        for (const auto &direction : {QString("end"), QString("start"), QString("both")})
            for (const auto &end : {QPointF(600, 100), QPointF(100, 450), QPointF(600, 400), QPointF(30, 30)})
                for (const auto &color : {QColor(Qt::black), QColor(207, 72, 23)})
                    QTest::newRow(qPrintable(QString("%1-%2,%3-%4").arg(direction).arg(end.x()).arg(end.y()).arg(color.name())))
                        << direction << end << color;
    }
    void completeArrowUsesOnlyOriginalEdges() {
        QFETCH(QString, direction); QFETCH(QPointF, end); QFETCH(QColor, color);
        auto item = arrow(direction);
        item.sourcePoints[1] = end;
        QVERIFY(rebuild(item));
        for (auto &stroke : item.strokes) stroke.color = color;
        const auto joined = wholeNativeArrowStrokes(item);
        QCOMPARE(joined.size(), qsizetype(1));
        QCOMPARE(joined.first().points.size(), direction == "both" ? qsizetype(8) : qsizetype(5));
        QCOMPARE(joined.first().color, color);
        QCOMPARE(joined.first().width, item.width);
        QVERIFY(sameVisibleEdges(item.strokes, joined));
        QCOMPARE(PaperDrawing::bounds(joined), PaperDrawing::bounds(item.strokes));
        // Qt rounds a cap and a 180-degree retrace join slightly differently:
        // the diagonal fixture differs by one boundary pixel per retraced tip.
        // Exact edge equality above governs geometry; this bounds raster drift.
        QVERIFY(sameRasterWithinOnePixel(render(item.strokes), render(joined)));
        const auto sampled = sampleStrokeForNativeSelection(joined.first().points);
        QVERIFY(sampled.valid());
        // Selection hits any sampled branch but operates on this ONE line;
        // all head and shaft points therefore share its native affine edit.
        for (const auto &stroke : item.strokes)
            for (const auto &point : stroke.points) QVERIFY(sampled.points.contains(point));
        QTransform transform;
        transform.translate(140, -20); transform.rotate(53); transform.scale(1.8, .7);
        QVERIFY(sameVisibleEdges(mapped(item.strokes, transform), mapped(joined, transform)));
    }
    void disconnectedStylesAndOtherObjectsPassThrough() {
        for (const auto &style : {QString("dashed"), QString("dotted")}) {
            for (const auto &direction : {QString("end"), QString("start"), QString("both")}) {
                const auto item = arrow(direction, style);
                QVERIFY(identical(item.strokes, wholeNativeArrowStrokes(item)));
            }
        }
        auto item = arrow();
        for (const auto &kind : {QString("line"), QString("wire"), QString("symbol"), QString("legacy")}) {
            item.kind = kind;
            QVERIFY(identical(item.strokes, wholeNativeArrowStrokes(item)));
        }
    }
    void nativeGesturePreviewsAndCommitsTheSameWholeArrow() {
        for (const auto &direction : {QString("end"), QString("start"), QString("both")}) {
            NativeGesture gesture;
            gesture.tool = "arrow"; gesture.arrowDirection = direction;
            gesture.color = QColor(30, 90, 177); gesture.width = 2;
            QVERIFY(gesture.begin({100, 100}));
            QVERIFY(gesture.move({600, 400}));
            QCOMPARE(gesture.preview().size(), qsizetype(1));
            const auto preview = gesture.preview();
            Item committed;
            QVERIFY(gesture.finish({600, 400}, &committed));
            QVERIFY(identical(preview, committed.strokes));
            QCOMPARE(committed.strokes.first().color, gesture.color);
            QCOMPARE(committed.strokes.first().width, gesture.width);
        }
    }
    void malformedTopologyAndMixedAppearancePassThrough() {
        const auto original = arrow("both");
        QVector<Item> cases;
        auto item = original; item.strokes.append(item.strokes.first()); cases.append(item);
        item = original; item.strokes[1].points[1] += QPointF(.01, 0); cases.append(item);
        item = original; item.strokes[2].points.removeLast(); cases.append(item);
        item = original; item.strokes[0].points.append(QPointF(3, 4)); cases.append(item);
        item = original; item.strokes[1].color = Qt::blue; cases.append(item);
        item = original; item.strokes[2].width += .1; cases.append(item);
        item = original; item.arrowDirection = "unknown"; cases.append(item);
        item = original; item.sourcePoints.clear(); cases.append(item);
        item = original; for (auto &stroke : item.strokes) stroke.color.setAlpha(127); cases.append(item);
        for (const auto &candidate : cases)
            QVERIFY(identical(candidate.strokes, wholeNativeArrowStrokes(candidate)));
    }
};
QTEST_APPLESS_MAIN(NativeArrowStrokeTest)
#include "NativeArrowStrokeTest.moc"

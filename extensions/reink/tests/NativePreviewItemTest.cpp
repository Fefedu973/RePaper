#include "NativePreviewItem.h"
#include <QImage>
#include <QPainter>
#include <QSignalSpy>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QtTest>
#include <limits>

namespace {
QVariantList segment(qreal y, qreal width = 4) {
    return {QVariantMap{{"width", width}, {"points", QVariantList{
        QVariantMap{{"x", 20.0}, {"y", y}}, QVariantMap{{"x", 180.0}, {"y", y}}}}}};
}
QImage render(NativePreviewItem &item) {
    QImage image(220, 220, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
    QPainter painter(&image); painter.translate(item.position()); item.paint(&painter); painter.end(); return image;
}
class MeasuredPreview : public NativePreviewItem {
public:
    using NativePreviewItem::NativePreviewItem;
    int paints = 0;
    QSize rasterSize;
    void paint(QPainter *painter) override {
        ++paints;
        rasterSize = {painter->device()->width(),painter->device()->height()};
        NativePreviewItem::paint(painter);
    }
};
}

class NativePreviewItemTest : public QObject {
    Q_OBJECT
private slots:
    void typedQmlFrameKeepsSharedGeometryAndMinimalNativeFootprint() {
        using namespace RePaperNative;
        qmlRegisterType<NativePreviewItem>("RePaper.TypedPreview",1,0,"NativePreviewItem");
        QQmlEngine engine; QQmlComponent component(&engine);
        component.setData("import QtQuick 2.15\nimport RePaper.TypedPreview 1.0\n"
            "NativePreviewItem { viewportRect: Qt.rect(0,0,220,220); property var sourceFrame; previewFrame: sourceFrame }",{});
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object,qPrintable(component.errorString()));
        auto *item=qobject_cast<NativePreviewItem *>(object.get());QVERIFY(item);
        QVERIFY(!item->flags().testFlag(QQuickItem::ItemHasContents));
        const auto frame=PreviewFrame::fromStrokes({{{{20,100},{180,100}},4,QColor("#136aca")}});
        QVERIFY(object->setProperty("sourceFrame",QVariant::fromValue(frame)));
        QVERIFY(item->hasStrokes());
        QCOMPARE(item->contentBounds(),QRectF(16,96,168,8));
        QCOMPARE(item->position(),QPointF(16,96));
        QCOMPARE(item->size(),QSizeF(168,8));
        QVERIFY(item->flags().testFlag(QQuickItem::ItemHasContents));
        const auto bridged=item->previewFrame().value<PreviewFrame>();
        QVERIFY(bridged==frame);
        QCOMPARE(bridged.strokes().first().points.constData(),frame.strokes().first().points.constData());
        QCOMPARE(render(*item).pixelColor(100,100),QColor("#136aca"));
    }
    void realQuickBackingImageFollowsInkAndIdleItemDoesNoPaintWork() {
        using namespace RePaperNative;
        QQuickWindow window;window.resize(1404,1872);window.setColor(Qt::white);
        MeasuredPreview item(window.contentItem());item.setViewportRect({0,0,1404,1872});
        const auto frame=PreviewFrame::fromStrokes({{{{20,100},{180,100}},4,Qt::black}});
        item.setPreviewFrame(QVariant::fromValue(frame));
        window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(item.paints>0);
        // This measures Qt's actual QQuickPaintedItem paint device, not just
        // the requested dirty rectangle on an otherwise full-page texture.
        QCOMPARE(item.rasterSize,QSize(168,8));
        QVERIFY(qint64(item.rasterSize.width())*item.rasterSize.height()<1404LL*1872/100);
        const int beforeMove=item.paints;
        item.setPreviewFrame(QVariant::fromValue(PreviewFrame::fromStrokes({{{{20,170},{180,170}},4,Qt::red}})));
        QTRY_VERIFY(item.paints>beforeMove);
        QCOMPARE(item.position(),QPointF(16,166));
        QCOMPARE(item.rasterSize,QSize(168,8));
        item.setPreviewFrame(QVariant::fromValue(PreviewFrame{}));
        const int paintsAfterClear=item.paints;
        QSignalSpy dirty(&item,&NativePreviewItem::frameRequested);
        for(int i=0;i<1000;++i){
            item.setViewportRect({0,0,qreal(1404+i%2),1872});
            item.setPreviewFrame(QVariant::fromValue(PreviewFrame{}));
        }
        QTest::qWait(40);
        QCOMPARE(item.paints,paintsAfterClear);QCOMPARE(dirty.size(),0);
        QVERIFY(!item.hasStrokes());QVERIFY(item.size().isEmpty());
        QVERIFY(!item.flags().testFlag(QQuickItem::ItemHasContents));
    }
    void repeatedTypedPacketHasConstantWorkAndCancellationClearsLatest() {
        using namespace RePaperNative;
        QVector<QPointF> points;points.reserve(200000);
        for(int i=0;i<200000;++i)points.append({qreal(i%200),qreal(i/200)});
        const auto frame=PreviewFrame::fromStrokes({{std::move(points),4,Qt::black}});
        NativePreviewItem item;item.setViewportRect({0,0,1404,1872});
        QSignalSpy dirty(&item,&NativePreviewItem::frameRequested);
        QSignalSpy bounds(&item,&NativePreviewItem::contentBoundsChanged);
        QSignalSpy changed(&item,&NativePreviewItem::strokesChanged);
        const QVariant packet=QVariant::fromValue(frame);
        item.setPreviewFrame(packet);
        for(int i=0;i<10000;++i)item.setPreviewFrame(packet);
        QCOMPARE(dirty.size(),1);QCOMPARE(bounds.size(),1);QCOMPARE(changed.size(),1);
        QVERIFY(item.previewFrame().value<PreviewFrame>()==frame);
        item.setPreviewFrame(QVariant::fromValue(PreviewFrame::fromStrokes({{{{20,100},{180,100}},4,Qt::red}})));
        item.setPreviewFrame(QVariant::fromValue(PreviewFrame{}));
        QCOMPARE(dirty.size(),2);QVERIFY(!item.hasStrokes());
        QTest::qWait(40);QCOMPARE(dirty.size(),2);
    }
    void boundedRasterMatchesWholeViewportColorsDashSegmentsAndRoundJoins() {
        using namespace RePaperNative;
        const QVector<PreviewStroke> strokes{
            {{{40,40},{40,180},{180,180}},24,Qt::white},
            {{{-40,30},{90,30},{90,90}},9,QColor("#136aca")},
            {{{100,90},{130,90}},3,QColor("#d90707")},
            {{{150,90},{180,90}},3,QColor("#d90707")},
            {{{100,140},{180,140},{150,120},{180,140},{150,160}},7,Qt::black},
            {{{55,170},{70,195},{45,195},{55,170}},2,Qt::black}};
        NativePreviewItem item;item.setViewportRect({0,0,220,220});
        item.setPreviewFrame(QVariant::fromValue(PreviewFrame::fromStrokes(strokes)));
        QImage expected(220,220,QImage::Format_ARGB32_Premultiplied);expected.fill(Qt::transparent);
        QPainter painter(&expected);painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRect(QRect(0,0,220,220));painter.setBrush(Qt::NoBrush);
        for(const auto &stroke:strokes){
            painter.setPen(QPen(stroke.color,stroke.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
            painter.drawPolyline(stroke.points.constData(),int(stroke.points.size()));
        }
        painter.end();QCOMPARE(render(item),expected);
    }
    void qmlBindingPreservesColorWithBlackFallback_data() {
        QTest::addColumn<QVariant>("sourceColor");
        QTest::addColumn<QColor>("expectedColor");
        QTest::newRow("qcolor") << QVariant(QColor("#a42b6e")) << QColor("#a42b6e");
        // NativeScene::previewStrokes() crosses the QML binding with HexArgb
        // QString values, not QColor variants. Exercise that production format.
        QTest::newRow("native-argb-red") << QVariant(QStringLiteral("#ffd90707")) << QColor("#d90707");
        QTest::newRow("native-argb-blue") << QVariant(QStringLiteral("#ff0062cc")) << QColor("#0062cc");
    }
    void qmlBindingPreservesColorWithBlackFallback() {
        QFETCH(QVariant, sourceColor);
        QFETCH(QColor, expectedColor);
        qmlRegisterType<NativePreviewItem>("RePaper.TestPreview", 1, 0, "NativePreviewItem");
        QQmlEngine engine; QQmlComponent component(&engine);
        component.setData("import QtQuick 2.15\nimport RePaper.TestPreview 1.0\n"
                          "NativePreviewItem { viewportRect: Qt.rect(0, 0, 220, 220); property var sourceStrokes: []; strokes: sourceStrokes }", {});
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *item = qobject_cast<NativePreviewItem *>(object.get()); QVERIFY(item);
        QSignalSpy frames(item, &NativePreviewItem::frameRequested);
        auto map = segment(100).first().toMap(); map.insert("color", sourceColor);
        QVERIFY(object->setProperty("sourceStrokes", QVariantList{map}));
        QTRY_COMPARE(frames.size(), 1);
        QCOMPARE(render(*item).pixelColor(100, 100), expectedColor);
        map.insert("color", "not-a-color");
        QVERIFY(object->setProperty("sourceStrokes", QVariantList{map}));
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(render(*item).pixelColor(100, 100), QColor(Qt::black));
    }
    void firstFrameAndNextIdleUpdateAreImmediate() {
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        item.setStrokes(segment(40));
        QCOMPARE(frames.size(), 1); // no event-loop turn or timer wait
        QVERIFY(qAlpha(render(item).pixel(100, 40)) > 240);
        QVERIFY(!frames.wait(80)); // an idle cooldown must not repaint
        item.setStrokes(segment(170));
        QCOMPARE(frames.size(), 2);
        const auto image = render(item);
        QVERIFY(qAlpha(image.pixel(100, 170)) > 240);
        QCOMPARE(qAlpha(image.pixel(100, 40)), 0);
    }
    void burstCoalescesAndRendersFirstAndLatestSamples() {
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        for (int i = 0; i < 1000; ++i) item.setStrokes(segment(20 + i % 100));
        QCOMPARE(frames.size(), 1);
        QVERIFY(qAlpha(render(item).pixel(100, 20)) > 240);
        QTRY_COMPARE(frames.size(), 2);
        const auto image = render(item);
        QVERIFY(qAlpha(image.pixel(100, 119)) > 240);
        QCOMPARE(qAlpha(image.pixel(100, 20)), 0);
        QVERIFY(!frames.wait(80)); QCOMPARE(frames.size(), 2);
    }
    void fullNativeSelectionLeavesRoomForItsOverlay() {
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        QVariantList strokes(128, segment(50).first());
        strokes.append(segment(170).first()); // selection border is a separate stroke
        item.setStrokes(strokes); QTRY_COMPARE(frames.size(), 1);
        QVERIFY(item.hasStrokes());
        const auto image = render(item);
        QVERIFY(qAlpha(image.pixel(100, 50)) > 240);
        QVERIFY(qAlpha(image.pixel(100, 170)) > 240);
    }
    void movingAndClearingInvalidatesOldAndNewInk() {
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        item.setStrokes(segment(40)); QTRY_COMPARE(frames.size(), 1);
        item.setStrokes(segment(170)); QTRY_COMPARE(frames.size(), 2);
        const auto dirty = frames.last().at(0).toRect();
        QVERIFY(dirty.contains(QPoint(20, 36))); QVERIFY(dirty.contains(QPoint(180, 173)));
        QCOMPARE(dirty.intersected(QRect(0, 0, 220, 220)), dirty);
        item.setStrokes({}); QCOMPARE(frames.size(), 3); QVERIFY(!item.hasStrokes());
        QCOMPARE(qAlpha(render(item).pixel(100, 170)), 0);
    }
    void wideNativeInkKeepsBorderAndClipsToViewport_data() {
        QTest::addColumn<qreal>("width");
        QTest::newRow("scaled-300-pixels") << qreal(300);
        QTest::newRow("maximum-finite-width") << qreal(1000000);
    }
    void wideNativeInkKeepsBorderAndClipsToViewport() {
        QFETCH(qreal, width);
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        auto border=segment(170,4).first().toMap();border.insert("color",QColor(Qt::red));
        item.setStrokes({segment(100,width).first(),border});
        QTRY_COMPARE(frames.size(),1);
        QCOMPARE(frames.first().at(0).toRect(),QRect(0,0,220,220));
        QImage image(300,300,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
        QPainter painter(&image);painter.translate(item.position());item.paint(&painter);painter.end();
        QCOMPARE(image.pixelColor(100,20),QColor(Qt::black));
        QCOMPARE(image.pixelColor(100,170),QColor(Qt::red));
        QCOMPARE(qAlpha(image.pixel(240,100)),0);
        QCOMPARE(qAlpha(image.pixel(100,240)),0);
    }
    void zeroWidthClearsOnlyItsInkAndPreservesTheBorder() {
        NativePreviewItem item;item.setViewportRect({0,0,220,220});
        QSignalSpy frames(&item,&NativePreviewItem::frameRequested);
        auto border=segment(170,4).first().toMap();border.insert("color",QColor(Qt::blue));
        item.setStrokes({segment(100,4).first(),border});QTRY_COMPARE(frames.size(),1);
        QCOMPARE(render(item).pixelColor(100,100),QColor(Qt::black));
        item.setStrokes({segment(100,0).first(),border});QTRY_COMPARE(frames.size(),2);
        const auto image=render(item);
        QCOMPARE(qAlpha(image.pixel(100,100)),0);
        QCOMPARE(image.pixelColor(100,170),QColor(Qt::blue));
        QVERIFY(frames.last().at(0).toRect().contains(QPoint(100,100)));
    }
    void invalidWidthsStillRejectTheWholeFrame_data() {
        QTest::addColumn<qreal>("width");
        QTest::newRow("beyond-coordinate-budget") << qreal(1000001);
        QTest::newRow("negative") << qreal(-1);
        QTest::newRow("infinite") << std::numeric_limits<qreal>::infinity();
        QTest::newRow("not-a-number") << std::numeric_limits<qreal>::quiet_NaN();
    }
    void invalidWidthsStillRejectTheWholeFrame() {
        QFETCH(qreal,width);
        NativePreviewItem item;item.setViewportRect({0,0,220,220});
        QSignalSpy frames(&item,&NativePreviewItem::frameRequested);
        item.setStrokes(segment(50));QTRY_COMPARE(frames.size(),1);
        item.setStrokes({segment(100,width).first(),segment(170).first()});QTRY_COMPARE(frames.size(),2);
        const auto image=render(item);
        QCOMPARE(qAlpha(image.pixel(100,50)),0);
        QCOMPARE(qAlpha(image.pixel(100,100)),0);
        QCOMPARE(qAlpha(image.pixel(100,170)),0);
    }
    void cancelPendingFrameDoesNotResurrectInk() {
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        item.setStrokes(segment(50));
        item.setStrokes(segment(170)); // the newest frame is still queued
        QCOMPARE(frames.size(), 1);
        item.setStrokes({});
        QCOMPARE(frames.size(), 2);
        QVERIFY(!frames.wait(80)); QCOMPARE(frames.size(), 2);
        QCOMPARE(qAlpha(render(item).pixel(100, 50)), 0);
        QCOMPARE(qAlpha(render(item).pixel(100, 170)), 0);
        // A new gesture after cancellation must not inherit the old cooldown.
        item.setStrokes(segment(100)); QCOMPARE(frames.size(), 3);
        QVERIFY(qAlpha(render(item).pixel(100, 100)) > 240);
    }
    void malformedFrameClearsOldInkAndBoundsStayFinite() {
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        item.setStrokes(segment(50)); QTRY_COMPARE(frames.size(), 1);
        item.setStrokes(segment(std::numeric_limits<qreal>::quiet_NaN()));
        QTRY_COMPARE(frames.size(), 2);
        QCOMPARE(qAlpha(render(item).pixel(100, 50)), 0);
        item.setStrokes(QVariantList(145, segment(80).first()));
        QVERIFY(!item.hasStrokes());
    }
    void paintingHonorsCallerClipAndRestoresPainter() {
        NativePreviewItem item; item.setViewportRect({0, 0, 220, 220});
        QSignalSpy frames(&item, &NativePreviewItem::frameRequested);
        item.setStrokes(segment(100)); QTRY_COMPARE(frames.size(), 1);
        QImage image(220, 220, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QPainter painter(&image); painter.setClipRect(QRect(50, 90, 40, 20));
        painter.setPen(Qt::red); const auto before = painter.pen(); painter.translate(item.position()); item.paint(&painter);
        QCOMPARE(painter.pen(), before); painter.end();
        QVERIFY(qAlpha(image.pixel(60, 100)) > 240); QCOMPARE(qAlpha(image.pixel(120, 100)), 0);
    }
};
QTEST_MAIN(NativePreviewItemTest)
#include "NativePreviewItemTest.moc"

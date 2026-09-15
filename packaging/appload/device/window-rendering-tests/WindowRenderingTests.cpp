#include "FBController.h"
#include "fbmanagement.h"
#include <QCryptographicHash>
#include <QLibrary>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QtTest>

static std::map<int, QPointer<FBController>> controllers;
namespace qtfb::management {
void registerController(FBKey key, QPointer<FBController> value) { controllers[key] = value; }
void unregisterController(FBKey key) { controllers.erase(key); }
bool isControllerAssociated(FBKey key) { return controllers.count(key) && controllers[key]; }
void forwardUserInput(FBKey, const UserInputContents &) {}
void sendDeviceStateChange(FBKey, const DeviceStateChangedContents &) {}
void start() {}
}

// Diagnostic only: resolve the SDK's actual e-paper painter node without
// creating its screen/render context or opening a framebuffer. The driver pins
// the library hash. EPNode::from is exported; the verified AArch64 vtable has
// destructor, deleting destructor, draw(QPainter*) in that order.
class EpaperNode {
public:
    explicit EpaperNode(QQuickPaintedItem *item) {
        auto create = reinterpret_cast<QSGGeometryNode *(*)(void *, QQuickPaintedItem *)>(
            library().resolve("_ZN9EPContext17createPainterNodeEP17QQuickPaintedItem"));
        auto from = reinterpret_cast<void *(*)(QSGGeometryNode *)>(
            library().resolve("_ZN6EPNode4fromEP15QSGGeometryNode"));
        if (!create || !from) qFatal("SDK e-paper node symbols unavailable");
        node = create(nullptr, item); // This SDK factory does not dereference its context.
        epNode = from(node);
        if (!node || !epNode) qFatal("SDK e-paper painter node unavailable");
    }
    ~EpaperNode() { delete node; }
    void paint(QPainter *painter) {
        auto vtable = *reinterpret_cast<void ***>(epNode);
        reinterpret_cast<void (*)(void *, QPainter *)>(vtable[2])(epNode, painter);
    }
    static QLibrary &library() {
        static QLibrary value(qEnvironmentVariable("REPAPER_EPAPER_PLUGIN"));
        return value;
    }
private:
    QSGGeometryNode *node = nullptr;
    void *epNode = nullptr;
};

class SpyItem : public QQuickPaintedItem {
public:
    QTransform received;
    int calls = 0;
    void paint(QPainter *painter) override {
        received = painter->worldTransform();
        ++calls;
        painter->fillRect(QRect(0, 0, 40, 30), Qt::blue);
    }
};

class Fixture {
public:
    QImage image{360, 480, QImage::Format_RGB32};
    FBController controller;
    Fixture(int rotation = 0) {
        controller.setWidth(180);
        controller.setHeight(240);
        controller.setProperty("allowScaling", true);
        controller.setProperty("fillMode", FBController::PreserveAspectFit);
        controller.setFramebufferID(4567);
        controller.setFbRotation(static_cast<FBController::Rotation>(rotation));
        QPainter painter(&image);
        painter.fillRect(image.rect(), Qt::white);
        painter.fillRect(QRect(0, 0, 180, 240), QColor(40, 60, 80));
        painter.fillRect(QRect(180, 0, 180, 240), QColor(140, 160, 180));
        painter.fillRect(QRect(0, 240, 180, 240), QColor(80, 100, 120));
        painter.fillRect(QRect(180, 240, 180, 240), QColor(180, 200, 220));
        painter.setPen(Qt::black);
        painter.drawText(QRect(25, 30, 300, 50), Qt::AlignCenter, "reCalc 1 + 2 = 3");
        painter.end();
        controller.associateSHM(&image);
        QCoreApplication::processEvents();
    }
    QImage localImage() {
        QImage result(180, 240, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        controller.paint(&painter);
        return result;
    }
};

static QImage canvas() {
    QImage result(1200, 1200, QImage::Format_RGB32);
    result.fill(QColor(255, 0, 255));
    return result;
}

static bool sameComposition(const QImage &actual, const QImage &reference, QString *reason) {
    // The e-paper backend samples the full framebuffer directly. The reference
    // first renders an item texture, so nearest-neighbor sampling can differ at
    // an edge by a pixel. Compare all flat regions away from those edges; this
    // still covers the window silhouette, all four asymmetric blocks, margins,
    // clipping and the entire background without making font rasterization an
    // accidental part of the geometry contract.
    int checked = 0;
    for (int y = 2; y < reference.height() - 2; y += 7) {
        const QRgb *rows[5];
        for (int dy = -2; dy <= 2; ++dy)
            rows[dy + 2] = reinterpret_cast<const QRgb *>(reference.constScanLine(y + dy));
        const auto *actualRow = reinterpret_cast<const QRgb *>(actual.constScanLine(y));
        for (int x = 2; x < reference.width() - 2; x += 7) {
            const QRgb expected = rows[2][x];
            bool flat = true;
            for (int dy = -2; dy <= 2 && flat; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if (rows[dy + 2][x + dx] != expected) { flat = false; break; }
            if (!flat) continue;
            ++checked;
            if (actualRow[x] != expected) {
                *reason = QString("Scene pixel (%1,%2): actual %3, expected %4")
                    .arg(x).arg(y).arg(actualRow[x], 0, 16).arg(expected, 0, 16);
                return false;
            }
        }
    }
    *reason = QString("Too few flat scene pixels checked: %1").arg(checked);
    return checked > 20000;
}

class RenderingTests : public QObject {
    Q_OBJECT
private slots:
    void nativeBackendPreservesIncomingTransform() {
        QVERIFY2(EpaperNode::library().load(), qPrintable(EpaperNode::library().errorString()));
        SpyItem item;
        item.setWidth(100); item.setHeight(80);
        EpaperNode node(&item);
        QImage output = canvas();
        QPainter painter(&output);
        QTransform scene;
        scene.translate(620, 410).rotate(90).scale(2, 2);
        painter.setWorldTransform(scene);
        node.paint(&painter);
        QCOMPARE(item.calls, 1);
        QCOMPARE(item.received, scene);
        QCOMPARE(painter.worldTransform(), scene);
        QCOMPARE(output.pixelColor(scene.map(QPoint(10, 10))), QColor(Qt::blue));
    }
    void composedScene_data() {
        QTest::addColumn<int>("framebufferRotation");
        QTest::addColumn<int>("sceneRotation");
        QTest::addColumn<int>("scale");
        QTest::addColumn<bool>("nativeNode");
        for (int fb = 0; fb < 4; ++fb)
            for (int scene : {0, 90, -90, 180})
                for (int scale : {1, 2})
                    for (bool native : {false, true})
                        QTest::newRow(qPrintable(QString("fb%1-scene%2-scale%3-%4")
                            .arg(fb).arg(scene).arg(scale).arg(native ? "epaper" : "direct")))
                            << fb << scene << scale << native;
    }
    void composedScene() {
        QFETCH(int, framebufferRotation);
        QFETCH(int, sceneRotation);
        QFETCH(int, scale);
        QFETCH(bool, nativeNode);
        Fixture fixture(framebufferRotation);
        const QImage local = fixture.localImage();
        QTransform scene;
        scene.translate(600, 600).rotate(sceneRotation).scale(scale, scale).translate(-90, -120);
        QImage expected = canvas(), actual = canvas();
        {
            QPainter painter(&expected);
            painter.setTransform(scene);
            painter.drawImage(QPoint(0, 0), local);
        }
        {
            QPainter painter(&actual);
            painter.setTransform(scene);
            if (nativeNode) EpaperNode(&fixture.controller).paint(&painter);
            else fixture.controller.paint(&painter);
        }
        QString reason;
        QVERIFY2(sameComposition(actual, expected, &reason), qPrintable(reason));
    }
    void movedWindowAndClippedRefresh() {
        Fixture fixture;
        const QImage local = fixture.localImage();
        EpaperNode node(&fixture.controller);
        QImage previous;
        int index = 0;
        for (const QPoint &position : {QPoint(60, 80), QPoint(400, 510), QPoint(760, 120)}) {
            QImage expected = canvas(), actual = canvas();
            for (bool reference : {true, false}) {
                QPainter painter(reference ? &expected : &actual);
                painter.translate(position);
                painter.scale(2, 2);
                // Models a window clip and a damage rectangle in item coordinates.
                painter.setClipRect(QRect(0, 0, 180, 240));
                if (index == 2) painter.setClipRect(QRect(20, 30, 100, 90), Qt::IntersectClip);
                if (reference) painter.drawImage(QPoint(0, 0), local);
                else node.paint(&painter);
            }
            const QString output = qEnvironmentVariable("REPAPER_RENDER_OUTPUT");
            if (!output.isEmpty()) {
                actual.save(output + QString("/move-%1-actual.png").arg(index));
                expected.save(output + QString("/move-%1-expected.png").arg(index));
            }
            QString reason;
            QVERIFY2(sameComposition(actual, expected, &reason), qPrintable(reason));
            if (!previous.isNull()) QVERIFY(actual != previous);
            previous = actual;
            ++index;
        }
    }
    void restoresCallerPainterState() {
        for (int rotation = 0; rotation < 4; ++rotation) {
            Fixture fixture(rotation);
            QImage output = canvas();
            QPainter painter(&output);
            painter.translate(430, 260);
            painter.rotate(-90);
            painter.scale(2, 2);
            painter.setClipRect(QRect(0, 0, 180, 240));
            painter.setOpacity(0.7);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            const auto transform = painter.worldTransform();
            const auto clip = painter.clipPath();
            fixture.controller.paint(&painter);
            QCOMPARE(painter.worldTransform(), transform);
            QCOMPARE(painter.clipPath(), clip);
            QCOMPARE(painter.opacity(), qreal(0.7));
            QCOMPARE(painter.compositionMode(), QPainter::CompositionMode_SourceOver);
        }
    }
};

QTEST_MAIN(RenderingTests)
#include "WindowRenderingTests.moc"

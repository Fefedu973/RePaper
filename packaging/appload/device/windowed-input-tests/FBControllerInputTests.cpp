#include "FBController.h"
#include "fbmanagement.h"
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QtTest>
#include <cmath>
#include <limits>

struct CapturedPacket { int key; qtfb::UserInputContents input; };
static std::vector<CapturedPacket> packets;
static std::map<int, QPointer<FBController>> controllers;

// Only the transport is replaced. The shipping FBController implementation and
// Qt event delivery/rendering remain real; no QML or device resources are used.
namespace qtfb::management {
void registerController(FBKey key, QPointer<FBController> value) { controllers[key] = value; }
void unregisterController(FBKey key) { controllers.erase(key); }
bool isControllerAssociated(FBKey key) { return controllers.count(key) && controllers[key]; }
void forwardUserInput(FBKey key, const UserInputContents &input) { packets.push_back({key, input}); }
void sendDeviceStateChange(FBKey, const DeviceStateChangedContents &) {}
void start() {}
}

class Fixture {
public:
    QImage image{1620, 2160, QImage::Format_RGB32};
    QQuickWindow window;
    FBController *controller = new FBController(window.contentItem());
    Fixture(QSize size, int rotation = 0, int mode = FBController::PreserveAspectFit, bool scaling = true) {
        window.resize(size);
        controller->setWidth(size.width());
        controller->setHeight(size.height());
        controller->setProperty("allowScaling", scaling);
        controller->setProperty("fillMode", mode);
        controller->setFramebufferID(4567);
        controller->setFbRotation(static_cast<FBController::Rotation>(rotation));
        image.fill(Qt::white);
        controller->associateSHM(&image);
        QCoreApplication::processEvents();
        packets.clear();
    }
    QImage painted() {
        QImage output(window.size(), QImage::Format_RGB32);
        output.fill(QColor(255, 0, 255));
        QPainter painter(&output);
        controller->paint(&painter);
        return output;
    }
    void pattern() {
        for(int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for(int x = 0; x < image.width(); ++x)
                row[x] = qRgb(x / 32, y / 32, 73);
        }
    }
};

class InputTests : public QObject {
    Q_OBJECT
private slots:
    void scaledCenter() {
        Fixture f({540, 720});
        const auto mapped = f.controller->convertPointToQTFBPixels({270, 360});
        QVERIFY(mapped.has_value());
        QCOMPARE(*mapped, QPoint(810, 1080));
    }
    void letterboxCenter() {
        Fixture f({900, 700});
        // Independent geometry: 525 x 700 image, left margin floor((900-525)/2).
        const auto mapped = f.controller->convertPointToQTFBPixels({449.5, 350});
        QVERIFY(mapped.has_value());
        QCOMPARE(*mapped, QPoint(810, 1080));
        QVERIFY(!f.controller->convertPointToQTFBPixels({100, 350}));
        QVERIFY(!f.controller->convertPointToQTFBPixels({800, 350}));
    }
    void asymmetricRotations_data() {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<QPointF>("local");
        QTest::newRow("portrait") << 0 << QPointF(207.5, 450);
        QTest::newRow("left") << 1 << QPointF(480, 420);
        QTest::newRow("right") << 2 << QPointF(160, 180);
        QTest::newRow("inverted") << 3 << QPointF(432.5, 150);
    }
    void asymmetricRotations() {
        QFETCH(int, rotation);
        QFETCH(QPointF, local);
        Fixture f({640, 600}, rotation);
        const auto mapped = f.controller->convertPointToQTFBPixels(local);
        QVERIFY(mapped.has_value());
        QCOMPARE(*mapped, QPoint(405, 1620));
    }
    void paintOracle_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<int>("rotation");
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("scaling");
        for(const QSize &size : {QSize(1620,2160), QSize(540,720), QSize(400,700), QSize(900,700)})
            for(int rotation = 0; rotation < 4; ++rotation)
                for(int mode = 0; mode < 4; ++mode) {
                    const QByteArray name = QByteArray::number(size.width()) + "x" + QByteArray::number(size.height())
                        + "-r" + QByteArray::number(rotation) + "-m" + QByteArray::number(mode);
                    QTest::newRow(name) << size << rotation << mode << true;
                }
        for(int rotation = 0; rotation < 4; ++rotation)
            QTest::newRow(qPrintable(QString("scaling-disabled-r%1").arg(rotation)))
                << QSize(1900, 2300) << rotation << int(FBController::PreserveAspectFit) << false;
    }
    void paintOracle() {
        QFETCH(QSize, size);
        QFETCH(int, rotation);
        QFETCH(int, mode);
        QFETCH(bool, scaling);
        Fixture f(size, rotation, mode, scaling);
        f.pattern();
        const QImage output = f.painted();
        int observed = 0;
        for(int y = 11; y < size.height(); y += 37)
            for(int x = 13; x < size.width(); x += 41) {
                const QRgb pixel = output.pixel(x, y);
                const auto mapped = f.controller->convertPointToQTFBPixels({x + .5, y + .5});
                if(pixel == qRgb(255, 0, 255)) {
                    QVERIFY2(!mapped, "Input was accepted in an unpainted margin");
                    continue;
                }
                ++observed;
                QVERIFY(mapped.has_value());
                QVERIFY(f.image.rect().contains(*mapped));
                QCOMPARE(f.image.pixel(*mapped), pixel);
            }
        // Some old Pad geometries paint entirely outside the item; preserve that behavior.
        if(scaling && mode != FBController::Pad) QVERIFY(observed > 0);
    }
    void renderSignatures() {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for(const QSize &size : {QSize(1620,2160), QSize(540,720), QSize(400,700), QSize(900,700)})
            for(int rotation = 0; rotation < 4; ++rotation)
                for(int mode = 0; mode < 4; ++mode) {
                    Fixture f(size, rotation, mode);
                    f.pattern();
                    const QImage output = f.painted();
                    hash.addData(reinterpret_cast<const char *>(output.constBits()), output.sizeInBytes());
                }
        qInfo().noquote() << "RENDER_SHA256=" + hash.result().toHex();
    }
    void invalidGeometryAndCoordinates() {
        FBController bare;
        bare.setFramebufferID(5678);
        bare.setWidth(540); bare.setHeight(720);
        QVERIFY(!bare.convertPointToQTFBPixels({20, 20}));
        QMouseEvent noImage(QEvent::MouseButtonPress, QPointF(20,20), QPointF(20,20), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        packets.clear(); bare.mousePressEvent(&noImage); QVERIFY(packets.empty());
        QImage empty;
        bare.associateSHM(&empty);
        QCoreApplication::processEvents();
        QVERIFY(!bare.convertPointToQTFBPixels({20, 20}));
        Fixture f({540,720});
        for(const QPointF &point : {QPointF(-1,10), QPointF(10,-1), QPointF(540,10), QPointF(10,720),
             QPointF(std::numeric_limits<double>::infinity(),10), QPointF(10,std::numeric_limits<double>::quiet_NaN())})
            QVERIFY(!f.controller->convertPointToQTFBPixels(point));
        f.controller->setWidth(0);
        QVERIFY(!f.controller->convertPointToQTFBPixels({0,0}));
        packets.clear(); f.controller->mousePressEvent(&noImage); QVERIFY(packets.empty());
        f.controller->setWidth(540); f.controller->setHeight(0);
        QVERIFY(!f.controller->convertPointToQTFBPixels({0,0}));
    }
    void pointerPackets_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<int>("rotation");
        QTest::addColumn<QPoint>("local");
        QTest::addColumn<QPoint>("expected");
        QTest::newRow("fullscreen") << QSize(1620,2160) << 0 << QPoint(810,1080) << QPoint(810,1080);
        QTest::newRow("window") << QSize(540,720) << 0 << QPoint(270,360) << QPoint(810,1080);
        QTest::newRow("letterbox") << QSize(900,700) << 0 << QPoint(450,350) << QPoint(811,1080);
        QTest::newRow("left") << QSize(640,600) << 1 << QPoint(480,420) << QPoint(405,1620);
        QTest::newRow("right") << QSize(640,600) << 2 << QPoint(160,180) << QPoint(405,1620);
        QTest::newRow("inverted") << QSize(640,600) << 3 << QPoint(433,150) << QPoint(403,1620);
    }
    void pointerPackets() {
        QFETCH(QSize, size);
        QFETCH(int, rotation);
        QFETCH(QPoint, local);
        QFETCH(QPoint, expected);
        Fixture f(size, rotation);
        f.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&f.window));
        packets.clear();
        QMouseEvent down(QEvent::MouseButtonPress, local, local, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, local, local, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        f.controller->mousePressEvent(&down);
        f.controller->mouseReleaseEvent(&up);
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[0].input.inputType, INPUT_PEN_PRESS);
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_RELEASE);
        for(const auto &packet : packets) {
            QCOMPARE(packet.key, 4567);
            QCOMPARE(QPoint(packet.input.x,packet.input.y), expected);
        }
        packets.clear();
        auto *device = QTest::createTouchDevice();
        auto sequence = QTest::touchEvent(&f.window, device, false);
        sequence.press(1, local, &f.window).commit();
        sequence.release(1, local, &f.window).commit();
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[0].input.inputType, INPUT_TOUCH_PRESS);
        QCOMPARE(packets[1].input.inputType, INPUT_TOUCH_RELEASE);
        for(const auto &packet : packets) {
            QCOMPARE(packet.key, 4567);
            QCOMPARE(QPoint(packet.input.x,packet.input.y), expected);
        }
    }
    void rejectedTouchPresses() {
        Fixture f({900,700});
        f.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&f.window));
        auto *device = QTest::createTouchDevice();
        auto sequence = QTest::touchEvent(&f.window, device, false);
        packets.clear();
        sequence.press(2, QPoint(100,350), &f.window).commit();
        sequence.release(2, QPoint(100,350), &f.window).commit();
        QVERIFY(packets.empty());
        f.controller->associateSHM(nullptr);
        QCoreApplication::processEvents();
        sequence.press(3, QPoint(450,350), &f.window).commit();
        sequence.release(3, QPoint(450,350), &f.window).commit();
        QVERIFY(packets.empty());
        f.controller->associateSHM(&f.image);
        QCoreApplication::processEvents();
        f.controller->setWidth(0);
        sequence.press(4, QPoint(450,350), &f.window).commit();
        sequence.release(4, QPoint(450,350), &f.window).commit();
        QVERIFY(packets.empty());
    }
};

QTEST_MAIN(InputTests)
#include "FBControllerInputTests.moc"

#include "FBController.h"
#include "fbmanagement.h"
#include <QGuiApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>
#include <limits>

struct CapturedPacket { int key; qtfb::UserInputContents input; };
static std::vector<CapturedPacket> packets;
static std::map<int, QPointer<FBController>> controllers;
namespace qtfb::management {
void registerController(FBKey key, QPointer<FBController> value) { controllers[key] = value; }
void unregisterController(FBKey key) { controllers.erase(key); }
bool isControllerAssociated(FBKey key) { return controllers.count(key) && controllers[key]; }
void forwardUserInput(FBKey key, const UserInputContents &input) { packets.push_back({key, input}); }
void sendDeviceStateChange(FBKey, const DeviceStateChangedContents &) {}
void start() {}
}

static QPointingDevice *pen;
static QPointingDevice *eraser;
static QPointingDevice *xochitlMousePen;
static ulong timestamp = 100;

class Fixture {
public:
    QQmlEngine engine;
    QQuickWindow window;
    QImage image{1620, 2160, QImage::Format_RGB32};
    FBController *controller = nullptr;
    Fixture(QSize size = {540, 720}, int rotation = 0) {
        QQmlComponent component(&engine);
        QByteArray qml = "import QtQuick\nimport net.asivery.Framebuffer 1.0\nFBController { width: 540; height: 720; allowScaling: true; fillMode: FBController.PreserveAspectFit;";
#ifndef STYLUS_BASELINE
        qml += "StylusInputHandler { objectName: \"stylus\" }";
#endif
        qml += "}";
        component.setData(qml, QUrl::fromLocalFile(QStringLiteral(STYLUS_SOURCE "/resources/qml/Fixture.qml")));
        if(component.isError()) qFatal("QML fixture: %s", qPrintable(component.errorString()));
        controller = qobject_cast<FBController *>(component.create());
        if(!controller) qFatal("Create fixture: %s", qPrintable(component.errorString()));
        controller->setParentItem(window.contentItem());
        window.resize(size);
        controller->setSize(size);
        controller->setFramebufferID(4567);
        controller->setFbRotation(static_cast<FBController::Rotation>(rotation));
        image.fill(Qt::white);
        controller->associateSHM(&image);
        window.show();
        QCoreApplication::processEvents();
        QTest::qWait(30);
        packets.clear();
    }
    void tablet(QPointF at, bool down, qreal pressure = .5, QPointingDevice *device = pen) {
        const QPointF global = window.mapToGlobal(at.toPoint());
        QWindowSystemInterface::handleTabletEvent(&window, timestamp += 20, device, at, global,
            down ? Qt::LeftButton : Qt::NoButton, down ? pressure : 0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
        QCoreApplication::processEvents();
    }
    void nativeMouse(QPointF at, QEvent::Type type) {
        const QPointF global = window.mapToGlobal(at.toPoint());
        const bool released = type == QEvent::MouseButtonRelease;
        QWindowSystemInterface::handleMouseEvent(nullptr, timestamp += 20, xochitlMousePen, at, global,
            released ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type, Qt::NoModifier, Qt::MouseEventNotSynthesized);
        QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
        QCoreApplication::processEvents();
    }
};

class StylusTests : public QObject {
    Q_OBJECT
private slots:
    void nativeTabletSequence() {
        Fixture f;
        f.tablet({270, 360}, true, .5);
        f.tablet({300, 400}, true, .75);
        f.tablet({300, 400}, false);
        QCOMPARE(packets.size(), size_t(3));
        QCOMPARE(packets[0].input.inputType, INPUT_PEN_PRESS);
        QCOMPARE(packets[0].input.x, 810);
        QCOMPARE(packets[0].input.y, 1080);
        QCOMPARE(packets[0].input.d, 50);
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_UPDATE);
        QCOMPARE(packets[1].input.x, 900);
        QCOMPARE(packets[1].input.y, 1200);
        QCOMPARE(packets[1].input.d, 75);
        QCOMPARE(packets[2].input.inputType, INPUT_PEN_RELEASE);
        QCOMPARE(packets[2].input.d, 0);
    }
#ifndef STYLUS_BASELINE
    void xochitlMousePenSequence() {
        Fixture f;
        f.nativeMouse({270, 360}, QEvent::MouseButtonPress);
        f.nativeMouse({300, 400}, QEvent::MouseMove);
        f.nativeMouse({300, 400}, QEvent::MouseButtonRelease);
        QCOMPARE(packets.size(), size_t(3));
        QCOMPARE(packets[0].input.inputType, INPUT_PEN_PRESS);
        QCOMPARE(packets[0].input.x, 810);
        QCOMPARE(packets[0].input.y, 1080);
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_UPDATE);
        QCOMPARE(packets[1].input.x, 900);
        QCOMPARE(packets[1].input.y, 1200);
        QCOMPARE(packets[2].input.inputType, INPUT_PEN_RELEASE);
    }
    void xochitlMouseCaptureIntegrationPackets() {
        Fixture f({810, 1080});
        f.nativeMouse({200, 150}, QEvent::MouseButtonPress);
        f.nativeMouse({200, 160}, QEvent::MouseMove);
        f.nativeMouse({200, 160}, QEvent::MouseButtonRelease);
        auto *touch = QTest::createTouchDevice();
        QTest::touchEvent(&f.window, touch).press(0, {200, 150}, &f.window);
        QTest::touchEvent(&f.window, touch).release(0, {200, 150}, &f.window);
        QCoreApplication::processEvents();
        QJsonArray captured;
        for (const auto &packet : packets) {
            const auto &input = packet.input;
            captured.append(QJsonObject{{"type", input.inputType}, {"devId", input.devId},
                {"x", input.x}, {"y", input.y}, {"pressure", input.d}});
        }
        const QByteArray path = qgetenv("REPAPER_NATIVE_MOUSE_PACKET_OUTPUT");
        if (!path.isEmpty()) {
            QFile file(QString::fromLocal8Bit(path));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(QJsonDocument(QJsonObject{
                {"origin", "Xochitl 3.28 binary-verified QWSI Mouse + Stylus/Pen/Position device contract -> real FBController"},
                {"pressureKind", "binary contact; native mouse route exposes no measured pressure"},
                {"packets", captured}}).toJson()) > 0);
        }
        QCOMPARE(packets.size(), size_t(5));
        QCOMPARE(packets[0].input.inputType, INPUT_PEN_PRESS);
        QCOMPARE(packets[0].input.x, 400);
        QCOMPARE(packets[0].input.y, 300);
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_UPDATE);
        QCOMPARE(packets[1].input.x, 400);
        QCOMPARE(packets[1].input.y, 320);
        QCOMPARE(packets[2].input.inputType, INPUT_PEN_RELEASE);
        QCOMPARE(packets[2].input.x, 400);
        QCOMPARE(packets[2].input.y, 320);
    }
    void xochitlMouseCaptureScrollPackets() {
        Fixture f({810, 1080});
        f.nativeMouse({200, 150}, QEvent::MouseButtonPress);
        f.nativeMouse({200, 160}, QEvent::MouseMove);
        f.nativeMouse({200, 160}, QEvent::MouseButtonRelease);
        auto *touch = QTest::createTouchDevice();
        QTest::touchEvent(&f.window, touch).press(0, {200, 150}, &f.window);
        QTest::touchEvent(&f.window, touch).release(0, {200, 150}, &f.window);
        QCoreApplication::processEvents();
        f.nativeMouse({450, 430}, QEvent::MouseButtonPress);
        for (int y : {380, 330, 280, 230})
            f.nativeMouse({450, y}, QEvent::MouseMove);
        f.nativeMouse({450, 230}, QEvent::MouseButtonRelease);
        QJsonArray captured;
        for (const auto &packet : packets) {
            const auto &input = packet.input;
            captured.append(QJsonObject{{"type", input.inputType}, {"devId", input.devId},
                {"x", input.x}, {"y", input.y}, {"pressure", input.d}});
        }
        const QByteArray path = qgetenv("REPAPER_NATIVE_MOUSE_SCROLL_PACKET_OUTPUT");
        if (!path.isEmpty()) {
            QFile file(QString::fromLocal8Bit(path));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(QJsonDocument(QJsonObject{
                {"origin", "Xochitl 3.28 binary-verified QWSI Mouse + Stylus/Pen/Position device contract -> real FBController; clicks then scroll"},
                {"pressureKind", "binary contact; native mouse route exposes no measured pressure"},
                {"expectScroll", true}, {"packets", captured}}).toJson()) > 0);
        }
        QCOMPARE(packets.size(), size_t(11));
        QCOMPARE(packets[5].input.inputType, INPUT_PEN_PRESS);
        QCOMPARE(packets[5].input.x, 900);
        QCOMPARE(packets[5].input.y, 860);
        for (size_t i = 6; i < 10; ++i) {
            QCOMPARE(packets[i].input.inputType, INPUT_PEN_UPDATE);
            QCOMPARE(packets[i].input.x, 900);
            QCOMPARE(packets[i].input.y, int(860 - (i - 5) * 100));
        }
        QCOMPARE(packets[10].input.inputType, INPUT_PEN_RELEASE);
        QCOMPARE(packets[10].input.y, 460);
    }
    void xochitlMouseAndQtTabletCanAlternate() {
        Fixture f;
        f.tablet({270, 360}, true);
        f.tablet({270, 360}, false);
        f.nativeMouse({270, 360}, QEvent::MouseButtonPress);
        f.nativeMouse({270, 360}, QEvent::MouseButtonRelease);
        f.tablet({270, 360}, true);
        f.tablet({270, 360}, false);
        QCOMPARE(packets.size(), size_t(6));
        for (const auto &packet : packets) {
            QCOMPARE(packet.input.x, 810);
            QCOMPARE(packet.input.y, 1080);
        }
    }
    void xochitlMouseReleaseOutside() {
        Fixture f;
        // AppLoad windows are items inside Xochitl's larger QQuickWindow. The
        // native digitizer release remains inside that physical host window.
        f.window.resize(1000, 1000);
        QCoreApplication::processEvents();
        f.nativeMouse({270, 360}, QEvent::MouseButtonPress);
        f.nativeMouse({600, 760}, QEvent::MouseButtonRelease);
        QCOMPARE(packets.size(), size_t(3));
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_UPDATE);
        QCOMPARE(packets[1].input.x, 1800);
        QCOMPARE(packets[1].input.y, 2280);
        QCOMPARE(packets[2].input.inputType, INPUT_PEN_RELEASE);
    }
    void xochitlMouseGrabCancellation() {
        Fixture f;
        f.nativeMouse({270, 360}, QEvent::MouseButtonPress);
        f.controller->ungrabMouse();
        QCOMPARE(packets.size(), size_t(3));
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_UPDATE);
        QVERIFY(packets[1].input.x < 0 || packets[1].input.y < 0);
        QCOMPARE(packets[2].input.inputType, INPUT_PEN_RELEASE);
        f.nativeMouse({270, 360}, QEvent::MouseButtonRelease);
        QCOMPARE(packets.size(), size_t(3));
        f.nativeMouse({270, 360}, QEvent::MouseButtonPress);
        f.nativeMouse({270, 360}, QEvent::MouseButtonRelease);
        QCOMPARE(packets.size(), size_t(5));
    }
    void captureIntegrationPackets() {
        Fixture f({810, 1080});
        f.tablet({200, 150}, true, .5);
        f.tablet({200, 150}, true, .75);
        f.tablet({200, 150}, false);
        auto *touch = QTest::createTouchDevice();
        QTest::touchEvent(&f.window, touch).press(0, {200, 150}, &f.window);
        QTest::touchEvent(&f.window, touch).release(0, {200, 150}, &f.window);
        QCoreApplication::processEvents();
        QCOMPARE(packets.size(), size_t(5));
        const int expectedTypes[] = {INPUT_PEN_PRESS, INPUT_PEN_UPDATE, INPUT_PEN_RELEASE, INPUT_TOUCH_PRESS, INPUT_TOUCH_RELEASE};
        QJsonArray output;
        for(size_t i = 0; i < packets.size(); ++i) {
            const auto &input = packets[i].input;
            QCOMPARE(input.inputType, expectedTypes[i]);
            QCOMPARE(input.x, 400);
            QCOMPARE(input.y, 300);
            output.append(QJsonObject{{"type", input.inputType}, {"devId", input.devId}, {"x", input.x},
                {"y", input.y}, {"pressure", input.d}});
        }
        const QByteArray path = qgetenv("REPAPER_STYLUS_PACKET_OUTPUT");
        if(!path.isEmpty()) {
            QFile file(QString::fromLocal8Bit(path));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(QJsonDocument(QJsonObject{{"origin", "ARM Qt QWindowSystemInterface -> production StylusInputHandler.qml -> real FBController -> captured QTFB transport"},
                {"packets", output}}).toJson()) > 0);
        }
    }
    void synthesisDoesNotDoubleTap() {
        QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTabletEvents, true);
        Fixture f;
        f.tablet({270, 360}, true);
        f.tablet({270, 360}, false);
        QCOMPARE(packets.size(), size_t(2));
        QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTabletEvents, false);
    }
    void eraserIsUsableAsPointer() {
        Fixture f;
        f.tablet({270, 360}, true, .6, eraser);
        f.tablet({270, 360}, false, 0, eraser);
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[0].input.d, 60);
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_RELEASE);
    }
    void hoverDoesNotPress() {
        Fixture f;
        f.tablet({270, 360}, false);
        f.tablet({300, 400}, false);
        QVERIFY(packets.empty());
    }
    void letterboxPressCannotStartByMovingInside() {
        Fixture f({900, 700});
        f.tablet({100, 350}, true);
        f.tablet({450, 350}, true);
        f.tablet({450, 350}, false);
        QVERIFY(packets.empty());
    }
    void dragOutsideReleasesOutside() {
        Fixture f;
        f.tablet({270, 360}, true);
        f.tablet({600, 760}, true);
        f.tablet({600, 760}, false);
        QCOMPARE(packets.size(), size_t(3));
        QCOMPARE(packets[1].input.x, 1800);
        QCOMPARE(packets[1].input.y, 2280);
        QCOMPARE(packets[2].input.inputType, INPUT_PEN_RELEASE);
        QVERIFY(packets[2].input.x >= 1620 || packets[2].input.y >= 2160);
        f.tablet({270, 360}, true);
        f.tablet({270, 360}, false);
        QCOMPARE(packets.size(), size_t(5));
    }
    void releaseOutsideWithoutMoveCancels() {
        Fixture f;
        f.tablet({270, 360}, true);
        f.tablet({600, 760}, false);
        QCOMPARE(packets.size(), size_t(3));
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_UPDATE);
        QCOMPARE(packets[2].input.inputType, INPUT_PEN_RELEASE);
        QVERIFY(packets[1].input.x < 0 || packets[1].input.x >= 1620
                || packets[1].input.y < 0 || packets[1].input.y >= 2160);
    }
    void rotations_data() {
        QTest::addColumn<int>("rotation");
        for(int i = 0; i < 4; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void rotations() {
        QFETCH(int, rotation);
        Fixture f({720, 720}, rotation);
        f.tablet({360, 360}, true);
        f.tablet({360, 360}, false);
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[0].input.x, 810);
        QCOMPARE(packets[0].input.y, 1080);
    }
    void physicalMouseStillWorks() {
        Fixture f;
        QTest::mouseClick(&f.window, Qt::LeftButton, Qt::NoModifier, {270, 360});
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[0].input.inputType, INPUT_PEN_PRESS);
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_RELEASE);
    }
    void touchStillWorks() {
        Fixture f;
        auto *device = QTest::createTouchDevice();
        QTest::touchEvent(&f.window, device).press(0, {270, 360}, &f.window);
        QTest::touchEvent(&f.window, device).release(0, {270, 360}, &f.window);
        QCoreApplication::processEvents();
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[0].input.inputType, INPUT_TOUCH_PRESS);
        QCOMPARE(packets[1].input.inputType, INPUT_TOUCH_RELEASE);
    }
    void deactivationReleasesOnce() {
        Fixture f;
        f.tablet({270, 360}, true);
        f.controller->setActive(false);
        f.tablet({270, 360}, false);
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[1].input.inputType, INPUT_PEN_RELEASE);
    }
    void invalidPressureIsFiniteAndClamped() {
        Fixture f;
        f.controller->penEvent(FBController::PenPress, {270, 360}, std::numeric_limits<qreal>::quiet_NaN());
        f.controller->penEvent(FBController::PenMove, {270, 360}, 5);
        f.controller->penEvent(FBController::PenMove, {270, 360}, -5);
        f.controller->penEvent(FBController::PenRelease, {270, 360}, 1);
        QCOMPARE(packets.size(), size_t(4));
        QCOMPARE(packets[0].input.d, 0);
        QCOMPARE(packets[1].input.d, 100);
        QCOMPARE(packets[2].input.d, 0);
        QCOMPARE(packets[3].input.d, 0);
    }
    void overlappingWindowGetsOnlyOneSequence() {
        Fixture f;
        FBController *under = new FBController(f.window.contentItem());
        under->setSize(f.controller->size());
        under->setProperty("allowScaling", true);
        under->setFramebufferID(9999);
        under->associateSHM(&f.image);
        QQmlComponent handler(&f.engine, QUrl::fromLocalFile(QStringLiteral(STYLUS_SOURCE "/resources/qml/StylusInputHandler.qml")));
        QObject *instance = handler.createWithInitialProperties({{"parent", QVariant::fromValue(under)}});
        QVERIFY2(instance, qPrintable(handler.errorString()));
        instance->setParent(under);
        f.controller->setZ(1);
        QCoreApplication::processEvents();
        packets.clear();
        f.tablet({270, 360}, true);
        f.tablet({270, 360}, false);
        QCOMPARE(packets.size(), size_t(2));
        QCOMPARE(packets[0].key, 4567);
        QCOMPARE(packets[1].key, 4567);
    }
#endif
};

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTabletEvents, false);
    QGuiApplication app(argc, argv);
    qmlRegisterType<FBController>("net.asivery.Framebuffer", 1, 0, "FBController");
    const auto caps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure;
    pen = new QPointingDevice("QA native pen", 901, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, caps, 1, 1, {}, {}, &app);
    eraser = new QPointingDevice("QA native eraser", 902, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser, caps, 1, 1, {}, {}, &app);
    QWindowSystemInterface::registerInputDevice(pen);
    QWindowSystemInterface::registerInputDevice(eraser);
    // Xochitl 3.28's actual binary constructs exactly this device shape and
    // calls QWSI::handleMouseEvent, not handleTabletEvent, for its digitizer.
    xochitlMousePen = new QPointingDevice("Xochitl digitizer mouse", 43323,
        QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position, 1, 1, {}, {}, &app);
    QWindowSystemInterface::registerInputDevice(xochitlMousePen);
    StylusTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "StylusInputTests.moc"

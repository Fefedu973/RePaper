#include "TabletMouseAdapter.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>
#include <memory>

namespace {
QPointingDevice *pen = nullptr;
QPointingDevice *eraser = nullptr;
ulong eventTime = 100;

class Fixture
{
public:
    QQmlEngine engine;
    QQuickWindow window;
    QQuickItem *root = nullptr;

    Fixture()
    {
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick 2.15
            import QtQuick.Controls 2.15
            Rectangle {
                width: 500; height: 500; color: "white"
                property int clicks: 0
                property int areaPresses: 0
                property int areaReleases: 0
                property int areaClicks: 0
                property int areaCancels: 0
                property int popupClicks: 0
                Button {
                    objectName: "button"; x: 40; y: 40; width: 160; height: 70
                    text: "Click"; onClicked: parent.clicks++
                }
                Rectangle {
                    id: marker; objectName: "marker"; x: 40; y: 180
                    width: 30; height: 30; color: "black"
                }
                MouseArea {
                    objectName: "area"; x: 30; y: 160; width: 180; height: 190
                    drag.target: marker
                    onPressed: parent.areaPresses++
                    onReleased: parent.areaReleases++
                    onClicked: parent.areaClicks++
                    onCanceled: parent.areaCancels++
                }
                Flickable {
                    objectName: "flick"; x: 250; y: 30; width: 200; height: 260
                    contentWidth: 200; contentHeight: 1000; clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Rectangle { width: 200; height: 1000; color: "#dddddd" }
                }
                Popup {
                    objectName: "popup"; x: 80; y: 280; width: 240; height: 140
                    modal: true; focus: true
                    contentItem: Button { text: "Popup"; onClicked: rootItem.popupClicks++ }
                }
                id: rootItem
            }
        )", QUrl());
        if (component.isError())
            qFatal("QML: %s", qPrintable(component.errorString()));
        root = qobject_cast<QQuickItem *>(component.create());
        if (!root)
            qFatal("Cannot instantiate QML controls: %s", qPrintable(component.errorString()));
        root->setParentItem(window.contentItem());
        window.resize(500, 500);
        window.show();
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }

    QObject *find(const char *name) const { return root->findChild<QObject *>(QString::fromLatin1(name)); }
    int count(const char *name) const { return root->property(name).toInt(); }

    void tablet(QEvent::Type type, QPointF local, const QPointingDevice *device = pen,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const QPointF global = QPointF(window.mapToGlobal(QPoint(0, 0))) + local;
        QTabletEvent event(type, device, local, global,
                           type == QEvent::TabletRelease ? 0 : .37,
                           0, 0, 0, 0, 0, modifiers,
                           type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                           type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
        event.setTimestamp(eventTime += 20);
        event.setAccepted(false);
        QCoreApplication::sendEvent(&window, &event);
        QCoreApplication::processEvents();
    }

    void click(QPointF at, const QPointingDevice *device = pen)
    {
        tablet(QEvent::TabletPress, at, device);
        tablet(QEvent::TabletRelease, at, device);
    }
};

class MouseProbe : public QObject
{
public:
    explicit MouseProbe(QWindow *target) : window(target) { qGuiApp->installEventFilter(this); }
    QWindow *window;
    int presses = 0;
    int releases = 0;
    QPointF lastPress;
    Qt::MouseEventSource source = Qt::MouseEventNotSynthesized;
    Qt::KeyboardModifiers modifiers;
    QInputDevice::DeviceType deviceType = QInputDevice::DeviceType::Unknown;
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != window)
            return false;
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            ++presses;
            lastPress = mouse->position();
            source = mouse->source();
            modifiers = mouse->modifiers();
            deviceType = mouse->pointingDevice()->type();
        } else if (event->type() == QEvent::MouseButtonRelease) {
            ++releases;
        }
        return false;
    }
};
}

class TabletMouseAdapterTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qputenv("REPAPER_APPLOAD_TABLET_INPUT", "0");
        pen = new QPointingDevice(QStringLiteral("test pen"), 7001,
            QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1, {}, {}, qGuiApp);
        eraser = new QPointingDevice(QStringLiteral("test eraser"), 7002,
            QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1, {}, {}, qGuiApp);
    }

    void optInAndSingleton()
    {
        qunsetenv("REPAPER_APPLOAD_TABLET_INPUT");
        QVERIFY(!repaper::installTabletMouseAdapter());
        qputenv("REPAPER_APPLOAD_TABLET_INPUT", "true");
        QVERIFY(!repaper::installTabletMouseAdapter());
        qputenv("REPAPER_APPLOAD_TABLET_INPUT", "1");
        auto *first = repaper::installTabletMouseAdapter();
        QVERIFY(first);
        QCOMPARE(first->parent(), qGuiApp);
        QCOMPARE(first->objectName(), QStringLiteral("repaper.tabletMouseAdapter"));
        QCOMPARE(first, repaper::installTabletMouseAdapter());
        delete first;
        qputenv("REPAPER_APPLOAD_TABLET_INPUT", "0");
        QVERIFY(!repaper::installTabletMouseAdapter());
    }

    void buttonPenAndEraser_data()
    {
        QTest::addColumn<bool>("useEraser");
        QTest::newRow("pen") << false;
        QTest::newRow("eraser") << true;
    }
    void buttonPenAndEraser()
    {
        QFETCH(bool, useEraser);
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        MouseProbe probe(&fixture.window);
        const auto *device = useEraser ? eraser : pen;
        fixture.tablet(QEvent::TabletPress, {100.25, 75.75}, device, Qt::ShiftModifier);
        QVERIFY(fixture.find("button")->property("pressed").toBool());
        fixture.tablet(QEvent::TabletRelease, {100.25, 75.75}, device, Qt::ShiftModifier);
        QCOMPARE(fixture.count("clicks"), 1);
        QVERIFY(!fixture.find("button")->property("pressed").toBool());
        QCOMPARE(probe.presses, 1);
        QCOMPARE(probe.releases, 1);
        QCOMPARE(probe.lastPress, QPointF(100.25, 75.75));
        QCOMPARE(probe.source, Qt::MouseEventSynthesizedByApplication);
        QCOMPARE(probe.modifiers, Qt::KeyboardModifiers(Qt::ShiftModifier));
        QCOMPARE(probe.deviceType, QInputDevice::DeviceType::Mouse);
    }

    void mouseAreaDrag()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        const QPointF before(fixture.find("marker")->property("x").toReal(), fixture.find("marker")->property("y").toReal());
        fixture.tablet(QEvent::TabletPress, {80, 220});
        fixture.tablet(QEvent::TabletMove, {100, 240});
        fixture.tablet(QEvent::TabletMove, {145, 280});
        fixture.tablet(QEvent::TabletRelease, {145, 280});
        QCOMPARE(fixture.count("areaPresses"), 1);
        QCOMPARE(fixture.count("areaReleases"), 1);
        QCOMPARE(fixture.count("areaClicks"), 0);
        QVERIFY(fixture.find("marker")->property("x").toReal() > before.x() + 20);
        QVERIFY(fixture.find("marker")->property("y").toReal() > before.y() + 20);
    }

    void flickableScroll()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        fixture.tablet(QEvent::TabletPress, {350, 240});
        for (int y : {220, 180, 140, 100, 60}) {
            fixture.tablet(QEvent::TabletMove, {350, qreal(y)});
            QTest::qWait(10);
        }
        fixture.tablet(QEvent::TabletRelease, {350, 60});
        QVERIFY(fixture.find("flick")->property("contentY").toReal() > 60);
    }

    void releaseOutsideDoesNotClickOrStick()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        fixture.tablet(QEvent::TabletPress, {100, 75});
        fixture.tablet(QEvent::TabletRelease, {-30, -30});
        QCOMPARE(fixture.count("clicks"), 0);
        QVERIFY(!fixture.find("button")->property("pressed").toBool());
        fixture.click({100, 75});
        QCOMPARE(fixture.count("clicks"), 1);
    }

    void cancelWindowLifecycle_data()
    {
        QTest::addColumn<int>("eventType");
        QTest::newRow("hide") << int(QEvent::Hide);
        QTest::newRow("close") << int(QEvent::Close);
        QTest::newRow("deactivate") << int(QEvent::WindowDeactivate);
    }
    void cancelWindowLifecycle()
    {
        QFETCH(int, eventType);
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        fixture.tablet(QEvent::TabletPress, {100, 75});
        QEvent cancellation(static_cast<QEvent::Type>(eventType));
        QCoreApplication::sendEvent(&fixture.window, &cancellation);
        QVERIFY(!fixture.find("button")->property("pressed").toBool());
        fixture.tablet(QEvent::TabletRelease, {100, 75});
        QCOMPARE(fixture.count("clicks"), 0);
        fixture.window.show();
        QCoreApplication::processEvents();
        fixture.click({100, 75});
        QCOMPARE(fixture.count("clicks"), 1);
    }

    void destroyedWindowClearsGesture()
    {
        repaper::TabletMouseAdapter adapter;
        auto fixture = std::make_unique<Fixture>();
        fixture->tablet(QEvent::TabletPress, {100, 75});
        fixture.reset();
        Fixture next;
        next.click({100, 75});
        QCOMPARE(next.count("clicks"), 1);
    }

    void hidingInsidePressedHandlerCancelsWithoutClick()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        const auto connection = QObject::connect(fixture.find("button"), SIGNAL(pressedChanged()),
                                                  &fixture.window, SLOT(hide()));
        QVERIFY(connection);
        fixture.tablet(QEvent::TabletPress, {100, 75});
        QVERIFY(!fixture.window.isVisible());
        QVERIFY(!fixture.find("button")->property("pressed").toBool());
        fixture.tablet(QEvent::TabletRelease, {100, 75});
        QCOMPARE(fixture.count("clicks"), 0);
        QObject::disconnect(connection);
        fixture.window.show();
        QCoreApplication::processEvents();
        fixture.click({100, 75});
        QCOMPARE(fixture.count("clicks"), 1);
    }

    void cancelMouseAreaWithoutMovingItsDragTarget()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        fixture.tablet(QEvent::TabletPress, {80, 220});
        fixture.tablet(QEvent::TabletMove, {110, 250});
        fixture.tablet(QEvent::TabletMove, {145, 280});
        const QPointF before(fixture.find("marker")->property("x").toReal(), fixture.find("marker")->property("y").toReal());
        QEvent cancellation(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&fixture.window, &cancellation);
        QCOMPARE(fixture.count("areaCancels"), 1);
        QCOMPARE(fixture.count("areaReleases"), 0);
        QCOMPARE(fixture.count("areaClicks"), 0);
        QCOMPARE(QPointF(fixture.find("marker")->property("x").toReal(), fixture.find("marker")->property("y").toReal()), before);
    }

    void unrelatedPenReleaseDoesNotEndGesture()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        fixture.tablet(QEvent::TabletPress, {100, 75}, pen);
        fixture.tablet(QEvent::TabletRelease, {100, 75}, eraser);
        QVERIFY(fixture.find("button")->property("pressed").toBool());
        fixture.tablet(QEvent::TabletRelease, {100, 75}, pen);
        QCOMPARE(fixture.count("clicks"), 1);
    }

    void hoverAndStrayReleaseDoNotClick()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        fixture.tablet(QEvent::TabletMove, {100, 75});
        fixture.tablet(QEvent::TabletRelease, {100, 75});
        QCOMPARE(fixture.count("clicks"), 0);
        QCOMPARE(fixture.count("areaPresses"), 0);
    }

    void popupControlReceivesClick()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        QVERIFY(QMetaObject::invokeMethod(fixture.find("popup"), "open"));
        QTest::qWait(30);
        fixture.click({180, 340});
        QCOMPARE(fixture.count("popupClicks"), 1);
        QCOMPARE(fixture.count("clicks"), 0);
    }

    void secondWindowReceivesOwnClick()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture first;
        Fixture second;
        second.window.setTransientParent(&first.window);
        second.window.setPosition(600, 100);
        second.click({100, 75});
        QCOMPARE(second.count("clicks"), 1);
        QCOMPARE(first.count("clicks"), 0);
    }

    void dragContinuesWhenEventsArriveAtAnotherWindow()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture first;
        Fixture second;
        second.window.setPosition(600, 100);
        first.tablet(QEvent::TabletPress, {80, 220});
        first.tablet(QEvent::TabletMove, {110, 250});
        const QPointF originDelta = QPointF(first.window.mapToGlobal(QPoint(0, 0)))
            - QPointF(second.window.mapToGlobal(QPoint(0, 0)));
        second.tablet(QEvent::TabletMove, originDelta + QPointF(160, 290));
        second.tablet(QEvent::TabletRelease, originDelta + QPointF(160, 290));
        QCOMPARE(first.count("areaPresses"), 1);
        QCOMPARE(first.count("areaReleases"), 1);
        QVERIFY(first.find("marker")->property("x").toReal() > 60);
        QCOMPARE(second.count("areaPresses"), 0);
        QCOMPARE(second.count("clicks"), 0);
    }

    void physicalMouseAndTouchRemainUsable()
    {
        repaper::TabletMouseAdapter adapter;
        Fixture fixture;
        QTest::mouseClick(&fixture.window, Qt::LeftButton, Qt::NoModifier, {100, 75});
        QCOMPARE(fixture.count("clicks"), 1);
        auto *touch = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
        QTest::touchEvent(&fixture.window, touch).press(0, {100, 75}, &fixture.window).commit();
        QCoreApplication::processEvents();
        QTest::touchEvent(&fixture.window, touch).release(0, {100, 75}, &fixture.window).commit();
        QCoreApplication::processEvents();
        QCOMPARE(fixture.count("clicks"), 2);
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    TabletMouseAdapterTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "TabletMouseAdapterTest.moc"

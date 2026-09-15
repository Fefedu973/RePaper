#include <QGuiApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTabletEvent>
#include <QTimer>
#include "shared/input/TabletMouseAdapter.h"

class EventRecorder : public QObject {
public:
    QJsonArray tabletEvents;
    QJsonArray mouseEvents;
    QJsonArray clickSnapshots;
    QQuickItem *root = nullptr;
    bool eventFilter(QObject *, QEvent *event) override {
        if(event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove || event->type() == QEvent::TabletRelease) {
            auto *tablet = static_cast<QTabletEvent *>(event);
            tabletEvents.append(QJsonObject{{"type", int(event->type())}, {"x", tablet->position().x()},
                {"y", tablet->position().y()}, {"pressure", tablet->pressure()}, {"buttons", int(tablet->buttons())}});
        }
        if(event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            mouseEvents.append(QJsonObject{{"type", int(event->type())}, {"x", mouse->position().x()},
                {"y", mouse->position().y()}, {"buttons", int(mouse->buttons())}, {"source", int(mouse->source())}});
        }
        if(root && (event->type() == QEvent::TabletRelease || event->type() == QEvent::TouchEnd)) {
            const int type = event->type();
            QTimer::singleShot(10, this, [this, type]() { clickSnapshots.append(QJsonObject{{"afterType", type}, {"clicks", root->property("clicks").toInt()}}); });
        }
        return false;
    }
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    auto *tabletAdapter = repaper::installTabletMouseAdapter();
    if(argc != 3) return 2;
    EventRecorder recorder;
    app.installEventFilter(&recorder);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import QtQuick.Controls
        Rectangle {
            color: "white"; width: 810; height: 1080
            property int clicks: 0
            readonly property real scrollContentY: scroll.contentY
            Button { x: 100; y: 100; width: 200; height: 100; text: "Stylus test"; onClicked: parent.clicks++ }
            Flickable {
                id: scroll
                x: 350; y: 100; width: 250; height: 400
                contentWidth: width; contentHeight: 1600; clip: true
                Rectangle { width: 250; height: 1600; color: "lightgray" }
            }
        }
    )", {});
    if(component.isError()) qFatal("QML: %s", qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(component.create());
    if(!root) qFatal("Cannot create QML");
    recorder.root = root;
    QQuickWindow window;
    window.resize(810, 1080);
    root->setParentItem(window.contentItem());
    window.show();
    const QString ready = QString::fromLocal8Bit(argv[1]);
    const QString report = QString::fromLocal8Bit(argv[2]);
    QTimer::singleShot(250, &app, [ready]() { QFile file(ready); if(file.open(QIODevice::WriteOnly)) file.write("ready\n"); });
    QTimer::singleShot(3500, &app, [&]() {
        const int clicks = root->property("clicks").toInt();
        bool penClickedOnce = false, touchClickedOnce = false;
        for(const auto &value : recorder.clickSnapshots) {
            const auto snapshot = value.toObject();
            if(snapshot["afterType"].toInt() == QEvent::TabletRelease && snapshot["clicks"].toInt() == 1)
                penClickedOnce = true;
            if(snapshot["afterType"].toInt() == QEvent::TouchEnd && snapshot["clicks"].toInt() == 2)
                touchClickedOnce = true;
        }
        const bool expectScroll = qEnvironmentVariableIntValue("REPAPER_STYLUS_EXPECT_SCROLL") == 1;
        const qreal scrollContentY = root->property("scrollContentY").toReal();
        const bool passed = clicks == 2 && recorder.tabletEvents.size() >= 3 && penClickedOnce && touchClickedOnce
            && (!expectScroll || scrollContentY >= 100);
        QFile file(report);
        if(!file.open(QIODevice::WriteOnly)) { app.exit(3); return; }
        file.write(QJsonDocument(QJsonObject{{"status", passed ? "PASS" : "FAIL"}, {"clicks", clicks},
            {"tabletEvents", recorder.tabletEvents}, {"mouseEvents", recorder.mouseEvents},
            {"clickSnapshots", recorder.clickSnapshots}, {"penClickedOnce", penClickedOnce},
            {"touchClickedOnce", touchClickedOnce}, {"windowWidth", window.width()},
            {"windowHeight", window.height()}, {"devicePixelRatio", window.devicePixelRatio()},
            {"tabletAdapterInstalled", tabletAdapter != nullptr},
            {"expectScroll", expectScroll}, {"scrollContentY", scrollContentY},
            {"synthesizeTabletMouse", QCoreApplication::testAttribute(Qt::AA_SynthesizeMouseForUnhandledTabletEvents)}}).toJson());
        file.close();
        window.grabWindow().save(report + ".png");
        app.exit(passed ? 0 : 1);
    });
    return app.exec();
}

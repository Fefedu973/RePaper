#include "KeyboardController.h"
#include "TabletMouseAdapter.h"
#include "MoodleController.h"
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    repaper::installTabletMouseAdapter();
    app.setOrganizationName("RePaper");
    app.setApplicationName("remoodle");
    auto args = app.arguments();
    MoodleController controller;
    if (args.contains("--configure-stdin")) {
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly))
            return 2;
        auto bytes = input.read(16385);
        if (bytes.size() > 16384)
            return 2;
        auto config = QJsonDocument::fromJson(bytes).object();
        QObject::connect(&controller, &MoodleController::loginFinished, &app, [&app](bool ok) {
            QTextStream(stdout) << (ok ? "Moodle account verified and encrypted."
                                       : "Moodle account verification failed.")
                                << Qt::endl;
            app.exit(ok ? 0 : 1);
        });
        QTimer::singleShot(0, &controller, [&controller, config] {
            controller.login(config["baseUrl"].toString(), config["token"].toString());
        });
        QTimer::singleShot(65000, &app, [&app] { app.exit(3); });
        return app.exec();
    }
    if (args.contains("--demo"))
        controller.loadDemo();
    QQmlApplicationEngine engine;
    repaper::registerKeyboardTypes();
    engine.rootContext()->setContextProperty("moodle", &controller);
    engine.load(QUrl("qrc:/remoodle/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    int screenshot = args.indexOf("--screenshot");
    if (screenshot >= 0 && screenshot + 1 < args.size())
        QTimer::singleShot(1000, &app, [&] {
            auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            app.exit(window && window->grabWindow().save(args[screenshot + 1]) ? 0 : 2);
        });
    return app.exec();
}

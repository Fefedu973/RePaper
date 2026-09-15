#include "AgendaController.h"
#include "BridgeClient.h"
#include "KeyboardController.h"
#include "TabletMouseAdapter.h"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <cstdio>

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    repaper::installTabletMouseAdapter();
    QCoreApplication::setOrganizationName("RePaper");
    QCoreApplication::setApplicationName("reagenda");
    QCommandLineParser arguments;
    arguments.addHelpOption();
    const QCommandLineOption screenshot("screenshot",
                                        "Save the rendered window then exit (desktop validation).", "path");
    const QCommandLineOption initialView("view", "Initial view: day, week or month.", "view", "week");
    const QCommandLineOption initialDate("date", "Initially selected ISO date.", "date");
    const QCommandLineOption screenshotPage(
        "screenshot-page", "Page to render for desktop validation: main, login, ics, sources.", "page",
        "main");
    arguments.addOption(screenshot);
    arguments.addOption(initialView);
    arguments.addOption(initialDate);
    arguments.addOption(screenshotPage);
    arguments.process(app);
    QQuickStyle::setStyle("Basic");
    repaper::registerKeyboardTypes();
    AgendaController agenda;
    agenda.setView(arguments.value(initialView));
    if (arguments.isSet(initialDate))
        agenda.selectDate(arguments.value(initialDate));
    repaper::BridgeClient bridge;
    agenda.setNoteHandler([&bridge](const QString &date, const QString &eventId, const QString &title,
                                   const QJsonObject &context) {
        if (bridge.busy())
            return false;
        const QString key = eventId.isEmpty() ? "reagenda:day:" + date : "reagenda:event:" + eventId;
        bridge.createNotebook(title.isEmpty() ? "Notes du " + date : title, key, context);
        return true;
    });
    QObject::connect(&bridge, &repaper::BridgeClient::imported, &bridge,
                     &repaper::BridgeClient::openDocument);
    QObject::connect(
        &bridge, &repaper::BridgeClient::failed, &agenda,
        [&agenda](const QString &, const QString &message) { agenda.reportBridgeError(message); });
    QObject::connect(&bridge, &repaper::BridgeClient::changed, &agenda, [&agenda, &bridge] {
        agenda.reportNoteProgress(bridge.message(), bridge.busy());
    });
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, &app, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    engine.rootContext()->setContextProperty("agenda", &agenda);
    engine.rootContext()->setContextProperty(
        "previewPage", arguments.isSet(screenshot) ? arguments.value(screenshotPage) : QString());
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &app,
        [](QObject *object, const QUrl &) {
            if (!object)
                QCoreApplication::exit(1);
        },
        Qt::QueuedConnection);
    engine.load(QUrl("qrc:/reagenda/Main.qml"));
    if (arguments.isSet(screenshot))
        QTimer::singleShot(700, &app, [&] {
            const auto *window = engine.rootObjects().isEmpty()
                                     ? nullptr
                                     : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            const bool saved =
                window && const_cast<QQuickWindow *>(window)->grabWindow().save(arguments.value(screenshot));
            app.exit(saved ? 0 : 2);
        });
    return app.exec();
}

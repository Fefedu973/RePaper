#include "PaperFonts.h"
#include "AppLoadViewport.h"
#include "Calculator.hpp"
#include "TabletMouseAdapter.h"
#include "layout/MathCanvas.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication::setOrganizationName("RePaper");
    QCoreApplication::setApplicationName("recalc");
    QCoreApplication::setApplicationVersion("0.1.0");
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    repaper::registerPaperFonts();
    repaper::installTabletMouseAdapter();
    qmlRegisterType<recalc::MathCanvas>("ReCalc", 1, 0, "MathCanvas");
    qmlRegisterUncreatableType<recalc::Calculator>("ReCalc", 1, 0, "Calculator", "Instance fournie par l’application");
    recalc::Calculator calculator;
    AppLoadViewport appLoadViewport;
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, &app, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors) std::fprintf(stderr, "reCalc: %s\n", qPrintable(error.toString()));
    });
    engine.rootContext()->setContextProperty("calc", &calculator);
    engine.rootContext()->setContextProperty("appLoadViewport", &appLoadViewport);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app, [](QObject *object, const QUrl &) { if (!object) QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl("qrc:/recalc/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    appLoadViewport.setWindow(qobject_cast<QQuickWindow *>(engine.rootObjects().first()));
    if (app.arguments().contains("--demo")) {
        calculator.command("clear");
        for (const auto &key : QStringList{"fraction", "1", "right", "2", "exit", "+", "sqrt", "2"}) calculator.command(key);
        calculator.calculate();
    }
    const auto screenshot = app.arguments().indexOf("--screenshot");
    if (screenshot >= 0 && screenshot + 1 < app.arguments().size()) {
        const auto path = app.arguments()[screenshot + 1];
        QTimer::singleShot(1200, &app, [&engine, path] {
            auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            const bool saved = window && window->grabWindow().save(path);
            std::fprintf(stderr, "reCalc capture %s: %s\n", qPrintable(path), saved ? "OK" : "FAILED");
            QCoreApplication::exit(saved ? 0 : 2);
        });
    }
    return app.exec();
}

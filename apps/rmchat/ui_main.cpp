#include "ChatController.h"
#include "LocalFiles.h"
#include "RichMessage.h"
#include "KeyboardController.h"
#include "TabletMouseAdapter.h"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QPainter>

namespace {
bool saveLayer(const QImage &layer, const QColor &background, const QString &path) {
    if (layer.isNull()) return false;
    QImage frame(layer.size(), QImage::Format_ARGB32_Premultiplied);
    frame.fill(background);
    QPainter painter(&frame);
    painter.drawImage(0, 0, layer);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    painter.fillRect(frame.rect(), background);
    painter.end();
    return frame.save(path);
}
}

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("RePaper");
    QCoreApplication::setApplicationName("rmchat");
    QCoreApplication::setApplicationVersion("0.1.0");
    repaper::installTabletMouseAdapter();
    QCommandLineParser arguments;
    arguments.addHelpOption();
    arguments.addOption({"core", "Absolute path of the private rmchat-core executable.", "path"});
    arguments.addOption({"vault-dir", "Private local vault directory.", "path"});
    arguments.addOption({"screenshot", "Render a disconnected preview, save it and exit.", "path"});
    arguments.addOption({"screenshot-page", "Preview page: main or login.", "page", "main"});
    arguments.process(app);
    const bool preview = arguments.isSet("screenshot");
    QQuickStyle::setStyle("Basic");
    repaper::registerKeyboardTypes();
    rmchat::registerRichTypes();
    rmchat::ChatController chat(arguments.value("core"), arguments.value("vault-dir"), preview);
    rmchat::LocalFiles localFiles;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("chat", &chat);
    engine.rootContext()->setContextProperty("localFiles", &localFiles);
    engine.rootContext()->setContextProperty("previewPage", preview ? arguments.value("screenshot-page") : QString());
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
        [](QObject *object, const QUrl &) { if (!object) QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl("qrc:/rmchat/Main.qml"));
    if (preview) QTimer::singleShot(700, &app, [&] {
        auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!window) { app.exit(2); return; }
        const auto path = arguments.value("screenshot");
        auto background = window->property("color").value<QColor>();
        if (!background.isValid() || background.alpha() != 255) background = QColor("#f8f7f3");
        const auto frame = window->grabWindow();
        if (!frame.isNull()) { app.exit(saveLayer(frame, background, path) ? 0 : 2); return; }
        // Qt 6.2's software/offscreen backend can return a null window grab.
        // The item layer grab still renders the same visible QML scene.
        const auto grab = window->contentItem()->grabToImage();
        if (!grab) { app.exit(2); return; }
        QObject::connect(grab.data(), &QQuickItemGrabResult::ready, &app,
            [grab, path, background, &app] { app.exit(saveLayer(grab->image(), background, path) ? 0 : 2); });
        QTimer::singleShot(3000, &app, [&app] { app.exit(2); });
    });
    return app.exec();
}

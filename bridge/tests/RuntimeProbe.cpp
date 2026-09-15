// Host test entry point; never included in the device runtime.
#include "BridgeClient.h"
#include "BridgeRuntime.h"
#include "BridgeService.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QJsonObject result;
    const auto args = app.arguments();
    if (args.size() == 6 && args[1] == "--ensure") {
        paper::BridgeRuntimeConfig config;
        config.runtime = args[2];
        config.endpoint = args[3];
        config.lock = args[4];
        config.manager = args[5];
        config.unit = "repaper-bridge-host-test.service";
        result = paper::ensureBridgeRuntime(config);
    } else if ((args.size() == 4 || args.size() == 5) && args[1] == "--request") {
        result = repaper::BridgeClient::request(args[2], args[3], args.size() == 5
            ? QJsonDocument::fromJson(args[4].toUtf8()).object() : QJsonObject{});
    } else if (args.size() == 6 && args[1] == "--native-request") {
        BridgeService service(args[2] + "/state", args[2] + "/store", false, false, false, nullptr, args[2] + "/inputs");
        result = service.handle(args[3], args[4], QJsonDocument::fromJson(args[5].toUtf8()).object());
    } else if (args.size() == 4 && args[1] == "--native-serve") {
        BridgeService service(args[2] + "/state", args[2] + "/store", false, false, false, nullptr, args[2] + "/inputs");
        if (!service.listen(args[3])) return 3;
        return app.exec();
    } else if (args.size() == 5 && args[1] == "--import-once") {
        repaper::BridgeClient client;
        int imported = 0;
        QString documentId;
        QObject::connect(&client, &repaper::BridgeClient::imported, &client, [&](const QString &id) { ++imported; documentId = id; });
        QObject::connect(&client, &repaper::BridgeClient::changed, &app, [&] { if (!client.busy()) app.quit(); });
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        client.importFile(args[2], args[3], args[4]); app.exec();
        result = {{"importedSignals", imported}, {"documentId", documentId}, {"message", client.message()}};
    } else if (args.size() == 5 && args[1] == "--notebook-once") {
        repaper::BridgeClient client;
        int imported = 0;
        QString documentId;
        QObject::connect(&client, &repaper::BridgeClient::imported, &client, [&](const QString &id) { ++imported; documentId = id; });
        QObject::connect(&client, &repaper::BridgeClient::changed, &app, [&] { if (!client.busy()) app.quit(); });
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        client.createNotebook(args[2], args[3], QJsonDocument::fromJson(args[4].toUtf8()).object()); app.exec();
        result = {{"importedSignals", imported}, {"documentId", documentId}, {"message", client.message()}};
    } else if (args.size() == 3 && args[1] == "--open-once") {
        repaper::BridgeClient client;
        int imported = 0;
        QObject::connect(&client, &repaper::BridgeClient::imported, &client, [&] { ++imported; });
        QObject::connect(&client, &repaper::BridgeClient::changed, &app, [&] {
            if (!client.busy()) app.quit();
        });
        QTimer::singleShot(5000, &app, &QCoreApplication::quit);
        client.openDocument(args[2]); app.exec();
        result = {{"importedSignals", imported}, {"message", client.message()}};
    } else {
        return 2;
    }
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return result.contains("error") ? 1 : 0;
}

// Process-boundary fixture. Real Office fidelity is checked separately with the
// packaged ARM engine; this helper exercises failure, cancellation, and caching.
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfWriter>
#include <QThread>
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
    QFile input(args.last());
    if (!input.open(QIODevice::ReadOnly))
        return 2;
    const auto data = input.readAll();
    const auto logPath = qEnvironmentVariable("REPAPER_CONVERTER_TEST_LOG");
    QFile log(logPath);
    if (!logPath.isEmpty() && log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QJsonObject call{{"arguments", QJsonArray::fromStringList(args)},
                         {"ldPreload", qEnvironmentVariable("LD_PRELOAD")},
                         {"ldLibraryPath", qEnvironmentVariable("LD_LIBRARY_PATH")},
                         {"qtfbKey", qEnvironmentVariable("QTFB_KEY")}};
        log.write(QJsonDocument(call).toJson(QJsonDocument::Compact) + '\n');
    }
    log.close();
    if (data.contains("REPAPER_TEST_MODE=WAIT"))
        QThread::sleep(30);
    if (data.contains("REPAPER_TEST_MODE=ALWAYS81"))
        return 81;
    if (data.contains("REPAPER_TEST_MODE=ONCE81")) {
        QFile marker(QFileInfo(input).absolutePath() + "/restarted");
        if (!marker.exists()) {
            marker.open(QIODevice::WriteOnly);
            return 81;
        }
    }
    if (data.contains("REPAPER_TEST_MODE=FAIL"))
        return 3;
    const auto index = args.indexOf("--outdir");
    if (index < 0 || index + 1 >= args.size())
        return 4;
    const auto path = args[index + 1] + "/input.pdf";
    if (data.contains("REPAPER_TEST_MODE=INVALID") || data.contains("REPAPER_TEST_MODE=HUGE")) {
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly))
            return 5;
        output.write(data.contains("REPAPER_TEST_MODE=HUGE") ? "%PDF-1.7\n" : "<html>Not a PDF</html>");
        if (data.contains("REPAPER_TEST_MODE=HUGE")) {
            output.seek(64 * 1024 * 1024);
            output.write("x");
        }
        return 0;
    }
    QPdfWriter writer(path);
    QPainter painter(&writer);
    painter.drawText(QPoint(100, 100), "Synthetic converted PDF");
    return 0;
}

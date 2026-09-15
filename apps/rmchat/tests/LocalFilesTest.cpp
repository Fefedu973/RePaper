#include "LocalFiles.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
bool writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QJsonObject readRecord(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
class Environment {
public:
    void set(const QByteArray &name, const QByteArray &value) {
        if (!previous.contains(name)) previous.insert(name, qgetenv(name));
        qputenv(name.constData(), value);
    }
    ~Environment() {
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (it.value().isNull()) qunsetenv(it.key().constData());
            else qputenv(it.key().constData(), it.value());
        }
    }
private:
    QMap<QByteArray, QByteArray> previous;
};

// Copies of this executable stand in for both launchers. No Python, shell,
// browser or clipboard program is executed, regardless of the payload.
int fakeLauncher(QCoreApplication &app) {
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return 70;
    const auto bytes = input.readAll();
    const auto recordPath = qEnvironmentVariable("RMCHAT_LOCALFILES_RECORD");
    const auto previous = readRecord(recordPath);
    const auto args = app.arguments();
    QJsonObject record{{"program", QFileInfo(args.first()).fileName()},
        {"arguments", QJsonArray::fromStringList(args.mid(1))},
        {"stdinBase64", QString::fromLatin1(bytes.toBase64())},
        {"starts", previous.value("starts").toInt() + 1},
        {"preloadPresent", qEnvironmentVariableIsSet("LD_PRELOAD")},
        {"qtfbPresent", qEnvironmentVariableIsSet("QTFB_KEY") || qEnvironmentVariableIsSet("QTFB_TEST_VALUE")}};
    if (!writeFile(recordPath, QJsonDocument(record).toJson(QJsonDocument::Compact))) return 71;
    const int hold = qEnvironmentVariableIntValue("RMCHAT_LOCALFILES_HOLD_MS");
    if (hold > 0) { QTimer::singleShot(qMin(hold, 1000), &app, &QCoreApplication::quit); app.exec(); }
    return qEnvironmentVariableIntValue("RMCHAT_LOCALFILES_EXIT");
}
struct Fixture {
    QTemporaryDir directory;
    Environment environment;
    QString bin, helper, record;
    bool ready = false;
    Fixture() {
        if (!directory.isValid()) return;
        bin = directory.filePath("bin");
        helper = directory.filePath("pc-handoff-fixture.py");
        record = directory.filePath("record.json");
        if (!QDir().mkpath(bin) || !writeFile(helper, "# LocalFiles test fixture: never executed\n")) return;
        for (const auto &name : QStringList{"powershell.exe", "python3"}) {
            const auto target = QDir(bin).filePath(name);
            if (!QFile::copy(QCoreApplication::applicationFilePath(), target) ||
                !QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) return;
        }
        // Keep real executables out of PATH rather than merely placing fakes first.
        environment.set("PATH", bin.toUtf8());
        environment.set("REPAPER_PC_EMULATOR", "1");
        environment.set("REPAPER_PC_HANDOFF_HELPER", helper.toUtf8());
        environment.set("RMCHAT_LOCALFILES_FAKE", "1");
        environment.set("RMCHAT_LOCALFILES_RECORD", record.toUtf8());
        environment.set("RMCHAT_LOCALFILES_HOLD_MS", "0");
        environment.set("RMCHAT_LOCALFILES_EXIT", "0");
        environment.set("LD_PRELOAD", "/nonexistent/localfiles-fixture.so");
        environment.set("QTFB_KEY", "fixture-framebuffer");
        environment.set("QTFB_TEST_VALUE", "fixture-framebuffer-extra");
        ready = true;
    }
    QJsonObject captured() const { return readRecord(record); }
    QByteArray input() const { return QByteArray::fromBase64(captured().value("stdinBase64").toString().toLatin1()); }
};
}

class LocalFilesTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#ifdef Q_OS_WIN
        QSKIP("These tests cover the WSL PC integration branch using isolated executable fixtures.");
#endif
        QStandardPaths::setTestModeEnabled(true);
    }
    void opensHttpsUsingJsonStdin_data() {
        QTest::addColumn<QString>("url");
        QTest::newRow("plain") << QString("https://example.invalid/course");
        QTest::newRow("unicode-and-shell-characters") << QString::fromUtf8("https://example.invalid/étude?q=$(fixture)&tick=%60value%60#章节");
        const QString prefix = "https://example.invalid/";
        QTest::newRow("4096-encoded-characters") << prefix + QString(4096 - prefix.size(), 'a');
    }
    void opensHttpsUsingJsonStdin() {
        QFETCH(QString, url);
        Fixture fixture; QVERIFY(fixture.ready);
        rmchat::LocalFiles files; QVERIFY(files.pcIntegration());
        QSignalSpy changed(&files, &rmchat::LocalFiles::changed);
        const QUrl requested(url, QUrl::StrictMode);
        QVERIFY(requested.isValid());
        files.openLink(requested);
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 3000);
        const auto record = fixture.captured();
        QCOMPARE(record.value("program").toString(), QString("python3"));
        QCOMPARE(record.value("arguments").toArray(), (QJsonArray{fixture.helper, "open"}));
        QCOMPARE(record.value("starts").toInt(), 1);
        QVERIFY(!record.value("preloadPresent").toBool()); QVERIFY(!record.value("qtfbPresent").toBool());
        const auto payload = QJsonDocument::fromJson(fixture.input()).object();
        QCOMPARE(payload.keys(), QStringList{"url"});
        QCOMPARE(QUrl(payload.value("url").toString()), requested);
        QCOMPARE(files.message(), QString::fromUtf8("Page ouverte dans le navigateur PC."));
    }
    void refusesInvalidLinks_data() {
        QTest::addColumn<QString>("url");
        for (const auto &url : QStringList{"http://example.invalid/", "file:///tmp/fixture", "javascript:fixture()",
                 "data:text/plain,fixture", "https:///missing-host", "https://name@example.invalid/",
                 "https://name:password@example.invalid/", "https://:password@example.invalid/", "https://example.invalid/%zz"})
            QTest::newRow(qPrintable(url)) << url;
        const QString prefix = "https://example.invalid/";
        QTest::newRow("4097-encoded-characters") << prefix + QString(4097 - prefix.size(), 'a');
        QTest::newRow("unicode-encoding-exceeds-limit") << prefix + QString(1000, QChar(0x00e9));
    }
    void refusesInvalidLinks() {
        QFETCH(QString, url);
        Fixture fixture; QVERIFY(fixture.ready);
        rmchat::LocalFiles files;
        files.openLink(QUrl(url, QUrl::StrictMode));
        QVERIFY(files.findChildren<QProcess *>().isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.record)); QVERIFY(files.message().isEmpty());
    }
    void opensOnlyConversationUuidOrRoot_data() {
        QTest::addColumn<QString>("id"); QTest::addColumn<QString>("expected");
        QTest::newRow("root") << QString() << QString("https://chatgpt.com/");
        QTest::newRow("uuid") << QString("b7eb1780-1ce8-459a-8fea-c58e7bfa1d07")
            << QString("https://chatgpt.com/c/b7eb1780-1ce8-459a-8fea-c58e7bfa1d07");
    }
    void opensOnlyConversationUuidOrRoot() {
        QFETCH(QString, id); QFETCH(QString, expected);
        Fixture fixture; QVERIFY(fixture.ready);
        rmchat::LocalFiles files;
        QSignalSpy changed(&files, &rmchat::LocalFiles::changed);
        files.openConversation(id);
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 3000);
        QCOMPARE(QJsonDocument::fromJson(fixture.input()).object().value("url").toString(), expected);
        QCOMPARE(fixture.captured().value("arguments").toArray(), (QJsonArray{fixture.helper, "open"}));
    }
    void rejectsOtherConversationIds_data() {
        QTest::addColumn<QString>("id");
        for (const auto &id : QStringList{"fixture-slug", "a", "../fixture", "https://example.invalid/", "bad?query",
                 "b7eb1780-1ce8-459a-8fea-c58e7bfa1d07/extra", "{b7eb1780-1ce8-459a-8fea-c58e7bfa1d07}"})
            QTest::newRow(qPrintable(id)) << id;
    }
    void rejectsOtherConversationIds() {
        QFETCH(QString, id);
        Fixture fixture; QVERIFY(fixture.ready);
        rmchat::LocalFiles files;
        files.openConversation(id);
        QVERIFY(files.findChildren<QProcess *>().isEmpty()); QVERIFY(!QFileInfo::exists(fixture.record));
    }
    void clipboardPayloadTravelsOnlyOnStdin_data() {
        QTest::addColumn<QString>("text");
        QTest::newRow("utf8-and-literal-shell-syntax") << QString::fromUtf8("Résumé 中文 🧪\r\n`backtick` $(Write-Output 'fixture-only')\n\"quoted\"; $variable\\path");
        QTest::newRow("empty") << QString();
        QTest::newRow("ascii-byte-boundary") << QString(256 * 1024, 'x');
        QTest::newRow("utf8-byte-boundary") << QString(128 * 1024, QChar(0x00e9));
    }
    void clipboardPayloadTravelsOnlyOnStdin() {
        QFETCH(QString, text);
        Fixture fixture; QVERIFY(fixture.ready);
        rmchat::LocalFiles files;
        QSignalSpy changed(&files, &rmchat::LocalFiles::changed);
        files.copyText(text);
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 3000);
        QCOMPARE(fixture.input(), text.toUtf8());
        const auto record = fixture.captured();
        QCOMPARE(record.value("program").toString(), QString("powershell.exe"));
        const auto arguments = record.value("arguments").toArray();
        QCOMPARE(arguments.size(), 7);
        QCOMPARE(arguments.at(0).toString(), QString("-NoLogo"));
        QCOMPARE(arguments.at(1).toString(), QString("-NoProfile"));
        QCOMPARE(arguments.at(2).toString(), QString("-NonInteractive"));
        QCOMPARE(arguments.at(3).toString(), QString("-WindowStyle"));
        QCOMPARE(arguments.at(4).toString(), QString("Hidden"));
        QCOMPARE(arguments.at(5).toString(), QString("-Command"));
        const auto script = arguments.last().toString();
        QVERIFY(script.contains("[Console]::In.ReadToEnd()")); QVERIFY(script.contains("Set-Clipboard -Value $v"));
        QVERIFY(script.contains("UTF8Encoding")); QVERIFY(!script.contains("fixture-only"));
        QVERIFY(!QJsonDocument(arguments).toJson().contains("backtick"));
        QVERIFY(!record.value("preloadPresent").toBool()); QVERIFY(!record.value("qtfbPresent").toBool());
        QCOMPARE(files.message(), QString::fromUtf8("Texte copié sur le PC."));
    }
    void rejectsClipboardBytesAboveLimit_data() {
        QTest::addColumn<QString>("text");
        QTest::newRow("ascii") << QString(256 * 1024 + 1, 'x');
        QTest::newRow("utf8") << QString(128 * 1024 + 1, QChar(0x00e9));
    }
    void rejectsClipboardBytesAboveLimit() {
        QFETCH(QString, text);
        Fixture fixture; QVERIFY(fixture.ready);
        rmchat::LocalFiles files;
        files.copyText(text);
        QVERIFY(files.findChildren<QProcess *>().isEmpty()); QVERIFY(!QFileInfo::exists(fixture.record));
    }
    void clipboardCoalescesWhileBusyAndCanRunAgain() {
        Fixture fixture; QVERIFY(fixture.ready);
        fixture.environment.set("RMCHAT_LOCALFILES_HOLD_MS", "150");
        rmchat::LocalFiles files;
        QSignalSpy changed(&files, &rmchat::LocalFiles::changed);
        files.copyText("first fixture"); files.copyText("ignored while busy");
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 3000);
        QCOMPARE(fixture.input(), QByteArray("first fixture"));
        QCOMPARE(fixture.captured().value("starts").toInt(), 1);
        changed.clear(); files.copyText("second fixture");
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 3000);
        QCOMPARE(fixture.input(), QByteArray("second fixture"));
        QCOMPARE(fixture.captured().value("starts").toInt(), 2);
    }
    void launcherFailureUsesFixedUserMessage() {
        Fixture fixture; QVERIFY(fixture.ready);
        fixture.environment.set("RMCHAT_LOCALFILES_EXIT", "1");
        rmchat::LocalFiles files;
        QSignalSpy changed(&files, &rmchat::LocalFiles::changed);
        files.copyText("private-fixture-text");
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 3000);
        QVERIFY(!files.message().contains("private-fixture-text"));
        QVERIFY(files.message().contains(QString::fromUtf8("n’a pas pu")));
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("RMCHAT_LOCALFILES_FAKE") == "1") return fakeLauncher(app);
    LocalFilesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "LocalFilesTest.moc"

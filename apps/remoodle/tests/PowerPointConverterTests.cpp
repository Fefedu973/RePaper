#include "PowerPointConverter.h"
#include "PowerPointPackage.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QtEndian>

class PowerPointConverterTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString source, logPath;
    QByteArray original;
    void input(const QByteArray &mode = "OK") {
        QFile fixture(QFINDTESTDATA("fixtures/circuits.pptx"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        original = fixture.readAll();
        // A ZIP archive comment controls the process fixture without changing
        // the valid presentation's slide/package parts.
        const auto comment = QByteArray("REPAPER_TEST_MODE=") + mode;
        const int end = original.lastIndexOf("PK\005\006");
        QVERIFY(end >= 0);
        original[end + 20] = char(comment.size());
        original[end + 21] = '\0';
        original += comment;
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), original.size());
    }
    QByteArray contents(const QString &path) const {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    QList<QJsonObject> calls() const {
        QList<QJsonObject> out;
        for (const auto &line : contents(logPath).split('\n'))
            if (!line.isEmpty())
                out << QJsonDocument::fromJson(line).object();
        return out;
    }
  private slots:
    void init() {
        QVERIFY(directory.isValid());
        source = directory.filePath("original.download");
        logPath = directory.filePath("calls.jsonl");
        QFile::remove(logPath);
        qputenv("REPAPER_CONVERTER_TEST_LOG", logPath.toUtf8());
        qputenv("REPAPER_OFFICE_CONVERTER", QByteArray(OFFICE_FIXTURE));
        input();
    }
    void cleanup() {
        qunsetenv("REPAPER_OFFICE_CONVERTER");
        qunsetenv("REPAPER_CONVERTER_TEST_LOG");
        qunsetenv("LD_PRELOAD");
        qunsetenv("LD_LIBRARY_PATH");
        qunsetenv("QTFB_KEY");
    }
    void types_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("mime");
        QTest::addColumn<QString>("suffix");
        QTest::newRow("ppt") << "Cours.ppt" << "" << "ppt";
        QTest::newRow("pptx") << "Cours.PPTX" << "application/octet-stream" << "pptx";
        QTest::newRow("legacy mime") << "Cours" << "application/vnd.ms-powerpoint" << "ppt";
        QTest::newRow("xml mime") << "Cours" << "application/vnd.openxmlformats-officedocument.presentationml.presentation" << "pptx";
        QTest::newRow("macro") << "Cours.pptm" << "application/vnd.ms-powerpoint.presentation.macroEnabled.12" << "";
        QTest::newRow("pdf") << "Cours.pdf" << "application/pdf" << "";
        QTest::newRow("double extension") << "Cours.pptx.html" << "text/html" << "";
    }
    void types() {
        QFETCH(QString, name); QFETCH(QString, mime); QFETCH(QString, suffix);
        QCOMPARE(PowerPointConverter::extension(name, mime), suffix);
    }
    void rejectsHtmlBeforeStartingAnyProcess() {
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("<html>Login expired</html>");
        file.close();
        PowerPointConverter converter;
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(!converter.convert(source, "pptx", source));
        QCOMPARE(failed.count(), 1);
        QVERIFY(calls().isEmpty());
        QCOMPARE(contents(source), QByteArray("<html>Login expired</html>"));
    }
    void missingEnginePreservesOriginal() {
        qputenv("REPAPER_OFFICE_CONVERTER", directory.filePath("missing").toUtf8());
        PowerPointConverter converter;
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(!converter.convert(source, "pptx", source));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(contents(source), original);
    }
    void nativeEquationWithoutCompatibilityPreviewIsRejectedPrecisely() {
        const auto bad = QFINDTESTDATA("fixtures/circuits-equation-no-preview.pptx");
        const auto good = QFINDTESTDATA("fixtures/circuits-equation.pptx");
        QVERIFY(!bad.isEmpty());
        QVERIFY(!good.isEmpty());
        QVERIFY(validatePowerPointPackage(bad).contains("équation Office sans aperçu"));
        QCOMPARE(validatePowerPointPackage(good), QString());
        PowerPointConverter converter;
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(!converter.convert(bad, "pptx", source));
        QCOMPARE(failed.count(), 1);
        QVERIFY(calls().isEmpty());
        QCOMPARE(contents(source), original);
    }
    void malformedPackageNeverStartsTheEngine_data() {
        QTest::addColumn<QString>("damage");
        QTest::newRow("truncated archive") << QString("truncate");
        QTest::newRow("wrong XML checksum") << QString("checksum");
        QTest::newRow("unbounded expansion") << QString("expansion");
    }
    void malformedPackageNeverStartsTheEngine() {
        QFETCH(QString, damage);
        auto damaged = original;
        if (damage == "truncate") damaged.chop(64);
        else {
            const int end = damaged.lastIndexOf("PK\005\006");
            const auto central = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(damaged.constData() + end + 16));
            if (damage == "checksum") {
                const int typeName = damaged.indexOf("[Content_Types].xml", int(central));
                QVERIFY(typeName >= 46);
                const int metadata = typeName - 46;
                QCOMPARE(damaged.mid(metadata, 4), QByteArray("PK\001\002"));
                damaged[metadata + 16] = char(damaged[metadata + 16] ^ 1);
            }
            else qToLittleEndian<quint32>(257 * 1024 * 1024, reinterpret_cast<uchar *>(damaged.data() + central + 24));
        }
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(damaged), damaged.size());
        file.close();
        PowerPointConverter converter;
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(!converter.convert(source, "pptx", source));
        QCOMPARE(failed.count(), 1);
        QVERIFY(calls().isEmpty());
        QCOMPARE(contents(source), damaged);
    }
    void convertsAtomicallyWithPrivateEnvironmentAndProfile() {
        qputenv("LD_PRELOAD", "not-the-appload-shim.so");
        qputenv("LD_LIBRARY_PATH", "/not-the-appload-qt");
        qputenv("QTFB_KEY", "123");
        PowerPointConverter converter;
        QSignalSpy done(&converter, &PowerPointConverter::converted);
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(converter.convert(source, "pptx", source));
        QCOMPARE(contents(source), original);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(contents(source).startsWith("%PDF-"));
        QCOMPARE(done.first().first().toString(), source);
        QCOMPARE(calls().size(), 1);
        const auto call = calls().first();
        QCOMPARE(call["ldPreload"].toString(), QString());
        QCOMPARE(call["ldLibraryPath"].toString(), QString());
        QCOMPARE(call["qtfbKey"].toString(), QString());
        QStringList args;
        for (auto value : call["arguments"].toArray())
            args << value.toString();
        QVERIFY(args.contains("--infilter=Impress MS PowerPoint 2007 XML"));
        QVERIFY(args.contains("--headless"));
        QVERIFY(!QFileInfo::exists(QFileInfo(args.last()).absolutePath()));
        QVERIFY(!converter.busy());
    }
    void failuresDoNotReplaceTheOriginal_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("exit error") << QByteArray("FAIL");
        QTest::newRow("html output") << QByteArray("INVALID");
        QTest::newRow("oversized PDF") << QByteArray("HUGE");
        QTest::newRow("restart loop") << QByteArray("ALWAYS81");
    }
    void failuresDoNotReplaceTheOriginal() {
        QFETCH(QByteArray, mode); input(mode);
        PowerPointConverter converter;
        QSignalSpy done(&converter, &PowerPointConverter::converted);
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(converter.convert(source, "pptx", source));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QCOMPARE(done.count(), 0);
        QCOMPARE(contents(source), original);
        QVERIFY(!converter.busy());
        if (mode == "ALWAYS81") QCOMPARE(calls().size(), 2);
    }
    void freshProfileRestartCompletesOnce() {
        input("ONCE81");
        PowerPointConverter converter;
        QSignalSpy done(&converter, &PowerPointConverter::converted);
        QVERIFY(converter.convert(source, "pptx", source));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QCOMPARE(calls().size(), 2);
        QVERIFY(contents(source).startsWith("%PDF-"));
    }
    void cancellationKeepsOriginalAndAllowsRetry() {
        input("WAIT");
        PowerPointConverter converter;
        QSignalSpy done(&converter, &PowerPointConverter::converted);
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(converter.convert(source, "pptx", source));
        QTRY_COMPARE_WITH_TIMEOUT(calls().size(), 1, 3000);
        QVERIFY(!converter.convert(source, "pptx", source));
        converter.cancel();
        QCOMPARE(contents(source), original);
        QCOMPARE(done.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!converter.busy());
        input();
        QVERIFY(converter.convert(source, "pptx", source));
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
    }
    void timeoutTerminatesTheProcessWithoutReplacingTheOriginal() {
        input("WAIT");
        PowerPointConverter converter(nullptr, 300);
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(converter.convert(source, "pptx", source));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QVERIFY(failed.first().first().toString().contains("trop de temps"));
        QCOMPARE(contents(source), original);
        QVERIFY(!converter.busy());
    }
    void realEngineConvertsPptxPptAndOfficeMath_data() {
        QTest::addColumn<QString>("filename");
        QTest::newRow("pptx text and graph") << QString("circuits.pptx");
        QTest::newRow("binary ppt text and graph") << QString("circuits.ppt");
        QTest::newRow("pptx native Office equation") << QString("circuits-equation.pptx");
    }
    void realEngineConvertsPptxPptAndOfficeMath() {
        const auto engine = qEnvironmentVariable("REPAPER_OFFICE_REAL_CONVERTER");
        if (engine.isEmpty()) QSKIP("Set REPAPER_OFFICE_REAL_CONVERTER to test the real packaged engine.");
        QFETCH(QString, filename);
        const auto fixture = QFINDTESTDATA("fixtures/" + filename);
        QVERIFY(!fixture.isEmpty());
        QFile::remove(source);
        QVERIFY(QFile::copy(fixture, source));
        qputenv("REPAPER_OFFICE_CONVERTER", engine.toUtf8());
        PowerPointConverter converter;
        QSignalSpy done(&converter, &PowerPointConverter::converted);
        QSignalSpy failed(&converter, &PowerPointConverter::failed);
        QVERIFY(converter.convert(source, QFileInfo(filename).suffix(), source));
        QTRY_VERIFY_WITH_TIMEOUT(done.count() + failed.count() > 0, 180000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(done.count(), 1);
        QVERIFY(contents(source).startsWith("%PDF-"));
        const auto artifacts = qEnvironmentVariable("REPAPER_OFFICE_TEST_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            const auto path = artifacts + "/" + filename + ".pdf";
            QFile::remove(path);
            QVERIFY(QFile::copy(source, path));
        }
    }
};
QTEST_GUILESS_MAIN(PowerPointConverterTests)
#include "PowerPointConverterTests.moc"

#include "app/Calculator.hpp"
#include "layout/MathCanvas.hpp"
#include <QFontMetricsF>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTemporaryDir>
#include <QtTest>

using namespace recalc;
class CanvasTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
private slots:
    void initTestCase() {
        QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8()); qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        QCoreApplication::setOrganizationName("RePaperTest"); QCoreApplication::setApplicationName("recalc_ui_test");
        qmlRegisterType<MathCanvas>("ReCalc", 1, 0, "MathCanvas");
        qmlRegisterUncreatableType<Calculator>("ReCalc", 1, 0, "Calculator", "Test instance");
    }
    void touchFractionSlots() {
        Calculator calc; calc.command("clear");
        for (const auto &key : QStringList{"fraction", "1", "down", "2", "exit"}) calc.command(key);
        auto fraction = calc.editor().root()->children[0]; QCOMPARE(fraction->kind, Kind::Fraction);
        QQuickWindow window; window.resize(500, 300);
        MathCanvas canvas(window.contentItem()); canvas.setWidth(500); canvas.setHeight(300); canvas.setFontSize(40); canvas.setCalculator(&calc);
        window.show(); QTest::qWait(50);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(43, 111));
        QCOMPARE(calc.editor().cursor().rowId, fraction->children[1]->id);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(43, 47));
        QCOMPARE(calc.editor().cursor().rowId, fraction->children[0]->id);
    }
    void numberHitAndViewportCoordinates() {
        Calculator calc; calc.command("clear"); for (const auto &key : QStringList{"1", "2", "3", "4", "5", "6", "7", "8"}) calc.command(key);
        QQuickWindow window; window.resize(300, 160);
        MathCanvas canvas(window.contentItem()); canvas.setWidth(180); canvas.setHeight(160); canvas.setFontSize(40); canvas.setCalculator(&calc);
        window.show(); QTest::qWait(50);
        QFont font("DejaVu Sans"); font.setPixelSize(40); const QFontMetricsF metrics(font);
        canvas.setViewportX(60);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(qRound(28 + metrics.horizontalAdvance("1234") - 60), 45));
        QCOMPARE(calc.editor().cursor().digit, 4);
        QCOMPARE(canvas.width(), 180.); QVERIFY(canvas.implicitWidth() > canvas.width());
        calc.command("9"); QCOMPARE(asText(calc.editor().root()), QString("123495678"));
    }
    void asynchronousEvaluationAndHistoryRecall() {
        Calculator calc; calc.command("clear");
        for (const auto &key : QStringList{"fraction", "1", "down", "2"}) calc.command(key);
        QSignalSpy historySignal(&calc, &Calculator::historyChanged);
        calc.calculate(); QTRY_COMPARE_WITH_TIMEOUT(historySignal.count(), 1, 2000);
        QCOMPARE(calc.result(), QString("= 1 / 2"));
        calc.command("MS"); QVERIFY(calc.hasMemory());
        const auto entries = calc.history(); QVERIFY(!entries.isEmpty());
        const auto id = entries.first().toMap().value("id").toString();
        const auto original = asLatex(calc.editor().root());
        calc.command("clear"); calc.loadHistory(id); QCOMPARE(asLatex(calc.editor().root()), original);
        calc.command("clear"); calc.command("M"); calc.calculate();
        QTRY_COMPARE_WITH_TIMEOUT(historySignal.count(), 2, 2000); QCOMPARE(calc.result(), QString("= 1 / 2"));
    }
    void helpDialogHasNoBindingLoop() {
        Calculator calc; QStringList bindingWarnings; QQmlApplicationEngine engine;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &warnings) {
            for (const auto &warning : warnings) if (warning.description().contains("Binding loop")) bindingWarnings.append(warning.toString());
        });
        engine.rootContext()->setContextProperty("calc", &calc); engine.load(QUrl("qrc:/recalc/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first()); QVERIFY(window);
        auto dialog = window->findChild<QObject *>("helpDialog"); QVERIFY(dialog);
        for (const auto &size : QList<QSize>{{800, 1066}, {1620, 2160}, {1000, 1300}}) {
            window->resize(size); QVERIFY(QMetaObject::invokeMethod(dialog, "open")); QTest::qWait(30);
            QVERIFY(dialog->property("height").toDouble() > 0); QVERIFY(dialog->property("height").toDouble() < window->height());
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        }
        QVERIFY2(bindingWarnings.isEmpty(), qPrintable(bindingWarnings.join('\n')));
    }
};
QTEST_MAIN(CanvasTests)
#include "CanvasTests.moc"

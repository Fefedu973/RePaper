#include "expression/Editor.hpp"
#include "evaluation/Evaluator.hpp"
#include "history/HistoryStore.hpp"
#include <QJsonArray>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace recalc;
static void digits(Editor &editor, const QString &s) { for (QChar c : s) QVERIFY(editor.insertDigit(QString(c))); }
static NodePtr number(const QString &text) { auto row = makeRow(); row->children.append(makeNode(Kind::Number, text)); return row; }
static NodePtr fraction(const QString &n, const QString &d) {
    auto row = makeRow(); auto value = makeNode(Kind::Fraction); value->children = {number(n), number(d)}; row->children.append(value); return row;
}
class CalculatorTests : public QObject {
    Q_OBJECT
private slots:
    void exactRationals() {
        auto row = fraction("1", "3"); row->children.append(makeNode(Kind::Operator, "+")); row->children.append(fraction("1", "6")->children.first());
        auto result = evaluate(row); QVERIFY2(result.ok, qPrintable(result.error)); QVERIFY(result.value.exact);
        QCOMPARE(result.value.numerator.convert_to<int>(), 1); QCOMPARE(result.value.denominator.convert_to<int>(), 2);
        QCOMPARE(result.display, QString("= 1 / 2")); QCOMPARE(result.approximation, QString("≈ 0,5"));
    }
    void decimalsAreExactAndDecimalRadix() {
        Editor e; digits(e, "0.1"); e.insertOperator("+"); digits(e, "0.2");
        auto result = evaluate(e.root()); QVERIFY(result.ok); QCOMPARE(result.display, QString("= 3 / 10"));
        QCOMPARE(evaluate(number("008")).display, QString("= 8"));
        QCOMPARE(evaluate(number("0009.00")).display, QString("= 9"));
    }
    void arithmeticPrecedence() {
        Editor e; digits(e, "2"); e.insertOperator("+"); digits(e, "3"); e.insertOperator("*"); digits(e, "4");
        QCOMPARE(evaluate(e.root()).display, QString("= 14"));
        e.clear(); e.insertOperator("-"); digits(e, "2"); e.insertPower(); digits(e, "2");
        QCOMPARE(evaluate(e.root()).display, QString("= -4"));
        e.clear(); digits(e, "2"); e.insertGroup(); digits(e, "3"); e.insertOperator("+"); digits(e, "4");
        QCOMPARE(evaluate(e.root()).display, QString("= 14"));
    }
    void structuredEditAndUndo() {
        Editor e; digits(e, "12"); QVERIFY(e.insertFraction());
        QCOMPARE(e.root()->children.first()->kind, Kind::Fraction);
        QCOMPARE(e.cursor().rowId, e.root()->children.first()->children[1]->id);
        digits(e, "3"); QCOMPARE(evaluate(e.root()).display, QString("= 4"));
        QVERIFY(e.move(-2)); QVERIFY(e.insertDigit("6")); QCOMPARE(evaluate(e.root()).display, QString("= 42"));
        QVERIFY(e.undo()); QCOMPARE(evaluate(e.root()).display, QString("= 4"));
        QVERIFY(e.redo()); QCOMPARE(evaluate(e.root()).display, QString("= 42"));
        QVERIFY(e.move(2)); QVERIFY(e.leave());
        QCOMPARE(e.cursor().rowId, e.root()->id); QCOMPARE(e.cursor().index, 1);
    }
    void numberCaretAndDelete() {
        Editor e; digits(e, "123"); e.move(-1); // Inside 12|3.
        QCOMPARE(e.cursor().digit, 2); e.insertDigit("4"); QCOMPARE(asText(e.root()), QString("1243"));
        e.backspace(); QCOMPARE(asText(e.root()), QString("123"));
        e.deleteForward(); QCOMPARE(asText(e.root()), QString("12"));
        e.clear(); e.insertFraction(); digits(e, "7"); e.move(2); e.backspace();
        QCOMPARE(asText(e.root()), QString("7")); QVERIFY(validTree(e.root()));
    }
    void powersAndRoots() {
        Editor e; e.insertRoot(); digits(e, "4"); QCOMPARE(evaluate(e.root()).display, QString("= 2"));
        e.clear(); e.insertRoot(); digits(e, "2"); auto r = evaluate(e.root()); QVERIFY(r.ok); QVERIFY(!r.value.exact); QVERIFY(r.display.startsWith("≈ "));
        e.clear(); e.insertRoot(3); e.insertOperator("-"); digits(e, "8"); QCOMPARE(evaluate(e.root()).display, QString("= -2"));
        e.clear(); digits(e, "2"); e.insertPower(); e.insertOperator("-"); digits(e, "3"); QCOMPARE(evaluate(e.root()).display, QString("= 1 / 8"));
        e.clear(); digits(e, "0"); e.insertPower(); digits(e, "0"); QVERIFY(!evaluate(e.root()).ok);
    }
    void trigModesAndDomains() {
        Editor e; e.insertFunction("sin"); digits(e, "90"); EvalContext degrees; degrees.degrees = true;
        auto d = evaluate(e.root(), degrees); QVERIFY(d.ok); QVERIFY(std::abs(double(d.value.approximate) - 1.) < 1e-12); QVERIFY(!d.value.exact);
        EvalContext radians; radians.degrees = false; auto r = evaluate(e.root(), radians); QVERIFY(r.ok); QVERIFY(std::abs(double(r.value.approximate) - std::sin(90.)) < 1e-12);
        e.clear(); e.insertFunction("tan"); digits(e, "90"); QVERIFY(!evaluate(e.root(), degrees).ok);
        e.clear(); e.insertFunction("ln"); digits(e, "0"); QVERIFY(!evaluate(e.root()).ok);
        e.clear(); e.insertRoot(); e.insertOperator("-"); digits(e, "1"); QVERIFY(!evaluate(e.root()).ok);
        QVERIFY(!evaluate(fraction("1", "0")).ok);
    }
    void budgetsAndMalformedTrees() {
        Editor e; digits(e, "2"); e.insertPower(); digits(e, "10000"); auto result = evaluate(e.root()); QVERIFY(!result.ok); QVERIFY(result.error.contains("4096"));
        e.clear(); for (int i = 0; i < 100; ++i) e.insertRoot(); QVERIFY(validTree(e.root())); QVERIFY(!e.error().isEmpty());
        auto json = toJson(number("5")); json["version"] = 99; QVERIFY(!fromJson(json));
        QJsonObject invalid{{"version", 1}, {"root", QJsonObject{{"kind", 0}, {"children", QJsonArray{QJsonObject{{"kind", 4}, {"children", QJsonArray{}}}}}}}};
        QVERIFY(!fromJson(invalid));
        auto malicious = number("1;system('anything')"); QVERIFY(!evaluate(malicious).ok);
        Value value; QVERIFY(!Value::read({{"exact", true}, {"n", "1"}, {"d", "0"}}, value));
    }
    void ansMemoryAndRoundtrip() {
        Editor e; e.insertSymbol("Ans"); QVERIFY(!evaluate(e.root()).ok);
        EvalContext ctx; ctx.answer.numerator = 7; ctx.hasAnswer = true; QCOMPARE(evaluate(e.root(), ctx).display, QString("= 7"));
        e.clear(); e.insertSymbol("M"); QVERIFY(!evaluate(e.root(), ctx).ok); ctx.memory.numerator = 3; ctx.memory.denominator = 2; ctx.hasMemory = true;
        QCOMPARE(evaluate(e.root(), ctx).display, QString("= 3 / 2"));
        Value loaded; QVERIFY(Value::read(ctx.memory.json(), loaded)); QCOMPARE(formatValue(loaded), QString("= 3 / 2"));
        e.clear(); e.insertRoot(); e.insertFraction(); digits(e, "1"); e.move(2); digits(e, "2");
        auto parsed = fromJson(toJson(e.root())); QVERIFY(parsed); QCOMPARE(asLatex(parsed), asLatex(e.root())); QCOMPARE(evaluate(parsed).display, evaluate(e.root()).display);
    }
    void boundedRandomEditingKeepsCursorValid() {
        QRandomGenerator random(0xC0FFEE); Editor editor;
        for (int i = 0; i < 2500; ++i) {
            switch(random.bounded(13)) {
            case 0: editor.insertDigit(QString::number(random.bounded(10))); break;
            case 1: editor.insertFraction(); break;
            case 2: editor.insertPower(); break;
            case 3: editor.insertRoot(); break;
            case 4: editor.backspace(); break;
            case 5: editor.deleteForward(); break;
            case 6: editor.move(-1); break;
            case 7: editor.move(1); break;
            case 8: editor.move(-2); break;
            case 9: editor.move(2); break;
            case 10: editor.undo(); break;
            case 11: editor.redo(); break;
            case 12: editor.insertOperator("+"); break;
            }
            QVERIFY(validTree(editor.root())); QVERIFY(cursorStops(editor.root()).contains(editor.cursor()));
        }
    }
    void sqlitePersistenceAndHistoryBound() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.path() + "/history.sqlite3";
        {
            HistoryStore store(path); QVERIFY2(store.ready(), qPrintable(store.error())); auto root = fraction("1", "7");
            QVERIFY(store.save(root, evaluate(root), true)); auto entries = store.entries(); QCOMPARE(entries.size(), 1);
            auto restored = store.load(entries.first().toMap().value("id").toString()); QVERIFY(restored); QCOMPARE(asLatex(restored), asLatex(root));
        }
        HistoryStore reopened(path); QCOMPARE(reopened.entries().size(), 1);
        for (int i = 0; i < 205; ++i) { auto root = number(QString::number(i)); QVERIFY(reopened.save(root, evaluate(root), false)); }
        QCOMPARE(reopened.entries().size(), 200);
    }
};
QTEST_GUILESS_MAIN(CalculatorTests)
#include "CalculatorTests.moc"

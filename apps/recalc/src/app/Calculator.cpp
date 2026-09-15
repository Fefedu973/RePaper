#include "Calculator.hpp"
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent>

namespace recalc {
static QString storageDirectory() {
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory); return directory;
}
Calculator::Calculator(QObject *parent) : QObject(parent), m_settings(), m_history(storageDirectory() + "/history.sqlite3") {
    m_context.degrees = m_settings.value("angleDegrees", true).toBool();
    m_context.precision = std::clamp(m_settings.value("precision", 12).toInt(), 6, 15);
    m_context.hasAnswer = Value::read(QJsonDocument::fromJson(m_settings.value("answer").toByteArray()).object(), m_context.answer);
    m_context.hasMemory = Value::read(QJsonDocument::fromJson(m_settings.value("memory").toByteArray()).object(), m_context.memory);
    m_message = m_history.error();
    QFile draft(storageDirectory() + "/draft.json");
    if (draft.open(QIODevice::ReadOnly) && draft.size() <= 131072) {
        if (auto root = fromJson(QJsonDocument::fromJson(draft.readAll()).object())) m_editor.replaceRoot(root);
    }
}
void Calculator::saveSettings() {
    m_settings.setValue("angleDegrees", m_context.degrees); m_settings.setValue("precision", m_context.precision);
    if (m_context.hasAnswer) m_settings.setValue("answer", QJsonDocument(m_context.answer.json()).toJson(QJsonDocument::Compact));
    if (m_context.hasMemory) m_settings.setValue("memory", QJsonDocument(m_context.memory.json()).toJson(QJsonDocument::Compact));
    else m_settings.remove("memory");
    m_settings.sync();
}
void Calculator::edited() {
    ++m_revision; m_resultCurrent = false; m_result.clear(); m_approximation.clear(); m_message = m_editor.error();
    QSaveFile file(storageDirectory() + "/draft.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(toJson(m_editor.root())).toJson(QJsonDocument::Compact));
        if (!file.commit()) m_message = "Le brouillon n’a pas pu être sauvegardé.";
    }
    emit expressionChanged(); emit stateChanged();
}
void Calculator::setCursor(Cursor cursor) { if (m_editor.setCursor(cursor)) emit cursorChanged(); }
void Calculator::command(const QString &key) {
    bool changed = false;
    if (key.size() == 1 && (key[0].isDigit() || key == "." || key == ",")) changed = m_editor.insertDigit(key == "," ? "." : key);
    else if (QStringList{"+", "-", "*", "/"}.contains(key)) changed = m_editor.insertOperator(key);
    else if (QStringList{"pi", "e", "Ans", "M"}.contains(key)) changed = m_editor.insertSymbol(key);
    else if (key == "fraction") changed = m_editor.insertFraction();
    else if (key == "power") changed = m_editor.insertPower();
    else if (key == "sqrt" || key == "cbrt") changed = m_editor.insertRoot(key == "sqrt" ? 2 : 3);
    else if (key == "(") changed = m_editor.insertGroup();
    else if (key == ")" || key == "exit") { m_editor.leave(); emit cursorChanged(); return; }
    else if (QStringList{"sin", "cos", "tan", "asin", "acos", "atan", "ln", "log", "abs", "exp"}.contains(key)) changed = m_editor.insertFunction(key);
    else if (key == "backspace") changed = m_editor.backspace();
    else if (key == "delete") changed = m_editor.deleteForward();
    else if (key == "clear") changed = m_editor.clear();
    else if (key == "undo") changed = m_editor.undo();
    else if (key == "redo") changed = m_editor.redo();
    else if (QStringList{"left", "right", "up", "down"}.contains(key)) {
        m_editor.move(key == "left" ? -1 : key == "right" ? 1 : key == "up" ? -2 : 2); emit cursorChanged(); return;
    } else if (key == "angle") {
        m_context.degrees = !m_context.degrees; saveSettings(); ++m_revision; m_result.clear(); m_approximation.clear(); m_resultCurrent = false; emit stateChanged(); return;
    } else if (key == "precision") {
        m_context.precision = m_context.precision == 6 ? 12 : m_context.precision == 12 ? 15 : 6; saveSettings();
        if (m_resultCurrent) { m_result = formatValue(m_lastResult.value, m_context.precision); m_approximation = m_lastResult.value.exact && m_lastResult.value.denominator != 1 ? approximateValue(m_lastResult.value, m_context.precision) : QString{}; }
        emit stateChanged(); return;
    } else if (key == "MS") {
        if (m_resultCurrent) { m_context.memory = m_lastResult.value; m_context.hasMemory = true; m_message = "Résultat enregistré dans M."; saveSettings(); }
        else m_message = "Calculez le résultat à mémoriser avec =.";
        emit stateChanged(); return;
    } else if (key == "MC") {
        m_context.hasMemory = false; saveSettings(); m_message = "Mémoire effacée."; emit stateChanged(); return;
    } else if (key == "=") { calculate(); return; }
    if (changed) edited();
    else if (!m_editor.error().isEmpty()) { m_message = m_editor.error(); emit stateChanged(); }
}
void Calculator::calculate() {
    if (busy()) return;
    const NodePtr snapshot = clone(m_editor.root()); const EvalContext context = m_context; const quint64 revision = m_revision;
    m_message.clear();
    disconnect(&m_watcher, nullptr, this, nullptr);
    connect(&m_watcher, &QFutureWatcher<EvalResult>::finished, this, [this, snapshot, context, revision] {
        const auto result = m_watcher.result();
        if (result.ok) {
            m_context.answer = result.value; m_context.hasAnswer = true; saveSettings();
            if (!m_history.save(snapshot, result, context.degrees)) m_message = m_history.error();
            emit historyChanged();
        }
        if (revision == m_revision) {
            m_lastResult = result; m_resultCurrent = result.ok; m_result = result.display; m_approximation = result.approximation;
            if (!result.ok) m_message = result.error;
        }
        emit stateChanged();
    });
    m_watcher.setFuture(QtConcurrent::run([snapshot, context] { return evaluate(snapshot, context); })); emit stateChanged();
}
void Calculator::loadHistory(const QString &id) {
    if (auto root = m_history.load(id)) {
        if (m_editor.replaceRoot(root)) {
            for (const auto &entry : m_history.entries()) if (entry.toMap().value("id").toString() == id) { m_context.degrees = entry.toMap().value("mode").toString() == "DEG"; break; }
            saveSettings(); edited();
        }
    }
    else { m_message = m_history.error(); emit stateChanged(); }
}
void Calculator::copy(bool latex) {
    QString content = latex ? asLatex(m_editor.root()) : asText(m_editor.root());
    if (m_resultCurrent) content += latex ? (m_lastResult.value.exact ? " = " : " ") + m_lastResult.latex : " " + m_result;
    QGuiApplication::clipboard()->setText(content); m_message = latex ? "LaTeX copié." : "Calcul copié."; emit stateChanged();
}
void Calculator::exportCalculation() {
    const QString directory = storageDirectory() + "/exports"; QDir().mkpath(directory);
    const QString path = directory + "/calcul-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz") + ".tex";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) m_message = "Impossible d’exporter le calcul.";
    else {
        QString content = "% reCalc — export autonome\n\\documentclass{article}\n\\usepackage{amsmath,amssymb}\n\\begin{document}\n\\[" + asLatex(m_editor.root());
        if (m_resultCurrent) content += (m_lastResult.value.exact ? " = " : " ") + m_lastResult.latex;
        content += "\\]\n\\end{document}\n";
        file.write(content.toUtf8()); m_message = file.commit() ? "Export LaTeX : " + path : "Échec de l’export.";
    }
    emit stateChanged();
}
}

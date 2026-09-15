#pragma once
#include "expression/Editor.hpp"
#include "evaluation/Evaluator.hpp"
#include "history/HistoryStore.hpp"
#include <QObject>
#include <QFutureWatcher>
#include <QSettings>

namespace recalc {
class Calculator : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString result READ result NOTIFY stateChanged)
    Q_PROPERTY(QString approximation READ approximation NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool degrees READ degrees NOTIFY stateChanged)
    Q_PROPERTY(bool hasMemory READ hasMemory NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(int precision READ precision NOTIFY stateChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
public:
    explicit Calculator(QObject *parent = nullptr);
    const Editor &editor() const { return m_editor; }
    QString result() const { return m_result; }
    QString approximation() const { return m_approximation; }
    QString message() const { return m_message; }
    bool busy() const { return m_watcher.isRunning(); }
    bool degrees() const { return m_context.degrees; }
    bool hasMemory() const { return m_context.hasMemory; }
    bool canUndo() const { return m_editor.canUndo(); }
    bool canRedo() const { return m_editor.canRedo(); }
    int precision() const { return m_context.precision; }
    QVariantList history() const { return m_history.entries(); }
    Q_INVOKABLE void command(const QString &command);
    Q_INVOKABLE void calculate();
    Q_INVOKABLE void loadHistory(const QString &id);
    Q_INVOKABLE void copy(bool latex);
    Q_INVOKABLE void exportCalculation();
    void setCursor(Cursor cursor);
signals:
    void expressionChanged();
    void cursorChanged();
    void stateChanged();
    void historyChanged();
private:
    Editor m_editor;
    EvalContext m_context;
    EvalResult m_lastResult;
    QSettings m_settings;
    HistoryStore m_history;
    QFutureWatcher<EvalResult> m_watcher;
    QString m_result, m_approximation, m_message;
    quint64 m_revision = 0;
    bool m_resultCurrent = false;
    void edited();
    void saveSettings();
};
}

#pragma once
#include "Ast.hpp"
#include <functional>

namespace recalc {
class Editor {
public:
    Editor();
    const NodePtr &root() const { return m_root; }
    Cursor cursor() const { return m_cursor; }
    bool setCursor(Cursor cursor);
    bool insertDigit(const QString &digit);
    bool insertOperator(const QString &op);
    bool insertSymbol(const QString &symbol);
    bool insertFraction();
    bool insertPower();
    bool insertRoot(int degree = 2);
    bool insertFunction(const QString &function);
    bool insertGroup();
    bool backspace();
    bool deleteForward();
    bool move(int direction); // -1 left, +1 right, -2 up, +2 down.
    bool leave();
    bool undo();
    bool redo();
    bool clear();
    bool replaceRoot(const NodePtr &root);
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString error() const { return m_error; }
private:
    struct Snapshot { NodePtr root; Cursor cursor; };
    NodePtr m_root;
    Cursor m_cursor;
    QVector<Snapshot> m_undo, m_redo;
    QString m_error;
    bool edit(const std::function<bool()> &command);
    NodePtr row() const;
    void boundary();
    void normalize();
    bool insertContainer(Kind kind, const QString &value, bool captureLeft);
};
}

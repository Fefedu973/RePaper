#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <memory>

namespace recalc {
// Row is a real editing container; an empty row is an explicit placeholder.
enum class Kind { Row, Number, Symbol, Operator, Fraction, Power, Root, Function, Group };
struct Node;
using NodePtr = std::shared_ptr<Node>;
struct Node {
    quint64 id = 0;
    Kind kind = Kind::Row;
    QString value;
    QVector<NodePtr> children;
};
struct Cursor {
    quint64 rowId = 0;
    int index = 0;       // Insertion boundary, or number child when digit >= 0.
    int digit = -1;      // Character insertion offset inside a Number.
    bool operator==(const Cursor &other) const { return rowId == other.rowId && index == other.index && digit == other.digit; }
};
constexpr int MaxNodes = 512;
constexpr int MaxDepth = 24;
constexpr int MaxDigits = 128;
NodePtr makeNode(Kind kind, const QString &value = {});
NodePtr makeRow();
NodePtr clone(const NodePtr &node);
NodePtr findNode(const NodePtr &root, quint64 id);
bool findParent(const NodePtr &root, quint64 id, NodePtr &parent, int &index);
bool validTree(const NodePtr &root, QString *error = nullptr);
QJsonObject toJson(const NodePtr &root);
NodePtr fromJson(const QJsonObject &json, QString *error = nullptr);
QString asText(const NodePtr &root);
QString asLatex(const NodePtr &root);
QVector<Cursor> cursorStops(const NodePtr &root);
}

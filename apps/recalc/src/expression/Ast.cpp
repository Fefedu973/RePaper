#include "Ast.hpp"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <atomic>

namespace recalc {
static std::atomic<quint64> nextId{1};
NodePtr makeNode(Kind kind, const QString &value) {
    auto n = std::make_shared<Node>(); n->id = nextId++; n->kind = kind; n->value = value; return n;
}
NodePtr makeRow() { return makeNode(Kind::Row); }
NodePtr clone(const NodePtr &n) {
    if (!n) return {};
    auto result = std::make_shared<Node>(*n); result->children.clear();
    for (const auto &child : n->children) result->children.append(clone(child));
    return result;
}
NodePtr findNode(const NodePtr &n, quint64 id) {
    if (!n) return {};
    if (n->id == id) return n;
    for (const auto &child : n->children) if (auto found = findNode(child, id)) return found;
    return {};
}
bool findParent(const NodePtr &n, quint64 id, NodePtr &parent, int &index) {
    for (int i = 0; i < n->children.size(); ++i) {
        if (n->children[i]->id == id) { parent = n; index = i; return true; }
        if (findParent(n->children[i], id, parent, index)) return true;
    }
    return false;
}
static bool validate(const NodePtr &n, int depth, int &count, QSet<quint64> &ids) {
    if (!n || ++count > MaxNodes || depth > MaxDepth || ids.contains(n->id)) return false;
    ids.insert(n->id);
    if (n->value.size() > MaxDigits) return false;
    static const QRegularExpression number(QStringLiteral("^[0-9]+(?:\\.[0-9]*)?$"));
    switch(n->kind) {
    case Kind::Number: if (!n->children.isEmpty() || !number.match(n->value).hasMatch()) return false; break;
    case Kind::Symbol: if (!n->children.isEmpty() || !QStringList{"pi", "e", "Ans", "M"}.contains(n->value)) return false; break;
    case Kind::Operator: if (!n->children.isEmpty() || !QStringList{"+", "-", "*", "/"}.contains(n->value)) return false; break;
    case Kind::Fraction: case Kind::Power: if (n->children.size() != 2) return false; break;
    case Kind::Root: if (n->children.size() != 1 || (n->value != "2" && n->value != "3")) return false; break;
    case Kind::Function: if (n->children.size() != 1 || !QStringList{"sin", "cos", "tan", "asin", "acos", "atan", "ln", "log", "abs", "exp"}.contains(n->value)) return false; break;
    case Kind::Group: if (n->children.size() != 1) return false; break;
    case Kind::Row: break;
    }
    for (const auto &child : n->children) {
        if (!child) return false;
        if ((n->kind == Kind::Row && child->kind == Kind::Row) || (n->kind != Kind::Row && child->kind != Kind::Row)) return false;
        if (!validate(child, depth + 1, count, ids)) return false;
    }
    return true;
}
bool validTree(const NodePtr &root, QString *error) {
    int count = 0; QSet<quint64> ids;
    const bool ok = root && root->kind == Kind::Row && validate(root, 0, count, ids);
    if (!ok && error) *error = QStringLiteral("Expression invalide ou limite de complexité atteinte.");
    return ok;
}
static QJsonObject encode(const NodePtr &n) {
    QJsonArray children; for (const auto &child : n->children) children.append(encode(child));
    return {{"kind", int(n->kind)}, {"value", n->value}, {"children", children}};
}
QJsonObject toJson(const NodePtr &root) { return {{"version", 1}, {"root", encode(root)}}; }
static NodePtr decode(const QJsonObject &obj, int depth, int &count) {
    if (++count > MaxNodes || depth > MaxDepth || !obj.value("kind").isDouble() || !obj.value("children").isArray()) return {};
    const int k = obj.value("kind").toInt(-1);
    if (k < int(Kind::Row) || k > int(Kind::Group)) return {};
    auto n = makeNode(Kind(k), obj.value("value").toString());
    const auto children = obj.value("children").toArray();
    if (children.size() > MaxNodes) return {};
    for (const auto &child : children) {
        if (!child.isObject()) return {};
        auto decoded = decode(child.toObject(), depth + 1, count);
        if (!decoded) return {};
        n->children.append(decoded);
    }
    return n;
}
NodePtr fromJson(const QJsonObject &json, QString *error) {
    if (json.value("version").toInt() != 1) { if (error) *error = "Version d’historique non prise en charge."; return {}; }
    int count = 0; auto n = decode(json.value("root").toObject(), 0, count);
    return validTree(n, error) ? n : NodePtr{};
}
static QString serialize(const NodePtr &n, bool latex) {
    auto sub = [&](int i) { return serialize(n->children[i], latex); };
    switch(n->kind) {
    case Kind::Row: {
        if (n->children.isEmpty()) return latex ? "\\square" : "□";
        QString result;
        for (int i = 0; i < n->children.size(); ++i) {
            if (i > 0 && n->children[i - 1]->kind != Kind::Operator && n->children[i]->kind != Kind::Operator) result += latex ? "\\cdot " : "×";
            result += serialize(n->children[i], latex);
        }
        return result;
    }
    case Kind::Number: return n->value;
    case Kind::Symbol: return n->value == "pi" ? (latex ? "\\pi " : "π") : n->value;
    case Kind::Operator: return n->value == "*" ? (latex ? "\\times " : "×") : n->value == "/" ? (latex ? "\\div " : "÷") : n->value;
    case Kind::Fraction: return latex ? "\\frac{" + sub(0) + "}{" + sub(1) + "}" : "(" + sub(0) + ")/(" + sub(1) + ")";
    case Kind::Power: return latex ? "{" + sub(0) + "}^{" + sub(1) + "}" : "(" + sub(0) + ")^(" + sub(1) + ")";
    case Kind::Root: return latex ? "\\sqrt" + QString(n->value == "3" ? "[3]" : "") + "{" + sub(0) + "}" : (n->value == "3" ? "cbrt(" : "sqrt(") + sub(0) + ")";
    case Kind::Function: return latex ? "\\operatorname{" + n->value + "}\\left(" + sub(0) + "\\right)" : n->value + "(" + sub(0) + ")";
    case Kind::Group: return latex ? "\\left(" + sub(0) + "\\right)" : "(" + sub(0) + ")";
    }
    return {};
}
QString asText(const NodePtr &root) { return serialize(root, false); }
QString asLatex(const NodePtr &root) { return serialize(root, true); }
static void addStops(const NodePtr &n, QVector<Cursor> &out) {
    if (n->kind == Kind::Row) {
        out.append({n->id, 0, -1});
        for (int i = 0; i < n->children.size(); ++i) {
            const auto &child = n->children[i];
            if (child->kind == Kind::Number) {
                for (int j = 1; j < child->value.size(); ++j) out.append({n->id, i, j});
            } else for (const auto &slot : child->children) addStops(slot, out);
            out.append({n->id, i + 1, -1});
        }
    }
}
QVector<Cursor> cursorStops(const NodePtr &root) { QVector<Cursor> out; addStops(root, out); return out; }
}

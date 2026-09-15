#include "Editor.hpp"
#include <algorithm>

namespace recalc {
Editor::Editor() : m_root(makeRow()), m_cursor{m_root->id, 0, -1} {}
NodePtr Editor::row() const { return findNode(m_root, m_cursor.rowId); }
void Editor::normalize() {
    auto r = row();
    if (!r || r->kind != Kind::Row) { m_cursor = {m_root->id, int(m_root->children.size()), -1}; return; }
    m_cursor.index = std::clamp(m_cursor.index, 0, int(r->children.size()));
    if (m_cursor.digit >= 0) {
        if (m_cursor.index >= r->children.size() || r->children[m_cursor.index]->kind != Kind::Number) m_cursor.digit = -1;
        else if (m_cursor.digit == 0) m_cursor.digit = -1;
        else if (m_cursor.digit >= r->children[m_cursor.index]->value.size()) { ++m_cursor.index; m_cursor.digit = -1; }
    }
}
bool Editor::setCursor(Cursor c) {
    const auto r = findNode(m_root, c.rowId);
    if (!r || r->kind != Kind::Row || c.index < 0 || c.index > r->children.size()) return false;
    m_cursor = c; normalize(); return true;
}
bool Editor::edit(const std::function<bool()> &command) {
    const Snapshot before{m_root, m_cursor};
    m_root = clone(m_root); m_error.clear();
    if (!command() || !validTree(m_root, &m_error)) { m_root = before.root; m_cursor = before.cursor; return false; }
    normalize(); m_undo.append(before);
    if (m_undo.size() > 64) m_undo.removeFirst();
    m_redo.clear(); return true;
}
void Editor::boundary() {
    if (m_cursor.digit < 0) return;
    auto r = row(); auto n = r->children[m_cursor.index];
    const auto right = n->value.mid(m_cursor.digit); n->value = n->value.left(m_cursor.digit);
    // A decimal point at the split belongs to the numeric piece on its left.
    if (!right.isEmpty()) r->children.insert(m_cursor.index + 1, makeNode(Kind::Number, right.startsWith('.') ? "0" + right : right));
    ++m_cursor.index; m_cursor.digit = -1;
}
bool Editor::insertDigit(const QString &digit) {
    if (digit.size() != 1 || (!digit[0].isDigit() && digit != ".")) return false;
    return edit([&] {
        auto r = row(); NodePtr number; int offset = 0;
        if (m_cursor.digit >= 0) { number = r->children[m_cursor.index]; offset = m_cursor.digit; }
        else if (m_cursor.index > 0 && r->children[m_cursor.index - 1]->kind == Kind::Number) {
            --m_cursor.index; number = r->children[m_cursor.index]; offset = number->value.size();
        } else if (m_cursor.index < r->children.size() && r->children[m_cursor.index]->kind == Kind::Number) number = r->children[m_cursor.index];
        if (!number) {
            number = makeNode(Kind::Number, digit == "." ? "0." : digit); r->children.insert(m_cursor.index, number);
            ++m_cursor.index; m_cursor.digit = -1; return true;
        }
        if (digit == "." && number->value.contains('.')) return false;
        if (number->value.size() >= MaxDigits) { m_error = "Nombre limité à 128 chiffres."; return false; }
        number->value.insert(offset, digit);
        if (number->value.startsWith('.')) { number->value.prepend('0'); ++offset; }
        m_cursor.digit = offset + 1; return true;
    });
}
bool Editor::insertOperator(const QString &op) {
    if (!QStringList{ "+", "-", "*", "/" }.contains(op)) return false;
    return edit([&] { boundary(); row()->children.insert(m_cursor.index++, makeNode(Kind::Operator, op)); return true; });
}
bool Editor::insertSymbol(const QString &symbol) {
    if (!QStringList{ "pi", "e", "Ans", "M" }.contains(symbol)) return false;
    return edit([&] { boundary(); row()->children.insert(m_cursor.index++, makeNode(Kind::Symbol, symbol)); return true; });
}
bool Editor::insertContainer(Kind kind, const QString &value, bool captureLeft) {
    return edit([&] {
        boundary(); auto r = row(); auto n = makeNode(kind, value);
        n->children.append(makeRow());
        if (kind == Kind::Fraction || kind == Kind::Power) n->children.append(makeRow());
        bool captured = false;
        if (captureLeft && m_cursor.index > 0 && r->children[m_cursor.index - 1]->kind != Kind::Operator) {
            n->children[0]->children.append(r->children.takeAt(--m_cursor.index)); captured = true;
        }
        r->children.insert(m_cursor.index, n);
        m_cursor = {n->children[captured && n->children.size() > 1 ? 1 : 0]->id, 0, -1}; return true;
    });
}
bool Editor::insertFraction() { return insertContainer(Kind::Fraction, {}, true); }
bool Editor::insertPower() { return insertContainer(Kind::Power, {}, true); }
bool Editor::insertRoot(int degree) { return (degree == 2 || degree == 3) && insertContainer(Kind::Root, QString::number(degree), false); }
bool Editor::insertFunction(const QString &function) { return insertContainer(Kind::Function, function, false); }
bool Editor::insertGroup() { return insertContainer(Kind::Group, {}, false); }
bool Editor::backspace() {
    // Traversing a container is a cursor move, so it does not destroy undo history.
    if (m_cursor.digit < 0 && m_cursor.index > 0) {
        const auto previous = row()->children[m_cursor.index - 1];
        if (!previous->children.isEmpty()) {
            const auto last = previous->children.last(); m_cursor = {last->id, int(last->children.size()), -1}; return true;
        }
    }
    return edit([&] {
        auto r = row();
        if (m_cursor.digit > 0) {
            auto n = r->children[m_cursor.index]; n->value.remove(--m_cursor.digit, 1);
            if (n->value.isEmpty()) { r->children.removeAt(m_cursor.index); m_cursor.digit = -1; }
            else if (n->value.startsWith('.')) { n->value.prepend('0'); ++m_cursor.digit; }
            return true;
        }
        if (m_cursor.index > 0) {
            auto n = r->children[m_cursor.index - 1];
            if (n->kind == Kind::Number && n->value.size() > 1) n->value.chop(1);
            else r->children.removeAt(--m_cursor.index);
            return true;
        }
        NodePtr container; int slot;
        if (!findParent(m_root, r->id, container, slot)) return false;
        NodePtr outer; int at;
        if (!findParent(m_root, container->id, outer, at)) return false;
        m_cursor = {outer->id, at, -1};
        if (r->children.isEmpty()) {
            outer->children.removeAt(at);
            for (const auto &childRow : container->children) for (const auto &child : childRow->children) outer->children.insert(at++, child);
            m_cursor.index = at;
        }
        return true;
    });
}
bool Editor::deleteForward() {
    return edit([&] {
        auto r = row();
        if (m_cursor.digit >= 0) {
            auto n = r->children[m_cursor.index];
            n->value.remove(m_cursor.digit, 1); return true;
        }
        if (m_cursor.index >= r->children.size()) return false;
        auto n = r->children[m_cursor.index];
        if (n->kind == Kind::Number && n->value.size() > 1) {
            n->value.remove(0, 1); if (n->value.startsWith('.')) n->value.prepend('0');
        } else r->children.removeAt(m_cursor.index);
        return true;
    });
}
bool Editor::move(int direction) {
    if (direction == -1 || direction == 1) {
        const auto stops = cursorStops(m_root); int index = stops.indexOf(m_cursor);
        if (index < 0) { normalize(); index = stops.indexOf(m_cursor); }
        index += direction;
        if (index < 0 || index >= stops.size()) return false;
        m_cursor = stops[index]; return true;
    }
    auto current = row(); NodePtr parent; int slot;
    while (findParent(m_root, current->id, parent, slot)) {
        if (parent->kind == Kind::Fraction || parent->kind == Kind::Power) {
            // On a fraction up means numerator; on a power up means exponent.
            const int target = parent->kind == Kind::Power ? (direction < 0 ? 1 : 0) : (direction < 0 ? 0 : 1);
            if (target != slot) {
                auto other = parent->children[target]; m_cursor = {other->id, std::min(m_cursor.index, int(other->children.size())), -1}; return true;
            }
        }
        current = parent;
    }
    return false;
}
bool Editor::leave() {
    NodePtr parent, outer; int slot, index;
    if (!findParent(m_root, m_cursor.rowId, parent, slot) || !findParent(m_root, parent->id, outer, index)) return false;
    m_cursor = {outer->id, index + 1, -1}; return true;
}
bool Editor::undo() {
    if (m_undo.isEmpty()) return false;
    m_redo.append({m_root, m_cursor}); auto state = m_undo.takeLast(); m_root = state.root; m_cursor = state.cursor; return true;
}
bool Editor::redo() {
    if (m_redo.isEmpty()) return false;
    m_undo.append({m_root, m_cursor}); auto state = m_redo.takeLast(); m_root = state.root; m_cursor = state.cursor; return true;
}
bool Editor::clear() { return edit([&] { m_root = makeRow(); m_cursor = {m_root->id, 0, -1}; return true; }); }
bool Editor::replaceRoot(const NodePtr &root) {
    if (!validTree(root, &m_error)) return false;
    return edit([&] { m_root = clone(root); m_cursor = {m_root->id, int(m_root->children.size()), -1}; return true; });
}
}

#include "MathCanvas.hpp"
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTouchEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace recalc {
struct MathCanvas::Box {
    NodePtr node;
    qreal width = 0, height = 0, baseline = 0, size = 42;
    QPointF offset;
    QString text;
    std::vector<std::unique_ptr<Box>> children;
};
static QFont mathFont(qreal size) { QFont font("DejaVu Sans"); font.setPixelSize(qRound(size)); return font; }
MathCanvas::MathCanvas(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton); setAcceptTouchEvents(true); setAntialiasing(true);
    setFlag(QQuickItem::ItemIsFocusScope, true); setActiveFocusOnTab(true);
}
MathCanvas::~MathCanvas() = default;
void MathCanvas::setCalculator(Calculator *calculator) {
    if (m_calculator == calculator) return;
    if (m_calculator) disconnect(m_calculator, nullptr, this, nullptr);
    m_calculator = calculator;
    if (calculator) {
        connect(calculator, &Calculator::expressionChanged, this, &MathCanvas::relayout);
        connect(calculator, &Calculator::cursorChanged, this, &MathCanvas::updateCursor);
    }
    relayout(); emit calculatorChanged();
}
void MathCanvas::setFontSize(qreal size) { if (m_fontSize == size) return; m_fontSize = std::clamp(size, 24., 80.); relayout(); emit fontSizeChanged(); }
void MathCanvas::setViewportX(qreal value) { if (value == m_viewport.x()) return; m_viewport.setX(value); update(); emit viewportChanged(); }
void MathCanvas::setViewportY(qreal value) { if (value == m_viewport.y()) return; m_viewport.setY(value); update(); emit viewportChanged(); }
std::unique_ptr<MathCanvas::Box> MathCanvas::layout(const NodePtr &node, qreal size) {
    auto b = std::make_unique<Box>(); b->node = node; b->size = size;
    const QFontMetricsF fm(mathFont(size));
    const qreal gap = size * .18;
    if (node->kind == Kind::Number || node->kind == Kind::Symbol || node->kind == Kind::Operator) {
        b->text = node->value;
        if (b->text == "pi") b->text = "π";
        if (node->kind == Kind::Number) b->text.replace('.', ',');
        if (node->kind == Kind::Operator) { if (b->text == "*") b->text = "×"; if (b->text == "/") b->text = "÷"; if (b->text == "-") b->text = "−"; }
        b->width = fm.horizontalAdvance(b->text) + (node->kind == Kind::Operator ? gap : 0);
        b->height = fm.height(); b->baseline = fm.ascent(); return b;
    }
    if (node->kind == Kind::Row) {
        if (node->children.isEmpty()) { b->width = size * .65; b->height = fm.height(); b->baseline = fm.ascent(); return b; }
        qreal descent = 0;
        for (const auto &child : node->children) {
            auto cb = layout(child, size); b->baseline = std::max(b->baseline, cb->baseline); descent = std::max(descent, cb->height - cb->baseline);
            b->children.push_back(std::move(cb));
        }
        b->height = b->baseline + descent;
        for (auto &child : b->children) { child->offset = {b->width, b->baseline - child->baseline}; b->width += child->width + size * .055; }
        return b;
    }
    if (node->kind == Kind::Fraction) {
        auto numerator = layout(node->children[0], std::max(20., size * .9));
        auto denominator = layout(node->children[1], std::max(20., size * .9));
        b->width = std::max(numerator->width, denominator->width) + gap * 2;
        numerator->offset = {(b->width - numerator->width) / 2, 0};
        denominator->offset = {(b->width - denominator->width) / 2, numerator->height + gap * 2};
        b->baseline = numerator->height + gap + size * .28;
        b->height = denominator->offset.y() + denominator->height;
        b->children.push_back(std::move(numerator)); b->children.push_back(std::move(denominator)); return b;
    }
    if (node->kind == Kind::Power) {
        auto base = layout(node->children[0], size); auto exponent = layout(node->children[1], std::max(18., size * .65));
        const qreal top = std::max(size * .3, exponent->height - base->baseline * .25);
        base->offset = {0, top}; exponent->offset = {base->width + gap * .35, 0};
        b->width = base->width + gap * .35 + exponent->width; b->baseline = top + base->baseline; b->height = std::max(top + base->height, exponent->height);
        b->children.push_back(std::move(base)); b->children.push_back(std::move(exponent)); return b;
    }
    auto child = layout(node->children[0], size);
    if (node->kind == Kind::Root) {
        const qreal lead = size * .72;
        child->offset = {lead, gap}; b->width = lead + child->width + gap;
        b->height = child->height + gap * 1.5; b->baseline = child->baseline + gap;
    } else {
        b->text = node->kind == Kind::Function ? node->value : QString{};
        const qreal label = fm.horizontalAdvance(b->text), bracket = size * .35;
        child->offset = {label + bracket + gap * .3, 0}; b->width = label + child->width + bracket * 2 + gap * .6;
        b->height = child->height; b->baseline = child->baseline;
    }
    b->children.push_back(std::move(child)); return b;
}
void MathCanvas::relayout() {
    if (!m_calculator) return;
    m_box = layout(m_calculator->editor().root(), m_fontSize); m_hits.clear();
    collectHits(*m_box, {28, 28}, 0);
    setImplicitWidth(m_box->width + 64); setImplicitHeight(m_box->height + 64);
    updateCursor(); update();
}
void MathCanvas::collectHits(const Box &b, const QPointF &origin, int depth) {
    if (b.node->kind == Kind::Row) {
        m_hits.append({{b.node->id, 0, -1}, QRectF(origin.x(), origin.y(), 3, b.height), depth});
        for (int i = 0; i < int(b.children.size()); ++i) {
            const auto &child = *b.children[size_t(i)];
            if (child.node->kind == Kind::Number) {
                QFontMetricsF fm(mathFont(child.size));
                for (int character = 1; character < child.text.size(); ++character) m_hits.append({{b.node->id, i, character}, QRectF(origin + child.offset + QPointF(fm.horizontalAdvance(child.text.left(character)), 0), QSizeF(3, child.height)), depth + 1});
            }
            m_hits.append({{b.node->id, i + 1, -1}, QRectF(origin.x() + child.offset.x() + child.width, origin.y(), 3, b.height), depth});
        }
    }
    for (const auto &child : b.children) collectHits(*child, origin + child->offset, depth + 1);
}
void MathCanvas::updateCursor() {
    if (!m_calculator) return;
    const auto old = m_caret;
    for (const auto &hit : m_hits) if (hit.cursor == m_calculator->editor().cursor()) { m_caret = hit.caret; break; }
    // No blinking: only the previous/new caret regions are invalidated on selection.
    update(old.united(m_caret).translated(-m_viewport).adjusted(-8, -8, 8, 8).toAlignedRect()); emit caretMoved();
}
static void bracket(QPainter *p, QRectF rect, bool left) {
    QPainterPath path;
    qreal x = left ? rect.right() : rect.left(), bend = left ? rect.left() : rect.right();
    path.moveTo(x, rect.top()); path.cubicTo(bend, rect.top() + rect.height() * .2, bend, rect.bottom() - rect.height() * .2, x, rect.bottom()); p->drawPath(path);
}
void MathCanvas::draw(QPainter *p, const Box &b, const QPointF &origin) {
    p->setFont(mathFont(b.size)); p->setPen(QPen(Qt::black, std::max(1.5, b.size / 24.)));
    if (b.node->kind == Kind::Number || b.node->kind == Kind::Symbol || b.node->kind == Kind::Operator) p->drawText(origin + QPointF(b.node->kind == Kind::Operator ? b.size * .09 : 0, b.baseline), b.text);
    else if (b.node->kind == Kind::Row && b.children.empty()) {
        p->setPen(QPen(QColor("#777777"), 1.5, Qt::DashLine)); p->drawRect(QRectF(origin + QPointF(4, b.size * .18), QSizeF(b.width - 8, b.height - b.size * .35)));
    } else if (b.node->kind == Kind::Fraction) {
        const qreal y = origin.y() + b.children[0]->height + b.size * .18;
        p->drawLine(QPointF(origin.x() + 2, y), QPointF(origin.x() + b.width - 2, y));
    } else if (b.node->kind == Kind::Root) {
        QPainterPath path; qreal x = origin.x(), y = origin.y(), h = b.height;
        path.moveTo(x + b.size * .08, y + h * .55); path.lineTo(x + b.size * .22, y + h * .5);
        path.lineTo(x + b.size * .39, y + h * .88); path.lineTo(x + b.size * .64, y + 2);
        path.lineTo(x + b.width, y + 2); p->drawPath(path);
        if (b.node->value == "3") { p->setFont(mathFont(b.size * .45)); p->drawText(origin + QPointF(0, b.size * .4), "3"); }
    } else if (b.node->kind == Kind::Group || b.node->kind == Kind::Function) {
        const auto &child = *b.children[0]; const qreal bracketWidth = b.size * .27;
        if (!b.text.isEmpty()) p->drawText(origin + QPointF(0, b.baseline), b.text);
        bracket(p, QRectF(origin + QPointF(child.offset.x() - bracketWidth, 2), QSizeF(bracketWidth * .75, b.height - 4)), true);
        bracket(p, QRectF(origin + QPointF(child.offset.x() + child.width + b.size * .06, 2), QSizeF(bracketWidth * .75, b.height - 4)), false);
    }
    for (const auto &child : b.children) draw(p, *child, origin + child->offset);
}
void MathCanvas::paint(QPainter *p) {
    p->fillRect(boundingRect(), Qt::white); p->setRenderHint(QPainter::Antialiasing, true);
    if (!m_box) return;
    p->translate(-m_viewport);
    draw(p, *m_box, {28, 28});
    p->fillRect(m_caret, Qt::black);
    p->fillRect(QRectF(m_caret.x() - 3, m_caret.y(), 9, 3), Qt::black);
    p->fillRect(QRectF(m_caret.x() - 3, m_caret.bottom() - 3, 9, 3), Qt::black);
}
void MathCanvas::chooseCursor(const QPointF &point) {
    qreal best = std::numeric_limits<qreal>::max(); const Hit *selected = nullptr;
    for (const auto &hit : m_hits) {
        qreal dx = std::abs(point.x() - hit.caret.x());
        qreal dy = point.y() < hit.caret.top() ? hit.caret.top() - point.y() : point.y() > hit.caret.bottom() ? point.y() - hit.caret.bottom() : 0;
        qreal score = dx * dx + dy * dy * 2 - hit.depth * 8;
        if (score < best) { best = score; selected = &hit; }
    }
    if (selected && m_calculator) m_calculator->setCursor(selected->cursor);
    forceActiveFocus();
}
void MathCanvas::mousePressEvent(QMouseEvent *event) { chooseCursor(event->position() + m_viewport); event->accept(); }
void MathCanvas::touchEvent(QTouchEvent *event) {
    if (event->type() == QEvent::TouchBegin && !event->points().isEmpty()) chooseCursor(event->points().first().position() + m_viewport);
    event->accept();
}
void MathCanvas::keyPressEvent(QKeyEvent *event) {
    if (!m_calculator) return;
    QString key;
    if (event->matches(QKeySequence::Undo)) key = "undo";
    else if (event->matches(QKeySequence::Redo)) key = "redo";
    else if (event->key() == Qt::Key_Left) key = "left";
    else if (event->key() == Qt::Key_Right) key = "right";
    else if (event->key() == Qt::Key_Up) key = "up";
    else if (event->key() == Qt::Key_Down) key = "down";
    else if (event->key() == Qt::Key_Backspace) key = "backspace";
    else if (event->key() == Qt::Key_Delete) key = "delete";
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Equal) key = "=";
    else if (event->key() == Qt::Key_Tab) key = "right";
    else if (event->key() == Qt::Key_Escape) key = "exit";
    else if (event->text() == "^") key = "power";
    else key = event->text();
    m_calculator->command(key); event->accept();
}
}

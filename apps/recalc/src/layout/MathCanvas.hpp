#pragma once
#include "app/Calculator.hpp"
#include <QQuickPaintedItem>
#include <memory>

namespace recalc {
class MathCanvas : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(recalc::Calculator *calculator READ calculator WRITE setCalculator NOTIFY calculatorChanged)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(qreal caretX READ caretX NOTIFY caretMoved)
    Q_PROPERTY(qreal caretY READ caretY NOTIFY caretMoved)
    Q_PROPERTY(qreal viewportX READ viewportX WRITE setViewportX NOTIFY viewportChanged)
    Q_PROPERTY(qreal viewportY READ viewportY WRITE setViewportY NOTIFY viewportChanged)
public:
    explicit MathCanvas(QQuickItem *parent = nullptr);
    ~MathCanvas() override;
    Calculator *calculator() const { return m_calculator; }
    void setCalculator(Calculator *calculator);
    qreal fontSize() const { return m_fontSize; }
    void setFontSize(qreal size);
    qreal caretX() const { return m_caret.x(); }
    qreal caretY() const { return m_caret.y(); }
    qreal viewportX() const { return m_viewport.x(); }
    qreal viewportY() const { return m_viewport.y(); }
    void setViewportX(qreal value);
    void setViewportY(qreal value);
    void paint(QPainter *painter) override;
signals:
    void calculatorChanged();
    void fontSizeChanged();
    void caretMoved();
    void viewportChanged();
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
private:
    struct Box;
    struct Hit { Cursor cursor; QRectF caret; int depth; };
    Calculator *m_calculator = nullptr;
    qreal m_fontSize = 42;
    std::unique_ptr<Box> m_box;
    QVector<Hit> m_hits;
    QRectF m_caret;
    QPointF m_viewport;
    void relayout();
    void updateCursor();
    void chooseCursor(const QPointF &point);
    std::unique_ptr<Box> layout(const NodePtr &node, qreal size);
    void draw(QPainter *painter, const Box &box, const QPointF &origin);
    void collectHits(const Box &box, const QPointF &origin, int depth);
};
}

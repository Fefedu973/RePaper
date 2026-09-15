#pragma once
#include "RichDocument.h"
#include <QQuickPaintedItem>
#include <QTimer>

namespace rmchat {
class RichMessage : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(qreal fontPixelSize READ fontPixelSize WRITE setFontPixelSize NOTIFY fontPixelSizeChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY layoutChanged)
    Q_PROPERTY(bool hasMath READ hasMath NOTIFY layoutChanged)
    Q_PROPERTY(int mathErrorCount READ mathErrorCount NOTIFY layoutChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY layoutChanged)
public:
    explicit RichMessage(QQuickItem *parent = nullptr);
    QString text() const { return m_text; }
    void setText(const QString &text);
    qreal fontPixelSize() const { return m_fontPixels; }
    void setFontPixelSize(qreal size);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    qreal contentHeight() const { return m_height; }
    bool hasMath() const { return m_document.hasMath(); }
    int mathErrorCount() const { return m_document.mathErrorCount(); }
    bool truncated() const { return m_document.truncated(); }
    void paint(QPainter *painter) override;
signals:
    void textChanged();
    void fontPixelSizeChanged();
    void colorChanged();
    void layoutChanged();
    void linkActivated(const QUrl &url);
protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void componentComplete() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
private:
    void rebuild();
    QString m_text;
    qreal m_fontPixels = 21;
    QColor m_color = Qt::black;
    qreal m_height = 1;
    RichDocument m_document;
    QTimer m_debounce;
    QString m_pressedLink;
    QPointF m_pressPosition;
};
void registerRichTypes();
}

#include "RichMessage.h"
#include <QAbstractTextDocumentLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QtMath>
#include <qqml.h>

namespace rmchat {
RichMessage::RichMessage(QQuickItem *parent) : QQuickPaintedItem(parent), m_document(this) {
    setAcceptedMouseButtons(Qt::LeftButton); setAntialiasing(true);
    m_debounce.setSingleShot(true); m_debounce.setInterval(40);
    connect(&m_debounce, &QTimer::timeout, this, &RichMessage::rebuild);
}
void RichMessage::setText(const QString &text) {
    if (m_text == text) return;
    m_text = text; emit textChanged();
    // First packet starts the timer; subsequent packets coalesce without starving it.
    if (!m_debounce.isActive()) m_debounce.start();
}
void RichMessage::setFontPixelSize(qreal size) {
    size = qBound<qreal>(10, size, 48);
    if (qFuzzyCompare(m_fontPixels, size)) return;
    m_fontPixels = size; emit fontPixelSizeChanged(); m_debounce.start();
}
void RichMessage::setColor(const QColor &color) {
    if (!color.isValid() || m_color == color) return;
    m_color = color; emit colorChanged(); m_debounce.start();
}
void RichMessage::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (!qFuzzyCompare(newGeometry.width(), oldGeometry.width())) m_debounce.start();
}
void RichMessage::componentComplete() { QQuickPaintedItem::componentComplete(); rebuild(); }
void RichMessage::rebuild() {
    m_debounce.stop();
    m_document.setSource(m_text, qMax<qreal>(40, width()), m_fontPixels, m_color,
        window() ? window()->effectiveDevicePixelRatio() : 1);
    m_height = qCeil(m_document.size().height()); setImplicitHeight(m_height);
    emit layoutChanged(); update();
}
void RichMessage::paint(QPainter *painter) {
    painter->setClipRect(boundingRect());
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, m_color);
    m_document.documentLayout()->draw(painter, context);
}
void RichMessage::mousePressEvent(QMouseEvent *event) {
    const auto anchor = m_document.documentLayout()->anchorAt(event->position());
    if (event->button() != Qt::LeftButton || !RichDocument::isSafeLink(QUrl(anchor))) { event->ignore(); return; }
    m_pressedLink = anchor; m_pressPosition = event->position(); event->accept();
}
void RichMessage::mouseReleaseEvent(QMouseEvent *event) {
    const auto anchor = m_document.documentLayout()->anchorAt(event->position());
    const bool activate = !m_pressedLink.isEmpty() && anchor == m_pressedLink &&
        QLineF(m_pressPosition, event->position()).length() < 12 && RichDocument::isSafeLink(QUrl(anchor));
    m_pressedLink.clear();
    if (activate) { event->accept(); emit linkActivated(QUrl(anchor)); } else event->ignore();
}
void RichMessage::mouseUngrabEvent() { m_pressedLink.clear(); }
void registerRichTypes() {
    static const int type = qmlRegisterType<RichMessage>("RePaper.Rich", 1, 0, "RichMessage");
    Q_UNUSED(type);
}
}

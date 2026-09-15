#include "NativePreviewItem.h"
#include <QPainter>
#include <cmath>

namespace {
constexpr int FrameIntervalMs = 16;
bool coordinate(const QVariant &value, qreal *out) {
    bool ok = false;
    *out = value.toDouble(&ok);
    return ok && std::isfinite(*out)
        && std::abs(*out) <= RePaperNative::PreviewFrame::MaximumCoordinate;
}
RePaperNative::PreviewFrame legacyFrame(const QVariantList &values) {
    QVector<RePaperNative::PreviewStroke> strokes;
    strokes.reserve(values.size());
    qsizetype count = 0;
    for (const auto &value : values) {
        const auto map = value.toMap();
        bool widthOk = false;
        const qreal width = map.value("width").toDouble(&widthOk);
        const auto points = map.value("points").toList();
        if (!widthOk || points.size() < 2
            || points.size() > RePaperNative::PreviewFrame::MaximumPoints-count) return {};
        count += points.size();
        RePaperNative::PreviewStroke stroke;
        stroke.width = width;
        stroke.color = map.value("color").value<QColor>();
        stroke.points.reserve(points.size());
        for (const auto &point : points) {
            const auto p = point.toMap(); qreal x = 0, y = 0;
            if (!coordinate(p.value("x"), &x) || !coordinate(p.value("y"), &y)) return {};
            stroke.points.append({x,y});
        }
        strokes.append(std::move(stroke));
    }
    return RePaperNative::PreviewFrame::fromStrokes(std::move(strokes));
}
bool validViewport(const QRectF &rect) {
    return std::isfinite(rect.x()) && std::isfinite(rect.y())
        && std::isfinite(rect.width()) && std::isfinite(rect.height())
        && std::abs(rect.x()) <= 1000000 && std::abs(rect.y()) <= 1000000
        && rect.width() > 0 && rect.height() > 0
        && rect.width() <= 1000000 && rect.height() <= 1000000;
}
}

NativePreviewItem::NativePreviewItem(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptTouchEvents(false);
    setFillColor(Qt::transparent);
    setAntialiasing(true);
    setFlag(QQuickItem::ItemHasContents, false);
    m_frameTimer.setSingleShot(true);
    m_frameTimer.setInterval(FrameIntervalMs);
    connect(&m_frameTimer, &QTimer::timeout, this, [this] {
        if (m_framePending) flushFrame();
    });
}

void NativePreviewItem::setViewportRect(const QRectF &viewport) {
    const QRectF bounded = validViewport(viewport) ? viewport : QRectF{};
    if (m_viewportRect == bounded) return;
    m_viewportRect = bounded;
    applyFrame(m_frame); // empty preview stays inert during every pinch/resize
    emit viewportRectChanged();
}

void NativePreviewItem::setPreviewFrame(const QVariant &value) {
    const auto frame = value.value<RePaperNative::PreviewFrame>();
    if (m_requestIsTyped && frame == m_requestedFrame) return;
    m_requestIsTyped = true;
    m_requestedFrame = frame;
    m_legacyRequested.clear();
    scheduleFrame(frame.isEmpty());
}

void NativePreviewItem::setStrokes(const QVariantList &strokes) {
    // Compatibility path only. Parse the newest legacy value once per frame.
    if (!m_requestIsTyped && strokes == m_legacyRequested) return;
    m_requestIsTyped = false;
    m_legacyRequested = strokes.size() <= RePaperNative::PreviewFrame::MaximumStrokes
        ? strokes : QVariantList{};
    scheduleFrame(m_legacyRequested.isEmpty());
}

void NativePreviewItem::scheduleFrame(bool empty) {
    m_framePending = true;
    if (empty) {
        m_frameTimer.stop();
        flushFrame(); // cancellation cannot resurrect a queued frame
    } else if (!m_frameTimer.isActive()) flushFrame();
}

void NativePreviewItem::flushFrame() {
    m_framePending = false;
    if (!m_requestIsTyped) m_requestedFrame = legacyFrame(m_legacyRequested);
    applyFrame(m_requestedFrame);
    if (!m_requestedFrame.isEmpty()) m_frameTimer.start();
}

void NativePreviewItem::applyFrame(const RePaperNative::PreviewFrame &frame) {
    const QRectF visible = frame.bounds().intersected(m_viewportRect);
    const QRectF bounds = visible.isEmpty() ? QRectF{}
        : QRectF(visible.toAlignedRect().intersected(m_viewportRect.toAlignedRect()));
    const bool geometryChanged = bounds != m_contentBounds;
    if (frame == m_frame && !geometryChanged) return;
    const QRectF dirty = m_contentBounds.united(bounds);
    m_frame = frame;
    m_contentBounds = bounds;
    // Move/resize the backing image as well as the native input footprint.
    // A retained empty texture/node must never occlude the native pen surface.
    setFlag(QQuickItem::ItemHasContents, !bounds.isEmpty());
    setPosition(bounds.topLeft());
    setSize(bounds.size());
    if (geometryChanged) emit contentBoundsChanged();
    emit strokesChanged();
    if (!bounds.isEmpty()) update(boundingRect().toAlignedRect());
    if (!dirty.isEmpty()) emit frameRequested(dirty.toAlignedRect());
}

void NativePreviewItem::paint(QPainter *painter) {
    if (m_contentBounds.isEmpty() || m_frame.isEmpty()) return;
    painter->save();
    painter->setClipRect(boundingRect(), Qt::IntersectClip);
    const QRectF clip = painter->clipBoundingRect().translated(m_contentBounds.topLeft());
    painter->translate(-m_contentBounds.topLeft());
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(Qt::NoBrush);
    const auto &strokes = m_frame.strokes();
    const auto &bounds = m_frame.strokeBounds();
    for (qsizetype i = 0; i < strokes.size(); ++i) {
        if (!bounds[i].intersects(clip)) continue;
        const auto &stroke = strokes[i];
        const QPen pen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setPen(pen);
        painter->drawPolyline(stroke.points.constData(), int(stroke.points.size()));
    }
    painter->restore();
}

#pragma once
#include <QColor>
#include <QMetaType>
#include <QPointF>
#include <QRectF>
#include <QSharedPointer>
#include <QVector>
#include <cmath>
#include <utility>

namespace RePaperNative {
struct PreviewStroke {
    QVector<QPointF> points;
    qreal width = 3;
    QColor color = Qt::black;
};

// One immutable, implicitly shared geometry packet per published preview.
// Its view-space points, validation and bounds survive the QML QVariant bridge;
// the painted item never converts a QVariantMap for each native sample.
class PreviewFrame {
public:
    static constexpr qsizetype MaximumStrokes = 144;
    static constexpr qsizetype MaximumPoints = 200128;
    static constexpr qreal MaximumCoordinate = 1000000;
    static PreviewFrame fromStrokes(QVector<PreviewStroke> strokes) {
        if (strokes.size() > MaximumStrokes) return {};
        auto data = QSharedPointer<Data>::create();
        data->strokes.reserve(strokes.size());
        data->strokeBounds.reserve(strokes.size());
        qsizetype points = 0;
        for (auto &stroke : strokes) {
            if (!std::isfinite(stroke.width) || stroke.width < 0
                || stroke.width > MaximumCoordinate || stroke.points.size() < 2
                || stroke.points.size() > MaximumPoints - points) return {};
            points += stroke.points.size();
            qreal left = 0, top = 0, right = 0, bottom = 0;
            bool first = true;
            for (const auto &p : std::as_const(stroke.points)) {
                if (!std::isfinite(p.x()) || !std::isfinite(p.y())
                    || std::abs(p.x()) > MaximumCoordinate
                    || std::abs(p.y()) > MaximumCoordinate) return {};
                if (first) { left = right = p.x(); top = bottom = p.y(); first = false; }
                else { left = qMin(left,p.x()); right = qMax(right,p.x());
                       top = qMin(top,p.y()); bottom = qMax(bottom,p.y()); }
            }
            // A native quantized zero width is invisible, not a cosmetic pen.
            if (stroke.width == 0) continue;
            stroke.color = stroke.color.isValid() ? stroke.color.toRgb() : QColor(Qt::black);
            const qreal margin = stroke.width / 2 + 2;
            const QRectF bounds(QPointF(left-margin,top-margin), QPointF(right+margin,bottom+margin));
            data->bounds = data->strokes.isEmpty() ? bounds : data->bounds.united(bounds);
            data->strokeBounds.append(bounds);
            data->strokes.append(std::move(stroke));
        }
        PreviewFrame frame;
        if (!data->strokes.isEmpty()) frame.m_data = std::move(data);
        return frame;
    }
    bool isEmpty() const { return !m_data; }
    QRectF bounds() const { return m_data ? m_data->bounds : QRectF{}; }
    const QVector<PreviewStroke> &strokes() const {
        static const QVector<PreviewStroke> empty;
        return m_data ? m_data->strokes : empty;
    }
    const QVector<QRectF> &strokeBounds() const {
        static const QVector<QRectF> empty;
        return m_data ? m_data->strokeBounds : empty;
    }
    bool operator==(const PreviewFrame &other) const { return m_data == other.m_data; }
    bool operator!=(const PreviewFrame &other) const { return !(*this == other); }
private:
    struct Data { QVector<PreviewStroke> strokes; QVector<QRectF> strokeBounds; QRectF bounds; };
    QSharedPointer<const Data> m_data;
};
}
Q_DECLARE_METATYPE(RePaperNative::PreviewFrame)

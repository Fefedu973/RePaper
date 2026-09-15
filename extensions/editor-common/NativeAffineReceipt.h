#pragma once
#include "Geometry.h"
#include <QVariantMap>

// A bounded geometric check after a native scene-change notification. This is
// neither an object-identity mapping nor a durable document/write receipt.
class NativeAffineReceipt {
public:
    static constexpr int MaximumStrokes = 128;
    static constexpr int MaximumPoints = 200000;
    static constexpr qreal Tolerance = 0.125;
    bool begin(const QVector<PaperDrawing::Stroke> &baseline, const QVariantMap &change);
    void clear();
    bool valid() const { return m_valid; }
    bool isNoop() const { return m_noop; }
    QString kind() const { return m_kind; }
    QString error() const { return m_error; }
    int expectedSelectionCount() const { return int(m_expected.size()); }
    const QVector<PaperDrawing::Stroke> &expected() const { return m_expected; }
    // Bounds of sampled positions; brush width is intentionally excluded.
    QRectF expectedBounds() const { return m_bounds; }
    bool matches(const QVector<PaperDrawing::Stroke> &observed, int observedSelectionCount) const;
private:
    bool m_valid = false, m_noop = false;
    QString m_kind, m_error;
    QVector<PaperDrawing::Stroke> m_expected;
    QRectF m_bounds;
};

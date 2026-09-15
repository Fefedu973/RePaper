#include "NativeAffineReceipt.h"
#include "NativeSelectionStyle.h"
#include <cmath>
#include <functional>

namespace {
using Strokes = QVector<PaperDrawing::Stroke>;
constexpr qreal CoordinateLimit = 1000000;
bool finitePoint(QPointF point) {
    return std::isfinite(point.x()) && std::isfinite(point.y()) &&
        std::abs(point.x()) <= CoordinateLimit && std::abs(point.y()) <= CoordinateLimit;
}
bool pointArgument(const QVariant &value, QPointF *point) {
    if (!value.canConvert<QPointF>()) return false;
    *point = value.toPointF(); return finitePoint(*point);
}
bool numberArgument(const QVariant &value, qreal *number) {
    bool ok = false; *number = value.toDouble(&ok);
    return ok && std::isfinite(*number);
}
bool validate(const Strokes &strokes) {
    if (strokes.size() > NativeAffineReceipt::MaximumStrokes) return false;
    qsizetype count = 0;
    for (const auto &stroke : strokes) {
        if (stroke.points.size() < 2 || stroke.points.size() > NativeAffineReceipt::MaximumPoints - count) return false;
        count += stroke.points.size();
        for (const auto point : stroke.points) if (!finitePoint(point)) return false;
    }
    return true;
}
QRectF bounds(const PaperDrawing::Polyline &points) {
    qreal left = points.first().x(), right = left, top = points.first().y(), bottom = top;
    for (const auto point : points) {
        left = qMin(left, point.x()); right = qMax(right, point.x());
        top = qMin(top, point.y()); bottom = qMax(bottom, point.y());
    }
    return {QPointF(left, top), QPointF(right, bottom)};
}
bool close(qreal a, qreal b) { return std::abs(a - b) <= NativeAffineReceipt::Tolerance; }
bool close(QPointF a, QPointF b) { return close(a.x(), b.x()) && close(a.y(), b.y()); }
bool samePaths(const Strokes &expected, const Strokes &observed) {
    if (expected.size() != observed.size()) return false;
    const int count = int(expected.size());
    QVector<QRectF> expectedBounds, observedBounds;
    for (const auto &stroke : expected) expectedBounds.append(bounds(stroke.points));
    for (const auto &stroke : observed) observedBounds.append(bounds(stroke.points));
    QVector<QVector<int>> candidates(count);
    for (int i = 0; i < count; ++i) {
        const auto &a = expected[i].points; const auto &ar = expectedBounds[i];
        for (int j = 0; j < count; ++j) {
            const auto &b = observed[j].points; const auto &br = observedBounds[j];
            if (a.size() != b.size() || !close(ar.topLeft(), br.topLeft()) || !close(ar.bottomRight(), br.bottomRight()) ||
                !close(a.first(), b.first()) || !close(a.last(), b.last()) || !close(a[a.size()/2], b[b.size()/2])) continue;
            bool match = true;
            for (qsizetype k = 0; k < a.size(); ++k) if (!close(a[k], b[k])) { match = false; break; }
            if (match) candidates[i].append(j);
        }
        if (candidates[i].isEmpty()) return false;
    }
    // A bijection preserves multiplicities. Greedy matching is incorrect when
    // two paths fall within the tolerance of one observed path but only one
    // matches a second path. At most 128 vertices are examined on either side.
    QVector<int> owner(count, -1);
    std::function<bool(int, QVector<bool>&)> assign = [&](int i, QVector<bool> &seen) {
        for (const int j : candidates[i]) {
            if (seen[j]) continue;
            seen[j] = true;
            if (owner[j] < 0 || assign(owner[j], seen)) { owner[j] = i; return true; }
        }
        return false;
    };
    for (int i = 0; i < count; ++i) { QVector<bool> seen(count, false); if (!assign(i, seen)) return false; }
    return true;
}
}

void NativeAffineReceipt::clear() {
    m_valid = false; m_noop = false; m_kind.clear(); m_error.clear(); m_expected.clear(); m_bounds = {};
}
bool NativeAffineReceipt::begin(const Strokes &baseline, const QVariantMap &change) {
    clear();
    const auto fail = [this](const char *error) { m_error = QString::fromLatin1(error); m_expected.clear(); return false; };
    if (baseline.isEmpty() || !validate(baseline)) return fail("Invalid or oversized baseline geometry.");
    const auto kind = change.value("kind").toString();
    QPointF anchor, delta; qreal sx = 1, sy = 1, angle = 0;
    if (kind == "move") {
        if (!pointArgument(change.value("delta"), &delta)) return fail("Invalid movement.");
    } else if (kind == "scale") {
        if (!pointArgument(change.value("anchor"), &anchor) || !numberArgument(change.value("sx"), &sx) ||
            !numberArgument(change.value("sy"), &sy) || sx <= 0 || sy <= 0 || sx > 10000 || sy > 10000)
            return fail("Invalid scaling.");
    } else if (kind == "rotate") {
        if (!pointArgument(change.value("anchor"), &anchor) || !numberArgument(change.value("angle"), &angle) || std::abs(angle) > 360)
            return fail("Invalid native rotation angle.");
    } else if (kind == "duplicate") {
        delta = QPointF(24, 24); // Matches NativePageHost's native duplicate placement.
    } else if (kind != "remove") return fail("Unsupported native operation.");
    if (kind != "remove") {
        m_expected = baseline;
        if (kind == "scale") for (auto &stroke : m_expected) {
            stroke.width = RePaperNative::scaledNativeStrokeWidth(stroke.width, sx, sy);
            if (!std::isfinite(stroke.width)) return fail("Scaled stroke width exceeds the native point format.");
        }
        for (auto &stroke : m_expected) for (auto &point : stroke.points) {
            if (kind == "move" || kind == "duplicate") point += delta;
            else if (kind == "scale") point = anchor + QPointF((point.x()-anchor.x())*sx, (point.y()-anchor.y())*sy);
            else {
                const auto relative = point - anchor;const qreal radians=angle*std::acos(qreal(-1))/180;
                point=anchor+QPointF(relative.x()*std::cos(radians)-relative.y()*std::sin(radians),
                    relative.x()*std::sin(radians)+relative.y()*std::cos(radians));
            }
            if (!finitePoint(point)) return fail("Transformed geometry exceeds finite coordinate limits.");
        }
        bool first = true;
        for (const auto &stroke : m_expected) {
            const auto rect = bounds(stroke.points);
            if (first) { m_bounds = rect; first = false; }
            else {
                // QRectF::united drops null rectangles; a horizontal/vertical
                // sampled path must still contribute its extrema here.
                m_bounds = QRectF(QPointF(qMin(m_bounds.left(),rect.left()), qMin(m_bounds.top(),rect.top())),
                    QPointF(qMax(m_bounds.right(),rect.right()), qMax(m_bounds.bottom(),rect.bottom())));
            }
        }
    }
    m_kind = kind; m_valid = true;
    // Sub-tolerance changes cannot distinguish an old snapshot from a new one.
    // Callers must skip dispatch for these no-ops instead of accepting old ink.
    m_noop = kind != "duplicate" && kind != "remove" && samePaths(m_expected, baseline);
    return true;
}
bool NativeAffineReceipt::matches(const Strokes &observed, int observedSelectionCount) const {
    return m_valid && observedSelectionCount == expectedSelectionCount() &&
        observedSelectionCount == observed.size() && validate(observed) && samePaths(m_expected, observed);
}

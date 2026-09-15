#include "NativeStrokeSampling.h"
#include <QLineF>
#include <cmath>

namespace RePaperNative {
NativeStrokeSamples sampleStrokeForNativeSelection(
    const QVector<QPointF> &path, qsizetype remainingPointBudget) {
    if (remainingPointBudget < 0 || remainingPointBudget > NativeStrokePointBudget
        || path.size() < 2 || path.size() > remainingPointBudget)
        return {{}, StrokeSamplingError::InvalidGeometryOrBudget};

    // Validate and count before allocating the interpolated path. Coordinates
    // are bounded before computing a segment count or converting it to int.
    QVector<int> subdivisions;
    qsizetype sampleCount = 0;
    for (qsizetype i = 0; i < path.size(); ++i) {
        const auto &point = path[i];
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())
            || std::abs(point.x()) > 1000000 || std::abs(point.y()) > 1000000)
            return {{}, StrokeSamplingError::InvalidCoordinates};
        const int pieces = i == 0 ? 1 : qMax(1, int(std::ceil(
            QLineF(path[i - 1], point).length() / NativeStrokeSampleSpacing)));
        if (pieces > remainingPointBudget - sampleCount)
            return {{}, StrokeSamplingError::PointBudgetExceeded};
        // The ordinary native/freehand path already meets the spacing bound.
        // Do not allocate a count array or copy all its points in that case.
        if (pieces != 1 && subdivisions.isEmpty()) {
            subdivisions.reserve(path.size());
            subdivisions.fill(1, i);
        }
        if (!subdivisions.isEmpty() || pieces != 1) subdivisions.append(pieces);
        sampleCount += pieces;
    }

    if (subdivisions.isEmpty()) return {path, StrokeSamplingError::None};

    NativeStrokeSamples result;
    result.points.reserve(sampleCount);
    result.points.append(path.first());
    for (qsizetype i = 1; i < path.size(); ++i) {
        const auto &previous = path[i - 1];
        const auto &point = path[i];
        const int pieces = subdivisions[i];
        for (int step = 1; step < pieces; ++step)
            result.points.append(previous + (point - previous) * (qreal(step) / pieces));
        result.points.append(point); // retain the exact source vertex bits
    }
    return result;
}
}

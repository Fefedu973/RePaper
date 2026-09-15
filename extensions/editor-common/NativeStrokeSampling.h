#pragma once
#include <QPointF>
#include <QVector>

namespace RePaperNative {
constexpr qsizetype NativeStrokePointBudget = 200000;
constexpr qreal NativeStrokeSampleSpacing = 8.0;

enum class StrokeSamplingError {
    None,
    InvalidGeometryOrBudget,
    InvalidCoordinates,
    PointBudgetExceeded
};

struct NativeStrokeSamples {
    QVector<QPointF> points;
    StrokeSamplingError error = StrokeSamplingError::None;
    bool valid() const { return error == StrokeSamplingError::None; }
};

// The native rectangle selector tests samples, not segment intersections.
// Preserve every source vertex and subdivide straight segments so a selection
// rectangle along a generated line can find it. Failure never returns a partial
// path. The caller supplies the remaining budget for its whole drawing batch.
NativeStrokeSamples sampleStrokeForNativeSelection(
    const QVector<QPointF> &path,
    qsizetype remainingPointBudget = NativeStrokePointBudget);
}

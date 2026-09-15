#include "NativeArrowStroke.h"
#include "DrawingModel.h"
#include <cmath>

bool RePaperNative::rebuildNativeObject(repaper::drawing::Item &item) {
    if(!repaper::drawing::rebuild(item))return false;
    item.strokes=wholeNativeArrowStrokes(item);
    return !item.strokes.isEmpty();
}

QVector<PaperDrawing::Stroke>
RePaperNative::wholeNativeArrowStrokes(const repaper::drawing::Item &item) {
    const auto &strokes = item.strokes;
    if (item.kind != "arrow" || item.style != "solid" || item.sourcePoints.size() != 2)
        return strokes;
    const bool end = item.arrowDirection == "end" || item.arrowDirection == "both";
    const bool start = item.arrowDirection == "start" || item.arrowDirection == "both";
    if ((!start && !end) || strokes.size() != 1 + int(start) + int(end)) return strokes;
    const auto &shaft = strokes.first();
    if (shaft.points != item.sourcePoints || !std::isfinite(shaft.width) || shaft.width <= 0
        || !shaft.color.isValid() || shaft.color.alpha() != 255
        || shaft.points[0] == shaft.points[1]) return strokes;
    for (const auto &stroke : strokes) {
        if (stroke.width != shaft.width || stroke.color != shaft.color) return strokes;
        for (const auto &point : stroke.points)
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return strokes;
    }
    const auto a = shaft.points[0], b = shaft.points[1];
    const auto *endHead = end ? &strokes[1].points : nullptr;
    const auto *startHead = start ? &strokes[1 + int(end)].points : nullptr;
    if ((endHead && (endHead->size() != 3 || (*endHead)[1] != b))
        || (startHead && (startHead->size() != 3 || (*startHead)[1] != a))) return strokes;

    PaperDrawing::Stroke joined = shaft;
    if (startHead && endHead) {
        joined.points = {(*startHead)[0], a, (*startHead)[2], a,
                         b, (*endHead)[0], b, (*endHead)[2]};
    } else if (endHead) {
        joined.points = {a, b, (*endHead)[0], b, (*endHead)[2]};
    } else {
        joined.points = {b, a, (*startHead)[0], a, (*startHead)[2]};
    }
    return {joined};
}

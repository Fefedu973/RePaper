#pragma once
#include "Geometry.h"

namespace repaper::drawing { struct Item; }
namespace RePaperNative {
// A freshly generated opaque solid arrow can be represented by one native
// line. The walk repeats an existing head edge; it adds no connecting ink.
// Only the drawing model's exact shaft/head topology is accepted. Other
// objects, styles, malformed arrows and translucent colors pass through.
QVector<PaperDrawing::Stroke> wholeNativeArrowStrokes(const repaper::drawing::Item &item);
bool rebuildNativeObject(repaper::drawing::Item &item);
}

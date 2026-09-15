#pragma once
#include "NativeObjectSnapshot.h"
#include <QVariantMap>

namespace NativeAgendaLayout {
constexpr int MaximumStrokes = 128;
struct Plan {
    QVector<PaperDrawing::Stroke> strokes;
    QRectF paperBounds, inkBounds;
    QString hash, displayedTitle, reason;
    QVariantMap fieldBounds;
    bool valid() const { return reason.isEmpty() && !strokes.isEmpty() && !hash.isEmpty(); }
};
// The native Dayplanner template uses scene units, not framebuffer pixels.
// Font bytes are read from Xochitl's registered resources at runtime only.
Plan create(const QVariantMap &fields, const QRectF &paperBounds, const QByteArray &fontData);
QRectF bounds(const QVector<PaperDrawing::Stroke> &strokes);
bool matches(const Plan &plan, const RePaperNative::NativeObjectSnapshot &snapshot);
}

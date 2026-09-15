#pragma once
#include <QtGlobal>
#include <cmath>
#include <limits>

namespace RePaperNative {
namespace detail {
inline qreal scaledNativeEncodedWidth(qreal encoded, qreal sx, qreal sy) {
    const auto invalid = std::numeric_limits<qreal>::quiet_NaN();
    if (!std::isfinite(encoded) || encoded < 0 || encoded > 65535 || std::floor(encoded) != encoded
        || !std::isfinite(sx) || !std::isfinite(sy) || sx <= 0 || sy <= 0) return invalid;
    const double determinant = double(sx) * double(sy);
    if (!std::isfinite(determinant)) return invalid;
    // Exact-target Line transform at 0xf79780: sqrt(abs(det)) also scales
    // maskScale. Width uses the float factor, /4, multiply, *4, then FCVTAU
    // (nearest integer, positive ties away from zero) before a uint16 store.
    const float factor = float(std::sqrt(std::abs(determinant)));
    const float pageWidth = float(encoded) * 0.25f;
    const float scaledPageWidth = pageWidth * factor;
    const float scaledEncoded = scaledPageWidth * 4.0f;
    if (!std::isfinite(scaledEncoded)) return invalid;
    const qreal rounded = std::floor(qreal(scaledEncoded) + 0.5);
    // Reject before dispatch instead of reproducing the native uint16 wrap.
    return rounded <= 65535 ? rounded : invalid;
}
}

inline bool nativeScaleWidthFits(quint16 maximumEncodedWidth, qreal sx, qreal sy) {
    return std::isfinite(detail::scaledNativeEncodedWidth(maximumEncodedWidth, sx, sy));
}

// current is a decoded native point width (an exact multiple of 1/4).
// Zero-width samples stay zero; callers choose their representative sample.
// Invalid inputs or a uint16 overflow return NaN.
inline qreal scaledNativeStrokeWidth(qreal current, qreal sx, qreal sy) {
    return detail::scaledNativeEncodedWidth(current * 4.0, sx, sy) / 4.0;
}
}

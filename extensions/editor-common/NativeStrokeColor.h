#pragma once
#include <QColor>

// Native palette codes documented by the pinned Scene Assistant primary source
// (SceneAssistant.cpp, commit 8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7).
// These are preview RGB values. Xochitl applies the device's own color profile.
// Only ARGB code 9 uses Line.rgba; palette lines may leave that word unrelated.
inline QColor nativeStrokeColor(int palette,quint32 rgba) {
    static constexpr QRgb colors[]={0xff000000,0xff7d7d7d,0xffffffff,0xffffff63,
        0xff00ff00,0xffff1493,0xff0062cc,0xffd90707,0xff7d7d7d,0,
        0xff91da71,0xff74d2e8,0xffc07fd2,0xfffae719};
    if(palette==9)return QColor::fromRgba(rgba);
    if(palette<0||palette>=int(sizeof(colors)/sizeof(colors[0])))return {};
    return QColor::fromRgba(colors[palette]);
}

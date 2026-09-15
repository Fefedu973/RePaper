#pragma once
#include <QDir>
#include <QFontDatabase>

namespace repaper {
inline void registerPaperFonts() {
    // Fonts remain on the owner's tablet. Neither sources nor releases bundle
    // reMarkable's proprietary font data. The native host exposes its resources.
    const QDir directory(qEnvironmentVariable("REPAPER_UI_FONT_DIR", "/tmp/repaper-fonts"));
    for (const QString &name : {"reMarkableSans-Regular.ttf", "reMarkableSans-Medium.ttf",
                               "reMarkableSans-Bold.ttf", "reMarkableSerifSmall-Regular.ttf"}) {
        const QString path = directory.filePath(name);
        if (QFileInfo::exists(path)) QFontDatabase::addApplicationFont(path);
    }
}
}

#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace repaper {
inline void exposeNativePaperFonts() {
    const QString path = QStringLiteral("/tmp/repaper-fonts");
    if (QFileInfo(path).isSymLink() || !QDir().mkpath(path)) return;
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    for (const QString &name : {"reMarkableSans-Regular.ttf", "reMarkableSans-Medium.ttf",
                               "reMarkableSans-Bold.ttf", "reMarkableSerifSmall-Regular.ttf"}) {
        QFile input(":/" + name);
        if (!input.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = input.readAll();
        if (bytes.isEmpty() || bytes.size() > 1024 * 1024) continue;
        const QString target = path + '/' + name;
        if (QFileInfo(target).isSymLink()) continue;
        QFile current(target);
        if (current.open(QIODevice::ReadOnly) && current.readAll() == bytes) continue;
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) continue;
        output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (output.write(bytes) != bytes.size()) { output.cancelWriting(); continue; }
        output.commit();
    }
}
}

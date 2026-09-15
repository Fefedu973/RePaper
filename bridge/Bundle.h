#pragma once
#include <QJsonObject>
#include <QString>
namespace paper {
struct Bundle {
    QString documentId;
    QString staging;
    QStringList entries;
    QString type;
    int pages = 0;
};
Bundle prepareBundle(const QString &stagingRoot, const QString &documentId, const QString &title,
                     const QString &sourcePath);
QJsonObject blankScene();
void writeDurable(const QString &path, const QByteArray &data);
void syncDirectory(const QString &path);
} // namespace paper

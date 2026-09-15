#pragma once
#include <QJsonObject>
#include <QString>

namespace paper {
// Validates an app-owned input and produces only a durable PDF snapshot. This
// never writes a reMarkable document, metadata file or native page structure.
QJsonObject prepareNativeImport(const QJsonObject &request, const QString &allowedInputRoot,
                                const QString &stagingRoot);
}

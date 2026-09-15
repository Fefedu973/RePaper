#pragma once
#include <QJsonObject>
#include <QString>
namespace paper {
QJsonObject nativeDocumentsRequest(const QString &method, const QString &route,
                                  const QJsonObject &body = {}, int timeoutMs = 15000);
}

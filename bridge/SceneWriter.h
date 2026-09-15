#pragma once
#include <QByteArray>
#include <QJsonObject>
namespace paper {
// Narrow .rm v6 writer: new notebooks with one native black fineliner layer.
// Never edits or merges an existing page or unknown block types.
QByteArray writeScene(const QJsonObject &scene);
bool validateScene(const QJsonObject &scene, QString *error = nullptr);
} // namespace paper

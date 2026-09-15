#pragma once
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>

namespace RePaperNative {
// Stable correlation with a known test document/page, without logging either ID.
inline QString contextFingerprint(const QString &documentId,const QString &pageId) {
    if(documentId.isEmpty()||pageId.isEmpty())return {};
    const QByteArray context=QJsonDocument(QJsonArray{documentId,pageId}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(context,QCryptographicHash::Sha256).toHex());
}
}

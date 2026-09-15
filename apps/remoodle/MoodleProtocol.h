#pragma once
#include <QJsonObject>
#include <QString>
#include <QUrl>
namespace moodle {
QString normalizeBase(const QString &value);
QString token(const QString &input, const QString &expectedSiteId = {});
QUrl launchUrl(const QString &base, const QString &passport);
QUrl publicFileUrl(const QUrl &url);
bool sameOrigin(const QUrl &left, const QUrl &right);
QString resourceId(const QString &account, int course, int module, int index, const QUrl &url);
QString revision(const QJsonObject &file);
QString plainText(const QString &html);
} // namespace moodle

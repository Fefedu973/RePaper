#include "MoodleProtocol.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrlQuery>
namespace moodle {
QString normalizeBase(const QString &value) {
    QUrl url(value.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        url.hasQuery() || url.hasFragment())
        return {};
    auto p = url.path();
    while (p.endsWith('/'))
        p.chop(1);
    url.setPath(p);
    return url.toString(QUrl::FullyEncoded);
}
bool sameOrigin(const QUrl &a, const QUrl &b) {
    return a.scheme() == b.scheme() && a.host().compare(b.host(), Qt::CaseInsensitive) == 0 &&
           a.port(443) == b.port(443) && a.userInfo().isEmpty() && b.userInfo().isEmpty();
}
QString token(const QString &input, const QString &expected) {
    auto text = input.trimmed();
    const QRegularExpression hex("^[a-fA-F0-9]{32}$"), privateToken("^[a-zA-Z0-9]{64}$");
    if (hex.match(text).hasMatch())
        return text;
    const auto prefix = QStringLiteral("moodlemobile://token=");
    if (text.startsWith(prefix))
        text = QUrl::fromPercentEncoding(text.mid(prefix.size()).toUtf8());
    const auto encoded = text.toLatin1();
    auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64() != encoded)
        return {};
    auto parts = QString::fromUtf8(decoded.decoded).split(":::");
    if ((parts.size() != 2 && parts.size() != 3) || !hex.match(parts[0]).hasMatch() ||
        !hex.match(parts[1]).hasMatch() || (parts.size() == 3 && !privateToken.match(parts[2]).hasMatch()))
        return {};
    if (!expected.isEmpty() && parts[0].compare(expected, Qt::CaseInsensitive) != 0)
        return {};
    return parts[1]; // Private browser autologin token is deliberately discarded.
}
QUrl launchUrl(const QString &base, const QString &passport) {
    if (normalizeBase(base).isEmpty())
        return {};
    QUrl url(base + "/admin/tool/mobile/launch.php");
    QUrlQuery q;
    q.addQueryItem("service", "moodle_mobile_app");
    q.addQueryItem("passport", passport);
    q.addQueryItem("confirmed", "1");
    url.setQuery(q);
    return url;
}
QUrl publicFileUrl(const QUrl &input) {
    auto url = input;
    url.setFragment({});
    url.setUserInfo({});
    QUrlQuery q;
    for (const auto &p : QUrlQuery(input).queryItems(QUrl::FullyDecoded)) {
        const auto key = p.first.toLower();
        if (key != "token" && key != "wstoken" && key != "privatetoken" && key != "access_token")
            q.addQueryItem(p.first, p.second);
    }
    url.setQuery(q);
    return url;
}
QString resourceId(const QString &account, int course, int module, int /*index*/, const QUrl &url) {
    // Listing position is presentation state, not remote identity. Retain the
    // legacy argument for callers, but reordering a Moodle folder must not import
    // another native copy of the same canonical resource.
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument(QJsonObject{{"account", account},
                                      {"course", course},
                                      {"module", module},
                                      {"url", publicFileUrl(url).toString(QUrl::FullyEncoded)}})
                .toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
            .toHex());
}
QString revision(const QJsonObject &file) {
    auto value = file;
    value["fileurl"] = publicFileUrl(QUrl(file["fileurl"].toString())).toString(QUrl::FullyEncoded);
    value.remove("token");
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(QJsonObject{{"url", value["fileurl"]},
                                                           {"size", value["filesize"]},
                                                           {"modified", value["timemodified"]},
                                                           {"name", value["filename"]},
                                                           {"hash", value["contenthash"]}})
                                     .toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}
QString plainText(const QString &html) {
    QTextDocument d;
    d.setHtml(html);
    return d.toPlainText().left(5000);
}
} // namespace moodle

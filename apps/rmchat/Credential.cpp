#include "Credential.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace rmchat {
QByteArray normalizeCredential(const QByteArray &input, QString *error) {
    if (error)
        error->clear();
    const auto fail = [error]() {
        if (error)
            *error = QStringLiteral("Session JSON invalide ou trop volumineuse (32 Kio maximum).");
        return QByteArray{};
    };
    if (input.isEmpty() || input.size() > credentialLimit)
        return fail();
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(input, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return fail();
    const auto source = document.object();
    QString kind, value;
    if (source.contains(QStringLiteral("version")) || source.contains(QStringLiteral("kind")) ||
        source.contains(QStringLiteral("provider"))) {
        if (source.size() != 4 || source.value("version").toDouble(-1) != 1 ||
            source.value("provider").toString() != QStringLiteral("chatgpt-web"))
            return fail();
        kind = source.value("kind").toString();
        value = source.value("value").toString();
        if (kind != QStringLiteral("access_token") && kind != QStringLiteral("session_token"))
            return fail();
    } else {
        kind = QStringLiteral("access_token");
        value = source.value("accessToken").toString();
    }
    if (value.isEmpty() || value != value.trimmed())
        return fail();
    for (const QChar character : value) {
        if (character.unicode() < 0x21 || character.unicode() > 0x7e ||
            (kind == QStringLiteral("session_token") &&
             (character == QLatin1Char(';') || character == QLatin1Char(','))))
            return fail();
    }
    const auto normalized = QJsonDocument(QJsonObject{{"version", 1}, {"provider", "chatgpt-web"},
                                                      {"kind", kind}, {"value", value}})
                                .toJson(QJsonDocument::Compact);
    if (normalized.size() > credentialLimit)
        return fail();
    return normalized;
}
}

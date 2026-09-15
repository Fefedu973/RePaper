#pragma once
#include <QByteArray>
#include <QString>

namespace rmchat {
constexpr qsizetype credentialLimit = 32 * 1024;
// Accept the strict IPC envelope or the JSON returned by api/auth/session.
// The normalized value belongs only to the host and the private IPC connection.
QByteArray normalizeCredential(const QByteArray &input, QString *error);
}

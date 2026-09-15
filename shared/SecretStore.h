#pragma once
#include <QByteArray>
#include <QString>

namespace repaper {
// A device-local encrypted vault. It does not protect against a compromised root account.
class SecretStore {
  public:
    explicit SecretStore(QString directory = {});
    bool put(const QString &name, const QByteArray &secret);
    QByteArray get(const QString &name) const;
    bool remove(const QString &name);
    QString error() const {
        return m_error;
    }

  private:
    QByteArray key(bool create) const;
    QString path(const QString &name) const;
    QString m_directory;
    mutable QString m_error;
};
} // namespace repaper

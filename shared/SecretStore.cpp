#include "SecretStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace repaper {
static constexpr auto ownerOnly = QFile::ReadOwner | QFile::WriteOwner;
SecretStore::SecretStore(QString directory) : m_directory(std::move(directory)) {
    if (m_directory.isEmpty())
        m_directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/vault";
    QDir().mkpath(m_directory);
    QFile::setPermissions(m_directory, ownerOnly | QFile::ExeOwner);
}
QString SecretStore::path(const QString &name) const {
    return m_directory + "/" + QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex() +
           ".secret";
}
QByteArray SecretStore::key(bool create) const {
    QFile file(m_directory + "/device.key");
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            m_error = "Coffre inaccessible";
            return {};
        }
        auto bytes = file.readAll();
        if (bytes.size() == 32)
            return bytes;
        m_error = "Clé de coffre invalide";
        return {};
    }
    if (!create)
        return {};
    QByteArray bytes(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), bytes.size()) != 1)
        return {};
    // NewOnly prevents another process' key from being overwritten during first use.
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return key(false);
    file.setPermissions(ownerOnly);
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        m_error = "Écriture de clé impossible";
        return {};
    }
    return bytes;
}
bool SecretStore::put(const QString &name, const QByteArray &secret) {
    m_error.clear();
    if (secret.size() > 65536 || name.isEmpty())
        return false;
    const auto k = key(true);
    if (k.size() != 32)
        return false;
    QByteArray iv(12, '\0'), cipher(secret.size() + 16, '\0'), tag(16, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(iv.data()), iv.size()) != 1)
        return false;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    int len = 0, tail = 0;
    const auto aad = name.toUtf8();
    if (!ctx ||
        EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char *>(k.constData()),
                           reinterpret_cast<const unsigned char *>(iv.constData())) != 1 ||
        EVP_EncryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char *>(aad.constData()),
                          aad.size()) != 1 ||
        EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()), &len,
                          reinterpret_cast<const unsigned char *>(secret.constData()), secret.size()) != 1)
        return false;
    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()) + len, &tail) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1)
        return false;
    cipher.resize(len + tail);
    QSaveFile file(path(name));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(ownerOnly);
    const auto content = QByteArray("RPV1") + iv + tag + cipher;
    return file.write(content) == content.size() && file.commit();
}
QByteArray SecretStore::get(const QString &name) const {
    m_error.clear();
    QFile file(path(name));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65600)
        return {};
    const auto data = file.readAll(), k = key(false);
    if (data.size() < 32 || !data.startsWith("RPV1") || k.size() != 32) {
        m_error = "Coffre endommagé";
        return {};
    }
    const auto iv = data.mid(4, 12), tag = data.mid(16, 16), cipher = data.mid(32), aad = name.toUtf8();
    QByteArray clear(cipher.size() + 16, '\0');
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free);
    int len = 0, tail = 0;
    if (!ctx ||
        EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char *>(k.constData()),
                           reinterpret_cast<const unsigned char *>(iv.constData())) != 1 ||
        EVP_DecryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char *>(aad.constData()),
                          aad.size()) != 1 ||
        EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(clear.data()), &len,
                          reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size()) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<char *>(tag.constData())) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(clear.data()) + len, &tail) != 1) {
        clear.fill('\0');
        m_error = "Authentification du coffre refusée";
        return {};
    }
    clear.resize(len + tail);
    return clear;
}
bool SecretStore::remove(const QString &name) {
    return !QFile::exists(path(name)) || QFile::remove(path(name));
}
} // namespace repaper

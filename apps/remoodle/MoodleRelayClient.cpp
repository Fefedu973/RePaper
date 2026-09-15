#include "MoodleRelayClient.h"
#include "MoodleProtocol.h"
#include "third_party/qrcodegen.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QImage>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QPainter>
#include <QRegularExpression>
#include <cmath>
#include <memory>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace {
QString qrPng(const QString &url) {
    try {
        const auto qr =
            qrcodegen::QrCode::encodeText(url.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int scale = 4, border = 4;
        QImage image((qr.getSize() + 2 * border) * scale, (qr.getSize() + 2 * border) * scale,
                     QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        for (int y = 0; y < qr.getSize(); ++y)
            for (int x = 0; x < qr.getSize(); ++x)
                if (qr.getModule(x, y))
                    painter.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
        painter.end();
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG"))
            return {};
        return "data:image/png;base64," + QString::fromLatin1(bytes.toBase64());
    } catch (const std::exception &) {
        return {};
    }
}
} // namespace
MoodleRelayClient::MoodleRelayClient(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this)) {
    m_poll.setSingleShot(true);
    m_poll.setInterval(2000);
    connect(&m_poll, &QTimer::timeout, this, &MoodleRelayClient::poll);
}
MoodleRelayClient::~MoodleRelayClient() {
    forget();
}
void MoodleRelayClient::forget() {
    ++m_epoch;
    m_poll.stop();
    if (m_reply)
        m_reply->abort();
    m_reply = nullptr;
    EVP_PKEY_free(m_key);
    m_key = nullptr;
    m_active = false;
    m_secret.clear();
    m_session.clear();
    m_code.clear();
    m_verificationUrl.clear();
    m_qrImage.clear();
    m_publicKey.clear();
    m_expected.clear();
}
void MoodleRelayClient::cancel() {
    if (!m_session.isEmpty() && !m_secret.isEmpty()) {
        QNetworkRequest request(QUrl(m_portal + "/api/sessions/" + m_session));
        request.setRawHeader("Authorization", ("Bearer " + m_secret).toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(10000);
        auto reply = m_network->deleteResource(request);
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    forget();
    emit changed();
}
void MoodleRelayClient::error(const QString &message) {
    cancel();
    m_message = message;
    emit failed(message);
    emit changed();
}
bool MoodleRelayClient::generateKey() {
    auto context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!context)
        return false;
    const bool ok = EVP_PKEY_keygen_init(context) > 0 &&
                    EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) > 0 &&
                    EVP_PKEY_keygen(context, &m_key) > 0;
    EVP_PKEY_CTX_free(context);
    if (!ok)
        return false;
    const int length = i2d_PUBKEY(m_key, nullptr);
    if (length <= 0)
        return false;
    QByteArray der(length, 0);
    auto data = reinterpret_cast<unsigned char *>(der.data());
    if (i2d_PUBKEY(m_key, &data) != length)
        return false;
    m_publicKey = QString::fromLatin1(der.toBase64());
    return true;
}
void MoodleRelayClient::request(const QString &method, const QString &path, const QJsonObject &body,
                                bool authenticated, Callback callback) {
    const int epoch = m_epoch;
    QNetworkRequest request(QUrl(m_portal + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    if (authenticated)
        request.setRawHeader("Authorization", ("Bearer " + m_secret).toUtf8());
    auto reply = m_network->sendCustomRequest(request, method.toUtf8(),
                                              QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        *bytes += reply->readAll();
        if (bytes->size() > 65536)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, epoch, callback] {
        *bytes += reply->readAll();
        reply->deleteLater();
        if (epoch != m_epoch)
            return;
        if (m_reply == reply)
            m_reply = nullptr;
        if (bytes->size() > 65536) {
            error("Réponse du portail trop volumineuse. Recommencez la connexion.");
            return;
        }
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(*bytes, &parse);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 200 && status < 300 && (parse.error != QJsonParseError::NoError || !doc.isObject())) {
            error("Réponse du portail invalide.");
            return;
        }
        callback(status >= 200 && status < 300 && reply->error() != QNetworkReply::NoError ? 0 : status,
                 doc.object());
    });
}
void MoodleRelayClient::start(const QString &portal, const QString &base, const QString &passport) {
    cancel();
    m_portal = moodle::normalizeBase(portal);
    const auto clean = moodle::normalizeBase(base);
    if (m_portal.isEmpty()) {
        error("Le portail de connexion HTTPS n’est pas configuré dans cette version.");
        return;
    }
    if (clean.isEmpty() || !QRegularExpression("^[a-fA-F0-9]{32}$").match(passport).hasMatch()) {
        error("Adresse Moodle invalide.");
        return;
    }
    if (!generateKey()) {
        error("Impossible de préparer la connexion chiffrée.");
        return;
    }
    m_active = true;
    m_deadline = QDeadlineTimer(600000);
    m_expected = QString::fromLatin1(
        QCryptographicHash::hash((clean + passport).toUtf8(), QCryptographicHash::Md5).toHex());
    m_message = "Préparation du QR de connexion…";
    emit changed();
    request(
        "POST", "/api/sessions",
        {{"moodleBase", clean}, {"passport", passport}, {"siteId", m_expected}, {"publicKey", m_publicKey}},
        false, [this](int status, const QJsonObject &value) {
            if (status < 200 || status >= 300) {
                error("Le portail de connexion est indisponible. Réessayez.");
                return;
            }
            const auto session = value["sessionId"].toString(), secret = value["deviceSecret"].toString(),
                       code = value["code"].toString(), url = value["verificationUrl"].toString();
            const auto expiry = QDateTime::fromString(value["expiresAt"].toString(), Qt::ISODate);
            const auto remaining = QDateTime::currentDateTimeUtc().secsTo(expiry);
            if (!QRegularExpression("^[A-Za-z0-9_-]{16,128}$").match(session).hasMatch() ||
                !QRegularExpression("^[A-Za-z0-9_-]{24,512}$").match(secret).hasMatch() ||
                !QRegularExpression("^[0-9]{8}$").match(code).hasMatch() || url.size() > 1024 ||
                !moodle::sameOrigin(QUrl(url), QUrl(m_portal)) || QUrl(url).scheme() != "https" ||
                !expiry.isValid() || remaining <= 0 || remaining > 610) {
                error("Session du portail invalide.");
                return;
            }
            m_session = session;
            m_secret = secret;
            m_code = code;
            m_verificationUrl = url;
            m_qrImage = qrPng(url);
            m_deadline = QDeadlineTimer(qMin<qint64>(remaining * 1000, 600000));
            if (m_qrImage.isEmpty()) {
                error("Impossible d’afficher le QR de connexion.");
                return;
            }
            m_message =
                "Scannez ce QR, ou ouvrez le site indiqué et saisissez le code. Le retour sera automatique.";
            emit changed();
            m_poll.start();
        });
}
QJsonObject MoodleRelayClient::decrypt(const QString &ciphertext) {
    const auto decoded =
        QByteArray::fromBase64Encoding(ciphertext.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!m_key || !decoded || decoded.decoded.toBase64() != ciphertext.toLatin1() ||
        decoded.decoded.size() != 256)
        return {};
    auto context = EVP_PKEY_CTX_new(m_key, nullptr);
    if (!context)
        return {};
    size_t length = 256;
    QByteArray plain(256, 0);
    const bool ok = EVP_PKEY_decrypt_init(context) > 0 &&
                    EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_OAEP_PADDING) > 0 &&
                    EVP_PKEY_CTX_set_rsa_oaep_md(context, EVP_sha256()) > 0 &&
                    EVP_PKEY_CTX_set_rsa_mgf1_md(context, EVP_sha256()) > 0 &&
                    EVP_PKEY_decrypt(context, reinterpret_cast<unsigned char *>(plain.data()), &length,
                                     reinterpret_cast<const unsigned char *>(decoded.decoded.constData()),
                                     decoded.decoded.size()) > 0;
    EVP_PKEY_CTX_free(context);
    if (!ok) {
        plain.fill('\0');
        return {};
    }
    plain.resize(int(length));
    QJsonParseError parse;
    const auto value = QJsonDocument::fromJson(plain, &parse).object();
    plain.fill('\0');
    if (parse.error != QJsonParseError::NoError)
        return {};
    if (value["kind"] == "qr") {
        const auto id = value["userid"].toDouble();
        if (value.size() != 3 || !value["userid"].isDouble() || id < 1 || id > 2147483647 ||
            id != std::floor(id) ||
            !QRegularExpression("^[a-fA-F0-9]{32}$").match(value["qrloginkey"].toString()).hasMatch())
            return {};
        return value;
    }
    return {};
}
void MoodleRelayClient::poll() {
    if (!m_active || m_reply || m_session.isEmpty())
        return;
    if (m_deadline.hasExpired()) {
        error("Le code a expiré. Recommencez la connexion.");
        return;
    }
    request("POST", "/api/sessions/" + m_session + "/poll", {}, true,
            [this](int status, const QJsonObject &value) {
                if (m_deadline.hasExpired()) {
                    error("Le code a expiré. Recommencez la connexion.");
                    return;
                }
                if (status == 0 || status == 429 || status >= 500) {
                    m_message = "Réseau indisponible, reprise automatique de l’attente…";
                    emit changed();
                    m_poll.start();
                    return;
                }
                if (status < 200 || status >= 300 || value["status"] == "expired") {
                    error("La demande a expiré ou a été annulée. Recommencez.");
                    return;
                }
                if (value["status"] == "pending") {
                    m_poll.start();
                    return;
                }
                if (value["status"] != "complete") {
                    error("État de connexion inattendu.");
                    return;
                }
                const auto payload = decrypt(value["ciphertext"].toString());
                if (payload.isEmpty()) {
                    error("Le retour chiffré ne correspond pas à cette tablette. Recommencez la connexion.");
                    return;
                }
                cancel();
                m_message = "Vérification du compte Moodle…";
                emit changed();
                emit qrReceived(payload["userid"].toInt(), payload["qrloginkey"].toString());
            });
}

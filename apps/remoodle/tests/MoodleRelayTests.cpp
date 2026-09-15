#include "MoodleRelayClient.h"
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslSocket>
#include <QtTest>
#include <cstring>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace {
class Reply : public QNetworkReply {
  public:
    Reply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override {
        setError(OperationCanceledError, "cancelled");
    }
    void deliver(const QJsonObject &object, int status = 200) {
        data = QJsonDocument(object).toJson(QJsonDocument::Compact);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setFinished(true);
        emit readyRead();
        emit finished();
    }
    qint64 bytesAvailable() const override {
        return data.size() + QNetworkReply::bytesAvailable();
    }

  protected:
    qint64 readData(char *buffer, qint64 size) override {
        const auto n = qMin<qint64>(data.size(), size);
        if (!n)
            return -1;
        memcpy(buffer, data.constData(), size_t(n));
        data.remove(0, int(n));
        return n;
    }

  private:
    QByteArray data;
};
QByteArray encrypt(const QString &spki, const QJsonObject &value, bool wrongHash = false) {
    const auto der = QByteArray::fromBase64(spki.toLatin1());
    const auto *data = reinterpret_cast<const unsigned char *>(der.constData());
    auto key = d2i_PUBKEY(nullptr, &data, der.size());
    if (!key)
        return {};
    auto ctx = EVP_PKEY_CTX_new(key, nullptr);
    QByteArray out(256, 0);
    size_t length = 256;
    const auto plain = QJsonDocument(value).toJson(QJsonDocument::Compact);
    const auto hash = wrongHash ? EVP_sha1() : EVP_sha256();
    const bool ok =
        ctx && EVP_PKEY_encrypt_init(ctx) > 0 &&
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) > 0 &&
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx, hash) > 0 && EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, hash) > 0 &&
        EVP_PKEY_encrypt(ctx, reinterpret_cast<unsigned char *>(out.data()), &length,
                         reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) > 0;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok)
        return {};
    out.resize(int(length));
    return out.toBase64();
}
class Portal : public QNetworkAccessManager {
  public:
    QString session = QString(32, 's'), secret = QString(48, 'z'), token = QString(32, 'a');
    QString mode = "qr";
    QJsonObject creation;
    QList<QNetworkRequest> requests;
    QList<QByteArray> bodies;
    int deletes = 0;
    bool hold = false;
    QPointer<Reply> pending;

  protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoing) override {
        requests << request;
        const auto body = outgoing ? outgoing->readAll() : QByteArray();
        bodies << body;
        auto reply = new Reply(request, this);
        QJsonObject response;
        if (op == DeleteOperation) {
            ++deletes;
            response = {{"ok", true}};
        } else if (request.url().path() == "/api/sessions") {
            creation = QJsonDocument::fromJson(body).object();
            response = {{"sessionId", session},
                        {"deviceSecret", mode == "header" ? secret + "\r\nHeader: value" : secret},
                        {"code", "12345678"},
                        {"verificationUrl", mode == "foreign" ? "https://foreign.example/"
                                                              : "https://portal.example/#session=" + session},
                        {"expiresAt", QDateTime::currentDateTimeUtc()
                                          .addSecs(mode == "expired" ? -1 : 600)
                                          .toString(Qt::ISODate)}};
        } else {
            QJsonObject payload{
                {"siteId", mode == "nonce" ? QString(32, '0') : creation["siteId"].toString()},
                {"token", token}};
            if (mode == "private")
                payload["privateToken"] = QString(64, 'b');
            if (mode.startsWith("qr")) {
                payload = {{"kind", "qr"}, {"userid", 123}, {"qrloginkey", QString(32, 'c')}};
                if (mode == "qr-negative")
                    payload["userid"] = -1;
                if (mode == "qr-fraction")
                    payload["userid"] = 1.5;
                if (mode == "qr-overflow")
                    payload["userid"] = 2147483648.0;
                if (mode == "qr-string")
                    payload["userid"] = "123";
                if (mode == "qr-key")
                    payload["qrloginkey"] = "not-a-key";
                if (mode == "qr-extra")
                    payload["token"] = token;
            }
            const auto cipher = encrypt(creation["publicKey"].toString(), payload, mode.endsWith("sha1"));
            response = {{"status", "complete"}, {"ciphertext", QString::fromLatin1(cipher)}};
        }
        if (hold) {
            pending = reply;
            return reply;
        }
        QTimer::singleShot(0, reply, [reply, response] { reply->deliver(response); });
        return reply;
    }
};
} // namespace
class MoodleRelayTests : public QObject {
    Q_OBJECT
  private slots:
    void encryptedReturnAndQr() {
        Portal portal;
        MoodleRelayClient client(nullptr, &portal);
        QSignalSpy received(&client, &MoodleRelayClient::qrReceived);
        client.start("https://portal.example", "https://school.example/moodle", QString(32, 'b'));
        QTRY_COMPARE(client.code(), QString("12345678"));
        QVERIFY(client.qrImage().startsWith("data:image/png;base64,"));
        QImage qr;
        QVERIFY(qr.loadFromData(QByteArray::fromBase64(client.qrImage().section(',', 1).toLatin1()), "PNG"));
        QVERIFY(qr.width() > 100);
        QCOMPARE(portal.creation["siteId"].toString(), client.expectedSiteId());
        QVERIFY(!portal.creation.contains("token"));
        client.poll();
        QTRY_COMPARE(received.count(), 1);
        QCOMPARE(received.first()[0].toInt(), 123);
        QCOMPARE(received.first()[1].toString(), QString(32, 'c'));
        QVERIFY(!client.active());
        QVERIFY(client.code().isEmpty());
        QCOMPARE(portal.deletes, 1);
        for (int i = 0; i < portal.requests.size(); ++i) {
            const auto &request = portal.requests[i];
            QCOMPARE(request.url().scheme(), QString("https"));
            QVERIFY(!request.url().toString().contains(portal.secret));
            QVERIFY(!portal.bodies[i].contains(portal.token.toUtf8()));
            QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                     int(QNetworkRequest::ManualRedirectPolicy));
            QVERIFY(request.sslConfiguration().peerVerifyMode() != QSslSocket::VerifyNone);
        }
        QCOMPARE(portal.requests[1].rawHeader("Authorization"), ("Bearer " + portal.secret).toUtf8());
    }
    void rejectsUnboundOrWrongEncryption_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"nonce", "qr-sha1", "private"})
            QTest::newRow(mode) << QString(mode);
    }
    void rejectsUnboundOrWrongEncryption() {
        QFETCH(QString, mode);
        Portal portal;
        portal.mode = mode;
        MoodleRelayClient client(nullptr, &portal);
        QSignalSpy received(&client, &MoodleRelayClient::qrReceived),
            failed(&client, &MoodleRelayClient::failed);
        client.start("https://portal.example", "https://school.example", QString(32, 'b'));
        QTRY_VERIFY(!client.code().isEmpty());
        client.poll();
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(received.count(), 0);
        QVERIFY(!client.active());
    }
    void officialQrCredentialsReturn() {
        Portal portal;
        portal.mode = "qr";
        MoodleRelayClient client(nullptr, &portal);
        QSignalSpy qr(&client, &MoodleRelayClient::qrReceived);
        client.start("https://portal.example", "https://school.example", QString(32, 'b'));
        QTRY_VERIFY(!client.code().isEmpty());
        client.poll();
        QTRY_COMPARE(qr.count(), 1);
        QCOMPARE(qr.first()[0].toInt(), 123);
        QCOMPARE(qr.first()[1].toString(), QString(32, 'c'));
        QCOMPARE(portal.deletes, 1);
    }
    void rejectsMalformedQr_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode :
             {"qr-negative", "qr-fraction", "qr-overflow", "qr-string", "qr-key", "qr-extra"})
            QTest::newRow(mode) << QString(mode);
    }
    void rejectsMalformedQr() {
        QFETCH(QString, mode);
        Portal portal;
        portal.mode = mode;
        MoodleRelayClient client(nullptr, &portal);
        QSignalSpy received(&client, &MoodleRelayClient::qrReceived),
            failed(&client, &MoodleRelayClient::failed);
        client.start("https://portal.example", "https://school.example", QString(32, 'b'));
        QTRY_VERIFY(!client.code().isEmpty());
        client.poll();
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(received.count(), 0);
    }
    void rejectsInvalidSession_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("foreign origin") << QString("foreign");
        QTest::newRow("expired") << QString("expired");
        QTest::newRow("header injection") << QString("header");
    }
    void rejectsInvalidSession() {
        QFETCH(QString, mode);
        Portal portal;
        portal.mode = mode;
        MoodleRelayClient client(nullptr, &portal);
        QSignalSpy failed(&client, &MoodleRelayClient::failed);
        client.start("https://portal.example", "https://school.example", QString(32, 'b'));
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(client.code().isEmpty());
    }
    void cancellationIgnoresLateResponse() {
        Portal portal;
        portal.hold = true;
        MoodleRelayClient client(nullptr, &portal);
        client.start("https://portal.example", "https://school.example", QString(32, 'b'));
        QVERIFY(portal.pending);
        auto old = portal.pending;
        client.cancel();
        old->deliver({{"sessionId", portal.session},
                      {"deviceSecret", portal.secret},
                      {"code", "12345678"},
                      {"verificationUrl", "https://portal.example/"},
                      {"expiresAt", QDateTime::currentDateTimeUtc().addSecs(600).toString(Qt::ISODate)}});
        QVERIFY(!client.active());
        QVERIFY(client.code().isEmpty());
        QVERIFY(client.publicKey().isEmpty());
    }
};
QTEST_GUILESS_MAIN(MoodleRelayTests)
#include "MoodleRelayTests.moc"

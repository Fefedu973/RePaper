#include "MoodleProtocol.h"
#include <QCryptographicHash>
#include <QUrlQuery>
#include <QtTest>
class MoodleTests : public QObject {
    Q_OBJECT
  private slots:
    void handoffDropsPrivateToken() {
        QString site(32, 'a'), token(32, 'b'), privateToken(64, 'c');
        auto encoded = (site + ":::" + token + ":::" + privateToken).toUtf8().toBase64();
        QCOMPARE(moodle::token("moodlemobile://token=" + QString::fromLatin1(encoded), site), token);
        QVERIFY(moodle::token(QString::fromLatin1(encoded), QString(32, 'd')).isEmpty());
    }
    void malformedToken() {
        for (auto s : {"", "garbage", "moodlemobile://token=bad", "https://example.org/token?secret=1"})
            QVERIFY(moodle::token(s).isEmpty());
        QCOMPARE(moodle::token(QString(32, 'f')), QString(32, 'f'));
    }
    void baseUrlValidation() {
        QCOMPARE(moodle::normalizeBase("https://school.example/moodle///"),
                 QString("https://school.example/moodle"));
        for (auto url : {"http://example.org", "https://user:pass@example.org",
                         "https://example.org/?token=1", "file:///etc/passwd", "https://example.org/#x"})
            QVERIFY(moodle::normalizeBase(url).isEmpty());
    }
    void launchPath() {
        auto u = moodle::launchUrl("https://school.example/moodle", "nonce");
        QCOMPARE(u.path(), QString("/moodle/admin/tool/mobile/launch.php"));
        QCOMPARE(QUrlQuery(u).queryItemValue("passport"), QString("nonce"));
        QCOMPARE(QUrlQuery(u).queryItemValue("service"), QString("moodle_mobile_app"));
    }
    void secretIndependentIdentity() {
        auto a = QUrl("https://school.example/file.php/1/a.pdf?token=first"),
             b = QUrl("https://school.example/file.php/1/a.pdf?token=second");
        QCOMPARE(moodle::resourceId("account", 1, 2, 0, a), moodle::resourceId("account", 1, 2, 0, b));
        QVERIFY(moodle::resourceId("a", 1, 2, 0, a) != moodle::resourceId("b", 1, 2, 0, a));
        QVERIFY(!moodle::publicFileUrl(a).toString().contains("first"));
    }
    void revisionsChangeWithContent() {
        QJsonObject a{{"fileurl", "https://school.example/f?token=a"},
                      {"timemodified", 1},
                      {"filesize", 12},
                      {"filename", "a.pdf"}},
            b = a;
        b["fileurl"] = "https://school.example/f?token=b";
        QCOMPARE(moodle::revision(a), moodle::revision(b));
        b["timemodified"] = 2;
        QVERIFY(moodle::revision(a) != moodle::revision(b));
    }
    void resourceIdentitySurvivesFolderReordering() {
        auto first = QUrl("https://school.example/pluginfile.php/42/folder/a.pdf?token=one"),
             same = QUrl("https://school.example/pluginfile.php/42/folder/a.pdf?token=two"),
             other = QUrl("https://school.example/pluginfile.php/42/folder/b.pdf");
        QCOMPARE(moodle::resourceId("account", 1, 2, 0, first), moodle::resourceId("account", 1, 2, 9, same));
        QVERIFY(moodle::resourceId("account", 1, 2, 0, first) !=
                moodle::resourceId("account", 1, 2, 0, other));
    }
    void crossOriginIsRejected() {
        QVERIFY(!moodle::sameOrigin(QUrl("https://school.example"), QUrl("https://evil.example")));
        QVERIFY(!moodle::sameOrigin(QUrl("https://school.example"), QUrl("http://school.example")));
        QVERIFY(moodle::sameOrigin(QUrl("https://school.example:443/f"), QUrl("https://school.example")));
    }
};
QTEST_GUILESS_MAIN(MoodleTests)
#include "MoodleTests.moc"

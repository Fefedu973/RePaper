#include "SecretStore.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
class SharedTest : public QObject {
    Q_OBJECT
  private slots:
    void encryptedRoundtrip() {
        QTemporaryDir dir;
        repaper::SecretStore store(dir.path());
        QVERIFY(store.put("moodle", "secret-token-value"));
        QCOMPARE(store.get("moodle"), QByteArray("secret-token-value"));
        auto files = QDir(dir.path()).entryList({"*.secret"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QFile file(dir.filePath(files[0]));
        QVERIFY(file.open(QIODevice::ReadWrite));
        auto cipher = file.readAll();
        QVERIFY(!cipher.contains("secret-token-value"));
        cipher[cipher.size() - 1] = cipher.back() ^ 1;
        file.resize(0);
        file.write(cipher);
        file.close();
        QVERIFY(store.get("moodle").isEmpty());
        QVERIFY(!store.error().isEmpty());
    }
    void secretNamesCannotEscape() {
        QTemporaryDir dir;
        repaper::SecretStore store(dir.path());
        QVERIFY(store.put("../../../token", "value"));
        QCOMPARE(store.get("../../../token"), QByteArray("value"));
        QVERIFY(store.remove("../../../token"));
        QVERIFY(store.get("../../../token").isEmpty());
    }
};
QTEST_GUILESS_MAIN(SharedTest)
#include "test_shared.moc"

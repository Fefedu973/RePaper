#include "NativeLineFactory.h"
#include <QtTest>

class NativeLineFactoryGateTest : public QObject {
    Q_OBJECT
private slots:
    void hostCannotCallPrivateFirmwareFactory() {
        QString error = "old error";
        QVERIFY(!RePaperNative::createNativeLineItem(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(error != "old error");
        QVERIFY(!RePaperNative::createNativeLineItem());
    }
};
QTEST_APPLESS_MAIN(NativeLineFactoryGateTest)
#include "NativeLineFactoryGateTest.moc"

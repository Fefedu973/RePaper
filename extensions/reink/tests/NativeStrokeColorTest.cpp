#include "NativeStrokeColor.h"
#include <QtTest>
class NativeStrokeColorTest:public QObject {
    Q_OBJECT
private slots:
    void paletteIgnoresUnrelatedRgbaWord(){
        QCOMPARE(nativeStrokeColor(0,0x00112233),QColor("#000000"));
        QCOMPARE(nativeStrokeColor(6,0),QColor("#0062cc"));
        QCOMPARE(nativeStrokeColor(7,0xffffffff),QColor("#d90707"));
        QCOMPARE(nativeStrokeColor(13,0),QColor("#fae719"));
        for(int index=0;index<14;++index)if(index!=9)QCOMPARE(nativeStrokeColor(index,0).alpha(),255);
    }
    void argbPreservesAllChannelsAndUnknownCodesFail(){
        QCOMPARE(nativeStrokeColor(9,0xff136aca).rgba(),QRgb(0xff136aca));
        QCOMPARE(nativeStrokeColor(9,0x80136aca).alpha(),128);
        QVERIFY(!nativeStrokeColor(-1,0xff123456).isValid());
        QVERIFY(!nativeStrokeColor(14,0xff123456).isValid());
    }
};
QTEST_GUILESS_MAIN(NativeStrokeColorTest)
#include "NativeStrokeColorTest.moc"

#include "Bundle.h"
#include "SceneWriter.h"
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
class FormatTest : public QObject {
    Q_OBJECT
  private slots:
    void rejectsInvalidGeometry() {
        auto scene = paper::blankScene();
        scene["strokes"] =
            QJsonArray{QJsonObject{{"width", 3},
                                   {"color", "black"},
                                   {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1405, 100}}}}};
        QVERIFY(!paper::validateScene(scene));
        QVERIFY(paper::writeScene(scene).isEmpty());
    }
    void newNativeNotebook() {
        QTemporaryDir dir;
        auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto bundle = paper::prepareBundle(dir.path(), id, "Note du jour", {});
        QCOMPARE(bundle.pages, 1);
        QCOMPARE(bundle.type, QString("notebook"));
        QVERIFY(QFile::exists(bundle.staging + "/" + id + ".content"));
        QVERIFY_EXCEPTION_THROWN(paper::prepareBundle(dir.path(), id, "Second", {}), std::runtime_error);
    }
    void nativeFormatHeaderAndLengths() {
        auto scene = paper::blankScene();
        scene["strokes"] =
            QJsonArray{QJsonObject{{"width", 3},
                                   {"color", "black"},
                                   {"points", QJsonArray{QJsonArray{100, 100}, QJsonArray{500, 300}}}}};
        auto bytes = paper::writeScene(scene);
        QVERIFY(bytes.startsWith("reMarkable .lines file, version=6          "));
        int pos = 43, blocks = 0;
        while (pos < bytes.size()) {
            QVERIFY(pos + 8 <= bytes.size());
            quint32 size =
                qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + pos));
            pos += 8 + int(size);
            ++blocks;
        }
        QCOMPARE(pos, bytes.size());
        QCOMPARE(blocks, 9);
    }
};
QTEST_GUILESS_MAIN(FormatTest)
#include "test_format.moc"

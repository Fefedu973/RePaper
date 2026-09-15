#include <QtTest>
#include <QRectF>
#include <QVariant>
#include <array>
#include <cstddef>
#include <memory>
#include <new>

// A local C++ ownership model, not an ABI fixture or a synthetic native item.
// These tests establish QList/shared_ptr/move-assignment behavior on the host.
// Native offsets, RTTI, allocator and Qt ABI need their separate exact-firmware
// evidence; no Xochitl constructor, vtable or architecture macro is used here.
namespace {
#pragma pack(push, 1)
struct PointModel {
    float x, y;
    quint16 speed, width;
    quint8 direction, pressure;
};
#pragma pack(pop)
struct LineModel {
    int tool = 19;
    int color = 0;
    quint32 rgba = 0xff000000;
    QList<PointModel> points;
    double maskScale = 1;
    float thickness = 3;
    QRectF bounds;
};
struct ItemModel {
    std::array<unsigned char, 0x48> header;
    LineModel line;
    std::array<unsigned char, 0x10> trailer;
};
using ItemsModel = QList<std::shared_ptr<ItemModel>>;

// Checks only this test's host model. They are not a firmware-layout test.
static_assert(sizeof(PointModel) == 14);
static_assert(sizeof(void *) == 8);
static_assert(sizeof(LineModel) == 0x58);
static_assert(offsetof(ItemModel, line) == 0x48);
static_assert(offsetof(ItemModel, trailer) == 0xa0);
static_assert(sizeof(ItemModel) == 0xb0);

ItemModel seed() {
    ItemModel item;
    for (size_t i = 0; i < item.header.size(); ++i) item.header[i] = 17 + i;
    for (size_t i = 0; i < item.trailer.size(); ++i) item.trailer[i] = 193 + i;
    item.line.points = {{10, 20, 25, 24, 0, 255}, {30, 40, 25, 24, 0, 255}};
    item.line.bounds = QRectF(8.5, 18.5, 23, 23);
    return item;
}
LineModel replacement(float x) {
    LineModel line;
    line.points = {{x, 60, 25, 40, 0, 255}, {x + 10, 70, 25, 40, 0, 255},
                   {x + 20, 60, 25, 40, 0, 255}};
    line.thickness = 5;
    line.bounds = QRectF(x - 2.5, 57.5, 25, 15);
    return line;
}
void replacePayload(ItemModel &item, LineModel &&line) {
    // Same object-lifetime operation as the production replacement, exercised
    // exclusively on an object that this test constructed with that subobject.
    auto *payload = std::launder(reinterpret_cast<LineModel *>(
        reinterpret_cast<unsigned char *>(&item) + offsetof(ItemModel, line)));
    *payload = std::move(line);
}
}

class NativeLineReplacementTest : public QObject {
    Q_OBJECT
private slots:
    void replacementKeepsHeaderTrailerAndReleasesOldSharedList() {
        const ItemModel original = seed();
        auto clone = std::make_shared<ItemModel>(original);
        const auto *originalPoints = original.line.points.constData();
        QCOMPARE(clone->line.points.constData(), originalPoints);
        QVERIFY(!original.line.points.isDetached());
        {
            auto geometry = replacement(100);
            const auto *newPoints = geometry.points.constData();
            replacePayload(*clone, std::move(geometry));
            QCOMPARE(clone->line.points.constData(), newPoints);
            QCOMPARE(clone->line.points.size(), 3);
            QCOMPARE(clone->line.thickness, 5.f);
            QCOMPARE(clone->line.bounds, QRectF(97.5, 57.5, 25, 15));
        }
        // A memcpy of LineModel would leave the old list reference outstanding.
        // Normal QList assignment and destruction return the source to refcount 1.
        QVERIFY(original.line.points.isDetached());
        QCOMPARE(original.line.points.constData(), originalPoints);
        QCOMPARE(original.line.points.size(), 2);
        QCOMPARE(original.line.points.at(0).x, 10.f);
        QCOMPARE(original.line.bounds, QRectF(8.5, 18.5, 23, 23));
        QVERIFY(clone->header == original.header);
        QVERIFY(clone->trailer == original.trailer);
    }

    void independentClonesHaveIndependentReplacementGeometry() {
        const auto original = seed();
        ItemsModel clones;
        for (int i = 0; i < 8; ++i) clones.append(std::make_shared<ItemModel>(original));
        for (int i = 0; i < clones.size(); ++i) {
            replacePayload(*clones.at(i), replacement(100 + 50 * i));
            QVERIFY(clones.at(i)->header == original.header);
            QVERIFY(clones.at(i)->trailer == original.trailer);
        }
        QVERIFY(original.line.points.isDetached());
        for (int i = 0; i < clones.size(); ++i) {
            QCOMPARE(clones.at(i)->line.points.at(0).x, float(100 + 50 * i));
            for (int j = 0; j < i; ++j) {
                QVERIFY(clones.at(i).get() != clones.at(j).get());
                QVERIFY(clones.at(i)->line.points.constData() != clones.at(j)->line.points.constData());
            }
        }
    }

    void nativeStyleSecondCloneDetachesPointsOnTranslation() {
        auto prepared = std::make_shared<ItemModel>(seed());
        replacePayload(*prepared, replacement(100));
        auto submitted = std::make_shared<ItemModel>(*prepared);
        QCOMPARE(submitted->line.points.constData(), prepared->line.points.constData());
        for (auto &point : submitted->line.points) {
            point.x += 25;
            point.y -= 10;
        }
        submitted->line.bounds.translate(25, -10);
        QVERIFY(submitted->line.points.constData() != prepared->line.points.constData());
        QCOMPARE(prepared->line.points.at(0).x, 100.f);
        QCOMPARE(prepared->line.points.at(0).y, 60.f);
        QCOMPARE(submitted->line.points.at(0).x, 125.f);
        QCOMPARE(submitted->line.bounds, QRectF(122.5, 47.5, 25, 15));
        QVERIFY(submitted->header == prepared->header);
        QVERIFY(submitted->trailer == prepared->trailer);
        prepared.reset();
        QCOMPARE(submitted->line.points.last().x, 145.f);
    }

    void variantAndQueuedCopyRetainOwnedItemsUntilLastConsumer() {
        int deleted = 0;
        ItemsModel queued;
        QList<std::weak_ptr<ItemModel>> lifetimes;
        {
            ItemsModel prepared;
            for (int i = 0; i < 4; ++i) {
                auto item = std::shared_ptr<ItemModel>(new ItemModel(seed()), [&deleted](ItemModel *p) {
                    ++deleted;
                    delete p;
                });
                replacePayload(*item, replacement(100 + 50 * i));
                lifetimes.append(item);
                prepared.append(std::move(item));
            }
            QVariant value(QMetaType::fromType<ItemsModel>(), &prepared);
            const QVariant qmlCopy = value;
            prepared.clear();
            value.clear();
            QCOMPARE(deleted, 0);
            // A queued consumer owns a copy after the input QVariant vanishes.
            queued = *static_cast<const ItemsModel *>(qmlCopy.constData());
        }
        QCOMPARE(deleted, 0);
        for (const auto &weak : lifetimes) QVERIFY(!weak.expired());
        QCOMPARE(queued.at(3)->line.points.at(0).x, 250.f);
        auto lastConsumer = queued;
        queued.clear();
        QCOMPARE(deleted, 0);
        lastConsumer.clear();
        QCOMPARE(deleted, 4);
        for (const auto &weak : lifetimes) QVERIFY(weak.expired());
    }
};

QTEST_APPLESS_MAIN(NativeLineReplacementTest)
#include "NativeLineReplacementTest.moc"

#define REPAPER_SELECTION_READER_TEST 1
#include "NativeSelectionProbe.cpp"
#include <QtTest>
#include <array>

class SceneController:public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* worker READ worker)
public:
    mutable int getterCalls=0;
    QObject*worker()const {++getterCalls;return nullptr;}
};
class NativeSelectionReaderTest:public QObject {
    Q_OBJECT
    template<class T,size_t N> static void put(std::array<char,N>&memory,size_t offset,T value) {
        Q_ASSERT(offset+sizeof(T)<=N);std::memcpy(memory.data()+offset,&value,sizeof(T));
    }
    template<size_t N> static Word address(std::array<char,N>&memory){return reinterpret_cast<Word>(memory.data());}
    template<size_t N> static void text(std::array<char,N>&memory,size_t offset,const QString&value) {
        put(memory,offset+8,reinterpret_cast<Word>(value.constData()));put(memory,offset+16,qsizetype(value.size()));
    }
private slots:
    void pcCannotReachNativeGetters() {
        SceneController c;QObject wrong;
        QCOMPARE(RePaperNative::observeOriginalSelection(nullptr,0)["status"],QString("wrong-thread-or-controller"));
        QCOMPARE(RePaperNative::observeOriginalSelection(&wrong,0)["status"],QString("wrong-controller-type"));
        QCOMPARE(RePaperNative::observeOriginalSelection(&c,-1)["status"],QString("invalid-layer"));
        const auto result=RePaperNative::observeOriginalSelection(&c,0);
        QCOMPARE(result["status"],QString("unsupported-build"));QVERIFY(!result["complete"].toBool());
        QCOMPARE(c.getterCalls,0);
    }
    void rangesRejectOverflowAndUnmappedMemory() {
        const Mappings maps;Word result=0;
        QVERIFY(!maps.read(0,result));QVERIFY(!maps.read(1,result));
        QVERIFY(!maps.contains(std::numeric_limits<Word>::max()-2,8));
    }
    void activeTreeFindsOnlyMatchingPageAndRejectsCycle() {
        alignas(8) std::array<char,0x218> worker{};
        alignas(8) std::array<char,0x40> node{};
        const QString key="fixture-page-b",other="fixture-page-a";
        put(worker,0x208,address(node));text(node,0x20,key);
        const Mappings maps;
        QVERIFY(activePage(maps,address(worker),key));QVERIFY(!activePage(maps,address(worker),other));
        put(node,0x10,address(node));QVERIFY(!activePage(maps,address(worker),other));
        put(node,0x30,qsizetype(129));QVERIFY(!activePage(maps,address(worker),key));
    }
    void pageVectorRequiresTypedExistingData() {
        alignas(8) std::array<char,0x218> worker{};
        alignas(8) std::array<char,16> entries{};
        alignas(8) std::array<char,0xe0> data{};
        const QString key="fixture-page";
        put(worker,0x1e0,address(entries));put(worker,0x1e8,address(entries)+16);
        put(entries,0,address(data));put(data,0,Word(0x15568d8));text(data,0x40,key);
        const Mappings maps;
        QCOMPARE(existingPageData(maps,address(worker),key),address(data));
        QCOMPARE(existingPageData(maps,address(worker),"missing"),Word(0));
        put(data,0,Word(0x1234));QCOMPARE(existingPageData(maps,address(worker),key),Word(0));
        put(worker,0x1e8,address(entries)+17);QCOMPARE(existingPageData(maps,address(worker),key),Word(0));
    }
    void hashChecksEntryIndexesCountAndDuplicates() {
        alignas(8) std::array<char,8> owner{};
        alignas(8) std::array<char,0x28> hash{};
        alignas(8) std::array<char,0x90> span{};
        alignas(8) std::array<char,24> entry{};
        std::memset(span.data(),0xff,128);
        put(owner,0,address(hash));put(hash,8,Word(1));put(hash,0x10,Word(128));put(hash,0x20,address(span));
        put(span,0x80,address(entry));put(span,0x88,quint8(1));put(span,0,quint8(0));
        put(entry,0,quint64(0x1000000000001));put(entry,8,Word(0x1234));
        const Mappings maps;QList<HashPair> pairs;
        QVERIFY(hashPairs(maps,address(owner),pairs));QCOMPARE(pairs.size(),1);QCOMPARE(pairs[0].key,quint64(0x1000000000001));
        put(span,0,quint8(1));QVERIFY(!hashPairs(maps,address(owner),pairs));
        put(span,0,quint8(0));put(span,1,quint8(0));QVERIFY(!hashPairs(maps,address(owner),pairs));
        put(span,1,quint8(0xff));put(hash,8,Word(2));QVERIFY(!hashPairs(maps,address(owner),pairs));
        put(hash,8,Word(1));put(span,0x80,Word(1));QVERIFY(!hashPairs(maps,address(owner),pairs));
        put(hash,0x10,Word(65536));QVERIFY(!hashPairs(maps,address(owner),pairs));
    }
    void listsRejectOversizeAndInvalidDataBeforeDereference() {
        alignas(8) std::array<char,24> list{};qsizetype count=9;const Mappings maps;
        QVERIFY(itemListHeader(maps,address(list),count));QCOMPARE(count,qsizetype(0));
        put(list,0x10,qsizetype(129));QVERIFY(!itemListHeader(maps,address(list),count));
        put(list,0x10,qsizetype(-1));QVERIFY(!itemListHeader(maps,address(list),count));
        put(list,0x10,qsizetype(1));put(list,8,Word(1));QVERIFY(!itemListHeader(maps,address(list),count));
    }
};
QTEST_GUILESS_MAIN(NativeSelectionReaderTest)
#include "NativeSelectionReaderTest.moc"

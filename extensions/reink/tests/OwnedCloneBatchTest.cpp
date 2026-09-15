#include "OwnedCloneBatch.h"
#include "NativeCreationSession.h"
#include "NativeInsertObservation.h"
#include <QtTest>

namespace {
struct Item { bool line=true; int value=7; };
using Items=QList<std::shared_ptr<Item>>;
const auto accepts=[](const Item *item){return item->line;};
}
class OwnedCloneBatchTest:public QObject {
    Q_OBJECT
private slots:
    void insertionObservationNeverTreatsDispatchOrStaleSelectionAsConfirmation() {
        const QVariantMap page{{"documentId","test-document"},{"pageId","page-a"},{"layer",0}};
        const auto observation=[&](QStringList ids){QVariantList items;for(const auto &id:ids)items.append(QVariantMap{{"idHex",id},{"type","SceneLineItem"}});
            return QVariantMap{{"status","observed"},{"complete",true},{"count",items.size()},{"items",items},{"layer",0},
                {"pageIdSha256",QString::fromLatin1(QCryptographicHash::hash(page.value("pageId").toString().toUtf8(),QCryptographicHash::Sha256).toHex())}};};
        const auto before=observation({"0001000000000001"});
        const auto after=observation({"0001000000000002","0001000000000003"});
        RePaperNative::NativeInsertObservation pending;pending.begin(page,before,2);QVERIFY(pending.pending());
        QVERIFY(!pending.observe(page,before));QVERIFY(pending.pending());
        QVERIFY(!pending.observe(page,{{"status","busy"}}));QVERIFY(pending.pending());
        auto wrongPage=page;wrongPage["pageId"]="page-b";QVERIFY(!pending.observe(wrongPage,after));
        auto incomplete=after;incomplete["complete"]=false;QVERIFY(!pending.observe(page,incomplete));
        auto foreign=after;foreign["pageIdSha256"]=QString(64,'f');QVERIFY(!pending.observe(page,foreign));
        foreign=after;foreign["layer"]=1;QVERIFY(!pending.observe(page,foreign));
        QVERIFY(!pending.observe(page,observation({"0001000000000001","0001000000000002"})));
        QVERIFY(!pending.observe(page,observation({"0001000000000002","0001000000000002"})));
        QVERIFY(pending.observe(page,after));QVERIFY(!pending.pending());
        // A blank page is a valid source context, and requires no lasso seed.
        pending.begin(page,observation({}),2);QVERIFY(pending.observe(page,after));QVERIFY(!pending.pending());
        pending.begin(page,{{"status","busy"}},2);QVERIFY(!pending.observe(page,after));QVERIFY(pending.pending());
        foreign=before;foreign["pageIdSha256"]=QString(64,'f');pending.begin(page,foreign,2);QVERIFY(!pending.observe(page,after));
        pending.clear();QVERIFY(!pending.pending());
    }
    void creationRequiresExplicitVolatileMatchingScope() {
        RePaperNative::NativeCreationSession session;
        const QVariantMap page{{"documentId","test-document"},{"pageId","page-a"},{"layer",0}};
        QVERIFY(!session.armed());QVERIFY(!session.matches(page));
        QVERIFY(!session.arm(page,false));QVERIFY(!session.matches(page));
        QVERIFY(session.arm(page,true));QVERIFY(session.matches(page));
        for(const auto key:{"documentId","pageId","layer"}){
            auto changed=page;changed[key]=QString(key)=="layer"?QVariant(1):QVariant("different");
            QVERIFY(!session.matches(changed));
        }
        session.clear();QVERIFY(!session.armed());QVERIFY(!session.matches(page));
        for(const auto key:{"documentId","pageId","layer"}){auto missing=page;missing.remove(key);QVERIFY(!session.arm(missing,true));}
    }
    void independentClonesPreserveSeedAndLifetime() {
        auto source=std::make_shared<Item>();
        int calls=0;QString error;
        auto batch=RePaperNative::collectOwnedClones<Item>(5,[&](bool *valid){
            *valid=true;++calls;return Items{std::make_shared<Item>(*source),std::make_shared<Item>(*source)};
        },accepts,&error);
        QCOMPARE(batch.size(),5);QCOMPARE(calls,3);QVERIFY(error.isEmpty());
        for(int i=0;i<batch.size();++i){batch[i]->value=i;for(int j=0;j<i;++j)QVERIFY(batch[i].get()!=batch[j].get());}
        QCOMPARE(source->value,7);QCOMPARE(batch[4]->value,4);
        std::weak_ptr<Item> weak=batch.front();batch.clear();QVERIFY(weak.expired());
    }
    void aliasingClonesAreRejectedWithoutTouchingSeed() {
        const auto seed=std::make_shared<Item>();QString error;
        const auto result=RePaperNative::collectOwnedClones<Item>(2,[&](bool *valid){*valid=true;return Items{seed};},accepts,&error);
        QVERIFY(result.isEmpty());QVERIFY(error.contains("réutilisé"));QCOMPARE(seed->value,7);
    }
    void emptyInvalidAndNonLineSelectionsFail() {
        for(int mode=0;mode<3;++mode){QString error;int calls=0;
            const auto result=RePaperNative::collectOwnedClones<Item>(1,[&](bool *valid){
                ++calls;*valid=mode!=0;return mode==2?Items{std::make_shared<Item>(Item{false,7})}:Items{};
            },accepts,&error);
            QVERIFY(result.isEmpty());QVERIFY(!error.isEmpty());QCOMPARE(calls,1);
        }
    }
    void budgetsRejectBeforeAnyMutation() {
        for(int mode=0;mode<4;++mode){QString error;int calls=0;RePaperNative::CloneBudget budget;
            if(mode==0)budget.outputItems=1;
            if(mode==1)budget.inputItemsPerCall=1;
            if(mode==2)budget.inspectedItems=1;
            if(mode==3)budget.calls=1;
            const auto result=RePaperNative::collectOwnedClones<Item>(2,[&](bool *valid){
                ++calls;*valid=true;Items items{std::make_shared<Item>()};if(mode==1)items.append(std::make_shared<Item>());return items;
            },accepts,&error,budget);
            QVERIFY(result.isEmpty());QVERIFY(!error.isEmpty());if(mode==0)QCOMPARE(calls,0);
        }
    }
};
QTEST_APPLESS_MAIN(OwnedCloneBatchTest)
#include "OwnedCloneBatchTest.moc"

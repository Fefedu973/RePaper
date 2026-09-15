#include "NativeObjectRegistry.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace repaper::drawing;
class NativeObjectRegistryTest:public QObject {
    Q_OBJECT
    static SemanticObject object(QString id,int width=100){return {id,{{"kind","rectangle"},{"width",width}}};}
    static NativeBinding binding(QString objectId,QString nativeId,QString version="v1"){return {objectId,{{nativeId,version}}};}
    static QByteArray read(const QString &path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
    static bool write(const QString &path,const QByteArray &bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
    static void openEmpty(NativeObjectRegistry &registry){
        QVERIFY(registry.activate("document","page"));QVERIFY(!registry.editable());
        QVERIFY(registry.observe(registry.epoch(),{}));QVERIFY(registry.editable());
    }
private slots:
    void confirmationIsRequiredAndRestartLocksUntilObserved(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto ticket=registry.begin({object("box")});QVERIFY(ticket.valid());QVERIFY(!registry.editable());
        QVERIFY(registry.confirm(ticket,{binding("box","native-A")}));QVERIFY(registry.editable());
        NativeObjectRegistry restarted(directory.path());QVERIFY(restarted.activate("document","page"));QVERIFY(!restarted.editable());
        QCOMPARE(restarted.objects().size(),1);QVERIFY(!restarted.begin({object("box",200)}).valid());
        QVERIFY(restarted.observe(restarted.epoch(),{{"native-A","v1"},{"unmanaged-ink","opaque-content"}}));QVERIFY(restarted.editable());
        QCOMPARE(restarted.objects()[0].parameters.value("width").toInt(),100);
    }
    void pageEpochRejectsLateRepliesAndNeverMixesPages(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        const auto oldEpoch=registry.epoch();auto ticket=registry.begin({object("old")});
        QVERIFY(registry.activate("document","other-page"));const auto otherPath=registry.storagePath();
        QVERIFY(!registry.confirm(ticket,{binding("old","native-old")}));QVERIFY(!registry.observe(oldEpoch,{}));
        QVERIFY(registry.objects().isEmpty());QVERIFY(!QFile::exists(otherPath));
        QVERIFY(registry.observe(registry.epoch(),{}));auto second=registry.begin({object("other")});
        QVERIFY(registry.confirm(second,{binding("other","native-other")}));
        QVERIFY(registry.activate("document","page"));QVERIFY(registry.objects().isEmpty());QVERIFY(!registry.editable());
    }
    void canceledOrUnattributedOperationStaysLocked(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto ticket=registry.begin({object("box")});QVERIFY(registry.cancel(ticket));QVERIFY(!registry.editable());
        QVERIFY(!registry.confirm(ticket,{binding("box","native-A")}));QVERIFY(registry.observe(registry.epoch(),{}));
        auto next=registry.begin({object("box")});QVERIFY(!registry.observe(registry.epoch(),{{"unattributed-item","v1"}}));
        QVERIFY(!registry.confirm(next,{binding("box","native-A")}));QVERIFY(!registry.editable());QVERIFY(!registry.pending());
    }
    void partialDeletionAndUnrecognizedEditLockWithoutChangingStoredParameters(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto ticket=registry.begin({object("component")});NativeBinding composed{"component",{{"line-A","v1"},{"line-B","v1"}}};
        QVERIFY(registry.confirm(ticket,{composed}));const auto original=read(registry.storagePath());
        QVERIFY(!registry.observe(registry.epoch(),{{"line-A","v1"}}));QVERIFY(!registry.editable());QCOMPARE(read(registry.storagePath()),original);
        QVERIFY(!registry.begin({object("component",200)}).valid());
        QVERIFY(!registry.observe(registry.epoch(),{{"line-A","changed"},{"line-B","v1"}}));QCOMPARE(registry.objects()[0],object("component"));
        QVERIFY(registry.observe(registry.epoch(),{{"line-B","v1"},{"line-A","v1"}}));QVERIFY(registry.editable());
    }
    void undoRedoRestoresKnownSemanticSnapshots(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto first=registry.begin({object("box",100)});QVERIFY(registry.confirm(first,{binding("box","native-A","content-1")}));
        auto second=registry.begin({object("box",200)});QVERIFY(registry.confirm(second,{binding("box","native-A","content-2")}));
        QVERIFY(registry.observe(registry.epoch(),{{"native-A","content-1"}}));QCOMPARE(registry.objects()[0].parameters["width"].toInt(),100);
        QVERIFY(registry.observe(registry.epoch(),{{"native-A","content-2"}}));QCOMPARE(registry.objects()[0].parameters["width"].toInt(),200);
        QVERIFY(registry.observe(registry.epoch(),{}));QVERIFY(registry.objects().isEmpty());
        QVERIFY(registry.observe(registry.epoch(),{{"native-A","content-2"}}));QCOMPARE(registry.objects().size(),1);
    }
    void ambiguousHistoryNeverPicksAStateByPosition(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto first=registry.begin({object("box",100)});QVERIFY(registry.confirm(first,{binding("box","native-A","same-content")}));
        auto second=registry.begin({object("box",200)});QVERIFY(registry.confirm(second,{binding("box","native-A","same-content")}));
        const auto original=read(registry.storagePath());
        QVERIFY(!registry.observe(registry.epoch(),{{"native-A","same-content"}}));QVERIFY(!registry.editable());
        QVERIFY(registry.error().contains("Multiple"));QCOMPARE(read(registry.storagePath()),original);
    }
    void copiesHaveIndependentObjectAndNativeIdentities(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto first=registry.begin({object("original")});QVERIFY(registry.confirm(first,{binding("original","native-A")}));
        auto copy=registry.begin({object("original"),object("copy")});
        QVERIFY(registry.confirm(copy,{binding("copy","native-B"),binding("original","native-A")}));
        auto edit=registry.begin({object("copy",250),object("original")});
        QVERIFY(registry.confirm(edit,{binding("original","native-A"),binding("copy","native-B","v2")}));
        QVERIFY(registry.observe(registry.epoch(),{{"native-B","v2"},{"native-A","v1"}}));
        QCOMPARE(registry.objects()[0].id,QString("copy"));QCOMPARE(registry.objects()[0].parameters["width"].toInt(),250);
        QCOMPARE(registry.objects()[1].id,QString("original"));QCOMPARE(registry.objects()[1].parameters["width"].toInt(),100);
    }
    void incompleteOrOverlappingConfirmationDoesNotPersist(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto ticket=registry.begin({object("one"),object("two")});
        QVERIFY(!registry.confirm(ticket,{binding("one","same"),binding("two","same")}));QVERIFY(!registry.editable());
        QVERIFY(!QFile::exists(registry.storagePath()));
        QVERIFY(registry.observe(registry.epoch(),{}));ticket=registry.begin({object("one"),object("two")});
        QVERIFY(!registry.confirm(ticket,{binding("one","native-A")}));QVERIFY(!registry.editable());
    }
    void malformedOrForeignPageFilesAreNeverOverwritten(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());openEmpty(registry);
        auto ticket=registry.begin({object("box")});QVERIFY(registry.confirm(ticket,{binding("box","native-A")}));const auto path=registry.storagePath();
        auto saved=QJsonDocument::fromJson(read(path)).object();saved.insert("pageId","different");
        const auto foreign=QJsonDocument(saved).toJson();QVERIFY(write(path,foreign));
        QVERIFY(!registry.activate("document","page"));QVERIFY(!registry.observe(registry.epoch(),{{"native-A","v1"}}));QCOMPARE(read(path),foreign);
        QVERIFY(write(path,"invalid original"));QVERIFY(!registry.activate("document","page"));
        QVERIFY(!registry.begin({object("new")}).valid());QCOMPARE(read(path),QByteArray("invalid original"));
    }
    void ioFailureRetainsPreviousStateAndLocks(){
        QTemporaryDir directory;const auto occupied=directory.filePath("not-a-directory");QVERIFY(write(occupied,"keep"));
        NativeObjectRegistry registry(occupied);openEmpty(registry);auto ticket=registry.begin({object("box")});
        QVERIFY(!registry.confirm(ticket,{binding("box","native-A")}));QVERIFY(!registry.editable());
        QVERIFY(registry.objects().isEmpty());QCOMPARE(read(occupied),QByteArray("keep"));
    }
    void opaquePageNamesCannotEscapeStorageDirectory(){
        QTemporaryDir directory;NativeObjectRegistry registry(directory.path());
        QVERIFY(registry.activate("../../private/\u00e9", "../other/page"));QVERIFY(registry.observe(registry.epoch(),{}));
        auto ticket=registry.begin({object("box")});QVERIFY(registry.confirm(ticket,{binding("box","opaque-native-id")}));
        QCOMPARE(QFileInfo(registry.storagePath()).absolutePath(),QFileInfo(directory.path()).absoluteFilePath());
        QCOMPARE(QDir(directory.path()).entryList({"*.registry.json"},QDir::Files).size(),1);
    }
    void staleWriterCannotOverwriteAnotherConfirmedRegistry(){
        QTemporaryDir directory;NativeObjectRegistry first(directory.path()),stale(directory.path());openEmpty(first);openEmpty(stale);
        auto a=first.begin({object("first")});auto b=stale.begin({object("stale")});
        QVERIFY(first.confirm(a,{binding("first","native-A")}));const auto confirmed=read(first.storagePath());
        QVERIFY(!stale.confirm(b,{binding("stale","native-B")}));QVERIFY(!stale.editable());QCOMPARE(read(first.storagePath()),confirmed);
        QVERIFY(stale.activate("document","page"));QVERIFY(!stale.editable());
        QVERIFY(stale.observe(stale.epoch(),{{"native-A","v1"}}));QCOMPARE(stale.objects()[0].id,QString("first"));
    }
};
QTEST_GUILESS_MAIN(NativeObjectRegistryTest)
#include "NativeObjectRegistryTest.moc"

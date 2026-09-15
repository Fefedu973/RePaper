#include "PdfLibrary.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

namespace {
QString id(int value) {
    return QStringLiteral("00000000-0000-4000-8000-%1").arg(value,12,16,QLatin1Char('0'));
}
bool bytes(const QString &path,const QByteArray &value) {
    QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(value)==value.size();
}
bool json(const QString &path,const QJsonObject &value) {return bytes(path,QJsonDocument(value).toJson(QJsonDocument::Compact));}
struct Fixture {
    QTemporaryDir directory;
    QString library=directory.filePath("library"),files=directory.filePath("files");
    Fixture() {QDir().mkpath(library);QDir().mkpath(files);}
    bool metadata(const QString &entryId,const QString &title,const QString &parent={},bool folder=false,bool deleted=false,QJsonObject extra={}) {
        extra.insert("type",folder?"CollectionType":"DocumentType");extra.insert("visibleName",title);
        extra.insert("parent",parent);extra.insert("deleted",deleted);
        return json(library+'/'+entryId+".metadata",extra);
    }
    bool pdf(const QString &entryId,bool downloaded=true,QJsonObject content={{"fileType","pdf"}}) {
        return json(library+'/'+entryId+".content",content)
            &&(!downloaded||bytes(library+'/'+entryId+".pdf","%PDF-1.4\n%%EOF\n"));
    }
};
int rowFor(const PdfLibrary &model,const QString &entryId) {
    for(int row=0;row<model.count();++row)if(model.data(model.index(row),PdfLibrary::EntryIdRole).toString()==entryId)return row;
    return -1;
}
QVariant value(const PdfLibrary &model,const QString &entryId,int role) {return model.data(model.index(rowFor(model,entryId)),role);}
QStringList titles(const PdfLibrary &model) {
    QStringList result;for(int row=0;row<model.count();++row)result.append(model.data(model.index(row),PdfLibrary::TitleRole).toString());return result;
}
QMap<QString,QByteArray> contents(const QString &root) {
    QMap<QString,QByteArray> result;QDirIterator iterator(root,QDir::Files,QDirIterator::Subdirectories);
    while(iterator.hasNext()) {const auto path=iterator.next();QFile file(path);if(file.open(QIODevice::ReadOnly))result.insert(path,file.readAll());}
    return result;
}
}

class PdfLibraryTest : public QObject {
    Q_OBJECT
private slots:
    void nativeTitlesFoldersGlobalFilterAndMissingDownloads() {
        Fixture f;QVERIFY(f.directory.isValid());
        QVERIFY(f.metadata(id(1),"Sciences",{},true));
        QVERIFY(f.metadata(id(2),"Première",id(1),true));
        QVERIFY(f.metadata(id(3),"Électricité",id(2)));QVERIFY(f.pdf(id(3)));
        QVERIFY(f.metadata(id(4),"À lire plus tard",id(2)));QVERIFY(f.pdf(id(4),false));
        QVERIFY(f.metadata(id(5),"Cours général"));QVERIFY(f.pdf(id(5)));
        QVERIFY(f.metadata(id(6),"Carnet manuscrit",id(2)));QVERIFY(f.pdf(id(6),true,{{"fileType","notebook"}}));
        const auto before=contents(f.library);
        PdfLibrary model(f.library,f.files);QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy counts(&model,&PdfLibrary::countChanged);QSignalSpy filters(&model,&PdfLibrary::filterChanged);
        QTRY_VERIFY(!model.busy());QVERIFY(model.error().isEmpty());QCOMPARE(model.mode(),QString("library"));
        QCOMPARE(model.locationTitle(),QString("Bibliothèque"));QVERIFY(!model.canGoBack());
        QCOMPARE(titles(model),QStringList({"Sciences","Cours général"}));QVERIFY(!counts.isEmpty());
        QCOMPARE(model.roleNames().value(PdfLibrary::FilePathRole),QByteArray("filePath"));
        QVERIFY(!model.data(model.index(99),PdfLibrary::TitleRole).isValid());
        model.setFilter("ÉLECT");QCOMPARE(model.count(),1);QCOMPARE(filters.size(),1);
        QCOMPARE(value(model,id(3),PdfLibrary::TitleRole).toString(),QString("Électricité"));
        QVERIFY(value(model,id(3),PdfLibrary::SubtitleRole).toString().contains("Sciences / Première"));
        QCOMPARE(value(model,id(3),PdfLibrary::FilePathRole).toString(),QFileInfo(f.library+'/'+id(3)+".pdf").canonicalFilePath());
        QVERIFY(value(model,id(3),PdfLibrary::AvailableRole).toBool());
        model.openFolder(id(2)); // Not listed by this filter.
        QCOMPARE(model.locationTitle(),QString("Bibliothèque"));QVERIFY(!model.busy());
        model.setFilter("première");QCOMPARE(model.count(),1);model.openFolder(id(2));
        QTRY_VERIFY(!model.busy());QCOMPARE(model.filter(),QString());QCOMPARE(model.locationTitle(),QString("Première"));
        QVERIFY(model.canGoBack());QCOMPARE(model.count(),2);
        QVERIFY(!value(model,id(4),PdfLibrary::AvailableRole).toBool());
        QVERIFY(value(model,id(4),PdfLibrary::FilePathRole).toString().isEmpty());
        QVERIFY(value(model,id(4),PdfLibrary::SubtitleRole).toString().startsWith(QString::fromUtf8("À télécharger")));
        QVERIFY(rowFor(model,id(6))<0);
        model.goUp();QTRY_VERIFY(!model.busy());QCOMPARE(model.locationTitle(),QString("Sciences"));
        QCOMPARE(titles(model),QStringList({"Première"}));model.goUp();QTRY_VERIFY(!model.busy());
        QVERIFY(!model.canGoBack());model.goUp();QCOMPARE(model.locationTitle(),QString("Bibliothèque"));
        QCOMPARE(contents(f.library),before); // No sidecar, metadata or PDF writes.
    }

    void deletedTrashOrphanCycleAndDocumentAncestorsAreSuppressed() {
        Fixture f;
        QVERIFY(f.metadata(id(1),"Dossier supprimé",{},true,true));
        QVERIFY(f.metadata(id(2),"Sous-dossier supprimé",id(1),true));
        QVERIFY(f.metadata(id(3),"Secret supprimé",id(2)));QVERIFY(f.pdf(id(3)));
        QVERIFY(f.metadata(id(4),"Corbeille",QString("TRASH"),true));
        QVERIFY(f.metadata(id(5),"Secret corbeille",id(4)));QVERIFY(f.pdf(id(5)));
        QVERIFY(f.metadata(id(6),"Secret direct",{},false,true));QVERIFY(f.pdf(id(6)));
        QVERIFY(f.metadata(id(7),"Secret orphelin",id(999)));QVERIFY(f.pdf(id(7)));
        QVERIFY(f.metadata(id(8),"Cycle A",id(9),true));QVERIFY(f.metadata(id(9),"Cycle B",id(8),true));
        QVERIFY(f.metadata(id(10),"Secret cycle",id(8)));QVERIFY(f.pdf(id(10)));
        QVERIFY(f.metadata(id(11),"PDF visible"));QVERIFY(f.pdf(id(11)));
        QVERIFY(f.metadata(id(12),"Secret sous PDF",id(11)));QVERIFY(f.pdf(id(12)));
        QVERIFY(f.metadata(id(13),"Secret chemin",QString("../outside")));QVERIFY(f.pdf(id(13)));
        PdfLibrary model(f.library,f.files);QTRY_VERIFY(!model.busy());
        QCOMPARE(titles(model),QStringList({"PDF visible"}));
        model.setFilter("secret");QCOMPARE(model.count(),0);
        model.setFilter("cycle");QCOMPARE(model.count(),0);
    }

    void fallbackRequiresAbsentFileTypeAndSafeUuidFilenames() {
        Fixture f;
        QVERIFY(f.metadata(id(1),"PDF avec contenu"));QVERIFY(f.pdf(id(1),true,{{"fileType","PDF"}}));
        QVERIFY(f.metadata(id(2),"PDF sans fileType"));QVERIFY(f.pdf(id(2),true,{}));
        QVERIFY(f.metadata(id(3),"PDF sans content"));QVERIFY(bytes(f.library+'/'+id(3)+".pdf","%PDF-1.4\n"));
        QVERIFY(f.metadata(id(4),"Carnet avec ancien PDF"));QVERIFY(f.pdf(id(4),true,{{"fileType","notebook"}}));
        QVERIFY(f.metadata(id(5),"Content malformé"));QVERIFY(f.pdf(id(5)));QVERIFY(bytes(f.library+'/'+id(5)+".content","{broken"));
        QVERIFY(f.metadata(id(6),"Type vide"));QVERIFY(f.pdf(id(6),true,{{"fileType",""}}));
        QVERIFY(f.metadata(id(7),"ID JSON ignoré",{},false,false,{{"id","../../outside.pdf"}}));QVERIFY(f.pdf(id(7)));
        QVERIFY(f.metadata("not-a-uuid","Nom de fichier arbitraire"));QVERIFY(f.pdf("not-a-uuid"));
        QVERIFY(f.metadata(id(8),"Parent arbitraire","not-a-uuid"));QVERIFY(f.pdf(id(8)));
        QVERIFY(f.metadata(id(9),"Metadata trop gros",{},false,false,{{"padding",QString(256*1024,'x')}}));QVERIFY(f.pdf(id(9)));
        QVERIFY(f.metadata(id(10),"Content trop gros"));QVERIFY(f.pdf(id(10),true,{{"fileType","pdf"},{"padding",QString(256*1024,'x')}}));
        PdfLibrary model(f.library,f.files);QTRY_VERIFY(!model.busy());QCOMPARE(model.count(),4);
        for(int entry:{1,2,3,7})QVERIFY(rowFor(model,id(entry))>=0);
        QCOMPARE(value(model,id(7),PdfLibrary::EntryIdRole).toString(),id(7));
        QCOMPARE(value(model,id(7),PdfLibrary::FilePathRole).toString(),QFileInfo(f.library+'/'+id(7)+".pdf").canonicalFilePath());
    }

    void duplicateCaseUuidMetadataCannotAcquireChildren() {
#ifndef Q_OS_UNIX
        QSKIP("Case-distinct native filenames require a case-sensitive test filesystem.");
#else
        Fixture f;const QString duplicate="abcdef01-2345-4678-9abc-def012345678";
        QVERIFY(f.metadata(duplicate,"Premier dossier",{},true));
        QVERIFY(f.metadata(duplicate.toUpper(),"Dossier ambigu",{},true));
        QVERIFY(f.metadata(id(1),"Enfant ambigu",duplicate));QVERIFY(f.pdf(id(1)));
        PdfLibrary model(f.library,f.files);QTRY_VERIFY(!model.busy());QCOMPARE(model.count(),0);
        model.setFilter("ambigu");QCOMPARE(model.count(),0);
#endif
    }

    void nativeSymlinksNeverReadOrExposeOutsideFiles() {
        Fixture f;const auto outside=f.directory.filePath("outside");QVERIFY(QDir().mkpath(outside));
        QVERIFY(json(outside+"/folder.metadata",{{"type","CollectionType"},{"visibleName","Dossier extérieur"},{"parent",""}}));
        if(!QFile::link(outside+"/folder.metadata",f.library+'/'+id(1)+".metadata"))QSKIP("Symbolic links unavailable.");
        QVERIFY(f.metadata(id(2),"Enfant extérieur",id(1)));QVERIFY(f.pdf(id(2)));
        QVERIFY(f.metadata(id(3),"PDF extérieur"));QVERIFY(f.pdf(id(3),false));
        QVERIFY(bytes(outside+"/private.pdf","%PDF-1.4\nprivate"));QVERIFY(QFile::link(outside+"/private.pdf",f.library+'/'+id(3)+".pdf"));
        QVERIFY(f.metadata(id(4),"Content extérieur"));QVERIFY(bytes(f.library+'/'+id(4)+".pdf","%PDF-1.4\n"));
        QVERIFY(json(outside+"/document.content",{{"fileType","pdf"}}));QVERIFY(QFile::link(outside+"/document.content",f.library+'/'+id(4)+".content"));
        PdfLibrary model(f.library,f.files);QTRY_VERIFY(!model.busy());QCOMPARE(model.count(),1);
        QCOMPARE(titles(model),QStringList({"PDF extérieur"}));QVERIFY(!value(model,id(3),PdfLibrary::AvailableRole).toBool());
        QVERIFY(value(model,id(3),PdfLibrary::FilePathRole).toString().isEmpty());
    }

    void filesystemNavigationFilteringAndRootBoundary() {
        Fixture f;const auto folder=f.files+"/Documents",nested=folder+"/Deuxième année";
        QVERIFY(QDir().mkpath(nested));QVERIFY(QDir().mkpath(f.files+"/.hidden"));
        QVERIFY(bytes(f.files+"/RÉSUMÉ.PDF","%PDF-1.4\n"));QVERIFY(bytes(f.files+"/.secret.pdf","%PDF-1.4\n"));
        QVERIFY(bytes(f.files+"/texte.txt","text"));QVERIFY(bytes(folder+"/Cours.pdf","%PDF-1.4\n"));
        const auto before=contents(f.files);
        PdfLibrary model(f.library,f.files);model.showFiles();QTRY_VERIFY(!model.busy());
        QCOMPARE(model.mode(),QString("files"));QCOMPARE(model.locationTitle(),QString("Fichiers"));QVERIFY(!model.canGoBack());
        QCOMPARE(titles(model),QStringList({"Documents","RÉSUMÉ.PDF"}));
        const auto canonicalFolder=QFileInfo(folder).canonicalFilePath();
        QVERIFY(value(model,canonicalFolder,PdfLibrary::FolderRole).toBool());
        model.openFolder(folder+"/../..");QCOMPARE(model.locationTitle(),QString("Fichiers"));QVERIFY(!model.busy());
        model.setFilter("résu");QCOMPARE(titles(model),QStringList({"RÉSUMÉ.PDF"}));
        model.openFolder(canonicalFolder);QVERIFY(!model.busy()); // Filtered-out folder is not listed.
        model.setFilter({});model.openFolder(canonicalFolder);QTRY_VERIFY(!model.busy());
        QCOMPARE(model.locationTitle(),QString("Documents"));QVERIFY(model.canGoBack());
        QCOMPARE(titles(model),QStringList({"Deuxième année","Cours.pdf"}));
        model.openFolder(QFileInfo(nested).canonicalFilePath());QTRY_VERIFY(!model.busy());QCOMPARE(model.count(),0);
        model.goUp();QTRY_VERIFY(!model.busy());QCOMPARE(model.locationTitle(),QString("Documents"));
        model.goUp();QTRY_VERIFY(!model.busy());QVERIFY(!model.canGoBack());
        model.goUp();QCOMPARE(model.locationTitle(),QString("Fichiers"));
        QCOMPARE(contents(f.files),before);
    }

    void filesystemSymlinksStayInsideRootAndAreRevalidated() {
        Fixture f;const auto outside=f.directory.filePath("outside"),inside=f.files+"/Inside",mutableFolder=f.files+"/Mutable";
        QVERIFY(QDir().mkpath(outside));QVERIFY(QDir().mkpath(inside));QVERIFY(QDir().mkpath(mutableFolder));
        QVERIFY(bytes(outside+"/secret.pdf","%PDF-1.4\n"));QVERIFY(bytes(inside+"/safe.pdf","%PDF-1.4\n"));
        if(!QFile::link(outside,f.files+"/Escape"))QSKIP("Symbolic links unavailable.");
        QVERIFY(QFile::link(outside+"/secret.pdf",f.files+"/secret.pdf"));QVERIFY(QFile::link(inside,f.files+"/Alias"));
        PdfLibrary model(f.library,f.files);model.showFiles();QTRY_VERIFY(!model.busy());
        QVERIFY(!titles(model).contains("Escape"));QVERIFY(!titles(model).contains("secret.pdf"));
        QVERIFY(titles(model).contains("Alias"));
        model.openFolder(outside);QVERIFY(!model.busy());QVERIFY(!model.canGoBack());
        const auto canonicalMutable=QFileInfo(mutableFolder).canonicalFilePath();QVERIFY(rowFor(model,canonicalMutable)>=0);
        QVERIFY(QDir().rmdir(mutableFolder));QVERIFY(QFile::link(outside,mutableFolder));
        model.openFolder(canonicalMutable);QVERIFY(!model.canGoBack());QVERIFY(!model.error().isEmpty());
        model.openFolder(QFileInfo(inside).canonicalFilePath());QTRY_VERIFY(!model.busy());
        QCOMPARE(titles(model),QStringList({"safe.pdf"}));
        QCOMPARE(model.data(model.index(0),PdfLibrary::FilePathRole).toString(),QFileInfo(inside+"/safe.pdf").canonicalFilePath());
    }

    void staleAsyncResultsAndFiltersCannotReplaceTheCurrentMode() {
        Fixture f;
        for(int i=1;i<=120;++i) {QVERIFY(f.metadata(id(i),QString("Native %1").arg(i)));QVERIFY(f.pdf(id(i),false));}
        QVERIFY(bytes(f.files+"/Visible.PDF","%PDF-1.4\n"));QVERIFY(bytes(f.files+"/Other.pdf","%PDF-1.4\n"));
        PdfLibrary model(f.library,f.files);QVERIFY(model.busy());
        model.showFiles();model.setFilter("visible");QTRY_VERIFY(!model.busy());
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));QCoreApplication::processEvents();
        QCOMPARE(model.mode(),QString("files"));QCOMPARE(model.filter(),QString("visible"));
        QCOMPARE(titles(model),QStringList({"Visible.PDF"}));
        model.showLibrary();model.showFiles();model.showLibrary();model.setFilter("Native 120");
        QTRY_VERIFY(!model.busy());QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));QCoreApplication::processEvents();
        QCOMPARE(model.mode(),QString("library"));QCOMPARE(titles(model),QStringList({"Native 120"}));
        auto *transient=new PdfLibrary(f.library,f.files);transient->showFiles();delete transient;
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
    }

    void unavailableRootsAndRemovedNativeFoldersRecoverReadOnly() {
        Fixture f;const auto missing=f.directory.filePath("missing");
        PdfLibrary unavailable(missing,missing);QTRY_VERIFY(!unavailable.busy());
        QVERIFY(!unavailable.error().isEmpty());QCOMPARE(unavailable.count(),0);
        unavailable.showFiles();QTRY_VERIFY(!unavailable.busy());QVERIFY(!unavailable.error().isEmpty());
        QVERIFY(QDir().mkpath(missing));QVERIFY(bytes(missing+"/Restored.pdf","%PDF-1.4\n"));
        unavailable.refresh();QTRY_VERIFY(!unavailable.busy());QVERIFY(unavailable.error().isEmpty());QCOMPARE(unavailable.count(),1);
        QVERIFY(f.metadata(id(1),"Dossier",{},true));QVERIFY(f.metadata(id(2),"Cours",id(1)));QVERIFY(f.pdf(id(2)));
        PdfLibrary model(f.library,f.files);QTRY_VERIFY(!model.busy());model.openFolder(id(1));QTRY_VERIFY(!model.busy());
        QVERIFY(model.canGoBack());QVERIFY(f.metadata(id(1),"Dossier",{},true,true));
        model.refresh();QTRY_VERIFY(!model.busy());QVERIFY(!model.canGoBack());QCOMPARE(model.count(),0);
        QCOMPARE(model.locationTitle(),QString("Bibliothèque"));QVERIFY(!model.error().isEmpty());
    }
};

QTEST_GUILESS_MAIN(PdfLibraryTest)
#include "PdfLibraryTest.moc"

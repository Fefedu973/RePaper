#include "EditorAdapter.h"
#include "InkCanvas.h"
#include <QFile>
#include <QStandardPaths>
#include <QtTest>
class PcAdapterTest:public QObject {
    Q_OBJECT
    static QString path(){return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json";}
private slots:
    void initTestCase(){QStandardPaths::setTestModeEnabled(true);QCoreApplication::setOrganizationName("RePaperTests");QCoreApplication::setApplicationName("editor-adapter");}
    void init(){QFile::remove(path());}
    void cleanup(){QFile::remove(path());}
    void repeatedUiReadsShareOneSnapshotUntilThePageChanges() {
        EditorAdapter adapter;InkCanvas page;
        QVERIFY(adapter.attachPcPage(&page));QVERIFY(adapter.chooseTool("line"));
        QVERIFY(adapter.setStrokeWidth(2));
        const auto before=adapter.m_stateBuildCount;
        for(int binding=0;binding<200;++binding)
            QCOMPARE(adapter.state().value("lineWidth").toDouble(),2.0);
        QCOMPARE(adapter.m_stateBuildCount,before+1);
        const auto old=adapter.state();
        QVERIFY(adapter.setStrokeWidth(4));
        QCOMPARE(adapter.state().value("lineWidth").toDouble(),4.0);
        QCOMPARE(old.value("lineWidth").toDouble(),2.0);
        QCOMPARE(adapter.m_stateBuildCount,before+2);
        QVERIFY(adapter.beginStencil("resistor-iec"));
        QCOMPARE(adapter.state().value("activeStencilId").toString(),QString("resistor-iec"));
    }
    void cachedSnapshotUpdatesWhenPageIsReplacedOrDestroyed() {
        EditorAdapter adapter;auto first=new InkCanvas;
        QVERIFY(adapter.attachPcPage(first));QVERIFY(adapter.chooseTool("arrow"));
        QCOMPARE(adapter.state().value("tool").toString(),QString("arrow"));
        InkCanvas replacement;replacement.setTool("ellipse");
        QVERIFY(adapter.attachPcPage(&replacement));
        QCOMPARE(adapter.state().value("tool").toString(),QString("ellipse"));
        delete first;
        QCOMPARE(adapter.state().value("tool").toString(),QString("ellipse"));
        auto last=new InkCanvas;QVERIFY(adapter.attachPcPage(last));
        adapter.state();delete last;
        QVERIFY(!adapter.available());QVERIFY(!adapter.state().value("hasSelection").toBool());
    }
    void catalogueIsSharedAndCallerEditsCannotChangeIt() {
        EditorAdapter first,second;
        const auto catalogue=first.stencils();QVERIFY(!catalogue.isEmpty());
        for(int read=0;read<200;++read) {
            const auto again=second.stencils();
            QCOMPARE(again.constData(),catalogue.constData());
        }
        auto modified=first.stencils();auto entry=modified.first().toMap();
        entry.insert("name","caller-owned change");modified[0]=entry;
        QVERIFY(first.stencils().first().toMap().value("name").toString()!=QString("caller-owned change"));
        QCOMPARE(second.stencils(),catalogue);
    }
    void sharedStencilInsertionAndInkPropertiesUseActualPage() {
        EditorAdapter adapter;QObject unrelated;QVERIFY(!adapter.attachPcPage(&unrelated));
        InkCanvas page;page.setWidth(1404);page.setHeight(1872);page.setSnapping(false);
        QVERIFY(adapter.attachPcPage(&page));QVERIFY(adapter.available());QCOMPARE(adapter.backend(),QString("pc-harness"));
        QVERIFY(adapter.insertStencil("resistor-iec"));QCOMPARE(page.itemCount(),1);
        QVERIFY(adapter.state()["hasSelection"].toBool());
        QVERIFY(adapter.duplicate());QCOMPARE(page.itemCount(),2);QVERIFY(adapter.undo());QCOMPARE(page.itemCount(),1);
        QVERIFY(adapter.chooseTool("arrow"));page.begin(300,400);page.end(800,800);
        QVERIFY(adapter.chooseTool("select"));page.begin(550,600);page.end(550,600);
        QVERIFY(adapter.state()["selectionIsArrow"].toBool());
        QVERIFY(adapter.setStrokeStyle("dashed"));QCOMPARE(page.selectedLineStyle(),QString("dashed"));
        QVERIFY(adapter.setStrokeWidth(8));QCOMPARE(page.selectedLineWidth(),qreal(8));
        QVERIFY(adapter.setArrowDirection("both"));QCOMPARE(page.selectedArrowDirection(),QString("both"));
        page.begin(800,800);page.end(900,700);
        QVERIFY(adapter.rotate());QVERIFY(adapter.scale(0.8));
        InkCanvas reloaded;QCOMPARE(reloaded.itemCount(),2);
        QVERIFY(adapter.remove());QCOMPARE(page.itemCount(),1);QVERIFY(adapter.undo());QCOMPARE(page.itemCount(),2);
    }
    void destroyedPageDisablesSidebar() {
        EditorAdapter adapter;auto page=new InkCanvas;QVERIFY(adapter.attachPcPage(page));delete page;
        QVERIFY(!adapter.available());QVERIFY(!adapter.insertStencil("node"));QVERIFY(!adapter.undo());
    }
    void configurableStencilPreviewPlacementAndSelectionUseSameParameters() {
        EditorAdapter adapter; InkCanvas page; page.setWidth(1404); page.setHeight(1872); page.setSnapping(false);
        QVERIFY(adapter.attachPcPage(&page));
        for (const auto &id: QStringList{"table","graph","bode"}) {
            QVERIFY(!adapter.stencilSchema(id).isEmpty());
            const auto preview=adapter.stencilPreview(id,adapter.stencilDefaults(id));
            QVERIFY(preview.value("valid").toBool());
            QCOMPARE(preview.value("strokes").toList().first().toMap().value("color").toString(),QString("#ffffff"));
        }
        const QVariantMap dimensions{{"rows",6},{"columns",9}};
        QVERIFY(adapter.beginConfiguredStencil("table",dimensions));
        page.begin(200,300); page.end(500,500);
        const auto id=page.documentSnapshot().value("items").toList().last().toMap().value("id").toString();
        QVERIFY(page.selectObject(id));
        QVERIFY(adapter.state().value("selectionCanConfigureStencil").toBool());
        QCOMPARE(adapter.state().value("selectedStencilId").toString(),QString("table"));
        QCOMPARE(adapter.state().value("selectedStencilParameters").toMap().value("rows").toInt(),6);
        QVERIFY(adapter.setStrokeColor("#136aca"));
        QVERIFY(adapter.setStencilParameters({{"rows",3}}));
        QCOMPARE(page.selectedStencilParameters().value("columns").toInt(),9);
        QCOMPARE(page.selectedStencilParameters().value("rows").toInt(),3);
        QCOMPARE(page.selectedLineColor(),QString("#136aca"));
        const auto before=page.documentSnapshot();
        QVERIFY(!adapter.setStencilParameters({{"rows",0}}));
        QVERIFY(!adapter.stencilPreview("graph",{{"tau",0}}).value("valid").toBool());
        QCOMPARE(page.documentSnapshot(),before);
        QVERIFY(adapter.undo()); QVERIFY(page.selectObject(id));
        QCOMPARE(page.selectedStencilParameters().value("rows").toInt(),6);
    }
    void colorsSurvivePropertyEditsUndoAndReload(){
        EditorAdapter adapter;InkCanvas page;page.setWidth(1404);page.setHeight(1872);page.setSnapping(false);
        QVERIFY(adapter.attachPcPage(&page));QVERIFY(adapter.chooseTool("arrow"));
        QVERIFY(adapter.setStrokeColor("#136aca"));page.begin(300,400);page.end(800,800);
        QVERIFY(adapter.chooseTool("select"));page.begin(550,600);page.end(550,600);
        QCOMPARE(adapter.state().value("selectedLineColor").toString(),QString("#136aca"));
        const QString objectId=page.selectedObjectId();
        const auto before=page.documentSnapshot();
        QVERIFY(!adapter.setStrokeColor("invalid"));QVERIFY(!adapter.setStrokeColor("#80112233"));
        QCOMPARE(page.documentSnapshot(),before);
        QVERIFY(adapter.setStrokeColor("#d90707"));QCOMPARE(page.selectedLineColor(),QString("#d90707"));
        QVERIFY(adapter.undo());QVERIFY(page.selectObject(objectId));QCOMPARE(page.selectedLineColor(),QString("#136aca"));
        QVERIFY(adapter.redo());QVERIFY(page.selectObject(objectId));QCOMPARE(page.selectedLineColor(),QString("#d90707"));
        QVERIFY(adapter.setStrokeStyle("dashed"));QVERIFY(adapter.setArrowDirection("both"));
        for(const auto &value:page.renderedStrokes())QCOMPARE(value.toMap().value("color").toString(),QString("#d90707"));
        InkCanvas reloaded;QCOMPARE(reloaded.documentSnapshot(),page.documentSnapshot());
    }
};
QTEST_MAIN(PcAdapterTest)
#include "PcAdapterTest.moc"

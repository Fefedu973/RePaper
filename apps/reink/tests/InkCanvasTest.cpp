#include "InkCanvas.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QtTest>

class InkCanvasTest:public QObject {
    Q_OBJECT
    static QString path(){return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json";}
    static QJsonObject document(){QFile file(path());if(!file.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(file.readAll()).object();}
    static QJsonObject item(int index=0){return document().value("items").toArray().at(index).toObject();}
    static QJsonObject geometry(int index=0){return item(index).value("geometry").toObject();}
    static QPointF point(const QJsonValue &value){const auto p=value.toArray();return {p.at(0).toDouble(),p.at(1).toDouble()};}
    static QPointF endpoint(int index,int object=0){return point(geometry(object).value("points").toArray().at(index));}
    static void select(InkCanvas &canvas,qreal x,qreal y){canvas.setTool("select");canvas.begin(x,y);canvas.end(x,y);}
    static QPoint windowPoint(InkCanvas *canvas,QPointF page) {
        const qreal scale=std::min(canvas->width()/1404.,canvas->height()/1872.);
        const QPointF offset((canvas->width()-1404*scale)/2,(canvas->height()-1872*scale)/2);
        return canvas->mapToScene(offset+page*scale).toPoint();
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("RePaperTests");
        QCoreApplication::setApplicationName("reink-test");
        QStandardPaths::setTestModeEnabled(true);
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<InkCanvas>("RePaper.Drawing",1,0,"InkCanvas");
    }
    void init(){QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json");}
    void cleanup(){QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json");}
    void penGestureUndoRedoAndRestart() {
        InkCanvas canvas;canvas.setWidth(702);canvas.setHeight(936);
        canvas.begin(50,50);canvas.move(100,80);canvas.end(160,120);
        QCOMPARE(canvas.itemCount(),1);QVERIFY(canvas.canUndo());
        canvas.undo();QCOMPARE(canvas.itemCount(),0);QVERIFY(canvas.canRedo());
        canvas.redo();QCOMPARE(canvas.itemCount(),1);
        InkCanvas restored;QCOMPARE(restored.itemCount(),1);
    }
    void oneSymbolIsOneUndoableItem() {
        InkCanvas canvas;canvas.setWidth(702);canvas.setHeight(936);canvas.setTool("symbol");canvas.setSymbolId("capacitor");
        canvas.begin(200,200);canvas.end(200,200);QCOMPARE(canvas.itemCount(),1);
        canvas.setTool("select");canvas.begin(200,200);canvas.end(200,200);QVERIFY(canvas.hasSelection());
        canvas.duplicateSelection();QCOMPARE(canvas.itemCount(),2);
        canvas.rotateSelection();canvas.scaleSelection(1.25);canvas.removeSelection();QCOMPARE(canvas.itemCount(),1);
        canvas.undo();QCOMPARE(canvas.itemCount(),2);
    }
    void cancelledGestureDoesNotCreateInk() {
        InkCanvas canvas;canvas.setWidth(702);canvas.setHeight(936);
        canvas.setLineStyle("dashed");canvas.begin(20,20);canvas.move(100,100);canvas.cancel();
        QCOMPARE(canvas.itemCount(),0);QVERIFY(!canvas.canUndo());
    }
    void emulatorSidebarInsertsEditableSymbol() {
        InkCanvas canvas;
        canvas.insertSymbol("opamp",600,900);QCOMPARE(canvas.itemCount(),1);QVERIFY(canvas.hasSelection());
        QCOMPARE(canvas.tool(),QString("select"));
        canvas.scaleSelection(1.25);canvas.rotateSelection();canvas.duplicateSelection();QCOMPARE(canvas.itemCount(),2);
        canvas.undo();QCOMPARE(canvas.itemCount(),1);
        canvas.insertSymbol("not-a-symbol",600,900);QCOMPARE(canvas.itemCount(),1);
    }
    void malformedSavedDrawingIsNotOverwritten() {
        const QString path=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json";
        InkCanvas initial;initial.insertSymbol("node",600,900);
        QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("invalid original");file.close();
        InkCanvas canvas;canvas.insertSymbol("opamp",600,900);QCOMPARE(canvas.itemCount(),1);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("invalid original"));
    }
    void endpointChangesRebuildArrowAndSurviveUndoAndRestart() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);canvas.setTool("arrow");
        canvas.begin(300,400);canvas.end(800,800);select(canvas,550,600);
        QVERIFY(canvas.selectionHasEndpoints());QVERIFY(canvas.selectionIsArrow());
        const auto before=document();
        // Press within the large handle target, keeping the grab offset stable.
        canvas.begin(810,808);canvas.move(960,658);canvas.end(960,658);
        QCOMPARE(endpoint(0),QPointF(300,400));QCOMPARE(endpoint(1),QPointF(950,650));
        const auto head=item().value("strokes").toArray().last().toObject().value("points").toArray();
        QCOMPARE(point(head.at(1)),endpoint(1));
        const auto moved=document();canvas.undo();QCOMPARE(document(),before);canvas.redo();QCOMPARE(document(),moved);
        InkCanvas restored;restored.setWidth(1404);restored.setHeight(1872);select(restored,625,525);
        QVERIFY(restored.selectionHasEndpoints());QCOMPARE(restored.selectedArrowDirection(),QString("end"));
        restored.begin(950,650);restored.move(1000,900);restored.cancel();QCOMPARE(document(),moved);
        restored.setSelectionLineWidth(8);restored.setSelectionLineStyle("dashed");restored.setSelectionArrowDirection("both");
        QCOMPARE(restored.selectedLineWidth(),qreal(8));QCOMPARE(restored.selectedLineStyle(),QString("dashed"));
        const auto styled=item();const auto strokes=styled.value("strokes").toArray();QVERIFY(strokes.size()>3);
        for(auto stroke:strokes)QCOMPARE(stroke.toObject().value("width").toDouble(),8.);
        QCOMPARE(point(strokes.at(strokes.size()-2).toObject().value("points").toArray().at(1)),endpoint(1));
        QCOMPARE(point(strokes.last().toObject().value("points").toArray().at(1)),endpoint(0));
        restored.undo();QCOMPARE(restored.selectedArrowDirection(),QString());restored.redo();QCOMPARE(item(),styled);
        InkCanvas finalLoad;finalLoad.setWidth(1404);finalLoad.setHeight(1872);select(finalLoad,625,525);
        QCOMPARE(finalLoad.selectedArrowDirection(),QString("both"));QCOMPARE(finalLoad.selectedLineStyle(),QString("dashed"));
    }
    void wireEndpointSnapsToOtherAnchorsAndKeepsOrthogonalRoute() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);
        canvas.setTool("line");canvas.begin(600,1000);canvas.end(1000,1200);
        canvas.setTool("wire");canvas.begin(300,400);canvas.end(800,800);select(canvas,550,400);
        QVERIFY(canvas.selectionIsWire());canvas.setSnapping(true);
        canvas.begin(800,800);canvas.end(590,1008);
        QCOMPARE(endpoint(0,1),QPointF(300,400));QCOMPARE(endpoint(1,1),QPointF(600,1000));
        auto route=item(1).value("strokes").toArray().first().toObject().value("points").toArray();
        QCOMPARE(point(route.at(1)),QPointF(600,400));
        canvas.setSelectionHorizontalFirst(false);
        route=item(1).value("strokes").toArray().first().toObject().value("points").toArray();
        QCOMPARE(point(route.at(1)),QPointF(300,1000));
        canvas.begin(300,400);canvas.end(405,505);
        QCOMPARE(endpoint(0,1),QPointF(400,500));QCOMPARE(endpoint(1,1),QPointF(600,1000));
        const auto saved=document();canvas.setSelectionArrowDirection("both");QCOMPARE(document(),saved);
        canvas.rotateSelection();canvas.scaleSelection(1.25);canvas.duplicateSelection();
        InkCanvas restored;QCOMPARE(restored.itemCount(),3);
    }
    void selectionPropertiesDoNotFlattenSymbols() {
        InkCanvas canvas;canvas.insertSymbol("opamp",600,900);
        QVERIFY(!canvas.selectionHasEndpoints());QVERIFY(!canvas.selectionCanChangeStyle());QVERIFY(!canvas.selectionIsArrow());
        const auto symbol=item();canvas.setSelectionLineStyle("dotted");canvas.setSelectionArrowDirection("start");QCOMPARE(item(),symbol);
        canvas.setSelectionLineWidth(5);const auto changed=item();
        for(auto stroke:changed.value("strokes").toArray())QCOMPARE(stroke.toObject().value("width").toDouble(),5.);
        canvas.undo();QCOMPARE(item(),symbol);canvas.redo();QCOMPARE(item(),changed);
        InkCanvas restored;QCOMPARE(restored.itemCount(),1);
    }
    void axisAlignedLineCanRotateAndScaleWithoutLosingEndpointGeometry() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);canvas.setTool("line");
        canvas.begin(300,400);canvas.end(800,400);select(canvas,550,400);
        canvas.scaleSelection(0.8);QCOMPARE(endpoint(0),QPointF(350,400));QCOMPARE(endpoint(1),QPointF(750,400));
        canvas.rotateSelection();QVERIFY(QLineF(endpoint(0),QPointF(550,200)).length()<1e-5);QVERIFY(QLineF(endpoint(1),QPointF(550,600)).length()<1e-5);
        canvas.setSelectionLineStyle("dotted");canvas.scaleSelection(1.25);
        InkCanvas restored;restored.setWidth(1404);restored.setHeight(1872);select(restored,550,400);
        QVERIFY(restored.selectionHasEndpoints());QCOMPARE(restored.selectedLineStyle(),QString("dotted"));
    }
    void endpointOrArrowDirectionCannotPutHeadOutsidePage() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);canvas.setTool("arrow");
        canvas.begin(100,600);canvas.end(100,200);select(canvas,100,400);const auto before=document();
        canvas.begin(100,200);canvas.end(0,200);QCOMPARE(document(),before);QVERIFY(canvas.status().contains("page"));
        canvas.clear();canvas.setTool("arrow");canvas.begin(5,0);canvas.end(300,50);select(canvas,150,25);
        const auto forward=document();canvas.setSelectionArrowDirection("both");QCOMPARE(document(),forward);QCOMPARE(canvas.selectedArrowDirection(),QString("end"));
    }
    void legacyArrowsAreRecoveredWithoutReinterpretingSymbols() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);canvas.setTool("arrow");
        canvas.setLineStyle("dashed");canvas.begin(300,400);canvas.end(800,800);select(canvas,550,600);canvas.rotateSelection();canvas.scaleSelection(1.25);
        canvas.insertSymbol("opamp",900,1200);
        auto old=document();auto objects=old.value("items").toArray();
        for(int i=0;i<objects.size();++i){auto object=objects[i].toObject();object.remove("geometry");objects[i]=object;}
        old.insert("schemaVersion",1);old.insert("items",objects);
        QFile file(path());QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(old).toJson());file.close();
        InkCanvas restored;restored.setWidth(1404);restored.setHeight(1872);QCOMPARE(restored.itemCount(),2);
        auto ends=objects.first().toObject().value("anchors").toArray();auto midpoint=(point(ends.at(0))+point(ends.at(1)))/2;
        select(restored,midpoint.x(),midpoint.y());QVERIFY(restored.selectionIsArrow());QCOMPARE(restored.selectedLineStyle(),QString("dashed"));
        restored.setSelectionArrowDirection("start");QCOMPARE(document().value("schemaVersion").toInt(),3);
        select(restored,900,1200);QVERIFY(!restored.selectionHasEndpoints());QVERIFY(!restored.selectionCanChangeStyle());
        InkCanvas finalLoad;QCOMPARE(finalLoad.itemCount(),2);
    }
    void malformedGeometryCannotTriggerUnboundedDashExpansion() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setTool("line");canvas.begin(300,400);canvas.end(800,800);
        auto data=document();auto objects=data.value("items").toArray();auto object=objects.first().toObject();auto shape=object.value("geometry").toObject();
        shape.insert("style","dotted");shape.insert("patternScale",0.000000001);object.insert("geometry",shape);objects[0]=object;data.insert("items",objects);
        const auto original=QJsonDocument(data).toJson();QFile file(path());QVERIFY(file.open(QIODevice::WriteOnly));file.write(original);file.close();
        InkCanvas restored;QCOMPARE(restored.itemCount(),0);restored.insertSymbol("node");
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),original);
    }
    void sourcePointBudgetNeverSavesAnUnreadableDocument() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setLineStyle("dashed");
        for(int stroke=0;stroke<13;++stroke) {
            canvas.begin(300,400+stroke*10);
            for(int p=1;p<20000;++p)canvas.move(p%2?302:300,400+stroke*10);
            canvas.end(302,400+stroke*10);
            QCOMPARE(canvas.itemCount(),std::min(stroke+1,12));
        }
        QVERIFY(canvas.status().contains("limite"));
        const auto saved=document();qint64 source=0,rendered=0;
        for(auto value:saved.value("items").toArray()) {
            const auto object=value.toObject();source+=object.value("geometry").toObject().value("points").toArray().size()+object.value("anchors").toArray().size();
            for(auto stroke:object.value("strokes").toArray())rendered+=stroke.toObject().value("points").toArray().size();
        }
        QCOMPARE(source,qint64(240000));QVERIFY(rendered<250000);
        InkCanvas restored;QCOMPARE(restored.itemCount(),12);
        canvas.insertSymbol("node",1000,1400);QCOMPARE(canvas.itemCount(),13);
        InkCanvas withSymbol;QCOMPARE(withSymbol.itemCount(),13);
    }
    void realMouseEndpointDragAndSelectionPanelFitEmulator() {
        QQmlApplicationEngine engine;QSignalSpy warnings(&engine,&QQmlApplicationEngine::warnings);engine.load(QUrl("qrc:/reink/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());QVERIFY(window);
        window->resize(936,1248);QVERIFY(QTest::qWaitForWindowExposed(window));
        auto canvas=window->findChild<InkCanvas *>("drawingCanvas");QVERIFY(canvas);canvas->setSnapping(false);canvas->setTool("arrow");QTest::qWait(30);
        const auto from=windowPoint(canvas,{300,400}),to=windowPoint(canvas,{850,850});
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,from);QTest::mouseMove(window,to);QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,to);
        QCOMPARE(canvas->itemCount(),1);canvas->setTool("select");QTest::qWait(30);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,windowPoint(canvas,{575,625}));QVERIFY(canvas->selectionHasEndpoints());
        const auto fixed=endpoint(0),oldEnd=endpoint(1);const auto before=document();
        const auto handle=windowPoint(canvas,oldEnd),destination=windowPoint(canvas,{1000,600});
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,handle);QTest::mouseMove(window,destination);QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,destination);
        QCOMPARE(endpoint(0),fixed);QVERIFY(QLineF(endpoint(1),QPointF(1000,600)).length()<5);QVERIFY(endpoint(1)!=oldEnd);
        const auto moved=document();canvas->undo();QCOMPARE(document(),before);canvas->redo();QCOMPARE(document(),moved);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,windowPoint(canvas,(endpoint(0)+endpoint(1))/2));
        QVERIFY(QMetaObject::invokeMethod(window,"showPalette",Q_ARG(QVariant,QVariant("properties"))));QTest::qWait(40);
        for(const auto &name:{"selectionWidth","selectionStyle","selectionDirection"}) {
            auto control=window->findChild<QQuickItem *>(name);QVERIFY(control);QVERIFY(control->isEnabled());
            const auto rect=control->mapRectToScene(control->boundingRect());QVERIFY2(QRectF(0,0,936,1248).contains(rect),qPrintable(QString::fromLatin1(name)));
        }
        auto style=window->findChild<QQuickItem *>("selectionStyle");style->forceActiveFocus();QTest::keyClick(window,Qt::Key_Down);
        QTRY_COMPARE(canvas->selectedLineStyle(),QString("dashed"));
        auto direction=window->findChild<QQuickItem *>("selectionDirection");direction->forceActiveFocus();QTest::keyClick(window,Qt::Key_Down);
        QTRY_COMPARE(canvas->selectedArrowDirection(),QString("start"));
        QTest::qWait(80);
        QCOMPARE(canvas->selectedArrowDirection(),QString("start"));
        QCOMPARE(direction->property("currentIndex").toInt(),1);
        QCOMPARE(style->property("currentIndex").toInt(),1);
        const auto evidence=qEnvironmentVariable("PAPER_UI_EVIDENCE");if(!evidence.isEmpty())QVERIFY(window->grabWindow().save(evidence));
        QCOMPARE(canvas->selectedArrowDirection(),QString("start"));
        QCOMPARE(direction->property("currentText").toString(),QString("Début ←"));
        QCOMPARE(warnings.size(),0);
        canvas->insertSymbol("capacitor",600,1100);QVERIFY(!style->isEnabled());QVERIFY(!direction->isEnabled());
        InkCanvas restored;QCOMPARE(restored.itemCount(),2);
    }
};
QTEST_MAIN(InkCanvasTest)
#include "InkCanvasTest.moc"

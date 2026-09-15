#include "InkCanvas.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>
#include <QtTest>

class DrawingGraphTest:public QObject {
    Q_OBJECT
    static QString path(){return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json";}
    static QJsonObject document(){QFile f(path());if(!f.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(f.readAll()).object();}
    static QJsonObject item(int index=0){return document().value("items").toArray().at(index).toObject();}
    static QString id(int index=0){return item(index).value("id").toString();}
    static QJsonObject geometry(int index=0){return item(index).value("geometry").toObject();}
    static QPointF point(QJsonValue value){auto p=value.toArray();return {p[0].toDouble(),p[1].toDouble()};}
    static QPointF endpoint(int end,int index=2){return point(geometry(index).value("points").toArray()[end]);}
    static QJsonObject connection(int index,const char *which){return item(index).value("connections").toObject().value(which).toObject();}
    static void setup(InkCanvas &canvas){canvas.setWidth(1404);canvas.setHeight(1872);canvas.setGridVisible(false);}
    static void circuit(InkCanvas &canvas){
        setup(canvas);canvas.insertSymbol("resistor-iec",400,500);canvas.insertSymbol("capacitor",900,900);
        canvas.setTool("wire");canvas.begin(520,500);canvas.end(780,900);
    }
    static QByteArray bytes(){QFile f(path());if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
    static bool write(QJsonObject doc){QFile f(path());return f.open(QIODevice::WriteOnly)&&f.write(QJsonDocument(doc).toJson())>0;}
    static bool addVoltageArrow(QJsonObject &doc,bool reversed,bool otherSide) {
        auto items=doc["items"].toArray();if(items.size()!=1)return false;
        auto object=items[0].toObject();auto geometry=object["geometry"].toObject();
        repaper::drawing::Item model;model.kind="symbol";model.symbolId=geometry["symbolId"].toString();
        model.width=geometry["width"].toDouble();
        for(auto entry:geometry["points"].toArray())model.sourcePoints.append(point(entry));
        model.voltageArrow=true;model.voltageArrowReversed=reversed;model.voltageArrowOtherSide=otherSide;
        if(!repaper::drawing::rebuild(model))return false;
        geometry["voltageArrow"]=true;geometry["voltageArrowReversed"]=reversed;geometry["voltageArrowOtherSide"]=otherSide;
        QJsonArray strokes;
        for(const auto &stroke:model.strokes) {
            QJsonArray points;for(auto p:stroke.points)points.append(QJsonArray{p.x(),p.y()});
            strokes.append(QJsonObject{{"points",points},{"width",stroke.width},{"color",stroke.color.name()}});
        }
        object["geometry"]=geometry;object["strokes"]=strokes;items[0]=object;doc["items"]=items;return true;
    }
    static PaperDrawing::Polyline route(int index) {
        PaperDrawing::Polyline result;for(auto value:geometry(index).value("wireRoute").toArray())result.append(point(value));return result;
    }
    static bool orthogonal(const PaperDrawing::Polyline &points) {
        if(points.size()<2)return false;
        for(int i=1;i<points.size();++i) {
            const auto d=points[i]-points[i-1];
            if(!std::isfinite(d.x())||!std::isfinite(d.y())||(std::abs(d.x())>1e-7&&std::abs(d.y())>1e-7))return false;
        }
        return true;
    }
    static bool enters(const PaperDrawing::Polyline &points,QRectF rectangle) {
        for(int i=1;i<points.size();++i) {
            const auto a=points[i-1],b=points[i];
            if(std::abs(a.y()-b.y())<1e-7&&a.y()>rectangle.top()&&a.y()<rectangle.bottom()
                    &&std::max(a.x(),b.x())>rectangle.left()&&std::min(a.x(),b.x())<rectangle.right())return true;
            if(std::abs(a.x()-b.x())<1e-7&&a.x()>rectangle.left()&&a.x()<rectangle.right()
                    &&std::max(a.y(),b.y())>rectangle.top()&&std::min(a.y(),b.y())<rectangle.bottom())return true;
        }
        return false;
    }
    static bool collinearOverlap(const PaperDrawing::Polyline &a,const PaperDrawing::Polyline &b) {
        for(int i=1;i<a.size();++i)for(int j=1;j<b.size();++j) {
            const QLineF first(a[i-1],a[i]),second(b[j-1],b[j]);
            const auto axis=first.p2()-first.p1();const qreal length=first.length();if(length<1e-7)continue;
            const auto other=second.p2()-second.p1();
            if(std::abs(axis.x()*other.y()-axis.y()*other.x())>1e-6)continue;
            const auto offset=second.p1()-first.p1();
            if(std::abs(axis.x()*offset.y()-axis.y()*offset.x())>1e-6)continue;
            const auto unit=axis/length;
            const qreal p=QPointF::dotProduct(second.p1()-first.p1(),unit),q=QPointF::dotProduct(second.p2()-first.p1(),unit);
            if(std::min(length,std::max(p,q))-std::max(qreal(0),std::min(p,q))>1e-6)return true;
        }
        return false;
    }
    static repaper::drawing::Item placementStencil(QString id,int quarterTurns=0) {
        using namespace repaper::drawing;
        Item item;item.kind="symbol";item.symbolId=id;item.sourcePoints={{100,100},{340,100},{100,260}};
        if(!rebuild(item))return {};
        QTransform rotation;rotation.translate(220,180);rotation.rotate(90*quarterTurns);rotation.translate(-220,-180);
        if(!transform(item,rotation))return {};return item;
    }
private slots:
    void initTestCase(){
        QCoreApplication::setOrganizationName("RePaperTests");QCoreApplication::setApplicationName("reink-graph-test");QStandardPaths::setTestModeEnabled(true);
    }
    void init(){QFile::remove(path());}
    void cleanup(){QFile::remove(path());}
    void stencilInsertionUsesItsLogicalAnchorInEveryOrientation_data() {
        QTest::addColumn<QString>("symbol");QTest::addColumn<QPointF>("localAnchor");QTest::addColumn<int>("turns");
        const QVector<QPair<QString,QPointF>> anchors={
            {"resistor-iec",{.5,.5}},{"voltage-source",{.5,.5}},{"voltage-generator-simple",{.5,.5}},
            {"node",{.5,.5}},{"opamp",{1,.5}},{"npn",{0,.5}},{"pnp",{0,.5}},{"mosfet",{0,.5}},
            {"ground",{.5,0}},{"connector",{0,.5}},{"lightbulb",{.5,.95}},{"square-root",{0,.55}},{"table",{0,0}}};
        for(const auto &entry:anchors)for(int turns=0;turns<4;++turns)
            QTest::newRow(qPrintable(entry.first+QString::number(turns)))<<entry.first<<entry.second<<turns;
    }
    void stencilInsertionUsesItsLogicalAnchorInEveryOrientation() {
        using namespace repaper::drawing;QFETCH(QString,symbol);QFETCH(QPointF,localAnchor);QFETCH(int,turns);
        auto item=placementStencil(symbol,turns);QCOMPARE(item.kind,QString("symbol"));
        QCOMPARE(defaultStencilAnchor(item),mapBox(item,localAnchor.x(),localAnchor.y()));
        const QPointF pointer(650,800);const auto result=placeStencilNearPointer(item,pointer,{},8);
        QVERIFY(!result.snapped);QVERIFY(result.localPortId.isEmpty());QVERIFY(result.targetAttachment.empty());
        QTransform move;move.translate(result.delta.x(),result.delta.y());QVERIFY(transform(item,move));
        QCOMPARE(defaultStencilAnchor(item),pointer);
    }
    void stencilSnapsAtThePointerAndExtendsPastTheWireEndpoint() {
        using namespace repaper::drawing;
        Item wire;wire.kind="wire";wire.sourcePoints={{100,500},{300,500}};QVERIFY(rebuild(wire));
        auto item=placementStencil("resistor-iec");const auto result=placeStencilNearPointer(item,{304,503},{wire},8);
        QVERIFY(result.snapped);QCOMPARE(result.localPortId,QString("left"));QCOMPARE(result.targetAttachment.objectId,wire.id);
        QCOMPARE(result.targetAttachment.portId,QString("end"));QCOMPARE(result.targetPoint,QPointF(300,500));
        QTransform move;move.translate(result.delta.x(),result.delta.y());QVERIFY(transform(item,move));
        QCOMPARE(item.anchors[0],wire.anchors[1]);QCOMPARE(defaultStencilAnchor(item),QPointF(420,500));
        // A port near the *proposed symbol* but far from the pointer is irrelevant.
        const auto distant=placeStencilNearPointer(placementStencil("resistor-iec"),{420,500},{wire},8);
        QVERIFY(!distant.snapped);QCOMPARE(distant.delta+defaultStencilAnchor(placementStencil("resistor-iec")),QPointF(420,500));
        QVERIFY(placeStencilNearPointer(item,{308,500},{wire},8).snapped);
        QVERIFY(!placeStencilNearPointer(item,{308.001,500},{wire},8).snapped);
        QVERIFY(!placeStencilNearPointer(item,{305,500},{wire},4).snapped); // 8 physical pixels at 2x zoom.
    }
    void stencilSnapHonorsBothWireDirectionsAndComponentPortTangents() {
        using namespace repaper::drawing;
        Item wire;wire.kind="wire";wire.sourcePoints={{300,500},{100,500}};QVERIFY(rebuild(wire));
        const auto item=placementStencil("resistor-iec");auto result=placeStencilNearPointer(item,{300,500},{wire},8);
        QVERIFY(result.snapped);QCOMPARE(result.localPortId,QString("left"));QCOMPARE(result.targetAttachment.portId,QString("start"));
        result=placeStencilNearPointer(item,{100,500},{wire},8);QVERIFY(result.snapped);QCOMPARE(result.localPortId,QString("right"));
        auto target=placementStencil("resistor-iec");QTransform shift;shift.translate(400,400);QVERIFY(transform(target,shift));
        result=placeStencilNearPointer(item,target.anchors[1]+QPointF(2,0),{target},8);
        QVERIFY(result.snapped);QCOMPARE(result.localPortId,QString("left"));QCOMPARE(result.targetAttachment.portId,QString("right"));
        wire.sourcePoints={{300,300},{300,500}};wire.wireRoute.clear();QVERIFY(rebuild(wire));
        QVERIFY(!placeStencilNearPointer(item,{300,500},{wire},8).snapped);
        result=placeStencilNearPointer(placementStencil("resistor-iec",1),{300,500},{wire},8);
        QVERIFY(result.snapped);QCOMPARE(result.localPortId,QString("left"));
    }
    void stencilWireInteriorSnapUsesThePointerSideWithoutInventingALink() {
        using namespace repaper::drawing;
        Item wire;wire.kind="wire";wire.sourcePoints={{100,500},{500,500}};QVERIFY(rebuild(wire));
        auto item=placementStencil("resistor-iec",1);auto result=placeStencilNearPointer(item,{300,497},{wire},8);
        QVERIFY(result.snapped);QCOMPARE(result.targetAttachment.portId,QString("route"));QCOMPARE(result.targetAttachment.wirePosition,.5);
        QCOMPARE(result.localPortId,QString("right"));QCOMPARE(result.targetPoint,QPointF(300,500));
        QTransform move;move.translate(result.delta.x(),result.delta.y());QVERIFY(transform(item,move));
        QVERIFY(defaultStencilAnchor(item).y()<500);Document doc={wire,item};QCOMPARE(attachCoincidentWireEndpoints(doc,item,&result),0);
        QVERIFY(doc[0].startAttachment.empty());QVERIFY(doc[0].endAttachment.empty());
        result=placeStencilNearPointer(placementStencil("resistor-iec",1),{300,503},{wire},8);
        QCOMPARE(result.localPortId,QString("left"));
    }
    void mathematicalStencilsAndInvalidToleranceNeverAcquireElectricalSnaps() {
        using namespace repaper::drawing;
        Item wire;wire.kind="wire";wire.sourcePoints={{100,500},{300,500}};QVERIFY(rebuild(wire));
        for(const auto &id:{"square-root","table","graph","bode"})
            QVERIFY(!placeStencilNearPointer(placementStencil(id),{300,500},{wire},8).snapped);
        auto item=placementStencil("resistor-iec");auto table=placementStencil("table");
        QVERIFY(!placeStencilNearPointer(item,table.anchors[0],{table},8).snapped);
        for(qreal tolerance:{qreal(0),qreal(-1),qreal(10001),std::numeric_limits<qreal>::quiet_NaN()})
            QVERIFY(!placeStencilNearPointer(item,{300,500},{wire},tolerance).snapped);
    }
    void chosenCoincidentEndpointBecomesARealConnectionWithoutChangingInk() {
        using namespace repaper::drawing;
        Item wire;wire.kind="wire";wire.sourcePoints={{100,500},{300,500}};QVERIFY(rebuild(wire));
        auto item=placementStencil("resistor-iec");const auto placement=placeStencilNearPointer(item,{300,500},{wire},8);
        QTransform position;position.translate(placement.delta.x(),placement.delta.y());QVERIFY(transform(item,position));
        Document doc={wire,item};QCOMPARE(attachCoincidentWireEndpoints(doc,item,&placement),1);
        QCOMPARE(doc[0].endAttachment.objectId,item.id);QCOMPARE(doc[0].endAttachment.portId,QString("left"));
        QCOMPARE(doc[0].sourcePoints,wire.sourcePoints);QCOMPARE(doc[0].strokes[0].points,wire.strokes[0].points);
        QTransform move;move.translate(40,20);QVERIFY(transform(doc[1],move));QVERIFY(reroute(doc));
        QCOMPARE(doc[0].sourcePoints.last(),doc[1].anchors[0]);QCOMPARE(doc[0].sourcePoints.first(),wire.sourcePoints.first());
        QCOMPARE(attachCoincidentWireEndpoints(doc,doc[1],&placement),0); // Existing references are never overwritten.
    }
    void onlyTheChosenExactFreeEndpointAcquiresTheNewStencil() {
        using namespace repaper::drawing;
        Item first;first.kind="wire";first.sourcePoints={{100,500},{300,500}};QVERIFY(rebuild(first));
        Item second=first;second.id="other-free-wire";
        auto item=placementStencil("resistor-iec");const auto placement=placeStencilNearPointer(item,{300,500},{first},8);
        QTransform position;position.translate(placement.delta.x(),placement.delta.y());QVERIFY(transform(item,position));
        Document doc={first,second,item};QCOMPARE(attachCoincidentWireEndpoints(doc,item),0);
        QCOMPARE(attachCoincidentWireEndpoints(doc,item,&placement),1);QVERIFY(doc[1].endAttachment.empty());
        doc={first,item};doc[0].sourcePoints[1]+=QPointF(.001,0);QCOMPARE(attachCoincidentWireEndpoints(doc,item,&placement),0);
        doc[0].sourcePoints[1]={std::numeric_limits<qreal>::quiet_NaN(),0};QCOMPARE(attachCoincidentWireEndpoints(doc,item,&placement),0);
        auto symbolic=placement;symbolic.targetAttachment={item.id,"left"};doc={first,item};
        QCOMPARE(attachCoincidentWireEndpoints(doc,item,&symbolic),0);
    }
    void verticalCircuitAvoidsUnconnectedWireOverlapsAfterMovement() {
        using namespace repaper::drawing;
        Item resistor;resistor.kind="symbol";resistor.symbolId="resistor-iec";resistor.voltageArrow=true;
        resistor.sourcePoints={{480,380},{480,620},{320,380}};QVERIFY(rebuild(resistor));
        Item capacitor;capacitor.kind="symbol";capacitor.symbolId="capacitor";capacitor.voltageArrow=true;
        capacitor.sourcePoints={{980,830},{980,1070},{820,830}};QVERIFY(rebuild(capacitor));
        Item source;source.kind="symbol";source.symbolId="voltage-source";source.voltageArrow=true;
        source.sourcePoints={{280,1070},{520,1070},{280,1230}};QVERIFY(rebuild(source));
        Document doc={resistor,capacitor,source};
        auto connect=[&](const Item &a,int start,const Item &b,int end) {
            Item wire;wire.kind="wire";wire.sourcePoints={a.anchors[start],b.anchors[end]};
            wire.startAttachment={a.id,a.portIds[start]};wire.endAttachment={b.id,b.portIds[end]};
            // Reproduce the old independent routing from the captured circuit,
            // before supplying any occupied wire lanes.
            PaperDrawing::OrthogonalRouteOptions options;
            options.obstacles={QRectF(320,380,160,240),QRectF(820,830,160,240),QRectF(280,1070,240,160)};
            options.startDirection=portDirection(doc,wire.startAttachment);options.endDirection=portDirection(doc,wire.endAttachment);
            wire.wireRoute=PaperDrawing::orthogonalRoute(wire.sourcePoints[0],wire.sourcePoints[1],options);
            return wire;
        };
        doc.append(connect(source,0,resistor,0));doc.append(connect(resistor,1,capacitor,0));doc.append(connect(capacitor,1,source,1));
        QVERIFY(collinearOverlap(doc[3].wireRoute,doc[4].wireRoute));
        QVERIFY(reroute(doc));
        for(int i=3;i<6;++i)for(int j=i+1;j<6;++j)QVERIFY(!collinearOverlap(doc[i].wireRoute,doc[j].wireRoute));
        const auto routed=doc;QVERIFY(reroute(doc));for(int i=3;i<6;++i)QCOMPARE(doc[i].wireRoute,routed[i].wireRoute);
        QTransform move;move.translate(40,60);QVERIFY(transform(doc[0],move));QVERIFY(reroute(doc));
        for(int i=3;i<6;++i)for(int j=i+1;j<6;++j)QVERIFY(!collinearOverlap(doc[i].wireRoute,doc[j].wireRoute));
        QCOMPARE(doc[3].sourcePoints.last(),doc[0].anchors[0]);QCOMPARE(doc[4].sourcePoints.first(),doc[0].anchors[1]);
        QVERIFY(withinBudget(doc));
    }
    void explicitWireBranchMayShareItsStubWithoutCreatingNewAttachments() {
        using namespace repaper::drawing;
        Item main;main.kind="wire";main.sourcePoints={{100,400},{700,400}};QVERIFY(rebuild(main));main.wireRoute=main.sourcePoints;
        Item branch;branch.kind="wire";branch.sourcePoints={{300,400},{700,600}};
        branch.startAttachment={main.id,"route",1./3};
        branch.wireRoute={{300,400},{500,400},{500,600},{700,600}};QVERIFY(rebuild(branch));
        Document doc={main,branch};QVERIFY(reroute(doc));
        QCOMPARE(doc[0].wireRoute,main.wireRoute);QCOMPARE(doc[1].wireRoute,branch.wireRoute);
        QCOMPARE(doc[1].startAttachment.objectId,main.id);QVERIFY(collinearOverlap(doc[0].wireRoute,doc[1].wireRoute));
        doc[1].startAttachment={};QVERIFY(reroute(doc));
        QVERIFY(!collinearOverlap(doc[0].wireRoute,doc[1].wireRoute));
        QVERIFY(doc[0].startAttachment.empty());QVERIFY(doc[0].endAttachment.empty());
        QVERIFY(doc[1].startAttachment.empty());QVERIFY(doc[1].endAttachment.empty());
    }
    void squareRootResizesAsOneMathematicalObjectWithoutWirePorts() {
        InkCanvas canvas;setup(canvas);canvas.insertSymbol("square-root",600,900);
        QCOMPARE(canvas.itemCount(),1);QVERIFY(canvas.selectionCanResize());const auto identity=id();
        QVERIFY(canvas.selectedPorts().isEmpty());QVERIFY(item()["anchors"].toArray().isEmpty());
        QVERIFY(item()["portIds"].toArray().isEmpty());
        const auto original=document();const auto oldOrigin=point(geometry()["points"].toArray()[0]);
        const auto oldPath=item()["strokes"].toArray()[0].toObject()["points"].toArray();
        QVERIFY(canvas.resizeSelection(600,160));QCOMPARE(canvas.itemCount(),1);
        const auto origin=point(geometry()["points"].toArray()[0]);
        const auto path=item()["strokes"].toArray()[0].toObject()["points"].toArray();
        QCOMPARE(path.size(),5);for(int i=0;i<4;++i)QCOMPARE(point(path[i])-origin,point(oldPath[i])-oldOrigin);
        QCOMPARE(point(path.last()).x()-origin.x(),600.);
        const auto wide=document();canvas.undo();QCOMPARE(document(),original);canvas.redo();QCOMPARE(document(),wide);
        QVERIFY(canvas.selectObject(identity));canvas.rotateSelection();const auto rotated=document();
        QVERIFY(rotated!=wide);InkCanvas reopened;setup(reopened);QCOMPARE(reopened.itemCount(),1);
        QVERIFY(reopened.selectObject(identity));QVERIFY(reopened.selectedPorts().isEmpty());
        QCOMPARE(QJsonObject::fromVariantMap(reopened.documentSnapshot()),rotated);
    }
    void voltageArrowStaysGroupedThroughMoveRotateResizeCopyUndoAndReload() {
        InkCanvas initial;setup(initial);initial.insertSymbol("resistor-iec",600,900);
        const auto identity=id();const auto originalPorts=item().value("portIds");
        const int bodyCount=item()["strokes"].toArray().size();
        auto annotated=document();QVERIFY(addVoltageArrow(annotated,true,true));QVERIFY(write(annotated));
        InkCanvas canvas;setup(canvas);canvas.setSnapping(false);
        QCOMPARE(canvas.itemCount(),1);QVERIFY(canvas.selectObject(identity));
        QCOMPARE(item()["portIds"],originalPorts);QCOMPARE(item()["strokes"].toArray().size(),bodyCount+2);
        const auto before=document();canvas.begin(600,900);canvas.end(700,1000);
        const auto moved=document();QVERIFY(moved!=before);QCOMPARE(canvas.itemCount(),1);
        const auto oldStrokes=before["items"].toArray()[0].toObject()["strokes"].toArray();
        const auto movedStrokes=item()["strokes"].toArray();
        for(int s=0;s<oldStrokes.size();++s) {
            const auto a=oldStrokes[s].toObject()["points"].toArray(),b=movedStrokes[s].toObject()["points"].toArray();
            QCOMPARE(a.size(),b.size());for(int p=0;p<a.size();++p)QVERIFY(QLineF(point(b[p]),point(a[p])+QPointF(100,100)).length()<1e-6);
        }
        canvas.undo();QCOMPARE(document(),before);canvas.redo();QCOMPARE(document(),moved);
        QVERIFY(canvas.selectObject(identity));canvas.rotateSelection();const auto rotated=document();QVERIFY(rotated!=moved);
        canvas.undo();QCOMPARE(document(),moved);canvas.redo();QCOMPARE(document(),rotated);
        QVERIFY(canvas.selectObject(identity));QVERIFY(canvas.resizeSelection(300,200));
        const auto resized=document();QCOMPARE(item()["strokes"].toArray().size(),bodyCount+2);
        const auto shaft=item()["strokes"].toArray()[bodyCount].toObject()["points"].toArray();
        QVERIFY(std::abs(QLineF(point(shaft[0]),point(shaft[1])).length()-300)<1e-6);
        canvas.undo();QCOMPARE(document(),rotated);canvas.redo();QCOMPARE(document(),resized);
        QVERIFY(canvas.selectObject(identity));canvas.duplicateSelection();QCOMPARE(canvas.itemCount(),2);
        QVERIFY(id(1)!=identity);QVERIFY(geometry(1)["voltageArrow"].toBool());
        QVERIFY(geometry(1)["voltageArrowReversed"].toBool());QVERIFY(geometry(1)["voltageArrowOtherSide"].toBool());
        canvas.undo();QCOMPARE(document(),resized);canvas.redo();QCOMPARE(canvas.itemCount(),2);
        const auto final=document();InkCanvas reopened;QCOMPARE(reopened.itemCount(),2);
        QCOMPARE(QJsonObject::fromVariantMap(reopened.documentSnapshot()),final);
        QVERIFY(reopened.selectObject(identity));reopened.removeSelection();QCOMPARE(reopened.itemCount(),1);
        reopened.undo();QCOMPARE(document(),final);
    }
    void malformedVoltageOptionsDoNotOverwriteSavedDrawings() {
        InkCanvas initial;initial.insertSymbol("resistor-iec",600,900);const auto valid=document();
        for(const auto *key:{"voltageArrow","voltageArrowReversed","voltageArrowOtherSide"}) {
            auto data=valid;auto objects=data["items"].toArray();auto object=objects[0].toObject();auto shape=object["geometry"].toObject();
            shape[key]="true";object["geometry"]=shape;objects[0]=object;data["items"]=objects;QVERIFY(write(data));
            const auto original=bytes();InkCanvas rejected;QCOMPARE(rejected.itemCount(),0);
            rejected.insertSymbol("node");QCOMPARE(bytes(),original);
        }
        auto old=valid;auto objects=old["items"].toArray();auto object=objects[0].toObject();auto shape=object["geometry"].toObject();
        for(const auto *key:{"voltageArrow","voltageArrowReversed","voltageArrowOtherSide"})shape.remove(key);
        object["geometry"]=shape;objects[0]=object;old["items"]=objects;QVERIFY(write(old));
        InkCanvas reopened;QCOMPARE(reopened.itemCount(),1);QCOMPARE(item()["strokes"],object["strokes"]);
    }
    void shapesKeepParametersHandlesAndIdentity(){
        InkCanvas canvas;setup(canvas);canvas.setSnapping(false);canvas.setTool("rectangle");
        canvas.begin(300,400);canvas.move(500,500);
        QCOMPARE(canvas.itemCount(),0);QVERIFY(canvas.documentSnapshot().value("items").toList().isEmpty());
        QVERIFY(!canvas.renderedStrokes(true).isEmpty());QVERIFY(canvas.renderedStrokes().isEmpty());
        canvas.end(700,700);const auto identity=id();QVERIFY(!QUuid(identity).isNull());
        QVERIFY(canvas.selectObject(identity));QCOMPARE(canvas.selectionKind(),QString("rectangle"));QVERIFY(canvas.selectionCanResize());
        QCOMPARE(canvas.selectedShapeWidth(),qreal(400));QCOMPARE(canvas.selectionHandlePoints().size(),4);
        canvas.begin(700,700);canvas.end(900,900);QCOMPARE(canvas.selectedShapeWidth(),qreal(600));QCOMPARE(canvas.selectedShapeHeight(),qreal(500));
        canvas.setSelectionCornerRadius(45);QCOMPARE(canvas.selectedCornerRadius(),qreal(45));
        canvas.rotateSelection();canvas.resizeSelection(400,200);
        const auto axes=geometry().value("points").toArray();const auto origin=point(axes[0]);
        QVERIFY(std::abs(QPointF::dotProduct(point(axes[1])-origin,point(axes[2])-origin))<1e-6);
        QCOMPARE(canvas.selectedShapeWidth(),qreal(400));QCOMPARE(canvas.selectedShapeHeight(),qreal(200));
        InkCanvas restored;setup(restored);QVERIFY(restored.selectObject(identity));QCOMPARE(restored.selectedCornerRadius(),qreal(45));
        QCOMPARE(restored.selectedObjectId(),identity);QCOMPARE(restored.selectedPorts().size(),4);
        restored.setSnapping(false);restored.setTool("ellipse");restored.begin(100,1100);restored.end(500,1500);
        QVERIFY(restored.selectObject(id(1)));restored.resizeSelection(200,300);
        const auto contour=item(1).value("strokes").toArray().first().toObject().value("points").toArray();
        QVERIFY(contour.size()>=65);QCOMPARE(point(contour.first()),point(contour.last()));QCOMPARE(restored.selectionKind(),QString("ellipse"));
        QVERIFY(!restored.selectionHasEndpoints());restored.setSelectionLineStyle("dashed");QCOMPARE(restored.selectedLineStyle(),QString("dashed"));
    }
    void attachmentsFollowOneUndoAndReload(){
        InkCanvas canvas;circuit(canvas);const auto left=id(0),right=id(1),wire=id(2);
        QCOMPARE(connection(2,"start").value("objectId").toString(),left);QCOMPARE(connection(2,"end").value("objectId").toString(),right);
        const auto before=document();QVERIFY(canvas.selectObject(left));canvas.begin(400,500);canvas.move(430,540);
        QCOMPARE(document(),before);QCOMPARE(QJsonObject::fromVariantMap(canvas.documentSnapshot()),before);
        canvas.end(500,600);QCOMPARE(endpoint(0),QPointF(620,600));QCOMPARE(endpoint(1),QPointF(780,900));
        const auto moved=document();canvas.undo();QCOMPARE(document(),before);canvas.redo();QCOMPARE(document(),moved);
        QVERIFY(canvas.selectObject(left));canvas.rotateSelection();QCOMPARE(endpoint(0),point(item(0).value("anchors").toArray()[1]));
        canvas.resizeSelection(300,160);QCOMPARE(endpoint(0),point(item(0).value("anchors").toArray()[1]));
        QVERIFY(canvas.selectObject(wire));canvas.setSelectionWireBend(60);QCOMPARE(canvas.selectedWireBend(),qreal(60));
        auto route=item(2).value("strokes").toArray().first().toObject().value("points").toArray();
        for(int p=1;p<route.size();++p){auto delta=point(route[p])-point(route[p-1]);QVERIFY(std::abs(delta.x())<1e-7||std::abs(delta.y())<1e-7);}
        const auto saved=document();InkCanvas restored;QCOMPARE(restored.itemCount(),3);QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),saved);
        QVERIFY(restored.selectObject(left));const auto oldEndpoint=endpoint(0);restored.removeSelection();QCOMPARE(restored.itemCount(),2);
        QVERIFY(connection(1,"start").value("objectId").toString().isEmpty());QCOMPARE(connection(1,"end").value("objectId").toString(),right);
        QCOMPARE(endpoint(0,1),oldEndpoint);restored.undo();QCOMPARE(document(),saved);restored.redo();QCOMPARE(restored.itemCount(),2);
    }
    void copiesRemapInternalReferencesAndDetachExternalReferences(){
        InkCanvas canvas;circuit(canvas);QStringList ids{id(0),id(1),id(2)};const auto original=document().value("items").toArray();
        canvas.duplicateObjects(ids);QCOMPARE(canvas.itemCount(),6);
        QSet<QString> seen;for(auto object:document().value("items").toArray()){auto identity=object.toObject().value("id").toString();QVERIFY(!seen.contains(identity));seen.insert(identity);}
        for(int i=0;i<3;++i)QCOMPARE(item(i),original[i].toObject());
        QCOMPARE(connection(5,"start").value("objectId").toString(),id(3));QCOMPARE(connection(5,"end").value("objectId").toString(),id(4));
        QVERIFY(canvas.selectObject(id(3)));canvas.rotateSelection();QCOMPARE(item(0),original[0].toObject());QCOMPARE(item(2),original[2].toObject());
        QVERIFY(canvas.selectObject(ids[2]));canvas.duplicateSelection();QCOMPARE(canvas.itemCount(),7);
        QVERIFY(connection(6,"start").value("objectId").toString().isEmpty());QVERIFY(connection(6,"end").value("objectId").toString().isEmpty());
        InkCanvas restored;QCOMPARE(restored.itemCount(),7);
    }
    void cancelledConnectedDragPreservesGraphAndRedo(){
        InkCanvas canvas;circuit(canvas);const auto before=document();QVERIFY(canvas.selectObject(id(0)));
        canvas.begin(400,500);canvas.move(500,600);canvas.move(550,650);QCOMPARE(document(),before);
        QCOMPARE(QJsonObject::fromVariantMap(canvas.documentSnapshot()),before);QVERIFY(canvas.renderedStrokes(true)!=canvas.renderedStrokes());
        canvas.cancel();QCOMPARE(QJsonObject::fromVariantMap(canvas.documentSnapshot()),before);
        canvas.undo();QCOMPARE(canvas.itemCount(),2);QVERIFY(canvas.canRedo());
        canvas.setTool("rectangle");canvas.begin(100,100);canvas.move(200,200);canvas.cancel();QVERIFY(canvas.canRedo());
        canvas.redo();QCOMPARE(document(),before);
    }
    void migrationAssignsIdentityOnceAndPreservesOpaqueGeometry(){
        InkCanvas canvas;canvas.insertSymbol("opamp",600,900);auto old=document();auto objects=old.value("items").toArray();
        auto object=objects[0].toObject();object.remove("id");object.remove("portIds");object.remove("connections");
        auto shape=object.value("geometry").toObject();shape.remove("symbolId");shape.remove("cornerRadius");shape.remove("wireBend");shape.insert("points",QJsonArray{});
        object.insert("geometry",shape);objects[0]=object;old.insert("schemaVersion",2);old.insert("items",objects);QVERIFY(write(old));
        InkCanvas migrated;QCOMPARE(migrated.itemCount(),1);QCOMPARE(document().value("schemaVersion").toInt(),3);
        QCOMPARE(item().value("strokes"),object.value("strokes"));const auto identity=id();QVERIFY(!QUuid(identity).isNull());
        InkCanvas reopened;QCOMPARE(reopened.itemCount(),1);QCOMPARE(id(),identity);QVERIFY(reopened.selectObject(identity));QVERIFY(!reopened.selectionCanResize());
    }
    void duplicateIdsAndStaleAttachmentsNeverOverwriteOriginal(){
        InkCanvas canvas;circuit(canvas);const auto valid=document();
        for(bool duplicate:{true,false}) {
            auto data=valid;auto objects=data.value("items").toArray();
            if(duplicate){auto object=objects[1].toObject();object.insert("id",objects[0].toObject().value("id"));objects[1]=object;}
            else {auto object=objects[2].toObject();auto refs=object.value("connections").toObject();auto start=refs.value("start").toObject();
                start.insert("portId",objects[0].toObject().value("portIds").toArray()[0]);refs.insert("start",start);object.insert("connections",refs);objects[2]=object;}
            data.insert("items",objects);QVERIFY(write(data));const auto original=bytes();
            InkCanvas rejected;QCOMPARE(rejected.itemCount(),0);rejected.insertSymbol("node");QCOMPARE(bytes(),original);
        }
    }
    void missingTargetsDetachWithoutMovingVisibleInk(){
        InkCanvas canvas;circuit(canvas);auto data=document();auto objects=data.value("items").toArray();objects.removeAt(0);data.insert("items",objects);QVERIFY(write(data));
        const auto visible=objects[1].toObject().value("strokes");InkCanvas restored;QCOMPARE(restored.itemCount(),2);
        const auto snapshot=QJsonObject::fromVariantMap(restored.documentSnapshot()).value("items").toArray();
        QCOMPARE(snapshot[1].toObject().value("strokes"),visible);QVERIFY(snapshot[1].toObject().value("connections").toObject().value("start").toObject().value("objectId").toString().isEmpty());
        QVERIFY(restored.status().contains("détachée"));
    }
    void invalidGestureCannotSaveNan(){
        InkCanvas canvas;setup(canvas);canvas.setTool("arrow");canvas.begin(100,100);canvas.move(200,200);canvas.end(qQNaN(),200);
        QCOMPARE(canvas.itemCount(),0);QVERIFY(!canvas.canUndo());QVERIFY(!QFile::exists(path()));
    }
    void stencilChoiceDefersInsertionAndRejectedParametersAreAtomic(){
        InkCanvas canvas;setup(canvas);QVERIFY(canvas.beginStencil("resistor-iec"));QCOMPARE(canvas.itemCount(),0);
        QCOMPARE(canvas.tool(),QString("symbol"));QVERIFY(!canvas.beginStencil("missing"));QCOMPARE(canvas.symbolId(),QString("resistor-iec"));
        canvas.begin(400,500);canvas.move(500,600);canvas.cancel();QCOMPARE(canvas.itemCount(),0);QVERIFY(!canvas.canUndo());
        canvas.begin(400,500);canvas.end(400,500);QCOMPARE(canvas.itemCount(),1);QVERIFY(canvas.selectObject(id()));
        const auto before=document();QVERIFY(!canvas.resizeSelection(-5,100));QVERIFY(!canvas.resizeSelection(qQNaN(),100));
        QVERIFY(!canvas.setSelectionCornerRadius(10));QVERIFY(!canvas.setSelectionWireBend(10));QCOMPARE(document(),before);
        QVERIFY(canvas.resizeSelection(300,200));canvas.undo();QCOMPARE(document(),before);canvas.undo();QCOMPARE(canvas.itemCount(),0);
    }
    void endpointDragDetachesThenReattachesWithoutLosingUndo(){
        InkCanvas canvas;circuit(canvas);const auto left=id(0),wire=id(2);const auto attached=document();
        QVERIFY(canvas.selectObject(wire));canvas.setSnapping(false);canvas.begin(520,500);canvas.end(600,650);
        QCOMPARE(endpoint(0),QPointF(600,650));QVERIFY(connection(2,"start").value("objectId").toString().isEmpty());
        const auto detached=document();canvas.setSnapping(true);canvas.begin(600,650);canvas.end(510,508);
        QCOMPARE(endpoint(0),QPointF(520,500));QCOMPARE(connection(2,"start").value("objectId").toString(),left);
        QCOMPARE(document(),attached);canvas.undo();QCOMPARE(document(),detached);canvas.redo();QCOMPARE(document(),attached);
        QVERIFY(canvas.selectObject(left));canvas.begin(400,500);canvas.end(500,600);QCOMPARE(endpoint(0),QPointF(620,600));
        const auto moved=document();InkCanvas restored;QCOMPARE(restored.itemCount(),3);
        QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),moved);
    }
    void movingComponentRejectsWholeGraphIfItsWireLeavesPage(){
        InkCanvas canvas;circuit(canvas);const auto left=id(0);QVERIFY(canvas.selectObject(id(2)));
        const auto beforeBend=document();QVERIFY(canvas.setSelectionWireBend(600));const auto valid=document();
        QVERIFY(canvas.selectObject(left));canvas.begin(400,500);canvas.end(900,600);
        QCOMPARE(document(),valid);QCOMPARE(QJsonObject::fromVariantMap(canvas.documentSnapshot()),valid);
        QVERIFY(canvas.status().contains("page"));canvas.undo();QCOMPARE(document(),beforeBend);
    }
    void radiusScalesAfterRotationAndSavedRenderRemainsEquivalent(){
        InkCanvas canvas;setup(canvas);canvas.setSnapping(false);canvas.setTool("rectangle");canvas.begin(400,500);canvas.end(700,700);
        const auto identity=id();QVERIFY(canvas.selectObject(identity));QVERIFY(canvas.setSelectionCornerRadius(40));
        canvas.rotateSelection();canvas.scaleSelection(0.5);QCOMPARE(canvas.selectedCornerRadius(),qreal(20));
        QVERIFY(canvas.resizeSelection(10,10));QCOMPARE(canvas.selectedCornerRadius(),qreal(5));
        const auto saved=document();const auto rendered=canvas.renderedStrokes();const auto originalBytes=bytes();
        QVERIFY(!canvas.setSelectionCornerRadius(qQNaN()));QVERIFY(!canvas.resizeSelection(qInf(),100));
        canvas.begin(550,600);canvas.move(700,800);canvas.cancel();QCOMPARE(bytes(),originalBytes);
        InkCanvas restored;QVERIFY(restored.selectObject(identity));QCOMPARE(restored.selectedCornerRadius(),qreal(5));
        QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),saved);QCOMPARE(restored.renderedStrokes(),rendered);
    }
    void configuredStencilsKeepParametersColorAndHistory_data(){
        QTest::addColumn<QString>("stencil");QTest::addColumn<QVariantMap>("parameters");
        QTest::newRow("table")<<QString("table")<<QVariantMap{{"rows",3},{"columns",5}};
        QTest::newRow("exponential")<<QString("graph")<<QVariantMap{{"curveType","exponential"},{"tau",2.5},{"initialValue",0.2}};
        QTest::newRow("second-order")<<QString("graph")<<QVariantMap{{"curveType","second-order"},{"omega0",3.0},{"damping",0.7}};
        QTest::newRow("bode")<<QString("bode")<<QVariantMap{{"mode","phase"},{"curve",true},{"yMin",-180.0},{"yMax",0.0}};
    }
    void configuredStencilsKeepParametersColorAndHistory(){
        QFETCH(QString,stencil);QFETCH(QVariantMap,parameters);
        QVariantMap normalized;QVERIFY(PaperDrawing::normalizeStencilParameters(stencil,parameters,&normalized));
        InkCanvas canvas;setup(canvas);QVERIFY(canvas.setLineColor("#d02030"));
        QVERIFY(canvas.beginConfiguredStencil(stencil,parameters));canvas.begin(600,800);
        const auto preview=canvas.renderedStrokes(true);QVERIFY(preview.size()>1);
        QCOMPARE(preview.first().toMap().value("color").toString(),QString("#ffffff"));
        canvas.end(600,800);QCOMPARE(canvas.itemCount(),1);const auto identity=id();
        QVERIFY(canvas.selectObject(identity));QCOMPARE(canvas.selectedStencilId(),stencil);
        QCOMPARE(canvas.selectedStencilParameters(),normalized);
        QCOMPARE(geometry().value("stencilParameters").toObject(),QJsonObject::fromVariantMap(normalized));
        QCOMPARE(canvas.selectedLineColor(),QString("#d02030"));QCOMPARE(canvas.selectedLineWidth(),qreal(2.3));
        canvas.setSelectionLineWidth(5);QCOMPARE(canvas.selectedLineWidth(),qreal(5));
        QVERIFY(canvas.setSelectionLineColor("#2030c0"));
        const auto before=document();QVERIFY(canvas.resizeSelection(420,270));canvas.rotateSelection();
        QCOMPARE(canvas.selectedStencilParameters(),normalized);QCOMPARE(canvas.selectedLineColor(),QString("#2030c0"));
        const auto rotated=document();canvas.undo();canvas.undo();QCOMPARE(document(),before);
        canvas.redo();canvas.redo();QCOMPARE(document(),rotated);
        QVERIFY(canvas.selectObject(identity));canvas.duplicateSelection();QCOMPARE(canvas.itemCount(),2);
        QCOMPARE(geometry(1).value("stencilParameters"),geometry(0).value("stencilParameters"));
        const auto saved=document();const auto rendered=canvas.renderedStrokes();
        for(auto object:saved.value("items").toArray()){
            const auto strokes=object.toObject().value("strokes").toArray();
            QCOMPARE(strokes.first().toObject().value("color").toString(),QString("#ffffff"));
            for(int i=1;i<strokes.size();++i)QCOMPARE(strokes[i].toObject().value("color").toString(),QString("#2030c0"));
        }
        InkCanvas restored;setup(restored);QCOMPARE(restored.itemCount(),2);QVERIFY(restored.selectObject(identity));
        QCOMPARE(restored.selectedStencilParameters(),normalized);QCOMPARE(restored.selectedLineColor(),QString("#2030c0"));
        QCOMPARE(restored.renderedStrokes(),rendered);QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),saved);
    }
    void parameterEditsPreserveForegroundAndAreOneUndoStep(){
        InkCanvas canvas;setup(canvas);QVERIFY(canvas.beginConfiguredStencil("table",{{"rows",3},{"columns",5}}));
        QVERIFY(canvas.setLineColor("#c02060"));canvas.insertSymbol("table",600,800);QCOMPARE(canvas.itemCount(),1);
        QCOMPARE(canvas.selectedStencilParameters().value("columns").toInt(),5);const auto identity=id();
        const auto before=document();QVERIFY(canvas.setSelectedStencilParameters({{"rows",7},{"opaqueBackground",false}}));
        QCOMPARE(canvas.selectedStencilParameters().value("rows").toInt(),7);
        QCOMPARE(canvas.selectedStencilParameters().value("columns").toInt(),5);
        QCOMPARE(canvas.selectedLineColor(),QString("#c02060"));
        for(auto stroke:item().value("strokes").toArray())QCOMPARE(stroke.toObject().value("color").toString(),QString("#c02060"));
        const auto transparent=document();canvas.undo();QCOMPARE(document(),before);canvas.redo();QCOMPARE(document(),transparent);
        QVERIFY(canvas.selectObject(identity));QVERIFY(canvas.setSelectedStencilParameters({{"opaqueBackground",true}}));
        QCOMPARE(canvas.selectedLineColor(),QString("#c02060"));
        QCOMPARE(item().value("strokes").toArray().first().toObject().value("color").toString(),QString("#ffffff"));
        const auto final=document();QVERIFY(!canvas.setSelectedStencilParameters({{"rows",31}}));QCOMPARE(document(),final);
        QVERIFY(!canvas.setSelectedStencilParameters({{"unknown",1}}));QCOMPARE(document(),final);
        canvas.undo();QCOMPARE(document(),transparent);canvas.redo();QCOMPARE(document(),final);
        InkCanvas restored;QVERIFY(restored.selectObject(identity));QCOMPARE(restored.selectedStencilParameters().value("rows").toInt(),7);
        QCOMPARE(restored.selectedLineColor(),QString("#c02060"));
    }
    void invalidConfiguredRequestsPreserveAnActiveGesture(){
        InkCanvas canvas;setup(canvas);canvas.setTool("line");canvas.begin(300,400);canvas.move(500,600);
        const auto preview=canvas.renderedStrokes(true);const auto snapshot=canvas.documentSnapshot();
        QSignalSpy settings(&canvas,&InkCanvas::settingsChanged);
        QVERIFY(!canvas.beginConfiguredStencil("",{}));QVERIFY(!canvas.beginConfiguredStencil("unknown",{}));
        QVERIFY(!canvas.beginConfiguredStencil("table",{{"rows",0}}));
        QVERIFY(!canvas.beginConfiguredStencil("table",{{"columns",2.5}}));
        QVERIFY(!canvas.beginConfiguredStencil("table",{{"opaqueBackground",QString("false")}}));
        QVERIFY(!canvas.beginConfiguredStencil("graph",{{"curveType","unknown"}}));
        QVERIFY(!canvas.beginConfiguredStencil("node",{{"rows",2}}));
        QCOMPARE(settings.size(),0);QCOMPARE(canvas.tool(),QString("line"));
        QCOMPARE(canvas.renderedStrokes(true),preview);QCOMPARE(canvas.documentSnapshot(),snapshot);
        canvas.end(700,800);QCOMPARE(canvas.itemCount(),1);QVERIFY(canvas.canUndo());
        const auto saved=document();canvas.setTool("rectangle");canvas.begin(500,500);canvas.end(500,500);
        QCOMPARE(canvas.itemCount(),1);QCOMPARE(document(),saved);
    }
    void alteredParametersAndBackgroundCannotOverwriteOriginal(){
        InkCanvas canvas;QVERIFY(canvas.beginConfiguredStencil("table",{{"rows",3},{"columns",5}}));
        canvas.insertSymbol("table",600,800);const auto valid=document();
        for(const auto &mutation:QStringList{"parameter-mismatch","unknown-parameter","non-object","background-color"}){
            auto changed=valid;auto objects=changed.value("items").toArray();auto object=objects[0].toObject();
            auto shape=object.value("geometry").toObject();auto params=shape.value("stencilParameters").toObject();
            if(mutation=="parameter-mismatch")params.insert("rows",4);
            if(mutation=="unknown-parameter")params.insert("ignored",true);
            shape.insert("stencilParameters",mutation=="non-object"?QJsonValue("invalid"):QJsonValue(params));
            if(mutation=="background-color"){
                auto strokes=object.value("strokes").toArray();auto first=strokes[0].toObject();first.insert("color","#202020");strokes[0]=first;object.insert("strokes",strokes);
            }
            object.insert("geometry",shape);objects[0]=object;changed.insert("items",objects);QVERIFY(write(changed));
            const auto original=bytes();InkCanvas rejected;QCOMPARE(rejected.itemCount(),0);rejected.insertSymbol("node");QCOMPARE(bytes(),original);
        }
    }
    void defaultParametersRemainOptionalAndWhiteBackgroundMasksInk(){
        InkCanvas canvas;setup(canvas);canvas.setSnapping(false);canvas.setTool("line");canvas.begin(550,800);canvas.end(650,800);
        QVERIFY(canvas.beginConfiguredStencil("table",{{"rows",3},{"columns",5}}));canvas.begin(480,720);canvas.end(480,720);
        QImage image(1404,1872,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
        {QPainter painter(&image);canvas.paint(&painter);}QCOMPARE(image.pixelColor(600,800),QColor(Qt::white));
        canvas.clear();QVERIFY(canvas.beginStencil("table"));canvas.insertSymbol("table",600,800);auto legacy=document();
        auto objects=legacy.value("items").toArray();auto object=objects[0].toObject();auto shape=object.value("geometry").toObject();
        shape.remove("stencilParameters");object.insert("geometry",shape);objects[0]=object;legacy.insert("items",objects);QVERIFY(write(legacy));
        InkCanvas restored;QCOMPARE(restored.itemCount(),1);QVERIFY(restored.selectObject(id()));
        QCOMPARE(restored.selectedStencilParameters(),PaperDrawing::stencilDefaults("table"));
        QVERIFY(restored.setSelectedStencilParameters({{"rows",6}}));QCOMPARE(geometry().value("stencilParameters").toObject().value("rows").toInt(),6);
    }
    void automaticRoutesRespectPortsAndPersistEverySegment(){
        InkCanvas canvas;circuit(canvas);QCOMPARE(canvas.itemCount(),3);
        QCOMPARE(geometry(2).value("wireRouteMode").toString(),QString("auto"));
        QVERIFY(!geometry(2).value("wireHasBend").toBool());
        const auto points=route(2);QVERIFY(orthogonal(points));QVERIFY(points.size()>=4);
        QCOMPARE(points.first(),QPointF(520,500));QCOMPARE(points.last(),QPointF(780,900));
        QVERIFY(points[1].x()>points.first().x());QCOMPARE(points[1].y(),points.first().y());
        QVERIFY(points.last().x()>points[points.size()-2].x());QCOMPARE(points.last().y(),points[points.size()-2].y());
        const auto saved=document();InkCanvas restored;
        QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),saved);
        QVERIFY(restored.selectObject(id(2)));QVERIFY(restored.selectedWireAutomatic());
        QCOMPARE(restored.selectedWireRoute().size(),points.size());
    }
    void movingAnObstacleReroutesExistingWireInOneUndoStep(){
        InkCanvas canvas;setup(canvas);canvas.setSnapping(false);canvas.insertSymbol("resistor-iec",700,1100);
        const auto component=id();canvas.setTool("wire");canvas.begin(200,700);canvas.end(1200,700);
        QCOMPARE(canvas.itemCount(),2);QCOMPARE(route(1),PaperDrawing::Polyline({{200,700},{1200,700}}));
        const auto before=document();QVERIFY(canvas.selectObject(component));
        canvas.begin(700,1100);canvas.move(700,700);QCOMPARE(document(),before);
        const auto preview=canvas.renderedStrokes(true);QVERIFY(preview!=canvas.renderedStrokes());
        canvas.end(700,700);const auto routed=route(1);QVERIFY(orthogonal(routed));QVERIFY(routed.size()>=4);
        QVERIFY(!enters(routed,QRectF(580,620,240,160)));
        QCOMPARE(routed.first(),QPointF(200,700));QCOMPARE(routed.last(),QPointF(1200,700));
        const auto after=document();canvas.undo();QCOMPARE(document(),before);canvas.redo();QCOMPARE(document(),after);
        InkCanvas restored;QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),after);
    }
    void manualSegmentDragPreservesEndpointsAndCanReturnToAutomatic(){
        InkCanvas canvas;setup(canvas);canvas.setSnapping(false);canvas.setTool("wire");canvas.begin(200,400);canvas.end(1000,800);
        QVERIFY(canvas.selectObject(id()));
        QVariantList pathPoints;
        for(auto p:PaperDrawing::Polyline{{200,400},{500,400},{500,800},{1000,800}})pathPoints.append(QVariantMap{{"x",p.x()},{"y",p.y()}});
        QVERIFY(canvas.setSelectionWireRoute(pathPoints));QVERIFY(!canvas.selectedWireAutomatic());
        QCOMPARE(canvas.selectionHandlePoints().size(),3);
        const auto before=document();canvas.begin(500,600);canvas.move(600,600);
        QCOMPARE(document(),before);QVERIFY(canvas.renderedStrokes(true)!=canvas.renderedStrokes());
        canvas.end(600,600);
        QCOMPARE(route(0),PaperDrawing::Polyline({{200,400},{600,400},{600,800},{1000,800}}));
        QCOMPARE(geometry().value("wireRouteMode").toString(),QString("manual"));
        const auto after=document();canvas.undo();QCOMPARE(document(),before);canvas.redo();QCOMPARE(document(),after);
        InkCanvas restored;QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),after);
        QVERIFY(restored.selectObject(id()));QVERIFY(restored.setSelectionWireBend(0));QVERIFY(restored.selectedWireAutomatic());
        QCOMPARE(geometry().value("wireRouteMode").toString(),QString("auto"));
        QCOMPARE(endpoint(0,0),QPointF(200,400));QCOMPARE(endpoint(1,0),QPointF(1000,800));
        const auto automatic=document();pathPoints[1]=QVariantMap{{"x",501},{"y",401}};
        QVERIFY(!restored.setSelectionWireRoute(pathPoints));QCOMPARE(document(),automatic);
    }
    void wireJunctionsFollowTheirTargetAndCopiesKeepInternalReferences(){
        InkCanvas canvas;setup(canvas);canvas.setSnapping(true);canvas.setTool("wire");
        canvas.begin(200,500);canvas.end(1000,500);const auto base=id();
        canvas.begin(400,500);canvas.end(400,900);const auto branch=id(1);
        QCOMPARE(connection(1,"start").value("objectId").toString(),base);
        QCOMPARE(connection(1,"start").value("portId").toString(),QString("route"));
        QCOMPARE(connection(1,"start").value("wirePosition").toDouble(),0.25);
        canvas.begin(1000,500);canvas.end(1200,900);const auto endBranch=id(2);
        QCOMPARE(connection(2,"start").value("objectId").toString(),base);
        QCOMPARE(connection(2,"start").value("portId").toString(),QString("end"));
        QVERIFY(canvas.selectObject(base));canvas.rotateSelection();
        QCOMPARE(endpoint(0,1),QPointF(600,300));QCOMPARE(endpoint(0,2),QPointF(600,900));
        QCOMPARE(connection(1,"start").value("wirePosition").toDouble(),0.25);
        const auto beforeCopies=document();canvas.duplicateObjects({base,branch,endBranch});QCOMPARE(canvas.itemCount(),6);
        QCOMPARE(connection(4,"start").value("objectId").toString(),id(3));
        QCOMPARE(connection(5,"start").value("objectId").toString(),id(3));
        QCOMPARE(connection(4,"start").value("wirePosition").toDouble(),0.25);
        const auto copied=document();InkCanvas restored;QCOMPARE(QJsonObject::fromVariantMap(restored.documentSnapshot()),copied);
        canvas.undo();QCOMPARE(document(),beforeCopies);QVERIFY(canvas.selectObject(base));canvas.removeSelection();
        QVERIFY(connection(0,"start").value("objectId").toString().isEmpty());
        QCOMPARE(endpoint(0,0),QPointF(600,300));
    }
    void malformedRoutesAndCyclesCannotOverwriteSavedDrawing(){
        InkCanvas canvas;setup(canvas);canvas.setTool("wire");canvas.begin(200,500);canvas.end(1000,500);
        canvas.begin(400,500);canvas.end(400,900);
        canvas.begin(200,500);canvas.end(200,900);const auto valid=document();
        for(const auto &mutation:QStringList{"route-endpoint","route-diagonal","route-mode","junction-fraction","cycle"}) {
            auto changed=valid;auto objects=changed.value("items").toArray();auto object=objects[0].toObject();
            auto shape=object.value("geometry").toObject();auto pathPoints=shape.value("wireRoute").toArray();
            if(mutation=="route-endpoint")pathPoints[0]=QJsonArray{201,500};
            if(mutation=="route-diagonal")pathPoints.insert(1,QJsonArray{600,550});
            if(mutation=="route-mode")shape.insert("wireRouteMode","unsupported");
            shape.insert("wireRoute",pathPoints);object.insert("geometry",shape);objects[0]=object;
            if(mutation=="junction-fraction") {
                object=objects[1].toObject();auto refs=object.value("connections").toObject();auto ref=refs.value("start").toObject();
                ref.insert("wirePosition",1.5);refs.insert("start",ref);object.insert("connections",refs);objects[1]=object;
            }
            if(mutation=="cycle") {
                object=objects[0].toObject();auto refs=object.value("connections").toObject();
                // Both start ports already coincide, so visible geometry cannot
                // expose this cyclic reference. The graph itself must reject it.
                refs.insert("start",QJsonObject{{"objectId",objects[2].toObject().value("id")},{"portId","start"}});
                object.insert("connections",refs);objects[0]=object;
            }
            changed.insert("items",objects);QVERIFY(write(changed));const auto original=bytes();
            InkCanvas rejected;QCOMPARE(rejected.itemCount(),0);rejected.insertSymbol("node");QCOMPARE(bytes(),original);
        }
    }
    void oldWireGeometryKeepsInkAndMigratesToTheCorrectRoutingMode(){
        for(bool explicitBend:{false,true}) {
            QFile::remove(path());InkCanvas canvas;setup(canvas);canvas.setSnapping(false);canvas.setTool("wire");
            canvas.begin(200,300);canvas.end(1000,800);QVERIFY(canvas.selectObject(id()));
            if(explicitBend)QVERIFY(canvas.setSelectionWireBend(80));
            auto old=document();auto objects=old.value("items").toArray();auto object=objects[0].toObject();
            auto shape=object.value("geometry").toObject();shape.remove("wireRoute");shape.remove("wireRouteMode");
            object.insert("geometry",shape);objects[0]=object;old.insert("items",objects);QVERIFY(write(old));
            InkCanvas restored;QCOMPARE(restored.itemCount(),1);QVERIFY(restored.selectObject(id()));
            QCOMPARE(restored.selectedWireAutomatic(),!explicitBend);
            const auto snapshot=QJsonObject::fromVariantMap(restored.documentSnapshot()).value("items").toArray().first().toObject();
            QCOMPARE(snapshot.value("strokes"),object.value("strokes"));
            QCOMPARE(snapshot.value("geometry").toObject().value("wireRouteMode").toString(),explicitBend?QString("legacy"):QString("auto"));
            QVERIFY(!restored.selectedWireRoute().isEmpty());
        }
    }
};
QTEST_MAIN(DrawingGraphTest)
#include "DrawingGraphTest.moc"

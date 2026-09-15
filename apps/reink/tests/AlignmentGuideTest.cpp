#include "InkCanvas.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QStandardPaths>
#include <QtTest>
#include <limits>

namespace Model=repaper::drawing;

class AlignmentGuideTest:public QObject {
    Q_OBJECT
    static Model::Item rectangle(const QString &id,QPointF center,QSizeF size={100,80}) {
        Model::Item item;item.id=id;item.kind="rectangle";
        const QPointF origin=center-QPointF(size.width()/2,size.height()/2);
        item.sourcePoints={origin,origin+QPointF(size.width(),0),origin+QPointF(0,size.height())};
        Model::rebuild(item);return item;
    }
    static QString path(){return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json";}
    static QByteArray bytes(){QFile f(path());return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}
    static QVariantList renderedInk(const InkCanvas &canvas,bool preview=false){
        QVariantList result;
        for(const auto &stroke:canvas.renderedStrokes(preview)){
            auto ink=stroke.toMap();ink.remove("objectId");ink.remove("preview");result.append(ink);
        }
        return result;
    }
    static QJsonObject item(const InkCanvas &canvas,int index) {
        return QJsonObject::fromVariantMap(canvas.documentSnapshot())["items"].toArray()[index].toObject();
    }
    static QPointF center(const InkCanvas &canvas,int index) {
        const auto points=item(canvas,index)["geometry"].toObject()["points"].toArray();
        const auto x=points[1].toArray(),y=points[2].toArray();
        return {(x[0].toDouble()+y[0].toDouble())/2,(x[1].toDouble()+y[1].toDouble())/2};
    }
    static void setup(InkCanvas &canvas,qreal scale=1) {
        canvas.setWidth(1404*scale);canvas.setHeight(1872*scale);canvas.setGridVisible(false);
        canvas.insertSymbol("resistor-iec",400,400);canvas.insertSymbol("capacitor",900,900);
    }
    static QImage painted(InkCanvas &canvas) {
        QImage result(canvas.width(),canvas.height(),QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::white);QPainter painter(&result);canvas.paint(&painter);return result;
    }
private slots:
    void firstTapUsesTheSymbolLogicalAnchor(){
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setGridVisible(false);canvas.setSnapping(false);
        for(const auto &symbol:QStringList{"resistor-iec","opamp","ground","square-root"}){
            canvas.clear();QVERIFY(canvas.beginStencil(symbol));
            canvas.begin(700,900);const auto preview=renderedInk(canvas,true);
            canvas.end(700,900);QCOMPARE(canvas.itemCount(),1);QCOMPARE(renderedInk(canvas),preview);
            const auto object=item(canvas,0);
            auto decoded=[](QJsonValue value){const auto p=value.toArray();return QPointF(p[0].toDouble(),p[1].toDouble());};
            QPointF anchor;
            if(symbol=="resistor-iec")anchor=center(canvas,0);
            else if(symbol=="opamp")anchor=decoded(object["anchors"].toArray().last());
            else if(symbol=="ground")anchor=decoded(object["anchors"].toArray().first());
            else anchor=decoded(object["strokes"].toArray().first().toObject()["points"].toArray().first());
            QCOMPARE(anchor,QPointF(700,900));
        }
    }
    void wireEndpointIsMagneticFromTheFirstTap(){
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setGridVisible(false);
        canvas.setTool("wire");canvas.begin(400,600);canvas.end(800,600);QCOMPARE(canvas.itemCount(),1);
        QVERIFY(canvas.beginStencil("resistor-iec"));canvas.begin(802,602);
        const auto preview=renderedInk(canvas,true);QCOMPARE(canvas.itemCount(),1);
        canvas.end(802,602);QCOMPARE(canvas.itemCount(),2);QCOMPARE(renderedInk(canvas),preview);
        const auto pin=item(canvas,1)["anchors"].toArray().first().toArray();
        QCOMPARE(QPointF(pin[0].toDouble(),pin[1].toDouble()),QPointF(800,600));
        QCOMPARE(center(canvas,1),QPointF(920,600));
        canvas.undo();QCOMPARE(canvas.itemCount(),1);canvas.redo();QCOMPARE(canvas.itemCount(),2);
        InkCanvas restored;QCOMPARE(restored.documentSnapshot(),canvas.documentSnapshot());
    }
    void initTestCase() {
        QCoreApplication::setOrganizationName("RePaperTests");QCoreApplication::setApplicationName("reink-alignment-test");
        QStandardPaths::setTestModeEnabled(true);
    }
    void init(){QFile::remove(path());}
    void cleanup(){QFile::remove(path());}
    void independentAxesUseNearbyObjectsWithoutChangingInk() {
        const auto moving=rectangle("moving",{300,300});
        const Model::Document targets{rectangle("horizontal",{500,305}),rectangle("vertical",{303,600})};
        const auto source=moving.sourcePoints;const auto ink=moving.strokes;
        const auto result=Model::alignTranslation(moving,targets,{0,2},8);
        QCOMPARE(result.delta,QPointF(3,5));QVERIFY(result.snappedX);QVERIFY(result.snappedY);
        QCOMPARE(result.guides.size(),2);QCOMPARE(result.guides[0].targetId,QString("vertical"));
        QCOMPARE(result.guides[1].targetId,QString("horizontal"));
        QVERIFY(std::abs(result.guides[0].line.dx())<1e-7);QVERIFY(std::abs(result.guides[1].line.dy())<1e-7);
        QCOMPARE(moving.sourcePoints,source);QCOMPARE(moving.strokes.size(),ink.size());
        for(int i=0;i<ink.size();++i)QCOMPARE(moving.strokes[i].points,ink[i].points);
    }
    void distantObjectsSelfAndUnsupportedInkDoNotSnap() {
        const auto moving=rectangle("moving",{300,300});
        const Model::Document targets{moving,rectangle("far",{300,1200})};
        const QPointF proposed(3.4,29.7);
        const auto result=Model::alignTranslation(moving,targets,proposed,8);
        QCOMPARE(result.delta,proposed);QVERIFY(result.guides.isEmpty());
        auto stroke=moving;stroke.kind="pen";
        const auto unsupported=Model::alignTranslation(stroke,{rectangle("close",{303,600})},{},8);
        QVERIFY(unsupported.guides.isEmpty());
    }
    void equallyCloseTargetsHaveStableIdentityOrder() {
        const auto moving=rectangle("moving",{300,300});
        const auto left=rectangle("a",{295,600}),right=rectangle("z",{305,600});
        const auto first=Model::alignTranslation(moving,{left,right},{},8);
        const auto second=Model::alignTranslation(moving,{right,left},{},8);
        QCOMPARE(first.delta,QPointF(-5,0));QCOMPARE(first.delta,second.delta);
        QVERIFY(first.guides==second.guides);QCOMPARE(first.guides[0].targetId,QString("a"));
    }
    void rotatedBoxesAndExplicitPortsAlignInPageCoordinates() {
        auto moving=rectangle("moving",{300,300});
        moving.sourcePoints={{340,250},{340,350},{260,250}};QVERIFY(Model::rebuild(moving));
        auto target=rectangle("target",{304,600});
        const auto rotated=Model::alignTranslation(moving,{target},{},8);
        QCOMPARE(rotated.delta,QPointF(4,0));QVERIFY(rotated.snappedX);
        moving.anchors={{321,309}};target.anchors={{322,604}};
        const auto ports=Model::alignTranslation(moving,{target},{},8);
        QCOMPARE(ports.delta.x(),qreal(1));QCOMPARE(ports.guides[0].line.x1(),qreal(322));
    }
    void invalidToleranceAndCoordinatesAreSafe() {
        const auto moving=rectangle("moving",{300,300});const Model::Document target{rectangle("target",{304,600})};
        const qreal nan=std::numeric_limits<qreal>::quiet_NaN();
        const auto invalid=Model::alignTranslation(moving,target,{nan,1},8);
        QCOMPARE(invalid.delta,QPointF());QVERIFY(invalid.guides.isEmpty());
        for(qreal tolerance:{qreal(-1),nan}) {
            const auto result=Model::alignTranslation(moving,target,{2,3},tolerance);
            QCOMPARE(result.delta,QPointF(2,3));QVERIFY(result.guides.isEmpty());
        }
    }
    void componentMoveHasTransientGuidesAndOneUndo() {
        InkCanvas canvas;setup(canvas);const auto identity=item(canvas,0)["id"].toString();
        QVERIFY(canvas.selectObject(identity));const auto saved=bytes();const auto snapshot=canvas.documentSnapshot();const auto strokes=canvas.renderedStrokes();
        QSignalSpy changes(&canvas,&InkCanvas::alignmentGuidesChanged);
        canvas.begin(400,400);canvas.move(905,600);
        QVERIFY(!canvas.alignmentGuides().isEmpty());QCOMPARE(canvas.documentSnapshot(),snapshot);
        QCOMPARE(canvas.renderedStrokes(),strokes);QCOMPARE(bytes(),saved);QVERIFY(!changes.isEmpty());
        const auto preview=canvas.renderedStrokes(true);const auto withGuides=painted(canvas);
        canvas.setSnapping(false);QVERIFY(canvas.alignmentGuides().isEmpty());
        QCOMPARE(canvas.renderedStrokes(true),preview);QVERIFY(painted(canvas)!=withGuides);
        canvas.cancel();QCOMPARE(canvas.documentSnapshot(),snapshot);QCOMPARE(bytes(),saved);
        canvas.setSnapping(true);canvas.begin(400,400);canvas.end(905,600);
        QCOMPARE(center(canvas,0),QPointF(900,600));QVERIFY(canvas.alignmentGuides().isEmpty());
        const auto aligned=canvas.documentSnapshot();QVERIFY(!bytes().contains("alignmentGuide"));
        canvas.undo();QCOMPARE(canvas.documentSnapshot(),snapshot);canvas.redo();QCOMPARE(canvas.documentSnapshot(),aligned);
    }
    void alignmentToleranceStaysEightScreenPixels_data() {
        QTest::addColumn<qreal>("scale");QTest::addColumn<qreal>("screenDistance");QTest::addColumn<bool>("snapped");
        QTest::newRow("native-inside")<<qreal(1)<<qreal(7.5)<<true;
        QTest::newRow("native-outside")<<qreal(1)<<qreal(8.5)<<false;
        QTest::newRow("half-inside")<<qreal(.5)<<qreal(7.5)<<true;
        QTest::newRow("half-outside")<<qreal(.5)<<qreal(8.5)<<false;
    }
    void alignmentToleranceStaysEightScreenPixels() {
        QFETCH(qreal,scale);QFETCH(qreal,screenDistance);QFETCH(bool,snapped);
        InkCanvas canvas;setup(canvas,scale);QVERIFY(canvas.selectObject(item(canvas,0)["id"].toString()));
        const qreal requested=900+screenDistance/scale;
        canvas.begin(400*scale,400*scale);canvas.move(requested*scale,600*scale);
        QCOMPARE(!canvas.alignmentGuides().isEmpty(),snapped);
        canvas.end(requested*scale,600*scale);QCOMPARE(center(canvas,0),QPointF(snapped?900:requested,600));
    }
    void placementGuidesDisappearOnCommitAndCancel() {
        InkCanvas canvas;setup(canvas);const auto before=canvas.documentSnapshot();
        QVERIFY(canvas.beginStencil("resistor-iec"));canvas.begin(906,600);canvas.move(906,600);
        QVERIFY(!canvas.alignmentGuides().isEmpty());QCOMPARE(canvas.documentSnapshot(),before);
        canvas.cancel();QVERIFY(canvas.alignmentGuides().isEmpty());QCOMPARE(canvas.documentSnapshot(),before);
        canvas.begin(906,600);canvas.end(906,600);QCOMPARE(canvas.itemCount(),3);
        QCOMPARE(center(canvas,2),QPointF(900,600));QVERIFY(canvas.alignmentGuides().isEmpty());
        canvas.undo();QCOMPARE(canvas.documentSnapshot(),before);
        canvas.setSnapping(false);QVERIFY(canvas.beginStencil("resistor-iec"));canvas.begin(907,600);canvas.end(907,600);
        QCOMPARE(center(canvas,2),QPointF(907,600));QVERIFY(canvas.alignmentGuides().isEmpty());
    }
    void tappingNearbyComponentNeverMovesIt() {
        InkCanvas canvas;setup(canvas);canvas.insertSymbol("resistor-iec",905,600);
        QVERIFY(canvas.selectObject(item(canvas,2)["id"].toString()));const auto before=canvas.documentSnapshot();
        canvas.begin(905,600);canvas.end(905,600);QCOMPARE(canvas.documentSnapshot(),before);QVERIFY(canvas.alignmentGuides().isEmpty());
    }
    void placementOrientationMatchesTheActualDipoleAxis_data() {
        QTest::addColumn<QString>("symbol");QTest::addColumn<bool>("vertical");
        for(const QString &symbol:{"resistor-iec","capacitor","voltage-source","current-source"}) {
            QTest::newRow(qPrintable(symbol+"-horizontal"))<<symbol<<false;
            QTest::newRow(qPrintable(symbol+"-vertical"))<<symbol<<true;
        }
    }
    void placementOrientationMatchesTheActualDipoleAxis() {
        QFETCH(QString,symbol);QFETCH(bool,vertical);
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);
        QVERIFY(canvas.beginStencil(symbol));QVERIFY(canvas.setStencilVertical(vertical));
        QVERIFY(canvas.setVoltageArrow(true));QVERIFY(canvas.setVoltageReversed(true));QVERIFY(canvas.setVoltageOtherSide(true));
        QCOMPARE(canvas.property("activeStencilId").toString(),symbol);QVERIFY(canvas.stencilSupportsVoltage());
        QCOMPARE(canvas.stencilVertical(),vertical);QVERIFY(canvas.stencilVoltageArrow());
        canvas.begin(700,900);canvas.end(700,900);QCOMPARE(canvas.itemCount(),1);QCOMPARE(center(canvas,0),QPointF(700,900));
        const auto object=item(canvas,0),geometry=object["geometry"].toObject();const auto anchors=object["anchors"].toArray();
        QCOMPARE(anchors.size(),2);const auto a=anchors[0].toArray(),b=anchors[1].toArray();
        const qreal dx=std::abs(b[0].toDouble()-a[0].toDouble()),dy=std::abs(b[1].toDouble()-a[1].toDouble());
        QVERIFY(vertical?dx<1e-7&&dy>1:dy<1e-7&&dx>1);
        QVERIFY(geometry["voltageArrow"].toBool());QVERIFY(geometry["voltageArrowReversed"].toBool());QVERIFY(geometry["voltageArrowOtherSide"].toBool());
        const auto saved=canvas.documentSnapshot();canvas.undo();QCOMPARE(canvas.itemCount(),0);canvas.redo();QCOMPARE(canvas.documentSnapshot(),saved);
        QVERIFY(canvas.beginStencil(symbol));QCOMPARE(canvas.stencilVertical(),vertical);QVERIFY(canvas.stencilVoltageArrow());
    }
    void voltageSettersEditOneSelectedObjectWithoutChangingCreationPreferences() {
        InkCanvas canvas;setup(canvas);QVERIFY(canvas.beginStencil("resistor-iec"));
        QVERIFY(canvas.setVoltageArrow(true));QVERIFY(canvas.setVoltageReversed(true));QVERIFY(canvas.setVoltageOtherSide(true));
        const auto selected=item(canvas,0)["id"].toString();QVERIFY(canvas.selectObject(selected));
        QVERIFY(canvas.selectionSupportsVoltage());QVERIFY(!canvas.selectedVoltageArrow());
        const auto before=canvas.documentSnapshot();const int count=item(canvas,0)["strokes"].toArray().size();
        QVERIFY(canvas.setVoltageArrow(true));QCOMPARE(canvas.itemCount(),2);QVERIFY(canvas.selectedVoltageArrow());
        QCOMPARE(item(canvas,0)["id"].toString(),selected);QCOMPARE(item(canvas,0)["strokes"].toArray().size(),count+2);
        const auto enabled=canvas.documentSnapshot();canvas.undo();QCOMPARE(canvas.documentSnapshot(),before);canvas.redo();QCOMPARE(canvas.documentSnapshot(),enabled);
        QVERIFY(canvas.selectObject(selected));QVERIFY(canvas.setVoltageReversed(true));QVERIFY(canvas.selectedVoltageReversed());
        QVERIFY(canvas.setVoltageOtherSide(true));QVERIFY(canvas.selectedVoltageOtherSide());
        QVERIFY(canvas.setVoltageArrow(false));QVERIFY(!canvas.selectedVoltageArrow());QCOMPARE(item(canvas,0)["strokes"].toArray().size(),count);
        const auto unchanged=bytes();QVERIFY(canvas.setVoltageArrow(false));QCOMPARE(bytes(),unchanged);
        QVERIFY(canvas.stencilVoltageArrow());QVERIFY(canvas.stencilVoltageReversed());QVERIFY(canvas.stencilVoltageOtherSide());
        InkCanvas restored;QCOMPARE(restored.documentSnapshot(),canvas.documentSnapshot());
    }
    void unsupportedSymbolsIgnoreVoltageAndKeepMathOrientation() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);
        QVERIFY(canvas.beginStencil("resistor-iec"));QVERIFY(canvas.setStencilVertical(true));
        QVERIFY(canvas.setVoltageArrow(true));QVERIFY(canvas.setVoltageReversed(true));QVERIFY(canvas.setVoltageOtherSide(true));
        for(const QString &symbol:{"square-root","table","opamp"}) {
            QVERIFY(canvas.beginStencil(symbol));QVERIFY(!canvas.stencilSupportsVoltage());
            canvas.begin(700,900);canvas.end(700,900);const int index=canvas.itemCount()-1;QVERIFY(index>=0);
            const auto object=item(canvas,index),geometry=object["geometry"].toObject();
            QCOMPARE(geometry["symbolId"].toString(),symbol);QVERIFY(!geometry["voltageArrow"].toBool());
            QVERIFY(!geometry["voltageArrowReversed"].toBool());QVERIFY(!geometry["voltageArrowOtherSide"].toBool());
            if(symbol!="opamp") {
                const auto p=geometry["points"].toArray();QCOMPARE(p[0].toArray()[1],p[1].toArray()[1]);
            }
            QVERIFY(canvas.selectObject(object["id"].toString()));QVERIFY(!canvas.selectionSupportsVoltage());
            const auto before=canvas.documentSnapshot();QVERIFY(!canvas.setVoltageArrow(true));QCOMPARE(canvas.documentSnapshot(),before);
        }
        canvas.setTool("rectangle");canvas.begin(100,100);canvas.end(200,200);QCOMPARE(canvas.itemCount(),4);
        QVERIFY(canvas.beginStencil("resistor-iec"));QVERIFY(canvas.stencilVertical());QVERIFY(canvas.stencilVoltageArrow());
    }
    void verticalAnnotatedPlacementClampsTheWholeGroupedItem() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setSnapping(false);
        QVERIFY(canvas.beginStencil("resistor-iec"));QVERIFY(canvas.setStencilVertical(true));QVERIFY(canvas.setVoltageArrow(true));
        canvas.begin(1404,0);canvas.end(1404,0);QCOMPARE(canvas.itemCount(),1);
        for(const auto &stroke:item(canvas,0)["strokes"].toArray())for(const auto &point:stroke.toObject()["points"].toArray()) {
            const auto p=point.toArray();QVERIFY(p[0].toDouble()>=0&&p[0].toDouble()<=1404);QVERIFY(p[1].toDouble()>=0&&p[1].toDouble()<=1872);
        }
    }
    void connectedComponentCanBeGrabbedInsideAWireBoundingBox() {
        InkCanvas canvas;canvas.setWidth(1404);canvas.setHeight(1872);canvas.setGridVisible(false);
        for(const auto &entry:QVector<QPair<QString,QPointF>>{{"resistor-iec",{400,500}},{"capacitor",{900,950}},{"voltage-source",{400,1150}}}) {
            QVERIFY(canvas.beginStencil(entry.first));QVERIFY(canvas.setStencilVertical(true));QVERIFY(canvas.setVoltageArrow(true));
            canvas.begin(entry.second.x(),entry.second.y());canvas.end(entry.second.x(),entry.second.y());
        }
        auto port=[&](int index,int number) {const auto p=item(canvas,index)["anchors"].toArray()[number].toArray();return QPointF(p[0].toDouble(),p[1].toDouble());};
        auto wire=[&](QPointF a,QPointF b) {canvas.setTool("wire");canvas.begin(a.x(),a.y());canvas.end(b.x(),b.y());};
        wire(port(2,0),port(0,0));wire(port(0,1),port(1,0));wire(port(1,1),port(2,1));QCOMPARE(canvas.itemCount(),6);
        const auto selected=item(canvas,0)["id"].toString();QVERIFY(canvas.selectObject(selected));
        const auto before=canvas.documentSnapshot();canvas.begin(400,500);QCOMPARE(canvas.selectedObjectId(),selected);
        canvas.move(906,650);QVERIFY(!canvas.alignmentGuides().isEmpty());
        canvas.move(400,500);QVERIFY(canvas.alignmentGuides().isEmpty());QCOMPARE(canvas.documentSnapshot(),before);
        canvas.end(906,650);QCOMPARE(center(canvas,0),QPointF(900,650));QCOMPARE(canvas.itemCount(),6);
        const auto endpoint=item(canvas,3)["geometry"].toObject()["points"].toArray()[1].toArray();
        QCOMPARE(QPointF(endpoint[0].toDouble(),endpoint[1].toDouble()),port(0,0));
        canvas.undo();QCOMPARE(canvas.documentSnapshot(),before);
    }
    void recentStencilChoicesAreUniqueBoundedAndOnlyChangeAfterValidChoice() {
        InkCanvas canvas;
        QCOMPARE(canvas.recentStencils(),QStringList({"resistor-iec","capacitor","voltage-source","square-root"}));
        QVERIFY(canvas.beginStencil("current-source"));
        QCOMPARE(canvas.recentStencils(),QStringList({"current-source","resistor-iec","capacitor","voltage-source"}));
        QVERIFY(canvas.beginStencil("capacitor"));
        QCOMPARE(canvas.recentStencils(),QStringList({"capacitor","current-source","resistor-iec","voltage-source"}));
        const auto before=canvas.recentStencils();QVERIFY(!canvas.beginStencil("unknown-symbol"));QCOMPARE(canvas.recentStencils(),before);
    }
};

QTEST_MAIN(AlignmentGuideTest)
#include "AlignmentGuideTest.moc"

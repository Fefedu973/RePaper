#include "Geometry.h"
#include "SceneWriter.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineF>
#include <QTemporaryDir>
#include <QTransform>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace PaperDrawing;

namespace {
constexpr double Pi=3.14159265358979323846;
double valueAtEnd(const Symbol &symbol, double yMin, double yMax) {
    return yMax-(symbol.strokes.last().points.last().y()-5)*(yMax-yMin)/70;
}
// Independent reference: integrate y'' + 2*zeta*y' + y = 1, y(0)=y'(0)=0.
// This exercises all damping regimes without duplicating the closed form.
double integratedUnitStep(double time, double damping) {
    double y=0, velocity=0;
    const int steps=20000;
    const double h=time/steps;
    auto acceleration=[&](double position,double speed) { return 1-2*damping*speed-position; };
    for(int i=0;i<steps;++i) {
        const double a1=acceleration(y,velocity), v1=velocity;
        const double v2=velocity+h*a1/2, a2=acceleration(y+h*v1/2,v2);
        const double v3=velocity+h*a2/2, a3=acceleration(y+h*v2/2,v3);
        const double v4=velocity+h*a3, a4=acceleration(y+h*v3,v4);
        y+=h*(v1+2*v2+2*v3+v4)/6;
        velocity+=h*(a1+2*a2+2*a3+a4)/6;
    }
    return y;
}
bool segmentEntersRect(QPointF a,QPointF b,QRectF rect) {
    // Independent check using Qt's bounded segment intersections, with a tiny
    // inset so a route exactly at the requested clearance remains legal.
    rect=rect.adjusted(1e-5,1e-5,-1e-5,-1e-5);
    if (rect.isEmpty()) return false;
    if (rect.contains(a) || rect.contains(b)) return true;
    const QLineF edges[]={QLineF(rect.topLeft(),rect.topRight()),QLineF(rect.topRight(),rect.bottomRight()),
        QLineF(rect.bottomRight(),rect.bottomLeft()),QLineF(rect.bottomLeft(),rect.topLeft())};
    for (const auto &edge:edges) if (QLineF(a,b).intersects(edge,nullptr)==QLineF::BoundedIntersection) return true;
    return false;
}
QString routeProblem(const Polyline &route,QPointF start,QPointF end,const OrthogonalRouteOptions &options) {
    if (route.size()<2 || route.first()!=start || route.last()!=end) return "Missing/exchanged endpoint";
    for (int i=1;i<route.size();++i) {
        const auto a=route[i-1],b=route[i];
        if (a==b || (a.x()!=b.x() && a.y()!=b.y())) return "Zero or diagonal segment";
        if (!std::isfinite(b.x()) || !std::isfinite(b.y())) return "Nonfinite point";
        for (int j=1;j<i-1;++j) {
            const QLineF earlier(route[j-1],route[j]), current(a,b);
            if (earlier.intersects(current,nullptr)==QLineF::BoundedIntersection) return "Self crossing/contact";
            // QLineF reports parallel collinear segments as NoIntersection.
            // A shared endpoint or one lying within the other segment still
            // constitutes an invalid return along an earlier wire leg.
            auto onLine=[](QPointF p,const QLineF &line) {
                return std::abs(QLineF(line.p1(),p).length()+QLineF(p,line.p2()).length()-line.length())<1e-7;
            };
            if (onLine(a,earlier) || onLine(b,earlier) ||
                onLine(earlier.p1(),current) || onLine(earlier.p2(),current)) return "Self overlap/contact";
        }
        if (i>1) {
            const auto previous=a-route[i-2],current=b-a;
            if (previous.x()*current.x()+previous.y()*current.y()<0) return "Immediate reversal";
            if ((previous.x()==0 && current.x()==0) || (previous.y()==0 && current.y()==0)) return "Redundant collinear bend";
        }
        for (auto obstacle:options.obstacles) {
            obstacle=obstacle.normalized();
            const auto inclusive=obstacle.adjusted(-1e-6,-1e-6,1e-6,1e-6);
            if ((i==1 && inclusive.contains(start)) || (i==route.size()-1 && inclusive.contains(end))) continue;
            const auto expanded=obstacle.adjusted(-options.clearance,-options.clearance,options.clearance,options.clearance);
            if (segmentEntersRect(a,b,expanded)) return "Obstacle/clearance collision";
        }
    }
    if (!options.startDirection.isNull() && QPointF::dotProduct(route[1]-start,options.startDirection)<=0) return "Wrong departure direction";
    if (!options.endDirection.isNull() && QPointF::dotProduct(route[route.size()-2]-end,options.endDirection)<=0) return "Wrong arrival direction";
    return {};
}
}

class GeometryTest:public QObject {
    Q_OBJECT
private slots:
    void smartRouteAvoidsOccupiedParallelLanesButAllowsPerpendicularCrossings() {
        OrthogonalRouteOptions options;options.grid=8;
        options.occupiedSegments={{{30,0},{70,0},{}}};
        options.preferredRoute={{0,0},{100,0}};
        const auto detour=orthogonalRoute({0,0},{100,0},options);
        QCOMPARE(routeProblem(detour,{0,0},{100,0},options),QString());QVERIFY(detour.size()>=4);
        for(int i=1;i<detour.size();++i) {
            if(detour[i].y()!=0||detour[i-1].y()!=0)continue;
            QVERIFY(std::max(detour[i].x(),detour[i-1].x())<=30||std::min(detour[i].x(),detour[i-1].x())>=70);
        }
        options.occupiedSegments={{{50,-40},{50,40},{}}};
        QCOMPARE(orthogonalRoute({0,0},{100,0},options),Polyline({{0,0},{100,0}}));
        options.occupiedSegments={{{100,0},{200,0},{}}};
        QCOMPARE(orthogonalRoute({0,0},{100,0},options),Polyline({{0,0},{100,0}}));
    }
    void smartRouteSharesAStubOnlyAtAnExplicitJunction() {
        OrthogonalRouteOptions options;options.startDirection={1,0};
        options.occupiedSegments={{{0,0},{400,0},{{0,0}}}};
        const Polyline preferred={{0,0},{100,0},{100,80}};options.preferredRoute=preferred;
        QCOMPARE(orthogonalRoute({0,0},{100,80},options),preferred);
        options.occupiedSegments[0].sharedJunctions={{300,0}};
        QVERIFY(orthogonalRoute({0,0},{100,80},options).isEmpty());
        options.occupiedSegments[0].sharedJunctions.clear();
        QVERIFY(orthogonalRoute({0,0},{100,80},options).isEmpty());
        options.startDirection={};
        const auto free=orthogonalRoute({0,0},{100,80},options);
        QCOMPARE(routeProblem(free,{0,0},{100,80},options),QString());
        QCOMPARE(free,Polyline({{0,0},{0,80},{100,80}}));
    }
    void smartRouteFindsAnotherExitForADetachedEndpointOnAnOccupiedPort() {
        OrthogonalRouteOptions options;options.obstacles={QRectF(100,0,100,100)};
        options.occupiedSegments={{{0,50},{100,50},{}}};
        const auto route=orthogonalRoute({0,200},{100,50},options);
        QCOMPARE(routeProblem(route,{0,200},{100,50},options),QString());
        QCOMPARE(route[route.size()-2].x(),qreal(100)); // Approach across the old wire.
        QVERIFY(route[route.size()-2].y()!=50);
        for(int i=1;i<route.size();++i)if(route[i-1].y()==50&&route[i].y()==50)
            QVERIFY(std::min(route[i-1].x(),route[i].x())>=100||std::max(route[i-1].x(),route[i].x())<=0);
        options.endDirection={-1,0};
        QVERIFY(orthogonalRoute({0,200},{100,50},options).isEmpty()); // A real port stays constrained.
    }
    void simpleVoltageGeneratorContainsOnlyACircleAndContinuousWire() {
        Symbol generator,original;
        for(const auto &symbol:electronicsCatalogue()) {
            if(symbol.id=="voltage-generator-simple")generator=symbol;
            if(symbol.id=="voltage-source")original=symbol;
        }
        QCOMPARE(generator.name,QString("Générateur de tension simple"));QCOMPARE(generator.category,QString("Sources"));
        QCOMPARE(generator.strokes.size(),2);QCOMPARE(generator.strokes[0].points,Polyline({{0,40},{120,40}}));
        const auto ring=generator.strokes[1].points;QCOMPARE(ring.size(),49);
        QVERIFY(QLineF(ring.first(),ring.last()).length()<1e-9);
        for(auto p:ring)QVERIFY(std::abs(QLineF(p,{60,40}).length()-26)<1e-9);
        QCOMPARE(generator.anchors,QVector<QPointF>({{0,40},{120,40}}));
        QCOMPARE(generator.portIds,QStringList({"start","end"}));QVERIFY(supportsVoltageArrow(generator));
        const auto vertical=transformStrokes(generator.strokes,{400,300},2,1);
        QCOMPARE(vertical[0].points,Polyline({{320,300},{320,540}}));
        for(auto p:vertical[1].points)QVERIFY(std::abs(QLineF(p,{320,420}).length()-52)<1e-9);
        // The earlier source keeps its explicit polarity markings and its
        // existing vertical terminals; this is a separate catalogue variant.
        QCOMPARE(original.strokes.size(),6);QCOMPARE(original.anchors,QVector<QPointF>({{60,0},{60,80}}));
        QCOMPARE(original.portIds,QStringList({"positive","negative"}));
    }
    void squareRootExtendsTheOverbarWithoutWideningTheHook() {
        const auto normal=squareRoot(QRectF(0,0,120,80));
        const auto wide=squareRoot(QRectF(0,0,360,80));
        QCOMPARE(normal.size(),5);QCOMPARE(wide.size(),5);
        for(int i=0;i<4;++i)QCOMPARE(wide[i],normal[i]);
        QCOMPARE(normal.last(),QPointF(120,8));QCOMPARE(wide.last(),QPointF(360,8));
        const auto tall=squareRoot(QRectF(10,20,360,160));
        for(int i=0;i<4;++i)QCOMPARE(tall[i]-QPointF(10,20),normal[i]*2);
        QCOMPARE(squareRoot(QRectF(370,180,-360,-160)),tall);
        for(auto p:tall)QVERIFY(QRectF(10,20,360,160).contains(p));
        QVERIFY(squareRoot(QRectF()).isEmpty());
        QVERIFY(squareRoot(QRectF(0,0,std::numeric_limits<qreal>::quiet_NaN(),80)).isEmpty());
        Symbol radical;for(const auto &symbol:electronicsCatalogue())if(symbol.id=="square-root")radical=symbol;
        QCOMPARE(radical.category,QString("Mathématiques"));QVERIFY(radical.anchors.isEmpty());QVERIFY(radical.portIds.isEmpty());
        QVERIFY(!supportsVoltageArrow(radical));QCOMPARE(radical.strokes[0].points,normal);
        for(int turn=0;turn<4;++turn) {
            const auto mapped=transformStrokes(radical.strokes,{300,400},2,turn);
            QTransform transform;transform.translate(300,400);transform.scale(2,2);transform.rotate(turn*90);
            QCOMPARE(mapped.size(),1);for(int i=0;i<normal.size();++i)QCOMPARE(mapped[0].points[i],transform.map(normal[i]));
        }
        QTemporaryDir directory;QString error;
        const auto strokes=QVector<Stroke>{{wide,2.3,Qt::black}};
        QVERIFY2(exportSvg(directory.path()+"/radical.svg",strokes,&error),qPrintable(error));
        QVERIFY2(exportPdf(directory.path()+"/radical.pdf",strokes,"Racine carrée",&error),qPrintable(error));
        QFile svg(directory.path()+"/radical.svg");QVERIFY(svg.open(QIODevice::ReadOnly));QVERIFY(svg.readAll().contains("360.000,8.000"));
        QFile pdf(directory.path()+"/radical.pdf");QVERIFY(pdf.open(QIODevice::ReadOnly));QCOMPARE(pdf.read(5),QByteArray("%PDF-"));
    }
    void voltageArrowsFollowTwoTerminalComponentsWithoutAddingPorts() {
        const QStringList supported={"resistor-iec","resistor-ansi","capacitor","capacitor-polarized","inductor",
            "diode","led","zener","voltage-source","current-source","voltage-generator-simple","signal-generator","battery","switch","switch-closed"};
        for(const auto &symbol:electronicsCatalogue()) {
            const auto originalAnchors=symbol.anchors;
            QCOMPARE(supportsVoltageArrow(symbol),supported.contains(symbol.id));
            QCOMPARE(supportsVoltageArrow(symbol.id),supported.contains(symbol.id));
            if(!supported.contains(symbol.id)) {QVERIFY(voltageArrowStrokes(symbol).isEmpty());continue;}
            const auto axis=symbol.anchors[1]-symbol.anchors[0];
            const QPointF normal(axis.y(),-axis.x());
            for(bool reversed:{false,true})for(bool otherSide:{false,true}) {
                const auto strokes=voltageArrowStrokes(symbol,reversed,otherSide);
                QCOMPARE(strokes.size(),2);QCOMPARE(strokes[0].points.size(),2);QCOMPARE(strokes[1].points.size(),3);
                const auto from=strokes[0].points[0],to=strokes[0].points[1];
                QCOMPARE(to-from,reversed?axis:-axis);QCOMPARE(strokes[1].points[1],to);
                const qreal position=QPointF::dotProduct(from-symbol.anchors[reversed?0:1],normal);
                for(const auto &body:symbol.strokes)for(auto point:body.points) {
                    const qreal bodyPosition=QPointF::dotProduct(point-symbol.anchors[0],normal);
                    QVERIFY2(otherSide?position<bodyPosition:position>bodyPosition,qPrintable(symbol.id));
                }
                for(const auto &stroke:strokes) {QCOMPARE(stroke.width,2.3);QCOMPARE(stroke.color,QColor(Qt::black));}
            }
            QCOMPARE(symbol.anchors,originalAnchors);
        }
        QVERIFY(!supportsVoltageArrow(QStringLiteral("unknown")));
    }
    void voltageArrowDefaultsPointUpBesideAVerticalResistor() {
        Symbol resistor;for(const auto &symbol:electronicsCatalogue())if(symbol.id=="resistor-iec")resistor=symbol;
        const auto arrow=voltageArrowStrokes(resistor);
        QCOMPARE(arrow[0].points,Polyline({{120,12},{0,12}}));
        const auto vertical=transformStrokes(arrow,{400,500},2,1);
        const auto body=transformStrokes(resistor.strokes,{400,500},2,1);
        QCOMPARE(vertical[0].points,Polyline({{376,740},{376,500}}));
        QVERIFY(vertical[0].points[0].x()>bounds(body).right());
        const auto other=transformStrokes(voltageArrowStrokes(resistor,false,true),{400,500},2,1);
        QVERIFY(other[0].points[0].x()<bounds(body).left());
        QVERIFY(other[0].points.last().y()<other[0].points.first().y());
        const auto reverse=voltageArrowStrokes(resistor,true);
        QCOMPARE(reverse[0].points,Polyline({{0,12},{120,12}}));
        auto invalid=resistor;invalid.anchors[0].setX(std::numeric_limits<qreal>::quiet_NaN());
        QVERIFY(voltageArrowStrokes(invalid).isEmpty());
    }
    void smartRouteAutomaticallyChoosesTheFreeElbow() {
        OrthogonalRouteOptions options;
        options.clearance=5; options.grid=10;
        options.obstacles={QRectF(40,-10,20,30)};
        const auto route=orthogonalRoute({0,0},{100,100},options);
        QCOMPARE(route,Polyline({{0,0},{0,100},{100,100}}));
        QCOMPARE(routeProblem(route,{0,0},{100,100},options),QString());
        options.obstacles.clear();
        QCOMPARE(orthogonalRoute({0,0},{100,100},options),Polyline({{0,0},{100,0},{100,100}}));
        options.horizontalFirst=false;
        QCOMPARE(orthogonalRoute({0,0},{100,100},options),Polyline({{0,0},{0,100},{100,100}}));
        QCOMPARE(orthogonalRoute({2,3},{2,50},options),Polyline({{2,3},{2,50}}));
    }
    void smartRouteDetoursWithClearanceAndStableGrid() {
        OrthogonalRouteOptions options;
        options.clearance=11; options.grid=8;
        options.obstacles={QRectF(80,-30,80,60)};
        const QPointF start(.25,.75),end(240.25,.75);
        const auto route=orthogonalRoute(start,end,options);
        QVERIFY(route.size()>=4);
        QCOMPARE(routeProblem(route,start,end,options),QString());
        for (const auto &point:route) {
            QVERIFY(point.x()==start.x() || point.x()==end.x() || std::abs(point.x()/8-std::round(point.x()/8))<1e-9);
            QVERIFY(point.y()==start.y() || point.y()==end.y() || std::abs(point.y()/8-std::round(point.y()/8))<1e-9);
        }
        options.preferredRoute=route;
        const QPointF movedStart(.5,1),movedEnd(240.5,1);
        const auto moved=orthogonalRoute(movedStart,movedEnd,options);
        QCOMPARE(routeProblem(moved,movedStart,movedEnd,options),QString());
        QCOMPARE(moved.size(),route.size());
        QCOMPARE(moved[1].y(),route[1].y());
    }
    void smartRouteFindsSeveralAlternatingPassages() {
        OrthogonalRouteOptions options;
        options.clearance=10; options.grid=10;
        options.obstacles={QRectF(50,-1000,20,1030),QRectF(120,-30,20,1030),QRectF(190,-1000,20,1030)};
        const auto route=orthogonalRoute({0,0},{300,0},options);
        QVERIFY(route.size()>=7);
        QCOMPARE(routeProblem(route,{0,0},{300,0},options),QString());
        for (const auto &point:route) QVERIFY(std::abs(point.y())<200);
        std::reverse(options.obstacles.begin(),options.obstacles.end());
        QCOMPARE(orthogonalRoute({0,0},{300,0},options),route);
        QCOMPARE(orthogonalRoute({0,0},{300,0},options),route);
    }
    void smartRouteHonorsAllPortOrientations_data() {
        QTest::addColumn<QPointF>("departure");
        QTest::addColumn<QPointF>("arrival");
        const QPointF directions[]={{1,0},{0,1},{-1,0},{0,-1}};
        for (int a=0;a<4;++a) for (int b=0;b<4;++b)
            QTest::newRow(qPrintable(QString("%1-to-%2").arg(a).arg(b))) << directions[a] << directions[b];
    }
    void smartRouteHonorsAllPortOrientations() {
        QFETCH(QPointF,departure); QFETCH(QPointF,arrival);
        const QPointF start(0,0),end(200,140);
        OrthogonalRouteOptions options;
        options.startDirection=departure; options.endDirection=arrival;
        options.grid=10; options.clearance=8;
        const auto startCenter=start-departure*20,endCenter=end-arrival*20;
        options.obstacles={QRectF(startCenter-QPointF(20,20),QSizeF(40,40)),
                           QRectF(endCenter-QPointF(20,20),QSizeF(40,40))};
        const auto route=orthogonalRoute(start,end,options);
        QCOMPARE(routeProblem(route,start,end,options),QString());
        const auto first=route[1]-start,last=route[route.size()-2]-end;
        QCOMPARE(first.x()*departure.y()-first.y()*departure.x(),0.0);
        QCOMPARE(last.x()*arrival.y()-last.y()*arrival.x(),0.0);
    }
    void smartRouteConnectsCloseFacingPinsWithoutAnArtificialLoop() {
        OrthogonalRouteOptions options;
        options.startDirection={1,0}; options.endDirection={-1,0};
        options.obstacles={QRectF(-40,-20,40,40),QRectF(10,-20,40,40)};
        QCOMPARE(orthogonalRoute({0,0},{10,0},options),Polyline({{0,0},{10,0}}));
    }
    void smartRouteKeepsThePreviousInteriorLanesWhileMovingComponents() {
        OrthogonalRouteOptions options;
        options.startDirection={1,0}; options.endDirection={-1,0};
        options.preferredRoute={{0,0},{40,0},{40,100},{100,100}};
        options.clearance=2;
        const auto preserved=orthogonalRoute({0,10},{100,110},options);
        QCOMPARE(preserved,Polyline({{0,10},{40,10},{40,110},{100,110}}));
        options.obstacles={QRectF(35,40,10,20)};
        const auto rerouted=orthogonalRoute({0,10},{100,110},options);
        QVERIFY(rerouted!=preserved);
        QCOMPARE(routeProblem(rerouted,{0,10},{100,110},options),QString());
    }
    void smartRouteDoesNotReturnACrossingWhenAPortIsBlocked() {
        OrthogonalRouteOptions options;
        options.startDirection={1,0}; options.endDirection={-1,0};
        options.clearance=0; options.grid=10;
        options.obstacles={QRectF(1,-100,30,200)};
        QVERIFY(orthogonalRoute({0,0},{200,0},options).isEmpty());
        options.obstacles={QRectF(0,-20,40,40)};
        options.startDirection={-1,0};
        const auto escaped=orthogonalRoute({0,0},{-100,100},options);
        QCOMPARE(routeProblem(escaped,{0,0},{-100,100},options),QString());
    }
    void smartRouteReroutesWhenEndpointMovementFoldsAnInteriorLane() {
        OrthogonalRouteOptions options;
        options.startDirection={0,-1}; options.endDirection={-1,0};
        options.preferredRoute={{280,1020},{280,592},{264,592},{264,500},{280,500}};
        options.obstacles={QRectF(280,420,240,160),QRectF(880,820,240,160),
            QRectF(160,1020,240,160),QRectF(660,1170,240,160),QRectF(1030,1400,240,160)};
        QCOMPARE(routeProblem(options.preferredRoute,{280,1020},{280,500},options),QString());
        options.obstacles[0]=QRectF(380,570,240,160);
        const auto route=orthogonalRoute({280,1020},{380,650},options);
        QCOMPARE(routeProblem(route,{280,1020},{380,650},options),QString());
        QCOMPARE(route,Polyline({{280,1020},{280,650},{380,650}}));
        options.obstacles.append(QRectF(260,740,40,60));
        const auto detour=orthogonalRoute({280,1020},{380,650},options);
        QCOMPARE(routeProblem(detour,{280,1020},{380,650},options),QString());
    }
    void smartRouteRejectsSelfIntersectionsInPreferredLanes_data() {
        QTest::addColumn<Polyline>("preferred");
        QTest::newRow("crossing") << Polyline({{0,0},{80,0},{80,80},{40,80},{40,-40},{100,-40}});
        QTest::newRow("nonconsecutive-corner-contact") << Polyline({{0,0},{80,0},{80,80},{0,80},{0,0},{100,0}});
        QTest::newRow("horizontal-overlap") << Polyline({{0,0},{80,0},{80,80},{20,80},{20,0},{100,0}});
        QTest::newRow("vertical-overlap") << Polyline({{0,0},{0,80},{80,80},{80,20},{0,20},{0,100}});
    }
    void smartRouteRejectsSelfIntersectionsInPreferredLanes() {
        QFETCH(Polyline,preferred);
        OrthogonalRouteOptions options;
        options.preferredRoute=preferred;
        QVERIFY(!routeProblem(preferred,preferred.first(),preferred.last(),options).isEmpty());
        const auto route=orthogonalRoute(preferred.first(),preferred.last(),options);
        QCOMPARE(routeProblem(route,preferred.first(),preferred.last(),options),QString());
        QVERIFY(route!=preferred);
    }
    void smartRouteShortensAFreeLeadWhenTheFirstObstacleIsClose() {
        OrthogonalRouteOptions options;
        options.grid=0; options.clearance=0;
        options.startDirection={1,0}; options.endDirection={-1,0};
        options.obstacles={QRectF(3,-10,20,20)};
        const auto route=orthogonalRoute({0,0},{100,50},options);
        QCOMPARE(routeProblem(route,{0,0},{100,50},options),QString());
        QVERIFY(route[1].x()<=3);
    }
    void smartRouteRejectsInvalidGeometry() {
        OrthogonalRouteOptions options;
        const auto nan=std::numeric_limits<qreal>::quiet_NaN();
        QVERIFY(orthogonalRoute({nan,0},{10,10},options).isEmpty());
        options.grid=-1;
        QVERIFY(orthogonalRoute({0,0},{10,10},options).isEmpty());
        options.grid=8; options.clearance=nan;
        QVERIFY(orthogonalRoute({0,0},{10,10},options).isEmpty());
        options.clearance=1; options.obstacles={QRectF(0,nan,10,10)};
        QVERIFY(orthogonalRoute({0,0},{10,10},options).isEmpty());
        options.obstacles.clear();
        QCOMPARE(orthogonalRoute({2,3},{2,3},options),Polyline({{2,3}}));
    }
    void symbolPortDirectionUsesStableIdsAndQuarterTurns() {
        Symbol symbol; symbol.strokes={{{{0,0},{100,0},{100,40},{0,40},{0,0}},2,Qt::black}};
        symbol.anchors={{0,20},{100,20},{50,0},{50,40}};
        symbol.portIds={"left","right","top","bottom"};
        QCOMPARE(portExitDirection(symbol,"left"),QPointF(-1,0));
        QCOMPARE(portExitDirection(symbol,"right",1),QPointF(0,1));
        QCOMPARE(portExitDirection(symbol,"top",-1),QPointF(-1,0));
        QCOMPARE(portExitDirection(symbol,"bottom",4),QPointF(0,1));
        QVERIFY(portExitDirection(symbol,"missing").isNull());
    }
    void configurableTableHasRequestedCells() {
        const auto table=configuredStencil("table",{{"rows",3},{"columns",5}});
        QCOMPARE(table.id,QString("table"));
        QCOMPARE(table.strokes.size(),7); // Closed border + 2 row + 4 column separators.
        QCOMPARE(table.strokes.first().points.first(),table.strokes.first().points.last());
        QCOMPARE(table.strokes[1].points,QVector<QPointF>({{5,5+70.0/3},{115,5+70.0/3}}));
        QCOMPARE(table.strokes[3].points,QVector<QPointF>({{27,5},{27,75}}));
        QCOMPARE(configuredStencil("table",{{"rows",1},{"columns",1}}).strokes.size(),1);
        QCOMPARE(configuredStencil("table",{{"rows",30},{"columns",30}}).strokes.size(),59);
        QVERIFY(stencilDefaults("table")["opaqueBackground"].toBool());
        QCOMPARE(stencilDefaults("graph")["curveType"].toString(),QString("none"));
        QVERIFY(!stencilDefaults("bode")["curve"].toBool());
    }
    void configurableParametersRejectInvalidValuesAtomically() {
        const double nan=std::numeric_limits<double>::quiet_NaN();
        const double inf=std::numeric_limits<double>::infinity();
        const QVector<QPair<QString,QVariantMap>> cases={
            {"unknown",{}},{"table",{{"rows",0}}},{"table",{{"columns",31}}},
            {"table",{{"rows",1.5}}},{"table",{{"rows",true}}},{"table",{{"rows","many"}}},
            {"table",{{"opaqueBackground","false"}}},{"table",{{"unexpected",4}}},
            {"graph",{{"tau",0}}},{"graph",{{"gain",nan}}},{"graph",{{"omega0",inf}}},
            {"graph",{{"damping",-1}}},{"graph",{{"yMin",4},{"yMax",4}}},
            {"graph",{{"yMin",4},{"yMax",3}}},{"graph",{{"yMin",0},{"yMax",1e-300}}},
            {"graph",{{"curveType","parabola"}}},{"graph",{{"curveType","second-order"},
                {"omega0",100},{"tMax",100},{"damping",0}}},
            {"bode",{{"frequencyMin",0}}},{"bode",{{"frequencyMin",100},{"frequencyMax",1}}},
            {"bode",{{"frequencyMin",1e-6},{"frequencyMax",1e6}}},
            {"bode",{{"damping",0}}},{"bode",{{"gain",-1}}},{"bode",{{"mode","other"}}}};
        for(const auto &test:cases) {
            QVariantMap output{{"untouched",123}}; QString error;
            QVERIFY2(!normalizeStencilParameters(test.first,test.second,&output,&error),qPrintable(test.first));
            QCOMPARE(output,QVariantMap({{"untouched",123}}));
            QVERIFY(!error.isEmpty());
            QVERIFY(configuredStencil(test.first,test.second).strokes.isEmpty());
        }
        QVariantMap normalized; QString error="stale";
        QVERIFY(normalizeStencilParameters("table",{{"rows",7.0}},&normalized,&error));
        QCOMPARE(normalized["rows"].toInt(),7);
        QCOMPARE(normalized["columns"].toInt(),4);
        QVERIFY(error.isEmpty());
    }
    void exponentialHasInitialValueTimeConstantAndAsymptote() {
        const QVariantMap p={{"grid",false},{"curveType","exponential"},{"gain",3.0},
            {"initialValue",-1.0},{"tau",2.0},{"tMax",2.0},{"yMin",-2.0},{"yMax",4.0}};
        const auto oneTau=configuredStencil("graph",p);
        QVERIFY(!oneTau.strokes.isEmpty());
        const auto &curve=oneTau.strokes.last().points;
        QCOMPARE(curve.first().x(),5.0);
        QVERIFY(std::abs(curve.first().y()-(75-70.0/6))<1e-10);
        QVERIFY(std::abs(valueAtEnd(oneTau,-2,4)-(-1+4*(1-std::exp(-1.0))))<1e-10);
        QVariantMap late=p; late["tMax"]=40;
        QVERIFY(std::abs(valueAtEnd(configuredStencil("graph",late),-2,4)-3)<1e-7);
        QVariantMap falling=p; falling["gain"]=-1; falling["initialValue"]=3;
        const auto descending=configuredStencil("graph",falling).strokes.last().points;
        for(int i=1;i<descending.size();++i) QVERIFY(descending[i].y()>=descending[i-1].y()-1e-10);
    }
    void sineMatchesAmplitudeOffsetFrequencyAndPhase_data() {
        QTest::addColumn<double>("amplitude");QTest::addColumn<double>("offset");QTest::addColumn<double>("frequency");
        QTest::addColumn<double>("phase");QTest::addColumn<double>("duration");
        QTest::newRow("unit")<<1.<<0.<<1.<<0.<<1.;
        QTest::newRow("shifted")<<2.<<3.<<.75<<30.<<2.5;
        QTest::newRow("positive-quadrature")<<2.<<3.<<.75<<90.<<2.;
        QTest::newRow("negative-quadrature")<<2.<<-3.<<2.<<-90.<<1.;
        QTest::newRow("full-phase")<<1.<<0.<<1.<<360.<<1.;
        QTest::newRow("zero-amplitude")<<0.<<3.<<1.<<37.<<1.;
        QTest::newRow("slow")<<1.<<0.<<1e-6<<0.<<1e6;
        QTest::newRow("fast")<<1.<<0.<<1e6<<0.<<1e-6;
        QTest::newRow("maximum-periods")<<1.<<0.<<32.<<-360.<<1.;
    }
    void sineMatchesAmplitudeOffsetFrequencyAndPhase() {
        QFETCH(double,amplitude);QFETCH(double,offset);QFETCH(double,frequency);QFETCH(double,phase);QFETCH(double,duration);
        const double minimum=offset-amplitude-1,maximum=offset+amplitude+1;
        const auto symbol=configuredStencil("graph",{{"grid",false},{"curveType","sine"},{"amplitude",amplitude},{"offset",offset},
            {"frequencyHz",frequency},{"phaseDegrees",phase},{"tMax",duration},{"yMin",minimum},{"yMax",maximum}});
        QCOMPARE(symbol.strokes.size(),5);const auto &curve=symbol.strokes.last();
        QCOMPARE(curve.width,2.3);QCOMPARE(curve.color,QColor(Qt::black));
        QCOMPARE(curve.points.first().x(),5.);QCOMPARE(curve.points.last().x(),115.);
        for(auto point:curve.points) {
            const double time=(point.x()-5)*duration/110;
            const double actual=maximum-(point.y()-5)*(maximum-minimum)/70;
            const double expected=offset+amplitude*std::sin(2*Pi*frequency*time+phase*Pi/180);
            QVERIFY(std::abs(actual-expected)<1e-10);
        }
    }
    void sineKeepsExactExtremaBetweenUniformSamples() {
        const auto symbol=configuredStencil("graph",{{"grid",false},{"curveType","sine"},{"amplitude",2.},{"offset",3.},
            {"frequencyHz",.7},{"phaseDegrees",0.},{"tMax",2.3},{"yMin",0.},{"yMax",6.}});
        QCOMPARE(symbol.strokes.size(),5);const auto &points=symbol.strokes.last().points;
        for(double phase:{.25,.75,1.25}) {
            const double x=5+(phase/.7)/2.3*110;
            const auto found=std::find_if(points.cbegin(),points.cend(),[&](QPointF point){return std::abs(point.x()-x)<1e-10;});
            QVERIFY(found!=points.cend());const double expected=phase==.75?1:5;
            QVERIFY(std::abs(6-(found->y()-5)*6/70-expected)<1e-10);
        }
    }
    void sineParametersAreStrictAndLimitedToThirtyTwoPeriods() {
        const QVariantMap base={{"curveType","sine"},{"frequencyHz",1.},{"tMax",1.}};
        const QVector<QPair<QString,QVariant>> invalid={{"amplitude",-1.},{"amplitude",1e6+1},{"amplitude",true},
            {"amplitude",QString("1")},{"offset",1e6+1},{"offset",std::numeric_limits<double>::infinity()},
            {"frequencyHz",0.},{"frequencyHz",1e6+1},{"frequencyHz",std::numeric_limits<double>::quiet_NaN()},
            {"phaseDegrees",-360.1},{"phaseDegrees",360.1},{"phaseDegrees",QVariant()},{"frequencyHz",32.000001}};
        for(const auto &entry:invalid) {
            auto parameters=base;parameters[entry.first]=entry.second;QVariantMap output{{"untouched",true}};QString error;
            QVERIFY(!normalizeStencilParameters("graph",parameters,&output,&error));QVERIFY(!error.isEmpty());
            QCOMPARE(output,QVariantMap({{"untouched",true}}));QVERIFY(configuredStencil("graph",parameters).strokes.isEmpty());
        }
        auto boundary=base;boundary["frequencyHz"]=32.;boundary["amplitude"]=1e6;boundary["offset"]=-1e6;boundary["phaseDegrees"]=-360.;
        QVERIFY(normalizeStencilParameters("graph",boundary,nullptr));
        QCOMPARE(stencilDefaults("graph")["amplitude"].toDouble(),1.);QCOMPARE(stencilDefaults("graph")["offset"].toDouble(),0.);
        QCOMPARE(stencilDefaults("graph")["frequencyHz"].toDouble(),1.);QCOMPARE(stencilDefaults("graph")["phaseDegrees"].toDouble(),0.);
    }
    void sineClippingKeepsSeparateCrossingsWithoutBorderBridges() {
        const auto symbol=configuredStencil("graph",{{"grid",false},{"curveType","sine"},{"amplitude",2.},{"offset",0.},
            {"frequencyHz",1.},{"phaseDegrees",90.},{"tMax",2.},{"yMin",-.5},{"yMax",.5}});
        QCOMPARE(symbol.strokes.size(),8);
        for(int i=4;i<symbol.strokes.size();++i) {
            const auto &points=symbol.strokes[i].points;QVERIFY(points.size()>2);
            QVERIFY(std::abs(points.first().y()-points.last().y())>69.9);
            for(auto point:points)QVERIFY(QRectF(5,5,110,70).contains(point));
            for(int j=1;j<points.size();++j)
                QVERIFY(!((points[j].y()==5&&points[j-1].y()==5)||(points[j].y()==75&&points[j-1].y()==75)));
        }
    }
    void sineSettingsLeaveLegacyCurvesAndOtherStencilSchemasUnchanged() {
        for(const auto &curve:{"none","exponential","second-order"}) {
            const QVariantMap legacy={{"grid",false},{"curveType",curve},{"gain",1.5},{"tau",2.},{"initialValue",.2},
                {"omega0",2.},{"damping",.7},{"tMax",3.},{"yMin",-.2},{"yMax",2.}};
            auto added=legacy;added["amplitude"]=100.;added["offset"]=-100.;added["frequencyHz"]=1e6;added["phaseDegrees"]=90.;
            const auto before=configuredStencil("graph",legacy),after=configuredStencil("graph",added);
            QVERIFY(!before.strokes.isEmpty());QCOMPARE(before.strokes.size(),after.strokes.size());
            for(int i=0;i<before.strokes.size();++i) {
                QCOMPARE(before.strokes[i].points,after.strokes[i].points);QCOMPARE(before.strokes[i].width,after.strokes[i].width);
                QCOMPARE(before.strokes[i].color,after.strokes[i].color);
            }
        }
        QCOMPARE(stencilDefaults("graph")["curveType"].toString(),QString("none"));
        QCOMPARE(stencilDefaults("graph")["yMin"].toDouble(),-.2);QCOMPARE(stencilDefaults("graph")["yMax"].toDouble(),1.4);
        for(const auto &id:{"table","bode"})for(const auto &key:{"amplitude","offset","frequencyHz","phaseDegrees"})
            QVERIFY(!stencilDefaults(id).contains(key));
    }
    void sineExportsKeepCurveGeometryWidthAndForegroundColor() {
        QTemporaryDir directory;QString error;
        const auto symbol=configuredStencil("graph",{{"grid",false},{"curveType","sine"},{"tMax",1.},{"yMin",-1.2},{"yMax",1.2}});
        auto strokes=transformStrokes(symbol.strokes,{100,200},2);QCOMPARE(strokes.size(),5);
        QVERIFY(exportScene(directory.path()+"/native.json",strokes,"Sinusoïde",&error));
        QFile nativeFile(directory.path()+"/native.json");QVERIFY(nativeFile.open(QIODevice::ReadOnly));
        const auto native=QJsonDocument::fromJson(nativeFile.readAll()).object();QVERIFY(paper::validateScene(native,&error));
        QVERIFY(!paper::writeScene(native).isEmpty());
        for(auto &stroke:strokes)stroke.color=QColor("#16877d");
        QVERIFY(exportScene(directory.path()+"/color.json",strokes,"Sinusoïde",&error));
        QFile sceneFile(directory.path()+"/color.json");QVERIFY(sceneFile.open(QIODevice::ReadOnly));
        const auto saved=QJsonDocument::fromJson(sceneFile.readAll()).object()["strokes"].toArray();
        QCOMPARE(saved.size(),strokes.size());
        for(int i=0;i<saved.size();++i) {
            QCOMPARE(saved[i].toObject()["color"].toString(),QString("#16877d"));
            QCOMPARE(saved[i].toObject()["width"].toDouble(),strokes[i].width);
            QCOMPARE(saved[i].toObject()["points"].toArray().size(),strokes[i].points.size());
        }
        QVERIFY(exportSvg(directory.path()+"/sine.svg",strokes,&error));
        QFile svg(directory.path()+"/sine.svg");QVERIFY(svg.open(QIODevice::ReadOnly));QCOMPARE(svg.readAll().count("stroke=\"#16877d\""),strokes.size());
        QVERIFY(exportPdf(directory.path()+"/sine.pdf",strokes,"Sinusoïde",&error));
        QFile pdf(directory.path()+"/sine.pdf");QVERIFY(pdf.open(QIODevice::ReadOnly));QCOMPARE(pdf.read(5),QByteArray("%PDF-"));
    }
    void secondOrderAgreesWithDifferentialEquationInEveryRegime() {
        for(double damping:{0.0,0.5,1.0-1e-8,1.0,1.0+1e-8,2.0,100.0}) {
            for(double time:{0.001,0.1,1.0,10.0}) {
                const auto symbol=configuredStencil("graph",{{"grid",false},{"curveType","second-order"},
                    {"gain",1.0},{"omega0",1.0},{"damping",damping},{"tMax",time},
                    {"yMin",-0.5},{"yMax",2.5}});
                QVERIFY(!symbol.strokes.isEmpty());
                const double expected=integratedUnitStep(time,damping);
                const double actual=valueAtEnd(symbol,-0.5,2.5);
                QVERIFY2(std::abs(actual-expected)<2e-8,
                    qPrintable(QString("zeta=%1 t=%2 actual=%3 expected=%4")
                        .arg(damping).arg(time).arg(actual,0,'g',16).arg(expected,0,'g',16)));
            }
        }
    }
    void secondOrderFrequencyAndGainScaleTheSameResponse() {
        const auto slow=configuredStencil("graph",{{"curveType","second-order"},{"omega0",1.0},
            {"gain",1.0},{"tMax",10.0},{"yMin",-0.5},{"yMax",2.5}});
        const auto fast=configuredStencil("graph",{{"curveType","second-order"},{"omega0",10.0},
            {"gain",3.0},{"tMax",1.0},{"yMin",-1.5},{"yMax",7.5}});
        const auto &a=slow.strokes.last().points, &b=fast.strokes.last().points;
        QCOMPARE(a.size(),b.size());
        for(int i=0;i<a.size();++i) QVERIFY(QLineF(a[i],b[i]).length()<1e-9);
    }
    void bodeGridUsesLogarithmicSubdivisionsAndEqualDecades() {
        const auto symbol=configuredStencil("bode",{{"frequencyMin",1.0},{"frequencyMax",100.0}});
        QVector<qreal> vertical;
        for(const auto &stroke:symbol.strokes) {
            if(stroke.points.size()==2 && stroke.points[0].x()==stroke.points[1].x())
                vertical.append(stroke.points[0].x());
        }
        QCOMPARE(vertical.size(),17);
        QVERIFY(std::abs(vertical[0]-(5+55*std::log10(2.0)))<1e-10);
        QVERIFY(std::abs(vertical[1]-(5+55*std::log10(3.0)))<1e-10);
        QCOMPARE(vertical[8],60.0); // 10 Hz lies at the exact middle of 1…100 Hz.
        QVERIFY(std::abs((vertical[9]-vertical[0])-55)<1e-10);
        QVERIFY(vertical[1]-vertical[0] < vertical[0]-5); // Not linear 1, 2, 3 spacing.
    }
    void bodeAtNaturalFrequencyHasCorrectGainAndPhaseUnits() {
        QVariantMap p={{"curve",true},{"frequencyMin",0.1},{"frequencyMax",1.0},
            {"omega0",2*Pi},{"damping",0.5},{"gain",2.0},{"yMin",-100.0},{"yMax",100.0}};
        const auto magnitude=configuredStencil("bode",p);
        QVERIFY(std::abs(valueAtEnd(magnitude,-100,100)-20*std::log10(2.0))<1e-9);
        p["mode"]="phase"; p["yMin"]=-180; p["yMax"]=0;
        const auto phase=configuredStencil("bode",p);
        QVERIFY(std::abs(valueAtEnd(phase,-180,0)+90)<1e-9);
        p["frequencyMax"]=1e4;
        QVERIFY(std::abs(valueAtEnd(configuredStencil("bode",p),-180,0)+180)<0.01);
    }
    void clippingPreservesEntryExitWithoutBorderBridges() {
        const auto symbol=configuredStencil("graph",{{"grid",false},{"curveType","second-order"},
            {"damping",0.0},{"omega0",1.0},{"tMax",4*Pi},{"yMin",0.8},{"yMax",1.2}});
        QCOMPARE(symbol.strokes.size(),8); // Four axis strokes + four separate visible crossings.
        for(int i=4;i<symbol.strokes.size();++i) {
            const auto &curve=symbol.strokes[i].points;
            QVERIFY(curve.size()>2);
            QVERIFY(std::abs(curve.first().y()-5)<1e-8 || std::abs(curve.first().y()-75)<1e-8);
            QVERIFY(std::abs(curve.last().y()-5)<1e-8 || std::abs(curve.last().y()-75)<1e-8);
            QVERIFY(std::abs(curve.first().y()-curve.last().y())>69.9);
            for(const auto &point:curve) QVERIFY(QRectF(5,5,110,70).contains(point));
        }
    }
    void largestStencilsRemainFiniteAndSmallEnoughForNativeInk() {
        const QVector<Symbol> symbols={
            configuredStencil("table",{{"rows",30},{"columns",30}}),
            configuredStencil("bode",{{"frequencyMin",1e-4},{"frequencyMax",1e4},{"curve",true},
                {"damping",1e-6},{"omega0",2*Pi},{"yMin",-10},{"yMax",10}}),
            configuredStencil("graph",{{"curveType","second-order"},{"damping",0.0},
                {"tMax",64*Pi},{"yMin",0.8},{"yMax",1.2}}),
            configuredStencil("graph",{{"curveType","second-order"},{"damping",100.0},
                {"omega0",1e6},{"tMax",1e6}}),
            configuredStencil("graph",{{"curveType","exponential"},{"tau",1e-6},{"tMax",1e6}}),
            configuredStencil("graph",{{"curveType","sine"},{"frequencyHz",32.},{"tMax",1.},{"yMin",-.2},{"yMax",.2}})};
        for(const auto &symbol:symbols) {
            QVERIFY(!symbol.strokes.isEmpty());
            QVERIFY(symbol.strokes.size()<100);
            QCOMPARE(symbol.strokes.first().width,2.3);
            int points=0;
            for(const auto &stroke:symbol.strokes) {
                points+=stroke.points.size();
                for(const auto &point:stroke.points) {
                    QVERIFY(std::isfinite(point.x()) && std::isfinite(point.y()));
                    QVERIFY(QRectF(0,0,120,80).contains(point));
                }
            }
            QVERIFY(points<2000);
        }
    }
    void parametricShapesStayClosedAndBounded() {
        const QRectF rect(10,20,240,80);
        QCOMPARE(roundedRectangle(rect,0), Polyline({{10,20},{250,20},{250,100},{10,100},{10,20}}));
        QCOMPARE(roundedRectangle(rect,-3), roundedRectangle(rect,0));
        QCOMPARE(roundedRectangle(rect,400), roundedRectangle(rect,40));
        QCOMPARE(roundedRectangle(QRectF(250,100,-240,-80),12), roundedRectangle(rect,12));
        QCOMPARE(ellipse(QRectF(250,100,-240,-80)), ellipse(rect));
        for (const auto &shape: {roundedRectangle(rect,12), ellipse(rect)}) {
            QCOMPARE(shape.first(),shape.last());
            for (const auto &point:shape) {
                QVERIFY(std::isfinite(point.x()) && std::isfinite(point.y()));
                QVERIFY(rect.contains(point));
            }
        }
        const auto oval=ellipse(rect);
        QCOMPARE(oval.size(),97);
        QCOMPARE(oval[0],QPointF(250,60));
        QCOMPARE(oval[24],QPointF(130,100));
        QCOMPARE(oval[48],QPointF(10,60));
        QCOMPARE(oval[72],QPointF(130,20));
        for(const auto &point:oval) {
            const qreal x=(point.x()-130)/120, y=(point.y()-60)/40;
            QVERIFY(std::abs(x*x+y*y-1)<1e-10);
        }
    }
    void invalidShapeParametersProduceNoContour() {
        const qreal inf=std::numeric_limits<qreal>::infinity();
        const qreal nan=std::numeric_limits<qreal>::quiet_NaN();
        for (const QRectF &rect: {QRectF(),QRectF(1,2,0,4),QRectF(inf,0,10,10),QRectF(0,0,nan,10)}) {
            QVERIFY(roundedRectangle(rect,4).isEmpty());
            QVERIFY(ellipse(rect).isEmpty());
        }
        QVERIFY(roundedRectangle(QRectF(0,0,10,10),nan).isEmpty());
        QVERIFY(roundedRectangle(QRectF(0,0,10,10),inf).isEmpty());
    }
    void samplingDensityDoesNotChangeDashes() {
        auto sparse=dashByArcLength({{0,0},{100,0}},{12,8});
        Polyline dense;for(int x=0;x<=100;++x)dense.append({qreal(x),0});
        auto sampled=dashByArcLength(dense,{12,8});
        QCOMPARE(sparse.size(),5);QCOMPARE(sampled.size(),sparse.size());
        for(int i=0;i<sparse.size();++i) {
            QVERIFY(QLineF(sparse[i].first(),sampled[i].first()).length()<1e-8);
            QVERIFY(QLineF(sparse[i].last(),sampled[i].last()).length()<1e-8);
        }
    }
    void dashesCrossCornersWithoutReset() {
        auto pieces=dashByArcLength({{0,0},{10,0},{10,20}},{16,4});
        QCOMPARE(pieces.size(),2);
        QCOMPARE(pieces[0].last(),QPointF(10,6));
        QCOMPARE(pieces[1].first(),QPointF(10,10));
        QCOMPARE(pieces[1].last(),QPointF(10,20));
    }
    void phaseAndInvalidPatterns() {
        auto pieces=dashByArcLength({{0,0},{20,0}},{4,2},5);
        QCOMPARE(pieces.first().first(),QPointF(1,0));
        QCOMPARE(pieces.first().last(),QPointF(5,0));
        QVERIFY(dashByArcLength({{0,0},{20,0}},{0,2}).isEmpty());
        QVERIFY(dashByArcLength({{0,0},{20,0}},{-1,2}).isEmpty());
        QCOMPARE(dashByArcLength({{1,1},{1,1},{10,1}},{3,2}).size(),2);
    }
    void routeAndSnapping() {
        auto route=orthogonalRoute({2,3},{10,20},true);
        QCOMPARE(route,Polyline({{2,3},{10,3},{10,20}}));
        QCOMPARE(orthogonalRoute({2,3},{2,20},true).size(),2);
        QCOMPARE(snapPoint({21,22},{{19,20},{40,40}},5,20),QPointF(19,20));
        QCOMPARE(snapPoint({29,31},{},5,20),QPointF(20,40));
        QCOMPARE(snapPoint({29,31},{},5,0),QPointF(29,31));
    }
    void catalogueHasUsableGeometry() {
        const auto catalogue=electronicsCatalogue();QVERIFY(catalogue.size()>=20);
        QSet<QString> ids;
        for(const auto &s:catalogue) {
            QVERIFY(!ids.contains(s.id));ids.insert(s.id);
            QVERIFY(!s.strokes.isEmpty());
            if(s.id=="square-root")QVERIFY(s.anchors.isEmpty());else QVERIFY(!s.anchors.isEmpty());
            QCOMPARE(s.portIds.size(),s.anchors.size());
            QSet<QString> ports;
            for(const auto &port:s.portIds) { QVERIFY(!port.isEmpty()); QVERIFY(!ports.contains(port)); ports.insert(port); }
            QVERIFY(bounds(s.strokes).width()>0);
            for(const auto &stroke:s.strokes)for(auto p:stroke.points) {
                QVERIFY(std::isfinite(p.x()));QVERIFY(std::isfinite(p.y()));
                QVERIFY(p.x()>=-0.001&&p.x()<=120.001);QVERIFY(p.y()>=-0.001&&p.y()<=80.001);
            }
        }
        const auto second=electronicsCatalogue();
        for(int i=0;i<catalogue.size();++i) QCOMPARE(catalogue[i].portIds,second[i].portIds);
        auto portsFor=[&](const QString &id) { for(const auto &s:catalogue)if(s.id==id)return s.portIds; return QStringList{}; };
        QCOMPARE(portsFor("npn"),QStringList({"base","collector","emitter"}));
        QCOMPARE(portsFor("diode"),QStringList({"anode","cathode"}));
        QCOMPARE(portsFor("opamp"),QStringList({"inverting","non-inverting","output"}));
    }
    void nativeSceneRoundTripAndExports() {
        QTemporaryDir directory;QVERIFY(directory.isValid());
        const auto strokes=transformStrokes(electronicsCatalogue()[0].strokes,{100,200},2);
        QString error;
        QVERIFY2(exportScene(directory.path()+"/symbol.json",strokes,"Résistance",&error),qPrintable(error));
        QFile file(directory.path()+"/symbol.json");QVERIFY(file.open(QIODevice::ReadOnly));
        auto json=QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(json["schemaVersion"].toInt(),1);
        QCOMPARE(json["strokes"].toArray().size(),strokes.size());
        QCOMPARE(json["strokes"].toArray()[0].toObject()["points"].toArray()[0].toArray(),QJsonArray({100,280}));
        QVERIFY2(paper::validateScene(json,&error),qPrintable(error));
        const auto native=paper::writeScene(json);
        QVERIFY(native.startsWith("reMarkable .lines file, version=6"));
        QVERIFY(native.size()>43);
        QVERIFY(exportSvg(directory.path()+"/symbol.svg",strokes,&error));
        QVERIFY(exportPdf(directory.path()+"/symbol.pdf",strokes,"Résistance",&error));
        QFile pdf(directory.path()+"/symbol.pdf");QVERIFY(pdf.open(QIODevice::ReadOnly));QCOMPARE(pdf.read(5),QByteArray("%PDF-"));
    }
    void everySymbolAndPenTapPassNativeWriter() {
        QTemporaryDir directory;QString error;
        for(const auto &symbol:electronicsCatalogue()) {
            const QString path=directory.path()+"/"+symbol.id+".json";
            QVERIFY(exportScene(path,transformStrokes(symbol.strokes,{500,700},2),symbol.name,&error));
            QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));
            const auto json=QJsonDocument::fromJson(file.readAll()).object();
            QVERIFY2(paper::validateScene(json,&error),qPrintable(symbol.id+": "+error));
            QVERIFY(!paper::writeScene(json).isEmpty());
        }
        const QString tapPath=directory.path()+"/tap.json";
        QVERIFY(exportScene(tapPath,{{{{1404,100}},3,Qt::black}},"Tap",&error));
        QFile tapFile(tapPath);QVERIFY(tapFile.open(QIODevice::ReadOnly));
        QVERIFY2(paper::validateScene(QJsonDocument::fromJson(tapFile.readAll()).object(),&error),qPrintable(error));
    }
};
QTEST_MAIN(GeometryTest)
#include "GeometryTest.moc"

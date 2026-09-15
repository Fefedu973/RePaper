#include "InkCanvas.h"
#include "BridgeClient.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QPainter>
#include <QPolygonF>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTransform>
#include <algorithm>
#include <cmath>

using namespace PaperDrawing;
namespace Model = repaper::drawing;
namespace {
QString documentPath() {
    const QString dir=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);return dir+"/drawing.json";
}
QJsonArray encodePoints(const QVector<QPointF> &points) {
    QJsonArray result;for(auto p:points)result.append(QJsonArray{p.x(),p.y()});return result;
}
bool decodePoints(const QJsonArray &array, QVector<QPointF> *out, int *budget) {
    if(array.size()>*budget)return false;
    *budget-=array.size();
    for(auto value:array) {
        auto pair=value.toArray();
        if(pair.size()!=2 || !pair[0].isDouble() || !pair[1].isDouble())return false;
        qreal x=pair[0].toDouble(),y=pair[1].toDouble();
        if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>20000||std::abs(y)>20000)return false;
        out->append({x,y});
    }
    return true;
}
bool sameStrokes(const QVector<Stroke> &left,const QVector<Stroke> &right) {
    if(left.size()!=right.size())return false;
    for(int i=0;i<left.size();++i) {
        if(left[i].points.size()!=right[i].points.size()||std::abs(left[i].width-right[i].width)>1e-6||left[i].color!=right[i].color)return false;
        for(int p=0;p<left[i].points.size();++p)if(QLineF(left[i].points[p],right[i].points[p]).length()>1e-5)return false;
    }
    return true;
}
bool insidePage(const QVector<Stroke> &strokes) {
    const auto box=bounds(strokes);
    return box.left()>=0&&box.top()>=0&&box.right()<=PageWidth&&box.bottom()<=PageHeight;
}
QRectF stencilCenterRange(const Model::Item &item,QPointF center) {
    auto box=bounds(item.strokes);
    for(const auto &point:Model::boxCorners(item)) {
        box.setLeft(std::min(box.left(),point.x()));box.setRight(std::max(box.right(),point.x()));
        box.setTop(std::min(box.top(),point.y()));box.setBottom(std::max(box.bottom(),point.y()));
    }
    return {QPointF(center.x()-box.left()+10,center.y()-box.top()+10),
            QPointF(PageWidth-(box.right()-center.x())-10,PageHeight-(box.bottom()-center.y())-10)};
}
QPointF clampStencilCenter(QPointF point,const QRectF &range) {
    return {std::clamp(point.x(),range.left(),range.right()),std::clamp(point.y(),range.top(),range.bottom())};
}
bool hitItem(const Model::Item &item,QPointF point,qreal radius) {
    if(!bounds(item.strokes).adjusted(-radius,-radius,radius,radius).contains(point))return false;
    if(Model::hasBox(item)&&QPolygonF(Model::boxCorners(item)).containsPoint(point,Qt::OddEvenFill))return true;
    for(const auto &stroke:item.strokes) {
        const qreal limit=radius+stroke.width/2;
        if(stroke.points.size()==1&&QLineF(point,stroke.points.first()).length()<=limit)return true;
        for(int i=1;i<stroke.points.size();++i) {
            const auto a=stroke.points[i-1],delta=stroke.points[i]-a;
            const qreal lengthSquared=QPointF::dotProduct(delta,delta);
            const qreal fraction=lengthSquared>1e-12?std::clamp(QPointF::dotProduct(point-a,delta)/lengthSquared,qreal(0),qreal(1)):0;
            if(QLineF(point,a+delta*fraction).length()<=limit)return true;
        }
    }
    return false;
}
}

InkCanvas::InkCanvas(QQuickItem *parent):QQuickPaintedItem(parent),m_bridge(new repaper::BridgeClient(this)) {
    setAntialiasing(true);load();
    connect(m_bridge,&repaper::BridgeClient::imported,this,[this](const QString &id){
        setStatus("Dessin importé dans la bibliothèque native. Identifiant : "+id);
    });
    connect(m_bridge,&repaper::BridgeClient::failed,this,[this](const QString &,const QString &message){setStatus(message);});
    connect(m_bridge,&repaper::BridgeClient::changed,this,[this]{if(m_bridge->busy())setStatus(m_bridge->message());});
}
void InkCanvas::setStatus(const QString &status){m_status=status;emit statusChanged();}
QVariantList InkCanvas::symbols() const {
    QVariantList out;
    for(const auto &symbol:electronicsCatalogue()) {
        QVariantList strokes;
        for(const auto &stroke:symbol.strokes) {
            QVariantList points;for(auto point:stroke.points)points.append(QVariantMap{{"x",point.x()},{"y",point.y()}});
            strokes.append(QVariantMap{{"points",points},{"width",stroke.width},{"color",stroke.color.name()}});
        }
        out.append(QVariantMap{{"id",symbol.id},{"name",symbol.name},{"strokes",strokes},{"width",120},{"height",80}});
    }
    return out;
}
void InkCanvas::setTool(const QString &value) {
    if(QStringList{"pen","line","wire","arrow","rectangle","ellipse","symbol","select"}.contains(value)&&m_tool!=value){cancel();m_tool=value;emit settingsChanged();}
}
void InkCanvas::setLineStyle(const QString &value){if(QStringList{"solid","dashed","dotted"}.contains(value)&&value!=m_style){m_style=value;emit settingsChanged();update();}}
void InkCanvas::setLineWidth(qreal value){if(!std::isfinite(value))return;value=std::clamp(value,qreal(1),qreal(12));if(value!=m_width){m_width=value;emit settingsChanged();}}
bool InkCanvas::setLineColor(const QString &value){
    const QColor color(value);if(!color.isValid()||color.alpha()!=255)return false;
    if(color!=m_color){m_color=color;emit settingsChanged();update();}return true;
}
QString InkCanvas::selectedLineColor() const {
    if(!hasSelection()||m_items[m_selection].strokes.isEmpty())return {};
    const auto color=Model::foregroundColor(m_items[m_selection]);
    return color.isValid()?color.name():QString();
}
bool InkCanvas::setSelectionLineColor(const QString &value){
    const QColor color(value);if(!hasSelection()||!color.isValid()||color.alpha()!=255)return false;
    cancel();auto item=m_items[m_selection];Model::setForegroundColor(item,color);
    return applySelectionItem(item);
}
void InkCanvas::setSnapping(bool value){if(value!=m_snapping){m_snapping=value;if(!value)setAlignmentGuides({});emit settingsChanged();}}
void InkCanvas::setGridVisible(bool value){if(value!=m_grid){m_grid=value;emit settingsChanged();update();}}
void InkCanvas::setHorizontalFirst(bool value){if(value!=m_horizontalFirst){m_horizontalFirst=value;emit settingsChanged();update();}}
void InkCanvas::setSymbolId(const QString &value){
    QVariantMap parameters;
    if(isConfigurableStencil(value)) {
        if(!normalizeStencilParameters(value,{},&parameters))return;
    } else {
        bool found=false;for(const auto &s:electronicsCatalogue())if(s.id==value){found=true;break;}
        if(!found)return;
    }
    m_symbolId=value;m_stencilParameters=parameters;emit settingsChanged();
}
qreal InkCanvas::pageScale() const{return std::max(qreal(0.001),std::min(width()/PageWidth,height()/PageHeight));}
QPointF InkCanvas::pageOffset() const{return {(width()-PageWidth*pageScale())/2,(height()-PageHeight*pageScale())/2};}
QPointF InkCanvas::toPage(qreal x,qreal y) const {
    QPointF p=(QPointF(x,y)-pageOffset())/pageScale();
    return {std::clamp(p.x(),qreal(0),PageWidth),std::clamp(p.y(),qreal(0),PageHeight)};
}
QVector<QPointF> InkCanvas::anchors(int excludedItem) const {QVector<QPointF> result;for(int i=0;i<m_items.size();++i)if(i!=excludedItem)result+=m_items[i].anchors;return result;}
QVector<Stroke> InkCanvas::allStrokes() const {QVector<Stroke> result;for(const auto &i:m_items)result+=i.strokes;return result;}
bool InkCanvas::hasEditableEndpoints(const DrawingItem &item){return Model::hasEndpoints(item);}
bool InkCanvas::hasSourcePath(const DrawingItem &item){return Model::hasPath(item);}
bool InkCanvas::selectionHasEndpoints() const{return hasSelection()&&hasEditableEndpoints(m_items[m_selection]);}
bool InkCanvas::selectionCanChangeStyle() const{return hasSelection()&&m_items[m_selection].kind!="symbol"&&hasSourcePath(m_items[m_selection]);}
bool InkCanvas::selectionIsArrow() const{return hasSelection()&&m_items[m_selection].kind=="arrow";}
bool InkCanvas::selectionIsWire() const{return hasSelection()&&m_items[m_selection].kind=="wire";}
qreal InkCanvas::selectedLineWidth() const{
    if(hasSelection()){
        const auto &item=m_items[m_selection];
        if(item.kind=="symbol"&&isConfigurableStencil(item.symbolId))return item.width;
        for(int i=0;i<item.strokes.size();++i)if(!Model::isBackgroundStroke(item,i))return item.strokes[i].width;
    }
    return 0;
}
QString InkCanvas::selectedLineStyle() const{return selectionCanChangeStyle()?m_items[m_selection].style:QString();}
QString InkCanvas::selectedArrowDirection() const{return selectionIsArrow()?m_items[m_selection].arrowDirection:QString();}
bool InkCanvas::selectedHorizontalFirst() const{return selectionIsWire()?m_items[m_selection].horizontalFirst:true;}
bool InkCanvas::rebuildItem(DrawingItem &item) { return Model::rebuild(item); }
QString InkCanvas::selectedObjectId() const{return hasSelection()?m_items[m_selection].id:QString();}
QString InkCanvas::selectionKind() const{return hasSelection()?m_items[m_selection].kind:QString();}
bool InkCanvas::selectionCanResize() const{return hasSelection()&&Model::hasBox(m_items[m_selection]);}
qreal InkCanvas::selectedShapeWidth() const{return selectionCanResize()?Model::boxWidth(m_items[m_selection]):0;}
qreal InkCanvas::selectedShapeHeight() const{return selectionCanResize()?Model::boxHeight(m_items[m_selection]):0;}
qreal InkCanvas::selectedCornerRadius() const{return selectionKind()=="rectangle"?m_items[m_selection].cornerRadius:0;}
qreal InkCanvas::selectedWireBend() const{return selectionIsWire()?m_items[m_selection].wireBend:0;}
QVariantList InkCanvas::selectedWireRoute() const {
    QVariantList result;if(!selectionIsWire())return result;
    for(auto point:Model::wirePoints(m_items[m_selection]))result.append(QVariantMap{{"x",point.x()},{"y",point.y()}});
    return result;
}
bool InkCanvas::selectedWireAutomatic() const {
    if(!selectionIsWire())return false;
    const auto &item=m_items[m_selection];return item.wireRouteMode=="auto"&&!item.wireHasBend&&std::abs(item.wireBend)<1e-9;
}
QVariantList InkCanvas::selectedPorts() const {
    QVariantList result;if(!hasSelection())return result;const auto &item=m_items[m_selection];
    for(int i=0;i<item.anchors.size()&&i<item.portIds.size();++i)result.append(QVariantMap{{"id",item.portIds[i]},{"x",item.anchors[i].x()},{"y",item.anchors[i].y()}});
    return result;
}
QVariantList InkCanvas::selectionHandlePoints() const {
    QVariantList result;if(!hasSelection())return result;
    const auto points=selectionHasEndpoints()?m_items[m_selection].sourcePoints:Model::boxCorners(m_items[m_selection]);
    for(int i=0;i<points.size();++i)result.append(QVariantMap{{"index",i},{"kind",selectionHasEndpoints()?"endpoint":"resize"},{"x",points[i].x()},{"y",points[i].y()}});
    if(selectionIsWire()) {
        const auto route=Model::wirePoints(m_items[m_selection]);
        for(int i=1;i+2<route.size();++i)if(QLineF(route[i],route[i+1]).length()>1e-7) {
            const auto center=(route[i]+route[i+1])/2;
            result.append(QVariantMap{{"index",i},{"kind","segment"},{"x",center.x()},{"y",center.y()}});
        }
    }
    return result;
}
bool InkCanvas::selectObject(const QString &id) {
    cancel();for(int i=0;i<m_items.size();++i)if(m_items[i].id==id){m_selection=i;m_tool="select";emit settingsChanged();emit documentChanged();update();return true;}return false;
}
bool InkCanvas::resizeSelection(qreal w,qreal h) {
    if(!selectionCanResize()||!std::isfinite(w)||!std::isfinite(h)||w<2||h<2||w>PageWidth||h>PageHeight)return false;
    cancel();auto item=m_items[m_selection];const auto center=Model::mapBox(item,0.5,0.5);
    const auto ax=(item.sourcePoints[1]-item.sourcePoints[0])/Model::boxWidth(item),ay=(item.sourcePoints[2]-item.sourcePoints[0])/Model::boxHeight(item);
    const auto origin=center-ax*w/2-ay*h/2;item.sourcePoints={origin,origin+ax*w,origin+ay*h};
    return rebuildItem(item)&&applySelectionItem(item);
}
bool InkCanvas::setSelectionCornerRadius(qreal radius) {
    if(selectionKind()!="rectangle"||!std::isfinite(radius)||radius<0)return false;
    cancel();auto item=m_items[m_selection];item.cornerRadius=std::min(radius,std::min(Model::boxWidth(item),Model::boxHeight(item))/2);
    return rebuildItem(item)&&applySelectionItem(item);
}
bool InkCanvas::setSelectionWireBend(qreal offset) {
    if(!selectionIsWire()||!std::isfinite(offset)||std::abs(offset)>2000)return false;
    cancel();auto item=m_items[m_selection];item.wireBend=offset;item.wireHasBend=std::abs(offset)>1e-9;
    item.wireRouteMode=item.wireHasBend?"legacy":"auto";item.wireRoute.clear();
    return Model::routeWire(item,m_items)&&applySelectionItem(item);
}
bool InkCanvas::setSelectionWireRoute(const QVariantList &values) {
    if(!selectionIsWire()||values.size()<2||values.size()>1024)return false;
    Polyline points;
    for(const auto &value:values) {
        const auto object=QJsonObject::fromVariantMap(value.toMap());
        if(!object.value("x").isDouble()||!object.value("y").isDouble())return false;
        points.append({object.value("x").toDouble(),object.value("y").toDouble()});
    }
    auto item=m_items[m_selection];if(!Model::setWireRoute(item,points))return false;
    cancel();return applySelectionItem(item);
}
bool InkCanvas::beginStencil(const QString &id) {
    return beginConfiguredStencil(id,{});
}
bool InkCanvas::selectionSupportsVoltage() const {
    return hasSelection()&&m_items[m_selection].kind=="symbol"&&supportsVoltageArrow(m_items[m_selection].symbolId);
}
bool InkCanvas::selectedVoltageArrow() const {return selectionSupportsVoltage()&&m_items[m_selection].voltageArrow;}
bool InkCanvas::selectedVoltageReversed() const {return selectionSupportsVoltage()&&m_items[m_selection].voltageArrowReversed;}
bool InkCanvas::selectedVoltageOtherSide() const {return selectionSupportsVoltage()&&m_items[m_selection].voltageArrowOtherSide;}
bool InkCanvas::setStencilVertical(bool vertical) {
    if(m_stencilVertical!=vertical){cancel();m_stencilVertical=vertical;emit settingsChanged();}return true;
}
bool InkCanvas::setVoltageOption(bool DrawingItem::*member,bool &preference,bool value) {
    if(m_tool=="select") {
        if(!selectionSupportsVoltage())return false;
        cancel();auto item=m_items[m_selection];if(item.*member==value)return true;
        item.*member=value;return rebuildItem(item)&&applySelectionItem(item);
    }
    if(preference!=value){cancel();preference=value;emit settingsChanged();}return true;
}
bool InkCanvas::setVoltageArrow(bool value){return setVoltageOption(&DrawingItem::voltageArrow,m_stencilVoltageArrow,value);}
bool InkCanvas::setVoltageReversed(bool value){return setVoltageOption(&DrawingItem::voltageArrowReversed,m_stencilVoltageReversed,value);}
bool InkCanvas::setVoltageOtherSide(bool value){return setVoltageOption(&DrawingItem::voltageArrowOtherSide,m_stencilVoltageOtherSide,value);}
void InkCanvas::configureStencilItem(DrawingItem &item,QPointF center) const {
    bool quarterTurn=m_stencilVertical;
    if(item.symbolId=="square-root"||isConfigurableStencil(item.symbolId))quarterTurn=false;
    else for(const auto &symbol:electronicsCatalogue())if(symbol.id==item.symbolId&&symbol.anchors.size()==2) {
        const auto axis=symbol.anchors[1]-symbol.anchors[0];
        const bool naturallyVertical=std::abs(axis.y())>std::abs(axis.x());
        quarterTurn=m_stencilVertical!=naturallyVertical;break;
    }
    if(quarterTurn) {
        const auto origin=center+QPointF(80,-120);
        item.sourcePoints={origin,origin+QPointF(0,240),origin+QPointF(-160,0)};
    } else {
        const auto origin=center-QPointF(120,80);
        item.sourcePoints={origin,origin+QPointF(240,0),origin+QPointF(0,160)};
    }
    const bool voltageSupported=supportsVoltageArrow(item.symbolId);
    item.voltageArrow=voltageSupported&&m_stencilVoltageArrow;
    item.voltageArrowReversed=voltageSupported&&m_stencilVoltageReversed;
    item.voltageArrowOtherSide=voltageSupported&&m_stencilVoltageOtherSide;
    if(rebuildItem(item)){
        const auto delta=center-Model::defaultStencilAnchor(item);
        for(auto &point:item.sourcePoints)point+=delta;
    }
}
bool InkCanvas::beginConfiguredStencil(const QString &id,const QVariantMap &parameters) {
    QVariantMap normalized;
    if(isConfigurableStencil(id)) {
        if(!normalizeStencilParameters(id,parameters,&normalized))return false;
    } else if(!parameters.isEmpty())return false;
    DrawingItem probe;probe.kind="symbol";probe.symbolId=id;probe.stencilParameters=normalized;probe.width=2.3;
    probe.sourcePoints={{0,0},{240,0},{0,160}};
    if(!rebuildItem(probe)||probe.strokes.isEmpty())return false;
    cancel();m_symbolId=id;m_stencilParameters=normalized;m_tool="symbol";
    m_recentStencils.removeAll(id);m_recentStencils.prepend(id);while(m_recentStencils.size()>4)m_recentStencils.removeLast();
    emit settingsChanged();return true;
}
QString InkCanvas::selectedStencilId() const {
    return hasSelection()&&m_items[m_selection].kind=="symbol"&&isConfigurableStencil(m_items[m_selection].symbolId)?m_items[m_selection].symbolId:QString();
}
QVariantMap InkCanvas::selectedStencilParameters() const {
    return selectedStencilId().isEmpty()?QVariantMap{}:m_items[m_selection].stencilParameters;
}
bool InkCanvas::setSelectedStencilParameters(const QVariantMap &parameters) {
    const auto id=selectedStencilId();if(id.isEmpty())return false;
    auto item=m_items[m_selection];QVariantMap merged=item.stencilParameters,normalized;
    for(auto it=parameters.cbegin();it!=parameters.cend();++it)merged.insert(it.key(),it.value());
    if(!normalizeStencilParameters(id,merged,&normalized))return false;
    const auto color=Model::foregroundColor(item);if(!color.isValid())return false;
    item.stencilParameters=normalized;
    // Rebuild from an explicit foreground color even when the background flag changes.
    item.strokes={Stroke{Polyline{},item.width,color}};
    if(!rebuildItem(item))return false;
    cancel();return applySelectionItem(item);
}
void InkCanvas::inferLegacyGeometry(DrawingItem &item) {
    if(item.strokes.isEmpty())return;
    item.width=item.strokes.first().width;
    if(item.anchors.size()!=2||QLineF(item.anchors[0],item.anchors[1]).length()<1e-6) {
        if(item.anchors.isEmpty()&&item.strokes.size()==1){item.kind="pen";item.sourcePoints=item.strokes.first().points;}
        return;
    }
    // Recover only geometry that exactly reproduces the saved strokes. Symbols
    // with unrelated anchors stay opaque; guessing would damage an old drawing.
    for(const auto &kind:QStringList{"arrow","line","wire"})for(const auto &style:QStringList{"solid","dashed","dotted"})for(bool horizontal:{true,false}) {
        DrawingItem candidate=item;candidate.kind=kind;candidate.style=style;candidate.sourcePoints=item.anchors;candidate.horizontalFirst=horizontal;
        if(kind=="arrow"&&item.strokes.last().points.size()==3) {
            const auto &head=item.strokes.last().points;
            const QPointF tangent=(item.anchors[1]-item.anchors[0])/QLineF(item.anchors[0],item.anchors[1]).length();
            candidate.headSize=QPointF::dotProduct(item.anchors[1]-head[0],tangent);
            if(candidate.headSize<=0||candidate.headSize>5000)continue;
        }
        if(style!="solid"&&item.strokes.size()>(kind=="arrow"?2:1)) {
            qreal length=0;const auto &first=item.strokes.first().points;
            for(int p=1;p<first.size();++p)length+=QLineF(first[p-1],first[p]).length();
            candidate.patternScale=length/(style=="dashed"?24:0.2);
            if(candidate.patternScale<=0||candidate.patternScale>1000)continue;
        }
        if(!rebuildItem(candidate))continue;
        if(sameStrokes(candidate.strokes,item.strokes)){item=candidate;return;}
    }
}
bool InkCanvas::fitsDocumentBudget(const DrawingItem &item,int replacing) const {
    qint64 rendered=0,source=0;
    auto count=[&](const DrawingItem &entry){source+=entry.sourcePoints.size()+entry.anchors.size()+entry.wireRoute.size();for(const auto &stroke:entry.strokes)rendered+=stroke.points.size();};
    for(int i=0;i<m_items.size();++i)if(i!=replacing)count(m_items[i]);count(item);
    return rendered<=250000&&source<=256000&&m_items.size()+(replacing<0?1:0)<=3000;
}
bool InkCanvas::applySelectionItem(const DrawingItem &item) {
    if(!hasSelection())return false;
    auto next=m_items;next[m_selection]=item;return commitDocument(next);
}
bool InkCanvas::commitDocument(Document next,const QString &message) {
    if(!Model::reroute(next)||!Model::withinBudget(next)){setStatus("La modification dépasserait la page ou la limite de points du dessin.");return false;}
    if(encodeDocument(next)==encodeDocument(m_items))return false;
    checkpoint();m_items=std::move(next);if(!message.isEmpty())setStatus(message);finishMutation();return true;
}
bool InkCanvas::previewItem(const DrawingItem &item) {
    auto next=m_beforeDrag;next[m_selection]=item;
    if(!Model::reroute(next)||!Model::withinBudget(next)){setStatus("La modification doit rester entièrement dans la page et sous la limite de points.");return false;}
    m_items=std::move(next);m_dragChanged=encodeDocument(m_items)!=encodeDocument(m_beforeDrag);update();emit documentChanged();return true;
}
void InkCanvas::setSelectionLineWidth(qreal width) {
    if(!hasSelection()||!std::isfinite(width)||width<1||width>12)return;
    cancel();auto item=m_items[m_selection];item.width=width;
    if(hasSourcePath(item)){if(!rebuildItem(item))return;}else for(int i=0;i<item.strokes.size();++i)if(!Model::isBackgroundStroke(item,i))item.strokes[i].width=width;
    applySelectionItem(item);
}
void InkCanvas::setSelectionLineStyle(const QString &style) {
    if(!selectionCanChangeStyle()||!QStringList{"solid","dashed","dotted"}.contains(style))return;
    cancel();auto item=m_items[m_selection];item.style=style;
    if(!rebuildItem(item)){setStatus("Ce style dépasserait la limite de points du dessin.");return;}applySelectionItem(item);
}
void InkCanvas::setSelectionArrowDirection(const QString &direction) {
    if(!selectionIsArrow()||!QStringList{"start","end","both","none"}.contains(direction))return;
    cancel();auto item=m_items[m_selection];item.arrowDirection=direction;if(rebuildItem(item))applySelectionItem(item);
}
void InkCanvas::setSelectionHorizontalFirst(bool horizontalFirst) {
    if(!selectionIsWire())return;
    cancel();auto item=m_items[m_selection];item.horizontalFirst=horizontalFirst;
    if(item.wireRouteMode=="auto")item.wireRoute.clear();
    if(Model::routeWire(item,m_items))applySelectionItem(item);
}
int InkCanvas::endpointAt(const QPointF &point) const {
    if(!selectionHasEndpoints())return -1;
    const auto &ends=m_items[m_selection].sourcePoints;
    const qreal radius=24/pageScale();
    const auto first=QLineF(point,ends[0]).length(),second=QLineF(point,ends[1]).length();
    if(std::min(first,second)>radius)return -1;
    return first<=second?0:1;
}
int InkCanvas::boxHandleAt(const QPointF &point) const {
    if(!selectionCanResize())return -1;
    const auto corners=Model::boxCorners(m_items[m_selection]);int found=-1;qreal closest=24/pageScale();
    for(int i=0;i<corners.size();++i){const qreal d=QLineF(point,corners[i]).length();if(d<closest){closest=d;found=i;}}
    return found;
}
int InkCanvas::wireSegmentAt(const QPointF &point) const {
    if(!selectionIsWire())return -1;
    const auto points=Model::wirePoints(m_items[m_selection]);int found=-1;qreal closest=24/pageScale();
    for(int i=1;i+2<points.size();++i) {
        const qreal distance=QLineF(point,(points[i]+points[i+1])/2).length();
        if(distance<closest){closest=distance;found=i;}
    }
    return found;
}

QVariantList InkCanvas::alignmentGuides() const {
    QVariantList result;
    for(const auto &guide:m_alignmentGuides)result.append(QVariantMap{{"x1",guide.line.x1()},{"y1",guide.line.y1()},
        {"x2",guide.line.x2()},{"y2",guide.line.y2()},{"targetId",guide.targetId}});
    return result;
}
void InkCanvas::setAlignmentGuides(const QVector<Model::AlignmentGuide> &guides) {
    if(m_alignmentGuides==guides)return;
    m_alignmentGuides=guides;update();emit alignmentGuidesChanged();
}

InkCanvas::DrawingItem InkCanvas::draftItem() const {
    DrawingItem item;item.kind=m_tool;item.style=m_style;item.width=m_width;item.horizontalFirst=m_horizontalFirst;
    if(m_tool=="wire")item.wireRoute=m_wireDraftRoute;
    if(m_tool=="symbol") {
        item.symbolId=m_symbolId;item.stencilParameters=m_stencilParameters;item.width=2.3;configureStencilItem(item,m_last);
    } else if(m_tool=="rectangle"||m_tool=="ellipse") {
        const auto box=QRectF(m_start,m_last).normalized();item.sourcePoints={box.topLeft(),box.topRight(),box.bottomLeft()};
    } else {
        item.sourcePoints=m_tool=="pen"?m_draft:Polyline{m_start,m_last};
        if(m_tool=="wire"&&m_snapping){item.startAttachment=Model::nearestPort(m_items,item.sourcePoints[0],22);item.endAttachment=Model::nearestPort(m_items,item.sourcePoints[1],22);}
    }
    if(!(item.kind=="wire"?Model::routeWire(item,m_items):rebuildItem(item))){item.strokes.clear();return item;}
    if(item.kind=="symbol"){
        m_stencilPlacement=Model::placeStencilNearPointer(item,m_last,m_items,m_snapping?8/pageScale():0);
        if(m_stencilPlacement.snapped){
            auto placed=item;QTransform shift;shift.translate(m_stencilPlacement.delta.x(),m_stencilPlacement.delta.y());
            if(Model::transform(placed,shift)&&insidePage(placed.strokes))item=std::move(placed);
            else m_stencilPlacement={};
        }
    }
    if(item.kind=="wire")m_wireDraftRoute=item.wireRoute;
    Model::setForegroundColor(item,m_color);
    return item;
}
QVector<Stroke> InkCanvas::draftStrokes() const {
    if(!m_drawing||m_tool=="select")return {};
    return draftItem().strokes;
}
void InkCanvas::paint(QPainter *painter) {
    painter->fillRect(boundingRect(),Qt::white);
    painter->translate(pageOffset());painter->scale(pageScale(),pageScale());
    painter->setClipRect(QRectF(0,0,PageWidth,PageHeight));
    painter->fillRect(QRectF(0,0,PageWidth,PageHeight),Qt::white);
    if(m_grid) {
        painter->setPen(QPen(QColor("#dadada"),1));
        for(int x=20;x<PageWidth;x+=40)for(int y=20;y<PageHeight;y+=40)painter->drawPoint(x,y);
    }
    painter->setRenderHint(QPainter::Antialiasing);
    paintStrokes(*painter,allStrokes());paintStrokes(*painter,draftStrokes());
    if(m_drawing&&m_tool=="symbol"&&m_stencilPlacement.snapped){
        painter->setPen(QPen(Qt::black,2/pageScale()));painter->setBrush(Qt::white);
        painter->drawEllipse(m_stencilPlacement.targetPoint,7/pageScale(),7/pageScale());
    }
    if(!m_alignmentGuides.isEmpty()) {
        painter->setPen(QPen(QColor("#777777"),1/pageScale(),Qt::DashLine));
        for(const auto &guide:m_alignmentGuides)painter->drawLine(guide.line);
    }
    if(hasSelection()) {
        painter->setPen(QPen(Qt::darkGray,2,Qt::DashLine));painter->setBrush(Qt::NoBrush);
        painter->drawRect(bounds(m_items[m_selection].strokes).adjusted(-12,-12,12,12));
        if(m_tool=="select"&&(selectionHasEndpoints()||selectionCanResize())) {
            const qreal radius=9/pageScale();
            painter->setPen(QPen(Qt::black,2/pageScale()));painter->setBrush(Qt::NoBrush);
            const auto handles=selectionHasEndpoints()?m_items[m_selection].sourcePoints:Model::boxCorners(m_items[m_selection]);
            for(auto endpoint:handles){painter->drawEllipse(endpoint,radius,radius);painter->drawLine(endpoint-QPointF(radius/2,0),endpoint+QPointF(radius/2,0));painter->drawLine(endpoint-QPointF(0,radius/2),endpoint+QPointF(0,radius/2));}
            if(selectionIsWire()) {
                const auto route=Model::wirePoints(m_items[m_selection]);
                for(int i=1;i+2<route.size();++i)if(QLineF(route[i],route[i+1]).length()>1e-7) {
                    const auto point=(route[i]+route[i+1])/2;
                    painter->drawRect(QRectF(point-QPointF(radius,radius),QSizeF(2*radius,2*radius)));
                }
            }
        }
    }
    if(m_drawing&&m_snapping&&(m_tool=="wire"||m_tool=="line"||m_tool=="arrow")) {
        painter->setPen(QPen(Qt::darkGray,2));painter->setBrush(Qt::NoBrush);painter->drawEllipse(m_last,10,10);
    }
}
void InkCanvas::begin(qreal x,qreal y) {
    if(m_drawing||!std::isfinite(x)||!std::isfinite(y))return;
    setAlignmentGuides({});
    m_start=m_last=toPage(x,y);m_draft={m_start};m_wireDraftRoute.clear();m_drawing=true;m_dragEndpoint=-1;m_dragCorner=-1;m_dragSegment=-1;m_dragChanged=false;
    if(m_tool=="select") {
        m_dragEndpoint=endpointAt(m_start);m_dragCorner=boxHandleAt(m_start);m_dragSegment=wireSegmentAt(m_start);
        if(m_dragEndpoint<0&&m_dragCorner<0&&m_dragSegment<0) {
            m_selection=-1;
            for(int i=m_items.size()-1;i>=0;--i)if(hitItem(m_items[i],m_start,16/pageScale())){m_selection=i;break;}
            m_dragEndpoint=endpointAt(m_start);m_dragCorner=boxHandleAt(m_start);m_dragSegment=wireSegmentAt(m_start);
        }
        if(hasSelection()){
            m_beforeDrag=m_items;m_original=m_items[m_selection];
            if(m_dragEndpoint>=0)m_handleOffset=m_original.sourcePoints[m_dragEndpoint]-m_start;
            if(m_dragCorner>=0)m_handleOffset=Model::boxCorners(m_original)[m_dragCorner]-m_start;
            if(m_dragSegment>=0) {
                const auto route=Model::wirePoints(m_original);m_handleOffset=(route[m_dragSegment]+route[m_dragSegment+1])/2-m_start;
            }
        }
        emit documentChanged();
    } else if(m_tool=="symbol") {
        m_alignmentPrototype=draftItem();
        const auto delta=m_start-Model::defaultStencilAnchor(m_alignmentPrototype);
        QTransform shift;shift.translate(delta.x(),delta.y());Model::transform(m_alignmentPrototype,shift);
        m_stencilCenterRange=stencilCenterRange(m_alignmentPrototype,m_start);
        move(x,y);
    }
    else if(m_snapping&&m_tool!="pen")m_start=m_last=snapPoint(m_start,anchors(),22,m_grid?20:0);
    update();
}
void InkCanvas::move(qreal x,qreal y) {
    if(!m_drawing||!std::isfinite(x)||!std::isfinite(y))return;
    m_last=toPage(x,y);
    if(m_tool=="select"&&hasSelection()) {
        if(m_dragEndpoint>=0) {
            if(QLineF(m_start,m_last).length()<0.1){m_items=m_beforeDrag;m_dragChanged=false;update();return;}
            auto endpoint=m_last+m_handleOffset;
            if(m_snapping)endpoint=snapPoint(endpoint,anchors(m_selection),22,m_grid?20:0);
            endpoint.setX(std::clamp(endpoint.x(),qreal(0),PageWidth));endpoint.setY(std::clamp(endpoint.y(),qreal(0),PageHeight));
            if(QLineF(endpoint,m_original.sourcePoints[1-m_dragEndpoint]).length()<1)return;
            auto item=m_original;item.sourcePoints[m_dragEndpoint]=endpoint;
            if(item.kind=="wire") {
                auto &attachment=m_dragEndpoint==0?item.startAttachment:item.endAttachment;
                attachment={};if(m_snapping)attachment=Model::nearestPort(m_beforeDrag,item.sourcePoints[m_dragEndpoint],22,item.id);
            }
            if(rebuildItem(item))previewItem(item);return;
        }
        if(m_dragSegment>=0) {
            if(QLineF(m_start,m_last).length()<0.1){m_items=m_beforeDrag;m_dragChanged=false;update();return;}
            auto point=m_last+m_handleOffset;if(m_snapping)point=snapPoint(point,{},22,m_grid?20:0);
            auto item=m_original;if(Model::moveWireSegment(item,m_dragSegment,point))previewItem(item);
            return;
        }
        if(m_dragCorner>=0) {
            const auto corners=Model::boxCorners(m_original);const auto fixed=corners[(m_dragCorner+2)%4];
            const auto ax=(m_original.sourcePoints[1]-m_original.sourcePoints[0])/Model::boxWidth(m_original);
            const auto ay=(m_original.sourcePoints[2]-m_original.sourcePoints[0])/Model::boxHeight(m_original);
            auto point=m_last+m_handleOffset;if(m_snapping)point=snapPoint(point,{},22,m_grid?20:0);
            const auto delta=point-fixed;const qreal dx=QPointF::dotProduct(delta,ax),dy=QPointF::dotProduct(delta,ay);
            if(std::abs(dx)<2||std::abs(dy)<2)return;
            const auto origin=fixed+ax*std::min(qreal(0),dx)+ay*std::min(qreal(0),dy);
            auto item=m_original;item.sourcePoints={origin,origin+ax*std::abs(dx),origin+ay*std::abs(dy)};
            if(rebuildItem(item))previewItem(item);return;
        }
        QPointF delta=m_last-m_start;
        if(QLineF(m_start,m_last).length()<0.1){m_items=m_beforeDrag;m_dragChanged=false;setAlignmentGuides({});update();emit documentChanged();return;}
        const QRectF box=bounds(m_original.strokes);
        delta.setX(std::clamp(delta.x(),-box.left(),PageWidth-box.right()));
        delta.setY(std::clamp(delta.y(),-box.top(),PageHeight-box.bottom()));
        QVector<Model::AlignmentGuide> guides;
        if(m_snapping&&Model::hasBox(m_original)) {
            const auto aligned=Model::alignTranslation(m_original,m_beforeDrag,delta,8/pageScale());
            const QPointF constrained(std::clamp(aligned.delta.x(),-box.left(),PageWidth-box.right()),
                                      std::clamp(aligned.delta.y(),-box.top(),PageHeight-box.bottom()));
            for(const auto &guide:aligned.guides) {
                const bool vertical=std::abs(guide.line.dx())<1e-7;
                if(std::abs(vertical?constrained.x()-aligned.delta.x():constrained.y()-aligned.delta.y())<1e-7)guides.append(guide);
            }
            delta=constrained;
        }
        auto item=m_original;QTransform transform;transform.translate(delta.x(),delta.y());
        if(item.kind=="wire"){item.startAttachment={};item.endAttachment={};}
        if(Model::transform(item,transform)&&previewItem(item))setAlignmentGuides(guides);else setAlignmentGuides({});
    } else if(m_tool=="pen") {
        if(m_draft.size()<20000&&QLineF(m_draft.last(),m_last).length()>=1.6)m_draft.append(m_last);
    } else if(m_tool=="symbol") {
        m_last=clampStencilCenter(m_last,m_stencilCenterRange);
        draftItem();
        if(m_stencilPlacement.snapped)setAlignmentGuides({});
        else if(m_snapping) {
            const auto aligned=Model::alignTranslation(m_alignmentPrototype,m_items,m_last-m_start,8/pageScale());
            const auto candidate=m_start+aligned.delta;
            m_last=clampStencilCenter(candidate,m_stencilCenterRange);
            QVector<Model::AlignmentGuide> guides;
            for(const auto &guide:aligned.guides) {
                const bool vertical=std::abs(guide.line.dx())<1e-7;
                if(std::abs(vertical?m_last.x()-candidate.x():m_last.y()-candidate.y())<1e-7)guides.append(guide);
            }
            setAlignmentGuides(guides);
        }
    } else if(m_snapping)m_last=snapPoint(m_last,anchors(),22,m_grid?20:0);
    update();
}
void InkCanvas::end(qreal x,qreal y) {
    if(!m_drawing)return;
    if(!std::isfinite(x)||!std::isfinite(y)){cancel();return;}
    move(x,y);
    bool changed=false;
    if(m_tool=="select") {
        if(hasSelection()&&m_dragChanged){m_undo.append(m_beforeDrag);m_redo.clear();while(m_undo.size()>60)m_undo.removeFirst();changed=true;}
    } else {
        auto item=draftItem();
        if(!item.strokes.isEmpty()) {
            if(!insidePage(item.strokes))setStatus("Le trait doit rester entièrement dans la page.");
            else {
                auto next=m_items;next.append(item);
                if(item.kind=="symbol"&&m_stencilPlacement.snapped)
                    Model::attachCoincidentWireEndpoints(next,item,&m_stencilPlacement);
                if(fitsDocumentBudget(item)&&Model::reroute(next)&&Model::withinBudget(next)) {
                    checkpoint();m_items=std::move(next);m_selection=-1;changed=true;
                } else setStatus("Le tracé doit rester dans la page et respecter les connexions et la limite de points.");
            }
        }
    }
    m_drawing=false;m_dragEndpoint=-1;m_dragCorner=-1;m_dragSegment=-1;m_dragChanged=false;m_draft.clear();m_wireDraftRoute.clear();m_beforeDrag.clear();m_alignmentPrototype={};m_stencilPlacement={};setAlignmentGuides({});
    if(changed)finishMutation();else {update();emit documentChanged();}
}
void InkCanvas::cancel() {
    if(m_drawing&&m_tool=="select"&&hasSelection())m_items=m_beforeDrag;
    m_drawing=false;m_dragEndpoint=-1;m_dragCorner=-1;m_dragSegment=-1;m_dragChanged=false;m_draft.clear();m_wireDraftRoute.clear();m_beforeDrag.clear();m_alignmentPrototype={};m_stencilPlacement={};setAlignmentGuides({});update();emit documentChanged();
}
void InkCanvas::checkpoint(){m_undo.append(m_items);m_redo.clear();while(m_undo.size()>60)m_undo.removeFirst();}
void InkCanvas::finishMutation(){save();update();emit documentChanged();}
void InkCanvas::undo(){cancel();if(m_undo.isEmpty())return;m_redo.append(m_items);m_items=m_undo.takeLast();m_selection=-1;finishMutation();}
void InkCanvas::redo(){cancel();if(m_redo.isEmpty())return;m_undo.append(m_items);m_items=m_redo.takeLast();m_selection=-1;finishMutation();}
void InkCanvas::clear(){cancel();if(m_items.isEmpty())return;checkpoint();m_items.clear();m_selection=-1;finishMutation();}
void InkCanvas::removeSelection(){cancel();if(!hasSelection())return;auto next=m_items;next.removeAt(m_selection);if(commitDocument(next)){m_selection=-1;emit documentChanged();}}
void InkCanvas::transformSelection(const QTransform &transform) {
    if(!hasSelection())return;
    const QRectF next=transform.mapRect(bounds(m_items[m_selection].strokes));
    if(next.left()<0||next.top()<0||next.right()>PageWidth||next.bottom()>PageHeight) {setStatus("La transformation sortirait de la page.");return;}
    auto item=m_items[m_selection];
    if(item.kind=="wire"){item.startAttachment={};item.endAttachment={};}
    if(!Model::transform(item,transform)){setStatus("La taille minimale ou maximale de cet élément est atteinte.");return;}
    applySelectionItem(item);
}
void InkCanvas::scaleSelection(qreal factor) {
    cancel();
    if(!hasSelection()||!std::isfinite(factor)||factor<0.5||factor>2)return;
    const QPointF center=bounds(m_items[m_selection].strokes).center();
    QTransform t;t.translate(center.x(),center.y());t.scale(factor,factor);t.translate(-center.x(),-center.y());transformSelection(t);
}
void InkCanvas::rotateSelection() {
    cancel();
    if(!hasSelection())return;
    const QPointF center=bounds(m_items[m_selection].strokes).center();
    QTransform t;t.translate(center.x(),center.y());t.rotate(90);t.translate(-center.x(),-center.y());transformSelection(t);
}
void InkCanvas::duplicateSelection() {
    cancel();if(hasSelection())duplicateObjects({m_items[m_selection].id});
}
void InkCanvas::duplicateObjects(const QStringList &ids) {
    cancel();if(ids.isEmpty()||ids.size()>3000)return;
    QVector<Stroke> strokes;for(const auto &item:m_items)if(ids.contains(item.id))strokes+=item.strokes;
    if(strokes.isEmpty())return;
    const QRectF box=bounds(strokes);
    const QPointF offset(box.right()+40<=PageWidth?40:box.left()>=40?-40:0,
                        box.bottom()+40<=PageHeight?40:box.top()>=40?-40:0);
    auto copies=Model::copies(m_items,ids,offset);if(copies.isEmpty())return;
    auto next=m_items;next+=copies;if(commitDocument(next)){m_selection=m_items.size()-1;emit documentChanged();}
}
void InkCanvas::insertSymbol(const QString &id,qreal x,qreal y) {
    if(!std::isfinite(x)||!std::isfinite(y)||m_items.size()>=3000)return;
    x=std::clamp(x,qreal(130),PageWidth-130);y=std::clamp(y,qreal(90),PageHeight-90);
    DrawingItem item;item.kind="symbol";item.symbolId=id;item.width=2.3;
    if(isConfigurableStencil(id)&&!normalizeStencilParameters(id,id==m_symbolId?m_stencilParameters:QVariantMap{},&item.stencilParameters))return;
    configureStencilItem(item,{x,y});if(!rebuildItem(item))return;
    const auto center=clampStencilCenter({x,y},stencilCenterRange(item,{x,y}));
    if(center!=QPointF(x,y)){configureStencilItem(item,center);if(!rebuildItem(item))return;}
    Model::setForegroundColor(item,m_color);
    if(!fitsDocumentBudget(item)){setStatus("Ce symbole dépasserait la limite de points du dessin.");return;}
    cancel();auto next=m_items;next.append(item);
    if(!Model::reroute(next)||!Model::withinBudget(next)){setStatus("Le symbole ne laisse pas de passage aux fils dans la page.");return;}
    checkpoint();m_items=std::move(next);m_selection=m_items.size()-1;m_tool="select";emit settingsChanged();finishMutation();
}
QJsonObject InkCanvas::encodeDocument(const Document &document) {
    QJsonArray items;
    for(const auto &item:document) {
        QJsonArray strokes;
        for(const auto &stroke:item.strokes)strokes.append(QJsonObject{{"points",encodePoints(stroke.points)},{"width",stroke.width},{"color",stroke.color.name()}});
        QJsonObject geometry{{"kind",item.kind},{"points",encodePoints(item.sourcePoints)},{"style",item.style},{"width",item.width},{"horizontalFirst",item.horizontalFirst},{"arrowDirection",item.arrowDirection},{"headSize",item.headSize},{"patternScale",item.patternScale},{"cornerRadius",item.cornerRadius},{"wireBend",item.wireBend},{"symbolId",item.symbolId}};
        geometry.insert("wireAxis",QJsonArray{item.wireAxis.x(),item.wireAxis.y()});
        geometry.insert("wireHasBend",item.wireHasBend);
        if(item.kind=="wire") {
            geometry.insert("wireRoute",encodePoints(item.wireRoute));
            geometry.insert("wireRouteMode",item.wireRouteMode);
        }
        if(!item.stencilParameters.isEmpty())geometry.insert("stencilParameters",QJsonObject::fromVariantMap(item.stencilParameters));
        if(item.kind=="symbol") {
            geometry.insert("voltageArrow",item.voltageArrow);
            geometry.insert("voltageArrowReversed",item.voltageArrowReversed);
            geometry.insert("voltageArrowOtherSide",item.voltageArrowOtherSide);
        }
        auto ref=[](const Model::Attachment &a){
            QJsonObject value{{"objectId",a.objectId},{"portId",a.portId}};
            if(a.wirePosition>=0)value.insert("wirePosition",a.wirePosition);
            return value;
        };
        items.append(QJsonObject{{"id",item.id},{"strokes",strokes},{"anchors",encodePoints(item.anchors)},{"portIds",QJsonArray::fromStringList(item.portIds)},{"geometry",geometry},
                                {"connections",QJsonObject{{"start",ref(item.startAttachment)},{"end",ref(item.endAttachment)}}}});
    }
    return {{"schemaVersion",3},{"items",items}};
}
QVariantMap InkCanvas::documentSnapshot() const {
    return encodeDocument(m_drawing&&m_tool=="select"&&hasSelection()?m_beforeDrag:m_items).toVariantMap();
}
QVariantList InkCanvas::renderedStrokes(bool includePreview) const {
    QVariantList result;const auto &doc=!includePreview&&m_drawing&&m_tool=="select"&&hasSelection()?m_beforeDrag:m_items;
    auto append=[&](const QVector<Stroke> &strokes,const QString &id,bool preview){
        for(const auto &stroke:strokes){QVariantList points;for(auto p:stroke.points)points.append(QVariantMap{{"x",p.x()},{"y",p.y()}});
            result.append(QVariantMap{{"objectId",id},{"points",points},{"width",stroke.width},{"color",stroke.color.name()},{"preview",preview}});}
    };
    for(const auto &item:doc)append(item.strokes,item.id,includePreview&&m_drawing&&m_tool=="select");
    if(includePreview)append(draftStrokes(),{},true);return result;
}
void InkCanvas::save() {
    if(m_persistenceBlocked){setStatus("Le dessin enregistré est invalide et reste conservé. Exportez votre travail courant avant de fermer.");return;}
    QSaveFile file(documentPath());
    const QByteArray bytes=QJsonDocument(encodeDocument(m_items)).toJson(QJsonDocument::Compact);
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())setStatus("Sauvegarde impossible : "+file.errorString());
}
void InkCanvas::load() {
    QFile file(documentPath());if(!file.exists())return;
    // Do not overwrite an unreadable previous drawing on the next pen gesture.
    m_persistenceBlocked=true;
    setStatus("Dessin local illisible ; original conservé. Les exports restent disponibles.");
    if(file.size()>32*1024*1024||!file.open(QIODevice::ReadOnly)){setStatus("Dessin local illisible ; fichier conservé.");return;}
    QJsonParseError error;auto json=QJsonDocument::fromJson(file.readAll(),&error).object();
    const int version=json.value("schemaVersion").toInt();
    if(error.error!=QJsonParseError::NoError||(version!=1&&version!=2&&version!=3)||!json.value("items").isArray()) {setStatus("Format du dessin local invalide ; fichier conservé.");return;}
    auto items=json.value("items").toArray();if(items.size()>3000)return;
    Document loaded;int budget=250000,sourceBudget=256000;QSet<QString> ids;
    auto validId=[](const QString &id){const QUuid uuid(id);return !uuid.isNull()&&uuid.toString(QUuid::WithoutBraces)==id;};
    for(auto value:items) {
        if(!value.isObject())return;
        auto object=value.toObject();DrawingItem item;
        if(!object.value("anchors").isArray()||!object.value("strokes").isArray())return;
        if(!decodePoints(object.value("anchors").toArray(),&item.anchors,&sourceBudget))return;
        const auto strokes=object.value("strokes").toArray();if(strokes.isEmpty()||strokes.size()>100000)return;
        for(auto v:strokes) {
            if(!v.isObject())return;
            auto s=v.toObject();Stroke stroke;
            stroke.width=s.value("width").toDouble();stroke.color=QColor(s.value("color").toString());
            if(!std::isfinite(stroke.width)||stroke.width<=0||stroke.width>100||!stroke.color.isValid()||!s.value("points").isArray()||s.value("points").toArray().isEmpty()||!decodePoints(s.value("points").toArray(),&stroke.points,&budget))return;
            item.strokes.append(stroke);
        }
        if(version==1){inferLegacyGeometry(item);sourceBudget-=item.sourcePoints.size();if(sourceBudget<0)return;}
        else {
            if(!object.value("geometry").isObject())return;
            const auto geometry=object.value("geometry").toObject();
            item.kind=geometry.value("kind").toString();item.style=geometry.value("style").toString();item.arrowDirection=geometry.value("arrowDirection").toString();
            item.width=geometry.value("width").toDouble();item.headSize=geometry.value("headSize").toDouble();item.patternScale=geometry.value("patternScale").toDouble();item.horizontalFirst=geometry.value("horizontalFirst").toBool();
            if(geometry.contains("wireAxis")){
                const auto axis=geometry.value("wireAxis").toArray();
                if(axis.size()!=2||!axis[0].isDouble()||!axis[1].isDouble())return;
                item.wireAxis={axis[0].toDouble(),axis[1].toDouble()};
            }
            item.wireHasBend=geometry.value("wireHasBend").toBool();
            for(const auto *key:{"voltageArrow","voltageArrowReversed","voltageArrowOtherSide"})
                if(geometry.contains(key)&&!geometry.value(key).isBool())return;
            item.voltageArrow=geometry.value("voltageArrow").toBool();
            item.voltageArrowReversed=geometry.value("voltageArrowReversed").toBool();
            item.voltageArrowOtherSide=geometry.value("voltageArrowOtherSide").toBool();
            if((item.voltageArrow||item.voltageArrowReversed||item.voltageArrowOtherSide)&&item.kind!="symbol")return;
            if(!QStringList{"legacy","symbol","pen","line","wire","arrow","rectangle","ellipse"}.contains(item.kind)||!QStringList{"solid","dashed","dotted"}.contains(item.style)||!QStringList{"start","end","both","none"}.contains(item.arrowDirection))return;
            if(!std::isfinite(item.width)||item.width<=0||item.width>100||!std::isfinite(item.headSize)||item.headSize<=0||item.headSize>5000||!std::isfinite(item.patternScale)||item.patternScale<=0||item.patternScale>1000)return;
            if(!geometry.value("points").isArray()||!decodePoints(geometry.value("points").toArray(),&item.sourcePoints,&sourceBudget))return;
            if(QStringList{"line","wire","arrow"}.contains(item.kind)&&item.sourcePoints.size()!=2)return;
            if(item.kind=="pen"&&(item.sourcePoints.isEmpty()||item.sourcePoints.size()>20000))return;
            if(version==3) {
                item.cornerRadius=geometry.value("cornerRadius").toDouble(-1);item.wireBend=geometry.value("wireBend").toDouble(30000);item.symbolId=geometry.value("symbolId").toString();
                if(!std::isfinite(item.cornerRadius)||item.cornerRadius<0||item.cornerRadius>10000||!std::isfinite(item.wireBend)||std::abs(item.wireBend)>2000)return;
                if(item.kind!="symbol"&&!item.symbolId.isEmpty())return;
            }
            if(geometry.contains("wireRouteMode")) {
                if(item.kind!="wire"||!geometry.value("wireRouteMode").isString())return;
                item.wireRouteMode=geometry.value("wireRouteMode").toString();
                if(item.wireRouteMode!="auto"&&item.wireRouteMode!="manual"&&item.wireRouteMode!="legacy")return;
            } else if(item.kind=="wire")item.wireRouteMode=item.wireHasBend||std::abs(item.wireBend)>1e-9?"legacy":"auto";
            if(geometry.contains("wireRoute")) {
                const auto route=geometry.value("wireRoute");
                if(item.kind!="wire"||!route.isArray()||route.toArray().size()>1024
                        ||!decodePoints(route.toArray(),&item.wireRoute,&sourceBudget))return;
                if(!item.wireRoute.isEmpty()&&(item.wireRoute.size()<2
                        ||QLineF(item.wireRoute.first(),item.sourcePoints.first()).length()>1e-7
                        ||QLineF(item.wireRoute.last(),item.sourcePoints.last()).length()>1e-7))return;
            }
            if(geometry.contains("stencilParameters")) {
                if(version!=3||item.kind!="symbol"||!isConfigurableStencil(item.symbolId)||!geometry.value("stencilParameters").isObject())return;
                if(!normalizeStencilParameters(item.symbolId,geometry.value("stencilParameters").toObject().toVariantMap(),&item.stencilParameters))return;
            } else if(item.kind=="symbol"&&isConfigurableStencil(item.symbolId)) {
                if(!normalizeStencilParameters(item.symbolId,{},&item.stencilParameters))return;
            }
            if((item.kind=="rectangle"||item.kind=="ellipse"||!item.symbolId.isEmpty())&&item.sourcePoints.size()!=3)return;
            if(hasSourcePath(item)) {
                auto canonical=item;if(!rebuildItem(canonical))return;
                if(!sameStrokes(canonical.strokes,item.strokes))return;
                item=canonical;
            }
        }
        if(version==3) {
            item.id=object.value("id").toString();if(!validId(item.id)||ids.contains(item.id))return;
            if(!object.value("portIds").isArray())return;
            QStringList ports;for(auto p:object.value("portIds").toArray()) {
                const auto name=p.toString();if(!p.isString()||name.isEmpty()||name.size()>64||ports.contains(name))return;
                for(auto c:name)if(!c.isLetterOrNumber()&&c!='-'&&c!='_')return;ports.append(name);
            }
            if(ports.size()!=item.anchors.size())return;
            if(hasSourcePath(item)&&ports!=item.portIds)return;
            item.portIds=ports;
            if(!object.value("connections").isObject())return;
            const auto connections=object.value("connections").toObject();
            auto decodeRef=[&](const QJsonValue &value,Model::Attachment &ref){
                if(!value.isObject())return false;auto obj=value.toObject();
                if(!obj.value("objectId").isString()||!obj.value("portId").isString())return false;
                ref={obj.value("objectId").toString(),obj.value("portId").toString()};
                if(obj.contains("wirePosition")) {
                    if(!obj.value("wirePosition").isDouble())return false;
                    ref.wirePosition=obj.value("wirePosition").toDouble();
                    if(!std::isfinite(ref.wirePosition)||ref.wirePosition<0||ref.wirePosition>1||ref.portId!="route")return false;
                }
                if(ref.portId=="route"&&ref.wirePosition<0)return false;
                if(ref.objectId.isEmpty())return ref.portId.isEmpty()&&ref.wirePosition==-1;
                return item.kind=="wire"&&validId(ref.objectId)&&ref.objectId!=item.id&&!ref.portId.isEmpty()&&ref.portId.size()<=64;
            };
            if(!decodeRef(connections.value("start"),item.startAttachment)||!decodeRef(connections.value("end"),item.endAttachment))return;
        } else Model::ensurePorts(item);
        ids.insert(item.id);
        loaded.append(item);
    }
    auto resolved=loaded;if(!Model::resolveAttachments(resolved)||!Model::withinBudget(resolved,false))return;
    // A valid stored attachment must agree with its visible endpoint. Do not
    // silently reapply stale graph metadata over a different saved drawing.
    for(int i=0;i<loaded.size();++i)if(!sameStrokes(loaded[i].strokes,resolved[i].strokes))return;
    bool detached=false;
    for(int i=0;i<loaded.size();++i)if((!loaded[i].startAttachment.empty()&&resolved[i].startAttachment.empty())
            ||(!loaded[i].endAttachment.empty()&&resolved[i].endAttachment.empty()))detached=true;
    loaded=std::move(resolved);
    m_items=loaded;
    m_persistenceBlocked=false;m_status.clear();
    if(version<3)save();
    if(detached)setStatus("Une connexion vers un objet absent a été détachée ; son tracé est conservé.");
}
void InkCanvas::exportDrawing(const QString &format) {
    if(format!="svg"&&format!="pdf"&&format!="scene")return;
    cancel();
    const QString suffix=format=="scene"?"paper-scene.json":format;
    const QString path=exportDirectory("reink")+"/reInk-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")+"."+suffix;
    QString error;const auto strokes=allStrokes();
    bool result=format=="svg"?exportSvg(path,strokes,&error):format=="pdf"?exportPdf(path,strokes,"reInk",&error):exportScene(path,strokes,"reInk",&error);
    setStatus(result?"Export créé : "+path:"Export impossible : "+error);
}
void InkCanvas::importDrawing() {
    cancel();
    if(m_bridge->busy()||m_items.isEmpty())return;
    const QString path=exportDirectory("reink")+"/latest.paper-scene.json";
    QString error;if(!exportScene(path,allStrokes(),"reInk",&error)){setStatus(error);return;}
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){setStatus(file.errorString());return;}
    const QString key="reink-"+QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());
    m_bridge->importFile(path,"reInk — Dessin",key);
}

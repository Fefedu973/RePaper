#include "EditorAdapter.h"
#include "NativeScene.h"
#include "NativePreviewFrame.h"
#include "Geometry.h"
#include "DrawingModel.h"
#include <QCoreApplication>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QThread>
#include <QScopedValueRollback>
#ifdef REPAPER_EDITOR_PC_HARNESS
#include "InkCanvas.h"
#endif

EditorAdapter::EditorAdapter(QObject *parent):QObject(parent),m_native(new NativeScene(this)) {
    connect(m_native,&NativeScene::changed,this,&EditorAdapter::notifyStateChanged);
    connect(m_native,&NativeScene::previewChanged,this,&EditorAdapter::notifyPreviewChanged);
}
void EditorAdapter::notifyStateChanged() {
    ++m_stateRevision;++m_overlayRevision;
    m_stateValid=false;m_overlayStateValid=false;
    emit changed();
#ifdef REPAPER_EDITOR_PC_HARNESS
    emit previewChanged();
#endif
}
void EditorAdapter::notifyPreviewChanged() {
    // Direct compatibility reads of state() remain current, while geometry-only
    // updates do not re-evaluate the entire toolbar/catalogue/properties graph.
    ++m_stateRevision;++m_overlayRevision;
    m_stateValid=false;m_overlayStateValid=false;
    emit previewChanged();
}
QVariantMap EditorAdapter::evidence() const {const auto native=m_native->evidence();return native.isEmpty()?m_evidence:native;}
bool EditorAdapter::available() const {
#ifdef REPAPER_EDITOR_PC_HARNESS
    return !m_page.isNull();
#else
    return m_native->available();
#endif
}
QString EditorAdapter::backend() const {
#ifdef REPAPER_EDITOR_PC_HARNESS
    return available()?"pc-harness":"pc-harness-unattached";
#else
    return available()?"xochitl-experimental":"xochitl-unvalidated";
#endif
}
QString EditorAdapter::status() const {
#ifdef REPAPER_EDITOR_PC_HARNESS
    return available()?"Banc PC : page de test vectorielle. Ce moteur est distinct de Xochitl."
        :"Banc PC : aucune page de test reliée au panneau.";
#else
    return m_native->status();
#endif
}
QVariantMap EditorAdapter::state() const {
    if(m_stateValid||m_readingState)return m_cachedState;
    QScopedValueRollback<bool> reading(m_readingState,true);
    const auto revision=m_stateRevision;
    ++m_stateBuildCount;
    const auto result=readState();
    if(revision==m_stateRevision){m_cachedState=result;m_stateValid=true;}
    return result;
}
QVariantMap EditorAdapter::overlayState() const {
    if(m_overlayStateValid)return m_cachedOverlayState;
    const auto revision=m_overlayRevision;
#ifdef REPAPER_EDITOR_PC_HARNESS
    const auto result=state();
#else
    const auto result=m_native->overlayState();
#endif
    if(revision==m_overlayRevision){m_cachedOverlayState=result;m_overlayStateValid=true;}
    return result;
}
QVariant EditorAdapter::previewFrame() const {return QVariant::fromValue(m_native->previewFrame());}
QVariantMap EditorAdapter::readState() const {
#ifndef REPAPER_EDITOR_PC_HARNESS
    return m_native->state();
#else
    QVariantMap result{{"tool","select"},{"lineStyle","solid"},{"lineWidth",3},{"lineColor","#000000"},{"selectedLineColor","#000000"},
        {"hasSelection",false},{"canUndo",false},{"canRedo",false},{"selectionHasEndpoints",false},
        {"selectionCanChangeStyle",false},{"selectionIsArrow",false},{"selectionIsWire",false},
        {"selectedLineWidth",3},{"selectedLineStyle","solid"},{"selectedArrowDirection","end"},
        {"selectedHorizontalFirst",true},{"horizontalFirst",true},{"selectedObjectId",""},{"selectionKind",""},
        {"selectionCanResize",false},{"selectedShapeWidth",0},{"selectedShapeHeight",0},{"selectedCornerRadius",0},
        {"selectedWireBend",0},{"selectedPorts",QVariantList{}},{"selectionHandlePoints",QVariantList{}},
        {"selectedStencilId",QString{}},{"selectedStencilParameters",QVariantMap{}},
        {"activeStencilId",QString{}},{"recentStencils",QStringList{}},{"stencilVertical",false},{"stencilSupportsVoltage",false},
        {"stencilVoltageArrow",false},{"stencilVoltageReversed",false},{"stencilVoltageOtherSide",false},
        {"selectionSupportsVoltage",false},{"selectedVoltageArrow",false},{"selectedVoltageReversed",false},{"selectedVoltageOtherSide",false}};
    if(available())for(auto i=result.begin();i!=result.end();++i)i.value()=m_page->property(i.key().toLatin1().constData());
    result.insert("supportedTools",QStringList{"select","pen","line","arrow","wire","rectangle","ellipse"});
    result.insert("canPlaceStencils",available());
    result.insert("selectionCanChangeColor",result.value("hasSelection").toBool());
    result.insert("selectionCanConfigureStencil",result.value("hasSelection").toBool()
                  && PaperDrawing::isConfigurableStencil(result.value("selectedStencilId").toString()));
    result.insert("selectionCanChangeWidth",result.value("selectionCanChangeStyle").toBool()
                  ||result.value("selectionCanConfigureStencil").toBool());
    return result;
#endif
}
QVariantList EditorAdapter::stencils() const {
    // The immutable catalogue is shared by page hosts, palettes and quickbars.
    // QVariant/QList copies share this storage until a caller explicitly edits it.
    static const QVariantList catalogue=[] {
    QVariantList result;
    for(const auto &symbol:PaperDrawing::electronicsCatalogue()) {
        QVariantList strokes;
        for(const auto &stroke:symbol.strokes) {
            QVariantList points;
            for(const auto &point:stroke.points)points.append(QVariantMap{{"x",point.x()},{"y",point.y()}});
            strokes.append(QVariantMap{{"points",points},{"width",stroke.width},{"color",stroke.color.name()}});
        }
        result.append(QVariantMap{{"id",symbol.id},{"name",symbol.name},{"strokes",strokes},{"width",120},{"height",80},
                                  {"configurable",PaperDrawing::isConfigurableStencil(symbol.id)}});
    }
    return result;
    }();
    return catalogue;
}
bool EditorAdapter::attachPcPage(QObject *page) {
#ifdef REPAPER_EDITOR_PC_HARNESS
    auto canvas=qobject_cast<InkCanvas*>(page);
    if(!canvas||canvas->thread()!=thread()||QThread::currentThread()!=thread())return false;
    if(m_page)disconnect(m_page,nullptr,this,nullptr);
    m_page=canvas;
    connect(canvas,&InkCanvas::documentChanged,this,&EditorAdapter::notifyStateChanged);
    connect(canvas,&InkCanvas::settingsChanged,this,&EditorAdapter::notifyStateChanged);
    connect(canvas,&QObject::destroyed,this,&EditorAdapter::notifyStateChanged);
    notifyStateChanged();return true;
#else
    Q_UNUSED(page);return false;
#endif
}
bool EditorAdapter::inspectNativeObject(QObject *object) {
    // Metadata only, on the GUI thread. Do not invoke methods, inspect line memory,
    // read arbitrary properties (e.g. QQuickAnchorLine), or retain a foreign QObject.
    if(!object||!QCoreApplication::instance()||QThread::currentThread()!=QCoreApplication::instance()->thread()
       ||object->thread()!=QThread::currentThread())return false;
    const auto meta=object->metaObject();
    const QStringList names{"addDrawingLine","applyPendingEdit","cancelPendingEdit","undo","redo",
        "rotateSelectedItems","scaleSelectedItems","moveSelectedItems","cloneAddAndSelectItems",
        "deleteSelectedItems","deleteSelection","cut","copy","pasteClipboardContent"};
    const QStringList properties{"sceneController","penHandler","viewSelectionRect","sceneSelectionRect","selectionItemCount"};
    QStringList methods;QVariantMap observedProperties;
    for(int i=0;i<meta->methodCount();++i){auto method=meta->method(i);if(names.contains(QString::fromLatin1(method.name())))methods.append(QString::fromLatin1(method.methodSignature()));}
    for(int i=0;i<meta->propertyCount();++i){auto property=meta->property(i);if(properties.contains(QString::fromLatin1(property.name())))observedProperties.insert(QString::fromLatin1(property.name()),QString::fromLatin1(property.typeName()));}
    m_evidence={{"className",QString::fromLatin1(meta->className())},{"methods",methods},{"properties",observedProperties},
        {"metadataOnly",true},{"mutationValidated",false}};
    notifyStateChanged();return true;
}
bool EditorAdapter::act(Action action,const QVariant &argument) {
    if(!available()||QThread::currentThread()!=thread())return false;
#ifdef REPAPER_EDITOR_PC_HARNESS
    auto canvas=qobject_cast<InkCanvas*>(m_page.data());if(!canvas)return false;
    switch(action) {
    case Action::Tool:canvas->setTool(argument.toString());break;
    case Action::Style:
        if(canvas->tool()=="select")canvas->setSelectionLineStyle(argument.toString());else canvas->setLineStyle(argument.toString());break;
    case Action::Width:
        if(canvas->tool()=="select")canvas->setSelectionLineWidth(argument.toDouble());else canvas->setLineWidth(argument.toDouble());break;
    case Action::Direction:canvas->setSelectionArrowDirection(argument.toString());break;
    case Action::Wire:
        if(canvas->tool()=="select")canvas->setSelectionHorizontalFirst(argument.toBool());else canvas->setHorizontalFirst(argument.toBool());break;
    case Action::Stencil:{const int count=canvas->itemCount();canvas->insertSymbol(argument.toString());return canvas->itemCount()==count+1;}
    case Action::Undo:canvas->undo();break;
    case Action::Redo:canvas->redo();break;
    case Action::Rotate:canvas->rotateSelection();break;
    case Action::Scale:canvas->scaleSelection(argument.toDouble());break;
    case Action::Duplicate:canvas->duplicateSelection();break;
    case Action::Remove:canvas->removeSelection();break;
    case Action::Color:
        if(canvas->tool()=="select")return canvas->setSelectionLineColor(argument.toString());
        return canvas->setLineColor(argument.toString());
    }
    return true;
#else
    const QStringList names{"tool","style","width","direction","wire","stencil","undo","redo","rotate","scale","duplicate","remove","color"};
    return m_native->action(names.at(static_cast<int>(action)),argument);
#endif
}
bool EditorAdapter::chooseTool(const QString &v){return act(Action::Tool,v);}
bool EditorAdapter::setStrokeStyle(const QString &v){return act(Action::Style,v);}
bool EditorAdapter::setStrokeWidth(qreal v){return act(Action::Width,v);}
bool EditorAdapter::setStrokeColor(const QString &v){return act(Action::Color,v);}
bool EditorAdapter::setArrowDirection(const QString &v){return act(Action::Direction,v);}
bool EditorAdapter::setWireOrientation(bool v){return act(Action::Wire,v);}
bool EditorAdapter::stencilOption(const char *method,const QString &action,bool value){
#ifdef REPAPER_EDITOR_PC_HARNESS
    if(!m_page)return false;
    bool result=false;
    return QMetaObject::invokeMethod(m_page,method,Qt::DirectConnection,Q_RETURN_ARG(bool,result),Q_ARG(bool,value))&&result;
#else
    Q_UNUSED(method);return m_native->action(action,value);
#endif
}
bool EditorAdapter::setStencilVertical(bool v){return stencilOption("setStencilVertical","stencilVertical",v);}
bool EditorAdapter::setVoltageArrow(bool v){return stencilOption("setVoltageArrow","voltageArrow",v);}
bool EditorAdapter::setVoltageReversed(bool v){return stencilOption("setVoltageReversed","voltageReversed",v);}
bool EditorAdapter::setVoltageOtherSide(bool v){return stencilOption("setVoltageOtherSide","voltageOtherSide",v);}
bool EditorAdapter::insertStencil(const QString &v){return act(Action::Stencil,v);}
bool EditorAdapter::undo(){return act(Action::Undo);}
bool EditorAdapter::redo(){return act(Action::Redo);}
bool EditorAdapter::rotate(){return act(Action::Rotate);}
bool EditorAdapter::scale(qreal v){return act(Action::Scale,v);}
bool EditorAdapter::duplicate(){return act(Action::Duplicate);}
bool EditorAdapter::remove(){return act(Action::Remove);}
bool EditorAdapter::beginPropertiesSession(){
#ifdef REPAPER_EDITOR_PC_HARNESS
    return false;
#else
    return available()&&QThread::currentThread()==thread()&&m_native->beginPropertiesSession();
#endif
}
bool EditorAdapter::acceptPropertiesSession(){
#ifdef REPAPER_EDITOR_PC_HARNESS
    return false;
#else
    return available()&&QThread::currentThread()==thread()&&m_native->acceptPropertiesSession();
#endif
}
bool EditorAdapter::cancelPropertiesSession(){
#ifdef REPAPER_EDITOR_PC_HARNESS
    return false;
#else
    return available()&&QThread::currentThread()==thread()&&m_native->cancelPropertiesSession();
#endif
}
bool EditorAdapter::attachNativePage(QObject *controller,QObject *view,QObject *host){
#ifdef REPAPER_EDITOR_PC_HARNESS
    Q_UNUSED(controller);Q_UNUSED(view);Q_UNUSED(host);return false;
#else
    const bool result=m_native->attach(controller,view,host);m_evidence=m_native->evidence();return result;
#endif
}
void EditorAdapter::refreshNativeState(bool objectsMayHaveChanged){m_native->refresh(objectsMayHaveChanged);}
void EditorAdapter::nativeAreaSelected(int layer,const QRectF &rect){m_native->nativeAreaSelected(layer,rect);}
void EditorAdapter::nativeContentChanged(){m_native->nativeContentChanged();}
void EditorAdapter::nativeViewTransformChanged(){m_native->nativeViewTransformChanged();}
void EditorAdapter::nativePreviewPresented(qulonglong token){m_native->nativePreviewPresented(token);}
void EditorAdapter::setNativeToolActive(bool active){m_native->setNativeToolActive(active);}
QVariantList EditorAdapter::previewStrokes() const{return m_native->previewStrokes();}
bool EditorAdapter::captureEnabled() const{return m_native->captureEnabled();}
bool EditorAdapter::pointerBegin(qreal x,qreal y,const QString &kind){return m_native->pointerBegin(x,y,kind);}
bool EditorAdapter::pointerMove(qreal x,qreal y){return m_native->pointerMove(x,y);}
bool EditorAdapter::pointerMoveBatch(const QVariantList &points){return m_native->pointerMoveBatch(points);}
bool EditorAdapter::pointerEnd(qreal x,qreal y){return m_native->pointerEnd(x,y);}
void EditorAdapter::pointerCancel(){m_native->pointerCancel();}
bool EditorAdapter::beginStencil(const QString &id){
#ifdef REPAPER_EDITOR_PC_HARNESS
    if(auto page=qobject_cast<InkCanvas*>(m_page.data()))return page->beginStencil(id);
    return false;
#else
    return m_native->action("stencil",id);
#endif
}
QVariantList EditorAdapter::stencilSchema(const QString &id) const { return PaperDrawing::stencilParameterSchema(id); }
QVariantMap EditorAdapter::stencilDefaults(const QString &id) const { return PaperDrawing::stencilDefaults(id); }
QVariantMap EditorAdapter::stencilPreview(const QString &id, const QVariantMap &parameters) const {
    QVariantMap normalized; QString error;
    if (!PaperDrawing::normalizeStencilParameters(id, parameters, &normalized, &error))
        return {{"valid",false},{"error",error}};
    repaper::drawing::Item item; item.kind="symbol"; item.symbolId=id; item.stencilParameters=normalized;
    item.width=2.3; item.sourcePoints={{0,0},{120,0},{0,80}};
    if (!repaper::drawing::rebuild(item)) return {{"valid",false},{"error","Ces paramètres ne peuvent pas être tracés."}};
    QVariantList strokes;
    for (const auto &stroke:item.strokes) {
        QVariantList points;
        for (auto p:stroke.points) points.append(QVariantMap{{"x",p.x()},{"y",p.y()}});
        strokes.append(QVariantMap{{"points",points},{"width",stroke.width},{"color",stroke.color.name()}});
    }
    return {{"valid",true},{"strokes",strokes},{"width",120},{"height",80},{"parameters",normalized}};
}
bool EditorAdapter::beginConfiguredStencil(const QString &id,const QVariantMap &parameters) {
    if (!available() || QThread::currentThread()!=thread()) return false;
#ifdef REPAPER_EDITOR_PC_HARNESS
    if (auto page=qobject_cast<InkCanvas*>(m_page.data())) return page->beginConfiguredStencil(id,parameters);
    return false;
#else
    return m_native->action("stencil",QVariantMap{{"id",id},{"parameters",parameters}});
#endif
}
bool EditorAdapter::setStencilParameters(const QVariantMap &parameters) {
    if (!available() || QThread::currentThread()!=thread()) return false;
#ifdef REPAPER_EDITOR_PC_HARNESS
    if (auto page=qobject_cast<InkCanvas*>(m_page.data())) return page->setSelectedStencilParameters(parameters);
    return false;
#else
    return m_native->action("stencilParameters",parameters);
#endif
}
bool EditorAdapter::resize(qreal w,qreal h){
#ifdef REPAPER_EDITOR_PC_HARNESS
    if(auto page=qobject_cast<InkCanvas*>(m_page.data()))return page->resizeSelection(w,h);
#else
    return m_native->resizeSelection(w,h);
#endif
    return false;
}
bool EditorAdapter::setCornerRadius(qreal v){
#ifdef REPAPER_EDITOR_PC_HARNESS
    if(auto page=qobject_cast<InkCanvas*>(m_page.data()))return page->setSelectionCornerRadius(v);
#else
    return m_native->action("cornerRadius",v);
#endif
    return false;
}
bool EditorAdapter::setWireBend(qreal v){
#ifdef REPAPER_EDITOR_PC_HARNESS
    if(auto page=qobject_cast<InkCanvas*>(m_page.data()))return page->setSelectionWireBend(v);
#else
    return m_native->action("wireBend",v);
#endif
    return false;
}
bool EditorAdapter::selectObject(const QString &id){
#ifdef REPAPER_EDITOR_PC_HARNESS
    if(auto page=qobject_cast<InkCanvas*>(m_page.data()))return page->selectObject(id);
#else
    Q_UNUSED(id);
#endif
    return false;
}

#include "NativeScene.h"
#include "TargetProfile.h"
#include "NativeDiagnostics.h"
#include "NativeSelectionProbe.h"
#include "NativeLineFactory.h"
#include "NativeStrokeSampling.h"
#include "NativeStrokeColor.h"
#include "NativeSelectionStyle.h"
#include "NativeSceneObserver.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QMetaType>
#include <QThread>
#include <QDebug>
#include <QTransform>
#include <QQuickItem>
#include <QLineF>
#include <QStandardPaths>
#include <QSettings>
#include <cmath>
#include <utility>
#ifdef REPAPER_WITH_NATIVE_ABI
#include "rm_Line.hpp"
#include "rm_SceneItem.hpp"
#include <cstring>
#include <memory>
#include <new>
#include <typeinfo>
#endif

namespace {
int stencilQuarterTurns(const QString &id,bool vertical){
    if(id=="square-root"||PaperDrawing::isConfigurableStencil(id))return 0;
    for(const auto &symbol:PaperDrawing::electronicsCatalogue())if(symbol.id==id&&symbol.anchors.size()==2){
        const auto axis=symbol.anchors[1]-symbol.anchors[0];
        const bool nativeVertical=std::abs(axis.y())>std::abs(axis.x());
        return nativeVertical!=vertical?1:0;
    }
    return vertical?1:0;
}
bool contextualWorking(const QVariantMap &context){return context.value("working",true).toBool();}
bool sameThread(const QObject *object,const QObject *owner) { return object&&object->thread()==owner->thread()&&QThread::currentThread()==owner->thread(); }
bool samePage(const QVariantMap &a,const QVariantMap &b) {
    return !a.value("documentId").toString().isEmpty()&&!a.value("pageId").toString().isEmpty()
        &&a.value("documentId")==b.value("documentId")&&a.value("pageId")==b.value("pageId")
        &&a.contains("layer")&&a.value("layer")==b.value("layer");
}
QRectF selectionGeometryRect(const QVector<PaperDrawing::Stroke> &strokes,const QTransform &transform={}){
    qreal left=0,right=0,top=0,bottom=0;bool first=true;
    for(const auto &stroke:strokes)for(const auto &source:stroke.points){
        const auto point=transform.map(source);
        if(first){left=right=point.x();top=bottom=point.y();first=false;}
        else {left=qMin(left,point.x());right=qMax(right,point.x());top=qMin(top,point.y());bottom=qMax(bottom,point.y());}
    }
    if(first)return {};
    // Handles and dimensions describe geometry, independent of fixed pen width.
    // Keep a one-unit interaction extent only on a degenerate line axis.
    if(right-left<1){const auto middle=(left+right)/2;left=middle-0.5;right=middle+0.5;}
    if(bottom-top<1){const auto middle=(top+bottom)/2;top=middle-0.5;bottom=middle+0.5;}
    return {QPointF(left,top),QPointF(right,bottom)};
}
#ifdef REPAPER_WITH_NATIVE_ABI
constexpr qsizetype NativeLineOffset=0x48;
constexpr qsizetype NativeSceneLineSize=0xb0;
static_assert(sizeof(Line)==0x58);
static_assert(sizeof(LinePoint)==0xe);
static_assert(offsetof(Line,points)==0x10&&offsetof(Line,bounds)==0x38);
bool nativeLineClone(const SceneItem *item) {
    if(!item||!item->vtable)return false;
    const auto table=static_cast<void* const*>(item->vtable);
    const auto rtti=static_cast<const std::type_info*>(table[-1]);
    if(!rtti)return false;
    const QByteArray name(rtti->name());
    return name=="13SceneLineItem"||name=="*13SceneLineItem";
}
#endif
QStringList signatures(QObject *object) {
    const QStringList allow{"addDrawingLine","cloneSelectedItems","cloneAddAndSelectItems","clearSelectedItems","deleteSelectedItems",
        "moveSelectedItems","scaleSelectedItems","rotateSelectedItems","applyPendingEdit","cancelPendingEdit","undo","redo"};
    QStringList result;
    if(object)for(int i=0;i<object->metaObject()->methodCount();++i){
        const auto method=object->metaObject()->method(i);
        const QString name=QString::fromLatin1(method.name());
        bool matches=allow.contains(name);
        for(const auto part:{"item","select","layer","page","scene","edit","document"})matches=matches||name.contains(part,Qt::CaseInsensitive);
        if(matches)result.append(QString::fromLatin1(method.methodSignature()));
    }
    return result;
}
QVariantList properties(QObject *object) {
    QVariantList result;
    if(object)for(int i=0;i<object->metaObject()->propertyCount();++i){
        const auto property=object->metaObject()->property(i);
        const QString name=QString::fromLatin1(property.name());
        bool matches=false;
        for(const auto part:{"item","select","layer","page","scene","edit","document"})matches=matches||name.contains(part,Qt::CaseInsensitive);
        // Metadata only: never invoke a property's getter or read its value.
        if(matches)result.append(QVariantMap{{"name",name},{"type",QString::fromLatin1(property.typeName())}});
    }
    return result;
}
}
NativeScene::NativeScene(QObject *parent,NativeObjectAccess *objectAccess,QString bindingsDirectory,NativeSelectionColor *colorAccess):QObject(parent),m_touchGuard(new NativeTouchGuard(this)) {
    // Invalidate before any external listener consumes the next immutable
    // packet. A semantic change also changes the preview; a pen move does not
    // broadcast a semantic/menu change in the opposite direction.
    connect(this,&NativeScene::previewChanged,this,[this]{m_previewFrameDirty=true;});
    connect(this,&NativeScene::changed,this,&NativeScene::previewChanged);
    m_aspectClock.start();m_aspectTimer.setSingleShot(true);m_aspectTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_aspectTimer,&QTimer::timeout,this,[this]{checkAspectHold(m_aspectClock.elapsed());});
    m_objectAccess=objectAccess?objectAccess:new NativeObjectAccess(this);
    if(bindingsDirectory.isEmpty())bindingsDirectory=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/repaper/native-objects";
    m_objectBindings=std::make_unique<RePaperNative::NativeObjectBindings>(bindingsDirectory);
    connect(m_objectAccess,&NativeObjectAccess::finished,this,&NativeScene::objectAccessFinished);
    m_colorEdit=colorAccess?colorAccess:new NativeSelectionColor(this);
    connect(m_colorEdit,&NativeSelectionColor::statusChanged,this,[this]{m_message=m_colorEdit->reason();emit changed();});
    connect(m_colorEdit,&NativeSelectionColor::finished,this,[this](bool success,const QString &message){
        if(!samePage(m_selectionContext,call("readState").toMap()))return;
        if(m_propertiesMutationPending){
            if(success&&RePaperNative::sameNativeHistory(m_propertiesMutationBaseline.history,m_colorEdit->beforeHistory())
                &&RePaperNative::nativeHistoryAppended(m_colorEdit->beforeHistory(),m_colorEdit->afterHistory())){
                m_propertiesColorHistory=m_colorEdit->afterHistory();inspectObjects(ObjectPhase::PropertiesColorObserve);
            }else {
                invalidatePropertiesSession("La modification de couleur n’a pas confirmé son historique.");
                if(m_selectionCleanupPending)checkSelectionObservation();else refresh();
            }
        }else if(m_selectionCleanupPending)checkSelectionObservation();else refresh();
        m_message=message;
        m_evidence.insert("nativeSelectionColorObserved",success);
        qInfo().noquote()<<"[RePaper native color]"<<"completed="<<success<<"reason="<<message;
        emit changed();
    });
    m_gesture.tool="line";
    m_gesture.arrowDirection="start"; // The head stays at the first press; the tail follows the pointer.
#ifdef REPAPER_WITH_NATIVE_ABI
    QSettings preferences("RePaper","editor");
    m_stencilVertical=preferences.value("stencil/vertical",false).toBool();
    m_gesture.voltageArrow=preferences.value("stencil/voltageArrow",false).toBool();
    m_gesture.voltageArrowReversed=preferences.value("stencil/voltageReversed",false).toBool();
    m_gesture.voltageArrowOtherSide=preferences.value("stencil/voltageOtherSide",false).toBool();
    const auto recent=preferences.value("stencil/recent").toStringList();
    if(!recent.isEmpty()&&recent.size()<=4)m_recentStencils=recent;
#endif
    connect(m_touchGuard,&NativeTouchGuard::cancelRequested,this,&NativeScene::pointerCancel);
    m_confirmationTimer.setSingleShot(true);m_confirmationTimer.setInterval(50);
    connect(&m_confirmationTimer,&QTimer::timeout,this,&NativeScene::checkInsertionObservation);
    m_previewHandoverTimer.setSingleShot(true);m_previewHandoverTimer.setInterval(1500);
    connect(&m_previewHandoverTimer,&QTimer::timeout,this,[this]{
        // The native command is already settled. A hidden/stalled window must
        // not keep the input lock forever while waiting for a display frame.
        m_evidence.insert("previewHandoverFrameTimedOut",true);
        nativePreviewPresented(m_previewToken);
    });
    m_selectionTimer.setSingleShot(true);m_selectionTimer.setInterval(50);
    connect(&m_selectionTimer,&QTimer::timeout,this,&NativeScene::checkSelectionObservation);
}
NativeScene::~NativeScene(){
    cancelObjectAccess();m_bindingsStopping.store(true);
    if(m_bindingsWorker){
        // Drain accepted registration jobs before destroying their owner: an
        // inserted native object must retain its binding even when a page is
        // closed immediately. Workers never wait for a GUI response.
        QMetaObject::invokeMethod(m_bindingsWorker,[]{QThread::currentThread()->quit();},Qt::QueuedConnection);
        m_bindingsThread.wait();
    }
}
void NativeScene::saveDrawingPreferences() const {
#ifdef REPAPER_WITH_NATIVE_ABI
    QSettings preferences("RePaper","editor");
    preferences.setValue("stencil/vertical",m_stencilVertical);
    preferences.setValue("stencil/voltageArrow",m_gesture.voltageArrow);
    preferences.setValue("stencil/voltageReversed",m_gesture.voltageArrowReversed);
    preferences.setValue("stencil/voltageOtherSide",m_gesture.voltageArrowOtherSide);
    preferences.setValue("stencil/recent",m_recentStencils);
#endif
}
bool NativeScene::attach(QObject *controller,QObject *view,QObject *host) {
    const auto attachmentGeneration=++m_attachmentGeneration;
    cancelObjectAccess();
    m_colorEdit->cancel();
    pointerCancel();
    clearCustomSelection();
    discardPendingPreview();
    delete m_sceneObserver;m_sceneObserver=nullptr;
    m_touchGuard->attach(nullptr);m_confirmationTimer.stop();
    for(auto object:{m_controller,m_view,m_host})if(object)disconnect(object,nullptr,this,nullptr);
    m_target=false;m_nativeToolActive=false;m_insertionObservation.clear();m_pageContext.clear();m_insertionContext.clear();m_controller.clear();m_view.clear();m_host.clear();
    if(!sameThread(controller,this)||!sameThread(view,this)||!sameThread(host,this))return false;
    m_controller=controller;m_view=view;m_host=host;m_selection={};m_selectionCount=0;
    m_touchGuard->attach(qobject_cast<QQuickItem*>(view));
    m_target=RePaperNative::matchesRunningXochitl();
    m_lineType=false;m_batchType=false;
    const QMetaType lineType=QMetaType::fromName("Line");
#ifdef REPAPER_WITH_NATIVE_ABI
    m_lineType=lineType.isValid()&&lineType.sizeOf()==sizeof(Line);
#endif
    m_coordinates=call("coordinateMappingAvailable").toBool();
    m_evidence={{"className",QString::fromLatin1(controller->metaObject()->className())},{"methods",signatures(controller)},
        {"properties",properties(controller)},
        {"exactTarget",m_target},{"lineMetaTypeSize",lineType.sizeOf()},
        {"coordinatesAvailable",m_coordinates},{"nativeIdentityValidated",false},{"atomicUndoValidated",false}};
    m_evidence.insert("probeVersion","0.8.2");
    m_evidence.insert("nativePersistentEditingAvailable",m_persistentEditingValidated);
    m_evidence.insert("singleBatchUndoStaticEvidence",true);
#ifdef REPAPER_WITH_NATIVE_ABI
    m_evidence.insert("compiledLayout",QVariantMap{{"lineSize",int(sizeof(Line))},
        {"sceneItemSize",int(sizeof(SceneItem))},{"attestedSceneLineItemSize",NativeSceneLineSize},
        {"attestedLineOffset",NativeLineOffset},{"lineSubobjectStaticEvidence",true},
        {"nativeIdentityValidated",false}});
#endif
    // Deliberately print metadata only: never document IDs, geometry or clipboard contents.
    qInfo().noquote()<<"[RePaper native]"<<QJsonDocument::fromVariant(m_evidence).toJson(QJsonDocument::Compact);
    for(auto object:{controller,view,host})connect(object,&QObject::destroyed,this,[this,attachmentGeneration]{
        if(attachmentGeneration!=m_attachmentGeneration)return;
        ++m_attachmentGeneration;
        cancelObjectAccess();
        m_colorEdit->cancel();
        pointerCancel();clearCustomSelection();m_confirmationTimer.stop();m_insertionObservation.clear();discardPendingPreview();
        delete m_sceneObserver;m_sceneObserver=nullptr;
        m_controller.clear();m_view.clear();m_host.clear();m_selection={};emit changed();
    });
    m_pageContext=call("readState").toMap();
    m_lastPublishedContext=m_pageContext;
    m_message.clear();emit changed();return m_target;
}
void NativeScene::attachSceneObserver(){
    delete m_sceneObserver;m_sceneObserver=nullptr;
    if(m_target){
        m_sceneObserver=new NativeSceneObserver(m_controller,this);
        connect(m_sceneObserver,&NativeSceneObserver::contentChanged,this,&NativeScene::nativeContentChanged);
        connect(m_sceneObserver,&NativeSceneObserver::readinessChanged,this,&NativeScene::changed);
        m_sceneObserver->tryAttach();
    }
}
QString NativeScene::creationReadinessReason() const {
    return creationReadinessReason(call("readState").toMap());
}
QString NativeScene::creationReadinessReason(const QVariantMap &context) const {
    if(!m_target)return "Modèle, firmware ou binaire non validé.";
    if(!m_lineType||!m_coordinates||!m_controller||!m_view||!m_host)return "Contrat des traits ou coordonnées natives indisponible.";
    if(selectionOperationPending())return "La sélection attend encore la fin de sa commande native.";
    if(context.value("working",true).toBool())return "Xochitl travaille ; réessayez quand la page est prête.";
    if(context.value("documentId").toString().isEmpty()||context.value("pageId").toString().isEmpty()||!context.contains("layer")||context.value("layer").toInt()<0)
        return "Aucune page ou couche native active.";
    const auto pending=m_controller->property("pendingEdit");
    if(!pending.canConvert<QTransform>()||!pending.value<QTransform>().isIdentity())return "Terminez la transformation de la sélection native avant cet essai.";
    return {};
}
bool NativeScene::available() const {
    if(!m_target||!m_lineType||!m_coordinates||!m_controller||!m_view||!m_host)return false;
    return available(call("readState").toMap());
}
bool NativeScene::available(const QVariantMap &context) const {
    if(!m_target||!m_lineType||!m_coordinates||!m_controller||!m_view||!m_host)return false;
    return !context.value("documentId").toString().isEmpty()&&!context.value("pageId").toString().isEmpty()
        &&context.contains("layer")&&context.value("layer").toInt()>=0;
}
void NativeScene::beginAspectHold(QPointF viewPoint,QPointF paperPoint){
    cancelAspectHold();
    if(!m_gesture.canLockAspectRatio()&&!m_objectGesture.canLockAspectRatio()&&!m_selectionGesture.canLockAspectRatio())return;
    m_aspectViewAnchor=viewPoint;m_aspectPaperPoint=paperPoint;
    if(m_aspectHold.begin(viewPoint,m_aspectClock.elapsed()))m_aspectTimer.start(NativeAspectHold::HoldDurationMs);
}
void NativeScene::cancelAspectHold(){m_aspectTimer.stop();m_aspectHold.reset();}
bool NativeScene::aspectRatioLocked() const {
    return m_aspectHold.locked()&&((m_gesture.active()&&m_gesture.aspectRatioLocked())
        ||(m_objectGesture.active()&&m_objectGesture.aspectRatioLocked())
        ||(m_selectionGesture.active()&&m_selectionGesture.aspectRatioLocked()));
}
void NativeScene::checkAspectHold(qint64 now){
    if(!m_aspectHold.waiting())return;
    if(!captureEnabled()||!available()||!m_touchGuard->acceptsBegin()
        ||(!m_gesture.canLockAspectRatio()&&!m_objectGesture.canLockAspectRatio()&&!m_selectionGesture.canLockAspectRatio())){
        cancelAspectHold();return;
    }
    // A move queued just before the deadline must cancel the hold even when
    // its 16 ms batch has not reached QML yet. Flush all samples before locking.
    if(!call("flushGestureInput").toBool()){cancelAspectHold();return;}
    now=qMax(now,m_aspectClock.elapsed());
    if(!m_aspectHold.waiting())return;
    if(!m_aspectHold.advance(now)){
        if(m_aspectHold.waiting())m_aspectTimer.start(1);
        return;
    }
    m_aspectTimer.stop();
    bool locked=false;
    if(m_objectGesture.canLockAspectRatio())locked=m_objectGesture.lockAspectRatio()&&m_objectGesture.update(m_aspectPaperPoint);
    else if(m_selectionGesture.canLockAspectRatio()){
        locked=m_selectionGesture.lockAspectRatio()&&m_selectionGesture.update(m_aspectPaperPoint);
        if(locked&&!RePaperNative::nativeScaleWidthFits(m_selectionMaximumPointWidth,m_selectionGesture.scaleX(),m_selectionGesture.scaleY())){
            pointerCancel();return;
        }
        if(locked)updateSelectionPreviewBounds();
    }else if(m_gesture.canLockAspectRatio())locked=m_gesture.lockAspectRatio()&&m_gesture.move(m_aspectPaperPoint);
    if(!locked)cancelAspectHold();
    emit previewChanged();
}
void NativeScene::setNativeToolActive(bool active) {
    if(m_nativeToolActive==active)return;
    pointerCancel();
    if(!active&&m_nativeToolActive)requestSelectionCleanup();
    m_nativeToolActive=active;
    if(active&&available()&&!selectionOperationPending()&&!creationOperationPending()){
        if(!m_sceneObserver)attachSceneObserver();
        else if(!m_sceneObserver->ready())m_sceneObserver->tryAttach();
        call("activateCustomTool");refresh();
    }
    emit changed();
}
QString NativeScene::status() const {
    if(m_propertiesSessionActive&&!m_propertiesSessionError.isEmpty())return m_propertiesSessionError;
    if(!m_message.isEmpty())return m_message;
    if(!m_target)return "Éditeur natif : modèle, firmware ou binaire non validé.";
    const auto reason=creationReadinessReason();
    if(!reason.isEmpty())return reason;
    return "Choisissez un outil puis tracez sur la page.";
}
QVariant NativeScene::call(const char *name,const QVariant &a,const QVariant &b) const {
    if(!m_host||!sameThread(m_host,this))return {};
    QVariant result;
    bool ok;
    if(b.isValid())ok=QMetaObject::invokeMethod(m_host,name,Qt::DirectConnection,Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,a),Q_ARG(QVariant,b));
    else if(a.isValid())ok=QMetaObject::invokeMethod(m_host,name,Qt::DirectConnection,Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,a));
    else ok=QMetaObject::invokeMethod(m_host,name,Qt::DirectConnection,Q_RETURN_ARG(QVariant,result));
    return ok?result:QVariant{};
}
QVariantMap NativeScene::state() const {
    auto result=call("readState").toMap();
    const auto readiness=creationReadinessReason(result);
    result.insert("tool",m_gesture.tool);result.insert("lineStyle",m_gesture.style);result.insert("lineWidth",m_gesture.width);
    result.insert("lineColor",m_gesture.color.name());
    result.insert("activeStencilId",m_gesture.symbolId);
    result.insert("recentStencils",m_recentStencils);
    result.insert("stencilVertical",m_stencilVertical);
    result.insert("stencilSupportsVoltage",PaperDrawing::supportsVoltageArrow(m_gesture.symbolId));
    result.insert("stencilVoltageArrow",m_gesture.voltageArrow);
    result.insert("stencilVoltageReversed",m_gesture.voltageArrowReversed);
    result.insert("stencilVoltageOtherSide",m_gesture.voltageArrowOtherSide);
    QString selectedColor=m_selectedStrokes.isEmpty()?QString():m_selectedStrokes.first().color.name();
    for(const auto &stroke:m_selectedStrokes)if(stroke.color.name()!=selectedColor){selectedColor.clear();break;}
    result.insert("selectedLineColor",selectedColor);
    result.insert("maximumStrokeWidth",4);
    result.insert("horizontalFirst",m_gesture.horizontalFirst);result.insert("arrowDirection",m_gesture.arrowDirection);
    result.insert("supportedTools",QStringList{"select","pen","line","arrow","wire","rectangle","ellipse"});
    const bool selected=m_nativeToolActive&&m_gesture.tool=="select"&&!m_selectionCleanupPending&&m_objectSelectionVerified&&!m_selectedStrokes.isEmpty()&&m_coordinates
        &&!(m_selectionWaiting&&m_affineReceipt.valid()&&m_affineReceipt.expectedSelectionCount()==0);
    const bool editable=selected&&!selectionOperationPending()&&!contextualWorking(result)&&!creationOperationPending()&&propertiesMutationAllowed();
    result.insert("canPlaceStencils",available(result));result.insert("hasSelection",selected);
    const bool semantic=selected&&m_hasObjectModel;
    const auto &model=displayedObjectModel();
    const bool configurable=semantic&&model.kind=="symbol"&&PaperDrawing::isConfigurableStencil(model.symbolId);
    result.insert("selectedStencilId",configurable?model.symbolId:QString{});
    result.insert("selectedStencilParameters",configurable?model.stencilParameters:QVariantMap{});
    result.insert("selectionCanConfigureStencil",configurable&&editable);
    result.insert("selectionSupportsVoltage",semantic&&editable&&model.kind=="symbol"&&PaperDrawing::supportsVoltageArrow(model.symbolId));
    result.insert("selectedVoltageArrow",semantic&&model.voltageArrow);
    result.insert("selectedVoltageReversed",semantic&&model.voltageArrowReversed);
    result.insert("selectedVoltageOtherSide",semantic&&model.voltageArrowOtherSide);
    if(configurable){
        const auto color=repaper::drawing::foregroundColor(model);
        result.insert("selectedLineColor",color.isValid()?color.name():QString{});
    }
    result.insert("selectionHasEndpoints",semantic&&repaper::drawing::hasEndpoints(model));
    result.insert("selectionCanChangeStyle",semantic&&editable&&!configurable);
    result.insert("selectionCanChangeWidth",semantic&&editable);
    result.insert("selectionIsArrow",semantic&&model.kind=="arrow");
    result.insert("selectionIsWire",semantic&&model.kind=="wire");
    result.insert("selectedLineStyle",semantic?model.style:QString());
    result.insert("selectedLineWidth",semantic?model.width:0);
    result.insert("selectedArrowDirection",model.arrowDirection);
    result.insert("selectedHorizontalFirst",model.horizontalFirst);
    result.insert("selectedWireBend",model.wireBend);
    result.insert("selectedCornerRadius",model.cornerRadius);
    result.insert("nativeIdentityValidated",false);result.insert("atomicUndoValidated",false);
    result.insert("nativeCreationInFlight",creationOperationPending());
    result.insert("nativeCreationReason",readiness);
    result.insert("nativeToolActive",m_nativeToolActive);
    if(creationOperationPending()||selectionOperationPending()||m_propertiesSessionActive){result.insert("canUndo",false);result.insert("canRedo",false);}
    result.insert("propertiesSessionActive",m_propertiesSessionActive);
    result.insert("propertiesSessionCanCancel",m_propertiesSessionActive&&m_propertiesSessionValid&&!selectionOperationPending()&&!creationOperationPending());
    result.insert("propertiesSessionCanAccept",!selectionOperationPending()&&!creationOperationPending());
    result.insert("propertiesSessionCanEdit",m_propertiesSessionActive&&propertiesMutationAllowed()&&!selectionOperationPending()&&!creationOperationPending());
    result.insert("propertiesSessionCancelPending",m_propertiesCancelPending);
    result.insert("propertiesCancelGeneration",QVariant::fromValue(m_propertiesCancelGeneration));
    result.insert("propertiesSessionError",m_propertiesSessionError);
    result.insert("nativePersistentEditingAvailable",m_persistentEditingValidated);
    result.insert("selectionCanTransform",editable);result.insert("selectionCanDuplicate",editable);result.insert("selectionCanRemove",editable);
    result.insert("selectionCanResize",selected&&(!semantic||repaper::drawing::hasBox(model)));
    result.insert("selectionCanChangeColor",editable&&samePage(m_selectionContext,result)&&readiness.isEmpty());
    result.insert("selectionKind",semantic?model.kind:QStringLiteral("native-strokes"));
    result.insert("nativeSelectionWaiting",selectionOperationPending());
    result.insert("nativeParametricSelection",semantic);
    result.insert("nativeSelectionInkOverlay",selectionInkOverlay());
    result.insert("nativeObjectBindingsReady",!m_objectCacheDirty&&m_objectBindings->ready());
    result.insert("nativeBoundObjectCount",m_objectBindings->activeObjectCount());
    const auto overlay=overlayState();
    for(auto it=overlay.cbegin();it!=overlay.cend();++it)result.insert(it.key(),it.value());
    return result;
}
QVariantMap NativeScene::overlayState() const {
    QVariantMap result;
    result.insert("nativeGestureActive",m_gesture.active()||m_regionSelecting||m_selectionGesture.active()||m_objectGesture.active());
    result.insert("aspectRatioLocked",aspectRatioLocked());
    result.insert("aspectRatioHoldPending",m_aspectHold.waiting());
    result.insert("aspectRatioLockAnchor",m_aspectViewAnchor);
    const bool selected=m_nativeToolActive&&m_gesture.tool=="select"&&!m_selectionCleanupPending
        &&m_objectSelectionVerified&&!m_selectedStrokes.isEmpty()&&m_coordinates
        &&!(m_selectionWaiting&&m_affineReceipt.valid()&&m_affineReceipt.expectedSelectionCount()==0);
    const bool semantic=selected&&m_hasObjectModel;
    const auto &model=displayedObjectModel();
    // A pending affine receipt still owns its dimensions even while selection
    // controls are temporarily hidden or await exact native verification.
    const QRectF rect=displayedSelectionRect();
    result.insert("selectedShapeWidth",semantic&&repaper::drawing::hasBox(model)?repaper::drawing::boxWidth(model):rect.width());
    result.insert("selectedShapeHeight",semantic&&repaper::drawing::hasBox(model)?repaper::drawing::boxHeight(model):rect.height());
    const bool stencilSnapped=m_gesture.active()&&m_gesture.stencilPlacement().snapped;
    result.insert("wireSnapActive",m_wireSnap.snapped||stencilSnapped);
    QTransform mapping;qreal scale=1;
    const bool mapped=(selected||stencilSnapped||m_wireSnap.snapped)&&previewTransform(&mapping,&scale);
    if(stencilSnapped&&mapped)result.insert("wireSnapPoint",mapping.map(m_gesture.stencilPlacement().targetPoint));
    else if(m_wireSnap.snapped&&mapped)result.insert("wireSnapPoint",mapping.map(m_wireSnap.point));
    result.insert("customSelectionRect",rect);
    QVariantList handles;
    if(semantic&&mapped)for(const auto &control:NativeObjectGesture::controls(model)){
        const auto point=mapping.map(control.point);
        handles.append(QVariantMap{{"x",point.x()},{"y",point.y()},{"kind",control.kind}});
    }
    else if(selected&&mapped)for(const auto &p:{rect.topLeft(),QPointF(rect.center().x(),rect.top()),rect.topRight(),QPointF(rect.right(),rect.center().y()),rect.bottomRight(),QPointF(rect.center().x(),rect.bottom()),rect.bottomLeft(),QPointF(rect.left(),rect.center().y())}){
        const auto point=mapping.map(p);
        handles.append(QVariantMap{{"x",point.x()},{"y",point.y()},{"kind","resize"}});
    }
    result.insert("selectionHandlePoints",handles);
    QVariantMap rotationPoint;
    if(selected&&mapped){
        auto point=NativeSelectionGesture::rotationHandlePoint(rect,scale);
        if(semantic)point=NativeObjectGesture::rotationPoint(model,scale);
        else if(m_selectionGesture.active()&&m_selectionGesture.handle()==NativeSelectionGesture::Handle::Rotate)
            point=m_selectionGesture.previewTransform().map(NativeSelectionGesture::rotationHandlePoint(m_selectionGesture.startRect(),scale));
        const auto position=mapping.map(point);
        rotationPoint={{"x",position.x()},{"y",position.y()}};
    }
    result.insert("selectionRotationHandle",rotationPoint);
    return result;
}
void NativeScene::refresh(bool objectsMayHaveChanged) {
    if(objectsMayHaveChanged)++m_nativeContentGeneration;
    // Scope follows the active page automatically; it is not a user permission.
    if(!m_target||!m_host)return;
    const auto context=call("readState").toMap();
    bool publish=context!=m_lastPublishedContext;
    m_lastPublishedContext=context;
    if(!samePage(m_pageContext,context)){
        cancelObjectAccess();
        m_colorEdit->cancel();
        m_pageContext=context;m_insertionObservation.clear();m_confirmationTimer.stop();m_gesture.cancel();
        m_message.clear();
        clearCustomSelection();
        discardPendingPreview();
        if(m_nativeToolActive)attachSceneObserver();
        else {delete m_sceneObserver;m_sceneObserver=nullptr;}
        publish=true;
        m_evidence.insert("documentPageFingerprint",RePaperNative::contextFingerprint(context.value("documentId").toString(),context.value("pageId").toString()));
        m_evidence.insert("documentPageFingerprintFormat","sha256:utf8:json-compact:[documentId,pageId]");
    }
    // Clearing an already empty, exact selection cannot change its baseline.
    // This signal is also emitted by native tool activation with no selection.
    if(objectsMayHaveChanged&&m_objectPhase==ObjectPhase::Idle
        &&(!m_objectSnapshot.nativeSelectionExact||!m_objectSnapshot.selectedIds.isEmpty()
           ||m_controller->property("selectionItemCount").toInt()!=0))m_objectCacheDirty=true;
    // SelectionCleared/areaSelected can precede a native affine apply. Keep
    // the baseline and its receipt intact until the real Scene wake-up.
    if(m_objectFaulted&&m_objectPhase==ObjectPhase::Idle){inspectObjects(ObjectPhase::CleanupRead);emit changed();return;}
    if(creationOperationPending()||selectionOperationPending()||m_selectionGesture.active()||m_objectGesture.active()||m_regionSelecting){if(publish)emit changed();return;}
    if(m_nativeToolActive&&m_objectCacheDirty){inspectObjects(ObjectPhase::Refresh);publish=true;}
    if(publish)emit changed();
}
void NativeScene::nativeAreaSelected(int layer,const QRectF &rect) {
    ++m_nativeContentGeneration;
    if(m_objectPhase!=ObjectPhase::Idle)return; // The exact worker receipt owns this operation.
    if(m_previewHandoverPending)return;
    if(!m_insertionObservation.pending()&&m_selectionWaiting){
        const auto context=call("readState").toMap();
        if(!samePage(m_selectionContext,context))return;
        if(context.value("layer",-1).toInt()!=layer)return;
        if(!m_selectionAwaitContent){m_selectedRect=rect;m_selectionSignal=true;checkSelectionObservation();}
        return;
    }
    if(!m_insertionObservation.pending()){m_objectCacheDirty=true;refresh();return;}
    const auto context=call("readState").toMap();
    if(!samePage(m_insertionContext,context)||context.value("layer",-1).toInt()!=layer)return;
    m_areaSelectedAfterDispatch=true;
    checkInsertionObservation();
}
void NativeScene::checkInsertionObservation() {
    if(!m_insertionObservation.pending())return;
    const auto context=call("readState").toMap();
    if(!samePage(m_insertionContext,context)){refresh();return;}
    ++m_observationAttempts;
    if(m_areaSelectedAfterDispatch&&!context.value("working",true).toBool()){
        const auto originals=RePaperNative::observeOriginalSelection(m_controller,context.value("layer",-1).toInt());
        if(m_insertionObservation.observe(context,originals)){
            m_confirmationTimer.stop();m_message.clear();
            m_evidence.insert("postDispatchSelectionObserved",true);
            beginPreviewHandover();
            emit changed();return;
        }
        // Original-ID inspection is diagnostic, not a prerequisite to creating
        // independent strokes. After a native selection event and a settled UI,
        // permit another command even if this optional inspection stays busy.
        // This is not attribution, a semantic binding, or a persistence receipt.
        if(m_observationAttempts>=6&&context.value("queueSize",-1).toInt()==0){
            m_insertionObservation.clear();m_confirmationTimer.stop();
            m_evidence.insert("postDispatchNativeSignalObserved",true);
            m_message.clear();
            beginPreviewHandover();
            emit changed();return;
        }
    }
    if(m_observationAttempts<100)m_confirmationTimer.start();
    else m_message="Xochitl n’a pas encore confirmé la sélection. Le dessin suivant attend sa réponse.";
    emit changed();
}
void NativeScene::discardPendingPreview(){
    m_previewHandoverTimer.stop();++m_previewToken;m_previewHandoverPending=false;m_pendingStrokes.clear();
}
void NativeScene::beginPreviewHandover(){
    m_confirmationTimer.stop();m_previewHandoverPending=true;
    m_evidence.insert("previewHandoverFrameTimedOut",false);m_previewHandoverTimer.start();
    const auto token=++m_previewToken;
    // The host waits for native tiles, requests their repaint, then waits for
    // a window frame submission. This is not a physical-display receipt.
    if(!call("presentCommittedPreview",QVariant::fromValue(token)).toBool())nativePreviewPresented(token);
}
void NativeScene::nativePreviewPresented(qulonglong token){
    if(!m_previewHandoverPending||token!=m_previewToken)return;
    if(!samePage(m_insertionContext,call("readState").toMap())){refresh();return;}
    discardPendingPreview();
    if(m_selectionCleanupPending)checkSelectionObservation();
    else if(m_nativeToolActive&&m_gesture.tool=="select"){m_selectionContext=call("readState").toMap();refresh();}
    emit changed();
}
bool NativeScene::initializeFromSelection(const QVariant &selection) {
    m_selectionCount=0;m_batchType=false;m_selectedStrokes.clear();m_selectionMaximumPointWidth=0;
#ifdef REPAPER_WITH_NATIVE_ABI
    using Items=QList<std::shared_ptr<SceneItem>>;
    // This exact type name and container size are prerequisites, never a guessed QVariant cast.
    const QByteArray name=selection.typeName()?selection.typeName():"";
    m_evidence.insert("selectedItemsMetaType",QString::fromLatin1(name));
    m_evidence.insert("selectedItemsMetaTypeSize",selection.metaType().sizeOf());
    m_evidence.remove("candidateSceneItemIdentity");
    m_evidence.insert("candidateSceneItemIdentities",QVariantList{});
    m_evidence.insert("selectedRuntimeTypes",QStringList{});
    m_evidence.insert("inspectedSceneLineItemCount",0);
    m_evidence.insert("totalSelectedItemCount",0);
    m_evidence.insert("inspectedSelectedItemCount",0);
    m_evidence.insert("candidateIdentitiesTruncated",false);
    m_evidence.insert("probeLimitExceeded",false);
    const auto logProbe=[this]{
        // Full QObject metadata is logged once on attachment. Repeating it on
        // every selection/receipt poll needlessly blocks the GUI's log output.
        QVariantMap summary;
        for(const auto *key:{"probeVersion","selectedItemsMetaType","selectedItemsMetaTypeSize","totalSelectedItemCount",
                "inspectedSelectedItemCount","inspectedSceneLineItemCount","selectedRuntimeTypes","probeLimitExceeded"})
            summary.insert(key,m_evidence.value(key));
        qInfo().noquote()<<"[RePaper native selection probe]"<<QJsonDocument::fromVariant(summary).toJson(QJsonDocument::Compact);
    };
    if(!m_target||name!="QList<std::shared_ptr<SceneItem>>"||selection.metaType().sizeOf()!=sizeof(Items)){logProbe();return false;}
    const auto &items=*static_cast<const Items*>(selection.constData());
    m_evidence.insert("totalSelectedItemCount",items.size());
    if(items.size()>3000){m_evidence.insert("probeLimitExceeded",true);logProbe();return false;}
    m_selectionCount=items.size();m_batchType=true;
    QStringList runtimeTypes;int inspected=0,lineItems=0;qsizetype pointCount=0;
    for(const auto &item:items) {
        if(inspected>=128)break;
        ++inspected;
        if(!item||!item->vtable)continue;
        // Real, retained SceneItem objects supplied by SceneController. Verify the
        // Itanium RTTI class before reading any SceneLineItem fields. No dummy stroke.
        const auto table=static_cast<void* const*>(item->vtable);
        const auto rtti=static_cast<const std::type_info*>(table[-1]);
        if(!rtti)continue;
        const QByteArray typeName(rtti->name());
        if(!runtimeTypes.contains(QString::fromLatin1(typeName)))runtimeTypes.append(QString::fromLatin1(typeName));
        if(typeName=="13SceneLineItem"||typeName=="*13SceneLineItem"){
            ++lineItems;
            if(items.size()<=128){
                const auto *line=std::launder(reinterpret_cast<const Line*>(reinterpret_cast<const unsigned char*>(item.get())+NativeLineOffset));
                if(line->points.size()<2||line->points.size()>200000-pointCount){m_selectedStrokes.clear();return false;}
                PaperDrawing::Stroke stroke;stroke.color=nativeStrokeColor(line->color,line->rgba);
                if(!stroke.color.isValid()){m_selectedStrokes.clear();return false;}
                stroke.width=0;
                for(const auto &p:line->points){
                    m_selectionMaximumPointWidth=qMax(m_selectionMaximumPointWidth,p.width);
                    if(stroke.width==0&&p.width)stroke.width=double(p.width)/4.0;
                    if(!std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>1000000||std::abs(p.y)>1000000){m_selectedStrokes.clear();return false;}
                    stroke.points.append({p.x,p.y});
                }
                pointCount+=line->points.size();m_selectedStrokes.append(std::move(stroke));
            }
        }
        // Exact-target cloneSelectedItems at 0x899250 clears the copied header
        // words at +0x10/+0x18 (store at 0x8994e8). This cloned selection cannot
        // establish original IDs. Do not read them or initialize a write factory.
    }
    m_evidence.insert("selectedRuntimeTypes",runtimeTypes);
    m_evidence.insert("clonedIdentityHeaderClearedByNative",true);
    m_evidence.insert("inspectedSceneLineItemCount",lineItems);
    m_evidence.insert("identityFieldReadSuppressed",true);
    m_evidence.insert("inspectedSelectedItemCount",inspected);
    m_evidence.insert("candidateIdentitiesTruncated",inspected<items.size());
    logProbe();
    if(m_selectedStrokes.size()!=items.size())m_selectedStrokes.clear();
    m_selectedRect=selectionGeometryRect(m_selectedStrokes);
    return !m_selectedStrokes.isEmpty();
#else
    Q_UNUSED(selection);return false;
#endif
}
void NativeScene::clearCustomSelection(){
    m_objectSelectionVerified=false;
    m_hasObjectModel=false;m_objectGesture.cancel();m_objectModel={};m_replacingItem={};m_transformedModel={};
    m_routingPreview.clear();m_replacingDocument.clear();m_replacementSubjects.clear();m_replacingOldGroups.clear();m_replacementFocusIds.clear();
    m_selectionTimer.stop();m_selectionWaiting=false;m_selectionSignal=false;
    m_selectedStrokes.clear();m_selectedRect={};m_selectionCount=0;
    m_selectionMaximumPointWidth=0;
    m_selection={};m_batchType=false;
    m_selectionGesture.cancel();m_regionSelecting=false;
    m_selectionAwaitContent=false;m_selectionCleanupPending=false;m_selectionContext.clear();
    m_affineReceipt.clear();
    m_affineDisplayRect={};
    m_selectionPreviewRect={};
}
void NativeScene::requestSelectionCleanup(){
    m_selectionGesture.cancel();m_objectGesture.cancel();m_regionSelecting=false;
    if(!selectionOperationPending()&&m_selectedStrokes.isEmpty()&&m_selectionCount==0){clearCustomSelection();return;}
    // Changing tools hides the overlay immediately but cannot cancel an
    // already queued native command. Clear its native selection only after
    // its event and receipt have completed, on this exact page and layer.
    m_selectionCleanupPending=true;
    if(!m_selectionWaiting)m_selectionAttempts=0;
    checkSelectionObservation();
}
void NativeScene::nativeContentChanged(){
    ++m_nativeContentGeneration;
    // The next consumer performs one fresh worker inspection. Do not release
    // and rebuild every cached model on each native stroke/queued wake-up.
    m_objectCacheDirty=true;
    if(m_objectPhase!=ObjectPhase::Idle)return;
    if(creationOperationPending())return;
    if(!m_selectionWaiting||!m_selectionAwaitContent){
        if(m_selectionCleanupPending)checkSelectionObservation();
        else if(m_nativeToolActive){if(!m_gesture.active()){pointerCancel();refresh();}}
        return;
    }
    // A verified Scene QObject sends this queued wake-up after an in-memory
    // mutation. Geometry is still checked below: it is not command attribution.
    m_selectionSignal=true;checkSelectionObservation();
}
void NativeScene::checkSelectionObservation(){
    if(m_objectPhase!=ObjectPhase::Idle)return;
    if(m_objectFaulted)return;
    if(!selectionOperationPending())return;
    const auto context=call("readState").toMap();
    if(!m_controller||!m_host||!samePage(context,m_selectionContext)){
        clearCustomSelection();emit changed();return;
    }
    ++m_selectionAttempts;
    if(m_colorEdit->busy())return; // Its exact job completion owns this lock.
    if(creationOperationPending()){
        if(m_selectionAttempts<100)m_selectionTimer.start();
        return;
    }
    if(!m_selectionWaiting&&m_selectionCleanupPending){
        if(m_objectSelectionVerified){inspectObjects(ObjectPhase::CleanupRead);return;}
        // There is no outstanding native operation here: either its receipt
        // has matched or the user is dismissing an idle selection. A busy
        // controller can postpone cleanup, but cannot release our input lock.
        if(!contextualWorking(context)&&context.value("queueSize",-1).toInt()==0&&call("clearCustomSelection").toBool()){
            clearCustomSelection();m_message.clear();emit changed();return;
        }
        if(m_selectionAttempts<100)m_selectionTimer.start();
        else m_message="La page doit encore libérer la sélection native.";
        emit changed();return;
    }
    if(m_selectionSignal&&!contextualWorking(context)&&context.value("queueSize",-1).toInt()==0){
        if(m_selectionAwaitContent){
            const auto snapshot=call("cloneSelection");
            const auto oldStrokes=m_selectedStrokes;const auto oldRect=m_selectedRect;const int oldCount=m_selectionCount;
            const auto oldMaximumWidth=m_selectionMaximumPointWidth;
            initializeFromSelection(snapshot);
            const int nativeCount=m_controller->property("selectionItemCount").toInt();
            const bool matches=m_affineReceipt.matches(m_selectedStrokes,nativeCount);
            if(!matches){m_selectedStrokes=oldStrokes;m_selectedRect=oldRect;m_selectionCount=oldCount;m_selectionMaximumPointWidth=oldMaximumWidth;
                if(m_selectionAttempts<100)m_selectionTimer.start();
                else m_message="Le résultat de la modification n’est pas encore confirmé.";
                emit changed();return;
            }
            m_selectionAwaitContent=false;m_affineReceipt.clear();
        }
        m_selectionWaiting=false;m_selectionTimer.stop();
        if(m_selectionCleanupPending){m_selectionAttempts=0;checkSelectionObservation();return;}
        refresh();
        if(m_sceneObserver&&!m_sceneObserver->ready())m_sceneObserver->tryAttach();
        m_message=m_selectedStrokes.isEmpty()?"Aucun trait sélectionné. Touchez un trait ou encadrez plusieurs traits."
            :"Déplacez les carrés pour redimensionner, ou le rond au-dessus pour tourner.";
        emit changed();return;
    }
    if(m_selectionAttempts<100)m_selectionTimer.start();
    else {
        // The working property is only a delayed spinner. Even a region
        // selection may still be queued after this bounded polling period.
        // Its later native callback remains able to finish the operation.
        m_message=m_selectionAwaitContent?"La modification attend encore la réponse de la page.":"La sélection attend encore la réponse de la page.";
        emit changed();
    }
}
bool NativeScene::transformSelection(const QVariantMap &change){
    const auto context=call("readState").toMap();
    if(!available()||!m_nativeToolActive||m_gesture.tool!="select"||m_selectedStrokes.isEmpty()
        ||selectionOperationPending()||creationOperationPending()||contextualWorking(context)
        ||!samePage(context,m_selectionContext)||!m_objectSelectionVerified
        ||!creationReadinessReason().isEmpty()||!propertiesMutationAllowed())return false;
    if(!m_hasObjectModel&&QStringList{"move","rotate","scale"}.contains(change.value("kind").toString())){
        repaper::drawing::Document subjects;QSet<quint64> covered;
        const QSet<quint64> selected(m_objectSnapshot.selectedIds.begin(),m_objectSnapshot.selectedIds.end());
        for(const auto &model:m_objectBindings->documentModels()){
            const auto ids=m_objectBindings->idsForObject(model.id);bool complete=!ids.isEmpty();
            for(const auto id:ids)if(!selected.contains(id)){complete=false;break;}
            if(complete){subjects.append(model);for(auto id:ids)covered.insert(id);}
        }
        if(!subjects.isEmpty()&&covered==selected){
            NativeAffineReceipt validation;
            if(!validation.begin(m_selectedStrokes,change))return false;
            if(validation.isNoop())return true;
            QTransform transform;const auto kind=change.value("kind").toString();
            if(kind=="move"){const auto delta=change.value("delta").toPointF();transform.translate(delta.x(),delta.y());}
            else {
                const auto anchor=change.value("anchor").toPointF();transform.translate(anchor.x(),anchor.y());
                if(kind=="rotate")transform.rotate(change.value("angle").toDouble());
                else transform.scale(change.value("sx").toDouble(),change.value("sy").toDouble());
                transform.translate(-anchor.x(),-anchor.y());
            }
            for(auto &subject:subjects)if(!NativeObjectGesture::transformGeometry(subject,transform))return false;
            return replaceObjectModels(subjects);
        }
    }
    if(m_hasObjectModel&&QStringList{"scale","move","rotate"}.contains(change.value("kind").toString())){
        const auto kind=change.value("kind").toString();QTransform transform;
        if(kind=="move"){
            const auto delta=change.value("delta").toPointF();transform.translate(delta.x(),delta.y());
        }else{
            const auto anchor=change.value("anchor").toPointF();transform.translate(anchor.x(),anchor.y());
            if(kind=="scale"){
                const auto sx=change.value("sx").toDouble(),sy=change.value("sy").toDouble();
                if(!std::isfinite(sx)||!std::isfinite(sy)||sx<=0||sy<=0||sx>10000||sy>10000)return false;
                transform.scale(sx,sy);
            }else transform.rotate(change.value("angle").toDouble());
            transform.translate(-anchor.x(),-anchor.y());
        }
        auto item=m_objectModel;
        return NativeObjectGesture::transformGeometry(item,transform)&&replaceObjectModel(std::move(item));
    }
    if(change.value("kind").toString()=="scale"&&!RePaperNative::nativeScaleWidthFits(m_selectionMaximumPointWidth,change.value("sx").toDouble(),change.value("sy").toDouble())){
        m_message="Cette échelle dépasserait la largeur de trait prise en charge.";emit changed();return false;
    }
    if(!m_affineReceipt.begin(m_selectedStrokes,change)){m_message=m_affineReceipt.error();emit changed();return false;}
    if(m_affineReceipt.isNoop()){m_affineReceipt.clear();emit changed();return true;}
    m_transformedModel={};
    if(m_hasObjectModel){
        auto item=m_objectModel;QTransform transform;const auto kind=change.value("kind").toString();
        if(kind=="rotate"){
            const auto anchor=change.value("anchor").toPointF();
            transform.translate(anchor.x(),anchor.y());transform.rotate(change.value("angle").toDouble());transform.translate(-anchor.x(),-anchor.y());
        }else if(kind=="move"||kind=="duplicate"){
            const auto delta=kind=="duplicate"?QPointF(24,24):change.value("delta").toPointF();transform.translate(delta.x(),delta.y());
        }
        if(kind!="remove"&&NativeObjectGesture::transformGeometry(item,transform))m_transformedModel=std::move(item);
    }
    m_affineDisplayRect={};
    if(m_affineReceipt.expectedSelectionCount()>0)m_affineDisplayRect=selectionGeometryRect(m_affineReceipt.expected());
    m_selectionWaiting=true;m_selectionSignal=false;m_selectionAttempts=0;m_selectionAwaitContent=true;
    m_pendingTransform=change;
    const bool ok=inspectObjects(ObjectPhase::AffineRead);
    emit changed();return ok;
}
bool NativeScene::resizeSelection(qreal width,qreal height){
    if(m_hasObjectModel&&repaper::drawing::hasBox(m_objectModel)){
        auto item=m_objectModel;pointerCancel();
        return NativeObjectGesture::resizeBox(item,width,height)&&replaceObjectModel(std::move(item));
    }
    if(!std::isfinite(width)||!std::isfinite(height)||width<1||height<1||width>10000||height>10000
        ||m_selectedRect.width()<0.1||m_selectedRect.height()<0.1)return false;
    pointerCancel();
    return transformSelection({{"kind","scale"},{"anchor",m_selectedRect.topLeft()},
        {"sx",width/m_selectedRect.width()},{"sy",height/m_selectedRect.height()}});
}
bool NativeScene::recolorSelection(const QColor &color){
    const auto context=call("readState").toMap();
    if(!color.isValid()||color.alpha()!=255||!available()||!m_nativeToolActive||m_gesture.tool!="select"
        ||m_selectedStrokes.isEmpty()||selectionOperationPending()||creationOperationPending()
        ||!samePage(m_selectionContext,context)||!propertiesMutationAllowed())return false;
    const auto readiness=creationReadinessReason();
    if(!readiness.isEmpty()){
        m_message=readiness;
        qWarning().noquote()<<"[RePaper native color]"<<"dispatched=false reason="<<readiness;
        emit changed();return false;
    }
    if(m_hasObjectModel&&PaperDrawing::isConfigurableStencil(m_objectModel.symbolId)){
        auto item=m_objectModel;
        if(repaper::drawing::foregroundColor(item)==color)return true;
        repaper::drawing::setForegroundColor(item,color);
        pointerCancel();return replaceObjectModel(std::move(item));
    }
    if(std::all_of(m_selectedStrokes.cbegin(),m_selectedStrokes.cend(),[&](const auto &stroke){return stroke.color==color;}))return true;
    pointerCancel();
    if(m_propertiesSessionActive){
        m_propertiesColor=color;
        const bool sent=inspectObjects(ObjectPhase::PropertiesColorRead);emit changed();return sent;
    }
    const bool sent=m_colorEdit->dispatch(m_controller,context.value("layer").toInt(),color);
    qInfo().noquote()<<"[RePaper native color]"<<"dispatched="<<sent<<"reason="<<m_colorEdit->reason();
    m_message=m_colorEdit->reason();emit changed();return sent;
}
QRectF NativeScene::displayedSelectionRect() const {
    if(m_hasObjectModel)return PaperDrawing::bounds(displayedObjectModel().strokes);
    if(m_selectionGesture.active())return m_selectionPreviewRect;
    return m_selectionWaiting&&m_affineReceipt.valid()?m_affineDisplayRect:m_selectedRect;
}
void NativeScene::updateSelectionPreviewBounds(){
    m_selectionPreviewRect=selectionGeometryRect(m_selectedStrokes,m_selectionGesture.previewTransform());
    m_previewFrameDirty=true;
}
QVariant NativeScene::nativeItems(const QVector<PaperDrawing::Stroke> &strokes) {
#ifdef REPAPER_WITH_NATIVE_ABI
    // No SceneLineItem constructor/header/vtable is synthesized. Xochitl's
    // native deserialization factory owns allocation and its shared_ptr deleter.
    // It works on blank pages; no selection, clipboard or seed cache is read.
    using Items=QList<std::shared_ptr<SceneItem>>;
    if(!m_target||!m_lineType||!m_controller||!m_host)return {};
    if(strokes.isEmpty()||strokes.size()>128){m_message="Ce premier essai accepte au maximum 128 traits par dessin.";return {};}
    QList<Line> lines;lines.reserve(strokes.size());qsizetype totalPoints=0;
    for(const auto &stroke:strokes) {
        if(!std::isfinite(stroke.width)||stroke.width<0.5||stroke.width>100||!stroke.color.isValid()){
            m_message="Géométrie ou budget du trait natif invalide.";return {};
        }
        const auto sampled=RePaperNative::sampleStrokeForNativeSelection(stroke.points,RePaperNative::NativeStrokePointBudget-totalPoints);
        if(!sampled.valid()){
            switch(sampled.error){
            case RePaperNative::StrokeSamplingError::InvalidCoordinates:m_message="Coordonnées natives invalides.";break;
            case RePaperNative::StrokeSamplingError::PointBudgetExceeded:m_message="Le dessin dépasse le nombre de points pris en charge.";break;
            default:m_message="Géométrie ou budget du trait natif invalide.";break;
            }
            return {};
        }
        QList<LinePoint> points;points.reserve(sampled.points.size());
        for(auto sample:sampled.points){
            // Same quarter-unit encoding as the native RM point writer. Device
            // brush appearance still needs visual calibration in the test page.
            points.append({float(sample.x()),float(sample.y()),25,quint16(qBound(1,qRound(stroke.width*4),65535)),0,255});
        }
        totalPoints+=sampled.points.size();
        QRectF bounds;bool first=true;
        for(auto p:stroke.points){const QRectF part(p-QPointF(stroke.width/2,stroke.width/2),QSizeF(stroke.width,stroke.width));bounds=first?part:bounds.united(part);first=false;}
        auto line=Line::fromPoints(std::move(points),bounds);
        // Upstream's 'thickness' field is actually RM starting_length. Diameter
        // is encoded by point.width/4; a fresh fragment starts at length zero.
        line.thickness=0;line.rgba=stroke.color.rgba();line.color=stroke.color==Qt::black?0:9;
        lines.append(std::move(line));
    }
    const auto context=call("readState").toMap();
    const auto stableContext=[&] {
        const auto now=call("readState").toMap();
        return !now.value("working",true).toBool()&&!context.value("documentId").toString().isEmpty()
            &&now.value("documentId")==context.value("documentId")&&now.value("pageId")==context.value("pageId")
            &&now.value("layer")==context.value("layer");
    };
    const auto itemType=QMetaType::fromName("QList<std::shared_ptr<SceneItem>>");
    if(!itemType.isValid()||itemType.sizeOf()!=sizeof(Items))return {};
    Items result;result.reserve(lines.size());
    for(qsizetype i=0;i<lines.size();++i){
        if(!stableContext())return {};
        auto item=RePaperNative::createNativeLineItem(&m_message);
        if(!item||!nativeLineClone(item.get()))return {};
        for(const auto &previous:result)if(previous.get()==item.get())return {};
        result.append(std::move(item));
    }
    if(!stableContext())return {};
    for(qsizetype i=0;i<result.size();++i){
        auto *line=std::launder(reinterpret_cast<Line*>(reinterpret_cast<unsigned char*>(result[i].get())+NativeLineOffset));
        // Qt's implicitly shared point list is released by normal Line move
        // assignment; neither the native header nor its trailer is touched.
        *line=std::move(lines[i]);
    }
    return QVariant(itemType,&result);
#else
    Q_UNUSED(strokes);return {};
#endif
}
bool NativeScene::commit(const repaper::drawing::Item &item,const repaper::drawing::StencilPlacementResult &placement) {
    if(!available()||selectionOperationPending()||creationOperationPending()||!creationReadinessReason().isEmpty()
        ||!repaper::drawing::withinBudget({item},false))return false;
    // Reject a pending transform without committing it implicitly. New private
    // native objects do not depend on the selection cleared during preparation.
    const auto pending=m_controller->property("pendingEdit");
    if(!pending.canConvert<QTransform>()||!pending.value<QTransform>().isIdentity())return false;
    const auto batch=nativeItems(item.strokes);
    if(!batch.isValid()){emit changed();return false;}
    const auto context=call("readState").toMap();
    if(!available()||!samePage(context,call("readState").toMap())
        ||!creationReadinessReason().isEmpty())return false;
    const auto center=PaperDrawing::bounds(item.strokes).center();
    m_insertionContext=m_objectContext=context;m_insertingItem=item;m_insertingPlacement=placement;m_objectPhase=ObjectPhase::Insert;
    const bool ok=m_objectAccess->insert(m_controller,context,batch,center);
    if(ok){m_pendingStrokes=item.strokes;m_evidence.insert("nativeInsertionIdsObserved",false);}
    else {m_objectPhase=ObjectPhase::Idle;m_insertingItem={};m_insertingPlacement={};}
    m_message=ok?"Ajout du dessin…"
        :"Insertion native refusée ; aucun remplacement externe de document n’a été effectué.";
    emit changed();return ok;
}
bool NativeScene::mapPoint(QPointF input,QPointF *output) const {
    const auto mapped=call("viewToPaper",QVariant::fromValue(input));
    if(!mapped.canConvert<QPointF>())return false;
    *output=mapped.toPointF();return std::isfinite(output->x())&&std::isfinite(output->y());
}
bool NativeScene::previewTransform(QTransform *transform,qreal *scale) const {
    if(!transform||!m_coordinates||!m_host)return false;
    // QML passes the exact native QTransform through QVariant without turning
    // each point into a JS object. Older host fixtures can supply the affine
    // basis; neither route reads native page contents or takes a Page lock.
    QVariant direct;
    if(m_host->metaObject()->indexOfMethod("readViewTransform()")>=0)direct=call("readViewTransform");
    if(direct.canConvert<QTransform>())*transform=direct.value<QTransform>();
    else {
    const auto mappedOrigin=call("paperToView",QPointF(0,0));
    const auto mappedX=call("paperToView",QPointF(1,0));
    const auto mappedY=call("paperToView",QPointF(0,1));
    if(!mappedOrigin.canConvert<QPointF>()||!mappedX.canConvert<QPointF>()||!mappedY.canConvert<QPointF>())return false;
    const auto origin=mappedOrigin.toPointF(),basisX=mappedX.toPointF(),basisY=mappedY.toPointF();
    *transform=QTransform(basisX.x()-origin.x(),basisX.y()-origin.y(),
        basisY.x()-origin.x(),basisY.y()-origin.y(),origin.x(),origin.y());
    }
    for(const qreal value:{transform->m11(),transform->m12(),transform->m13(),transform->m21(),transform->m22(),transform->m23(),transform->m31(),transform->m32(),transform->m33()})
        if(!std::isfinite(value))return false;
    if(!transform->isAffine()||!transform->isInvertible())return false;
    const qreal requestedScale=call("viewScale").toDouble();
    if(scale)*scale=std::isfinite(requestedScale)&&requestedScale>0?requestedScale:1;
    return true;
}
RePaperNative::PreviewFrame NativeScene::previewFrame() const {
    if(!m_previewFrameDirty)return m_previewFrame;
    m_previewFrameDirty=false;m_previewFrame={};
    const bool selectionVisible=m_nativeToolActive&&m_gesture.tool=="select"&&!m_selectionCleanupPending;
    if(!m_gesture.active()&&m_pendingStrokes.isEmpty()&&(!selectionVisible||(m_selectedStrokes.isEmpty()&&!m_regionSelecting)))return {};
    QTransform paperToView;qreal scale=1;
    if(!previewTransform(&paperToView,&scale))return {};
    auto strokes=m_gesture.active()?m_gesture.preview():m_pendingStrokes;
    if(m_nativeToolActive&&m_gesture.tool=="select"&&!m_selectionCleanupPending&&m_pendingStrokes.isEmpty()){
        strokes=(m_selectionGesture.active()||selectionInkOverlay())?m_selectedStrokes:QVector<PaperDrawing::Stroke>{};
        if(m_selectionWaiting&&m_affineReceipt.valid())strokes=m_affineReceipt.expected();
        if(!m_hasObjectModel&&!m_replacingDocument.isEmpty()){
            strokes.clear();for(const auto &item:m_replacingDocument)strokes+=item.strokes;
        }
        if(m_hasObjectModel&&(m_objectGesture.active()||m_objectPhase==ObjectPhase::ReplaceRead||m_objectPhase==ObjectPhase::ReplaceSelect||m_objectPhase==ObjectPhase::Replace||m_objectPhase==ObjectPhase::ReplaceReselect)){
            strokes=displayedObjectModel().strokes;
            if(m_routingPreview.size()>1)for(qsizetype i=1;i<m_routingPreview.size();++i)strokes+=m_routingPreview[i].strokes;
            else if(m_replacingDocument.size()>1)for(qsizetype i=1;i<m_replacingDocument.size();++i)strokes+=m_replacingDocument[i].strokes;
        }
        if(m_selectionGesture.active()){
            const auto selectionTransform=m_selectionGesture.previewTransform();
            for(auto &stroke:strokes){
                stroke.width=RePaperNative::scaledNativeStrokeWidth(stroke.width,m_selectionGesture.scaleX(),m_selectionGesture.scaleY());
                for(auto &point:stroke.points)point=selectionTransform.map(point);
            }
        }
        const QRectF rect=m_regionSelecting?QRectF(m_regionStart,m_regionEnd).normalized():displayedSelectionRect();
        if(rect.isValid()&&(!m_selectedStrokes.isEmpty()||m_regionSelecting)){
            PaperDrawing::Stroke border;border.color=Qt::black;border.width=1.5/scale;
            border.points=m_hasObjectModel&&!m_regionSelecting?NativeObjectGesture::outline(displayedObjectModel()):
                QVector<QPointF>{rect.topLeft(),rect.topRight(),rect.bottomRight(),rect.bottomLeft()};
            if(!border.points.isEmpty())border.points.append(border.points.first());
            strokes.append(border);
        }
    }
    const auto &guides=m_gesture.active()?m_gesture.alignmentGuides():m_objectGesture.alignmentGuides();
    for(const auto &guide:guides){
        const auto segments=PaperDrawing::dashByArcLength({guide.line.p1(),guide.line.p2()},{6/scale,6/scale});
        for(const auto &segment:segments)strokes.append(PaperDrawing::Stroke{segment,1.5/scale,QColor("#777777")});
    }
    QVector<RePaperNative::PreviewStroke> frameStrokes;frameStrokes.reserve(strokes.size());
    for(const auto &stroke:std::as_const(strokes)){
        RePaperNative::PreviewStroke output;output.width=stroke.width*scale;output.color=stroke.color;
        output.points.reserve(stroke.points.size());
        for(const auto &point:stroke.points)output.points.append(paperToView.map(point));
        frameStrokes.append(std::move(output));
    }
    m_previewFrame=RePaperNative::PreviewFrame::fromStrokes(std::move(frameStrokes));
    return m_previewFrame;
}
QVariantList NativeScene::previewStrokes() const {
    // Compatibility surface only. The live painted item receives previewFrame
    // and shares its immutable point buffers across QVariant and QML.
    QVariantList result;
    const auto frame=previewFrame();
    result.reserve(frame.strokes().size());
    for(const auto &stroke:frame.strokes()){
        QVariantList points;points.reserve(stroke.points.size());
        for(const auto &point:stroke.points)points.append(QVariantMap{{"x",point.x()},{"y",point.y()}});
        result.append(QVariantMap{{"points",points},{"width",stroke.width},{"color",stroke.color.name(QColor::HexArgb)}});
    }
    return result;
}
bool NativeScene::pointerBegin(qreal x,qreal y,const QString &kind) {
    if(m_objectFaulted){refresh();return false;}
    if(!m_nativeToolActive||!captureEnabled()||!available()||creationOperationPending()||!m_touchGuard->acceptsBegin()
        ||selectionOperationPending()||m_gesture.active()||m_selectionGesture.active()||m_objectGesture.active()||m_regionSelecting||kind!="pen")return false;
    m_message=creationReadinessReason();
    if(!m_message.isEmpty()){emit changed();return false;}
    QPointF point;if(!mapPoint({x,y},&point))return false;
    if(m_gesture.tool=="select"){
        if(m_sceneObserver&&!m_sceneObserver->ready())m_sceneObserver->tryAttach();
        m_objectGesture.alignmentDocument=m_objectBindings->documentModels();
        m_objectGesture.alignmentTolerance=8/qMax(.01,call("viewScale").toDouble());
        if(m_hasObjectModel&&m_objectGesture.begin(m_objectModel,point,call("viewScale").toDouble())){beginAspectHold({x,y},point);emit changed();return true;}
        if(!m_hasObjectModel&&m_objectSelectionVerified&&!m_selectedStrokes.isEmpty()&&m_selectionGesture.begin(m_selectedRect,point,call("viewScale").toDouble())){beginAspectHold({x,y},point);updateSelectionPreviewBounds();emit changed();return true;}
        if(m_objectSelectionVerified&&!m_selectedStrokes.isEmpty()){
            // An idle, repaired selection can be cleared before queuing the
            // next region on the same native worker. Keep this press so changing
            // the selected object does not require an extra tap.
            if(!call("clearCustomSelection").toBool())return false;
            clearCustomSelection();
        }else {
            requestSelectionCleanup();
            if(selectionOperationPending())return false;
        }
        m_regionStart=m_regionEnd=point;m_regionSelecting=true;
        m_selectionContext=call("readState").toMap();emit changed();return true;
    }
    const auto start=snappedWirePoint(point);
    m_gesture.wireDocument=m_objectBindings->documentModels();
    m_gesture.alignmentTolerance=8/qMax(.01,call("viewScale").toDouble());
    m_gesture.stencilTapTolerance=4/qMax(.01,call("viewScale").toDouble());
    m_gesture.wireStart=m_wireSnap.snapped?repaper::drawing::Attachment{m_wireSnap.objectId,m_wireSnap.portId,m_wireSnap.wirePosition}:repaper::drawing::Attachment{};
    m_gesture.wireEnd={};
    const bool ok=m_gesture.begin(start);if(ok)beginAspectHold({x,y},point);emit changed();return ok;
}
bool NativeScene::pointerMove(qreal x,qreal y) {
    return pointerMoveBatch({QPointF(x,y)});
}
bool NativeScene::pointerMoveBatch(const QVariantList &samples) {
    if(!m_selectionGesture.active()&&!m_objectGesture.active()&&!m_regionSelecting&&!m_gesture.active())return false;
    if(!captureEnabled()||samples.isEmpty()||samples.size()>256){pointerCancel();return false;}
    QVector<QPointF> viewPoints;viewPoints.reserve(samples.size());
    for(const auto &sample:samples){
        QPointF point;
        if(sample.canConvert<QPointF>())point=sample.toPointF();
        else {
            const auto map=sample.toMap();bool xOk=false,yOk=false;
            const auto x=map.value("x").toDouble(&xOk),y=map.value("y").toDouble(&yOk);
            if(!xOk||!yOk){pointerCancel();return false;}
            point={x,y};
        }
        if(!std::isfinite(point.x())||!std::isfinite(point.y())||std::abs(point.x())>1000000||std::abs(point.y())>1000000){pointerCancel();return false;}
        viewPoints.append(point);
    }
    if(m_aspectHold.waiting()){
        const auto now=m_aspectClock.elapsed();
        for(const auto &point:viewPoints)m_aspectHold.move(point,now);
        if(!m_aspectHold.waiting())m_aspectTimer.stop();
    }
    if(m_selectionGesture.active()||m_objectGesture.active()||m_regionSelecting){
        QPointF point;if(!mapPoint(viewPoints.last(),&point)){pointerCancel();return false;}
        m_aspectPaperPoint=point;
        if(m_objectGesture.active()){
            if(m_objectModel.kind=="wire"&&m_objectGesture.kind()=="endpoint"){
                m_wireSnap=m_objectBindings->snapWirePoint(m_objectGesture.endpointCandidate(point),14/qMax(.01,call("viewScale").toDouble()),m_objectModel.id);
                if(m_wireSnap.snapped)point=m_objectGesture.pointerForEndpoint(m_wireSnap.point);
            }
            if(!m_objectGesture.update(point)){pointerCancel();return false;}
            if(!updateObjectRoutingPreview()){pointerCancel();return false;}
        }else if(m_regionSelecting)m_regionEnd=point;else if(!m_selectionGesture.update(point)){pointerCancel();return false;}
        if(m_selectionGesture.active()){
            if(!RePaperNative::nativeScaleWidthFits(m_selectionMaximumPointWidth,m_selectionGesture.scaleX(),m_selectionGesture.scaleY())){pointerCancel();return false;}
            updateSelectionPreviewBounds();
        }
        emit previewChanged();return true;
    }
    QVector<QPointF> paperPoints;paperPoints.reserve(viewPoints.size());
    if(viewPoints.size()==1){
        QPointF point;if(!mapPoint(viewPoints.first(),&point)){pointerCancel();return false;}
        paperPoints.append(point);
    }else {
        QPointF origin,basisX,basisY;
        if(!mapPoint({0,0},&origin)||!mapPoint({1,0},&basisX)||!mapPoint({0,1},&basisY)){pointerCancel();return false;}
        const QTransform mapping(basisX.x()-origin.x(),basisX.y()-origin.y(),basisY.x()-origin.x(),basisY.y()-origin.y(),origin.x(),origin.y());
        for(const auto &point:viewPoints)paperPoints.append(mapping.map(point));
    }
    if(m_gesture.tool=="wire"){
        paperPoints.last()=snappedWirePoint(paperPoints.last());
        m_gesture.wireEnd=m_wireSnap.snapped?repaper::drawing::Attachment{m_wireSnap.objectId,m_wireSnap.portId,m_wireSnap.wirePosition}:repaper::drawing::Attachment{};
    }
    m_aspectPaperPoint=paperPoints.last();
    const bool ok=m_gesture.moveBatch(paperPoints);if(!ok){pointerCancel();return false;}emit previewChanged();return ok;
}
bool NativeScene::pointerEnd(qreal x,qreal y) {
    cancelAspectHold();
    QPointF point;repaper::drawing::Item item;
    if(m_objectGesture.active()){
        if(!captureEnabled()||!mapPoint({x,y},&point)){pointerCancel();return false;}
        if(m_objectModel.kind=="wire"&&m_objectGesture.kind()=="endpoint"){
            m_wireSnap=m_objectBindings->snapWirePoint(m_objectGesture.endpointCandidate(point),14/qMax(.01,call("viewScale").toDouble()),m_objectModel.id);
            if(m_wireSnap.snapped)point=m_objectGesture.pointerForEndpoint(m_wireSnap.point);
        }
        if(!m_objectGesture.update(point)||!updateObjectRoutingPreview()){pointerCancel();return false;}
        const auto model=displayedObjectModel();m_objectGesture.cancel();m_wireSnap={};m_routingPreview.clear();
        const bool ok=replaceObjectModel(model);
        emit changed();return ok;
    }
    if(m_regionSelecting||m_selectionGesture.active()){
        if(!captureEnabled()||!mapPoint({x,y},&point)){pointerCancel();return false;}
        if(m_regionSelecting){
            m_regionSelecting=false;m_regionEnd=point;
            QRectF rect=QRectF(m_regionStart,point).normalized();
            const qreal scale=qMax(0.01,call("viewScale").toDouble());
            if(QLineF(m_regionStart,point).length()*scale<8)rect=QRectF(point-QPointF(12/scale,12/scale),QSizeF(24/scale,24/scale));
            if(!samePage(m_selectionContext,call("readState").toMap())){clearCustomSelection();emit changed();return false;}
            m_selectionWaiting=true;m_selectionSignal=false;m_selectionAttempts=0;
            const bool ok=call("selectCustomRegion",rect).toBool();
            if(!ok)m_selectionWaiting=false;
            const bool observed=ok&&inspectObjects(ObjectPhase::Region);
            emit changed();return observed;
        }
        if(!m_selectionGesture.finish(point)){pointerCancel();return false;}
        QVariantMap change;
        if(m_selectionGesture.handle()==NativeSelectionGesture::Handle::Move)change={{"kind","move"},{"delta",m_selectionGesture.delta()}};
        else if(m_selectionGesture.handle()==NativeSelectionGesture::Handle::Rotate)change={{"kind","rotate"},{"anchor",m_selectionGesture.anchor()},{"angle",m_selectionGesture.rotationAngleDegrees()}};
        else change={{"kind","scale"},{"anchor",m_selectionGesture.anchor()},{"sx",m_selectionGesture.scaleX()},{"sy",m_selectionGesture.scaleY()}};
        return transformSelection(change);
    }
    if(!m_gesture.active())return false;
    if(!captureEnabled()||!mapPoint({x,y},&point)){pointerCancel();return false;}
    qInfo().noquote()<<"[RePaper native gesture]"<<QJsonDocument::fromVariant(m_gesture.inputSummary(point)).toJson(QJsonDocument::Compact);
    const auto endpoint=snappedWirePoint(point);
    m_gesture.wireEnd=m_wireSnap.snapped?repaper::drawing::Attachment{m_wireSnap.objectId,m_wireSnap.portId,m_wireSnap.wirePosition}:repaper::drawing::Attachment{};
    const auto placement=m_gesture.stencilPlacement();
    if(!m_gesture.finish(endpoint,&item)){pointerCancel();return false;}
    // finish() clears the gesture. Keep its final geometry across native batch
    // preparation and dispatch so no intermediate change can erase live ink.
    m_pendingStrokes=item.strokes;
    const bool committed=commit(item,placement);
    if(!committed){m_pendingStrokes.clear();emit changed();}
    return committed;
}
void NativeScene::pointerCancel(){
    cancelAspectHold();
    const bool repaint=m_gesture.active()||m_selectionGesture.active()||m_objectGesture.active()||m_regionSelecting
        ||m_wireSnap.snapped||!m_pendingStrokes.isEmpty()||selectionInkOverlay();
    m_gesture.cancel();m_selectionGesture.cancel();m_objectGesture.cancel();m_regionSelecting=false;m_wireSnap={};m_routingPreview.clear();
    // Native pan/zoom also sends transformChanged while no editor ink is shown.
    // Do not rebuild editor state for every such idle navigation sample.
    if(repaint)emit previewChanged();
}
void NativeScene::nativeViewTransformChanged(){
    // Zoom changes projection, not page content. A hidden/inactive editor has
    // no work here; an idle selection or committed preview still needs its
    // projected handles/ink invalidated without a document inspection.
    if(m_gesture.active()||m_selectionGesture.active()||m_objectGesture.active()||m_regionSelecting){pointerCancel();return;}
    if(!m_pendingStrokes.isEmpty()||selectionInkOverlay())emit previewChanged();
}
bool NativeScene::action(const QString &action,const QVariant &argument) {
    if(!available())return false;
    if(action=="tool"){
        if(!QStringList{"select","pen","line","arrow","wire","rectangle","ellipse"}.contains(argument.toString()))return false;
        if(m_gesture.tool!=argument.toString())requestSelectionCleanup();
        pointerCancel();m_gesture.tool=argument.toString();m_message.clear();refresh();emit changed();return true;
    }
    if(action=="stencil"){
        const bool configured=argument.metaType().id()==QMetaType::QVariantMap;
        const auto request=argument.toMap();
        const QString id=configured?request.value("id").toString():argument.toString();
        QVariantMap parameters;
        if(PaperDrawing::isConfigurableStencil(id)){
            if(configured&&request.value("parameters").metaType().id()!=QMetaType::QVariantMap)return false;
            if(!PaperDrawing::normalizeStencilParameters(id,configured?request.value("parameters").toMap():QVariantMap{},&parameters))return false;
        }else{
            bool found=false;for(const auto &s:PaperDrawing::electronicsCatalogue())if(s.id==id)found=true;
            if(!found||configured)return false;
        }
        pointerCancel();requestSelectionCleanup();m_gesture.tool="symbol";m_gesture.symbolId=id;
        m_gesture.symbolQuarterTurns=stencilQuarterTurns(id,m_stencilVertical);
        m_recentStencils.removeAll(id);m_recentStencils.prepend(id);while(m_recentStencils.size()>4)m_recentStencils.removeLast();
        m_gesture.stencilParameters=parameters;saveDrawingPreferences();emit changed();return true;
    }
    if(action=="color"&&m_gesture.tool!="select"){
        const QColor color(argument.toString());if(!color.isValid()||color.alpha()!=255)return false;
        m_gesture.color=color;
    }
    else if(action=="color")return recolorSelection(QColor(argument.toString()));
    else if(m_gesture.tool=="select"&&QStringList{"style","width","direction","wire","wireBend","cornerRadius","stencilParameters","voltageArrow","voltageReversed","voltageOtherSide"}.contains(action))return objectProperty(action,argument);
    else if(action=="stencilVertical"){m_stencilVertical=argument.toBool();m_gesture.symbolQuarterTurns=stencilQuarterTurns(m_gesture.symbolId,m_stencilVertical);}
    else if(action=="voltageArrow")m_gesture.voltageArrow=argument.toBool();
    else if(action=="voltageReversed")m_gesture.voltageArrowReversed=argument.toBool();
    else if(action=="voltageOtherSide")m_gesture.voltageArrowOtherSide=argument.toBool();
    else if(action=="style"&&m_gesture.tool!="select"&&QStringList{"solid","dashed","dotted"}.contains(argument.toString()))m_gesture.style=argument.toString();
    else if(action=="width"&&m_gesture.tool!="select"&&std::isfinite(argument.toDouble())&&argument.toDouble()>=0.5&&argument.toDouble()<=4)m_gesture.width=argument.toDouble();
    else if(action=="direction"&&m_gesture.tool!="select"&&QStringList{"end","start","both","none"}.contains(argument.toString()))m_gesture.arrowDirection=argument.toString();
    else if(action=="wire"&&m_gesture.tool!="select")m_gesture.horizontalFirst=argument.toBool();
    else if(QStringList{"undo","redo"}.contains(action)){
        if(m_propertiesSessionActive||creationOperationPending()||selectionOperationPending()||!creationReadinessReason().isEmpty())return false;
        pointerCancel();m_pendingHistoryAction=action;
        m_message=action=="undo"?"Annulation…":"Rétablissement…";
        if(!m_selectedStrokes.isEmpty()||m_selectionCount>0||m_controller->property("selectionItemCount").toInt()>0){
            // Native Undo cancels a live selection instead of consuming its
            // history command. Clear both native selection stores first and
            // wait for their exact worker receipt before invoking Undo/Redo.
            m_selectionContext=call("readState").toMap();
            m_selectionCleanupPending=true;m_selectionWaiting=true;
            const bool ok=inspectObjects(ObjectPhase::CleanupRead);emit changed();return ok;
        }
        return dispatchHistoryAction();
    } else if(action=="scale")return transformSelection({{"kind","scale"},{"anchor",m_selectedRect.center()},{"sx",argument.toDouble()},{"sy",argument.toDouble()}});
    else if(action=="rotate")return transformSelection({{"kind","rotate"},{"anchor",m_selectedRect.center()},{"angle",90}});
    else if(action=="remove"||action=="duplicate")return transformSelection({{"kind",action}});
    else {m_message="Les paramètres de forme et connexions demandent encore le raccordement des objets natifs.";emit changed();return false;}
    if(action=="stencilVertical"||action.startsWith("voltage"))saveDrawingPreferences();
    pointerCancel();emit changed();return true;
}

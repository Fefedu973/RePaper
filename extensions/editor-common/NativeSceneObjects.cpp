#include "NativeScene.h"
#include "NativeSelectionStyle.h"
#include "NativeArrowStroke.h"
#include "NativeSceneObserver.h"
#include <QSet>
#include <QDebug>
#include <QTransform>
#include <algorithm>

namespace {
using namespace RePaperNative;
bool sameScope(const NativeObjectSnapshot &snapshot,const QVariantMap &context){
    return snapshot.complete&&snapshot.pendingEditIdentity&&!snapshot.documentId.isEmpty()&&!snapshot.pageId.isEmpty()
        &&snapshot.documentId==context.value("documentId").toString()
        &&snapshot.pageId==context.value("pageId").toString()&&snapshot.layer==context.value("layer",-1).toInt();
}
bool sameIds(QVector<quint64> a,QVector<quint64> b){std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());return a==b;}
bool sameStrokes(const QVector<PaperDrawing::Stroke> &a,const QVector<PaperDrawing::Stroke> &b){
    if(a.size()!=b.size())return false;
    for(qsizetype i=0;i<a.size();++i)if(a[i].points!=b[i].points||a[i].width!=b[i].width||a[i].color!=b[i].color)return false;
    return true;
}
QVector<quint64> allIds(const QVector<QVector<quint64>> &groups){QVector<quint64> result;for(const auto &group:groups)result+=group;return result;}
const NativeObjectLine *lineWithId(const NativeObjectSnapshot &snapshot,quint64 id){
    const auto it=std::find_if(snapshot.lines.cbegin(),snapshot.lines.cend(),[=](const auto &line){return line.id==id;});
    return it==snapshot.lines.cend()?nullptr:&*it;
}
QRectF geometryRect(const QVector<PaperDrawing::Stroke> &strokes){
    bool first=true; qreal left=0,right=0,top=0,bottom=0;
    for(const auto &stroke:strokes)for(const auto &point:stroke.points){
        if(first){left=right=point.x();top=bottom=point.y();first=false;}
        else {left=qMin(left,point.x());right=qMax(right,point.x());top=qMin(top,point.y());bottom=qMax(bottom,point.y());}
    }
    if(first)return {};
    if(right-left<1){const auto middle=(left+right)/2;left=middle-0.5;right=middle+0.5;}
    if(bottom-top<1){const auto middle=(top+bottom)/2;top=middle-0.5;bottom=middle+0.5;}
    return {QPointF(left,top),QPointF(right,bottom)};
}
}

void NativeScene::cancelObjectAccess(){
    ++m_bindingsGeneration;m_bindingsPending=false;
    m_bindingsRegistrationComplete=false;m_bindingsRegistrationBound=false;m_bindingsInsertedIds.clear();
    m_bindingsSnapshot={};
    cancelAspectHold();
    if(m_objectAccess)m_objectAccess->cancel();
    m_objectPhase=ObjectPhase::Idle;m_objectContext.clear();m_objectSnapshot={};m_affineObjectBaseline={};
    m_pendingHistoryAction.clear();
    clearPropertiesSession();
    m_pendingTransform.clear();m_affineLineages.clear();m_insertingItem={};m_insertingPlacement={};
    m_hasObjectModel=false;m_objectModel={};m_replacingItem={};m_objectGesture.cancel();
    m_replacingDocument.clear();m_replacingOldGroups.clear();m_replacementFocusIds.clear();m_routingPreview.clear();
    m_objectCacheDirty=true;m_objectSelectionVerified=false;m_objectFaulted=false;m_wireSnap={};
    if(m_objectBindings)m_objectBindings->clear();
}
bool NativeScene::inspectObjects(ObjectPhase phase){
    if(!available()||m_objectPhase!=ObjectPhase::Idle||m_objectAccess->busy())return false;
    m_objectContext=call("readState").toMap();m_objectPhase=phase;
    if(m_objectAccess->inspect(m_controller,m_objectContext))return true;
    failObjectOperation(m_objectAccess->reason());return false;
}
bool NativeScene::selectObjects(ObjectPhase phase,const RePaperNative::NativeObjectSnapshot &snapshot,const QVector<quint64> &ids){
    m_objectPhase=phase;
    if(m_objectAccess->select(m_controller,m_objectContext,snapshot,ids))return true;
    failObjectOperation(m_objectAccess->reason());return false;
}
void NativeScene::failObjectOperation(const QString &reason){
    m_bindingsRegistrationComplete=false;m_bindingsRegistrationBound=false;m_bindingsInsertedIds.clear();
    m_bindingsSnapshot={};
    const auto phase=m_objectPhase;m_objectPhase=ObjectPhase::Idle;
    m_pendingHistoryAction.clear();
    if(m_propertiesMutationPending||phase==ObjectPhase::PropertiesCancel)
        invalidatePropertiesSession("La page n’a pas confirmé toutes les modifications de cette fenêtre. Validez pour conserver l’état actuel.");
    m_propertiesCancelPending=false;
    m_objectBindings->invalidate();m_objectCacheDirty=true;m_objectSelectionVerified=false;
    m_objectGesture.cancel();m_hasObjectModel=false;
    m_evidence.insert("nativeObjectAccessError",reason);
    qWarning().noquote()<<"[RePaper native object]"<<"phase="<<int(phase)<<"error="<<reason;
    m_message=phase==ObjectPhase::Insert&&reason=="native-insertion-history-unavailable"
        ?"Le dessin n’a pas été ajouté : son insertion a été refusée avant l’écriture."
        :"La page n’a pas confirmé les traits concernés. Resélectionnez l’objet ou rouvrez la page.";
    // A failed read also invalidates the cached selection. A fresh snapshot +
    // empty exact selection repairs native records, or a page change abandons
    // this scope. Tool changes cannot strand an unacknowledged cleanup request.
    m_objectFaulted=true;m_selectionWaiting=true;m_selectionCleanupPending=true;
    m_selectionTimer.stop();
    if(phase==ObjectPhase::Refresh||phase==ObjectPhase::AffineRead){m_selectionAwaitContent=false;m_affineReceipt.clear();}
    if(phase!=ObjectPhase::CleanupRead&&phase!=ObjectPhase::CleanupSelect){
        const auto attachment=m_attachmentGeneration;
        QTimer::singleShot(0,this,[this,attachment]{if(attachment==m_attachmentGeneration&&m_objectFaulted)refresh();});
    }
    if(phase==ObjectPhase::Insert){m_insertingItem={};m_insertingPlacement={};discardPendingPreview();}
    emit changed();
}
bool NativeScene::loadObjectSelection(const RePaperNative::NativeObjectSnapshot &snapshot,const QVector<quint64> &ids){
    if(!sameScope(snapshot,call("readState").toMap())||!snapshot.nativeSelectionExact||ids.size()>NativeAffineReceipt::MaximumStrokes)return false;
    QVector<PaperDrawing::Stroke> strokes;quint16 maximumWidth=0;qsizetype points=0;QSet<quint64> seen;
    for(auto id:ids){
        const auto *line=lineWithId(snapshot,id);
        if(!line||seen.contains(id)||line->stroke.points.size()<2)return false;
        seen.insert(id);points+=line->stroke.points.size();
        if(points>NativeAffineReceipt::MaximumPoints)return false;
        strokes.append(line->stroke);maximumWidth=qMax(maximumWidth,line->maximumPointWidth);
    }
    m_objectSnapshot=snapshot;m_selectedStrokes=std::move(strokes);m_selectionCount=ids.size();
    m_selectionMaximumPointWidth=maximumWidth;m_selectedRect=geometryRect(m_selectedStrokes);
    m_selectionContext=call("readState").toMap();m_objectSelectionVerified=true;
    m_hasObjectModel=m_objectBindings->selectedModel(ids,&m_objectModel)&&NativeObjectGesture::supported(m_objectModel);
    return true;
}
bool NativeScene::selectionInkOverlay() const {
    return m_nativeToolActive&&m_gesture.tool=="select"&&!m_selectionCleanupPending
        &&m_objectSelectionVerified&&!m_selectedStrokes.isEmpty()&&m_pendingStrokes.isEmpty();
}
QPointF NativeScene::snappedWirePoint(QPointF point){
    m_wireSnap={};
    if(m_gesture.tool=="wire"&&!m_objectCacheDirty&&m_objectBindings->ready())
        m_wireSnap=m_objectBindings->snapWirePoint(point,14/qMax(0.01,call("viewScale").toDouble()));
    return m_wireSnap.snapped?m_wireSnap.point:point;
}
void NativeScene::objectAccessFinished(bool success){
    const auto phase=m_objectPhase;
    if(phase==ObjectPhase::Idle)return;
    const auto context=call("readState").toMap();
    if(m_objectContext.value("documentId")!=context.value("documentId")||m_objectContext.value("pageId")!=context.value("pageId")
        ||m_objectContext.value("layer")!=context.value("layer")){cancelObjectAccess();refresh();return;}
    if(!success){failObjectOperation(m_objectAccess->reason());return;}
    const auto snapshot=m_objectAccess->result();
    if(!sameScope(snapshot,context)){failObjectOperation("object-snapshot-scope-mismatch");return;}
    const auto insertedIds=m_bindingsRegistrationComplete?m_bindingsInsertedIds:m_objectAccess->insertedIds();
    QVector<QVector<quint64>> newGroups;
    if(phase==ObjectPhase::Insert&&(!snapshot.nativeSelectionExact||!snapshot.selectedIds.isEmpty())){
        failObjectOperation("creation-selection-not-cleared");return;
    }
    if(phase==ObjectPhase::Replace){
        qsizetype offset=0;
        for(const auto &item:m_replacingDocument){newGroups.append(insertedIds.mid(offset,item.strokes.size()));offset+=item.strokes.size();}
        if(offset!=insertedIds.size()||newGroups.isEmpty()||!snapshot.nativeSelectionExact||!sameIds(snapshot.selectedIds,insertedIds)){
            failObjectOperation("replacement-selection-not-confirmed");return;
        }
    }
    if(!m_bindingsWorker){
        m_bindingsWorker=new QObject;
        m_bindingsWorker->moveToThread(&m_bindingsThread);
        connect(&m_bindingsThread,&QThread::finished,m_bindingsWorker,&QObject::deleteLater);
        m_bindingsThread.setObjectName(QStringLiteral("RePaper object models"));
        m_bindingsThread.start(QThread::LowPriority);
    }
    // Native memory was copied under DocumentWorker's page guard. Disk I/O,
    // geometry validation and model regeneration now operate exclusively on
    // this immutable value snapshot, outside the GUI/native worker threads.
    auto bindings=std::make_shared<NativeObjectBindings>(*m_objectBindings);
    const auto generation=++m_bindingsGeneration;
    const auto contentGeneration=m_nativeContentGeneration;
    const bool registered=m_bindingsRegistrationComplete,registeredBound=m_bindingsRegistrationBound;
    auto before=m_affineObjectBaseline;before.history={};
    auto geometrySnapshot=snapshot;geometrySnapshot.history={};
    // History retains native command owners. Keep those references entirely
    // on the GUI side; semantic bindings never inspect or retain history.
    m_bindingsSnapshot=snapshot;
    const auto item=m_insertingItem;
    const auto placement=m_insertingPlacement;
    const auto oldGroups=m_replacingOldGroups;
    const auto models=m_replacingDocument;
    const bool duplicate=phase==ObjectPhase::AffineObserve&&m_pendingTransform.value("kind")=="duplicate";
    m_bindingsPending=true;
    QMetaObject::invokeMethod(m_bindingsWorker,[this,bindings,generation,contentGeneration,registered,registeredBound,phase,snapshot=std::move(geometrySnapshot),insertedIds,before,item,placement,oldGroups,newGroups,models,duplicate]{
        bool bound=false;
        if(registered){bindings->observe(snapshot);bound=registeredBound;}
        else if(phase==ObjectPhase::Insert)
            bound=bindings->registerInserted(item,snapshot,insertedIds,placement.snapped?&placement:nullptr);
        else if(phase==ObjectPhase::Replace)
            bound=bindings->registerReplacementsBatch(before,oldGroups,snapshot,newGroups,models);
        else if(duplicate)
            bound=bindings->registerDuplicate(before,before.selectedIds,snapshot,snapshot.selectedIds,QPointF(24,24));
        else bound=bindings->observe(snapshot);
        if(m_bindingsStopping.load())return;
        QMetaObject::invokeMethod(this,[this,bindings,generation,contentGeneration,phase,insertedIds,bound,duplicate]{
            if(generation!=m_bindingsGeneration||phase!=m_objectPhase)return;
            const auto snapshot=std::move(m_bindingsSnapshot);m_bindingsSnapshot={};
            m_bindingsPending=false;
            const auto context=call("readState").toMap();
            if(!sameScope(snapshot,context)){cancelObjectAccess();refresh();return;}
            m_objectBindings=std::make_unique<NativeObjectBindings>(std::move(*bindings));
            if(snapshot.sceneIdentity&&(!m_sceneObserver||!m_sceneObserver->observesIdentity(snapshot.sceneIdentity))){
                attachSceneObserver();
                // A reload can retain the old Scene and keep the same pageId.
                // Its newly attached sender must agree with the worker receipt.
                if(m_sceneObserver&&m_sceneObserver->ready()&&!m_sceneObserver->observesIdentity(snapshot.sceneIdentity))
                    ++m_nativeContentGeneration;
            }
            const auto pending=m_controller->property("pendingEdit");
            if(contentGeneration!=m_nativeContentGeneration||!pending.canConvert<QTransform>()||!pending.value<QTransform>().isIdentity()){
                // A native change after the copied receipt is not covered by
                // this model computation. Retain accepted binding persistence,
                // but keep the input gate and inspect again on DocumentWorker.
                // Do not register the same insertion/duplicate twice.
                m_objectCacheDirty=true;
                if(phase==ObjectPhase::Insert||phase==ObjectPhase::Replace||duplicate){
                    m_bindingsRegistrationComplete=true;m_bindingsRegistrationBound=bound;m_bindingsInsertedIds=insertedIds;
                }
                if(!m_objectAccess->inspect(m_controller,m_objectContext))failObjectOperation(m_objectAccess->reason());
                return;
            }
            m_bindingsRegistrationComplete=false;m_bindingsRegistrationBound=false;m_bindingsInsertedIds.clear();
            objectBindingsFinished(phase,snapshot,insertedIds,bound);
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
void NativeScene::objectBindingsFinished(ObjectPhase phase,const NativeObjectSnapshot &snapshot,
                                        const QVector<quint64> &insertedIds,bool bound){
    const auto context=call("readState").toMap();
    m_objectPhase=ObjectPhase::Idle;
    m_objectCacheDirty=false;
    m_evidence.insert("nativeObjectAccessError",QString());
    const auto fail=[&](const QString &reason){m_objectPhase=phase;failObjectOperation(reason);};
    if(phase==ObjectPhase::Insert){
        if(!snapshot.nativeSelectionExact||!snapshot.selectedIds.isEmpty()){fail("creation-selection-not-cleared");return;}
        qInfo().noquote()<<"[RePaper native insertion]"<<"lines="<<insertedIds.size()<<"selectionCleared="<<snapshot.selectedIds.isEmpty()<<"binding="<<bound;
        m_evidence.insert("nativeInsertionIdsObserved",true);m_evidence.insert("nativeInsertedObjectBound",bound);
        m_insertingItem={};m_insertingPlacement={};m_objectSnapshot=snapshot;
        if(!loadObjectSelection(snapshot,snapshot.selectedIds)){fail("inserted-selection-geometry-invalid");return;}
        m_message=bound?QString():"Dessin ajouté ; l’association des traits n’a pas pu être enregistrée.";
        beginPreviewHandover();emit changed();return;
    }
    if(phase==ObjectPhase::ReplaceRead){
        const auto error=ObjectAccessDetail::selectionRequestError(m_objectSnapshot,snapshot,m_objectSnapshot.selectedIds);
        if(!error.isEmpty()||!snapshot.nativeSelectionExact||!sameIds(snapshot.selectedIds,m_objectSnapshot.selectedIds)){
            fail(error.isEmpty()?QStringLiteral("replacement-baseline-changed"):error);return;
        }
        if(!preparePropertiesMutation(snapshot)){fail("properties-history-changed-before-edit");return;}
        if(!prepareObjectReplacement(m_replacingItem)){fail("connected-routing-preparation-failed");return;}
        const auto ids=allIds(m_replacingOldGroups);
        if(!sameIds(ids,snapshot.selectedIds)){m_affineObjectBaseline=snapshot;selectObjects(ObjectPhase::ReplaceSelect,snapshot,ids);return;}
        dispatchObjectReplacement(snapshot);
        emit changed();return;
    }
    if(phase==ObjectPhase::ReplaceSelect){
        if(!snapshot.nativeSelectionExact||!sameIds(snapshot.selectedIds,allIds(m_replacingOldGroups))
            ||!sameNativeObjectContent(m_affineObjectBaseline,snapshot)||!sameNativeHistory(m_affineObjectBaseline.history,snapshot.history)){
            fail("connected-selection-not-confirmed");return;
        }
        dispatchObjectReplacement(snapshot);emit changed();return;
    }
    if(phase==ObjectPhase::Replace){
        const auto &inserted=insertedIds;QVector<QVector<quint64>> newGroups;qsizetype offset=0;
        for(const auto &item:m_replacingDocument){newGroups.append(inserted.mid(offset,item.strokes.size()));offset+=item.strokes.size();}
        if(offset!=inserted.size()||newGroups.isEmpty()||!snapshot.nativeSelectionExact||!sameIds(snapshot.selectedIds,inserted)){
            fail("replacement-selection-not-confirmed");return;
        }
        finishPropertiesMutation(snapshot);
        m_replacementFocusIds.clear();
        for(qsizetype i=0;i<m_replacementFocusCount&&i<newGroups.size();++i)m_replacementFocusIds+=newGroups[i];
        m_message=bound?QString():"Forme modifiée ; ses paramètres n’ont pas pu être enregistrés.";
        if(!sameIds(snapshot.selectedIds,m_replacementFocusIds)){selectObjects(ObjectPhase::ReplaceReselect,snapshot,m_replacementFocusIds);return;}
        if(!loadObjectSelection(snapshot,snapshot.selectedIds)){fail("replacement-geometry-not-confirmed");return;}
        m_selectionWaiting=false;m_selectionAwaitContent=false;m_objectGesture.cancel();m_replacingItem={};m_affineObjectBaseline={};
        m_replacingDocument.clear();m_replacingOldGroups.clear();m_replacementFocusIds.clear();m_routingPreview.clear();
        if(m_selectionCleanupPending){inspectObjects(ObjectPhase::CleanupRead);return;}
        emit changed();return;
    }
    if(phase==ObjectPhase::ReplaceReselect){
        if(!sameIds(snapshot.selectedIds,m_replacementFocusIds)||!loadObjectSelection(snapshot,snapshot.selectedIds)){
            fail("connected-focus-selection-not-confirmed");return;
        }
        m_selectionWaiting=false;m_selectionAwaitContent=false;m_objectGesture.cancel();m_replacingItem={};m_affineObjectBaseline={};
        m_replacingDocument.clear();m_replacingOldGroups.clear();m_replacementFocusIds.clear();m_routingPreview.clear();
        if(m_selectionCleanupPending){inspectObjects(ObjectPhase::CleanupRead);return;}
        emit changed();return;
    }
    if(phase==ObjectPhase::CleanupRead){
        selectObjects(ObjectPhase::CleanupSelect,snapshot,{});return;
    }
    if(phase==ObjectPhase::CleanupSelect){
        if(!snapshot.nativeSelectionExact||!snapshot.selectedIds.isEmpty()){fail("selection-cleanup-not-observed");return;}
        const auto failureMessage=m_objectFaulted?m_message:QString();
        m_objectFaulted=false;clearCustomSelection();m_objectSnapshot=snapshot;
        m_pendingTransform.clear();m_affineLineages.clear();m_message=failureMessage;
        observePropertiesSession(snapshot);
        if(!m_pendingHistoryAction.isEmpty()){dispatchHistoryAction();return;}
        emit changed();return;
    }
    if(phase==ObjectPhase::HistoryObserve){
        // This read was queued only after the history call returned, behind
        // its native worker command. Never revive the pre-Undo selection or
        // use its stale ink to expand the selection restored by history.
        if(!snapshot.nativeSelectionExact||!snapshot.selectedIds.isEmpty()){
            m_selectionCleanupPending=true;inspectObjects(ObjectPhase::CleanupRead);return;
        }
        clearCustomSelection();m_objectSnapshot=snapshot;m_message.clear();emit changed();return;
    }
    if(phase==ObjectPhase::PropertiesCancelRead){
        observePropertiesSession(snapshot);
        if(!m_propertiesSessionActive||!m_propertiesSessionValid){
            m_propertiesCancelPending=false;m_selectionWaiting=false;m_selectionCleanupPending=false;emit changed();return;
        }
        m_objectPhase=ObjectPhase::PropertiesCancel;
        if(!m_objectAccess->cancelPropertySession(m_controller,m_objectContext,m_propertiesBaseline,snapshot))
            failObjectOperation(m_objectAccess->reason());
        emit changed();return;
    }
    if(phase==ObjectPhase::PropertiesCancel){
        if(!RePaperNative::nativeObjectContentRestored(m_propertiesBaseline,snapshot)
            ||!snapshot.nativeSelectionExact||!snapshot.selectedIds.isEmpty()){
            fail("properties-baseline-restoration-not-confirmed");return;
        }
        clearCustomSelection();m_objectSnapshot=snapshot;clearPropertiesSession();
        ++m_propertiesCancelGeneration;m_message.clear();emit changed();return;
    }
    if(phase==ObjectPhase::PropertiesColorRead){
        if(!snapshot.nativeSelectionExact||!sameIds(snapshot.selectedIds,m_objectSnapshot.selectedIds)
            ||!preparePropertiesMutation(snapshot)){fail("properties-color-baseline-changed");return;}
        if(!m_colorEdit->dispatch(m_controller,context.value("layer").toInt(),m_propertiesColor,snapshot.history)){
            m_propertiesMutationPending=false;observePropertiesSession(snapshot);m_message=m_colorEdit->reason();emit changed();return;
        }
        emit changed();return;
    }
    if(phase==ObjectPhase::PropertiesColorObserve){
        if(!RePaperNative::sameNativeHistory(m_propertiesColorHistory,snapshot.history))
            invalidatePropertiesSession("L’historique a changé après la couleur. Validez pour conserver l’état actuel.");
        else finishPropertiesMutation(snapshot);
        m_propertiesColorHistory={};
        // Continue through the normal exact-selection reconciliation below.
    }
    if(phase==ObjectPhase::AffineRead){
        // This is actual active ink, queued after preceding native operations.
        // Preserve the user's baseline: an independently edited selection must
        // not silently receive a transform calculated for older geometry.
        NativeAffineReceipt unchanged;
        if(!unchanged.begin(m_selectedStrokes,{{"kind","move"},{"delta",QPointF()}})
            ||!snapshot.nativeSelectionExact||!sameIds(m_objectSnapshot.selectedIds,snapshot.selectedIds)){fail("selection-changed-before-transform");return;}
        QVector<PaperDrawing::Stroke> actual;
        for(auto id:snapshot.selectedIds){const auto *line=lineWithId(snapshot,id);if(!line){fail("selected-line-missing");return;}actual.append(line->stroke);}
        if(!unchanged.matches(actual,snapshot.selectedIds.size())){fail("selection-geometry-changed-before-transform");return;}
        if(!preparePropertiesMutation(snapshot)){fail("properties-history-changed-before-transform");return;}
        QString error;m_affineLineages=RePaperNative::selectedNativeLineages(snapshot,snapshot.selectedIds,&error);
        if(!error.isEmpty()||m_affineLineages.size()!=snapshot.selectedIds.size()){fail("selection-lineages-unavailable");return;}
        if(m_pendingTransform.value("kind")=="scale"){
            quint16 width=0;for(auto id:snapshot.selectedIds)width=qMax(width,lineWithId(snapshot,id)->maximumPointWidth);
            if(!RePaperNative::nativeScaleWidthFits(width,m_pendingTransform.value("sx").toDouble(),m_pendingTransform.value("sy").toDouble())){fail("scaled-width-exceeds-native-format");return;}
        }
        m_affineObjectBaseline=snapshot;
        if(!call("transformCustomSelection",m_pendingTransform).toBool()){fail("native-transform-dispatch-failed");return;}
        // SceneController's transform and apply callbacks are already queued
        // on this same worker. GUI pendingEdit becoming identity is no receipt.
        inspectObjects(ObjectPhase::AffineObserve);emit changed();return;
    }
    if(phase==ObjectPhase::AffineObserve){
        QVector<quint64> ids;
        const auto kind=m_pendingTransform.value("kind").toString();
        if(kind=="remove"){
            for(const auto &line:snapshot.lines)if(m_affineLineages.contains(line.lineageId)){fail("removed-lineage-still-active");return;}
        }else if(kind=="duplicate"){
            ids=snapshot.selectedIds;
            for(auto id:ids)if(lineWithId(m_affineObjectBaseline,id)){fail("duplicate-reused-original-id");return;}
            // Known Copy selection + new IDs is attribution. Geometry alone is
            // insufficient when another object happens to occupy that place.
            if(!bound){
                fail("native-duplicate-binding-unconfirmed");return;
            }
        }else {
            const auto resolved=RePaperNative::resolveNativeLineages(snapshot,m_affineLineages);
            if(!resolved.valid){fail(resolved.reason);return;}ids=resolved.ids;
        }
        QVector<PaperDrawing::Stroke> actual;
        for(auto id:ids){const auto *line=lineWithId(snapshot,id);if(!line){fail("transformed-line-missing");return;}actual.append(line->stroke);}
        if(!m_affineReceipt.matches(actual,ids.size())){fail("actual-transform-geometry-not-confirmed");return;}
        // Native affine replacement leaves stale IDs in its selection records.
        // Rebuild both native selected lists and records from active identities.
        selectObjects(ObjectPhase::AffineSelect,snapshot,ids);return;
    }
    if(phase==ObjectPhase::AffineSelect){
        if(!loadObjectSelection(snapshot,snapshot.selectedIds)||!m_affineReceipt.matches(m_selectedStrokes,m_selectionCount)){
            fail("transformed-selection-not-confirmed");return;
        }
        finishPropertiesMutation(snapshot);
        m_selectionWaiting=false;m_selectionAwaitContent=false;m_affineReceipt.clear();m_affineDisplayRect={};
        m_pendingTransform.clear();m_affineLineages.clear();m_affineObjectBaseline={};m_message.clear();
        if(m_selectionCleanupPending){inspectObjects(ObjectPhase::CleanupRead);return;}
        emit changed();return;
    }
    // Native region selection and external native changes enter here. Only
    // explicitly recorded object memberships are expanded; nearby ink never is.
    if(m_selectionCleanupPending){inspectObjects(ObjectPhase::CleanupRead);return;}
    observePropertiesSession(snapshot);
    if(m_nativeToolActive&&m_gesture.tool=="select"){
        const auto expanded=m_objectBindings->expandSelection(snapshot.selectedIds);
        if(!expanded.valid){fail(expanded.reason);return;}
        if(!snapshot.nativeSelectionExact||!sameIds(expanded.ids,snapshot.selectedIds)){
            if(phase==ObjectPhase::Expand){fail("selection-expansion-not-exact");return;}
            selectObjects(ObjectPhase::Expand,snapshot,expanded.ids);return;
        }
        if(!loadObjectSelection(snapshot,snapshot.selectedIds)){fail("actual-selection-geometry-invalid");return;}
        m_selectionWaiting=false;m_selectionSignal=false;m_selectionAwaitContent=false;
        m_message.clear();
    }else m_objectSnapshot=snapshot;
    emit changed();
}
bool NativeScene::dispatchHistoryAction(){
    if(m_pendingHistoryAction.isEmpty())return false;
    const auto action=m_pendingHistoryAction;m_pendingHistoryAction.clear();
    const auto attachment=m_attachmentGeneration;
    const auto context=call("readState").toMap();
    // aboutToUndo/Redo and selection-cleared signals can synchronously call
    // refresh() before SceneController actually enqueues its history command.
    // Hold the operation phase across that call; enqueue our read afterwards.
    m_objectPhase=ObjectPhase::HistoryDispatch;
    clearCustomSelection();m_objectCacheDirty=true;m_objectBindings->invalidate();
    const bool ok=call("selectionAction",action,QVariant(0)).toBool();
    if(attachment!=m_attachmentGeneration||m_objectPhase!=ObjectPhase::HistoryDispatch)return ok;
    const auto current=call("readState").toMap();
    m_objectPhase=ObjectPhase::Idle;
    if(context.value("documentId")!=current.value("documentId")||context.value("pageId")!=current.value("pageId")
        ||context.value("layer")!=current.value("layer")){refresh();return ok;}
    if(!ok){m_message="La page n’a pas lancé la commande d’historique.";emit changed();return false;}
    inspectObjects(ObjectPhase::HistoryObserve);emit changed();return true;
}
void NativeScene::clearPropertiesSession(){
    m_propertiesSessionActive=false;m_propertiesSessionValid=false;m_propertiesMutationPending=false;m_propertiesCancelPending=false;
    m_propertiesBaseline={};m_propertiesCurrent={};m_propertiesMutationBaseline={};m_propertiesColorHistory={};
    m_propertiesCommandCount=0;m_propertiesSessionError.clear();m_propertiesColor={};
}
void NativeScene::invalidatePropertiesSession(const QString &reason){
    if(!m_propertiesSessionActive)return;
    m_propertiesSessionValid=false;m_propertiesMutationPending=false;m_propertiesMutationBaseline={};
    m_propertiesSessionError=reason;
}
bool NativeScene::propertiesMutationAllowed() const {
    return !m_propertiesSessionActive||(m_propertiesSessionValid&&!m_propertiesCancelPending&&m_propertiesCommandCount<128&&m_propertiesCurrent.history.appendIsolated);
}
bool NativeScene::beginPropertiesSession(){
    if(m_propertiesSessionActive)return true;
    if(!available()||!m_nativeToolActive||m_gesture.tool!="select"||selectionOperationPending()||creationOperationPending()||!m_objectSelectionVerified
        ||m_selectedStrokes.isEmpty()||!sameScope(m_objectSnapshot,call("readState").toMap())||!creationReadinessReason().isEmpty())return false;
    if(!m_objectSnapshot.history.valid){
        m_message="La page ne permet pas de conserver l’historique de cette fenêtre.";emit changed();return false;
    }
    m_propertiesBaseline=m_propertiesCurrent=m_objectSnapshot;
    m_propertiesSessionActive=m_propertiesSessionValid=true;m_propertiesCommandCount=0;m_propertiesSessionError.clear();
    if(!m_propertiesCurrent.history.appendIsolated)
        m_propertiesSessionError="Le prochain changement ne peut pas être isolé dans l’historique. Annulez ou validez cette fenêtre.";
    emit changed();return true;
}
bool NativeScene::acceptPropertiesSession(){
    if(selectionOperationPending()||creationOperationPending())return false;
    clearPropertiesSession();emit changed();return true;
}
bool NativeScene::cancelPropertiesSession(){
    if(!m_propertiesSessionActive||!m_propertiesSessionValid||selectionOperationPending()||creationOperationPending()
        ||!sameScope(m_propertiesCurrent,call("readState").toMap()))return false;
    pointerCancel();m_propertiesCancelPending=true;m_selectionCleanupPending=true;m_selectionWaiting=true;
    m_message="Annulation des modifications…";
    const bool sent=inspectObjects(ObjectPhase::PropertiesCancelRead);emit changed();return sent;
}
void NativeScene::observePropertiesSession(const RePaperNative::NativeObjectSnapshot &snapshot){
    if(!m_propertiesSessionActive||!m_propertiesSessionValid||m_propertiesMutationPending)return;
    if(!sameNativeHistory(m_propertiesCurrent.history,snapshot.history)||!sameNativeObjectContent(m_propertiesCurrent,snapshot)){
        invalidatePropertiesSession("La page a été modifiée en dehors de cette fenêtre. Validez pour conserver l’état actuel.");return;
    }
    m_propertiesCurrent=snapshot;
}
bool NativeScene::preparePropertiesMutation(const RePaperNative::NativeObjectSnapshot &snapshot){
    if(!m_propertiesSessionActive)return true;
    observePropertiesSession(snapshot);
    if(!propertiesMutationAllowed())return false;
    if(!snapshot.history.appendIsolated){
        m_propertiesSessionError="Le prochain changement ne peut pas être isolé dans l’historique. Annulez ou validez cette fenêtre.";return false;
    }
    m_propertiesMutationBaseline=snapshot;m_propertiesMutationPending=true;return true;
}
void NativeScene::finishPropertiesMutation(const RePaperNative::NativeObjectSnapshot &snapshot){
    if(!m_propertiesSessionActive||!m_propertiesMutationPending)return;
    if(!nativeHistoryAppended(m_propertiesMutationBaseline.history,snapshot.history)){
        invalidatePropertiesSession("L’historique n’a pas confirmé une commande propre à cette fenêtre. Validez pour conserver l’état actuel.");return;
    }
    m_propertiesCurrent=snapshot;m_propertiesMutationBaseline={};m_propertiesMutationPending=false;
    ++m_propertiesCommandCount;
    if(m_propertiesCommandCount==128)m_propertiesSessionError="Cette fenêtre a atteint 128 modifications. Annulez ou validez pour continuer.";
}
const repaper::drawing::Item &NativeScene::displayedObjectModel() const {
    if(!m_routingPreview.isEmpty())return m_routingPreview.first();
    if(m_objectGesture.active())return m_objectGesture.preview();
    if(m_objectPhase==ObjectPhase::ReplaceRead||m_objectPhase==ObjectPhase::ReplaceSelect||m_objectPhase==ObjectPhase::Replace||m_objectPhase==ObjectPhase::ReplaceReselect)return m_replacingItem;
    if(m_selectionWaiting&&m_affineReceipt.valid()&&NativeObjectGesture::supported(m_transformedModel))return m_transformedModel;
    return m_objectModel;
}
bool NativeScene::prepareObjectReplacement(const repaper::drawing::Item &item){
    if(!m_objectBindings->connectionModelsReady())return false;
    const auto before=m_objectBindings->documentModels();auto document=before;
    const auto subjects=m_replacementSubjects.isEmpty()?repaper::drawing::Document{item}:m_replacementSubjects;
    QStringList focus;
    for(const auto &subject:subjects){
        auto primary=std::find_if(document.begin(),document.end(),[&](const auto &candidate){return candidate.id==subject.id;});
        if(primary==document.end()||focus.contains(subject.id))return false;
        *primary=subject;focus.append(subject.id);
    }
    if(!repaper::drawing::reroute(document))return false;
    repaper::drawing::Document models;QVector<QVector<quint64>> groups;
    for(const auto &id:focus)for(const auto &model:document)if(model.id==id){models.append(model);groups.append(m_objectBindings->idsForObject(id));break;}
    for(qsizetype i=0;i<document.size();++i)if(!focus.contains(document[i].id)&&document[i].kind=="wire"&&!sameStrokes(document[i].strokes,before[i].strokes)){
        models.append(document[i]);groups.append(m_objectBindings->idsForObject(document[i].id));
    }
    qsizetype strokeCount=0;
    for(auto &model:models){
        if(!RePaperNative::rebuildNativeObject(model))return false;
        strokeCount+=model.strokes.size();
    }
    for(const auto &ids:groups)if(ids.isEmpty())return false;
    if(strokeCount>128||!repaper::drawing::withinBudget(models,false))return false;
    m_replacingDocument=std::move(models);m_replacingOldGroups=std::move(groups);m_replacingItem=m_replacingDocument.first();
    m_replacementFocusCount=focus.size();
    return true;
}
bool NativeScene::dispatchObjectReplacement(const NativeObjectSnapshot &snapshot){
    QVector<PaperDrawing::Stroke> strokes;for(const auto &item:m_replacingDocument)strokes+=item.strokes;
    const auto items=nativeItems(strokes);
    if(!items.isValid()){failObjectOperation("replacement-item-preparation-failed");return false;}
    m_affineObjectBaseline=snapshot;m_objectPhase=ObjectPhase::Replace;
    if(!m_objectAccess->replace(m_controller,m_objectContext,snapshot,snapshot.selectedIds,items,PaperDrawing::bounds(strokes).center())){
        failObjectOperation(m_objectAccess->reason());return false;
    }
    return true;
}
bool NativeScene::updateObjectRoutingPreview(){
    m_routingPreview.clear();
    if(!m_objectGesture.active())return true;
    if(!m_objectBindings->connectionModelsReady())return false;
    auto item=m_objectGesture.preview();
    if(item.kind=="wire"&&m_objectGesture.kind()=="endpoint"){
        auto &ref=m_objectGesture.endpointIndex()==0?item.startAttachment:item.endAttachment;
        ref=m_wireSnap.snapped?repaper::drawing::Attachment{m_wireSnap.objectId,m_wireSnap.portId,m_wireSnap.wirePosition}:repaper::drawing::Attachment{};
    }
    const auto before=m_objectBindings->documentModels();auto document=before;
    auto primary=std::find_if(document.begin(),document.end(),[&](const auto &candidate){return candidate.id==item.id;});
    if(primary==document.end())return false;
    *primary=item;if(!repaper::drawing::reroute(document))return false;
    m_routingPreview.append(*primary);
    for(qsizetype i=0;i<document.size();++i)if(document[i].id!=item.id&&document[i].kind=="wire"&&!sameStrokes(document[i].strokes,before[i].strokes))m_routingPreview.append(document[i]);
    return true;
}
bool NativeScene::replaceObjectModel(repaper::drawing::Item item){
    const auto context=call("readState").toMap();
    if(!available()||!m_nativeToolActive||m_gesture.tool!="select"||!m_hasObjectModel||!m_objectSelectionVerified
        ||item.id!=m_objectModel.id||selectionOperationPending()||creationOperationPending()
        ||!sameScope(m_objectSnapshot,context)||!creationReadinessReason().isEmpty()||!propertiesMutationAllowed()
        ||!RePaperNative::rebuildNativeObject(item)||!repaper::drawing::withinBudget({item},false)||item.strokes.size()>128)return false;
    if(item.sourcePoints==m_objectModel.sourcePoints&&item.width==m_objectModel.width&&item.style==m_objectModel.style
        &&item.arrowDirection==m_objectModel.arrowDirection&&item.headSize==m_objectModel.headSize
        &&item.patternScale==m_objectModel.patternScale&&item.cornerRadius==m_objectModel.cornerRadius
        &&item.wireBend==m_objectModel.wireBend&&item.horizontalFirst==m_objectModel.horizontalFirst
        &&item.wireAxis==m_objectModel.wireAxis&&item.wireHasBend==m_objectModel.wireHasBend
        &&item.wireRouteMode==m_objectModel.wireRouteMode&&item.wireRoute==m_objectModel.wireRoute
        &&item.startAttachment.objectId==m_objectModel.startAttachment.objectId&&item.startAttachment.portId==m_objectModel.startAttachment.portId
        &&item.startAttachment.wirePosition==m_objectModel.startAttachment.wirePosition
        &&item.endAttachment.objectId==m_objectModel.endAttachment.objectId&&item.endAttachment.portId==m_objectModel.endAttachment.portId
        &&item.endAttachment.wirePosition==m_objectModel.endAttachment.wirePosition
        &&item.symbolId==m_objectModel.symbolId&&item.stencilParameters==m_objectModel.stencilParameters
        &&item.voltageArrow==m_objectModel.voltageArrow&&item.voltageArrowReversed==m_objectModel.voltageArrowReversed
        &&item.voltageArrowOtherSide==m_objectModel.voltageArrowOtherSide
        &&repaper::drawing::foregroundColor(item)==repaper::drawing::foregroundColor(m_objectModel)){emit changed();return true;}
    m_replacementSubjects={item};m_replacementFocusCount=1;
    m_replacingItem=std::move(item);m_routingPreview.clear();m_selectionWaiting=true;m_selectionAwaitContent=false;
    m_message.clear();
    const bool ok=inspectObjects(ObjectPhase::ReplaceRead);emit changed();return ok;
}
bool NativeScene::replaceObjectModels(const repaper::drawing::Document &items){
    if(items.size()==1)return replaceObjectModel(items.first());
    if(items.isEmpty()||items.size()>128||!m_objectSelectionVerified||selectionOperationPending()||creationOperationPending()
        ||!sameScope(m_objectSnapshot,call("readState").toMap())||!propertiesMutationAllowed())return false;
    QVector<quint64> selected;qsizetype count=0;
    for(const auto &item:items){
        const auto ids=m_objectBindings->idsForObject(item.id);if(ids.isEmpty())return false;
        selected+=ids;count+=item.strokes.size();
    }
    if(count>128||!sameIds(selected,m_objectSnapshot.selectedIds)||!repaper::drawing::withinBudget(items,false))return false;
    m_replacementSubjects=items;m_replacementFocusCount=items.size();m_replacingItem=items.first();
    m_routingPreview.clear();m_selectionWaiting=true;m_selectionAwaitContent=false;m_message.clear();
    const bool ok=inspectObjects(ObjectPhase::ReplaceRead);emit changed();return ok;
}
bool NativeScene::objectProperty(const QString &action,const QVariant &argument){
    if(!m_hasObjectModel)return false;
    auto item=m_objectModel;
    const qreal value=argument.toDouble();
    if(action=="style"&&!(item.kind=="symbol"&&PaperDrawing::isConfigurableStencil(item.symbolId))
        &&QStringList{"solid","dashed","dotted"}.contains(argument.toString()))item.style=argument.toString();
    else if(action=="width"&&std::isfinite(value)&&value>=.5&&value<=100)item.width=value;
    else if(action=="direction"&&item.kind=="arrow"&&QStringList{"end","start","both","none"}.contains(argument.toString()))item.arrowDirection=argument.toString();
    else if(action=="wire"&&item.kind=="wire"){item.horizontalFirst=argument.toBool();item.wireRoute.clear();item.wireRouteMode="auto";item.wireHasBend=false;item.wireBend=0;}
    else if(action=="wireAuto"&&item.kind=="wire"){item.wireRoute.clear();item.wireRouteMode="auto";item.wireHasBend=false;item.wireBend=0;}
    else if(action=="wireBend"&&item.kind=="wire"&&std::isfinite(value)&&std::abs(value)<=2000){item.wireBend=value;item.wireHasBend=std::abs(value)>1e-9;item.wireRouteMode=item.wireHasBend?"legacy":"auto";item.wireRoute.clear();}
    else if(action=="cornerRadius"&&item.kind=="rectangle"&&std::isfinite(value)&&value>=0)item.cornerRadius=value;
    else if(item.kind=="symbol"&&PaperDrawing::supportsVoltageArrow(item.symbolId)&&action=="voltageArrow")item.voltageArrow=argument.toBool();
    else if(item.kind=="symbol"&&PaperDrawing::supportsVoltageArrow(item.symbolId)&&action=="voltageReversed")item.voltageArrowReversed=argument.toBool();
    else if(item.kind=="symbol"&&PaperDrawing::supportsVoltageArrow(item.symbolId)&&action=="voltageOtherSide")item.voltageArrowOtherSide=argument.toBool();
    else if(action=="stencilParameters"&&item.kind=="symbol"&&PaperDrawing::isConfigurableStencil(item.symbolId)){
        if(argument.metaType().id()!=QMetaType::QVariantMap)return false;
        QVariantMap normalized, requested=item.stencilParameters;
        const auto updates=argument.toMap();
        for(auto it=updates.cbegin();it!=updates.cend();++it)requested.insert(it.key(),it.value());
        if(!PaperDrawing::normalizeStencilParameters(item.symbolId,requested,&normalized))return false;
        const auto color=repaper::drawing::foregroundColor(item);
        if(!color.isValid())return false;
        item.stencilParameters=normalized;
        item.strokes={PaperDrawing::Stroke{{},item.width,color}};
    }
    else return false;
    pointerCancel();return replaceObjectModel(std::move(item));
}

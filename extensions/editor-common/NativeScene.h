#pragma once
#include "NativeGesture.h"
#include "NativeInsertObservation.h"
#include "NativeTouchGuard.h"
#include "NativeSelectionGesture.h"
#include "NativeAffineReceipt.h"
#include "NativeSelectionColor.h"
#include "NativeObjectAccess.h"
#include "NativeObjectBindings.h"
#include "NativeObjectGesture.h"
#include "NativeAspectHold.h"
#include "NativePreviewFrame.h"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <QVariantMap>
#include <atomic>
class NativeSceneObserver;

class NativeScene:public QObject {
    Q_OBJECT
public:
    explicit NativeScene(QObject *parent=nullptr, NativeObjectAccess *objectAccess=nullptr, QString bindingsDirectory={}, NativeSelectionColor *colorAccess=nullptr);
    ~NativeScene() override;
    bool attach(QObject *controller,QObject *view,QObject *commandHost);
    bool available() const;
    QString status() const;
    QVariantMap state() const;
    // Geometry that changes while dragging has a separate notification. Menus
    // and the immutable tool settings need not rebuild for every pen sample.
    QVariantMap overlayState() const;
    RePaperNative::PreviewFrame previewFrame() const;
    QVariantMap evidence() const { return m_evidence; }
    QVariantList previewStrokes() const;
    // Keep the native pen blocked while a custom tool is selected, including
    // while its preceding command is busy. Permission to BEGIN is separate.
    bool captureEnabled() const { return (m_nativeToolActive||selectionOperationPending()||creationOperationPending())&&m_controller&&m_view&&m_host; }
    void setNativeToolActive(bool active);
    bool action(const QString &action,const QVariant &argument={});
    bool pointerBegin(qreal x,qreal y,const QString &kind);
    bool pointerMove(qreal x,qreal y);
    bool pointerMoveBatch(const QVariantList &points);
    bool pointerEnd(qreal x,qreal y);
    void pointerCancel();
    void refresh(bool objectsMayHaveChanged=false);
    void nativeAreaSelected(int layer, const QRectF &rect={});
    void nativeContentChanged();
    void nativeViewTransformChanged();
    void nativePreviewPresented(qulonglong token);
    bool resizeSelection(qreal width,qreal height);
    bool beginPropertiesSession();
    bool acceptPropertiesSession();
    bool cancelPropertiesSession();
signals:
    void changed();
    void previewChanged();
private:
    friend class NativeSelectionLifecycleTest;
    friend class NativeObjectWorkflowTest;
    QVariant call(const char *name,const QVariant &a={},const QVariant &b={}) const;
    bool mapPoint(QPointF input,QPointF *output) const;
    bool commit(const repaper::drawing::Item &item,const repaper::drawing::StencilPlacementResult &placement={});
    bool initializeFromSelection(const QVariant &selection);
    virtual QVariant nativeItems(const QVector<PaperDrawing::Stroke> &strokes);
    QString creationReadinessReason() const;
    QString creationReadinessReason(const QVariantMap &context) const;
    bool available(const QVariantMap &context) const;
    bool previewTransform(QTransform *transform, qreal *scale=nullptr) const;
    void beginAspectHold(QPointF viewPoint,QPointF paperPoint);
    void cancelAspectHold();
    void checkAspectHold(qint64 now);
    bool aspectRatioLocked() const;
    void checkInsertionObservation();
    bool creationOperationPending() const { return m_objectPhase==ObjectPhase::Insert||m_insertionObservation.pending()||m_previewHandoverPending; }
    void beginPreviewHandover();
    void discardPendingPreview();
    void checkSelectionObservation();
    bool selectionOperationPending() const { return m_objectPhase!=ObjectPhase::Idle||m_selectionWaiting||m_selectionCleanupPending||m_propertiesMutationPending||m_propertiesCancelPending||(m_colorEdit&&m_colorEdit->busy()); }
    void requestSelectionCleanup();
    void clearCustomSelection();
    void attachSceneObserver();
    bool transformSelection(const QVariantMap &change);
    bool recolorSelection(const QColor &color);
    QRectF displayedSelectionRect() const;
    void updateSelectionPreviewBounds();
    enum class ObjectPhase { Idle, Refresh, Region, Expand, AffineRead, AffineObserve, AffineSelect, Insert, CleanupRead, CleanupSelect, ReplaceRead, ReplaceSelect, Replace, ReplaceReselect, HistoryDispatch, HistoryObserve, PropertiesCancelRead, PropertiesCancel, PropertiesColorRead, PropertiesColorObserve };
    bool inspectObjects(ObjectPhase phase);
    void objectAccessFinished(bool success);
    void objectBindingsFinished(ObjectPhase phase, const RePaperNative::NativeObjectSnapshot &snapshot,
                                const QVector<quint64> &insertedIds, bool bound);
    bool loadObjectSelection(const RePaperNative::NativeObjectSnapshot &snapshot, const QVector<quint64> &ids);
    bool selectObjects(ObjectPhase phase, const RePaperNative::NativeObjectSnapshot &snapshot, const QVector<quint64> &ids);
    void cancelObjectAccess();
    void failObjectOperation(const QString &reason);
    QPointF snappedWirePoint(QPointF point);
    bool selectionInkOverlay() const;
    bool replaceObjectModel(repaper::drawing::Item item);
    bool replaceObjectModels(const repaper::drawing::Document &items);
    bool prepareObjectReplacement(const repaper::drawing::Item &item);
    bool dispatchObjectReplacement(const RePaperNative::NativeObjectSnapshot &snapshot);
    bool updateObjectRoutingPreview();
    bool objectProperty(const QString &action,const QVariant &argument);
    bool dispatchHistoryAction();
    bool propertiesMutationAllowed() const;
    bool preparePropertiesMutation(const RePaperNative::NativeObjectSnapshot &snapshot);
    void finishPropertiesMutation(const RePaperNative::NativeObjectSnapshot &snapshot);
    void observePropertiesSession(const RePaperNative::NativeObjectSnapshot &snapshot);
    void invalidatePropertiesSession(const QString &reason);
    void clearPropertiesSession();
    const repaper::drawing::Item &displayedObjectModel() const;
    NativeObjectAccess *m_objectAccess=nullptr;
    std::unique_ptr<RePaperNative::NativeObjectBindings> m_objectBindings;
    QThread m_bindingsThread;
    QObject *m_bindingsWorker=nullptr;
    std::atomic_bool m_bindingsStopping{false};
    quint64 m_bindingsGeneration=0;
    bool m_bindingsPending=false;
    quint64 m_nativeContentGeneration=0;
    bool m_bindingsRegistrationComplete=false,m_bindingsRegistrationBound=false;
    QVector<quint64> m_bindingsInsertedIds;
    RePaperNative::NativeObjectSnapshot m_bindingsSnapshot;
    RePaperNative::NativeObjectSnapshot m_objectSnapshot, m_affineObjectBaseline;
    ObjectPhase m_objectPhase=ObjectPhase::Idle;
    QVariantMap m_objectContext,m_pendingTransform;
    QString m_pendingHistoryAction;
    RePaperNative::NativeObjectSnapshot m_propertiesBaseline,m_propertiesCurrent,m_propertiesMutationBaseline;
    RePaperNative::NativeHistorySnapshot m_propertiesColorHistory;
    bool m_propertiesSessionActive=false,m_propertiesSessionValid=false,m_propertiesMutationPending=false,m_propertiesCancelPending=false;
    int m_propertiesCommandCount=0;
    qulonglong m_propertiesCancelGeneration=0;
    QString m_propertiesSessionError;
    QColor m_propertiesColor;
    QVector<quint64> m_affineLineages;
    repaper::drawing::Item m_insertingItem;
    repaper::drawing::StencilPlacementResult m_insertingPlacement;
    repaper::drawing::Item m_objectModel,m_replacingItem,m_transformedModel;
    repaper::drawing::Document m_replacingDocument,m_replacementSubjects,m_routingPreview;
    qsizetype m_replacementFocusCount=1;
    QVector<QVector<quint64>> m_replacingOldGroups;
    QVector<quint64> m_replacementFocusIds;
    NativeObjectGesture m_objectGesture;
    bool m_hasObjectModel=false;
    bool m_objectCacheDirty=true, m_objectSelectionVerified=false, m_objectFaulted=false;
    QVariantMap m_lastPublishedContext;
    mutable RePaperNative::PreviewFrame m_previewFrame;
    mutable bool m_previewFrameDirty=true;
    RePaperNative::NativeWireSnap m_wireSnap;
    QPointer<QObject> m_controller,m_view,m_host;
    NativeGesture m_gesture;
    QStringList m_recentStencils{"resistor-iec","capacitor","voltage-source","square-root"};
    bool m_stencilVertical=false;
    void saveDrawingPreferences() const;
    NativeAspectHold m_aspectHold;
    QElapsedTimer m_aspectClock;
    QTimer m_aspectTimer;
    QPointF m_aspectViewAnchor,m_aspectPaperPoint;
    QVector<PaperDrawing::Stroke> m_pendingStrokes;
    NativeSelectionGesture m_selectionGesture;
    NativeAffineReceipt m_affineReceipt;
    NativeSceneObserver *m_sceneObserver=nullptr;
    NativeSelectionColor *m_colorEdit=nullptr;
    QVector<PaperDrawing::Stroke> m_selectedStrokes;
    QRectF m_selectedRect;
    QRectF m_affineDisplayRect;
    QRectF m_selectionPreviewRect;
    QPointF m_regionStart,m_regionEnd;
    QVariantMap m_selectionContext;
    bool m_regionSelecting=false,m_selectionWaiting=false,m_selectionSignal=false;
    bool m_selectionAwaitContent=false;
    bool m_selectionCleanupPending=false;
    int m_selectionAttempts=0;
    quint64 m_attachmentGeneration=0;
    QVariantMap m_evidence;
    QVariant m_selection;
    QString m_message;
    RePaperNative::NativeInsertObservation m_insertionObservation;
    QVariantMap m_pageContext,m_insertionContext;
    NativeTouchGuard *m_touchGuard;
    QTimer m_confirmationTimer;
    QTimer m_previewHandoverTimer;
    QTimer m_selectionTimer;
    bool m_target=false,m_coordinates=false,m_lineType=false,m_batchType=false,m_nativeToolActive=false;
    bool m_areaSelectedAfterDispatch=false;
    int m_observationAttempts=0;
    bool m_previewHandoverPending=false;
    qulonglong m_previewToken=0;
    // Local parametric controls are exposed separately. The full persistent
    // capability still includes unvalidated dynamic connections and transport.
    const bool m_persistentEditingValidated=false;
    int m_selectionCount=0;
    quint16 m_selectionMaximumPointWidth=0;
};

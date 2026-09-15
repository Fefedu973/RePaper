#include <QQuickItem>
#include <QQuickWindow>
#include <QHoverEvent>
#include <QTouchEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QRegion>
#include "NativePreviewItem.h"
#include "NativeInputScheduler.h"
#include "MockNativePenColorModel.h"
#include <QtTest>
#include <limits>
#include <atomic>
#include <thread>

// Test the actual shipped NativePageHost QML with deterministic stand-ins for
// its in-process services. This does not emulate or claim the tablet pen driver.
class InputAdapter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool captureEnabled READ captureEnabled NOTIFY changed)
    Q_PROPERTY(QVariantList previewStrokes READ previewStrokes NOTIFY changed)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantMap overlayState READ overlayState NOTIFY previewChanged)
    Q_PROPERTY(QVariant previewFrame READ previewFrame NOTIFY previewChanged)
    Q_PROPERTY(QString backend READ backend CONSTANT)
    Q_PROPERTY(QString status READ status CONSTANT)
    Q_PROPERTY(QVariantList stencils READ stencils CONSTANT)
public:
    InputAdapter() { connect(this,&InputAdapter::changed,this,&InputAdapter::previewChanged); }
    bool selected=false,busy=false,gesture=false;
    int attachments=0;
    mutable int stateReads=0,stencilReads=0;
    int refreshes=0,selectionRefreshes=0,areaSelections=0,projections=0;
    RePaperNative::PreviewFrame frame;
    int begins=0,moves=0,ends=0,cancels=0;
    int propertiesBegins=0,propertiesAccepts=0,propertiesCancels=0,undoCalls=0;
    bool deferPropertiesCancel=false,rejectPropertiesCancel=false;
    bool deferPropertiesMutation=false;
    QVariantMap propertiesBaseline;
    QVariantMap pendingPropertyChanges;
    QVariantList propertyWrites;
    QStringList propertyCallOrder;
    QVector<qulonglong> presentedTokens;
    QVector<QPointF> movePoints;
    QPointF beginPoint,movePoint,endPoint;
    bool available() const { return true; }
    bool captureEnabled() const { return selected; }
    QVariantList previewStrokes() const { return {}; }
    QVariantMap pageState{{"tool","line"},{"hasSelection",false},{"selectionHandlePoints",QVariantList{}},
        {"nativeGestureActive",false},{"nativeSelectionWaiting",false},{"lineStyle","solid"},{"lineWidth",3},
        {"selectionCanTransform",true},{"selectionCanDuplicate",true},{"selectionCanRemove",true},
        {"propertiesSessionCanAccept",true},{"propertiesSessionActive",false},{"propertiesCancelGeneration",0}};
    QVariantMap state() const { ++stateReads;return pageState; }
    QVariantMap overlayState() const {
        auto result=pageState;
        result.insert("nativeGestureActive",gesture||pageState.value("nativeGestureActive").toBool());
        return result;
    }
    QVariant previewFrame() const { return QVariant::fromValue(frame); }
    QString backend() const { return "xochitl-test-services"; }
    QString status() const { return {}; }
    QVariantList stencils() const { ++stencilReads;return {}; }
    void setSelection(bool value) {pageState.insert("hasSelection",value);emit changed();}
    Q_INVOKABLE bool chooseTool(const QString &tool) {
        propertyCallOrder.append("tool:"+tool);
        // NativeScene refreshes and notifies even when reselecting the same tool.
        pageState.insert("tool",tool);emit changed();return true;
    }
    bool writeProperty(const QString &kind,const QVariantList &values,const QVariantMap &changes) {
        propertyWrites.append(QVariantMap{{"kind",kind},{"values",values}});
        propertyCallOrder.append("write:"+kind);
        if(deferPropertiesMutation){
            pendingPropertyChanges=changes;pageState.insert("nativeSelectionWaiting",true);
            pageState.insert("propertiesSessionCanEdit",false);pageState.insert("propertiesSessionCanCancel",false);
            pageState.insert("propertiesSessionCanAccept",false);
        }else for(auto it=changes.cbegin();it!=changes.cend();++it)pageState.insert(it.key(),it.value());
        emit changed();return true;
    }
    Q_INVOKABLE bool resize(qreal width,qreal height) {
        return writeProperty("resize",{width,height},{{"selectedShapeWidth",width},{"selectedShapeHeight",height}});
    }
    Q_INVOKABLE bool setStrokeColor(const QString &color) {
        return writeProperty("color",{color},{{pageState.value("tool")=="select"?"selectedLineColor":"lineColor",color}});
    }
    void finishPropertyMutation() {
        for(auto it=pendingPropertyChanges.cbegin();it!=pendingPropertyChanges.cend();++it)pageState.insert(it.key(),it.value());
        pendingPropertyChanges.clear();pageState.insert("nativeSelectionWaiting",false);
        pageState.insert("propertiesSessionCanEdit",true);pageState.insert("propertiesSessionCanCancel",true);
        pageState.insert("propertiesSessionCanAccept",true);emit changed();
    }
    Q_INVOKABLE bool beginPropertiesSession() {
        if(pageState.value("nativeSelectionWaiting").toBool())return false;
        if(pageState.value("propertiesSessionActive").toBool())return true;
        ++propertiesBegins;propertiesBaseline=pageState;
        pageState.insert("propertiesSessionActive",true);pageState.insert("propertiesSessionCanEdit",true);
        pageState.insert("propertiesSessionCanCancel",true);pageState.insert("propertiesSessionCanAccept",true);emit changed();return true;
    }
    Q_INVOKABLE bool acceptPropertiesSession() {
        if(pageState.value("nativeSelectionWaiting").toBool())return false;
        if(!pageState.value("propertiesSessionActive").toBool())return true;
        propertyCallOrder.append("accept");
        ++propertiesAccepts;pageState.insert("propertiesSessionActive",false);pageState.insert("propertiesSessionCanEdit",false);
        pageState.insert("propertiesSessionCanCancel",false);emit changed();return true;
    }
    Q_INVOKABLE bool cancelPropertiesSession() {
        ++propertiesCancels;
        if(rejectPropertiesCancel||!pageState.value("propertiesSessionCanCancel").toBool())return false;
        pageState.insert("propertiesSessionCanCancel",false);pageState.insert("propertiesSessionCanAccept",false);
        pageState.insert("propertiesSessionCancelPending",true);pageState.insert("nativeSelectionWaiting",true);
        emit changed();if(!deferPropertiesCancel)finishPropertiesCancel(true);return true;
    }
    void finishPropertiesCancel(bool success) {
        const int generation=pageState.value("propertiesCancelGeneration").toInt();
        if(success){pageState=propertiesBaseline;pageState.insert("hasSelection",false);pageState.insert("propertiesSessionActive",false);}
        pageState.insert("propertiesSessionCancelPending",false);pageState.insert("nativeSelectionWaiting",false);
        pageState.insert("propertiesSessionCanAccept",true);pageState.insert("propertiesSessionCanCancel",!success);
        pageState.insert("propertiesCancelGeneration",generation+(success?1:0));emit changed();
    }
    Q_INVOKABLE bool undo(){++undoCalls;return true;}
    Q_INVOKABLE bool attachNativePage(QObject*,QObject*,QObject*) { ++attachments; return true; }
    Q_INVOKABLE void setNativeToolActive(bool value) {
        if(selected!=value){gesture=false;selected=value;emit changed();}
    }
    Q_INVOKABLE void refreshNativeState(bool objectsMayHaveChanged=false) { ++refreshes;if(objectsMayHaveChanged)++selectionRefreshes; }
    Q_INVOKABLE void nativeViewTransformChanged() { ++projections;emit previewChanged(); }
    Q_INVOKABLE void nativeAreaSelected(int,const QVariant&) { ++areaSelections; }
    Q_INVOKABLE void nativeContentChanged() {}
    Q_INVOKABLE void nativePreviewPresented(qulonglong token) { presentedTokens.append(token); }
    Q_INVOKABLE bool pointerBegin(qreal x,qreal y,const QString&) {
        ++begins;beginPoint={x,y};gesture=!busy;emit previewChanged();return gesture;
    }
    Q_INVOKABLE bool pointerMove(qreal x,qreal y) {
        ++moves;movePoint={x,y};movePoints.append(movePoint);emit previewChanged();return gesture;
    }
    Q_INVOKABLE bool pointerMoveBatch(const QVariantList &points) {
        for(const auto &point:points){const auto p=point.toMap();pointerMove(p.value("x").toReal(),p.value("y").toReal());}
        return gesture;
    }
    Q_INVOKABLE bool pointerEnd(qreal x,qreal y) {
        ++ends;endPoint={x,y};const bool accepted=gesture;gesture=false;frame={};emit previewChanged();return accepted;
    }
    Q_INVOKABLE void pointerCancel() { ++cancels;gesture=false;frame={};pageState.insert("nativeGestureActive",false);emit previewChanged(); }
signals:
    void changed();
    void previewChanged();
};
class InputEpaperMode : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(int mode MEMBER mode)
public:
    enum Mode { Default, Animation };
    Q_ENUM(Mode)
    int mode=Default;
};
class InputWorker : public QObject {
    Q_OBJECT
    Q_PROPERTY(int jobQueueSize MEMBER jobQueueSize)
public:
    int jobQueueSize=0;
};
class InputController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pageId MEMBER pageId NOTIFY changed)
    Q_PROPERTY(bool working MEMBER working NOTIFY changed)
    Q_PROPERTY(bool undoAvailable MEMBER undoAvailable NOTIFY changed)
    Q_PROPERTY(bool redoAvailable MEMBER redoAvailable NOTIFY changed)
    Q_PROPERTY(int currentLayer MEMBER currentLayer NOTIFY changed)
    Q_PROPERTY(int selectionItemCount MEMBER selectionItemCount NOTIFY changed)
    Q_PROPERTY(QObject* worker READ worker CONSTANT)
public:
    struct Call { QString name; QVariantList arguments; };
    QVector<Call> calls;
    InputWorker queue;
    QString pageId="fixture-page";
    bool working=false,undoAvailable=false,redoAvailable=false;
    int currentLayer=0,selectionItemCount=0;
    QObject *worker() {return &queue;}
    Q_INVOKABLE void cancelPendingEdit(){calls.append(Call{"cancel",{}});}
    Q_INVOKABLE void clearSelectedItems(){calls.append(Call{"clear",{}});selectionItemCount=0;emit selectionCleared();}
    Q_INVOKABLE void addSelectionRect(QRect rect,int mode){calls.append(Call{"select",{rect,mode}});}
    Q_INVOKABLE void scaleSelectedItems(int layer,QPointF anchor,qreal sx,qreal sy){calls.append(Call{"scale",{layer,anchor,sx,sy}});}
    Q_INVOKABLE void moveSelectedItems(int layer,QPointF delta){calls.append(Call{"move",{layer,delta}});}
    Q_INVOKABLE void rotateSelectedItems(int layer,QPointF anchor,qreal angle){calls.append(Call{"rotate",{layer,anchor,angle}});}
    Q_INVOKABLE void deleteSelectedItems(int layer){calls.append(Call{"remove",{layer}});}
    Q_INVOKABLE QVariantList cloneSelectedItems(int layer,qreal scale){calls.append(Call{"clone",{layer,scale}});return {QVariantMap{{"fixture",true}}};}
    Q_INVOKABLE QRectF getItemBoundingRect(QVariantList){return {10,20,200,100};}
    Q_INVOKABLE void cloneAddAndSelectItems(int layer,QVariantList items,qreal scale,QPointF origin){calls.append(Call{"insert",{layer,items,scale,origin}});}
    Q_INVOKABLE void applyPendingEdit(int layer){calls.append(Call{"apply",{layer}});}
signals:
    void changed();
    void selectionCleared();
    void areaSelected(int layer,QRectF rect);
    void documentContentChanged();
    void currentLayerChanged();
};
class InputBlocker : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject* manager MEMBER manager)
public:
    QObject *manager=nullptr;
};
class InputSurfaceManager : public QObject {
    Q_OBJECT
public:
    QQuickItem *surface=nullptr;
    QQuickItem *overlay=nullptr;
    QRegion nativeRegion;
    std::atomic_bool captures{false};
    int updates=0;
    static QRegion occlusion(QQuickItem *item) {
        if(!item || !item->isVisible() || !item->isEnabled())return {};
        QRegion result;
        for(auto *child:item->childItems())result+=occlusion(child);
        // The pinned firmware walks the paint tree and treats ItemHasContents
        // as native-ink occlusion even when the item paints transparent pixels.
        // Only EPScreenModeItem is exempt (c33370..c33698, a50a30..a50bbc).
        if(qobject_cast<InputBlocker*>(item)
            || (item->flags().testFlag(QQuickItem::ItemHasContents) && !qobject_cast<InputEpaperMode*>(item)))
            result+=item->mapRectToScene(item->boundingRect()).toAlignedRect();
        return result;
    }
    Q_INVOKABLE void updateRegions() {
        ++updates; bool visible=false;
        if(surface)for(auto *child:surface->childItems())
            if(qobject_cast<InputBlocker*>(child)&&child->isVisible())visible=true;
        captures=visible;
        if(overlay)nativeRegion=QRegion(overlay->mapRectToScene(overlay->boundingRect()).toAlignedRect())-occlusion(overlay);
    }
};
class InputNativePen : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* surfaceManager MEMBER surfaceManager CONSTANT)
public:
    QObject *surfaceManager=nullptr;
signals:
    void penDownChanged(bool down);
};
struct InputNativeContact {
    QPointer<NativeInputScheduler> owner;
    std::thread worker;
    ~InputNativeContact(){if(owner)owner->setNativePenInput(nullptr);if(worker.joinable())worker.join();}
};
class InputTouchRecognizer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* target READ target WRITE setTarget NOTIFY targetChanged)
public:
    QPointer<QObject> watched;
    bool adopt=false;
    int received=0;
    QObject *target()const{return watched;}
    void setTarget(QObject *value){if(watched)watched->removeEventFilter(this);watched=value;if(watched)watched->installEventFilter(this);emit targetChanged();}
    Q_INVOKABLE void cancelSequence(){adopt=false;}
    bool eventFilter(QObject*,QEvent *event)override{
        if(event->type()!=QEvent::TouchBegin&&event->type()!=QEvent::TouchUpdate&&event->type()!=QEvent::TouchEnd)return false;
        ++received;return adopt;
    }
signals:
    void targetChanged();
};
class InputToolbarModel : public QObject {
    Q_OBJECT
public:
    enum class SelectionToolMode { Select, Snippet };
    Q_ENUM(SelectionToolMode)
};
class InputTileManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal scale MEMBER scale NOTIFY transformChanged)
    Q_PROPERTY(bool selectedIncluded MEMBER selectedIncluded)
    Q_PROPERTY(bool pendingTiles MEMBER pendingTiles NOTIFY pendingTilesChanged)
    Q_PROPERTY(QTransform sceneToViewTransform MEMBER sceneToViewTransform NOTIFY transformChanged)
public:
    qreal scale=1;
    bool selectedIncluded=true,pendingTiles=false;
    QTransform sceneToViewTransform;
    Q_INVOKABLE QPointF viewToScene(QPointF point)const{return sceneToViewTransform.inverted().map(point);}
    Q_INVOKABLE QPointF sceneToView(QPointF point)const{return sceneToViewTransform.map(point);}
signals:
    void transformChanged();
    void pendingTilesChanged();
};
class NativePageHostInputTest : public QObject {
    Q_OBJECT
    QQmlEngine engine;
    QQuickWindow window;
    QScopedPointer<QObject> sceneOwner,hostOwner;
    QQuickItem *scene=nullptr,*host=nullptr,*surface=nullptr;
    InputAdapter *adapter=nullptr;
    InputController *controller=nullptr;
    QStringList qmlWarnings;
    QVariant invoke(const char *name,const QVariant &a={},const QVariant &b={}) {
        QVariant result;bool invoked;
        if(b.isValid())invoked=QMetaObject::invokeMethod(host,name,Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,a),Q_ARG(QVariant,b));
        else if(a.isValid())invoked=QMetaObject::invokeMethod(host,name,Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,a));
        else invoked=QMetaObject::invokeMethod(host,name,Q_RETURN_ARG(QVariant,result));
        if(!invoked)QTest::qFail("Host method unavailable",__FILE__,__LINE__);
        return result;
    }
private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(editor_panels);
        QQuickStyle::setStyle("Basic");
        connect(&engine,&QQmlEngine::warnings,this,[this](const QList<QQmlError> &warnings){for(const auto &warning:warnings)qmlWarnings.append(warning.toString());});
        qmlRegisterType<InputAdapter>("RePaper.Editor",1,0,"EditorAdapter");
        qmlRegisterType<NativePreviewItem>("RePaper.Editor",1,0,"NativePreviewItem");
        qmlRegisterType<NativeInputScheduler>("RePaper.Editor",1,0,"NativeInputScheduler");
        qmlRegisterType<InputEpaperMode>("xofm.libs.epaper",1,0,"ScreenModeItem");
        qmlRegisterType<InputController>("RePaper.Test",1,0,"InputController");
        qmlRegisterType<InputTileManager>("RePaper.Test",1,0,"InputTileManager");
        qmlRegisterType<InputTouchRecognizer>("RePaper.Test",1,0,"InputTouchRecognizer");
        qmlRegisterType<InputBlocker>("xofm.libs.peninput",1,0,"PenInputBlocker");
        qmlRegisterUncreatableType<InputToolbarModel>("xofm.libs.toolbar",1,0,"ToolbarModel","enum fixture");
        qmlRegisterType<MockNativePenColorModel>("xofm.libs.toolbar",1,0,"PenColorModel");
        // An unversioned import resolves registered major versions as well.
        window.resize(600,700);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick 2.15
            import xofm.libs.toolbar
            import RePaper.Test 1.0
            Item {
                width: 600; height: 700
                property bool penClose: true
                property QtObject repaperNativeGestures: InputTouchRecognizer {}
                property string pageId: "fixture-page"
                property bool isLoading: true
                property bool pageError: false
                property rect availableSceneRect: Qt.rect(30, 40, 400, 500)
                property var document: ({ id: "fixture-document" })
                property QtObject toolbar: QtObject {
                    id: fixtureToolbar
                    property bool repaperToolActive: true
                    property var repaperEditorHost: null
                    property var toolbarProvider: ({colorProfile: 1})
                    property QtObject writingPen: QtObject {
                        property int color: 0
                        property real colorCode: 4278190080
                        property int tool: 6
                        signal propertyChanged()
                    }
                    property var repaperWritingPen: writingPen
                    function repaperSetWritingColor(rgb, paletteEnum) {
                        writingPen.color = paletteEnum; writingPen.colorCode = rgb
                        writingPen.propertyChanged(); return true
                    }
                    property QtObject selectionPen: QtObject {}
                    property var selectedPen: null
                    property QtObject selectionButton: QtObject {
                        property string penToolType: "selection"
                        property var pen: fixtureToolbar.selectionPen
                        property int selectedMode: ToolbarModel.SelectionToolMode.Snippet
                        function selectionToolModeSelected(mode) { selectedMode = mode }
                    }
                    function selectSelection() { selectedPen = selectionButton; repaperToolActive = false }
                }
                property QtObject controller: InputController {}
                property QtObject tileManager: null
                property QtObject readyTileManager: InputTileManager {}
                property QtObject viewport: QtObject {
                    property bool blockingUpdates: false
                    property int repaintRequests: 0
                    function requestRepaintDirty() { repaintRequests += 1 }
                }
                property QtObject penInput: QtObject { property var surfaceManager: null }
                property int selectionCloseCount: 0
                property bool forceMovePreference: false
                function endItemSelection() { selectionCloseCount += 1 }
                function forceMoveTool() { forceMovePreference = true }
            }
        )",QUrl("file:host-fixture.qml"));
        sceneOwner.reset(component.create());QVERIFY2(sceneOwner,qPrintable(component.errorString()));
        scene=qobject_cast<QQuickItem*>(sceneOwner.data());QVERIFY(scene);
        controller=qobject_cast<InputController*>(scene->property("controller").value<QObject*>());QVERIFY(controller);
        scene->setParentItem(window.contentItem());
        QQmlComponent nativeHost(&engine,QUrl::fromLocalFile(QStringLiteral(NATIVE_HOST_PATH)));
        hostOwner.reset(nativeHost.createWithInitialProperties({{"deviceScene",QVariant::fromValue(sceneOwner.data())},
            {"parent",QVariant::fromValue(scene)}}));
        QVERIFY2(hostOwner,qPrintable(nativeHost.errorString()));
        host=qobject_cast<QQuickItem*>(hostOwner.data());QVERIFY(host);
        adapter=qobject_cast<InputAdapter*>(host->property("editor").value<QObject*>());QVERIFY(adapter);
        // The native controller can exist before its tile manager. No observer
        // or cached coordinate provider may attach during this opening phase.
        QTest::qWait(20);
        QCOMPARE(adapter->attachments,0);
        scene->setProperty("tileManager",scene->property("readyTileManager"));
        QTest::qWait(20);
        QCOMPARE(adapter->attachments,0);
        scene->setProperty("isLoading",false);
        QTRY_COMPARE(adapter->attachments,1);
        // MouseArea is the only direct item exposing acceptedButtons.
        for(auto child:host->childItems())if(child->property("acceptedButtons").isValid())surface=child;
        QVERIFY(surface);window.show();QTest::qWait(20);
        QVERIFY(adapter->captureEnabled());
    }
    void init() {
        if(!controller)return;
        invoke("cancelPreviewPresentation",false);
        adapter->presentedTokens.clear();
        adapter->movePoints.clear();
        host->setProperty("inspectorOpen",false);
        adapter->propertiesBegins=adapter->propertiesAccepts=adapter->propertiesCancels=adapter->undoCalls=0;
        adapter->deferPropertiesCancel=adapter->rejectPropertiesCancel=false;
        adapter->deferPropertiesMutation=false;adapter->pendingPropertyChanges.clear();
        adapter->propertyWrites.clear();adapter->propertyCallOrder.clear();
        adapter->pageState.insert("nativeSelectionWaiting",false);adapter->pageState.insert("nativeCreationInFlight",false);
        adapter->pageState.insert("working",false);adapter->pageState.remove("nativeCreationReason");
        adapter->pageState.insert("propertiesSessionActive",false);adapter->pageState.insert("propertiesSessionCancelPending",false);
        adapter->pageState.insert("propertiesSessionCanCancel",false);adapter->pageState.insert("propertiesSessionCanAccept",true);
        adapter->pageState.insert("aspectRatioLocked",false);
        adapter->pageState.insert("aspectRatioHoldPending",false);
        adapter->pageState.remove("aspectRatioLockAnchor");
        host->setVisible(true);
        scene->setProperty("penClose",true);
        auto *native=qobject_cast<InputTouchRecognizer*>(scene->property("repaperNativeGestures").value<QObject*>());
        native->setTarget(nullptr);native->adopt=false;
        scene->property("tileManager").value<QObject*>()->setProperty("pendingTiles",false);
        auto viewport=scene->property("viewport").value<QObject*>();
        viewport->setProperty("blockingUpdates",false);viewport->setProperty("repaintRequests",0);
        controller->calls.clear();controller->working=false;controller->selectionItemCount=2;
        controller->currentLayer=0;
        adapter->setSelection(false);adapter->chooseTool("line");
        auto toolbar=scene->property("toolbar").value<QObject*>();toolbar->setProperty("repaperToolActive",true);
        adapter->setNativeToolActive(true);
    }
    void quickColorsLoadWithAnAlreadyAvailablePenAndAfterReactivation() {
        auto *loader=host->findChild<QQuickItem*>("nativeQuickColorsHost");QVERIFY(loader);
        QVERIFY(loader->property("active").toBool());QVERIFY(loader->isVisible());
        QTRY_VERIFY(loader->property("item").value<QObject*>());
        auto *palette=loader->property("item").value<QObject*>();
        QCOMPARE(palette->objectName(),QString("nativeQuickColors"));
        const auto findButton=[](QQuickItem *item,const QString &name,auto &&find)->QQuickItem*{
            if(item->objectName()==name)return item;
            for(auto *child:item->childItems())if(auto *found=find(child,name,find))return found;
            return nullptr;
        };
        for(const auto &key:{"black","red","green"}){
            auto *button=findButton(qobject_cast<QQuickItem*>(palette),QString("quickColor_")+key,findButton);QVERIFY(button);
            QVERIFY(button->isVisible());QVERIFY(button->isEnabled());
        }
        auto *toolbar=scene->property("toolbar").value<QObject*>();QVERIFY(toolbar);
        const auto pen=toolbar->property("repaperWritingPen");
        toolbar->setProperty("repaperWritingPen",QVariant::fromValue<QObject*>(nullptr));
        QVERIFY(loader->property("active").toBool());QCOMPARE(loader->property("item").value<QObject*>(),palette);
        QVERIFY(!loader->isVisible());
        toolbar->setProperty("repaperWritingPen",pen);
        QCOMPARE(loader->property("item").value<QObject*>(),palette);
        QVERIFY(loader->isVisible());
        host->setProperty("inspectorOpen",true);QVERIFY(!loader->isVisible());
        host->setProperty("inspectorOpen",false);QVERIFY(loader->isVisible());
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void transformedSurfaceMapsThroughItemCoordinates() {
        surface->setScale(1.5);surface->setTransformOrigin(QQuickItem::TopLeft);
        const auto local=QPointF(40,50);
        const auto windowPoint=surface->mapToScene(local).toPoint();
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,windowPoint);
        QCOMPARE(adapter->beginPoint,host->mapFromItem(surface,local));
        QTest::mouseMove(&window,windowPoint+QPoint(90,60));
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,windowPoint+QPoint(90,60));
        QCOMPARE(adapter->endPoint,adapter->beginPoint+QPointF(90,60));
        QVERIFY(adapter->moves>0);
        surface->setScale(1);
    }
    void nativeEraserBypassesRealBlockerFromFirstPointAndNextTipKeepsTool() {
        using Tool=NativeInputScheduler::NativeTool;
        auto *scheduler=host->findChild<NativeInputScheduler*>("nativeInputScheduler");QVERIFY(scheduler);
        const auto original=scene->property("penInput");
        InputNativePen input;InputSurfaceManager manager;manager.surface=surface;input.surfaceManager=&manager;
        std::atomic<Tool> tool{Tool::Unknown};
        scene->setProperty("penInput",QVariant::fromValue<QObject*>(&input));
        scheduler->setNativeToolProbe([&]{return tool.load();});
        struct Restore {
            NativeInputScheduler *scheduler;QQuickItem *scene;QVariant original;
            ~Restore(){scheduler->setNativeToolProbe({});scene->setProperty("penInput",original);}
        } restore{scheduler,scene,original};
        const auto contact=[&](Tool value,bool expectedCapture){
            tool=value;std::atomic_bool done{false},route{!expectedCapture};
            InputNativeContact pending{scheduler,std::thread([&]{emit input.penDownChanged(true);route=manager.captures.load();done=true;})};
            QTRY_VERIFY_WITH_TIMEOUT(done.load(),1000);
            pending.worker.join();pending.owner.clear();
            QCOMPARE(route.load(),expectedCapture);QCOMPARE(surface->isVisible(),expectedCapture);
        };
        const int starts=adapter->begins,ends=adapter->ends;
        contact(Tool::Eraser,false);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(130,140));
        QTest::mouseMove(&window,QPoint(160,180));
        emit input.penDownChanged(false);QVERIFY(!surface->isVisible());
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(200,230));
        QCOMPARE(adapter->begins,starts);QCOMPARE(adapter->ends,ends);QVERIFY(!adapter->gesture);
        contact(Tool::Pen,true);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(130,140));
        QCOMPARE(adapter->begins,starts+1);QVERIFY(adapter->gesture);
        emit input.penDownChanged(false);QVERIFY(surface->isVisible());
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(200,230));
        QCOMPARE(adapter->ends,ends+1);QCOMPARE(adapter->state().value("tool").toString(),QString("line"));
        contact(Tool::Unknown,false);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(130,140));
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(200,230));
        QCOMPARE(adapter->begins,starts+1);QCOMPARE(adapter->ends,ends+1);
        contact(Tool::Pen,true);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(130,140));
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(200,230));
        QCOMPARE(adapter->begins,starts+2);QCOMPARE(adapter->ends,ends+2);
    }
    void nativeEraserRegionIncludesThePageWithEveryCustomTool_data() {
        QTest::addColumn<QString>("customTool");
        QTest::newRow("stencil")<<QString("symbol");
        QTest::newRow("orthogonal wire")<<QString("wire");
        QTest::newRow("custom selection")<<QString("select");
    }
    void nativeEraserRegionIncludesThePageWithEveryCustomTool() {
        QFETCH(QString,customTool);
        using Tool=NativeInputScheduler::NativeTool;
        auto *scheduler=host->findChild<NativeInputScheduler*>("nativeInputScheduler");QVERIFY(scheduler);
        auto *preview=host->findChild<NativePreviewItem*>();QVERIFY(preview);
        QVERIFY(!preview->flags().testFlag(QQuickItem::ItemHasContents));QVERIFY(!preview->hasStrokes());
        adapter->chooseTool(customTool);
        const auto original=scene->property("penInput");
        InputNativePen input;InputSurfaceManager manager;manager.surface=surface;manager.overlay=host;input.surfaceManager=&manager;
        std::atomic<Tool> tool{Tool::Unknown};
        scene->setProperty("penInput",QVariant::fromValue<QObject*>(&input));
        scheduler->setNativeToolProbe([&]{return tool.load();});
        struct Restore {
            NativeInputScheduler *scheduler;QQuickItem *scene;QVariant original;
            ~Restore(){scheduler->setNativeToolProbe({});scene->setProperty("penInput",original);}
        } restore{scheduler,scene,original};
        const auto contact=[&](Tool value){
            tool=value;std::atomic_bool done{false};
            InputNativeContact pending{scheduler,std::thread([&]{emit input.penDownChanged(true);done=true;})};
            QTRY_VERIFY_WITH_TIMEOUT(done.load(),1000);
            pending.worker.join();pending.owner.clear();
        };
        const QPoint start(130,140),middle(180,200),end(220,250);
        contact(Tool::Pen);
        QVERIFY(surface->isVisible());QVERIFY(!preview->isVisible());
        QVERIFY(!manager.nativeRegion.contains(start));
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,start);
        QTest::mouseMove(&window,middle);
        QVERIFY(adapter->gesture);
        adapter->frame=RePaperNative::PreviewFrame::fromStrokes({{{QPointF(start),QPointF(middle)},3,Qt::black}});
        emit adapter->previewChanged();
        QVERIFY(preview->hasStrokes());QVERIFY(preview->isVisible());
        QVERIFY(preview->flags().testFlag(QQuickItem::ItemHasContents));
        QVERIFY(preview->width()<100);QVERIFY(preview->height()<100);
        const int starts=adapter->begins,ends=adapter->ends;
        // A native eraser contact cancels pending custom input and must remove
        // BOTH the explicit blocker and the transparent painted-page occlusion
        // before Digitizer returns to its first QRegion::contains decision.
        contact(Tool::Eraser);
        QVERIFY(!manager.captures.load());QVERIFY(!adapter->gesture);
        QVERIFY2(manager.nativeRegion.contains(start),"Transparent full-page preview still excludes native eraser input");
        QVERIFY(manager.nativeRegion.contains(middle));QVERIFY(manager.nativeRegion.contains(end));
        QVERIFY(!preview->isVisible());
        emit input.penDownChanged(false);
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,end);
        QCOMPARE(adapter->begins,starts);QCOMPARE(adapter->ends,ends);
        QVERIFY(!preview->isVisible());
        contact(Tool::Pen);
        QVERIFY(surface->isVisible());QVERIFY(!preview->isVisible());
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,start);
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,end);
        QCOMPARE(adapter->begins,starts+1);QCOMPARE(adapter->ends,ends+1);
        QCOMPARE(adapter->state().value("tool").toString(),customTool);
    }
    void stencilBarStaysAvailableForTheWireTool() {
        auto *bar=host->findChild<QQuickItem*>("nativeStencilQuickBar");QVERIFY(bar);
        adapter->chooseTool("symbol");QVERIFY(bar->isVisible());
        adapter->chooseTool("wire");QVERIFY(bar->isVisible());
        host->setProperty("inspectorOpen",true);QVERIFY(!bar->isVisible());
        host->setProperty("inspectorOpen",false);QVERIFY(bar->isVisible());
        adapter->chooseTool("select");QVERIFY(!bar->isVisible());
        adapter->chooseTool("line");QVERIFY(!bar->isVisible());
        adapter->chooseTool("wire");adapter->setNativeToolActive(false);QVERIFY(!bar->isVisible());
        adapter->setNativeToolActive(true);QVERIFY(bar->isVisible());
    }
    void fingersRemainNativeAndTheFollowingPenDrawsImmediately() {
        auto *native=qobject_cast<InputTouchRecognizer*>(scene->property("repaperNativeGestures").value<QObject*>());QVERIFY(native);
        auto *device=QTest::createTouchDevice();
        auto sequence=QTest::touchEvent(&window,device,false);
        const int starts=adapter->begins,ends=adapter->ends;
        scene->setProperty("penClose",false);native->setTarget(&window);native->adopt=true;native->received=0;
        QVERIFY(!surface->property("enabled").toBool());QVERIFY(!surface->isVisible());
        sequence.press(0,QPoint(120,130),&window).commit();QTest::qWait(20);
        QCOMPARE(adapter->begins,starts);QVERIFY(!adapter->gesture);QVERIFY(!surface->property("pressed").toBool());
        sequence.move(0,QPoint(150,160),&window).commit();QTest::qWait(20);
        sequence.stationary(0).press(1,QPoint(220,250),&window).commit();QTest::qWait(20);
        sequence.move(0,QPoint(130,140),&window).move(1,QPoint(250,280),&window).commit();QTest::qWait(20);
        sequence.stationary(0).release(1,QPoint(250,280),&window).commit();QTest::qWait(20);
        sequence.release(0,QPoint(150,160),&window).commit();QTest::qWait(20);
        QCOMPARE(adapter->begins,starts);QCOMPARE(adapter->ends,ends);QCOMPARE(native->received,6);
        native->adopt=false;native->setTarget(nullptr);scene->setProperty("penClose",true);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(130,140));
        QCOMPARE(adapter->begins,starts+1);QVERIFY(adapter->gesture);
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(200,230));
        QCOMPARE(adapter->ends,ends+1);QVERIFY(!adapter->gesture);
    }
    void loadingDuringWritingDoesNotReattachOrCancelGesture() {
        const int attachments=adapter->attachments;
        const int cancels=adapter->cancels;
        adapter->gesture=true;
        controller->setProperty("working",true);
        scene->setProperty("isLoading",true);
        QTest::qWait(25);
        QCOMPARE(adapter->attachments,attachments);
        QCOMPARE(adapter->cancels,cancels);
        QVERIFY(adapter->gesture);
        controller->setProperty("working",false);
        scene->setProperty("isLoading",false);
        QTest::qWait(25);
        QCOMPARE(adapter->attachments,attachments);
        QCOMPARE(adapter->cancels,cancels);
        QVERIFY(adapter->gesture);
        adapter->gesture=false;
    }
    void newPageWaitsForMatchingControllerAndTilesThenAttachesOnce() {
        const int attachments=adapter->attachments;
        const auto tiles=scene->property("tileManager");
        scene->setProperty("isLoading",true);
        scene->setProperty("pageId","next-page");
        scene->setProperty("tileManager",QVariant::fromValue<QObject*>(nullptr));
        QTest::qWait(25);
        QCOMPARE(adapter->attachments,attachments);
        QVERIFY(!adapter->captureEnabled());
        scene->setProperty("tileManager",tiles);
        scene->setProperty("isLoading",false);
        QTest::qWait(25);
        QCOMPARE(adapter->attachments,attachments);
        controller->setProperty("pageId","next-page");
        QTRY_COMPARE(adapter->attachments,attachments+1);
        QVERIFY(adapter->captureEnabled());
        invoke("attach");
        QCOMPARE(adapter->attachments,attachments+1);
        scene->setProperty("pageId","fixture-page");
        controller->setProperty("pageId","fixture-page");
        QTRY_COMPARE(adapter->attachments,attachments+2);
    }
    void nativeTouchTargetIsNeverChangedByCustomTools() {
        const auto native=qobject_cast<InputTouchRecognizer*>(scene->property("repaperNativeGestures").value<QObject*>());QVERIFY(native);
        QObject previousTarget;
        native->setTarget(&previousTarget);
        adapter->setNativeToolActive(false);QCOMPARE(native->target(),&previousTarget);
        adapter->setNativeToolActive(true);QCOMPARE(native->target(),&previousTarget);
        host->setVisible(false);QCOMPARE(native->target(),&previousTarget);
        host->setVisible(true);QCOMPARE(native->target(),&previousTarget);
        adapter->setNativeToolActive(false);QCOMPARE(native->target(),&previousTarget);
        native->setTarget(nullptr);adapter->setNativeToolActive(true);QVERIFY(!native->target());
    }
    void fingerCannotDrawWhilePenIsHovering() {
        QVERIFY(scene->property("penClose").toBool());
        auto *device=QTest::createTouchDevice();auto sequence=QTest::touchEvent(&window,device,false);
        const int starts=adapter->begins,ends=adapter->ends;
        sequence.press(0,QPoint(120,130),&window).commit();QTest::qWait(20);
        sequence.move(0,QPoint(170,190),&window).commit();QTest::qWait(20);
        sequence.release(0,QPoint(170,190),&window).commit();QTest::qWait(20);
        QCOMPARE(adapter->begins,starts);QCOMPARE(adapter->ends,ends);QVERIFY(!surface->property("pressed").toBool());
    }
    void penReleaseSurvivesAnEarlierProximityExit() {
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        QVERIFY(adapter->gesture);scene->setProperty("penClose",false);
        QVERIFY(surface->property("enabled").toBool());const int ends=adapter->ends;
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(250,270));
        QCOMPARE(adapter->ends,ends+1);QCOMPARE(adapter->endPoint,QPointF(250,270));
        QVERIFY(!surface->property("enabled").toBool());QVERIFY(!adapter->gesture);
    }
    void gestureFlushDeliversEveryPendingPointSynchronouslyAndKeepsContactActive() {
        const auto scheduler=host->findChild<NativeInputScheduler*>();QVERIFY(scheduler);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        QVERIFY(adapter->gesture);QVERIFY(surface->property("pressed").toBool());
        const int moves=adapter->moves,ends=adapter->ends,cancels=adapter->cancels;
        adapter->movePoints.clear();
        const QVector<QPointF> samples{{120,130},{160,180},{120,130}};
        for(const auto &point:samples)scheduler->queueMove(point.x(),point.y());
        QCOMPARE(adapter->moves,moves);
        QVERIFY(invoke("flushGestureInput").toBool());
        QCOMPARE(adapter->movePoints,samples);
        QCOMPARE(adapter->moves,moves+3);
        QCOMPARE(adapter->ends,ends);QCOMPARE(adapter->cancels,cancels);
        QVERIFY(adapter->gesture);QVERIFY(surface->property("stylusPressed").toBool());
        QVERIFY(surface->property("pressed").toBool());
        // A second flush must not resend old points or act like finish().
        QVERIFY(invoke("flushGestureInput").toBool());QCOMPARE(adapter->moves,moves+3);
        scheduler->queueMove(200,230);
        QVERIFY(invoke("flushGestureInput").toBool());
        QCOMPARE(adapter->moves,moves+4);QCOMPARE(adapter->movePoint,QPointF(200,230));
        QVERIFY(adapter->gesture);QCOMPARE(adapter->ends,ends);
        // Release still flushes a subsequent pending point and ends once.
        scheduler->queueMove(210,240);
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(220,250));
        const QVector<QPointF> expectedPrefix{{120,130},{160,180},{120,130},{200,230},{210,240}};
        QVERIFY(adapter->movePoints.size()>=expectedPrefix.size());
        QCOMPARE(adapter->movePoints.mid(0,expectedPrefix.size()),expectedPrefix);
        // Qt may synthesize a final position update while delivering release.
        // Any such update follows every queued point and uses the release point.
        for(qsizetype i=expectedPrefix.size();i<adapter->movePoints.size();++i)
            QCOMPARE(adapter->movePoints[i],QPointF(220,250));
        QCOMPARE(adapter->endPoint,QPointF(220,250));QCOMPARE(adapter->ends,ends+1);
        QVERIFY(!adapter->gesture);
        const int movesAfterRelease=adapter->moves;
        scheduler->queueMove(300,350);
        QVERIFY(invoke("flushGestureInput").toBool());QCOMPARE(adapter->moves,movesAfterRelease);
    }
    void inactiveHostFlushCancelsAndDropsBufferedMovement_data() {
        QTest::addColumn<bool>("hideHost");
        QTest::newRow("hidden host") << true;
        QTest::newRow("capture disabled") << false;
    }
    void inactiveHostFlushCancelsAndDropsBufferedMovement() {
        QFETCH(bool,hideHost);
        const auto scheduler=host->findChild<NativeInputScheduler*>();QVERIFY(scheduler);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        QVERIFY(adapter->gesture);
        const int moves=adapter->moves,ends=adapter->ends;
        scheduler->queueMove(200,230);
        const int cancels=adapter->cancels;
        if(hideHost)host->setVisible(false);
        else adapter->setNativeToolActive(false);
        QVERIFY(!invoke("flushGestureInput").toBool());
        QCOMPARE(adapter->cancels,cancels+1);QVERIFY(!adapter->gesture);
        QCOMPARE(adapter->moves,moves);QCOMPARE(adapter->ends,ends);
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(200,230));
        host->setVisible(true);adapter->setNativeToolActive(true);
        scheduler->queueMove(300,350);
        QVERIFY(invoke("flushGestureInput").toBool());
        QCOMPARE(adapter->moves,moves);QVERIFY(!adapter->gesture);
    }
    void creationSignalsStayInsideCustomSelectionProtocol() {
        QVERIFY(invoke("handleNativeSelection",0,QRectF(100,100,50,30)).toBool());
        QVERIFY(invoke("handleNativeSelection",0,QRectF()).toBool());
        adapter->setNativeToolActive(false);
        QVERIFY(!invoke("handleNativeSelection",0,QRectF(100,100,50,30)).toBool());
    }
    void selectionInkOverlayRestoresPreviousNativeTileInclusion() {
        const auto tiles=scene->property("tileManager").value<QObject*>();
        const auto overlay=[&](bool enabled){adapter->pageState.insert("nativeSelectionInkOverlay",enabled);emit adapter->changed();};
        QVERIFY(tiles->property("selectedIncluded").toBool());
        overlay(true);QVERIFY(!tiles->property("selectedIncluded").toBool());
        host->setVisible(false);QVERIFY(tiles->property("selectedIncluded").toBool());
        host->setVisible(true);QVERIFY(!tiles->property("selectedIncluded").toBool());
        overlay(false);QVERIFY(tiles->property("selectedIncluded").toBool());
        tiles->setProperty("selectedIncluded",false);
        overlay(true);overlay(false);QVERIFY(!tiles->property("selectedIncluded").toBool());
        tiles->setProperty("selectedIncluded",true);
        overlay(true);adapter->setNativeToolActive(false);QVERIFY(tiles->property("selectedIncluded").toBool());
        overlay(false);adapter->setNativeToolActive(true);
    }
    void wireSnapMarkerShowsTheMappedPortOnlyWhileSnapped() {
        const auto marker=host->findChild<QQuickItem*>("wireSnapIndicator");QVERIFY(marker);
        QVERIFY(!marker->isVisible());
        adapter->pageState.insert("wireSnapPoint",QPointF(125,175));
        adapter->pageState.insert("wireSnapActive",true);emit adapter->changed();
        QVERIFY(marker->isVisible());QCOMPARE(marker->position()+QPointF(7,7),QPointF(125,175));
        adapter->pageState.insert("wireSnapActive",false);emit adapter->changed();QVERIFY(!marker->isVisible());
    }
    void aspectRatioBadgeRequiresActiveLockAndCaptureAndHidesOnReset() {
        const auto badge=host->findChild<QQuickItem*>("aspectRatioLockIndicator");QVERIFY(badge);
        QVERIFY(!badge->isVisible());
        adapter->pageState.insert("aspectRatioLockAnchor",QPointF(120,230));
        adapter->pageState.insert("aspectRatioHoldPending",true);emit adapter->changed();
        QVERIFY(!badge->isVisible());
        adapter->pageState.insert("aspectRatioHoldPending",false);
        adapter->pageState.insert("aspectRatioLocked",true);emit adapter->changed();
        QVERIFY(badge->isVisible());
        bool hasLockLabel=false;
        for(const auto child:badge->childItems())
            if(child->property("text").toString()==QString::fromUtf8("Proportions verrouillées"))hasLockLabel=true;
        QVERIFY(hasLockLabel);
        adapter->setNativeToolActive(false);QVERIFY(!badge->isVisible());
        adapter->setNativeToolActive(true);QVERIFY(badge->isVisible());
        host->setVisible(false);QVERIFY(!badge->isVisible());
        host->setVisible(true);QVERIFY(badge->isVisible());
        adapter->pageState.insert("aspectRatioLocked",false);emit adapter->changed();
        QVERIFY(!badge->isVisible());
        adapter->pageState.remove("aspectRatioLocked");
        adapter->pageState.remove("aspectRatioLockAnchor");emit adapter->changed();
        QVERIFY(!badge->isVisible());
    }
    void aspectRatioBadgeStaysInsideAvailablePage_data() {
        QTest::addColumn<QPointF>("anchor");
        QTest::newRow("top left") << QPointF(30,40);
        QTest::newRow("top right") << QPointF(430,40);
        QTest::newRow("bottom left") << QPointF(30,540);
        QTest::newRow("bottom right") << QPointF(430,540);
        QTest::newRow("outside top left") << QPointF(-100,-100);
        QTest::newRow("outside bottom right") << QPointF(900,900);
    }
    void aspectRatioBadgeStaysInsideAvailablePage() {
        QFETCH(QPointF,anchor);
        const auto badge=host->findChild<QQuickItem*>("aspectRatioLockIndicator");QVERIFY(badge);
        adapter->pageState.insert("aspectRatioLocked",true);
        adapter->pageState.insert("aspectRatioLockAnchor",anchor);emit adapter->changed();
        QVERIFY(badge->isVisible());QVERIFY(badge->width()>0);QVERIFY(badge->height()>0);
        const QRectF page=scene->property("availableSceneRect").toRectF().adjusted(8,8,-8,-8);
        const QRectF badgeRect(badge->position(),QSizeF(badge->width(),badge->height()));
        QVERIFY2(page.contains(badgeRect),qPrintable(QString("Badge (%1,%2,%3,%4) escaped available page at anchor (%5,%6)")
            .arg(badgeRect.x()).arg(badgeRect.y()).arg(badgeRect.width()).arg(badgeRect.height()).arg(anchor.x()).arg(anchor.y())));
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void closedPageDoesNotCallMissingCoordinateMapper(){
        const auto manager=scene->property("tileManager");
        QVERIFY(scene->setProperty("tileManager",QVariant::fromValue<QObject*>(nullptr)));
        QVERIFY(!invoke("coordinateMappingAvailable").toBool());
        for(int i=0;i<100;++i){
            const auto paper=invoke("viewToPaper",QPointF(20,30)).toPointF();
            const auto view=invoke("paperToView",QPointF(20,30)).toPointF();
            QVERIFY(!std::isfinite(paper.x()));QVERIFY(!std::isfinite(view.x()));
        }
        QVERIFY(scene->setProperty("tileManager",manager));
        QVERIFY(invoke("coordinateMappingAvailable").toBool());
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void nativePreviewGateCoversGestureAndCommitThenRestores(){
        const auto update=[&](const char *key,bool value){adapter->pageState.insert(key,value);emit adapter->changed();};
        QCOMPARE(host->property("nativePreviewActive").toBool(),false);
        update("nativeGestureActive",true);
        QVERIFY(host->property("nativePreviewActive").toBool());
        update("nativeCreationInFlight",true);
        update("nativeGestureActive",false);
        QVERIFY(host->property("nativePreviewActive").toBool());
        update("nativeCreationInFlight",false);
        QVERIFY(!host->property("nativePreviewActive").toBool());
        update("nativeSelectionWaiting",true);
        QVERIFY(host->property("nativePreviewActive").toBool());
        host->setVisible(false);QVERIFY(!host->property("nativePreviewActive").toBool());
        host->setVisible(true);QVERIFY(host->property("nativePreviewActive").toBool());
        adapter->setNativeToolActive(false);QVERIFY(!host->property("nativePreviewActive").toBool());
        update("nativeSelectionWaiting",false);
        adapter->setNativeToolActive(true);
        QVERIFY(!host->property("nativePreviewActive").toBool());
    }
    void committedPreviewWaitsForTilesAndUnblockedSubmittedFrame(){
        auto tiles=scene->property("tileManager").value<QObject*>();
        auto viewport=scene->property("viewport").value<QObject*>();
        tiles->setProperty("pendingTiles",true);viewport->setProperty("blockingUpdates",true);
        QVERIFY(invoke("presentCommittedPreview",101).toBool());
        QCOMPARE(viewport->property("repaintRequests").toInt(),0);
        QVERIFY(QMetaObject::invokeMethod(&window,"frameSwapped"));
        QCoreApplication::processEvents();QVERIFY(adapter->presentedTokens.isEmpty());
        tiles->setProperty("pendingTiles",false);
        QCOMPARE(viewport->property("repaintRequests").toInt(),0);
        viewport->setProperty("blockingUpdates",false);
        QCOMPARE(viewport->property("repaintRequests").toInt(),1);
        QVERIFY(adapter->presentedTokens.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(&window,"frameSwapped"));
        QTRY_COMPARE(adapter->presentedTokens,QVector<qulonglong>{101});
        QCOMPARE(host->property("previewPresentationToken").toULongLong(),qulonglong(0));
        QVERIFY(QMetaObject::invokeMethod(&window,"frameSwapped"));
        QCoreApplication::processEvents();QCOMPARE(adapter->presentedTokens.size(),1);
    }
    void supersededAndHiddenPreviewCannotAcknowledgeAQueuedOldFrame(){
        auto tiles=scene->property("tileManager").value<QObject*>();
        QVERIFY(invoke("presentCommittedPreview",201).toBool());
        QVERIFY(QMetaObject::invokeMethod(&window,"frameSwapped"));
        tiles->setProperty("pendingTiles",true);
        QVERIFY(invoke("presentCommittedPreview",202).toBool());
        QCoreApplication::processEvents();QVERIFY(adapter->presentedTokens.isEmpty());
        host->setVisible(false);
        QCOMPARE(adapter->presentedTokens,QVector<qulonglong>{202});
        tiles->setProperty("pendingTiles",false);
        QVERIFY(QMetaObject::invokeMethod(&window,"frameSwapped"));
        QCoreApplication::processEvents();QCOMPARE(adapter->presentedTokens.size(),1);
        QVERIFY(!invoke("presentCommittedPreview",203).toBool());
        host->setVisible(true);
    }
    void missingOrReplacedPresentationServicesReleaseOrRejectThePreview(){
        const auto tiles=scene->property("tileManager"),viewport=scene->property("viewport");
        tiles.value<QObject*>()->setProperty("pendingTiles",true);
        QVERIFY(invoke("presentCommittedPreview",301).toBool());
        QVERIFY(scene->setProperty("tileManager",QVariant::fromValue<QObject*>(nullptr)));
        QCOMPARE(adapter->presentedTokens,QVector<qulonglong>{301});
        QVERIFY(!invoke("presentCommittedPreview",302).toBool());
        QVERIFY(scene->setProperty("tileManager",tiles));
        QVERIFY(scene->setProperty("viewport",QVariant::fromValue<QObject*>(nullptr)));
        QVERIFY(!invoke("presentCommittedPreview",303).toBool());
        QVERIFY(scene->setProperty("viewport",viewport));
        QVERIFY(invoke("presentCommittedPreview",304).toBool());
        host->setParentItem(nullptr);
        QCOMPARE(adapter->presentedTokens,QVector<qulonglong>({301,304}));
        QVERIFY(!invoke("presentCommittedPreview",305).toBool());
        host->setParentItem(scene);
        QVERIFY(invoke("presentCommittedPreview",306).toBool());
        QVERIFY(scene->setProperty("pageId","another-fixture-page"));
        QCOMPARE(host->property("previewPresentationToken").toULongLong(),qulonglong(0));
        // NativeScene invalidates page tokens on attach, so QML drops the old ack.
        QCOMPARE(adapter->presentedTokens,QVector<qulonglong>({301,304}));
        QVERIFY(scene->setProperty("pageId","fixture-page"));
        tiles.value<QObject*>()->setProperty("pendingTiles",false);
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void idleHoverDoesNotScheduleEditorStateUpdates() {
        surface->setProperty("hoverEnabled",true);
        QTest::qWait(10);
        QVERIFY(!surface->property("pressed").toBool());
        const int before=adapter->moves;
        QHoverEvent move(QEvent::HoverMove,QPointF(120,120),QPointF(100,100),Qt::NoModifier);
        QCoreApplication::sendEvent(surface,&move);
        QTest::qWait(30);QCOMPARE(adapter->moves,before);QVERIFY(!adapter->gesture);
        surface->setProperty("hoverEnabled",false);
    }
    void busyPressIsConsumedWhileSurfaceRemainsVisible() {
        adapter->busy=true;
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        QVERIFY(surface->isEnabled());QVERIFY(surface->isVisible());
        QVERIFY(surface->property("pressed").toBool()); // accepted despite pointerBegin=false
        QVERIFY(!adapter->gesture);
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        adapter->busy=false;
    }
    void nativeToolChangeDisablesSurfaceAndCancelsGesture() {
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        QVERIFY(adapter->gesture);
        auto toolbar=scene->property("toolbar").value<QObject*>();QVERIFY(toolbar);
        toolbar->setProperty("repaperToolActive",false);
        QVERIFY(!adapter->gesture);QVERIFY(!surface->property("enabled").toBool());QVERIFY(!surface->isVisible());
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(160,180));
        toolbar->setProperty("repaperToolActive",true);
        QVERIFY(surface->property("enabled").toBool());
    }
    void paletteSelectUsesRealSelectionButtonAndPreservesExistingSelection() {
        auto toolbar=scene->property("toolbar").value<QObject*>();QVERIFY(toolbar);
        toolbar->setProperty("repaperToolActive",true);
        const int closeCount=scene->property("selectionCloseCount").toInt();
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(host,"activateNativeSelection",Q_RETURN_ARG(QVariant,result)));
        QVERIFY(result.toBool());
        QCOMPARE(toolbar->property("selectedPen").value<QObject*>(),toolbar->property("selectionButton").value<QObject*>());
        auto selected=toolbar->property("selectedPen").value<QObject*>();QVERIFY(selected);
        QCOMPARE(selected->property("selectedMode").toInt(),int(InputToolbarModel::SelectionToolMode::Select));
        QVERIFY(!adapter->captureEnabled());QVERIFY(!surface->property("enabled").toBool());
        QVERIFY(!scene->property("forceMovePreference").toBool());
        QCOMPARE(scene->property("selectionCloseCount").toInt(),closeCount);
        toolbar->setProperty("repaperToolActive",true);
    }
    void customRegionCoversFractionalEdgesAndRejectsInvalidBounds() {
        QVERIFY(invoke("selectCustomRegion",QRectF(10.8,20.6,1.3,2.7)).toBool());
        QCOMPARE(controller->calls.size(),1);QCOMPARE(controller->calls[0].name,QString("select"));
        QCOMPARE(controller->calls[0].arguments[0].toRect(),QRect(10,20,3,4));
        controller->calls.clear();
        for(const auto rect:{QRectF(0,0,0,10),QRectF(0,0,10,-2),QRectF(1e100,0,2,2),
            QRectF(999999,0,10,10),QRectF(std::numeric_limits<qreal>::quiet_NaN(),0,2,2)})
            QVERIFY(!invoke("selectCustomRegion",rect).toBool());
        QVERIFY(controller->calls.isEmpty());
        controller->working=true;QVERIFY(!invoke("selectCustomRegion",QRectF(0,0,20,30)).toBool());
        QVERIFY(controller->calls.isEmpty());
    }
    void freeScaleAndMoveEachApplyExactlyOnce() {
        controller->currentLayer=2;
        QVERIFY(invoke("transformCustomSelection",QVariantMap{{"kind","scale"},{"anchor",QPointF(50,70)},{"sx",1.5},{"sy",0.5}}).toBool());
        QCOMPARE(controller->calls.size(),2);QCOMPARE(controller->calls[0].name,QString("scale"));
        QCOMPARE(controller->calls[0].arguments,QVariantList({2,QPointF(50,70),1.5,0.5}));
        QCOMPARE(controller->calls[1].name,QString("apply"));QCOMPARE(controller->calls[1].arguments,QVariantList({2}));
        controller->calls.clear();
        QVERIFY(invoke("transformCustomSelection",QVariantMap{{"kind","move"},{"delta",QPointF(-30,50)}}).toBool());
        QCOMPARE(controller->calls.size(),2);QCOMPARE(controller->calls[0].name,QString("move"));
        QCOMPARE(controller->calls[0].arguments,QVariantList({2,QPointF(-30,50)}));
        QCOMPARE(controller->calls[1].name,QString("apply"));
    }
    void invalidAffineOperationsDoNotDispatchPartialEdits() {
        for(const auto change:{QVariantMap{{"kind","scale"},{"anchor",QPointF(10,20)},{"sx",0},{"sy",0.5}},
            QVariantMap{{"kind","scale"},{"anchor",QPointF(10,20)},{"sx",1.5},{"sy",-0.5}},
            QVariantMap{{"kind","scale"},{"anchor",QPointF(10,20)},{"sx",1e100},{"sy",0.5}},
            QVariantMap{{"kind","move"},{"delta",QPointF(1e100,20)}},
            QVariantMap{{"kind","rotate"},{"anchor",QPointF(10,20)},{"angle",361}}})
            QVERIFY(!invoke("transformCustomSelection",change).toBool());
        QVERIFY(controller->calls.isEmpty());
    }
    void verifiedTransformDoesNotWaitForDelayedGuiSelectionCount() {
        controller->selectionItemCount=0;
        QVERIFY(invoke("transformCustomSelection",QVariantMap{{"kind","move"},{"delta",QPointF(10,20)}}).toBool());
        QCOMPARE(controller->calls.size(),2);QCOMPARE(controller->calls[0].name,QString("move"));
        QCOMPARE(controller->calls[1].name,QString("apply"));
        controller->calls.clear();controller->working=true;
        QVERIFY(!invoke("transformCustomSelection",QVariantMap{{"kind","move"},{"delta",QPointF(10,20)}}).toBool());
        QVERIFY(controller->calls.isEmpty());
    }
    void freeRotationDispatchesDegreesAndOneApply(){
        QVERIFY(invoke("transformCustomSelection",QVariantMap{{"kind","rotate"},{"anchor",QPointF(50,70)},{"angle",-37.5}}).toBool());
        QCOMPARE(controller->calls.size(),2);QCOMPARE(controller->calls[0].name,QString("rotate"));
        QCOMPARE(controller->calls[0].arguments,QVariantList({0,QPointF(50,70),-37.5}));
        QCOMPARE(controller->calls[1].name,QString("apply"));
    }
    void deleteAndDuplicateUseTheirOwnNativeCommands() {
        QVERIFY(invoke("transformCustomSelection",QVariantMap{{"kind","remove"}}).toBool());
        QCOMPARE(controller->calls.size(),1);QCOMPARE(controller->calls[0].name,QString("remove"));
        controller->calls.clear();
        QVERIFY(invoke("transformCustomSelection",QVariantMap{{"kind","duplicate"}}).toBool());
        QCOMPARE(controller->calls.size(),2);QCOMPARE(controller->calls[0].name,QString("clone"));
        QCOMPARE(controller->calls[1].name,QString("insert"));
        QCOMPARE(controller->calls[1].arguments[3].toPointF(),QPointF(134,94));
    }
    void customHookOwnsSelectionWithoutSwitchingTheNativeBrush() {
        auto toolbar=scene->property("toolbar").value<QObject*>();const auto selected=toolbar->property("selectedPen");
        adapter->chooseTool("select");
        QVERIFY(invoke("handleNativeSelection",0,QRectF(10,20,200,100)).toBool());
        QCOMPARE(toolbar->property("selectedPen"),selected);QVERIFY(adapter->captureEnabled());
        adapter->chooseTool("line");QVERIFY(invoke("handleNativeSelection",0,QRectF(10,20,200,100)).toBool());
        QVERIFY(controller->calls.isEmpty());
    }
    void nativeZoomSamplesDoNotDispatchIdleEditorWork_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("native pen")<<QString("native");
        QTest::newRow("custom tool idle")<<QString("custom");
        QTest::newRow("custom selection")<<QString("selection");
        QTest::newRow("pending native commit")<<QString("pending");
        QTest::newRow("retained preview")<<QString("preview");
    }
    void nativeZoomSamplesDoNotDispatchIdleEditorWork() {
        QFETCH(QString,mode);
        if(mode=="native")adapter->setNativeToolActive(false);
        if(mode=="selection"){
            adapter->chooseTool("select");adapter->setSelection(true);
            QTRY_COMPARE(adapter->propertiesBegins,1);
        }
        if(mode=="pending"){adapter->pageState.insert("nativeSelectionWaiting",true);emit adapter->changed();}
        if(mode=="preview"){
            adapter->frame=RePaperNative::PreviewFrame::fromStrokes({{{QPointF(50,80),QPointF(100,100)},3,Qt::black}});
            emit adapter->previewChanged();
        }
        QCoreApplication::processEvents();
        const int cancels=adapter->cancels,refreshes=adapter->refreshes;
        const int begins=adapter->propertiesBegins,accepts=adapter->propertiesAccepts;
        const int projections=adapter->projections;
        adapter->stateReads=0;
        auto *tiles=scene->property("tileManager").value<QObject*>();QVERIFY(tiles);
        for(int i=0;i<120;++i)QVERIFY(QMetaObject::invokeMethod(tiles,"transformChanged"));
        QCoreApplication::processEvents();
        QCOMPARE(adapter->stateReads,0);QCOMPARE(adapter->refreshes,refreshes);
        QCOMPARE(adapter->cancels,cancels);QCOMPARE(adapter->propertiesBegins,begins);
        QCOMPARE(adapter->propertiesAccepts,accepts);
        QCOMPARE(adapter->projections,projections+((mode=="selection"||mode=="pending"||mode=="preview")?1:0));
        adapter->frame={};emit adapter->previewChanged();
    }
    void nativeTransformPassesThroughQmlAsAnExactAffineValue() {
        auto *tiles=qobject_cast<InputTileManager*>(scene->property("tileManager").value<QObject*>());QVERIFY(tiles);
        QTransform transform;transform.translate(33,-47);transform.rotate(17);transform.scale(2.5,2.5);
        tiles->sceneToViewTransform=transform;emit tiles->transformChanged();
        const auto transported=invoke("readViewTransform");
        QVERIFY(transported.canConvert<QTransform>());QCOMPARE(transported.value<QTransform>(),transform);
        QCOMPARE(invoke("paperToView",QPointF(12,34)).toPointF(),transform.map(QPointF(12,34)));
        QCOMPARE(invoke("viewToPaper",transform.map(QPointF(12,34))).toPointF(),QPointF(12,34));
        tiles->sceneToViewTransform={};emit tiles->transformChanged();
    }
    void nativeZoomCancelsALiveGestureOnceThenDetaches() {
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        QVERIFY(adapter->gesture);
        const int cancels=adapter->cancels,moves=adapter->moves;
        adapter->stateReads=0;
        auto *tiles=scene->property("tileManager").value<QObject*>();
        for(int i=0;i<120;++i)QVERIFY(QMetaObject::invokeMethod(tiles,"transformChanged"));
        QCoreApplication::processEvents();
        QCOMPARE(adapter->cancels,cancels+1);QVERIFY(!adapter->gesture);
        QCOMPARE(adapter->stateReads,0);QCOMPARE(adapter->moves,moves);
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
    }
    void nativeControllerBurstsStayDisconnectedWhenCustomToolsAreInactive() {
        adapter->setNativeToolActive(false);QCoreApplication::processEvents();
        const int refreshes=adapter->refreshes,areas=adapter->areaSelections,cancels=adapter->cancels;
        adapter->stateReads=0;
        for(int i=0;i<120;++i){
            emit controller->selectionCleared();emit controller->areaSelected(0,QRectF(10,20,30,40));
            emit controller->currentLayerChanged();
        }
        QCoreApplication::processEvents();
        QCOMPARE(adapter->refreshes,refreshes);QCOMPARE(adapter->areaSelections,areas);
        QCOMPARE(adapter->cancels,cancels);QCOMPARE(adapter->stateReads,0);
        adapter->setNativeToolActive(true);
        emit controller->selectionCleared();
        QCOMPARE(adapter->refreshes,refreshes+1);QVERIFY(adapter->selectionRefreshes>0);
    }
    void previewSamplesDoNotReadTheFullEditorStateOrSynchronizeProperties_data() {
        QTest::addColumn<bool>("withInspector");
        QTest::newRow("drawing")<<false;
        QTest::newRow("inspector open")<<true;
    }
    void previewSamplesDoNotReadTheFullEditorStateOrSynchronizeProperties() {
        QFETCH(bool,withInspector);
        if(withInspector){adapter->chooseTool("select");adapter->setSelection(true);QTRY_COMPARE(adapter->propertiesBegins,1);}
        QCoreApplication::processEvents();
        const int begins=adapter->propertiesBegins,accepts=adapter->propertiesAccepts,cancels=adapter->propertiesCancels;
        adapter->stateReads=0;
        for(int i=0;i<120;++i){
            adapter->frame=RePaperNative::PreviewFrame::fromStrokes({{{QPointF(50,80),QPointF(100+i,100)},3,Qt::black}});
            emit adapter->previewChanged();
        }
        QCoreApplication::processEvents();
        QCOMPARE(adapter->stateReads,0);QCOMPARE(adapter->propertiesBegins,begins);
        QCOMPARE(adapter->propertiesAccepts,accepts);QCOMPARE(adapter->propertiesCancels,cancels);
        adapter->frame={};emit adapter->previewChanged();
    }
    void retainedInspectorReopensWithFreshSessionsAndDiscardsUnfinishedCanceledText() {
        adapter->pageState.insert("selectedShapeWidth",240);adapter->pageState.insert("selectedShapeHeight",160);
        adapter->pageState.insert("selectionCanResize",true);
        adapter->chooseTool("select");adapter->setSelection(true);QTRY_COMPARE(adapter->propertiesBegins,1);
        auto *width=host->findChild<QQuickItem*>("editorShapeWidth");QVERIFY(width);
        auto *field=width->property("contentItem").value<QQuickItem*>();QVERIFY(field);
        for(int i=0;i<20;++i){
            QVERIFY(invoke("keepPropertiesAndClose",adapter->pageState).toBool());
            QVERIFY(!host->property("inspectorOpen").toBool());
            host->setProperty("inspectorOpen",true);QTRY_COMPARE(adapter->propertiesBegins,i+2);
            QCOMPARE(host->findChild<QQuickItem*>("editorShapeWidth"),width);
        }
        field->forceActiveFocus();QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(&window,Qt::Key_A,Qt::ControlModifier);
        for(const auto character:QString("999"))QTest::keyClick(&window,character.toLatin1());
        QCOMPARE(field->property("text").toString(),QString("999"));
        adapter->propertyWrites.clear();
        auto *cancel=host->findChild<QQuickItem*>("cancelNativeProperties");QVERIFY(cancel);
        QTest::mouseClick(&window,Qt::LeftButton,Qt::NoModifier,cancel->mapToScene(QPointF(cancel->width()/2,cancel->height()/2)).toPoint());
        QVERIFY(!host->property("inspectorOpen").toBool());QVERIFY(adapter->propertyWrites.isEmpty());
        adapter->setSelection(true);QTRY_COMPARE(adapter->propertiesBegins,22);
        QCOMPARE(host->findChild<QQuickItem*>("editorShapeWidth"),width);
        QCOMPARE(field->property("text").toString(),QString("240"));
        QCOMPARE(adapter->pageState.value("selectedShapeWidth").toInt(),240);
    }
    void directCustomSelectionKeepsCaptureAndClosesProperties() {
        auto toolbar=scene->property("toolbar").value<QObject*>();QVERIFY(toolbar);
        toolbar->setProperty("repaperToolActive",true);const auto selected=toolbar->property("selectedPen");
        adapter->chooseTool("rectangle");adapter->setSelection(true);
        QTest::mousePress(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));QVERIFY(adapter->gesture);
        QVERIFY(invoke("activateCustomSelection").toBool());
        QCOMPARE(adapter->pageState.value("tool").toString(),QString("select"));
        QVERIFY(adapter->captureEnabled());QVERIFY(!adapter->gesture);QVERIFY(!host->property("inspectorOpen").toBool());
        QCOMPARE(toolbar->property("selectedPen"),selected);QVERIFY(controller->calls.isEmpty());
        QTest::mouseRelease(&window,Qt::LeftButton,Qt::NoModifier,QPoint(120,130));
        host->setProperty("inspectorOpen",true);QVERIFY(invoke("activateCustomSelection").toBool());
        QVERIFY(!host->property("inspectorOpen").toBool());QVERIFY(adapter->pageState.value("hasSelection").toBool());
        toolbar->setProperty("repaperToolActive",false);QVERIFY(!invoke("activateCustomSelection").toBool());
        QVERIFY(!adapter->captureEnabled());QVERIFY(controller->calls.isEmpty());
        toolbar->setProperty("repaperToolActive",true);
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void inspectorPreservesCustomPenAndSelection() {
        auto toolbar=scene->property("toolbar").value<QObject*>();const auto selected=toolbar->property("selectedPen");
        const int closeCount=scene->property("selectionCloseCount").toInt();
        adapter->chooseTool("select");adapter->setSelection(true);QTest::qWait(20);
        QVERIFY(host->property("hasCustomSelection").toBool());QVERIFY(host->property("inspectorOpen").toBool());
        QCOMPARE(toolbar->property("selectedPen"),selected);QVERIFY(adapter->captureEnabled());
        QCOMPARE(scene->property("selectionCloseCount").toInt(),closeCount);QVERIFY(controller->calls.isEmpty());
        const auto sidebars=host->findChildren<QObject*>();bool loaded=false;
        for(auto object:sidebars)if(object->property("inspectorOnly").toBool()){
            loaded=true;QCOMPARE(object->property("mode").toString(),QString("properties"));
            QCOMPARE(object->property("editor").value<QObject*>(),static_cast<QObject*>(adapter));
        }
        QVERIFY(loaded);QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
        host->setProperty("inspectorOpen",false);QVERIFY(adapter->captureEnabled());
        QCOMPARE(toolbar->property("selectedPen"),selected);
    }
    void propertiesCancelWaitsForConfirmationAndClosesWithoutDocumentUndo() {
        adapter->pageState.insert("selectedShapeWidth",240);adapter->pageState.insert("selectedLineColor","#000000");
        adapter->chooseTool("select");adapter->setSelection(true);
        QTRY_COMPARE(adapter->propertiesBegins,1);QVERIFY(host->property("inspectorOpen").toBool());
        // Simulate two confirmed native edits; the worker tests verify rollback.
        adapter->pageState.insert("selectedShapeWidth",380);adapter->pageState.insert("selectedLineColor","#d90707");emit adapter->changed();
        auto *button=host->findChild<QQuickItem*>("cancelNativeProperties");QVERIFY(button);QVERIFY(button->isVisible());QVERIFY(button->isEnabled());
        adapter->deferPropertiesCancel=true;
        QTest::mouseClick(&window,Qt::LeftButton,Qt::NoModifier,button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint());
        QCOMPARE(adapter->propertiesCancels,1);QCOMPARE(adapter->undoCalls,0);QVERIFY(host->property("inspectorOpen").toBool());
        QVERIFY(!button->isEnabled());QVERIFY(!host->findChild<QQuickItem*>("acceptNativeProperties")->isEnabled());
        adapter->setSelection(false);QVERIFY(button->isVisible()); // Keep progress visible while the worker clears selection.
        adapter->finishPropertiesCancel(true);QVERIFY(!host->property("inspectorOpen").toBool());
        QCOMPARE(adapter->pageState.value("selectedShapeWidth").toInt(),240);
        QCOMPARE(adapter->pageState.value("selectedLineColor").toString(),QString("#000000"));
        QCOMPARE(adapter->propertiesAccepts,0);QCOMPARE(adapter->undoCalls,0);
    }
    void propertiesCancelRemainsAvailableAfterDeletingTheSelection() {
        adapter->chooseTool("select");adapter->setSelection(true);QTRY_COMPARE(adapter->propertiesBegins,1);
        adapter->setSelection(false); // A confirmed Delete may leave no selected ink.
        const auto button=host->findChild<QQuickItem*>("cancelNativeProperties");QVERIFY(button);QVERIFY(button->isVisible());QVERIFY(button->isEnabled());
        QVERIFY(invoke("cancelProperties").toBool());QCOMPARE(adapter->propertiesCancels,1);QVERIFY(!host->property("inspectorOpen").toBool());
    }
    void propertiesSessionWaitsForNativeReadinessBeforeCapturingBaseline() {
        adapter->pageState.insert("working",true);adapter->chooseTool("select");adapter->setSelection(true);QTest::qWait(20);
        QCOMPARE(adapter->propertiesBegins,0);QVERIFY(!host->property("propertiesSessionAttempted").toBool());
        adapter->pageState.insert("working",false);adapter->pageState.insert("nativeCreationReason","pending native transform");emit adapter->changed();QTest::qWait(20);
        QCOMPARE(adapter->propertiesBegins,0);
        adapter->pageState.insert("nativeCreationReason",QString());emit adapter->changed();QTRY_COMPARE(adapter->propertiesBegins,1);
    }
    void propertiesCancelRefusalKeepsPopupOpenAndAllowsRetry() {
        adapter->chooseTool("select");adapter->setSelection(true);QTRY_COMPARE(adapter->propertiesBegins,1);
        adapter->rejectPropertiesCancel=true;QVERIFY(!invoke("cancelProperties").toBool());
        QVERIFY(host->property("inspectorOpen").toBool());QCOMPARE(adapter->undoCalls,0);
        adapter->rejectPropertiesCancel=false;QVERIFY(invoke("cancelProperties").toBool());
        QVERIFY(!host->property("inspectorOpen").toBool());QCOMPARE(adapter->propertiesCancels,2);
    }
    void propertiesValidationKeepsEditsAndStartsANewBaselineOnReopen() {
        adapter->pageState.insert("selectedShapeWidth",240);adapter->chooseTool("select");adapter->setSelection(true);
        QTRY_COMPARE(adapter->propertiesBegins,1);adapter->pageState.insert("selectedShapeWidth",380);emit adapter->changed();
        for(int i=0;i<8;++i)emit adapter->changed();QTest::qWait(20);QCOMPARE(adapter->propertiesBegins,1);
        auto *button=host->findChild<QQuickItem*>("acceptNativeProperties");QVERIFY(button);QVERIFY(button->isVisible());
        QTest::mouseClick(&window,Qt::LeftButton,Qt::NoModifier,button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint());
        QCOMPARE(adapter->propertiesAccepts,1);QVERIFY(!host->property("inspectorOpen").toBool());
        QCOMPARE(adapter->pageState.value("selectedShapeWidth").toInt(),380);QCOMPARE(adapter->undoCalls,0);
        host->setProperty("inspectorOpen",true);QTRY_COMPARE(adapter->propertiesBegins,2);
        QCOMPARE(adapter->propertiesBaseline.value("selectedShapeWidth").toInt(),380);
        adapter->pageState.insert("selectedShapeWidth",500);emit adapter->changed();QVERIFY(invoke("cancelProperties").toBool());
        QCOMPARE(adapter->pageState.value("selectedShapeWidth").toInt(),380);QVERIFY(!host->property("inspectorOpen").toBool());
    }
    void directSelectClosesPropertiesAndAcceptsAfterPendingEdit() {
        adapter->chooseTool("select");adapter->setSelection(true);QTRY_COMPARE(adapter->propertiesBegins,1);
        adapter->pageState.insert("nativeSelectionWaiting",true);emit adapter->changed();
        QVERIFY(invoke("activateCustomSelection").toBool());QVERIFY(!host->property("inspectorOpen").toBool());
        QVERIFY(host->property("acceptPropertiesWhenIdle").toBool());QCOMPARE(adapter->propertiesAccepts,0);
        adapter->pageState.insert("nativeSelectionWaiting",false);emit adapter->changed();
        QCOMPARE(adapter->propertiesAccepts,1);QVERIFY(!host->property("acceptPropertiesWhenIdle").toBool());
        QVERIFY(!host->property("inspectorOpen").toBool());QCOMPARE(adapter->propertiesCancels,0);
    }
    void directSelectCommitsUnfinishedPropertyBeforeToolRefresh_data() {
        QTest::addColumn<bool>("colorInput");
        QTest::newRow("numeric width") << false;
        QTest::newRow("hex color") << true;
    }
    void directSelectCommitsUnfinishedPropertyBeforeToolRefresh() {
        QFETCH(bool,colorInput);
        adapter->pageState.insert("selectedShapeWidth",240);adapter->pageState.insert("selectedShapeHeight",160);
        adapter->pageState.insert("selectedLineColor","#000000");adapter->pageState.insert("selectionCanResize",true);
        adapter->pageState.insert("selectionCanChangeColor",true);
        adapter->chooseTool("select");adapter->setSelection(true);QTRY_COMPARE(adapter->propertiesBegins,1);
        QQuickItem *input=nullptr;
        if(colorInput){
            const auto picker=host->findChild<QQuickItem*>("selectionStrokeColor");QVERIFY(picker);
            input=picker->findChild<QQuickItem*>("strokeColorHex");
        }else{
            const auto width=host->findChild<QQuickItem*>("editorShapeWidth");QVERIFY(width);
            input=width->property("contentItem").value<QQuickItem*>();
        }
        QVERIFY(input);QVERIFY(input->isEnabled());
        adapter->deferPropertiesMutation=true;
        adapter->propertyWrites.clear();adapter->propertyCallOrder.clear();
        input->forceActiveFocus();QTRY_VERIFY(input->hasActiveFocus());
        const QString text=colorInput?QString("#136aca"):QString("300");
        QTest::keyClick(&window,Qt::Key_A,Qt::ControlModifier);
        for(const auto character:text)QTest::keyClick(&window,character.toLatin1());
        QCOMPARE(input->property("text").toString(),text);QVERIFY(adapter->propertyWrites.isEmpty());
        // The real toolbar uses NoFocus; invoke its host route while the field
        // still owns focus, without Enter or a simulated focus-stealing click.
        QVERIFY(invoke("activateCustomSelection").toBool());
        const QString kind=colorInput?QString("color"):QString("resize");
        QCOMPARE(adapter->propertyWrites.size(),1);
        QCOMPARE(adapter->propertyWrites.first().toMap().value("kind").toString(),kind);
        const QVariantList expected=colorInput?QVariantList{QString("#136aca")}:QVariantList{qreal(300),qreal(160)};
        QCOMPARE(adapter->propertyWrites.first().toMap().value("values").toList(),expected);
        QCOMPARE(adapter->propertyCallOrder,QStringList({"write:"+kind,"tool:select"}));
        QVERIFY(!host->property("inspectorOpen").toBool());QVERIFY(host->property("acceptPropertiesWhenIdle").toBool());
        QCOMPARE(adapter->propertiesAccepts,0);QVERIFY(adapter->captureEnabled());
        adapter->finishPropertyMutation();
        QCOMPARE(adapter->propertiesAccepts,1);
        QCOMPARE(adapter->propertyCallOrder,QStringList({"write:"+kind,"tool:select","accept"}));
        QVERIFY(!host->property("acceptPropertiesWhenIdle").toBool());QVERIFY(!host->property("inspectorOpen").toBool());
        QCOMPARE(adapter->propertiesCancels,0);QCOMPARE(adapter->undoCalls,0);
        QVERIFY2(qmlWarnings.isEmpty(),qPrintable(qmlWarnings.join('\n')));
    }
    void fingersCanClickPropertiesWithoutStartingAPageStroke(){
        scene->setProperty("penClose",false);adapter->chooseTool("select");adapter->setSelection(true);host->setProperty("inspectorOpen",true);
        QTest::qWait(20);auto *button=host->findChild<QQuickItem*>("closeNativeInspector");QVERIFY(button);QVERIFY(button->isVisible());
        const int begins=adapter->begins,ends=adapter->ends;
        const auto position=button->mapToScene(QPointF(button->width()/2,button->height()/2)).toPoint();
        auto *device=QTest::createTouchDevice();auto sequence=QTest::touchEvent(&window,device,false);
        sequence.press(0,position,&window).commit();QTest::qWait(20);sequence.release(0,position,&window).commit();QTest::qWait(20);
        QVERIFY(!host->property("inspectorOpen").toBool());QCOMPARE(adapter->begins,begins);QCOMPARE(adapter->ends,ends);
        QVERIFY(host->findChild<QQuickItem*>("nativePropertiesButton")->isVisible());
    }
};
QTEST_MAIN(NativePageHostInputTest)
#include "NativePageHostInputTest.moc"

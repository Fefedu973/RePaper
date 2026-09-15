import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15
import "qrc:/paper" as Paper
import xofm.libs.epaper as Epaper
import xofm.libs.peninput
import xofm.libs.toolbar
import RePaper.Editor 1.0

// Original integration code. Native method contracts were checked against the
// exact local firmware and the pinned community references in PROVENANCE.md.
Item {
    id: host
    required property var deviceScene
    readonly property var controller: deviceScene.controller
    property alias editor: adapter
    // One cold snapshot and one small live overlay snapshot per notification.
    // Reading the full native state separately in every binding used to
    // re-enter the controller while those same bindings were being evaluated.
    readonly property var editorState: adapter.state || ({})
    readonly property var overlayState: adapter.overlayState || ({})
    property bool inspectorOpen: false
    property bool propertiesSessionAttempted: false
    property bool propertiesSessionDispatching: false
    property bool acceptPropertiesWhenIdle: false
    property var observedPropertiesCancelGeneration: 0
    property int attachmentGeneration: 0
    property var attachedController: null
    property var attachedTileManager: null
    property string attachedPageId: ""
    readonly property bool attachmentMatchesPage: attachedController === controller
        && attachedTileManager === deviceScene.tileManager && attachedPageId === deviceScene.pageId
    readonly property bool pageReadyForAttachment: !!controller && !!deviceScene.tileManager && !!deviceScene.viewport
        && deviceScene.isLoading === false && deviceScene.pageError === false && !controller.working
        && !!deviceScene.pageId && controller.pageId === deviceScene.pageId
    onPageReadyForAttachmentChanged: Qt.callLater(host.attachWhenReady)
    readonly property bool nativeOperationPending: !!editorState.nativeSelectionWaiting || !!editorState.nativeCreationInFlight
    // DeviceSceneView folds this into its own ScreenDriver.snapMode, exactly
    // as the native draw-and-hold shape overlay does for live e-paper updates.
    readonly property bool nativePreviewActive: visible && adapter.captureEnabled && (!!overlayState.nativeGestureActive || nativeOperationPending)
    onNativePreviewActiveChanged: { if (!nativePreviewActive) cancelPreviewPresentation(true) }
    readonly property var previewWindow: host.Window.window
    property var previewPresentationToken: 0
    property int previewPresentationGeneration: 0
    property bool previewFrameRequested: false
    property bool previewFrameSubmitted: false
    property var previewPresentationViewport: null
    property var previewPresentationTiles: null
    onPreviewWindowChanged: cancelPreviewPresentation(true)
    onVisibleChanged: { if (!visible) cancelPreviewPresentation(true) }
    readonly property bool hasCustomSelection: adapter.captureEnabled && editorState.tool === "select" && !!editorState.hasSelection
    onHasCustomSelectionChanged: { if (hasCustomSelection && !acceptPropertiesWhenIdle) inspectorOpen = true }
    onInspectorOpenChanged: {
        propertiesSessionAttempted = false
        if (inspectorOpen) Qt.callLater(host.ensurePropertiesSession)
        else if (!propertiesSessionDispatching) host.keepPropertiesAndClose()
    }
    anchors.fill: parent

    EditorAdapter { id: adapter }
    function ensurePropertiesSession() {
        if (!inspectorOpen || !hasCustomSelection || nativeOperationPending || !!editorState.working || !!editorState.nativeCreationReason
                || propertiesSessionAttempted || propertiesSessionDispatching || acceptPropertiesWhenIdle) return
        propertiesSessionAttempted = true
        propertiesSessionDispatching = true
        adapter.beginPropertiesSession()
        propertiesSessionDispatching = false
    }
    function keepPropertiesAndClose(snapshot) {
        if (propertiesSessionDispatching) return false
        const state = snapshot || editorState
        if (!inspectorOpen && !state.propertiesSessionActive && !acceptPropertiesWhenIdle) return true
        propertiesSessionDispatching = true
        const accepted = adapter.acceptPropertiesSession()
        acceptPropertiesWhenIdle = !accepted && (!!state.nativeSelectionWaiting || !!state.nativeCreationInFlight)
        if (accepted || acceptPropertiesWhenIdle) inspectorOpen = false
        propertiesSessionDispatching = false
        return accepted
    }
    function cancelProperties() {
        if (propertiesSessionDispatching) return false
        propertiesSessionDispatching = true
        const accepted = adapter.cancelPropertiesSession()
        propertiesSessionDispatching = false
        host.syncPropertiesSession(adapter.state)
        return accepted
    }
    function syncPropertiesSession(snapshot) {
        if (propertiesSessionDispatching) return
        const state = snapshot || editorState
        const generation = Number(state.propertiesCancelGeneration) || 0
        if (generation !== observedPropertiesCancelGeneration) {
            observedPropertiesCancelGeneration = generation
            acceptPropertiesWhenIdle = false
            propertiesSessionDispatching = true
            inspectorOpen = false
            propertiesSessionDispatching = false
            return
        }
        if (acceptPropertiesWhenIdle && !state.nativeSelectionWaiting && !state.nativeCreationInFlight) host.keepPropertiesAndClose(state)
        if (inspectorOpen && (state.tool !== "select" || !host.deviceScene.toolbar || !host.deviceScene.toolbar.repaperToolActive)) {
            host.keepPropertiesAndClose(state)
            return
        }
        if (inspectorOpen) Qt.callLater(host.ensurePropertiesSession)
    }
    Connections {
        target: host.inspectorOpen || host.acceptPropertiesWhenIdle || host.editorState.propertiesSessionActive
            || host.editorState.propertiesSessionCancelPending ? adapter : null
        // A confirmed Cancel or queued acceptance closes immediately. Read one
        // cold snapshot at this session boundary, before touching derived QML
        // bindings; those bindings may still be settling this notification.
        function onChanged() { host.syncPropertiesSession(adapter.state) }
    }
    function cancelInput() {
        const hadGesture = input.stylusPressed || !!overlayState.nativeGestureActive
        input.stylusPressed = false
        inputScheduler.cancel()
        if (hadGesture) adapter.pointerCancel()
    }
    function flushGestureInput() {
        if (!host.visible || !adapter.captureEnabled || !inputScheduler.penCaptureAllowed) { host.cancelInput(); return false }
        inputScheduler.flush()
        return true
    }
    NativeInputScheduler {
        id: inputScheduler
        objectName: "nativeInputScheduler"
        target: input
        coordinateItem: host
        nativePenInput: host.deviceScene.penInput
        enabled: host.visible && adapter.captureEnabled
        onCancelRequested: { host.cancelInput(); host.cancelPreviewPresentation(true) }
        onMoveBatch: points => adapter.pointerMoveBatch(points)
    }
    Binding {
        target: host.deviceScene.tileManager
        property: "selectedIncluded"
        value: false
        when: !!host.deviceScene.tileManager && host.visible && adapter.captureEnabled && inputScheduler.penCaptureAllowed && !!host.editorState.nativeSelectionInkOverlay
        restoreMode: Binding.RestoreBindingOrValue
    }
    function attach() {
        // NativeScene caches its coordinate provider and acquires the native
        // Page observer here. Both must wait until Xochitl has loaded the page.
        // Normal rendering/working transitions must not reset an active tool.
        if (!pageReadyForAttachment || attachmentMatchesPage) return
        inspectorOpen = false
        propertiesSessionAttempted = false
        acceptPropertiesWhenIdle = false
        attachmentGeneration += 1
        cancelPreviewPresentation(false)
        host.cancelInput()
        if (deviceScene.toolbar) deviceScene.toolbar.repaperEditorHost = host
        adapter.attachNativePage(controller, deviceScene, host)
        attachedController = controller
        attachedTileManager = deviceScene.tileManager
        attachedPageId = deviceScene.pageId
        syncNativeTool()
    }
    function attachWhenReady() {
        syncNativeTool()
        attach()
    }
    function syncNativeTool() {
        adapter.setNativeToolActive(attachmentMatchesPage && !!deviceScene.toolbar && deviceScene.toolbar.repaperToolActive)
    }
    function activateCustomSelection() {
        host.syncNativeTool()
        if (!adapter.available || !adapter.captureEnabled) return false
        if (inspectorLoader.item && !inspectorLoader.item.commitPendingPropertyInputs()) return false
        host.cancelInput()
        if (!adapter.chooseTool("select")) return false
        host.keepPropertiesAndClose()
        return true
    }
    Connections {
        target: host.deviceScene.toolbar
        function onRepaperToolActiveChanged() { host.syncNativeTool() }
    }
    Component.onCompleted: { Paper.Theme.unit = 2; Qt.callLater(host.attachWhenReady) }
    Component.onDestruction: {
        cancelPreviewPresentation(true)
        host.cancelInput()
        if (deviceScene.toolbar && deviceScene.toolbar.repaperEditorHost === host)
            deviceScene.toolbar.repaperEditorHost = null
    }
    onControllerChanged: Qt.callLater(host.attachWhenReady)
    Connections {
        target: host.deviceScene
        function onPageIdChanged() {
            host.cancelPreviewPresentation(false)
            host.cancelInput()
            Qt.callLater(host.attachWhenReady)
        }
        function onTileManagerChanged() { host.cancelPreviewPresentation(true); host.cancelInput(); Qt.callLater(host.attachWhenReady) }
        function onViewportChanged() { host.cancelPreviewPresentation(true); Qt.callLater(host.attachWhenReady) }
        function onVisibleChanged() { if (!host.deviceScene.visible) { host.keepPropertiesAndClose(); host.cancelPreviewPresentation(true); host.cancelInput() } }
    }
    function refreshViewProjection() {
        if (host.visible && adapter.captureEnabled && (host.hasCustomSelection || host.nativeOperationPending || preview.hasStrokes))
            adapter.nativeViewTransformChanged()
    }
    Connections {
        // Native pinch-to-zoom emits once per sample. Cancel a live gesture
        // immediately, then coalesce projection of retained handles/ink. Idle
        // tools without a selection or pending preview have no listener.
        target: host.visible && adapter.captureEnabled && (input.stylusPressed || host.overlayState.nativeGestureActive
            || host.hasCustomSelection || host.nativeOperationPending || preview.hasStrokes) ? host.deviceScene.tileManager : null
        function onTransformChanged() {
            if (input.stylusPressed || host.overlayState.nativeGestureActive) host.cancelInput()
            if (host.hasCustomSelection || host.nativeOperationPending || preview.hasStrokes) Qt.callLater(host.refreshViewProjection)
        }
    }
    Connections {
        target: host.previewPresentationToken ? host.deviceScene.tileManager : null
        function onPendingTilesChanged() { host.requestCommittedPreviewFrame() }
    }
    Connections {
        target: host.previewPresentationToken ? host.deviceScene.viewport : null
        function onBlockingUpdatesChanged() { host.requestCommittedPreviewFrame() }
    }
    Connections {
        target: host.previewPresentationToken ? host.previewWindow : null
        function onFrameSwapped() {
            if (!host.previewFrameRequested || !host.previewPresentationToken) return
            const token = host.previewPresentationToken, generation = host.previewPresentationGeneration
            host.previewFrameRequested = false
            host.previewFrameSubmitted = true
            // Qt has submitted a frame. This is not an e-paper display receipt.
            // Leave the render callback before changing the preview's scene graph.
            Qt.callLater(function() {
                if (token !== host.previewPresentationToken || generation !== host.previewPresentationGeneration) return
                if (!host.previewPresentationAvailable()) { host.cancelPreviewPresentation(true); return }
                if (host.previewPresentationTiles.pendingTiles || host.previewPresentationViewport.blockingUpdates) {
                    host.previewFrameSubmitted = false
                    host.requestCommittedPreviewFrame()
                    return
                }
                host.cancelPreviewPresentation(false)
                adapter.nativePreviewPresented(token)
            })
        }
    }
    Connections {
        target: host.visible && (adapter.captureEnabled || host.nativeOperationPending
            || host.editorState.propertiesSessionActive || host.editorState.propertiesSessionCancelPending) ? host.controller : null
        function onSelectionCleared() { host.cancelInput(); adapter.refreshNativeState(true) }
        function onAreaSelected(layer, rect) {
            const originController = host.controller, originPage = host.deviceScene.pageId, generation = host.attachmentGeneration
            Qt.callLater(function() {
                if (adapter.captureEnabled && originController === host.controller && originPage === host.deviceScene.pageId && generation === host.attachmentGeneration)
                    adapter.nativeAreaSelected(layer, rect)
            })
        }
        function onCurrentLayerChanged() { host.keepPropertiesAndClose(); host.cancelInput(); adapter.refreshNativeState() }
    }
    function coordinateMappingAvailable() { return !!deviceScene.tileManager }
    function cancelPreviewPresentation(acknowledge) {
        const token = previewPresentationToken
        previewPresentationToken = 0
        previewPresentationGeneration += 1
        previewFrameRequested = false
        previewFrameSubmitted = false
        previewPresentationViewport = null
        previewPresentationTiles = null
        if (acknowledge && token) adapter.nativePreviewPresented(token)
    }
    function previewPresentationAvailable() {
        return visible && deviceScene.visible && !!previewWindow
            && !!previewPresentationViewport && previewPresentationViewport === deviceScene.viewport
            && !!previewPresentationTiles && previewPresentationTiles === deviceScene.tileManager
    }
    function requestCommittedPreviewFrame() {
        if (!previewPresentationToken) return
        if (!previewPresentationAvailable()) { cancelPreviewPresentation(true); return }
        if (previewPresentationTiles.pendingTiles || previewPresentationViewport.blockingUpdates) {
            // If rendering becomes busy again, a previously queued frame cannot
            // retire this preview even when those tiles finish before callLater.
            if (previewFrameRequested || previewFrameSubmitted) previewPresentationGeneration += 1
            previewFrameRequested = false
            previewFrameSubmitted = false
            return
        }
        if (previewFrameRequested || previewFrameSubmitted) return
        previewFrameRequested = true
        // The native insertion has already been observed before this API is called.
        // Its tiles must be ready before repainting and retiring the retained ink.
        previewPresentationViewport.requestRepaintDirty()
        previewWindow.update()
    }
    function presentCommittedPreview(token) {
        cancelPreviewPresentation(false)
        if (!token || !visible || !deviceScene.visible || !previewWindow || !deviceScene.viewport || !deviceScene.tileManager) return false
        previewPresentationToken = token
        previewPresentationViewport = deviceScene.viewport
        previewPresentationTiles = deviceScene.tileManager
        requestCommittedPreviewFrame()
        return true
    }
    function viewToPaper(point) { return deviceScene.tileManager ? deviceScene.tileManager.viewToScene(point) : Qt.point(NaN, NaN) }
    function paperToView(point) { return deviceScene.tileManager ? deviceScene.tileManager.sceneToView(point) : Qt.point(NaN, NaN) }
    function viewScale() { return deviceScene.tileManager ? deviceScene.tileManager.scale : 1 }
    // Exact 3.28 SceneTileManager property 19 is a QTransform. Return the native
    // value intact, avoiding three QML calls to rebuild an affine basis.
    function readViewTransform() { return deviceScene.tileManager ? deviceScene.tileManager.sceneToViewTransform : null }
    function readState() {
        return { canUndo: controller ? controller.undoAvailable : false,
                 canRedo: controller ? controller.redoAvailable : false,
                 working: controller ? controller.working : true,
                 queueSize: controller && controller.worker ? controller.worker.jobQueueSize : -1,
                 layer: controller ? controller.currentLayer : -1,
                 documentId: deviceScene.document ? deviceScene.document.id.toString().replace(/[{}]/g, "") : "",
                 pageId: deviceScene.pageId }
    }
    function cloneSelection() {
        if (!controller || controller.working || controller.selectionItemCount < 1 || controller.selectionItemCount > 128) return null
        return controller.cloneSelectedItems(controller.currentLayer, 1.0)
    }
    // Called by the exact-resource hook BEFORE Xochitl opens or clears its
    // selection UI. Our own tool keeps both the selection and the input surface.
    function handleNativeSelection(layer, rect) {
        return adapter.captureEnabled
    }
    function clearCustomSelection() {
        if (!controller || controller.working) return false
        controller.cancelPendingEdit()
        controller.clearSelectedItems()
        return true
    }
    function selectCustomRegion(rect) {
        if (!controller || controller.working || !adapter.available || !adapter.captureEnabled) return false
        if (![rect.x, rect.y, rect.width, rect.height, rect.x + rect.width, rect.y + rect.height].every(v => isFinite(v) && Math.abs(v) <= 1000000) || rect.width <= 0 || rect.height <= 0) return false
        const left = Math.floor(rect.x), top = Math.floor(rect.y)
        controller.addSelectionRect(Qt.rect(left, top, Math.ceil(rect.x + rect.width) - left, Math.ceil(rect.y + rect.height) - top), 0)
        return true
    }
    function transformCustomSelection(change) {
        // NativeScene has just verified the exact selected IDs on the worker.
        // The GUI selection count can still describe the preceding selection.
        if (!controller || controller.working || !adapter.available || !adapter.captureEnabled) return false
        const layer = controller.currentLayer
        const pointValid = p => p && isFinite(p.x) && isFinite(p.y) && Math.abs(p.x) <= 1000000 && Math.abs(p.y) <= 1000000
        if (change.kind === "scale") {
            if (!pointValid(change.anchor) || !isFinite(change.sx) || !isFinite(change.sy) || change.sx <= 0 || change.sy <= 0 || change.sx > 10000 || change.sy > 10000) return false
            controller.scaleSelectedItems(layer, change.anchor, change.sx, change.sy)
        } else if (change.kind === "move") {
            if (!pointValid(change.delta)) return false
            controller.moveSelectedItems(layer, change.delta)
        } else if (change.kind === "rotate") {
            if (!pointValid(change.anchor) || !isFinite(change.angle) || Math.abs(change.angle) > 360) return false
            controller.rotateSelectedItems(layer, change.anchor, change.angle)
        } else if (change.kind === "remove") {
            controller.deleteSelectedItems(layer)
            return true
        } else if (change.kind === "duplicate") {
            const items = controller.cloneSelectedItems(layer, 1.0)
            const rect = controller.getItemBoundingRect(items)
            controller.cloneAddAndSelectItems(layer, items, 1.0, Qt.point(rect.x + rect.width / 2 + 24, rect.y + rect.height / 2 + 24))
            return true
        } else return false
        // Exactly one pending affine edit is committed at the gesture's end.
        controller.applyPendingEdit(layer)
        return true
    }
    function prepareInsertion() {
        if (!controller || controller.working || !adapter.available) return false
        deviceScene.endItemSelection()
        return true
    }
    function activateCustomTool() {
        if (!controller || controller.working) return false
        // As for a native tool change, settle the former native selection once.
        // Inserting a custom object must never switch selectedPen to the lasso.
        deviceScene.endItemSelection()
        return true
    }
    function activateNativeSelection() {
        if (!controller || controller.working || !adapter.available || !deviceScene.toolbar) return false
        const toolbar = deviceScene.toolbar
        // forceMoveTool only sets DocumentViewTools' future-open preference.
        // Select the actual SelectionButton so selectedPen, activePen and our
        // blocking surface all change together through Xochitl's own protocol.
        toolbar.selectSelection()
        const tool = toolbar.selectedPen
        if (!tool || tool.penToolType !== "selection" || tool.pen !== toolbar.selectionPen) return false
        tool.selectionToolModeSelected(ToolbarModel.SelectionToolMode.Select)
        // Do not endItemSelection here: an existing native selection and its
        // pending transform must survive opening its properties/handles.
        return tool.selectedMode === ToolbarModel.SelectionToolMode.Select && !toolbar.repaperToolActive
    }
    function insertBatch(items, origin) {
        if (!controller || controller.working || !adapter.available) return false
        // One QList becomes one native history command. This return value only
        // means dispatched; completion and original-item observation are separate.
        controller.cloneAddAndSelectItems(controller.currentLayer, items, 1.0, origin)
        deviceScene.viewport.requestRepaintDirty()
        return true // dispatched; physical result/Undo still requires native QA
    }
    function selectionAction(action, value) {
        if (!controller || controller.working || !adapter.available) return false
        if (action === "undo") { controller.undo(); return true }
        if (action === "redo") { controller.redo(); return true }
        const selection = deviceScene.selectionHandler
        if (!selection) return false
        const layer = selection.selectionLayer
        const rect = selection.sceneSelectionRect
        const origin = Qt.point(rect.x + rect.width / 2, rect.y + rect.height / 2)
        if (action === "rotate") controller.rotateSelectedItems(layer, origin, 90)
        else if (action === "scale" && isFinite(value) && value >= 0.1 && value <= 10)
            controller.scaleSelectedItems(layer, origin, value, value)
        else if (action === "remove") controller.deleteSelectedItems(layer)
        else if (action === "duplicate") {
            const items = controller.cloneSelectedItems(layer, 1.0)
            controller.cloneAddAndSelectItems(layer, items, 1.0, Qt.point(origin.x + 24, origin.y + 24))
        } else return false
        if (action === "rotate" || action === "scale") controller.applyPendingEdit(layer)
        return true
    }

    // This paints only an uncommitted preview; Xochitl owns all committed content.
    Epaper.ScreenModeItem {
        anchors.fill: parent
        mode: Epaper.ScreenModeItem.Animation
        visible: host.nativePreviewActive
    }
    NativePreviewItem {
        id: preview
        objectName: "nativeInkPreview"
        viewportRect: Qt.rect(0, 0, host.width, host.height)
        x: contentBounds.x; y: contentBounds.y
        width: contentBounds.width; height: contentBounds.height
        // Native routing subtracts visible ItemHasContents bounds. Empty
        // previews contribute no surface; live ink contributes only its bounds.
        visible: host.visible && adapter.captureEnabled && inputScheduler.penCaptureAllowed && hasStrokes
        previewFrame: adapter.previewFrame
    }
    Rectangle {
        objectName: "wireSnapIndicator"
        readonly property var snapPoint: host.overlayState.wireSnapPoint || Qt.point(0, 0)
        visible: adapter.captureEnabled && inputScheduler.penCaptureAllowed && !!host.overlayState.wireSnapActive && isFinite(snapPoint.x) && isFinite(snapPoint.y)
        x: snapPoint.x - 7; y: snapPoint.y - 7
        width: 14; height: 14; radius: 7
        color: "white"; border.color: "black"; border.width: 2
        Rectangle { anchors.centerIn: parent; width: 4; height: 4; radius: 2; color: "black" }
    }
    Repeater {
        model: host.hasCustomSelection && inputScheduler.penCaptureAllowed ? host.overlayState.selectionHandlePoints : []
        Rectangle {
            x: modelData.x - 9; y: modelData.y - 9
            width: 18; height: 18; radius: modelData.kind === "endpoint" ? 9 : 3
            rotation: modelData.kind === "bend" ? 45 : 0
            color: "white"; border.color: "black"; border.width: 2
        }
    }
    Rectangle {
        objectName: "selectionRotationHandle"
        visible: host.hasCustomSelection && inputScheduler.penCaptureAllowed && host.overlayState.selectionRotationHandle !== undefined && host.overlayState.selectionRotationHandle.x !== undefined
        x: ((host.overlayState.selectionRotationHandle || {}).x || 0) - 14
        y: ((host.overlayState.selectionRotationHandle || {}).y || 0) - 14
        width: 28; height: 28; radius: 14
        color: "white"; border.color: "black"; border.width: 2
        Text { anchors.centerIn: parent; text: "↻"; color: "black"; font.pixelSize: 24 }
    }
    MouseArea {
        id: input
        property bool stylusPressed: false
        x: host.deviceScene.availableSceneRect.x
        y: host.deviceScene.availableSceneRect.y
        width: host.deviceScene.availableSceneRect.width
        height: host.deviceScene.availableSceneRect.height
        // Finger events stay on the original native gesture target. Keep the
        // pen surface alive through release even if proximity changes first.
        enabled: adapter.captureEnabled && inputScheduler.penCaptureAllowed && (host.deviceScene.penClose || stylusPressed)
        visible: enabled
        acceptedButtons: Qt.LeftButton
        // A native navigation gesture may steal the pointer; cancellation then
        // discards the preview rather than committing a partial shape.
        preventStealing: false
        onPressed: mouse => {
            if (!host.deviceScene.penClose || !inputScheduler.penCaptureAllowed) { mouse.accepted = false; return }
            stylusPressed = true
            const point = host.mapFromItem(input, mouse.x, mouse.y)
            // Busy custom tools still own this press: never let the underlying
            // native pen draw while the custom toolbar tool is selected.
            mouse.accepted = true
            if (adapter.pointerBegin(point.x, point.y, "pen")) inputScheduler.begin()
            else inputScheduler.cancel()
        }
        onPositionChanged: mouse => {
            const point = host.mapFromItem(input, mouse.x, mouse.y)
            // Match TextDragHotspot's PenInputBlocker contract. Its forwarded
            // moves need not agree with MouseArea.pressed; C++ owns gesture state.
            inputScheduler.queueMove(point.x, point.y)
        }
        onReleased: mouse => {
            if (!stylusPressed || !inputScheduler.penCaptureAllowed) { host.cancelInput(); return }
            const point = host.mapFromItem(input, mouse.x, mouse.y)
            inputScheduler.finish()
            adapter.pointerEnd(point.x, point.y)
            stylusPressed = false
        }
        onCanceled: host.cancelInput()
        PenInputBlocker {
            anchors.fill: parent
            manager: host.deviceScene.penInput.surfaceManager
        }
    }
    Rectangle {
        objectName: "aspectRatioLockIndicator"
        readonly property var lockAnchor: host.overlayState.aspectRatioLockAnchor || Qt.point(0, 0)
        visible: adapter.captureEnabled && inputScheduler.penCaptureAllowed && !!host.overlayState.aspectRatioLocked
        z: 2
        width: lockText.implicitWidth + 24; height: 44
        x: Math.max(host.deviceScene.availableSceneRect.x + 8, Math.min(lockAnchor.x + 22, host.deviceScene.availableSceneRect.x + host.deviceScene.availableSceneRect.width - width - 8))
        y: Math.max(host.deviceScene.availableSceneRect.y + 8, Math.min(lockAnchor.y - height - 18, host.deviceScene.availableSceneRect.y + host.deviceScene.availableSceneRect.height - height - 8))
        color: "white"; border.color: "black"; border.width: 2
        Text { id: lockText; anchors.centerIn: parent; text: "Proportions verrouillées"; color: "black"; font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.caption }
    }
    Paper.Button {
        id: propertiesButton
        objectName: "nativePropertiesButton"
        z: 3
        visible: host.hasCustomSelection && !host.inspectorOpen
        x: host.deviceScene.availableSceneRect.x + 24
        y: host.deviceScene.availableSceneRect.y + 24
        text: "Propriétés"; onClicked: host.inspectorOpen = true
        PenInputBlocker { anchors.fill: parent; manager: host.deviceScene.penInput.surfaceManager }
    }
    Loader {
        id: quickStencilBar
        objectName: "nativeStencilQuickBar"
        z: 3
        visible: adapter.captureEnabled && (host.editorState.tool === "symbol" || host.editorState.tool === "wire") && !host.inspectorOpen
        property bool requested: false
        active: requested
        width: Math.min(960, host.deviceScene.availableSceneRect.width - 48)
        height: item ? item.implicitHeight : 104
        x: host.deviceScene.availableSceneRect.x + host.deviceScene.availableSceneRect.width - width - 24
        y: host.deviceScene.availableSceneRect.y + host.deviceScene.availableSceneRect.height - height - 24
        enabled: !host.nativeOperationPending
        function ensureLoaded() {
            if (visible && !requested) {
                requested = true
                setSource("qrc:/repaper/editor/NativeStencilQuickBar.qml", { editor: adapter })
            }
        }
        onVisibleChanged: ensureLoaded()
        Component.onCompleted: ensureLoaded()
        Connections {
            target: quickStencilBar.item
            function onPenRequested() {
                host.cancelInput()
                if (host.deviceScene.toolbar && typeof host.deviceScene.toolbar.repaperResumePen === "function")
                    host.deviceScene.toolbar.repaperResumePen()
            }
        }
        PenInputBlocker { anchors.fill: parent; manager: host.deviceScene.penInput.surfaceManager }
    }
    Loader {
        id: quickColors
        objectName: "nativeQuickColorsHost"
        z: 3
        visible: host.pageReadyForAttachment && !!host.deviceScene.toolbar && !!host.deviceScene.toolbar.repaperWritingPen && !host.inspectorOpen
        property bool requested: false
        active: requested
        width: item ? item.implicitWidth : 308
        height: item ? item.implicitHeight : 108
        x: host.deviceScene.availableSceneRect.x + 24
        y: host.deviceScene.availableSceneRect.y + host.deviceScene.availableSceneRect.height - height - 24
            - (quickStencilBar.visible && width + quickStencilBar.width + 72 > host.deviceScene.availableSceneRect.width ? quickStencilBar.height + 12 : 0)
        function loadPalette() {
            if (visible && !requested) {
                requested = true
                setSource("qrc:/repaper/editor/NativeQuickColors.qml", {toolbar: host.deviceScene.toolbar, editorHost: host})
            }
        }
        onVisibleChanged: loadPalette()
        onLoaded: item.toolbar = Qt.binding(function() { return host.deviceScene.toolbar })
        Component.onCompleted: loadPalette()
        PenInputBlocker { anchors.fill: parent; manager: host.deviceScene.penInput.surfaceManager }
    }
    Rectangle {
        id: inspector
        z: 4
        visible: (host.hasCustomSelection || !!host.editorState.propertiesSessionActive || !!host.editorState.propertiesSessionCancelPending) && host.inspectorOpen
        width: Math.min(720, host.deviceScene.availableSceneRect.width - 48)
        height: Math.min(1000, host.deviceScene.availableSceneRect.height - 48)
        x: host.deviceScene.availableSceneRect.x + host.deviceScene.availableSceneRect.width - width - 24
        y: host.deviceScene.availableSceneRect.y + 24
        color: "white"; border.color: "black"; border.width: 2
        Text {
            anchors.left: parent.left; anchors.leftMargin: 24; anchors.top: parent.top; anchors.topMargin: 24
            text: "Propriétés"; color: "black"; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title
        }
        Loader {
            id: inspectorLoader
            anchors.fill: parent; anchors.topMargin: 104; anchors.margins: 2
            property bool requested: false
            active: requested
            function ensureLoaded() {
                if (inspector.visible && !requested) {
                    requested = true
                    setSource("qrc:/repaper/editor/ReInkSidebar.qml", {editor: adapter, mode: "properties", inspectorOnly: true})
                }
            }
            onVisibleChanged: ensureLoaded()
            Component.onCompleted: ensureLoaded()
        }
        Connections {
            target: inspectorLoader.item
            function onPropertiesAcceptRequested() { host.keepPropertiesAndClose() }
            function onPropertiesCancelRequested() { host.cancelProperties() }
        }
        Paper.IconButton {
            objectName: "closeNativeInspector"; anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 4
            symbol: "close"; text: "Fermer"; enabled: !host.nativeOperationPending; focusPolicy: Qt.NoFocus
            onClicked: {
                if (inspectorLoader.item && !inspectorLoader.item.commitPendingPropertyInputs()) return
                host.keepPropertiesAndClose()
            }
        }
        PenInputBlocker { anchors.fill: parent; manager: host.deviceScene.penInput.surfaceManager }
    }
}

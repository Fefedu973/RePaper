import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQml.Models 2.15
import "qrc:/paper" as Paper
import xofm.libs.toolbar

Rectangle {
    id: root
    objectName: "nativeQuickColors"
    required property var toolbar
    property var editorHost: null
    readonly property var editor: editorHost ? editorHost.editor : null
    property real buttonSize: 96
    readonly property var nativePen: toolbar ? toolbar.repaperWritingPen : null
    readonly property bool customActive: !!toolbar && !!toolbar.repaperToolActive
    readonly property var editorState: visible && editor ? editor.state : ({})
    readonly property var overlayState: visible && editor && editor.overlayState !== undefined ? editor.overlayState : editorState
    readonly property bool busy: !!editorState.working || !!overlayState.nativeGestureActive
        || !!editorState.nativeSelectionWaiting || !!editorState.nativeCreationInFlight
        || (!!toolbar && !!toolbar.repaperEditorHost && !!toolbar.repaperEditorHost.nativeOperationPending)
    readonly property bool editorReady: visible && !!editor && !!editor.available && !!editor.captureEnabled
        && !busy && editorState.tool !== "select" && !editorState.hasSelection
    readonly property bool canChoose: visible && !!nativePen && !busy && (!customActive || editorReady)
    readonly property string nativeColor: {
        penRevision
        return nativePen ? normalizedColor(nativePalette.displayColor(nativePen.color, nativePen.colorCode)) : ""
    }
    readonly property string currentColor: customActive ? normalizedColor(editorState.lineColor) : nativeColor
    property int penRevision: 0
    property int paletteRevision: 0
    property bool synchronizing: false
    property string lastEditorColor: ""
    readonly property real padding: 6
    readonly property real gap: 4
    implicitWidth: 3 * buttonSize + 2 * gap + 2 * padding
    implicitHeight: buttonSize + 2 * padding
    color: "white"
    border.color: "black"
    border.width: 1
    radius: 8

    function normalizedColor(value) {
        if (value === undefined || value === null) return ""
        const text = String(value).toLowerCase()
        if (/^#[0-9a-f]{6}$/.test(text)) return text
        if (/^#ff[0-9a-f]{6}$/.test(text)) return "#" + text.substring(3)
        if (text === "black") return "#000000"
        if (text === "white") return "#ffffff"
        return ""
    }
    function choiceForKey(key) {
        for (let index = 0; index < nativeChoices.count; ++index) {
            const choice = nativeChoices.objectAt(index)
            if (choice && choice.displayName.toLowerCase() === key) return choice
        }
        return null
    }
    function choiceForColor(color) {
        for (let index = 0; index < nativeChoices.count; ++index) {
            const choice = nativeChoices.objectAt(index)
            if (choice && normalizedColor(choice.displayColor) === color) return choice
        }
        return null
    }
    function refreshPalette() {
        paletteRevision += 1
        syncNativeToEditor()
    }
    function syncNativeToEditor() {
        if (synchronizing || !customActive || !editorReady || !nativeColor) return
        // setStrokeColor also edits a native selection. Only creation tools
        // may inherit the current handwriting pen automatically.
        synchronizing = true
        const color = nativeColor
        if (normalizedColor(editorState.lineColor) === color || editor.setStrokeColor(color)) lastEditorColor = color
        synchronizing = false
    }
    function syncEditorColor() {
        if (synchronizing || !customActive || !editorReady) return
        if (!lastEditorColor) { syncNativeToEditor(); return }
        const color = normalizedColor(editorState.lineColor)
        if (!color || color === lastEditorColor) return
        lastEditorColor = color
        // Follow any supported native palette color chosen in the full
        // editor. Custom RGB values remain available in the editor itself.
        const choice = choiceForColor(color)
        if (!choice || !nativePalette.isValidColor(choice.toolColor, choice.rgb)) return
        synchronizing = true
        toolbar.repaperSetWritingColor(choice.rgb, choice.toolColor)
        synchronizing = false
    }
    function chooseColor(key) {
        if (!canChoose) return false
        const choice = choiceForKey(key)
        if (!choice || !nativePalette.isValidColor(choice.toolColor, choice.rgb)) return false
        synchronizing = true
        const color = normalizedColor(choice.displayColor)
        const accepted = toolbar.repaperSetWritingColor(choice.rgb, choice.toolColor)
        let changed = accepted
        if (accepted && customActive) {
            changed = normalizedColor(editorState.lineColor) === color || editor.setStrokeColor(color)
            if (changed) lastEditorColor = color
        }
        synchronizing = false
        return changed
    }
    onNativePenChanged: Qt.callLater(root.syncNativeToEditor)
    onNativeColorChanged: Qt.callLater(root.syncNativeToEditor)
    onCustomActiveChanged: {
        lastEditorColor = ""
        if (customActive) Qt.callLater(root.syncNativeToEditor)
    }
    onEditorReadyChanged: if (editorReady && !lastEditorColor) Qt.callLater(root.syncNativeToEditor)
    onVisibleChanged: if (visible) {
        // Native pens publish a generic propertyChanged signal. A retained,
        // hidden palette was disconnected while the pen color could change.
        penRevision += 1
        Qt.callLater(root.syncNativeToEditor)
    }
    Component.onCompleted: Qt.callLater(root.syncNativeToEditor)

    PenColorModel {
        id: nativePalette
        tool: root.nativePen ? root.nativePen.tool : 0
        colorProfile: root.toolbar ? root.toolbar.toolbarProvider.colorProfile : 0
    }
    Instantiator {
        id: nativeChoices
        model: nativePalette
        delegate: QtObject {
            required property string displayName
            required property var displayColor
            required property int toolColor
            required property var rgb
            onDisplayNameChanged: Qt.callLater(root.refreshPalette)
            onDisplayColorChanged: Qt.callLater(root.refreshPalette)
            onToolColorChanged: Qt.callLater(root.refreshPalette)
            onRgbChanged: Qt.callLater(root.refreshPalette)
        }
        onObjectAdded: Qt.callLater(root.refreshPalette)
        onObjectRemoved: Qt.callLater(root.refreshPalette)
    }
    Connections {
        target: root.visible ? root.nativePen : null
        function onPropertyChanged() {
            root.penRevision += 1
            if (!root.synchronizing) Qt.callLater(root.syncNativeToEditor)
        }
    }
    Connections {
        target: root.visible && root.customActive ? root.editor : null
        function onChanged() { if (!root.synchronizing) Qt.callLater(root.syncEditorColor) }
    }
    Row {
        x: root.padding
        y: root.padding
        spacing: root.gap
        Repeater {
            model: [{key: "black", label: "Noir"}, {key: "red", label: "Rouge"}, {key: "green", label: "Vert"}]
            delegate: AbstractButton {
                id: colorButton
                required property var modelData
                objectName: "quickColor_" + modelData.key
                readonly property var choice: { root.paletteRevision; return root.choiceForKey(modelData.key) }
                readonly property string ink: choice ? root.normalizedColor(choice.displayColor) : ""
                readonly property bool selected: !!ink && root.currentColor === ink
                width: root.buttonSize
                height: root.buttonSize
                text: modelData.label
                focusPolicy: Qt.NoFocus
                enabled: root.canChoose && !!choice
                onClicked: root.chooseColor(modelData.key)
                background: Rectangle {
                    color: colorButton.down ? "#e0e0e0" : "white"
                    border.color: colorButton.selected ? "black" : "transparent"
                    border.width: 3
                    radius: 5
                }
                contentItem: Item {
                    opacity: colorButton.enabled ? 1 : 0.35
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 14
                        width: 30
                        height: 30
                        radius: 15
                        color: colorButton.ink || "white"
                        border.color: "black"
                        border.width: 1
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 54
                        text: colorButton.text
                        font.family: Paper.Theme.sans
                        font.pixelSize: 20
                        font.bold: colorButton.selected
                        color: "black"
                    }
                }
            }
        }
    }
}

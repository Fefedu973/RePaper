import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "qrc:/paper" as Paper
import RePaper.Drawing 1.0
import "qrc:/repaper/editor"

ApplicationWindow {
    id: window
    width: 810; height: 1080; visible: true
    title: "reInk"; color: "white"; font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.body
    palette.window: "white"; palette.base: "white"; palette.button: "white"
    palette.highlight: "#202020"; palette.highlightedText: "white"
    function showPalette(mode) {
        canvas.cancel()
        if (mode === "properties") canvas.tool = "select"
        tools.mode = mode
        tools.open()
    }
    Shortcut { sequence: "Escape"; onActivated: { canvas.cancel(); tools.close() } }
    onActiveChanged: if (!active) canvas.cancel()
    QtObject {
        id: pageAdapter
        readonly property bool available: true
        readonly property string status: canvas.status
        readonly property var stencils: canvas.symbols
        readonly property var state: ({
            tool:canvas.tool, lineStyle:canvas.lineStyle, lineWidth:canvas.lineWidth,
            hasSelection:canvas.hasSelection, canUndo:canvas.canUndo, canRedo:canvas.canRedo,
            selectionHasEndpoints:canvas.selectionHasEndpoints, selectionCanChangeStyle:canvas.selectionCanChangeStyle,
            selectionIsArrow:canvas.selectionIsArrow, selectionIsWire:canvas.selectionIsWire,
            selectedLineWidth:canvas.selectedLineWidth, selectedLineStyle:canvas.selectedLineStyle,
            selectedArrowDirection:canvas.selectedArrowDirection, selectedHorizontalFirst:canvas.selectedHorizontalFirst,
            horizontalFirst:canvas.horizontalFirst, selectionKind:canvas.selectionKind,
            selectionCanResize:canvas.selectionCanResize, selectedShapeWidth:canvas.selectedShapeWidth,
            selectedShapeHeight:canvas.selectedShapeHeight, selectedCornerRadius:canvas.selectedCornerRadius,
            selectedWireBend:canvas.selectedWireBend
        })
        function chooseTool(tool) { canvas.tool = tool; return canvas.tool === tool }
        function beginStencil(id) { canvas.symbolId = id; canvas.tool = "symbol"; return canvas.symbolId === id }
        function setStrokeStyle(style) { if(canvas.tool === "select") canvas.setSelectionLineStyle(style); else canvas.lineStyle = style }
        function setStrokeWidth(width) { if(canvas.tool === "select") canvas.setSelectionLineWidth(width); else canvas.lineWidth = width }
        function setArrowDirection(direction) { canvas.setSelectionArrowDirection(direction) }
        function setWireOrientation(horizontal) { if(canvas.tool === "select") canvas.setSelectionHorizontalFirst(horizontal); else canvas.horizontalFirst = horizontal }
        function resize(width,height) { canvas.resizeSelection(width,height) }
        function setCornerRadius(radius) { canvas.setSelectionCornerRadius(radius) }
        function setWireBend(offset) { canvas.setSelectionWireBend(offset) }
        function undo() { canvas.undo() }
        function redo() { canvas.redo() }
        function scale(factor) { canvas.scaleSelection(factor) }
        function rotate() { canvas.rotateSelection() }
        function duplicate() { canvas.duplicateSelection() }
        function remove() { canvas.removeSelection() }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Paper.Theme.margin; spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Label { text: "reInk"; font.pixelSize: Paper.Theme.title; font.family: Paper.Theme.serif }
            Item { Layout.fillWidth: true }
            Paper.Button { text: "Annuler"; enabled: canvas.canUndo; implicitHeight: Paper.Theme.control; onClicked: canvas.undo() }
            Paper.Button { text: "Rétablir"; enabled: canvas.canRedo; implicitHeight: Paper.Theme.control; onClicked: canvas.redo() }
            Paper.IconButton { text: "Fermer"; symbol: "close"; onClicked: Qt.quit() }
        }
        RowLayout {
            Layout.fillWidth: true
            Paper.Button { objectName: "openInkPalette"; text: "Dessin"; implicitHeight: Paper.Theme.control; onClicked: window.showPalette("reink") }
            Paper.Button { objectName: "openStencilPalette"; text: "reStencil"; implicitHeight: Paper.Theme.control; onClicked: window.showPalette("restencil") }
            Paper.Button { objectName: "openSelectionPalette"; text: "Propriétés"; implicitHeight: Paper.Theme.control; onClicked: window.showPalette("properties") }
            Item { Layout.fillWidth: true }
            Paper.CheckBox { text: "Grille"; checked: canvas.gridVisible; onToggled: canvas.gridVisible = checked }
            Paper.CheckBox { text: "Aimantation"; checked: canvas.snapping; onToggled: canvas.snapping = checked }
        }
        InkCanvas {
            id: canvas; objectName: "drawingCanvas"
            Layout.fillWidth: true; Layout.fillHeight: true
            MouseArea {
                anchors.fill: parent; preventStealing: true; acceptedButtons: Qt.LeftButton
                enabled: touch.downCount === 0
                onPressed: function(mouse) { canvas.begin(mouse.x,mouse.y) }
                onPositionChanged: function(mouse) { if(pressed) canvas.move(mouse.x,mouse.y) }
                onReleased: function(mouse) { canvas.end(mouse.x,mouse.y) }
                onCanceled: canvas.cancel()
            }
            MultiPointTouchArea {
                id: touch
                anchors.fill: parent; mouseEnabled: false
                minimumTouchPoints: 1; maximumTouchPoints: 2
                property int downCount: 0
                property int activeId: -1
                property bool drawing: false
                onPressed: function(points) {
                    downCount += points.length
                    if(downCount === 1) { activeId = points[0].pointId; drawing = true; canvas.begin(points[0].x,points[0].y) }
                    else { drawing = false; canvas.cancel() }
                }
                onUpdated: function(points) {
                    if(drawing) for(let i=0;i<points.length;++i) if(points[i].pointId === activeId) canvas.move(points[i].x,points[i].y)
                }
                onReleased: function(points) {
                    if(drawing) for(let i=0;i<points.length;++i) if(points[i].pointId === activeId) { canvas.end(points[i].x,points[i].y); drawing = false }
                    downCount = Math.max(0,downCount-points.length)
                }
                onCanceled: { canvas.cancel(); drawing = false; downCount = 0 }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: ""; Layout.fillWidth: true; font.pixelSize: Paper.Theme.caption }
            Paper.Button { text: "Exporter"; implicitHeight: Paper.Theme.control; onClicked: exports.open() }
            Paper.Button { text: "Importer dans la bibliothèque"; implicitHeight: Paper.Theme.control; enabled: canvas.itemCount > 0; onClicked: canvas.importDrawing() }
            Paper.Button { text: "Effacer"; implicitHeight: Paper.Theme.control; enabled: canvas.itemCount > 0; onClicked: clearDialog.open() }
        }
        Label {
            Layout.fillWidth: true
            text: canvas.status; visible: Paper.Theme.actionable(text)
            wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.caption; color: "#555"
        }
    }
    NativeEditorPalette {
        id: tools; parent: Overlay.overlay; editor: pageAdapter
        x: Math.max(12,parent.width-width-20); y: Math.max(12,Math.min(125,parent.height-height-12))
    }
    Popup {
        id: exports; parent: Overlay.overlay; x: Math.max(12,parent.width-width-20); y: parent.height-height-90
        width: 280; padding: 16; modal: true
        background: Rectangle { color: "white"; border.color: "black"; border.width: 1 }
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        ColumnLayout {
            anchors.fill: parent
            Label { text: "Exporter le dessin"; font.bold: true }
            Paper.Button { text: "SVG"; Layout.fillWidth: true; onClicked: { canvas.exportDrawing("svg"); exports.close() } }
            Paper.Button { text: "PDF"; Layout.fillWidth: true; onClicked: { canvas.exportDrawing("pdf"); exports.close() } }
            Paper.Button { text: "Traits natifs"; Layout.fillWidth: true; onClicked: { canvas.exportDrawing("scene"); exports.close() } }
        }
        height: 228
    }
    Paper.Dialog {
        id: clearDialog; anchors.centerIn: parent; title: "Effacer ce dessin ?"
        standardButtons: Dialog.Ok | Dialog.Cancel; modal: true
        Label { text: "Cette action reste annulable."; wrapMode: Text.WordWrap }
        onAccepted: canvas.clear()
    }
}

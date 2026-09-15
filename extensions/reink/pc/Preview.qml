import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import RePaper.Editor 1.0
import RePaper.Drawing 1.0

ApplicationWindow {
    id: window
    visible: true; width: 1100; height: 1100
    title: "reInk · reStencil — banc PC"
    color: "#eeeeee"; font.pixelSize: 20
    palette.window: "white"; palette.button: "white"; palette.highlight: "#202020"; palette.highlightedText: "white"
    EditorAdapter { id: pageAdapter }
    function showPalette(mode) {
        page.cancel()
        palette.mode = mode
        palette.open()
    }
    Shortcut { sequence: "Escape"; onActivated: { page.cancel(); palette.close() } }
    onActiveChanged: if (!active) page.cancel()
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 16; spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Page de test"; font.pixelSize: 28; font.bold: true }
            Item { Layout.fillWidth: true }
            Button { objectName: "openInkPalette"; text: "reInk"; implicitHeight: 54; onClicked: window.showPalette("reink") }
            Button { objectName: "openStencilPalette"; text: "reStencil"; implicitHeight: 54; onClicked: window.showPalette("restencil") }
            Button { objectName: "openSelectionPalette"; text: "Propriétés"; implicitHeight: 54; onClicked: window.showPalette("properties") }
            Button { text: "Annuler"; implicitHeight: 54; enabled: page.canUndo; onClicked: page.undo() }
            Button { text: "Rétablir"; implicitHeight: 54; enabled: page.canRedo; onClicked: page.redo() }
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: "Banc PC · " + ({pen:"Trait libre", line:"Ligne", arrow:"Flèche", wire:"Fil orthogonal", rectangle:"Rectangle", ellipse:"Ellipse", symbol:"Placer un symbole", select:"Sélection"}[page.tool] || page.tool)
                color: "#555"
            }
            CheckBox { text: "Grille"; checked: page.gridVisible; onToggled: page.gridVisible = checked }
            CheckBox { text: "Aimantation"; checked: page.snapping; onToggled: page.snapping = checked }
        }
        InkCanvas {
            id: page; objectName: "editorPreviewPage"
            Layout.fillWidth: true; Layout.fillHeight: true
            Component.onCompleted: pageAdapter.attachPcPage(page)
            MouseArea {
                anchors.fill: parent; preventStealing: true
                acceptedButtons: Qt.LeftButton
                enabled: touch.downCount === 0
                onPressed: function(mouse) { page.begin(mouse.x,mouse.y) }
                onPositionChanged: function(mouse) { if (pressed) page.move(mouse.x,mouse.y) }
                onReleased: function(mouse) { page.end(mouse.x,mouse.y) }
                onCanceled: page.cancel()
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
                    if (downCount === 1) {
                        activeId = points[0].pointId; drawing = true
                        page.begin(points[0].x,points[0].y)
                    } else { drawing = false; page.cancel() }
                }
                onUpdated: function(points) {
                    if (drawing) for (let i=0; i<points.length; ++i)
                        if (points[i].pointId === activeId) page.move(points[i].x,points[i].y)
                }
                onReleased: function(points) {
                    if (drawing) for (let i=0; i<points.length; ++i)
                        if (points[i].pointId === activeId) { page.end(points[i].x,points[i].y); drawing = false }
                    downCount = Math.max(0,downCount-points.length)
                }
                onCanceled: { page.cancel(); drawing = false; downCount = 0 }
            }
        }
        Label {
            Layout.fillWidth: true; font.pixelSize: 16; color: "#555"; wrapMode: Text.WordWrap
            text: page.status || "Page de test locale. Les essais dans Xochitl restent distincts. Échap ou un second doigt abandonne le geste."
        }
    }
    NativeEditorPalette {
        id: palette
        parent: Overlay.overlay
        editor: pageAdapter
        x: Math.max(12, parent.width-width-20); y: Math.max(12,Math.min(80,parent.height-height-12))
    }
}

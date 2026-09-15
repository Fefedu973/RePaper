import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "qrc:/paper" as Paper
import RePaper.Drawing 1.0
import RePaper.Keyboard 1.0

ApplicationWindow {
    id: window
    visible: true
    width: 810; height: 1080
    title: "reStencil"
    color: "white"
    font.family: Paper.Theme.sans
    font.pixelSize: Paper.Theme.body
    property bool editing: false
    KeyboardController { id: keyboard; objectName: "textKeyboard"; window: window }
    palette.window: "white"
    palette.base: "white"
    palette.button: "white"
    palette.highlight: "#202020"
    palette.highlightedText: "white"

    ColumnLayout {
        anchors.fill: parent; anchors.margins: Paper.Theme.margin; anchors.bottomMargin: Paper.Theme.margin + keyboard.platformHeight; spacing: 16
        RowLayout {
            Layout.fillWidth: true
            Label { text: "reStencil"; font.pixelSize: Paper.Theme.title; font.family: Paper.Theme.serif }
            Item { Layout.fillWidth: true }
            Paper.Button { visible: catalogue.emulatorMode; text: window.editing ? "Aperçu" : "Page d’essai"; implicitHeight: Paper.Theme.control; onClicked: window.editing = !window.editing }
            Paper.IconButton { text: "Fermer"; symbol: "close"; onClicked: Qt.quit() }
        }
        RowLayout {
            Layout.fillWidth: true
            Paper.TextField {
                objectName: "symbolSearch"
                Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                placeholderText: "Rechercher un symbole…"
                onTextChanged: catalogue.query = text
            }
            Paper.ComboBox {
                implicitHeight: Paper.Theme.control; implicitWidth: 180
                model: ["Toutes catégories", "Passifs", "Semi-conducteurs", "Amplificateurs", "Sources", "Connexions"]
                onCurrentIndexChanged: catalogue.category = currentIndex === 0 ? "" : currentText
            }
            Paper.CheckBox { text: "Favoris"; onToggled: catalogue.favoritesOnly = checked }
        }
        SplitView {
            Layout.fillWidth: true; Layout.fillHeight: true; orientation: Qt.Horizontal
            ScrollView {
                SplitView.preferredWidth: window.editing ? window.width * 0.32 : window.width * 0.5
                SplitView.minimumWidth: 290
                clip: true
                GridView {
                    id: grid
                    model: catalogue.symbols
                    cellWidth: Math.floor(width / Math.max(1, Math.floor(width / 164)))
                    cellHeight: 160
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Item {
                        width: grid.cellWidth; height: grid.cellHeight
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 6
                            color: "white"
                            border.width: catalogue.selectedId === modelData.symbolId ? 1 : 0
                            border.color: catalogue.selectedId === modelData.symbolId ? "black" : "#ccc"
                            SymbolPreview {
                                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                                height: 110; symbolId: modelData.symbolId
                            }
                            Label {
                                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                anchors.margins: 10; horizontalAlignment: Text.AlignHCenter
                                text: (modelData.favorite ? "★ " : "") + modelData.name
                                wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.body
                            }
                            MouseArea { anchors.fill: parent; onClicked: catalogue.selectedId = modelData.symbolId }
                        }
                    }
                    Label {
                        anchors.centerIn: parent; text: "Aucun symbole"; visible: grid.count === 0
                    }
                }
            }
            Pane {
                SplitView.fillWidth: true; SplitView.minimumWidth: 300
                padding: 20
                ColumnLayout {
                    visible: !window.editing
                    anchors.fill: parent; spacing: 16
                    Label { Layout.fillWidth: true; text: catalogue.selected.name || ""; font.pixelSize: Paper.Theme.title; font.family: Paper.Theme.serif; wrapMode: Text.WordWrap }
                    Label { text: catalogue.selected.standard || ""; color: "#555" }
                    SymbolPreview {
                        Layout.fillWidth: true; Layout.preferredHeight: 230
                        symbolId: catalogue.selectedId; showAnchors: anchorsCheck.checked
                    }
                    Paper.CheckBox { id: anchorsCheck; text: "Points de connexion" }
                    Paper.Button {
                        Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                        text: catalogue.selected.favorite ? "Retirer des favoris" : "Ajouter aux favoris"
                        onClicked: catalogue.toggleFavorite(catalogue.selectedId)
                    }
                    Paper.Button {
                        objectName: "previewInsertButton"
                        Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                        primary: true; text: "Insérer"
                        enabled: activePageAdapter.available
                        onClicked: { window.editing = true; activePageAdapter.insert(catalogue.selectedId) }
                    }
                    Label {
                        Layout.fillWidth: true; text: catalogue.insertionStatus; visible: false
                        wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.caption; color: "#555"
                    }
                    Label { text: "Exporter ce symbole"; font.bold: true }
                    RowLayout {
                        Paper.Button { text: "SVG"; implicitHeight: Paper.Theme.control; onClicked: catalogue.exportSelected("svg") }
                        Paper.Button { text: "PDF"; implicitHeight: Paper.Theme.control; onClicked: catalogue.exportSelected("pdf") }
                        Paper.Button { text: "Traits"; implicitHeight: Paper.Theme.control; onClicked: catalogue.exportSelected("scene") }
                    }
                    Paper.Button { Layout.fillWidth: true; implicitHeight: Paper.Theme.control; text: "Importer les traits en bibliothèque"; onClicked: catalogue.importNative(false) }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    anchors.fill: parent; visible: window.editing; spacing: 10
                    Label { Layout.fillWidth: true; text: "Page d’essai"; font.pixelSize: Paper.Theme.title; font.family: Paper.Theme.serif; wrapMode: Text.WordWrap }
                    Paper.Button { Layout.fillWidth: true; implicitHeight: Paper.Theme.control; text: "Insérer : " + (catalogue.selected.name || ""); enabled: activePageAdapter.available; onClicked: activePageAdapter.insert(catalogue.selectedId) }
                    RowLayout {
                        Paper.Button { objectName: "selectionUndo"; text: "Annuler"; implicitHeight: Paper.Theme.control; enabled: testPage.canUndo; onClicked: testPage.undo() }
                        Paper.Button { text: "−"; implicitHeight: Paper.Theme.control; enabled: testPage.hasSelection; onClicked: testPage.scaleSelection(0.8) }
                        Paper.Button { objectName: "selectionScaleUp"; text: "+"; implicitHeight: Paper.Theme.control; enabled: testPage.hasSelection; onClicked: testPage.scaleSelection(1.25) }
                        Paper.Button { objectName: "selectionRotate"; text: "90°"; implicitHeight: Paper.Theme.control; enabled: testPage.hasSelection; onClicked: testPage.rotateSelection() }
                    }
                    InkCanvas {
                        id: testPage; objectName: "emulatorPage"; Layout.fillWidth: true; Layout.fillHeight: true; tool: "select"
                        Component.onCompleted: activePageAdapter.attachEmulatorPage(testPage)
                        MouseArea {
                            anchors.fill: parent; preventStealing: true
                            onPressed: function(mouse) { testPage.begin(mouse.x,mouse.y) }
                            onPositionChanged: function(mouse) { if(pressed) testPage.move(mouse.x,mouse.y) }
                            onReleased: function(mouse) { testPage.end(mouse.x,mouse.y) }
                            onCanceled: testPage.cancel()
                        }
                    }
                    RowLayout {
                        Paper.Button { objectName: "selectionCopy"; text: "Copier"; enabled: testPage.hasSelection; implicitHeight: Paper.Theme.control; onClicked: testPage.duplicateSelection() }
                        Paper.Button { text: "Supprimer"; enabled: testPage.hasSelection; implicitHeight: Paper.Theme.control; onClicked: testPage.removeSelection() }
                        Paper.Button { text: "Exporter les traits"; implicitHeight: Paper.Theme.control; onClicked: testPage.exportDrawing("scene") }
                    }
                    Label { Layout.fillWidth: true; text: testPage.status; wrapMode: Text.WrapAnywhere; font.pixelSize: Paper.Theme.caption; color: "#555" }
                }
            }
        }
        RowLayout {
            Label { text: "Pack complet"; Layout.fillWidth: true; font.bold: true }
            Paper.Button { text: "SVG"; implicitHeight: Paper.Theme.control; onClicked: catalogue.exportPack("svg") }
            Paper.Button { text: "PDF"; implicitHeight: Paper.Theme.control; onClicked: catalogue.exportPack("pdf") }
            Paper.Button { text: "Traits natifs"; implicitHeight: Paper.Theme.control; onClicked: catalogue.exportPack("scene") }
            Paper.Button { text: "Importer le pack"; implicitHeight: Paper.Theme.control; onClicked: catalogue.importNative(true) }
        }
        Label {
            Layout.fillWidth: true; text: catalogue.status; visible: Paper.Theme.actionable(text)
            font.pixelSize: Paper.Theme.caption; wrapMode: Text.WrapAnywhere; color: "#555"
        }
        TouchKeyboard { controller: keyboard; Layout.fillWidth: true }
    }
}

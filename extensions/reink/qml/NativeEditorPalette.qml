import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "qrc:/paper" as Paper

Popup {
    id: popup
    required property var editor
    property string mode: "reink"
    property bool contentRequested: false
    objectName: "nativeEditorPalette"
    width: Math.min(360 * Paper.Theme.unit, parent ? parent.width - 24 * Paper.Theme.unit : 360 * Paper.Theme.unit)
    height: Math.min(500 * Paper.Theme.unit, parent ? parent.height - 24 * Paper.Theme.unit : 500 * Paper.Theme.unit)
    padding: 12 * Paper.Theme.unit
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: "white"; border.color: "black"; border.width: Paper.Theme.line }
    contentItem: ColumnLayout {
        spacing: 4 * Paper.Theme.unit
        RowLayout {
            Layout.fillWidth: true
            Label { text: "reInk · reStencil"; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title; Layout.fillWidth: true }
            Paper.IconButton { symbol: "close"; text: "Fermer"; onClicked: popup.close() }
        }
        Loader {
            id: toolsLoader
            Layout.fillWidth: true; Layout.fillHeight: true
            active: popup.contentRequested
            sourceComponent: Component {
                ReInkSidebar {
                    editor: popup.editor
                    mode: popup.mode
                }
            }
        }
        Connections {
            target: toolsLoader.item
            function onToolChosen() { popup.close() }
        }
    }
    function syncToolsMode() {
        if (toolsLoader.item) toolsLoader.item.mode = popup.mode
    }
    onModeChanged: syncToolsMode()
    onAboutToShow: {
        contentRequested = true
        // Reset the retained sidebar before opening; changing it from onOpened
        // exposes the previous mode's layout during the first pointer event.
        syncToolsMode()
    }
}

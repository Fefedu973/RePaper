import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import RePaper.Keyboard 1.0

Rectangle {
    id: keyboard

    component Key: Button {
        id: key
        implicitHeight: 48
        font.family: Qt.fontFamilies().indexOf("reMarkable Sans") >= 0 ? "reMarkable Sans" : "DejaVu Sans"
        font.pixelSize: 14
        leftPadding: 8; rightPadding: 8
        contentItem: Text { text: key.text; font: key.font; color: key.down ? "white" : "black"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { color: key.down ? "black" : "white"; border.color: key.checked ? "black" : "#b3b3b3"; border.width: key.checked ? 2 : 1 }
    }
    property KeyboardController controller
    property bool symbols: false
    property bool uppercase: false
    property string pasteMessage: ""
    Connections { target: keyboard.controller; function onPasteFailed() { keyboard.pasteMessage = "Presse-papiers indisponible. Réessayez avec Ctrl+V ou Coller." } }
    onVisibleChanged: if (!visible) pasteMessage = ""
    visible: controller !== null && controller.fallbackVisible
    implicitHeight: visible ? keys.implicitHeight + 12 : 0
    color: "white"
    Rectangle { width: parent.width; height: 1; color: "black" }
    ColumnLayout {
        id: keys
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 6; spacing: 4
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Key { objectName: "keyboardPaste"; text: "Coller"; implicitHeight: 40; focusPolicy: Qt.NoFocus; onClicked: { keyboard.pasteMessage = ""; keyboard.controller.paste() } }
            Key { objectName: "keyboardHide"; text: "Masquer"; implicitHeight: 40; focusPolicy: Qt.NoFocus; onClicked: keyboard.controller.dismiss() }
        }
        Label { visible: text.length > 0; text: keyboard.pasteMessage; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: 14 }
        GridLayout {
            columns: 10; rowSpacing: 4; columnSpacing: 4; Layout.fillWidth: true
            Repeater {
                model: keyboard.symbols ? ["1","2","3","4","5","6","7","8","9","0","@",":","/",".","-","_","?","&","=","%","+","!","#","$","(",")","[","]",",",";"] : "azertyuiopqsdfghjklmwxcvbnàéèç".split("")
                delegate: Key {
                    required property string modelData
                    objectName: "keyboardKey_" + modelData
                    text: keyboard.uppercase ? modelData.toUpperCase() : modelData
                    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 1; implicitHeight: 48
                    font.pixelSize: 18; focusPolicy: Qt.NoFocus
                    onClicked: keyboard.controller.insertText(text)
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 4
            Key { text: keyboard.symbols ? "ABC" : "123 /"; implicitHeight: 48; focusPolicy: Qt.NoFocus; onClicked: keyboard.symbols = !keyboard.symbols }
            Key { text: "Maj"; checkable: true; checked: keyboard.uppercase; implicitHeight: 48; focusPolicy: Qt.NoFocus; onClicked: keyboard.uppercase = !keyboard.uppercase }
            Key { text: "←"; implicitWidth: 50; implicitHeight: 48; focusPolicy: Qt.NoFocus; onClicked: keyboard.controller.moveCursor(-1) }
            Key { text: "Espace"; Layout.fillWidth: true; Layout.minimumWidth: 70; implicitHeight: 48; focusPolicy: Qt.NoFocus; onClicked: keyboard.controller.insertText(" ") }
            Key { text: "→"; implicitWidth: 50; implicitHeight: 48; focusPolicy: Qt.NoFocus; onClicked: keyboard.controller.moveCursor(1) }
            Key { objectName: "keyboardBackspace"; text: "⌫"; implicitWidth: 65; implicitHeight: 48; focusPolicy: Qt.NoFocus; onClicked: keyboard.controller.backspace() }
            Key { objectName: "keyboardEnter"; text: "Entrée"; implicitWidth: 95; implicitHeight: 48; focusPolicy: Qt.NoFocus; onClicked: keyboard.controller.enter() }
        }
    }
}

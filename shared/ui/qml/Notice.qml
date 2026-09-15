import QtQuick 2.15
import QtQuick.Layouts 1.15
Rectangle {
    id: notice
    property string text: ""
    property bool shown: false
    visible: shown && text.length > 0
    color: "white"
    Rectangle { width: parent.width; height: Theme.line; color: Theme.divider }
    implicitHeight: Math.max(Theme.control, message.implicitHeight + 24 * Theme.unit)
    onTextChanged: { shown = Theme.actionable(text); if (shown) timeout.restart() }
    Timer { id: timeout; interval: 12000; onTriggered: notice.shown = false }
    Text { id: message; anchors.left: parent.left; anchors.right: dismiss.left; anchors.verticalCenter: parent.verticalCenter; anchors.margins: 12 * Theme.unit; text: notice.text; textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.family: Theme.sans; font.pixelSize: Theme.body; color: "black" }
    IconButton { id: dismiss; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; symbol: "close"; text: "Fermer le message"; onClicked: notice.shown = false }
}

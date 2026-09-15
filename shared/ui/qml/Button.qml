import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.Button {
    id: control
    property bool primary: false
    property bool quiet: false
    implicitHeight: Theme.control
    implicitWidth: Math.max(Theme.control, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: 16 * Theme.unit; rightPadding: leftPadding
    topPadding: 8 * Theme.unit; bottomPadding: topPadding
    font.family: Theme.sans; font.pixelSize: Theme.body; font.weight: Font.Medium
    contentItem: Text {
        text: control.text; font: control.font; textFormat: Text.PlainText
        color: control.primary !== control.down ? "white" : "black"
        opacity: control.enabled ? 1 : 0.35
        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: control.primary !== control.down ? "black" : "white"
        border.width: control.quiet && !control.activeFocus ? 0 : Theme.line
        border.color: control.enabled ? "black" : Theme.divider
        Rectangle {
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: 2 * Theme.unit; color: "black"; visible: control.checked && !control.primary
        }
    }
}

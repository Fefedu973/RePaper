import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.CheckBox {
    id: control
    implicitHeight: Theme.control
    font.family: Theme.sans; font.pixelSize: Theme.body
    spacing: 12 * Theme.unit
    indicator: Rectangle {
        implicitWidth: 20 * Theme.unit; implicitHeight: implicitWidth
        x: control.leftPadding; y: (control.height-height)/2
        color: control.checked ? "black" : "white"; border.color: "black"; border.width: Theme.line
        Text { anchors.centerIn: parent; text: "✓"; color: "white"; font.pixelSize: 15 * Theme.unit; visible: control.checked }
    }
    contentItem: Text { text: control.text; font: control.font; color: "black"; verticalAlignment: Text.AlignVCenter; leftPadding: control.indicator.width + control.spacing }
}

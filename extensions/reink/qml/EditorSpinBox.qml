import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
import "qrc:/paper" as Paper

Controls.SpinBox {
    id: control
    implicitWidth: 180 * Paper.Theme.unit
    implicitHeight: Paper.Theme.control
    leftPadding: 40 * Paper.Theme.unit
    rightPadding: leftPadding
    topPadding: 4 * Paper.Theme.unit
    bottomPadding: topPadding
    font.family: Paper.Theme.sans
    font.pixelSize: Paper.Theme.body
    contentItem: TextInput {
        text: control.textFromValue(control.value, control.locale)
        font: control.font
        color: control.enabled ? "black" : Paper.Theme.muted
        selectionColor: "black"; selectedTextColor: "white"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        selectByMouse: true
    }
    up.indicator: Rectangle {
        x: control.width - width
        implicitWidth: 40 * Paper.Theme.unit; implicitHeight: Paper.Theme.control
        height: control.height
        color: control.up.pressed ? "black" : "white"
        border.width: Paper.Theme.line; border.color: control.enabled ? "black" : Paper.Theme.divider
        Text { anchors.centerIn: parent; text: "+"; font.family: Paper.Theme.sans; font.pixelSize: 22 * Paper.Theme.unit; color: control.up.pressed ? "white" : "black"; opacity: control.enabled && (control.wrap || control.value < control.to) ? 1 : 0.3 }
    }
    down.indicator: Rectangle {
        implicitWidth: 40 * Paper.Theme.unit; implicitHeight: Paper.Theme.control
        height: control.height
        color: control.down.pressed ? "black" : "white"
        border.width: Paper.Theme.line; border.color: control.enabled ? "black" : Paper.Theme.divider
        Text { anchors.centerIn: parent; text: "−"; font.family: Paper.Theme.sans; font.pixelSize: 22 * Paper.Theme.unit; color: control.down.pressed ? "white" : "black"; opacity: control.enabled && (control.wrap || control.value > control.from) ? 1 : 0.3 }
    }
    background: Rectangle {
        color: "white"; border.width: Paper.Theme.line
        border.color: control.enabled ? "black" : Paper.Theme.divider
    }
}

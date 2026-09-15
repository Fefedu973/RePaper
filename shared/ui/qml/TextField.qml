import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.TextField {
    id: control
    implicitHeight: Theme.control
    padding: 12 * Theme.unit
    color: "black"; placeholderTextColor: Theme.muted
    font.family: Theme.sans; font.pixelSize: Theme.body
    selectionColor: "black"; selectedTextColor: "white"
    background: Rectangle {
        color: "white"
        border.width: control.activeFocus ? 2 * Theme.unit : Theme.line
        border.color: control.enabled ? "black" : Theme.divider
    }
}

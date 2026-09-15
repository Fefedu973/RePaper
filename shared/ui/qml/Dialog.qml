import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.Dialog {
    id: control
    padding: 24 * Theme.unit
    font.family: Theme.sans; font.pixelSize: Theme.body
    background: Rectangle { color: "white"; border.color: "black"; border.width: Theme.line }
    enter: Transition {}
    exit: Transition {}
    header: Text {
        text: control.title; textFormat: Text.PlainText; wrapMode: Text.WordWrap
        font.family: Theme.serif; font.pixelSize: Theme.title; color: "black"
        leftPadding: control.padding; rightPadding: control.padding
        topPadding: control.padding; bottomPadding: 20 * Theme.unit
    }
    footer: Controls.DialogButtonBox {
        visible: count > 0; standardButtons: control.standardButtons
        spacing: 12 * Theme.unit
        padding: control.padding
        delegate: Button {}
        background: Rectangle { color: "white" }
    }
    Controls.Overlay.modal: Rectangle { color: "#bfffffff" }
}

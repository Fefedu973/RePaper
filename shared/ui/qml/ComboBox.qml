import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.ComboBox {
    id: control
    implicitHeight: Theme.control
    implicitWidth: 200 * Theme.unit
    font.family: Theme.sans; font.pixelSize: Theme.body
    leftPadding: 12 * Theme.unit; rightPadding: 36 * Theme.unit
    contentItem: Text { text: control.displayText; font: control.font; color: "black"; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    indicator: Icon { name: "next"; rotation: 90; x: control.width - width - 8 * Theme.unit; y: (control.height-height)/2 }
    background: Rectangle { color: "white"; border.color: "black"; border.width: Theme.line }
    delegate: MenuItem { width: ListView.view.width; text: control.textRole ? (Array.isArray(control.model) ? modelData[control.textRole] : model[control.textRole]) : modelData; highlighted: control.highlightedIndex === index }
    popup: Controls.Popup {
        y: control.height; width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight, 320 * Theme.unit)
        padding: Theme.line
        contentItem: ListView { clip: true; implicitHeight: contentHeight; model: control.popup.visible ? control.delegateModel : null; currentIndex: control.highlightedIndex }
        background: Rectangle { color: "white"; border.color: "black"; border.width: Theme.line }
    }
}

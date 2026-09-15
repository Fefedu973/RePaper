import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.MenuItem {
    id: control
    implicitHeight: Theme.control
    implicitWidth: Math.max(240 * Theme.unit, implicitContentWidth + 32 * Theme.unit)
    leftPadding: 16 * Theme.unit; rightPadding: leftPadding
    font.family: Theme.sans; font.pixelSize: Theme.body
    contentItem: Text { text: control.text; textFormat: Text.PlainText; font: control.font; color: control.highlighted ? "white" : "black"; opacity: control.enabled ? 1 : 0.35; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    background: Rectangle { color: control.highlighted ? "black" : "white" }
}

import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.ScrollBar {
    id: control
    policy: Controls.ScrollBar.AsNeeded
    padding: 2 * Theme.unit
    contentItem: Rectangle { implicitWidth: 2 * Theme.unit; implicitHeight: 2 * Theme.unit; color: "black"; opacity: control.size < 1 ? 1 : 0 }
    background: Item { implicitWidth: 6 * Theme.unit; implicitHeight: 6 * Theme.unit }
}

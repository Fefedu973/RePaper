import QtQuick 2.15
Button {
    id: control
    property string symbol: "more"
    quiet: true
    implicitWidth: Theme.control
    contentItem: Item {
        Icon { anchors.centerIn: parent; name: control.symbol; ink: control.down ? "white" : "black"; opacity: control.enabled ? 1 : 0.3 }
    }
}

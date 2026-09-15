import QtQuick 2.15
import QtQuick.Controls 2.15 as Controls
Controls.Menu {
    padding: 8 * Theme.unit
    font.family: Theme.sans; font.pixelSize: Theme.body
    background: Rectangle { color: "white"; border.color: "black"; border.width: Theme.line }
    delegate: MenuItem {}
    enter: Transition {}
    exit: Transition {}
}

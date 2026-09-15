import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import RePaper.Keyboard 1.0

ApplicationWindow {
    id: window; width: 936; height: 1248; visible: true
    property int acceptedCount: 0
    KeyboardController { id: keyboard; objectName: "controller"; window: window }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 20
        TextField { objectName: "plain"; Layout.fillWidth: true; implicitHeight: 52; selectByMouse: true; onAccepted: window.acceptedCount++ }
        TextField { objectName: "password"; Layout.fillWidth: true; implicitHeight: 52; echoMode: TextInput.Password; selectByMouse: true }
        TextField { objectName: "readonly"; Layout.fillWidth: true; readOnly: true; text: "Lecture seule"; selectByMouse: true }
        TextArea { objectName: "multiline"; Layout.fillWidth: true; implicitHeight: 140; selectByMouse: true }
        Button { objectName: "outside"; text: "Autre commande"; onClicked: forceActiveFocus() }
        Item { Layout.fillHeight: true }
        TouchKeyboard { objectName: "panel"; controller: keyboard; Layout.fillWidth: true }
    }
}

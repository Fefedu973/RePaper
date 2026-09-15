import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "qrc:/paper" as Paper

RowLayout {
    id: picker
    property string currentColor: "#000000"
    signal colorChosen(string color)
    signal keyboardRequested()
    readonly property var colors: ["#000000", "#7d7d7d", "#ffffff", "#d90707", "#0062cc", "#249b45", "#fae719", "#c07fd2", "#74d2e8"]
    function discardPendingInput() {
        hexInput.hasUserEdit = false
        hexInput.text = Qt.binding(function() { return picker.currentColor })
    }
    spacing: 8 * Paper.Theme.unit
    Rectangle {
        Layout.preferredWidth: 24 * Paper.Theme.unit; Layout.preferredHeight: 24 * Paper.Theme.unit
        color: picker.currentColor || "transparent"
        border.color: "black"; border.width: Paper.Theme.line
        Text { anchors.centerIn: parent; visible: !picker.currentColor; text: "—" }
    }
    Paper.ComboBox {
        objectName: "strokeColorChoices"
        Layout.fillWidth: true; implicitHeight: Paper.Theme.control
        model: ["Noir", "Gris", "Blanc", "Rouge", "Bleu", "Vert", "Jaune", "Violet", "Cyan"]
        currentIndex: picker.colors.indexOf(picker.currentColor.toLowerCase())
        displayText: currentIndex < 0 ? (picker.currentColor ? "Personnalisée" : "Couleurs variées") : currentText
        onActivated: index => picker.colorChosen(picker.colors[index])
    }
    Paper.TextField {
        id: hexInput
        objectName: "strokeColorHex"
        property bool hasUserEdit: false
        Layout.preferredWidth: 100 * Paper.Theme.unit; implicitHeight: Paper.Theme.control
        text: picker.currentColor
        placeholderText: "#RRGGBB"
        maximumLength: 7
        validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        onTextEdited: hasUserEdit = true
        onActiveFocusChanged: if (activeFocus && enabled) { picker.keyboardRequested(); Qt.inputMethod.show() }
        onEditingFinished: {
            if (!hasUserEdit || !acceptableInput) return
            // Enter and focus loss can both finish one edit while the native
            // selection color is still awaiting its asynchronous refresh.
            hasUserEdit = false
            if (text.toLowerCase() !== picker.currentColor.toLowerCase()) picker.colorChosen(text.toLowerCase())
        }
    }
}

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "qrc:/paper" as Paper

ColumnLayout {
    id: configuration
    required property var editor
    property string symbolId: ""
    property var values: ({})
    property bool editingSelection: false
    property bool canEdit: true
    readonly property var preview: symbolId.length ? editor.stencilPreview(symbolId, values) : ({})
    signal accepted()
    signal canceled()
    signal keyboardRequested()
    spacing: 10 * Paper.Theme.unit

    function setValue(key, value) {
        const next = Object.assign({}, values)
        next[key] = value
        if (symbolId === "bode" && key === "mode") {
            next.yMin = value === "phase" ? -180 : -60
            next.yMax = value === "phase" ? 0 : 20
        }
        if (symbolId === "graph" && key === "curveType" && value === "sine" && values.curveType !== "sine") {
            const amplitude = Math.abs(Number(next.amplitude)) || 1
            const offset = Number(next.offset) || 0
            next.yMin = Math.max(-1e6, offset - 1.2 * amplitude)
            next.yMax = Math.min(1e6, offset + 1.2 * amplitude)
        }
        values = next
    }
    function fieldVisible(key) {
        if (symbolId === "graph") {
            const curve = values.curveType || "none"
            if (key === "gain") return curve === "exponential" || curve === "second-order"
            if (key === "tau" || key === "initialValue") return curve === "exponential"
            if (key === "omega0" || key === "damping") return curve === "second-order"
            if (["amplitude", "offset", "frequencyHz", "phaseDegrees"].indexOf(key) >= 0) return curve === "sine"
        }
        if (symbolId === "bode" && ["gain", "omega0", "damping"].indexOf(key) >= 0) return values.curve === true
        return true
    }
    function commitInputs() { focusTarget.forceActiveFocus() }
    function apply() {
        commitInputs()
        if (!canEdit || !editor.available || preview.valid !== true) return false
        return editingSelection ? editor.setStencilParameters(values)
                                : editor.beginConfiguredStencil(symbolId, values)
    }
    Item { id: focusTarget }
    Label {
        Layout.fillWidth: true; font.bold: true
        text: configuration.symbolId === "table" ? "Tableau" : configuration.symbolId === "bode" ? "Diagramme de Bode" : "Graphique"
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.caption
        visible: configuration.symbolId !== "table"
        text: configuration.symbolId === "table" ? ""
            : configuration.symbolId === "bode" ? "Fréquence logarithmique en Hz. Gain en dB ou phase en degrés."
            : configuration.values.curveType === "sine" ? "Sinusoïde : fréquence en Hz, phase en degrés, temps en secondes."
            : configuration.values.curveType === "second-order" ? "Temps en secondes. Réponse à un échelon unitaire."
            : "Temps en secondes."
    }
    Rectangle {
        Layout.fillWidth: true; Layout.preferredHeight: Math.min(120 * Paper.Theme.unit, width * 2 / 3)
        color: "white"; border.color: Paper.Theme.divider; border.width: Paper.Theme.line
        StencilPreview { anchors.fill: parent; drawing: configuration.preview }
    }
    Repeater {
        model: configuration.symbolId.length ? configuration.editor.stencilSchema(configuration.symbolId) : []
        RowLayout {
            id: field
            required property var modelData
            spacing: 8 * Paper.Theme.unit
            Layout.fillWidth: true
            visible: configuration.fieldVisible(modelData.key)
            enabled: configuration.canEdit
            Label {
                Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.preferredWidth: 100 * Paper.Theme.unit
                text: field.modelData.label; wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.body
            }
            Loader {
                Layout.fillWidth: true; Layout.preferredWidth: 200 * Paper.Theme.unit
                Layout.minimumWidth: 100 * Paper.Theme.unit; Layout.minimumHeight: Paper.Theme.control
                sourceComponent: field.modelData.type === "choice" ? choiceInput
                    : field.modelData.type === "bool" ? booleanInput
                    : field.modelData.type === "integer" ? integerInput : numberInput
            }
            Component {
                id: integerInput
                EditorSpinBox {
                    objectName: "stencilParameter_" + field.modelData.key
                    from: field.modelData.min; to: field.modelData.max
                    stepSize: field.modelData.step || 1; editable: true; implicitHeight: Paper.Theme.control
                    value: Number(configuration.values[field.modelData.key])
                    onValueModified: configuration.setValue(field.modelData.key, value)
                    onActiveFocusChanged: if (activeFocus) { configuration.keyboardRequested(); Qt.inputMethod.show() }
                }
            }
            Component {
                id: numberInput
                Paper.TextField {
                    id: numberField
                    objectName: "stencilParameter_" + field.modelData.key
                    implicitHeight: Paper.Theme.control; selectByMouse: true
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    function refreshValue() {
                        const value = configuration.values[field.modelData.key]
                        text = value === null || value === undefined ? "" : String(Number(Number(value).toPrecision(8)))
                    }
                    Component.onCompleted: refreshValue()
                    Connections {
                        target: configuration
                        function onValuesChanged() { if (!numberField.activeFocus) numberField.refreshValue() }
                    }
                    onActiveFocusChanged: if (activeFocus) { configuration.keyboardRequested(); Qt.inputMethod.show() }
                    onEditingFinished: {
                        const input = text.trim().replace(",", ".")
                        const parsed = input.length ? Number(input) : NaN
                        configuration.setValue(field.modelData.key, isFinite(parsed) ? parsed : null)
                        if (!activeFocus) refreshValue()
                    }
                }
            }
            Component {
                id: choiceInput
                Paper.ComboBox {
                    objectName: "stencilParameter_" + field.modelData.key
                    implicitHeight: Paper.Theme.control
                    model: field.modelData.options; textRole: "label"
                    currentIndex: {
                        const options = field.modelData.options || []
                        for (let i = 0; i < options.length; ++i)
                            if (options[i].value === configuration.values[field.modelData.key]) return i
                        return 0
                    }
                    onActivated: function(index) { configuration.setValue(field.modelData.key, field.modelData.options[index].value) }
                }
            }
            Component {
                id: booleanInput
                Paper.CheckBox {
                    objectName: "stencilParameter_" + field.modelData.key
                    implicitHeight: Paper.Theme.control
                    checked: configuration.values[field.modelData.key] === true
                    onToggled: configuration.setValue(field.modelData.key, checked)
                }
            }
        }
    }
    Label {
        Layout.fillWidth: true; visible: text.length > 0
        text: configuration.preview.error || ""; wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.caption
    }
    RowLayout {
        Layout.fillWidth: true
        Paper.Button {
            objectName: "cancelStencilConfiguration"
            Layout.fillWidth: true; implicitHeight: Paper.Theme.control; text: "Annuler"
            onClicked: configuration.canceled()
        }
        Paper.Button {
            objectName: "applyStencilConfiguration"
            Layout.fillWidth: true; implicitHeight: Paper.Theme.control
            text: configuration.editingSelection ? "Appliquer" : "Placer"
            enabled: configuration.canEdit && configuration.editor.available && configuration.preview.valid === true
            onClicked: {
                if (configuration.apply()) configuration.accepted()
            }
        }
    }
}

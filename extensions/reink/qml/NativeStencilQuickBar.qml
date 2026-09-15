import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "qrc:/paper" as Paper

Rectangle {
    id: bar
    required property var editor
    property bool optionsOpen: false
    readonly property bool stencilActive: state("tool", "symbol") === "symbol"
    readonly property bool voltageOptionsVisible: stencilActive && optionsOpen && state("stencilSupportsVoltage", false) && state("stencilVoltageArrow", false)
    signal penRequested()
    readonly property var pageState: visible ? (editor.state || ({})) : ({})
    property var quickSymbols: []
    readonly property var catalogue: editor.stencils || []
    function state(key, fallback) { return pageState[key] === undefined ? fallback : pageState[key] }
    function circuitSymbols() {
        const pinned = ["signal-generator", "switch", "switch-closed"]
        const recent = state("recentStencils", []).concat(["resistor-iec", "capacitor", "voltage-source"])
        const result = []
        for (let i = 0; i < recent.length && result.length < 3; ++i) {
            const id = recent[i]
            if (id !== "square-root" && pinned.indexOf(id) < 0 && result.indexOf(id) < 0) result.push(id)
        }
        return result.concat(pinned)
    }
    function symbol(id) {
        const all = catalogue
        for (let i = 0; i < all.length; ++i) if (all[i].id === id) return all[i]
        return ({ id: id, name: id, strokes: [] })
    }
    implicitWidth: 480 * Paper.Theme.unit
    implicitHeight: Paper.Theme.control + 4 * Paper.Theme.unit + (voltageOptionsVisible ? Paper.Theme.control : 0)
    color: "white"; border.color: "black"; border.width: Paper.Theme.line
    function syncQuickSymbols() {
        const next = circuitSymbols()
        // A state notification need not replace six thumbnail delegates.
        if (next.join("\u001f") !== quickSymbols.join("\u001f")) quickSymbols = next
    }
    onPageStateChanged: {
        if (!stencilActive || !state("stencilSupportsVoltage", false) || !state("stencilVoltageArrow", false)) optionsOpen = false
        if (visible) syncQuickSymbols()
    }
    onVisibleChanged: {
        if (!visible) optionsOpen = false
        else syncQuickSymbols()
    }
    Component.onCompleted: syncQuickSymbols()
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 2 * Paper.Theme.unit; spacing: 0
        RowLayout {
            Layout.fillWidth: true; spacing: 0
            Paper.IconButton {
                objectName: "quickResumePen"; symbol: "pen"; text: "Stylo"
                onClicked: bar.penRequested()
            }
            Paper.Button {
                id: wireButton
                objectName: "quickOrthogonalWire"; text: "Fil"; quiet: true
                Layout.preferredWidth: 76 * Paper.Theme.unit
                leftPadding: 8 * Paper.Theme.unit; rightPadding: leftPadding
                checked: bar.state("tool", "") === "wire"
                contentItem: RowLayout {
                    spacing: 6 * Paper.Theme.unit
                    Paper.Icon {
                        objectName: "quickOrthogonalWireIcon"; name: "wire"
                        ink: wireButton.down ? "white" : "black"
                    }
                    Text {
                        text: wireButton.text; font: wireButton.font
                        color: wireButton.down ? "white" : "black"
                        verticalAlignment: Text.AlignVCenter
                        Layout.fillWidth: true
                    }
                }
                onClicked: if (bar.editor.chooseTool("wire")) {
                    bar.optionsOpen = false
                    actionsViewport.contentX = 0
                }
            }
            Flickable {
                id: actionsViewport
                objectName: "quickStencilActions"
                Layout.fillWidth: true; Layout.preferredHeight: Paper.Theme.control
                contentWidth: actions.implicitWidth; contentHeight: height; clip: true
                boundsBehavior: Flickable.StopAtBounds; flickableDirection: Flickable.HorizontalFlick
                Row {
                    id: actions; height: parent.height; spacing: 0
                    Repeater {
                        model: bar.quickSymbols
                        Paper.Button {
                            id: recentButton
                            objectName: "quickStencil_" + modelData
                            readonly property var drawing: bar.symbol(modelData)
                            width: Paper.Theme.control; height: Paper.Theme.control; quiet: true
                            leftPadding: 4 * Paper.Theme.unit; rightPadding: leftPadding
                            topPadding: 4 * Paper.Theme.unit; bottomPadding: topPadding
                            checked: bar.stencilActive && bar.state("activeStencilId", "") === modelData
                            text: drawing.name
                            contentItem: StencilPreview { drawing: recentButton.drawing; thumbnail: true }
                            onClicked: bar.editor.beginStencil(modelData)
                        }
                    }
                    Paper.Button {
                        objectName: "quickStencilOrientation"
                        width: 92 * Paper.Theme.unit; height: Paper.Theme.control; quiet: true
                        leftPadding: 8 * Paper.Theme.unit; rightPadding: leftPadding
                        visible: bar.stencilActive && bar.state("activeStencilId", "") !== "square-root"
                        text: bar.state("stencilVertical", false) ? "Vertical" : "Horizontal"
                        onClicked: bar.editor.setStencilVertical(!bar.state("stencilVertical", false))
                    }
                    Paper.Button {
                        objectName: "quickStencilVoltage"
                        width: 84 * Paper.Theme.unit; height: Paper.Theme.control; quiet: true
                        visible: bar.stencilActive && bar.state("stencilSupportsVoltage", false)
                        checked: bar.state("stencilVoltageArrow", false); text: "Tension"
                        onClicked: bar.editor.setVoltageArrow(!bar.state("stencilVoltageArrow", false))
                    }
                    Paper.IconButton {
                        objectName: "quickStencilVoltageOptions"; symbol: "more"; text: "Flèche de tension"
                        visible: bar.stencilActive && bar.state("stencilSupportsVoltage", false) && bar.state("stencilVoltageArrow", false)
                        onClicked: bar.optionsOpen = !bar.optionsOpen
                    }
                }
            }
        }
        RowLayout {
            visible: bar.voltageOptionsVisible; Layout.fillWidth: true; spacing: 0
            Paper.Button {
                objectName: "quickVoltageReverse"; text: "Inverser la flèche"; quiet: true; Layout.fillWidth: true
                onClicked: bar.editor.setVoltageReversed(!bar.state("stencilVoltageReversed", false))
            }
            Paper.Button {
                objectName: "quickVoltageSide"; text: "Changer de côté"; quiet: true; Layout.fillWidth: true
                onClicked: bar.editor.setVoltageOtherSide(!bar.state("stencilVoltageOtherSide", false))
            }
        }
    }
}

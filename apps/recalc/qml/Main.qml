import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReCalc 1.0
import "qrc:/paper" as Paper

ApplicationWindow {
    id: window
    width: 1000
    height: 1300
    visible: true
    title: "reCalc"
    color: "white"
    readonly property var hostViewport: typeof appLoadViewport !== "undefined" ? appLoadViewport : null
    property real u: Math.min(1, surface.width / (440 * Paper.Theme.unit), surface.height / (820 * Paper.Theme.unit))
    Binding { target: Paper.Theme; property: "unit"; value: window.hostViewport && window.hostViewport.available ? 2 : 1 }
    readonly property real fontScale: Math.max(0.8, u)
    property bool advanced: false
    font.family: Paper.Theme.sans
    font.pixelSize: Paper.Theme.body * fontScale

    component SurfaceSheet: Pane {
        id: sheet
        property string title: ""
        parent: surface
        x: 8 * Paper.Theme.unit; y: x
        width: Math.max(1, surface.width - 2 * x)
        height: Math.max(1, surface.height - 2 * y)
        visible: false; z: 100
        padding: 16 * Paper.Theme.unit
        topPadding: 80 * Paper.Theme.unit
        function open() { visible = true; forceActiveFocus() }
        function close() { visible = false; formula.forceActiveFocus() }
        Keys.onEscapePressed: close()
        background: Rectangle { color: "white"; border.color: Paper.Theme.divider; border.width: Paper.Theme.line; MouseArea { anchors.fill: parent } }
        property Item heading: RowLayout {
            parent: sheet
            x: sheet.padding; y: sheet.padding
            width: sheet.width - 2 * sheet.padding
            spacing: 8 * Paper.Theme.unit
            Text { text: sheet.title; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title }
            Paper.IconButton { text: "Fermer"; symbol: "close"; onClicked: sheet.close() }
        }
    }

    component PaperButton: Paper.Button {
        implicitHeight: Paper.Theme.control * window.u
        implicitWidth: 0
        leftPadding: 8 * Paper.Theme.unit * window.u
        rightPadding: leftPadding
        topPadding: 6 * Paper.Theme.unit * window.u
        bottomPadding: topPadding
        font.pixelSize: Paper.Theme.body * window.fontScale
        focusPolicy: Qt.NoFocus
    }

    Item {
        id: surface
        objectName: "calculatorSurface"
        width: window.hostViewport && window.hostViewport.available ? window.hostViewport.width : window.width
        height: window.hostViewport && window.hostViewport.available ? window.hostViewport.height : window.height
        transform: Matrix4x4 { matrix: window.hostViewport && window.hostViewport.available ? window.hostViewport.contentTransform : Qt.matrix4x4() }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24 * Paper.Theme.unit * window.u
        spacing: 10 * Paper.Theme.unit * window.u
        RowLayout {
            Layout.fillWidth: true
            spacing: 8 * Paper.Theme.unit * window.u
            Text {
                text: "reCalc"
                Layout.fillWidth: true
                font.family: Paper.Theme.serif
                font.pixelSize: Paper.Theme.title * window.fontScale
            }
            Text { text: calc.hasMemory ? "M" : ""; font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.caption * window.fontScale }
            PaperButton {
                text: calc.degrees ? "DEG" : "RAD"
                implicitWidth: 60 * Paper.Theme.unit * window.u
                quiet: true
                font.pixelSize: Paper.Theme.caption * window.fontScale
                onClicked: { calc.command("angle"); formula.forceActiveFocus() }
            }
            Paper.IconButton {
                text: "Historique"
                symbol: "history"
                implicitWidth: Paper.Theme.control * window.u
                implicitHeight: implicitWidth
                focusPolicy: Qt.NoFocus
                onClicked: historyDrawer.open()
            }
            Paper.IconButton {
                id: moreButton
                text: "Options"
                symbol: "more"
                implicitWidth: Paper.Theme.control * window.u
                implicitHeight: implicitWidth
                focusPolicy: Qt.NoFocus
                onClicked: actions.open()
                SurfaceSheet {
                    id: actions
                    title: "Options"
                    contentItem: ScrollView {
                        id: actionsScroll
                        contentWidth: availableWidth
                        contentHeight: actionsContent.implicitHeight
                        clip: true
                        ColumnLayout {
                            id: actionsContent
                            width: actionsScroll.availableWidth
                            spacing: 8 * Paper.Theme.unit
                            Paper.Button { quiet: true; Layout.fillWidth: true; text: "Copier le calcul"; onClicked: { calc.copy(false); actions.close() } }
                            Paper.Button { quiet: true; Layout.fillWidth: true; text: "Copier en LaTeX"; onClicked: { calc.copy(true); actions.close() } }
                            Paper.Button { quiet: true; Layout.fillWidth: true; text: "Exporter en LaTeX"; onClicked: { calc.exportCalculation(); actions.close() } }
                            Paper.Button { quiet: true; Layout.fillWidth: true; text: "Précision : " + calc.precision + " chiffres"; onClicked: calc.command("precision") }
                            Paper.Button { quiet: true; Layout.fillWidth: true; text: "Mode d’emploi"; onClicked: { actions.close(); help.open() } }
                            Paper.Button { quiet: true; Layout.fillWidth: true; text: "Fermer reCalc"; onClicked: Qt.quit() }
                        }
                    }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: Paper.Theme.line; color: Paper.Theme.divider }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120 * Paper.Theme.unit * window.u
            Flickable {
                id: formulaScroll
                anchors.fill: parent
                anchors.margins: 2
                contentWidth: Math.max(width, formula.implicitWidth)
                contentHeight: Math.max(height, formula.implicitHeight)
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                MathCanvas {
                    id: formula
                    calculator: calc
                    fontSize: 34 * Paper.Theme.unit * window.u
                    // Paint only the visible viewport; a long expression never allocates a giant texture.
                    x: formulaScroll.contentX
                    y: formulaScroll.contentY
                    viewportX: formulaScroll.contentX
                    viewportY: formulaScroll.contentY
                    width: formulaScroll.width
                    height: formulaScroll.height
                    focus: true
                    onCaretMoved: {
                        if (caretX < formulaScroll.contentX + 20) formulaScroll.contentX = Math.max(0, caretX - 40)
                        if (caretX > formulaScroll.contentX + formulaScroll.width - 35) formulaScroll.contentX = Math.min(formulaScroll.contentWidth - formulaScroll.width, caretX - formulaScroll.width + 55)
                        if (caretY < formulaScroll.contentY + 10) formulaScroll.contentY = Math.max(0, caretY - 30)
                        if (caretY > formulaScroll.contentY + formulaScroll.height - 60) formulaScroll.contentY = Math.min(formulaScroll.contentHeight - formulaScroll.height, caretY - formulaScroll.height + 90)
                    }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: calc.busy || calc.result.length > 0 || calc.approximation.length > 0 || calc.message.length > 0
            spacing: 6 * Paper.Theme.unit * window.u
            Flickable {
                visible: calc.busy || calc.result.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: 40 * Paper.Theme.unit * window.u
                contentWidth: Math.max(width, resultText.implicitWidth)
                contentHeight: height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Text {
                    id: resultText
                    text: calc.busy ? "Calcul…" : calc.result
                    color: "black"
                    font.family: Paper.Theme.sans
                    font.pixelSize: 28 * Paper.Theme.unit * window.fontScale
                }
            }
            Text {
                text: calc.approximation
                visible: text.length > 0
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.family: Paper.Theme.sans
                font.pixelSize: Paper.Theme.body * window.fontScale
                color: Paper.Theme.muted
            }
            Text {
                text: calc.message
                visible: text.length > 0
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.family: Paper.Theme.sans
                font.pixelSize: Paper.Theme.caption * window.fontScale
                color: Paper.Theme.muted
            }
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 7
            columnSpacing: 6 * Paper.Theme.unit * window.u
            Repeater {
                model: [ ["↶", "undo"], ["↷", "redo"], ["←", "left"], ["↑", "up"], ["↓", "down"], ["→", "right"], ["Sortir ↗", "exit"] ]
                delegate: PaperButton {
                    required property var modelData
                    Layout.fillWidth: true
                    leftPadding: 3 * Paper.Theme.unit * window.u
                    rightPadding: leftPadding
                    text: modelData[0]
                    enabled: modelData[1] === "undo" ? calc.canUndo : modelData[1] === "redo" ? calc.canRedo : true
                    onClicked: { calc.command(modelData[1]); formula.forceActiveFocus() }
                }
            }
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 6
            columnSpacing: 6 * Paper.Theme.unit * window.u
            rowSpacing: 6 * Paper.Theme.unit * window.u
            Repeater {
                model: window.advanced ? [ ["sin⁻¹", "asin"], ["cos⁻¹", "acos"], ["tan⁻¹", "atan"], ["eˣ", "exp"], ["|x|", "abs"], ["∛", "cbrt"] ] : [ ["sin", "sin"], ["cos", "cos"], ["tan", "tan"], ["ln", "ln"], ["log", "log"], ["π", "pi"] ]
                delegate: PaperButton {
                    required property var modelData
                    text: modelData[0]
                    Layout.fillWidth: true
                    onClicked: { calc.command(modelData[1]); formula.forceActiveFocus() }
                }
            }
            Repeater {
                model: [ ["7", "7"], ["8", "8"], ["9", "9"], ["÷", "/"], ["a⁄b", "fraction"], ["⌫", "backspace"],
                    ["4", "4"], ["5", "5"], ["6", "6"], ["×", "*"], ["xⁿ", "power"], ["Effacer", "clear"],
                    ["1", "1"], ["2", "2"], ["3", "3"], ["−", "-"], ["√", "sqrt"], ["(", "("],
                    ["0", "0"], [",", "."], ["Ans", "Ans"], ["+", "+"], [")", ")"], ["=", "="] ]
                delegate: PaperButton {
                    required property var modelData
                    Layout.fillWidth: true
                    font.pixelSize: (modelData[1] === "clear" ? Paper.Theme.body : 20 * Paper.Theme.unit) * window.fontScale
                    primary: modelData[1] === "="
                    text: modelData[0]
                    enabled: modelData[1] !== "=" || !calc.busy
                    onClicked: { calc.command(modelData[1]); formula.forceActiveFocus() }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6 * Paper.Theme.unit * window.u
            PaperButton { text: window.advanced ? "Fonctions 2/2" : "Fonctions 1/2"; Layout.fillWidth: true; Layout.preferredWidth: 140 * Paper.Theme.unit * window.u; onClicked: { window.advanced = !window.advanced; formula.forceActiveFocus() } }
            PaperButton { text: "e"; Layout.fillWidth: true; Layout.preferredWidth: 48 * Paper.Theme.unit * window.u; onClicked: { calc.command("e"); formula.forceActiveFocus() } }
            PaperButton { text: "MS"; Layout.fillWidth: true; Layout.preferredWidth: 48 * Paper.Theme.unit * window.u; onClicked: { calc.command("MS"); formula.forceActiveFocus() } }
            PaperButton { text: "MR"; Layout.fillWidth: true; Layout.preferredWidth: 48 * Paper.Theme.unit * window.u; onClicked: { calc.command("M"); formula.forceActiveFocus() } }
            PaperButton { text: "MC"; Layout.fillWidth: true; Layout.preferredWidth: 48 * Paper.Theme.unit * window.u; enabled: calc.hasMemory; onClicked: { calc.command("MC"); formula.forceActiveFocus() } }
        }
    }
    SurfaceSheet {
        id: historyDrawer
        title: "Historique"
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24 * Paper.Theme.unit * window.u
            spacing: 12 * Paper.Theme.unit * window.u
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: calc.history
                clip: true
                spacing: 0
                ScrollBar.vertical: Paper.ScrollBar {}
                delegate: Rectangle {
                    id: entry
                    required property var modelData
                    width: ListView.view.width
                    height: historyContent.implicitHeight + 28 * Paper.Theme.unit * window.u
                    color: historyMouse.pressed ? "#f3f3f3" : "white"
                    Column {
                        id: historyContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 14 * Paper.Theme.unit * window.u
                        anchors.bottomMargin: anchors.topMargin
                        spacing: 8 * Paper.Theme.unit * window.u
                        Text { width: parent.width; text: entry.modelData.expression; elide: Text.ElideRight; font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.body * window.fontScale }
                        Text { width: parent.width; text: entry.modelData.result; elide: Text.ElideRight; font.family: Paper.Theme.sans; font.pixelSize: 18 * Paper.Theme.unit * window.fontScale }
                        Text { width: parent.width; text: entry.modelData.date + " · " + entry.modelData.mode; elide: Text.ElideRight; color: Paper.Theme.muted; font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.caption * window.fontScale }
                    }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: Paper.Theme.line; color: Paper.Theme.divider }
                    MouseArea { id: historyMouse; anchors.fill: parent; onClicked: { calc.loadHistory(entry.modelData.id); historyDrawer.close(); formula.forceActiveFocus() } }
                }
                Text { anchors.centerIn: parent; visible: parent.count === 0; text: "Aucun calcul enregistré."; font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.body * window.fontScale }
            }
        }
    }
    SurfaceSheet {
        id: help
        objectName: "helpDialog"
        title: "Mode d’emploi"
        contentItem: Flickable {
            id: helpScroll
            implicitWidth: 0
            implicitHeight: 0
            clip: true
            contentWidth: width
            contentHeight: helpText.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: Paper.ScrollBar {}
            Text {
            id: helpText
            width: helpScroll.width
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            font.family: Paper.Theme.sans
            font.pixelSize: Paper.Theme.body
            text: "a/b place le nombre précédent au numérateur. xⁿ le place en base.\n\nTouchez une case, un chiffre ou utilisez les flèches. ↑ et ↓ passent entre numérateur et dénominateur ou base et exposant. Sortir ↗ quitte la structure courante.\n\n⌫ efface un chiffre ; une case vide se retire en conservant le contenu voisin. ↶ et ↷ annulent ou rétablissent une édition.\n\n= calcule. Une fraction exacte reste exacte. ≈ indique une approximation ; DEG/RAD s’applique aux fonctions trigonométriques.\n\nMS mémorise le dernier résultat affiché, MR insère M et MC efface la mémoire. Ans désigne le dernier résultat calculé.\n\nTout est local. Historique : 200 calculs. Les exports LaTeX se trouvent dans le dossier exports des données de reCalc."
            }
        }
    }
    }
}

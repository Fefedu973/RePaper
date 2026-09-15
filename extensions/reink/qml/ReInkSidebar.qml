import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "qrc:/paper" as Paper

// The host owns the current page. This component only supplies its palette.
Pane {
    id: panel
    objectName: "editorSidebar"
    required property var editor
    property string mode: "reink"
    property bool keyboardRequested: false
    property bool inspectorOnly: false
    property bool discardingPropertyInputs: false
    property string configuringStencil: ""
    property bool editingStencilSelection: false
    property bool awaitingStencilAccept: false
    property var submittedStencilParameters: ({})
    // Hidden, retained palettes must not keep reevaluating the native page map.
    readonly property var pageState: visible ? (editor.state || ({})) : ({})
    property bool componentReady: false
    property bool catalogueRequested: false
    property var stencilCatalogue: []
    property string stencilQuery: ""
    readonly property var filteredStencils: {
        const query = stencilQuery.toLocaleLowerCase()
        return query.length ? stencilCatalogue.filter(function(symbol) {
            return symbol.name.toLocaleLowerCase().indexOf(query) >= 0
        }) : stencilCatalogue
    }
    readonly property bool nativeEditor: typeof editor.backend === "string" && editor.backend.indexOf("xochitl") === 0
    readonly property bool propertiesDialog: inspectorOnly && nativeEditor
    readonly property var drawingWidths: [2, 3, 5, 8].filter(function(value) { return value <= panel.state("maximumStrokeWidth", 30) })
    signal toolChosen()
    signal propertiesAcceptRequested()
    signal propertiesCancelRequested()
    implicitWidth: 360 * Paper.Theme.unit
    implicitHeight: 500 * Paper.Theme.unit
    padding: 12 * Paper.Theme.unit
    font.family: Paper.Theme.sans
    font.pixelSize: Paper.Theme.body
    Component.onCompleted: {
        componentReady = true
        if (nativeEditor) Paper.Theme.unit = 2
        ensureStencilCatalogue()
    }
    background: Rectangle { color: "white" }
    palette.window: "white"
    palette.button: "white"
    palette.highlight: "#202020"
    palette.highlightedText: "white"

    function state(key, fallback) { return pageState[key] === undefined ? fallback : pageState[key] }
    function parameter(key, fallback) {
        const value = Number(state(key, fallback))
        return isFinite(value) ? Math.round(value) : fallback
    }
    function supports(tool) {
        return editor.available && state("supportedTools", ["pen", "line", "arrow", "wire", "rectangle", "ellipse", "select"]).indexOf(tool) >= 0
    }
    function choose(tool) { if (editor.chooseTool(tool)) toolChosen() }
    function chooseStencil(symbolId, configurable) {
        // Keep the complete action in the retained panel, independently of
        // the virtual list delegate that initiated it.
        if (configurable) configureStencil(symbolId, false)
        else if (editor.beginStencil(symbolId)) toolChosen()
    }
    function commitPendingPropertyInputs() {
        propertyInputFocus.forceActiveFocus()
        if (configuringStencil.length && editingStencilSelection) {
            if (!stencilConfiguration.apply()) return false
            submittedStencilParameters = Object.assign({}, stencilConfiguration.values)
            if (propertiesDialog && !state("propertiesSessionCanAccept", false)) {
                awaitingStencilAccept = true
                return false
            }
        }
        return true
    }
    function finishStencilAccept() {
        if (!awaitingStencilAccept || !state("propertiesSessionCanAccept", false)) return
        awaitingStencilAccept = false
        if (state("selectedStencilId", "") !== configuringStencil) return
        const actual = state("selectedStencilParameters", {})
        for (const key in submittedStencilParameters)
            if (actual[key] !== submittedStencilParameters[key]) return
        propertiesAcceptRequested()
    }
    onPageStateChanged: Qt.callLater(finishStencilAccept)
    function discardPendingPropertyInputs() {
        awaitingStencilAccept = false
        discardingPropertyInputs = true
        try {
            selectionColor.discardPendingInput()
            propertyInputFocus.forceActiveFocus()
            // A canceled session may remain open if native rollback is rejected.
            // Restore displayed values without breaking their state bindings.
            shapeWidth.value = Qt.binding(function() { return panel.parameter("selectedShapeWidth", 1) })
            shapeHeight.value = Qt.binding(function() { return panel.parameter("selectedShapeHeight", 1) })
            cornerRadius.value = Qt.binding(function() { return panel.parameter("selectedCornerRadius", 0) })
            wireBend.value = Qt.binding(function() { return panel.parameter("selectedWireBend", 0) })
            configuringStencil = ""
        } finally {
            discardingPropertyInputs = false
        }
    }
    function requestKeyboard(control) {
        if (control.activeFocus && control.enabled) {
            keyboardRequested = true
            Qt.inputMethod.show()
        }
    }
    onVisibleChanged: {
        if (!visible && keyboardRequested) { Qt.inputMethod.hide(); keyboardRequested = false }
        if (visible) ensureStencilCatalogue()
        else if (componentReady && propertiesDialog) discardPendingPropertyInputs()
    }
    onModeChanged: {
        awaitingStencilAccept = false
        configuringStencil = ""
        resetPaletteScroll()
        ensureStencilCatalogue()
        // Loading the native inspector follows a selection notification. Do
        // not reselect the active tool and recursively refresh that binding.
        if (mode === "properties" && editor && editor.available && state("hasSelection", false)
                && state("tool", "select") !== "select")
            editor.chooseTool("select")
    }

    // Pane is a focus scope: focusing it can leave its editor focused. A leaf
    // item gives footer actions a real focus transfer without another control.
    Item { id: propertyInputFocus }
    function resetPaletteScroll() {
        Qt.callLater(function() {
            if (paletteScroll.contentItem) paletteScroll.contentItem.contentY = 0
            if (stencilCatalogueLoader.item) stencilCatalogueLoader.item.positionViewAtBeginning()
        })
    }
    function ensureStencilCatalogue() {
        if (!componentReady || !visible || mode !== "restencil" || catalogueRequested) return
        // Keep a single catalogue value and a single list instance for this
        // palette's lifetime. Native QVariant geometry is fetched only here.
        stencilCatalogue = editor.stencils || []
        catalogueRequested = true
    }
    onConfiguringStencilChanged: resetPaletteScroll()
    function configureStencil(id, editing) {
        awaitingStencilAccept = false
        editingStencilSelection = editing
        stencilConfiguration.values = editing
            ? Object.assign({}, state("selectedStencilParameters", {})) : editor.stencilDefaults(id)
        configuringStencil = id
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10 * Paper.Theme.unit
        RowLayout {
            id: tabs
            objectName: "paletteTabs"
            visible: !panel.inspectorOnly
            Layout.fillWidth: true
            spacing: 0
            property int currentIndex: panel.mode === "restencil" ? 1 : panel.mode === "properties" ? 2 : 0
            Repeater {
                model: ["Dessin", "Symboles", "Propriétés"]
                Paper.Button {
                    Layout.fillWidth: true
                    text: modelData
                    quiet: true; checkable: true; checked: tabs.currentIndex === index
                    onClicked: panel.mode = index === 1 ? "restencil" : index === 2 ? "properties" : "reink"
                }
            }
        }
        Label {
            visible: text.length > 0 && (!panel.editor.available || Paper.Theme.actionable(text))
            Layout.fillWidth: true
            text: panel.editor.status
            wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.caption; color: "#555"
        }
        ScrollView {
            id: paletteScroll
            visible: panel.mode !== "restencil" || panel.configuringStencil.length > 0
            Layout.fillWidth: true; Layout.fillHeight: true
            rightPadding: 8 * Paper.Theme.unit
            contentWidth: availableWidth; clip: true
            contentHeight: scrollContent.implicitHeight
            ScrollBar.vertical: Paper.ScrollBar {
                parent: paletteScroll
                x: paletteScroll.width - width
                y: paletteScroll.topPadding
                height: paletteScroll.availableHeight
            }
            ColumnLayout {
                id: scrollContent
                width: paletteScroll.availableWidth
                spacing: 12 * Paper.Theme.unit
                StencilConfiguration {
                    id: stencilConfiguration
                    objectName: "stencilConfiguration"
                    Layout.fillWidth: true
                    visible: panel.configuringStencil.length > 0
                    editor: panel.editor
                    symbolId: panel.configuringStencil
                    editingSelection: panel.editingStencilSelection
                    canEdit: !panel.awaitingStencilAccept && (editingSelection
                        ? panel.state("selectionCanConfigureStencil", false)
                          && panel.state("selectedStencilId", "") === symbolId
                          && (!panel.propertiesDialog || panel.state("propertiesSessionCanEdit", false))
                        : panel.state("canPlaceStencils", true))
                    onKeyboardRequested: panel.keyboardRequested = true
                    onCanceled: { panel.awaitingStencilAccept = false; panel.configuringStencil = "" }
                    onAccepted: {
                        const placing = !panel.editingStencilSelection
                        panel.configuringStencil = ""
                        if (placing) panel.toolChosen()
                    }
                }
                ColumnLayout {
                    visible: panel.mode === "reink" && !stencilConfiguration.visible
                    Layout.fillWidth: true; spacing: 12 * Paper.Theme.unit
                    GridLayout {
                        columns: 2; Layout.fillWidth: true; columnSpacing: 8 * Paper.Theme.unit; rowSpacing: 8 * Paper.Theme.unit
                        Repeater {
                            model: [{id:"pen", name:"Trait libre"}, {id:"line", name:"Ligne"}, {id:"arrow", name:"Flèche"},
                                {id:"wire", name:"Fil orthogonal"}, {id:"rectangle", name:"Rectangle"},
                                {id:"ellipse", name:"Ellipse"}, {id:"select", name:"Sélection"}]
                            Paper.Button {
                                objectName: "editorTool_" + modelData.id
                                text: modelData.name; Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                                enabled: panel.supports(modelData.id)
                                checkable: true; checked: panel.state("tool", "select") === modelData.id
                                onClicked: panel.choose(modelData.id)
                            }
                        }
                    }
                    Label { text: "Trait"; font.bold: true }
                    StrokeColorPicker {
                        objectName: "editorStrokeColor"; Layout.fillWidth: true
                        enabled: panel.editor.available && panel.state("tool", "select") !== "select"
                        currentColor: panel.state("lineColor", "#000000")
                        onColorChosen: color => panel.editor.setStrokeColor(color)
                        onKeyboardRequested: panel.keyboardRequested = true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Paper.ComboBox {
                            objectName: "editorStrokeStyle"
                            Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                            enabled: panel.editor.available && panel.state("tool", "select") !== "select"
                            model: ["Continu", "Tirets", "Pointillés"]
                            currentIndex: ["solid", "dashed", "dotted"].indexOf(panel.state("lineStyle", "solid"))
                            onActivated: function(index) { panel.editor.setStrokeStyle(["solid", "dashed", "dotted"][index]) }
                        }
                        Paper.ComboBox {
                            objectName: "editorStrokeWidth"
                            Layout.preferredWidth: 120 * Paper.Theme.unit; implicitHeight: Paper.Theme.control
                            enabled: panel.editor.available && panel.state("tool", "select") !== "select"
                            model: panel.drawingWidths.map(function(value) { return ({2:"Fin", 3:"Moyen", 5:"Épais", 8:"Fort"})[value] + " · " + value })
                            currentIndex: panel.drawingWidths.indexOf(panel.state("lineWidth", 3))
                            displayText: currentIndex < 0 ? Number(panel.state("lineWidth", 3)).toFixed(1) : currentText
                            onActivated: function(index) { panel.editor.setStrokeWidth(panel.drawingWidths[index]) }
                        }
                    }

                }
                ColumnLayout {
                    visible: panel.mode === "properties" && !stencilConfiguration.visible
                    enabled: !panel.propertiesDialog || panel.state("propertiesSessionCanEdit", false)
                    Layout.fillWidth: true; spacing: 10 * Paper.Theme.unit
                    Label {
                        Layout.fillWidth: true; font.bold: true; wrapMode: Text.WordWrap
                        text: panel.state("hasSelection", false) ? "Modifier la sélection" : "Sélectionnez un élément sur la page."
                    }
                    Paper.Button {
                        objectName: "configureSelectedStencil"
                        visible: panel.state("selectionCanConfigureStencil", false)
                        Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                        text: "Configurer"
                        onClicked: panel.configureStencil(panel.state("selectedStencilId", ""), true)
                    }
                    Paper.Button {
                        visible: !panel.state("hasSelection", false)
                        text: "Outil de sélection"; Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                        enabled: panel.supports("select"); onClicked: panel.choose("select")
                    }
                    Label { text: "Couleur du trait"; visible: panel.state("hasSelection", false); font.bold: true }
                    ColumnLayout {
                        visible: panel.state("selectionSupportsVoltage", false); Layout.fillWidth: true
                        Paper.CheckBox {
                            objectName: "selectionVoltageArrow"; text: "Flèche de tension"
                            checked: panel.state("selectedVoltageArrow", false)
                            onClicked: panel.editor.setVoltageArrow(!panel.state("selectedVoltageArrow", false))
                        }
                        RowLayout {
                            visible: panel.state("selectedVoltageArrow", false); Layout.fillWidth: true
                            Paper.Button {
                                objectName: "selectionVoltageReverse"; text: "Inverser"; Layout.fillWidth: true
                                onClicked: panel.editor.setVoltageReversed(!panel.state("selectedVoltageReversed", false))
                            }
                            Paper.Button {
                                objectName: "selectionVoltageSide"; text: "Changer de côté"; Layout.fillWidth: true
                                onClicked: panel.editor.setVoltageOtherSide(!panel.state("selectedVoltageOtherSide", false))
                            }
                        }
                    }
                    StrokeColorPicker {
                        id: selectionColor
                        objectName: "selectionStrokeColor"; Layout.fillWidth: true
                        visible: panel.state("hasSelection", false)
                        enabled: panel.editor.available && panel.state("selectionCanChangeColor", false)
                        currentColor: panel.state("selectedLineColor", "#000000")
                        onColorChosen: color => { if (!panel.discardingPropertyInputs) panel.editor.setStrokeColor(color) }
                        onKeyboardRequested: panel.keyboardRequested = true
                    }

                    RowLayout {
                        visible: !panel.nativeEditor || panel.state("selectionCanChangeStyle", false) || panel.state("selectionCanChangeWidth", false)
                        Layout.fillWidth: true
                        Paper.ComboBox {
                            objectName: "selectionStyle"
                            Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                            enabled: panel.editor.available && panel.state("selectionCanChangeStyle", false)
                            model: ["Continu", "Tirets", "Pointillés"]
                            currentIndex: ["solid", "dashed", "dotted"].indexOf(panel.state("selectedLineStyle", "solid"))
                            onActivated: function(index) { if (!panel.discardingPropertyInputs) panel.editor.setStrokeStyle(["solid", "dashed", "dotted"][index]) }
                        }
                        Paper.ComboBox {
                            objectName: "selectionWidth"
                            Layout.preferredWidth: 96 * Paper.Theme.unit; implicitHeight: Paper.Theme.control
                            enabled: panel.editor.available && panel.state("selectionCanChangeWidth", panel.state("selectionCanChangeStyle", false))
                            model: ["1", "2", "3", "5", "8", "12"]
                            currentIndex: [1,2,3,5,8,12].indexOf(panel.state("selectedLineWidth", 3))
                            displayText: Number(panel.state("selectedLineWidth", 3)).toFixed(1)
                            onActivated: function(index) { if (!panel.discardingPropertyInputs) panel.editor.setStrokeWidth([1,2,3,5,8,12][index]) }
                        }
                    }
                    Paper.ComboBox {
                        objectName: "selectionDirection"
                        visible: panel.state("selectionIsArrow", false)
                        Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                        enabled: panel.editor.available && panel.state("selectionIsArrow", false) && panel.state("selectionCanChangeStyle", false)
                        model: ["Fin →", "Début ←", "Deux bouts ↔", "Aucune"]
                        currentIndex: Math.max(0, ["end", "start", "both", "none"].indexOf(panel.state("selectedArrowDirection", "end")))
                        onActivated: function(index) { if (!panel.discardingPropertyInputs) panel.editor.setArrowDirection(["end", "start", "both", "none"][index]) }
                    }
                    GridLayout {
                        visible: panel.state("selectionCanResize", false)
                        enabled: panel.editor.available && panel.state("selectionCanTransform", true); columns: 2; Layout.fillWidth: true; columnSpacing: 8 * Paper.Theme.unit; rowSpacing: 8 * Paper.Theme.unit
                        Label { text: "Largeur" }
                        EditorSpinBox {
                            id: shapeWidth
                            objectName: "editorShapeWidth"; Layout.fillWidth: true
                            from: 1; to: 10000; editable: true; value: panel.parameter("selectedShapeWidth", 1)
                            onValueModified: if (!panel.discardingPropertyInputs) panel.editor.resize(value, panel.parameter("selectedShapeHeight", 1))
                            onActiveFocusChanged: panel.requestKeyboard(shapeWidth)
                        }
                        Label { text: "Hauteur" }
                        EditorSpinBox {
                            id: shapeHeight
                            objectName: "editorShapeHeight"; Layout.fillWidth: true
                            from: 1; to: 10000; editable: true; value: panel.parameter("selectedShapeHeight", 1)
                            onValueModified: if (!panel.discardingPropertyInputs) panel.editor.resize(panel.parameter("selectedShapeWidth", 1), value)
                            onActiveFocusChanged: panel.requestKeyboard(shapeHeight)
                        }
                        Label { text: "Arrondi"; visible: panel.state("selectionKind", "") === "rectangle" }
                        EditorSpinBox {
                            id: cornerRadius
                            objectName: "editorCornerRadius"; Layout.fillWidth: true
                            visible: panel.state("selectionKind", "") === "rectangle"
                            from: 0; to: Math.max(0, Math.floor(Math.min(panel.parameter("selectedShapeWidth", 1), panel.parameter("selectedShapeHeight", 1))/2))
                            editable: true; value: panel.parameter("selectedCornerRadius", 0)
                            onValueModified: if (!panel.discardingPropertyInputs) panel.editor.setCornerRadius(value)
                            onActiveFocusChanged: panel.requestKeyboard(cornerRadius)
                        }
                    }
                    RowLayout {
                        visible: panel.state("selectionIsWire", false)
                        enabled: panel.editor.available && panel.state("selectionCanTransform", true); Layout.fillWidth: true
                        Paper.Button {
                            text: "Routage automatique"; Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                            onClicked: if (!panel.discardingPropertyInputs) panel.editor.setWireBend(0)
                        }
                        EditorSpinBox {
                            id: wireBend
                            visible: false
                            objectName: "editorWireBend"; Layout.preferredWidth: 160 * Paper.Theme.unit
                            from: -2000; to: 2000; stepSize: 10; editable: true; value: panel.parameter("selectedWireBend", 0)
                            onValueModified: if (!panel.discardingPropertyInputs) panel.editor.setWireBend(value)
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                            onActiveFocusChanged: panel.requestKeyboard(wireBend)
                        }
                    }
                    GridLayout {
                        columns: 3; Layout.fillWidth: true; columnSpacing: 8 * Paper.Theme.unit; rowSpacing: 8 * Paper.Theme.unit
                        enabled: panel.editor.available && panel.state("hasSelection", false)
                        Paper.Button { text: "Réduire"; enabled: panel.state("selectionCanTransform", true); Layout.fillWidth: true; implicitHeight: Paper.Theme.control; onClicked: if (!panel.discardingPropertyInputs) panel.editor.scale(0.8) }
                        Paper.Button { text: "Agrandir"; enabled: panel.state("selectionCanTransform", true); Layout.fillWidth: true; implicitHeight: Paper.Theme.control; onClicked: if (!panel.discardingPropertyInputs) panel.editor.scale(1.25) }
                        Paper.Button { text: "90°"; enabled: panel.state("selectionCanTransform", true); Layout.fillWidth: true; implicitHeight: Paper.Theme.control; onClicked: if (!panel.discardingPropertyInputs) panel.editor.rotate() }
                        Paper.Button { text: "Dupliquer"; enabled: panel.state("selectionCanDuplicate", true); Layout.columnSpan: 2; Layout.fillWidth: true; implicitHeight: Paper.Theme.control; onClicked: if (!panel.discardingPropertyInputs) panel.editor.duplicate() }
                        Paper.Button { text: "Supprimer"; enabled: panel.state("selectionCanRemove", true); Layout.fillWidth: true; implicitHeight: Paper.Theme.control; onClicked: if (!panel.discardingPropertyInputs) panel.editor.remove() }
                    }
                }
            }
        }
        Loader {
            id: stencilCatalogueLoader
            objectName: "editorStencilCatalogueLoader"
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: panel.mode === "restencil" && !panel.configuringStencil.length
            active: panel.catalogueRequested
            sourceComponent: Component {
                ListView {
                    id: stencilList
                    objectName: "editorStencilList"
                    clip: true
                    model: panel.filteredStencils
                    spacing: 5
                    boundsBehavior: Flickable.StopAtBounds
                    cacheBuffer: 0
                    reuseItems: true
                    ScrollBar.vertical: Paper.ScrollBar {}
                    header: Item {
                        width: stencilList.width - 8 * Paper.Theme.unit
                        implicitHeight: stencilHeader.implicitHeight + 5
                        ColumnLayout {
                            id: stencilHeader
                            width: parent.width
                            spacing: 5
                            Label { text: "Couleur"; font.bold: true }
                            StrokeColorPicker {
                                objectName: "editorSymbolColor"; Layout.fillWidth: true
                                enabled: panel.editor.available && panel.state("tool", "select") !== "select"
                                currentColor: panel.state("lineColor", "#000000")
                                onColorChosen: color => panel.editor.setStrokeColor(color)
                                onKeyboardRequested: panel.keyboardRequested = true
                            }
                            Paper.TextField {
                                id: search; objectName: "editorStencilSearch"
                                Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                                placeholderText: "Rechercher un symbole ou un graphique"
                                text: panel.stencilQuery
                                onTextChanged: {
                                    panel.stencilQuery = text
                                    stencilList.positionViewAtBeginning()
                                }
                                onActiveFocusChanged: panel.requestKeyboard(search)
                            }
                        }
                    }
                    delegate: Paper.Button {
                        id: stencilButton
                        required property var modelData
                        objectName: "editorStencil_" + modelData.id
                        width: stencilList.width - 8 * Paper.Theme.unit
                        implicitHeight: Math.max(Paper.Theme.control, implicitContentHeight + topPadding + bottomPadding)
                        text: modelData.name
                        enabled: panel.editor.available && panel.state("canPlaceStencils", true)
                        contentItem: RowLayout {
                            spacing: 12 * Paper.Theme.unit
                            StencilPreview {
                                Layout.preferredWidth: 54 * Paper.Theme.unit
                                Layout.preferredHeight: 36 * Paper.Theme.unit
                                drawing: stencilButton.modelData
                                thumbnail: true
                                thumbnailPadding: 8
                                minimumThumbnailWidth: 1.2
                                visible: (stencilButton.modelData.strokes || []).length > 0
                                opacity: stencilButton.enabled ? 1 : 0.4
                            }
                            Label {
                                Layout.fillWidth: true
                                text: stencilButton.text; font: stencilButton.font; color: "black"
                                opacity: stencilButton.enabled ? 1 : 0.4
                                verticalAlignment: Text.AlignVCenter; wrapMode: Text.WordWrap
                            }
                        }
                        onClicked: panel.chooseStencil(modelData.id, modelData.configurable === true)
                    }
                }
            }
        }
        RowLayout {
            visible: !panel.propertiesDialog && !panel.configuringStencil.length
            Layout.fillWidth: true
            Paper.Button { text: "Annuler"; Layout.fillWidth: true; implicitHeight: Paper.Theme.control; enabled: panel.editor.available && panel.state("canUndo", false); onClicked: panel.editor.undo() }
            Paper.Button { text: "Rétablir"; Layout.fillWidth: true; implicitHeight: Paper.Theme.control; enabled: panel.editor.available && panel.state("canRedo", false); onClicked: panel.editor.redo() }
        }
        RowLayout {
            visible: panel.propertiesDialog
            Layout.fillWidth: true
            Paper.Button {
                objectName: "cancelNativeProperties"; text: "Annuler"
                Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                focusPolicy: Qt.NoFocus
                enabled: panel.editor.available && panel.state("propertiesSessionCanCancel", false)
                onClicked: { panel.discardPendingPropertyInputs(); panel.propertiesCancelRequested() }
            }
            Paper.Button {
                objectName: "acceptNativeProperties"; text: "Valider"
                Layout.fillWidth: true; implicitHeight: Paper.Theme.control
                focusPolicy: Qt.NoFocus
                enabled: panel.editor.available && !panel.awaitingStencilAccept && panel.state("propertiesSessionCanAccept", false)
                onClicked: { if (panel.commitPendingPropertyInputs()) panel.propertiesAcceptRequested() }
            }
        }
    }
}


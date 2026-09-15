import QtQuick 2.15
import QtQuick.Controls 2.15
import QtTest 1.15
import "../../qml"

// Palette contract only; not a test of native allocation or tablet insertion.
TestCase {
    id: testCase
    name: "NativeToolPaletteUI"
    width: 720; height: 1000
    when: windowShown
    visible: true
    property int cancelRequests: 0
    property int acceptRequests: 0
    property int toolChosenRequests: 0
    property var sessionEvents: []
    QtObject {
        id: editor
        property bool available: true
        property string backend: "xochitl-native"
        property string status: ""
        property string chosen: ""
        property var stencils: [
            { id: "resistor-iec", name: "Résistance IEC" },
            { id: "table", name: "Tableau", configurable: true },
            { id: "graph", name: "Graphique", configurable: true },
            { id: "bode", name: "Diagramme de Bode", configurable: true }
        ]
        property var state: ({})
        property var propertyWrites: []
        property var undoState: ({})
        property var redoState: ({})
        property int undoCalls: 0
        property int redoCalls: 0
        property bool acknowledgeProperties: true
        property bool busyAfterPropertyWrite: false
        property var configuredStarts: []
        property int stencilWriteAttempts: 0
        property bool rejectStencilWrite: false
        property bool previewProductionGeometry: false
        function chooseTool(tool) { if (!available) return false; chosen = tool; return true }
        function beginStencil(symbol) { if (!available) return false; chosen = symbol; return true }
        // This mock supplies the public parameter contract. Geometry and native
        // insertion are covered by the C++ suites, not reproduced here.
        function stencilSchema(symbol) {
            if (previewProductionGeometry) return nativePaletteGeometry.schema(symbol)
            function number(key, value, min, max, integer) {
                return {key: key, label: key, type: integer ? "integer" : "number",
                    default: value, min: min, max: max, step: 1}
            }
            function choice(key, value, options) {
                return {key: key, label: key, type: "choice", default: value,
                    options: options.map(function(option) { return {value: option, label: option} })}
            }
            function flag(key, value) { return {key: key, label: key, type: "bool", default: value} }
            let fields = []
            if (symbol === "table") fields = [number("rows", 4, 1, 30, true), number("columns", 4, 1, 30, true)]
            if (symbol === "graph") fields = [flag("grid", true),
                choice("curveType", "none", ["none", "exponential", "second-order", "sine"]),
                number("gain", 1, -1e6, 1e6), number("tau", 1, 1e-6, 1e6),
                number("initialValue", 0, -1e6, 1e6), number("omega0", 1, 1e-6, 1e6),
                number("damping", 0.5, 0, 100), number("tMax", 10, 1e-6, 1e6),
                number("yMin", -0.2, -1e6, 1e6), number("yMax", 1.4, -1e6, 1e6),
                number("amplitude", 1, 0, 1e6), number("offset", 0, -1e6, 1e6),
                number("frequencyHz", 1, 1e-6, 1e6), number("phaseDegrees", 0, -360, 360)]
            if (symbol === "bode") fields = [choice("mode", "gain", ["gain", "phase"]), flag("curve", false),
                number("frequencyMin", 0.1, 1e-6, 1e6), number("frequencyMax", 100, 1e-6, 1e6),
                number("gain", 1, 1e-6, 1e6), number("omega0", 2 * Math.PI, 1e-6, 1e6),
                number("damping", 0.5, 1e-6, 100), number("yMin", -60, -1e6, 1e6),
                number("yMax", 20, -1e6, 1e6)]
            return fields.length ? fields.concat([flag("opaqueBackground", true)]) : []
        }
        function stencilDefaults(symbol) {
            const result = {}
            stencilSchema(symbol).forEach(function(field) { result[field.key] = field.default })
            return result
        }
        function stencilPreview(symbol, parameters) {
            if (previewProductionGeometry) return nativePaletteGeometry.preview(symbol, parameters)
            const values = Object.assign({}, stencilDefaults(symbol), parameters)
            const schema = stencilSchema(symbol)
            for (let i = 0; i < schema.length; ++i) {
                const field = schema[i], value = values[field.key]
                if ((field.type === "number" || field.type === "integer")
                        && (typeof value !== "number" || !isFinite(value)
                            || value < field.min || value > field.max
                            || (field.type === "integer" && Math.floor(value) !== value)))
                    return {valid: false, error: "Nombre invalide : " + field.key}
            }
            if (symbol !== "table" && values.yMax <= values.yMin)
                return {valid: false, error: "Intervalle invalide"}
            if (symbol === "bode" && values.frequencyMax <= values.frequencyMin)
                return {valid: false, error: "Fréquences invalides"}
            const strokes = []
            const count = symbol === "table" ? values.rows + values.columns + 2 : 1
            for (let i = 0; i < count; ++i)
                strokes.push({points: [{x: 0, y: i * 2}, {x: 120, y: i * 2}], width: 1})
            return {valid: true, width: 120, height: 80, strokes: strokes, parameters: values}
        }
        function beginConfiguredStencil(symbol, parameters) {
            if (!available || !stencilPreview(symbol, parameters).valid) return false
            configuredStarts = configuredStarts.concat([{symbol: symbol, values: Object.assign({}, parameters)}])
            return true
        }
        function setStencilParameters(parameters) {
            ++stencilWriteAttempts
            if (rejectStencilWrite || !available || !stencilPreview(state.selectedStencilId, parameters).valid) return false
            const values = Object.assign({}, parameters)
            return writeProperty("stencil", [values], {selectedStencilParameters: values})
        }
        function writeProperty(kind, values, changes) {
            propertyWrites = propertyWrites.concat([{kind: kind, values: values}])
            testCase.sessionEvents = testCase.sessionEvents.concat(["write:" + kind])
            if (acknowledgeProperties) state = Object.assign({}, state, changes)
            if (busyAfterPropertyWrite) state = Object.assign({}, state, {
                propertiesSessionCanEdit: false, propertiesSessionCanCancel: false, propertiesSessionCanAccept: false
            })
            return true
        }
        function setStrokeColor(color) {
            chosen = color
            return writeProperty("color", [color], state.tool === "select"
                ? {selectedLineColor: color} : {lineColor: color})
        }
        function setStrokeStyle(style) { return writeProperty("style", [style], {selectedLineStyle: style}) }
        function setStrokeWidth(width) { return writeProperty("width", [width], {selectedLineWidth: width}) }
        function setArrowDirection(direction) { return writeProperty("direction", [direction], {selectedArrowDirection: direction}) }
        function resize(width, height) { return writeProperty("resize", [width, height], {selectedShapeWidth: width, selectedShapeHeight: height}) }
        function setCornerRadius(radius) { return writeProperty("radius", [radius], {selectedCornerRadius: radius}) }
        function setWireBend(bend) { return writeProperty("bend", [bend], {selectedWireBend: bend}) }
        function undo() { ++undoCalls; state = Object.assign({}, undoState); return true }
        function redo() { ++redoCalls; state = Object.assign({}, redoState); return true }
    }
    ReInkSidebar {
        id: panel; anchors.fill: parent; editor: editor
        onPropertiesCancelRequested: { ++testCase.cancelRequests; testCase.sessionEvents = testCase.sessionEvents.concat(["cancel"]) }
        onPropertiesAcceptRequested: { ++testCase.acceptRequests; testCase.sessionEvents = testCase.sessionEvents.concat(["accept"]) }
        onToolChosen: ++testCase.toolChosenRequests
    }
    function control(name, root) {
        function find(item) {
            if (item.objectName === name) return item
            const children = item.children || []
            for (let i = 0; i < children.length; ++i) {
                const result = find(children[i])
                if (result) return result
            }
            return null
        }
        return find(root || panel)
    }
    function button(text) {
        function find(item) {
            if (item.visible && item.text === text && item.clicked !== undefined) return item
            const children = item.children || []
            for (let i = 0; i < children.length; ++i) {
                const result = find(children[i])
                if (result) return result
            }
            return null
        }
        return find(panel)
    }
    function showProperties() {
        editor.state = Object.assign({}, editor.state, {
            tool: "select", hasSelection: true, selectionKind: "rectangle",
            selectionCanChangeColor: true, selectionCanChangeStyle: true,
            selectionCanTransform: true, selectionCanResize: true,
            selectedLineColor: "#000000", selectedLineStyle: "solid", selectedLineWidth: 3,
            selectedShapeWidth: 240, selectedShapeHeight: 160, selectedCornerRadius: 10,
            canUndo: true, canRedo: false
        })
        panel.mode = "properties"
        wait(20)
        editor.propertyWrites = []
        editor.undoState = Object.assign({}, editor.state, {canUndo: false, canRedo: true})
    }
    function typeInto(input, text) {
        input.forceActiveFocus()
        keyClick(Qt.Key_A, Qt.ControlModifier)
        if (!text.length) keyClick(Qt.Key_Backspace)
        for (let i = 0; i < text.length; ++i) keyClick(text.charAt(i))
    }
    function reveal(item) {
        for (let parent = item.parent; parent; parent = parent.parent) {
            if (parent.contentY !== undefined && parent.contentItem !== undefined) {
                const y = item.mapToItem(parent.contentItem, 0, 0).y
                parent.contentY = Math.max(0, Math.min(parent.contentHeight - parent.height,
                    y - Math.max(0, (parent.height - item.height) / 2)))
                break
            }
        }
        wait(20)
        verify(item.visible && item.enabled)
    }
    function clickRevealed(item) { reveal(item); mouseClick(item) }
    function configureNewStencil(symbol) {
        panel.mode = "restencil"
        wait(20)
        const list = findChild(panel, "editorStencilList")
        if (list) {
            for (let index = 0; index < panel.filteredStencils.length; ++index) {
                if (panel.filteredStencils[index].id === symbol) {
                    list.positionViewAtIndex(index, ListView.Contain)
                    wait(20)
                    break
                }
            }
        }
        clickRevealed(control("editorStencil_" + symbol))
        const configuration = control("stencilConfiguration")
        tryCompare(configuration, "symbolId", symbol)
        verify(configuration.visible)
        return configuration
    }
    function configureSelectedStencil(symbol, dialog) {
        showProperties()
        editor.state = Object.assign({}, editor.state, {
            selectionCanConfigureStencil: true, selectedStencilId: symbol,
            selectedStencilParameters: editor.stencilDefaults(symbol),
            propertiesSessionCanEdit: true, propertiesSessionCanCancel: true, propertiesSessionCanAccept: true
        })
        panel.inspectorOnly = dialog === true
        wait(20)
        clickRevealed(control("configureSelectedStencil"))
        const configuration = control("stencilConfiguration")
        tryCompare(configuration, "symbolId", symbol)
        verify(configuration.editingSelection)
        return configuration
    }
    function pendingPropertyCases() {
        return [
            {tag: "width", name: "editorShapeWidth", text: "300", original: "240", kind: "resize", values: [300, 160]},
            {tag: "height", name: "editorShapeHeight", text: "210", original: "160", kind: "resize", values: [240, 210]},
            {tag: "radius", name: "editorCornerRadius", text: "25", original: "10", kind: "radius", values: [25]},
            {tag: "wire bend", name: "editorWireBend", text: "75", original: "20", kind: "bend", values: [75]},
            {tag: "hex color", name: "strokeColorHex", text: "#136aca", original: "#000000", kind: "color", values: ["#136aca"]}
        ]
    }
    function showPropertiesDialog(data) {
        showProperties()
        editor.state = Object.assign({}, editor.state, {
            propertiesSessionCanEdit: true, propertiesSessionCanCancel: true, propertiesSessionCanAccept: true,
            selectionIsWire: data && data.kind === "bend", selectedWireBend: 20
        })
        panel.inspectorOnly = true
        wait(20)
        editor.busyAfterPropertyWrite = true
        const field = data.name === "strokeColorHex"
            ? control(data.name, control("selectionStrokeColor")) : control(data.name).contentItem
        verify(field.enabled)
        return field
    }
    function init() {
        if (panel.propertiesDialog) panel.discardPendingPropertyInputs()
        panel.inspectorOnly = false
        panel.mode = "reink"
        editor.available = true; editor.backend = "xochitl-native"; editor.status = ""; editor.chosen = ""
        editor.propertyWrites = []; editor.undoCalls = 0; editor.redoCalls = 0
        editor.undoState = {}; editor.redoState = {}; editor.acknowledgeProperties = true
        editor.busyAfterPropertyWrite = false
        editor.previewProductionGeometry = false
        editor.configuredStarts = []; editor.stencilWriteAttempts = 0; editor.rejectStencilWrite = false
        panel.configuringStencil = ""; panel.editingStencilSelection = false
        cancelRequests = 0; acceptRequests = 0; toolChosenRequests = 0; sessionEvents = []
        editor.state = {
            tool: "line", maximumStrokeWidth: 4,
            supportedTools: ["line", "arrow", "rectangle", "select"],
            hasSelection: false, selectionCanChangeStyle: false,
            selectionCanTransform: false, selectionCanDuplicate: false, selectionCanRemove: false,
            canUndo: false, canRedo: false
        }
        wait(30)
    }
    function test_toolsAreReadyWithoutPerNotebookActivation() {
        verify(panel.nativeEditor)
        compare(control("armNativeCreationTest"), null)
        const rectangle = control("editorTool_rectangle")
        verify(rectangle.visible && rectangle.enabled)
        mouseClick(rectangle); compare(editor.chosen, "rectangle")
        editor.state = Object.assign({}, editor.state, {pageId: "another-page"})
        wait(10); verify(rectangle.enabled)
        mouseClick(control("editorTool_arrow")); compare(editor.chosen, "arrow")
    }
    function test_symbolsAreAvailableWithoutSelection() {
        panel.mode = "restencil"; wait(20)
        const symbol = control("editorStencil_resistor-iec")
        verify(symbol.enabled); mouseClick(symbol)
        compare(editor.chosen, "resistor-iec")
    }
    function test_tableDraftUpdatesPreviewBeforePlacement() {
        const configuration = configureNewStencil("table")
        compare(configuration.preview.strokes.length, 10)
        compare(editor.configuredStarts.length, 0)
        compare(editor.chosen, "")
        const rows = control("stencilParameter_rows")
        reveal(rows)
        typeInto(rows.contentItem, "7")
        keyClick(Qt.Key_Return)
        tryCompare(rows, "value", 7)
        compare(configuration.preview.parameters.rows, 7)
        compare(configuration.preview.strokes.length, 13)
        const columns = control("stencilParameter_columns")
        reveal(columns)
        typeInto(columns.contentItem, "3")
        // Placer must commit the still-focused integer editor as well.
        clickRevealed(control("applyStencilConfiguration"))
        compare(editor.configuredStarts.length, 1)
        compare(editor.configuredStarts[0].symbol, "table")
        compare(editor.configuredStarts[0].values.rows, 7)
        compare(editor.configuredStarts[0].values.columns, 3)
        compare(toolChosenRequests, 1)
        compare(panel.configuringStencil, "")
    }
    function test_cancelingStencilDraftDoesNotPlaceOrWrite() {
        configureNewStencil("table")
        const rows = control("stencilParameter_rows")
        reveal(rows)
        typeInto(rows.contentItem, "8")
        clickRevealed(control("cancelStencilConfiguration"))
        compare(panel.configuringStencil, "")
        compare(editor.configuredStarts.length, 0)
        compare(editor.propertyWrites.length, 0)
        compare(toolChosenRequests, 0)
        const reopened = configureNewStencil("table")
        compare(reopened.values.rows, 4)
    }
    function test_stencilNumbersKeepDraftTextAndParseOnPlacement_data() {
        return [{tag: "decimal comma", text: "1,5", value: 1.5},
            {tag: "scientific notation", text: "1e-3", value: 0.001}]
    }
    function test_stencilNumbersKeepDraftTextAndParseOnPlacement(data) {
        const configuration = configureNewStencil("graph")
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, data.text)
        compare(duration.text, data.text)
        compare(configuration.values.tMax, 10)
        clickRevealed(control("applyStencilConfiguration"))
        compare(editor.configuredStarts.length, 1)
        compare(editor.configuredStarts[0].values.tMax, data.value)
        compare(toolChosenRequests, 1)
    }
    function test_invalidStencilNumberPreventsPlacement_data() {
        return [{tag: "empty", text: ""}, {tag: "letters", text: "abc"},
            {tag: "incomplete exponent", text: "1e"}, {tag: "infinity", text: "Infinity"},
            {tag: "multiple commas", text: "1,2,3"}, {tag: "below minimum", text: "0"},
            {tag: "above maximum", text: "1000001"}]
    }
    function test_invalidStencilNumberPreventsPlacement(data) {
        const configuration = configureNewStencil("graph")
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, data.text)
        clickRevealed(control("applyStencilConfiguration"))
        compare(editor.configuredStarts.length, 0)
        compare(toolChosenRequests, 0)
        verify(configuration.visible)
        verify(!configuration.preview.valid)
        verify(configuration.preview.error.length > 0)
        verify(!control("applyStencilConfiguration").enabled)
    }
    function test_graphCurveChoiceShowsOnlyApplicableParameters() {
        const configuration = configureNewStencil("graph")
        const curve = control("stencilParameter_curveType")
        verify(!control("stencilParameter_gain").visible)
        verify(!control("stencilParameter_tau").visible)
        verify(!control("stencilParameter_omega0").visible)
        reveal(curve)
        curve.forceActiveFocus()
        keyClick(Qt.Key_Down)
        compare(configuration.values.curveType, "exponential")
        verify(control("stencilParameter_gain").visible)
        verify(control("stencilParameter_tau").visible)
        verify(control("stencilParameter_initialValue").visible)
        verify(!control("stencilParameter_omega0").visible)
        verify(!control("stencilParameter_damping").visible)
        keyClick(Qt.Key_Down)
        compare(configuration.values.curveType, "second-order")
        verify(control("stencilParameter_gain").visible)
        verify(!control("stencilParameter_tau").visible)
        verify(!control("stencilParameter_initialValue").visible)
        verify(control("stencilParameter_omega0").visible)
        verify(control("stencilParameter_damping").visible)
        compare(configuration.preview.parameters.curveType, "second-order")
        verify(!control("stencilParameter_amplitude").visible)
        keyClick(Qt.Key_Down)
        compare(configuration.values.curveType, "sine")
        verify(!control("stencilParameter_gain").visible)
        verify(!control("stencilParameter_omega0").visible)
        verify(!control("stencilParameter_damping").visible)
        for (const key of ["amplitude", "offset", "frequencyHz", "phaseDegrees"])
            verify(control("stencilParameter_" + key).visible)
        compare(configuration.values.yMin, -1.2)
        compare(configuration.values.yMax, 1.2)
        compare(editor.configuredStarts.length, 0)
    }
    function test_sineProductionGeometryCanBeConfiguredAndPlaced() {
        editor.previewProductionGeometry = true
        const configuration = configureNewStencil("graph")
        configuration.setValue("curveType", "sine")
        configuration.setValue("amplitude", 2)
        configuration.setValue("offset", 0.5)
        configuration.setValue("frequencyHz", 0.5)
        configuration.setValue("phaseDegrees", 90)
        configuration.setValue("yMin", -2)
        configuration.setValue("yMax", 3)
        verify(configuration.preview.valid)
        compare(configuration.preview.parameters.curveType, "sine")
        compare(configuration.preview.parameters.phaseDegrees, 90)
        const phase = control("stencilParameter_phaseDegrees")
        reveal(phase); verify(phase.visible)
        verify(configuration.apply())
        compare(editor.configuredStarts.length, 1)
        compare(editor.configuredStarts[0].values.frequencyHz, 0.5)
        compare(editor.configuredStarts[0].values.amplitude, 2)
    }
    function test_bodePhaseUpdatesBoundsAndCurveControls() {
        const configuration = configureNewStencil("bode")
        verify(!control("stencilParameter_gain").visible)
        verify(!control("stencilParameter_omega0").visible)
        verify(!control("stencilParameter_damping").visible)
        const curve = control("stencilParameter_curve")
        reveal(curve)
        curve.forceActiveFocus()
        keyClick(Qt.Key_Space)
        verify(configuration.values.curve)
        verify(control("stencilParameter_gain").visible)
        verify(control("stencilParameter_omega0").visible)
        verify(control("stencilParameter_damping").visible)
        const mode = control("stencilParameter_mode")
        reveal(mode)
        mode.forceActiveFocus()
        keyClick(Qt.Key_Down)
        compare(configuration.values.mode, "phase")
        compare(configuration.values.yMin, -180)
        compare(configuration.values.yMax, 0)
        compare(control("stencilParameter_yMin").text, "-180")
        compare(control("stencilParameter_yMax").text, "0")
        compare(configuration.preview.parameters.mode, "phase")
        keyClick(Qt.Key_Up)
        compare(configuration.values.yMin, -60)
        compare(configuration.values.yMax, 20)
    }
    function test_selectedStencilAppliesEditedParameters() {
        configureSelectedStencil("table", false)
        const apply = control("applyStencilConfiguration")
        compare(apply.text, "Appliquer")
        const rows = control("stencilParameter_rows")
        reveal(rows)
        typeInto(rows.contentItem, "9")
        clickRevealed(apply)
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].kind, "stencil")
        compare(editor.propertyWrites[0].values[0].rows, 9)
        compare(editor.configuredStarts.length, 0)
        compare(toolChosenRequests, 0)
        compare(acceptRequests, 0)
        compare(panel.configuringStencil, "")
    }
    function test_stencilDialogCancelDiscardsUnsubmittedNumber() {
        configureSelectedStencil("graph", true)
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, "2,5")
        mouseClick(control("cancelNativeProperties"))
        compare(cancelRequests, 1)
        compare(acceptRequests, 0)
        compare(editor.propertyWrites.length, 0)
        compare(editor.stencilWriteAttempts, 0)
        compare(sessionEvents, ["cancel"])
        compare(panel.configuringStencil, "")
    }
    function test_stencilDialogAcceptCommitsPendingNumberBeforeRequest() {
        configureSelectedStencil("graph", true)
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, "2,5")
        mouseClick(control("acceptNativeProperties"))
        tryCompare(testCase, "acceptRequests", 1)
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].kind, "stencil")
        compare(editor.propertyWrites[0].values[0].tMax, 2.5)
        compare(sessionEvents, ["write:stencil", "accept"])
        compare(cancelRequests, 0)
    }
    function test_stencilDialogAcceptWaitsForNativeAcknowledgement() {
        configureSelectedStencil("graph", true)
        editor.busyAfterPropertyWrite = true
        editor.acknowledgeProperties = false
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, "3e-2")
        mouseClick(control("acceptNativeProperties"))
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].values[0].tMax, 0.03)
        compare(acceptRequests, 0)
        editor.state = Object.assign({}, editor.state, {
            selectedStencilParameters: editor.propertyWrites[0].values[0],
            propertiesSessionCanEdit: true, propertiesSessionCanCancel: true, propertiesSessionCanAccept: true
        })
        tryCompare(testCase, "acceptRequests", 1)
        compare(sessionEvents, ["write:stencil", "accept"])
        compare(editor.stencilWriteAttempts, 1)
    }
    function test_stencilDialogDifferentAcknowledgedParametersCannotAccept() {
        const configuration = configureSelectedStencil("graph", true)
        editor.busyAfterPropertyWrite = true
        editor.acknowledgeProperties = false
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, "3")
        mouseClick(control("acceptNativeProperties"))
        compare(editor.propertyWrites.length, 1)
        compare(acceptRequests, 0)
        // The host becomes idle after rejecting/rolling back the edit. Merely
        // being idle must not validate a draft it did not actually acknowledge.
        editor.state = Object.assign({}, editor.state, {
            propertiesSessionCanEdit: true, propertiesSessionCanCancel: true, propertiesSessionCanAccept: true
        })
        wait(30)
        compare(acceptRequests, 0)
        compare(editor.stencilWriteAttempts, 1)
        verify(configuration.visible)
        compare(configuration.values.tMax, 3)
    }
    function test_stencilDialogInvalidNumberCannotAccept() {
        const configuration = configureSelectedStencil("graph", true)
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, "1e")
        mouseClick(control("acceptNativeProperties"))
        wait(30)
        compare(acceptRequests, 0)
        compare(editor.stencilWriteAttempts, 0)
        verify(configuration.visible)
        verify(!configuration.preview.valid)
    }
    function test_stencilDialogRejectedWriteCannotAccept() {
        const configuration = configureSelectedStencil("graph", true)
        editor.rejectStencilWrite = true
        const duration = control("stencilParameter_tMax")
        reveal(duration)
        typeInto(duration, "2")
        mouseClick(control("acceptNativeProperties"))
        wait(30)
        compare(acceptRequests, 0)
        compare(editor.stencilWriteAttempts, 1)
        compare(editor.propertyWrites.length, 0)
        verify(configuration.visible)
    }
    function test_stencilSelectionDraftDisablesWhileNativeSessionIsBusy() {
        configureSelectedStencil("table", true)
        editor.state = Object.assign({}, editor.state, {
            propertiesSessionCanEdit: false, propertiesSessionCanCancel: false, propertiesSessionCanAccept: false
        })
        wait(20)
        verify(!control("stencilParameter_rows").enabled)
        verify(!control("applyStencilConfiguration").enabled)
        verify(!control("acceptNativeProperties").enabled)
        compare(editor.propertyWrites.length, 0)
    }
    function test_stencilSelectionDraftCannotModifyDifferentSelection() {
        const configuration = configureSelectedStencil("table", true)
        editor.state = Object.assign({}, editor.state, {
            selectedStencilId: "graph", selectedStencilParameters: editor.stencilDefaults("graph")
        })
        wait(20)
        verify(!control("stencilParameter_rows").enabled)
        verify(!control("applyStencilConfiguration").enabled)
        mouseClick(control("acceptNativeProperties"))
        wait(20)
        compare(acceptRequests, 0)
        compare(editor.stencilWriteAttempts, 0)
        verify(configuration.visible)
    }
    function test_unsupportedPageStillAllowsCatalogueSearch() {
        editor.available = false; editor.status = "Page indisponible"; wait(10)
        verify(!control("editorTool_rectangle").enabled)
        panel.mode = "restencil"; wait(20)
        verify(control("editorStencilSearch").enabled)
        verify(!control("editorStencil_resistor-iec").enabled)
    }
    function test_creationDoesNotEnableUnimplementedProperties() {
        editor.state = Object.assign({}, editor.state, {hasSelection: true})
        panel.mode = "properties"; wait(20)
        verify(!control("selectionWidth").enabled)
        verify(!control("selectionStyle").enabled)
    }
    function test_widthChoicesRespectNativeCapability() {
        compare(control("editorStrokeWidth").count, 2)
        editor.state = Object.assign({}, editor.state, {maximumStrokeWidth: 30})
        tryCompare(control("editorStrokeWidth"), "count", 4)
    }
    function test_colorPickerRoutesCreationAndSelectionSeparately() {
        const creation = control("editorStrokeColor")
        verify(creation.enabled);creation.colorChosen("#136aca");compare(editor.chosen, "#136aca")
        editor.state = Object.assign({}, editor.state, {tool:"select", hasSelection:true, selectionCanChangeColor:true, selectedLineColor:"#d90707"})
        panel.mode = "properties";wait(20)
        const selection = control("selectionStrokeColor")
        verify(selection.visible && selection.enabled);compare(selection.currentColor, "#d90707")
        selection.colorChosen("#0062cc");compare(editor.chosen, "#0062cc")
        editor.state = Object.assign({}, editor.state, {selectionCanChangeColor:false})
        wait(10);verify(!selection.enabled)
    }
    function test_propertiesStateRefreshDoesNotWriteEdits() {
        showProperties()
        for (let i = 0; i < 20; ++i) {
            editor.state = Object.assign({}, editor.state, {
                selectedShapeWidth: 240 + i, selectedShapeHeight: 160 + i,
                selectedCornerRadius: i, selectedLineWidth: i % 2 ? 3 : 5,
                selectedLineStyle: i % 2 ? "solid" : "dashed",
                selectedLineColor: i % 2 ? "#000000" : "#d90707"
            })
            wait(0)
        }
        compare(editor.propertyWrites.length, 0)
        compare(editor.undoCalls, 0)
        compare(control("editorShapeWidth").value, 259)
        compare(control("selectionStyle").currentIndex, 0)
        compare(control("selectionStrokeColor").currentColor, "#000000")
    }
    function test_propertiesWidthEditUndoAndRedoDoNotRepeatWrites() {
        showProperties()
        const width = control("editorShapeWidth")
        verify(width.enabled)
        typeInto(width.contentItem, "300")
        keyClick(Qt.Key_Return)
        tryCompare(width, "value", 300)
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].kind, "resize")
        compare(editor.propertyWrites[0].values, [300, 160])
        editor.redoState = Object.assign({}, editor.state)
        mouseClick(button("Annuler"))
        compare(editor.undoCalls, 1)
        tryCompare(width, "value", 240)
        wait(30)
        compare(editor.propertyWrites.length, 1)
        mouseClick(button("Rétablir"))
        compare(editor.redoCalls, 1)
        tryCompare(width, "value", 300)
        wait(30)
        compare(editor.propertyWrites.length, 1)
    }
    function test_propertiesColorEditUndoAndFocusChangesDoNotReplayColor() {
        showProperties()
        const picker = control("selectionStrokeColor")
        const hex = control("strokeColorHex", picker)
        verify(hex.enabled)
        typeInto(hex, "#136aca")
        keyClick(Qt.Key_Return)
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].values, ["#136aca"])
        compare(picker.currentColor, "#136aca")
        mouseClick(button("Annuler"))
        compare(editor.undoCalls, 1)
        tryCompare(picker, "currentColor", "#000000")
        tryCompare(hex, "text", "#000000")
        hex.forceActiveFocus()
        button("Rétablir").forceActiveFocus()
        wait(30)
        compare(editor.propertyWrites.length, 1)
    }
    function test_propertiesDelayedColorAcknowledgementDoesNotRepeatOnUndoFocus() {
        showProperties()
        editor.acknowledgeProperties = false
        const hex = control("strokeColorHex", control("selectionStrokeColor"))
        typeInto(hex, "#136aca")
        keyClick(Qt.Key_Return)
        compare(editor.propertyWrites.length, 1)
        mouseClick(button("Annuler"))
        compare(editor.undoCalls, 1)
        wait(30)
        compare(editor.propertyWrites.length, 1)
    }
    function test_propertiesNewColorEditAfterSubmissionStillApplies() {
        showProperties()
        editor.acknowledgeProperties = false
        const hex = control("strokeColorHex", control("selectionStrokeColor"))
        typeInto(hex, "#136aca")
        keyClick(Qt.Key_Return)
        compare(editor.propertyWrites.length, 1)
        typeInto(hex, "#249b45")
        keyClick(Qt.Key_Return)
        compare(editor.propertyWrites.length, 2)
        compare(editor.propertyWrites[1].values, ["#249b45"])
        mouseClick(button("Annuler"))
        compare(editor.undoCalls, 1)
        wait(30)
        compare(editor.propertyWrites.length, 2)
    }
    function test_propertiesStyleUserActivationAndUndoStateDoNotLoop() {
        showProperties()
        const style = control("selectionStyle")
        verify(style.enabled)
        style.forceActiveFocus()
        keyClick(Qt.Key_Down)
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].kind, "style")
        compare(editor.propertyWrites[0].values, ["dashed"])
        mouseClick(button("Annuler"))
        compare(editor.undoCalls, 1)
        tryCompare(style, "currentIndex", 0)
        wait(30)
        compare(editor.propertyWrites.length, 1)
    }
    function test_dialogCancelDiscardsUnsubmittedInputBeforeRequest_data() { return pendingPropertyCases() }
    function test_dialogCancelDiscardsUnsubmittedInputBeforeRequest(data) {
        const input = showPropertiesDialog(data)
        const cancel = control("cancelNativeProperties")
        verify(cancel.visible && cancel.enabled)
        typeInto(input, data.text)
        compare(editor.propertyWrites.length, 0)
        mouseClick(cancel)
        compare(cancelRequests, 1)
        compare(acceptRequests, 0)
        compare(editor.propertyWrites.length, 0)
        compare(sessionEvents, ["cancel"])
        compare(editor.undoCalls, 0); compare(editor.redoCalls, 0)
        // If native cancellation cannot finish, the open form shows the last
        // acknowledged values and does not resurrect the discarded edit.
        tryCompare(input, "text", data.original)
        input.forceActiveFocus()
        panel.commitPendingPropertyInputs()
        wait(20)
        compare(editor.propertyWrites.length, 0)
        verify(!panel.discardingPropertyInputs)
    }
    function test_dialogAcceptSubmitsPendingInputBeforeRequest_data() { return pendingPropertyCases() }
    function test_dialogAcceptSubmitsPendingInputBeforeRequest(data) {
        const input = showPropertiesDialog(data)
        const accept = control("acceptNativeProperties")
        verify(accept.visible && accept.enabled)
        editor.acknowledgeProperties = false
        typeInto(input, data.text)
        compare(editor.propertyWrites.length, 0)
        mouseClick(accept)
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].kind, data.kind)
        compare(editor.propertyWrites[0].values, data.values)
        compare(acceptRequests, 1)
        compare(cancelRequests, 0)
        compare(sessionEvents, ["write:" + data.kind, "accept"])
        compare(editor.undoCalls, 0); compare(editor.redoCalls, 0)
        verify(!accept.enabled) // A queued write disabled it during onClicked.
        wait(20)
        compare(editor.propertyWrites.length, 1)
    }
    function test_dialogDiscardRestoresLiveInputBindings() {
        const input = showPropertiesDialog(pendingPropertyCases()[0])
        typeInto(input, "300")
        mouseClick(control("cancelNativeProperties"))
        compare(cancelRequests, 1)
        editor.state = Object.assign({}, editor.state, {selectedShapeWidth: 280, selectedLineColor: "#d90707"})
        tryCompare(control("editorShapeWidth"), "value", 280)
        tryCompare(input, "text", "280")
        const hex = control("strokeColorHex", control("selectionStrokeColor"))
        tryCompare(hex, "text", "#d90707")
        compare(editor.propertyWrites.length, 0)
        typeInto(hex, "#249b45")
        mouseClick(control("acceptNativeProperties"))
        compare(editor.propertyWrites.length, 1)
        compare(editor.propertyWrites[0].values, ["#249b45"])
        compare(acceptRequests, 1)
    }
    function test_nonNativeInspectorKeepsDocumentUndoFooter() {
        showProperties()
        editor.backend = "pc-fixture"
        panel.inspectorOnly = true
        wait(20)
        verify(!panel.propertiesDialog)
        verify(!control("cancelNativeProperties").visible)
        verify(!control("acceptNativeProperties").visible)
        mouseClick(button("Annuler"))
        compare(editor.undoCalls, 1)
        compare(cancelRequests, 0); compare(acceptRequests, 0)
    }
    function test_zNativeLayoutPreviews() {
        if (!nativePalettePreviewDirectory.length) skip("No local preview requested")
        editor.previewProductionGeometry = true
        editor.stencils = nativePaletteGeometry.catalogue()
        // The production catalogue is CONSTANT. This fixture alone replaces
        // its mock at runtime to render all real symbols after behavior tests.
        panel.stencilCatalogue = editor.stencils
        function save(name) {
            wait(50)
            grabImage(panel).save(nativePalettePreviewDirectory + "/" + name + ".png")
        }
        save("reink")
        panel.mode = "restencil"; save("restencil")
        showProperties(); save("properties")
        for (const symbol of ["table", "graph", "bode"]) {
            const configuration = configureNewStencil(symbol)
            save(symbol)
            reveal(control("applyStencilConfiguration")); save(symbol + "-footer")
            if (symbol === "graph") {
                configuration.setValue("curveType", "sine")
                configuration.setValue("tMax", 3)
                reveal(configuration); save("graph-sine")
                reveal(control("applyStencilConfiguration")); save("graph-sine-footer")
            }
            panel.configuringStencil = ""
        }
    }
    function test_openingAnotherStencilStartsAtItsHeading() {
        configureNewStencil("bode")
        reveal(control("applyStencilConfiguration"))
        panel.configuringStencil = ""
        const configuration = configureNewStencil("graph")
        wait(30)
        verify(configuration.mapToItem(panel, 0, 0).y >= control("paletteTabs").height)
    }
}

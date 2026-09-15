import QtQuick 2.15
import QtQuick.Controls 2.15
import QtTest 1.15
import "qrc:/paper" as Paper
import "../../qml"

TestCase {
    id: testCase
    name: "NativeStencilQuickBarUI"
    width: 1024; height: 340; visible: true; when: windowShown
    property int penRequests: 0
    QtObject {
        id: editor
        property var stencils: nativePaletteGeometry.catalogue()
        property var state: ({})
        property string chosenTool: ""
        function setValue(key, value) { const next = Object.assign({}, state); next[key] = value; state = next; return true }
        function chooseTool(tool) { chosenTool = tool; return setValue("tool", tool) }
        function beginStencil(id) { setValue("tool", "symbol"); setValue("activeStencilId", id); setValue("stencilSupportsVoltage", id !== "square-root"); return true }
        function setStencilVertical(value) { return setValue("stencilVertical", value) }
        function setVoltageArrow(value) { return setValue("stencilVoltageArrow", value) }
        function setVoltageReversed(value) { return setValue("stencilVoltageReversed", value) }
        function setVoltageOtherSide(value) { return setValue("stencilVoltageOtherSide", value) }
    }
    NativeStencilQuickBar {
        id: bar; width: 960; height: implicitHeight; editor: editor
        onPenRequested: ++testCase.penRequests
    }
    function control(name) { return findChild(bar, name) }
    function tap(name) {
        const item = control(name); verify(item); verify(item.visible)
        const viewport = control("quickStencilActions")
        let ancestor = item.parent
        while (ancestor && ancestor !== viewport.contentItem) ancestor = ancestor.parent
        if (ancestor) {
            const x = item.mapToItem(viewport.contentItem, 0, 0).x
            if (x < viewport.contentX) viewport.contentX = x
            else if (x + item.width > viewport.contentX + viewport.width) viewport.contentX = x + item.width - viewport.width
            wait(0)
        }
        mouseClick(item, item.width/2, item.height/2); wait(0)
    }
    function init() {
        Paper.Theme.unit = 2; bar.width = 960; bar.optionsOpen = false; testCase.penRequests = 0; editor.chosenTool = ""
        editor.state = {tool:"symbol", activeStencilId:"resistor-iec", recentStencils:["resistor-iec","capacitor","voltage-source","square-root"],
            stencilSupportsVoltage:true, stencilVertical:false, stencilVoltageArrow:false,
            stencilVoltageReversed:false, stencilVoltageOtherSide:false}
        wait(20)
        control("quickStencilActions").contentX = 0
    }
    function test_orientationVoltageAndReturnAreDirect() {
        tap("quickStencilOrientation"); compare(editor.state.stencilVertical, true)
        compare(control("quickStencilOrientation").text, "Vertical")
        tap("quickStencilVoltage"); compare(editor.state.stencilVoltageArrow, true)
        tap("quickStencilVoltageOptions"); compare(bar.optionsOpen, true)
        tap("quickVoltageReverse"); compare(editor.state.stencilVoltageReversed, true)
        tap("quickVoltageSide"); compare(editor.state.stencilVoltageOtherSide, true)
        tap("quickResumePen"); compare(testCase.penRequests, 1)
    }
    function test_recentSymbolsRemainOneTapAndHideIrrelevantOptions() {
        tap("quickStencilVoltage"); tap("quickStencilVoltageOptions"); compare(bar.voltageOptionsVisible, true)
        tap("quickStencil_capacitor"); compare(editor.state.activeStencilId, "capacitor")
        compare(control("quickStencil_square-root"), null)
        for (const id of ["signal-generator", "switch", "switch-closed"]) {
            tap("quickStencil_" + id); compare(editor.state.activeStencilId, id)
            verify(bar.symbol(id).strokes.length > 0)
        }
        // The mathematical symbol remains usable from the complete catalogue.
        editor.beginStencil("square-root"); compare(editor.state.activeStencilId, "square-root")
        compare(control("quickStencilVoltage").visible, false)
        compare(control("quickStencilOrientation").visible, false)
        compare(bar.voltageOptionsVisible, false); compare(bar.optionsOpen, false)
    }
    function test_penRemainsReachableInNarrowViewport() {
        bar.width = 440; wait(20)
        const pen = control("quickResumePen"); verify(pen.width >= 96); verify(pen.height >= 96)
        tap("quickResumePen"); compare(testCase.penRequests, 1)
    }
    function test_wireAndComponentsRemainOneTapApart() {
        tap("quickStencilVoltage"); tap("quickStencilVoltageOptions"); compare(bar.voltageOptionsVisible, true)
        tap("quickOrthogonalWire")
        compare(editor.chosenTool, "wire"); compare(editor.state.tool, "wire")
        verify(control("quickOrthogonalWire").checked)
        compare(control("quickOrthogonalWireIcon").name, "wire")
        compare(control("quickOrthogonalWire").text, "Fil")
        compare(control("quickStencilActions").contentX, 0)
        compare(bar.voltageOptionsVisible, false); compare(bar.optionsOpen, false)
        compare(control("quickStencilOrientation").visible, false)
        compare(control("quickStencilVoltage").visible, false)
        compare(control("quickStencil_resistor-iec").checked, false)
        tap("quickStencil_capacitor")
        compare(editor.state.tool, "symbol"); compare(editor.state.activeStencilId, "capacitor")
        compare(control("quickOrthogonalWire").checked, false)
        compare(control("quickStencil_capacitor").checked, true)
        tap("quickOrthogonalWire"); tap("quickResumePen"); compare(testCase.penRequests, 1)
    }
    function test_wireRemainsFixedAndReachableBesidePenWhenActionsScroll() {
        bar.width = 440; wait(20)
        const wire = control("quickOrthogonalWire"), pen = control("quickResumePen")
        const viewport = control("quickStencilActions")
        verify(wire.width >= 96); verify(wire.height >= 96)
        verify(wire.x >= pen.x + pen.width)
        verify(wire.mapToItem(bar, wire.width, 0).x <= bar.width)
        const position = wire.mapToItem(bar, 0, 0)
        viewport.contentX = viewport.contentWidth - viewport.width; wait(0)
        compare(wire.mapToItem(bar, 0, 0), position)
        tap("quickOrthogonalWire"); compare(editor.state.tool, "wire")
        compare(viewport.contentX, 0)
        tap("quickStencil_resistor-iec"); compare(editor.state.tool, "symbol")
        tap("quickResumePen"); compare(testCase.penRequests, 1)
    }
    function test_captureProductionControls() {
        if (!nativePalettePreviewDirectory) skip("No screenshot directory requested")
        editor.setVoltageArrow(true); wait(60)
        grabImage(bar).save(nativePalettePreviewDirectory + "/stencil-quick-bar.png")
        bar.optionsOpen = true; wait(60)
        grabImage(bar).save(nativePalettePreviewDirectory + "/stencil-quick-bar-options.png")
        tap("quickOrthogonalWire"); wait(60)
        grabImage(bar).save(nativePalettePreviewDirectory + "/stencil-quick-bar-wire.png")
        bar.width = 440; wait(60)
        grabImage(bar).save(nativePalettePreviewDirectory + "/stencil-quick-bar-wire-narrow.png")
    }
}

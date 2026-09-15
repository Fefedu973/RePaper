import QtQuick 2.15
import QtQuick.Controls 2.15
import QtTest 1.15
import "../../qml"

TestCase {
    id: testCase
    name: "NativeQuickColorsUI"
    width: 400; height: 200; visible: true; when: windowShown
    property var fixtureToolbar: null
    QtObject {
        id: editorFixture
        property bool available: true
        property bool captureEnabled: !!testCase.fixtureToolbar && testCase.fixtureToolbar.repaperToolActive
        property var state: ({})
        property int colorCalls: 0
        signal changed()
        onStateChanged: changed()
        function setStrokeColor(color) {
            colorCalls += 1
            state = Object.assign({}, state, {lineColor: color})
            return true
        }
    }
    QtObject {
        id: hostFixture
        property var editor: editorFixture
        property bool nativeOperationPending: false
    }
    NativeQuickColors {
        id: bar
        toolbar: testCase.fixtureToolbar
        editorHost: hostFixture
        width: implicitWidth; height: implicitHeight
    }
    function setState(changes) { editorFixture.state = Object.assign({}, editorFixture.state, changes) }
    function tap(key) {
        const button = findChild(bar, "quickColor_" + key)
        verify(button); verify(button.visible); verify(button.enabled)
        mouseClick(button, button.width/2, button.height/2)
        wait(20)
    }
    function init() {
        verify(nativeQuickColorToolbarMethods.length > 100)
        editorFixture.state = {tool:"stencil", lineColor:"#000000", hasSelection:false, working:false,
            nativeGestureActive:false, nativeSelectionWaiting:false, nativeCreationInFlight:false}
        editorFixture.colorCalls = 0
        hostFixture.nativeOperationPending = false
        const source = [
            'import QtQuick 2.15',
            'Item { id: toolbar',
            'property var selectedPen: primaryTool',
            'property var primaryPen: primarySettings',
            'property var secondaryPen: secondarySettings',
            'property var toolbarProvider: ({colorProfile: 1})',
            'property var repaperEditorHost: null',
            'readonly property bool repaperToolActive: selectedPen === customTool',
            'property bool secondaryAvailable: true',
            'property int nativeColorCalls: 0',
            'property int closeCalls: 0',
            'signal penColorSelected(var rgb, int paletteEnum)',
            'onPenColorSelected: (rgb, paletteEnum) => { nativeColorCalls += 1; selectedPen.pen.color = paletteEnum; selectedPen.pen.colorCode = rgb }',
            'function selectPen(type) { if (type === "secondary") { if (secondaryAvailable) selectedPen = secondaryTool } else if (type === "primary") selectedPen = primaryTool }',
            'function closeFoldout() { closeCalls += 1 }',
            'function usePrimary() { selectedPen = primaryTool }',
            'function useSecondary() { selectedPen = secondaryTool }',
            'function useCustom() { selectedPen = customTool }',
            'function useEraser() { selectedPen = eraserTool }',
            'component NativePen: QtObject { property int color: 0; property real colorCode: 4278190080; property int tool: 6; property int thickness: 2; signal propertyChanged(); onColorChanged: propertyChanged(); onColorCodeChanged: propertyChanged(); onToolChanged: propertyChanged() }',
            'NativePen { id: primarySettings }',
            'NativePen { id: secondarySettings }',
            'QtObject { id: primaryTool; objectName: "primaryPenMenu"; property var pen: primarySettings }',
            'QtObject { id: secondaryTool; objectName: "secondaryPenMenu"; property var pen: secondarySettings }',
            'QtObject { id: customTool; objectName: "RePaperStencilTool"; property var pen: primarySettings }',
            'QtObject { id: eraserTool; objectName: "eraserMenu"; property var pen: primarySettings }',
            nativeQuickColorToolbarMethods,
            '}'
        ].join('\n')
        fixtureToolbar = Qt.createQmlObject(source, testCase, "NativeQuickColorsToolbarFixture")
        fixtureToolbar.repaperEditorHost = hostFixture
        wait(20)
        verify(bar.choiceForKey("black")); verify(bar.choiceForKey("red")); verify(bar.choiceForKey("green"))
    }
    function cleanup() {
        const fixture = fixtureToolbar
        fixtureToolbar = null
        fixture.destroy()
        wait(0)
    }
    function test_nativeButtonsWriteRealPenContractAndPreserveBrush() {
        tap("red")
        compare(fixtureToolbar.primaryPen.color, 9)
        compare(fixtureToolbar.primaryPen.colorCode, 0xffd90707)
        compare(fixtureToolbar.nativeColorCalls, 1)
        compare(fixtureToolbar.primaryPen.tool, 6)
        compare(fixtureToolbar.primaryPen.thickness, 2)
        compare(findChild(bar, "quickColor_red").selected, true)
        tap("green")
        compare(fixtureToolbar.primaryPen.color, 9)
        compare(fixtureToolbar.primaryPen.colorCode, 0xff249b45)
        compare(findChild(bar, "quickColor_green").selected, true)
        tap("black")
        compare(fixtureToolbar.primaryPen.color, 0)
        compare(editorFixture.colorCalls, 0)
    }
    function test_secondaryColorTransfersToStencilAndReturnsInOneTap() {
        fixtureToolbar.useSecondary()
        tap("green")
        compare(fixtureToolbar.repaperLastWritingPenType, "secondary")
        fixtureToolbar.useCustom()
        tryCompare(editorFixture, "colorCalls", 1)
        compare(editorFixture.state.lineColor, "#249b45")
        compare(fixtureToolbar.primaryPen.color, 0)
        tap("red")
        compare(fixtureToolbar.repaperToolActive, true)
        compare(fixtureToolbar.secondaryPen.colorCode, 0xffd90707)
        compare(editorFixture.state.lineColor, "#d90707")
        verify(fixtureToolbar.repaperResumePen())
        compare(fixtureToolbar.selectedPen.objectName, "secondaryPenMenu")
        compare(fixtureToolbar.repaperToolActive, false)
        compare(fixtureToolbar.closeCalls, 1)
        compare(bar.currentColor, "#d90707")
    }
    function test_fullPaletteChangeFollowsNativeWithoutRecursion() {
        fixtureToolbar.useCustom(); wait(20)
        editorFixture.colorCalls = 0
        editorFixture.setStrokeColor("#0062cc")
        wait(20)
        compare(fixtureToolbar.primaryPen.colorCode, 0xff0062cc)
        compare(editorFixture.colorCalls, 1)
        editorFixture.setStrokeColor("#123456")
        wait(20)
        compare(editorFixture.state.lineColor, "#123456")
        compare(fixtureToolbar.primaryPen.colorCode, 0xff0062cc)
        compare(editorFixture.colorCalls, 2)
    }
    function test_nativeMenuColorChangeUpdatesSelectionAndNextStencil() {
        fixtureToolbar.primaryPen.color = 9
        fixtureToolbar.primaryPen.colorCode = 0xff249b45
        wait(20)
        compare(bar.currentColor, "#249b45")
        fixtureToolbar.useCustom()
        tryCompare(editorFixture, "colorCalls", 1)
        compare(editorFixture.state.lineColor, "#249b45")
    }
    function test_retainedHiddenPaletteWaitsThenResyncsTheNativePenOnReopen() {
        fixtureToolbar.useCustom();wait(20)
        editorFixture.colorCalls=0
        bar.visible=false
        fixtureToolbar.primaryPen.color=9
        fixtureToolbar.primaryPen.colorCode=0xff249b45
        fixtureToolbar.primaryPen.propertyChanged()
        wait(20)
        compare(editorFixture.colorCalls,0)
        compare(bar.chooseColor("red"),false)
        bar.visible=true
        tryCompare(editorFixture,"colorCalls",1)
        compare(bar.currentColor,"#249b45")
        compare(editorFixture.state.lineColor,"#249b45")
    }
    function test_selectionCannotBeRecoloredByAutomaticSyncOrQuickButtons() {
        setState({tool:"select", hasSelection:true})
        fixtureToolbar.useCustom()
        fixtureToolbar.secondaryPen.color = 9
        fixtureToolbar.primaryPen.color = 9
        fixtureToolbar.primaryPen.colorCode = 0xffd90707
        wait(20)
        compare(editorFixture.colorCalls, 0)
        compare(bar.chooseColor("green"), false)
        compare(editorFixture.colorCalls, 0)
        setState({tool:"stencil", hasSelection:false})
        tryCompare(editorFixture, "colorCalls", 1)
        compare(editorFixture.state.lineColor, "#d90707")
    }
    function test_activeGestureAndNativeCommandBlockChanges() {
        fixtureToolbar.useCustom(); wait(20)
        setState({nativeGestureActive:true})
        compare(bar.chooseColor("red"), false)
        compare(fixtureToolbar.primaryPen.color, 0)
        setState({nativeGestureActive:false})
        hostFixture.nativeOperationPending = true
        compare(bar.chooseColor("red"), false)
        compare(fixtureToolbar.repaperResumePen(), false)
        compare(fixtureToolbar.repaperToolActive, true)
        compare(fixtureToolbar.repaperSetWritingColor(0xffd90707, 9), false)
        compare(fixtureToolbar.primaryPen.color, 0)
    }
    function test_invalidNativeColorUnavailableAndModelResetRebuildsChoices() {
        fixtureToolbar.primaryPen.tool = 99
        wait(20)
        compare(findChild(bar, "quickColor_red").enabled, false)
        compare(bar.chooseColor("red"), false)
        fixtureToolbar.primaryPen.tool = 6
        wait(20)
        tap("red")
        compare(fixtureToolbar.primaryPen.colorCode, 0xffd90707)
    }
    function test_eraserColorReturnsToLastWritingPen() {
        fixtureToolbar.useSecondary()
        fixtureToolbar.useEraser()
        tap("green")
        compare(fixtureToolbar.selectedPen.objectName, "secondaryPenMenu")
        compare(fixtureToolbar.secondaryPen.colorCode, 0xff249b45)
    }
    function test_unavailableSecondaryFallsBackToPrimary() {
        fixtureToolbar.useSecondary()
        fixtureToolbar.useCustom()
        fixtureToolbar.secondaryAvailable = false
        verify(fixtureToolbar.repaperResumePen())
        compare(fixtureToolbar.selectedPen.objectName, "primaryPenMenu")
        compare(fixtureToolbar.repaperLastWritingPenType, "primary")
    }
    function test_controlsKeepNativeTouchSize() {
        compare(bar.implicitWidth, 308)
        compare(bar.implicitHeight, 108)
        for (const key of ["black", "red", "green"]) {
            const button = findChild(bar, "quickColor_" + key)
            compare(button.width, 96); compare(button.height, 96)
        }
    }
    function test_captureProductionControls() {
        if (!nativePalettePreviewDirectory) skip("No screenshot directory requested")
        tap("green"); wait(60)
        grabImage(bar).save(nativePalettePreviewDirectory + "/native-quick-colors.png")
    }
}

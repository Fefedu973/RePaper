import QtQuick 2.15
import QtQuick.Controls 2.15
import QtTest 1.15
import "qrc:/paper" as Paper

TestCase {
    id: testCase
    name: "NativePalettePerformance"
    width: 720; height: 1000; visible: true; when: windowShown
    property var currentPanel: null
    readonly property var editor: nativePalettePerformanceEditor

    function createPanel(baseline, mode, shown) {
        const url = baseline ? "file://" + nativePaletteBaselineDirectory + "/extensions/reink/qml/ReInkSidebar.qml"
                             : Qt.resolvedUrl("../../qml/ReInkSidebar.qml")
        const component = Qt.createComponent(url)
        compare(component.status, Component.Ready, component.errorString())
        const start = Date.now()
        currentPanel = component.createObject(testCase, {editor: editor, mode: mode,
            width: 720, height: 1000, visible: shown})
        verify(currentPanel, component.errorString())
        const creationMs = Date.now() - start
        wait(40)
        return creationMs
    }
    function descendants(item, prefix) {
        let count = item.objectName && item.objectName.indexOf(prefix) === 0 ? 1 : 0
        const children = item.children || []
        for (let i=0; i<children.length; ++i) count += descendants(children[i], prefix)
        return count
    }
    function report(label, creationMs) {
        const metrics = editor.counters()
        metrics.catalogueSize = editor.catalogueSize()
        metrics.stencilDelegates = descendants(currentPanel, "editorStencil_")
        metrics.creationMs = creationMs
        console.log("REPAPER_UI_PERF " + label + " " + JSON.stringify(metrics))
        return metrics
    }
    function init() {
        Paper.Theme.unit=2
        editor.resetCatalogue(0)
        editor.resetState()
        editor.resetCounters()
    }
    function cleanup() {
        if (currentPanel) currentPanel.destroy()
        currentPanel=null
        wait(0)
    }
    function test_baselineSnapshot_data() {
        return [{tag:"hidden-drawing",mode:"reink",shown:false,size:0},
                {tag:"visible-stencil",mode:"restencil",shown:true,size:0},
                {tag:"large-stencil",mode:"restencil",shown:true,size:400}]
    }
    function test_baselineSnapshot(data) {
        if (!nativePaletteBaselineDirectory) skip("Baseline snapshot path not requested")
        editor.resetCatalogue(data.size);editor.resetCounters()
        const elapsed=createPanel(true,data.mode,data.shown)
        const metrics=report("baseline/"+data.tag,elapsed)
        compare(metrics.stencilDelegates,editor.catalogueSize())
    }
    function test_hiddenDrawingDoesNotReadOrInstantiateCatalogue() {
        const elapsed=createPanel(false,"reink",false)
        const metrics=report("current/hidden-drawing",elapsed)
        compare(metrics.stencilReads,0)
        compare(metrics.stencilDelegates,0)
        editor.resetCounters()
        for(let i=0;i<120;++i) editor.changeState({lineWidth:(i%4)+1})
        wait(0)
        compare(editor.counters().stencilReads,0)
    }
    function test_catalogueDelegatesAreBoundedByViewport_data() {
        return [{tag:"production",size:0},{tag:"400-symbols",size:400}]
    }
    function test_catalogueDelegatesAreBoundedByViewport(data) {
        editor.resetCatalogue(data.size);editor.resetCounters()
        const elapsed=createPanel(false,"restencil",true)
        const metrics=report("current/"+data.tag,elapsed)
        verify(metrics.stencilReads <= 1,"Catalogue must be read once per panel")
        verify(metrics.stencilDelegates>0)
        verify(metrics.stencilDelegates<=16,"Only the viewport plus a small cache may instantiate delegates")
        const list=findChild(currentPanel,"editorStencilList");verify(list)
        const originalContent=list.contentY
        list.positionViewAtEnd();wait(40)
        verify(list.contentY>originalContent)
        verify(descendants(currentPanel,"editorStencil_")<=16)
        const search=findChild(currentPanel,"editorStencilSearch");verify(search)
        search.text="Diagramme de Bode";wait(40)
        compare(list.count,1)
        const button=findChild(currentPanel,"editorStencil_bode");verify(button);verify(button.visible)
    }
    function test_reopeningRetainsCatalogueAndDoesNotReReadIt() {
        createPanel(false,"restencil",true)
        const list=findChild(currentPanel,"editorStencilList");verify(list)
        editor.resetCounters()
        for(let i=0;i<20;++i) {
            currentPanel.visible=false;wait(0)
            currentPanel.visible=true;wait(0)
            compare(findChild(currentPanel,"editorStencilList"),list)
        }
        compare(editor.counters().stencilReads,0)
        verify(descendants(currentPanel,"editorStencil_")<=16)
    }
}

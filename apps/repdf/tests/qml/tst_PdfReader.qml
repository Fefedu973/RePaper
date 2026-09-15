import QtQuick 2.15
import QtQuick.Controls 2.15
import QtTest 1.15
import "../../qml" as Reader

TestCase {
    id: testCase
    name: "PdfReaderUI"
    when: windowShown
    width: 32; height: 32; visible: true
    property var readerWindow: null
    Component { id: readerComponent; Reader.Main {} }
    ListModel {
        id: library
        property string mode: "library"
        property string locationTitle: "Bibliothèque"
        property string filter: ""
        property bool busy: false
        property string error: ""
        property bool canGoBack: false
        property int libraryRequests: 0
        property int filesRequests: 0
        property int backRequests: 0
        property int refreshRequests: 0
        property string openedFolder: ""
        function showLibrary() { ++libraryRequests; mode = "library" }
        function showFiles() { ++filesRequests; mode = "files" }
        function goUp() { ++backRequests; canGoBack = false }
        function refresh() { ++refreshRequests }
        function openFolder(id) { openedFolder = id; locationTitle = "Cours"; canGoBack = true }
    }
    function control(name) {
        function visualChild(item) {
            if (item.objectName === name) return item
            const children = item.children || []
            for (let i = 0; i < children.length; ++i) {
                const found = visualChild(children[i])
                if (found) return found
            }
            return null
        }
        const popup = findChild(readerWindow, "pdfPicker")
        const result = findChild(readerWindow, name) || visualChild(readerWindow.contentItem)
            || (popup && visualChild(popup.contentItem))
        verify(result !== null, name)
        return result
    }
    function typeInto(input, text) {
        input.forceActiveFocus()
        keyClick(Qt.Key_A, Qt.ControlModifier)
        if (!text.length) keyClick(Qt.Key_Backspace)
        for (let i = 0; i < text.length; ++i) keyClick(text.charAt(i))
    }
    function verifyInsideWindow(item) {
        verify(item.visible, item.objectName + " is visible")
        verify(item.width > 0 && item.height > 0, item.objectName + " has a usable size")
        const point = item.mapToItem(readerWindow.contentItem, 0, 0)
        verify(point.x >= -0.5 && point.y >= -0.5, item.objectName + " starts inside the window")
        verify(point.x + item.width <= readerWindow.width + 0.5,
               item.objectName + " fits the window width")
        verify(point.y + item.height <= readerWindow.height + 0.5,
               item.objectName + " fits the window height")
    }
    function openFixture() {
        verify(readerWindow.openDocument("/fixture/test.pdf", "Test"))
        tryCompare(control("pdfPicker"), "visible", false)
        wait(30)
    }
    function enterReadingMode() {
        mouseClick(control("enterPdfFullscreen"))
        tryCompare(readerWindow, "readingMode", true)
        tryCompare(readerWindow, "readingControlsVisible", true)
        tryCompare(control("pdfReadingControls"), "visible", true)
        wait(30)
    }
    function tapPdf(useTouch) {
        const viewport = control("pdfViewport")
        const x = viewport.width * 0.35
        const y = viewport.height * 0.45
        if (useTouch) {
            const sequence = touchEvent(viewport)
            sequence.press(0, viewport, x, y).commit()
            sequence.release(0, viewport, x, y).commit()
        } else {
            mouseClick(viewport, x, y)
        }
    }
    function initTestCase() {
        readerWindow = readerComponent.createObject(null, {library: library, width: 540, height: 770})
        verify(readerWindow !== null)
        tryCompare(readerWindow, "visible", true)
        compare(readerWindow.readingControlsTimeout, 4000)
    }
    function cleanupTestCase() { readerWindow.destroy() }
    function init() {
        readerWindow.setReadingMode(false)
        readerWindow.readingControlsTimeout = 4000
        readerWindow.width = 540; readerWindow.height = 770
        library.clear()
        library.append([
            {entryId: "folder-cours", title: "Cours", subtitle: "Dossier", filePath: "", folder: true, available: true},
            {entryId: "pdf-cours", title: "Circuits.pdf", subtitle: "5 pages", filePath: "/fixture/Circuits.pdf", folder: false, available: true},
            {entryId: "missing", title: "Document absent", subtitle: "PDF indisponible", filePath: "", folder: false, available: false}
        ])
        library.mode = "library"; library.locationTitle = "Bibliothèque"; library.filter = ""
        library.busy = false; library.error = ""; library.canGoBack = false
        library.libraryRequests = 0; library.filesRequests = 0; library.backRequests = 0
        library.refreshRequests = 0; library.openedFolder = ""
        const view = control("pdfView")
        view.closeDocument(); view.rejectOpen = false; view.openCalls = 0; view.busy = false
        view.aspectRatio = 0.75; view.rotation = 0
        readerWindow.setZoom(1)
        readerWindow.showPicker()
        tryCompare(control("pdfPicker"), "opened", true)
        wait(30)
    }
    function test_initialPickerAndControlsFitSmallWindow() {
        verify(control("pdfPicker").visible)
        verify(!control("previousPdfPage").enabled)
        verify(!control("nextPdfPage").enabled)
        verify(!control("zoomInPdf").enabled)
        const picker = control("pdfPicker")
        verify(picker.width <= readerWindow.width)
        verify(picker.y + picker.height <= readerWindow.height)
        for (const name of ["pdfLibraryTab", "pdfFilesTab", "pdfPickerBack", "pdfPickerRefresh", "pdfPickerFilter", "pdfPickerClose"]) {
            const item = control(name)
            verify(item.height >= 48, name)
            const point = item.mapToItem(readerWindow.contentItem, 0, 0)
            verify(point.x >= 0 && point.x + item.width <= readerWindow.width, name)
        }
    }
    function test_pickerRemainsUsableInCompactWindow_data() {
        return [{tag: "300 px high", height: 300}, {tag: "240 px high", height: 240}]
    }
    function test_pickerRemainsUsableInCompactWindow(data) {
        readerWindow.width = 400; readerWindow.height = data.height
        verify(waitForRendering(control("pdfSurface")))
        for (const name of ["pdfPickerPanel", "pdfLibraryTab", "pdfFilesTab", "pdfPickerBack",
                            "pdfPickerRefresh", "pdfPickerFilter", "pdfPickerClose", "pdfPickerEntries"])
            verifyInsideWindow(control(name))
        const entries = control("pdfPickerEntries")
        verify(entries.height >= 24, "The compact picker retains a tappable results area")
        const firstEntry = control("pdfEntry_0")
        mouseClick(firstEntry, firstEntry.width / 2, Math.min(12, entries.height / 2))
        compare(library.openedFolder, "folder-cours")
        verify(control("pdfPicker").opened)
    }
    function test_folderAndSourceButtonsRouteToLibrary() {
        mouseClick(control("pdfFilesTab"))
        compare(library.filesRequests, 1)
        compare(library.mode, "files")
        mouseClick(control("pdfLibraryTab"))
        compare(library.libraryRequests, 1)
        mouseClick(control("pdfEntry_0"))
        compare(library.openedFolder, "folder-cours")
        compare(control("pdfView").openCalls, 0)
        verify(control("pdfPicker").opened)
        mouseClick(control("pdfPickerBack"))
        compare(library.backRequests, 1)
        mouseClick(control("pdfPickerRefresh"))
        compare(library.refreshRequests, 1)
    }
    function test_searchEditsModelFilter() {
        typeInto(control("pdfPickerFilter"), "circuits")
        compare(library.filter, "circuits")
        library.filter = ""
        tryCompare(control("pdfPickerFilter"), "text", "")
    }
    function test_pdfEntryOpensAndDismissesPicker() {
        const keyboard = control("pdfKeyboard")
        const dismissBefore = keyboard.dismissCalls
        mouseClick(control("pdfEntry_1"))
        const view = control("pdfView")
        compare(view.openCalls, 1)
        compare(view.lastPath, "/fixture/Circuits.pdf")
        compare(view.lastTitle, "Circuits.pdf")
        tryCompare(control("pdfPicker"), "visible", false)
        verify(keyboard.dismissCalls > dismissBefore)
        compare(control("pdfPageNumber").text, "1")
        verify(control("nextPdfPage").enabled)
    }
    function test_unavailableEntryCannotOpen() {
        const entry = control("pdfEntry_2")
        verify(!entry.enabled)
        mouseClick(entry)
        compare(control("pdfView").openCalls, 0)
        verify(control("pdfPicker").opened)
    }
    function test_failedOpenKeepsPickerAndDisplaysError() {
        control("pdfView").rejectOpen = true
        mouseClick(control("pdfEntry_1"))
        compare(control("pdfView").openCalls, 1)
        verify(control("pdfPicker").opened)
        compare(control("pdfPickerError").text, "Le PDF est illisible.")
        verify(control("pdfPickerError").visible)
    }
    function test_busyLibraryPreventsNavigationAndOpening() {
        library.busy = true
        wait(10)
        verify(!control("pdfFilesTab").enabled)
        verify(!control("pdfPickerRefresh").enabled)
        verify(!control("pdfEntry_1").enabled)
        compare(control("pdfView").openCalls, 0)
    }
    function test_pageNavigationAndJumpRespectBounds() {
        verify(readerWindow.openDocument("/fixture/test.pdf", "Test"))
        const view = control("pdfView")
        verify(!control("previousPdfPage").enabled)
        mouseClick(control("nextPdfPage"))
        compare(view.pageNumber, 2)
        typeInto(control("pdfPageNumber"), "5")
        keyClick(Qt.Key_Return)
        compare(view.pageNumber, 5)
        verify(!control("nextPdfPage").enabled)
        mouseClick(control("previousPdfPage"))
        compare(view.pageNumber, 4)
        compare(control("pdfPageNumber").text, "4")
        // A stale or invalid editor value must not escape the page interval.
        const field = control("pdfPageNumber")
        field.forceActiveFocus(); field.text = "100"
        control("fitPdfWidth").forceActiveFocus()
        compare(view.pageNumber, 4)
        compare(field.text, "4")
    }
    function test_zoomFitRotationAndViewportSize() {
        verify(readerWindow.openDocument("/fixture/test.pdf", "Test"))
        const view = control("pdfView")
        const oldWidth = view.width
        mouseClick(control("zoomInPdf"))
        compare(readerWindow.zoom, 1.25)
        verify(view.width > oldWidth)
        mouseClick(control("fitPdfWidth"))
        compare(readerWindow.zoom, 1)
        mouseClick(control("rotatePdfPage"))
        compare(view.rotation, 90)
        fuzzyCompare(view.height, view.width / view.aspectRatio, 0.1)
        readerWindow.setZoom(100)
        compare(readerWindow.zoom, 4)
        verify(!control("zoomInPdf").enabled)
        readerWindow.setZoom(0)
        compare(readerWindow.zoom, 0.5)
        verify(!control("zoomOutPdf").enabled)
    }
    function test_readingModeRequiresDocumentAndHidesNormalControls() {
        verify(!readerWindow.readingMode)
        verify(!readerWindow.readingControlsVisible)
        verify(!control("enterPdfFullscreen").enabled)
        verify(!control("pdfReadingControls").visible)
        openFixture()
        compare(control("enterPdfFullscreen").text, "Plein écran")
        enterReadingMode()
        for (const name of ["pdfHeader", "pdfNavigation", "pdfFooter", "pdfReaderStatus"])
            verify(!control(name).visible, name + " hidden while reading")
        const viewport = control("pdfViewport")
        const origin = viewport.mapToItem(readerWindow.contentItem, 0, 0)
        fuzzyCompare(origin.x, 0, 0.1)
        fuzzyCompare(origin.y, 0, 0.1)
        fuzzyCompare(viewport.width, readerWindow.width, 0.1)
        fuzzyCompare(viewport.height, readerWindow.height, 0.1)
        compare(control("exitPdfFullscreen").text, "Quitter le plein écran")
        mouseClick(control("exitPdfFullscreen"))
        tryCompare(readerWindow, "readingMode", false)
        verify(!readerWindow.readingControlsVisible)
        verify(!control("pdfReadingControls").visible)
        for (const name of ["pdfHeader", "pdfNavigation", "pdfFooter", "pdfReaderStatus"])
            verify(control(name).visible, name + " restored after reading")
        verify(viewport.height < readerWindow.height)
    }
    function test_directReadingModeActivationInitializesControls() {
        openFixture()
        readerWindow.readingMode = true
        tryCompare(readerWindow, "readingControlsVisible", true)
        verify(control("pdfReadingControls").visible)
        readerWindow.readingMode = false
        verify(!readerWindow.readingControlsVisible)
        verify(!control("pdfReadingControls").visible)
    }
    function test_widthAndAspectRatioSurviveResize_data() {
        return [
            {tag: "normal portrait", reading: false, aspectRatio: 0.75},
            {tag: "normal landscape", reading: false, aspectRatio: 1.5},
            {tag: "immersive portrait", reading: true, aspectRatio: 0.75},
            {tag: "immersive landscape", reading: true, aspectRatio: 1.5}
        ]
    }
    function test_widthAndAspectRatioSurviveResize(data) {
        openFixture()
        const view = control("pdfView")
        const viewport = control("pdfViewport")
        view.aspectRatio = data.aspectRatio
        readerWindow.setReadingMode(data.reading)
        for (const size of [{width: 400, height: 300}, {width: 1000, height: 500}, {width: 540, height: 770}]) {
            readerWindow.width = size.width; readerWindow.height = size.height
            wait(50)
            verifyInsideWindow(viewport)
            if (data.reading) {
                fuzzyCompare(viewport.width, size.width, 0.1)
                fuzzyCompare(viewport.height, size.height, 0.1)
            }
            for (const zoom of [0.5, 1, 1.25, 4]) {
                readerWindow.setZoom(zoom)
                tryVerify(function() { return Math.abs(view.width - viewport.width * zoom) < 0.1 })
                fuzzyCompare(view.height, view.width / data.aspectRatio, 0.1)
                verify(viewport.contentWidth >= view.width - 0.1)
                verify(viewport.contentHeight >= view.height - 0.1)
            }
            readerWindow.setZoom(1)
            fuzzyCompare(view.x, 0, 0.1)
            fuzzyCompare(view.y, 0, 0.1)
            fuzzyCompare(viewport.contentWidth, viewport.width, 0.1)
        }
    }
    function test_rotationMaintainsWidthFit() {
        openFixture()
        const view = control("pdfView")
        const viewport = control("pdfViewport")
        const portraitRatio = view.aspectRatio
        mouseClick(control("rotatePdfPage"))
        compare(view.rotation, 90)
        fuzzyCompare(view.aspectRatio, 1 / portraitRatio, 0.001)
        fuzzyCompare(view.width, viewport.width, 0.1)
        fuzzyCompare(view.height, viewport.width * portraitRatio, 0.1)
        enterReadingMode()
        fuzzyCompare(view.width, viewport.width, 0.1)
        fuzzyCompare(view.height, viewport.width * portraitRatio, 0.1)
    }
    function test_controlsStayInsideResizedWindow_data() {
        return [
            {tag: "compact", width: 400, height: 300},
            {tag: "portrait", width: 540, height: 770},
            {tag: "landscape", width: 1000, height: 500}
        ]
    }
    function test_controlsStayInsideResizedWindow(data) {
        openFixture()
        readerWindow.width = data.width; readerWindow.height = data.height
        wait(50)
        verifyInsideWindow(control("pdfViewport"))
        for (const name of ["openPdfPicker", "closePdfReader", "previousPdfPage", "nextPdfPage",
                            "pdfPageNumber", "zoomOutPdf", "zoomInPdf", "fitPdfWidth",
                            "rotatePdfPage", "enterPdfFullscreen"])
            verifyInsideWindow(control(name))
        enterReadingMode()
        verifyInsideWindow(control("pdfReadingControls"))
        for (const name of ["readingZoomOutPdf", "readingZoomInPdf", "readingPreviousPdfPage",
                            "readingNextPdfPage", "exitPdfFullscreen"])
            verifyInsideWindow(control(name))
        mouseClick(control("exitPdfFullscreen"))
        tryCompare(readerWindow, "readingMode", false)
        verifyInsideWindow(control("enterPdfFullscreen"))
    }
    function test_readingTapTogglesControls_data() {
        return [{tag: "mouse", useTouch: false}, {tag: "touch", useTouch: true}]
    }
    function test_condensedReaderKeepsActionsReachable_data() {
        return [{tag: "minimum native window at 2x", width: 200, height: 350},
                {tag: "short native window at 2x", width: 250, height: 175}]
    }
    function test_condensedReaderKeepsActionsReachable(data) {
        openFixture()
        readerWindow.width = data.width; readerWindow.height = data.height
        wait(50)
        verify(readerWindow.condensedReader)
        for (const name of ["pdfReaderMore", "openPdfPicker", "previousPdfPage", "nextPdfPage", "pdfPageNumber", "pdfViewport"])
            verifyInsideWindow(control(name))
        verify(!control("pdfFooter").visible)
        readerWindow.setZoom(2)
        mouseClick(control("pdfReaderMore"))
        const options = control("pdfReaderOptions")
        tryCompare(options, "visible", true)
        const list = control("pdfReaderOptionsEntries")
        verifyInsideWindow(list)
        list.positionViewAtIndex(3, ListView.Center)
        wait(50)
        const fit = control("compactFitPdfWidth")
        verifyInsideWindow(fit)
        mouseClick(fit)
        tryCompare(options, "visible", false)
        compare(readerWindow.zoom, 1)
        mouseClick(control("pdfReaderMore"))
        list.positionViewAtBeginning()
        wait(50)
        mouseClick(control("compactEnterPdfFullscreen"))
        tryCompare(readerWindow, "readingMode", true)
        for (const name of ["pdfReadingControls", "readingZoomOutPdf", "readingZoomInPdf", "readingPreviousPdfPage", "readingNextPdfPage", "fitReadingPdfWidth", "exitPdfFullscreen"])
            verifyInsideWindow(control(name))
        mouseClick(control("exitPdfFullscreen"))
        tryCompare(readerWindow, "readingMode", false)
    }
    function test_readingTapTogglesControls(data) {
        openFixture()
        enterReadingMode()
        tapPdf(data.useTouch)
        tryCompare(readerWindow, "readingControlsVisible", false)
        verify(!control("pdfReadingControls").visible)
        verify(readerWindow.readingMode)
        tapPdf(data.useTouch)
        tryCompare(readerWindow, "readingControlsVisible", true)
        verify(control("pdfReadingControls").visible)
        verify(readerWindow.readingMode)
    }
    function test_readingDragScrollDoesNotToggleControls_data() {
        return [
            {tag: "mouse shown", useTouch: false, controlsVisible: true},
            {tag: "mouse hidden", useTouch: false, controlsVisible: false},
            {tag: "touch shown", useTouch: true, controlsVisible: true},
            {tag: "touch hidden", useTouch: true, controlsVisible: false}
        ]
    }
    function test_readingDragScrollDoesNotToggleControls(data) {
        openFixture()
        readerWindow.readingControlsTimeout = 10000
        enterReadingMode()
        readerWindow.setZoom(2)
        if (!data.controlsVisible) {
            tapPdf(data.useTouch)
            tryCompare(readerWindow, "readingControlsVisible", false)
        }
        const viewport = control("pdfViewport")
        const x = viewport.width * 0.35
        const y = viewport.height * 0.45
        verify(viewport.contentHeight > viewport.height + 120)
        compare(viewport.contentY, 0)
        if (data.useTouch) {
            const sequence = touchEvent(viewport)
            sequence.press(0, viewport, x, y).commit()
            for (let offset = 30; offset <= 120; offset += 30) {
                sequence.move(0, viewport, x, y - offset).commit()
                wait(20)
            }
            sequence.release(0, viewport, x, y - 120).commit()
        } else {
            mouseDrag(viewport, x, y, 0, -120, Qt.LeftButton, Qt.NoModifier, 20)
        }
        tryVerify(function() { return viewport.contentY > 0 })
        compare(readerWindow.readingControlsVisible, data.controlsVisible)
        compare(control("pdfReadingControls").visible, data.controlsVisible)
        verify(readerWindow.readingMode)
        viewport.cancelFlick()
    }
    function test_readingControlsNavigateZoomAndReturnToNormal() {
        openFixture()
        enterReadingMode()
        const view = control("pdfView")
        const viewport = control("pdfViewport")
        verify(!control("readingPreviousPdfPage").enabled)
        mouseClick(control("readingZoomInPdf"))
        compare(readerWindow.zoom, 1.25)
        fuzzyCompare(view.width, viewport.width * 1.25, 0.1)
        verify(readerWindow.readingControlsVisible)
        mouseClick(control("readingZoomOutPdf"))
        compare(readerWindow.zoom, 1)
        readerWindow.setZoom(4)
        verify(!control("readingZoomInPdf").enabled)
        readerWindow.setZoom(0.5)
        verify(!control("readingZoomOutPdf").enabled)
        readerWindow.setZoom(2)
        viewport.contentY = 100
        mouseClick(control("readingNextPdfPage"))
        compare(view.pageNumber, 2)
        compare(viewport.contentX, 0)
        compare(viewport.contentY, 0)
        verify(readerWindow.readingControlsVisible)
        mouseClick(control("readingPreviousPdfPage"))
        compare(view.pageNumber, 1)
        view.pageNumber = view.pageCount
        verify(!control("readingNextPdfPage").enabled)
        mouseClick(control("exitPdfFullscreen"))
        tryCompare(readerWindow, "readingMode", false)
        tryVerify(function() { return viewport.height < readerWindow.height })
        // The normal toolbar needs a layout/render pass before hit testing it.
        verify(waitForRendering(control("pdfSurface")))
        compare(control("pdfPageNumber").text, "5")
        mouseClick(control("previousPdfPage"))
        compare(view.pageNumber, 4)
        verify(!readerWindow.readingControlsVisible)
    }
    function test_readingControlsTimeoutAndTapRecovery() {
        openFixture()
        readerWindow.readingControlsTimeout = 180
        enterReadingMode()
        tryCompare(readerWindow, "readingControlsVisible", false, 1000)
        verify(!control("pdfReadingControls").visible)
        verify(readerWindow.readingMode)
        tapPdf(false)
        tryCompare(readerWindow, "readingControlsVisible", true)
        tryCompare(readerWindow, "readingControlsVisible", false, 1000)
        verify(readerWindow.readingMode)
        readerWindow.setReadingMode(false)
        verify(!readerWindow.readingControlsVisible)
        verify(control("enterPdfFullscreen").visible)
    }
    function test_readingControlActionRestartsTimeout_data() {
        return [
            {tag: "next page", button: "readingNextPdfPage", initialPage: 1, page: 2, zoom: 1},
            {tag: "previous page", button: "readingPreviousPdfPage", initialPage: 2, page: 1, zoom: 1},
            {tag: "zoom in", button: "readingZoomInPdf", initialPage: 1, page: 1, zoom: 1.25},
            {tag: "zoom out", button: "readingZoomOutPdf", initialPage: 1, page: 1, zoom: 0.75}
        ]
    }
    function test_readingControlActionRestartsTimeout(data) {
        openFixture()
        control("pdfView").pageNumber = data.initialPage
        readerWindow.readingControlsTimeout = 600
        enterReadingMode()
        wait(350)
        mouseClick(control(data.button))
        compare(control("pdfView").pageNumber, data.page)
        compare(readerWindow.zoom, data.zoom)
        wait(350)
        verify(readerWindow.readingControlsVisible, data.button + " restarts the controls timeout")
        tryCompare(readerWindow, "readingControlsVisible", false, 1000)
    }
}

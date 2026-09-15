import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import RePdf 1.0
import RePaper.Keyboard 1.0
import "qrc:/paper" as Paper

ApplicationWindow {
    id: window
    objectName: "pdfReaderWindow"
    width: 1000; height: 1300; visible: true
    title: reader.title.length ? reader.title + " · rePDF" : "rePDF"
    color: "white"; font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.body
    palette.window: "white"; palette.base: "white"; palette.button: "white"
    palette.highlight: "#222222"; palette.highlightedText: "white"
    property var library: typeof pdfLibrary !== "undefined" ? pdfLibrary : null
    property real zoom: 1
    property string openError: ""
    property bool readingMode: false
    property bool readingControlsVisible: false
    property int readingControlsTimeout: 4000
    readonly property bool hasDocument: reader.pageCount > 0
    readonly property var hostViewport: typeof appLoadViewport !== "undefined" ? appLoadViewport : null
    readonly property real u: Paper.Theme.unit
    readonly property bool compact: surface.height < 520 * u
    readonly property bool narrow: surface.width < 380 * u
    readonly property bool condensedReader: surface.width < 350 * u || surface.height < 260 * u
    readonly property bool narrowReadingControls: surface.width < 350 * u
    readonly property bool shortReadingControls: surface.height < 200 * u
    readonly property real outerMargin: readingMode ? 0 : (compact ? 8 : 24) * u
    Binding {
        target: Paper.Theme
        property: "unit"
        value: window.hostViewport && window.hostViewport.available ? 2 : 1
    }
    readonly property rect keyboardRectangle: hostViewport && hostViewport.available
        ? hostViewport.mapRectFromWindow(keyboard.platformRectangle || Qt.rect(0, 0, 0, 0))
        : keyboard.platformRectangle || Qt.rect(0, 0, 0, 0)
    readonly property real keyboardOcclusion: keyboardRectangle.width > 0 && keyboardRectangle.height > 0
        ? Math.max(0, Math.min(surface.height, surface.height - keyboardRectangle.y)) : 0

    function setReadingMode(enabled) {
        readingMode = enabled && hasDocument
    }
    onReadingModeChanged: {
        if (readingMode && !hasDocument) { readingMode = false; return }
        readingControlsVisible = readingMode
        if (readingMode) { keyboard.dismiss(); readingControlsTimer.restart() }
        else readingControlsTimer.stop()
    }
    function keepReadingControls() {
        if (!readingMode) return
        readingControlsVisible = true
        readingControlsTimer.restart()
    }
    function toggleReadingControls() {
        if (!readingMode) return
        readingControlsVisible = !readingControlsVisible
        if (readingControlsVisible) readingControlsTimer.restart()
        else readingControlsTimer.stop()
    }
    onHasDocumentChanged: if (!hasDocument) setReadingMode(false)

    function resetViewport() { viewport.contentX = 0; viewport.contentY = 0 }
    function setZoom(value) {
        zoom = Math.max(0.5, Math.min(4, Math.round(value * 100) / 100))
        resetViewport()
    }
    function openDocument(path, displayName) {
        openError = ""
        if (!reader.openFile(path, displayName || "")) {
            openError = reader.error || "Ce PDF ne peut pas être ouvert."
            return false
        }
        setReadingMode(false)
        setZoom(1)
        picker.close()
        keyboard.dismiss()
        return true
    }
    function showPicker() { openError = ""; picker.open() }
    function commitPageNumber() {
        const text = pageInput.text.trim()
        const value = /^\d+$/.test(text) ? Number(text) : NaN
        if (isFinite(value) && value >= 1 && value <= reader.pageCount)
            reader.pageNumber = value
        pageInput.text = window.hasDocument ? String(reader.pageNumber) : ""
    }
    Component.onCompleted: Qt.callLater(function() { if (!window.hasDocument) window.showPicker() })

    KeyboardController { id: keyboard; objectName: "pdfKeyboard"; window: window }
    Timer {
        id: readingControlsTimer; interval: window.readingControlsTimeout
        onTriggered: window.readingControlsVisible = false
    }
    Shortcut {
        sequence: "Escape"; enabled: window.readingMode && !picker.visible
        onActivated: window.setReadingMode(false)
    }
    component PaperButton: Paper.Button {
        implicitHeight: Paper.Theme.control
        implicitWidth: 86 * window.u
    }


    Item {
        id: surface; objectName: "pdfSurface"
        width: window.hostViewport && window.hostViewport.available ? window.hostViewport.width : window.width
        height: window.hostViewport && window.hostViewport.available ? window.hostViewport.height : window.height
        transform: Matrix4x4 {
            matrix: window.hostViewport && window.hostViewport.available
                ? window.hostViewport.contentTransform : Qt.matrix4x4()
        }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: window.outerMargin
        spacing: (window.compact ? 6 : 12) * window.u
        RowLayout {
            objectName: "pdfHeader"; visible: !window.readingMode
            Layout.fillWidth: true; spacing: 8 * window.u
            Label {
                Layout.fillWidth: true; Layout.minimumWidth: 0
                text: reader.title || "rePDF"; textFormat: Text.PlainText
                elide: Text.ElideMiddle; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title
            }
            Paper.IconButton { objectName: "openPdfPicker"; text: "Ouvrir"; symbol: "folder"; onClicked: window.showPicker() }
            Paper.IconButton {
                objectName: "enterPdfFullscreen"; text: "Plein écran"; symbol: "fullscreen"
                visible: !window.condensedReader
                enabled: window.hasDocument; onClicked: window.setReadingMode(true)
            }
            Paper.IconButton { objectName: "closePdfReader"; text: "Fermer"; symbol: "close"; visible: !window.condensedReader; onClicked: Qt.quit() }
            Paper.IconButton {
                objectName: "pdfReaderMore"; text: "Plus"; symbol: "more"; visible: window.condensedReader
                onClicked: { keyboard.dismiss(); readerOptions.visible = true; readerOptions.forceActiveFocus() }
            }
        }
        RowLayout {
            objectName: "pdfNavigation"; visible: !window.readingMode
            Layout.fillWidth: true; spacing: 8 * window.u
            Paper.IconButton {
                objectName: "previousPdfPage"; text: "Préc."; symbol: "back"
                enabled: window.hasDocument && reader.pageNumber > 1
                onClicked: { reader.pageNumber -= 1; window.resetViewport() }
            }
            Paper.TextField {
                id: pageInput; objectName: "pdfPageNumber"
                Layout.preferredWidth: 60 * window.u
                enabled: window.hasDocument; horizontalAlignment: Text.AlignHCenter
                text: ""; selectByMouse: true; inputMethodHints: Qt.ImhDigitsOnly
                validator: IntValidator { bottom: 1; top: Math.max(1, reader.pageCount) }
                onEditingFinished: window.commitPageNumber()
                onActiveFocusChanged: if (!activeFocus) window.commitPageNumber()
            }
            Label {
                objectName: "pdfPageCount"; Layout.fillWidth: true; Layout.minimumWidth: 0
                text: "/ " + reader.pageCount; elide: Text.ElideRight
            }
            Paper.IconButton {
                objectName: "nextPdfPage"; text: "Suiv."; symbol: "next"
                enabled: window.hasDocument && reader.pageNumber < reader.pageCount
                onClicked: { reader.pageNumber += 1; window.resetViewport() }
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 24 * window.u
            color: "white"; border.color: Paper.Theme.divider; border.width: window.readingMode ? 0 : Paper.Theme.line
            Flickable {
                id: viewport; objectName: "pdfViewport"
                anchors.fill: parent; anchors.margins: parent.border.width; clip: true
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: Math.max(width, reader.width)
                contentHeight: Math.max(height, reader.height)
                ScrollBar.vertical: Paper.ScrollBar { policy: ScrollBar.AsNeeded }
                ScrollBar.horizontal: Paper.ScrollBar { policy: ScrollBar.AsNeeded }
                Rectangle {
                    x: reader.x; y: reader.y; width: reader.width; height: reader.height
                    color: "white"; visible: window.hasDocument
                }
                PdfView {
                    id: reader; objectName: "pdfView"
                    x: Math.max(0, (viewport.width - width) / 2)
                    y: 0
                    width: Math.max(1, viewport.width * window.zoom)
                    height: window.hasDocument ? width / Math.max(0.01, aspectRatio) : 0
                    visible: window.hasDocument
                    TapHandler {
                        enabled: window.readingMode && window.hasDocument && !picker.visible
                        acceptedButtons: Qt.LeftButton
                        gesturePolicy: TapHandler.DragThreshold
                        onTapped: window.toggleReadingControls()
                    }
                    onDocumentChanged: {
                        pageInput.text = window.hasDocument ? String(reader.pageNumber) : ""
                        window.resetViewport()
                    }
                    onViewChanged: {
                        if (!pageInput.activeFocus)
                            pageInput.text = window.hasDocument ? String(reader.pageNumber) : ""
                        window.resetViewport()
                    }
                }
            }
            Column {
                anchors.centerIn: parent; width: Math.max(0, parent.width - 40 * window.u)
                visible: !window.hasDocument; spacing: 12 * window.u
                Label {
                    width: parent.width; text: "rePDF"
                    font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title; horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
                PaperButton { anchors.horizontalCenter: parent.horizontalCenter; implicitWidth: 180 * window.u; text: "Choisir un PDF"; onClicked: window.showPicker() }
            }
        }
        GridLayout {
            objectName: "pdfFooter"; visible: !window.readingMode && !window.condensedReader
            Layout.fillWidth: true
            columns: window.narrow ? 3 : 6
            columnSpacing: 6 * window.u; rowSpacing: 6 * window.u
            PaperButton {
                objectName: "zoomOutPdf"; text: "−"; implicitWidth: Paper.Theme.control
                Layout.row: 0; Layout.column: 0
                enabled: window.hasDocument && window.zoom > 0.5
                onClicked: window.setZoom(window.zoom - 0.25)
            }
            Label { Layout.row: 0; Layout.column: 1; Layout.preferredWidth: 56 * window.u; text: Math.round(window.zoom * 100) + " %"; horizontalAlignment: Text.AlignHCenter }
            PaperButton {
                objectName: "zoomInPdf"; text: "+"; implicitWidth: Paper.Theme.control
                Layout.row: 0; Layout.column: 2
                enabled: window.hasDocument && window.zoom < 4
                onClicked: window.setZoom(window.zoom + 0.25)
            }
            Item { visible: !window.narrow; Layout.row: 0; Layout.column: 3; Layout.fillWidth: true; Layout.minimumWidth: 0 }
            Paper.IconButton {
                objectName: "fitPdfWidth"; text: "Ajuster"; symbol: "fit-width"
                Layout.row: window.narrow ? 1 : 0; Layout.column: window.narrow ? 0 : 4
                Layout.columnSpan: window.narrow ? 2 : 1; Layout.fillWidth: window.narrow
                enabled: window.hasDocument
                onClicked: window.setZoom(1)
            }
            Paper.IconButton {
                objectName: "rotatePdfPage"; text: "Rotation"; symbol: "rotate"
                Layout.row: window.narrow ? 1 : 0; Layout.column: window.narrow ? 2 : 5
                enabled: window.hasDocument; onClicked: reader.rotateClockwise()
            }
        }
        Label {
            objectName: "pdfReaderStatus"; Layout.fillWidth: true
            visible: !window.readingMode && !window.compact
            text: reader.error || (reader.busy ? "Chargement de la page…" : "")
            textFormat: Text.PlainText; wrapMode: Text.WordWrap; font.pixelSize: Paper.Theme.caption; color: Paper.Theme.muted
        }
        TouchKeyboard { controller: keyboard; Layout.fillWidth: true; visible: keyboard.fallbackVisible && !picker.visible && !window.readingMode }
    }

    Rectangle {
        objectName: "pdfReadingControls"
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: (window.shortReadingControls ? 4 : 12) * window.u
        width: Math.min(520 * window.u, Math.max(0, parent.width - 24 * window.u)); height: readingControls.implicitHeight + 16 * window.u
        visible: window.readingMode && window.readingControlsVisible && window.hasDocument
        color: "white"; border.color: "black"; border.width: Paper.Theme.line
        GridLayout {
            id: readingControls
            anchors.fill: parent; anchors.margins: 8 * window.u
            columns: window.narrowReadingControls ? 3 : 6
            columnSpacing: 6 * window.u; rowSpacing: (window.shortReadingControls ? 4 : 8) * window.u
                PaperButton {
                    objectName: "readingZoomOutPdf"; text: "−"; implicitWidth: Paper.Theme.control
                    Layout.row: window.narrowReadingControls ? 1 : 0; Layout.column: 0
                    enabled: window.zoom > 0.5
                    onClicked: { window.setZoom(window.zoom - 0.25); window.keepReadingControls() }
                }
                PaperButton {
                    objectName: "readingZoomInPdf"; text: "+"; implicitWidth: Paper.Theme.control
                    Layout.row: window.narrowReadingControls ? 1 : 0; Layout.column: 1
                    enabled: window.zoom < 4
                    onClicked: { window.setZoom(window.zoom + 0.25); window.keepReadingControls() }
                }
                Item { visible: !window.narrowReadingControls; Layout.row: 0; Layout.column: 2; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                Paper.IconButton {
                    objectName: "readingPreviousPdfPage"; text: "Préc."; symbol: "back"
                    Layout.row: 0; Layout.column: window.narrowReadingControls ? 0 : 3
                    enabled: reader.pageNumber > 1
                    onClicked: { reader.pageNumber -= 1; window.resetViewport(); window.keepReadingControls() }
                }
                Label {
                    Layout.row: 0; Layout.column: window.narrowReadingControls ? 1 : 4
                    Layout.preferredWidth: 54 * window.u; Layout.minimumWidth: 32 * window.u
                    text: reader.pageNumber + "/" + reader.pageCount
                    elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter
                }
                Paper.IconButton {
                    objectName: "readingNextPdfPage"; text: "Suiv."; symbol: "next"
                    Layout.row: 0; Layout.column: window.narrowReadingControls ? 2 : 5
                    enabled: reader.pageNumber < reader.pageCount
                    onClicked: { reader.pageNumber += 1; window.resetViewport(); window.keepReadingControls() }
                }
                Paper.IconButton {
                    objectName: "fitReadingPdfWidth"; text: "Ajuster"; symbol: "fit-width"
                    Layout.row: 1; Layout.column: window.narrowReadingControls ? 2 : 0
                    Layout.columnSpan: window.narrowReadingControls ? 1 : 2
                    onClicked: { window.setZoom(1); window.keepReadingControls() }
                }
                PaperButton {
                    objectName: "exitPdfFullscreen"; text: window.narrowReadingControls ? "Quitter la lecture" : "Quitter le plein écran"; Layout.fillWidth: true
                    Layout.row: window.narrowReadingControls ? 2 : 1; Layout.column: window.narrowReadingControls ? 0 : 2
                    Layout.columnSpan: window.narrowReadingControls ? 3 : 4
                    onClicked: window.setReadingMode(false)
                }
        }
    }

    FocusScope {
        id: readerOptions; objectName: "pdfReaderOptions"
        anchors.fill: parent; visible: false
        Keys.onEscapePressed: event => { visible = false; event.accepted = true }
        Rectangle { anchors.fill: parent; color: "white" }
        MouseArea { anchors.fill: parent; preventStealing: true; onWheel: wheel => wheel.accepted = true }
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 8 * window.u; spacing: 8 * window.u
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Options"; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
                Paper.IconButton { objectName: "closePdfReaderOptions"; text: "Fermer les options"; symbol: "close"; onClicked: readerOptions.visible = false }
            }
            ListView {
                objectName: "pdfReaderOptionsEntries"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                ScrollBar.vertical: Paper.ScrollBar { policy: ScrollBar.AsNeeded }
                model: [
                    {name: "compactEnterPdfFullscreen", label: "Plein écran"},
                    {name: "compactZoomInPdf", label: "Agrandir"},
                    {name: "compactZoomOutPdf", label: "Réduire"},
                    {name: "compactFitPdfWidth", label: "Ajuster à la largeur"},
                    {name: "compactRotatePdfPage", label: "Rotation"},
                    {name: "compactClosePdfReader", label: "Fermer"}
                ]
                delegate: Paper.Button {
                    required property var modelData
                    required property int index
                    objectName: modelData.name; text: modelData.label; quiet: true
                    width: ListView.view.width; height: Paper.Theme.control
                    enabled: index === 5 || (window.hasDocument && (index !== 1 || window.zoom < 4) && (index !== 2 || window.zoom > 0.5))
                    contentItem: Text {
                        text: parent.text; font: parent.font; textFormat: Text.PlainText
                        opacity: parent.enabled ? 1 : 0.35; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Paper.Theme.line; color: Paper.Theme.divider }
                    onClicked: {
                        readerOptions.visible = false
                        if (index === 0) window.setReadingMode(true)
                        else if (index === 1) window.setZoom(window.zoom + 0.25)
                        else if (index === 2) window.setZoom(window.zoom - 0.25)
                        else if (index === 3) window.setZoom(1)
                        else if (index === 4) reader.rotateClockwise()
                        else Qt.quit()
                    }
                }
            }
        }
    }

    FocusScope {
        id: picker; objectName: "pdfPicker"
        anchors.fill: parent; visible: false
        readonly property bool opened: visible
        readonly property alias contentItem: pickerPanel
        function open() { visible = true; forceActiveFocus() }
        function close() { visible = false; keyboard.dismiss() }
        Keys.onEscapePressed: event => { close(); event.accepted = true }
        MouseArea {
            anchors.fill: parent; preventStealing: true
            onWheel: wheel => wheel.accepted = true
        }
        Rectangle {
            id: pickerPanel; objectName: "pdfPickerPanel"
            readonly property real padding: (window.compact ? 8 : 24) * window.u
            x: 0; y: 0
            width: surface.width
            height: Math.max(1, surface.height - window.keyboardOcclusion)
            color: "white"
        ColumnLayout {
            anchors.fill: parent; anchors.margins: pickerPanel.padding
            spacing: (window.compact ? 6 : 12) * window.u
            RowLayout {
                Layout.fillWidth: true
                Label { text: window.narrow ? "PDF" : "Ouvrir un PDF"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title }
                Paper.IconButton { objectName: "pdfPickerClose"; text: "Fermer"; symbol: "close"; onClicked: picker.close() }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 8 * window.u
                PaperButton {
                    objectName: "pdfLibraryTab"; text: window.narrow ? "Tablette" : "Bibliothèque"; Layout.fillWidth: true; quiet: true
                    checkable: true; checked: !!window.library && window.library.mode === "library"
                    enabled: !!window.library && !window.library.busy
                    onClicked: { window.openError = ""; window.library.showLibrary() }
                }
                PaperButton {
                    objectName: "pdfFilesTab"; text: "Fichiers"; Layout.fillWidth: true; quiet: true
                    checkable: true; checked: !!window.library && window.library.mode === "files"
                    enabled: !!window.library && !window.library.busy
                    onClicked: { window.openError = ""; window.library.showFiles() }
                }
            }
            Label {
                visible: !window.compact
                text: window.library ? window.library.locationTitle : ""; textFormat: Text.PlainText
                Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideMiddle
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 8 * window.u
                Paper.IconButton {
                    objectName: "pdfPickerBack"; text: "Retour"; symbol: "back"
                    enabled: !!window.library && window.library.canGoBack && !window.library.busy
                    onClicked: { window.openError = ""; window.library.goUp() }
                }
                Paper.TextField {
                    id: filterField; objectName: "pdfPickerFilter"
                    Layout.fillWidth: true; Layout.minimumWidth: 0; selectByMouse: true
                    placeholderText: window.narrow ? "Filtrer" : "Rechercher un PDF ou un dossier"
                    text: window.library ? window.library.filter : ""
                    onTextEdited: if (window.library) window.library.filter = text
                    onAccepted: keyboard.dismiss()
                }
                Paper.IconButton {
                    objectName: "pdfPickerRefresh"; text: "Actualiser"; symbol: "refresh"
                    enabled: !!window.library && !window.library.busy
                    onClicked: window.library.refresh()
                }
            }
            Label {
                objectName: "pdfPickerError"; Layout.fillWidth: true
                text: window.openError || (window.library ? window.library.error : "")
                textFormat: Text.PlainText; visible: text.length > 0; wrapMode: Text.WordWrap
            }
            ListView {
                id: entries; objectName: "pdfPickerEntries"
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: (window.compact ? 24 : 48) * window.u
                clip: true; spacing: 0; model: window.library
                ScrollBar.vertical: Paper.ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: ItemDelegate {
                    id: entry
                    required property int index
                    required property string entryId
                    required property string title
                    required property string subtitle
                    required property string filePath
                    required property bool folder
                    required property bool available
                    objectName: "pdfEntry_" + index
                    width: ListView.view.width; height: 76 * window.u
                    leftPadding: 0; rightPadding: 0; topPadding: 12 * window.u; bottomPadding: topPadding
                    enabled: (folder || available) && !!window.library && !window.library.busy
                    contentItem: RowLayout {
                        spacing: 16 * window.u
                        Paper.Icon { name: entry.folder ? "folder" : "document"; ink: entry.enabled ? "black" : Paper.Theme.muted; Layout.preferredWidth: Paper.Theme.icon; Layout.preferredHeight: Paper.Theme.icon }
                        ColumnLayout {
                            Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 4 * window.u
                            Label {
                                Layout.fillWidth: true; Layout.minimumWidth: 0
                                text: entry.title; textFormat: Text.PlainText; elide: Text.ElideRight
                                font.pixelSize: Paper.Theme.body; color: entry.enabled ? "black" : Paper.Theme.muted
                            }
                            Label {
                                Layout.fillWidth: true; Layout.minimumWidth: 0
                                text: !entry.folder && !entry.available ? (entry.subtitle || "PDF indisponible") : entry.subtitle
                                textFormat: Text.PlainText; elide: Text.ElideRight; font.pixelSize: Paper.Theme.caption; color: Paper.Theme.muted
                            }
                        }
                    }
                    background: Rectangle {
                        color: entry.down ? "#f3f3f3" : "white"
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Paper.Theme.line; color: Paper.Theme.divider }
                    }
                    onClicked: {
                        window.openError = ""
                        if (folder) window.library.openFolder(entryId)
                        else window.openDocument(filePath, title)
                    }
                }
                Label {
                    anchors.centerIn: parent; width: Math.max(0, parent.width - 24 * window.u)
                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                    visible: entries.count === 0
                    text: window.library && window.library.busy ? "Chargement…"
                        : filterField.text.length ? "Aucun résultat pour cette recherche." : "Aucun PDF dans ce dossier."
                    color: Paper.Theme.muted
                }
            }
            TouchKeyboard { controller: keyboard; Layout.fillWidth: true }
        }
        }
    }
    }
}

import QtQuick 2.15

// UI contract fixture only. Real parsing/rendering is covered by PdfViewTest.
Item {
    property string source: ""
    property string title: ""
    property string error: ""
    property bool busy: false
    property int pageNumber: 0
    property int pageCount: 0
    property real aspectRatio: 0.75
    property int rotation: 0
    property bool rejectOpen: false
    property int openCalls: 0
    property string lastPath: ""
    property string lastTitle: ""
    signal documentChanged()
    signal viewChanged()
    onPageNumberChanged: viewChanged()
    function openFile(path, displayName) {
        ++openCalls; lastPath = path; lastTitle = displayName
        if (rejectOpen) { error = "Le PDF est illisible."; return false }
        source = path; title = displayName || "Document PDF"
        pageCount = 5; pageNumber = 1; rotation = 0; error = ""
        documentChanged()
        return true
    }
    function closeDocument() {
        source = ""; title = ""; pageCount = 0; pageNumber = 0; error = ""
        documentChanged()
    }
    function rotateClockwise() {
        rotation = (rotation + 90) % 360
        aspectRatio = 1 / aspectRatio
        viewChanged()
    }
}

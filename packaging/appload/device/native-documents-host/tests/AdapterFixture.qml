import QtQuick
import net.asivery.AppLoad 1.0

RePaperNativeDocuments {
    id: adapter
    socketPath: testSocketPath
    stateDirectory: testStateDirectory
    library: fakeLibrary
    libraryController: fakeLibraryController
    documentController: fakeDocumentController
    deviceScreenInfo: fakeDeviceScreenInfo
    windowNavigator: fakeNavigator
    explorer: fakeExplorer
    documentViewLoader: fakeLoader

    property QtObject fakeLibrary: QtObject {
        property bool isReady: true
        property var entries: ({})
        function entryForId(id) { return entries[id] || null }
    }
    property QtObject fakeLibraryController: QtObject {
        property int createCalls: 0
        property int orientationCalls: 0
        property int coverCalls: 0
        property string requestedPage: ""
        property bool rejectCreation: false
        function createDocument(folder, title, id, page) {
            createCalls++
            requestedPage = page
            if (rejectCreation) return ""
            fakeLibrary.entries[id] = { id: id, visibleName: title, lastOpenedPage: 0 }
            return id
        }
        function setOrientation(id, orientation) { orientationCalls++ }
        function setCoverPageNumber(id, page) { coverCalls++ }
    }
    property QtObject fakeDocumentController: QtObject {
        property int templateCalls: 0
        property string templateName: ""
        function setTemplateForPage(id, page, template, paperSize) { templateCalls++; templateName = template }
    }
    property QtObject fakeDeviceScreenInfo: QtObject { property size paperSize: Qt.size(1620, 2160) }
    property QtObject fakeExplorer: QtObject { property string currentFolderId: "root" }
    property QtObject fakeNavigator: QtObject {
        property int openCalls: 0
        property string lastRoute: ""
        property string lastId: ""
        property bool confirmImmediately: true
        function open(route, args) {
            openCalls++
            lastRoute = route
            lastId = args.documentId
            if (confirmImmediately) fakeLoader.item = { documentLoaded: true, document: fakeLibrary.entryForId(args.documentId) }
        }
    }
    property QtObject fakeLoader: QtObject { property var item: null }
}

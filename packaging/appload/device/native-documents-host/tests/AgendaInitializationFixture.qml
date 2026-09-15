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
    agendaPage: testAgendaPage
    agendaHeader: fakeAgendaHeader
    property string callTrace: ""

    property QtObject fakeAgendaHeader: QtObject {
        property bool busy: false
        property string reason: "SYNTHETIC_HEADER_FAILURE"
        property bool autoPrepared: true
        property bool autoFinished: true
        property bool allowPrepare: true
        property bool allowCommit: true
        property bool finishSuccess: true
        property bool failInspection: false
        property bool synchronous: false
        property int prepareCalls: 0
        property int commitCalls: 0
        property int nativeInserts: 0
        property int cancelCalls: 0
        property int epoch: 0
        property string requestId: ""
        property string hash: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        property string state: "empty"
        property string day: ""
        property string date: ""
        property string time: ""
        signal prepared(string request, var receipt)
        signal finished(string request, bool success, var receipt)
        function prepare(request, controller, context, fields, pageSize) {
            prepareCalls++
            if (!allowPrepare || busy) return false
            busy = true; requestId = request; epoch++
            day = fields.day; date = fields.date; time = fields.time
            adapter.callTrace += "|header-prepare"
            const operation = epoch
            if (autoPrepared) {
                if (synchronous) deliverPrepared()
                else Qt.callLater(function() { if (busy && epoch === operation) deliverPrepared() })
            }
            return true
        }
        function deliverPrepared() {
            if (!busy) return
            if (failInspection) {
                busy = false
                finished(requestId, false, { code: reason, planHash: hash })
                return
            }
            adapter.callTrace += "|header-prepared"
            prepared(requestId, { state: state, planHash: hash })
        }
        function commit(request) {
            commitCalls++
            if (!allowCommit || !busy || request !== requestId) return false
            adapter.callTrace += "|header-commit"
            if (state === "empty") nativeInserts++
            const operation = epoch
            if (autoFinished) {
                if (synchronous) deliverFinished()
                else Qt.callLater(function() { if (busy && epoch === operation) deliverFinished() })
            }
            return true
        }
        function deliverFinished() {
            if (!busy) return
            busy = false
            adapter.callTrace += "|header-finished"
            const result = state === "empty" ? "inserted" : "already-present"
            if (finishSuccess) state = "already-present"
            finished(requestId, finishSuccess, { state: result, planHash: hash, code: reason })
        }
        function cancel(request) {
            if (request !== requestId || !busy) return
            busy = false; epoch++; cancelCalls++
            adapter.callTrace += "|header-cancel"
        }
    }

    function installDocument(id, title, page, orientation, templateName) {
        const entry = { id: id, visibleName: title, lastOpenedPage: 0, pageId: page,
            orientation: orientation, templateName: templateName, isTrashed: false,
            templateForPage: function(index) { return this.templateName } }
        fakeLibrary.entries[id] = entry
        fakeLibrary.parents[id] = fakeLibrary.folderId
        return entry
    }
    function restoreDocument(id, page) { installDocument(id, "Notes conservées", page, Qt.Vertical, "P Day") }

    property QtObject fakeLibrary: QtObject {
        property bool isReady: true
        property string folderId: "d3f462b1-8a5e-420f-a6e2-7d051cf07417"
        property var entries: ({})
        property var parents: ({})
        property int loadCalls: 0
        property string requestedTemplateId: ""
        property bool autoLoadTemplate: true
        function requestLoadEntry(id) {
            loadCalls++
            requestedTemplateId = adapter.canonicalId(id)
            adapter.callTrace += "|load"
            if (autoLoadTemplate) Qt.callLater(finishTemplateLoad)
        }
        function finishTemplateLoad() {
            if (!requestedTemplateId.length) return
            entries[requestedTemplateId] = { id: requestedTemplateId, isTrashed: false, isTemplate: true }
            adapter.callTrace += "|template-loaded"
        }
        function entryForId(id) {
            if (id === folderId) return { id: folderId, isTrashed: false }
            return entries[id] || null
        }
        function meetingNotesId() { return folderId }
        function parentIdForId(id) { return id === folderId ? "root" : (parents[id] || "root") }
    }
    property QtObject fakeLibraryController: QtObject {
        property int createCalls: 0
        property int orientationCalls: 0
        property int coverCalls: 0
        property int moveCalls: 0
        function createDocument(folder, title, id, page) {
            createCalls++
            adapter.installDocument(id, title, page, Qt.Vertical, "Blank")
            fakeLibrary.parents[id] = folder
            return id
        }
        function setOrientation(id, orientation) {
            orientationCalls++
            fakeLibrary.entries[id].orientation = orientation
        }
        function setCoverPageNumber(id, page) { coverCalls++ }
        function moveEntries(ids, folder) {
            moveCalls++
            for (const id of ids) fakeLibrary.parents[id] = folder
            return true
        }
    }
    property QtObject fakeDocumentController: QtObject {
        property int templateCalls: 0
        property int customTemplateCalls: 0
        property int unloadedTemplateCalls: 0
        property bool autoApplyTemplate: true
        property bool allowTemplate: true
        property string pendingDocument: ""
        property string pendingTemplate: ""
        function setTemplateForPage(id, page, templateName, paperSize) {
            templateCalls++
            if (templateName.indexOf(":p") >= 0) {
                customTemplateCalls++
                if (!fakeLibrary.entryForId(templateName.slice(0, -2))) {
                    unloadedTemplateCalls++
                    return false
                }
                if (!allowTemplate) return false
                adapter.callTrace += "|apply"
                pendingDocument = id; pendingTemplate = templateName
                fakeWorker.jobQueueSize++
                fakeSceneController.working = true
                if (autoApplyTemplate) Qt.callLater(finishTemplateApply)
                return true
            }
            fakeLibrary.entries[id].templateName = templateName
            return true
        }
        function finishTemplateApply() {
            if (!pendingDocument.length) return
            fakeLibrary.entries[pendingDocument].templateName = pendingTemplate
            pendingDocument = ""; pendingTemplate = ""
            fakeWorker.jobQueueSize = 0
            fakeSceneController.working = false
            adapter.callTrace += "|template-applied"
        }
    }
    property QtObject fakeDeviceScreenInfo: QtObject { property size paperSize: Qt.size(1620, 2160) }
    property QtObject fakeExplorer: QtObject { property string currentFolderId: "root" }
    property QtObject fakeWorker: QtObject { property int jobQueueSize: 0 }
    property QtObject fakeSceneController: QtObject {
        property string pageId: ""
        property string currentLayer: "layer-1"
        property bool working: false
        property var worker: fakeWorker
    }
    property QtObject fakeView: QtObject {
        property bool documentLoaded: true
        property bool isLoading: false
        property var document: null
        property string currentPageId: ""
        property int currentPage: 0
        property var sceneController: fakeSceneController
    }
    property QtObject fakeNavigator: QtObject {
        property int openCalls: 0
        property bool wrongPage: false
        property bool wrongDocument: false
        function open(route, args) {
            openCalls++
            fakeView.document = fakeLibrary.entryForId(args.documentId)
            fakeView.currentPageId = wrongPage ? "fc084b72-b559-4255-adbb-bf1f2d44a854" : fakeView.document.pageId
            fakeSceneController.pageId = fakeView.currentPageId
            if (wrongDocument) fakeView.document = { id: "92daa7c7-2fdd-49c6-91f9-c167f3534ea4" }
            fakeLoader.item = fakeView
        }
    }
    property QtObject fakeLoader: QtObject { property var item: null }
}

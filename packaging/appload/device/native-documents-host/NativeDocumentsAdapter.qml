import QtQuick
import net.asivery.AppLoad 1.0

NativeDocumentHost {
    id: adapter
    required property var library
    required property var libraryController
    required property var documentController
    required property var deviceScreenInfo
    required property var windowNavigator
    required property var explorer
    required property var documentViewLoader
    property var documentImporter: null
    // Supplied by the AppLoad hook. Older notebook clients do not need it.
    property var agendaPage: null
    property var agendaHeader: null

    importsEnabled: nativeImports.available
    property QtObject nativeImports: RePaperNativeImports {
        host: adapter
        library: adapter.library
        libraryController: adapter.libraryController
        documentImporter: adapter.documentImporter
        explorer: adapter.explorer
    }

    enabled: !!library && !!library.isReady && !!libraryController
        && typeof libraryController.createDocument === "function"
        && !!documentController && !!deviceScreenInfo && !!windowNavigator && !!explorer

    property string pendingOpenRequest: ""
    property string pendingOpenDocument: ""
    property var pendingAgendaContext: ({})
    property string pendingAgendaPhase: ""
    property string pendingAgendaHash: ""
    property string pendingHeaderHash: ""
    property int openChecks: 0
    property Timer openConfirmation: Timer {
        interval: 100
        repeat: true
        onTriggered: adapter.checkOpenedDocument()
    }
    property Connections agendaResults: Connections {
        target: adapter.agendaPage
        ignoreUnknownSignals: true
        function onPrepared(request, receipt) {
            if (request !== adapter.pendingOpenRequest || adapter.pendingAgendaPhase !== "preparing") return
            if (!adapter.agendaTargetCurrent()) {
                adapter.failOpening("NATIVE_AGENDA_PAGE_CHANGED", "La page a changé pendant sa préparation. Rouvrez la note pour réessayer.")
                return
            }
            if (!receipt || (receipt.state !== "empty" && receipt.state !== "already-present")
                    || typeof receipt.planHash !== "string") {
                adapter.failOpening("NATIVE_AGENDA_PREPARATION_FAILED", "La page d’agenda n’a pas pu être préparée.")
                return
            }
            adapter.pendingAgendaHash = receipt.planHash
            if (!adapter.beginAgendaPage(request, receipt.planHash)) { adapter.clearOpening(); return }
            adapter.pendingAgendaPhase = "committing"
            // already-present is also committed: the helper releases its
            // prepared operation and confirms the existing text without inserting.
            if (!adapter.agendaPage.commit(request) && request === adapter.pendingOpenRequest)
                adapter.failOpening("NATIVE_AGENDA_COMMIT_FAILED", "Le carnet est créé, mais son préremplissage n’a pas été confirmé. Réessayez la même note.")
        }
        function onFinished(request, success, receipt) {
            if (request !== adapter.pendingOpenRequest) return
            if (!success || adapter.pendingAgendaPhase !== "committing" || !receipt
                    || receipt.planHash !== adapter.pendingAgendaHash || !adapter.agendaTargetCurrent()) {
                adapter.failOpening("NATIVE_AGENDA_PREFILL_FAILED", "Le carnet est créé, mais son préremplissage n’a pas été confirmé. Réessayez la même note.")
                return
            }
            // The title changes native history. Inspect again before inserting
            // header ink, keeping the durable dispatch pending until both finish.
            adapter.prepareHeader("header-preparing")
        }
    }

    property Connections headerResults: Connections {
        target: adapter.agendaHeader
        ignoreUnknownSignals: true
        function onPrepared(request, receipt) {
            if (request !== adapter.pendingOpenRequest
                    || (adapter.pendingAgendaPhase !== "header-check" && adapter.pendingAgendaPhase !== "header-preparing")) return
            if (!adapter.agendaTargetCurrent() || !receipt
                    || (receipt.state !== "empty" && receipt.state !== "already-present")
                    || typeof receipt.planHash !== "string" || !/^[0-9a-f]{64}$/.test(receipt.planHash)) {
                adapter.failOpening("NATIVE_AGENDA_HEADER_FAILED", "L’en-tête de la page d’agenda n’a pas pu être vérifié.")
                return
            }
            if (adapter.pendingAgendaPhase === "header-check") {
                adapter.pendingHeaderHash = receipt.planHash
                adapter.agendaHeader.cancel(request)
                const view = adapter.documentViewLoader.item
                adapter.pendingAgendaPhase = "preparing"
                const fields = Object.assign({}, adapter.pendingAgendaContext.fields,
                    { headerPrepared: true, headerPlanHash: adapter.pendingHeaderHash })
                if (!adapter.agendaPage.prepare(request, view.sceneController, adapter.pageContext(view),
                        fields, adapter.deviceScreenInfo.paperSize) && request === adapter.pendingOpenRequest)
                    adapter.failOpening(adapter.agendaPage.reason || "NATIVE_AGENDA_PREPARATION_FAILED", "La page d’agenda n’a pas pu être préparée. Réessayez la même note.")
                return
            }
            if (receipt.planHash !== adapter.pendingHeaderHash) {
                adapter.failOpening("NATIVE_AGENDA_HEADER_CHANGED", "L’en-tête de la page a changé pendant sa préparation.")
                return
            }
            adapter.pendingAgendaPhase = "header-committing"
            if (!adapter.agendaHeader.commit(request) && request === adapter.pendingOpenRequest)
                adapter.failOpening(adapter.agendaHeader.reason || "NATIVE_AGENDA_HEADER_FAILED", "L’en-tête du carnet n’a pas pu être confirmé. Réessayez la même note.")
        }
        function onFinished(request, success, receipt) {
            if (request !== adapter.pendingOpenRequest) return
            if (!success || adapter.pendingAgendaPhase !== "header-committing" || !receipt
                    || receipt.planHash !== adapter.pendingHeaderHash || !adapter.agendaTargetCurrent()) {
                adapter.failOpening(receipt && receipt.code ? receipt.code : "NATIVE_AGENDA_HEADER_FAILED", "L’en-tête du carnet n’a pas pu être confirmé. Réessayez la même note.")
                return
            }
            if (!adapter.completeAgendaPage(request, adapter.pendingAgendaHash)) { adapter.clearOpening(); return }
            adapter.finishOpening()
        }
    }
    function pageContext(view) {
        return { documentId: pendingOpenDocument, pageId: canonicalId(pendingAgendaContext.pageId),
            layer: view.sceneController.currentLayer, documentView: view }
    }
    function prepareHeader(phase) {
        const request = pendingOpenRequest, view = documentViewLoader.item
        pendingAgendaPhase = phase
        if (!agendaHeader.prepare(request, view.sceneController, pageContext(view),
                pendingAgendaContext.fields, deviceScreenInfo.paperSize) && request === pendingOpenRequest)
            failOpening(agendaHeader.reason || "NATIVE_AGENDA_HEADER_FAILED", "L’en-tête du carnet n’a pas pu être préparé. Réessayez la même note.")
    }

    function canonicalId(value) {
        return value === null || value === undefined ? "" : value.toString().replace(/[{}]/g, "").toLowerCase()
    }

    function folderForNote(request, context) {
        if (context.agenda) {
            if (typeof library.meetingNotesId !== "function" || typeof library.parentIdForId !== "function"
                    || typeof libraryController.moveEntries !== "function") {
                adapter.rejected(request, "NATIVE_AGENDA_FOLDER_UNAVAILABLE", "Le dossier natif des notes d’agenda n’est pas disponible.")
                return ""
            }
            // This is the same native, idempotent folder getter used by
            // CalendarActions.qml. Xochitl owns its identity and persistence.
            const folder = canonicalId(library.meetingNotesId())
            const entry = folder.length ? library.entryForId(folder) : null
            if (!entry || canonicalId(entry.id) !== folder || entry.isTrashed
                    || canonicalId(library.parentIdForId(entry.id)) === "trash") {
                adapter.rejected(request, "NATIVE_AGENDA_FOLDER_UNAVAILABLE", "Le dossier natif des notes d’agenda n’a pas pu être retrouvé.")
                return ""
            }
            return folder
        }
        const value = explorer.currentFolderId
        // The native notebook dialog converts navigation::EntityId to QString
        // before Qt converts its createDocument argument to entry::Id.
        const folder = value === null || value === undefined ? "" : value.toString()
        if (!folder.length) adapter.rejected(request, "NATIVE_FOLDER_UNAVAILABLE", "Ouvrez un dossier de la bibliothèque avant de créer une note.")
        return folder
    }

    function clearOpening() {
        const request = pendingOpenRequest
        pendingOpenRequest = ""
        pendingOpenDocument = ""
        pendingAgendaContext = ({})
        pendingAgendaPhase = ""
        pendingAgendaHash = ""
        pendingHeaderHash = ""
        openConfirmation.stop()
        if (request.length && agendaPage && typeof agendaPage.cancel === "function") agendaPage.cancel(request)
        if (request.length && agendaHeader && typeof agendaHeader.cancel === "function") agendaHeader.cancel(request)
    }
    function failOpening(code, message) {
        const request = pendingOpenRequest
        clearOpening()
        if (request.length) adapter.rejected(request, code, message)
    }
    function finishOpening() {
        const request = pendingOpenRequest, document = pendingOpenDocument
        clearOpening()
        if (request.length) adapter.opened(request, document)
    }
    function agendaTargetCurrent() {
        const view = documentViewLoader ? documentViewLoader.item : null
        return !!view && !!view.documentLoaded && !!view.document
            && canonicalId(view.document.id) === pendingOpenDocument
            && canonicalId(view.currentPageId) === canonicalId(pendingAgendaContext.pageId)
            && !!view.sceneController
            && canonicalId(view.sceneController.pageId) === canonicalId(pendingAgendaContext.pageId)
            && (!pendingAgendaPhase.length || (view.document.orientation === Qt.Vertical
                && typeof view.document.templateForPage === "function"
                && view.document.templateForPage(view.currentPage).toString() === "P Day"))
    }
    function checkOpenedDocument() {
        if (!pendingOpenRequest.length) { openConfirmation.stop(); return }
        if (++openChecks >= 120) {
            failOpening("NATIVE_OPEN_NOT_CONFIRMED", "La bibliothèque n’a pas confirmé l’ouverture et la préparation du carnet.")
            return
        }
        const view = documentViewLoader ? documentViewLoader.item : null
        if (pendingAgendaPhase.length && !agendaTargetCurrent()) {
            failOpening("NATIVE_AGENDA_PAGE_CHANGED", "La page a changé pendant sa préparation. Rouvrez la note pour réessayer.")
            return
        }
        if (view && view.documentLoaded && view.document
                && canonicalId(view.document.id) === pendingOpenDocument) {
            if (!pendingAgendaContext.initializePage) { finishOpening(); return }
            if (agendaTargetCurrent() && !view.isLoading && !view.sceneController.working && !pendingAgendaPhase.length) {
                if (!agendaPage || typeof agendaPage.prepare !== "function" || typeof agendaPage.commit !== "function"
                        || !agendaHeader || typeof agendaHeader.prepare !== "function" || typeof agendaHeader.commit !== "function") {
                    failOpening("NATIVE_AGENDA_PREFILL_UNAVAILABLE", "Le service de préremplissage des notes d’agenda n’est pas disponible.")
                    return
                }
                if (agendaPage.busy || agendaHeader.busy) {
                    failOpening("NATIVE_AGENDA_BUSY", "Une autre page d’agenda est en cours de préparation. Réessayez dans un instant.")
                    return
                }
                if (view.document.orientation !== Qt.Vertical || typeof view.document.templateForPage !== "function"
                        || view.document.templateForPage(view.currentPage).toString() !== "P Day") {
                    failOpening("NATIVE_AGENDA_TEMPLATE_CHANGED", "Appliquez le modèle Agenda journée en portrait à la première page du carnet, puis réessayez.")
                    return
                }
                if (!view.sceneController.worker || view.sceneController.worker.jobQueueSize !== 0) return
                prepareHeader("header-check")
            }
        }
    }

    onEnabledChanged: {
        if (!enabled) clearOpening()
    }

    onCreateRequested: (request, documentId, pageId, displayName) => {
        try {
            const context = adapter.agendaContext(request)
            // A reserved UUID survives a lost reply or process restart. Never
            // recreate or reformat an existing note, including one renamed by its user.
            const existing = library.entryForId(documentId)
            if (existing && canonicalId(existing.id) === documentId) {
                if (context.agenda) {
                    const folder = folderForNote(request, context)
                    if (!folder.length) return
                    const parent = canonicalId(library.parentIdForId(existing.id))
                    if (existing.isTrashed || parent === "trash") {
                        adapter.rejected(request, "NATIVE_AGENDA_NOTE_TRASHED", "Ce carnet est dans la corbeille. Restaurez-le avant de l’ouvrir depuis l’agenda.")
                        return
                    }
                    // QList<entry::Id> in Qt 6.10 requires the real entry::Id
                    // gadget for each element; a JS list of UUID strings loses it.
                    if (parent !== folder && (!libraryController.moveEntries([existing.id], folder)
                            || canonicalId(library.parentIdForId(existing.id)) !== folder)) {
                        adapter.rejected(request, "NATIVE_AGENDA_MOVE_FAILED", "Le carnet n’a pas pu être rangé dans le dossier des notes d’agenda.")
                        return
                    }
                }
                adapter.created(request, documentId)
                return
            }
            const folder = folderForNote(request, context)
            if (!folder.length) return
            // The four-argument overload is verified in ferrari 3.28.0.169's
            // Qt metadata. Xochitl creates its own notebook and page structures.
            const id = libraryController.createDocument(folder, displayName, documentId, pageId)
            const document = library.entryForId(documentId)
            if (canonicalId(id) !== documentId || !document || canonicalId(document.id) !== documentId) {
                adapter.rejected(request, "NATIVE_CREATE_NOT_CONFIRMED", "La bibliothèque n’a pas confirmé la création du carnet.")
                return
            }
            libraryController.setOrientation(document.id, Qt.Vertical)
            documentController.setTemplateForPage(document.id, document.lastOpenedPage,
                context.agenda ? "P Day" : "Blank", deviceScreenInfo.paperSize)
            libraryController.setCoverPageNumber(document.id, -1)
            adapter.created(request, documentId)
        } catch (error) {
            // Do not log notebook titles or native error payloads.
            adapter.rejected(request, "NATIVE_CREATE_FAILED", "Le carnet n’a pas pu être créé par reMarkable.")
        }
    }

    onOpenRequested: (request, documentId) => {
        if (pendingOpenRequest.length) {
            adapter.rejected(request, "NATIVE_OPEN_BUSY", "Un carnet est déjà en cours d’ouverture.")
            return
        }
        try {
            const document = library.entryForId(documentId)
            if (!document || canonicalId(document.id) !== documentId) {
                adapter.rejected(request, "NATIVE_DOCUMENT_MISSING", "Ce carnet n’est plus disponible dans la bibliothèque.")
                return
            }
            pendingOpenRequest = request
            pendingOpenDocument = documentId
            pendingAgendaContext = adapter.agendaContext(request)
            pendingAgendaPhase = ""
            pendingAgendaHash = ""
            pendingHeaderHash = ""
            openChecks = 0
            windowNavigator.open("legacydevice/window/main", { documentId: documentId })
            openConfirmation.start()
            checkOpenedDocument()
        } catch (error) {
            clearOpening()
            adapter.rejected(request, "NATIVE_OPEN_FAILED", "Le carnet n’a pas pu être ouvert par reMarkable.")
        }
    }
}

import QtQuick

QtObject {
    id: adapter
    required property var host
    required property var library
    required property var libraryController
    required property var documentImporter
    required property var explorer

    readonly property bool available: !!host && host.enabled && !!library && !!library.isReady
        && typeof library.entryForId === "function" && !!libraryController
        && typeof libraryController.setVisibleName === "function" && !!documentImporter
        && typeof documentImporter.importFromUrls === "function" && !!explorer
    property var jobs: ({})
    property Timer confirmation: Timer {
        interval: 100
        repeat: true
        onTriggered: adapter.checkJobs()
    }
    property Connections requests: Connections {
        target: adapter.host
        function onImportRequested(key, sourceUrl, token, documentId, displayName, dispatched, completed) {
            adapter.requestImport(key, sourceUrl, token, documentId, displayName, dispatched, completed)
        }
    }
    property Connections entries: Connections {
        target: adapter.library
        ignoreUnknownSignals: true
        function onEntryImported(visibleName, id) {
            for (const key of Object.keys(adapter.jobs)) {
                const job = adapter.jobs[key]
                if (!adapter.isToken(visibleName, job.token)) continue
                const documentId = adapter.canonicalId(id)
                if (!documentId.length || !adapter.host.identifyImport(key, documentId)) {
                    adapter.forget(key)
                    continue
                }
                job.documentId = documentId
                adapter.confirm(key)
            }
        }
    }
    property Connections imports: Connections {
        target: adapter.documentImporter
        ignoreUnknownSignals: true
        function onFailed(sourceUrl) {
            const url = adapter.urlString(sourceUrl)
            for (const key of Object.keys(adapter.jobs)) {
                if (adapter.jobs[key].url !== url) continue
                adapter.host.failImport(key, "NATIVE_IMPORT_FAILED", "reMarkable n’a pas pu importer ce document.")
                adapter.forget(key)
            }
        }
        function onFinished(sourceUrl) {
            const url = adapter.urlString(sourceUrl)
            for (const key of Object.keys(adapter.jobs)) {
                if (adapter.jobs[key].url !== url) continue
                adapter.jobs[key].finished = true
                adapter.confirmation.start()
            }
        }
    }

    onAvailableChanged: {
        if (!available) {
            jobs = ({})
            confirmation.stop()
        }
    }

    function canonicalId(value) {
        return value === null || value === undefined ? "" : value.toString().replace(/[{}]/g, "").toLowerCase()
    }
    function urlString(value) {
        return value === null || value === undefined ? "" : Qt.resolvedUrl(value).toString()
    }
    function isToken(value, token) {
        return value === token || value === token + ".pdf"
    }
    function forget(key) {
        delete jobs[key]
        if (!Object.keys(jobs).length) confirmation.stop()
    }
    function uncertain(key) {
        host.failImport(key, "NATIVE_IMPORT_UNCERTAIN", "L’import attend une confirmation de la bibliothèque reMarkable. Réessayez pour vérifier son état.")
        forget(key)
    }
    function requestImport(key, sourceUrl, token, documentId, displayName, dispatched, completed) {
        if (!available) {
            host.failImport(key, "NATIVE_IMPORT_UNAVAILABLE", "L’import natif n’est pas disponible.")
            return
        }
        if (jobs[key]) return
        const job = { url: urlString(sourceUrl), token: token, documentId: canonicalId(documentId),
            title: displayName, completed: completed, finished: false, checks: 0, metadataChecks: 0 }
        jobs[key] = job
        try {
            if (job.documentId.length) {
                confirmation.start()
                confirm(key)
                return
            }
            if (dispatched) {
                // A lost reply must never result in a second native import.
                job.documentId = canonicalId(host.findImportedDocument(key))
                if (!job.documentId.length) { uncertain(key); return }
                if (!host.identifyImport(key, job.documentId)) { forget(key); return }
                confirmation.start()
                confirm(key)
                return
            }
            const value = explorer.currentFolderId
            // As in the native notebook window, convert navigation::EntityId
            // to QString before Qt converts the argument to entry::Id.
            const folder = value === null || value === undefined ? "" : value.toString()
            if (!folder.length) {
                host.failImport(key, "NATIVE_FOLDER_UNAVAILABLE", "Ouvrez un dossier de la bibliothèque avant d’importer un document.")
                forget(key)
                return
            }
            if (!host.dispatchImport(key)) { forget(key); return }
            confirmation.start()
            // DocumentImporter owns the document format and indexing. Its Qt
            // metadata declares QList<QUrl>, entry::Id for this public method.
            if (!documentImporter.importFromUrls([Qt.resolvedUrl(sourceUrl)], folder) && jobs[key]) uncertain(key)
        } catch (error) {
            if (jobs[key]) uncertain(key)
        }
    }
    function confirm(key) {
        const job = jobs[key]
        if (!job || !job.documentId.length) return
        try {
            const entry = library.entryForId(job.documentId)
            if (!entry || canonicalId(entry.id) !== job.documentId) return
            // Persist the native ID before changing the temporary visible name.
            if (!host.identifyImport(key, job.documentId)) { forget(key); return }
            if (!job.completed && isToken(entry.visibleName, job.token)
                    && !libraryController.setVisibleName(job.documentId, job.title)) {
                host.failImport(key, "NATIVE_IMPORT_NAME_FAILED", "Le document a été importé, mais son titre n’a pas pu être appliqué.")
                forget(key)
                return
            }
            host.completeImport(key, job.documentId)
            forget(key)
        } catch (error) {
            if (jobs[key]) uncertain(key)
        }
    }
    function checkJobs() {
        let needsPolling = false
        for (const key of Object.keys(jobs)) {
            const job = jobs[key]
            if (job.documentId.length) {
                needsPolling = true
                confirm(key)
                if (jobs[key] && ++job.checks >= 120) uncertain(key)
            } else if (job.finished && job.metadataChecks < 200) {
                needsPolling = true
                if (job.metadataChecks++ % 10 !== 0) continue
                job.documentId = canonicalId(host.findImportedDocument(key))
                if (job.documentId.length) {
                    if (!host.identifyImport(key, job.documentId)) { forget(key); continue }
                    confirm(key)
                }
            }
            // Retain the listener for late entryImported/failed callbacks even
            // after a caller disconnects or the HTTP request times out.
        }
        if (!needsPolling) confirmation.stop()
    }
}

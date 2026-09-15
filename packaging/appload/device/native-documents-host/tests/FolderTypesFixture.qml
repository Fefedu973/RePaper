import QtQuick
import net.asivery.AppLoad 1.0

RePaperNativeDocuments {
    socketPath: testSocketPath
    stateDirectory: testStateDirectory
    library: typedLibrary
    libraryController: typedLibraryController
    documentController: typedDocumentController
    explorer: typedExplorer
    deviceScreenInfo: QtObject { property size paperSize: Qt.size(1620, 2160) }
    windowNavigator: QtObject {
        function open(route, args) {
            nativeView.item = {
                documentLoaded: true,
                document: typedLibrary.entryForId(args.documentId)
            }
        }
    }
    documentViewLoader: QtObject { id: nativeView; property var item: null }
}

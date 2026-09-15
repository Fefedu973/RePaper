import QtQuick
import net.asivery.AppLoad 1.0

RePaperNativeDocuments {
    socketPath: testSocketPath
    stateDirectory: testStateDirectory
    library: typedLibrary
    libraryController: typedLibraryController
    documentImporter: typedImporter
    explorer: typedExplorer
    documentController: QtObject {}
    deviceScreenInfo: QtObject {}
    windowNavigator: QtObject { function open(route, args) {} }
    documentViewLoader: QtObject { property var item: null }
}

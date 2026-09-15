pragma Singleton
import QtQuick 2.15
QtObject {
    // One UI unit represents two Paper Pro screen pixels. Native-host surfaces
    // set unit to 2; standalone Qt apps already have devicePixelRatio == 2.
    property real unit: 1
    readonly property real body: 14 * unit
    readonly property real caption: 11 * unit
    readonly property real title: 24 * unit
    readonly property real control: 48 * unit
    readonly property real icon: 24 * unit
    readonly property real line: Math.max(1, unit)
    readonly property real margin: 40 * unit
    readonly property color ink: "#000000"
    readonly property color paper: "#ffffff"
    readonly property color muted: "#555555"
    readonly property color divider: "#b3b3b3"
    readonly property string sans: Qt.fontFamilies().indexOf("reMarkable Sans") >= 0 ? "reMarkable Sans" : "DejaVu Sans"
    readonly property string serif: Qt.fontFamilies().indexOf("reMarkable Serif Small") >= 0 ? "reMarkable Serif Small" : "DejaVu Serif"
    function actionable(message) {
        if (/impossible|échou|invalide|indisponible|erreur|ne .*pas|expir|reconnect/i.test(message)) return true
        return message && !/cache|hors ligne|bridge|simulation|données fictives|autonome|cours chargé|prêt|mes cours/i.test(message)
    }
}

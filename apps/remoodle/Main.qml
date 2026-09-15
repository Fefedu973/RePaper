import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import RePaper.Keyboard 1.0
import "qrc:/paper" as Paper

ApplicationWindow {
    id: window
    width: 810; height: 1080; visible: true; title: "reMoodle"
    color: "white"
    font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.body
    palette.window: "white"; palette.base: "white"; palette.button: "white"
    palette.highlight: "black"; palette.highlightedText: "white"
    property bool settingsOpen: false
    property bool searchOpen: false
    readonly property bool connecting: !moodle.loggedIn || settingsOpen
    KeyboardController { id: keyboard; window: window }
    Connections {
        target: moodle
        function onLoginFinished(success) { if (success) { window.settingsOpen = false; keyboard.dismiss() } }
    }
    function format(item) {
        if (item.kind !== "file") return item.detail || ""
        const name = (item.name || "").toLowerCase()
        const type = /\.pptx?$/.test(name) ? "PowerPoint" : item.mime === "application/pdf" || /\.pdf$/.test(name) ? "PDF" : "Image"
        return type + (item.imported ? " · Importé" : "")
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Paper.Theme.margin
        anchors.bottomMargin: Paper.Theme.margin + keyboard.platformHeight
        spacing: 20
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Paper.IconButton {
                symbol: "back"; text: "Retour"
                visible: moodle.canGoBack && !window.connecting || window.settingsOpen
                onClicked: { keyboard.dismiss(); if(window.settingsOpen) window.settingsOpen=false; else moodle.back() }
            }
            Label {
                text: window.connecting ? "reMoodle" : moodle.title
                font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title
                Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            Paper.IconButton {
                visible: !window.connecting; symbol: "search"; text: "Rechercher"
                onClicked: { window.searchOpen = !window.searchOpen; if(window.searchOpen) search.forceActiveFocus(); else { moodle.search=""; keyboard.dismiss() } }
            }
            Paper.IconButton {
                visible: !window.connecting; symbol: moodle.busy ? "close" : "refresh"; text: moodle.busy ? "Annuler" : "Actualiser"
                onClicked: moodle.busy ? moodle.cancel() : moodle.refresh()
            }
            Paper.IconButton { visible: moodle.loggedIn && !window.settingsOpen; symbol: "settings"; text: "Compte"; onClicked: { keyboard.dismiss(); window.settingsOpen = true } }
            Paper.IconButton { symbol: "close"; text: "Fermer reMoodle"; onClicked: window.close() }
        }
        ScrollView {
            id: loginScroll; visible: window.connecting
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth; clip: true
            contentHeight: loginContent.implicitHeight
            ColumnLayout {
                id: loginContent
                width: loginScroll.availableWidth; spacing: 24
                Item { Layout.preferredHeight: 32 }
                Label { text: "Votre établissement"; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title }
                Paper.TextField {
                    id: base; objectName: "loginBaseField"; Layout.fillWidth: true
                    text: moodle.loginBaseUrl; onTextEdited: moodle.loginBaseUrl = text
                    placeholderText: "https://e-campus.cpe.fr"; selectByMouse: true
                }
                Label { Layout.fillWidth: true; text: "Ouvrez le lien de connexion sur votre téléphone ou votre ordinateur, puis ajoutez le QR de votre profil Moodle."; wrapMode: Text.WordWrap }
                RowLayout {
                    Paper.Button { primary: true; text: moodle.waitingForBrowser ? "Connexion en cours…" : "Se connecter"; enabled: !moodle.waitingForBrowser && !moodle.busy; onClicked: { keyboard.dismiss(); moodle.openLogin(base.text) } }
                    Paper.Button { quiet: true; text: "Annuler"; visible: moodle.waitingForBrowser || moodle.busy; onClicked: moodle.cancel() }
                }
                RowLayout {
                    visible: moodle.loginCode.length > 0; Layout.fillWidth: true; spacing: 24
                    Image { source: moodle.loginQr; Layout.preferredWidth: 180; Layout.preferredHeight: 180; fillMode: Image.PreserveAspectFit; smooth: false }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 12
                        Label { text: moodle.loginCode.slice(0,4) + " " + moodle.loginCode.slice(4); font.pixelSize: 28; font.letterSpacing: 3 }
                        Label { text: moodle.portalUrl; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                        Label { text: "Valable 10 minutes"; font.pixelSize: Paper.Theme.caption }
                        Paper.Button { text: "Ouvrir le lien"; visible: moodle.pcIntegration; enabled: !moodle.handoffBusy; onClicked: moodle.openPortal() }
                    }
                }
                Label { Layout.fillWidth: true; visible: moodle.waitingForBrowser; text: "Dans Moodle : Profil → Application mobile. Les deux appareils doivent être sur le même Wi-Fi."; wrapMode: Text.WordWrap }
                Paper.Button { quiet: true; text: "Se déconnecter"; visible: moodle.loggedIn; enabled: !moodle.busy; onClicked: moodle.logout() }
            }
        }
        Paper.TextField {
            id: search; visible: !window.connecting && window.searchOpen
            Layout.fillWidth: true; placeholderText: "Rechercher…"
            text: moodle.search; onTextEdited: moodle.search = text
        }
        ListView {
            id: list; visible: !window.connecting
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; spacing: 0; model: moodle.items
            boundsBehavior: Flickable.StopAtBounds; maximumFlickVelocity: 1400; flickDeceleration: 4500
            ScrollBar.vertical: Paper.ScrollBar {}
            delegate: ItemDelegate {
                id: entry
                required property var modelData
                width: list.width; implicitHeight: Math.max(88, row.implicitHeight + 32)
                enabled: !moodle.busy; padding: 0
                contentItem: RowLayout {
                    id: row; spacing: 20
                    Paper.Icon { name: entry.modelData.kind === "file" ? "document" : "folder"; Layout.preferredWidth: 24; Layout.preferredHeight: 28 }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 7
                        Label { text: entry.modelData.name || "Sans titre"; textFormat: Text.PlainText; font.pixelSize: Paper.Theme.body; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Label { text: window.format(entry.modelData); textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; maximumLineCount: 1; elide: Text.ElideRight; font.pixelSize: Paper.Theme.caption; color: Paper.Theme.muted }
                    }
                    Paper.Icon { name: "next"; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
                }
                background: Rectangle {
                    color: entry.down ? "#ededed" : "white"
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Paper.Theme.divider }
                }
                onClicked: { keyboard.dismiss(); moodle.openItem(modelData) }
            }
            Label { anchors.centerIn: parent; visible: list.count === 0; width: parent.width - 40; text: moodle.busy ? "Chargement…" : moodle.search.length ? "Aucun résultat" : "Aucun document"; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
        }
        RowLayout {
            visible: moodle.busy && !window.connecting; Layout.fillWidth: true
            Label { text: /conversion/i.test(moodle.message) ? "Conversion…" : /télécharg/i.test(moodle.message) ? "Téléchargement…" : "Chargement…"; font.pixelSize: Paper.Theme.caption }
            ProgressBar {
                Layout.fillWidth: true; value: moodle.progress; indeterminate: false
                background: Rectangle { implicitHeight: 2; color: "#dddddd" }
                contentItem: Item { implicitHeight: 2; Rectangle { width: parent.width * (moodle.progress || 0.1); height: 2; color: "black" } }
            }
        }
        Paper.Notice { Layout.fillWidth: true; text: moodle.message }
        TouchKeyboard { Layout.fillWidth: true; controller: keyboard }
    }
}

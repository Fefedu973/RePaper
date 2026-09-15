import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import RePaper.Keyboard 1.0
import RePaper.Rich 1.0

ApplicationWindow {
    id: window
    objectName: "rmchatWindow"
    width: 1080; height: 1440; visible: true
    title: "RMChat · ChatGPT"
    color: "#f8f7f3"
    font.family: "DejaVu Sans"
    font.pixelSize: 21
    property bool narrow: width < 850
    property bool showHistory: !narrow
    property bool connectionOpen: false
    property bool followResponse: true
    readonly property color ink: "#22231f"
    readonly property color muted: "#62655d"
    readonly property color line: "#d5d6ce"
    KeyboardController { id: keyboard; objectName: "textKeyboard"; window: window }
    onNarrowChanged: showHistory = !narrow

    function selectFile(mode) {
        keyboard.dismiss()
        localFiles.mode = mode
        localFiles.home()
        picker.open()
    }
    Connections {
        target: chat
        function onChanged() {
            if (chat.connected) window.connectionOpen = false
            if (window.followResponse && chat.streamingText.length > 0)
                Qt.callLater(function() { messagesView.positionViewAtEnd() })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        anchors.bottomMargin: 20 + keyboard.platformHeight
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Button {
                text: window.showHistory ? "‹" : "☰"
                ToolTip.text: "Afficher les conversations"
                ToolTip.visible: hovered
                implicitWidth: 54; implicitHeight: 52
                onClicked: { keyboard.dismiss(); window.showHistory = !window.showHistory }
            }
            Item {
                Layout.fillWidth: true; implicitWidth: 190; implicitHeight: 62
                Column {
                    anchors.verticalCenter: parent.verticalCenter; spacing: 1
                    Label { text: "RMChat"; color: window.ink; font.pixelSize: 34; font.bold: true }
                    Label { text: "Votre compte ChatGPT"; color: window.muted; font.pixelSize: 16 }
                }
            }
            Button {
                objectName: "connectionButton"
                text: chat.connected ? "Compte" : "Connexion"
                implicitHeight: 52
                onClicked: { keyboard.dismiss(); window.connectionOpen = !window.connectionOpen }
            }
            Button { text: "Fermer"; implicitHeight: 52; onClicked: window.close() }
        }
        Rectangle { Layout.fillWidth: true; height: 2; color: window.ink }

        Pane {
            visible: chat.statusMessage.length > 0 || chat.readOnlyPreview
            Layout.fillWidth: true
            padding: 14
            background: Rectangle { color: chat.errorKind.length > 0 ? "#ece9df" : "#efefe8"; radius: 8 }
            contentItem: ColumnLayout {
                id: notice
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true; spacing: 14
                    Label {
                        Layout.fillWidth: true
                        text: chat.readOnlyPreview ? "Aperçu de l’interface · aucune connexion au compte" : chat.statusMessage
                        color: window.ink; font.pixelSize: 18; wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                    }
                    Button { text: "Interrompre"; visible: chat.busy; onClicked: chat.cancel() }
                }
                Label {
                    visible: chat.errorDetails.length > 0
                    Layout.fillWidth: true; text: chat.errorDetails; color: window.muted
                    font.pixelSize: 16; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                }
                RowLayout {
                    visible: !chat.busy && (chat.errorKind === "UPSTREAM_FORBIDDEN" || chat.errorKind === "WEB_AUTH_REQUIRED")
                    Layout.fillWidth: true; spacing: 12
                    Button { text: "Ouvrir ChatGPT sur le PC"; font.pixelSize: 17; onClicked: localFiles.openConversation(chat.conversationId) }
                    Button { text: "Copier le brouillon"; font.pixelSize: 17; enabled: chat.draft.length > 0; onClicked: localFiles.copyText(chat.draft) }
                    Item { Layout.fillWidth: true }
                }
            }
        }
        ProgressBar { Layout.fillWidth: true; visible: chat.busy; indeterminate: true; implicitHeight: 4 }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 20
            Rectangle {
                visible: window.showHistory
                Layout.preferredWidth: window.narrow ? Math.min(300, window.width - 48) : 280
                Layout.fillHeight: true
                color: "#efefe8"; radius: 10; border.color: window.line
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 16; spacing: 14
                    Button {
                        objectName: "newConversationButton"
                        text: "+ Nouveau chat"
                        Layout.fillWidth: true; implicitHeight: 54
                        enabled: !chat.busy
                        onClicked: { keyboard.dismiss(); chat.newConversation(); if (window.narrow) window.showHistory = false }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "CONVERSATIONS"; font.pixelSize: 14; font.bold: true; color: window.muted; Layout.fillWidth: true }
                        ToolButton { text: "Actualiser"; font.pixelSize: 13; implicitHeight: 44; ToolTip.text: "Actualiser les conversations"; ToolTip.visible: hovered; enabled: chat.connected && !chat.busy; onClicked: chat.refreshConversations() }
                    }
                    ListView {
                        id: historyView; objectName: "conversationsList"
                        Layout.fillWidth: true; Layout.fillHeight: true
                        model: chat.conversations; clip: true; spacing: 8
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            required property var modelData
                            width: historyView.width; implicitHeight: Math.max(68, historyTitle.implicitHeight + 24)
                            enabled: !chat.busy
                            background: Rectangle { radius: 7; color: modelData.id === chat.conversationId ? "#fffefa" : "transparent"; border.color: modelData.id === chat.conversationId ? "#babdb1" : "transparent" }
                            contentItem: Label { id: historyTitle; text: modelData.title || "Sans titre"; color: window.ink; font.pixelSize: 19; maximumLineCount: 3; elide: Text.ElideRight; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
                            onClicked: { keyboard.dismiss(); window.followResponse = true; chat.openConversation(modelData.id); if (window.narrow) window.showHistory = false }
                        }
                        Label {
                            anchors.centerIn: parent; width: parent.width - 12
                            visible: historyView.count === 0
                            text: chat.connected ? "Vos conversations apparaîtront ici." : "Connectez votre compte pour retrouver vos conversations."
                            color: window.muted; font.pixelSize: 18; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter
                        }
                        footer: Button { width: historyView.width; visible: chat.hasMoreConversations; height: visible ? 52 : 0; text: "Charger la suite"; enabled: !chat.busy; onClicked: chat.loadMoreConversations() }
                    }
                }
            }

            ColumnLayout {
                visible: !window.narrow || !window.showHistory
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 16
                ScrollView {
                    id: loginScroll
                    visible: !chat.connected || window.connectionOpen
                    Layout.fillWidth: true; Layout.fillHeight: true
                    clip: true; contentWidth: availableWidth
                    ColumnLayout {
                        width: loginScroll.availableWidth; spacing: 22
                        Item { Layout.preferredHeight: 18 }
                        Label { text: "Connecter ChatGPT"; font.pixelSize: 34; font.bold: true; color: window.ink; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Label { text: "Connectez-vous sur votre PC, puis importez votre session dans l’app."; color: window.muted; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Pane {
                            Layout.fillWidth: true; padding: 20
                            background: Rectangle { color: "#fffefa"; border.color: window.line; radius: 12 }
                            contentItem: ColumnLayout {
                                id: connectionSteps
                                spacing: 18
                                Label { text: "1. Connectez-vous à ChatGPT"; font.bold: true; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                Button { text: "Ouvrir ChatGPT sur le PC"; enabled: !chat.readOnlyPreview; implicitHeight: 54; onClicked: localFiles.openBrowser(false) }
                                Rectangle { Layout.fillWidth: true; height: 1; color: window.line }
                                Label { text: "2. Exportez votre session"; font.bold: true; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                Label { text: "Dans Chrome ou Edge, ouvrez l’extension « RMChat — connexion locale », puis cliquez sur « Exporter la session ». Le fichier est enregistré dans Téléchargements."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: window.muted; font.pixelSize: 18 }
                                Label { text: "Première utilisation : installez l’extension fournie avec RMChat en suivant son guide sur le PC."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: window.muted; font.pixelSize: 18 }
                                Rectangle { Layout.fillWidth: true; height: 1; color: window.line }
                                Label { text: "3. Importez le fichier dans RMChat"; font.bold: true; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                Button { objectName: "importSessionButton"; text: "Choisir le fichier de session…"; implicitHeight: 56; enabled: !chat.busy && !chat.readOnlyPreview; onClicked: window.selectFile("session") }
                            }
                        }
                        Label { text: "Les accès sont enregistrés dans le coffre chiffré de cette app."; Layout.fillWidth: true; color: window.muted; font.pixelSize: 17; wrapMode: Text.WordWrap }
                        Label { text: localFiles.message; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: window.muted; font.pixelSize: 17 }
                        Button { objectName: "connectSavedButton"; text: "Utiliser la session enregistrée"; visible: chat.credentialStored && !chat.connected; enabled: !chat.busy; implicitHeight: 56; onClicked: chat.connectSaved() }
                        Label { text: chat.accountLabel; visible: text.length > 0; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Button { text: "Déconnecter ce compte"; visible: chat.credentialStored; enabled: !chat.busy; onClicked: chat.logout() }
                        Item { Layout.preferredHeight: 12 }
                    }
                }

                ColumnLayout {
                    visible: chat.connected && !window.connectionOpen
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 16
                    RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        Label { text: chat.conversationTitle || "Nouvelle conversation"; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight; color: window.ink; font.pixelSize: 27; font.bold: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        ComboBox {
                            objectName: "modelSelector"
                            Layout.fillWidth: true
                            model: chat.models; textRole: "name"; valueRole: "id"
                            displayText: count === 0 ? "Aucun modèle accessible" : currentText
                            enabled: !chat.busy && count > 0
                            currentIndex: { for (var i = 0; i < chat.models.length; ++i) if (chat.models[i].id === chat.selectedModelId) return i; return -1 }
                            onActivated: chat.selectedModelId = currentValue
                        }
                        Button { objectName: "refreshModelsButton"; text: "Actualiser"; font.pixelSize: 16; enabled: chat.connected && !chat.busy; onClicked: chat.refreshModels() }
                    }
                    Label { text: chat.modelStatus; visible: text.length > 0; color: window.muted; font.pixelSize: 17; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
                    ListView {
                        id: messagesView; objectName: "messagesList"
                        Layout.fillWidth: true; Layout.fillHeight: true
                        model: chat.messages; clip: true; spacing: 18
                        ScrollBar.vertical: ScrollBar {}
                        onMovementEnded: window.followResponse = atYEnd
                        header: Button { width: messagesView.width; visible: chat.hasMoreMessages; height: visible ? 50 : 0; text: "Charger la suite des messages"; enabled: !chat.busy; onClicked: { window.followResponse = false; chat.loadMoreMessages() } }
                        delegate: Rectangle {
                            required property var modelData
                            width: messagesView.width
                            height: messageColumn.implicitHeight + 34
                            radius: 10; color: modelData.role === "user" ? "#eaece2" : "#fffefa"
                            border.color: window.line
                            Column {
                                id: messageColumn
                                x: 17; y: 17; width: parent.width - 34; spacing: 9
                                RowLayout {
                                    width: parent.width; spacing: 8
                                    Label { Layout.fillWidth: true; text: modelData.role === "user" ? "VOUS" : modelData.role === "assistant" ? "CHATGPT" : "INFORMATION"; font.pixelSize: 14; font.bold: true; color: window.muted }
                                    ToolButton { text: "Copier"; font.pixelSize: 14; implicitHeight: 36; enabled: (modelData.text || "").length > 0; onClicked: localFiles.copyText(modelData.text) }
                                }
                                RichMessage {
                                    id: messageBody; objectName: "richMessage"
                                    width: parent.width; height: contentHeight
                                    text: modelData.text || ""; fontPixelSize: 21; color: window.ink
                                    onLinkActivated: function(url) { localFiles.openLink(url) }
                                }
                                Label { width: parent.width; text: messageBody.truncated ? "Affichage abrégé. Le bouton Copier récupère le texte complet." : "Certaines formules sont affichées en source LaTeX."; visible: messageBody.truncated || messageBody.mathErrorCount > 0; font.pixelSize: 15; color: window.muted; wrapMode: Text.WordWrap }
                                Label { width: parent.width; text: modelData.status || ""; visible: text.length > 0; font.pixelSize: 16; color: window.muted; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
                            }
                        }
                        Label { anchors.centerIn: parent; width: parent.width - 30; visible: messagesView.count === 0 && chat.streamingText.length === 0; text: "Que souhaitez-vous explorer ?"; font.pixelSize: 28; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: window.muted }
                        footer: Column {
                            width: messagesView.width; spacing: 8
                            visible: chat.streamingText.length > 0
                            Label { text: "CHATGPT · RÉPONSE EN COURS"; font.pixelSize: 14; font.bold: true; color: window.muted; visible: parent.visible }
                            RichMessage { width: parent.width; height: contentHeight; text: chat.streamingText; fontPixelSize: 21; color: window.ink; onLinkActivated: function(url) { localFiles.openLink(url) } }
                        }
                    }
                    Label { text: localFiles.message; visible: text.length > 0; Layout.fillWidth: true; color: window.muted; font.pixelSize: 16; wrapMode: Text.WordWrap; textFormat: Text.PlainText }
                    Flow {
                        Layout.fillWidth: true; spacing: 8
                        visible: chat.attachments.length > 0
                        Repeater {
                            model: chat.attachments
                            delegate: Button { required property var modelData; text: "PDF · " + modelData.name + "  ×"; font.pixelSize: 17; enabled: !chat.busy; onClicked: chat.removeAttachment(modelData.id) }
                        }
                    }
                    Pane {
                        Layout.fillWidth: true; padding: 14
                        background: Rectangle { color: "#fffefa"; radius: 12; border.color: "#aeb2a2"; border.width: 2 }
                        contentItem: ColumnLayout {
                            id: composerLayout
                            spacing: 10
                            ScrollView {
                                Layout.fillWidth: true; Layout.preferredHeight: Math.min(160, Math.max(70, composer.implicitHeight))
                                contentWidth: availableWidth; clip: true
                                TextArea {
                                    id: composer; objectName: "composer"
                                    placeholderText: "Écrivez votre message…"
                                    text: chat.draft; onTextChanged: if (chat.draft !== text) chat.draft = text
                                    readOnly: chat.busy; selectByMouse: true; wrapMode: TextEdit.Wrap
                                    font.pixelSize: 22; color: window.ink; background: null
                                    Keys.onPressed: function(event) { if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier) && chat.canSend) { keyboard.dismiss(); window.followResponse = true; chat.send(); event.accepted = true } }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Button { text: "+ PDF"; implicitHeight: 52; enabled: !chat.busy && chat.attachments.length < 4; onClicked: window.selectFile("pdf") }
                                Item { Layout.fillWidth: true }
                                Button { objectName: "sendButton"; text: chat.busy ? "Interrompre" : "Envoyer ↑"; implicitHeight: 52; enabled: chat.busy || chat.canSend; onClicked: { keyboard.dismiss(); if (chat.busy) chat.cancel(); else { window.followResponse = true; chat.send() } } }
                            }
                        }
                    }
                }
            }
        }
        TouchKeyboard { Layout.fillWidth: true; controller: keyboard }
    }

    Dialog {
        id: picker; objectName: "loginDialog"
        anchors.centerIn: Overlay.overlay
        width: Math.min(window.width - 48, 780)
        height: Math.min(window.height - 80, 960)
        modal: true; title: localFiles.mode === "pdf" ? "Joindre un PDF" : "Importer une session ChatGPT"
        standardButtons: Dialog.Cancel
        onOpened: standardButton(Dialog.Cancel).text = "Annuler"
        contentItem: ColumnLayout {
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Button { text: "↑ Dossier parent"; enabled: localFiles.canGoUp; onClicked: localFiles.up() }
                Button { text: "Téléchargements"; onClicked: localFiles.home() }
            }
            Label { text: localFiles.folder.toString().replace("file://", ""); Layout.fillWidth: true; font.pixelSize: 16; color: window.muted; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText }
            ListView {
                id: filesView; objectName: "filesList"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 6
                model: localFiles.entries; ScrollBar.vertical: ScrollBar {}
                delegate: ItemDelegate {
                    required property var modelData
                    width: filesView.width; implicitHeight: 64
                    contentItem: Label { text: (modelData.isDirectory ? "▸  " : localFiles.mode === "pdf" ? "PDF  " : "JSON  ") + modelData.name; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter; textFormat: Text.PlainText }
                    onClicked: {
                        if (modelData.isDirectory) localFiles.openFolder(modelData.url)
                        else { picker.close(); if (localFiles.mode === "pdf") chat.addPdf(modelData.url); else chat.importSessionFile(modelData.url) }
                    }
                }
            }
            Label { text: localFiles.message; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: 17; color: window.muted }
        }
    }
}

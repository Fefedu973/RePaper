import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import RePaper.Keyboard 1.0
import "qrc:/paper" as Paper

ApplicationWindow {
    id: window
    visible: true; width: 810; height: 1080; title: "reAgenda"; color: "white"
    font.family: Paper.Theme.sans; font.pixelSize: Paper.Theme.body
    palette.window: "white"; palette.base: "white"; palette.button: "white"
    palette.highlight: "black"; palette.highlightedText: "white"
    property var selectedEvent: ({})
    property string noteTitle: "Notes"
    Connections {
        target: agenda
        function onNoteRequested(date, eventId, title) {
            window.noteTitle = title || ("Notes du " + date)
            eventDialog.close(); noteDialog.open()
        }
    }
    KeyboardController { id: keyboard; objectName: "textKeyboard"; window: window }
    function revealField(scroll, field) {
        if (!field || !scroll.visible) return
        Qt.callLater(function() {
            if (!field || !scroll.visible) return
            var flick = scroll.contentItem
            var pos = field.mapToItem(flick.contentItem, 0, 0)
            if (pos.y < flick.contentY) flick.contentY = Math.max(0, pos.y - 8)
            else if (pos.y + field.height > flick.contentY + flick.height)
                flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, pos.y + field.height - flick.height + 8))
        })
    }
    Component.onCompleted: {
        if (previewPage === "login") loginDialog.open()
        else if (previewPage === "ics") icsDialog.open()
        else if (previewPage === "sources") sourcesDialog.open()
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: Paper.Theme.margin; spacing: 20
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Label { text: "reAgenda"; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title; Layout.fillWidth: true }
            Paper.IconButton { text: "Mes calendriers"; symbol: "settings"; onClicked: sourcesDialog.open() }
            Paper.IconButton { text: "Actualiser"; symbol: "refresh"; enabled: !agenda.busy; onClicked: agenda.refresh() }
            Paper.IconButton { text: "Fermer reAgenda"; symbol: "close"; onClicked: Qt.quit() }
        }
        RowLayout {
            Layout.fillWidth: true
            Paper.IconButton { text: "Période précédente"; symbol: "back"; onClicked: agenda.shift(-1) }
            Label { text: agenda.periodLabel; textFormat: Text.PlainText; Layout.fillWidth: true; Layout.minimumWidth: 0; horizontalAlignment: Text.AlignHCenter; font.family: Paper.Theme.serif; font.pixelSize: Paper.Theme.title; elide: Text.ElideRight }
            Paper.IconButton { text: "Période suivante"; symbol: "next"; onClicked: agenda.shift(1) }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 4
            Paper.Button { quiet: true; text: "Aujourd’hui"; onClicked: agenda.today() }
            Item { Layout.fillWidth: true }
            Repeater {
                model: [{name:"Jour",value:"day"},{name:"Semaine",value:"week"},{name:"Mois",value:"month"}]
                delegate: Paper.Button {
                    required property var modelData
                    quiet: true; text: modelData.name; checkable: true; checked: agenda.view === modelData.value
                    onClicked: agenda.view = modelData.value
                }
            }
        }
        RowLayout {
            visible: agenda.view === "month"; Layout.fillWidth: true; spacing: 0
            Repeater { model: ["Lun", "Mar", "Mer", "Jeu", "Ven", "Sam", "Dim"]; delegate: Label { required property string modelData; text: modelData; font.pixelSize: Paper.Theme.caption; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter } }
        }
        ScrollView {
            id: calendarScroll
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth; contentHeight: calendarGrid.implicitHeight; clip: true
            ScrollBar.vertical: Paper.ScrollBar {}
            GridLayout {
                id: calendarGrid
                width: calendarScroll.availableWidth
                columns: agenda.view === "month" ? 7 : 1
                columnSpacing: 0; rowSpacing: agenda.view === "month" ? 0 : 24
                Repeater {
                    model: agenda.days
                    delegate: Rectangle {
                        id: dayCard
                        required property var modelData
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        implicitHeight: agenda.view === "month" ? Math.max(110, calendarScroll.availableHeight / 6) : dayContent.implicitHeight
                        color: "white"
                        Rectangle { width: parent.width; height: 1; color: Paper.Theme.divider }
                        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; visible: agenda.view === "month"; color: Paper.Theme.divider }
                        ColumnLayout {
                            id: dayContent
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                            anchors.margins: agenda.view === "month" ? 8 : 0
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: agenda.view === "month" ? dayCard.modelData.dayNumber : agenda.view === "day" ? "Note du jour" : dayCard.modelData.label
                                    textFormat: Text.PlainText; font.pixelSize: agenda.view === "month" ? 16 : 18
                                    font.family: agenda.view === "month" ? Paper.Theme.sans : Paper.Theme.serif
                                    font.bold: agenda.view === "month" && dayCard.modelData.today; color: dayCard.modelData.inMonth ? "black" : "#777777"
                                    Layout.fillWidth: true; Layout.preferredHeight: 40; verticalAlignment: Text.AlignVCenter
                                }
                                Paper.IconButton {
                                    visible: agenda.view !== "month"; text: "Note du jour"; symbol: "note"
                                    enabled: !agenda.noteBusy
                                    onClicked: agenda.requestNote(dayCard.modelData.date, "", "Notes du " + dayCard.modelData.date)
                                }
                            }
                            Repeater {
                                model: agenda.view === "month" ? dayCard.modelData.events.slice(0, 2) : dayCard.modelData.events
                                delegate: Item {
                                    id: eventRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    implicitHeight: agenda.view === "month" ? 24 : Math.max(88, eventContent.implicitHeight + 32)
                                    MouseArea { anchors.fill: parent; enabled: agenda.view !== "month"; onClicked: { window.selectedEvent = eventRow.modelData; eventDialog.open() } }
                                    RowLayout {
                                        id: eventContent
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                        spacing: 16
                                        Label { visible: agenda.view !== "month"; text: eventRow.modelData.time; Layout.preferredWidth: 95; font.pixelSize: Paper.Theme.caption; wrapMode: Text.WordWrap }
                                        ColumnLayout {
                                            Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 6
                                            Label { text: eventRow.modelData.title; textFormat: Text.PlainText; font.pixelSize: agenda.view === "month" ? Paper.Theme.caption : Paper.Theme.body; font.strikeout: eventRow.modelData.cancelled; wrapMode: agenda.view === "month" ? Text.NoWrap : Text.Wrap; elide: Text.ElideRight; Layout.fillWidth: true }
                                            Label { visible: agenda.view !== "month" && text.length > 0; text: (eventRow.modelData.location || "") + (eventRow.modelData.cancelled ? " · Annulé" : ""); textFormat: Text.PlainText; font.pixelSize: Paper.Theme.caption; color: Paper.Theme.muted; Layout.fillWidth: true; elide: Text.ElideRight }
                                        }
                                        Paper.IconButton {
                                            visible: agenda.view !== "month"; symbol: "note"; text: "Ouvrir les notes"; enabled: !agenda.noteBusy
                                            onClicked: agenda.requestNote(dayCard.modelData.date, eventRow.modelData.id, eventRow.modelData.title, eventRow.modelData)
                                        }
                                    }
                                }
                            }
                            Label { visible: agenda.view === "month" && dayCard.modelData.events.length > 2; text: "+ " + (dayCard.modelData.events.length - 2); font.pixelSize: Paper.Theme.caption; Layout.fillWidth: true }
                            Label {
                                visible: dayCard.modelData.events.length === 0 && agenda.view !== "month"
                                text: dayCard.modelData.uncached ? "Calendrier à actualiser" : "Aucun événement"
                                font.pixelSize: Paper.Theme.caption; color: Paper.Theme.muted
                                Layout.topMargin: 16; Layout.bottomMargin: 24
                            }
                        }
                        MouseArea { anchors.fill: parent; enabled: agenda.view === "month"; onClicked: { agenda.selectDate(dayCard.modelData.date); agenda.view = "day" } }
                    }
                }
            }
        }
        Label { visible: agenda.busy; text: "Actualisation…"; font.pixelSize: Paper.Theme.caption }
        Paper.Notice { Layout.fillWidth: true; text: agenda.message }
    }

    Paper.Dialog {
        id: eventDialog
        anchors.centerIn: parent
        width: Math.min(window.width - 50, 620)
        title: window.selectedEvent.title || "Événement"
        modal: true
        standardButtons: Dialog.Close
        ColumnLayout {
            width: parent.width
            spacing: 18
            Label { textFormat: Text.PlainText; text: window.selectedEvent.date + " · " + window.selectedEvent.time; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { textFormat: Text.PlainText; text: window.selectedEvent.location || ""; visible: text.length > 0; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { textFormat: Text.PlainText; text: window.selectedEvent.teacher || ""; visible: text.length > 0; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { textFormat: Text.PlainText; text: window.selectedEvent.status === "CANCELLED" ? "Annulé" : (window.selectedEvent.status || ""); visible: text.length > 0; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { textFormat: Text.PlainText; text: window.selectedEvent.description || ""; visible: text.length > 0; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Paper.Button {
                objectName: "eventNotesButton"
                text: "Ouvrir les notes"
                Layout.fillWidth: true
                enabled: !agenda.noteBusy
                onClicked: agenda.requestNote(window.selectedEvent.date, window.selectedEvent.id, window.selectedEvent.title, window.selectedEvent)
            }
        }
    }
    Paper.Dialog {
        id: noteDialog
        objectName: "noteProgressDialog"
        anchors.centerIn: parent
        width: Math.min(window.width - 50, 620)
        title: window.noteTitle
        modal: true
        standardButtons: Dialog.Close
        Label {
            objectName: "noteProgressMessage"
            width: parent.width
            textFormat: Text.PlainText
            text: agenda.noteMessage
            wrapMode: Text.Wrap
            font.pixelSize: 14
        }
    }
    Paper.Dialog {
        id: sourcesDialog
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 680)
        height: Math.min(window.height - 80, 760)
        title: "Mes calendriers"
        modal: true
        standardButtons: Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            RowLayout {
                Paper.Button { text: "Ajouter un ICS"; onClicked: { sourcesDialog.close(); icsDialog.open() } }
                Paper.Button { text: agenda.cpeConnected ? "Reconnecter CPE" : "Connecter CPE"; onClicked: { sourcesDialog.close(); loginDialog.open() } }
            }
            Paper.Button { text: "Notes et absences CPE"; onClicked: { sourcesDialog.close(); schoolDialog.open() } }
            ScrollView {
                id: sourcesScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: sourcesScroll.availableWidth
                    Repeater {
                        model: agenda.sources
                        delegate: Pane {
                            background: Rectangle { color: "white"; Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Paper.Theme.divider } }
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                anchors.fill: parent
                                Label { textFormat: Text.PlainText; text: modelData.label; font.bold: true; Layout.fillWidth: true }
                                Label { textFormat: Text.PlainText; text: modelData.updated ? "Mis à jour : " + modelData.updated : "Pas encore synchronisé"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                Label { textFormat: Text.PlainText; text: modelData.needsLogin ? "Connexion CPE nécessaire pour actualiser" : modelData.error; visible: text.length > 0; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                RowLayout {
                                    Paper.Button { visible: modelData.kind === "cpe" && agenda.cpeConnected; text: "Déconnecter"; onClicked: agenda.disconnectCpe() }
                                    Paper.Button { text: "Retirer le calendrier"; onClicked: agenda.removeSource(modelData.id) }
                                }
                            }
                        }
                    }
                    Label { textFormat: Text.PlainText; visible: agenda.sources.length === 0; text: "Ajoutez votre calendrier ICS ou votre compte CPE Lyon."; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
        }
    }
    Paper.Dialog {
        id: loginDialog
        objectName: "loginDialog"
        x: (window.width - width) / 2
        y: Math.max(20, (window.height - keyboard.platformHeight - height) / 2)
        width: Math.min(window.width - 40, 680)
        height: Math.min(window.height - keyboard.platformHeight - 40, 720)
        title: "My CPE Lyon"
        modal: true
        standardButtons: Dialog.Cancel
        onOpened: username.forceActiveFocus()
        onClosed: { password.text = ""; keyboard.dismiss() }
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            ScrollView {
                id: loginScroll; Layout.fillWidth: true; Layout.fillHeight: true; contentWidth: availableWidth; clip: true
                onHeightChanged: window.revealField(loginScroll, keyboard.target)
                ColumnLayout {
                    width: loginScroll.availableWidth; spacing: 14
                    Label { textFormat: Text.PlainText; text: "Votre compte CPE Lyon."; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pixelSize: 19 }
                    Paper.TextField { id: username; objectName: "loginUsername"; placeholderText: "Identifiant CPE"; Layout.fillWidth: true; implicitHeight: 52; selectByMouse: true; onActiveFocusChanged: if (activeFocus) window.revealField(loginScroll, username) }
                    Paper.TextField { id: password; objectName: "loginPassword"; placeholderText: "Mot de passe CPE"; echoMode: TextInput.Password; Layout.fillWidth: true; implicitHeight: 52; onActiveFocusChanged: if (activeFocus) window.revealField(loginScroll, password) }
                    Paper.Button { primary: true; text: "Se connecter"; enabled: !agenda.busy; Layout.fillWidth: true; implicitHeight: 52; onClicked: { agenda.connectCpe(username.text, password.text); password.text = ""; loginDialog.close() } }
                }
            }
            TouchKeyboard { controller: keyboard; Layout.fillWidth: true }
        }
    }
    Paper.Dialog {
        id: icsDialog
        objectName: "icsDialog"
        x: (window.width - width) / 2
        y: Math.max(20, (window.height - keyboard.platformHeight - height) / 2)
        width: Math.min(window.width - 40, 680)
        height: Math.min(window.height - keyboard.platformHeight - 40, 740)
        title: "Ajouter un calendrier ICS"
        modal: true
        standardButtons: Dialog.Cancel
        onOpened: calendarUrl.forceActiveFocus()
        onClosed: { calendarUrl.text = ""; keyboard.dismiss() }
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            ScrollView {
                id: icsScroll; Layout.fillWidth: true; Layout.fillHeight: true; contentWidth: availableWidth; clip: true
                onHeightChanged: window.revealField(icsScroll, keyboard.target)
                ColumnLayout {
                    width: icsScroll.availableWidth; spacing: 14
                    Label { textFormat: Text.PlainText; text: "Lien de votre calendrier (Google, ENT…) ou fichier .ics."; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 19 }
                    Paper.TextField { id: calendarName; objectName: "icsName"; placeholderText: "Nom du calendrier"; Layout.fillWidth: true; implicitHeight: 52; onActiveFocusChanged: if (activeFocus) window.revealField(icsScroll, calendarName) }
                    Paper.TextField { id: calendarUrl; objectName: "icsUrl"; placeholderText: "https://… ou /home/root/calendrier.ics"; Layout.fillWidth: true; implicitHeight: 52; selectByMouse: true; onActiveFocusChanged: if (activeFocus) window.revealField(icsScroll, calendarUrl) }
                    Paper.Button { primary: true; text: "Ajouter"; enabled: calendarUrl.text.length > 0; Layout.fillWidth: true; implicitHeight: 52; onClicked: { agenda.addIcs(calendarName.text, calendarUrl.text); icsDialog.close() } }
                }
            }
            TouchKeyboard { controller: keyboard; Layout.fillWidth: true; symbols: true }
        }
    }
    Paper.Dialog {
        id: schoolDialog
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 680)
        height: Math.min(window.height - 80, 860)
        title: "Notes et absences"
        modal: true
        standardButtons: Dialog.Close
        ScrollView {
            id: schoolScroll
            anchors.fill: parent
            contentWidth: availableWidth
            ColumnLayout {
                width: schoolScroll.availableWidth
                Label { textFormat: Text.PlainText; text: "Notes et moyennes fournies par CPE. Aucun coefficient ou barème n'est inventé."; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 }
                Repeater {
                    model: agenda.grades
                    delegate: Pane {
                            background: Rectangle { color: "white"; Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Paper.Theme.divider } }
                        required property var modelData
                        Layout.fillWidth: true
                        ColumnLayout {
                            width: parent.width
                            Label { textFormat: Text.PlainText; text: modelData.title; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Label { textFormat: Text.PlainText; text: modelData.details; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 20 }
                        }
                    }
                }
                Label { textFormat: Text.PlainText; visible: agenda.grades.length === 0; text: "Aucune note."; font.pixelSize: 19 }
                Label { textFormat: Text.PlainText; text: "Absences"; font.bold: true; font.pixelSize: 28 }
                Repeater {
                    model: agenda.absences
                    delegate: Pane {
                            background: Rectangle { color: "white"; Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Paper.Theme.divider } }
                        required property var modelData
                        Layout.fillWidth: true
                        ColumnLayout {
                            width: parent.width
                            Label { textFormat: Text.PlainText; text: modelData.title; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                            Label { textFormat: Text.PlainText; text: modelData.details; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 20 }
                        }
                    }
                }
                Label { textFormat: Text.PlainText; visible: agenda.absences.length === 0; text: "Aucune absence."; font.pixelSize: 19 }
            }
        }
    }
}

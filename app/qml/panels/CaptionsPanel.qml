import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Cutline

// Caption authoring backed by project commands. SRT/WebVTT parsing, writing and monitor burn-in
// use the same engine data, so the words visible here are the words delivered by export.
Item {
    id: panel
    property string panelId: "captions"
    property string selectedCue: ""
    property var currentTrack: null

    function trackIndex(id) {
        for (let i = 0; i < session.captionTracks.length; ++i)
            if (session.captionTracks[i].id === id) return i
        return -1
    }
    function cueById(id) {
        for (let i = 0; i < session.captions.length; ++i)
            if (session.captions[i].id === id) return session.captions[i]
        return null
    }
    function loadTrack() {
        const i = trackIndex(session.captionTrack)
        trackPicker.currentIndex = i
        currentTrack = i >= 0 ? session.captionTracks[i] : null
        if (currentTrack) {
            trackName.text = currentTrack.name
            trackLanguage.text = currentTrack.language
            fontFamily.text = currentTrack.family
            fontSize.value = currentTrack.size * 100
            bold.checked = currentTrack.bold
            italic.checked = currentTrack.italic
            align.currentIndex = Math.max(0, ["left", "center", "right"].indexOf(currentTrack.align))
            position.currentIndex = Math.max(0, ["top", "middle", "bottom"].indexOf(currentTrack.position))
            outline.value = currentTrack.outlineWidth * 100
            background.value = currentTrack.backgroundOpacity * 100
            lineWidth.value = currentTrack.maxWidth * 100
        }
        loadCue()
    }
    function loadCue() {
        const cue = cueById(selectedCue)
        if (!cue) {
            selectedCue = ""
            cueText.text = ""
            cueSpeaker.text = ""
            cueStart.value = session.playhead
            cueEnd.value = session.playhead + 2
            return
        }
        cueText.text = cue.text
        cueSpeaker.text = cue.speaker
        cueStart.value = cue.start
        cueEnd.value = cue.end
    }
    function commitCue() {
        if (selectedCue === "") {
            const id = session.captionAdd(session.captionTrack, cueStart.value, cueEnd.value, cueText.text, cueSpeaker.text)
            if (id !== "") selectedCue = id
        } else {
            session.captionUpdate(selectedCue, cueStart.value, cueEnd.value, cueText.text, cueSpeaker.text)
        }
    }
    function commitStyle() {
        if (!currentTrack) return
        session.captionSetTrackStyle(currentTrack.id, fontFamily.text, fontSize.value / 100, bold.checked, italic.checked,
                                     align.currentText, position.currentText, outline.value / 100,
                                     background.value / 100, lineWidth.value / 100)
    }

    Connections {
        target: session
        function onCaptionsChanged() { panel.loadTrack() }
    }
    Component.onCompleted: loadTrack()

    FileDialog {
        id: importDialog
        title: qsTr("Import captions")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Caption files (*.srt *.vtt)"), qsTr("All files (*)")]
        onAccepted: session.captionImport(session.captionTrack, selectedFile.toString())
    }
    FileDialog {
        id: exportDialog
        property string format: "srt"
        title: format === "vtt" ? qsTr("Export WebVTT") : qsTr("Export SubRip")
        fileMode: FileDialog.SaveFile
        nameFilters: format === "vtt" ? [qsTr("WebVTT (*.vtt)")] : [qsTr("SubRip (*.srt)")]
        onAccepted: session.captionExport(session.captionTrack, selectedFile.toString(), format)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            spacing: 5
            ComboBox {
                id: trackPicker
                objectName: "captionTrackPicker"
                Layout.fillWidth: true
                model: session.captionTracks
                textRole: "name"
                valueRole: "id"
                onActivated: session.captionSelectTrack(currentValue)
            }
            Chip {
                objectName: "captionAddTrack"
                text: qsTr("+ Track")
                onClicked: session.captionAddTrack(qsTr("Captions %1").arg(session.captionTracks.length + 1), "en")
            }
            Chip { text: qsTr("Import…"); onClicked: importDialog.open() }
            Chip {
                text: qsTr("SRT")
                enabled: panel.currentTrack !== null
                onClicked: { exportDialog.format = "srt"; exportDialog.open() }
            }
            Chip {
                text: qsTr("VTT")
                enabled: panel.currentTrack !== null
                onClicked: { exportDialog.format = "vtt"; exportDialog.open() }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            Layout.bottomMargin: 5
            visible: panel.currentTrack !== null
            spacing: 5
            TextField {
                id: trackName
                Layout.fillWidth: true
                placeholderText: qsTr("Track name")
                color: Theme.text
                selectByMouse: true
                onEditingFinished: if (panel.currentTrack) session.captionUpdateTrack(panel.currentTrack.id, text, trackLanguage.text)
            }
            TextField {
                id: trackLanguage
                implicitWidth: 72
                placeholderText: qsTr("Language")
                color: Theme.text
                selectByMouse: true
                onEditingFinished: if (panel.currentTrack) session.captionUpdateTrack(panel.currentTrack.id, trackName.text, text)
            }
            Chip {
                text: qsTr("Delete track")
                tone: Theme.danger
                onClicked: if (panel.currentTrack) session.captionRemoveTrack(panel.currentTrack.id)
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            ListView {
                id: cueList
                objectName: "captionCueList"
                SplitView.preferredWidth: parent.width * 0.5
                SplitView.minimumWidth: 180
                clip: true
                model: session.captions
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: cueRow
                    required property var modelData
                    width: cueList.width
                    height: 64
                    color: panel.selectedCue === modelData.id ? Theme.selection : "transparent"
                    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.border }
                    Column {
                        anchors { fill: parent; margins: 6 }
                        spacing: 2
                        Text {
                            text: Number(cueRow.modelData.start).toFixed(3) + "  ->  " + Number(cueRow.modelData.end).toFixed(3)
                            color: Theme.accent; font.family: Theme.mono; font.pixelSize: 10
                        }
                        Text {
                            width: parent.width
                            text: (cueRow.modelData.speaker ? cueRow.modelData.speaker + ": " : "") + cueRow.modelData.text
                            color: Theme.text; font.pixelSize: 12; maximumLineCount: 2; elide: Text.ElideRight; wrapMode: Text.WordWrap
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: { panel.selectedCue = cueRow.modelData.id; panel.loadCue() }
                        onDoubleClicked: session.seek(cueRow.modelData.start)
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: parent.count === 0
                    text: session.captionTracks.length === 0 ? qsTr("Add a caption track or import SRT/WebVTT") : qsTr("No captions on this track")
                    color: Theme.faint; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; width: parent.width - 30
                }
            }

            ScrollView {
                SplitView.fillWidth: true
                clip: true
                ColumnLayout {
                    width: Math.max(220, parent.width)
                    spacing: 5
                    Label { Layout.margins: Theme.pad; Layout.bottomMargin: 0; text: panel.selectedCue === "" ? qsTr("New caption") : qsTr("Edit caption"); color: Theme.text; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true; Layout.leftMargin: Theme.pad; Layout.rightMargin: Theme.pad
                        Text { text: qsTr("In"); color: Theme.muted }
                        NumberBox { id: cueStart; Layout.fillWidth: true; minimum: 0; maximum: 86400 }
                        Text { text: qsTr("Out"); color: Theme.muted }
                        NumberBox { id: cueEnd; Layout.fillWidth: true; minimum: 0.001; maximum: 86400 }
                        Chip { text: qsTr("At playhead"); onClicked: { cueStart.value = session.playhead; cueEnd.value = session.playhead + 2 } }
                    }
                    TextField {
                        id: cueSpeaker
                        Layout.fillWidth: true; Layout.leftMargin: Theme.pad; Layout.rightMargin: Theme.pad
                        placeholderText: qsTr("Speaker (optional)"); color: Theme.text; selectByMouse: true
                    }
                    TextArea {
                        id: cueText
                        objectName: "captionText"
                        Layout.fillWidth: true; Layout.leftMargin: Theme.pad; Layout.rightMargin: Theme.pad
                        implicitHeight: 72
                        placeholderText: qsTr("Caption text"); color: Theme.text; wrapMode: TextEdit.Wrap; selectByMouse: true
                        background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 3 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.leftMargin: Theme.pad; Layout.rightMargin: Theme.pad
                        Chip { objectName: "captionSave"; text: panel.selectedCue === "" ? qsTr("Add cue") : qsTr("Save cue"); enabled: panel.currentTrack !== null && cueText.text.trim().length > 0 && cueEnd.value > cueStart.value; onClicked: panel.commitCue() }
                        Chip { text: qsTr("New"); onClicked: { panel.selectedCue = ""; panel.loadCue() } }
                        Chip { text: qsTr("Delete"); tone: Theme.danger; enabled: panel.selectedCue !== ""; onClicked: { if (session.captionRemove(panel.selectedCue)) { panel.selectedCue = ""; panel.loadCue() } } }
                        Item { Layout.fillWidth: true }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                    Label { Layout.leftMargin: Theme.pad; text: qsTr("Track style"); color: Theme.text; font.bold: true }
                    GridLayout {
                        Layout.fillWidth: true; Layout.leftMargin: Theme.pad; Layout.rightMargin: Theme.pad
                        columns: 4; columnSpacing: 5; rowSpacing: 4
                        Text { text: qsTr("Font"); color: Theme.muted }
                        TextField { id: fontFamily; Layout.columnSpan: 2; Layout.fillWidth: true; color: Theme.text; onEditingFinished: panel.commitStyle() }
                        NumberBox { id: fontSize; minimum: 0.5; maximum: 50; onCommitted: panel.commitStyle() }
                        Text { text: qsTr("Face"); color: Theme.muted }
                        Check { id: bold; text: qsTr("Bold"); onToggled: panel.commitStyle() }
                        Check { id: italic; text: qsTr("Italic"); onToggled: panel.commitStyle() }
                        Item { width: 1; height: 1 }
                        Text { text: qsTr("Place"); color: Theme.muted }
                        ComboBox { id: align; model: ["left", "center", "right"]; onActivated: panel.commitStyle() }
                        ComboBox { id: position; model: ["top", "middle", "bottom"]; onActivated: panel.commitStyle() }
                        Item { width: 1; height: 1 }
                        Text { text: qsTr("Outline %"); color: Theme.muted }
                        NumberBox { id: outline; minimum: 0; maximum: 5; onCommitted: panel.commitStyle() }
                        Text { text: qsTr("Box opacity %"); color: Theme.muted }
                        NumberBox { id: background; minimum: 0; maximum: 100; onCommitted: panel.commitStyle() }
                        Text { text: qsTr("Line width %"); color: Theme.muted }
                        NumberBox { id: lineWidth; minimum: 10; maximum: 100; onCommitted: panel.commitStyle() }
                    }
                    Item { Layout.fillHeight: true; implicitHeight: 8 }
                }
            }
        }
    }
}

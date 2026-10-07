import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Cutline

// Text-based editing: the words said in the media under the playhead, as text. Choose words and delete them and their
// time leaves the timeline; take out the "um"s or the long pauses in one step; make captions from the words.
Item {
    id: panel
    property string panelId: "transcript"
    // The words chosen, as indices into the transcript; -1 when none.
    property int selFirst: -1
    property int selLast: -1
    property int anchorWord: -1
    property bool closeGaps: true
    readonly property bool hasSelection: selFirst >= 0
    readonly property bool ready: session.transcriptState === "ready"

    function chooseWord(index, extend) {
        if (extend && anchorWord >= 0) {
            selFirst = Math.min(anchorWord, index)
            selLast = Math.max(anchorWord, index)
        } else {
            anchorWord = index
            selFirst = index
            selLast = index
        }
        panel.forceActiveFocus()
    }
    function deleteChosen() {
        if (!hasSelection) return
        if (session.transcriptDelete(selFirst, selLast, closeGaps)) { selFirst = -1; selLast = -1; anchorWord = -1 }
    }
    function isHit(index) {
        const hits = session.transcriptHits
        for (let h = 0; h < hits.length; ++h) {
            if (index >= hits[h].first && index < hits[h].first + hits[h].count) return true
        }
        return false
    }
    Keys.onDeletePressed: deleteChosen()
    Keys.onBackPressed: deleteChosen()
    Connections {
        target: session
        function onTranscriptMediaChanged() { panel.selFirst = -1; panel.selLast = -1; panel.anchorWord = -1 }
    }
    onReadyChanged: { selFirst = -1; selLast = -1; anchorWord = -1 }

    FileDialog {
        id: importDialog
        title: qsTr("Import a transcript")
        nameFilters: [qsTr("Transcript JSON (*.json)"), qsTr("All files (*)")]
        onAccepted: session.transcriptImport(selectedFile.toString())
    }
    Popup {
        id: wordEditor
        property int word: -1
        anchors.centerIn: parent
        width: 280
        padding: 10
        modal: true
        background: Rectangle { color: Theme.panelHeader; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            Text { text: qsTr("Correct the word"); color: Theme.muted; font.pixelSize: 11 }
            TextField {
                id: wordText
                objectName: "transcriptWordText"
                Layout.fillWidth: true
                color: Theme.text
                selectByMouse: true
                background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 3 }
                onAccepted: { session.transcriptSetText(wordEditor.word, text); wordEditor.close() }
            }
        }
        onOpened: wordText.forceActiveFocus()
    }
    Popup {
        id: speakerEditor
        anchors.centerIn: parent
        width: 280
        padding: 10
        modal: true
        background: Rectangle { color: Theme.panelHeader; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            anchors.fill: parent
            spacing: 6
            Text { text: qsTr("Who says the chosen words?"); color: Theme.muted; font.pixelSize: 11 }
            TextField {
                id: speakerText
                objectName: "transcriptSpeakerText"
                Layout.fillWidth: true
                color: Theme.text
                placeholderText: qsTr("Name (empty clears it)")
                selectByMouse: true
                background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 3 }
                onAccepted: { session.transcriptSetSpeaker(panel.selFirst, panel.selLast, text); speakerEditor.close() }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 4
                Repeater {
                    model: session.transcriptSpeakers
                    delegate: Chip { required property string modelData; text: modelData; onClicked: speakerText.text = modelData }
                }
            }
        }
        onOpened: speakerText.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // What is being edited, and making it.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            spacing: 6
            Text {
                Layout.fillWidth: true
                text: session.transcriptMedia === "" ? qsTr("Select a clip to see what is said in it") : session.transcriptName
                color: Theme.text
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
            }
            Chip {
                objectName: "transcriptMake"
                visible: session.transcriptState === "none" || session.transcriptState === "ready"
                text: session.transcriptState === "ready" ? qsTr("Transcribe again") : qsTr("Transcribe")
                enabled: session.transcriptMedia !== "" && session.transcriptEngine !== ""
                ToolTip.text: session.transcriptEngine === "" ? qsTr("The speech engine was not found: see Preferences, Transcription") : qsTr("Listen to the media and write down the words, on this computer (%1)").arg(session.transcriptEngine)
                onClicked: session.transcribe()
            }
            Chip { text: qsTr("Import…"); enabled: session.transcriptMedia !== ""; ToolTip.text: qsTr("Read a transcript made elsewhere (whisper.cpp JSON)"); onClicked: importDialog.open() }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            visible: session.transcriptState === "working"
            text: qsTr("Transcribing… the progress is in Background Jobs, where it can be stopped.")
            color: Theme.warning
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            visible: session.transcriptState === "unavailable"
            text: qsTr("There is no transcript for this media and the speech engine is not installed. Put whisper.cpp's whisper-cli and a ggml model in a \"whisper\" folder beside the application, or import a transcript.")
            color: Theme.muted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        // Whole-transcript clean-up.
        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            Layout.bottomMargin: 4
            visible: panel.ready
            spacing: 4
            Chip {
                objectName: "transcriptFillers"
                property int count: panel.ready ? session.transcriptFillerCount(phrasesBox.checked) : 0
                text: qsTr("Remove fillers (%1)").arg(count)
                enabled: count > 0
                ToolTip.text: qsTr("Delete the um, uh, er and hmm sounds from the timeline")
                onClicked: session.transcriptRemoveFillers(phrasesBox.checked, panel.closeGaps)
            }
            Check { id: phrasesBox; text: qsTr("and “you know”, “I mean”") }
            Chip {
                objectName: "transcriptPauses"
                property int count: panel.ready ? session.transcriptPauseCount(pauseLength.value, 0.15) : 0
                text: qsTr("Close pauses over") + " (" + count + ")"
                enabled: count > 0
                ToolTip.text: qsTr("Take out silences between words that are longer than this, leaving a little at each side")
                onClicked: session.transcriptRemovePauses(pauseLength.value, 0.15)
            }
            NumberBox { id: pauseLength; width: 54; value: 0.8; minimum: 0.2; maximum: 30 }
            Text { text: qsTr("s"); color: Theme.muted; font.pixelSize: 11; verticalAlignment: Text.AlignVCenter; height: 24 }
            Chip { objectName: "transcriptCaptions"; text: qsTr("Make captions"); ToolTip.text: qsTr("Captions from the words on the timeline, in a “Transcript” caption track"); onClicked: session.transcriptCaptions(42, 2) }
            Check { text: qsTr("Close gaps"); checked: panel.closeGaps; onToggled: panel.closeGaps = checked }
        }

        // Find.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.pad
            Layout.rightMargin: Theme.pad
            Layout.bottomMargin: 4
            visible: panel.ready
            spacing: 4
            TextField {
                id: findText
                objectName: "transcriptFind"
                Layout.fillWidth: true
                implicitHeight: 24
                placeholderText: qsTr("Find words")
                color: Theme.text
                font.pixelSize: 12
                selectByMouse: true
                background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 3 }
                onTextChanged: session.transcriptFind(text)
                onAccepted: session.transcriptNextHit(1)
            }
            Text { text: session.transcriptHits.length > 0 ? qsTr("%1 found").arg(session.transcriptHits.length) : ""; color: Theme.muted; font.pixelSize: 11 }
            Chip { text: "▲"; enabled: session.transcriptHits.length > 0; onClicked: session.transcriptNextHit(-1) }
            Chip { text: "▼"; enabled: session.transcriptHits.length > 0; onClicked: session.transcriptNextHit(1) }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        // The words.
        ListView {
            id: paragraphs
            objectName: "transcriptList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: panel.ready
            model: session.transcriptParagraphs
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            delegate: Item {
                id: paragraph
                required property var modelData
                width: paragraphs.width
                height: body.height + 12
                Column {
                    id: body
                    x: Theme.pad
                    y: 6
                    width: parent.width - 2 * Theme.pad
                    spacing: 2
                    Text {
                        visible: paragraph.modelData.speaker !== ""
                        text: paragraph.modelData.speaker
                        color: Theme.accent
                        font.pixelSize: 10
                        font.bold: true
                    }
                    Flow {
                        width: parent.width
                        spacing: 4
                        Repeater {
                            model: paragraph.modelData.words
                            delegate: Rectangle {
                                id: cell
                                required property var modelData
                                readonly property bool chosen: modelData.i >= panel.selFirst && modelData.i <= panel.selLast && panel.selFirst >= 0
                                readonly property bool current: modelData.i === session.transcriptWord
                                width: label.implicitWidth + 6
                                height: label.implicitHeight + 2
                                radius: 3
                                color: chosen ? Theme.selection : (panel.isHit(modelData.i) ? Qt.rgba(1, 0.82, 0.3, 0.25) : "transparent")
                                border.width: current ? 1 : 0
                                border.color: Theme.accent
                                Text {
                                    id: label
                                    anchors.centerIn: parent
                                    text: cell.modelData.t
                                    font.pixelSize: 13
                                    font.italic: cell.modelData.low
                                    font.strikeout: false
                                    color: cell.modelData.f ? Theme.warning : (cell.chosen ? Theme.accent : Theme.text)
                                    opacity: cell.modelData.on ? (cell.modelData.part ? 0.75 : 1.0) : 0.35
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton
                                    onPressed: (mouse) => panel.chooseWord(cell.modelData.i, (mouse.modifiers & Qt.ShiftModifier) !== 0)
                                    onDoubleClicked: session.transcriptSeek(cell.modelData.i)
                                }
                            }
                        }
                    }
                }
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.border; opacity: 0.5 }
            }
        }
        Item { Layout.fillHeight: true; Layout.fillWidth: true; visible: !panel.ready }

        // What can be done with the chosen words.
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; visible: panel.ready }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            visible: panel.ready
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: panel.hasSelection ? qsTr("%n word(s) chosen", "", panel.selLast - panel.selFirst + 1) : session.transcriptSummary
                color: Theme.muted
                font.pixelSize: 10
                elide: Text.ElideRight
            }
            Chip { objectName: "transcriptDelete"; text: qsTr("Delete"); tone: Theme.danger; enabled: panel.hasSelection; ToolTip.text: qsTr("Take the chosen words out of the timeline (Delete)"); onClicked: panel.deleteChosen() }
            Chip { text: qsTr("Go to"); enabled: panel.hasSelection; ToolTip.text: qsTr("Move the playhead to the first chosen word (or double-click a word)"); onClicked: session.transcriptSeek(panel.selFirst) }
            Chip { text: qsTr("Correct…"); enabled: panel.hasSelection && panel.selFirst === panel.selLast; onClicked: { wordEditor.word = panel.selFirst; wordText.text = ""; wordEditor.open() } }
            Chip { text: qsTr("Speaker…"); enabled: panel.hasSelection; onClicked: { speakerText.text = ""; speakerEditor.open() } }
        }
    }
}

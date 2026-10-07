import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Cutline

// Exporting: pick what the file is for, see exactly what that will do to this sequence on this machine, and queue it.
Popup {
    id: dialog
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: 860
    height: 560
    padding: 12
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }

    property var presets: []
    property string presetId: ""
    property var plan: ({})
    readonly property var selected: {
        for (let i = 0; i < presets.length; ++i) if (presets[i].id === presetId) return presets[i]
        return null
    }

    function refresh() {
        plan = presetId === "" ? ({}) : session.exportPlan(presetId, fileName.text, useHardware.checked, useMarks.checked)
    }
    function suggestedName() {
        const folder = session.exportFolder()
        const ext = selected ? selected.extension : "mp4"
        return folder + "/" + session.projectName + "." + ext
    }
    function openFresh() {
        presets = session.exportPresets()
        if (presetId === "" && presets.length > 0) presetId = presets[0].id
        fileName.text = suggestedName()
        refresh()
        open()
    }
    function formatSize(bytes) {
        if (!bytes) return "—"
        if (bytes > 1e9) return (bytes / 1e9).toFixed(1) + " GB"
        if (bytes > 1e6) return (bytes / 1e6).toFixed(0) + " MB"
        return Math.max(1, Math.round(bytes / 1e3)) + " kB"
    }
    onPresetIdChanged: {
        if (!selected) return
        // The name follows the format: a preset that writes MOV does not keep a name ending .mp4.
        const text = fileName.text
        const dot = text.lastIndexOf(".")
        if (dot > text.lastIndexOf("/")) fileName.text = text.substring(0, dot + 1) + selected.extension
        refresh()
    }

    FileDialog {
        id: chooser
        fileMode: FileDialog.SaveFile
        title: qsTr("Save the export as")
        onAccepted: { fileName.text = selectedFile.toString().replace("file:///", ""); dialog.refresh() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        RowLayout {
            Text { text: qsTr("Export"); color: Theme.text; font.pixelSize: 15; font.bold: true; Layout.fillWidth: true }
            Chip { text: "✕"; onClicked: dialog.close() }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // What the file is for.
            ListView {
                id: list
                objectName: "exportPresetList"
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                clip: true
                model: dialog.presets
                section.property: "category"
                section.delegate: Text { text: section; color: Theme.accent; font.pixelSize: 11; font.bold: true; topPadding: 8; bottomPadding: 2 }
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: 26
                    color: modelData.id === dialog.presetId ? Theme.selection : (hover.containsMouse ? Theme.hover : "transparent")
                    Text { anchors { left: parent.left; leftMargin: 8; right: parent.right; verticalCenter: parent.verticalCenter } text: modelData.name; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                    MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true; onClicked: dialog.presetId = modelData.id }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8
                Text { text: dialog.selected ? dialog.selected.name : ""; color: Theme.text; font.pixelSize: 14; font.bold: true }
                Text { text: dialog.selected ? dialog.selected.description : ""; color: Theme.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }

                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: fileName
                        objectName: "exportFileName"
                        Layout.fillWidth: true
                        color: Theme.text
                        selectByMouse: true
                        background: Rectangle { color: Theme.field; border.color: Theme.border }
                        onEditingFinished: dialog.refresh()
                    }
                    Chip { text: qsTr("Browse…"); onClicked: chooser.open() }
                }
                RowLayout {
                    spacing: 16
                    Check { id: useHardware; objectName: "exportUseHardware"; checked: true; text: qsTr("Use the graphics card encoder when there is one"); onToggled: dialog.refresh() }
                }
                RowLayout {
                    spacing: 16
                    Check { id: useMarks; objectName: "exportUseMarks"; checked: false; enabled: session.markIn >= 0 || session.markOut >= 0; text: qsTr("Only from the in point to the out point"); onToggled: dialog.refresh() }
                    Check { id: overwrite; objectName: "exportOverwrite"; checked: false; text: qsTr("Replace an existing file") }
                }

                // What it will do here.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.bg
                    border.color: Theme.border
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 4
                        Text {
                            objectName: "exportRefusal"
                            visible: dialog.plan.ok === false
                            text: dialog.plan.refusal || ""
                            color: Theme.danger
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        GridLayout {
                            visible: dialog.plan.ok === true
                            columns: 2
                            columnSpacing: 12
                            rowSpacing: 3
                            Text { text: qsTr("Picture"); color: Theme.muted; font.pixelSize: 12 }
                            Text { objectName: "exportEncoder"; text: dialog.plan.encoder ? (dialog.plan.encoder + (dialog.plan.hardware ? qsTr("  (graphics card)") : qsTr("  (software)")) + "  " + (dialog.plan.summary || "")) : qsTr("none"); color: Theme.text; font.pixelSize: 12 }
                            Text { text: qsTr("Sound"); color: Theme.muted; font.pixelSize: 12 }
                            Text { text: dialog.plan.audioEncoder ? dialog.plan.audioEncoder : qsTr("none"); color: Theme.text; font.pixelSize: 12 }
                            Text { text: qsTr("Length"); color: Theme.muted; font.pixelSize: 12 }
                            Text { text: Number(dialog.plan.duration || 0).toFixed(1) + " s"; color: Theme.text; font.pixelSize: 12 }
                            Text { text: qsTr("About"); color: Theme.muted; font.pixelSize: 12 }
                            Text { text: dialog.formatSize(dialog.plan.estimatedBytes); color: Theme.text; font.pixelSize: 12 }
                        }
                        Repeater {
                            model: dialog.plan.notes || []
                            delegate: Text { required property string modelData; text: "• " + modelData; color: Theme.warning; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 11 }
                        }
                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            Chip { text: qsTr("Cancel"); onClicked: dialog.close() }
            Chip {
                objectName: "exportAdd"
                text: qsTr("Add to queue")
                enabled: dialog.plan.ok === true
                onClicked: { if (session.exportQueueAdd(dialog.presetId, fileName.text, "", overwrite.checked, useHardware.checked, useMarks.checked) !== "") dialog.close() }
            }
            Chip {
                objectName: "exportNow"
                text: qsTr("Export")
                active: true
                enabled: dialog.plan.ok === true
                onClicked: {
                    if (session.exportQueueAdd(dialog.presetId, fileName.text, "", overwrite.checked, useHardware.checked, useMarks.checked) !== "") {
                        session.exportPause(false)
                        session.showExports()
                        dialog.close()
                    }
                }
            }
        }
    }
}

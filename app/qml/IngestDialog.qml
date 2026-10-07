import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Cutline

// Ingest: bring files into the project the careful way. Each file is copied into the project's media folder (and read back
// to prove the copy is the same), imported, and given a proxy if asked. The jobs run in the background and can be stopped.
Dialog {
    id: dialog
    title: qsTr("Ingest media")
    modal: true
    width: 560
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Cancel
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
    property var files: []
    property var plan: ({ ok: false, items: [], problem: "", bytes: 0, freeBytes: 0 })
    property string destination: ""
    readonly property bool copying: copyBox.checked

    function refresh() {
        plan = session.ingestPlan(files, copyBox.checked, destination)
    }
    function size(bytes) {
        if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + " GB"
        if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " MB"
        return Math.max(1, Math.round(bytes / 1024)) + " KB"
    }
    onAboutToShow: { files = []; destination = session.ingestFolder(); refresh() }

    FileDialog {
        id: picker
        title: qsTr("Choose the media to ingest")
        fileMode: FileDialog.OpenFiles
        onAccepted: { dialog.files = selectedFiles.map(String); dialog.refresh() }
    }
    FolderDialog {
        id: folderPicker
        title: qsTr("Copy the media into")
        onAccepted: { dialog.destination = selectedFolder.toString(); dialog.refresh() }
    }

    contentItem: ColumnLayout {
        spacing: 8
        RowLayout {
            Chip { objectName: "ingestChoose"; text: qsTr("Choose files…"); onClicked: picker.open() }
            Text { Layout.fillWidth: true; text: dialog.files.length === 0 ? qsTr("No files chosen") : qsTr("%1 file(s), %2").arg(dialog.files.length).arg(dialog.size(dialog.plan.bytes)); color: Theme.muted; font.pixelSize: 11 }
        }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(160, Math.max(0, count * 22))
            visible: count > 0
            clip: true
            model: dialog.plan.items
            ScrollBar.vertical: ScrollBar {}
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                height: 22
                Text { Layout.fillWidth: true; text: modelData.name; color: modelData.problem !== "" ? Theme.danger : Theme.text; font.pixelSize: 11; elide: Text.ElideMiddle }
                Text { text: modelData.problem !== "" ? modelData.problem : (modelData.copy ? dialog.size(modelData.bytes) : qsTr("in place")); color: modelData.problem !== "" ? Theme.danger : Theme.muted; font.pixelSize: 10 }
            }
        }
        Check { id: copyBox; objectName: "ingestCopy"; text: qsTr("Copy into the project's media folder"); checked: true; onToggled: dialog.refresh() }
        RowLayout {
            enabled: copyBox.checked
            Text { Layout.fillWidth: true; text: dialog.destination; color: Theme.faint; font.pixelSize: 10; elide: Text.ElideMiddle }
            Chip { text: qsTr("Folder…"); onClicked: folderPicker.open() }
        }
        Check { id: verifyBox; objectName: "ingestVerify"; enabled: copyBox.checked; text: qsTr("Read each copy back and compare it with the original"); checked: true }
        RowLayout {
            Text { text: qsTr("Proxies"); color: Theme.muted; Layout.preferredWidth: 60 }
            ComboBox { id: proxyBox; objectName: "ingestProxy"; Layout.fillWidth: true; model: session.proxyChoices(); textRole: "label"; valueRole: "id"; currentIndex: 4 }
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.plan.problem !== ""
            text: dialog.plan.problem
            color: Theme.danger
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.copying && dialog.plan.freeBytes > 0
            text: qsTr("%1 free where the copies go").arg(dialog.size(dialog.plan.freeBytes))
            color: Theme.faint
            font.pixelSize: 10
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            Chip {
                objectName: "ingestStart"
                text: qsTr("Start")
                enabled: dialog.files.length > 0 && dialog.plan.ok
                active: true
                onClicked: {
                    session.ingestFiles(dialog.files, copyBox.checked, verifyBox.checked, dialog.destination, proxyBox.currentValue)
                    dialog.close()
                }
            }
        }
    }
}

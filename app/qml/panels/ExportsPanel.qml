import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The export queue: what is waiting, what is running, and what has finished and how it went.
Item {
    property string panelId: "exports"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            Text { Layout.fillWidth: true; text: qsTr("%1 exports").arg(session.exportJobs.length); color: Theme.muted; font.pixelSize: 11 }
            Chip { text: qsTr("New export…"); objectName: "exportsNew"; onClicked: session.trigger("file.export_media") }
            Chip { text: session.exportQueuePaused ? qsTr("Resume queue") : qsTr("Pause queue"); active: session.exportQueuePaused; objectName: "exportsPause"; onClicked: session.exportPause(!session.exportQueuePaused) }
            Chip { text: qsTr("Clear finished"); onClicked: session.exportClearFinished() }
        }
        ListView {
            id: list
            objectName: "exportJobList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: session.exportJobs
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                width: list.width
                height: 74
                color: "transparent"
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.border }
                ColumnLayout {
                    anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad; topMargin: 4; bottomMargin: 4 }
                    spacing: 2
                    RowLayout {
                        Text { Layout.fillWidth: true; text: row.modelData.name; color: Theme.text; font.pixelSize: 12; font.bold: true; elide: Text.ElideRight }
                        Text {
                            text: row.modelData.stateText
                            color: row.modelData.state === "failed" ? Theme.danger : row.modelData.state === "done" ? Theme.accent : row.modelData.state === "running" ? Theme.warning : Theme.muted
                            font.pixelSize: 11
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        font.pixelSize: 10
                        color: Theme.faint
                        elide: Text.ElideMiddle
                        text: (row.modelData.presetName || row.modelData.preset) + (row.modelData.encoder ? "  ·  " + row.modelData.encoder + (row.modelData.hardware ? " (card)" : "") : "") + "  ·  " + row.modelData.output
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        implicitHeight: 6
                        visible: row.modelData.state === "running" || row.modelData.state === "queued"
                        value: row.modelData.progress
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text !== ""
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        color: row.modelData.error !== "" || row.modelData.problems.length > 0 ? Theme.danger : Theme.muted
                        text: row.modelData.error !== "" ? row.modelData.error
                            : row.modelData.problems.length > 0 ? qsTr("The finished file has problems: %1").arg(row.modelData.problems.join("; "))
                            : row.modelData.state === "done" ? qsTr("%1 MB in %2 s").arg((row.modelData.bytes / 1e6).toFixed(1)).arg(row.modelData.seconds.toFixed(1)) : ""
                    }
                    RowLayout {
                        spacing: 4
                        Chip { height: 20; text: qsTr("Cancel"); visible: row.modelData.state === "running" || row.modelData.state === "queued"; onClicked: session.exportCancel(row.modelData.id) }
                        Chip { height: 20; text: qsTr("Retry"); visible: row.modelData.canRetry; onClicked: session.exportRetry(row.modelData.id) }
                        Chip { height: 20; text: qsTr("Show in folder"); visible: row.modelData.state === "done"; onClicked: session.exportReveal(row.modelData.output) }
                        Chip { height: 20; text: "▲"; visible: row.modelData.waiting; onClicked: session.exportMove(row.modelData.id, row.index - 1) }
                        Chip { height: 20; text: "▼"; visible: row.modelData.waiting; onClicked: session.exportMove(row.modelData.id, row.index + 1) }
                        Chip { height: 20; text: qsTr("Remove"); visible: row.modelData.state !== "running"; onClicked: session.exportRemove(row.modelData.id) }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: parent.count === 0
                text: qsTr("Nothing queued. File > Export Media… sends the sequence here.")
                color: Theme.faint
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Background work: imports, analysis, proxies, exports. Each shows what it is doing and can be stopped.
Item {
    property string panelId: "jobs"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            Text { Layout.fillWidth: true; text: qsTr("%1 jobs").arg(session.jobs.length); color: Theme.muted; font.pixelSize: 11 }
            Chip { text: qsTr("Clear finished"); onClicked: session.clearFinishedJobs() }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: session.jobs
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: list.width
                height: 48
                color: "transparent"
                ColumnLayout {
                    anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad; topMargin: 4; bottomMargin: 4 }
                    spacing: 2
                    RowLayout {
                        Text { Layout.fillWidth: true; text: modelData.title; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                        Text {
                            text: modelData.state
                            color: modelData.state === "failed" ? Theme.danger : modelData.state === "succeeded" ? Theme.accent : Theme.muted
                            font.pixelSize: 10
                        }
                        Chip { text: "×"; visible: modelData.active; tone: Theme.danger; implicitWidth: 22; onClicked: session.cancelJob(modelData.id) }
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        implicitHeight: 6
                        from: 0; to: 1
                        value: modelData.progress < 0 ? 0 : modelData.progress
                        indeterminate: modelData.progress < 0 && modelData.active
                    }
                    Text {
                        text: modelData.error !== "" ? modelData.error : modelData.message
                        color: modelData.error !== "" ? Theme.danger : Theme.muted
                        font.pixelSize: 10
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
            }
            Text { anchors.centerIn: parent; visible: list.count === 0; text: qsTr("Nothing is running"); color: Theme.faint; font.pixelSize: 12 }
        }
    }
}

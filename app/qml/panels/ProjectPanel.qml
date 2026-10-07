import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The media in the project. Double-click puts a clip on the timeline at the playhead; dragging a row onto the timeline
// puts it where it is dropped.
Item {
    property string panelId: "project"

    function fmt(seconds) {
        const m = Math.floor(seconds / 60), s = Math.floor(seconds % 60)
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: Theme.panelHeader
            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSoft }
            RowLayout {
                anchors { fill: parent; margins: Theme.pad }
                spacing: 6
                Chip { text: qsTr("Import…"); onClicked: session.trigger("file.import_media") }
                Chip { objectName: "projectIngest"; text: qsTr("Ingest…"); onClicked: session.trigger("file.ingest_media") }
                Chip {
                    text: qsTr("Insert")
                    enabled: list.currentIndex >= 0
                    onClicked: session.insertMedia(session.media[list.currentIndex].id, true)
                    ToolTip.text: qsTr("Insert at the playhead, pushing later clips along (,)")
                }
                Chip {
                    text: qsTr("Overwrite")
                    enabled: list.currentIndex >= 0
                    onClicked: session.insertMedia(session.media[list.currentIndex].id, false)
                    ToolTip.text: qsTr("Overwrite at the playhead (.)")
                }
                Item { Layout.fillWidth: true }
                Check {
                    objectName: "useProxies"
                    text: qsTr("Proxies")
                    checked: session.useProxies
                    onToggled: session.useProxies = checked
                    ToolTip.text: qsTr("Play proxies where they are ready; exports always use the originals")
                    ToolTip.visible: hovered
                }
                Text { text: session.media.length; color: Theme.faint; font.family: Theme.mono; font.pixelSize: 11 }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: session.media
            currentIndex: -1
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                width: list.width
                height: 48
                color: ListView.isCurrentItem ? Theme.selection : (rowMouse.containsMouse ? Theme.hover : "transparent")
                Rectangle { visible: ListView.isCurrentItem; anchors { left: parent.left; top: parent.top; bottom: parent.bottom } width: 3; color: Theme.accent }
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.borderSoft }
                property string mediaId: modelData.id
                Drag.active: rowMouse.drag.active
                Drag.keys: ["media"]
                Drag.hotSpot.x: 20
                Drag.hotSpot.y: 20
                Row {
                    anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad }
                    spacing: 8
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 38; height: 28; radius: 5
                        color: row.modelData.video ? "#397fa6" : "#3b9365"
                        Text { anchors.centerIn: parent; text: row.modelData.video ? "V" : "A"; color: Theme.text; font.family: Theme.ui; font.pixelSize: 11; font.bold: true }
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        Text { text: row.modelData.name; color: row.modelData.missing ? Theme.danger : Theme.text; font.family: Theme.ui; font.pixelSize: 12; font.weight: Font.DemiBold; elide: Text.ElideRight; width: row.width - 140 }
                        Text {
                            text: fmt(row.modelData.duration) + (row.modelData.video ? "  video" : "") + (row.modelData.audio ? "  audio" : "") +
                                  (row.modelData.proxyStatus === "ready" ? "  proxy ready" : row.modelData.proxyStatus === "stale" ? "  proxy out of date" : row.modelData.proxyStatus === "missing" ? "  proxy file missing" : "") +
                                  (row.modelData.proxyBusy ? "  making proxy " + Math.round(row.modelData.proxyProgress * 100) + "%" : "") + (row.modelData.missing ? "  OFFLINE" : "")
                            color: row.modelData.proxyStatus === "stale" || row.modelData.proxyStatus === "missing" ? Theme.warning : Theme.muted
                            font.family: Theme.ui
                            font.pixelSize: 10
                        }
                    }
                }
                Chip {
                    z: 2
                    anchors { right: parent.right; rightMargin: Theme.pad; verticalCenter: parent.verticalCenter }
                    height: 22
                    visible: row.modelData.video && !row.modelData.missing && !row.modelData.proxyBusy
                    text: row.modelData.proxyStatus === "none" ? qsTr("+ Proxy") : qsTr("Proxy")
                    active: row.modelData.proxyStatus === "ready"
                    onClicked: proxyMenu.popup()
                    Menu {
                        id: proxyMenu
                        MenuItem { text: row.modelData.proxyStatus === "none" ? qsTr("Make a proxy (HD)") : qsTr("Make it again (HD)"); onTriggered: session.proxyCreate(row.modelData.id, "720") }
                        MenuItem { text: qsTr("Make a full HD proxy"); onTriggered: session.proxyCreate(row.modelData.id, "1080") }
                        MenuItem { text: qsTr("Make a light proxy"); onTriggered: session.proxyCreate(row.modelData.id, "540") }
                        MenuItem { text: qsTr("Remove the proxy"); enabled: row.modelData.proxyStatus !== "none"; onTriggered: session.proxyRemove(row.modelData.id) }
                    }
                }
                ProgressBar {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 3
                    visible: row.modelData.proxyBusy
                    value: row.modelData.proxyProgress
                }
                MouseArea {
                    id: rowMouse
                    z: 1
                    anchors.fill: parent
                    hoverEnabled: true
                    drag.target: row
                    drag.threshold: 8
                    onPressed: { list.currentIndex = row.index; session.selectMedia(row.modelData.id) }
                    onDoubleClicked: session.insertMedia(row.modelData.id, false)
                    onReleased: { row.x = 0; row.y = row.index * row.height }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: session.projectOpen ? qsTr("Import media to begin") : qsTr("Create or open a project")
                color: Theme.faint
                font.pixelSize: 13
            }
        }
    }
}

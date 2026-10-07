import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Multicam: the angle monitor, live cutting against a running clock, and the list of cuts to refine.
// Digits 1-9 cut to the angle with that number at the clock; clicking a tile does the same.
Item {
    id: panel
    property string panelId: "multicam"
    readonly property bool open: session.multicamGroup !== ""

    MulticamSetupDialog { id: setup }

    // No group open: the groups in the project, and a way to make one.
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: 6
        visible: !panel.open

        RowLayout {
            Text { text: qsTr("Multicam groups"); color: Theme.text; font.bold: true; Layout.fillWidth: true }
            Chip { text: qsTr("New group…"); onClicked: setup.openFresh() }
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: session.multicamGroups
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                height: 34
                color: "transparent"
                RowLayout {
                    anchors.fill: parent
                    Text { text: modelData.name; color: Theme.text; Layout.fillWidth: true; elide: Text.ElideRight }
                    Text { text: qsTr("%1 angles, %2 s").arg(modelData.angles).arg(Number(modelData.duration).toFixed(1)); color: Theme.muted; font.pixelSize: 11 }
                    Chip { text: qsTr("Open"); onClicked: session.multicamOpen(modelData.id) }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: parent.count === 0
                text: qsTr("No groups yet. Choose two or more clips of one event and line them up.")
                color: Theme.faint
                width: parent.width - 24
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: panel.open

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 6
            spacing: 6
            Chip { text: session.multicamPlaying ? "■" : "▶"; objectName: "multicamPlay"; onClicked: session.multicamPlay(!session.multicamPlaying); ToolTip.text: qsTr("Play or stop (Shift+Space)") }
            Text { text: Number(session.multicamPosition).toFixed(2) + " / " + Number(session.multicamDuration).toFixed(1) + " s"; color: Theme.text; font.family: Theme.mono; font.pixelSize: 12 }
            Text { text: session.multicamName; color: Theme.muted; elide: Text.ElideRight; Layout.fillWidth: true }
            Text { text: qsTr("sync: %1").arg(session.multicamSync); color: Theme.faint; font.pixelSize: 11 }
            Chip { text: qsTr("Lay out…"); onClicked: layout.open() }
            Chip { text: qsTr("Delete"); tone: Theme.danger; onClicked: session.multicamDelete() }
            Chip { text: "✕"; onClicked: session.multicamClose() }
        }

        Slider {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            from: 0
            to: Math.max(0.1, session.multicamDuration)
            value: session.multicamPosition
            onMoved: session.multicamSeek(value)
        }

        MulticamItem {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            session: appSession
        }

        // One button per angle: the key that cuts to it, its name (double-click to rename) and a frame-by-frame sync nudge.
        Flow {
            Layout.fillWidth: true
            Layout.margins: 6
            spacing: 6
            Repeater {
                model: session.multicamAngles
                delegate: Rectangle {
                    required property var modelData
                    width: 150
                    height: 46
                    radius: 3
                    color: modelData.active ? Qt.rgba(1, 0.42, 0.42, 0.18) : Theme.field
                    border.color: modelData.active ? Theme.danger : Theme.border
                    MouseArea {
                        anchors.fill: parent
                        onClicked: session.multicamCut(modelData.index)
                        onDoubleClicked: { renameField.text = modelData.name; renameField.visible = true; renameField.forceActiveFocus() }
                    }
                    Text { x: 6; y: 4; text: (modelData.key !== "" ? modelData.key + "  " : "") + modelData.name; color: Theme.text; font.pixelSize: 12; width: parent.width - 12; elide: Text.ElideRight }
                    Row {
                        x: 6; y: 24; spacing: 4
                        Chip { height: 18; text: "◂"; onClicked: session.multicamNudgeSync(modelData.index, -1); ToolTip.text: qsTr("Sync one frame earlier") }
                        Text { text: qsTr("%1 s").arg(Number(modelData.offset).toFixed(2)); color: Theme.muted; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter }
                        Chip { height: 18; text: "▸"; onClicked: session.multicamNudgeSync(modelData.index, 1); ToolTip.text: qsTr("Sync one frame later") }
                    }
                    TextField {
                        id: renameField
                        visible: false
                        anchors.fill: parent
                        color: Theme.text
                        background: Rectangle { color: Theme.bg; border.color: Theme.accent }
                        onAccepted: { session.multicamRenameAngle(modelData.index, text); visible = false }
                        onActiveFocusChanged: if (!activeFocus) visible = false
                    }
                }
            }
        }

        // The cuts: each can be nudged, given another angle, or removed; selecting one parks the clock on it.
        ListView {
            id: cuts
            objectName: "multicamCuts"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(140, 26 * Math.max(1, count))
            clip: true
            model: session.multicamCuts
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: cuts.width
                height: 26
                color: "transparent"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    spacing: 4
                    Text { text: Number(modelData.time).toFixed(2) + " s"; color: Theme.text; font.family: Theme.mono; font.pixelSize: 11; Layout.preferredWidth: 74 }
                    ComboBox {
                        Layout.preferredWidth: 130
                        implicitHeight: 22
                        model: session.multicamAngles.map(a => a.name)
                        currentIndex: modelData.angleIndex
                        onActivated: (i) => session.multicamChangeCut(modelData.index, i)
                    }
                    Chip { text: "−"; enabled: modelData.index > 0; height: 20; onClicked: session.multicamNudgeCut(modelData.index, -1); ToolTip.text: qsTr("One frame earlier") }
                    Chip { text: "+"; enabled: modelData.index > 0; height: 20; onClicked: session.multicamNudgeCut(modelData.index, 1); ToolTip.text: qsTr("One frame later") }
                    Chip { text: qsTr("Go"); height: 20; onClicked: session.multicamSeek(modelData.time) }
                    Chip { text: "✕"; enabled: modelData.index > 0; height: 20; onClicked: session.multicamRemoveCut(modelData.index); ToolTip.text: qsTr("Remove this cut") }
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }

    // Laying the programme out as ordinary clips at the timeline playhead.
    Popup {
        id: layout
        anchors.centerIn: Overlay.overlay
        width: 320
        padding: 12
        modal: true
        background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Text { text: qsTr("Lay out on the timeline"); color: Theme.text; font.bold: true }
            RowLayout { Text { text: qsTr("Picture on"); color: Theme.muted; Layout.preferredWidth: 80 } ComboBox { id: videoTrack; Layout.fillWidth: true; model: session.trackIds("video") } }
            RowLayout { Text { text: qsTr("Sound on"); color: Theme.muted; Layout.preferredWidth: 80 } ComboBox { id: audioTrack; Layout.fillWidth: true; model: [qsTr("(none)")].concat(session.trackIds("audio")) } }
            ComboBox {
                id: soundFrom
                Layout.fillWidth: true
                model: [qsTr("Sound follows the picture")].concat(session.multicamAngles.map(a => qsTr("Sound from %1 throughout").arg(a.name)))
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                Chip { text: qsTr("Cancel"); onClicked: layout.close() }
                Chip {
                    text: qsTr("Lay out")
                    active: true
                    onClicked: {
                        const ok = session.multicamFlatten(videoTrack.currentText, audioTrack.currentIndex === 0 ? "" : audioTrack.currentText, soundFrom.currentIndex > 0, soundFrom.currentIndex - 1)
                        if (ok) layout.close()
                    }
                }
            }
        }
    }
}

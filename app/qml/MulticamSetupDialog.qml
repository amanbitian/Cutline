import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Making a multicam group: tick the clips that are angles of one event, name them, say how they line up and which
// one the rest line up to (it is also the angle the programme starts on).
Popup {
    id: dialog
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: 640
    height: 460
    padding: 12
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }

    property var candidates: []
    property int reference: 0

    function openFresh() {
        candidates = session.multicamCandidatesList()
        reference = 0
        groupName.text = qsTr("Multicam")
        open()
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Text { text: qsTr("New multicam group"); color: Theme.text; font.pixelSize: 15; font.bold: true }
        RowLayout {
            Text { text: qsTr("Name"); color: Theme.muted; Layout.preferredWidth: 60 }
            TextField { id: groupName; objectName: "multicamGroupName"; Layout.fillWidth: true; color: Theme.text; background: Rectangle { color: Theme.field; border.color: Theme.border } }
            Text { text: qsTr("Line up by"); color: Theme.muted }
            ComboBox {
                id: syncMethod
                objectName: "multicamSyncMethod"
                model: [qsTr("Sound"), qsTr("Timecode"), qsTr("Marker"), qsTr("By hand")]
                readonly property var keys: ["audio", "timecode", "marker", "manual"]
            }
        }
        Text {
            color: Theme.faint
            font.pixelSize: 11
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: syncMethod.currentIndex === 0 ? qsTr("The sound of each clip is compared with the reference's (the first minute and a half), so a clap or any shared noise lines them up.")
                : syncMethod.currentIndex === 1 ? qsTr("Clips are lined up by the timecode each was recorded with.")
                : syncMethod.currentIndex === 2 ? qsTr("Give the time of one shared moment (a clap, a slate) in every clip.")
                : qsTr("Give, for every clip, the time in it that matches the start of the reference.")
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.candidates
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                property bool included: check.checked
                property string mediaId: modelData.id
                property string angleName: nameField.text
                property real offsetSeconds: parseFloat(offsetField.text) || 0
                property string markerText: markerField.text
                width: ListView.view.width
                height: 34
                color: index === dialog.reference ? Theme.selection : "transparent"
                RowLayout {
                    anchors.fill: parent
                    spacing: 6
                    Check { id: check; checked: row.modelData.hasVideo; enabled: row.modelData.hasVideo }
                    Text { text: row.modelData.name; color: Theme.text; Layout.preferredWidth: 150; elide: Text.ElideRight }
                    Text { text: Number(row.modelData.duration).toFixed(1) + " s" + (row.modelData.hasAudio ? "" : qsTr(", no sound")); color: Theme.muted; font.pixelSize: 11; Layout.preferredWidth: 100 }
                    TextField { id: nameField; placeholderText: qsTr("Angle name"); Layout.preferredWidth: 110; color: Theme.text; background: Rectangle { color: Theme.field; border.color: Theme.border } }
                    TextField { id: offsetField; visible: syncMethod.currentIndex === 3; placeholderText: qsTr("offset s"); Layout.preferredWidth: 70; color: Theme.text; background: Rectangle { color: Theme.field; border.color: Theme.border } }
                    TextField { id: markerField; visible: syncMethod.currentIndex === 2; placeholderText: qsTr("marker s"); Layout.preferredWidth: 70; color: Theme.text; background: Rectangle { color: Theme.field; border.color: Theme.border } }
                    Chip { text: index === dialog.reference ? qsTr("reference") : qsTr("make reference"); active: index === dialog.reference; onClicked: dialog.reference = row.index }
                }
            }
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            Chip { text: qsTr("Cancel"); onClicked: dialog.close() }
            Chip {
                objectName: "multicamCreate"
                text: qsTr("Create")
                active: true
                onClicked: {
                    // Rows are delegates of the list: gather the ticked ones in order.
                    const chosen = []
                    let referenceIndex = 0
                    for (let i = 0; i < list.count; ++i) {
                        const item = list.itemAtIndex(i)
                        if (item && item.included) {
                            if (i === dialog.reference) referenceIndex = chosen.length
                            chosen.push({ mediaId: item.mediaId, name: item.angleName, offset: item.offsetSeconds, marker: item.markerText })
                        }
                    }
                    const id = session.multicamCreate(chosen, groupName.text, syncMethod.keys[syncMethod.currentIndex], referenceIndex)
                    if (id !== "") dialog.close()
                }
            }
        }
    }
}

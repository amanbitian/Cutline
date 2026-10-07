import QtQuick
import Cutline

// A panel this build has a place for but no content yet; it says so rather than showing something that looks real.
Item {
    property string panelId: ""
    Column {
        anchors.centerIn: parent
        spacing: 6
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: dockTitle()
            color: Theme.muted
            font.pixelSize: 14
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Not built yet")
            color: Theme.faint
            font.pixelSize: 12
        }
    }
    function dockTitle() {
        switch (panelId) {
        case "scopes": return qsTr("Scopes")
        case "audio_mixer": return qsTr("Audio Mixer")
        case "markers": return qsTr("Markers")
        default: return panelId
        }
    }
}

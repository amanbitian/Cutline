import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The spaces the sequence is graded in and shown in. Pictures are brought into the working space as they are drawn
// and the finished picture is converted to the display space; a sequence made before colour management is brought up to
// the current rules when it is given spaces (that is how it is told to treat them as meaningful).
Dialog {
    id: dialog
    title: qsTr("Sequence colour")
    modal: true
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Ok | Dialog.Cancel
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
    property var spaces: session.colorSpaces()

    function indexOf(id) {
        for (let i = 0; i < spaces.length; ++i) if (spaces[i].id === id) return i
        return 0
    }
    onAboutToShow: {
        spaces = session.colorSpaces()
        working.currentIndex = indexOf(session.sequenceColor().working)
        display.currentIndex = indexOf(session.sequenceColor().display)
    }

    ColumnLayout {
        spacing: 8
        width: 360
        RowLayout {
            Text { text: qsTr("Working space"); color: Theme.muted; Layout.preferredWidth: 110 }
            ComboBox { id: working; objectName: "workingSpace"; Layout.fillWidth: true; model: dialog.spaces; textRole: "label"; valueRole: "id" }
        }
        RowLayout {
            Text { text: qsTr("Display space"); color: Theme.muted; Layout.preferredWidth: 110 }
            ComboBox { id: display; objectName: "displaySpace"; Layout.fillWidth: true; model: dialog.spaces; textRole: "label"; valueRole: "id" }
        }
        Text {
            Layout.fillWidth: true
            text: session.sequenceColor().managed ? qsTr("Pictures are converted into the working space, and the result into the display space.")
                                                  : qsTr("This sequence was made before colour management. Choosing spaces brings it up to the current rendering rules.")
            color: Theme.faint
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("ACES and camera log spaces are scene-referred: highlights above white are fitted under 1 with a soft knee. This is not the ACES output transform.")
            color: Theme.faint
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
    }
    onAccepted: session.setSequenceColor(working.currentValue, display.currentValue)
}

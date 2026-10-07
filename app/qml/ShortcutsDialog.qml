import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Every command with its keys. Select one, press Record, press the key combination to give it; a key that another
// command had is taken from it, and the status line says so.
Dialog {
    id: dialog
    title: qsTr("Keyboard shortcuts")
    modal: true
    width: 640
    height: 520
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Close
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
    property var commands: session.commandList()
    property string filter: ""
    property bool recording: false

    onAboutToShow: { commands = session.commandList(); recording = false }

    function visibleCommands() {
        const q = filter.toLowerCase()
        return commands.filter(c => q === "" || c.label.toLowerCase().indexOf(q) >= 0 || c.category.toLowerCase().indexOf(q) >= 0)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            TextField {
                Layout.fillWidth: true
                placeholderText: qsTr("Filter")
                color: Theme.text
                background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 2 }
                onTextChanged: dialog.filter = text
            }
            Chip {
                text: dialog.recording ? qsTr("Press keys…") : qsTr("Record")
                active: dialog.recording
                enabled: list.currentIndex >= 0
                onClicked: { dialog.recording = !dialog.recording; recorder.forceActiveFocus() }
            }
            Chip { text: qsTr("Reset all"); tone: Theme.danger; onClicked: { session.resetShortcuts(); dialog.commands = session.commandList() } }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.visibleCommands()
            currentIndex: -1
            ScrollBar.vertical: ScrollBar {}
            section.property: "category"
            section.delegate: Rectangle {
                required property string section
                width: list.width; height: 22; color: Theme.panelHeader
                Text { anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter } text: parent.section; color: Theme.muted; font.pixelSize: 11; font.bold: true }
            }
            delegate: Rectangle {
                required property var modelData
                required property int index
                width: list.width
                height: 26
                color: ListView.isCurrentItem ? Theme.selection : "transparent"
                RowLayout {
                    anchors { fill: parent; leftMargin: 14; rightMargin: 8 }
                    Text { Layout.fillWidth: true; text: modelData.label; color: Theme.text; font.pixelSize: 12 }
                    Text { text: modelData.shortcut; color: Theme.accent; font.pixelSize: 11; font.family: Theme.mono }
                }
                MouseArea { anchors.fill: parent; onClicked: { list.currentIndex = index; dialog.recording = false } }
            }
        }
    }

    // Takes the next key press while recording.
    Item {
        id: recorder
        focus: dialog.recording
        Keys.onPressed: (event) => {
            if (!dialog.recording) return
            if (event.key === Qt.Key_Control || event.key === Qt.Key_Shift || event.key === Qt.Key_Alt || event.key === Qt.Key_Meta) return
            const names = {}
            names[Qt.Key_Space] = "Space"; names[Qt.Key_Left] = "Left"; names[Qt.Key_Right] = "Right"; names[Qt.Key_Up] = "Up"; names[Qt.Key_Down] = "Down"
            names[Qt.Key_Home] = "Home"; names[Qt.Key_End] = "End"; names[Qt.Key_Delete] = "Delete"; names[Qt.Key_Backspace] = "Backspace"
            names[Qt.Key_Return] = "Enter"; names[Qt.Key_Escape] = "Escape"; names[Qt.Key_Tab] = "Tab"; names[Qt.Key_Comma] = "Comma"
            names[Qt.Key_Period] = "Period"; names[Qt.Key_Slash] = "Slash"; names[Qt.Key_Backslash] = "Backslash"; names[Qt.Key_Semicolon] = "Semicolon"
            names[Qt.Key_Apostrophe] = "Quote"; names[Qt.Key_Equal] = "Equal"; names[Qt.Key_Minus] = "Minus"
            let key = names[event.key]
            if (key === undefined) {
                if (event.key >= Qt.Key_A && event.key <= Qt.Key_Z) key = String.fromCharCode(65 + event.key - Qt.Key_A)
                else if (event.key >= Qt.Key_0 && event.key <= Qt.Key_9) key = String.fromCharCode(48 + event.key - Qt.Key_0)
                else if (event.key >= Qt.Key_F1 && event.key <= Qt.Key_F12) key = "F" + (event.key - Qt.Key_F1 + 1)
                else return
            }
            let chord = (event.modifiers & Qt.ControlModifier ? "Ctrl+" : "") + (event.modifiers & Qt.ShiftModifier ? "Shift+" : "") + (event.modifiers & Qt.AltModifier ? "Alt+" : "") + key
            const command = dialog.visibleCommands()[list.currentIndex]
            if (command && session.bindShortcut(command.id, chord)) dialog.commands = session.commandList()
            dialog.recording = false
            event.accepted = true
        }
    }
}

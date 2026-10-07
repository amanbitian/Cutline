import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Type a few words, press Enter, run the command. The same commands as the menus and keys, found by name.
Popup {
    id: palette
    modal: true
    width: 520
    height: 380
    x: (parent.width - width) / 2
    y: 80
    padding: 10
    background: Rectangle { color: Theme.panel; border.color: Theme.accent; radius: 4 }
    property var results: session.searchCommands("")

    onAboutToShow: { field.text = ""; results = session.searchCommands(""); list.currentIndex = 0 }
    onOpened: field.forceActiveFocus()

    function run() {
        if (list.currentIndex >= 0 && list.currentIndex < results.length) {
            const id = results[list.currentIndex].id
            close()
            session.trigger(id)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        TextField {
            id: field
            Layout.fillWidth: true
            placeholderText: qsTr("Type a command")
            color: Theme.text
            background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 2 }
            onTextChanged: { palette.results = session.searchCommands(text); list.currentIndex = 0 }
            Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
            Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
            Keys.onReturnPressed: palette.run()
            Keys.onEnterPressed: palette.run()
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: palette.results
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                required property int index
                width: list.width
                height: 28
                color: ListView.isCurrentItem ? Theme.selection : "transparent"
                RowLayout {
                    anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                    Text { text: modelData.category; color: Theme.faint; font.pixelSize: 10; Layout.preferredWidth: 70 }
                    Text { Layout.fillWidth: true; text: modelData.label; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                    Text { text: modelData.shortcut; color: Theme.muted; font.pixelSize: 11; font.family: Theme.mono }
                }
                MouseArea { anchors.fill: parent; onClicked: { list.currentIndex = index; palette.run() } }
            }
        }
    }
}

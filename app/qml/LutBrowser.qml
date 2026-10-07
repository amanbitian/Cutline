import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Cutline

// Picks a look-up table from the folders that have been added, by name; applying one adds a LUT effect to the clip.
Popup {
    id: browser
    modal: true
    width: 460
    height: 420
    anchors.centerIn: Overlay.overlay
    padding: 12
    property var entries: session.lutEntries(search.text)
    property var folders: []

    onAboutToShow: refresh()
    function refresh() { entries = session.lutEntries(search.text) }

    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Text { text: qsTr("Look-up tables"); color: Theme.text; font.pixelSize: 14; font.bold: true }
        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search by name")
                color: Theme.text
                background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 2 }
                onTextChanged: browser.refresh()
            }
            Chip { text: qsTr("Add folder…"); onClicked: folderDialog.open() }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Looks that come with Cutline are in “Cutline Looks”. Put your own .cube files (3D cubes or 1D curves) in the folder below, or add another folder.")
            color: Theme.muted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            visible: session.lutFolder() !== ""
            Text { Layout.fillWidth: true; text: session.lutFolder(); color: Theme.faint; font.pixelSize: 10; elide: Text.ElideMiddle }
            Chip { text: qsTr("Open"); onClicked: session.exportReveal(session.lutFolder()) }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: browser.entries
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: list.width
                height: 62
                color: area.containsMouse ? Theme.hover : "transparent"
                RowLayout {
                    anchors { fill: parent; leftMargin: 6; rightMargin: 6; topMargin: 3; bottomMargin: 3 }
                    spacing: 8
                    Image {
                        Layout.preferredWidth: 96
                        Layout.preferredHeight: 54
                        source: "image://lutpreview/" + encodeURIComponent(modelData.path)
                        sourceSize: Qt.size(192, 108)
                        asynchronous: true
                        cache: false
                        fillMode: Image.PreserveAspectFit
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text { Layout.fillWidth: true; text: modelData.name; color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight }
                        Text { Layout.fillWidth: true; text: modelData.group; color: Theme.faint; font.pixelSize: 10; elide: Text.ElideRight }
                    }
                    Text { text: modelData.curves ? qsTr("1D curves") : modelData.size + "³"; color: Theme.muted; font.pixelSize: 10 }
                }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    onDoubleClicked: { session.addEffect("lut", modelData.path); browser.close() }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Chip { text: qsTr("Close"); onClicked: browser.close() }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Folder with .cube files")
        onAccepted: {
            browser.folders = browser.folders.concat([selectedFolder.toString()])
            session.setLutFolders(browser.folders)
            browser.refresh()
        }
    }
}

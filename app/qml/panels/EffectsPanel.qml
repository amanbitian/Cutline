import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The effects that can be put on a clip, grouped, searchable. Double-click adds one to the selected clip.
Item {
    property string panelId: "effects"
    property string mode: "effects"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            Layout.bottomMargin: 0
            spacing: 4
            Chip { objectName: "modeEffects"; text: qsTr("Effects"); active: mode === "effects"; onClicked: mode = "effects" }
            Chip { objectName: "modeTransitions"; text: qsTr("Transitions"); active: mode === "transitions"; onClicked: mode = "transitions" }
            Item { Layout.fillWidth: true }
            Text { visible: mode === "transitions"; text: qsTr("Double-click to put one on the cut at the playhead"); color: Theme.faint; font.pixelSize: 10 }
        }
        TextField {
            id: search
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            placeholderText: qsTr("Search effects")
            color: Theme.text
            background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 2 }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: mode === "effects" ? session.effectCatalogue(search.text) : session.transitionCatalogue(search.text)
            ScrollBar.vertical: ScrollBar {}
            section.property: "category"
            section.delegate: Rectangle {
                required property string section
                width: list.width
                height: 24
                color: Theme.panelHeader
                Text { anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter } text: parent.section; color: Theme.muted; font.pixelSize: 11; font.bold: true }
            }
            delegate: Rectangle {
                required property var modelData
                width: list.width
                height: 26
                color: area.containsMouse ? Theme.hover : "transparent"
                Text { anchors { left: parent.left; leftMargin: 18; verticalCenter: parent.verticalCenter } text: modelData.name; color: Theme.text; font.pixelSize: 12 }
                Text { anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter } visible: mode === "effects" && modelData.needsAsset === true; text: modelData.assetKind === "colorspace" ? qsTr("space") : ".cube"; color: Theme.faint; font.pixelSize: 10 }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    onDoubleClicked: {
                        if (mode === "transitions") session.addTransition(modelData.id, 1.0)
                        else if (modelData.assetKind === "colorspace") spaceMenu.popup()
                        else if (modelData.needsAsset) lutBrowser.open()
                        else session.addEffect(modelData.id, "")
                    }
                }
            }
        }
    }
    LutBrowser { id: lutBrowser }
    ColorSpaceMenu { id: spaceMenu }
}

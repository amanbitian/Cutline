import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Cutline

// Titles and graphics of the project: the controls of the title that is chosen on the timeline (a template's own fields, the
// way a motion-graphics template offers them), the library of the project's graphics with thumbnails, and the templates
// a title can be made from. Placing one on the timeline, making one, importing and exporting a template are each one step.
Item {
    id: panel
    property string panelId: "graphics"
    property string chosen: ""            // a graphic of the library
    property string chosenTemplate: ""    // a template of the browser
    property real placeSeconds: 5
    readonly property var revision: session.graphicsRevision
    readonly property var library: revision >= 0 ? session.graphicLibrary() : []
    readonly property var templates: revision >= 0 ? session.graphicTemplates() : []
    readonly property var clip: (revision >= 0 && session.selectionCount >= 0) ? session.selectedGraphic() : ({})
    readonly property bool hasClipGraphic: clip.graphic !== undefined
    readonly property var chosenInfo: {
        for (let i = 0; i < library.length; ++i) if (library[i].id === chosen) return library[i]
        return null
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import a template or graphic")
        nameFilters: [qsTr("Graphics (*.json)"), qsTr("All files (*)")]
        onAccepted: session.importGraphicTemplate(selectedFile.toString())
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export as a template")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("Template (*.json)")]
        onAccepted: session.exportGraphicTemplate(panel.chosen, selectedFile.toString())
    }

    Flickable {
        anchors.fill: parent
        contentHeight: column.height + 2 * Theme.pad
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            x: Theme.pad
            y: Theme.pad
            width: parent.width - 2 * Theme.pad
            spacing: 8

            // ---- the title chosen on the timeline
            Text {
                Layout.fillWidth: true
                text: panel.hasClipGraphic ? qsTr("On the timeline: %1").arg(panel.clip.name) : qsTr("Choose a title on the timeline to change its text and colours here")
                color: panel.hasClipGraphic ? Theme.text : Theme.muted
                font.pixelSize: 12
                font.bold: panel.hasClipGraphic
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: panel.hasClipGraphic ? panel.clip.controls : []
                delegate: RowLayout {
                    id: control
                    required property var modelData
                    Layout.fillWidth: true
                    Text { text: control.modelData.label; color: Theme.muted; font.pixelSize: 11; Layout.preferredWidth: 90; elide: Text.ElideRight }
                    Loader {
                        Layout.fillWidth: true
                        sourceComponent: control.modelData.type === "number" ? numberField : (control.modelData.type === "colour" ? colourField : textField)
                        onLoaded: item.info = control.modelData
                    }
                }
            }
            RowLayout {
                visible: panel.hasClipGraphic && panel.clip.kind === "graphic"
                Layout.fillWidth: true
                Chip { objectName: "editClipGraphic"; text: qsTr("Edit in designer"); onClicked: session.openGraphic(panel.clip.graphic) }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

            // ---- the library
            RowLayout {
                Layout.fillWidth: true
                Text { text: qsTr("Graphics"); color: Theme.text; font.pixelSize: 12; font.bold: true; Layout.fillWidth: true }
                Chip { objectName: "newGraphic"; text: qsTr("New"); enabled: session.projectOpen; onClicked: panel.chosen = session.newGraphic("") }
                Chip { objectName: "importGraphic"; text: qsTr("Import…"); enabled: session.projectOpen; onClicked: importDialog.open() }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: panel.library
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        objectName: "graphic_" + modelData.id
                        width: 120
                        height: 92
                        color: panel.chosen === modelData.id ? Theme.selection : Theme.field
                        border.color: panel.chosen === modelData.id ? Theme.accent : Theme.border
                        radius: 3
                        Image {
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 3 }
                            height: 62
                            source: card.modelData.preview
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                        }
                        Text {
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 4 }
                            text: card.modelData.name + (card.modelData.uses > 0 ? "  ×" + card.modelData.uses : "")
                            color: Theme.text
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: panel.chosen = card.modelData.id
                            onDoubleClicked: if (card.modelData.kind === "graphic") session.openGraphic(card.modelData.id)
                        }
                    }
                }
            }
            Text { visible: panel.library.length === 0; text: qsTr("No graphics yet. Make one, or start from a template below."); color: Theme.faint; font.pixelSize: 11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Flow {
                Layout.fillWidth: true
                spacing: 4
                visible: panel.chosenInfo !== null
                Chip { objectName: "placeGraphic"; text: qsTr("Add to timeline"); onClicked: session.placeGraphic(panel.chosen, panel.placeSeconds) }
                NumberBox { objectName: "placeSeconds"; value: panel.placeSeconds; minimum: 0.1; maximum: 3600; width: 54; onCommitted: (v) => panel.placeSeconds = v }
                Text { text: qsTr("s"); color: Theme.muted; font.pixelSize: 11; height: 22; verticalAlignment: Text.AlignVCenter }
                Chip { objectName: "designGraphic"; text: qsTr("Design"); enabled: panel.chosenInfo !== null && panel.chosenInfo.kind === "graphic"; onClicked: session.openGraphic(panel.chosen) }
                Chip { objectName: "exportGraphic"; text: qsTr("Export…"); onClicked: exportDialog.open() }
                Chip { objectName: "deleteGraphic"; text: qsTr("Delete"); tone: Theme.danger; onClicked: { session.deleteGraphic(panel.chosen); panel.chosen = "" } }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

            // ---- templates
            Text { text: qsTr("Templates"); color: Theme.text; font.pixelSize: 12; font.bold: true }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: panel.templates
                    delegate: Rectangle {
                        id: tcard
                        required property var modelData
                        objectName: "template_" + modelData.id
                        width: 120
                        height: 92
                        color: panel.chosenTemplate === modelData.id ? Theme.selection : Theme.field
                        border.color: panel.chosenTemplate === modelData.id ? Theme.accent : Theme.border
                        radius: 3
                        Image {
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 3 }
                            height: 62
                            source: tcard.modelData.preview
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Text {
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 4 }
                            text: tcard.modelData.name
                            color: Theme.text
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: panel.chosenTemplate = tcard.modelData.id
                            onDoubleClicked: panel.chosen = session.newGraphicFromTemplate(tcard.modelData.id, tcard.modelData.version, "")
                            ToolTip.visible: containsMouse && tcard.modelData.description.length > 0
                            ToolTip.text: tcard.modelData.description
                            ToolTip.delay: 600
                        }
                    }
                }
            }
            RowLayout {
                visible: panel.chosenTemplate !== ""
                Chip {
                    objectName: "newFromTemplate"
                    text: qsTr("New title from template")
                    onClicked: {
                        for (let i = 0; i < panel.templates.length; ++i) {
                            if (panel.templates[i].id === panel.chosenTemplate) panel.chosen = session.newGraphicFromTemplate(panel.chosenTemplate, panel.templates[i].version, "")
                        }
                    }
                }
            }
        }
    }

    Component {
        id: textField
        TextField {
            property var info
            text: info ? info.value : ""
            color: Theme.text
            font.pixelSize: 11
            selectByMouse: true
            background: Rectangle { color: Theme.bg; border.color: parent.activeFocus ? Theme.accent : Theme.border; radius: 2 }
            onEditingFinished: if (info && text !== info.value) session.setGraphicControl(panel.clip.graphic, info.name, text)
        }
    }
    Component {
        id: colourField
        RowLayout {
            property var info
            spacing: 4
            Rectangle { width: 22; height: 22; radius: 3; color: info ? Qt.color(info.value.length === 9 ? "#" + info.value.substr(7, 2) + info.value.substr(1, 6) : info.value) : "black"; border.color: Theme.border }
            TextField {
                Layout.fillWidth: true
                text: info ? info.value : ""
                color: Theme.text
                font.pixelSize: 11
                selectByMouse: true
                background: Rectangle { color: Theme.bg; border.color: parent.activeFocus ? Theme.accent : Theme.border; radius: 2 }
                onEditingFinished: if (info && text !== info.value) session.setGraphicControl(panel.clip.graphic, info.name, text)
            }
        }
    }
    Component {
        id: numberField
        NumberBox {
            property var info
            value: info ? parseFloat(info.value) : 0
            minimum: info ? info.minimum : -1e9
            maximum: info ? info.maximum : 1e9
            onCommitted: (v) => session.setGraphicControl(panel.clip.graphic, info.name, v.toString())
        }
    }
}

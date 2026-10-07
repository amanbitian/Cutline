import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Where a title or graphic is drawn: text, shapes and pictures on a canvas, moved, resized and turned with the pointer, with
// every property in a list beside it. What is done here is kept in the panel (with its own undo) until Save, which puts it in
// the project as one step; every clip that shows the graphic changes with it.
Item {
    id: panel
    property string panelId: "graphic_designer"
    readonly property int revision: session.designerRevision
    readonly property bool isOpen: session.designerOpen
    readonly property var doc: revision >= 0 && isOpen ? session.designer() : ({})
    readonly property var elements: revision >= 0 && isOpen ? session.designerElements() : []
    readonly property var properties: revision >= 0 && isOpen ? session.designerProperties() : []
    readonly property var selection: revision >= 0 && isOpen ? session.designerSelection() : ({})
    readonly property var guides: revision >= 0 && isOpen ? session.designerGuides() : []
    readonly property var entrance: revision >= 0 && isOpen ? session.designerEntrance() : ({})
    readonly property bool hasSelection: selection.id !== undefined

    Text {
        anchors.centerIn: parent
        visible: !panel.isOpen
        horizontalAlignment: Text.AlignHCenter
        color: Theme.muted
        font.pixelSize: 12
        text: qsTr("No graphic is open.\nDouble-click one in the Titles panel, or make a new one.")
    }
    Chip {
        visible: !panel.isOpen
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.verticalCenter; topMargin: 30 }
        objectName: "designNew"
        text: qsTr("New graphic")
        enabled: session.projectOpen
        onClicked: session.newGraphic("")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6
        visible: panel.isOpen

        // ---- toolbar
        Flow {
            Layout.fillWidth: true
            spacing: 4
            TextField {
                id: nameField
                objectName: "designName"
                width: 150
                height: 24
                text: panel.doc.name !== undefined ? panel.doc.name : ""
                color: Theme.text
                font.pixelSize: 11
                selectByMouse: true
                background: Rectangle { color: Theme.bg; border.color: nameField.activeFocus ? Theme.accent : Theme.border; radius: 2 }
                onEditingFinished: session.designerSetName(text)
            }
            Chip { objectName: "designText"; text: qsTr("+ Text"); onClicked: session.designerAdd("text") }
            Chip { objectName: "designRectangle"; text: qsTr("+ Box"); onClicked: session.designerAdd("rectangle") }
            Chip { objectName: "designEllipse"; text: qsTr("+ Oval"); onClicked: session.designerAdd("ellipse") }
            Chip { objectName: "designImage"; text: qsTr("+ Picture"); onClicked: session.designerAdd("image") }
            Chip { objectName: "designDuplicate"; text: qsTr("Duplicate"); enabled: panel.hasSelection; onClicked: session.designerDuplicate() }
            Chip { objectName: "designDelete"; text: qsTr("Delete"); tone: Theme.danger; enabled: panel.hasSelection; onClicked: session.designerRemove() }
            Chip { objectName: "designUndo"; text: qsTr("Undo"); enabled: panel.doc.canUndo === true; onClicked: session.designerUndo() }
            Chip { objectName: "designRedo"; text: qsTr("Redo"); enabled: panel.doc.canRedo === true; onClicked: session.designerRedo() }
            Chip { objectName: "designFront"; text: qsTr("To front"); enabled: panel.hasSelection; onClicked: session.designerRestack("front") }
            Chip { objectName: "designBack"; text: qsTr("To back"); enabled: panel.hasSelection; onClicked: session.designerRestack("back") }
            Repeater {
                model: [["left", "⇤"], ["centre", "↔"], ["right", "⇥"], ["top", "⤒"], ["middle", "↕"], ["bottom", "⤓"]]
                delegate: Chip {
                    required property var modelData
                    objectName: "align_" + modelData[0]
                    text: modelData[1]
                    enabled: panel.hasSelection
                    ToolTip.text: qsTr("Align %1").arg(modelData[0])
                    onClicked: session.designerAlign(modelData[0])
                }
            }
            Chip { objectName: "designSave"; text: panel.doc.dirty ? qsTr("Save ●") : qsTr("Save"); active: panel.doc.dirty === true; onClicked: session.designerSave() }
            Chip { objectName: "designTemplate"; text: qsTr("Save as template"); onClicked: session.designerSaveAsTemplate(nameField.text, "") }
            Chip { objectName: "designClose"; text: qsTr("Close"); onClicked: session.designerClose() }
        }
        Text {
            Layout.fillWidth: true
            visible: (panel.doc.problems !== undefined && panel.doc.problems.length > 0) || (panel.doc.missingFonts !== undefined && panel.doc.missingFonts.length > 0)
            color: Theme.warning
            font.pixelSize: 10
            wrapMode: Text.WordWrap
            text: {
                let lines = []
                if (panel.doc.problems) lines = lines.concat(panel.doc.problems)
                if (panel.doc.missingFonts && panel.doc.missingFonts.length > 0)
                    lines.push(qsTr("Not installed here (another font is used): %1").arg(panel.doc.missingFonts.join(", ")))
                return lines.join("  ·  ")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            // ---- the canvas
            Rectangle {
                id: well
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 160
                color: Theme.bg
                border.color: Theme.border
                clip: true

                Image {
                    id: picture
                    anchors.fill: parent
                    anchors.margins: 6
                    source: panel.doc.preview !== undefined ? panel.doc.preview : ""
                    fillMode: Image.PreserveAspectFit
                    cache: false
                    smooth: true
                }
                // Everything that is drawn over the picture sits in an item the size of the painted picture.
                Item {
                    id: canvas
                    objectName: "designCanvas"
                    x: picture.x + (picture.width - picture.paintedWidth) / 2
                    y: picture.y + (picture.height - picture.paintedHeight) / 2
                    width: picture.paintedWidth
                    height: picture.paintedHeight

                    Repeater {
                        model: panel.guides
                        delegate: Rectangle {
                            required property var modelData
                            color: "#ff3fb0"
                            x: modelData.vertical ? modelData.at * canvas.width : 0
                            y: modelData.vertical ? 0 : modelData.at * canvas.height
                            width: modelData.vertical ? 1 : canvas.width
                            height: modelData.vertical ? canvas.height : 1
                        }
                    }
                    Rectangle {
                        visible: panel.hasSelection
                        x: (panel.selection.x || 0) * canvas.width
                        y: (panel.selection.y || 0) * canvas.height
                        width: (panel.selection.width || 0) * canvas.width
                        height: (panel.selection.height || 0) * canvas.height
                        rotation: panel.selection.rotation || 0
                        color: "transparent"
                        border.color: Theme.accent
                        border.width: 1
                    }
                    Repeater {
                        model: panel.hasSelection ? panel.selection.grips : []
                        delegate: Rectangle {
                            required property var modelData
                            width: 9
                            height: 9
                            radius: modelData.name === "turn" ? 5 : 1
                            x: modelData.x * canvas.width - 4.5
                            y: modelData.y * canvas.height - 4.5
                            color: modelData.name === "turn" ? Theme.accent : Theme.bg
                            border.color: Theme.accent
                        }
                    }
                    MouseArea {
                        id: pointer
                        anchors.fill: parent
                        property bool down: false
                        onPressed: (mouse) => {
                            down = true
                            session.designerPress(mouse.x / width, mouse.y / height, true)
                        }
                        onPositionChanged: (mouse) => {
                            if (down) session.designerDrag(mouse.x / width, mouse.y / height, (mouse.modifiers & Qt.ShiftModifier) !== 0, (mouse.modifiers & Qt.AltModifier) === 0)
                        }
                        onReleased: { down = false; session.designerRelease() }
                    }
                }
            }

            // ---- layers and properties
            ColumnLayout {
                Layout.preferredWidth: 250
                Layout.minimumWidth: 220
                Layout.maximumWidth: 280
                Layout.fillHeight: true
                spacing: 6

                Text { text: qsTr("Layers (top first)"); color: Theme.muted; font.pixelSize: 11 }
                ListView {
                    id: layers
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(150, Math.max(40, contentHeight))
                    clip: true
                    model: panel.elements
                    delegate: Rectangle {
                        required property var modelData
                        width: layers.width
                        height: 22
                        color: modelData.selected ? Theme.selection : "transparent"
                        Text { anchors { fill: parent; leftMargin: 6 } text: modelData.label; color: Theme.text; font.pixelSize: 11; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
                        MouseArea { anchors.fill: parent; onClicked: session.designerSelect(modelData.id) }
                    }
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: props.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    ColumnLayout {
                        id: props
                        width: parent.width - 10
                        spacing: 3
                        Repeater {
                            model: panel.properties
                            delegate: ColumnLayout {
                                id: row
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    visible: row.index === 0 || panel.properties[row.index - 1].group !== row.modelData.group
                                    text: row.modelData.group
                                    color: Theme.accent
                                    font.pixelSize: 10
                                    font.bold: true
                                    topPadding: 4
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: row.modelData.label; color: Theme.muted; font.pixelSize: 11; Layout.preferredWidth: 84; elide: Text.ElideRight }
                                    Loader {
                                        Layout.fillWidth: true
                                        sourceComponent: row.modelData.kind === "number" ? numberField
                                                       : row.modelData.kind === "toggle" ? toggleField
                                                       : row.modelData.kind === "choice" ? choiceField
                                                       : row.modelData.kind === "colour" ? colourField
                                                       : row.modelData.kind === "asset" ? pictureField : textField
                                        onLoaded: item.info = row.modelData
                                    }
                                }
                            }
                        }

                        // ---- entrance
                        Text { visible: panel.hasSelection; text: qsTr("Entrance"); color: Theme.accent; font.pixelSize: 10; font.bold: true; topPadding: 6 }
                        RowLayout {
                            visible: panel.hasSelection
                            Layout.fillWidth: true
                            ComboBox {
                                id: entranceBox
                                objectName: "designEntrance"
                                Layout.fillWidth: true
                                model: panel.entrance.kinds !== undefined ? panel.entrance.kinds : []
                                currentIndex: panel.entrance.kinds !== undefined ? panel.entrance.kinds.indexOf(panel.entrance.kind) : -1
                                onActivated: session.designerSetEntrance(currentText, entranceSeconds.value)
                            }
                            NumberBox {
                                id: entranceSeconds
                                objectName: "designEntranceSeconds"
                                width: 50
                                value: panel.entrance.seconds !== undefined ? panel.entrance.seconds : 0.5
                                minimum: 0.05
                                maximum: 60
                                onCommitted: (v) => { value = v; if (entranceBox.currentText !== "none") session.designerSetEntrance(entranceBox.currentText, v) }
                            }
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
            onEditingFinished: if (info && text !== info.value) session.designerSetProperty(info.name, text)
        }
    }
    Component {
        id: numberField
        NumberBox {
            property var info
            value: info ? parseFloat(info.value) : 0
            minimum: info ? info.minimum : -1e9
            maximum: info ? info.maximum : 1e9
            onCommitted: (v) => session.designerSetProperty(info.name, v.toString())
        }
    }
    Component {
        id: toggleField
        Check {
            property var info
            checked: info ? info.value === "true" : false
            onToggled: session.designerSetProperty(info.name, checked ? "true" : "false")
        }
    }
    Component {
        id: choiceField
        ComboBox {
            property var info
            model: info ? info.choices : []
            currentIndex: info ? info.choices.indexOf(info.value) : -1
            onActivated: session.designerSetProperty(info.name, currentText)
        }
    }
    Component {
        id: colourField
        RowLayout {
            property var info
            spacing: 4
            Rectangle { width: 22; height: 22; radius: 3; color: info ? Qt.color("#" + info.value.substr(7, 2) + info.value.substr(1, 6)) : "black"; border.color: Theme.border }
            TextField {
                Layout.fillWidth: true
                text: info ? info.value : ""
                color: Theme.text
                font.pixelSize: 11
                selectByMouse: true
                background: Rectangle { color: Theme.bg; border.color: parent.activeFocus ? Theme.accent : Theme.border; radius: 2 }
                onEditingFinished: if (info && text.toUpperCase() !== info.value) session.designerSetProperty(info.name, text)
            }
        }
    }
    Component {
        id: pictureField
        ComboBox {
            property var info
            property var pictures: session.designerPictures()
            model: [qsTr("(choose a picture)")].concat(pictures.map(p => p.label))
            currentIndex: {
                if (!info) return 0
                for (let i = 0; i < pictures.length; ++i) if (pictures[i].value === info.value) return i + 1
                return 0
            }
            onActivated: (i) => { if (i > 0) session.designerSetProperty(info.name, pictures[i - 1].value) }
        }
    }
}

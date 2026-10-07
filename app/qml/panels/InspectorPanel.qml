import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The selected clip's effects: each with its parameters, a stopwatch to animate them, keyframe navigation, and a way to
// add, reorder, switch off or remove an effect. Values are committed when a control is released or confirmed, so one
// gesture is one undo step.
Item {
    property string panelId: "effect_controls"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.panelHeader
            RowLayout {
                anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad }
                Text {
                    Layout.fillWidth: true
                    text: session.selectionCount === 0 ? qsTr("No clip selected")
                          : session.selectionCount > 1 ? qsTr("%1 clips selected - showing %2").arg(session.selectionCount).arg(session.inspectorClip)
                          : session.inspectorClip
                    color: Theme.text
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Chip { text: qsTr("Analyse"); enabled: session.selectionCount > 0; onClicked: analyseMenu.open() }
                Chip { text: qsTr("+ Effect"); enabled: session.selectionCount > 0; onClicked: addMenu.open() }
            }
        }

        ListView {
            id: effectsList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: session.inspector
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: block
                required property var modelData
                required property int index
                width: effectsList.width
                height: head.height + paramColumn.height + maskColumn.height + 16
                color: Theme.field
                border.color: Theme.border

                Rectangle {
                    id: head
                    width: parent.width
                    height: 28
                    color: Theme.panelHeader
                    RowLayout {
                        anchors { fill: parent; leftMargin: 6; rightMargin: 6 }
                        spacing: 4
                        CheckBox {
                            checked: block.modelData.enabled
                            enabled: !block.modelData.intrinsic
                            onToggled: session.setEffectEnabled(block.modelData.id, checked)
                            implicitWidth: 22
                            implicitHeight: 22
                            ToolTip.text: qsTr("Effect on / off")
                            ToolTip.visible: hovered
                        }
                        Text { Layout.fillWidth: true; text: block.modelData.name; color: block.modelData.enabled ? Theme.text : Theme.faint; font.pixelSize: 12; font.bold: true; elide: Text.ElideRight }
                        Chip { text: "▲"; visible: !block.modelData.intrinsic; onClicked: session.moveEffect(block.modelData.id, -1) }
                        Chip { text: "▼"; visible: !block.modelData.intrinsic; onClicked: session.moveEffect(block.modelData.id, 1) }
                        Chip { text: "×"; visible: !block.modelData.intrinsic; tone: Theme.danger; onClicked: session.removeEffect(block.modelData.id) }
                    }
                }

                Column {
                    id: paramColumn
                    anchors { top: head.bottom; left: parent.left; right: parent.right; topMargin: 6 }
                    spacing: 4
                    Text {
                        visible: block.modelData.preset !== ""
                        text: block.modelData.preset
                        color: Theme.muted
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                        width: parent.width - 16
                        x: 8
                    }
                    Repeater {
                        model: block.modelData.parameters
                        delegate: ParameterRow {
                            required property var modelData
                            width: paramColumn.width
                            effectId: block.modelData.id
                            param: modelData
                        }
                    }
                }

                // The masks that limit where this effect applies, and the tools to draw more.
                Column {
                    id: maskColumn
                    objectName: "maskColumn"
                    anchors { top: paramColumn.bottom; left: parent.left; right: parent.right; topMargin: 6; leftMargin: 8; rightMargin: 8 }
                    spacing: 4
                    visible: !block.modelData.intrinsic
                    RowLayout {
                        width: parent.width
                        spacing: 4
                        Text { text: qsTr("Masks"); color: Theme.muted; font.pixelSize: 11; Layout.fillWidth: true }
                        Chip { objectName: "maskRectangle"; text: "▭"; ToolTip.text: qsTr("Add a rectangle mask"); onClicked: session.maskAdd(block.modelData.id, "rectangle") }
                        Chip { objectName: "maskEllipse"; text: "◯"; ToolTip.text: qsTr("Add an ellipse mask"); onClicked: session.maskAdd(block.modelData.id, "ellipse") }
                        Chip { objectName: "maskPath"; text: "✎"; ToolTip.text: qsTr("Draw a path mask on the monitor: click points, click the first to close"); onClicked: { session.maskSetTarget(block.modelData.id); session.maskSetTool("pen") } }
                    }
                    Repeater {
                        model: block.modelData.masks
                        delegate: Rectangle {
                            id: maskRow
                            required property var modelData
                            width: maskColumn.width
                            height: maskContent.height + 8
                            color: modelData.active ? Theme.selection : "transparent"
                            border.color: Theme.border
                            Column {
                                id: maskContent
                                x: 6; y: 4
                                width: parent.width - 12
                                spacing: 3
                                RowLayout {
                                    width: parent.width
                                    spacing: 4
                                    Text { text: maskRow.modelData.shape + (maskRow.modelData.points > 0 ? " (" + maskRow.modelData.points + ")" : ""); color: Theme.text; font.pixelSize: 11; Layout.fillWidth: true }
                                    ComboBox {
                                        implicitWidth: 84; implicitHeight: 22; font.pixelSize: 10
                                        model: ["add", "subtract", "intersect"]
                                        currentIndex: Math.max(0, model.indexOf(maskRow.modelData.combine))
                                        onActivated: (i) => session.maskSetMode(maskRow.modelData.id, model[i], maskRow.modelData.inverted)
                                    }
                                    Check { text: qsTr("Invert"); checked: maskRow.modelData.inverted; onToggled: session.maskSetMode(maskRow.modelData.id, maskRow.modelData.combine, checked) }
                                    Chip { text: maskRow.modelData.active ? qsTr("Editing") : qsTr("Edit"); active: maskRow.modelData.active; onClicked: session.maskSelect(maskRow.modelData.active ? "" : maskRow.modelData.id) }
                                    Chip { text: "×"; tone: Theme.danger; onClicked: session.maskRemove(maskRow.modelData.id) }
                                }
                                Repeater {
                                    model: [{ key: "feather", label: qsTr("Feather"), max: 500 }, { key: "expansion", label: qsTr("Expansion"), max: 500 }, { key: "opacity", label: qsTr("Opacity"), max: 1 }]
                                    delegate: RowLayout {
                                        required property var modelData
                                        width: maskContent.width
                                        spacing: 4
                                        Text { text: modelData.label; color: Theme.muted; font.pixelSize: 10; Layout.preferredWidth: 60 }
                                        NumberBox {
                                            Layout.preferredWidth: 70
                                            minimum: modelData.key === "expansion" ? -500 : 0
                                            maximum: modelData.max
                                            value: maskRow.modelData[modelData.key]
                                            onCommitted: (v) => session.maskSetNumber(maskRow.modelData.id, modelData.key, v)
                                        }
                                        Chip {
                                            height: 20
                                            text: maskRow.modelData.keyHere.indexOf(modelData.key) >= 0 ? "◆" : (maskRow.modelData.animated.indexOf(modelData.key) >= 0 ? "◇" : "○")
                                            active: maskRow.modelData.animated.indexOf(modelData.key) >= 0
                                            ToolTip.text: qsTr("Key this value at the playhead; again to remove the key")
                                            onClicked: session.maskToggleKey(maskRow.modelData.id, modelData.key)
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                }
                                RowLayout {
                                    width: parent.width
                                    spacing: 4
                                    Chip { text: qsTr("Track forward"); ToolTip.text: qsTr("Follow the middle of the shape from the playhead to the end of the clip"); onClicked: session.maskTrack(maskRow.modelData.id) }
                                    Chip {
                                        text: qsTr("Stop animating shape")
                                        visible: maskRow.modelData.animated.length > 0
                                        onClicked: { for (const p of maskRow.modelData.animated) session.maskStopAnimating(maskRow.modelData.id, p) }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: effectsList.count === 0
                text: session.selectionCount === 0 ? qsTr("Select a clip to see its controls") : qsTr("This clip has no effects yet")
                color: Theme.faint
                font.pixelSize: 12
            }
        }
    }

    Menu {
        id: analyseMenu
        width: 280
        MenuItem { text: qsTr("Stabilise (similarity: shift, rotation, zoom)"); onTriggered: session.analyseClip("stabilize") }
        MenuItem { text: qsTr("Motion for slow motion (optical flow)"); onTriggered: session.analyseClip("optical_flow") }
    }

    Menu {
        id: addMenu
        width: 260
        Repeater {
            model: session.effectCatalogue("")
            delegate: MenuItem {
                required property var modelData
                text: modelData.category + "  ›  " + modelData.name
                onTriggered: {
                    if (modelData.assetKind === "colorspace") spaceMenu.popup()
                    else if (modelData.needsAsset) lutBrowser.open()
                    else session.addEffect(modelData.id, "")
                }
            }
        }
    }

    LutBrowser { id: lutBrowser }
    ColorSpaceMenu { id: spaceMenu }
}

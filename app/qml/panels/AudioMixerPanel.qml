import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Realtime channel strips backed by the engine's existing bus/send mixer. Meter
// readings come from the block being played; the interface never renders audio a
// second time merely to animate a bar.
Item {
    id: panel
    property string panelId: "audio_mixer"

    function busIndex(id) {
        for (let i = 0; i < session.audioBuses.length; ++i)
            if (session.audioBuses[i].id === id) return i
        return 0
    }
    function levelHeight(db, available) {
        if (db <= -60) return 0
        return Math.max(0, Math.min(available, available * (db + 60) / 60))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.pad
            spacing: 6
            Text { text: qsTr("Audio Track Mixer"); color: Theme.text; font.bold: true }
            Text { text: qsTr("post-fader peak / RMS"); color: Theme.faint; font.pixelSize: 10; Layout.fillWidth: true }
            TextField {
                id: busName
                implicitWidth: 110
                placeholderText: qsTr("Bus name")
                color: Theme.text
                selectByMouse: true
                onAccepted: addBus.clicked()
            }
            Chip {
                id: addBus
                objectName: "audioAddBus"
                text: qsTr("Add bus")
                onClicked: {
                    if (session.audioAddBus(busName.text) !== "") busName.clear()
                }
            }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: strips.implicitWidth + Theme.pad * 2
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {}

            Row {
                id: strips
                x: Theme.pad
                height: flick.height - 12
                spacing: 6

                Repeater {
                    model: session.audioMixer
                    delegate: Rectangle {
                        id: strip
                        required property var modelData
                        width: 142
                        height: strips.height
                        radius: 3
                        color: modelData.master ? "#202522" : Theme.panelHeader
                        border.color: modelData.solo ? Theme.warning : modelData.muted ? Theme.danger : Theme.border

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 4

                            TextField {
                                Layout.fillWidth: true
                                text: strip.modelData.name
                                readOnly: strip.modelData.master || strip.modelData.locked
                                color: Theme.text
                                horizontalAlignment: Text.AlignHCenter
                                selectByMouse: true
                                onEditingFinished: if (!readOnly && text !== strip.modelData.name) session.audioRename(strip.modelData.id, text)
                            }

                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                Chip {
                                    text: "M"; active: strip.modelData.muted; tone: Theme.danger
                                    enabled: !strip.modelData.master && !strip.modelData.locked
                                    onClicked: session.setTrackFlag(strip.modelData.id, "muted", !strip.modelData.muted)
                                }
                                Chip {
                                    text: "S"; active: strip.modelData.solo; tone: Theme.warning
                                    enabled: !strip.modelData.master && !strip.modelData.locked
                                    onClicked: session.setTrackFlag(strip.modelData.id, "solo", !strip.modelData.solo)
                                }
                                Text { text: strip.modelData.isBus && !strip.modelData.master ? qsTr("BUS") : strip.modelData.channelLayout.toUpperCase(); color: Theme.faint; font.pixelSize: 9 }
                            }

                            // Automation: read follows what is written; write, touch and latch record the fader and pan while the sequence plays.
                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                visible: !strip.modelData.master
                                spacing: 2
                                Repeater {
                                    model: [{ id: "read", letter: "R", tip: qsTr("Read: follow the automation") }, { id: "write", letter: "W", tip: qsTr("Write: record every move from the first until the sequence stops") },
                                            { id: "touch", letter: "T", tip: qsTr("Touch: record while the control is held; on release it returns to what was there") },
                                            { id: "latch", letter: "L", tip: qsTr("Latch: record from the first touch until the sequence stops") }]
                                    delegate: Chip {
                                        required property var modelData
                                        objectName: "auto_" + modelData.id
                                        height: 20
                                        text: modelData.letter
                                        active: strip.modelData.autoMode === modelData.id
                                        tone: modelData.id === "read" ? Theme.accent : Theme.danger
                                        ToolTip.text: modelData.tip
                                        onClicked: session.audioSetAutomationMode(strip.modelData.id, modelData.id)
                                    }
                                }
                                Chip {
                                    objectName: "record_" + strip.modelData.id
                                    height: 20
                                    visible: !strip.modelData.isBus
                                    text: "●"
                                    tone: Theme.danger
                                    active: session.audioRecording
                                    ToolTip.text: session.audioRecording ? qsTr("Stop the take and put it on this track") : qsTr("Record a voice-over onto this track from the playhead, with the sequence playing")
                                    onClicked: session.audioRecording ? session.audioRecordStop() : session.audioRecordStart(strip.modelData.id, "", true)
                                }
                                Text { visible: strip.modelData.autoVolume || strip.modelData.autoPan; text: "●"; color: Theme.accent; font.pixelSize: 9; ToolTip.visible: false }
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 120

                                Rectangle {
                                    id: meterWell
                                    x: 10; width: 18; anchors { top: parent.top; bottom: parent.bottom }
                                    radius: 2; color: "#090a0a"; border.color: Theme.border
                                    Rectangle {
                                        width: parent.width - 4; x: 2; anchors.bottom: parent.bottom
                                        height: panel.levelHeight(strip.modelData.rmsDb, parent.height - 4)
                                        color: Theme.accent; opacity: 0.48
                                    }
                                    Rectangle {
                                        width: parent.width - 4; x: 2; anchors.bottom: parent.bottom
                                        height: panel.levelHeight(strip.modelData.peakDb, parent.height - 4)
                                        color: strip.modelData.peakDb > -1 ? Theme.danger : Theme.accent
                                        opacity: 0.86
                                    }
                                }

                                Slider {
                                    id: fader
                                    anchors { left: meterWell.right; leftMargin: 8; right: parent.right; top: parent.top; bottom: parent.bottom }
                                    orientation: Qt.Vertical
                                    from: -96; to: 24
                                    // What automation says at the playhead, where there is automation.
                                    value: strip.modelData.autoVolume ? strip.modelData.liveGainDb : strip.modelData.gainDb
                                    enabled: !strip.modelData.master && !strip.modelData.locked
                                    property bool automated: false
                                    onPressedChanged: {
                                        if (!enabled) return
                                        if (pressed) {
                                            automated = session.audioAutomate(strip.modelData.id, "volume", value, "begin")
                                        } else if (automated) {
                                            session.audioAutomate(strip.modelData.id, "volume", value, "end")
                                            automated = false
                                        } else {
                                            session.audioSetLevel(strip.modelData.id, value, pan.value)
                                        }
                                    }
                                    onMoved: if (automated) session.audioAutomate(strip.modelData.id, "volume", value, "move")
                                }
                            }

                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: strip.modelData.peakDb <= -100 ? "-∞ dB" : Number(strip.modelData.peakDb).toFixed(1) + " dB"
                                color: strip.modelData.peakDb > -1 ? Theme.danger : Theme.muted
                                font.family: Theme.mono; font.pixelSize: 10
                            }

                            Slider {
                                id: pan
                                Layout.fillWidth: true
                                from: -1; to: 1; stepSize: 0.01
                                value: strip.modelData.autoPan ? strip.modelData.livePan : strip.modelData.pan
                                enabled: !strip.modelData.master && !strip.modelData.locked
                                property bool automated: false
                                onPressedChanged: {
                                    if (!enabled) return
                                    if (pressed) {
                                        automated = session.audioAutomate(strip.modelData.id, "pan", value, "begin")
                                    } else if (automated) {
                                        session.audioAutomate(strip.modelData.id, "pan", value, "end")
                                        automated = false
                                    } else {
                                        session.audioSetLevel(strip.modelData.id, fader.value, value)
                                    }
                                }
                                onMoved: if (automated) session.audioAutomate(strip.modelData.id, "pan", value, "move")
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: Math.abs(strip.modelData.pan) < 0.01 ? qsTr("C") : (strip.modelData.pan < 0 ? qsTr("L %1").arg(Math.round(-strip.modelData.pan * 100)) : qsTr("R %1").arg(Math.round(strip.modelData.pan * 100)))
                                color: Theme.muted; font.pixelSize: 10
                            }

                            ComboBox {
                                id: output
                                Layout.fillWidth: true
                                visible: !strip.modelData.master
                                enabled: !strip.modelData.locked
                                model: session.audioBuses
                                textRole: "name"
                                valueRole: "id"
                                Component.onCompleted: currentIndex = panel.busIndex(strip.modelData.outputBusId)
                                onActivated: session.audioSetOutput(strip.modelData.id, currentValue)
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Output routing")
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: !strip.modelData.master
                                spacing: 2
                                Repeater {
                                    model: strip.modelData.sends
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Text { Layout.fillWidth: true; text: modelData.busName; color: Theme.muted; font.pixelSize: 9; elide: Text.ElideRight }
                                        NumberBox {
                                            implicitWidth: 38
                                            value: modelData.gainDb; minimum: -96; maximum: 24
                                            onCommitted: (v) => session.audioSetSend(strip.modelData.id, modelData.busId, v, modelData.preFader, true)
                                        }
                                        Chip {
                                            height: 20; text: modelData.preFader ? qsTr("Pre") : qsTr("Post"); active: modelData.preFader
                                            onClicked: session.audioSetSend(strip.modelData.id, modelData.busId, modelData.gainDb, !modelData.preFader, true)
                                        }
                                        Chip { height: 20; text: "×"; tone: Theme.danger; onClicked: session.audioSetSend(strip.modelData.id, modelData.busId, 0, false, false) }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: session.audioBuses.length > 1
                                    spacing: 2
                                    ComboBox {
                                        id: sendBus
                                        Layout.fillWidth: true
                                        model: session.audioBuses
                                        textRole: "name"; valueRole: "id"
                                        currentIndex: session.audioBuses.length > 1 ? 1 : 0
                                    }
                                    Chip {
                                        height: 22; text: "+"
                                        enabled: sendBus.currentValue !== "" && sendBus.currentValue !== strip.modelData.id
                                        onClicked: session.audioSetSend(strip.modelData.id, sendBus.currentValue, 0, false, true)
                                        ToolTip.text: qsTr("Add post-fader send")
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                visible: strip.modelData.isBus && !strip.modelData.master
                                Text { Layout.fillWidth: true; text: qsTr("%1 send(s)").arg(strip.modelData.sends.length); color: Theme.faint; font.pixelSize: 9 }
                                Chip { text: qsTr("Remove"); tone: Theme.danger; onClicked: session.audioRemoveBus(strip.modelData.id) }
                            }
                        }
                    }
                }
            }
        }
    }
}

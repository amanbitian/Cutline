import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// What the chosen sound is for, and how loud it should be: pick a role to lay down its starting effects, match the sound
// or the whole programme to a delivery target, and see the measurement. Everything it does is an ordinary undoable edit
// made of ordinary effects, which stay in the effect stack to be changed or removed.
Item {
    id: panel
    property string panelId: "audio_essentials"
    readonly property var chosen: session.audioSelection
    readonly property bool hasSound: chosen.clip !== undefined
    readonly property var loudness: session.audioLoudness
    property bool applyChain: true
    property real repair: 0.5
    property real reverb: 0.0
    property bool tone: true
    property bool dynamics: true
    property string duckTrack: ""
    property string targetId: "r128"

    function apply(role) {
        session.audioApplyRole(role, applyChain, repair, reverb, tone, dynamics, role === "music" ? duckTrack : "")
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

            Text {
                Layout.fillWidth: true
                text: panel.hasSound ? panel.chosen.name + (panel.chosen.count > 1 ? qsTr("  (+%1 more)").arg(panel.chosen.count - 1) : "") : qsTr("Choose the sound of a clip (or its picture) to set what it is for")
                color: panel.hasSound ? Theme.text : Theme.muted
                font.pixelSize: 12
                font.bold: panel.hasSound
                wrapMode: Text.WordWrap
            }

            // ---- role
            Text { text: qsTr("What is this sound?"); color: Theme.muted; font.pixelSize: 11 }
            Flow {
                Layout.fillWidth: true
                spacing: 4
                Repeater {
                    model: session.audioRoles()
                    delegate: Chip {
                        required property var modelData
                        objectName: "role_" + modelData.id
                        text: modelData.label
                        active: panel.hasSound && panel.chosen.role === modelData.id
                        enabled: panel.hasSound
                        onClicked: panel.apply(modelData.id)
                    }
                }
                Chip {
                    objectName: "role_none"
                    text: qsTr("Clear")
                    tone: Theme.danger
                    enabled: panel.hasSound && (panel.chosen.role !== "" || panel.chosen.hasChain)
                    onClicked: session.audioApplyRole("", true, 0, 0, true, true, "")
                }
            }
            Check { text: qsTr("Lay down the starting effects for the role"); checked: panel.applyChain; onToggled: panel.applyChain = checked }

            // ---- the starting chain's options
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                visible: panel.applyChain
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: qsTr("Reduce noise"); color: Theme.muted; font.pixelSize: 11; Layout.preferredWidth: 90 }
                    Slider { Layout.fillWidth: true; from: 0; to: 1; value: panel.repair; onMoved: panel.repair = value }
                    Text { text: Math.round(panel.repair * 100) + "%"; color: Theme.muted; font.pixelSize: 10; Layout.preferredWidth: 34 }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: qsTr("Reduce reverb"); color: Theme.muted; font.pixelSize: 11; Layout.preferredWidth: 90 }
                    Slider { Layout.fillWidth: true; from: 0; to: 1; value: panel.reverb; onMoved: panel.reverb = value }
                    Text { text: Math.round(panel.reverb * 100) + "%"; color: Theme.muted; font.pixelSize: 10; Layout.preferredWidth: 34 }
                }
                RowLayout {
                    Check { text: qsTr("Tone"); checked: panel.tone; onToggled: panel.tone = checked }
                    Check { text: qsTr("Dynamics"); checked: panel.dynamics; onToggled: panel.dynamics = checked }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: qsTr("Music ducks under"); color: Theme.muted; font.pixelSize: 11 }
                    ComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("(nothing)")].concat(session.trackIds("audio"))
                        onActivated: (i) => panel.duckTrack = i === 0 ? "" : session.trackIds("audio")[i - 1]
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

            // ---- loudness
            Text { text: qsTr("Loudness"); color: Theme.text; font.pixelSize: 12; font.bold: true }
            ComboBox {
                id: targetBox
                objectName: "loudnessTarget"
                Layout.fillWidth: true
                model: session.audioLoudnessTargets()
                textRole: "name"
                valueRole: "id"
                onActivated: panel.targetId = currentValue
                Component.onCompleted: panel.targetId = currentValue
            }
            Text {
                Layout.fillWidth: true
                text: targetBox.currentIndex >= 0 ? session.audioLoudnessTargets()[targetBox.currentIndex].note : ""
                color: Theme.faint
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: 4
                Chip {
                    objectName: "measureClip"
                    text: qsTr("Match this sound")
                    enabled: panel.hasSound && panel.chosen.measurable
                    ToolTip.text: panel.hasSound && !panel.chosen.measurable ? qsTr("A clip at another speed, reversed or with a speed ramp is matched through the programme") : qsTr("Measure the sound as its file holds it and set the clip's volume to reach the target")
                    onClicked: session.audioMeasure("clip", panel.targetId)
                }
                Chip {
                    objectName: "measureProgramme"
                    text: qsTr("Match the programme")
                    ToolTip.text: qsTr("Play the whole mix (or the marked range) through the meter and set the master's volume to reach the target")
                    onClicked: session.audioMeasure("programme", panel.targetId)
                }
                Chip { objectName: "measureOnly"; text: qsTr("Measure only"); onClicked: session.audioMeasure("programme", "") }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                visible: panel.loudness.state !== undefined
                Text {
                    visible: panel.loudness.state === "measuring"
                    text: qsTr("Measuring… (progress is in Background Jobs)")
                    color: Theme.warning
                    font.pixelSize: 11
                }
                GridLayout {
                    visible: panel.loudness.state === "done"
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 1
                    Text { text: qsTr("Integrated"); color: Theme.muted; font.pixelSize: 11 }
                    Text { objectName: "integratedLufs"; text: panel.loudness.integratedLufs <= -199 ? qsTr("silence") : Number(panel.loudness.integratedLufs).toFixed(1) + " LUFS"; color: Theme.text; font.family: Theme.mono; font.pixelSize: 11 }
                    Text { text: qsTr("Loudest 400 ms"); color: Theme.muted; font.pixelSize: 11 }
                    Text { text: Number(panel.loudness.loudestMomentaryLufs).toFixed(1) + " LUFS"; color: Theme.text; font.family: Theme.mono; font.pixelSize: 11 }
                    Text { text: qsTr("True peak"); color: Theme.muted; font.pixelSize: 11 }
                    Text { text: Number(panel.loudness.truePeakDb).toFixed(1) + " dBTP"; color: panel.loudness.truePeakDb > -1 ? Theme.danger : Theme.text; font.family: Theme.mono; font.pixelSize: 11 }
                    Text { text: qsTr("Measured"); color: Theme.muted; font.pixelSize: 11 }
                    Text { text: Number(panel.loudness.seconds).toFixed(1) + " s"; color: Theme.text; font.family: Theme.mono; font.pixelSize: 11 }
                }
                Text {
                    Layout.fillWidth: true
                    visible: panel.loudness.message !== undefined
                    text: panel.loudness.message || ""
                    color: panel.loudness.overCeiling ? Theme.warning : Theme.accent
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}

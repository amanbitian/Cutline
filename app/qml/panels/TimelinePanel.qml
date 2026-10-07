import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The timeline: the tools along the top, the tracks and clips below, and a drop target for media.
Item {
    property string panelId: "timeline"

    readonly property var tools: [
        {id: "selection", glyph: "↖", tip: qsTr("Selection (V)")},
        {id: "track_select", glyph: "⇥", tip: qsTr("Track select forward (A)")},
        {id: "ripple", glyph: "⇤", tip: qsTr("Ripple edit (B)")},
        {id: "roll", glyph: "⇔", tip: qsTr("Rolling edit (N)")},
        {id: "razor", glyph: "✂", tip: qsTr("Razor (C)")},
        {id: "slip", glyph: "↔", tip: qsTr("Slip (Y)")},
        {id: "slide", glyph: "⇆", tip: qsTr("Slide (U)")},
        {id: "hand", glyph: "✋", tip: qsTr("Hand (H)")},
        {id: "zoom", glyph: "⌕", tip: qsTr("Zoom (Z)")}
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.panelHeader
            RowLayout {
                anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad }
                spacing: 3
                Repeater {
                    model: tools
                    delegate: Chip {
                        required property var modelData
                        text: modelData.glyph
                        active: session.tool === modelData.id
                        ToolTip.text: modelData.tip
                        onClicked: session.tool = modelData.id
                    }
                }
                Rectangle { width: 1; Layout.fillHeight: true; Layout.margins: 6; color: Theme.border }
                Chip { text: qsTr("Snap"); active: session.snap; ToolTip.text: qsTr("Snap to edges and the playhead (S)"); onClicked: session.snap = !session.snap }
                Chip { text: "+V"; ToolTip.text: qsTr("Add a video track"); onClicked: session.trigger("timeline.add_video_track") }
                Chip { text: "+A"; ToolTip.text: qsTr("Add an audio track"); onClicked: session.trigger("timeline.add_audio_track") }
                Item { Layout.fillWidth: true }
                Text { text: qsTr("%1 selected").arg(session.selectionCount); visible: session.selectionCount > 0; color: Theme.muted; font.pixelSize: 11 }
                Chip { text: "−"; ToolTip.text: qsTr("Zoom out (-)"); onClicked: timelineItem.zoom(1 / 1.5) }
                Chip { text: "+"; ToolTip.text: qsTr("Zoom in (=)"); onClicked: timelineItem.zoom(1.5) }
                Chip { text: qsTr("Fit"); ToolTip.text: qsTr("Zoom to fit (\\)"); onClicked: timelineItem.zoomToFit() }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TimelineItem {
                id: timelineItem
                anchors.fill: parent
                session: appSession
            }
            DropArea {
                anchors.fill: parent
                keys: ["media"]
                onDropped: (drop) => {
                    if (drop.source && drop.source.mediaId !== undefined) {
                        timelineItem.dropMedia(drop.x, drop.y, drop.source.mediaId, session.ctrlHeld())
                    }
                }
            }
        }
    }
}

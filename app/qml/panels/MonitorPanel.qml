import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The program monitor: the picture at the playhead and the transport under it.
Item {
    property string panelId: "program_monitor"
    property alias monitor: monitorItem

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        MonitorItem {
            id: monitorItem
            Layout.fillWidth: true
            Layout.fillHeight: true
            session: appSession
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.panelHeader
            RowLayout {
                anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad }
                spacing: 4
                Chip { text: "⏮"; ToolTip.text: qsTr("Go to start (Home)"); onClicked: session.trigger("transport.go_start") }
                Chip { text: "◀|"; ToolTip.text: qsTr("Step back (Left)"); onClicked: session.trigger("transport.step_back") }
                Chip {
                    text: session.playing ? "⏸" : "▶"
                    active: session.playing
                    ToolTip.text: qsTr("Play / pause (Space)")
                    onClicked: session.trigger("transport.play_pause")
                }
                Chip { text: "|▶"; ToolTip.text: qsTr("Step forward (Right)"); onClicked: session.trigger("transport.step_forward") }
                Chip { text: "⏭"; ToolTip.text: qsTr("Go to end (End)"); onClicked: session.trigger("transport.go_end") }
                Chip { text: "↻"; active: session.loop; ToolTip.text: qsTr("Loop"); onClicked: session.trigger("transport.loop") }
                Rectangle { width: 1; Layout.fillHeight: true; Layout.margins: 6; color: Theme.border }
                Chip { text: "I"; ToolTip.text: qsTr("Mark in (I)"); onClicked: session.trigger("timeline.mark_in") }
                Chip { text: "O"; ToolTip.text: qsTr("Mark out (O)"); onClicked: session.trigger("timeline.mark_out") }
                Chip { text: "◆"; ToolTip.text: qsTr("Add marker (M)"); onClicked: session.trigger("timeline.add_marker") }
                Rectangle { width: 1; Layout.fillHeight: true; Layout.margins: 6; color: Theme.border }
                Chip { objectName: "toolMaskSelect"; text: "⌖"; active: session.maskTool === "select"; ToolTip.text: qsTr("Edit the chosen mask on the picture"); onClicked: session.maskSetTool(session.maskTool === "select" ? "none" : "select") }
                Chip { objectName: "toolMaskRectangle"; text: "▭"; active: session.maskTool === "rectangle"; ToolTip.text: qsTr("Draw a rectangle mask: drag on the picture (Shift for a square)"); onClicked: session.maskSetTool(session.maskTool === "rectangle" ? "none" : "rectangle") }
                Chip { objectName: "toolMaskEllipse"; text: "◯"; active: session.maskTool === "ellipse"; ToolTip.text: qsTr("Draw an ellipse mask: drag on the picture (Shift for a circle)"); onClicked: session.maskSetTool(session.maskTool === "ellipse" ? "none" : "ellipse") }
                Chip { objectName: "toolMaskPen"; text: "✎"; active: session.maskTool === "pen"; ToolTip.text: qsTr("Draw a path mask: click points; click the first one or press Enter to close; double-click an edge to add a point, a point to round it"); onClicked: session.maskSetTool(session.maskTool === "pen" ? "none" : "pen") }
                Item { Layout.fillWidth: true }
                Chip {
                    objectName: "gpuInfo"
                    text: session.gpuInfo !== "" ? qsTr("GPU") : qsTr("CPU")
                    active: session.gpuInfo !== ""
                    ToolTip.text: session.gpuInfo !== "" ? session.gpuInfo : qsTr("Software rendering")
                }
                Text {
                    visible: Math.abs(session.shuttleRate) > 0 && Math.abs(session.shuttleRate) !== 1
                    text: session.shuttleRate + "x"
                    color: Theme.warning
                    font.pixelSize: 12
                }
                ComboBox {
                    id: quality
                    implicitWidth: 92
                    implicitHeight: 24
                    model: [qsTr("Auto"), qsTr("Full"), qsTr("1/2"), qsTr("1/4"), qsTr("1/8")]
                    readonly property var keys: ["auto", "full", "half", "quarter", "eighth"]
                    currentIndex: Math.max(0, keys.indexOf(session.monitorQuality))
                    onActivated: (i) => session.monitorQuality = keys[i]
                    ToolTip.text: qsTr("Playback resolution")
                    ToolTip.visible: hovered
                    font.pixelSize: 11
                }
                Chip { text: "▭"; active: session.safeMargins; ToolTip.text: qsTr("Safe margins"); onClicked: session.safeMargins = !session.safeMargins }
                Chip { text: qsTr("Fit"); active: monitorItem.zoomMode === "fit"; onClicked: monitorItem.zoomMode = "fit" }
                Chip { text: "100%"; active: monitorItem.zoomMode === "actual"; onClicked: monitorItem.zoomMode = "actual" }
                Chip { text: "⛶"; ToolTip.text: qsTr("Full screen"); onClicked: session.trigger("monitor.fullscreen") }
            }
        }
    }
}

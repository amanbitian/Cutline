import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The speed of one clip as a graph over its own time. Drag a point up or down to change the speed there, sideways to move
// the cut between two segments; double-click the graph to cut a segment in two. Everything is edited on a working copy
// and written to the clip as one undoable step (or after every edit with live preview on).
Popup {
    id: dialog
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: Overlay.overlay
    width: 780
    height: 520
    padding: 12
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }

    property int selectedSegment: 0
    property int selectedBoundary: -1
    property var segments: session.rampSegments
    property real duration: session.rampDuration
    readonly property real marginLeft: 46
    readonly property real marginRight: 12
    readonly property real marginTop: 12
    readonly property real marginBottom: 22

    function speedRange() {
        var lo = -1, hi = 2
        for (var i = 0; i < segments.length; ++i) {
            lo = Math.min(lo, segments[i].startSpeed, segments[i].endSpeed)
            hi = Math.max(hi, segments[i].startSpeed, segments[i].endSpeed)
        }
        return { lo: lo - 0.25, hi: hi + 0.25 }
    }
    function xOf(t) { return marginLeft + (duration > 0 ? t / duration : 0) * (graph.width - marginLeft - marginRight) }
    function yOf(v) { var r = speedRange(); return marginTop + (r.hi - v) / (r.hi - r.lo) * (graph.height - marginTop - marginBottom) }
    function timeAt(x) { return Math.max(0, Math.min(duration, (x - marginLeft) / (graph.width - marginLeft - marginRight) * duration)) }
    function speedAt(y) { var r = speedRange(); return r.hi - (y - marginTop) / (graph.height - marginTop - marginBottom) * (r.hi - r.lo) }
    // The draggable points: each segment's two ends. Two ends that meet at the same speed are one point.
    function handles() {
        var list = []
        for (var i = 0; i < segments.length; ++i) {
            var s = segments[i]
            var join = i > 0 && Math.abs(segments[i - 1].endSpeed - s.startSpeed) < 1e-9
            if (join) list[list.length - 1].side = "both"
            else list.push({ boundary: i, side: "after", t: s.start, v: s.startSpeed })
            list.push({ boundary: i + 1, side: "before", t: s.start + s.duration, v: s.endSpeed })
        }
        return list
    }
    function handleAt(x, y) {
        var list = handles()
        for (var i = 0; i < list.length; ++i) {
            if (Math.abs(xOf(list[i].t) - x) <= 8 && Math.abs(yOf(list[i].v) - y) <= 8) return list[i]
        }
        return null
    }
    function segmentAt(t) {
        for (var i = 0; i < segments.length; ++i) {
            if (t < segments[i].start + segments[i].duration || i === segments.length - 1) return i
        }
        return 0
    }
    onSegmentsChanged: { if (selectedSegment >= segments.length) selectedSegment = Math.max(0, segments.length - 1); graph.requestPaint() }
    onOpened: { selectedSegment = 0; selectedBoundary = -1 }
    onClosed: session.rampClose()

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Text { text: qsTr("Speed ramp"); color: Theme.text; font.pixelSize: 15; font.bold: true; Layout.fillWidth: true }
            Text { text: qsTr("%1 s").arg(Number(dialog.duration).toFixed(2)); color: Theme.muted; objectName: "rampDuration" }
            Chip { text: "✕"; onClicked: dialog.close() }
        }

        Canvas {
            id: graph
            objectName: "rampGraph"
            Layout.fillWidth: true
            Layout.fillHeight: true
            property var drag: null
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = "#101111"
                ctx.fillRect(0, 0, width, height)
                var r = dialog.speedRange()
                // Horizontal grid at whole multiples and the zero line.
                ctx.font = "10px sans-serif"
                ctx.textAlign = "right"
                var step = (r.hi - r.lo) > 8 ? 2 : 1
                for (var v = Math.ceil(r.lo); v <= r.hi; v += step) {
                    var y = dialog.yOf(v)
                    ctx.strokeStyle = v === 0 ? "#5c6462" : "#242828"
                    ctx.lineWidth = 1
                    ctx.beginPath(); ctx.moveTo(dialog.marginLeft, y); ctx.lineTo(width - dialog.marginRight, y); ctx.stroke()
                    ctx.fillStyle = "#8a9290"
                    ctx.fillText(Math.round(v * 100) + "%", dialog.marginLeft - 6, y + 3)
                }
                ctx.textAlign = "center"
                var ticks = Math.max(1, Math.floor(dialog.duration))
                var every = ticks > 12 ? Math.ceil(ticks / 12) : 1
                for (var t = 0; t <= dialog.duration + 1e-9; t += every) {
                    ctx.fillStyle = "#8a9290"
                    ctx.fillText(t.toFixed(0) + "s", dialog.xOf(t), height - 6)
                }
                // The segment being edited.
                var segs = dialog.segments
                if (dialog.selectedSegment >= 0 && dialog.selectedSegment < segs.length) {
                    var sel = segs[dialog.selectedSegment]
                    ctx.fillStyle = "rgba(217,255,98,0.08)"
                    ctx.fillRect(dialog.xOf(sel.start), dialog.marginTop, dialog.xOf(sel.start + sel.duration) - dialog.xOf(sel.start), height - dialog.marginTop - dialog.marginBottom)
                }
                // The speed, filled down to the zero line.
                var pts = session.rampGraph
                if (pts.length > 1) {
                    ctx.beginPath()
                    ctx.moveTo(dialog.xOf(pts[0][0]), dialog.yOf(0))
                    for (var i = 0; i < pts.length; ++i) ctx.lineTo(dialog.xOf(pts[i][0]), dialog.yOf(pts[i][1]))
                    ctx.lineTo(dialog.xOf(pts[pts.length - 1][0]), dialog.yOf(0))
                    ctx.closePath()
                    ctx.fillStyle = "rgba(217,255,98,0.16)"
                    ctx.fill()
                    ctx.beginPath()
                    ctx.moveTo(dialog.xOf(pts[0][0]), dialog.yOf(pts[0][1]))
                    for (var j = 1; j < pts.length; ++j) ctx.lineTo(dialog.xOf(pts[j][0]), dialog.yOf(pts[j][1]))
                    ctx.strokeStyle = "#d9ff62"
                    ctx.lineWidth = 2
                    ctx.stroke()
                }
                // The playhead, if it is inside the clip.
                var local = session.playhead - session.rampClipStart
                if (local >= 0 && local <= dialog.duration) {
                    ctx.strokeStyle = "#ff5a4f"
                    ctx.lineWidth = 1.5
                    ctx.beginPath(); ctx.moveTo(dialog.xOf(local), dialog.marginTop); ctx.lineTo(dialog.xOf(local), height - dialog.marginBottom); ctx.stroke()
                }
                var hs = dialog.handles()
                for (var k = 0; k < hs.length; ++k) {
                    var h = hs[k]
                    ctx.beginPath()
                    ctx.arc(dialog.xOf(h.t), dialog.yOf(h.v), 5, 0, Math.PI * 2)
                    ctx.fillStyle = h.boundary === dialog.selectedBoundary ? "#ffffff" : "#101111"
                    ctx.fill()
                    ctx.strokeStyle = "#d9ff62"
                    ctx.lineWidth = 2
                    ctx.stroke()
                }
            }
            Connections { target: session; function onRampChanged() { graph.requestPaint() } function onPlayheadChanged() { graph.requestPaint() } }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            MouseArea {
                anchors.fill: parent
                objectName: "rampGraphMouse"
                onPressed: (mouse) => {
                    var h = dialog.handleAt(mouse.x, mouse.y)
                    if (h) {
                        graph.drag = h
                        dialog.selectedBoundary = h.boundary
                        session.rampDrag(true)
                    } else {
                        graph.drag = null
                        dialog.selectedBoundary = -1
                        dialog.selectedSegment = dialog.segmentAt(dialog.timeAt(mouse.x))
                        session.rampPreview(dialog.timeAt(mouse.x))
                    }
                    graph.requestPaint()
                }
                onPositionChanged: (mouse) => {
                    if (!graph.drag) return
                    var d = graph.drag
                    var inner = d.boundary > 0 && d.boundary < dialog.segments.length
                    if (inner) session.rampMoveBoundary(d.boundary, dialog.timeAt(mouse.x))
                    session.rampSetBoundarySpeed(d.boundary, Math.round(dialog.speedAt(mouse.y) * 20) / 20, d.side)
                    session.rampPreview(dialog.timeAt(mouse.x))
                }
                onReleased: { if (graph.drag) session.rampDrag(false); graph.drag = null }
                onDoubleClicked: (mouse) => {
                    if (!dialog.handleAt(mouse.x, mouse.y)) session.rampSplit(dialog.timeAt(mouse.x))
                }
            }
        }

        // The segment selected on the graph: its speeds, how it eases, and what to do with it.
        RowLayout {
            spacing: 8
            readonly property var seg: dialog.segments.length > dialog.selectedSegment ? dialog.segments[dialog.selectedSegment] : null
            Text { text: qsTr("Segment %1").arg(dialog.selectedSegment + 1); color: Theme.muted }
            Text { text: qsTr("from %"); color: Theme.muted }
            NumberBox {
                objectName: "rampStartSpeed"
                Layout.preferredWidth: 70
                value: parent.seg ? parent.seg.startSpeed * 100 : 100
                minimum: -10000; maximum: 10000
                onCommitted: (v) => session.rampSetSegmentSpeed(dialog.selectedSegment, v / 100, parent.seg.endSpeed)
            }
            Text { text: qsTr("to %"); color: Theme.muted }
            NumberBox {
                objectName: "rampEndSpeed"
                Layout.preferredWidth: 70
                value: parent.seg ? parent.seg.endSpeed * 100 : 100
                minimum: -10000; maximum: 10000
                onCommitted: (v) => session.rampSetSegmentSpeed(dialog.selectedSegment, parent.seg.startSpeed, v / 100)
            }
            Chip { text: qsTr("Linear"); onClicked: session.rampEase(dialog.selectedSegment, "linear"); ToolTip.text: qsTr("Straight change of speed") }
            Chip { text: qsTr("Ease in"); onClicked: session.rampEase(dialog.selectedSegment, "in"); ToolTip.text: qsTr("Slow at the start of the segment") }
            Chip { text: qsTr("Ease out"); onClicked: session.rampEase(dialog.selectedSegment, "out"); ToolTip.text: qsTr("Slow at the end of the segment") }
            Chip { text: qsTr("Ease in/out"); onClicked: session.rampEase(dialog.selectedSegment, "inout") }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            spacing: 8
            Chip { text: qsTr("Freeze"); onClicked: session.rampFreeze(dialog.selectedSegment); ToolTip.text: qsTr("Hold one picture for this segment") }
            Chip { text: qsTr("Reverse"); onClicked: session.rampReverse(dialog.selectedSegment) }
            Chip { text: qsTr("Cut here"); onClicked: session.rampSplit(session.playhead - session.rampClipStart); ToolTip.text: qsTr("Cut the segment at the playhead") }
            Chip {
                text: qsTr("Join")
                enabled: dialog.selectedBoundary > 0 && dialog.selectedBoundary < dialog.segments.length
                onClicked: session.rampRemoveBoundary(dialog.selectedBoundary)
                ToolTip.text: qsTr("Remove the selected cut between two segments")
            }
            Text { text: qsTr("All speeds ×"); color: Theme.muted }
            NumberBox { id: scale; Layout.preferredWidth: 56; value: 1; minimum: 0.01; maximum: 100; onCommitted: (v) => { session.rampScale(v); value = 1 } }
            Text { text: qsTr("Length (s)"); color: Theme.muted }
            NumberBox { Layout.preferredWidth: 64; value: dialog.duration; minimum: 0.04; maximum: 3600; onCommitted: (v) => session.rampSetDuration(v) }
            Item { Layout.fillWidth: true }
        }
        Text {
            objectName: "rampProblem"
            visible: session.rampProblem !== ""
            text: session.rampProblem
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        RowLayout {
            spacing: 8
            Check {
                objectName: "rampLive"
                text: qsTr("Live preview")
                checked: session.rampLive
                onToggled: session.rampLive = checked
                ToolTip.text: qsTr("Write each change to the clip as you make it (every change is one undo step)")
                ToolTip.visible: hovered
            }
            Item { Layout.fillWidth: true }
            Chip { text: qsTr("Remove ramp"); onClicked: session.rampClear() }
            Chip { text: qsTr("Revert"); enabled: session.rampDirty; onClicked: session.rampReset() }
            Chip { objectName: "rampApplyButton"; text: qsTr("Apply"); active: session.rampDirty; enabled: session.rampDirty && session.rampProblem === ""; onClicked: session.rampApply() }
        }
    }
}

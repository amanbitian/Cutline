import QtQuick
import QtQuick.Controls
import Cutline

// Draws the panel arrangement the layout works out: each group of panels with its tab strip, the splitters between
// them, and any floating windows. Dragging a tab onto another group docks it there (the middle makes a tab, an edge
// splits); dragging a splitter resizes; a tab's menu floats or closes it.
Item {
    id: area
    required property var dock
    property var arrangement: ({groups: [], splitters: []})

    function refresh() {
        arrangement = dock.arrange(width, height)
    }
    onWidthChanged: refresh()
    onHeightChanged: refresh()
    Component.onCompleted: refresh()
    Connections { target: area.dock; function onChanged() { area.refresh() } }

    // The tab being dragged, and where it would land.
    property string dragPanel: ""
    property real dragX: 0
    property real dragY: 0
    property var dropTarget: null   // {panel, zone, x, y, width, height}

    function targetAt(px, py) {
        const groups = arrangement.groups
        for (let i = groups.length - 1; i >= 0; --i) {
            const g = groups[i]
            if (px >= g.x && px < g.x + g.width && py >= g.y && py < g.y + g.height) {
                const active = g.tabs[Math.max(0, Math.min(g.active, g.tabs.length - 1))].id
                if (active === dragPanel && g.tabs.length === 1) return null
                const fx = (px - g.x) / g.width, fy = (py - g.y) / g.height
                let zone = "center", rect = {x: g.x, y: g.y, width: g.width, height: g.height}
                if (g.floating < 0) {
                    if (fy < 0.18) { zone = "top"; rect = {x: g.x, y: g.y, width: g.width, height: g.height * 0.3} }
                    else if (fy > 0.82) { zone = "bottom"; rect = {x: g.x, y: g.y + g.height * 0.7, width: g.width, height: g.height * 0.3} }
                    else if (fx < 0.2) { zone = "left"; rect = {x: g.x, y: g.y, width: g.width * 0.3, height: g.height} }
                    else if (fx > 0.8) { zone = "right"; rect = {x: g.x + g.width * 0.7, y: g.y, width: g.width * 0.3, height: g.height} }
                }
                return {panel: active, zone: zone, x: rect.x, y: rect.y, width: rect.width, height: rect.height}
            }
        }
        return null
    }

    Repeater {
        model: area.arrangement.groups
        delegate: PanelGroup {
            required property var modelData
            x: modelData.x; y: modelData.y; width: modelData.width; height: modelData.height
            group: modelData
            dock: area.dock
            floating: modelData.floating >= 0
            z: floating ? 10 : 0
            onTabDragStarted: (panel) => { area.dragPanel = panel }
            onTabDragMoved: (px, py) => {
                const p = mapToItem(area, px, py)
                area.dragX = p.x; area.dragY = p.y
                area.dropTarget = area.targetAt(p.x, p.y)
            }
            onTabDropped: (panel) => {
                const t = area.dropTarget
                area.dragPanel = ""
                area.dropTarget = null
                if (t && t.panel !== panel) area.dock.dock(panel, t.panel, t.zone, 0.3)
            }
        }
    }

    Repeater {
        model: area.arrangement.splitters
        delegate: Rectangle {
            required property var modelData
            x: modelData.x; y: modelData.y; width: modelData.width; height: modelData.height
            color: grab.containsMouse || grab.pressed ? Theme.accent : "transparent"
            opacity: grab.pressed ? 0.6 : 0.35
            z: 5
            MouseArea {
                id: grab
                anchors.fill: parent
                anchors.margins: -3
                hoverEnabled: true
                cursorShape: modelData.horizontal ? Qt.SplitHCursor : Qt.SplitVCursor
                property real lastPos: 0
                onPressed: (mouse) => { lastPos = modelData.horizontal ? mouse.x : mouse.y }
                onPositionChanged: (mouse) => {
                    if (!pressed) return
                    const here = modelData.horizontal ? mouse.x : mouse.y
                    const delta = here - lastPos
                    if (Math.abs(delta) >= 1) area.dock.moveSplitter(modelData.path, modelData.index, delta, modelData.extent)
                }
            }
        }
    }

    // The drop preview.
    Rectangle {
        visible: area.dropTarget !== null
        x: area.dropTarget ? area.dropTarget.x : 0
        y: area.dropTarget ? area.dropTarget.y : 0
        width: area.dropTarget ? area.dropTarget.width : 0
        height: area.dropTarget ? area.dropTarget.height : 0
        color: Qt.rgba(0.85, 1, 0.38, 0.18)
        border.color: Theme.accent
        border.width: 2
        z: 20
    }
}

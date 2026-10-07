import QtQuick
import QtQuick.Controls
import Cutline

// One group of panels: a strip of tabs over the active panel's content.
Rectangle {
    id: root
    required property var group
    required property var dock
    property bool floating: false
    signal tabDragStarted(string panel)
    signal tabDragMoved(real x, real y)
    signal tabDropped(string panel)

    color: Theme.panel
    border.color: floating ? Theme.accent : Theme.border
    border.width: 1
    clip: true

    readonly property string activePanel: group.tabs.length > 0 ? group.tabs[Math.max(0, Math.min(group.active, group.tabs.length - 1))].id : ""

    Rectangle {
        id: strip
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.tabHeight
        color: Theme.panelHeader

        Row {
            anchors.fill: parent
            Repeater {
                model: root.group.tabs
                delegate: Rectangle {
                    id: tab
                    required property var modelData
                    required property int index
                    readonly property bool current: modelData.id === root.activePanel
                    width: Math.min(titleText.implicitWidth + 44, 220)
                    height: strip.height
                    color: current ? Theme.panel : (tabMouse.containsMouse ? Theme.hover : "transparent")
                    Rectangle {
                        visible: tab.current
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 2
                        color: Theme.accent
                    }
                    Text {
                        id: titleText
                        anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                        text: tab.modelData.title
                        color: tab.current ? Theme.text : Theme.muted
                        font.family: Theme.ui
                        font.pixelSize: 12
                        font.weight: tab.current ? Font.DemiBold : Font.Normal
                    }
                    Text {
                        anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                        text: "×"
                        visible: tab.current || tabMouse.containsMouse || closeArea.containsMouse
                        color: closeArea.containsMouse ? Theme.danger : Theme.faint
                        font.family: Theme.ui
                        font.pixelSize: 14
                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            onClicked: root.dock.closePanel(tab.modelData.id)
                        }
                    }
                    Behavior on color { ColorAnimation { duration: 90 } }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        anchors.rightMargin: 22
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        property real startX: 0
                        property real startY: 0
                        property bool dragging: false
                        onPressed: (mouse) => {
                            startX = mouse.x; startY = mouse.y; dragging = false
                            if (mouse.button === Qt.LeftButton) root.dock.activate(tab.modelData.id)
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed || mouse.buttons !== Qt.LeftButton) return
                            if (!dragging && (Math.abs(mouse.x - startX) > 8 || Math.abs(mouse.y - startY) > 8)) {
                                dragging = true
                                root.tabDragStarted(tab.modelData.id)
                            }
                            if (dragging) {
                                const p = mapToItem(root, mouse.x, mouse.y)
                                root.tabDragMoved(p.x, p.y)
                            }
                        }
                        onReleased: (mouse) => {
                            if (dragging) root.tabDropped(tab.modelData.id)
                            dragging = false
                        }
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) tabMenu.popup()
                        }
                    }
                    Menu {
                        id: tabMenu
                        MenuItem { text: qsTr("Close Panel"); onTriggered: root.dock.closePanel(tab.modelData.id) }
                        MenuItem {
                            text: qsTr("Float Panel")
                            enabled: !root.floating
                            onTriggered: root.dock.floatPanel(tab.modelData.id, root.x + 40, root.y + 40, Math.max(360, root.width * 0.6), Math.max(260, root.height * 0.6))
                        }
                    }
                }
            }
        }
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.border }
    }

    Loader {
        id: content
        anchors { left: parent.left; right: parent.right; top: strip.bottom; bottom: parent.bottom }
        source: {
            switch (root.activePanel) {
            case "project": return "panels/ProjectPanel.qml"
            case "program_monitor": return "panels/MonitorPanel.qml"
            case "timeline": return "panels/TimelinePanel.qml"
            case "effect_controls": return "panels/InspectorPanel.qml"
            case "effects": return "panels/EffectsPanel.qml"
            case "history": return "panels/HistoryPanel.qml"
            case "jobs": return "panels/JobsPanel.qml"
            case "multicam": return "panels/MulticamPanel.qml"
            case "exports": return "panels/ExportsPanel.qml"
            case "audio_mixer": return "panels/AudioMixerPanel.qml"
            case "scopes": return "panels/ScopesPanel.qml"
            case "transcript": return "panels/TranscriptPanel.qml"
            case "audio_essentials": return "panels/AudioEssentialsPanel.qml"
            case "captions": return "panels/CaptionsPanel.qml"
            case "graphics": return "panels/GraphicsPanel.qml"
            case "graphic_designer": return "panels/GraphicDesignerPanel.qml"
            case "": return ""
            default: return "panels/PlaceholderPanel.qml"
            }
        }
        onLoaded: if (item && item.hasOwnProperty("panelId")) item.panelId = root.activePanel
    }
}

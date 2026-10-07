import QtQuick
import QtQuick.Controls
import Cutline

// A small flat button used throughout the panels: text or a one-letter glyph, optionally checkable.
Button {
    id: chip
    property bool active: false
    property color tone: Theme.accent
    implicitHeight: 26
    implicitWidth: Math.max(28, label.implicitWidth + 16)
    padding: 0
    hoverEnabled: true
    opacity: enabled ? 1.0 : 0.48
    scale: down ? 0.97 : 1.0

    background: Rectangle {
        radius: 4
        color: chip.active ? Theme.accentSoft : (chip.down ? Theme.pressed : (chip.hovered ? Theme.hover : "transparent"))
        border.width: 1
        border.color: chip.active || chip.activeFocus ? chip.tone : (chip.hovered ? Theme.faint : Theme.border)
        Behavior on color { ColorAnimation { duration: 90 } }
        Behavior on border.color { ColorAnimation { duration: 90 } }
    }
    contentItem: Text {
        id: label
        text: chip.text
        color: !chip.enabled ? Theme.faint : (chip.active ? chip.tone : Theme.text)
        font.family: Theme.ui
        font.pixelSize: 12
        font.weight: chip.active ? Font.DemiBold : Font.Normal
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    Behavior on scale { NumberAnimation { duration: 70 } }
    ToolTip.visible: hovered && ToolTip.text.length > 0
    ToolTip.delay: 450
}

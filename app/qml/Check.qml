import QtQuick
import QtQuick.Controls
import Cutline

// A tick box that looks like the rest of the interface: the box, the tick, and the label beside it.
CheckBox {
    id: box
    implicitHeight: 24
    spacing: 8
    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: box.leftPadding
        y: (box.height - height) / 2
        radius: 3
        color: Theme.bg
        border.color: box.checked ? Theme.accent : Theme.border
        Text { anchors.centerIn: parent; visible: box.checked; text: "✓"; color: Theme.accent; font.pixelSize: 12 }
    }
    contentItem: Text {
        text: box.text
        color: box.enabled ? Theme.text : Theme.faint
        font.pixelSize: 12
        leftPadding: box.indicator.width + box.spacing
        verticalAlignment: Text.AlignVCenter
    }
}

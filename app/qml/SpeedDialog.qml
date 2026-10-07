import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Speed and direction of the selected clips.
Dialog {
    id: dialog
    title: qsTr("Clip speed")
    modal: true
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Ok | Dialog.Cancel
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }

    ColumnLayout {
        spacing: 8
        RowLayout {
            Text { text: qsTr("Speed (%)"); color: Theme.muted; Layout.preferredWidth: 90 }
            NumberBox { id: percent; value: 100; minimum: 1; maximum: 10000; Layout.preferredWidth: 90; onCommitted: (v) => value = v }
        }
        Check { id: reversed; text: qsTr("Reverse") }
        Check { id: pitch; text: qsTr("Keep audio pitch") }
    }
    onAccepted: session.setClipSpeed(percent.value, reversed.checked, pitch.checked)
}

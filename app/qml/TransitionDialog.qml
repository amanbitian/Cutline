import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// A transition on the timeline: its kind, how long it lasts and where it sits against the cut.
Dialog {
    id: dialog
    title: qsTr("Transition")
    modal: true
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Ok | Dialog.Cancel
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
    property string transitionId: ""
    property var kinds: session.transitionCatalogue("")

    function show(id) {
        const info = session.transitionInfo(id)
        if (info.id === undefined) return
        transitionId = id
        kinds = session.transitionCatalogue("")
        for (let i = 0; i < kinds.length; ++i) if (kinds[i].id === info.kind) kindBox.currentIndex = i
        seconds.value = info.seconds
        alignBox.currentIndex = info.alignment === "start" ? 1 : info.alignment === "end" ? 2 : 0
        alignBox.enabled = !info.oneSided
        open()
    }

    ColumnLayout {
        spacing: 8
        width: 360
        RowLayout {
            Text { text: qsTr("Kind"); color: Theme.muted; Layout.preferredWidth: 90 }
            ComboBox { id: kindBox; objectName: "transitionKind"; Layout.fillWidth: true; model: dialog.kinds; textRole: "name"; valueRole: "id" }
        }
        Text { Layout.fillWidth: true; text: dialog.kinds.length > kindBox.currentIndex && kindBox.currentIndex >= 0 ? dialog.kinds[kindBox.currentIndex].description : ""; color: Theme.faint; font.pixelSize: 11; wrapMode: Text.WordWrap }
        RowLayout {
            Text { text: qsTr("Length (seconds)"); color: Theme.muted; Layout.preferredWidth: 90 }
            NumberBox { id: seconds; objectName: "transitionSeconds"; value: 1; minimum: 0.04; maximum: 60; Layout.preferredWidth: 90; onCommitted: (v) => value = v }
        }
        RowLayout {
            Text { text: qsTr("Against the cut"); color: Theme.muted; Layout.preferredWidth: 90 }
            ComboBox { id: alignBox; Layout.fillWidth: true; model: [qsTr("Centred on the cut"), qsTr("Starting at the cut"), qsTr("Ending at the cut")] }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("A transition needs spare picture beyond the cut (the clips' handles); it is shortened to what they allow.")
            color: Theme.faint
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Item { Layout.fillWidth: true }
            Chip { objectName: "transitionRemove"; text: qsTr("Remove transition"); tone: Theme.danger; onClicked: { session.removeTransition(dialog.transitionId); dialog.close() } }
        }
    }
    onAccepted: session.changeTransition(transitionId, kindBox.currentValue, seconds.value, alignBox.currentIndex === 1 ? "start" : alignBox.currentIndex === 2 ? "end" : "center")
}

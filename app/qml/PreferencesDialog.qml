import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// Every preference, built from the declarations in ui/Preferences.cpp: grouped by category, each with the control its
// kind needs, a note when it takes effect only after a restart, and a way back to its default.
Dialog {
    id: dialog
    title: qsTr("Preferences")
    modal: true
    width: 760
    height: 560
    anchors.centerIn: Overlay.overlay
    standardButtons: Dialog.Close
    background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
    property var prefs: session.preferenceList()
    property string category: ""

    function categories() {
        const seen = []
        for (const p of prefs) if (seen.indexOf(p.category) < 0) seen.push(p.category)
        return seen
    }
    function reload() { prefs = session.preferenceList() }
    onAboutToShow: { reload(); if (category === "") category = categories()[0] }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        ListView {
            id: cats
            Layout.preferredWidth: 150
            Layout.fillHeight: true
            model: dialog.categories()
            delegate: Rectangle {
                required property string modelData
                width: cats.width
                height: 30
                color: modelData === dialog.category ? Theme.selection : "transparent"
                Text { anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter } text: modelData; color: Theme.text; font.pixelSize: 12 }
                MouseArea { anchors.fill: parent; onClicked: dialog.category = parent.modelData }
            }
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: Theme.border }

        ListView {
            id: rows
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: dialog.prefs.filter(p => p.category === dialog.category)
            ScrollBar.vertical: ScrollBar {}
            delegate: Item {
                id: pref
                required property var modelData
                width: rows.width - 12
                height: 46
                ColumnLayout {
                    anchors { left: parent.left; right: control.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                    spacing: 0
                    Text { text: pref.modelData.label + (pref.modelData.restart ? qsTr("  (after restart)") : ""); color: Theme.text; font.pixelSize: 12 }
                    Text { text: pref.modelData.description; color: Theme.muted; font.pixelSize: 10; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
                Row {
                    id: control
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    spacing: 6
                    Loader {
                        anchors.verticalCenter: parent.verticalCenter
                        sourceComponent: {
                            switch (pref.modelData.kind) {
                            case 0: return boolEditor
                            case 1: case 2: return numberEditor
                            case 4: return choiceEditor
                            default: return textEditor
                            }
                        }
                    }
                    Chip {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "↺"
                        visible: !pref.modelData.isDefault
                        implicitWidth: 22
                        onClicked: { session.resetPreference(pref.modelData.key); dialog.reload() }
                    }
                }
                Component { id: boolEditor; Switch { checked: pref.modelData.value; onToggled: { session.setPreference(pref.modelData.key, checked); dialog.reload() } } }
                Component {
                    id: numberEditor
                    NumberBox {
                        width: 90
                        value: pref.modelData.value
                        minimum: pref.modelData.min
                        maximum: pref.modelData.max
                        onCommitted: (v) => { session.setPreference(pref.modelData.key, pref.modelData.kind === 1 ? Math.round(v) : v); dialog.reload() }
                    }
                }
                Component {
                    id: choiceEditor
                    ComboBox {
                        width: 130
                        model: pref.modelData.choices
                        currentIndex: pref.modelData.choices.indexOf(pref.modelData.value)
                        onActivated: (i) => { session.setPreference(pref.modelData.key, pref.modelData.choices[i]); dialog.reload() }
                    }
                }
                Component {
                    id: textEditor
                    TextField {
                        width: 200
                        text: pref.modelData.value
                        color: Theme.text
                        background: Rectangle { color: Theme.field; border.color: Theme.border; radius: 2 }
                        onEditingFinished: { session.setPreference(pref.modelData.key, text); dialog.reload() }
                    }
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import Cutline

// The colour spaces a clip's picture can be declared to be in (what log footage needs when its file says nothing).
Menu {
    id: menu
    width: 300
    Repeater {
        model: session.colorSpaces()
        delegate: MenuItem {
            required property var modelData
            text: modelData.group + "  ›  " + modelData.label
            onTriggered: session.addInputColorSpace(modelData.id)
        }
    }
}

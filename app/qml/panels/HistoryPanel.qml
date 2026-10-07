import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// The undo stack as steps: click one to return the project to just after it. Steps after the current one can be redone.
Item {
    property string panelId: "history"

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: ["(Start)"].concat(session.history)
        ScrollBar.vertical: ScrollBar {}
        delegate: Rectangle {
            required property string modelData
            required property int index
            width: list.width
            height: 24
            color: index === session.appliedSteps ? Theme.selection : (area.containsMouse ? Theme.hover : "transparent")
            Text {
                anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                text: modelData
                color: index <= session.appliedSteps ? Theme.text : Theme.faint
                font.pixelSize: 12
                font.italic: index > session.appliedSteps
            }
            MouseArea { id: area; anchors.fill: parent; hoverEnabled: true; onClicked: session.undoTo(index) }
        }
    }
}

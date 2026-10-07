import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

// One parameter of an effect: its name, a stopwatch, keyframe navigation, a slider (where it has a range) and number
// fields for each component, and a reset.
Item {
    id: row
    required property string effectId
    required property var param
    implicitHeight: param.dimension > 1 || !(param.hasMin && param.hasMax) ? 50 : 28
    height: implicitHeight

    function commit(index, value) {
        const components = []
        for (let i = 0; i < param.dimension; ++i) components.push(i === index ? value : param.value.components[i])
        session.setParameter(effectId, param.name, components)
    }

    RowLayout {
        anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
        spacing: 4

        Chip {
            text: "⏱"
            active: row.param.keyframed
            enabled: row.param.keyframeable
            ToolTip.text: row.param.keyframed ? qsTr("Stop animating") : qsTr("Animate: add a keyframe here")
            onClicked: session.toggleAnimation(row.effectId, row.param.name)
            implicitWidth: 24
        }
        Text { text: row.param.label; color: Theme.text; font.pixelSize: 11; Layout.preferredWidth: 82; elide: Text.ElideRight }

        // Keyframe navigation, shown when the parameter is animated.
        Row {
            visible: row.param.keyframed
            spacing: 1
            Chip { text: "◀"; implicitWidth: 20; onClicked: session.jumpToKeyframe(row.effectId, row.param.name, false) }
            Chip { text: "◆"; implicitWidth: 20; active: row.param.keyHere; onClicked: session.toggleKeyframe(row.effectId, row.param.name) }
            Chip { text: "▶"; implicitWidth: 20; onClicked: session.jumpToKeyframe(row.effectId, row.param.name, true) }
        }

        Loader {
            Layout.fillWidth: true
            sourceComponent: row.param.dimension === 1 && row.param.hasMin && row.param.hasMax ? sliderEditor : fieldsEditor
        }

        Chip {
            text: "↺"
            implicitWidth: 22
            visible: !isDefault()
            ToolTip.text: qsTr("Reset to the default")
            onClicked: session.resetParameter(row.effectId, row.param.name)
            function isDefault() {
                for (let i = 0; i < row.param.dimension; ++i)
                    if (Math.abs(row.param.value.components[i] - row.param.default.components[i]) > 1e-9) return false
                return !row.param.keyframed
            }
        }
    }

    Component {
        id: sliderEditor
        RowLayout {
            spacing: 6
            Slider {
                id: slider
                Layout.fillWidth: true
                from: row.param.min
                to: row.param.max
                value: row.param.value.components[0]
                implicitHeight: 20
                onPressedChanged: if (!pressed) row.commit(0, value)
            }
            NumberBox {
                Layout.preferredWidth: 58
                value: slider.pressed ? slider.value : row.param.value.components[0]
                minimum: row.param.min
                maximum: row.param.max
                onCommitted: (v) => row.commit(0, v)
            }
        }
    }

    Component {
        id: fieldsEditor
        RowLayout {
            spacing: 4
            Repeater {
                model: row.param.dimension
                delegate: NumberBox {
                    required property int index
                    Layout.fillWidth: true
                    value: row.param.value.components[index]
                    minimum: row.param.hasMin ? row.param.min : -1e9
                    maximum: row.param.hasMax ? row.param.max : 1e9
                    onCommitted: (v) => row.commit(index, v)
                }
            }
        }
    }
}

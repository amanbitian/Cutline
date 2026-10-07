import QtQuick
import QtQuick.Controls
import Cutline

// A number field: shows a value, takes a typed one (limited to its range), and says when the person has confirmed it.
TextField {
    id: box
    property real value: 0
    property real minimum: -1e9
    property real maximum: 1e9
    signal committed(real v)

    text: Number(value.toFixed(4)).toString()
    color: Theme.text
    font.pixelSize: 11
    implicitHeight: 22
    horizontalAlignment: TextInput.AlignRight
    selectByMouse: true
    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
    background: Rectangle { color: Theme.bg; border.color: box.activeFocus ? Theme.accent : Theme.border; radius: 2 }
    onEditingFinished: {
        const parsed = parseFloat(text)
        if (!isNaN(parsed)) {
            const clamped = Math.max(minimum, Math.min(maximum, parsed))
            if (Math.abs(clamped - value) > 1e-9) committed(clamped)
            else text = Number(value.toFixed(4)).toString()
        } else {
            text = Number(value.toFixed(4)).toString()
        }
    }
}

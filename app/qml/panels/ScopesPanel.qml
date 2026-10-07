import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Cutline

Item {
    property string panelId: "scopes"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 6
            spacing: 3
            Chip { text: qsTr("Waveform"); active: session.scopeMode === "waveform"; onClicked: session.scopeMode = "waveform" }
            Chip { text: qsTr("RGB Parade"); active: session.scopeMode === "parade"; onClicked: session.scopeMode = "parade" }
            Chip { text: qsTr("Vector"); active: session.scopeMode === "vectorscope"; onClicked: session.scopeMode = "vectorscope" }
            Chip { text: qsTr("Histogram"); active: session.scopeMode === "histogram"; onClicked: session.scopeMode = "histogram" }
            Item { Layout.fillWidth: true }
        }
        ScopeItem {
            objectName: "videoScope"
            Layout.fillWidth: true
            Layout.fillHeight: true
            session: appSession
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.bottomMargin: 4
            Text { text: session.scopeMode === "vectorscope" ? qsTr("Cb / Cr") : session.scopeMode === "histogram" ? qsTr("0 — 255") : qsTr("0 — 100 IRE"); color: Theme.faint; font.pixelSize: 9 }
            Item { Layout.fillWidth: true }
            Text { text: qsTr("asynchronous · latest frame"); color: Theme.faint; font.pixelSize: 9 }
        }
    }
}

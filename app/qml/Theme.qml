pragma Singleton
import QtQuick

// One place for the interface's colours and sizes, so a panel never invents its own.
QtObject {
    readonly property color bg: "#0d1013"
    readonly property color panel: "#15191e"
    readonly property color panelHeader: "#1a1f25"
    readonly property color raised: "#1d232a"
    readonly property color field: "#20262d"
    readonly property color border: "#303842"
    readonly property color borderSoft: "#252c34"
    readonly property color text: "#edf1f5"
    readonly property color muted: "#9aa5b1"
    readonly property color faint: "#687583"
    readonly property color accent: "#c4ed6a"
    readonly property color accentSoft: "#27351f"
    readonly property color danger: "#ff7180"
    readonly property color warning: "#f1c75b"
    readonly property color selection: "#24303a"
    readonly property color hover: "#20262d"
    readonly property color pressed: "#29313a"
    readonly property int rowHeight: 28
    readonly property int tabHeight: 32
    readonly property int pad: 10
    readonly property string ui: "Segoe UI"
    readonly property string mono: "Consolas"
}

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Cutline

ApplicationWindow {
    id: window
    width: 1600
    height: 940
    minimumWidth: 900
    minimumHeight: 560
    visible: true
    title: session.projectOpen ? session.projectName + " — Cutline" : "Cutline"
    color: Theme.bg
    font.family: Theme.ui
    font.pixelSize: 12

    // The colours every control inherits, so menus, fields and dialogs match the panels.
    palette.windowText: Theme.text
    palette.text: Theme.text
    palette.buttonText: Theme.text
    palette.window: Theme.panel
    palette.base: Theme.field
    palette.button: Theme.field
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.text
    palette.placeholderText: Theme.faint
    palette.toolTipBase: Theme.panelHeader
    palette.toolTipText: Theme.text
    palette.mid: Theme.border
    palette.dark: Theme.bg

    // ---- one shared place for a menu entry that runs an application command
    component CommandItem: MenuItem {
        property string cmd: ""
        property string label: ""
        text: label + (shortcut !== "" ? "\t" + shortcut : "")
        property string shortcut: ""
        onCmdChanged: shortcut = session.shortcutText(cmd)
        onTriggered: session.trigger(cmd)
    }

    DockController { id: dockController; session: appSession }

    // Keys not taken by a text field arrive here; the keymap decides what they mean.
    Item {
        id: keys
        anchors.fill: parent
        focus: true
        Keys.onPressed: (event) => { if (session.handleKey(event.key, event.modifiers, event.text, true, event.isAutoRepeat)) event.accepted = true }
        Keys.onReleased: (event) => { if (!event.isAutoRepeat && session.handleKey(event.key, event.modifiers, event.text, false, false)) event.accepted = true }
    }

    menuBar: MenuBar {
        background: Rectangle { color: Theme.panelHeader }
        Menu {
            title: qsTr("&File")
            onAboutToShow: { for (let i = 0; i < count; ++i) { const it = itemAt(i); if (it.cmd !== undefined) it.shortcut = session.shortcutText(it.cmd) } }
            CommandItem { cmd: "file.new_project"; label: qsTr("New Project…") }
            CommandItem { cmd: "file.open_project"; label: qsTr("Open Project…") }
            CommandItem { cmd: "file.save"; label: qsTr("Save") }
            MenuSeparator {}
            CommandItem { cmd: "file.import_media"; label: qsTr("Import Media…") }
            CommandItem { cmd: "file.ingest_media"; label: qsTr("Ingest Media (copy and proxy)…") }
            CommandItem { cmd: "file.export_media"; label: qsTr("Export Media…") }
            MenuSeparator {}
            MenuItem { text: qsTr("Open a demo project"); onTriggered: session.newDemoProject() }
            MenuSeparator {}
            CommandItem { cmd: "file.quit"; label: qsTr("Quit") }
        }
        Menu {
            title: qsTr("&Edit")
            onAboutToShow: { for (let i = 0; i < count; ++i) { const it = itemAt(i); if (it.cmd !== undefined) it.shortcut = session.shortcutText(it.cmd) } }
            CommandItem { cmd: "edit.undo"; label: qsTr("Undo"); enabled: session.canUndo }
            CommandItem { cmd: "edit.redo"; label: qsTr("Redo"); enabled: session.canRedo }
            MenuSeparator {}
            CommandItem { cmd: "edit.cut"; label: qsTr("Cut") }
            CommandItem { cmd: "edit.copy"; label: qsTr("Copy") }
            CommandItem { cmd: "edit.paste"; label: qsTr("Paste") }
            CommandItem { cmd: "edit.paste_insert"; label: qsTr("Paste Insert") }
            CommandItem { cmd: "edit.duplicate"; label: qsTr("Duplicate") }
            CommandItem { cmd: "edit.delete"; label: qsTr("Clear") }
            CommandItem { cmd: "edit.ripple_delete"; label: qsTr("Ripple Delete") }
            MenuSeparator {}
            CommandItem { cmd: "edit.select_all"; label: qsTr("Select All") }
            CommandItem { cmd: "edit.deselect_all"; label: qsTr("Deselect All") }
            MenuSeparator {}
            CommandItem { cmd: "edit.command_palette"; label: qsTr("Command Palette…") }
            CommandItem { cmd: "edit.preferences"; label: qsTr("Preferences…") }
        }
        Menu {
            title: qsTr("&Sequence")
            onAboutToShow: { for (let i = 0; i < count; ++i) { const it = itemAt(i); if (it.cmd !== undefined) it.shortcut = session.shortcutText(it.cmd) } }
            CommandItem { cmd: "timeline.add_edit"; label: qsTr("Add Edit") }
            CommandItem { cmd: "timeline.add_edit_all"; label: qsTr("Add Edit to All Tracks") }
            MenuSeparator {}
            CommandItem { cmd: "timeline.insert"; label: qsTr("Insert") }
            CommandItem { cmd: "timeline.overwrite"; label: qsTr("Overwrite") }
            CommandItem { cmd: "timeline.lift"; label: qsTr("Lift") }
            CommandItem { cmd: "timeline.extract"; label: qsTr("Extract") }
            MenuSeparator {}
            CommandItem { cmd: "timeline.link"; label: qsTr("Link / Unlink") }
            CommandItem { cmd: "timeline.enable"; label: qsTr("Enable / Disable Clip") }
            CommandItem { cmd: "timeline.speed"; label: qsTr("Speed / Duration…") }
            CommandItem { cmd: "timeline.speed_ramp"; label: qsTr("Speed Ramp…") }
            MenuItem { text: qsTr("Colour Settings…"); onTriggered: colorDialog.open() }
            MenuSeparator {}
            CommandItem { cmd: "timeline.mark_in"; label: qsTr("Mark In") }
            CommandItem { cmd: "timeline.mark_out"; label: qsTr("Mark Out") }
            CommandItem { cmd: "timeline.clear_in_out"; label: qsTr("Clear In and Out") }
            CommandItem { cmd: "timeline.add_marker"; label: qsTr("Add Marker") }
            MenuSeparator {}
            CommandItem { cmd: "timeline.next_edit"; label: qsTr("Go to Next Edit Point") }
            CommandItem { cmd: "timeline.previous_edit"; label: qsTr("Go to Previous Edit Point") }
            MenuSeparator {}
            CommandItem { cmd: "timeline.add_video_track"; label: qsTr("Add Video Track") }
            CommandItem { cmd: "timeline.add_audio_track"; label: qsTr("Add Audio Track") }
        }
        Menu {
            title: qsTr("&View")
            onAboutToShow: { for (let i = 0; i < count; ++i) { const it = itemAt(i); if (it.cmd !== undefined) it.shortcut = session.shortcutText(it.cmd) } }
            CommandItem { cmd: "timeline.snap"; label: qsTr("Snap"); checkable: true; checked: session.snap }
            CommandItem { cmd: "timeline.zoom_in"; label: qsTr("Zoom In") }
            CommandItem { cmd: "timeline.zoom_out"; label: qsTr("Zoom Out") }
            CommandItem { cmd: "timeline.zoom_fit"; label: qsTr("Zoom to Fit") }
            MenuSeparator {}
            CommandItem { cmd: "monitor.fullscreen"; label: qsTr("Full Screen Monitor") }
            CommandItem { cmd: "monitor.safe_margins"; label: qsTr("Safe Margins"); checkable: true; checked: session.safeMargins }
            Menu {
                title: qsTr("Playback Resolution")
                CommandItem { cmd: "monitor.quality_auto"; label: qsTr("Auto"); checkable: true; checked: session.monitorQuality === "auto" }
                CommandItem { cmd: "monitor.quality_full"; label: qsTr("Full"); checkable: true; checked: session.monitorQuality === "full" }
                CommandItem { cmd: "monitor.quality_half"; label: qsTr("1/2"); checkable: true; checked: session.monitorQuality === "half" }
                CommandItem { cmd: "monitor.quality_quarter"; label: qsTr("1/4"); checkable: true; checked: session.monitorQuality === "quarter" }
            }
            CommandItem { cmd: "monitor.zoom_fit"; label: qsTr("Monitor Zoom: Fit") }
            CommandItem { cmd: "monitor.zoom_100"; label: qsTr("Monitor Zoom: 100%") }
        }
        Menu {
            title: qsTr("&Window")
            Menu {
                title: qsTr("Workspaces")
                Repeater {
                    model: dockController.workspaces
                    delegate: MenuItem {
                        required property string modelData
                        text: modelData
                        checkable: true
                        checked: modelData === dockController.current
                        onTriggered: dockController.switchWorkspace(modelData)
                    }
                }
                MenuSeparator {}
                CommandItem { cmd: "workspace.save_as"; label: qsTr("Save as New Workspace…") }
                CommandItem { cmd: "workspace.reset"; label: qsTr("Reset Current Workspace") }
            }
            MenuSeparator {}
            Repeater {
                model: ["project", "program_monitor", "timeline", "effect_controls", "effects", "history", "jobs", "scopes", "audio_mixer", "markers", "multicam", "exports", "audio_essentials", "transcript", "captions", "graphics", "graphic_designer"]
                delegate: MenuItem {
                    required property string modelData
                    text: dockController.panelTitle(modelData)
                    checkable: true
                    checked: dockController.closedPanels.indexOf(modelData) < 0
                    onTriggered: dockController.togglePanel(modelData)
                }
            }
        }
        Menu {
            title: qsTr("&Help")
            CommandItem { cmd: "help.shortcuts"; label: qsTr("Keyboard Shortcuts…") }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The workspace bar.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            color: Theme.panelHeader
            RowLayout {
                anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad }
                spacing: 4
                Rectangle {
                    width: 22; height: 22; radius: 5
                    color: Theme.accent
                    Text { anchors.centerIn: parent; text: "C"; color: Theme.bg; font.family: Theme.ui; font.pixelSize: 13; font.bold: true }
                }
                Text { text: "CUTLINE"; color: Theme.text; font.family: Theme.ui; font.pixelSize: 12; font.bold: true; font.letterSpacing: 1.5; leftPadding: 2; rightPadding: 12 }
                Repeater {
                    model: dockController.workspaces
                    delegate: Rectangle {
                        id: workspaceTab
                        required property string modelData
                        readonly property bool current: modelData === dockController.current
                        implicitWidth: label.implicitWidth + 24
                        implicitHeight: 30
                        radius: 5
                        color: current ? Theme.accentSoft : (workspaceMouse.containsMouse ? Theme.hover : "transparent")
                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: parent.modelData + (parent.current && dockController.modified ? " •" : "")
                            color: parent.current ? Theme.text : Theme.muted
                            font.family: Theme.ui
                            font.pixelSize: 12
                            font.weight: parent.current ? Font.DemiBold : Font.Normal
                        }
                        Rectangle { visible: parent.current; anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 2; color: Theme.accent }
                        MouseArea { id: workspaceMouse; anchors.fill: parent; hoverEnabled: true; onClicked: dockController.switchWorkspace(parent.modelData) }
                        Behavior on color { ColorAnimation { duration: 90 } }
                    }
                }
                Item { Layout.fillWidth: true }
                Chip { text: qsTr("Save layout"); visible: dockController.modified; onClicked: dockController.saveWorkspace() }
                Chip { text: qsTr("Reset"); visible: dockController.modified; onClicked: dockController.resetWorkspace() }
                Chip { text: "⌘"; ToolTip.text: qsTr("Command palette"); onClicked: session.trigger("edit.command_palette") }
            }
            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: Theme.border }
        }

        DockArea {
            id: dockArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            dock: dockController
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Theme.panelHeader
            Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: Theme.borderSoft }
            RowLayout {
                anchors { fill: parent; leftMargin: Theme.pad; rightMargin: Theme.pad }
                spacing: 8
                Rectangle { width: 6; height: 6; radius: 3; color: session.projectOpen ? Theme.accent : Theme.faint }
                Text { Layout.fillWidth: true; text: session.statusMessage; color: Theme.muted; font.family: Theme.ui; font.pixelSize: 11; elide: Text.ElideRight }
                Text { visible: session.jobs.some(j => j.active); text: qsTr("WORKING"); color: Theme.warning; font.family: Theme.ui; font.pixelSize: 10; font.bold: true; font.letterSpacing: 0.8 }
                Rectangle {
                    visible: session.projectOpen
                    implicitWidth: statusTime.implicitWidth + 16; implicitHeight: 20; radius: 4
                    color: Theme.field; border.color: Theme.borderSoft
                    Text { id: statusTime; anchors.centerIn: parent; text: session.timecode; color: Theme.accent; font.family: Theme.mono; font.pixelSize: 11 }
                }
            }
        }
    }

    // ---- dialogs and windows

    FileDialog {
        id: importDialog
        title: qsTr("Import media")
        fileMode: FileDialog.OpenFiles
        onAccepted: session.importMedia(selectedFiles.map(u => u.toString()))
    }
    FolderDialog {
        id: openDialog
        title: qsTr("Open a project (.cutline folder)")
        onAccepted: session.openProject(selectedFolder.toString())
    }
    FolderDialog {
        id: newFolderDialog
        title: qsTr("Where to create the project")
        onAccepted: { newProjectDialog.folder = selectedFolder.toString(); newProjectDialog.open() }
    }
    Dialog {
        id: newProjectDialog
        property string folder: ""
        title: qsTr("New project")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Ok | Dialog.Cancel
        background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            Text { text: qsTr("Name"); color: Theme.muted; font.pixelSize: 11 }
            TextField { id: projectName; text: qsTr("Untitled"); color: Theme.text; Layout.preferredWidth: 280; background: Rectangle { color: Theme.field; border.color: Theme.border } }
        }
        onAccepted: session.newProject(folder, projectName.text)
    }
    Dialog {
        id: workspaceDialog
        title: qsTr("Save workspace")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Ok | Dialog.Cancel
        background: Rectangle { color: Theme.panel; border.color: Theme.border; radius: 4 }
        TextField { id: workspaceName; placeholderText: qsTr("Name"); color: Theme.text; width: 260; background: Rectangle { color: Theme.field; border.color: Theme.border } }
        onAccepted: if (workspaceName.text !== "") dockController.saveWorkspaceAs(workspaceName.text)
    }
    SpeedDialog { id: speedDialog }
    IngestDialog { id: ingestDialog }
    TransitionDialog { id: transitionDialog }
    SequenceColorDialog { id: colorDialog }
    RampDialog { id: rampDialog }
    PreferencesDialog { id: preferencesDialog }
    ShortcutsDialog { id: shortcutsDialog }
    CommandPalette { id: palette }

    Window {
        id: fullscreen
        visible: false
        flags: Qt.Window
        color: "black"
        visibility: visible ? Window.FullScreen : Window.Hidden
        MonitorItem { anchors.fill: parent; session: appSession }
        Shortcut { sequence: "Escape"; onActivated: fullscreen.visible = false }
        MouseArea { anchors.fill: parent; onDoubleClicked: fullscreen.visible = false }
    }

    Connections {
        target: session
        function onImportRequested() { importDialog.open() }
        function onIngestRequested() { ingestDialog.open() }
        function onTransitionRequested(id) { transitionDialog.show(id) }
        function onDesignerRequested() { dockController.showPanel("graphic_designer") }
        function onOpenRequested() { openChooser.popup() }
        function onSpeedRequested() { speedDialog.open() }
        function onRampRequested() { rampDialog.open() }
        function onPreferencesRequested() { preferencesDialog.open() }
        function onShortcutsRequested() { shortcutsDialog.open() }
        function onCommandPaletteRequested() { palette.open() }
        function onQuitRequested() { Qt.quit() }
        function onExportRequested() { exportDialog.openFresh() }
        function onMonitorAction(action) { if (action === "fullscreen") fullscreen.visible = !fullscreen.visible }
    }
    Connections { target: dockController; function onSaveAsRequested() { workspaceDialog.open() } }

    Menu {
        id: openChooser
        MenuItem { text: qsTr("New project…"); onTriggered: newFolderDialog.open() }
        MenuItem { text: qsTr("Open project…"); onTriggered: openDialog.open() }
        MenuItem { text: qsTr("Demo project"); onTriggered: session.newDemoProject() }
    }
    ExportDialog { id: exportDialog }

    // A first window with no project says what to do.
    Rectangle {
        anchors.centerIn: parent
        width: 360; height: 150
        visible: !session.projectOpen
        color: Theme.panel
        border.color: Theme.border
        radius: 6
        z: 100
        ColumnLayout {
            anchors.centerIn: parent
            spacing: 10
            Text { Layout.alignment: Qt.AlignHCenter; text: qsTr("Start a project"); color: Theme.text; font.pixelSize: 16 }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Chip { text: qsTr("New…"); onClicked: newFolderDialog.open() }
                Chip { text: qsTr("Open…"); onClicked: openDialog.open() }
                Chip { text: qsTr("Demo"); onClicked: session.newDemoProject() }
            }
        }
    }
}

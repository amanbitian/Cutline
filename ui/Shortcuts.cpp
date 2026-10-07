#include "ui/Shortcuts.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace cutline::ui {
namespace {

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string Trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The canonical spelling of a key name, or empty if it is not a key.
std::string CanonicalKey(const std::string& raw) {
  const auto text = Trim(raw);
  if (text.empty()) return {};
  const auto lower = Lower(text);
  if (text.size() == 1 && std::isalnum(static_cast<unsigned char>(text[0]))) {
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(text[0]))));
  }
  if (lower.size() >= 2 && lower[0] == 'f') {
    const auto digits = lower.substr(1);
    if (std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
      const auto number = std::stoi(digits);
      if (number >= 1 && number <= 24) return "F" + std::to_string(number);
    }
  }
  for (const auto& name : KeyNames()) {
    if (Lower(name) == lower) return name;
  }
  return {};
}

}  // namespace

const std::vector<std::string>& KeyNames() {
  static const std::vector<std::string> names{
      "Space", "Left", "Right", "Up", "Down", "Home", "End", "PageUp", "PageDown", "Delete", "Backspace", "Enter",
      "Escape", "Tab", "Insert", "Equal", "Minus", "Comma", "Period", "Slash", "Backslash", "Semicolon", "Quote",
      "BracketLeft", "BracketRight", "Backtick"};
  return names;
}

bool Shortcut::operator<(const Shortcut& other) const {
  return std::tie(ctrl, shift, alt, key) < std::tie(other.ctrl, other.shift, other.alt, other.key);
}

std::optional<Shortcut> ParseShortcut(const std::string& text) {
  Shortcut shortcut;
  std::vector<std::string> parts;
  std::string part;
  std::istringstream stream(text);
  while (std::getline(stream, part, '+')) parts.push_back(part);
  // "Ctrl++" is not a thing: the plus key is spelled "Equal" with Shift, so a trailing plus is malformed.
  if (parts.empty() || (!text.empty() && text.back() == '+')) return std::nullopt;
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    const auto modifier = Lower(Trim(parts[index]));
    if (modifier == "ctrl" || modifier == "control" || modifier == "cmd") shortcut.ctrl = true;
    else if (modifier == "shift") shortcut.shift = true;
    else if (modifier == "alt" || modifier == "option") shortcut.alt = true;
    else return std::nullopt;
  }
  shortcut.key = CanonicalKey(parts.back());
  if (shortcut.key.empty()) return std::nullopt;
  return shortcut;
}

std::string ToString(const Shortcut& shortcut) {
  std::string text;
  if (shortcut.ctrl) text += "Ctrl+";
  if (shortcut.shift) text += "Shift+";
  if (shortcut.alt) text += "Alt+";
  return text + shortcut.key;
}

// ---------------------------------------------------------------- registry ----

void CommandRegistry::Add(AppCommand command) {
  if (command.id.empty()) throw std::invalid_argument("A command needs an id");
  if (Find(command.id) != nullptr) throw std::invalid_argument("The command " + command.id + " is registered twice");
  for (const auto& shortcut : command.defaults) {
    if (CanonicalKey(shortcut.key) != shortcut.key || shortcut.key.empty()) {
      throw std::invalid_argument("The command " + command.id + " has a default that is not a valid chord");
    }
  }
  commands_.push_back(std::move(command));
}

const AppCommand* CommandRegistry::Find(const std::string& id) const {
  for (const auto& command : commands_) {
    if (command.id == id) return &command;
  }
  return nullptr;
}

std::vector<std::string> CommandRegistry::Categories() const {
  std::vector<std::string> categories;
  for (const auto& command : commands_) {
    if (std::find(categories.begin(), categories.end(), command.category) == categories.end()) categories.push_back(command.category);
  }
  return categories;
}

std::vector<const AppCommand*> CommandRegistry::Search(const std::string& query) const {
  std::vector<std::string> words;
  std::istringstream stream(Lower(query));
  std::string word;
  while (stream >> word) words.push_back(word);
  std::vector<std::pair<int, const AppCommand*>> hits;
  for (const auto& command : commands_) {
    const auto label = Lower(command.label);
    const auto haystack = label + " " + Lower(command.id) + " " + Lower(command.description) + " " + Lower(command.category);
    bool all = true;
    for (const auto& w : words) all = all && haystack.find(w) != std::string::npos;
    if (!all) continue;
    int rank = 2;
    if (!words.empty()) {
      if (label.rfind(words.front(), 0) == 0) rank = 0;
      else if (label.find(words.front()) != std::string::npos) rank = 1;
    }
    hits.emplace_back(rank, &command);
  }
  std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<const AppCommand*> found;
  for (const auto& hit : hits) found.push_back(hit.second);
  return found;
}

const CommandRegistry& BuiltInCommands() {
  static const CommandRegistry registry = [] {
    CommandRegistry r;
    const auto key = [](const char* chord) { return *ParseShortcut(chord); };
    const auto add = [&](const char* id, const char* label, const char* category, const char* description,
                         std::vector<const char*> chords = {}) {
      AppCommand command{id, label, category, description, {}};
      for (const auto* chord : chords) command.defaults.push_back(key(chord));
      r.Add(std::move(command));
    };
    // File
    add("file.new_project", "New Project", "File", "Create an empty project.", {"Ctrl+Alt+N"});
    add("file.open_project", "Open Project...", "File", "Open a project package.", {"Ctrl+O"});
    add("file.save", "Save", "File", "Save the project (every edit is already journalled; this takes a snapshot).", {"Ctrl+S"});
    add("file.import_media", "Import Media...", "File", "Bring media files into the project.", {"Ctrl+I"});
    add("file.ingest_media", "Ingest Media...", "File", "Copy media into the project exactly, import it and make proxies.", {"Ctrl+Alt+I"});
    add("file.export_media", "Export Media...", "File", "Render the sequence to a file.", {"Ctrl+M"});
    add("file.quit", "Quit", "File", "Close the application.", {"Ctrl+Q"});
    // Edit
    add("edit.undo", "Undo", "Edit", "Undo the last edit.", {"Ctrl+Z"});
    add("edit.redo", "Redo", "Edit", "Redo the edit that was undone.", {"Ctrl+Shift+Z", "Ctrl+Y"});
    add("edit.select_all", "Select All", "Edit", "Select every clip in the sequence.", {"Ctrl+A"});
    add("edit.deselect_all", "Deselect All", "Edit", "Clear the selection.", {"Ctrl+Shift+A"});
    add("edit.copy", "Copy", "Edit", "Copy the selected clips.", {"Ctrl+C"});
    add("edit.cut", "Cut", "Edit", "Cut the selected clips to the clipboard.", {"Ctrl+X"});
    add("edit.paste", "Paste", "Edit", "Paste clips at the playhead, overwriting.", {"Ctrl+V"});
    add("edit.paste_insert", "Paste Insert", "Edit", "Paste clips at the playhead, pushing later clips along.", {"Ctrl+Shift+V"});
    add("edit.duplicate", "Duplicate", "Edit", "Duplicate the selected clips after themselves.", {"Ctrl+Alt+D"});
    add("edit.delete", "Clear", "Edit", "Delete the selected clips, leaving a gap.", {"Delete"});
    add("edit.ripple_delete", "Ripple Delete", "Edit", "Delete the selected clips and close the gap.", {"Shift+Delete"});
    add("edit.preferences", "Preferences...", "Edit", "Open the preferences.", {"Ctrl+Comma"});
    add("edit.command_palette", "Command Palette...", "Edit", "Search every command by name.", {"Ctrl+Shift+P"});
    // Sequence and editing
    add("timeline.add_edit", "Add Edit", "Timeline", "Cut the clips under the playhead.", {"Ctrl+K"});
    add("timeline.add_edit_all", "Add Edit to All Tracks", "Timeline", "Cut every track at the playhead.", {"Ctrl+Shift+K"});
    add("timeline.insert", "Insert", "Timeline", "Insert the source range at the playhead, pushing later clips along.", {"Comma"});
    add("timeline.overwrite", "Overwrite", "Timeline", "Lay the source range over the timeline at the playhead.", {"Period"});
    add("timeline.lift", "Lift", "Timeline", "Remove the marked range, leaving a gap.", {"Semicolon"});
    add("timeline.extract", "Extract", "Timeline", "Remove the marked range and close the gap.", {"Quote"});
    add("timeline.link", "Link / Unlink", "Timeline", "Link the selected clips, or unlink them if they are linked.", {"Ctrl+L"});
    add("timeline.enable", "Enable / Disable Clip", "Timeline", "Switch the selected clips on or off.", {"Shift+E"});
    add("timeline.speed", "Speed / Duration...", "Timeline", "Change the speed of the selected clips.", {"Ctrl+R"});
    add("multicam.play", "Multicam Play/Stop", "Multicam", "Play or stop the open multicam group.", {"Shift+Space"});
    add("multicam.next_cut", "Next Multicam Cut", "Multicam", "Move to the next cut of the open group.", {"Alt+Down"});
    add("multicam.previous_cut", "Previous Multicam Cut", "Multicam", "Move to the previous cut of the open group.", {"Alt+Up"});
    add("multicam.cut_1", "Cut to Angle 1", "Multicam", "Cut the open multicam group to angle 1 at the playhead, also while playing.", {"1"});
    add("multicam.cut_2", "Cut to Angle 2", "Multicam", "Cut the open multicam group to angle 2 at the playhead, also while playing.", {"2"});
    add("multicam.cut_3", "Cut to Angle 3", "Multicam", "Cut the open multicam group to angle 3 at the playhead, also while playing.", {"3"});
    add("multicam.cut_4", "Cut to Angle 4", "Multicam", "Cut the open multicam group to angle 4 at the playhead, also while playing.", {"4"});
    add("multicam.cut_5", "Cut to Angle 5", "Multicam", "Cut the open multicam group to angle 5 at the playhead, also while playing.", {"5"});
    add("multicam.cut_6", "Cut to Angle 6", "Multicam", "Cut the open multicam group to angle 6 at the playhead, also while playing.", {"6"});
    add("multicam.cut_7", "Cut to Angle 7", "Multicam", "Cut the open multicam group to angle 7 at the playhead, also while playing.", {"7"});
    add("multicam.cut_8", "Cut to Angle 8", "Multicam", "Cut the open multicam group to angle 8 at the playhead, also while playing.", {"8"});
    add("multicam.cut_9", "Cut to Angle 9", "Multicam", "Cut the open multicam group to angle 9 at the playhead, also while playing.", {"9"});
    add("timeline.speed_ramp", "Speed Ramp...", "Timeline", "Edit the selected clip speed as a graph: ramps, freezes and reverses.", {"Ctrl+Alt+R"});
    add("timeline.next_edit", "Go to Next Edit Point", "Timeline", "Move the playhead to the next cut.", {"Down"});
    add("timeline.previous_edit", "Go to Previous Edit Point", "Timeline", "Move the playhead to the previous cut.", {"Up"});
    add("timeline.mark_in", "Mark In", "Timeline", "Set the in point at the playhead.", {"I"});
    add("timeline.mark_out", "Mark Out", "Timeline", "Set the out point at the playhead.", {"O"});
    add("timeline.clear_in", "Clear In", "Timeline", "Remove the in point.", {"Ctrl+Shift+I"});
    add("timeline.clear_out", "Clear Out", "Timeline", "Remove the out point.", {"Ctrl+Shift+O"});
    add("timeline.clear_in_out", "Clear In and Out", "Timeline", "Remove both marks.", {"Ctrl+Shift+X"});
    add("timeline.go_in", "Go to In", "Timeline", "Move the playhead to the in point.", {"Shift+I"});
    add("timeline.go_out", "Go to Out", "Timeline", "Move the playhead to the out point.", {"Shift+O"});
    add("timeline.add_marker", "Add Marker", "Timeline", "Put a marker at the playhead.", {"M"});
    add("timeline.snap", "Snap", "Timeline", "Switch snapping to edges and the playhead on or off.", {"S"});
    add("timeline.zoom_in", "Zoom In", "Timeline", "Zoom the timeline in.", {"Equal"});
    add("timeline.zoom_out", "Zoom Out", "Timeline", "Zoom the timeline out.", {"Minus"});
    add("timeline.zoom_fit", "Zoom to Fit", "Timeline", "Fit the whole sequence in the window.", {"Backslash"});
    add("timeline.add_video_track", "Add Video Track", "Timeline", "Add a video track above the others.");
    add("timeline.add_audio_track", "Add Audio Track", "Timeline", "Add an audio track below the others.");
    // Tools
    add("tool.selection", "Selection Tool", "Tools", "Select, move and trim clips.", {"V"});
    add("tool.track_select", "Track Select Forward", "Tools", "Select everything from the click onward on a track.", {"A"});
    add("tool.ripple", "Ripple Edit Tool", "Tools", "Trim a clip and move everything after it.", {"B"});
    add("tool.roll", "Rolling Edit Tool", "Tools", "Move a cut without changing the length.", {"N"});
    add("tool.razor", "Razor Tool", "Tools", "Cut a clip where you click.", {"C"});
    add("tool.slip", "Slip Tool", "Tools", "Change which part of the media a clip shows.", {"Y"});
    add("tool.slide", "Slide Tool", "Tools", "Move a clip between its neighbours.", {"U"});
    add("tool.hand", "Hand Tool", "Tools", "Scroll the timeline.", {"H"});
    add("tool.zoom", "Zoom Tool", "Tools", "Click to zoom the timeline.", {"Z"});
    // Transport
    add("transport.play_pause", "Play / Pause", "Transport", "Start or stop playback.", {"Space"});
    add("transport.play_reverse", "Shuttle Left", "Transport", "Play backward, faster each press.", {"J"});
    add("transport.stop", "Shuttle Stop", "Transport", "Stop playback.", {"K"});
    add("transport.play_forward", "Shuttle Right", "Transport", "Play forward, faster each press.", {"L"});
    add("transport.step_back", "Step Back One Frame", "Transport", "Move the playhead one frame earlier.", {"Left"});
    add("transport.step_forward", "Step Forward One Frame", "Transport", "Move the playhead one frame later.", {"Right"});
    add("transport.step_back_many", "Step Back Five Frames", "Transport", "Move the playhead five frames earlier.", {"Shift+Left"});
    add("transport.step_forward_many", "Step Forward Five Frames", "Transport", "Move the playhead five frames later.", {"Shift+Right"});
    add("transport.go_start", "Go to Start", "Transport", "Move the playhead to the start of the sequence.", {"Home"});
    add("transport.go_end", "Go to End", "Transport", "Move the playhead to the end of the sequence.", {"End"});
    add("transport.loop", "Loop", "Transport", "Loop playback over the marked range.", {"Ctrl+Shift+L"});
    // Monitor and view
    add("monitor.fullscreen", "Full Screen Monitor", "View", "Show the program monitor full screen.", {"Ctrl+Backtick"});
    add("monitor.safe_margins", "Safe Margins", "View", "Show the title-safe and action-safe areas.");
    add("monitor.quality_full", "Playback Resolution: Full", "View", "Render the monitor at full size.");
    add("monitor.quality_half", "Playback Resolution: 1/2", "View", "Render the monitor at half size.");
    add("monitor.quality_quarter", "Playback Resolution: 1/4", "View", "Render the monitor at quarter size.");
    add("monitor.quality_auto", "Playback Resolution: Auto", "View", "Render only as large as the monitor shows.");
    add("monitor.zoom_fit", "Monitor Zoom: Fit", "View", "Fit the picture in the monitor.");
    add("monitor.zoom_100", "Monitor Zoom: 100%", "View", "Show the picture at its own pixel size.");
    // Workspaces and windows
    add("workspace.editing", "Workspace: Editing", "Window", "Arrange the panels for editing.", {"Alt+Shift+1"});
    add("workspace.assembly", "Workspace: Assembly", "Window", "Arrange the panels for assembling a rough cut.", {"Alt+Shift+2"});
    add("workspace.color", "Workspace: Color", "Window", "Arrange the panels for grading.", {"Alt+Shift+3"});
    add("workspace.audio", "Workspace: Audio", "Window", "Arrange the panels for mixing.", {"Alt+Shift+4"});
    add("workspace.effects", "Workspace: Effects", "Window", "Arrange the panels for effects work.", {"Alt+Shift+5"});
    add("workspace.multicam", "Workspace: Multicam", "Window", "Arrange the panels for cutting between camera angles.", {"Alt+Shift+6"});
    add("workspace.text", "Workspace: Text", "Window", "Arrange the panels for editing by the words that are said.", {"Alt+Shift+7"});
    add("workspace.graphics", "Workspace: Graphics", "Window", "Arrange the panels for designing titles and graphics.", {"Alt+Shift+8"});
    add("workspace.reset", "Reset Workspace", "Window", "Put the current workspace back as it was.");
    add("workspace.save_as", "Save as New Workspace...", "Window", "Keep the current arrangement under a name.");
    add("panel.project", "Project Panel", "Window", "Show or hide the project panel.");
    add("panel.program_monitor", "Program Monitor", "Window", "Show or hide the program monitor.");
    add("panel.timeline", "Timeline", "Window", "Show or hide the timeline.");
    add("panel.effect_controls", "Effect Controls", "Window", "Show or hide the effect controls.", {"Shift+5"});
    add("panel.effects", "Effects", "Window", "Show or hide the effects browser.", {"Shift+7"});
    add("panel.history", "History", "Window", "Show or hide the undo history.");
    add("panel.audio_essentials", "Essential Sound", "Window", "Show or hide the audio roles and loudness panel.");
    add("panel.transcript", "Transcript", "Window", "Show or hide the transcript panel.");
    add("panel.captions", "Captions", "Window", "Show or hide the caption authoring panel.");
    add("panel.graphics", "Titles", "Window", "Show or hide the library of titles and graphics and the controls of the title chosen on the timeline.");
    add("panel.graphic_designer", "Graphic Designer", "Window", "Show or hide the canvas where titles and graphics are drawn.");
    add("panel.jobs", "Background Jobs", "Window", "Show or hide analysis, proxy and export jobs.");
    // Help
    add("help.shortcuts", "Keyboard Shortcuts...", "Help", "Show and change the keyboard shortcuts.", {"Ctrl+Alt+K"});
    return r;
  }();
  return registry;
}

// ------------------------------------------------------------------ keymap ----

Keymap::Keymap(const CommandRegistry& registry) : registry_(&registry) { ResetAll(); }

void Keymap::ResetAll() {
  bound_.clear();
  owner_.clear();
  for (const auto& command : registry_->All()) {
    for (const auto& shortcut : command.defaults) {
      // A default shared by two commands is a registry mistake; the first keeps it.
      if (owner_.count(shortcut) != 0) continue;
      owner_[shortcut] = command.id;
      bound_[command.id].push_back(shortcut);
    }
  }
}

std::vector<std::string> Keymap::Bind(const std::string& command_id, const Shortcut& shortcut) {
  if (registry_->Find(command_id) == nullptr) throw std::invalid_argument("Unknown command: " + command_id);
  if (shortcut.key.empty()) throw std::invalid_argument("A shortcut needs a key");
  std::vector<std::string> displaced;
  if (const auto found = owner_.find(shortcut); found != owner_.end()) {
    if (found->second == command_id) return displaced;
    displaced.push_back(found->second);
    Unbind(found->second, shortcut);
  }
  owner_[shortcut] = command_id;
  bound_[command_id].push_back(shortcut);
  return displaced;
}

void Keymap::Unbind(const std::string& command_id, const Shortcut& shortcut) {
  auto& list = bound_[command_id];
  list.erase(std::remove(list.begin(), list.end(), shortcut), list.end());
  if (const auto found = owner_.find(shortcut); found != owner_.end() && found->second == command_id) owner_.erase(found);
}

void Keymap::Clear(const std::string& command_id) {
  const auto list = bound_[command_id];
  for (const auto& shortcut : list) Unbind(command_id, shortcut);
}

void Keymap::Reset(const std::string& command_id) {
  const auto* command = registry_->Find(command_id);
  if (command == nullptr) throw std::invalid_argument("Unknown command: " + command_id);
  Clear(command_id);
  for (const auto& shortcut : command->defaults) (void)Bind(command_id, shortcut);
}

std::optional<std::string> Keymap::Lookup(const Shortcut& shortcut) const {
  const auto found = owner_.find(shortcut);
  if (found == owner_.end()) return std::nullopt;
  return found->second;
}

std::vector<Shortcut> Keymap::ShortcutsFor(const std::string& command_id) const {
  const auto found = bound_.find(command_id);
  return found == bound_.end() ? std::vector<Shortcut>{} : found->second;
}

bool Keymap::IsDefault() const {
  for (const auto& command : registry_->All()) {
    auto have = ShortcutsFor(command.id);
    auto want = command.defaults;
    std::sort(have.begin(), have.end());
    std::sort(want.begin(), want.end());
    if (!(have == want)) return false;
  }
  return true;
}

std::string Keymap::ToJson() const {
  std::vector<std::string> entries;
  for (const auto& command : registry_->All()) {
    auto have = ShortcutsFor(command.id);
    auto want = command.defaults;
    std::sort(have.begin(), have.end());
    std::sort(want.begin(), want.end());
    if (have == want) continue;
    std::vector<std::string> chords;
    for (const auto& shortcut : ShortcutsFor(command.id)) chords.push_back("\"" + json::Escape(ToString(shortcut)) + "\"");
    entries.push_back(json::Object().Add("command", command.id).AddRaw("shortcuts", json::Array(chords)).Build());
  }
  return json::Object().Add("version", static_cast<std::int64_t>(1)).AddRaw("changes", json::Array(entries)).Build();
}

void Keymap::FromJson(const std::string& text, std::vector<std::string>* warnings) {
  const auto note = [&](const std::string& message) {
    if (warnings != nullptr) warnings->push_back(message);
  };
  ResetAll();
  const auto root = json::Parse(text);
  const auto* changes = root.Find("changes");
  if (changes == nullptr || !changes->is_array()) {
    note("The saved keymap has no list of changes");
    return;
  }
  // First clear every command the file mentions, so one command's new chord is not taken by another's
  // old default before that command's own entry has been read.
  for (const auto& change : changes->items) {
    const auto* id = change.Find("command");
    if (id == nullptr || !id->is_string()) continue;
    if (registry_->Find(id->text) != nullptr) Clear(id->text);
  }
  for (const auto& change : changes->items) {
    const auto* id = change.Find("command");
    const auto* chords = change.Find("shortcuts");
    if (id == nullptr || !id->is_string() || chords == nullptr || !chords->is_array()) {
      note("A saved keymap entry is malformed and was skipped");
      continue;
    }
    if (registry_->Find(id->text) == nullptr) {
      note("The saved keymap names the command " + id->text + ", which this build does not have");
      continue;
    }
    for (const auto& chord : chords->items) {
      const auto parsed = chord.is_string() ? ParseShortcut(chord.text) : std::nullopt;
      if (!parsed) {
        note("The saved shortcut " + (chord.is_string() ? chord.text : std::string("(not text)")) + " for " + id->text + " is not a valid chord");
        continue;
      }
      (void)Bind(id->text, *parsed);
    }
  }
}

}  // namespace cutline::ui

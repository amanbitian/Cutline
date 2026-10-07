#pragma once

// Application commands and their keyboard shortcuts.
//
// A command is something a person can ask the editor to do (split at the playhead, play, undo), named by
// a stable id. Menus, the command palette, toolbar buttons and shortcuts all refer to commands by id, so
// binding a key is only ever a mapping from a key chord to an id. The registry says which commands exist
// and what they are called; the keymap says which chords run them, starting from the defaults and keeping
// only the user's differences when saved.
//
// A chord runs at most one command: binding a chord that another command has takes it from that command
// and says so, rather than leaving two commands fighting over a key.

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

struct Shortcut final {
  bool ctrl{false};
  bool shift{false};
  bool alt{false};
  // A letter or digit (upper case), F1..F24, or one of the names KeyNames() lists.
  std::string key;

  [[nodiscard]] bool operator==(const Shortcut& other) const {
    return ctrl == other.ctrl && shift == other.shift && alt == other.alt && key == other.key;
  }
  [[nodiscard]] bool operator<(const Shortcut& other) const;
};

// "Ctrl+Shift+Z" in any case, modifiers in any order. Nothing when the text is not a valid chord.
[[nodiscard]] std::optional<Shortcut> ParseShortcut(const std::string& text);
// The canonical text: modifiers in the order Ctrl, Shift, Alt, then the key.
[[nodiscard]] std::string ToString(const Shortcut& shortcut);
// The names of the non-alphanumeric keys that can be bound.
[[nodiscard]] const std::vector<std::string>& KeyNames();

struct AppCommand final {
  std::string id;
  std::string label;
  std::string category;
  std::string description;
  std::vector<Shortcut> defaults;
};

class CommandRegistry final {
 public:
  // Throws std::invalid_argument for an empty or repeated id, or a default that is not a valid chord.
  void Add(AppCommand command);
  [[nodiscard]] const AppCommand* Find(const std::string& id) const;
  [[nodiscard]] const std::vector<AppCommand>& All() const { return commands_; }
  // Categories in the order they first appear.
  [[nodiscard]] std::vector<std::string> Categories() const;
  // Commands whose label, id or description contains every word of the query (case-insensitive), best
  // matches (label starts with the first word) first. The command palette and the shortcut editor use it.
  [[nodiscard]] std::vector<const AppCommand*> Search(const std::string& query) const;

 private:
  std::vector<AppCommand> commands_;
};

// Every command the editor has, with Premiere-style default shortcuts.
[[nodiscard]] const CommandRegistry& BuiltInCommands();

class Keymap final {
 public:
  explicit Keymap(const CommandRegistry& registry);

  // Gives `shortcut` to `command_id`. If another command had it, that command loses it and is returned.
  // Throws std::invalid_argument for an unknown command.
  std::vector<std::string> Bind(const std::string& command_id, const Shortcut& shortcut);
  void Unbind(const std::string& command_id, const Shortcut& shortcut);
  // Removes every shortcut of the command.
  void Clear(const std::string& command_id);
  void Reset(const std::string& command_id);
  void ResetAll();

  [[nodiscard]] std::optional<std::string> Lookup(const Shortcut& shortcut) const;
  [[nodiscard]] std::vector<Shortcut> ShortcutsFor(const std::string& command_id) const;
  [[nodiscard]] bool IsDefault() const;

  // Only what differs from the defaults is written, so a newer build's new defaults still reach a user who
  // never touched them.
  [[nodiscard]] std::string ToJson() const;
  // Applies saved differences over the defaults. Commands this build does not have and chords that do not
  // parse are skipped and described in `warnings`.
  void FromJson(const std::string& json, std::vector<std::string>* warnings = nullptr);

 private:
  const CommandRegistry* registry_;
  std::map<std::string, std::vector<Shortcut>> bound_;
  std::map<Shortcut, std::string> owner_;
};

}  // namespace cutline::ui

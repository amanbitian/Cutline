#pragma once

// Application preferences: a declared set of typed settings with defaults and limits, validated on every
// change, observable, and saved as JSON.
//
// Declaring a preference in one place (key, kind, default, limits, the words a settings page shows) means a
// settings page, the validation of a loaded file and the code that reads the value cannot disagree about what
// a preference is. A value that does not satisfy its declaration is refused when set and replaced by its
// default, with a warning, when loaded.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace cutline::ui {

using PreferenceValue = std::variant<bool, std::int64_t, double, std::string>;

enum class PreferenceKind { Bool, Integer, Real, Text, Choice };

struct PreferenceDef final {
  std::string key;  // "playback.default_quality"
  PreferenceKind kind{PreferenceKind::Bool};
  PreferenceValue default_value{false};
  std::string category;
  std::string label;
  std::string description;
  // Integer and Real limits; ignored for other kinds.
  double minimum{0.0};
  double maximum{0.0};
  // Choice: the values allowed.
  std::vector<std::string> choices;
  // A change only takes effect the next time the application starts.
  bool needs_restart{false};
};

class PreferenceSchema final {
 public:
  // Throws std::invalid_argument for a repeated key or a default that does not satisfy its own declaration.
  void Add(PreferenceDef def);
  [[nodiscard]] const PreferenceDef* Find(const std::string& key) const;
  [[nodiscard]] const std::vector<PreferenceDef>& All() const { return defs_; }
  [[nodiscard]] std::vector<std::string> Categories() const;
  // Empty when the value is acceptable for the preference, otherwise what is wrong with it.
  [[nodiscard]] std::string Check(const PreferenceDef& def, const PreferenceValue& value) const;

 private:
  std::vector<PreferenceDef> defs_;
};

[[nodiscard]] const PreferenceSchema& BuiltInPreferences();

class Preferences final {
 public:
  explicit Preferences(const PreferenceSchema& schema = BuiltInPreferences());

  [[nodiscard]] const PreferenceSchema& schema() const { return *schema_; }
  [[nodiscard]] const PreferenceValue& Get(const std::string& key) const;
  [[nodiscard]] bool GetBool(const std::string& key) const;
  [[nodiscard]] std::int64_t GetInt(const std::string& key) const;
  [[nodiscard]] double GetReal(const std::string& key) const;
  [[nodiscard]] const std::string& GetText(const std::string& key) const;

  // Throws std::invalid_argument for an unknown key or a value the declaration does not allow; nothing changes.
  void Set(const std::string& key, PreferenceValue value);
  void Reset(const std::string& key);
  void ResetAll();
  [[nodiscard]] bool IsDefault(const std::string& key) const;

  // Called after a value changes, with its key. Returns an id for Unobserve.
  using Observer = std::function<void(const std::string& key, const PreferenceValue& value)>;
  int Observe(Observer observer);
  void Unobserve(int id);

  // Only values that differ from the defaults are written.
  [[nodiscard]] std::string ToJson() const;
  // Keys this build does not know and values that are not acceptable are skipped, described in `warnings`.
  void FromJson(const std::string& json, std::vector<std::string>* warnings = nullptr);
  // Atomic: written beside the file and renamed over it. Load returns false when the file does not exist.
  void Save(const std::string& path) const;
  bool Load(const std::string& path, std::vector<std::string>* warnings = nullptr);

 private:
  const PreferenceSchema* schema_;
  std::map<std::string, PreferenceValue> values_;
  std::map<int, Observer> observers_;
  int next_observer_{1};
};

}  // namespace cutline::ui

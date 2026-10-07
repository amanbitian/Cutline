#include "ui/Preferences.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cutline::ui {
namespace {

bool KindMatches(PreferenceKind kind, const PreferenceValue& value) {
  switch (kind) {
    case PreferenceKind::Bool: return std::holds_alternative<bool>(value);
    case PreferenceKind::Integer: return std::holds_alternative<std::int64_t>(value);
    case PreferenceKind::Real: return std::holds_alternative<double>(value);
    case PreferenceKind::Text:
    case PreferenceKind::Choice: return std::holds_alternative<std::string>(value);
  }
  return false;
}

std::string ValueJson(const PreferenceValue& value) {
  if (const auto* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
  if (const auto* i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
  if (const auto* d = std::get_if<double>(&value)) return json::Number(*d);
  return "\"" + json::Escape(std::get<std::string>(value)) + "\"";
}

}  // namespace

void PreferenceSchema::Add(PreferenceDef def) {
  if (def.key.empty()) throw std::invalid_argument("A preference needs a key");
  if (Find(def.key) != nullptr) throw std::invalid_argument("The preference " + def.key + " is declared twice");
  if (const auto problem = Check(def, def.default_value); !problem.empty()) {
    throw std::invalid_argument("The default of " + def.key + " is not acceptable: " + problem);
  }
  defs_.push_back(std::move(def));
}

const PreferenceDef* PreferenceSchema::Find(const std::string& key) const {
  for (const auto& def : defs_) {
    if (def.key == key) return &def;
  }
  return nullptr;
}

std::vector<std::string> PreferenceSchema::Categories() const {
  std::vector<std::string> categories;
  for (const auto& def : defs_) {
    if (std::find(categories.begin(), categories.end(), def.category) == categories.end()) categories.push_back(def.category);
  }
  return categories;
}

std::string PreferenceSchema::Check(const PreferenceDef& def, const PreferenceValue& value) const {
  if (!KindMatches(def.kind, value)) return "it is the wrong kind of value";
  switch (def.kind) {
    case PreferenceKind::Integer: {
      const auto v = std::get<std::int64_t>(value);
      if (static_cast<double>(v) < def.minimum || static_cast<double>(v) > def.maximum) {
        return "it must be between " + std::to_string(static_cast<std::int64_t>(def.minimum)) + " and " + std::to_string(static_cast<std::int64_t>(def.maximum));
      }
      break;
    }
    case PreferenceKind::Real: {
      const auto v = std::get<double>(value);
      if (!std::isfinite(v) || v < def.minimum || v > def.maximum) return "it must be between " + json::Number(def.minimum) + " and " + json::Number(def.maximum);
      break;
    }
    case PreferenceKind::Choice: {
      const auto& v = std::get<std::string>(value);
      if (std::find(def.choices.begin(), def.choices.end(), v) == def.choices.end()) return "it must be one of the listed choices";
      break;
    }
    default: break;
  }
  return {};
}

const PreferenceSchema& BuiltInPreferences() {
  static const PreferenceSchema schema = [] {
    PreferenceSchema s;
    const auto boolean = [&](const char* key, bool value, const char* category, const char* label, const char* description, bool restart = false) {
      PreferenceDef d;
      d.key = key; d.kind = PreferenceKind::Bool; d.default_value = value; d.category = category; d.label = label; d.description = description; d.needs_restart = restart;
      s.Add(std::move(d));
    };
    const auto integer = [&](const char* key, std::int64_t value, std::int64_t lo, std::int64_t hi, const char* category, const char* label, const char* description, bool restart = false) {
      PreferenceDef d;
      d.key = key; d.kind = PreferenceKind::Integer; d.default_value = value; d.minimum = static_cast<double>(lo); d.maximum = static_cast<double>(hi);
      d.category = category; d.label = label; d.description = description; d.needs_restart = restart;
      s.Add(std::move(d));
    };
    const auto real = [&](const char* key, double value, double lo, double hi, const char* category, const char* label, const char* description, bool restart = false) {
      PreferenceDef d;
      d.key = key; d.kind = PreferenceKind::Real; d.default_value = value; d.minimum = lo; d.maximum = hi;
      d.category = category; d.label = label; d.description = description; d.needs_restart = restart;
      s.Add(std::move(d));
    };
    const auto choice = [&](const char* key, const char* value, std::vector<std::string> choices, const char* category, const char* label, const char* description, bool restart = false) {
      PreferenceDef d;
      d.key = key; d.kind = PreferenceKind::Choice; d.default_value = std::string(value); d.choices = std::move(choices);
      d.category = category; d.label = label; d.description = description; d.needs_restart = restart;
      s.Add(std::move(d));
    };
    const auto text = [&](const char* key, const char* value, const char* category, const char* label, const char* description) {
      PreferenceDef d;
      d.key = key; d.kind = PreferenceKind::Text; d.default_value = std::string(value); d.category = category; d.label = label; d.description = description;
      s.Add(std::move(d));
    };
    // General
    integer("general.autosave_minutes", 5, 1, 60, "General", "Snapshot every (minutes)", "How often a recovery snapshot of the project is taken while it changes.");
    boolean("general.restore_last_project", true, "General", "Reopen the last project at start", "Open the project that was open when the application last closed.");
    boolean("general.confirm_quit", true, "General", "Confirm before quitting", "Ask before closing while a background job is running.");
    // Appearance
    choice("appearance.theme", "dark", {"dark", "light"}, "Appearance", "Theme", "The colour scheme of the interface.");
    real("appearance.ui_scale", 1.0, 0.75, 2.0, "Appearance", "Interface scale", "Scales text and controls.", true);
    // Playback
    choice("playback.default_quality", "auto", {"full", "half", "quarter", "auto"}, "Playback", "Playback resolution", "How large the monitor renders while playing.");
    boolean("playback.loop", false, "Playback", "Loop playback", "Return to the start (or the in point) when playback reaches the end.");
    real("playback.pre_roll_seconds", 2.0, 0.0, 10.0, "Playback", "Pre-roll (seconds)", "How far before the playhead a preview-around-the-cut starts.");
    real("playback.post_roll_seconds", 2.0, 0.0, 10.0, "Playback", "Post-roll (seconds)", "How far after the playhead a preview-around-the-cut ends.");
    integer("playback.read_ahead_frames", 8, 0, 64, "Playback", "Read-ahead frames", "How many frames are decoded ahead of the playhead.", true);
    boolean("playback.use_gpu", true, "Playback", "Compose on the GPU", "Draw the monitor picture on the graphics card where it can match the software renderer exactly (exports always use the software renderer). Applies when a project is opened.", true);
    // Transcription
    text("speech.whisper_path", "", "Transcription", "Speech engine (whisper-cli)", "The whisper.cpp command-line tool. Empty looks for a \"whisper\" folder beside the application.");
    text("speech.model_path", "", "Transcription", "Speech model (ggml-*.bin)", "The model file the engine listens with. Empty uses the one beside the engine.");
    text("speech.language", "auto", "Transcription", "Spoken language", "A language code such as en or de, or auto to let the model decide (a model trained on English only has no choice).");
    integer("speech.threads", 0, 0, 64, "Transcription", "Engine threads", "How many threads the engine uses; 0 lets it choose.");
    real("speech.cut_margin", 0.0, 0.0, 0.5, "Transcription", "Time left on at each cut (seconds)", "When words are deleted, this much of each end of the removed run is kept, because word times from the engine are good to about a tenth of a second.");
    boolean("playback.hardware_decode", true, "Playback", "Decode video on the GPU", "Use the graphics card video decoder where it supports the file, and keep the picture on the card. Applies when a project is opened.", true);
    integer("playback.frame_cache_mb", 256, 64, 8192, "Playback", "Decoded-frame cache (MB)", "Memory kept for decoded pictures.", true);
    // Monitor
    boolean("monitor.show_safe_margins", false, "Program Monitor", "Show safe margins", "Draw the action-safe and title-safe areas over the picture.");
    real("monitor.action_safe_percent", 93.0, 50.0, 100.0, "Program Monitor", "Action safe (%)", "The share of the picture treated as safe for action.");
    real("monitor.title_safe_percent", 90.0, 50.0, 100.0, "Program Monitor", "Title safe (%)", "The share of the picture treated as safe for titles.");
    boolean("monitor.show_timecode", true, "Program Monitor", "Show timecode", "Draw the playhead timecode in the monitor.");
    choice("monitor.background", "checker", {"black", "grey", "checker"}, "Program Monitor", "Transparency background", "What shows behind a picture that has transparency.");
    // Timeline
    boolean("timeline.snap", true, "Timeline", "Snap", "Snap clips and the playhead to edges, markers and each other.");
    integer("timeline.snap_distance_px", 8, 1, 30, "Timeline", "Snap distance (pixels)", "How close a drag has to come before it snaps.");
    integer("timeline.video_track_height", 56, 24, 200, "Timeline", "Video track height", "Pixels.");
    integer("timeline.audio_track_height", 40, 24, 200, "Timeline", "Audio track height", "Pixels.");
    boolean("timeline.show_clip_names", true, "Timeline", "Show clip names", "Write each clip's name on it.");
    boolean("timeline.linked_selection", true, "Timeline", "Linked selection", "Selecting a clip selects the clips linked to it.");
    boolean("timeline.ripple_all_tracks", true, "Timeline", "Ripple edits affect all tracks", "A ripple trim shifts later clips on every unlocked track, not only its own.");
    boolean("timeline.scroll_follows_playhead", true, "Timeline", "Scroll with playhead", "Keep the playhead in view while playing.");
    real("timeline.default_transition_seconds", 1.0, 0.04, 10.0, "Timeline", "Default transition (seconds)", "The length of a transition applied without a length.");
    real("timeline.default_still_seconds", 5.0, 0.04, 600.0, "Timeline", "Default still length (seconds)", "The length of a still image or title placed on the timeline.");
    // Media
    boolean("media.use_proxies", false, "Media", "Use proxies in the monitor", "Play proxies where they exist; exports always use the originals.");
    text("media.cache_folder", "", "Media", "Cache folder", "Where optical-flow and render caches are kept; empty means beside the project.");
    return s;
  }();
  return schema;
}

// ------------------------------------------------------------------- values ----

Preferences::Preferences(const PreferenceSchema& schema) : schema_(&schema) { ResetAll(); }

const PreferenceValue& Preferences::Get(const std::string& key) const {
  const auto found = values_.find(key);
  if (found == values_.end()) throw std::invalid_argument("Unknown preference: " + key);
  return found->second;
}

bool Preferences::GetBool(const std::string& key) const { return std::get<bool>(Get(key)); }
std::int64_t Preferences::GetInt(const std::string& key) const { return std::get<std::int64_t>(Get(key)); }
double Preferences::GetReal(const std::string& key) const { return std::get<double>(Get(key)); }
const std::string& Preferences::GetText(const std::string& key) const { return std::get<std::string>(Get(key)); }

void Preferences::Set(const std::string& key, PreferenceValue value) {
  const auto* def = schema_->Find(key);
  if (def == nullptr) throw std::invalid_argument("Unknown preference: " + key);
  // A whole number given for a real preference is a real number.
  if (def->kind == PreferenceKind::Real && std::holds_alternative<std::int64_t>(value)) value = static_cast<double>(std::get<std::int64_t>(value));
  if (const auto problem = schema_->Check(*def, value); !problem.empty()) {
    throw std::invalid_argument("The preference " + key + " cannot be set: " + problem);
  }
  if (values_[key] == value) return;
  values_[key] = value;
  for (const auto& [id, observer] : observers_) observer(key, value);
}

void Preferences::Reset(const std::string& key) {
  const auto* def = schema_->Find(key);
  if (def == nullptr) throw std::invalid_argument("Unknown preference: " + key);
  Set(key, def->default_value);
}

void Preferences::ResetAll() {
  for (const auto& def : schema_->All()) {
    const auto before = values_.find(def.key);
    const bool changed = before != values_.end() && !(before->second == def.default_value);
    values_[def.key] = def.default_value;
    if (changed) {
      for (const auto& [id, observer] : observers_) observer(def.key, def.default_value);
    }
  }
}

bool Preferences::IsDefault(const std::string& key) const {
  const auto* def = schema_->Find(key);
  return def != nullptr && Get(key) == def->default_value;
}

int Preferences::Observe(Observer observer) {
  const auto id = next_observer_++;
  observers_[id] = std::move(observer);
  return id;
}

void Preferences::Unobserve(int id) { observers_.erase(id); }

std::string Preferences::ToJson() const {
  auto values = json::Object();
  for (const auto& def : schema_->All()) {
    if (IsDefault(def.key)) continue;
    values.AddRaw(def.key, ValueJson(Get(def.key)));
  }
  return json::Object().Add("version", static_cast<std::int64_t>(1)).AddRaw("values", values.Build()).Build();
}

void Preferences::FromJson(const std::string& text, std::vector<std::string>* warnings) {
  const auto note = [&](const std::string& message) {
    if (warnings != nullptr) warnings->push_back(message);
  };
  const auto root = json::Parse(text);
  ResetAll();
  const auto* values = root.Find("values");
  if (values == nullptr || !values->is_object()) {
    note("The saved preferences have no values");
    return;
  }
  for (const auto& [key, value] : values->members) {
    const auto* def = schema_->Find(key);
    if (def == nullptr) {
      note("The saved preference " + key + " is not one this build has");
      continue;
    }
    PreferenceValue parsed = false;
    switch (value.kind) {
      case json::Value::Kind::Bool: parsed = value.boolean; break;
      case json::Value::Kind::String: parsed = value.text; break;
      case json::Value::Kind::Number:
        if (def->kind == PreferenceKind::Integer) {
          if (value.number != std::floor(value.number)) {
            note("The saved preference " + key + " is not a whole number; its default is used");
            continue;
          }
          parsed = static_cast<std::int64_t>(value.number);
        } else {
          parsed = value.number;
        }
        break;
      default:
        note("The saved preference " + key + " has a value of no usable kind; its default is used");
        continue;
    }
    if (const auto problem = schema_->Check(*def, parsed); !problem.empty()) {
      note("The saved preference " + key + " is not acceptable (" + problem + "); its default is used");
      continue;
    }
    values_[key] = parsed;
  }
}

void Preferences::Save(const std::string& path) const {
  const std::filesystem::path target(path);
  if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
  auto temporary = target;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Could not write the preferences to " + path);
    out << ToJson();
    if (!out) throw std::runtime_error("Could not write the preferences to " + path);
  }
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(target, error);
    std::filesystem::rename(temporary, target);
  }
}

bool Preferences::Load(const std::string& path, std::vector<std::string>* warnings) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream content;
  content << in.rdbuf();
  try {
    FromJson(content.str(), warnings);
  } catch (const std::exception& error) {
    // A damaged file must not stop the application from starting: the defaults stand, and it is said.
    ResetAll();
    if (warnings != nullptr) warnings->push_back(std::string("The preferences file could not be read and the defaults are used: ") + error.what());
  }
  return true;
}

}  // namespace cutline::ui

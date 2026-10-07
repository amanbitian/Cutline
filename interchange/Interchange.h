#pragma once

// Timeline interchange: one neutral description of an edit that every exchange format
// reads into and writes from.
//
// Why a neutral model and not one converter per pair of formats. OpenTimelineIO, EDL and
// Final Cut XML each describe tracks of clips with times, differing in syntax and in what
// they can say. Going through one model means a format has only to be taught how to speak
// it, and the project side (building a timeline from a project, applying one to a project)
// is written and tested once. A format that cannot express something says so in the
// `Report`; nothing is dropped without a line in it.
//
// Times are exact rationals, as everywhere else. A format with its own time notation (a
// frame count at a rate, a timecode) converts at its edge and says when it had to round.

#include "core/commands/Command.h"
#include "core/model/Types.h"
#include "core/util/JsonParse.h"
#include "core/time/RationalTime.h"
#include "timeline/Sequence.h"

#include <optional>
#include <string>
#include <vector>

namespace cutline::project {
class ProjectStore;
}

namespace cutline::interchange {

// ------------------------------------------------------------------ report ----

struct Issue final {
  enum class Severity { Note, Warning, Error };
  Severity severity{Severity::Note};
  // Where it happened, in the format's own terms ("Track V1 / Clip 3").
  std::string where;
  std::string message;
};

struct Report final {
  std::vector<Issue> issues;

  void Note(std::string where, std::string message);
  void Warn(std::string where, std::string message);
  void Error(std::string where, std::string message);
  [[nodiscard]] int Count(Issue::Severity severity) const;
  [[nodiscard]] bool HasErrors() const { return Count(Issue::Severity::Error) != 0; }
  // True when something was changed, dropped or assumed: a warning or an error.
  [[nodiscard]] bool Lossy() const { return Count(Issue::Severity::Warning) + Count(Issue::Severity::Error) != 0; }
  [[nodiscard]] std::string ToText() const;
  // Does any issue mention this text?
  [[nodiscard]] bool Mentions(const std::string& text) const;
};

// ------------------------------------------------------------------- model ----

struct Media final {
  std::string id;
  std::string name;
  std::string url;          // a file URL or path, as the format gave it
  std::string fingerprint;
  time::RationalTime duration;
  time::RationalTime start_timecode;  // the media's own first timestamp
};

struct Marker final {
  std::string id;
  time::RationalTime start;
  time::RationalTime end;
  std::string label;
  model::MarkerKind kind{model::MarkerKind::Comment};
  std::string color;
  std::string metadata_json{"{}"};
};

// A clip on a track. Positions are on the sequence's timeline.
struct Item final {
  std::string id;
  std::string name;
  model::SourceKind source_kind{model::SourceKind::Media};
  std::string source_id;  // a media id, or the id of a nested sequence
  time::RationalTime source_in;
  time::RationalTime source_out;
  time::RationalTime timeline_start;
  time::RationalTime playback_rate{1, 1};
  bool reversed{false};
  bool maintain_pitch{false};
  bool enabled{true};
  std::string linked_group;
  std::vector<timeline::Effect> effects;
  std::vector<Marker> markers;

  [[nodiscard]] time::RationalTime duration() const { return source_out.Subtract(source_in).Divide(playback_rate); }
  [[nodiscard]] time::RationalTime end() const { return timeline_start.Add(duration()); }
};

struct Transition final {
  std::string id;
  std::string kind;
  model::TransitionAlignment alignment{model::TransitionAlignment::Center};
  std::optional<std::string> from_item;
  std::optional<std::string> to_item;
  time::RationalTime start;
  time::RationalTime duration;
};

struct Track final {
  std::string id;
  std::string name;
  model::TrackKind kind{model::TrackKind::Video};
  std::int64_t order{0};
  bool locked{false};
  bool muted{false};
  bool solo{false};
  double gain_db{0.0};
  double pan{0.0};
  std::string channel_layout{"stereo"};
  std::vector<Item> items;  // by timeline_start, not overlapping
  std::vector<Transition> transitions;
  std::vector<timeline::Effect> effects;
};

struct Sequence final {
  std::string id;
  commands::SequenceSettings settings;
  std::vector<Track> tracks;
  std::vector<timeline::Effect> effects;
  std::vector<Marker> markers;
};

struct Timeline final {
  std::vector<Media> media;
  // The sequence being exchanged is first; the sequences it nests follow.
  std::vector<Sequence> sequences;

  [[nodiscard]] const Media* FindMedia(const std::string& id) const;
  [[nodiscard]] const Sequence* FindSequence(const std::string& id) const;
};

// ----------------------------------------------------------- project edge ----

// Describes a sequence of the project, and every sequence it nests, as a Timeline.
// Things the neutral model has no place for (audio buses and sends, adjustment clips)
// are reported rather than carried.
[[nodiscard]] Timeline FromProject(const project::ProjectStore& store, const std::string& sequence_id, Report& report);

struct ApplyOptions final {
  std::string author{"interchange"};
  std::string timestamp_utc{"2026-01-01T00:00:00Z"};
  // Prepended to every id the import creates, so an import cannot collide with what
  // the project already holds. Empty keeps the ids as they are.
  std::string id_prefix;
};

struct ApplyResult final {
  int media_created{0};
  int sequences_created{0};
  int clips_created{0};
  int transitions_created{0};
  int effects_created{0};
  int markers_created{0};
  // Ids of the sequences made, root first (with the prefix applied).
  std::vector<std::string> sequence_ids;
};

// Makes the timeline in the project, one ordinary command at a time, so the whole import
// is undoable step by step and goes through the same validation as any edit. A command the
// project refuses is reported with where it came from and the import carries on with the
// rest. Media that already exists under the same id is reused, not replaced.
ApplyResult ApplyToProject(project::ProjectStore& store, const Timeline& timeline, const ApplyOptions& options,
                           Report& report);

// ------------------------------------------------------------ shared tools ----

// A rate as a double (24, 23.976023976023978, 29.97002997002997) to the exact rational it
// stands for: an integer when it is one, a broadcast rate when it is within a millionth of
// one, otherwise the closest fraction with a small denominator. `exact` is false for the
// last case, which the caller should report.
[[nodiscard]] time::FrameRate RateFromDouble(double rate, bool& exact);

// The parts of the exchange model that formats with no field for them carry as JSON in a
// comment or metadata block: exact rationals, effects (parameters, keyframes and all), and
// markers. Reading is strict about shape and lenient about nothing.
[[nodiscard]] std::string ExactToJson(const time::RationalTime& t);
[[nodiscard]] time::RationalTime ExactFromJson(const json::Value* value);
[[nodiscard]] std::string EffectToJson(const timeline::Effect& effect);
[[nodiscard]] timeline::Effect EffectFromJson(const json::Value& value);
[[nodiscard]] std::string MarkerToJson(const Marker& marker);
[[nodiscard]] Marker MarkerFromJson(const json::Value& value);

// The id with its prefix applied, and without it.
[[nodiscard]] std::string Prefixed(const std::string& prefix, const std::string& id);

}  // namespace cutline::interchange

#pragma once

// An immutable, in-memory snapshot of one sequence and everything it needs to
// be played.
//
// The compiler works on this rather than querying SQLite per frame. At 60 fps a
// SQL round trip per track per frame is the wrong shape, and a snapshot also
// means the compiler is a pure function that can be tested without a database.
// The store bumps its revision on every edit, so a snapshot is reloaded when
// `source_revision` falls behind rather than on a timer.

#include "captions/Captions.h"
#include "core/anim/Keyframe.h"
#include "core/model/RenderVersion.h"
#include "core/model/Types.h"
#include "core/time/RationalTime.h"
#include "effects/MaskDocument.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cutline::timeline {

struct Parameter final {
  std::string id;
  std::string name;
  anim::AnimatedValue value;
};

struct EffectMask final {
  std::string id;
  std::int64_t order{};
  effects::mask::Document document;
};

struct Effect final {
  std::string id;
  std::string effect_type;
  // Optional reusable preset name. For asset-backed effects such as a .cube
  // LUT, this is the project-relative asset reference stored by AddEffect.
  std::string preset_name;
  // For a preset_name of the form project:<id>, the project's own graphic as the compositor reads it
  // (render/Graphics.h bundle JSON), filled in when the sequence is loaded so the picture does not
  // depend on any file outside the project.
  std::string inline_asset;
  std::int64_t order{};
  bool enabled{true};
  bool intrinsic{false};
  std::vector<Parameter> parameters;
  std::vector<EffectMask> masks;
};

struct Clip final {
  std::string id;
  model::SourceKind source_kind{model::SourceKind::Media};
  // Media id, nested sequence id, or empty for an adjustment clip.
  std::string source_id;
  time::RationalTime source_in;
  time::RationalTime source_out;
  time::RationalTime timeline_start;
  time::RationalTime playback_rate{1, 1};
  bool reversed{false};
  // Audio at another speed keeps its pitch (time-stretched) instead of changing it.
  bool maintain_pitch{false};
  // What the sound is for: "dialogue", "music", "effects", "ambience" or empty.
  std::string audio_role;
  bool enabled{true};
  std::string linked_group;
  std::string name;
  std::vector<Effect> effects;

  // Timeline extent. A clip played faster occupies less timeline than source.
  [[nodiscard]] time::RationalTime duration() const { return source_out.Subtract(source_in).Divide(playback_rate); }
  [[nodiscard]] time::RationalTime end() const { return timeline_start.Add(duration()); }
  // Cached for binary search; the loader fills these from the database columns.
  std::int64_t start_ticks{};
  std::int64_t end_ticks{};
};

struct Transition final {
  std::string id;
  std::string kind;
  model::TransitionAlignment alignment{model::TransitionAlignment::Center};
  std::optional<std::string> from_clip_id;
  std::optional<std::string> to_clip_id;
  time::RationalTime timeline_start;
  time::RationalTime duration;
  std::int64_t start_ticks{};
  std::int64_t end_ticks{};
  std::vector<Effect> effects;
};

// A copy of a track's signal into a bus, before or after its fader.
struct TrackSend final {
  std::string bus_id;
  double gain_db{0.0};
  bool pre_fader{false};
};

struct Track final {
  std::string id;
  model::TrackKind kind{model::TrackKind::Video};
  // Compositing and mix order. Higher order sits above / later in the stack.
  std::int64_t order{};
  bool locked{false};
  bool muted{false};
  bool solo{false};
  double gain_db{0.0};
  double pan{0.0};
  std::string channel_layout{"stereo"};
  std::string name;
  // Audio routing. A bus holds no clips and sums the tracks routed to it; a track
  // with no output bus goes to the master.
  bool is_bus{false};
  std::string output_bus_id;
  std::vector<TrackSend> sends;
  // Sorted by start_ticks. Overlap is forbidden by the store, so at most one
  // clip on a track is active at any instant -- which is what lets the compiler
  // binary search instead of scanning.
  std::vector<Clip> clips;
  std::vector<Transition> transitions;
  std::vector<Effect> effects;
};

struct Sequence final {
  std::string id;
  std::string name;
  time::FrameRate frame_rate{};
  std::int64_t width{};
  std::int64_t height{};
  time::FrameRate pixel_aspect{1, 1};
  std::int64_t sample_rate{};
  std::string channel_layout{"stereo"};
  std::string working_color_space{"rec709"};
  std::string display_color_space{"rec709"};
  // The rendering rules this sequence was made under (core/model/RenderVersion.h).
  // A snapshot built by hand renders under the current rules.
  std::int64_t render_version{model::kCurrentRenderVersion};
  model::FieldOrder field_order{model::FieldOrder::Progressive};
  bool drop_frame{false};
  // Video tracks ascending by order, then audio tracks ascending by order.
  std::vector<Track> tracks;
  std::vector<Effect> effects;
  // Timed text, by caption track, each track's cues ordered by start time.
  std::vector<captions::Track> caption_tracks;
  // The project revision this snapshot was taken at.
  std::int64_t source_revision{};

  [[nodiscard]] const Track* FindTrack(const std::string& track_id) const;
  // Timeline extent of the sequence: the end of its last clip.
  [[nodiscard]] time::RationalTime Duration() const;
};

// A sequence graph, so nested sequences can be compiled without another
// database round trip mid-frame. Index 0 is always the sequence being played.
struct SequenceGraph final {
  std::vector<Sequence> sequences;
  [[nodiscard]] const Sequence* Find(const std::string& sequence_id) const;
  [[nodiscard]] const Sequence* root() const { return sequences.empty() ? nullptr : &sequences.front(); }
};

}  // namespace cutline::timeline

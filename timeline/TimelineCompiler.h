#pragma once

// Turns a sequence plus a time into a playback plan: what to decode, where to
// read it, in what order to composite it, and with which effect values.
//
// The compiler is a pure function over a Sequence snapshot. It allocates only
// the plan it returns, performs no I/O, and does not touch the database, so it
// is safe to call from the render thread once per frame.
//
// Two properties it is responsible for, both of which the previous version got
// wrong:
//   * compositing order follows the track's declared order, not its id;
//   * a muted or non-soloed track contributes nothing, for video as well as
//     audio.

#include "timeline/Sequence.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cutline::timeline {

// An effect with its parameters already sampled at the frame being compiled.
struct SampledParameter final {
  std::string name;
  anim::Value value;
};

struct SampledMask final {
  std::string id;
  std::int64_t order{};
  effects::mask::Document document;
};

struct SampledEffect final {
  std::string id;
  std::string effect_type;
  std::string preset_name;
  // The project's own graphic when preset_name is project:<id>; see timeline::Effect.
  std::string inline_asset;
  // Position in the owner's stack. Carried so a consumer can order effects by
  // comparing a number rather than looking each id back up.
  std::int64_t order{};
  bool intrinsic{false};
  std::vector<SampledParameter> parameters;
  std::vector<SampledMask> masks;
};

// One thing to decode and draw. The plan carries these in composite order.
struct SourceRequest final {
  std::string clip_id;
  std::string track_id;
  std::int64_t track_order{};
  model::TrackKind kind{model::TrackKind::Video};
  model::SourceKind source_kind{model::SourceKind::Media};
  // Media id, nested sequence id, or empty for an adjustment clip.
  std::string source_id;
  // Where in the source to read. For a reversed clip this already counts back
  // from the out point, so a decoder never needs to know about direction.
  time::RationalTime source_time;
  time::RationalTime playback_rate{1, 1};
  bool reversed{false};
  // A time-remap curve can stop, accelerate, or run backwards at this instant.
  // source_rate is signed source-seconds per timeline-second; the rational
  // playback_rate remains its absolute decoder/mixer-friendly approximation.
  bool time_remapped{false};
  double source_rate{1.0};
  std::string linked_group;
  // Nesting depth: 0 for the sequence being played, 1 for a clip inside a
  // nested sequence, and so on.
  int depth{0};
  // Clip effects, then the effects of the track it sits on.
  std::vector<SampledEffect> effects;
  // Audio only, taken from the track.
  double gain_db{0.0};
  double pan{0.0};
};

// An active transition. `progress` runs 0 at the first frame to 1 at the last,
// and the two sides name the SourceRequests being mixed. Either side may be
// absent, which means black (video) or silence (audio).
struct TransitionMix final {
  std::string id;
  std::string kind;
  std::string track_id;
  std::int64_t track_order{};
  std::optional<std::string> from_clip_id;
  std::optional<std::string> to_clip_id;
  double progress{0.0};
  std::vector<SampledEffect> effects;
};

// A caption showing at the instant compiled, with the style it is drawn in.
struct CaptionDraw final {
  std::string track_id;
  std::string cue_id;
  std::string text;
  std::string style_json;
};

struct PlaybackPlan final {
  std::string sequence_id;
  time::RationalTime sequence_time;
  time::FrameRate frame_rate{};
  std::int64_t width{};
  std::int64_t height{};
  std::string working_color_space;
  std::string display_color_space;
  // The root sequence's render version: one render, one set of rules, however
  // deeply it nests.
  std::int64_t render_version{model::kCurrentRenderVersion};
  // Ascending track order: index 0 composites first and ends up at the bottom.
  std::vector<SourceRequest> video;
  // Ascending track order. Mixed, not stacked, but a stable order keeps
  // summation deterministic.
  std::vector<SourceRequest> audio;
  std::vector<TransitionMix> transitions;
  std::vector<SampledEffect> sequence_effects;
  // Captions to burn into the picture: empty unless the compile options ask for them.
  std::vector<CaptionDraw> captions;

  [[nodiscard]] bool empty() const { return video.empty() && audio.empty(); }
};

struct CompileOptions final {
  // Put the captions showing at this time into the plan, to be drawn into the picture. Off for
  // delivery with captions as separate files; on for the monitor's caption display and for a burn-in.
  bool include_captions{false};
  // How far to follow nested sequences. Cycles are rejected at edit time, so
  // this is a guard against pathological depth rather than against loops.
  int max_nesting_depth{8};
  // When false, muted and non-soloed tracks are included anyway. Used by export
  // paths that need every track regardless of monitoring state.
  bool honour_mute_and_solo{true};
  // Audio quality over speed: pitch-preserving time-stretch uses its offline
  // parameters (wider search, longer windows) instead of the preview's. Export sets it.
  bool high_quality_audio{false};
};

// Whether `track`, which belongs to `sequence`, is heard or seen under the options'
// mute and solo policy: a muted track contributes nothing, and once any track of
// a medium is soloed only soloed tracks of that medium do. Shared so that the
// picture compiler and the audio mixer cannot disagree about it.
[[nodiscard]] bool TrackContributes(const Sequence& sequence, const Track& track, const CompileOptions& options);

class TimelineCompiler final {
 public:
  // Compiles one instant of the graph's root sequence.
  [[nodiscard]] PlaybackPlan Compile(const SequenceGraph& graph, const time::RationalTime& sequence_time,
                                     const CompileOptions& options = {}) const;

  // Convenience overload for a sequence with no nesting.
  [[nodiscard]] PlaybackPlan Compile(const Sequence& sequence, const time::RationalTime& sequence_time,
                                     const CompileOptions& options = {}) const;

  // Times within [from, until) where the set of active sources changes: every
  // clip and transition edge. Always begins with `from`.
  //
  // Audio has to be mixed in pieces bounded by these, not in fixed blocks. A
  // block that straddles a cut would otherwise be mixed entirely from the plan
  // at its start, so the outgoing clip would be heard across the join.
  [[nodiscard]] std::vector<time::RationalTime> BoundariesIn(const SequenceGraph& graph,
                                                             const time::RationalTime& from,
                                                             const time::RationalTime& until) const;

  // The frames a decoder should be asked to prepare for the window
  // [from, until). Used to drive read-ahead: it reports every distinct source
  // that becomes active anywhere in the range, so a seek does not stall.
  [[nodiscard]] std::vector<SourceRequest> CompileRange(const SequenceGraph& graph, const time::RationalTime& from,
                                                        const time::RationalTime& until,
                                                        const CompileOptions& options = {}) const;
};

}  // namespace cutline::timeline

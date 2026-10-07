#include "timeline/TimelineCompiler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace cutline::timeline {
namespace {

void Require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

void ValidateSequence(const Sequence& sequence) {
  Require(!sequence.id.empty(), "Sequence has no identity");
  Require(sequence.frame_rate.numerator > 0 && sequence.frame_rate.denominator > 0,
          "Sequence has an invalid frame rate");
  Require(sequence.width > 0 && sequence.height > 0, "Sequence has an invalid frame size");
}

// Any track soloed anywhere in the sequence mutes every non-soloed track of the
// same medium. Solo is scoped per medium so soloing a video track does not
// silence the mix.
struct SoloState final {
  bool video{false};
  bool audio{false};

  [[nodiscard]] bool AnyFor(model::TrackKind kind) const {
    return kind == model::TrackKind::Video ? video : audio;
  }
};

[[nodiscard]] SoloState DetectSolo(const Sequence& sequence) {
  SoloState state;
  for (const auto& track : sequence.tracks) {
    // A bus is not soloed: it stays audible while the tracks that feed it are.
    if (!track.solo || track.is_bus) continue;
    if (track.kind == model::TrackKind::Video) {
      state.video = true;
    } else {
      state.audio = true;
    }
  }
  return state;
}

[[nodiscard]] bool TrackContributes(const Track& track, const SoloState& solo, const CompileOptions& options) {
  if (!options.honour_mute_and_solo) return true;
  // Muting applies to video as well as audio: a muted video track shows nothing.
  if (track.muted) return false;
  if (track.is_bus) return true;
  // Once anything of this medium is soloed, only soloed tracks contribute.
  if (solo.AnyFor(track.kind) && !track.solo) return false;
  return true;
}

// Finds the clip active at `ticks`, or nullptr. Clips on a track are sorted and
// may not overlap, so at most one can match and a binary search suffices.
[[nodiscard]] const Clip* ActiveClip(const Track& track, std::int64_t ticks) {
  const auto position = std::upper_bound(
      track.clips.begin(), track.clips.end(), ticks,
      [](std::int64_t value, const Clip& candidate) { return value < candidate.start_ticks; });
  if (position == track.clips.begin()) return nullptr;
  const auto& candidate = *std::prev(position);
  // Half-open: a clip ending exactly here is no longer on screen.
  if (ticks >= candidate.end_ticks) return nullptr;
  return &candidate;
}

// Samples an effect stack at a time expressed in the owner's local timebase.
[[nodiscard]] std::vector<SampledEffect> SampleEffects(const std::vector<Effect>& effects,
                                                       const time::RationalTime& local_time) {
  std::vector<SampledEffect> sampled;
  sampled.reserve(effects.size());
  for (const auto& effect : effects) {
    if (!effect.enabled) continue;
    SampledEffect entry;
    entry.id = effect.id;
    entry.effect_type = effect.effect_type;
    entry.preset_name = effect.preset_name;
    entry.inline_asset = effect.inline_asset;
    entry.order = effect.order;
    entry.intrinsic = effect.intrinsic;
    entry.parameters.reserve(effect.parameters.size());
    for (const auto& parameter : effect.parameters) {
      entry.parameters.push_back({parameter.name, parameter.value.Sample(local_time)});
    }
    const auto seconds = static_cast<double>(local_time.numerator()) /
                         static_cast<double>(local_time.denominator());
    entry.masks.reserve(effect.masks.size());
    for (const auto& mask : effect.masks) {
      entry.masks.push_back({mask.id, mask.order, effects::mask::Evaluate(mask.document, seconds)});
    }
    std::stable_sort(entry.masks.begin(), entry.masks.end(),
                     [](const SampledMask& left, const SampledMask& right) { return left.order < right.order; });
    sampled.push_back(std::move(entry));
  }
  // Render order is the stack order, which is what an inspector shows. Sorting
  // on the carried order keeps this O(n log n); looking each id back up in the
  // source list per comparison made a deep stack quadratic.
  std::stable_sort(sampled.begin(), sampled.end(),
                   [](const SampledEffect& left, const SampledEffect& right) { return left.order < right.order; });
  return sampled;
}

// Where in the source to read, for a clip active at `sequence_time`.
//
// Forward: source_in + elapsed * rate.
// Reversed: source_out - elapsed * rate, so the decoder is handed a plain
// source time and never has to know the clip plays backwards.
struct SourceMapping final {
  time::RationalTime time;
  time::RationalTime rate{1, 1};
  double signed_rate{1.0};
  bool reversed{false};
  bool remapped{false};
};

[[nodiscard]] time::RationalTime FromSeconds(double seconds) {
  constexpr std::int64_t kScale = 1'000'000;
  return {static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kScale))), kScale};
}

[[nodiscard]] const Parameter* TimeRemapParameter(const Clip& clip) {
  for (const auto& effect : clip.effects) {
    if (!effect.enabled || effect.effect_type != "time_remap") continue;
    for (const auto& parameter : effect.parameters) {
      if (parameter.name == "source_offset") return &parameter;
    }
  }
  return nullptr;
}

[[nodiscard]] SourceMapping SourceTimeFor(const Clip& clip, const time::RationalTime& sequence_time) {
  const auto local = sequence_time.Subtract(clip.timeline_start);
  const auto* remap = TimeRemapParameter(clip);
  if (remap == nullptr) {
    const auto elapsed = local.Multiply(clip.playback_rate);
    return {clip.reversed ? clip.source_out.Subtract(elapsed) : clip.source_in.Add(elapsed), clip.playback_rate,
            (clip.reversed ? -1.0 : 1.0) * static_cast<double>(clip.playback_rate.numerator()) /
                static_cast<double>(clip.playback_rate.denominator()),
            clip.reversed, false};
  }

  const auto span = clip.source_out.Subtract(clip.source_in);
  const auto span_seconds = static_cast<double>(span.numerator()) / static_cast<double>(span.denominator());
  const auto sample_offset = [&](const time::RationalTime& at) {
    return std::clamp(remap->value.Sample(at).scalar(), 0.0, span_seconds);
  };
  const auto offset = sample_offset(local);
  // A central finite difference works for linear, eased, and Bezier segments;
  // held keys naturally produce exactly zero and therefore a freeze frame.
  const time::RationalTime step{1, 1000};
  const auto low = local.Compare(step) > 0 ? local.Subtract(step) : time::RationalTime(0, 1);
  const auto high = local.Add(step).Compare(clip.duration()) < 0 ? local.Add(step) : clip.duration();
  const auto seconds_between = high.Subtract(low);
  const auto denominator = static_cast<double>(seconds_between.numerator()) /
                           static_cast<double>(seconds_between.denominator());
  const auto signed_rate = denominator > 0.0 ? (sample_offset(high) - sample_offset(low)) / denominator : 0.0;
  constexpr std::int64_t kRateScale = 1'000'000;
  const auto magnitude = std::abs(signed_rate);
  const auto rate = time::RationalTime(static_cast<std::int64_t>(std::llround(magnitude * kRateScale)), kRateScale);
  return {clip.source_in.Add(FromSeconds(offset)), rate, signed_rate, signed_rate < 0.0, true};
}

[[nodiscard]] SourceRequest BuildRequest(const Sequence& sequence, const Track& track, const Clip& clip,
                                         const time::RationalTime& sequence_time, int depth) {
  (void)sequence;
  SourceRequest request;
  request.clip_id = clip.id;
  request.track_id = track.id;
  request.track_order = track.order;
  request.kind = track.kind;
  request.source_kind = clip.source_kind;
  request.source_id = clip.source_id;
  const auto mapping = SourceTimeFor(clip, sequence_time);
  request.source_time = mapping.time;
  request.playback_rate = mapping.rate;
  request.reversed = mapping.reversed;
  request.time_remapped = mapping.remapped;
  request.source_rate = mapping.signed_rate;
  request.linked_group = clip.linked_group;
  request.depth = depth;
  request.gain_db = track.kind == model::TrackKind::Audio ? track.gain_db : 0.0;
  request.pan = track.kind == model::TrackKind::Audio ? track.pan : 0.0;

  // Clip effect keyframes are authored relative to the start of the clip, so
  // trimming or moving a clip carries its animation with it.
  const auto clip_local = sequence_time.Subtract(clip.timeline_start);
  request.effects = SampleEffects(clip.effects, clip_local);

  // Track effects apply after the clip's own, in sequence time.
  auto track_effects = SampleEffects(track.effects, sequence_time);
  request.effects.insert(request.effects.end(), std::make_move_iterator(track_effects.begin()),
                         std::make_move_iterator(track_effects.end()));
  return request;
}

[[nodiscard]] const Transition* ActiveTransition(const Track& track, std::int64_t ticks) {
  for (const auto& transition : track.transitions) {
    if (ticks >= transition.start_ticks && ticks < transition.end_ticks) return &transition;
  }
  return nullptr;
}

// Collects the plan for one sequence, appending into `plan`. Recurses into
// nested sequences, offsetting time into the nested sequence's own timebase.
void CompileInto(const SequenceGraph* graph, const Sequence& sequence, const time::RationalTime& sequence_time,
                 const CompileOptions& options, int depth, PlaybackPlan& plan,
                 std::unordered_set<std::string>& visiting) {
  ValidateSequence(sequence);
  if (depth > options.max_nesting_depth) return;
  // Cycles are rejected when the edit is made; this is a second line of defence
  // so a hand-built or corrupted graph cannot hang the render thread.
  if (!visiting.insert(sequence.id).second) return;

  const auto solo = DetectSolo(sequence);
  const auto ticks = sequence_time.ToTicks();

  for (const auto& track : sequence.tracks) {
    if (!TrackContributes(track, solo, options)) continue;

    if (const auto* transition = ActiveTransition(track, ticks)) {
      TransitionMix mix;
      mix.id = transition->id;
      mix.kind = transition->kind;
      mix.track_id = track.id;
      mix.track_order = track.order;
      mix.from_clip_id = transition->from_clip_id;
      mix.to_clip_id = transition->to_clip_id;
      const auto elapsed = sequence_time.Subtract(transition->timeline_start);
      const auto ratio = elapsed.Divide(transition->duration);
      mix.progress = static_cast<double>(ratio.numerator()) / static_cast<double>(ratio.denominator());
      mix.effects = SampleEffects(transition->effects, elapsed);
      plan.transitions.push_back(std::move(mix));

      // Both sides of a transition are decoded, even though only one of them is
      // the clip the playhead sits inside. Without this the outgoing clip would
      // vanish the moment the playhead crossed the cut.
      for (const auto* side : {&transition->from_clip_id, &transition->to_clip_id}) {
        if (!side->has_value()) continue;
        const auto found = std::find_if(track.clips.begin(), track.clips.end(),
                                        [&](const Clip& candidate) { return candidate.id == **side; });
        if (found == track.clips.end() || !found->enabled) continue;
        // Reading past a clip's own range during a transition is what its
        // handles are for; clamp so a decoder is never asked for a frame
        // outside the media.
        auto clamped = sequence_time;
        if (clamped.Compare(found->timeline_start) < 0) clamped = found->timeline_start;
        const auto last = found->end();
        if (clamped.Compare(last) >= 0) clamped = last;
        auto request = BuildRequest(sequence, track, *found, clamped, depth);
        if (track.kind == model::TrackKind::Video) {
          plan.video.push_back(std::move(request));
        } else {
          plan.audio.push_back(std::move(request));
        }
      }
      continue;
    }

    const auto* clip = ActiveClip(track, ticks);
    if (clip == nullptr || !clip->enabled) continue;

    if (clip->source_kind == model::SourceKind::Sequence) {
      // A nested clip contributes whatever its own sequence resolves to at the
      // corresponding inner time.
      const auto* nested = graph != nullptr ? graph->Find(clip->source_id) : nullptr;
      if (nested != nullptr) {
        const auto inner_time = SourceTimeFor(*clip, sequence_time).time;
        CompileInto(graph, *nested, inner_time, options, depth + 1, plan, visiting);
      }
      // The clip itself is still reported so its effect stack and the track's
      // can be applied to the nested result.
    }

    auto request = BuildRequest(sequence, track, *clip, sequence_time, depth);
    if (track.kind == model::TrackKind::Video) {
      plan.video.push_back(std::move(request));
    } else {
      plan.audio.push_back(std::move(request));
    }
  }

  // A sequence's own effects shape that sequence's output. A nested sequence's
  // are not the root's: appending them here applied a nested sequence's grade or
  // volume to everything else in the plan.
  if (depth == 0) {
    auto sequence_effects = SampleEffects(sequence.effects, sequence_time);
    plan.sequence_effects.insert(plan.sequence_effects.end(), std::make_move_iterator(sequence_effects.begin()),
                                 std::make_move_iterator(sequence_effects.end()));
  }
  visiting.erase(sequence.id);
}

// Composite order: deepest nesting first, then ascending track order. Stable so
// that two requests from the same track keep the order they were collected in,
// which is what makes a transition's from/to pair deterministic.
void SortByCompositeOrder(std::vector<SourceRequest>& requests) {
  std::stable_sort(requests.begin(), requests.end(), [](const SourceRequest& left, const SourceRequest& right) {
    if (left.depth != right.depth) return left.depth > right.depth;
    return left.track_order < right.track_order;
  });
}

}  // namespace

const Track* Sequence::FindTrack(const std::string& track_id) const {
  const auto found = std::find_if(tracks.begin(), tracks.end(),
                                  [&](const Track& candidate) { return candidate.id == track_id; });
  return found == tracks.end() ? nullptr : &*found;
}

time::RationalTime Sequence::Duration() const {
  time::RationalTime longest;
  for (const auto& track : tracks) {
    if (track.clips.empty()) continue;
    // Clips are sorted and non-overlapping, so the last one ends last.
    const auto end = track.clips.back().end();
    if (end.Compare(longest) > 0) longest = end;
  }
  return longest;
}

bool TrackContributes(const Sequence& sequence, const Track& track, const CompileOptions& options) {
  return TrackContributes(track, DetectSolo(sequence), options);
}

const Sequence* SequenceGraph::Find(const std::string& sequence_id) const {
  const auto found = std::find_if(sequences.begin(), sequences.end(),
                                  [&](const Sequence& candidate) { return candidate.id == sequence_id; });
  return found == sequences.end() ? nullptr : &*found;
}

namespace {

// Compiles `root`, resolving nested sequences through `graph` when there is one.
PlaybackPlan CompileRoot(const SequenceGraph* graph, const Sequence& root, const time::RationalTime& sequence_time,
                         const CompileOptions& options) {
  ValidateSequence(root);

  PlaybackPlan plan;
  plan.sequence_id = root.id;
  plan.sequence_time = sequence_time;
  plan.frame_rate = root.frame_rate;
  plan.width = root.width;
  plan.height = root.height;
  plan.working_color_space = root.working_color_space;
  plan.display_color_space = root.display_color_space;
  plan.render_version = root.render_version;

  std::unordered_set<std::string> visiting;
  CompileInto(graph, root, sequence_time, options, 0, plan, visiting);

  if (options.include_captions) {
    for (const auto& active : captions::ActiveCues(root.caption_tracks, sequence_time)) {
      plan.captions.push_back({active.track->id, active.cue->id, active.cue->text,
                               active.cue->resolved_style_json.empty() ? active.cue->style_json : active.cue->resolved_style_json});
    }
  }

  SortByCompositeOrder(plan.video);
  SortByCompositeOrder(plan.audio);
  std::stable_sort(plan.transitions.begin(), plan.transitions.end(),
                   [](const TransitionMix& left, const TransitionMix& right) {
                     return left.track_order < right.track_order;
                   });
  return plan;
}

}  // namespace

PlaybackPlan TimelineCompiler::Compile(const SequenceGraph& graph, const time::RationalTime& sequence_time,
                                       const CompileOptions& options) const {
  const auto* root = graph.root();
  Require(root != nullptr, "Sequence graph is empty");
  return CompileRoot(&graph, *root, sequence_time, options);
}

std::vector<time::RationalTime> TimelineCompiler::BoundariesIn(const SequenceGraph& graph,
                                                               const time::RationalTime& from,
                                                               const time::RationalTime& until) const {
  std::vector<time::RationalTime> boundaries{from};
  const auto consider = [&](const time::RationalTime& candidate) {
    if (candidate.Compare(from) <= 0 || candidate.Compare(until) >= 0) return;
    boundaries.push_back(candidate);
  };
  for (const auto& sequence : graph.sequences) {
    for (const auto& track : sequence.tracks) {
      // Non-overlapping clips are sorted by start, so their ends are sorted too.
      // Seek to the first possible edge and visit only clips in this window.
      // Compare exact rationals: tick rounding can hide very short clips.
      auto clip = std::upper_bound(track.clips.begin(), track.clips.end(), from,
                                   [](const time::RationalTime& value, const Clip& candidate) {
                                     return value.Compare(candidate.end()) < 0;
                                   });
      for (; clip != track.clips.end() && clip->timeline_start.Compare(until) < 0; ++clip) {
        consider(clip->timeline_start);
        consider(clip->end());
      }
      for (const auto& transition : track.transitions) {
        consider(transition.timeline_start);
        consider(transition.timeline_start.Add(transition.duration));
      }
    }
  }
  std::sort(boundaries.begin(), boundaries.end(),
            [](const time::RationalTime& left, const time::RationalTime& right) { return left.Compare(right) < 0; });
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end(),
                               [](const time::RationalTime& left, const time::RationalTime& right) {
                                 return left.Compare(right) == 0;
                               }),
                   boundaries.end());
  return boundaries;
}

PlaybackPlan TimelineCompiler::Compile(const Sequence& sequence, const time::RationalTime& sequence_time,
                                       const CompileOptions& options) const {
  // Compiled in place. This used to wrap a *copy* of the sequence in a graph, so
  // every frame duplicated every clip, effect and keyframe in the project and
  // the active-clip binary search was hidden behind an O(project) copy.
  return CompileRoot(nullptr, sequence, sequence_time, options);
}

std::vector<SourceRequest> TimelineCompiler::CompileRange(const SequenceGraph& graph, const time::RationalTime& from,
                                                          const time::RationalTime& until,
                                                          const CompileOptions& options) const {
  const auto* root = graph.root();
  Require(root != nullptr, "Sequence graph is empty");
  Require(until.Compare(from) > 0, "Read-ahead range must be positive");

  // Compile at every boundary in the window rather than at a fixed stride: the
  // set of active sources only changes where a clip or transition starts or
  // ends, so this reports every source in the range exactly once at the time it
  // becomes active, no matter how short the clips are.
  const auto boundaries = BoundariesIn(graph, from, until);

  std::vector<SourceRequest> requests;
  std::unordered_set<std::string> seen;
  for (const auto& boundary : boundaries) {
    auto plan = Compile(graph, boundary, options);
    for (auto* bucket : {&plan.video, &plan.audio}) {
      for (auto& request : *bucket) {
        // One entry per clip: the earliest time it is needed.
        if (!seen.insert(request.clip_id).second) continue;
        requests.push_back(std::move(request));
      }
    }
  }
  return requests;
}

}  // namespace cutline::timeline

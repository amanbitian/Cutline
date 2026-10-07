// Timeline compiler: composite order, mute/solo, transitions, nesting,
// retiming, and read-ahead.

#include "core/anim/Keyframe.h"
#include "core/commands/Command.h"
#include "core/project/ProjectStore.h"
#include "timeline/SequenceLoader.h"
#include "timeline/TimelineCompiler.h"
#include "timeline/Multicam.h"
#include "core/db/Sql.h"
#include "effects/GraphicsDocument.h"
#include "effects/MaskDocument.h"
#include "render/Compositor.h"
#include "captions/Captions.h"
#include "tests/native/TestHarness.h"

#include <algorithm>
#include <vector>
#include <optional>
#include <memory>
#include <map>
#include <chrono>
#include <mutex>
#include <cmath>
#include <string>

namespace commands = cutline::commands;
namespace model = cutline::model;
namespace rates = cutline::time;
using cutline::anim::Interpolation;
using cutline::anim::Value;
using cutline::project::ProjectStore;
using cutline::time::RationalTime;
using cutline::timeline::Clip;
using cutline::timeline::CompileOptions;
using cutline::timeline::Effect;
using cutline::timeline::Parameter;
using cutline::timeline::PlaybackPlan;
using cutline::timeline::Sequence;
using cutline::timeline::SequenceGraph;
using cutline::timeline::SourceRequest;
using cutline::timeline::TimelineCompiler;
using cutline::timeline::Track;
using cutline::timeline::Transition;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

Clip MakeClip(std::string id, std::string source, std::int64_t start, std::int64_t duration) {
  Clip clip;
  clip.id = std::move(id);
  clip.source_kind = model::SourceKind::Media;
  clip.source_id = std::move(source);
  clip.source_in = Seconds(0);
  clip.source_out = Seconds(duration);
  clip.timeline_start = Seconds(start);
  clip.start_ticks = clip.timeline_start.ToTicks();
  clip.end_ticks = clip.end().ToTicks();
  return clip;
}

Track MakeTrack(std::string id, model::TrackKind kind, std::int64_t order) {
  Track track;
  track.id = std::move(id);
  track.kind = kind;
  track.order = order;
  return track;
}

Sequence MakeSequence(std::string id = "seq-1") {
  Sequence sequence;
  sequence.id = std::move(id);
  sequence.name = "Main";
  sequence.frame_rate = rates::kFrameRate2997;
  sequence.width = 1920;
  sequence.height = 1080;
  sequence.sample_rate = 48000;
  return sequence;
}

// Re-sorts a track's clips and refreshes the tick cache, mirroring what the
// loader guarantees.
void Finalise(Sequence& sequence) {
  for (auto& track : sequence.tracks) {
    for (auto& clip : track.clips) {
      clip.start_ticks = clip.timeline_start.ToTicks();
      clip.end_ticks = clip.end().ToTicks();
    }
    std::sort(track.clips.begin(), track.clips.end(),
              [](const Clip& left, const Clip& right) { return left.start_ticks < right.start_ticks; });
    for (auto& transition : track.transitions) {
      transition.start_ticks = transition.timeline_start.ToTicks();
      transition.end_ticks = transition.timeline_start.Add(transition.duration).ToTicks();
    }
  }
}

std::vector<std::string> ClipIds(const std::vector<SourceRequest>& requests) {
  std::vector<std::string> ids;
  ids.reserve(requests.size());
  for (const auto& request : requests) ids.push_back(request.clip_id);
  return ids;
}

}  // namespace

// ----------------------------------------------------------- basic resolve ----

CUTLINE_TEST(CompileResolvesTheActiveClipAndSourceTime) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(4));
  CHECK_EQ(plan.video.size(), std::size_t{1});
  CHECK_EQ(plan.video[0].clip_id, std::string("clip-1"));
  CHECK_EQ(plan.video[0].source_id, std::string("media-1"));
  CHECK_EQ(plan.video[0].source_time.Compare(Seconds(4)), 0);
  CHECK_EQ(plan.width, 1920);
  CHECK_EQ(plan.display_color_space, std::string("rec709"));
}

CUTLINE_TEST(TimeRemapCurvesCreateRampsFreezesAndReverseSegments) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  Effect remap;
  remap.id = "remap-1";
  remap.effect_type = "time_remap";
  Parameter source_time;
  source_time.id = "remap-source";
  source_time.name = "source_offset";
  source_time.value = cutline::anim::AnimatedValue(std::vector<cutline::anim::Keyframe>{
      {Seconds(0), Value::Scalar(0.0), Interpolation::Linear, {}, {}},
      {Seconds(2), Value::Scalar(4.0), Interpolation::Hold, {}, {}},
      {Seconds(4), Value::Scalar(4.0), Interpolation::Linear, {}, {}},
      {Seconds(8), Value::Scalar(8.0), Interpolation::Linear, {}, {}},
      {Seconds(10), Value::Scalar(6.0), Interpolation::Linear, {}, {}},
  });
  remap.parameters.push_back(std::move(source_time));
  clip.effects.push_back(std::move(remap));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto fast = compiler.Compile(sequence, Seconds(1)).video.front();
  CHECK_EQ(fast.source_time.Compare(Seconds(2)), 0);
  CHECK(fast.time_remapped);
  CHECK(std::abs(fast.source_rate - 2.0) < 1e-3);
  CHECK(!fast.reversed);

  const auto freeze = compiler.Compile(sequence, Seconds(3)).video.front();
  CHECK_EQ(freeze.source_time.Compare(Seconds(4)), 0);
  CHECK(std::abs(freeze.source_rate) < 1e-9);
  CHECK_EQ(freeze.playback_rate.numerator(), std::int64_t{0});

  const auto backwards = compiler.Compile(sequence, Seconds(9)).video.front();
  CHECK_EQ(backwards.source_time.Compare(Seconds(7)), 0);
  CHECK(backwards.reversed);
  CHECK(std::abs(backwards.source_rate + 1.0) < 1e-3);
}

CUTLINE_TEST(ClipRangesAreHalfOpen) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 5));
  track.clips.push_back(MakeClip("clip-2", "media-1", 5, 5));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  // At exactly 5s the first clip has ended and the second has begun; a closed
  // range would report both and double-expose the cut frame.
  const auto plan = compiler.Compile(sequence, Seconds(5));
  CHECK_EQ(plan.video.size(), std::size_t{1});
  CHECK_EQ(plan.video[0].clip_id, std::string("clip-2"));
  CHECK_EQ(plan.video[0].source_time.Compare(Seconds(0)), 0);
}

CUTLINE_TEST(CompileIsEmptyInAGap) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 2));
  track.clips.push_back(MakeClip("clip-2", "media-1", 10, 2));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  CHECK(compiler.Compile(sequence, Seconds(5)).empty());
  CHECK(compiler.Compile(sequence, Seconds(50)).empty());
}

CUTLINE_TEST(DisabledClipsContributeNothing) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.enabled = false;
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  CHECK(compiler.Compile(sequence, Seconds(4)).empty());
}

CUTLINE_TEST(CompileRejectsAnInvalidSequence) {
  Sequence broken;
  const TimelineCompiler compiler;
  CHECK_THROWS(compiler.Compile(broken, Seconds(0)));

  auto no_rate = MakeSequence();
  no_rate.frame_rate = {0, 1};
  CHECK_THROWS(compiler.Compile(no_rate, Seconds(0)));
}

// ------------------------------------------------------- composite ordering ----

CUTLINE_TEST(CompositeOrderFollowsTrackOrderNotTrackId) {
  // The previous compiler sorted by track id, so V2 ("v2") sorted before
  // V10 ("v10") and the stack came out wrong. Order must come from the
  // declared track order.
  auto sequence = MakeSequence();
  for (const auto& [id, order] : std::vector<std::pair<std::string, std::int64_t>>{
           {"v10", 10}, {"v2", 2}, {"v1", 1}}) {
    auto track = MakeTrack(id, model::TrackKind::Video, order);
    track.clips.push_back(MakeClip("clip-" + id, "media-1", 0, 10));
    sequence.tracks.push_back(std::move(track));
  }
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.video.size(), std::size_t{3});
  // Ascending order: V1 at the bottom, V10 on top.
  CHECK_EQ(plan.video[0].track_id, std::string("v1"));
  CHECK_EQ(plan.video[1].track_id, std::string("v2"));
  CHECK_EQ(plan.video[2].track_id, std::string("v10"));
}

CUTLINE_TEST(AudioIsOrderedToo) {
  // The previous compiler never sorted audio at all.
  auto sequence = MakeSequence();
  for (const auto& [id, order] : std::vector<std::pair<std::string, std::int64_t>>{
           {"a3", 3}, {"a1", 1}, {"a2", 2}}) {
    auto track = MakeTrack(id, model::TrackKind::Audio, order);
    track.clips.push_back(MakeClip("clip-" + id, "media-1", 0, 10));
    sequence.tracks.push_back(std::move(track));
  }
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.audio.size(), std::size_t{3});
  CHECK_EQ(plan.audio[0].track_id, std::string("a1"));
  CHECK_EQ(plan.audio[1].track_id, std::string("a2"));
  CHECK_EQ(plan.audio[2].track_id, std::string("a3"));
}

CUTLINE_TEST(MulticamSynchronisesBuildsAnAngleMonitorRecordsCutsAndFlattens) {
  namespace mc = cutline::timeline::multicam;
  mc::Group group;
  group.id = "concert";
  group.duration = Seconds(10);
  mc::Angle wide;
  wide.id = "wide";
  wide.source_id = "media-wide";
  wide.duration = Seconds(20);
  wide.timecode_start = Seconds(100);
  wide.marker = Seconds(5);
  mc::Angle close = wide;
  close.id = "close";
  close.source_id = "media-close";
  close.timecode_start = Seconds(98);
  close.marker = Seconds(7);
  group.angles = {wide, close};

  CHECK(mc::Synchronize(group, mc::SyncMethod::Timecode, "wide") == 1.0);
  CHECK_EQ(group.angles[1].source_offset.Compare(Seconds(2)), 0);
  CHECK(mc::Synchronize(group, mc::SyncMethod::Marker, "wide") == 1.0);
  CHECK_EQ(group.angles[1].source_offset.Compare(Seconds(2)), 0);

  const auto monitor = mc::MonitorRequests(group, Seconds(1));
  CHECK_EQ(monitor.size(), std::size_t{2});
  CHECK_EQ(monitor[0].source_time.Compare(Seconds(1)), 0);
  CHECK_EQ(monitor[1].source_time.Compare(Seconds(3)), 0);

  mc::RecordSwitch(group, Seconds(0), "wide");
  mc::RecordSwitch(group, Seconds(4), "close");
  mc::RecordSwitch(group, Seconds(7), "wide");
  CHECK_EQ(mc::ActiveAngle(group, Seconds(5))->id, std::string("close"));
  const auto clips = mc::Flatten(group);
  CHECK_EQ(clips.size(), std::size_t{3});
  CHECK_EQ(clips[0].source_id, std::string("media-wide"));
  CHECK_EQ(clips[1].source_id, std::string("media-close"));
  CHECK_EQ(clips[1].source_in.Compare(Seconds(6)), 0);
  CHECK_EQ(clips[2].timeline_start.Compare(Seconds(7)), 0);
}

CUTLINE_TEST(MulticamAudioSyncUsesLocalCrossCorrelationAndReportsConfidence) {
  namespace mc = cutline::timeline::multicam;
  mc::Group group;
  group.id = "interview";
  group.duration = Seconds(5);
  mc::Angle reference;
  reference.id = "a";
  reference.source_id = "a";
  reference.duration = Seconds(5);
  reference.envelope_rate = 10;
  reference.audio_envelope = {0, 0, 1, -1, 0.5f, 0, 0, 0, 0, 0, 0, 0};
  mc::Angle delayed = reference;
  delayed.id = "b";
  delayed.source_id = "b";
  delayed.audio_envelope = {0, 0, 0, 0, 0, 1, -1, 0.5f, 0, 0, 0, 0, 0, 0, 0};
  group.angles = {reference, delayed};
  const auto confidence = mc::Synchronize(group, mc::SyncMethod::Audio, "a", 5);
  CHECK(confidence > 0.95);
  CHECK_EQ(group.angles[1].source_offset.Compare(RationalTime(3, 10)), 0);
}

// --------------------------------------------------------- mute and solo ----

CUTLINE_TEST(MutedVideoTracksAreExcluded) {
  // The previous compiler only honoured mute on audio tracks, so a muted video
  // track still rendered.
  auto sequence = MakeSequence();
  auto hidden = MakeTrack("v1", model::TrackKind::Video, 0);
  hidden.muted = true;
  hidden.clips.push_back(MakeClip("clip-hidden", "media-1", 0, 10));
  auto visible = MakeTrack("v2", model::TrackKind::Video, 1);
  visible.clips.push_back(MakeClip("clip-visible", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(hidden));
  sequence.tracks.push_back(std::move(visible));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.video.size(), std::size_t{1});
  CHECK_EQ(plan.video[0].clip_id, std::string("clip-visible"));
}

CUTLINE_TEST(MutedAudioTracksAreExcluded) {
  auto sequence = MakeSequence();
  auto muted = MakeTrack("a1", model::TrackKind::Audio, 0);
  muted.muted = true;
  muted.clips.push_back(MakeClip("clip-muted", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(muted));
  Finalise(sequence);

  const TimelineCompiler compiler;
  CHECK(compiler.Compile(sequence, Seconds(1)).audio.empty());
}

CUTLINE_TEST(SoloSuppressesOtherTracksOfTheSameMedium) {
  // Solo was in the schema but never consulted.
  auto sequence = MakeSequence();
  auto soloed = MakeTrack("a1", model::TrackKind::Audio, 0);
  soloed.solo = true;
  soloed.clips.push_back(MakeClip("clip-solo", "media-1", 0, 10));
  auto other = MakeTrack("a2", model::TrackKind::Audio, 1);
  other.clips.push_back(MakeClip("clip-other", "media-1", 0, 10));
  // A video track must not be silenced by an audio solo.
  auto video = MakeTrack("v1", model::TrackKind::Video, 0);
  video.clips.push_back(MakeClip("clip-video", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(soloed));
  sequence.tracks.push_back(std::move(other));
  sequence.tracks.push_back(std::move(video));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.audio.size(), std::size_t{1});
  CHECK_EQ(plan.audio[0].clip_id, std::string("clip-solo"));
  CHECK_EQ(plan.video.size(), std::size_t{1});
}

CUTLINE_TEST(ExportCanIgnoreMonitoringState) {
  auto sequence = MakeSequence();
  auto muted = MakeTrack("v1", model::TrackKind::Video, 0);
  muted.muted = true;
  muted.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(muted));
  Finalise(sequence);

  CompileOptions options;
  options.honour_mute_and_solo = false;
  const TimelineCompiler compiler;
  CHECK_EQ(compiler.Compile(sequence, Seconds(1), options).video.size(), std::size_t{1});
}

CUTLINE_TEST(LockedTracksStillPlay) {
  // Locking prevents editing, not playback: a locked track must still render.
  auto sequence = MakeSequence();
  auto locked = MakeTrack("v1", model::TrackKind::Video, 0);
  locked.locked = true;
  locked.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(locked));
  Finalise(sequence);

  const TimelineCompiler compiler;
  CHECK_EQ(compiler.Compile(sequence, Seconds(1)).video.size(), std::size_t{1});
}

CUTLINE_TEST(TrackGainAndPanReachTheAudioPlan) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.gain_db = -6.0;
  track.pan = -0.5;
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK(std::abs(plan.audio[0].gain_db + 6.0) < 1e-12);
  CHECK(std::abs(plan.audio[0].pan + 0.5) < 1e-12);
}

// ----------------------------------------------------------------- retiming ----

CUTLINE_TEST(PlaybackRateCompressesTimelineAndAdvancesSourceFaster) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.playback_rate = RationalTime(2, 1);  // 2x
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  // 10s of source at 2x occupies 5s of timeline.
  const auto plan = compiler.Compile(sequence, Seconds(4));
  CHECK_EQ(plan.video.size(), std::size_t{1});
  CHECK_EQ(plan.video[0].source_time.Compare(Seconds(8)), 0);
  // Past the compressed extent there is nothing.
  CHECK(compiler.Compile(sequence, Seconds(6)).empty());
}

CUTLINE_TEST(SlowMotionStretchesTheTimeline) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 4);
  clip.playback_rate = RationalTime(1, 2);  // half speed
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  // 4s of source at 0.5x occupies 8s of timeline.
  const auto plan = compiler.Compile(sequence, Seconds(6));
  CHECK_EQ(plan.video[0].source_time.Compare(Seconds(3)), 0);
  CHECK(compiler.Compile(sequence, Seconds(9)).empty());
}

CUTLINE_TEST(ReversedClipsReadBackwardsFromTheOutPoint) {
  // Reverse playback was structurally impossible before: the rate had to be
  // positive and there was no direction flag.
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.reversed = true;
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  CHECK_EQ(compiler.Compile(sequence, Seconds(0)).video[0].source_time.Compare(Seconds(10)), 0);
  CHECK_EQ(compiler.Compile(sequence, Seconds(3)).video[0].source_time.Compare(Seconds(7)), 0);
  CHECK(compiler.Compile(sequence, Seconds(3)).video[0].reversed);
}

CUTLINE_TEST(SourceTimeStaysExactAtFractionalRates) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  Clip clip;
  clip.id = "clip-1";
  clip.source_id = "media-1";
  clip.source_in = RationalTime::FromFrames(100, rates::kFrameRate23976);
  clip.source_out = RationalTime::FromFrames(300, rates::kFrameRate23976);
  clip.timeline_start = RationalTime::FromFrames(50, rates::kFrameRate23976);
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto at = RationalTime::FromFrames(75, rates::kFrameRate23976);
  const auto plan = compiler.Compile(sequence, at);
  // 25 frames into the clip means source frame 125, exactly.
  CHECK_EQ(plan.video[0].source_time.ToFrames(rates::kFrameRate23976, cutline::time::RoundingMode::Exact), 125);
}

// -------------------------------------------------------------- transitions ----

CUTLINE_TEST(ATransitionReportsBothSidesAndItsProgress) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 6));
  track.clips.push_back(MakeClip("clip-2", "media-2", 6, 6));
  Transition transition;
  transition.id = "t-1";
  transition.kind = "cross_dissolve";
  transition.from_clip_id = "clip-1";
  transition.to_clip_id = "clip-2";
  transition.timeline_start = Seconds(5);
  transition.duration = Seconds(2);
  track.transitions.push_back(std::move(transition));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(6));
  CHECK_EQ(plan.transitions.size(), std::size_t{1});
  CHECK(std::abs(plan.transitions[0].progress - 0.5) < 1e-9);
  // Both sides are decoded, which is what the previous point-sampling compiler
  // could not express.
  const auto ids = ClipIds(plan.video);
  CHECK_EQ(plan.video.size(), std::size_t{2});
  CHECK(std::find(ids.begin(), ids.end(), "clip-1") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "clip-2") != ids.end());
}

CUTLINE_TEST(TransitionProgressRunsFromZeroToOne) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  Transition transition;
  transition.id = "t-1";
  transition.kind = "cross_dissolve";
  transition.to_clip_id = "clip-1";
  transition.timeline_start = Seconds(0);
  transition.duration = Seconds(4);
  track.transitions.push_back(std::move(transition));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  CHECK(std::abs(compiler.Compile(sequence, Seconds(0)).transitions[0].progress) < 1e-9);
  CHECK(std::abs(compiler.Compile(sequence, Seconds(1)).transitions[0].progress - 0.25) < 1e-9);
  CHECK(std::abs(compiler.Compile(sequence, Seconds(3)).transitions[0].progress - 0.75) < 1e-9);
  // Half-open, so the frame at the end belongs to the clip, not the transition.
  CHECK(compiler.Compile(sequence, Seconds(4)).transitions.empty());
}

CUTLINE_TEST(ASingleSidedTransitionReportsOneSource) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  Transition fade;
  fade.id = "t-fade";
  fade.kind = "cross_dissolve";
  fade.alignment = model::TransitionAlignment::Start;
  fade.to_clip_id = "clip-1";
  fade.timeline_start = Seconds(0);
  fade.duration = Seconds(2);
  track.transitions.push_back(std::move(fade));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.transitions.size(), std::size_t{1});
  CHECK(!plan.transitions[0].from_clip_id.has_value());
  CHECK_EQ(plan.video.size(), std::size_t{1});
  CHECK_EQ(plan.video[0].clip_id, std::string("clip-1"));
}

CUTLINE_TEST(TransitionSourceTimesAreClampedToTheirClips) {
  // During a transition the outgoing clip is read beyond where the playhead
  // sits. It must never be asked for a time outside its own range.
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 5));
  track.clips.push_back(MakeClip("clip-2", "media-2", 5, 5));
  Transition transition;
  transition.id = "t-1";
  transition.kind = "cross_dissolve";
  transition.from_clip_id = "clip-1";
  transition.to_clip_id = "clip-2";
  transition.timeline_start = Seconds(4);
  transition.duration = Seconds(2);
  track.transitions.push_back(std::move(transition));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  // At 5.5s, clip-1 has already ended at 5s.
  const auto plan = compiler.Compile(sequence, {11, 2});
  for (const auto& request : plan.video) {
    if (request.clip_id == "clip-1") {
      CHECK(request.source_time.Compare(Seconds(5)) <= 0);
    }
    if (request.clip_id == "clip-2") {
      CHECK(request.source_time.Compare(Seconds(0)) >= 0);
    }
  }
}

// ------------------------------------------------------------------ effects ----

CUTLINE_TEST(ClipEffectsAreSampledInClipLocalTime) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 10, 10);

  Parameter opacity;
  opacity.id = "p-opacity";
  opacity.name = "opacity";
  cutline::anim::AnimatedValue curve;
  // Keyframes are authored from the start of the clip, so moving the clip
  // carries the animation with it.
  curve.SetKeyframe({Seconds(0), Value::Scalar(0.0), Interpolation::Linear, {}, {}});
  curve.SetKeyframe({Seconds(4), Value::Scalar(1.0), Interpolation::Linear, {}, {}});
  opacity.value = curve;

  Effect effect;
  effect.id = "fx-1";
  effect.effect_type = "opacity";
  effect.parameters.push_back(std::move(opacity));
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  // The clip starts at 10s; 2s in means halfway along a 4s ramp.
  const auto plan = compiler.Compile(sequence, Seconds(12));
  CHECK_EQ(plan.video[0].effects.size(), std::size_t{1});
  CHECK_EQ(plan.video[0].effects[0].parameters.size(), std::size_t{1});
  CHECK(std::abs(plan.video[0].effects[0].parameters[0].value.scalar() - 0.5) < 1e-9);
}

CUTLINE_TEST(TrackEffectsFollowClipEffects) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);

  Effect clip_effect;
  clip_effect.id = "fx-clip";
  clip_effect.effect_type = "lumetri";
  clip.effects.push_back(std::move(clip_effect));

  Effect track_effect;
  track_effect.id = "fx-track";
  track_effect.effect_type = "master_grade";
  track.effects.push_back(std::move(track_effect));

  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.video[0].effects.size(), std::size_t{2});
  CHECK_EQ(plan.video[0].effects[0].id, std::string("fx-clip"));
  CHECK_EQ(plan.video[0].effects[1].id, std::string("fx-track"));
}

CUTLINE_TEST(DisabledEffectsAreDropped) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  Effect effect;
  effect.id = "fx-1";
  effect.effect_type = "lumetri";
  effect.enabled = false;
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  CHECK(compiler.Compile(sequence, Seconds(1)).video[0].effects.empty());
}

CUTLINE_TEST(EffectsAreSampledInStackOrder) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  for (const auto& [id, order] : std::vector<std::pair<std::string, std::int64_t>>{
           {"fx-third", 2}, {"fx-first", 0}, {"fx-second", 1}}) {
    Effect effect;
    effect.id = id;
    effect.effect_type = "generic";
    effect.order = order;
    clip.effects.push_back(std::move(effect));
  }
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.video[0].effects.size(), std::size_t{3});
  CHECK_EQ(plan.video[0].effects[0].id, std::string("fx-first"));
  CHECK_EQ(plan.video[0].effects[1].id, std::string("fx-second"));
  CHECK_EQ(plan.video[0].effects[2].id, std::string("fx-third"));
}

CUTLINE_TEST(SequenceEffectsReachThePlan) {
  auto sequence = MakeSequence();
  Effect master;
  master.id = "fx-master";
  master.effect_type = "output_lut";
  sequence.effects.push_back(std::move(master));
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(sequence, Seconds(1));
  CHECK_EQ(plan.sequence_effects.size(), std::size_t{1});
  CHECK_EQ(plan.sequence_effects[0].id, std::string("fx-master"));
}

// ------------------------------------------------------------------ nesting ----

CUTLINE_TEST(NestedSequencesResolveThroughToTheirSources) {
  auto inner = MakeSequence("seq-inner");
  auto inner_track = MakeTrack("iv1", model::TrackKind::Video, 0);
  inner_track.clips.push_back(MakeClip("inner-clip", "media-inner", 0, 10));
  inner.tracks.push_back(std::move(inner_track));
  Finalise(inner);

  auto outer = MakeSequence("seq-outer");
  auto outer_track = MakeTrack("ov1", model::TrackKind::Video, 0);
  Clip nested;
  nested.id = "nest-1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.source_id = "seq-inner";
  nested.source_in = Seconds(0);
  nested.source_out = Seconds(10);
  nested.timeline_start = Seconds(5);
  outer_track.clips.push_back(std::move(nested));
  outer.tracks.push_back(std::move(outer_track));
  Finalise(outer);

  SequenceGraph graph;
  graph.sequences.push_back(std::move(outer));
  graph.sequences.push_back(std::move(inner));

  const TimelineCompiler compiler;
  // 7s on the outer timeline is 2s into the nested sequence.
  const auto plan = compiler.Compile(graph, Seconds(7));
  const auto ids = ClipIds(plan.video);
  CHECK(std::find(ids.begin(), ids.end(), "nest-1") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "inner-clip") != ids.end());
  for (const auto& request : plan.video) {
    if (request.clip_id == "inner-clip") {
      CHECK_EQ(request.source_time.Compare(Seconds(2)), 0);
      CHECK_EQ(request.depth, 1);
      CHECK_EQ(request.source_id, std::string("media-inner"));
    }
  }
}

CUTLINE_TEST(ANestedSequencesOwnEffectsDoNotBecomeTheOuterSequencesEffects) {
  auto inner = MakeSequence("seq-inner");
  Effect inner_grade;
  inner_grade.id = "fx-inner-master";
  inner_grade.effect_type = "output_lut";
  inner.effects.push_back(std::move(inner_grade));
  auto inner_track = MakeTrack("iv1", model::TrackKind::Video, 0);
  inner_track.clips.push_back(MakeClip("inner-clip", "media-inner", 0, 10));
  inner.tracks.push_back(std::move(inner_track));
  Finalise(inner);

  auto outer = MakeSequence("seq-outer");
  Effect outer_grade;
  outer_grade.id = "fx-outer-master";
  outer_grade.effect_type = "output_lut";
  outer.effects.push_back(std::move(outer_grade));
  auto outer_track = MakeTrack("ov1", model::TrackKind::Video, 0);
  Clip nested;
  nested.id = "nest-1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.source_id = "seq-inner";
  nested.source_in = Seconds(0);
  nested.source_out = Seconds(10);
  nested.timeline_start = Seconds(0);
  outer_track.clips.push_back(std::move(nested));
  outer.tracks.push_back(std::move(outer_track));
  Finalise(outer);

  SequenceGraph graph;
  graph.sequences.push_back(std::move(outer));
  graph.sequences.push_back(std::move(inner));

  const auto plan = TimelineCompiler{}.Compile(graph, Seconds(1));
  // Only the outer sequence's effect is in its plan.
  CHECK_EQ(plan.sequence_effects.size(), std::size_t{1});
  CHECK_EQ(plan.sequence_effects[0].id, std::string("fx-outer-master"));
}

CUTLINE_TEST(NestedContentCompositesBeneathItsHost) {
  auto inner = MakeSequence("seq-inner");
  auto inner_track = MakeTrack("iv1", model::TrackKind::Video, 0);
  inner_track.clips.push_back(MakeClip("inner-clip", "media-inner", 0, 10));
  inner.tracks.push_back(std::move(inner_track));
  Finalise(inner);

  auto outer = MakeSequence("seq-outer");
  auto outer_track = MakeTrack("ov1", model::TrackKind::Video, 0);
  Clip nested;
  nested.id = "nest-1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.source_id = "seq-inner";
  nested.source_in = Seconds(0);
  nested.source_out = Seconds(10);
  nested.timeline_start = Seconds(0);
  outer_track.clips.push_back(std::move(nested));
  outer.tracks.push_back(std::move(outer_track));
  Finalise(outer);

  SequenceGraph graph;
  graph.sequences.push_back(std::move(outer));
  graph.sequences.push_back(std::move(inner));

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(graph, Seconds(1));
  CHECK_EQ(plan.video.size(), std::size_t{2});
  // The nested result has to be drawn before the host clip that wraps it.
  CHECK_EQ(plan.video[0].clip_id, std::string("inner-clip"));
  CHECK_EQ(plan.video[1].clip_id, std::string("nest-1"));
}

CUTLINE_TEST(NestingDepthIsBounded) {
  // A graph that references itself must not hang the render thread, even though
  // the store refuses to create one.
  auto looping = MakeSequence("seq-loop");
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  Clip nested;
  nested.id = "nest-self";
  nested.source_kind = model::SourceKind::Sequence;
  nested.source_id = "seq-loop";
  nested.source_in = Seconds(0);
  nested.source_out = Seconds(10);
  nested.timeline_start = Seconds(0);
  track.clips.push_back(std::move(nested));
  looping.tracks.push_back(std::move(track));
  Finalise(looping);

  SequenceGraph graph;
  graph.sequences.push_back(std::move(looping));

  const TimelineCompiler compiler;
  CompileOptions options;
  options.max_nesting_depth = 3;
  CHECK_NO_THROW(compiler.Compile(graph, Seconds(1), options));
}

// ---------------------------------------------------------------- read-ahead ----

CUTLINE_TEST(TimelineBoundariesMatchFullScanAcrossGapsTransitionsAndExactEdges) {
  SequenceGraph graph;
  for (int sequence_index = 0; sequence_index < 2; ++sequence_index) {
    auto sequence = MakeSequence("seq-" + std::to_string(sequence_index));
    auto track = MakeTrack("v1", model::TrackKind::Video, 0);
    track.muted = true;  // Boundary enumeration is independent of visibility.
    for (int index = 0; index < 8; ++index) {
      auto clip = MakeClip("clip-" + std::to_string(index), "media", index * 3 - 2, 2);
      clip.playback_rate = {3, 2};
      clip.enabled = index % 2 == 0;
      track.clips.push_back(std::move(clip));
    }
    // Transitions are allowed to be in arbitrary order and overlap clip edges.
    for (const int start : {7, -1, 19, 2}) {
      Transition transition;
      transition.timeline_start = {start * 3 + sequence_index, 3};
      transition.duration = {4, 3};
      track.transitions.push_back(std::move(transition));
    }
    sequence.tracks.push_back(std::move(track));
    sequence.tracks.push_back(MakeTrack("empty", model::TrackKind::Audio, 0));
    Finalise(sequence);
    graph.sequences.push_back(std::move(sequence));
  }
  // An edge below tick resolution must remain visible to the exact range API.
  auto tiny = MakeTrack("tiny", model::TrackKind::Video, 1);
  auto clip = MakeClip("tiny", "media", 1, 1);
  clip.source_out = {1, 1'000'000'000'000};
  tiny.clips.push_back(std::move(clip));
  graph.sequences.front().tracks.push_back(std::move(tiny));
  Finalise(graph.sequences.front());

  const auto verify = [&](const RationalTime& from, const RationalTime& until) {
    std::vector<RationalTime> expected{from};
    const auto consider = [&](const RationalTime& at) {
      if (at.Compare(from) > 0 && at.Compare(until) < 0) expected.push_back(at);
    };
    for (const auto& sequence : graph.sequences) {
      for (const auto& track : sequence.tracks) {
        for (const auto& candidate : track.clips) {
          consider(candidate.timeline_start);
          consider(candidate.end());
        }
        for (const auto& transition : track.transitions) {
          consider(transition.timeline_start);
          consider(transition.timeline_start.Add(transition.duration));
        }
      }
    }
    std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.Compare(b) < 0; });
    expected.erase(std::unique(expected.begin(), expected.end(),
                               [](const auto& a, const auto& b) { return a.Compare(b) == 0; }), expected.end());
    const auto actual = TimelineCompiler{}.BoundariesIn(graph, from, until);
    CHECK_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) CHECK_EQ(actual[index].Compare(expected[index]), 0);
  };
  for (int third = -9; third < 78; ++third) {
    for (const int span : {1, 3, 20, 90}) verify({third, 3}, {third + span, 3});
  }
  verify({999'999'999'999, 1'000'000'000'000}, {1'000'000'000'002, 1'000'000'000'000});
}

CUTLINE_TEST(CompileRangeReportsEverySourceInTheWindow) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 2));
  track.clips.push_back(MakeClip("clip-2", "media-2", 2, 2));
  track.clips.push_back(MakeClip("clip-3", "media-3", 4, 2));
  track.clips.push_back(MakeClip("clip-far", "media-4", 100, 2));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  const TimelineCompiler compiler;
  const auto requests = compiler.CompileRange(graph, Seconds(0), Seconds(6));
  const auto ids = ClipIds(requests);
  CHECK_EQ(requests.size(), std::size_t{3});
  CHECK(std::find(ids.begin(), ids.end(), "clip-1") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "clip-2") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "clip-3") != ids.end());
  // Outside the window, so not worth decoding yet.
  CHECK(std::find(ids.begin(), ids.end(), "clip-far") == ids.end());
}

CUTLINE_TEST(CompileRangeFindsClipsShorterThanAFrame) {
  // Sampling the window at a fixed stride would step over these.
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  for (std::int64_t index = 0; index < 20; ++index) {
    Clip clip;
    clip.id = "tiny-" + std::to_string(index);
    clip.source_id = "media-1";
    clip.source_in = Seconds(0);
    clip.source_out = {1, 1000};
    clip.timeline_start = {index, 1000};
    track.clips.push_back(std::move(clip));
  }
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  const TimelineCompiler compiler;
  const auto requests = compiler.CompileRange(graph, Seconds(0), Seconds(1));
  CHECK_EQ(requests.size(), std::size_t{20});
}

CUTLINE_TEST(CompileRangeRejectsAnEmptyWindow) {
  auto sequence = MakeSequence();
  sequence.tracks.push_back(MakeTrack("v1", model::TrackKind::Video, 0));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  const TimelineCompiler compiler;
  CHECK_THROWS(compiler.CompileRange(graph, Seconds(5), Seconds(5)));
  CHECK_THROWS(compiler.CompileRange(graph, Seconds(5), Seconds(1)));
}

CUTLINE_TEST(SequenceDurationIsTheEndOfItsLastClip) {
  auto sequence = MakeSequence();
  auto video = MakeTrack("v1", model::TrackKind::Video, 0);
  video.clips.push_back(MakeClip("clip-1", "media-1", 0, 5));
  auto audio = MakeTrack("a1", model::TrackKind::Audio, 0);
  audio.clips.push_back(MakeClip("clip-2", "media-1", 0, 12));
  sequence.tracks.push_back(std::move(video));
  sequence.tracks.push_back(std::move(audio));
  Finalise(sequence);

  CHECK_EQ(sequence.Duration().Compare(Seconds(12)), 0);
  CHECK(sequence.FindTrack("v1") != nullptr);
  CHECK(sequence.FindTrack("nope") == nullptr);
}

// ------------------------------------------------------- loader integration ----

namespace {

// Builds a real project and returns the store, so the loader is exercised
// against the actual schema rather than a hand-built snapshot.
std::unique_ptr<ProjectStore> BuildProject() {
  auto store = std::make_unique<ProjectStore>(":memory:");
  store->Initialize();

  int counter = 0;
  const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter);
    command.project_id = "project-1";
    command.author_id = "tester";
    command.base_revision = store->CurrentRevision();
    command.timestamp_utc = "2026-10-05T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter);
    const auto result = store->Execute(command);
    (void)result;
  };

  run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Loaded"});

  commands::ImportMediaPayload media;
  media.id = "media-1";
  media.display_name = "shot.mov";
  media.original_path = "/footage/shot.mov";
  media.fingerprint = "fp-1";
  media.duration = Seconds(60);
  run(commands::CommandType::ImportMedia, media);

  commands::CreateSequencePayload sequence;
  sequence.id = "seq-1";
  sequence.settings.name = "Main";
  sequence.settings.frame_rate = rates::kFrameRate2997;
  sequence.settings.width = 1920;
  sequence.settings.height = 1080;
  sequence.settings.sample_rate = 48000;
  sequence.settings.working_color_space = "rec2020";
  run(commands::CommandType::CreateSequence, sequence);

  // Two video tracks, created out of order to prove the loader sorts them.
  run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
  run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
  run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});

  const auto clip = [](std::string id, std::string track, std::int64_t start, std::int64_t length) {
    commands::InsertClipPayload payload;
    payload.id = std::move(id);
    payload.track_id = std::move(track);
    payload.source_kind = model::SourceKind::Media;
    payload.media_id = "media-1";
    payload.source_in = Seconds(0);
    payload.source_out = Seconds(length);
    payload.timeline_start = Seconds(start);
    return payload;
  };
  // Inserted out of timeline order on purpose.
  run(commands::CommandType::InsertClip, clip("clip-late", "v1", 20, 5));
  run(commands::CommandType::InsertClip, clip("clip-early", "v1", 0, 5));
  run(commands::CommandType::InsertClip, clip("clip-upper", "v2", 0, 5));
  run(commands::CommandType::InsertClip, clip("clip-audio", "a1", 0, 5));

  commands::AddEffectPayload effect;
  effect.id = "fx-1";
  effect.owner_kind = model::EffectOwner::Clip;
  effect.owner_id = "clip-early";
  effect.effect_type = "opacity";
  effect.preset_name = "looks/invert.cube";
  effect.parameters = {{"fx-1:value", "value", Value::Scalar(1.0)}};
  run(commands::CommandType::AddEffect, effect);

  commands::SetKeyframePayload first;
  first.parameter_id = "fx-1:value";
  first.keyframe = {Seconds(0), Value::Scalar(0.0), Interpolation::Linear, {}, {}};
  run(commands::CommandType::SetKeyframe, first);
  commands::SetKeyframePayload second;
  second.parameter_id = "fx-1:value";
  second.keyframe = {Seconds(4), Value::Scalar(1.0), Interpolation::Linear, {}, {}};
  run(commands::CommandType::SetKeyframe, second);

  cutline::effects::mask::Document mask;
  mask.shape = cutline::effects::mask::Shape::Ellipse;
  mask.animations.push_back({"center_x", "linear", {{0.0, 0.25}, {4.0, 0.75}}});
  run(commands::CommandType::AddMask,
      commands::AddMaskPayload{"mask-1", "fx-1", 0, cutline::effects::mask::ToJson(mask)});

  commands::AddTransitionPayload transition;
  transition.id = "t-1";
  transition.track_id = "v1";
  transition.kind = "cross_dissolve";
  transition.to_clip_id = "clip-early";
  // A fade in from nothing begins at the clip's first frame, so it is aligned to
  // *start* there. It used to be declared centre-aligned, which would put it
  // half before the clip; the store now enforces that a transition is placed
  // the way it says it is.
  transition.alignment = model::TransitionAlignment::Start;
  transition.timeline_start = Seconds(0);
  transition.duration = Seconds(2);
  run(commands::CommandType::AddTransition, transition);

  return store;
}

}  // namespace

CUTLINE_TEST(LoaderBuildsASnapshotThatCompiles) {
  auto store = BuildProject();
  const auto graph = cutline::timeline::LoadSequenceGraph(*store, "seq-1");
  CHECK_EQ(graph.sequences.size(), std::size_t{1});

  const auto* sequence = graph.root();
  CHECK(sequence != nullptr);
  CHECK_EQ(sequence->working_color_space, std::string("rec2020"));
  CHECK_EQ(sequence->tracks.size(), std::size_t{3});
  // Video tracks first, ascending by order, then audio.
  CHECK_EQ(sequence->tracks[0].id, std::string("v1"));
  CHECK_EQ(sequence->tracks[1].id, std::string("v2"));
  CHECK_EQ(sequence->tracks[2].id, std::string("a1"));

  // Clips arrive sorted by start time regardless of insertion order.
  const auto* v1 = sequence->FindTrack("v1");
  CHECK_EQ(v1->clips.size(), std::size_t{2});
  CHECK_EQ(v1->clips[0].id, std::string("clip-early"));
  CHECK_EQ(v1->clips[1].id, std::string("clip-late"));
  CHECK_EQ(v1->transitions.size(), std::size_t{1});

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(graph, Seconds(3));
  CHECK_EQ(plan.sequence_id, std::string("seq-1"));
  CHECK_EQ(plan.video.size(), std::size_t{2});
  CHECK_EQ(plan.video[0].track_id, std::string("v1"));
  CHECK_EQ(plan.video[1].track_id, std::string("v2"));
  CHECK_EQ(plan.audio.size(), std::size_t{1});
}

CUTLINE_TEST(LoadedKeyframesSampleCorrectly) {
  auto store = BuildProject();
  const auto graph = cutline::timeline::LoadSequenceGraph(*store, "seq-1");
  const TimelineCompiler compiler;

  // clip-early starts at 0 and its opacity ramps 0 -> 1 over 4s.
  const auto plan = compiler.Compile(graph, Seconds(2));
  bool checked = false;
  for (const auto& request : plan.video) {
    if (request.clip_id != "clip-early") continue;
    CHECK_EQ(request.effects.size(), std::size_t{1});
    CHECK_EQ(request.effects[0].preset_name, std::string("looks/invert.cube"));
    CHECK_EQ(request.effects[0].parameters.size(), std::size_t{1});
    CHECK(std::abs(request.effects[0].parameters[0].value.scalar() - 0.5) < 1e-9);
    CHECK_EQ(request.effects[0].masks.size(), std::size_t{1});
    CHECK(std::abs(request.effects[0].masks[0].document.center_x - 0.5) < 1e-9);
    checked = true;
  }
  CHECK(checked);
}

CUTLINE_TEST(LoadedTransitionsAreActiveAtTheRightTimes) {
  auto store = BuildProject();
  const auto graph = cutline::timeline::LoadSequenceGraph(*store, "seq-1");
  const TimelineCompiler compiler;
  CHECK_EQ(compiler.Compile(graph, Seconds(1)).transitions.size(), std::size_t{1});
  CHECK(compiler.Compile(graph, Seconds(3)).transitions.empty());
}

CUTLINE_TEST(LoaderReflectsEditsAfterAReload) {
  auto store = BuildProject();
  const auto before = cutline::timeline::LoadSequenceGraph(*store, "seq-1");
  CHECK_EQ(before.root()->FindTrack("v1")->clips.size(), std::size_t{2});

  commands::CommandEnvelope remove;
  remove.command_id = "cmd-remove";
  remove.project_id = "project-1";
  remove.author_id = "tester";
  remove.base_revision = store->CurrentRevision();
  remove.timestamp_utc = "2026-10-05T01:00:00Z";
  remove.type = commands::CommandType::DeleteClip;
  remove.payload = commands::DeleteClipPayload{"clip-late"};
  remove.idempotency_key = "key-remove";
  const auto result = store->Execute(remove);
  (void)result;

  const auto after = cutline::timeline::LoadSequenceGraph(*store, "seq-1");
  CHECK_EQ(after.root()->FindTrack("v1")->clips.size(), std::size_t{1});
  // The snapshot records the revision it was taken at, so a cache can tell it
  // has gone stale without diffing the content.
  CHECK(after.root()->source_revision > before.root()->source_revision);
}

CUTLINE_TEST(LoadingAnUnknownSequenceFails) {
  auto store = BuildProject();
  CHECK_THROWS(cutline::timeline::LoadSequenceGraph(*store, "seq-missing"));
}

CUTLINE_TEST(LoaderFollowsNestedSequences) {
  auto store = BuildProject();

  int counter = 1000;
  const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter);
    command.project_id = "project-1";
    command.author_id = "tester";
    command.base_revision = store->CurrentRevision();
    command.timestamp_utc = "2026-10-05T02:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter);
    const auto result = store->Execute(command);
    (void)result;
  };

  commands::CreateSequencePayload outer;
  outer.id = "seq-outer";
  outer.settings.name = "Outer";
  outer.settings.frame_rate = rates::kFrameRate2997;
  outer.settings.width = 1920;
  outer.settings.height = 1080;
  outer.settings.sample_rate = 48000;
  run(commands::CommandType::CreateSequence, outer);
  run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"ov1", "seq-outer", 0, "stereo", "V1"});

  commands::InsertClipPayload nested;
  nested.id = "nest-1";
  nested.track_id = "ov1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.nested_sequence_id = "seq-1";
  nested.source_in = Seconds(0);
  nested.source_out = Seconds(25);
  nested.timeline_start = Seconds(0);
  run(commands::CommandType::InsertClip, nested);

  const auto graph = cutline::timeline::LoadSequenceGraph(*store, "seq-outer");
  CHECK_EQ(graph.sequences.size(), std::size_t{2});
  CHECK_EQ(graph.root()->id, std::string("seq-outer"));
  CHECK(graph.Find("seq-1") != nullptr);

  const TimelineCompiler compiler;
  const auto plan = compiler.Compile(graph, Seconds(2));
  const auto ids = ClipIds(plan.video);
  CHECK(std::find(ids.begin(), ids.end(), "nest-1") != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "clip-early") != ids.end());
}

// ------------------------------------------------- compile cost and loading ----

CUTLINE_TEST(CompilingASequenceInPlaceMatchesCompilingItsGraph) {
  // The single-sequence overload used to wrap a copy of the sequence in a graph.
  // Compiling in place must give exactly the same plan.
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 5));
  track.clips.push_back(MakeClip("clip-2", "media-2", 5, 5));
  Effect fx;
  fx.id = "fx-1";
  fx.effect_type = "opacity";
  track.clips[0].effects.push_back(fx);
  sequence.tracks.push_back(std::move(track));
  auto audio = MakeTrack("a1", model::TrackKind::Audio, 0);
  audio.clips.push_back(MakeClip("clip-a", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(audio));
  Finalise(sequence);

  SequenceGraph graph;
  graph.sequences.push_back(sequence);

  const TimelineCompiler compiler;
  for (const std::int64_t second : {0, 3, 5, 9}) {
    const auto direct = compiler.Compile(sequence, Seconds(second));
    const auto via_graph = compiler.Compile(graph, Seconds(second));
    CHECK_EQ(ClipIds(direct.video).size(), ClipIds(via_graph.video).size());
    CHECK_EQ(ClipIds(direct.audio).size(), ClipIds(via_graph.audio).size());
    for (std::size_t index = 0; index < direct.video.size(); ++index) {
      CHECK_EQ(direct.video[index].clip_id, via_graph.video[index].clip_id);
      CHECK_EQ(direct.video[index].source_time.Compare(via_graph.video[index].source_time), 0);
      CHECK_EQ(direct.video[index].effects.size(), via_graph.video[index].effects.size());
    }
  }
}

CUTLINE_TEST(PerFrameCompileCostDoesNotGrowWithTheNumberOfClips) {
  // Binary search makes finding the active clip logarithmic, but only if nothing
  // else touches every clip. A hidden whole-sequence copy per frame made this
  // linear in project size: 2 ms per frame at 10,000 clips. The check is a
  // generous ratio rather than an absolute time, so it does not depend on the
  // machine, yet fails by two orders of magnitude if a copy returns.
  const auto build = [](std::int64_t clips) {
    auto sequence = MakeSequence();
    auto track = MakeTrack("v1", model::TrackKind::Video, 0);
    for (std::int64_t index = 0; index < clips; ++index) {
      auto clip = MakeClip("clip-" + std::to_string(index), "media-1", index, 1);
      Effect fx;
      fx.id = "fx-" + std::to_string(index);
      fx.effect_type = "opacity";
      clip.effects.push_back(std::move(fx));
      track.clips.push_back(std::move(clip));
    }
    sequence.tracks.push_back(std::move(track));
    Finalise(sequence);
    return sequence;
  };
  const auto small = build(10);
  const auto large = build(20000);

  const TimelineCompiler compiler;
  const auto measure = [&compiler](const Sequence& sequence, std::int64_t clips) {
    constexpr int kIterations = 300;
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < kIterations; ++index) {
      const auto plan = compiler.Compile(sequence, Seconds(index % clips));
      if (plan.video.size() != 1) throw std::runtime_error("expected one active clip");
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  const auto small_cost = measure(small, 10);
  const auto large_cost = measure(large, 20000);
  CHECK(large_cost < small_cost * 50.0 + 0.05);
}

CUTLINE_TEST(LoadingASequenceDoesNotReadOtherSequencesEffects) {
  // The parameter and keyframe queries used to read the whole project, so every
  // load -- and every nested sequence within it -- paid for every other
  // sequence's animation. They are now scoped to the sequence being loaded.
  auto store = BuildProject();

  int counter = 3000;
  const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter);
    command.project_id = "project-1";
    command.author_id = "tester";
    command.base_revision = store->CurrentRevision();
    command.timestamp_utc = "2026-10-06T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter);
    const auto result = store->Execute(command);
    (void)result;
  };

  // A second sequence carrying a good deal of animation of its own.
  commands::CreateSequencePayload other;
  other.id = "seq-other";
  other.settings.name = "Other";
  other.settings.frame_rate = rates::kFrameRate2997;
  other.settings.width = 1920;
  other.settings.height = 1080;
  other.settings.sample_rate = 48000;
  run(commands::CommandType::CreateSequence, other);
  run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"ov1", "seq-other", 0, "stereo", "V1"});
  commands::InsertClipPayload clip;
  clip.id = "other-clip";
  clip.track_id = "ov1";
  clip.source_kind = model::SourceKind::Media;
  clip.media_id = "media-1";
  clip.source_in = Seconds(0);
  clip.source_out = Seconds(5);
  clip.timeline_start = Seconds(0);
  run(commands::CommandType::InsertClip, clip);
  for (int effect_index = 0; effect_index < 4; ++effect_index) {
    commands::AddEffectPayload effect;
    effect.id = "other-fx-" + std::to_string(effect_index);
    effect.owner_kind = model::EffectOwner::Clip;
    effect.owner_id = "other-clip";
    effect.effect_type = "grade";
    effect.order = effect_index;
    effect.parameters = {{effect.id + ":a", "exposure", Value::Scalar(0.0)},
                         {effect.id + ":b", "contrast", Value::Scalar(100.0)}};
    run(commands::CommandType::AddEffect, effect);
    for (int key = 0; key < 3; ++key) {
      commands::SetKeyframePayload keyframe;
      keyframe.parameter_id = effect.id + ":a";
      keyframe.keyframe = {Seconds(key), Value::Scalar(0.1 * key), Interpolation::Linear, {}, {}};
      run(commands::CommandType::SetKeyframe, keyframe);
    }
  }

  // seq-1 owns exactly one effect with one parameter and two keyframes.
  cutline::timeline::LoadStatistics statistics;
  {
    const std::lock_guard<std::mutex> lock(store->mutex());
    const auto sequence = cutline::timeline::LoadSequence(store->connection(), "seq-1", &statistics);
    CHECK_EQ(sequence.id, std::string("seq-1"));
  }
  CHECK_EQ(statistics.effects, std::int64_t{1});
  CHECK_EQ(statistics.parameters, std::int64_t{1});
  CHECK_EQ(statistics.keyframes, std::int64_t{2});

  // And the other sequence's own load reports its own, larger, set.
  cutline::timeline::LoadStatistics other_statistics;
  {
    const std::lock_guard<std::mutex> lock(store->mutex());
    const auto sequence = cutline::timeline::LoadSequence(store->connection(), "seq-other", &other_statistics);
    CHECK_EQ(sequence.id, std::string("seq-other"));
  }
  CHECK_EQ(other_statistics.effects, std::int64_t{4});
  CHECK_EQ(other_statistics.parameters, std::int64_t{8});
  CHECK_EQ(other_statistics.keyframes, std::int64_t{12});
}

// ----------------------------------------- editing preserves what is rendered ----
//
// These compare the compiled result *before* an edit with the result *after* it,
// sampled across the whole clip. That is the property a razor cut must have --
// the picture and the animation are the same either side of it -- and it is not
// something a test of the stored rows can show: the rows can look plausible and
// still play differently.

namespace {

// A project driven only through the command service, with a compiled view of it.
class Editor final {
 public:
  Editor() : store_(std::make_unique<ProjectStore>(":memory:")) {
    store_->Initialize();
    Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Edit"});
    commands::ImportMediaPayload media;
    media.id = "media-1";
    media.display_name = "m.mov";
    media.original_path = "/m.mov";
    media.fingerprint = "fp";
    media.duration = Seconds(120);
    Run(commands::CommandType::ImportMedia, media);
    commands::CreateSequencePayload sequence;
    sequence.id = "seq-1";
    sequence.settings.name = "Main";
    sequence.settings.frame_rate = {25, 1};
    sequence.settings.width = 1920;
    sequence.settings.height = 1080;
    sequence.settings.sample_rate = 48000;
    Run(commands::CommandType::CreateSequence, sequence);
    Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
  }

  void Run(commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter_);
    command.project_id = "project-1";
    command.author_id = "tester";
    command.base_revision = store_->CurrentRevision();
    command.timestamp_utc = "2026-10-06T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter_);
    const auto result = store_->Execute(command);
    (void)result;
  }

  void InsertClip(const std::string& id, std::int64_t start, std::int64_t source_in, std::int64_t source_out,
                  RationalTime rate = {1, 1}, bool reversed = false) {
    commands::InsertClipPayload clip;
    clip.id = id;
    clip.track_id = "v1";
    clip.source_kind = model::SourceKind::Media;
    clip.media_id = "media-1";
    clip.source_in = Seconds(source_in);
    clip.source_out = Seconds(source_out);
    clip.timeline_start = Seconds(start);
    clip.playback_rate = rate;
    clip.reversed = reversed;
    Run(commands::CommandType::InsertClip, clip);
  }

  // One effect with one parameter, keyed at the given clip-local times.
  void Animate(const std::string& clip_id, const std::string& effect_type, const std::string& parameter,
               const Value& constant, const std::vector<cutline::anim::Keyframe>& keys) {
    commands::AddEffectPayload effect;
    effect.id = clip_id + "-" + effect_type;
    effect.owner_kind = model::EffectOwner::Clip;
    effect.owner_id = clip_id;
    effect.effect_type = effect_type;
    effect.parameters = {{effect.id + ":" + parameter, parameter, constant}};
    Run(commands::CommandType::AddEffect, effect);
    for (const auto& key : keys) {
      commands::SetKeyframePayload keyframe;
      keyframe.parameter_id = effect.id + ":" + parameter;
      keyframe.keyframe = key;
      Run(commands::CommandType::SetKeyframe, keyframe);
    }
  }

  // What the compiler reports at a time: the active clip's source time and every
  // sampled effect value, flattened so the two halves of a split compare equal
  // even though their effect ids differ.
  struct Sample final {
    RationalTime source_time;
    std::map<std::string, double> values;
  };

  using Samples = std::vector<std::optional<Sample>>;

  [[nodiscard]] Samples Capture(const std::vector<RationalTime>& times) const {
    const auto graph = cutline::timeline::LoadSequenceGraph(*store_, "seq-1");
    const TimelineCompiler compiler;
    Samples samples;
    samples.reserve(times.size());
    for (const auto& time : times) {
      const auto plan = compiler.Compile(graph, time);
      if (plan.video.size() != 1) {
        samples.emplace_back(std::nullopt);
        continue;
      }
      Sample sample;
      sample.source_time = plan.video[0].source_time;
      for (const auto& effect : plan.video[0].effects) {
        for (const auto& parameter : effect.parameters) {
          for (int component = 0; component < parameter.value.dimension; ++component) {
            sample.values[effect.effect_type + "." + parameter.name + "#" + std::to_string(component)] =
                parameter.value.components[static_cast<std::size_t>(component)];
          }
        }
      }
      samples.emplace_back(std::move(sample));
    }
    return samples;
  }

  [[nodiscard]] ProjectStore& store() { return *store_; }

 private:
  std::unique_ptr<ProjectStore> store_;
  int counter_{0};
};

// Every 1/100 s across [from, to), which includes off-frame times.
std::vector<RationalTime> Grid(std::int64_t from_seconds, std::int64_t to_seconds) {
  std::vector<RationalTime> times;
  for (std::int64_t tick = from_seconds * 100; tick < to_seconds * 100; ++tick) times.emplace_back(tick, 100);
  return times;
}

// Reports the first disagreement between two captures, or "" when they match.
std::string FirstDifference(const Editor::Samples& before, const Editor::Samples& after,
                            const std::vector<RationalTime>& times, double tolerance) {
  for (std::size_t index = 0; index < before.size(); ++index) {
    const auto where = std::to_string(times[index].numerator()) + "/" + std::to_string(times[index].denominator());
    if (before[index].has_value() != after[index].has_value()) {
      return "clip presence differs at " + where + "s";
    }
    if (!before[index].has_value()) continue;
    if (before[index]->source_time.Compare(after[index]->source_time) != 0) {
      return "source time differs at " + where + "s: " + std::to_string(before[index]->source_time.numerator()) +
             "/" + std::to_string(before[index]->source_time.denominator()) + " became " +
             std::to_string(after[index]->source_time.numerator()) + "/" +
             std::to_string(after[index]->source_time.denominator());
    }
    if (before[index]->values.size() != after[index]->values.size()) return "effect set differs at " + where + "s";
    for (const auto& [name, value] : before[index]->values) {
      const auto found = after[index]->values.find(name);
      if (found == after[index]->values.end()) return name + " missing at " + where + "s";
      // Relative to the value's size: the curve solver converges x to 1e-7, which a
      // value range in the hundreds (a 300-pixel move) turns into a larger absolute
      // error that is still a few parts in 10^9.
      if (std::abs(found->second - value) > tolerance * std::max(1.0, std::abs(value))) {
        return name + " differs at " + where + "s: " + std::to_string(value) + " became " +
               std::to_string(found->second);
      }
    }
  }
  return {};
}

}  // namespace

CUTLINE_TEST(SplittingAClipLeavesItsPlaybackUnchangedAtEveryRateAndDirection) {
  // The split mapped source time forward for every clip. For a reversed clip the
  // left half should keep the *tail* of the source range and the right half the
  // head, so a cut at timeline 4 s changed what played at 1 s from source 9 s to
  // source 3 s. Forward clips at other rates are covered too, so the fix cannot
  // break the case that already worked.
  struct Case final {
    RationalTime rate;
    bool reversed;
  };
  const Case cases[] = {{{1, 1}, false}, {{1, 1}, true}, {{2, 1}, false}, {{2, 1}, true},
                        {{1, 2}, false}, {{1, 2}, true}, {{3, 2}, true}};
  // Where to cut, as a fraction of the clip's timeline length, plus the two
  // extremes that are easy to get wrong: one frame in, and one frame from the end.
  enum class Where { OneFrameIn, Quarter, Middle, OneFrameFromEnd };

  for (const auto& test : cases) {
    // Source 5..25 is 20 s of media. At rate r the clip occupies 20/r seconds.
    const auto length = Seconds(20).Divide(test.rate);
    for (const auto where : {Where::OneFrameIn, Where::Quarter, Where::Middle, Where::OneFrameFromEnd}) {
      Editor editor;
      const std::int64_t start = 7;  // not at zero, so absolute and clip-local time differ
      editor.InsertClip("clip", start, 5, 25, test.rate, test.reversed);

      RationalTime at;
      switch (where) {
        case Where::OneFrameIn: at = Seconds(start).Add({1, 25}); break;
        case Where::Quarter: at = Seconds(start).Add(length.Divide(4)); break;
        case Where::Middle: at = Seconds(start).Add(length.Divide(2)); break;
        case Where::OneFrameFromEnd: at = Seconds(start).Add(length).Subtract({1, 25}); break;
      }

      const auto times = Grid(0, 70);
      const auto before = editor.Capture(times);
      editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-b", at});
      const auto after = editor.Capture(times);

      const auto difference = FirstDifference(before, after, times, 0.0);
      if (!difference.empty()) {
        cutline::testing::Fail("split changed playback", __FILE__, __LINE__,
                               difference + " (rate " + std::to_string(test.rate.numerator()) + "/" +
                                   std::to_string(test.rate.denominator()) + ", reversed " +
                                   (test.reversed ? "yes" : "no") + ")");
      }
      CHECK_NO_THROW(editor.store().ValidateDatabase());
    }
  }
}

CUTLINE_TEST(SplittingAReversedClipGivesTheLeftHalfTheTailOfItsSource) {
  // The same property stated structurally, so a failure points at the rows.
  Editor editor;
  editor.InsertClip("clip", 0, 0, 10, {1, 1}, true);  // plays source 10 -> 0 over timeline 0 -> 10
  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-b", Seconds(4)});

  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto& clips = graph.root()->FindTrack("v1")->clips;
  CHECK_EQ(clips.size(), std::size_t{2});
  // Left half, timeline 0..4, plays source 10 down to 6.
  CHECK_EQ(clips[0].id, std::string("clip"));
  CHECK_EQ(clips[0].source_in.Compare(Seconds(6)), 0);
  CHECK_EQ(clips[0].source_out.Compare(Seconds(10)), 0);
  // Right half, timeline 4..10, plays source 6 down to 0.
  CHECK_EQ(clips[1].id, std::string("clip-b"));
  CHECK_EQ(clips[1].source_in.Compare(Seconds(0)), 0);
  CHECK_EQ(clips[1].source_out.Compare(Seconds(6)), 0);
  CHECK(clips[0].reversed && clips[1].reversed);
}

CUTLINE_TEST(SplittingAnAnimatedClipPreservesItsAnimationForEveryInterpolation) {
  // Keyframes are in clip-local time and the right half starts later, so copying
  // them unchanged restarted its animation: a 0 -> 1 ramp read 0.5 before the cut
  // and 0.1 after it. The curves have to be split, not just copied -- including
  // the eased and Bezier ones, whose shape depends on where in the segment the
  // cut falls.
  using cutline::anim::Interpolation;
  using cutline::anim::Keyframe;
  using cutline::anim::Value;

  const Interpolation modes[] = {Interpolation::Hold,     Interpolation::Linear,    Interpolation::EaseIn,
                                 Interpolation::EaseOut,  Interpolation::EaseInOut, Interpolation::Bezier};
  // Clip-local seconds at which to cut: before the first key (2), exactly on a
  // key, inside each segment, exactly on the last key, and after it.
  const std::int64_t cuts_in_tenths[] = {10, 20, 35, 60, 75, 90, 95};

  for (const auto mode : modes) {
    for (const auto cut_tenths : cuts_in_tenths) {
      Editor editor;
      editor.InsertClip("clip", 3, 10, 20);  // timeline 3..13, local time 0..10
      const auto key = [&](std::int64_t tenths, double value) {
        // An asymmetric Bezier with overshoot, so a wrong split cannot hide.
        return Keyframe{{tenths, 10}, Value::Scalar(value), mode, {0.2, 0.7}, {0.8, 1.4}};
      };
      editor.Animate("clip", "opacity", "value", Value::Scalar(0.5),
                     {key(20, 0.1), key(60, 0.9), key(90, 0.3)});

      const auto cut = Seconds(3).Add({cut_tenths, 10});
      const auto times = Grid(0, 16);
      const auto before = editor.Capture(times);
      editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-b", cut});
      const auto after = editor.Capture(times);

      const auto difference = FirstDifference(before, after, times, 1e-6);
      if (!difference.empty()) {
        cutline::testing::Fail("split changed the animation", __FILE__, __LINE__,
                               difference + " (mode " + cutline::anim::ToString(mode) + ", cut at local " +
                                   std::to_string(cut_tenths) + "/10 s)");
      }
    }
  }
}

CUTLINE_TEST(SplittingPreservesMultiComponentAnimationOnARetimedClip) {
  // Position is two components, and the clip plays at 2x so its timeline length
  // is half its source length: animation follows timeline time, not source time.
  using cutline::anim::Interpolation;
  using cutline::anim::Keyframe;
  using cutline::anim::Value;

  Editor editor;
  editor.InsertClip("clip", 5, 0, 20, {2, 1});  // 10 s on the timeline, 5..15
  editor.Animate("clip", "motion", "position", Value::Vec2(0, 0),
                 {{{0, 1}, Value::Vec2(0, 0), Interpolation::EaseInOut, {}, {}},
                  {{4, 1}, Value::Vec2(300, -80), Interpolation::Linear, {}, {}},
                  {{9, 1}, Value::Vec2(-40, 120), Interpolation::EaseOut, {}, {}}});

  const auto times = Grid(0, 20);
  const auto before = editor.Capture(times);
  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-b", Seconds(5).Add({13, 4})});
  const auto after = editor.Capture(times);
  CHECK_EQ(FirstDifference(before, after, times, 1e-6), std::string());
}

CUTLINE_TEST(SplittingTwiceStillPreservesTheAnimation) {
  // Splitting an already-split clip rebases a second time. An error that only
  // shows when the origin is not the first key would appear here.
  using cutline::anim::Interpolation;
  using cutline::anim::Keyframe;
  using cutline::anim::Value;

  Editor editor;
  editor.InsertClip("clip", 2, 0, 12);
  editor.Animate("clip", "opacity", "value", Value::Scalar(1.0),
                 {{{0, 1}, Value::Scalar(0.0), Interpolation::EaseInOut, {}, {}},
                  {{12, 1}, Value::Scalar(1.0), Interpolation::EaseInOut, {}, {}}});

  const auto times = Grid(0, 16);
  const auto before = editor.Capture(times);
  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-b", Seconds(5)});
  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-b", "clip-c", Seconds(9).Add({1, 3})});
  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-d", Seconds(3).Add({1, 7})});
  CHECK_EQ(FirstDifference(before, editor.Capture(times), times, 1e-6), std::string());
  CHECK_NO_THROW(editor.store().ValidateDatabase());
}

CUTLINE_TEST(RippleDeleteShiftsTransitionsByTheClipsTheyAreAttachedTo) {
  // Transitions were shifted when their *start* was at or after the gap's end.
  // A centre-aligned fade-in on the first clip after the gap starts half a
  // transition before that clip, so it was left behind while its clip moved.
  Editor editor;
  editor.InsertClip("a", 0, 0, 3);
  editor.InsertClip("x", 3, 10, 15);   // the clip about to be removed: timeline 3..8
  editor.InsertClip("b", 8, 20, 25);   // timeline 8..13

  commands::AddTransitionPayload fade;
  fade.id = "fade-in";
  fade.track_id = "v1";
  fade.kind = "cross_dissolve";
  fade.alignment = model::TransitionAlignment::Center;
  fade.to_clip_id = "b";
  fade.timeline_start = Seconds(7).Add({1, 2});  // 7.5 .. 8.5, centred on b's start
  fade.duration = Seconds(1);
  editor.Run(commands::CommandType::AddTransition, fade);

  editor.Run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{"x"});

  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto& track = *graph.root()->FindTrack("v1");
  CHECK_EQ(track.clips.size(), std::size_t{2});
  CHECK_EQ(track.clips[1].id, std::string("b"));
  CHECK_EQ(track.clips[1].timeline_start.Compare(Seconds(3)), 0);
  // b moved from 8 to 3, so its fade must now run 2.5 .. 3.5.
  CHECK_EQ(track.transitions.size(), std::size_t{1});
  CHECK_EQ(track.transitions[0].timeline_start.Compare(Seconds(2).Add({1, 2})), 0);
  CHECK_NO_THROW(editor.store().ValidateDatabase());
}

CUTLINE_TEST(UndoingASplitRestoresPlaybackAndAnimationExactly) {
  // The split rewrites keyframes in place on the left half and creates them on
  // the right, so undo has to put every one back, with its handles.
  using cutline::anim::Interpolation;
  using cutline::anim::Keyframe;
  using cutline::anim::Value;

  Editor editor;
  editor.InsertClip("clip", 4, 10, 30, {1, 1}, true);
  editor.Animate("clip", "opacity", "value", Value::Scalar(1.0),
                 {{{1, 1}, Value::Scalar(0.1), Interpolation::Bezier, {0.2, 0.7}, {0.8, 1.4}},
                  {{8, 1}, Value::Scalar(0.9), Interpolation::EaseInOut, {}, {}},
                  {{15, 1}, Value::Scalar(0.2), Interpolation::Linear, {}, {}}});

  const auto times = Grid(0, 30);
  const auto original = editor.Capture(times);

  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-b", Seconds(10).Add({1, 3})});
  const auto split = editor.Capture(times);
  CHECK_EQ(FirstDifference(original, split, times, 1e-6), std::string());

  const auto undone = editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  (void)undone;
  CHECK_EQ(FirstDifference(original, editor.Capture(times), times, 0.0), std::string());
  CHECK_NO_THROW(editor.store().ValidateDatabase());

  const auto redone = editor.store().Redo("tester", "2026-10-06T02:00:00Z");
  (void)redone;
  CHECK_EQ(FirstDifference(split, editor.Capture(times), times, 0.0), std::string());
  CHECK_NO_THROW(editor.store().ValidateDatabase());
}

// ------------------------------------------------ trimming and animation ----
//
// Keyframes live in clip-local time, which starts at the clip's first frame. A head
// trim moves that origin without moving the picture, so the store moves the
// keyframes back by the same amount: the same source frame still plays at the same
// timeline time with the same animated value. (Before this, an opacity ramp slid
// against the picture by the length of the trim.)

namespace {

void AnimateOpacity(Editor& editor) {
  using cutline::anim::Interpolation;
  editor.Animate("clip", "opacity", "value", cutline::anim::Value::Scalar(1.0),
                 {{{1, 1}, cutline::anim::Value::Scalar(0.1), Interpolation::Linear, {}, {}},
                  {{3, 1}, cutline::anim::Value::Scalar(0.9), Interpolation::Bezier, {0.2, 0.7}, {0.8, 1.4}},
                  {{6, 1}, cutline::anim::Value::Scalar(0.3), Interpolation::EaseInOut, {}, {}}});
}

std::vector<RationalTime> GridBetween(const RationalTime& from, const RationalTime& until) {
  std::vector<RationalTime> times;
  for (auto time = from; time.Compare(until) < 0; time = time.Add({1, 100})) times.push_back(time);
  return times;
}

}  // namespace

CUTLINE_TEST(TrimmingAClipsHeadKeepsEveryKeyframeOnTheFrameItWasOn) {
  struct Case final {
    RationalTime rate;
    bool reversed;
    std::int64_t head_trim_seconds;  // negative extends the head
  };
  for (const auto& test : {Case{{1, 1}, false, 2}, Case{{2, 1}, false, 3}, Case{{1, 1}, true, 2}, Case{{3, 2}, true, 2},
                           Case{{1, 2}, false, 1}, Case{{1, 1}, false, -1}, Case{{2, 1}, true, -1}}) {
    Editor editor;
    editor.InsertClip("clip", 4, 10, 30, test.rate, test.reversed);
    AnimateOpacity(editor);

    // The clip occupies [4, 4 + 20 / rate). Trim d seconds of timeline off the head.
    const auto end = RationalTime(4, 1).Add(Seconds(20).Divide(test.rate));
    const auto trimmed = Seconds(test.head_trim_seconds);
    const auto new_start = RationalTime(4, 1).Add(trimmed);
    const auto source_change = trimmed.Multiply(test.rate);
    const auto new_in = test.reversed ? Seconds(10) : Seconds(10).Add(source_change);
    const auto new_out = test.reversed ? Seconds(30).Subtract(source_change) : Seconds(30);

    // Compare over the part of the clip that existed both before and after.
    const auto from = test.head_trim_seconds > 0 ? new_start : RationalTime(4, 1);
    const auto times = GridBetween(from, end);
    const auto before = editor.Capture(times);
    editor.Run(commands::CommandType::TrimClip, commands::TrimClipPayload{"clip", new_in, new_out, new_start});
    const auto after = editor.Capture(times);
    const auto difference = FirstDifference(before, after, times, 1e-9);
    if (!difference.empty()) {
      cutline::testing::Fail("head trim moved the animation", __FILE__, __LINE__,
                             "rate " + std::to_string(test.rate.numerator()) + "/" + std::to_string(test.rate.denominator()) +
                                 (test.reversed ? " reversed" : "") + ", trim " + std::to_string(test.head_trim_seconds) +
                                 " s: " + difference);
    }
    CHECK_NO_THROW(editor.store().ValidateDatabase());
  }
}

CUTLINE_TEST(OnlyAHeadTrimMovesKeyframesAndUndoPutsThemBack) {
  using cutline::anim::Value;
  Editor editor;
  editor.InsertClip("clip", 4, 10, 30);
  AnimateOpacity(editor);
  const auto times = GridBetween({4, 1}, {24, 1});
  const auto original = editor.Capture(times);

  // A tail trim leaves the head, and so the animation, exactly where it was.
  editor.Run(commands::CommandType::TrimClip, commands::TrimClipPayload{"clip", Seconds(10), Seconds(25), Seconds(4)});
  const auto tail_times = GridBetween({4, 1}, {19, 1});
  const auto tail = editor.Capture(tail_times);
  const Editor::Samples head_of_original(original.begin(), original.begin() + 1500);
  CHECK_EQ(FirstDifference(head_of_original, tail, tail_times, 1e-12), std::string());

  // A slip changes what plays, not when the animation runs: values by timeline
  // time are unchanged, source times are not.
  editor.Run(commands::CommandType::TrimClip, commands::TrimClipPayload{"clip", Seconds(12), Seconds(27), Seconds(4)});
  const auto slipped = editor.Capture(GridBetween({4, 1}, {19, 1}));
  for (std::size_t index = 0; index < 1500; ++index) {
    CHECK(slipped[index].has_value());
    CHECK(slipped[index]->values == original[index]->values);
    CHECK(slipped[index]->source_time.Compare(original[index]->source_time) != 0);
  }

  // A head trim and its undo.
  editor.Run(commands::CommandType::TrimClip, commands::TrimClipPayload{"clip", Seconds(14), Seconds(27), Seconds(6)});
  const auto trimmed = editor.Capture(GridBetween({6, 1}, {19, 1}));
  const auto undone = editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  (void)undone;
  const auto restored = editor.Capture(GridBetween({4, 1}, {19, 1}));
  CHECK(restored[0].has_value());
  // After the undo the clip is the slipped one again, keyframes included.
  CHECK_EQ(FirstDifference(slipped, restored, GridBetween({4, 1}, {19, 1}), 0.0), std::string());
  CHECK_NO_THROW(editor.store().ValidateDatabase());
  (void)trimmed;
}

CUTLINE_TEST(MovingAClipTakesItsAnimationWithIt) {
  Editor editor;
  editor.InsertClip("clip", 4, 10, 30);
  AnimateOpacity(editor);
  const auto times = GridBetween({4, 1}, {24, 1});
  const auto original = editor.Capture(times);

  editor.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip", "v1", Seconds(9)});
  const auto moved = editor.Capture(GridBetween({9, 1}, {29, 1}));
  CHECK_EQ(FirstDifference(original, moved, times, 0.0), std::string());
}

CUTLINE_TEST(TheRenderVersionTravelsFromTheDatabaseThroughTheSnapshotToThePlan) {
  Editor editor;  // its sequence is created without a version: the current one
  const auto current = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK_EQ(current.root()->render_version, cutline::model::kCurrentRenderVersion);
  CHECK_EQ(TimelineCompiler{}.Compile(current, Seconds(0)).render_version, cutline::model::kCurrentRenderVersion);

  auto settings = commands::SequenceSettings{};
  settings.name = "Main";
  settings.frame_rate = {25, 1};
  settings.width = 1920;
  settings.height = 1080;
  settings.sample_rate = 48000;
  settings.render_version = cutline::model::kLegacyRenderVersion;
  editor.Run(commands::CommandType::UpdateSequenceSettings, commands::UpdateSequenceSettingsPayload{"seq-1", settings});
  const auto legacy = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK_EQ(legacy.root()->render_version, cutline::model::kLegacyRenderVersion);
  CHECK_EQ(TimelineCompiler{}.Compile(legacy, Seconds(0)).render_version, cutline::model::kLegacyRenderVersion);
}

CUTLINE_TEST(ThePitchPolicyReachesTheSnapshot) {
  Editor editor;
  editor.InsertClip("clip", 0, 0, 10, {1, 1}, false);
  editor.Run(commands::CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"clip", {2, 1}, false, true, true});
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(graph.root()->tracks[0].clips[0].maintain_pitch);
}

// ------------------------------------------------------ buses and sends ----

namespace {

void AddAudio(Editor& editor, const std::string& id, std::int64_t order) {
  editor.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{id, "seq-1", order, "stereo", id});
}

commands::SetTrackRoutingPayload Routing(const std::string& id, bool is_bus, const std::string& output = {},
                                         std::vector<commands::TrackSend> sends = {}) {
  commands::SetTrackRoutingPayload routing;
  routing.id = id;
  routing.is_bus = is_bus;
  routing.output_bus_id = output;
  routing.sends = std::move(sends);
  return routing;
}

const cutline::timeline::Track& Find(const cutline::timeline::SequenceGraph& graph, const std::string& id) {
  const auto* track = graph.root()->FindTrack(id);
  if (track == nullptr) throw std::runtime_error("no track " + id);
  return *track;
}

}  // namespace

CUTLINE_TEST(RoutingReachesTheSnapshotAndSurvivesUndoAndRedo) {
  Editor editor;
  AddAudio(editor, "a1", 0);
  AddAudio(editor, "bus-1", 1);
  AddAudio(editor, "bus-2", 2);
  editor.Run(commands::CommandType::SetTrackRouting, Routing("bus-1", true));
  editor.Run(commands::CommandType::SetTrackRouting, Routing("bus-2", true));
  editor.Run(commands::CommandType::SetTrackRouting, Routing("a1", false, "bus-1", {{"bus-2", -6.0, true}}));

  auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(Find(graph, "bus-1").is_bus);
  CHECK(!Find(graph, "a1").is_bus);
  CHECK_EQ(Find(graph, "a1").output_bus_id, std::string("bus-1"));
  CHECK_EQ(Find(graph, "a1").sends.size(), std::size_t{1});
  CHECK_EQ(Find(graph, "a1").sends[0].bus_id, std::string("bus-2"));
  CHECK(Find(graph, "a1").sends[0].gain_db == -6.0);
  CHECK(Find(graph, "a1").sends[0].pre_fader);

  // Changing the routing replaces it whole.
  editor.Run(commands::CommandType::SetTrackRouting, Routing("a1", false));
  graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(Find(graph, "a1").output_bus_id.empty());
  CHECK(Find(graph, "a1").sends.empty());

  (void)editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK_EQ(Find(graph, "a1").output_bus_id, std::string("bus-1"));
  CHECK_EQ(Find(graph, "a1").sends.size(), std::size_t{1});
  (void)editor.store().Undo("tester", "2026-10-06T01:00:01Z");
  graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(Find(graph, "a1").output_bus_id.empty());
  CHECK(Find(graph, "a1").sends.empty());
  (void)editor.store().Redo("tester", "2026-10-06T02:00:00Z");
  graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK_EQ(Find(graph, "a1").output_bus_id, std::string("bus-1"));
}

CUTLINE_TEST(RoutingRefusesWhatCouldNeverSoundRight) {
  Editor editor;
  AddAudio(editor, "a1", 0);
  AddAudio(editor, "a2", 1);
  AddAudio(editor, "bus-1", 2);
  AddAudio(editor, "bus-2", 3);
  editor.Run(commands::CommandType::SetTrackRouting, Routing("bus-1", true));
  editor.Run(commands::CommandType::SetTrackRouting, Routing("bus-2", true, "bus-1"));

  const auto refused = [&](const commands::SetTrackRoutingPayload& routing) {
    bool threw = false;
    try {
      editor.Run(commands::CommandType::SetTrackRouting, routing);
    } catch (const std::exception&) {
      threw = true;
    }
    return threw;
  };
  CHECK(refused(Routing("a1", false, "a2")));                            // a track is not a bus
  CHECK(refused(Routing("a1", false, "nowhere")));                       // unknown target
  CHECK(refused(Routing("bus-1", true, "bus-2")));                       // bus-2 already feeds bus-1: a loop
  CHECK(refused(Routing("bus-1", true, {}, {{"bus-2", 0.0, false}})));   // a send closes the same loop
  CHECK(refused(Routing("bus-1", true, "bus-1")));                       // itself
  CHECK(refused(Routing("v1", false)));                                  // a video track has no routing

  // A track that holds clips is not a bus, and a bus holds no clips.
  commands::InsertClipPayload clip;
  clip.id = "audio-clip";
  clip.track_id = "a2";
  clip.media_id = "media-1";
  clip.source_in = Seconds(0);
  clip.source_out = Seconds(2);
  clip.timeline_start = Seconds(0);
  editor.Run(commands::CommandType::InsertClip, clip);
  CHECK(refused(Routing("a2", true)));
  clip.id = "bus-clip";
  clip.track_id = "bus-1";
  CHECK_THROWS(editor.Run(commands::CommandType::InsertClip, clip));
  CHECK_THROWS(editor.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"audio-clip", "bus-1", Seconds(0)}));

  // A bus that still receives tracks cannot stop being one.
  editor.Run(commands::CommandType::SetTrackRouting, Routing("a1", false, "bus-1"));
  CHECK(refused(Routing("bus-1", false)));

  // None of the refusals left anything half-written.
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(Find(graph, "bus-1").is_bus);
  CHECK_EQ(Find(graph, "bus-2").output_bus_id, std::string("bus-1"));
  CHECK(!Find(graph, "a2").is_bus);
}

CUTLINE_TEST(DeletingABusSendsItsInputsToTheMasterAndUndoRestoresThem) {
  Editor editor;
  AddAudio(editor, "a1", 0);
  AddAudio(editor, "a2", 1);
  AddAudio(editor, "bus-1", 2);
  editor.Run(commands::CommandType::SetTrackRouting, Routing("bus-1", true));
  editor.Run(commands::CommandType::SetTrackRouting, Routing("a1", false, "bus-1"));
  editor.Run(commands::CommandType::SetTrackRouting, Routing("a2", false, {}, {{"bus-1", -3.0, false}}));

  editor.Run(commands::CommandType::RemoveTrack, commands::RemoveTrackPayload{"bus-1"});
  auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(Find(graph, "a1").output_bus_id.empty());
  CHECK(Find(graph, "a2").sends.empty());
  CHECK(graph.root()->FindTrack("bus-1") == nullptr);

  (void)editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(Find(graph, "bus-1").is_bus);
  CHECK_EQ(Find(graph, "a1").output_bus_id, std::string("bus-1"));
  CHECK_EQ(Find(graph, "a2").sends.size(), std::size_t{1});
  CHECK(Find(graph, "a2").sends[0].gain_db == -3.0);
}

CUTLINE_TEST(ABusStaysAudibleWhileTheTracksFeedingItAreSoloed) {
  Editor editor;
  AddAudio(editor, "a1", 0);
  AddAudio(editor, "a2", 1);
  AddAudio(editor, "bus-1", 2);
  editor.Run(commands::CommandType::SetTrackRouting, Routing("bus-1", true));
  commands::SetTrackStatePayload solo;
  solo.id = "a1";
  solo.solo = true;
  editor.Run(commands::CommandType::SetTrackState, solo);
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(cutline::timeline::TrackContributes(*graph.root(), Find(graph, "bus-1"), {}));
  CHECK(cutline::timeline::TrackContributes(*graph.root(), Find(graph, "a1"), {}));
  CHECK(!cutline::timeline::TrackContributes(*graph.root(), Find(graph, "a2"), {}));
}

// ---------------------------------------------------------------------- captions ----

CUTLINE_TEST(CaptionsReachTheSnapshotInOrderWithTheirStylesResolvedAndOnlyShowWhenAsked) {
  Editor editor;
  commands::AddCaptionTrackPayload track;
  track.id = "cap-en";
  track.sequence_id = "seq-1";
  track.language = "en";
  track.style_json = "{\"size\":0.08,\"position\":\"top\"}";
  editor.Run(commands::CommandType::AddCaptionTrack, track);
  commands::AddCaptionsPayload cues;
  cues.track_id = "cap-en";
  const auto cue = [](const std::string& id, std::int64_t start_ms, std::int64_t end_ms, const std::string& text, const std::string& style = "{}") {
    commands::CaptionCuePayload c;
    c.id = id;
    c.start = RationalTime(start_ms, 1000);
    c.end = RationalTime(end_ms, 1000);
    c.text = text;
    c.style_json = style;
    return c;
  };
  cues.cues = {cue("late", 6000, 8000, "Later"), cue("early", 1000, 3000, "Earlier", "{\"size\":0.03}"), cue("mid", 2000, 5000, "Middle")};
  editor.Run(commands::CommandType::AddCaptions, cues);

  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto& tracks = graph.root()->caption_tracks;
  CHECK_EQ(tracks.size(), std::size_t{1});
  CHECK_EQ(tracks[0].language, std::string("en"));
  CHECK_EQ(tracks[0].cues.size(), std::size_t{3});
  CHECK_EQ(tracks[0].cues[0].id, std::string("early"));  // by start time, however they were added
  CHECK_EQ(tracks[0].cues[2].id, std::string("late"));
  // The track's style with the cue's laid over it, ready for drawing.
  const auto early = cutline::captions::ParseStyle(tracks[0].cues[0].resolved_style_json);
  CHECK(std::abs(early.size - 0.03) < 1e-12);
  CHECK_EQ(early.position, std::string("top"));
  CHECK(std::abs(cutline::captions::ParseStyle(tracks[0].cues[1].resolved_style_json).size - 0.08) < 1e-12);

  // The compiler puts them in the plan only when asked, and only the ones showing.
  const TimelineCompiler compiler;
  CHECK(compiler.Compile(graph, RationalTime(5, 2)).captions.empty());
  cutline::timeline::CompileOptions with;
  with.include_captions = true;
  const auto plan = compiler.Compile(graph, RationalTime(5, 2), with);
  CHECK_EQ(plan.captions.size(), std::size_t{2});  // early and mid are both on at 2.5 s
  CHECK_EQ(plan.captions[0].text, std::string("Earlier"));
  CHECK_EQ(plan.captions[1].text, std::string("Middle"));
  CHECK(compiler.Compile(graph, RationalTime(1, 2), with).captions.empty());
  CHECK(compiler.Compile(graph, Seconds(5), with).captions.empty());  // mid ended at 5 s: the end is outside, and the next starts at 6
  const auto late = compiler.Compile(graph, Seconds(6), with);
  CHECK_EQ(late.captions.size(), std::size_t{1});
  CHECK_EQ(late.captions[0].text, std::string("Later"));
}

// ------------------------------------------------------------------ speed ramps ----

namespace {

commands::SetSpeedRampPayload Ramp(const std::string& clip, std::vector<commands::SpeedSegment> segments) {
  commands::SetSpeedRampPayload ramp;
  ramp.clip_id = clip;
  ramp.segments = std::move(segments);
  return ramp;
}

// Where in the media the clip reads at a timeline time, from the compiled plan.
double SourceSecondsAt(ProjectStore& store, const RationalTime& at) {
  const auto graph = cutline::timeline::LoadSequenceGraph(store, "seq-1");
  const auto plan = TimelineCompiler{}.Compile(graph, at);
  if (plan.video.empty()) return -1.0;
  const auto& t = plan.video[0].source_time;
  return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator());
}

}  // namespace

CUTLINE_TEST(ASpeedRampSetsTheClipsLengthAndPlaysTheSourceThroughItsSegmentsExactlyAtTheirBoundaries) {
  Editor editor;
  editor.InsertClip("clip", 0, 10, 20);  // media 10..20 s at timeline 0, ten seconds long
  // Two seconds at normal speed, two easing from 1 to 3 times (4 s of source), a one-second freeze, two at double speed.
  editor.Run(commands::CommandType::SetSpeedRamp,
             Ramp("clip", {{Seconds(2), 1.0, 1.0}, {Seconds(2), 1.0, 3.0}, {Seconds(1), 0.0, 0.0}, {Seconds(2), 2.0, 2.0}}));
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto& clip = graph.root()->tracks[0].clips[0];
  CHECK(clip.end().Compare(Seconds(7)) == 0);                 // the clip is now as long as the ramp
  CHECK(clip.source_in.Compare(Seconds(10)) == 0);            // and uses exactly the source the ramp sweeps: 2 + 4 + 0 + 4 = 10 s
  CHECK(clip.source_out.Compare(Seconds(20)) == 0);
  CHECK_EQ(clip.effects.size(), std::size_t{1});
  CHECK_EQ(clip.effects[0].effect_type, std::string("time_remap"));

  // At the segment boundaries the mapping is exact; in between it follows the speed.
  const auto at = [&](double seconds) { return SourceSecondsAt(editor.store(), RationalTime(static_cast<std::int64_t>(seconds * 1000), 1000)); };
  CHECK(std::abs(at(0.0) - 10.0) < 1e-4);
  CHECK(std::abs(at(2.0) - 12.0) < 1e-4);
  CHECK(std::abs(at(4.0) - 16.0) < 1e-4);   // 2 s at 1x, then 2 s averaging 2x
  CHECK(std::abs(at(5.0) - 16.0) < 1e-4);   // the freeze
  CHECK(std::abs(at(4.5) - 16.0) < 1e-4);
  CHECK(std::abs(at(6.0) - 18.0) < 1e-4);
  CHECK(std::abs(at(6.96) - 19.92) < 1e-3);
  // Inside the ease the position is the integral of the speed: at 3 s, 1 s into 1 -> 3, it has advanced 1 + 0.5 = 1.5.
  CHECK(std::abs(at(3.0) - (12.0 + 1.5)) < 5e-3);
  // Nothing after the end.
  CHECK(at(7.0) < 0.0);

  // It is one edit, and undo gives back the clip as it was.
  (void)editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  const auto before = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(before.root()->tracks[0].clips[0].end().Compare(Seconds(10)) == 0);
  CHECK(before.root()->tracks[0].clips[0].effects.empty());
  (void)editor.store().Redo("tester", "2026-10-06T02:00:00Z");
  CHECK(std::abs(SourceSecondsAt(editor.store(), Seconds(4)) - 16.0) < 1e-4);
}

CUTLINE_TEST(ASegmentThatRunsBackwardsReachesBackAndTheWindowGrowsToIt) {
  Editor editor;
  editor.InsertClip("clip", 0, 30, 40);
  // Two seconds forward, two back to where it started, two forward again.
  editor.Run(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(2), 1.0, 1.0}, {Seconds(2), -1.0, -1.0}, {Seconds(2), 1.0, 1.0}}));
  const auto at = [&](double seconds) { return SourceSecondsAt(editor.store(), RationalTime(static_cast<std::int64_t>(seconds * 1000), 1000)); };
  CHECK(std::abs(at(1.0) - 31.0) < 1e-4);
  CHECK(std::abs(at(2.0) - 32.0) < 1e-4);
  CHECK(std::abs(at(3.0) - 31.0) < 1e-4);   // going back
  CHECK(std::abs(at(4.0) - 30.0) < 1e-4);
  CHECK(std::abs(at(5.0) - 31.0) < 1e-4);
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(graph.root()->tracks[0].clips[0].source_out.Compare(Seconds(32)) == 0);  // the window reaches only as far as it goes
  CHECK(graph.root()->tracks[0].clips[0].end().Compare(Seconds(6)) == 0);

  // A ramp that begins by running back reaches before the clip's start, which is allowed while the media has it.
  Editor behind;
  behind.InsertClip("clip", 0, 30, 40);
  behind.Run(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(3), -1.0, -1.0}, {Seconds(3), 1.0, 1.0}}));
  CHECK(std::abs(SourceSecondsAt(behind.store(), Seconds(3)) - 27.0) < 1e-4);
  const auto reaches = cutline::timeline::LoadSequenceGraph(behind.store(), "seq-1");
  CHECK(reaches.root()->tracks[0].clips[0].source_in.Compare(Seconds(27)) == 0);
}

CUTLINE_TEST(SplittingARampedClipPlaysExactlyWhatItPlayedBeforeAndUndoPutsItBack) {
  Editor editor;
  editor.InsertClip("clip", 0, 10, 20);
  editor.Run(commands::CommandType::SetSpeedRamp,
             Ramp("clip", {{Seconds(2), 1.0, 1.0}, {Seconds(2), 1.0, 3.0}, {Seconds(1), 0.0, 0.0}, {Seconds(2), 2.0, 2.0}}));
  std::vector<std::pair<RationalTime, double>> before;
  for (std::int64_t quarter = 0; quarter < 28; ++quarter) {
    const RationalTime when(quarter, 4);
    before.emplace_back(when, SourceSecondsAt(editor.store(), when));
  }
  // Cut in the middle of the ease, and again inside the freeze.
  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip", "clip-b", RationalTime(7, 2)});
  editor.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-b", "clip-c", RationalTime(9, 2)});
  double worst = 0.0;
  for (const auto& [when, seconds] : before) worst = std::max(worst, std::abs(SourceSecondsAt(editor.store(), when) - seconds));
  if (!(worst < 2e-3)) cutline::testing::Fail("split changed the mapping", __FILE__, __LINE__, std::to_string(worst) + " s");
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK_EQ(graph.root()->tracks[0].clips.size(), std::size_t{3});
  CHECK(graph.root()->tracks[0].clips[0].end().Compare(RationalTime(7, 2)) == 0);
  CHECK(graph.root()->tracks[0].clips[2].end().Compare(Seconds(7)) == 0);
  (void)editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  (void)editor.store().Undo("tester", "2026-10-06T01:00:01Z");
  CHECK_EQ(cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1").root()->tracks[0].clips.size(), std::size_t{1});
  CHECK(std::abs(SourceSecondsAt(editor.store(), Seconds(4)) - 16.0) < 1e-4);
}

CUTLINE_TEST(ARampKeepsTheAudioPartnerInStepAndCanBeTakenOffToLeaveSteadyPlayback) {
  Editor editor;
  editor.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});
  editor.InsertClip("clip", 0, 10, 20);
  commands::InsertClipPayload audio;
  audio.id = "audio";
  audio.track_id = "a1";
  audio.media_id = "media-1";
  audio.source_in = Seconds(10);
  audio.source_out = Seconds(20);
  audio.timeline_start = Seconds(0);
  editor.Run(commands::CommandType::InsertClip, audio);
  editor.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip", "audio"}, "take"});
  editor.Run(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(2), 1.0, 1.0}, {Seconds(2), 3.0, 3.0}}));
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto* partner = graph.root()->FindTrack("a1");
  CHECK(partner != nullptr);
  CHECK(partner->clips[0].end().Compare(Seconds(4)) == 0);
  CHECK_EQ(partner->clips[0].effects.size(), std::size_t{1});
  CHECK(partner->clips[0].source_out.Compare(Seconds(18)) == 0);

  // Taking the ramp off leaves the clip its length and an even speed through the same source.
  editor.Run(commands::CommandType::ClearSpeedRamp, commands::ClearSpeedRampPayload{"clip"});
  const auto cleared = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(cleared.root()->tracks[0].clips[0].effects.empty());
  CHECK(cleared.root()->FindTrack("a1")->clips[0].effects.empty());
  CHECK(cleared.root()->tracks[0].clips[0].end().Compare(Seconds(4)) == 0);
  CHECK(std::abs(SourceSecondsAt(editor.store(), Seconds(2)) - 14.0) < 1e-6);  // 8 s of source over 4 s: twice the speed, evenly
  CHECK_THROWS(editor.Run(commands::CommandType::ClearSpeedRamp, commands::ClearSpeedRampPayload{"clip"}));
}

CUTLINE_TEST(ARampIsRefusedWhereItCouldNotPlayAndTheEditsItWouldBreakAreRefused) {
  Editor editor;
  editor.InsertClip("clip", 0, 10, 20);
  editor.InsertClip("next", 12, 0, 5);
  const auto refused = [&](commands::CommandType type, commands::CommandPayload payload) {
    bool threw = false;
    try {
      editor.Run(type, std::move(payload));
    } catch (const std::exception&) {
      threw = true;
    }
    return threw;
  };
  // Longer than the room before the next clip.
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(20), 1.0, 1.0}})));
  // Past the end of the media (120 s), before its start, going nowhere, and not a number.
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(10), 50.0, 50.0}})));
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(10), -50.0, -50.0}})));
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(5), 0.0, 0.0}})));
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(5), std::nan(""), 1.0}})));
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("clip", {})));
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("clip", {{RationalTime(0, 1), 1.0, 1.0}})));
  CHECK(refused(commands::CommandType::SetSpeedRamp, Ramp("nothing", {{Seconds(5), 1.0, 1.0}})));
  CHECK_EQ(cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1").root()->tracks[0].clips[0].effects.size(), std::size_t{0});

  // Once there is a ramp, a trim or a steady speed would silently break it, so they are refused.
  editor.Run(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(4), 1.0, 2.0}}));
  commands::TrimClipPayload trim;
  trim.id = "clip";
  trim.source_in = Seconds(11);
  trim.source_out = Seconds(14);
  trim.timeline_start = Seconds(0);
  CHECK(refused(commands::CommandType::TrimClip, trim));
  CHECK(refused(commands::CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"clip", {2, 1}, false, true, false}));
  // Moving it is fine: only where it sits changes.
  editor.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip", "v1", Seconds(6)});
  CHECK(std::abs(SourceSecondsAt(editor.store(), RationalTime(13, 2)) - 10.5) < 0.05);
}

CUTLINE_TEST(ARampSurvivesClosingAndReopeningTheProject) {
  const auto package = std::filesystem::temp_directory_path() / "cutline-test-ramp-reopen";
  std::filesystem::remove_all(package);
  const auto id = ProjectStore::GenerateProjectUuid();
  double expected = 0.0;
  {
    commands::CommandEnvelope create;
    create.command_id = "c0";
    create.project_id = id;
    create.author_id = "tester";
    create.timestamp_utc = "2026-10-06T00:00:00Z";
    create.type = commands::CommandType::CreateProject;
    create.payload = commands::CreateProjectPayload{"Ramp"};
    create.idempotency_key = "k0";
    auto store = ProjectStore::CreatePackage(package, create);
    int n = 0;
    const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
      commands::CommandEnvelope command;
      command.command_id = "cmd-" + std::to_string(++n);
      command.project_id = id;
      command.author_id = "tester";
      command.base_revision = store->CurrentRevision();
      command.timestamp_utc = "2026-10-06T00:00:00Z";
      command.type = type;
      command.payload = std::move(payload);
      command.idempotency_key = "key-" + std::to_string(n);
      (void)store->Execute(command);
    };
    commands::ImportMediaPayload media;
    media.id = "media-1";
    media.display_name = "m";
    media.original_path = "/m";
    media.fingerprint = "fp";
    media.duration = Seconds(100);
    run(commands::CommandType::ImportMedia, media);
    commands::CreateSequencePayload sequence;
    sequence.id = "seq-1";
    sequence.settings.name = "S";
    sequence.settings.frame_rate = {25, 1};
    sequence.settings.width = 640;
    sequence.settings.height = 360;
    sequence.settings.sample_rate = 48000;
    run(commands::CommandType::CreateSequence, sequence);
    run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
    commands::InsertClipPayload clip;
    clip.id = "clip";
    clip.track_id = "v1";
    clip.media_id = "media-1";
    clip.source_in = Seconds(10);
    clip.source_out = Seconds(20);
    clip.timeline_start = Seconds(0);
    run(commands::CommandType::InsertClip, clip);
    run(commands::CommandType::SetSpeedRamp, Ramp("clip", {{Seconds(3), 0.5, 2.5}, {Seconds(2), 0.0, 0.0}}));
    expected = SourceSecondsAt(*store, RationalTime(5, 2));
  }
  auto reopened = ProjectStore::OpenPackage(package);
  CHECK(std::abs(SourceSecondsAt(*reopened, RationalTime(5, 2)) - expected) < 1e-9);
  CHECK(reopened->CurrentRevision() > 0);
  reopened.reset();
  std::filesystem::remove_all(package);
}

// -------------------------------------------------------------- multicam, stored ----

namespace {

void ImportAngle(Editor& editor, const std::string& id, std::int64_t seconds, bool with_audio, std::int64_t timecode = 0) {
  commands::ImportMediaPayload media;
  media.id = id;
  media.display_name = id;
  media.original_path = "/" + id;
  media.fingerprint = "fp-" + id;
  media.duration = Seconds(seconds);
  media.start_timecode = Seconds(timecode);
  commands::MediaStream picture;
  picture.stream_index = 0;
  picture.kind = model::StreamKind::Video;
  picture.codec = "h264";
  picture.width = 1920;
  picture.height = 1080;
  picture.frame_rate = {25, 1};
  media.streams.push_back(picture);
  if (with_audio) {
    commands::MediaStream sound;
    sound.stream_index = 1;
    sound.kind = model::StreamKind::Audio;
    sound.codec = "aac";
    sound.sample_rate = 48000;
    sound.channel_count = 2;
    media.streams.push_back(sound);
  }
  editor.Run(commands::CommandType::ImportMedia, media);
}

commands::CreateMulticamGroupPayload ConcertGroup() {
  commands::CreateMulticamGroupPayload group;
  group.id = "concert";
  group.name = "Concert";
  group.duration = Seconds(10);
  group.angles = {{"wide", "cam-wide", "Wide", std::nullopt}, {"close", "cam-close", "Close", std::nullopt}, {"b-roll", "cam-broll", "B-roll", std::nullopt}};
  return group;
}

// Wide starts the group at its own 0; Close started 2 s before the wide camera; B-roll has no sound and is short.
void SetUpConcert(Editor& editor) {
  ImportAngle(editor, "cam-wide", 30, true, 100);
  ImportAngle(editor, "cam-close", 30, true, 98);
  ImportAngle(editor, "cam-broll", 8, false, 100);
  editor.Run(commands::CommandType::CreateMulticamGroup, ConcertGroup());
  commands::SetMulticamSyncPayload sync;
  sync.group_id = "concert";
  sync.method = "timecode";
  sync.reference_angle_id = "wide";
  sync.confidence = 1.0;
  sync.offsets = {{"wide", Seconds(0)}, {"close", Seconds(2)}, {"b-roll", RationalTime(-1, 2)}};
  editor.Run(commands::CommandType::SetMulticamSync, sync);
}

void Cut(Editor& editor, std::int64_t at_ms, const std::string& angle) {
  editor.Run(commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{"concert", RationalTime(at_ms, 1000), angle});
}

bool Refused(Editor& editor, commands::CommandType type, commands::CommandPayload payload) {
  try {
    editor.Run(type, std::move(payload));
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

}  // namespace

CUTLINE_TEST(AMulticamGroupIsStoredWithItsAnglesOffsetsAndCutsAndUndoTakesThemBack) {
  namespace mc = cutline::timeline::multicam;
  Editor editor;
  SetUpConcert(editor);
  Cut(editor, 0, "wide");
  Cut(editor, 4000, "close");
  Cut(editor, 7000, "wide");

  const auto ids = mc::ListGroupIds(editor.store());
  CHECK_EQ(ids.size(), std::size_t{1});
  const auto group = mc::LoadGroup(editor.store(), "concert");
  CHECK_EQ(group.name, std::string("Concert"));
  CHECK_EQ(group.sync_method, std::string("timecode"));
  CHECK_EQ(group.reference_angle_id, std::string("wide"));
  CHECK_EQ(group.angles.size(), std::size_t{3});
  CHECK_EQ(group.angles[1].id, std::string("close"));
  CHECK_EQ(group.angles[1].source_id, std::string("cam-close"));
  CHECK(group.angles[1].source_offset.Compare(Seconds(2)) == 0);
  CHECK(group.angles[1].timecode_start.Compare(Seconds(98)) == 0);   // taken from the media
  CHECK(group.angles[2].source_offset.Compare(RationalTime(-1, 2)) == 0);
  CHECK(group.angles[2].duration.Compare(Seconds(8)) == 0);
  CHECK_EQ(group.switches.size(), std::size_t{3});
  CHECK_EQ(mc::ActiveAngle(group, Seconds(5))->id, std::string("close"));
  CHECK_EQ(mc::ActiveAngle(group, Seconds(8))->id, std::string("wide"));
  // The angle monitor sees every angle that has picture at that instant: at 9 s B-roll (offset -0.5) is at 8.5 s, past its end at 8 s.
  CHECK_EQ(mc::MonitorRequests(group, Seconds(1)).size(), std::size_t{3});
  CHECK_EQ(mc::MonitorRequests(group, Seconds(9)).size(), std::size_t{2});

  // A cut is one edit; undoing all of it removes the cuts, and the group with them.
  (void)editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  CHECK_EQ(mc::LoadGroup(editor.store(), "concert").switches.size(), std::size_t{2});
  (void)editor.store().Redo("tester", "2026-10-06T01:00:01Z");
  CHECK_EQ(mc::LoadGroup(editor.store(), "concert").switches.size(), std::size_t{3});
  CHECK_NO_THROW(editor.store().ValidateDatabase());
  for (int i = 0; i < 6; ++i) (void)editor.store().Undo("tester", "2026-10-06T02:00:00Z");
  CHECK(mc::ListGroupIds(editor.store()).empty());
}

CUTLINE_TEST(CutsReplaceCoalesceAndRemoveLikeTheInMemoryEngineAndTheEditsThatCouldNotWorkAreRefused) {
  namespace mc = cutline::timeline::multicam;
  Editor editor;
  SetUpConcert(editor);
  Cut(editor, 0, "wide");
  Cut(editor, 3000, "close");
  Cut(editor, 3000, "b-roll");   // same moment: replaces
  Cut(editor, 5000, "b-roll");   // already showing: nothing to cut
  Cut(editor, 6000, "wide");
  auto group = mc::LoadGroup(editor.store(), "concert");
  CHECK_EQ(group.switches.size(), std::size_t{3});
  CHECK_EQ(group.switches[1].angle_id, std::string("b-roll"));

  // The same recording through the in-memory engine gives the same cuts.
  mc::Group memory = group;
  memory.switches.clear();
  mc::RecordSwitch(memory, Seconds(0), "wide");
  mc::RecordSwitch(memory, Seconds(3), "close");
  mc::RecordSwitch(memory, Seconds(3), "b-roll");
  mc::RecordSwitch(memory, Seconds(5), "b-roll");
  mc::RecordSwitch(memory, Seconds(6), "wide");
  CHECK_EQ(memory.switches.size(), group.switches.size());
  for (std::size_t i = 0; i < memory.switches.size(); ++i) {
    CHECK(memory.switches[i].timeline_time.Compare(group.switches[i].timeline_time) == 0);
    CHECK_EQ(memory.switches[i].angle_id, group.switches[i].angle_id);
  }

  // Removing the middle cut lets the first angle run on; with Wide on both sides of it that is one cut.
  editor.Run(commands::CommandType::RemoveMulticamSwitch, commands::RemoveMulticamSwitchPayload{"concert", Seconds(3)});
  CHECK(mc::LoadGroup(editor.store(), "concert").switches.size() == 1);
  // A cut that is not there cannot be removed.
  CHECK(Refused(editor, commands::CommandType::RemoveMulticamSwitch, commands::RemoveMulticamSwitchPayload{"concert", Seconds(3)}));
  editor.Run(commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{"concert", Seconds(8), "close"});
  CHECK(mc::LoadGroup(editor.store(), "concert").switches.size() == 2);

  // Refusals.
  CHECK(Refused(editor, commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{"concert", Seconds(2), "nobody"}));
  CHECK(Refused(editor, commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{"concert", Seconds(10), "wide"}));
  CHECK(Refused(editor, commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{"nothing", Seconds(2), "wide"}));
  CHECK(Refused(editor, commands::CommandType::CreateMulticamGroup, ConcertGroup()));   // exists
  auto one = ConcertGroup();
  one.id = "single";
  one.angles.resize(1);
  CHECK(Refused(editor, commands::CommandType::CreateMulticamGroup, one));
  auto twice = ConcertGroup();
  twice.id = "twice";
  twice.angles[1].id = "wide";
  CHECK(Refused(editor, commands::CommandType::CreateMulticamGroup, twice));
  auto missing = ConcertGroup();
  missing.id = "missing";
  missing.angles[1].media_id = "no-such-media";
  CHECK(Refused(editor, commands::CommandType::CreateMulticamGroup, missing));
  CHECK(mc::ListGroupIds(editor.store()).size() == 1);   // none of them left anything behind
  commands::SetMulticamSyncPayload bad;
  bad.group_id = "concert";
  bad.method = "audio";
  bad.offsets = {{"nobody", Seconds(1)}};
  CHECK(Refused(editor, commands::CommandType::SetMulticamSync, bad));
  bad.method = "guess";
  bad.offsets = {{"wide", Seconds(1)}};
  CHECK(Refused(editor, commands::CommandType::SetMulticamSync, bad));
  // Media that angles use cannot be removed from under the group.
  CHECK(Refused(editor, commands::CommandType::RemoveMedia, commands::RemoveMediaPayload{"cam-close"}));
  editor.Run(commands::CommandType::DeleteMulticamGroup, commands::DeleteMulticamGroupPayload{"concert"});
  CHECK(mc::ListGroupIds(editor.store()).empty());
  editor.Run(commands::CommandType::RemoveMedia, commands::RemoveMediaPayload{"cam-close"});
}

CUTLINE_TEST(FlatteningLaysTheCutsOutAsOrdinaryClipsWithTheSoundFollowingOrFixedAndEveryAngleStaysReachable) {
  namespace mc = cutline::timeline::multicam;
  Editor editor;
  editor.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});
  SetUpConcert(editor);
  Cut(editor, 0, "wide");
  Cut(editor, 4000, "close");
  Cut(editor, 7000, "b-roll");
  Cut(editor, 7500, "wide");

  commands::FlattenMulticamGroupPayload flatten;
  flatten.group_id = "concert";
  flatten.video_track_id = "v1";
  flatten.audio_track_id = "a1";
  flatten.timeline_start = Seconds(20);
  flatten.id_prefix = "take";
  editor.Run(commands::CommandType::FlattenMulticamGroup, flatten);

  // The same clips the in-memory flattening gives, moved to start at 20 s.
  const auto expected = mc::Flatten(mc::LoadGroup(editor.store(), "concert"));
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto* video = graph.root()->FindTrack("v1");
  const auto* audio = graph.root()->FindTrack("a1");
  CHECK(video != nullptr && audio != nullptr);
  CHECK_EQ(video->clips.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK_EQ(video->clips[i].source_id, expected[i].source_id);
    CHECK(video->clips[i].source_in.Compare(expected[i].source_in) == 0);
    CHECK(video->clips[i].source_out.Compare(expected[i].source_out) == 0);
    CHECK(video->clips[i].timeline_start.Compare(expected[i].timeline_start.Add(Seconds(20))) == 0);
  }
  // Wide 0-4, Close 4-7 (source 6-9), B-roll 7-7.5 (source 6.5-7), Wide 7.5-10.
  CHECK_EQ(video->clips.size(), std::size_t{4});
  CHECK(video->clips[1].source_in.Compare(Seconds(6)) == 0);
  CHECK(video->clips[2].source_in.Compare(RationalTime(13, 2)) == 0);
  // The sound follows the picture, except where the angle has none (B-roll), and each pair is linked.
  CHECK_EQ(audio->clips.size(), std::size_t{3});
  CHECK_EQ(audio->clips[1].source_id, std::string("cam-close"));
  CHECK(!video->clips[1].linked_group.empty() && video->clips[1].linked_group == audio->clips[1].linked_group);
  CHECK(video->clips[2].linked_group.empty());

  // The group is untouched, so any angle can be cut again or flattened differently.
  CHECK_EQ(mc::LoadGroup(editor.store(), "concert").angles.size(), std::size_t{3});
  CHECK_EQ(mc::LoadGroup(editor.store(), "concert").switches.size(), std::size_t{4});

  // One edit: undo clears every clip it made.
  (void)editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  const auto cleared = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK(cleared.root()->FindTrack("v1")->clips.empty() && cleared.root()->FindTrack("a1")->clips.empty());
  (void)editor.store().Redo("tester", "2026-10-06T01:00:01Z");

  // A fixed sound angle: one continuous clip from the angle that has the good microphone.
  editor.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
  editor.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a2", "seq-1", 1, "stereo", "A2"});
  flatten.video_track_id = "v2";
  flatten.audio_track_id = "a2";
  flatten.audio_angle_id = "close";
  flatten.id_prefix = "alt";
  editor.Run(commands::CommandType::FlattenMulticamGroup, flatten);
  const auto fixed = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK_EQ(fixed.root()->FindTrack("a2")->clips.size(), std::size_t{1});
  const auto& whole = fixed.root()->FindTrack("a2")->clips[0];
  CHECK(whole.source_in.Compare(Seconds(2)) == 0 && whole.source_out.Compare(Seconds(12)) == 0);
  CHECK(whole.timeline_start.Compare(Seconds(20)) == 0);

  // Refusals: sound from an angle without any, the wrong kind of track, and a clash with clips already there.
  flatten.id_prefix = "x";
  flatten.audio_angle_id = "b-roll";
  CHECK(Refused(editor, commands::CommandType::FlattenMulticamGroup, flatten));
  flatten.audio_angle_id.clear();
  flatten.video_track_id = "a1";
  CHECK(Refused(editor, commands::CommandType::FlattenMulticamGroup, flatten));
  flatten.video_track_id = "v1";
  CHECK(Refused(editor, commands::CommandType::FlattenMulticamGroup, flatten));
  commands::FlattenMulticamGroupPayload nothing = flatten;
  nothing.id_prefix = "y";
  nothing.video_track_id = "v2";
  nothing.timeline_start = Seconds(100);
  editor.Run(commands::CommandType::DeleteMulticamGroup, commands::DeleteMulticamGroupPayload{"concert"});
  CHECK(Refused(editor, commands::CommandType::FlattenMulticamGroup, nothing));   // the group is gone
  CHECK(cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1").root()->FindTrack("v1")->clips.size() == 4);   // the clips are ordinary and stay
}

CUTLINE_TEST(AStoredGroupSurvivesReopeningAndAnAudioSyncResultIsStoredAsItsOffsets) {
  namespace mc = cutline::timeline::multicam;
  const auto package = std::filesystem::temp_directory_path() / "cutline-test-multicam-reopen";
  std::filesystem::remove_all(package);
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    commands::CommandEnvelope create;
    create.command_id = "c0";
    create.project_id = id;
    create.author_id = "tester";
    create.timestamp_utc = "2026-10-06T00:00:00Z";
    create.type = commands::CommandType::CreateProject;
    create.payload = commands::CreateProjectPayload{"Multicam"};
    create.idempotency_key = "k0";
    auto store = ProjectStore::CreatePackage(package, create);
    int n = 0;
    const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
      commands::CommandEnvelope command;
      command.command_id = "cmd-" + std::to_string(++n);
      command.project_id = id;
      command.author_id = "tester";
      command.base_revision = store->CurrentRevision();
      command.timestamp_utc = "2026-10-06T00:00:00Z";
      command.type = type;
      command.payload = std::move(payload);
      command.idempotency_key = "key-" + std::to_string(n);
      (void)store->Execute(command);
    };
    for (const auto* name : {"a", "b"}) {
      commands::ImportMediaPayload media;
      media.id = name;
      media.display_name = name;
      media.original_path = std::string("/") + name;
      media.fingerprint = std::string("fp") + name;
      media.duration = Seconds(5);
      run(commands::CommandType::ImportMedia, media);
    }
    commands::CreateMulticamGroupPayload group;
    group.id = "interview";
    group.duration = Seconds(5);
    group.angles = {{"a", "a", "A", std::nullopt}, {"b", "b", "B", RationalTime(1, 2)}};
    run(commands::CommandType::CreateMulticamGroup, group);

    // Audio envelopes are analysed outside the project; only the answer is stored.
    mc::Group analysis;
    analysis.id = "interview";
    analysis.duration = Seconds(5);
    mc::Angle reference;
    reference.id = "a";
    reference.source_id = "a";
    reference.duration = Seconds(5);
    reference.envelope_rate = 10;
    reference.audio_envelope = {0, 0, 1, -1, 0.5f, 0, 0, 0, 0, 0, 0, 0};
    mc::Angle delayed = reference;
    delayed.id = "b";
    delayed.source_id = "b";
    delayed.audio_envelope = {0, 0, 0, 0, 0, 1, -1, 0.5f, 0, 0, 0, 0, 0, 0, 0};
    analysis.angles = {reference, delayed};
    const auto confidence = mc::Synchronize(analysis, mc::SyncMethod::Audio, "a", 5);
    commands::SetMulticamSyncPayload sync;
    sync.group_id = "interview";
    sync.method = "audio";
    sync.reference_angle_id = "a";
    sync.confidence = confidence;
    for (const auto& [angle, offset] : mc::Offsets(analysis)) sync.offsets.push_back({angle, offset});
    run(commands::CommandType::SetMulticamSync, sync);
    run(commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{"interview", Seconds(0), "a"});
    run(commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{"interview", RationalTime(5, 2), "b"});
  }
  auto reopened = ProjectStore::OpenPackage(package);
  const auto group = mc::LoadGroup(*reopened, "interview");
  CHECK_EQ(group.sync_method, std::string("audio"));
  CHECK(group.sync_confidence > 0.95);
  CHECK(group.angles[1].source_offset.Compare(RationalTime(3, 10)) == 0);
  CHECK(group.angles[1].marker.has_value() && group.angles[1].marker->Compare(RationalTime(1, 2)) == 0);
  CHECK_EQ(group.switches.size(), std::size_t{2});
  reopened.reset();
  std::filesystem::remove_all(package);
}

// ------------------------------------------------------- graphics kept in the project ----

namespace {

namespace gfx = cutline::render::graphics;

gfx::Element Box(const std::string& id, double x, double y, double width, double height, std::array<double, 4> fill) {
  gfx::Element box;
  box.id = id;
  box.type = gfx::ElementType::Rectangle;
  box.x = x;
  box.y = y;
  box.width = width;
  box.height = height;
  box.fill = fill;
  return box;
}

// A card that slides in from the left over the first two seconds.
std::string SlidingCard(double blue = 0.8) {
  gfx::Document document;
  document.width = 1920;
  document.height = 1080;
  auto card = Box("card", 0.0, 0.25, 0.25, 0.5, {0.1, 0.3, blue, 1.0});
  card.animations.push_back({"x", "linear", {{0.0, 0.0}, {2.0, 0.5}}});
  document.elements.push_back(card);
  return gfx::ToJson(document);
}

std::string BannerTemplate(std::int64_t version, double height) {
  gfx::TemplatePackage package;
  package.id = "banner";
  package.version = version;
  package.name = "Banner";
  package.document.elements.push_back(Box("bar", 0.0, 0.8, 1.0, height, {1.0, 1.0, 1.0, 1.0}));
  gfx::Control colour{"colour", "bar", "fill", "#FF0000"};
  gfx::Control thickness{"thickness", "bar", "height", "0.1"};
  thickness.minimum = 0.05;
  thickness.maximum = 0.2;
  package.controls = {colour, thickness};
  return gfx::ToJson(package);
}

cutline::media::VideoFrame RenderAt(Editor& editor, const RationalTime& at, cutline::render::Statistics* statistics = nullptr) {
  const auto graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto plan = TimelineCompiler{}.Compile(graph, at);
  cutline::render::CompositorConfig config;
  config.width = 192;
  config.height = 108;
  config.output_format = cutline::media::PixelFormat::RgbaF32;
  cutline::render::Statistics local;
  return cutline::render::Compositor(config).Compose(
      plan, [](const cutline::timeline::SourceRequest&) { return static_cast<const cutline::media::VideoFrame*>(nullptr); },
      statistics != nullptr ? *statistics : local);
}

struct Pixel {
  float r, g, b;
};
Pixel PixelAt(const cutline::media::VideoFrame& frame, int x, int y) {
  const auto* texel = frame.row_f32(y) + static_cast<std::size_t>(x) * 4;
  return {texel[0], texel[1], texel[2]};
}

void AddGraphicTo(Editor& editor, const std::string& graphic, const std::string& clip, std::int64_t start, std::int64_t length) {
  editor.Run(commands::CommandType::AddGraphicClip,
             commands::AddGraphicClipPayload{clip, "v1", graphic, Seconds(start), Seconds(length), ""});
}

}  // namespace

CUTLINE_TEST(AGraphicLivesInTheProjectAndItsClipDrawsItAnimatedWithNoFileOutsideTheProject) {
  Editor editor;
  commands::CreateGraphicPayload graphic;
  graphic.id = "card";
  graphic.name = "Sliding card";
  graphic.document_json = SlidingCard();
  editor.Run(commands::CommandType::CreateGraphic, graphic);
  AddGraphicTo(editor, "card", "card-clip", 1, 4);

  const auto loaded = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  const auto& clip = loaded.root()->tracks[0].clips[0];
  CHECK(clip.source_kind == model::SourceKind::Adjustment);
  CHECK(clip.end().Compare(Seconds(5)) == 0);
  CHECK_EQ(clip.effects.size(), std::size_t{1});
  CHECK_EQ(clip.effects[0].effect_type, std::string("graphic"));
  CHECK_EQ(clip.effects[0].preset_name, std::string("project:card"));
  CHECK(!clip.effects[0].inline_asset.empty());

  // Two seconds in (clip-local 1 s) the card is half way along its slide: its left edge is at x = 0.25 of the picture.
  cutline::render::Statistics statistics;
  const auto middle = RenderAt(editor, Seconds(2), &statistics);
  CHECK_EQ(statistics.graphics_elements_drawn, 1);
  CHECK_EQ(statistics.adjustment_layers, 0);   // drawn as a graphic, not applied as an adjustment
  CHECK(statistics.effect_errors.empty());
  CHECK(PixelAt(middle, 60, 54).b > 0.7f);      // columns 48 to 95 of 192 are the card
  CHECK(PixelAt(middle, 20, 54).b < 0.05f);     // where it has left
  CHECK(PixelAt(middle, 100, 54).b < 0.05f);    // and where it has not reached
  // At the start of the clip it is at the left; after the slide it stays at the end position.
  const auto start = RenderAt(editor, Seconds(1));
  CHECK(PixelAt(start, 10, 54).b > 0.7f && PixelAt(start, 60, 54).b < 0.05f);
  const auto end = RenderAt(editor, RationalTime(9, 2));
  CHECK(PixelAt(end, 10, 54).b < 0.05f && PixelAt(end, 120, 54).b > 0.7f);
  // Nothing before or after the clip.
  CHECK(PixelAt(RenderAt(editor, RationalTime(1, 2)), 10, 54).b < 0.05f);

  // The picture comes from the project: editing the graphic changes every clip that shows it, in one undoable edit.
  const auto before = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1").root()->tracks[0].clips[0].effects[0].inline_asset;
  commands::UpdateGraphicPayload update;
  update.id = "card";
  update.document_json = SlidingCard(0.2);
  editor.Run(commands::CommandType::UpdateGraphic, update);
  CHECK(cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1").root()->tracks[0].clips[0].effects[0].inline_asset != before);
  CHECK(PixelAt(RenderAt(editor, Seconds(2)), 60, 54).b < 0.4f);
  (void)editor.store().Undo("tester", "2026-10-06T01:00:00Z");
  CHECK(PixelAt(RenderAt(editor, Seconds(2)), 60, 54).b > 0.7f);

  // A graphic that clips show cannot be deleted from under them; one that none shows can.
  CHECK(Refused(editor, commands::CommandType::DeleteGraphic, commands::DeleteGraphicPayload{"card"}));
  editor.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"card-clip"});
  editor.Run(commands::CommandType::DeleteGraphic, commands::DeleteGraphicPayload{"card"});
  // The whole thing undoes.
  (void)editor.store().Undo("tester", "2026-10-06T02:00:00Z");
  (void)editor.store().Undo("tester", "2026-10-06T02:00:01Z");
  CHECK(PixelAt(RenderAt(editor, Seconds(2)), 60, 54).b > 0.7f);
  CHECK_NO_THROW(editor.store().ValidateDatabase());
}

CUTLINE_TEST(GraphicEditsThatCouldNotDrawAreRefusedBeforeTheyReachTheProject) {
  Editor editor;
  const auto refuses = [&](commands::CommandType type, commands::CommandPayload payload) { return Refused(editor, type, std::move(payload)); };
  commands::CreateGraphicPayload bad;
  bad.id = "bad";
  bad.document_json = "{\"schema_version\":1,\"width\":0,\"height\":100,\"elements\":[]}";   // no size
  CHECK(refuses(commands::CommandType::CreateGraphic, bad));
  bad.document_json = "not json";
  CHECK(refuses(commands::CommandType::CreateGraphic, bad));
  bad.document_json = "";
  CHECK(refuses(commands::CommandType::CreateGraphic, bad));
  bad.kind = "template";
  bad.template_id = "banner";
  bad.template_version = 1;
  CHECK(refuses(commands::CommandType::CreateGraphic, bad));   // not installed
  bad.kind = "movie";
  CHECK(refuses(commands::CommandType::CreateGraphic, bad));
  commands::CreateGraphicPayload good;
  good.id = "ok";
  good.document_json = SlidingCard();
  editor.Run(commands::CommandType::CreateGraphic, good);
  CHECK(refuses(commands::CommandType::CreateGraphic, good));   // exists
  // A document edit for a graphic that does not exist, with nothing to change, and a clip where none fits.
  commands::UpdateGraphicPayload nothing;
  nothing.id = "ok";
  CHECK(refuses(commands::CommandType::UpdateGraphic, nothing));
  commands::UpdateGraphicPayload missing;
  missing.id = "nobody";
  missing.name = "x";
  CHECK(refuses(commands::CommandType::UpdateGraphic, missing));
  commands::UpdateGraphicPayload values;
  values.id = "ok";
  values.values = std::map<std::string, std::string>{{"a", "b"}};
  CHECK(refuses(commands::CommandType::UpdateGraphic, values));   // a plain graphic has no controls
  CHECK(refuses(commands::CommandType::AddGraphicClip, commands::AddGraphicClipPayload{"c", "v1", "nobody", Seconds(0), Seconds(1), ""}));
  CHECK(refuses(commands::CommandType::AddGraphicClip, commands::AddGraphicClipPayload{"c", "nowhere", "ok", Seconds(0), Seconds(1), ""}));
  editor.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});
  CHECK(refuses(commands::CommandType::AddGraphicClip, commands::AddGraphicClipPayload{"c", "a1", "ok", Seconds(0), Seconds(1), ""}));
  AddGraphicTo(editor, "ok", "first", 0, 3);
  CHECK(refuses(commands::CommandType::AddGraphicClip, commands::AddGraphicClipPayload{"second", "v1", "ok", Seconds(2), Seconds(3), ""}));   // overlaps
  CHECK(refuses(commands::CommandType::InstallGraphicTemplate, commands::InstallGraphicTemplatePayload{"{\"template_id\":\"x\"}"}));
  CHECK_EQ(cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1").root()->tracks[0].clips.size(), std::size_t{1});
}

CUTLINE_TEST(TemplatePackagesAreInstalledByVersionAndAnInstanceKeepsDrawingTheVersionItWasMadeWith) {
  Editor editor;
  editor.Run(commands::CommandType::InstallGraphicTemplate, commands::InstallGraphicTemplatePayload{BannerTemplate(1, 0.1)});
  // Installing exactly the same package again changes nothing; a changed package under the same version is refused.
  editor.Run(commands::CommandType::InstallGraphicTemplate, commands::InstallGraphicTemplatePayload{BannerTemplate(1, 0.1)});
  CHECK(Refused(editor, commands::CommandType::InstallGraphicTemplate, commands::InstallGraphicTemplatePayload{BannerTemplate(1, 0.15)}));

  commands::CreateGraphicPayload instance;
  instance.id = "news";
  instance.name = "News banner";
  instance.kind = "template";
  instance.template_id = "banner";
  instance.template_version = 1;
  instance.values = {{"colour", "#FF0000"}, {"thickness", "0.1"}};
  editor.Run(commands::CommandType::CreateGraphic, instance);
  AddGraphicTo(editor, "news", "news-clip", 0, 5);
  const auto first = RenderAt(editor, Seconds(1));
  CHECK(PixelAt(first, 96, 95).r > 0.9f && PixelAt(first, 96, 95).g < 0.1f);   // the bar, in the red the values gave it
  CHECK(PixelAt(first, 96, 50).r < 0.05f);

  // Version 2 is a thicker bar. It is a new entry; the instance made with version 1 keeps drawing version 1.
  editor.Run(commands::CommandType::InstallGraphicTemplate, commands::InstallGraphicTemplatePayload{BannerTemplate(2, 0.2)});
  CHECK(PixelAt(RenderAt(editor, Seconds(1)), 96, 100).r < 0.05f);   // still thin: version 2 would reach row 100, version 1 stops at row 97

  // The values are the template's controls and are held to their ranges.
  commands::UpdateGraphicPayload thicker;
  thicker.id = "news";
  thicker.values = std::map<std::string, std::string>{{"colour", "#00FF00"}, {"thickness", "0.2"}};
  editor.Run(commands::CommandType::UpdateGraphic, thicker);
  const auto second = RenderAt(editor, Seconds(1));
  CHECK(PixelAt(second, 96, 95).g > 0.9f && PixelAt(second, 96, 95).r < 0.1f);
  CHECK(PixelAt(second, 96, 105).g > 0.9f);   // 0.8 + 0.2 reaches the bottom edge
  thicker.values = std::map<std::string, std::string>{{"thickness", "0.5"}};   // above the control's maximum
  CHECK(Refused(editor, commands::CommandType::UpdateGraphic, thicker));
  thicker.values = std::map<std::string, std::string>{{"nonsense", "1"}};
  CHECK(Refused(editor, commands::CommandType::UpdateGraphic, thicker));
  thicker.values = std::map<std::string, std::string>{{"colour", "red"}};
  CHECK(Refused(editor, commands::CommandType::UpdateGraphic, thicker));

  // A version in use cannot be removed; an unused one can, and a missing one cannot.
  CHECK(Refused(editor, commands::CommandType::RemoveGraphicTemplate, commands::RemoveGraphicTemplatePayload{"banner", 1}));
  editor.Run(commands::CommandType::RemoveGraphicTemplate, commands::RemoveGraphicTemplatePayload{"banner", 2});
  CHECK(Refused(editor, commands::CommandType::RemoveGraphicTemplate, commands::RemoveGraphicTemplatePayload{"banner", 2}));
  // An instance of a version that is not installed is refused.
  commands::CreateGraphicPayload orphan = instance;
  orphan.id = "orphan";
  orphan.template_version = 2;
  CHECK(Refused(editor, commands::CommandType::CreateGraphic, orphan));
  // The clip of a template instance carries the template effect type.
  const auto instance_graph = cutline::timeline::LoadSequenceGraph(editor.store(), "seq-1");
  CHECK_EQ(instance_graph.root()->tracks[0].clips[0].effects[0].effect_type, std::string("motion_graphics_template"));
}

CUTLINE_TEST(GraphicsAndTemplatesSurviveReopeningAndAProjectFromBeforeThemGetsEmptyTables) {
  const auto package = std::filesystem::temp_directory_path() / "cutline-test-graphics-reopen";
  std::filesystem::remove_all(package);
  const auto id = ProjectStore::GenerateProjectUuid();
  std::string expected;
  {
    commands::CommandEnvelope create;
    create.command_id = "c0";
    create.project_id = id;
    create.author_id = "tester";
    create.timestamp_utc = "2026-10-06T00:00:00Z";
    create.type = commands::CommandType::CreateProject;
    create.payload = commands::CreateProjectPayload{"Graphics"};
    create.idempotency_key = "k0";
    auto store = ProjectStore::CreatePackage(package, create);
    int n = 0;
    const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
      commands::CommandEnvelope command;
      command.command_id = "cmd-" + std::to_string(++n);
      command.project_id = id;
      command.author_id = "tester";
      command.base_revision = store->CurrentRevision();
      command.timestamp_utc = "2026-10-06T00:00:00Z";
      command.type = type;
      command.payload = std::move(payload);
      command.idempotency_key = "key-" + std::to_string(n);
      (void)store->Execute(command);
    };
    commands::CreateSequencePayload sequence;
    sequence.id = "seq-1";
    sequence.settings.name = "S";
    sequence.settings.frame_rate = {25, 1};
    sequence.settings.width = 640;
    sequence.settings.height = 360;
    sequence.settings.sample_rate = 48000;
    run(commands::CommandType::CreateSequence, sequence);
    run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
    run(commands::CommandType::InstallGraphicTemplate, commands::InstallGraphicTemplatePayload{BannerTemplate(1, 0.1)});
    commands::CreateGraphicPayload instance;
    instance.id = "news";
    instance.kind = "template";
    instance.template_id = "banner";
    instance.template_version = 1;
    instance.values = {{"colour", "#0000FF"}};
    run(commands::CommandType::CreateGraphic, instance);
    run(commands::CommandType::AddGraphicClip, commands::AddGraphicClipPayload{"clip", "v1", "news", Seconds(0), Seconds(2), ""});
    expected = cutline::timeline::LoadSequenceGraph(*store, "seq-1").root()->tracks[0].clips[0].effects[0].inline_asset;
  }
  {
    auto reopened = ProjectStore::OpenPackage(package);
    const auto graph = cutline::timeline::LoadSequenceGraph(*reopened, "seq-1");
    CHECK_EQ(graph.root()->tracks[0].clips[0].effects[0].inline_asset, expected);
    CHECK(!expected.empty());
    const std::lock_guard<std::mutex> lock(reopened->mutex());
    cutline::db::Execute(reopened->connection(), "DROP TABLE graphics;");
    cutline::db::Execute(reopened->connection(), "DROP TABLE graphic_templates;");
    cutline::db::Execute(reopened->connection(), "DELETE FROM effects;");
    cutline::db::Execute(reopened->connection(), "DELETE FROM clips;");
    cutline::db::Execute(reopened->connection(), "UPDATE project_meta SET schema_version = 10 WHERE singleton = 1;");
  }
  auto migrated = ProjectStore::OpenPackage(package);
  {
    const std::lock_guard<std::mutex> lock(migrated->mutex());
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT COUNT(*) FROM graphics;"), std::int64_t{0});
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT COUNT(*) FROM graphic_templates;"), std::int64_t{0});
  }
  migrated.reset();
  std::filesystem::remove_all(package);
}

int main() { return cutline::testing::RunAll("timeline"); }

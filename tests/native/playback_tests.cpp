// Playback engine and ingest: the two layers that own mutable state.

#include "audio/AudioSink.h"
#include "captions/Captions.h"
#include "render/TextRaster.h"
#include "core/db/Sql.h"
#include "core/jobs/PriorityScheduler.h"
#include "core/util/Sha256.h"
#include "media/Ingest.h"
#include "media/ProxyWorkflow.h"
#include "media/Providers.h"
#include "media/SyntheticSource.h"
#include "playback/PlaybackEngine.h"
#include "tests/native/TestHarness.h"
#include "timeline/SequenceLoader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace commands = cutline::commands;
namespace model = cutline::model;
namespace rates = cutline::time;
using cutline::media::SyntheticPattern;
using cutline::media::SyntheticSpec;
using cutline::playback::EngineConfig;
using cutline::playback::PlaybackEngine;
using cutline::project::ProjectStore;
using cutline::time::RationalTime;
using cutline::timeline::Clip;
using cutline::timeline::Sequence;
using cutline::timeline::SequenceGraph;
using cutline::timeline::Track;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

bool Near(float actual, float expected, float tolerance = 1e-3f) { return std::abs(actual - expected) < tolerance; }

SyntheticSpec CounterSpec() {
  SyntheticSpec spec;
  spec.pattern = SyntheticPattern::Counter;
  spec.width = 160;
  spec.height = 90;
  spec.frame_rate = {30, 1};
  spec.duration = Seconds(20);
  spec.tone_hz = 1000.0;
  spec.tone_amplitude = 0.5f;
  return spec;
}

Clip MakeClip(std::string id, std::string media_id, std::int64_t start, std::int64_t in, std::int64_t out) {
  Clip clip;
  clip.id = std::move(id);
  clip.source_kind = model::SourceKind::Media;
  clip.source_id = std::move(media_id);
  clip.source_in = Seconds(in);
  clip.source_out = Seconds(out);
  clip.timeline_start = Seconds(start);
  clip.start_ticks = clip.timeline_start.ToTicks();
  clip.end_ticks = clip.end().ToTicks();
  return clip;
}

Sequence MakeSequence(std::string id = "seq-1") {
  Sequence sequence;
  sequence.id = std::move(id);
  sequence.name = "Main";
  sequence.frame_rate = {30, 1};
  sequence.width = 160;
  sequence.height = 90;
  sequence.sample_rate = 48000;
  return sequence;
}

Track MakeTrack(std::string id, model::TrackKind kind, std::int64_t order) {
  Track track;
  track.id = std::move(id);
  track.kind = kind;
  track.order = order;
  return track;
}

void Finalise(Sequence& sequence) {
  for (auto& track : sequence.tracks) {
    for (auto& clip : track.clips) {
      clip.start_ticks = clip.timeline_start.ToTicks();
      clip.end_ticks = clip.end().ToTicks();
    }
  }
}

// Maps every media id onto one synthetic counter source.
cutline::playback::MediaLocator CounterLocator() {
  const auto path = CounterSpec().ToPath();
  return [path](const std::string&) { return path; };
}

EngineConfig FloatConfig() {
  EngineConfig config;
  config.compositor.output_format = cutline::media::PixelFormat::Rgba8;
  // Most tests exercise deterministic synchronous behavior. Read-ahead has
  // dedicated cases below; production keeps the EngineConfig defaults.
  config.decode_workers = 0;
  return config;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

SequenceGraph SingleTrackGraph() {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  return graph;
}

}  // namespace

// ------------------------------------------------------------ engine basics ----

CUTLINE_TEST(TheEngineNeedsASequenceAndALocator) {
  cutline::media::RegisterAllProviders();
  CHECK_THROWS(PlaybackEngine(SequenceGraph{}, CounterLocator()));
  CHECK_THROWS(PlaybackEngine(SingleTrackGraph(), nullptr));
  auto config = FloatConfig();
  config.frame_cache_bytes = 0;
  CHECK_THROWS(PlaybackEngine(SingleTrackGraph(), CounterLocator(), config));
  config.frame_cache_bytes = 1024;
  config.decode_workers = 1;
  config.read_ahead_frames = 1;
  config.max_pending_decodes = 0;
  CHECK_THROWS(PlaybackEngine(SingleTrackGraph(), CounterLocator(), config));
  config.max_pending_decodes = 1;
  config.worker_source_limit = 0;
  CHECK_THROWS(PlaybackEngine(SingleTrackGraph(), CounterLocator(), config));
}

CUTLINE_TEST(TheEngineRendersTheCorrectSourceFrame) {
  // The counter pattern encodes its own frame index, so this proves the whole
  // chain -- compile, decode, composite -- landed on the right picture rather
  // than merely producing one.
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(SingleTrackGraph(), CounterLocator(), FloatConfig());

  const auto frame = engine.RenderFrame(RationalTime::FromFrames(90, {30, 1}));
  CHECK_EQ(frame.width(), 160);
  CHECK_EQ(cutline::media::ReadFrameCounter(frame), std::int64_t{90});
}

CUTLINE_TEST(TheMonitorPrefersAProxyAndFallsBackToTheOriginal) {
  cutline::media::RegisterAllProviders();
  SyntheticSpec original = CounterSpec();
  original.pattern = SyntheticPattern::Solid;
  original.red = 1.0f;
  original.green = 0.0f;
  SyntheticSpec proxy = original;
  proxy.red = 0.0f;
  proxy.green = 1.0f;

  auto config = FloatConfig();
  config.prefer_proxies = true;
  config.proxy_locator = [path = proxy.ToPath()](const std::string&) { return path; };
  PlaybackEngine using_proxy(SingleTrackGraph(),
                             [path = original.ToPath()](const std::string&) { return path; }, config);
  const auto proxy_frame = using_proxy.RenderFrame(Seconds(0));
  const auto* proxy_pixel = proxy_frame.row_u8(45) + 80 * 4;
  CHECK(proxy_pixel[1] > proxy_pixel[0]);

  auto originals = using_proxy.OriginalQualityClone();
  const auto cloned_frame = originals->RenderFrame(Seconds(0));
  const auto* cloned_pixel = cloned_frame.row_u8(45) + 80 * 4;
  CHECK(cloned_pixel[0] > cloned_pixel[1]);

  config.proxy_locator = [](const std::string&) { return std::string(); };
  PlaybackEngine fallback(SingleTrackGraph(),
                          [path = original.ToPath()](const std::string&) { return path; }, config);
  const auto original_frame = fallback.RenderFrame(Seconds(0));
  const auto* original_pixel = original_frame.row_u8(45) + 80 * 4;
  CHECK(original_pixel[0] > original_pixel[1]);

  config.prefer_proxies = false;
  config.proxy_locator = [path = proxy.ToPath()](const std::string&) { return path; };
  PlaybackEngine export_quality(SingleTrackGraph(),
                                [path = original.ToPath()](const std::string&) { return path; }, config);
  const auto export_frame = export_quality.RenderFrame(Seconds(0));
  const auto* export_pixel = export_frame.row_u8(45) + 80 * 4;
  CHECK(export_pixel[0] > export_pixel[1]);
}

CUTLINE_TEST(ClipSourceOffsetsReachTheDecoder) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  // Starts at timeline 0 but reads from 5s into the source.
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 5, 15));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  // One second in on the timeline is six seconds into the source: frame 180.
  const auto frame = engine.RenderFrame(Seconds(1));
  CHECK_EQ(cutline::media::ReadFrameCounter(frame), std::int64_t{180});
}

CUTLINE_TEST(RetimedClipsReadTheirSourceFaster) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 0, 10);
  clip.playback_rate = RationalTime(2, 1);
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  // At 2x, two timeline seconds is four source seconds: frame 120.
  const auto frame = engine.RenderFrame(Seconds(2));
  CHECK_EQ(cutline::media::ReadFrameCounter(frame), std::int64_t{120});
}

CUTLINE_TEST(ReversedClipsReadTheirSourceBackwards) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 0, 10);
  clip.reversed = true;
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  // Two seconds in, a reversed 10s clip is reading source second 8: frame 240.
  const auto frame = engine.RenderFrame(Seconds(2));
  CHECK_EQ(cutline::media::ReadFrameCounter(frame), std::int64_t{240});
}

CUTLINE_TEST(TheFrameCacheServesRepeatedRequests) {
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(SingleTrackGraph(), CounterLocator(), FloatConfig());

  const auto at = Seconds(3);
  (void)engine.RenderFrame(at);
  const auto after_first = engine.statistics().cache_misses;
  (void)engine.RenderFrame(at);
  (void)engine.RenderFrame(at);
  CHECK_EQ(engine.statistics().cache_misses, after_first);
  CHECK(engine.statistics().cache_hits >= 2);
}

CUTLINE_TEST(TheFrameCacheIsBounded) {
  cutline::media::RegisterAllProviders();
  auto config = FloatConfig();
  // Room for roughly four 160x90 frames, so walking the timeline evicts.
  config.frame_cache_bytes = 4 * 160 * 128 * 4;
  PlaybackEngine engine(SingleTrackGraph(), CounterLocator(), config);

  // Walk far enough to evict, then come back to the start: that must miss.
  for (std::int64_t frame = 0; frame < 20; ++frame) {
    (void)engine.RenderFrame(RationalTime::FromFrames(frame, {30, 1}));
  }
  const auto before = engine.statistics().cache_misses;
  (void)engine.RenderFrame(RationalTime::FromFrames(0, {30, 1}));
  CHECK(engine.statistics().cache_misses > before);
  CHECK(engine.statistics().cache_evictions > 0);
}

CUTLINE_TEST(PrimeReadAheadWarmsTheRequestedScrubFrame) {
  cutline::media::RegisterAllProviders();
  auto config = FloatConfig();
  config.decode_workers = 1;
  config.read_ahead_frames = 3;
  config.max_pending_decodes = 3;
  PlaybackEngine engine(SingleTrackGraph(), CounterLocator(), config);

  engine.PrimeReadAhead(Seconds(5));
  CHECK(WaitUntil([&engine] { return engine.statistics().read_ahead_completed >= 1; }));
  const auto before = engine.statistics();
  const auto frame = engine.RenderFrame(Seconds(5));

  CHECK_EQ(cutline::media::ReadFrameCounter(frame), std::int64_t{150});
  CHECK_EQ(engine.statistics().cache_misses, before.cache_misses);
  CHECK(engine.statistics().read_ahead_cache_hits > before.read_ahead_cache_hits);
}

CUTLINE_TEST(PrimeReadAheadDoesNotWaitForAnActiveVideoRender) {
  cutline::media::RegisterAllProviders();
  auto config = FloatConfig();
  config.decode_workers = 1;
  config.read_ahead_frames = 3;
  config.max_pending_decodes = 3;

  std::promise<void> locator_entered;
  auto entered = locator_entered.get_future();
  std::promise<void> release_locator;
  auto release = release_locator.get_future().share();
  std::atomic<bool> first{true};
  const auto path = CounterSpec().ToPath();
  PlaybackEngine engine(
      SingleTrackGraph(),
      [&](const std::string&) {
        if (first.exchange(false)) {
          locator_entered.set_value();
          release.wait();
        }
        return path;
      },
      config);

  auto rendering = std::async(std::launch::async, [&] { return engine.RenderFrame(Seconds(0)); });
  CHECK(entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  auto priming = std::async(std::launch::async, [&] { engine.PrimeReadAhead(Seconds(5)); });
  const bool returned_while_render_was_blocked =
      priming.wait_for(std::chrono::seconds(1)) == std::future_status::ready;

  release_locator.set_value();
  priming.get();
  const auto frame = rendering.get();
  CHECK(frame.valid());
  CHECK(returned_while_render_was_blocked);
}

CUTLINE_TEST(RenderingQueuesTheFollowingFramesForReadAhead) {
  cutline::media::RegisterAllProviders();
  auto config = FloatConfig();
  config.decode_workers = 1;
  config.read_ahead_frames = 4;
  config.max_pending_decodes = 4;
  PlaybackEngine engine(SingleTrackGraph(), CounterLocator(), config);

  (void)engine.RenderFrame(Seconds(0));
  CHECK(WaitUntil([&engine] { return engine.statistics().read_ahead_completed >= 4; }));
  const auto before = engine.statistics();
  const auto next = engine.RenderFrame(RationalTime::FromFrames(1, {30, 1}));

  CHECK_EQ(cutline::media::ReadFrameCounter(next), std::int64_t{1});
  CHECK_EQ(engine.statistics().cache_misses, before.cache_misses);
  CHECK(engine.statistics().read_ahead_cache_hits > before.read_ahead_cache_hits);
}

CUTLINE_TEST(DuplicateClipsShareOneDecodedMediaFrame) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", model::TrackKind::Video, 0);
  auto upper = MakeTrack("v2", model::TrackKind::Video, 1);
  lower.clips.push_back(MakeClip("clip-lower", "media-1", 0, 0, 10));
  upper.clips.push_back(MakeClip("clip-upper", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());

  (void)engine.RenderFrame(Seconds(2));
  CHECK_EQ(engine.statistics().cache_misses, std::int64_t{1});
  CHECK_EQ(engine.statistics().cache_hits, std::int64_t{1});
}

CUTLINE_TEST(OfflineMediaRendersAsAGapAndIsOnlyAttemptedOnce) {
  cutline::media::RegisterAllProviders();
  // An empty path means the media is offline.
  PlaybackEngine engine(SingleTrackGraph(), [](const std::string&) { return std::string{}; }, FloatConfig());

  const auto frame = engine.RenderFrame(Seconds(1));
  CHECK_EQ(frame.width(), 160);
  // Black, not a crash and not a stale picture.
  const auto* pixel = frame.row_u8(45) + 80 * 4;
  CHECK_EQ(static_cast<int>(pixel[0]), 0);
  CHECK_EQ(engine.statistics().offline_media, std::int64_t{1});

  // Re-rendering must not retry the lookup; a missing file would otherwise cost
  // a failed open on every frame.
  for (int index = 0; index < 5; ++index) (void)engine.RenderFrame(Seconds(1));
  CHECK_EQ(engine.statistics().offline_media, std::int64_t{1});
}

CUTLINE_TEST(EditingDropsCachedPicturesButKeepsDecodersOpen) {
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(SingleTrackGraph(), CounterLocator(), FloatConfig());
  (void)engine.RenderFrame(Seconds(1));
  CHECK(engine.statistics().cache_misses > 0);

  engine.UpdateSequence(SingleTrackGraph());
  const auto before = engine.statistics().cache_misses;
  (void)engine.RenderFrame(Seconds(1));
  // The same time now misses, because the edit could have changed what that
  // clip resolves to.
  CHECK(engine.statistics().cache_misses > before);
}

CUTLINE_TEST(NestedSequencesAreComposedRecursively) {
  cutline::media::RegisterAllProviders();
  auto inner = MakeSequence("seq-inner");
  auto inner_track = MakeTrack("iv1", model::TrackKind::Video, 0);
  inner_track.clips.push_back(MakeClip("inner-clip", "media-1", 0, 0, 10));
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

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  // The picture that comes out is the nested sequence's own frame, carried up
  // through the clip that wraps it.
  const auto frame = engine.RenderFrame(Seconds(2));
  CHECK_EQ(cutline::media::ReadFrameCounter(frame), std::int64_t{60});
}

CUTLINE_TEST(TheEngineMixesAudioForTheSamePlan) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip-audio", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  const auto block = engine.RenderAudio(Seconds(1), 1024);
  CHECK_EQ(block.frames(), std::int64_t{1024});
  CHECK_EQ(block.channels(), 2);
  // The synthetic tone is at 0.5 amplitude.
  CHECK(Near(block.Peak(0), 0.5f, 0.02f));
}


// ------------------------------------------------- real audio through the engine ----
//
// The same closed-form chirp the media tests use (see cmake/Fixtures.cmake),
// followed through the whole engine: a real decoder, the mixer's retiming and
// reversal, a nested sequence. Positions that are whole samples are compared
// exactly; the one fractional rate is compared in a region where the chirp is slow
// enough for four-point interpolation to be accurate, and the tolerance says so.

namespace {

constexpr double kPi = 3.14159265358979323846;
double ChirpLeft(double t) { return 0.5 * std::sin(2.0 * kPi * (200.0 * t + 1500.0 * t * t)); }

[[nodiscard]] std::optional<std::string> FixturePath(const std::string& name) {
  const auto configured = cutline::testing::EnvironmentValue("CUTLINE_FIXTURE_DIR");
  if (configured.empty()) return std::nullopt;
  const auto path = std::filesystem::path(configured) / name;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
  return path.string();
}

// One media id, "chirp", mapped onto the two-channel reference file.
cutline::playback::MediaLocator ChirpLocator(const std::string& path) {
  return [path](const std::string& id) { return id == "chirp" ? path : std::string(); };
}

Sequence AudioSequence(std::string id, Clip clip) {
  auto sequence = MakeSequence(std::move(id));
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  return sequence;
}

// Worst difference between rendered left-channel samples and the chirp read at
// source position `position(n)` seconds, over `count` samples.
template <typename Position>
double WorstAgainstChirp(const cutline::media::AudioBuffer& block, std::int64_t count, Position position) {
  double worst = 0.0;
  for (std::int64_t n = 0; n < count; ++n) {
    worst = std::max(worst, std::abs(static_cast<double>(block.channel(0)[n]) - ChirpLeft(position(n))));
  }
  return worst;
}

}  // namespace

CUTLINE_TEST(TheEngineReadsARealFileAtTheClipsRateAndDirection) {
  const auto path = FixturePath("av-ref.mkv");
  SKIP_UNLESS(path.has_value(), "av-ref.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  // Twice the speed, forwards: output n is source sample 24000 + 2n, a whole sample.
  {
    auto clip = MakeClip("clip", "chirp", 0, 0, 3);
    clip.source_in = RationalTime(1, 2);
    clip.source_out = Seconds(3);
    clip.playback_rate = RationalTime(2, 1);
    SequenceGraph graph;
    graph.sequences.push_back(AudioSequence("seq-1", clip));
    PlaybackEngine engine(std::move(graph), ChirpLocator(*path), FloatConfig());
    const auto block = engine.RenderAudioAt(1234, 9000, engine.compile_options());
    const auto worst = WorstAgainstChirp(
        block, 9000, [](std::int64_t n) { return (24000.0 + 2.0 * static_cast<double>(1234 + n)) / 48000.0; });
    CHECK(worst < 1e-5);
  }
  // Reversed at its own speed: output n is source sample (out - 1) - n.
  {
    auto clip = MakeClip("clip", "chirp", 0, 1, 3);
    clip.reversed = true;
    SequenceGraph graph;
    graph.sequences.push_back(AudioSequence("seq-1", clip));
    PlaybackEngine engine(std::move(graph), ChirpLocator(*path), FloatConfig());
    const auto block = engine.RenderAudioAt(500, 9000, engine.compile_options());
    const auto worst = WorstAgainstChirp(
        block, 9000, [](std::int64_t n) { return (3.0 * 48000.0 - 1.0 - static_cast<double>(500 + n)) / 48000.0; });
    CHECK(worst < 1e-5);
  }
  // Three halves of the speed, where the chirp is below about 1.4 kHz.
  {
    auto clip = MakeClip("clip", "chirp", 0, 0, 3);
    clip.playback_rate = RationalTime(3, 2);
    SequenceGraph graph;
    graph.sequences.push_back(AudioSequence("seq-1", clip));
    PlaybackEngine engine(std::move(graph), ChirpLocator(*path), FloatConfig());
    const auto block = engine.RenderAudioAt(0, 12000, engine.compile_options());
    const auto worst =
        WorstAgainstChirp(block, 12000, [](std::int64_t n) { return 1.5 * static_cast<double>(n) / 48000.0; });
    CHECK(worst < 2e-3);
  }
}

CUTLINE_TEST(TheEnginePlaysARealFileThroughANestedSequence) {
  const auto path = FixturePath("av-ref.mkv");
  SKIP_UNLESS(path.has_value(), "av-ref.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  // The inner sequence plays the file from its start. The outer clip plays the
  // inner sequence's seconds 1 to 3, reversed: the first sample out is the last one
  // before inner second 3.
  Clip inner_clip = MakeClip("inner-clip", "chirp", 0, 0, 4);
  auto wrap = MakeClip("wrap", "seq-inner", 0, 1, 3);
  wrap.source_kind = model::SourceKind::Sequence;
  wrap.reversed = true;

  SequenceGraph graph;
  graph.sequences.push_back(AudioSequence("seq-outer", wrap));
  graph.sequences.push_back(AudioSequence("seq-inner", inner_clip));
  PlaybackEngine engine(std::move(graph), ChirpLocator(*path), FloatConfig());

  const auto block = engine.RenderAudioAt(0, 20000, engine.compile_options());
  const auto worst = WorstAgainstChirp(
      block, 20000, [](std::int64_t n) { return (3.0 * 48000.0 - 1.0 - static_cast<double>(n)) / 48000.0; });
  CHECK(worst < 1e-5);
  CHECK(block.Peak(0) > 0.3f);
}

CUTLINE_TEST(RenderingInBlocksGivesTheSameSamplesAsRenderingAtOnce) {
  const auto path = FixturePath("av-ref.mkv");
  SKIP_UNLESS(path.has_value(), "av-ref.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto clip = MakeClip("clip", "chirp", 1, 0, 3);
  clip.playback_rate = RationalTime(3, 4);
  SequenceGraph graph;
  graph.sequences.push_back(AudioSequence("seq-1", clip));
  PlaybackEngine engine(std::move(graph), ChirpLocator(*path), FloatConfig());

  constexpr std::int64_t kTotal = 3 * 48000;
  const auto whole = engine.RenderAudioAt(0, kTotal, engine.compile_options());
  std::int64_t position = 0;
  std::int64_t step = 1;
  double worst = 0.0;
  while (position < kTotal) {
    const auto count = std::min<std::int64_t>(step, kTotal - position);
    const auto piece = engine.RenderAudioAt(position, count, engine.compile_options());
    for (std::int64_t n = 0; n < count; ++n) {
      worst = std::max(worst, static_cast<double>(std::abs(piece.channel(1)[n] - whole.channel(1)[position + n])));
    }
    position += count;
    step = step * 3 + 1;  // 1, 4, 13, 40, ... awkward sizes, none a codec or video frame
    if (step > 9000) step = 7;
  }
  CHECK(worst == 0.0);
}

// ---------------------------------------------------------------- transport ----

CUTLINE_TEST(PlaybackAdvancesOnTheAudioClock) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip-audio", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  cutline::audio::OfflineSink sink({48000, 2, 4800});  // 100 ms blocks

  CHECK(!engine.playing());
  engine.Play(sink);
  CHECK(engine.playing());

  // Ten blocks is one second of audio, so the position must have moved a
  // second -- driven by rendered samples, not by a wall clock.
  CHECK_EQ(sink.RenderBlocks(10), 10);
  CHECK_EQ(engine.position().Compare(Seconds(1)), 0);

  engine.Pause();
  CHECK(!engine.playing());
}

CUTLINE_TEST(PositionIsCompensatedForOutputLatency) {
  // The offline sink reports no latency, so position equals elapsed audio. The
  // compensation itself is checked by construction: position() subtracts the
  // sink's latency, and a sink that claims some would pull the picture back.
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip-audio", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  cutline::audio::OfflineSink sink({48000, 2, 2400});
  CHECK_EQ(sink.latency().Compare({0, 1}), 0);

  engine.Play(sink);
  CHECK_EQ(sink.RenderBlocks(4), 4);
  CHECK_EQ(engine.position().Compare({1, 5}), 0);  // 4 * 50 ms
  engine.Pause();
}

CUTLINE_TEST(SeekingMovesTheTransport) {
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(SingleTrackGraph(), CounterLocator(), FloatConfig());
  CHECK_EQ(engine.position().Compare({0, 1}), 0);
  engine.Seek(Seconds(4));
  CHECK_EQ(engine.position().Compare(Seconds(4)), 0);
}

CUTLINE_TEST(PlaybackStopsAtTheEndOfTheSequence) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  // A one-second sequence.
  track.clips.push_back(MakeClip("clip-audio", "media-1", 0, 0, 1));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());
  cutline::audio::OfflineSink sink({48000, 2, 4800});
  engine.Play(sink);
  // Asked for three seconds of blocks; it must stop after one.
  CHECK_EQ(sink.RenderBlocks(30), 10);
  engine.Pause();
}

// ------------------------------------------------------------------- ingest ----

namespace {

ProjectStore* NewProject(std::unique_ptr<ProjectStore>& holder) {
  holder = std::make_unique<ProjectStore>(":memory:");
  holder->Initialize();
  commands::CommandEnvelope create;
  create.command_id = "cmd-create";
  create.project_id = "project-1";
  create.author_id = "tester";
  create.base_revision = 0;
  create.timestamp_utc = "2026-10-05T00:00:00Z";
  create.type = commands::CommandType::CreateProject;
  create.payload = commands::CreateProjectPayload{"Ingest"};
  create.idempotency_key = "key-create";
  (void)holder->Execute(create);
  return holder.get();
}

cutline::media::IngestRequest Request(const ProjectStore& store, std::string media_id, std::string path) {
  cutline::media::IngestRequest request;
  request.project_id = "project-1";
  request.author_id = "tester";
  request.timestamp_utc = "2026-10-05T00:01:00Z";
  request.media_id = std::move(media_id);
  request.path = std::move(path);
  request.base_revision = store.CurrentRevision();
  return request;
}

}  // namespace

CUTLINE_TEST(IngestRecordsMediaAndItsStreams) {
  cutline::media::RegisterAllProviders();
  std::unique_ptr<ProjectStore> holder;
  auto* store = NewProject(holder);

  const auto result = cutline::media::IngestFile(*store, Request(*store, "media-1", CounterSpec().ToPath()));
  CHECK_EQ(result.media_id, std::string("media-1"));
  CHECK(!result.fingerprint.empty());
  CHECK_EQ(result.fingerprint.size(), std::size_t{64});
  CHECK(!result.already_present);
  CHECK_EQ(result.probe.streams.size(), std::size_t{2});

  const std::lock_guard<std::mutex> lock(store->mutex());
  CHECK_EQ(cutline::db::ScalarInt(store->connection(), "SELECT COUNT(*) FROM media;"), 1);
  // Both the video and the audio stream were recorded, which is what a colour
  // and routing decision will later read.
  CHECK_EQ(cutline::db::ScalarInt(store->connection(), "SELECT COUNT(*) FROM media_streams;"), 2);
  CHECK_EQ(cutline::db::ScalarInt(store->connection(),
                                  "SELECT duration_num FROM media WHERE id = 'media-1';"),
           20);
}

CUTLINE_TEST(IngestingTheSameMediaTwiceDoesNotDuplicateIt) {
  cutline::media::RegisterAllProviders();
  std::unique_ptr<ProjectStore> holder;
  auto* store = NewProject(holder);

  const auto path = CounterSpec().ToPath();
  const auto first = cutline::media::IngestFile(*store, Request(*store, "media-1", path));
  const auto second = cutline::media::IngestFile(*store, Request(*store, "media-2", path));
  CHECK(!first.already_present);
  CHECK(second.already_present);
  // The second import resolves to the item that is already there.
  CHECK_EQ(second.media_id, std::string("media-1"));

  const std::lock_guard<std::mutex> lock(store->mutex());
  CHECK_EQ(cutline::db::ScalarInt(store->connection(), "SELECT COUNT(*) FROM media;"), 1);
}


CUTLINE_TEST(IngestIsOneCommandAndUndoesAsAUnit) {
  cutline::media::RegisterAllProviders();
  std::unique_ptr<ProjectStore> holder;
  auto* store = NewProject(holder);
  const auto before = store->CurrentRevision();

  const auto result = cutline::media::IngestFile(*store, Request(*store, "media-1", CounterSpec().ToPath()));
  // One revision, not two: nothing can happen between the media and its streams.
  CHECK_EQ(result.revision, before + 1);
  {
    const std::lock_guard<std::mutex> lock(store->mutex());
    CHECK_EQ(cutline::db::ScalarInt(store->connection(), "SELECT COUNT(*) FROM media_streams;"), 2);
  }

  const auto undone = store->Undo("tester", "2026-10-05T00:02:00Z");
  (void)undone;
  const std::lock_guard<std::mutex> lock(store->mutex());
  CHECK_EQ(cutline::db::ScalarInt(store->connection(), "SELECT COUNT(*) FROM media;"), 0);
  CHECK_EQ(cutline::db::ScalarInt(store->connection(), "SELECT COUNT(*) FROM media_streams;"), 0);
}

CUTLINE_TEST(IngestingMediaThatWasLeftWithoutStreamsRepairsIt) {
  // What an interrupted two-command import (the old behaviour) left behind: the
  // media row, no streams. Finding it again by fingerprint used to report success
  // and leave it that way.
  cutline::media::RegisterAllProviders();
  std::unique_ptr<ProjectStore> holder;
  auto* store = NewProject(holder);
  const auto path = CounterSpec().ToPath();

  commands::ImportMediaPayload bare;
  bare.id = "media-old";
  bare.display_name = "old";
  bare.original_path = path;
  bare.fingerprint = cutline::util::Sha256::Of("cutline-virtual-v1:" + path);
  bare.duration = Seconds(20);
  commands::CommandEnvelope import;
  import.command_id = "cmd-import-old";
  import.project_id = "project-1";
  import.author_id = "tester";
  import.base_revision = store->CurrentRevision();
  import.timestamp_utc = "2026-10-05T00:00:30Z";
  import.type = commands::CommandType::ImportMedia;
  import.payload = bare;
  import.idempotency_key = "key-import-old";
  (void)store->Execute(import);

  const auto result = cutline::media::IngestFile(*store, Request(*store, "media-new", path));
  CHECK(result.already_present);
  CHECK(result.repaired);
  CHECK_EQ(result.media_id, std::string("media-old"));
  const std::lock_guard<std::mutex> lock(store->mutex());
  CHECK_EQ(cutline::db::ScalarInt(store->connection(), "SELECT COUNT(*) FROM media_streams WHERE media_id = 'media-old';"), 2);
}

CUTLINE_TEST(RelinkingMediaMakesTheEngineReadTheNewFile) {
  // The engine used to hold a decoder per media id for ever, so after a relink it
  // went on reading the old file until the process ended.
  cutline::media::RegisterAllProviders();
  auto loud = CounterSpec();
  loud.tone_amplitude = 0.5f;
  auto quiet = CounterSpec();
  quiet.tone_amplitude = 0.125f;
  auto path = std::make_shared<std::string>(loud.ToPath());
  const cutline::playback::MediaLocator locator = [path](const std::string&) { return *path; };

  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(sequence);

  PlaybackEngine engine(graph, locator, FloatConfig());
  CHECK(Near(engine.RenderAudio(Seconds(1), 2048).Peak(0), 0.5f, 0.02f));

  *path = quiet.ToPath();
  engine.UpdateSequence(graph);
  CHECK(Near(engine.RenderAudio(Seconds(1), 2048).Peak(0), 0.125f, 0.02f));
}

CUTLINE_TEST(AudioRenderingPublishesPostFaderTrackAndMasterMeters) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.gain_db = -6.0;
  track.clips.push_back(MakeClip("clip", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  PlaybackEngine engine(graph, CounterLocator(), FloatConfig());
  (void)engine.RenderAudio(Seconds(1), 2048);
  const auto levels = engine.audio_levels();
  CHECK(levels.contains("a1"));
  CHECK(levels.contains("master"));
  CHECK(levels.at("a1").peak_db < -11.0 && levels.at("a1").peak_db > -13.5);
  CHECK(std::abs(levels.at("a1").peak_db - levels.at("master").peak_db) < 0.2);
  CHECK(levels.at("a1").rms_db < levels.at("a1").peak_db);
}

CUTLINE_TEST(MediaThatWasOfflineIsTriedAgainAfterAnEdit) {
  cutline::media::RegisterAllProviders();
  auto path = std::make_shared<std::string>();  // offline
  const cutline::playback::MediaLocator locator = [path](const std::string&) { return *path; };

  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(sequence);

  PlaybackEngine engine(graph, locator, FloatConfig());
  CHECK(engine.RenderAudio(Seconds(1), 2048).Peak(0) == 0.0f);
  CHECK_EQ(engine.statistics().offline_media, std::int64_t{1});
  // Many frames later it is still not retried: one attempt, not one per frame.
  for (int index = 0; index < 5; ++index) (void)engine.RenderAudio(Seconds(1), 512);
  CHECK_EQ(engine.statistics().offline_media, std::int64_t{1});

  *path = CounterSpec().ToPath();  // the file turns up
  engine.UpdateSequence(graph);
  CHECK(engine.RenderAudio(Seconds(1), 2048).Peak(0) > 0.3f);
}

CUTLINE_TEST(IngestIsValidated) {
  cutline::media::RegisterAllProviders();
  std::unique_ptr<ProjectStore> holder;
  auto* store = NewProject(holder);

  CHECK_THROWS(cutline::media::IngestFile(*store, Request(*store, "", CounterSpec().ToPath())));
  CHECK_THROWS(cutline::media::IngestFile(*store, Request(*store, "media-1", "")));
  // A path no provider recognises is refused rather than imported as an empty
  // library entry.
  CHECK_THROWS(cutline::media::IngestFile(*store, Request(*store, "media-1", "/no/such/file.mov")));
}

CUTLINE_TEST(FingerprintsIdentifyMediaForRelink) {
  cutline::media::RegisterAllProviders();
  std::unique_ptr<ProjectStore> holder;
  auto* store = NewProject(holder);

  const auto result = cutline::media::IngestFile(*store, Request(*store, "media-1", CounterSpec().ToPath()));
  const auto found = cutline::media::FindByFingerprint(*store, result.fingerprint);
  CHECK(found.has_value());
  CHECK_EQ(*found, std::string("media-1"));
  CHECK(!cutline::media::FindByFingerprint(*store, "not-a-real-fingerprint").has_value());
}

CUTLINE_TEST(FileFingerprintsAreStableAndDiscriminating) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-fingerprint";
  std::filesystem::create_directories(directory);
  const auto first = directory / "a.bin";
  const auto second = directory / "b.bin";
  const auto copy = directory / "a-copy.bin";
  {
    std::ofstream file(first, std::ios::binary | std::ios::trunc);
    for (int index = 0; index < 100000; ++index) file.put(static_cast<char>(index % 251));
  }
  {
    std::ofstream file(second, std::ios::binary | std::ios::trunc);
    for (int index = 0; index < 100000; ++index) file.put(static_cast<char>((index + 1) % 251));
  }
  std::filesystem::copy_file(first, copy, std::filesystem::copy_options::overwrite_existing);

  const auto a = cutline::media::FingerprintFile(first.string());
  const auto b = cutline::media::FingerprintFile(second.string());
  const auto a_again = cutline::media::FingerprintFile(copy.string());

  // Same bytes, same fingerprint, whatever the file is called: that is what
  // lets relink find footage that has moved or been renamed.
  CHECK_EQ(a, a_again);
  CHECK(a != b);
  CHECK_EQ(a.size(), std::size_t{64});

  CHECK_THROWS(cutline::media::FingerprintFile((directory / "missing.bin").string()));
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(FingerprintsNoticeAChangeAnywhereInTheFile) {
  // Sampling head, middle, and tail rather than only the head is what makes a
  // change in the body detectable.
  const auto directory = std::filesystem::temp_directory_path() / "cutline-fingerprint-mid";
  std::filesystem::create_directories(directory);
  const auto original = directory / "original.bin";
  const auto altered = directory / "altered.bin";

  const auto write = [](const std::filesystem::path& path, std::size_t poison_at) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (std::size_t index = 0; index < 400000; ++index) {
      file.put(index == poison_at ? static_cast<char>(0x7F) : static_cast<char>(index % 97));
    }
  };
  write(original, 400001);     // unchanged
  write(altered, 200000);      // one byte different, right in the middle

  CHECK(cutline::media::FingerprintFile(original.string()) != cutline::media::FingerprintFile(altered.string()));
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(IngestFeedsTheEngineEndToEnd) {
  // The whole slice in one test: import media into a project, build a sequence
  // that cuts it, load a snapshot, and render the frame the timeline asks for.
  cutline::media::RegisterAllProviders();
  std::unique_ptr<ProjectStore> holder;
  auto* store = NewProject(holder);

  const auto ingested = cutline::media::IngestFile(*store, Request(*store, "media-1", CounterSpec().ToPath()));
  CHECK(!ingested.already_present);

  int counter = 0;
  const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-seq-" + std::to_string(++counter);
    command.project_id = "project-1";
    command.author_id = "tester";
    command.base_revision = store->CurrentRevision();
    command.timestamp_utc = "2026-10-05T00:02:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-seq-" + std::to_string(counter);
    (void)store->Execute(command);
  };

  commands::CreateSequencePayload sequence;
  sequence.id = "seq-1";
  sequence.settings.name = "Main";
  sequence.settings.frame_rate = {30, 1};
  sequence.settings.width = 160;
  sequence.settings.height = 90;
  sequence.settings.sample_rate = 48000;
  run(commands::CommandType::CreateSequence, sequence);
  run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});

  commands::InsertClipPayload clip;
  clip.id = "clip-1";
  clip.track_id = "v1";
  clip.source_kind = model::SourceKind::Media;
  clip.media_id = "media-1";
  clip.source_in = Seconds(4);
  clip.source_out = Seconds(12);
  clip.timeline_start = Seconds(0);
  run(commands::CommandType::InsertClip, clip);

  // Resolve media ids back to their recorded paths, which is what a real host
  // does when it hands the engine a project.
  std::unordered_map<std::string, std::string> paths;
  {
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Statement statement(store->connection(), "SELECT id, original_path FROM media;");
    while (statement.Step()) paths.emplace(statement.ColumnText(0), statement.ColumnText(1));
  }
  CHECK_EQ(paths.size(), std::size_t{1});

  const auto graph = cutline::timeline::LoadSequenceGraph(*store, "seq-1");
  PlaybackEngine engine(graph,
                        [paths](const std::string& media_id) {
                          const auto found = paths.find(media_id);
                          return found == paths.end() ? std::string{} : found->second;
                        },
                        FloatConfig());

  // One timeline second into a clip that starts 4s into the source is source
  // second 5: frame 150.
  const auto frame = engine.RenderFrame(Seconds(1));
  CHECK_EQ(cutline::media::ReadFrameCounter(frame), std::int64_t{150});
  CHECK_EQ(engine.statistics().offline_media, std::int64_t{0});
  CHECK_EQ(engine.statistics().decode_failures, std::int64_t{0});
}

// -------------------------------------------------------------- transport safety ----
//
// What happens when the playhead moves, the timeline ends, or something fails while
// audio is being produced on another thread. Offline sinks are driven by hand so
// each case is deterministic; the real-device cases at the end run only where a
// device opened.

CUTLINE_TEST(ASeekWhileABlockIsBeingRenderedDropsThatBlockAndKeepsTheNewPosition) {
  // The old callback read the cursor, rendered, then wrote the advanced cursor
  // back: a seek that landed in between was overwritten and playback resumed from
  // where it had been. The locator runs in the middle of the first render, which is
  // where a real seek would arrive.
  cutline::media::RegisterAllProviders();
  std::shared_ptr<PlaybackEngine> engine;
  bool moved = false;
  const cutline::playback::MediaLocator locator = [&](const std::string&) {
    if (!moved && engine != nullptr) {
      moved = true;
      engine->Seek(Seconds(5));
    }
    return CounterSpec().ToPath();
  };

  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  engine = std::make_shared<PlaybackEngine>(std::move(graph), locator, FloatConfig());

  cutline::audio::OfflineSink sink({48000, 2, 480});
  engine->Play(sink);
  auto block = cutline::media::AudioBuffer::Allocate(48000, 2, 480);

  CHECK(sink.RenderBlock(block));  // dropped: made for the position before the seek
  CHECK_EQ(engine->statistics().audio_blocks_dropped, std::int64_t{1});
  CHECK_EQ(sink.clock().Now().Compare({0, 1}), 0);  // and not counted as played
  CHECK(block.Peak(0) == 0.0f);

  CHECK(sink.RenderBlock(block));
  // The next block starts at the seek target, not where playback was.
  CHECK_EQ(engine->position().Compare(Seconds(5).Add({480, 48000})), 0);
  CHECK(block.Peak(0) > 0.1f);
  engine->Pause();
}

CUTLINE_TEST(ThePlayheadStopsAtTheEndAndSeekingBackRestartsPlayback) {
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip", "media-1", 0, 0, 1));  // one second
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());

  cutline::audio::OfflineSink sink({48000, 2, 4800});
  engine.Play(sink);
  CHECK(engine.playback_state() == cutline::audio::SinkState::Running);
  const auto carried_audio = sink.RenderBlocks(50);
  CHECK_EQ(carried_audio, 10);  // one second at 100 ms a block
  CHECK(engine.playback_state() == cutline::audio::SinkState::Finished);
  CHECK(engine.position().Compare(Seconds(1)) <= 0);  // never past the end

  // Seeking back plays again.
  engine.Seek(Seconds(0));
  CHECK(engine.playback_state() == cutline::audio::SinkState::Running);
  CHECK_EQ(sink.RenderBlocks(3), 3);
  engine.Pause();
}

CUTLINE_TEST(AFailureWhileRenderingAudioIsReportedNotThrown) {
  cutline::media::RegisterAllProviders();
  const cutline::playback::MediaLocator locator = [](const std::string&) -> std::string {
    throw std::runtime_error("the project database went away");
  };
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  PlaybackEngine engine(std::move(graph), locator, FloatConfig());

  cutline::audio::OfflineSink sink({48000, 2, 480});
  engine.Play(sink);
  auto block = cutline::media::AudioBuffer::Allocate(48000, 2, 480);
  CHECK_NO_THROW((void)sink.RenderBlock(block));
  CHECK(engine.playback_state() == cutline::audio::SinkState::Failed);
  CHECK(engine.playback_error().find("database went away") != std::string::npos);
  engine.Pause();
}

CUTLINE_TEST(AudioIsRenderedAtTheDevicesRateNotTheEnginesOwn) {
  // A 44.1 kHz device on a 48 kHz engine. Rendering 441-frame device blocks at the
  // engine's rate played everything 8% flat. A 1 kHz tone has 2000 zero crossings a
  // second at whatever rate it is rendered correctly.
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", model::TrackKind::Audio, 0);
  track.clips.push_back(MakeClip("clip", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  PlaybackEngine engine(std::move(graph), CounterLocator(), FloatConfig());  // 48 kHz

  cutline::audio::OfflineSink sink({44100, 2, 441});
  engine.Play(sink);
  auto block = cutline::media::AudioBuffer::Allocate(44100, 2, 441);
  int crossings = 0;
  float previous = 0.0f;
  for (int index = 0; index < 100; ++index) {  // one second
    CHECK(sink.RenderBlock(block));
    for (std::int64_t n = 0; n < block.frames(); ++n) {
      const auto sample = block.channel(0)[n];
      if ((previous < 0.0f) != (sample < 0.0f)) ++crossings;
      previous = sample;
    }
  }
  engine.Pause();
  CHECK(std::abs(crossings - 2000) <= 8);
}

CUTLINE_TEST(PicturesAudioEditsAndSeeksFromThreeThreadsDoNotCorruptEachOther) {
  // Not a proof of freedom from races (there is no thread sanitizer on this
  // toolchain), but the shape that used to break: audio pulled on one thread while
  // another renders pictures and a third replaces the sequence and seeks. It has to
  // finish, with sensible counters and a valid cursor.
  cutline::media::RegisterAllProviders();
  auto sequence = MakeSequence();
  auto audio = MakeTrack("a1", model::TrackKind::Audio, 0);
  audio.clips.push_back(MakeClip("clip-a", "media-1", 0, 0, 10));
  auto video = MakeTrack("v1", model::TrackKind::Video, 0);
  video.clips.push_back(MakeClip("clip-v", "media-1", 0, 0, 10));
  sequence.tracks.push_back(std::move(video));
  sequence.tracks.push_back(std::move(audio));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(sequence);
  PlaybackEngine engine(graph, CounterLocator(), FloatConfig());

  cutline::audio::OfflineSink sink({48000, 2, 480});
  engine.Play(sink);

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::thread sound([&] {
    auto block = cutline::media::AudioBuffer::Allocate(48000, 2, 480);
    try {
      while (!stop.load()) (void)sink.RenderBlock(block);
    } catch (...) {
      ++failures;
    }
  });
  std::thread picture([&] {
    try {
      for (int index = 0; index < 120 && !stop.load(); ++index) {
        const auto frame = engine.RenderFrame(RationalTime(index, 30));
        if (frame.width() != 160) ++failures;
      }
    } catch (...) {
      ++failures;
    }
  });
  std::thread editor([&] {
    try {
      for (int index = 0; index < 60 && !stop.load(); ++index) {
        engine.UpdateSequence(graph);
        engine.Seek(RationalTime(index % 9, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    } catch (...) {
      ++failures;
    }
  });
  picture.join();
  editor.join();
  stop.store(true);
  sound.join();
  engine.Pause();

  CHECK_EQ(failures.load(), 0);
  CHECK_EQ(engine.statistics().frames_rendered, std::int64_t{120});
  CHECK(engine.position().Compare({0, 1}) >= 0);
  CHECK(engine.position().Compare(Seconds(10)) <= 0);
}

CUTLINE_TEST(ARealDeviceSinkFlushesFinishesAndReportsFailure) {
  // Only where a device actually opened: otherwise the default sink is the offline
  // one, which the cases above already cover. Everything written here is silence.
  auto sink = cutline::audio::OpenDefaultAudioSink({48000, 2, 480});
  SKIP_INAPPLICABLE(sink->name() == "wasapi", "no audio device on this machine");

  const auto wait_for = [&](auto&& condition, int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
  };

  // Runs, and the clock follows what the device played.
  std::atomic<int> blocks{0};
  sink->Start([&](cutline::media::AudioBuffer&) {
    ++blocks;
    return cutline::audio::BlockResult::Audio;
  });
  // The clock counts what the device has accepted, and a fresh device buffer is
  // accepted whole at once (here 200 ms), so after a flush it restarts from about
  // one buffer, not from zero.
  CHECK(wait_for([&] { return sink->clock().Now().Compare({3, 4}) > 0; }, 5000));
  CHECK(sink->state() == cutline::audio::SinkState::Running);

  // A flush returns promptly and restarts the clock.
  const auto flush_started = std::chrono::steady_clock::now();
  sink->Flush();
  CHECK(std::chrono::steady_clock::now() - flush_started < std::chrono::milliseconds(400));
  CHECK(sink->clock().Now().Compare({1, 2}) < 0);
  CHECK(wait_for([&] { return sink->clock().Now().Compare({3, 10}) > 0; }, 3000));
  sink->Stop();

  // End of stream: finished once what was queued has been played.
  std::atomic<int> calls{0};
  sink->Start([&](cutline::media::AudioBuffer&) {
    return ++calls < 4 ? cutline::audio::BlockResult::Audio : cutline::audio::BlockResult::End;
  });
  CHECK(wait_for([&] { return sink->state() == cutline::audio::SinkState::Finished; }, 5000));
  CHECK_EQ(calls.load(), 4);  // and the producer stopped asking
  sink->Stop();

  // A failing callback is a Failed sink with a message, not a terminated process.
  sink->Start([&](cutline::media::AudioBuffer&) -> cutline::audio::BlockResult {
    throw std::runtime_error("mixer exploded");
  });
  CHECK(wait_for([&] { return sink->state() == cutline::audio::SinkState::Failed; }, 5000));
  CHECK(sink->error().find("mixer exploded") != std::string::npos);
  sink->Stop();
}


// -------------------------------------------------------------------- render cache ----

namespace {

cutline::timeline::Effect ExposureEffect(const std::string& id, double stops) {
  cutline::timeline::Effect effect;
  effect.id = id;
  effect.effect_type = "grade";
  cutline::timeline::Parameter parameter;
  parameter.id = id + ":exposure";
  parameter.name = "exposure";
  parameter.value = cutline::anim::AnimatedValue(cutline::anim::Value::Scalar(stops));
  effect.parameters.push_back(std::move(parameter));
  return effect;
}

// Two clips on one track: the first plain, the second graded by `stops`.
SequenceGraph TwoClipGraph(double stops) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-a", "media-1", 0, 0, 5));
  auto second = MakeClip("clip-b", "media-1", 5, 0, 5);
  second.effects.push_back(ExposureEffect("fx-b", stops));
  track.clips.push_back(std::move(second));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  return graph;
}

EngineConfig CachedConfig(const std::shared_ptr<cutline::render::RenderCache>& cache) {
  auto config = FloatConfig();
  config.render_cache = cache;
  return config;
}

float RedAt(const cutline::media::VideoFrame& frame, int x, int y) {
  return cutline::media::ConvertFrame(frame, cutline::media::PixelFormat::RgbaF32).row_f32(y)[x * 4];
}

}  // namespace

CUTLINE_TEST(ARenderedFrameIsServedFromTheCacheTheSecondTimeAndAnEditOnlyRemakesWhatItTouches) {
  auto cache = std::make_shared<cutline::render::RenderCache>();
  PlaybackEngine engine(TwoClipGraph(0.5), CounterLocator(), CachedConfig(cache));
  const auto first = engine.RenderFrame(Seconds(2));
  CHECK_EQ(engine.statistics().render_cache_stores, std::int64_t{1});
  const auto again = engine.RenderFrame(Seconds(2));
  CHECK_EQ(engine.statistics().render_cache_hits, std::int64_t{1});
  CHECK(Near(RedAt(first, 20, 20), RedAt(again, 20, 20), 1e-6f));
  CHECK(again.presentation_time.Compare(Seconds(2)) == 0);

  const auto graded = engine.RenderFrame(Seconds(7));
  const auto before_edit = engine.statistics();

  // The grade on the second clip changes. The first clip's frames are the same function of the same
  // inputs, so they are still found; the second clip's are not.
  engine.UpdateSequence(TwoClipGraph(-1.0));
  const auto untouched = engine.RenderFrame(Seconds(2));
  CHECK_EQ(engine.statistics().render_cache_hits, before_edit.render_cache_hits + 1);
  CHECK(Near(RedAt(untouched, 20, 20), RedAt(first, 20, 20), 1e-6f));
  const auto remade = engine.RenderFrame(Seconds(7));
  CHECK_EQ(engine.statistics().render_cache_hits, before_edit.render_cache_hits + 1);  // that one was not
  CHECK_EQ(engine.statistics().render_cache_stores, before_edit.render_cache_stores + 1);
  // And it is the new grade (half a stop up before, a stop down now), not a stale picture.
  const auto brightest = [&](const cutline::media::VideoFrame& f) {
    float best = 0.0f;
    for (int y = 0; y < f.height(); y += 3) {
      for (int x = 0; x < f.width(); x += 3) best = std::max(best, RedAt(f, x, y));
    }
    return best;
  };
  CHECK(brightest(remade) < brightest(graded));
}

CUTLINE_TEST(AnIncompletePictureIsNotKeptToBeServedAgain) {
  auto cache = std::make_shared<cutline::render::RenderCache>();
  // Media that is offline renders as a gap and reports a missing frame.
  PlaybackEngine engine(TwoClipGraph(0.0), [](const std::string&) { return std::string{}; }, CachedConfig(cache));
  (void)engine.RenderFrame(Seconds(2));
  (void)engine.RenderFrame(Seconds(2));
  CHECK_EQ(engine.statistics().render_cache_stores, std::int64_t{0});
  CHECK_EQ(engine.statistics().render_cache_hits, std::int64_t{0});
  CHECK_EQ(cache->statistics().entries, std::size_t{0});
}

CUTLINE_TEST(ChangedMediaIsNotServedFromAPictureMadeFromTheOldFile) {
  auto cache = std::make_shared<cutline::render::RenderCache>();
  auto spec = CounterSpec();
  auto path = std::make_shared<std::string>(spec.ToPath());
  PlaybackEngine engine(TwoClipGraph(0.0), [path](const std::string&) { return *path; }, CachedConfig(cache));
  const auto original = engine.RenderFrame(Seconds(2));
  (void)engine.RenderFrame(Seconds(2));
  CHECK_EQ(engine.statistics().render_cache_hits, std::int64_t{1});
  // The source at that location is replaced by something else.
  spec.pattern = SyntheticPattern::Bars;
  *path = spec.ToPath();
  engine.UpdateSequence(TwoClipGraph(0.0));  // the app does this when media is relinked: decoders are reopened
  const auto replaced = engine.RenderFrame(Seconds(2));
  CHECK_EQ(engine.statistics().render_cache_hits, std::int64_t{1});
  bool different = false;
  for (int y = 0; y < original.height(); y += 4) {
    for (int x = 0; x < original.width(); x += 4) different = different || std::abs(RedAt(original, x, y) - RedAt(replaced, x, y)) > 0.05f;
  }
  CHECK(different);
}

CUTLINE_TEST(RenderingARangeAheadMakesPlayingItAMatterOfReadingAndTheIndicatorShowsWhatIsReady) {
  auto cache = std::make_shared<cutline::render::RenderCache>();
  PlaybackEngine engine(TwoClipGraph(0.5), CounterLocator(), CachedConfig(cache));
  std::int64_t last_done = 0, last_total = 0;
  const auto first = engine.PreRender(Seconds(1), Seconds(2), {}, [&](std::int64_t done, std::int64_t total) {
    last_done = done;
    last_total = total;
  });
  CHECK_EQ(first.rendered, std::int64_t{30});
  CHECK_EQ(first.already_cached, std::int64_t{0});
  CHECK_EQ(last_done, std::int64_t{30});
  CHECK_EQ(last_total, std::int64_t{30});

  // The indicator: 30 ready frames from 1 s, then none.
  const auto ready = engine.CachedFrames(Seconds(1), 60);
  CHECK_EQ(ready.size(), std::size_t{60});
  CHECK(std::all_of(ready.begin(), ready.begin() + 30, [](bool b) { return b; }));
  CHECK(std::none_of(ready.begin() + 30, ready.end(), [](bool b) { return b; }));

  // Doing it again renders nothing; extending the range renders only the new part; playing it is all hits.
  const auto second = engine.PreRender(Seconds(1), Seconds(2));
  CHECK_EQ(second.rendered, std::int64_t{0});
  CHECK_EQ(second.already_cached, std::int64_t{30});
  const auto extended = engine.PreRender(Seconds(1), RationalTime(5, 2));
  CHECK_EQ(extended.already_cached, std::int64_t{30});
  CHECK_EQ(extended.rendered, std::int64_t{15});
  const auto hits_before = engine.statistics().render_cache_hits;
  for (int frame = 0; frame < 45; ++frame) (void)engine.RenderFrame(RationalTime(30 + frame, 30));
  CHECK_EQ(engine.statistics().render_cache_hits, hits_before + 45);

  // An edit to the second clip leaves frames inside the first clip ready and takes away the second's.
  (void)engine.RenderFrame(Seconds(7));
  CHECK(engine.CachedFrames(Seconds(7), 1)[0]);
  engine.UpdateSequence(TwoClipGraph(-1.0));
  CHECK(!engine.CachedFrames(Seconds(7), 1)[0]);
  const auto all_ready = engine.CachedFrames(Seconds(1), 45);
  CHECK(std::all_of(all_ready.begin(), all_ready.end(), [](bool b) { return b; }));  // those are all inside the first clip

  // Cancelling stops between frames and says so.
  int polls = 0;
  const auto cancelled = engine.PreRender(Seconds(6), Seconds(8), [&]() { return ++polls > 4; });
  CHECK(cancelled.cancelled);
  CHECK_EQ(cancelled.rendered, std::int64_t{4});
}

CUTLINE_TEST(APreviewAndAnExportShareWhatEitherHasAlreadyRenderedThroughTheDiskTier) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-engine-cache";
  std::filesystem::remove_all(directory);
  cutline::render::RenderCacheConfig cache_config;
  cache_config.disk_directory = directory;
  std::vector<float> from_preview;
  {
    auto cache = std::make_shared<cutline::render::RenderCache>(cache_config);
    PlaybackEngine preview(TwoClipGraph(0.5), CounterLocator(), CachedConfig(cache));
    const auto frame = preview.RenderFrame(Seconds(7));
    for (int x = 0; x < frame.width(); x += 8) from_preview.push_back(RedAt(frame, x, 40));
  }
  {
    // A second engine, as an export has, over a fresh cache that finds the first one's files.
    auto cache = std::make_shared<cutline::render::RenderCache>(cache_config);
    PlaybackEngine exporter(TwoClipGraph(0.5), CounterLocator(), CachedConfig(cache));
    const auto frame = exporter.RenderFrame(Seconds(7));
    CHECK_EQ(exporter.statistics().render_cache_hits, std::int64_t{1});
    CHECK_EQ(cache->statistics().disk_hits, std::int64_t{1});
    std::size_t i = 0;
    for (int x = 0; x < frame.width(); x += 8) CHECK(Near(RedAt(frame, x, 40), from_preview[i++], 1e-6f));
  }
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(CaptionsBurnedInForAnExportAreTheSameAsTheMonitorsAndTheCacheKeepsThemApart) {
  SKIP_INAPPLICABLE(cutline::render::text::Available(), "this build has no text rasteriser");
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", model::TrackKind::Video, 0);
  track.clips.push_back(MakeClip("clip-a", "media-1", 0, 0, 5));
  sequence.tracks.push_back(std::move(track));
  cutline::captions::Track captions;
  captions.id = "cap";
  cutline::captions::Cue cue;
  cue.id = "c1";
  cue.start = Seconds(1);
  cue.end = Seconds(4);
  cue.text = "Burned in";
  cue.resolved_style_json = "{\"size\":0.12}";
  captions.cues.push_back(cue);
  sequence.caption_tracks.push_back(captions);
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(sequence);

  const auto difference = [](const cutline::media::VideoFrame& a, const cutline::media::VideoFrame& b) {
    double sum = 0.0;
    for (int y = 0; y < a.height(); ++y) {
      for (int x = 0; x < a.width(); ++x) sum += std::abs(RedAt(a, x, y) - RedAt(b, x, y));
    }
    return sum;
  };

  auto cache = std::make_shared<cutline::render::RenderCache>();
  PlaybackEngine monitor(graph, CounterLocator(), CachedConfig(cache));
  const auto without = monitor.RenderFrame(Seconds(2));
  // An export asks for captions through the compile options; the monitor's own settings are untouched.
  auto options = monitor.compile_options();
  options.include_captions = true;
  const auto burned = monitor.RenderFrame(Seconds(2), options);
  CHECK(difference(without, burned) > 5.0);
  CHECK(difference(without, monitor.RenderFrame(Seconds(2))) < 1e-6);  // still without, and from the cache, not the burned frame
  // A monitor set to show captions draws exactly what the export drew.
  auto showing = CachedConfig(std::make_shared<cutline::render::RenderCache>());
  showing.compile.include_captions = true;
  PlaybackEngine other(graph, CounterLocator(), showing);
  CHECK(difference(other.RenderFrame(Seconds(2)), burned) < 1e-6);
  // Outside the cue it makes no difference.
  CHECK(difference(monitor.RenderFrame(Seconds(0)), monitor.RenderFrame(Seconds(0), options)) < 1e-6);
}

CUTLINE_TEST(PrioritySchedulerRunsAVisibleFrameBeforeQueuedBackgroundAnalysis) {
  using namespace std::chrono_literals;
  cutline::jobs::PriorityScheduler scheduler({1, 8, 0});
  std::mutex mutex;
  std::condition_variable entered;
  std::condition_variable release;
  bool blocker_started = false;
  bool unblock = false;
  std::vector<std::string> order;
  CHECK(scheduler.Submit("blocker", cutline::jobs::Priority::Interactive, 1,
                         [&](const auto&) {
                           std::unique_lock<std::mutex> lock(mutex);
                           blocker_started = true;
                           entered.notify_one();
                           release.wait(lock, [&] { return unblock; });
                         }).id != 0);
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(entered.wait_for(lock, 1s, [&] { return blocker_started; }));
  }
  CHECK(scheduler.Submit("analysis", cutline::jobs::Priority::Analysis, 1,
                         [&](const auto&) { order.push_back("analysis"); }).id != 0);
  CHECK(scheduler.Submit("visible", cutline::jobs::Priority::VisibleFrame, 1,
                         [&](const auto&) { order.push_back("visible"); }).id != 0);
  {
    const std::lock_guard<std::mutex> lock(mutex);
    unblock = true;
  }
  release.notify_one();
  CHECK(scheduler.WaitIdle(2s));
  CHECK_EQ(order.size(), std::size_t{2});
  CHECK_EQ(order[0], std::string{"visible"});
  CHECK_EQ(order[1], std::string{"analysis"});
}

CUTLINE_TEST(PrioritySchedulerMakesRoomForInteractiveWorkAndCancelsOldGenerations) {
  using namespace std::chrono_literals;
  cutline::jobs::PriorityScheduler scheduler({1, 2, 0});
  std::mutex mutex;
  std::condition_variable entered;
  std::condition_variable release;
  bool blocker_started = false;
  bool unblock = false;
  CHECK(scheduler.Submit("blocker", cutline::jobs::Priority::Interactive, 1,
                         [&](const auto&) {
                           std::unique_lock<std::mutex> lock(mutex);
                           blocker_started = true;
                           entered.notify_one();
                           release.wait(lock, [&] { return unblock; });
                         }).id != 0);
  {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(entered.wait_for(lock, 1s, [&] { return blocker_started; }));
  }
  std::atomic<int> ran{0};
  const auto proxy = scheduler.Submit("proxy", cutline::jobs::Priority::Proxy, 1,
                                      [&](const auto&) { ++ran; });
  const auto analysis = scheduler.Submit("analysis", cutline::jobs::Priority::Analysis, 1,
                                         [&](const auto&) { ++ran; });
  CHECK(proxy.id != 0 && analysis.id != 0);
  const auto visible = scheduler.Submit("visible", cutline::jobs::Priority::VisibleFrame, 2,
                                        [&](const auto&) { ran.fetch_add(10); });
  CHECK(visible.result == cutline::jobs::SubmitResult::ReplacedBackgroundWork);
  scheduler.AdvanceGeneration(2);
  CHECK(scheduler.Submit("stale", cutline::jobs::Priority::VisibleFrame, 1,
                         [&](const auto&) { ran.fetch_add(100); }).result == cutline::jobs::SubmitResult::Stale);
  {
    const std::lock_guard<std::mutex> lock(mutex);
    unblock = true;
  }
  release.notify_one();
  CHECK(scheduler.WaitIdle(2s));
  CHECK_EQ(ran.load(), 10);
  const auto stats = scheduler.statistics();
  CHECK_EQ(stats.replaced, std::uint64_t{1});
  CHECK(stats.cancelled >= 2);
}

int main() { return cutline::testing::RunAll("playback"); }

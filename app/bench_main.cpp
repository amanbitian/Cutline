// Per-frame cost of the render and mix paths.
//
// Exists so optimisation is measured rather than asserted. It times the work a
// monitor actually does once per frame -- compile a plan, decode, composite,
// mix -- at a realistic size and track count, and reports a per-frame figure
// against the frame budget.
//
//   cutline_bench [frames]

// getenv is used for optional benchmark inputs; the MSVC deprecation is noise here.
#define _CRT_SECURE_NO_WARNINGS

#include "audio/AudioMixer.h"
#include "core/commands/Command.h"
#include "core/project/ProjectStore.h"
#include "timeline/SequenceLoader.h"
#include "media/Providers.h"
#include "media/SyntheticSource.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/D3D11Compositor.h"
#include "media/Source.h"
#include "render/FlowCache.h"
#include "timeline/TimelineCompiler.h"

#include <fstream>
#include <algorithm>
#include <chrono>
#include <utility>
#include <tuple>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace model = cutline::model;
using cutline::anim::Value;
using cutline::media::PixelFormat;
using cutline::media::VideoFrame;
using cutline::render::Compositor;
using cutline::render::CompositorConfig;
using cutline::time::RationalTime;
using cutline::timeline::Clip;
using cutline::timeline::Effect;
using cutline::timeline::Parameter;
using cutline::timeline::Sequence;
using cutline::timeline::SourceRequest;
using cutline::timeline::TimelineCompiler;
using cutline::timeline::Track;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr double kFrameBudgetMs = 1000.0 / 25.0;

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

Effect MakeEffect(std::string id, std::string type, std::vector<std::pair<std::string, Value>> parameters,
                  std::int64_t order) {
  Effect effect;
  effect.id = std::move(id);
  effect.effect_type = std::move(type);
  effect.order = order;
  for (auto& [name, value] : parameters) {
    Parameter parameter;
    parameter.id = effect.id + ":" + name;
    parameter.name = name;
    parameter.value = cutline::anim::AnimatedValue(value);
    effect.parameters.push_back(std::move(parameter));
  }
  return effect;
}

Sequence BuildSequence(int video_tracks, int effects_per_clip) {
  Sequence sequence;
  sequence.id = "bench";
  sequence.name = "Bench";
  sequence.frame_rate = {25, 1};
  sequence.width = kWidth;
  sequence.height = kHeight;
  sequence.sample_rate = 48000;

  for (int index = 0; index < video_tracks; ++index) {
    Track track;
    track.id = "v" + std::to_string(index);
    track.kind = model::TrackKind::Video;
    track.order = index;

    Clip clip;
    clip.id = "clip-" + std::to_string(index);
    clip.source_kind = model::SourceKind::Media;
    clip.source_id = "media-" + std::to_string(index);
    clip.source_in = Seconds(0);
    clip.source_out = Seconds(60);
    clip.timeline_start = Seconds(0);
    clip.start_ticks = clip.timeline_start.ToTicks();
    clip.end_ticks = clip.end().ToTicks();

    if (index > 0) {
      clip.effects.push_back(MakeEffect(clip.id + ":motion", "motion",
                                        {{"scale", Value::Vec2(80.0, 80.0)},
                                         {"position", Value::Vec2(10.0 * index, 0.0)}},
                                        0));
    }
    for (int effect = 0; effect < effects_per_clip; ++effect) {
      clip.effects.push_back(MakeEffect(clip.id + ":grade" + std::to_string(effect), "grade",
                                        {{"exposure", Value::Scalar(0.05)},
                                         {"saturation", Value::Scalar(98.0)}},
                                        effect + 1));
    }
    clip.effects.push_back(MakeEffect(clip.id + ":opacity", "opacity",
                                      {{"value", Value::Scalar(index == 0 ? 1.0 : 0.8)}},
                                      effects_per_clip + 1));
    track.clips.push_back(std::move(clip));
    sequence.tracks.push_back(std::move(track));
  }

  Track audio;
  audio.id = "a0";
  audio.kind = model::TrackKind::Audio;
  audio.order = 0;
  Clip audio_clip;
  audio_clip.id = "clip-audio";
  audio_clip.source_kind = model::SourceKind::Media;
  audio_clip.source_id = "media-0";
  audio_clip.source_in = Seconds(0);
  audio_clip.source_out = Seconds(60);
  audio_clip.timeline_start = Seconds(0);
  audio_clip.start_ticks = 0;
  audio_clip.end_ticks = audio_clip.end().ToTicks();
  audio.clips.push_back(std::move(audio_clip));
  sequence.tracks.push_back(std::move(audio));
  return sequence;
}

template <typename Body>
double TimeMilliseconds(int iterations, Body&& body) {
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < iterations; ++index) body(index);
  const auto finished = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(finished - start).count() / iterations;
}

void Report(const std::string& label, double milliseconds) {
  std::cout << "  " << std::left << std::setw(38) << label << std::right << std::setw(10) << std::fixed;
  // Costs under 0.1 ms are shown in microseconds: three decimals of milliseconds
  // printed the compile-scaling cases as 0.000 and hid exactly what they measure.
  if (milliseconds < 0.1) {
    std::cout << std::setprecision(3) << (milliseconds * 1000.0) << " us/call";
  } else {
    std::cout << std::setprecision(3) << milliseconds << " ms/frame";
    std::cout << "   " << std::setw(6) << std::setprecision(1) << (1000.0 / milliseconds) << " fps";
    if (milliseconds > kFrameBudgetMs) std::cout << "  (over a 25 fps budget)";
  }
  std::cout << "\n";
}

// One video track holding `clips` back-to-back one-second clips, each with an
// effect, which is the shape that exposes anything linear in project size.
Sequence BuildLongSequence(std::int64_t clips) {
  Sequence sequence;
  sequence.id = "long";
  sequence.name = "Long";
  sequence.frame_rate = {25, 1};
  sequence.width = 1920;
  sequence.height = 1080;
  sequence.sample_rate = 48000;
  Track track;
  track.id = "v0";
  track.kind = model::TrackKind::Video;
  track.order = 0;
  for (std::int64_t index = 0; index < clips; ++index) {
    Clip clip;
    clip.id = "clip-" + std::to_string(index);
    clip.source_kind = model::SourceKind::Media;
    clip.source_id = "media-0";
    clip.source_in = Seconds(0);
    clip.source_out = Seconds(1);
    clip.timeline_start = Seconds(index);
    clip.start_ticks = clip.timeline_start.ToTicks();
    clip.end_ticks = clip.end().ToTicks();
    clip.effects.push_back(MakeEffect(clip.id + ":fx", "opacity", {{"value", Value::Scalar(0.9)}}, 0));
    track.clips.push_back(std::move(clip));
  }
  sequence.tracks.push_back(std::move(track));
  return sequence;
}

// A project with one small target sequence and many others carrying animation,
// so a load that reads the whole project is visibly slower than one that does not.
std::unique_ptr<cutline::project::ProjectStore> BuildAnimatedProject(int other_sequences) {
  namespace commands = cutline::commands;
  auto store = std::make_unique<cutline::project::ProjectStore>(":memory:");
  store->Initialize();
  int counter = 0;
  const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter);
    command.project_id = "bench";
    command.author_id = "bench";
    command.base_revision = store->CurrentRevision();
    command.timestamp_utc = "2026-10-06T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter);
    const auto result = store->Execute(command);
    (void)result;
  };
  run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Bench"});
  commands::ImportMediaPayload media;
  media.id = "media-0";
  media.display_name = "m.mov";
  media.original_path = "/m.mov";
  media.fingerprint = "fp";
  media.duration = Seconds(600);
  run(commands::CommandType::ImportMedia, media);

  for (int sequence_index = 0; sequence_index <= other_sequences; ++sequence_index) {
    const auto id = sequence_index == 0 ? std::string("target") : "other-" + std::to_string(sequence_index);
    commands::CreateSequencePayload sequence;
    sequence.id = id;
    sequence.settings.name = id;
    sequence.settings.frame_rate = {25, 1};
    sequence.settings.width = 1920;
    sequence.settings.height = 1080;
    sequence.settings.sample_rate = 48000;
    run(commands::CommandType::CreateSequence, sequence);
    run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{id + "-v", id, 0, "stereo", "V1"});
    commands::InsertClipPayload clip;
    clip.id = id + "-clip";
    clip.track_id = id + "-v";
    clip.source_kind = model::SourceKind::Media;
    clip.media_id = "media-0";
    clip.source_in = Seconds(0);
    clip.source_out = Seconds(5);
    clip.timeline_start = Seconds(0);
    run(commands::CommandType::InsertClip, clip);
    // The target is small; every other sequence is animation-heavy.
    const int effects = sequence_index == 0 ? 1 : 10;
    for (int effect_index = 0; effect_index < effects; ++effect_index) {
      commands::AddEffectPayload effect;
      effect.id = id + "-fx" + std::to_string(effect_index);
      effect.owner_kind = model::EffectOwner::Clip;
      effect.owner_id = clip.id;
      effect.effect_type = "grade";
      effect.order = effect_index;
      effect.parameters = {{effect.id + ":a", "exposure", Value::Scalar(0.0)}};
      run(commands::CommandType::AddEffect, effect);
      for (int key = 0; key < 3 && sequence_index != 0; ++key) {
        commands::SetKeyframePayload keyframe;
        keyframe.parameter_id = effect.id + ":a";
        keyframe.keyframe = {Seconds(key), Value::Scalar(0.1 * key), cutline::anim::Interpolation::Linear, {}, {}};
        run(commands::CommandType::SetKeyframe, keyframe);
      }
    }
  }
  return store;
}

// ---- edit command latency ----------------------------------------------------

struct Percentiles final {
  double median{};
  double p95{};
  double p99{};
  double max{};
};

Percentiles Summarise(std::vector<double> milliseconds) {
  std::sort(milliseconds.begin(), milliseconds.end());
  const auto at = [&](double fraction) {
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(milliseconds.size() - 1) + 0.5);
    return milliseconds[std::min(index, milliseconds.size() - 1)];
  };
  return {at(0.5), at(0.95), at(0.99), milliseconds.back()};
}

void ReportLatency(const std::string& label, const std::vector<double>& milliseconds) {
  const auto stats = Summarise(milliseconds);
  std::cout << "  " << std::left << std::setw(34) << label << std::right << " n=" << std::setw(4) << milliseconds.size()
            << "  median " << std::setw(8) << std::fixed << std::setprecision(3) << stats.median << "  p95 " << std::setw(8)
            << stats.p95 << "  p99 " << std::setw(8) << stats.p99 << "  max " << std::setw(8) << stats.max << " ms\n";
}

// A project of \`tracks\` video tracks with \`per_track\` one-second clips each,
// some animated, some joined by transitions, driven only through the command
// service and timed per command.
class EditProject final {
 public:
  EditProject(std::unique_ptr<cutline::project::ProjectStore> store, std::string project_id, int tracks, int per_track)
      : store_(std::move(store)), project_id_(std::move(project_id)), tracks_(tracks), per_track_(per_track) {}

  double Run(cutline::commands::CommandType type, cutline::commands::CommandPayload payload) {
    cutline::commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter_);
    command.project_id = project_id_;
    command.author_id = "bench";
    command.base_revision = store_->CurrentRevision();
    command.timestamp_utc = "2026-10-06T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter_);
    const auto start = std::chrono::steady_clock::now();
    const auto result = store_->Execute(command);
    const auto stop = std::chrono::steady_clock::now();
    (void)result;
    return std::chrono::duration<double, std::milli>(stop - start).count();
  }

  void Build() {
    namespace commands = cutline::commands;
    commands::ImportMediaPayload media;
    media.id = "media-0";
    media.display_name = "m.mov";
    media.original_path = "/m.mov";
    media.fingerprint = "fp";
    media.duration = Seconds(7200);
    Run(commands::CommandType::ImportMedia, media);
    commands::CreateSequencePayload sequence;
    sequence.id = "seq";
    sequence.settings.name = "seq";
    sequence.settings.frame_rate = {25, 1};
    sequence.settings.width = 1920;
    sequence.settings.height = 1080;
    sequence.settings.sample_rate = 48000;
    Run(commands::CommandType::CreateSequence, sequence);

    for (int track = 0; track < tracks_; ++track) {
      Run(commands::CommandType::AddVideoTrack,
          commands::AddTrackPayload{"t" + std::to_string(track), "seq", track, "stereo", "V"});
      for (int index = 0; index < per_track_; ++index) {
        commands::InsertClipPayload clip;
        clip.id = ClipId(track, index);
        clip.track_id = "t" + std::to_string(track);
        clip.source_kind = model::SourceKind::Media;
        clip.media_id = "media-0";
        clip.source_in = Seconds(index);
        clip.source_out = Seconds(index + 1);
        clip.timeline_start = Seconds(index);
        Run(commands::CommandType::InsertClip, clip);
      }
    }
    // Track 0: a dissolve at every tenth cut. Track 1: animated clips.
    for (int index = 10; index < per_track_; index += 10) {
      commands::AddTransitionPayload join;
      join.id = "tr-" + std::to_string(index);
      join.track_id = "t0";
      join.kind = "cross_dissolve";
      join.from_clip_id = ClipId(0, index - 1);
      join.to_clip_id = ClipId(0, index);
      join.timeline_start = RationalTime(index * 4 - 1, 4);
      join.duration = RationalTime(1, 2);
      Run(commands::CommandType::AddTransition, join);
    }
    for (int index = 0; index < per_track_; index += 2) {
      commands::AddEffectPayload effect;
      effect.id = "fx-" + std::to_string(index);
      effect.owner_kind = model::EffectOwner::Clip;
      effect.owner_id = ClipId(1, index);
      effect.effect_type = "grade";
      effect.parameters = {{effect.id + ":e", "exposure", Value::Scalar(0.0)},
                           {effect.id + ":p", "position", Value::Vec2(0.0, 0.0)}};
      Run(commands::CommandType::AddEffect, effect);
      for (const auto& [tenths, value] : std::vector<std::pair<int, double>>{{0, 0.0}, {5, 1.0}, {9, 0.2}}) {
        commands::SetKeyframePayload keyframe;
        keyframe.parameter_id = effect.id + ":e";
        keyframe.keyframe = {RationalTime(tenths, 10), Value::Scalar(value), cutline::anim::Interpolation::EaseInOut, {}, {}};
        Run(commands::CommandType::SetKeyframe, keyframe);
      }
    }
  }

  [[nodiscard]] static std::string ClipId(int track, int index) {
    return "c" + std::to_string(track) + "-" + std::to_string(index);
  }
  void Close() { store_.reset(); }  // release the database before its files are deleted
  [[nodiscard]] cutline::project::ProjectStore& store() { return *store_; }
  [[nodiscard]] int per_track() const { return per_track_; }
  [[nodiscard]] std::int64_t clip_count() const { return static_cast<std::int64_t>(tracks_) * per_track_; }

 private:
  std::unique_ptr<cutline::project::ProjectStore> store_;
  std::string project_id_;
  int tracks_;
  int per_track_;
  int counter_{0};
};

void MeasureEditLatency(const std::string& storage_label, bool on_disk, int per_track, bool durable = false) {
  namespace commands = cutline::commands;
  commands::CommandEnvelope create;
  create.command_id = "cmd-create";
  create.project_id = "bench-edit";
  create.author_id = "bench";
  create.timestamp_utc = "2026-10-06T00:00:00Z";
  create.type = commands::CommandType::CreateProject;
  create.payload = commands::CreateProjectPayload{"Edits"};
  create.idempotency_key = "key-create";

  std::unique_ptr<cutline::project::ProjectStore> store;
  std::filesystem::path package;
  if (on_disk) {
    package = std::filesystem::temp_directory_path() / "cutline-bench-edits.vproj";
    std::filesystem::remove_all(package);
    cutline::project::SnapshotPolicy policy;
    if (durable) policy.durability = cutline::project::Durability::Full;
    store = cutline::project::ProjectStore::CreatePackage(package, create, policy);
  } else {
    store = std::make_unique<cutline::project::ProjectStore>(":memory:");
    store->Initialize();
    const auto result = store->Execute(create);
    (void)result;
  }

  EditProject project(std::move(store), "bench-edit", 4, per_track);
  const auto build_start = std::chrono::steady_clock::now();
  project.Build();
  const auto build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - build_start).count();

  std::cout << "\n  " << storage_label << ": " << project.clip_count() << " clips on 4 tracks, "
            << (per_track / 10) << " transitions, " << (per_track / 2) << " animated clips (built in " << std::fixed
            << std::setprecision(1) << build_seconds << " s)\n";

  const int rounds = std::min(per_track - 2, 150);
  std::vector<double> move, slip, split, speed, add_transition, retime_transition, undo;

  for (int index = 1; index <= rounds; ++index) {
    // Move to free space past the end of the sequence.
    move.push_back(project.Run(commands::CommandType::MoveClip,
                               commands::MoveClipPayload{EditProject::ClipId(2, index), "t2", Seconds(2000 + index)}));
    // A slip: new source range, same place on the timeline.
    slip.push_back(project.Run(commands::CommandType::TrimClip,
                               commands::TrimClipPayload{EditProject::ClipId(3, index), Seconds(3000 + index),
                                                         Seconds(3001 + index), Seconds(index)}));
  }
  for (int index = 0; index < rounds && index * 2 < per_track; ++index) {
    // Cut an animated clip in half: splits its keyframed curves.
    split.push_back(project.Run(commands::CommandType::SplitClip,
                                commands::SplitClipPayload{EditProject::ClipId(1, index * 2),
                                                           "split-" + std::to_string(index),
                                                           RationalTime(index * 2 * 2 + 1, 2)}));
  }
  // Odd clips on the animated track: none of them is split above and none has a
  // transition, so each edit is valid and independent of the others.
  for (int index = 0; index < rounds && index * 2 + 1 < per_track; ++index) {
    speed.push_back(project.Run(commands::CommandType::SetClipSpeed,
                                commands::SetClipSpeedPayload{EditProject::ClipId(1, index * 2 + 1), RationalTime(2, 1), false}));
  }
  for (int index = 20; index < per_track && index < 20 + rounds * 2; index += 2) {
    // Join pairs on a track that has no transitions yet (track 3, away from the slips).
    commands::AddTransitionPayload join;
    join.id = "bench-tr-" + std::to_string(index);
    join.track_id = "t3";
    join.kind = "cross_dissolve";
    join.from_clip_id = EditProject::ClipId(3, index - 1);
    join.to_clip_id = EditProject::ClipId(3, index);
    join.timeline_start = RationalTime(index * 4 - 1, 4);
    join.duration = RationalTime(1, 2);
    add_transition.push_back(project.Run(commands::CommandType::AddTransition, join));
  }
  for (int index = 10; index < per_track; index += 10) {
    retime_transition.push_back(project.Run(
        commands::CommandType::SetTransitionTiming,
        commands::SetTransitionTimingPayload{"tr-" + std::to_string(index), RationalTime(index * 10 - 3, 10), RationalTime(3, 5)}));
  }
  for (int index = 0; index < rounds && project.store().CanUndo(); ++index) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = project.store().Undo("bench", "2026-10-06T01:00:00Z");
    (void)result;
    undo.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }

  ReportLatency("MoveClip", move);
  ReportLatency("TrimClip (slip)", slip);
  ReportLatency("SplitClip, animated", split);
  ReportLatency("SetClipSpeed", speed);
  ReportLatency("AddTransition", add_transition);
  ReportLatency("SetTransitionTiming", retime_transition);
  ReportLatency("Undo", undo);

  if (on_disk) {
    project.Close();
    std::filesystem::remove_all(package);
  }
}

// ---- decode and seek on real files ----------------------------------------------
//
// Reads the reference fixtures that the test build generates (see
// cmake/Fixtures.cmake), so it needs CUTLINE_FIXTURE_DIR or a prior test
// configure. They are small (64x48 lossless, 160x90 and 320x180 H.264), so these
// figures describe the demux, seek and resample machinery rather than the cost
// of decoding production-resolution video, which they say nothing about.

[[nodiscard]] std::optional<std::string> BenchFixture(const std::string& name) {
  const char* configured = std::getenv("CUTLINE_FIXTURE_DIR");
  const std::filesystem::path directory = configured != nullptr ? std::filesystem::path(configured)
                                                                : std::filesystem::path("build/native/fixtures");
  const auto path = directory / name;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
  return path.string();
}

[[nodiscard]] double SinceMilliseconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void MeasureDecode() {
  namespace media = cutline::media;
  const auto open = [](const std::string& name) -> std::unique_ptr<media::Source> {
    const auto path = BenchFixture(name);
    return path.has_value() ? media::SourceRegistry::Instance().Open(*path) : nullptr;
  };

  std::cout << "\n  audio, 1024-frame blocks read in order (a playback-sized request)\n";
  for (const auto& [file, label, rate, channels] :
       std::vector<std::tuple<std::string, std::string, int, int>>{{"av-ref.mkv", "PCM, 48 kHz stereo", 48000, 2},
                                                                   {"chirp-aac.m4a", "AAC, 48 kHz mono", 48000, 1},
                                                                   {"chirp-44k.wav", "PCM 44.1 -> 48 kHz mono", 48000, 1}}) {
    auto source = open(file);
    if (source == nullptr) {
      std::cout << "  " << label << ": fixture not found\n";
      continue;
    }
    std::vector<double> block;
    for (std::int64_t start = 0; start + 1024 <= 190000; start += 1024) {
      const auto begin = std::chrono::steady_clock::now();
      const auto buffer = source->ReadAudio(RationalTime(start, rate), rate, channels, 1024);
      block.push_back(SinceMilliseconds(begin));
      if (!buffer.has_value()) throw std::runtime_error("audio read returned nothing");
    }
    ReportLatency(label, block);
  }

  std::cout << "\n  audio, a block at a random place (each read seeks)\n";
  for (const auto& [file, label, rate, channels] :
       std::vector<std::tuple<std::string, std::string, int, int>>{{"av-ref.mkv", "PCM in Matroska, stereo", 48000, 2},
                                                                   {"chirp-aac.m4a", "AAC in MP4, mono", 48000, 1},
                                                                   {"chirp-44k.mkv", "PCM 44.1 -> 48 kHz, Matroska", 48000, 1},
                                                                   {"chirp-44k.wav", "PCM 44.1 -> 48 kHz, WAV", 48000, 1}}) {
    auto source = open(file);
    if (source == nullptr) {
      std::cout << "  " << label << ": fixture not found\n";
      continue;
    }
    // The first read pays for the one-off packet index on a coarse time base;
    // report it on its own rather than folding it into a median.
    const auto first_begin = std::chrono::steady_clock::now();
    const auto first = source->ReadAudio(RationalTime(100000, rate), rate, channels, 1024);
    const auto first_cost = SinceMilliseconds(first_begin);
    if (!first.has_value()) throw std::runtime_error("audio read returned nothing");
    std::vector<double> seeks;
    std::uint64_t state = 12345;
    for (int index = 0; index < 300; ++index) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const auto start = static_cast<std::int64_t>((state >> 33) % 185000);
      const auto begin = std::chrono::steady_clock::now();
      const auto buffer = source->ReadAudio(RationalTime(start, rate), rate, channels, 1024);
      seeks.push_back(SinceMilliseconds(begin));
      if (!buffer.has_value()) throw std::runtime_error("audio read returned nothing");
    }
    ReportLatency(label, seeks);
    std::cout << "      (first read, which includes any one-off index: " << std::fixed << std::setprecision(3)
              << first_cost << " ms)\n";
  }

  std::cout << "\n  video, frames read in order, and at random places (each random read seeks)\n";
  for (const auto& [file, label] : std::vector<std::pair<std::string, std::string>>{
           {"bars-2997.mp4", "H.264 320x180, GOP 30"}, {"av-ref.mkv", "FFV1 64x48, intra-only"}}) {
    auto source = open(file);
    if (source == nullptr) {
      std::cout << "  " << label << ": fixture not found\n";
      continue;
    }
    const auto* video = source->probe().PrimaryVideo();
    const auto rate = video->frame_rate;
    std::vector<double> in_order;
    for (int frame = 0; frame < 90; ++frame) {
      const auto begin = std::chrono::steady_clock::now();
      const auto decoded = source->ReadVideo(RationalTime::FromFrames(frame, rate));
      in_order.push_back(SinceMilliseconds(begin));
      if (!decoded.has_value()) throw std::runtime_error("video read returned nothing");
    }
    ReportLatency(label + ", in order", in_order);
    std::vector<double> random;
    std::uint64_t state = 99;
    for (int index = 0; index < 200; ++index) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const auto frame = static_cast<std::int64_t>((state >> 33) % 100);
      const auto begin = std::chrono::steady_clock::now();
      const auto decoded = source->ReadVideo(RationalTime::FromFrames(frame, rate));
      random.push_back(SinceMilliseconds(begin));
      if (!decoded.has_value()) throw std::runtime_error("video read returned nothing");
    }
    ReportLatency(label + ", random", random);
  }

  std::cout << "\n  audio and video from one file, alternating (what playback does)\n";
  if (auto source = open("av-aac.mp4"); source != nullptr) {
    std::vector<double> step;
    std::int64_t position = 0;
    for (int index = 0; index < 150; ++index) {
      const auto begin = std::chrono::steady_clock::now();
      const auto audio = source->ReadAudio(RationalTime(position, 48000), 48000, 1, 1920);
      const auto frame = source->ReadVideo(RationalTime(position, 48000));
      step.push_back(SinceMilliseconds(begin));
      if (!audio.has_value() || !frame.has_value()) throw std::runtime_error("read returned nothing");
      position += 1920;
      if (position + 1920 > 190000) position = 0;
    }
    ReportLatency("H.264 + AAC, 40 ms of each per step", step);
  }

  // A long file, supplied by the caller, to show how the one-off packet index
  // scales: CUTLINE_BENCH_LONG_AUDIO=path\to\file.
  if (const char* long_path = std::getenv("CUTLINE_BENCH_LONG_AUDIO"); long_path != nullptr) {
    auto source = media::SourceRegistry::Instance().Open(long_path);
    const auto duration = source->probe().duration;
    const auto rate = 48000;
    const auto total_samples = duration.Rescale(rate, cutline::time::RoundingMode::Floor);
    std::cout << "\n  long audio file: " << std::fixed << std::setprecision(0) << static_cast<double>(duration.numerator()) / duration.denominator()
              << " s\n";
    const auto first_begin = std::chrono::steady_clock::now();
    const auto first = source->ReadAudio(RationalTime(total_samples / 2, rate), rate, 1, 1024);
    const auto first_cost = SinceMilliseconds(first_begin);
    if (!first.has_value()) throw std::runtime_error("audio read returned nothing");
    std::vector<double> seeks;
    std::uint64_t state = 4242;
    for (int index = 0; index < 300; ++index) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const auto start = static_cast<std::int64_t>((state >> 33) % static_cast<std::uint64_t>(total_samples - 2048));
      const auto begin = std::chrono::steady_clock::now();
      const auto buffer = source->ReadAudio(RationalTime(start, rate), rate, 1, 1024);
      seeks.push_back(SinceMilliseconds(begin));
      if (!buffer.has_value()) throw std::runtime_error("audio read returned nothing");
    }
    ReportLatency("random seek", seeks);
    std::cout << "      (first read, including the packet index: " << std::fixed << std::setprecision(1) << first_cost << " ms)\n";
  }
}

void MeasureReadAhead() {
  const auto path = BenchFixture("bars-2997.mp4");
  if (!path.has_value()) {
    std::cout << "  H.264 fixture not found\n";
    return;
  }
  auto probe_source = cutline::media::SourceRegistry::Instance().Open(*path);
  if (probe_source == nullptr || probe_source->probe().PrimaryVideo() == nullptr) {
    std::cout << "  H.264 fixture could not be opened\n";
    return;
  }
  const auto rate = probe_source->probe().PrimaryVideo()->frame_rate;
  const auto duration = probe_source->probe().duration;
  probe_source.reset();

  Sequence sequence;
  sequence.id = "scrub";
  sequence.name = "Scrub latency";
  sequence.frame_rate = rate;
  sequence.width = 320;
  sequence.height = 180;
  sequence.sample_rate = 48000;
  Track track;
  track.id = "v1";
  track.kind = model::TrackKind::Video;
  Clip clip;
  clip.id = "clip";
  clip.source_kind = model::SourceKind::Media;
  clip.source_id = "media";
  clip.source_in = RationalTime{};
  clip.source_out = duration;
  clip.timeline_start = RationalTime{};
  clip.start_ticks = 0;
  clip.end_ticks = clip.end().ToTicks();
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  cutline::timeline::SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));

  cutline::playback::EngineConfig config;
  config.compositor.output_format = PixelFormat::Rgba8;
  config.decode_workers = 1;
  config.read_ahead_frames = 1;
  config.max_pending_decodes = 1;
  cutline::playback::PlaybackEngine engine(std::move(graph), [path](const std::string&) { return *path; }, config);

  constexpr int kSamples = 80;
  std::vector<RationalTime> positions;
  std::vector<double> ready;
  positions.reserve(kSamples);
  ready.reserve(kSamples);
  for (int index = 0; index < kSamples; ++index) {
    // 37 is coprime to the fixture's 119 usable frames, giving unique, nonlocal
    // seeks instead of accidentally measuring sequential decode.
    const auto frame = static_cast<std::int64_t>((index * 37) % 119);
    const auto at = RationalTime::FromFrames(frame, rate);
    positions.push_back(at);
    const auto completed = engine.statistics().read_ahead_completed;
    const auto begin = std::chrono::steady_clock::now();
    engine.Seek(at);
    while (engine.statistics().read_ahead_completed == completed && SinceMilliseconds(begin) < 2000.0) {
      // Windows commonly rounds a sub-millisecond sleep up to a 15.6 ms timer
      // tick, which would measure the polling method instead of decoder latency.
      std::this_thread::yield();
    }
    if (engine.statistics().read_ahead_completed == completed) {
      throw std::runtime_error("read-ahead did not complete within two seconds");
    }
    ready.push_back(SinceMilliseconds(begin));
  }

  std::vector<double> cached_render;
  cached_render.reserve(kSamples);
  for (const auto& at : positions) {
    const auto begin = std::chrono::steady_clock::now();
    const auto frame = engine.RenderFrame(at);
    cached_render.push_back(SinceMilliseconds(begin));
    if (!frame.valid()) throw std::runtime_error("cached playback render was invalid");
  }
  ReportLatency("seek request -> raw frame cached", ready);
  ReportLatency("cached raw frame -> composed frame", cached_render);
  const auto statistics = engine.statistics();
  std::cout << "      queued " << statistics.read_ahead_queued << ", completed " << statistics.read_ahead_completed
            << ", consumed from read-ahead " << statistics.read_ahead_cache_hits << "\n";
}

// Linked picture-and-sound edits: tracks 2 and 3 of a fresh project have their clips
// linked in pairs, so every edit below also edits the partner, in one command.
void MeasureLinkedEdits(int per_track) {
  namespace commands = cutline::commands;
  commands::CommandEnvelope create;
  create.command_id = "cmd-create";
  create.project_id = "bench-linked";
  create.author_id = "bench";
  create.timestamp_utc = "2026-10-06T00:00:00Z";
  create.type = commands::CommandType::CreateProject;
  create.payload = commands::CreateProjectPayload{"Linked"};
  create.idempotency_key = "key-create";
  auto store = std::make_unique<cutline::project::ProjectStore>(":memory:");
  store->Initialize();
  const auto created = store->Execute(create);
  (void)created;

  EditProject project(std::move(store), "bench-linked", 4, per_track);
  project.Build();
  for (int index = 0; index < per_track; ++index) {
    project.Run(commands::CommandType::LinkClips,
                commands::LinkClipsPayload{{EditProject::ClipId(2, index), EditProject::ClipId(3, index)},
                                           "pair-" + std::to_string(index)});
  }
  const int span = std::min(per_track / 4, 60);
  std::vector<double> move, trim, split, speed;
  for (int index = 0; index < span; ++index) {
    move.push_back(project.Run(commands::CommandType::MoveClip,
                               commands::MoveClipPayload{EditProject::ClipId(2, index), "t2", Seconds(8000 + index)}));
  }
  for (int index = span; index < 2 * span; ++index) {
    // The last tenth of a second off the tail.
    trim.push_back(project.Run(commands::CommandType::TrimClip,
                               commands::TrimClipPayload{EditProject::ClipId(2, index), Seconds(index),
                                                         RationalTime(index * 10 + 9, 10), Seconds(index)}));
  }
  for (int index = 2 * span; index < 3 * span; ++index) {
    split.push_back(project.Run(commands::CommandType::SplitClip,
                                commands::SplitClipPayload{EditProject::ClipId(2, index), "lsplit-" + std::to_string(index),
                                                           RationalTime(index * 2 + 1, 2)}));
  }
  for (int index = 3 * span; index < 4 * span; ++index) {
    speed.push_back(project.Run(commands::CommandType::SetClipSpeed,
                                commands::SetClipSpeedPayload{EditProject::ClipId(2, index), RationalTime(1, 1), true}));
  }
  std::cout << "\n  linked pairs, in-memory (each edit also edits the partner), " << project.clip_count() << " clips"
            << std::endl;
  ReportLatency("MoveClip, linked pair", move);
  ReportLatency("TrimClip tail, linked pair", trim);
  ReportLatency("SplitClip, linked pair", split);
  ReportLatency("SetClipSpeed (reverse), linked pair", speed);
}

}  // namespace

// What motion-compensated slow motion costs and what the analysis cache saves, on 1080p pictures with
// real texture. Two pictures, the second the first moved six pixels right.
void MeasureOpticalFlow() {
  using cutline::render::FlowCache;
  const auto make = [](int shift) {
    auto frame = cutline::media::VideoFrame::Allocate(PixelFormat::RgbaF32, 1920, 1080);
    for (int y = 0; y < 1080; ++y) {
      auto* row = frame.row_f32(y);
      for (int x = 0; x < 1920; ++x) {
        const int u = x - shift;
        const auto value = static_cast<float>((u * 17 + y * 31 + u * y * 3) % 101) / 100.0f;
        row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = value;
        row[x * 4 + 3] = 1.0f;
      }
    }
    return frame;
  };
  const auto first = make(0);
  const auto second = make(6);
  const cutline::render::OpticalFlowConfig config;
  std::cout << "\n== Optical flow, 1080p, default search (block 8, radius 12) ===\n";
  cutline::render::FlowField field;
  Report("estimate (forward + backward check)", TimeMilliseconds(3, [&](int) { field = cutline::render::EstimateOpticalFlow(first, second, config); }));
  Report("synthesise one in-between picture", TimeMilliseconds(3, [&](int) { (void)cutline::render::InterpolateOpticalFlow(first, second, 0.5f, field); }));
  Report("cache key for the pair", TimeMilliseconds(20, [&](int) { (void)cutline::render::MakeFlowKey(first, second, config); }));
  FlowCache cache;
  (void)cutline::render::EstimateOpticalFlowCached(cache, first, second, config);
  Report("pair already analysed: key + lookup", TimeMilliseconds(20, [&](int) { (void)cutline::render::EstimateOpticalFlowCached(cache, first, second, config); }));
}

// The GPU compositor against the software one on the same plans, at 1080p with 8-bit sources the way a decoder hands
// them over (so the upload is counted), and, when an ffmpeg binary is at hand, hardware decode against software decode.
void MeasureGpu(int frames) {
  using cutline::render::gpu::D3D11Compositor;
  D3D11Compositor::Options options;
  std::string why;
  auto gpu = D3D11Compositor::Create(options, &why);
  if (gpu == nullptr) {
    std::cout << "\nNo hardware Direct3D 11 adapter (" << why << "): nothing to measure.\n";
    return;
  }
  std::cout << "\n== GPU compositor, 1080p, 8-bit sources, adapter: " << gpu->device().name << " ===\n";
  cutline::media::SyntheticSpec spec;
  spec.pattern = cutline::media::SyntheticPattern::Bars;
  spec.width = kWidth;
  spec.height = kHeight;
  spec.duration = Seconds(60);
  auto source = cutline::media::OpenSynthetic(spec);
  auto decoded = source->ReadVideo(Seconds(1));
  if (!decoded.has_value()) throw std::runtime_error("synthetic source produced no frame");
  const auto picture = decoded->Clone();
  const TimelineCompiler compiler;
  CompositorConfig config;
  config.output_format = PixelFormat::Rgba8;
  const Compositor software(config);
  const auto resolver = [&picture](const SourceRequest&) { return &picture; };
  for (const int tracks : {1, 2, 4}) {
    const auto sequence = BuildSequence(tracks, 2);
    cutline::render::Statistics stats;
    cutline::render::gpu::GpuStatistics device;
    {
      const auto plan = compiler.Compile(sequence, RationalTime(0, 25));
      std::string reason;
      if (!gpu->Supports(plan, config, &reason)) {
        std::cout << "  " << tracks << " track(s): not renderable on the GPU (" << reason << ")\n";
        continue;
      }
      (void)gpu->Compose(plan, config, resolver, stats, &device);  // warm
    }
    const auto cpu_ms = TimeMilliseconds(frames, [&](int index) { (void)software.Compose(compiler.Compile(sequence, RationalTime(index % 1000, 25)), resolver); });
    double upload = 0, on_card = 0, readback = 0;
    const auto gpu_ms = TimeMilliseconds(frames, [&](int index) {
      (void)gpu->Compose(compiler.Compile(sequence, RationalTime(index % 1000, 25)), config, resolver, stats, &device);
      upload += device.upload_ms;
      on_card += device.gpu_ms;
      readback += device.readback_ms;
    });
    std::cout << "  " << tracks << " track(s), 3 effects: software " << std::fixed << std::setprecision(1) << cpu_ms << " ms, GPU " << gpu_ms << " ms (upload "
              << upload / frames << ", on the card " << std::setprecision(2) << on_card / frames << ", read back " << std::setprecision(1) << readback / frames << ") -> "
              << cpu_ms / gpu_ms << "x\n";
  }

  // Hardware decode against software decode, on a full-HD H.264 file the bundled ffmpeg makes.
  std::filesystem::path binary;
  for (const auto* candidate : {".tools/ffmpeg/bin/ffmpeg.exe", "../.tools/ffmpeg/bin/ffmpeg.exe", "../../.tools/ffmpeg/bin/ffmpeg.exe"}) {
    if (std::filesystem::exists(candidate)) binary = candidate;
  }
  if (binary.empty()) {
    std::cout << "  (no ffmpeg binary found: decode comparison skipped)\n";
    return;
  }
  const auto file = std::filesystem::temp_directory_path() / "cutline-bench-1080p.mp4";
  if (!std::filesystem::exists(file)) {
    const auto command = "\"" + binary.string() + "\" -v error -y -f lavfi -i testsrc2=s=1920x1080:r=25:d=8 -c:v libopenh264 -b:v 12M -g 25 -pix_fmt yuv420p \"" + file.string() + "\"";
    if (std::system(("\"" + command + "\"").c_str()) != 0) {
      std::cout << "  (could not make a 1080p file: decode comparison skipped)\n";
      return;
    }
  }
  auto software_source = cutline::media::SourceRegistry::Instance().Open(file.string());
  cutline::media::OpenOptions open;
  open.d3d11_device = gpu->native_device();
  auto hardware_source = cutline::media::SourceRegistry::Instance().Open(file.string(), open);
  if (software_source == nullptr || hardware_source == nullptr || !hardware_source->ReadDeviceVideo(RationalTime(0, 1)).has_value()) {
    std::cout << "  (this adapter cannot decode H.264 in hardware: decode comparison skipped)\n";
    return;
  }
  const auto sequence = BuildSequence(1, 2);
  const int count = std::min(frames, 150);
  cutline::render::Statistics stats;
  cutline::render::gpu::GpuStatistics device;
  const auto time_at = [](int index) { return RationalTime(index, 25); };
  const auto software_ms = TimeMilliseconds(count, [&](int index) {
    auto picture_now = software_source->ReadVideo(time_at(index));
    const auto plan = compiler.Compile(sequence, time_at(index));
    const cutline::render::FrameResolver resolve = [&](const SourceRequest&) { return &*picture_now; };
    (void)gpu->Compose(plan, config, resolve, stats, &device);
  });
  const auto hardware_ms = TimeMilliseconds(count, [&](int index) {
    const auto plan = compiler.Compile(sequence, time_at(index));
    const cutline::render::FrameResolver none = [](const SourceRequest&) { return nullptr; };
    const cutline::render::gpu::DeviceFrameResolver on_card = [&](const SourceRequest& request) {
      auto frame = hardware_source->ReadDeviceVideo(request.source_time);
      return frame ? *frame : cutline::render::gpu::DeviceFrame{};
    };
    (void)gpu->Compose(plan, config, none, stats, &device, on_card);
  });
  const auto decode_only_ms = TimeMilliseconds(count, [&](int index) { (void)software_source->ReadVideo(time_at(index)); });
  std::cout << "  1080p H.264 read + one GPU layer: software decode + upload " << std::fixed << std::setprecision(2) << software_ms << " ms (decode alone " << decode_only_ms
            << "), hardware decode, no copy " << hardware_ms << " ms -> " << software_ms / hardware_ms << "x\n";
}

// A narrow window near the tail exposes range-query work unrelated to the window.
void MeasureTimelineRange() {
  const TimelineCompiler compiler;
  std::cout << "\n== Timeline range queries, one-second window near the tail ===\n";
  for (const std::int64_t clips : {10, 1000, 10000, 50000}) {
    cutline::timeline::SequenceGraph graph;
    graph.sequences.push_back(BuildLongSequence(clips));
    const RationalTime from(clips * 2 - 3, 2);
    const auto until = from.Add(Seconds(1));
    Report("BoundariesIn, " + std::to_string(clips) + " clips", TimeMilliseconds(1000, [&](int) {
      const auto boundaries = compiler.BoundariesIn(graph, from, until);
      if (boundaries.size() != 2) throw std::runtime_error("expected two boundaries");
    }));
    Report("CompileRange, " + std::to_string(clips) + " clips", TimeMilliseconds(1000, [&](int) {
      const auto requests = compiler.CompileRange(graph, from, until);
      if (requests.size() != 2) throw std::runtime_error("expected two clips in the window");
    }));
  }
}

// One clip with one effect on it, for timing what a single colour tool costs.
Sequence SingleEffectSequence(Effect effect) {
  auto sequence = BuildSequence(1, 0);
  auto& clip = sequence.tracks.front().clips.front();
  clip.effects.clear();
  effect.order = 1;
  if (!effect.effect_type.empty()) clip.effects.push_back(std::move(effect));
  return sequence;
}

std::filesystem::path WriteBenchLut(int size) {
  const auto path = std::filesystem::temp_directory_path() / ("cutline-bench-lut-" + std::to_string(size) + ".cube");
  if (std::filesystem::exists(path)) return path;
  std::ofstream out(path);
  out << "LUT_3D_SIZE " << size << "\n";
  for (int b = 0; b < size; ++b) {
    for (int g = 0; g < size; ++g) {
      for (int r = 0; r < size; ++r) {
        const double x = r / double(size - 1), y = g / double(size - 1), z = b / double(size - 1);
        // A gentle S-curve and a cool shift: not the identity, so nothing can skip the work.
        const auto curve = [](double v) { return v * v * (3.0 - 2.0 * v) * 0.6 + v * 0.4; };
        out << curve(x) * 0.97 << " " << curve(y) << " " << std::min(1.0, curve(z) * 1.03 + 0.01) << "\n";
      }
    }
  }
  return path;
}

// What each colour tool costs on a 1080p frame: in software, and on the card where the card can do it.
void MeasureColor(int frames) {
  using cutline::render::gpu::D3D11Compositor;
  std::cout << "\n== Colour tools, 1080p, one effect on one clip, float source ===\n";
  cutline::media::SyntheticSpec spec;
  spec.pattern = cutline::media::SyntheticPattern::Bars;
  spec.width = kWidth;
  spec.height = kHeight;
  spec.duration = Seconds(60);
  auto source = cutline::media::OpenSynthetic(spec);
  auto decoded = source->ReadVideo(Seconds(1));
  if (!decoded.has_value()) throw std::runtime_error("synthetic source produced no frame");
  const auto picture = cutline::media::ConvertFrame(*decoded, PixelFormat::RgbaF32);
  const auto resolver = [&picture](const SourceRequest&) { return &picture; };
  CompositorConfig config;
  config.output_format = PixelFormat::Rgba8;
  const Compositor software(config);
  const TimelineCompiler compiler;
  std::string why;
  auto gpu = D3D11Compositor::Create({}, &why);

  struct Case {
    std::string label;
    Effect effect;
  };
  std::vector<Case> cases;
  cases.push_back({"(no effect: the floor)", MakeEffect("e", "", {}, 1)});
  cases.push_back({"basic color (grade)", MakeEffect("e", "grade", {{"exposure", Value::Scalar(0.2)}, {"contrast", Value::Scalar(115.0)}, {"saturation", Value::Scalar(110.0)}, {"temperature", Value::Scalar(8.0)}}, 1)});
  for (const int size : {17, 33, 65}) {
    auto effect = MakeEffect("e", "lut", {{"intensity", Value::Scalar(1.0)}}, 1);
    effect.preset_name = WriteBenchLut(size).string();
    cases.push_back({"LUT " + std::to_string(size) + "^3", std::move(effect)});
  }
  cases.push_back({"color wheels", MakeEffect("e", "color_wheels", {{"lift", Value::Vec4(0.02, 0.0, -0.02, 0.0)}, {"gamma", Value::Vec4(1.05, 1.0, 0.95, 1.0)}, {"gain", Value::Vec4(1.0, 1.02, 1.05, 1.0)}, {"shadows", Value::Vec3(0.0, 0.0, 0.05)}, {"midtones", Value::Vec3(0.03, 0.0, 0.0)}, {"highlights", Value::Vec3(0.0, 0.0, -0.04)}}, 1)});
  cases.push_back({"curves", MakeEffect("e", "curves", {{"master", Value::Vec3(0.22, 0.52, 0.8)}, {"red", Value::Vec3(0.3, 0.5, 0.7)}, {"green", Value::Vec3(0.25, 0.5, 0.75)}, {"blue", Value::Vec3(0.2, 0.5, 0.78)}, {"luma", Value::Vec3(0.25, 0.5, 0.75)}}, 1)});
  cases.push_back({"hue curves", MakeEffect("e", "hue_curves", {{"hue_vs_hue_a", Value::Vec3(10.0, 0.0, -10.0)}, {"hue_vs_sat_a", Value::Vec3(0.1, 0.0, 0.2)}, {"hue_vs_luma_b", Value::Vec3(0.0, 0.1, 0.0)}}, 1)});
  cases.push_back({"color adjust", MakeEffect("e", "color_adjust", {{"temperature", Value::Scalar(10.0)}, {"tint", Value::Scalar(-5.0)}, {"vibrance", Value::Scalar(25.0)}, {"shadows", Value::Scalar(15.0)}, {"highlights", Value::Scalar(-15.0)}}, 1)});
  cases.push_back({"HSL secondary", MakeEffect("e", "hsl_secondary", {{"hue_center", Value::Scalar(30.0)}, {"hue_width", Value::Scalar(60.0)}, {"hue_softness", Value::Scalar(20.0)}, {"hue_shift", Value::Scalar(25.0)}}, 1)});
  cases.push_back({"channel mixer", MakeEffect("e", "channel_mixer", {{"red", Value::Vec3(0.9, 0.1, 0.0)}, {"green", Value::Vec3(0.0, 1.0, 0.0)}, {"blue", Value::Vec3(0.05, 0.0, 0.95)}}, 1)});
  cases.push_back({"tint", MakeEffect("e", "tint", {{"map_black", Value::Vec3(0.05, 0.0, 0.1)}, {"map_white", Value::Vec3(1.0, 0.95, 0.85)}, {"amount", Value::Scalar(1.0)}}, 1)});
  cases.push_back({"black and white", MakeEffect("e", "black_and_white", {{"amount", Value::Scalar(1.0)}}, 1)});

  for (auto& entry : cases) {
    const auto sequence = SingleEffectSequence(entry.effect);
    (void)software.Compose(compiler.Compile(sequence, RationalTime(0, 25)), resolver);   // warm: a LUT is read once
    const auto cpu_ms = TimeMilliseconds(frames, [&](int index) { (void)software.Compose(compiler.Compile(sequence, RationalTime(index % 1000, 25)), resolver); });
    std::cout << "  " << std::left << std::setw(22) << entry.label << std::right << "software " << std::setw(8) << std::fixed << std::setprecision(1) << cpu_ms << " ms";
    if (gpu != nullptr) {
      const auto plan = compiler.Compile(sequence, RationalTime(0, 25));
      std::string reason;
      if (!gpu->Supports(plan, config, &reason)) {
        std::cout << "   GPU: not on the card (" << reason << ")\n";
        continue;
      }
      cutline::render::Statistics stats;
      cutline::render::gpu::GpuStatistics device;
      (void)gpu->Compose(plan, config, resolver, stats, &device);
      double on_card = 0;
      const auto gpu_ms = TimeMilliseconds(frames, [&](int index) {
        (void)gpu->Compose(compiler.Compile(sequence, RationalTime(index % 1000, 25)), config, resolver, stats, &device);
        on_card += device.gpu_ms;
      });
      std::cout << "   GPU " << std::setw(6) << gpu_ms << " ms (on the card " << std::setprecision(2) << on_card / frames << ")";
    }
    std::cout << "\n";
  }
}

int main(int argc, char* argv[]) {
  try {
    cutline::media::RegisterAllProviders();
    if (argc > 1 && std::string(argv[1]) == "--range") {
      std::cout << "Cutline timeline range benchmark\n";
      MeasureTimelineRange();
      return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--gpu") {
      std::cout << "Cutline GPU benchmark\n";
      MeasureGpu(argc > 2 ? std::max(1, std::atoi(argv[2])) : 60);
      return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--color") {
      std::cout << "Cutline colour benchmark\n";
      MeasureColor(argc > 2 ? std::max(1, std::atoi(argv[2])) : 20);
      return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--playback") {
      std::cout << "Cutline playback latency benchmark\n";
      MeasureReadAhead();
      return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--flow") {
      std::cout << "Cutline optical-flow benchmark\n";
      MeasureOpticalFlow();
      return 0;
    }
    const int frames = argc > 1 ? std::max(1, std::atoi(argv[1])) : 60;

    std::cout << "Cutline render benchmark: " << kWidth << "x" << kHeight << ", " << frames << " frames per case\n";

    // A decoded source frame, reused so decode cost does not mask render cost.
    cutline::media::SyntheticSpec spec;
    spec.pattern = cutline::media::SyntheticPattern::Bars;
    spec.width = kWidth;
    spec.height = kHeight;
    spec.duration = Seconds(60);
    auto source = cutline::media::OpenSynthetic(spec);
    auto decoded = source->ReadVideo(Seconds(1));
    if (!decoded.has_value()) throw std::runtime_error("synthetic source produced no frame");
    const auto source_8bit = decoded->Clone();
    const auto source_float = cutline::media::ConvertFrame(source_8bit, PixelFormat::RgbaF32);

    const TimelineCompiler compiler;

    std::cout << "\n== Compile ====================================================\n";
    for (const int tracks : {1, 4, 16}) {
      const auto sequence = BuildSequence(tracks, 2);
      const auto milliseconds = TimeMilliseconds(frames * 20, [&](int index) {
        const auto plan = compiler.Compile(sequence, RationalTime(index % 1000, 25));
        if (plan.video.empty()) throw std::runtime_error("empty plan");
      });
      Report(std::to_string(tracks) + " video track(s)", milliseconds);
    }

    std::cout << "\n== Composite (8-bit sources) ==================================\n";
    for (const int tracks : {1, 2, 4}) {
      const auto sequence = BuildSequence(tracks, 2);
      CompositorConfig config;
      config.output_format = PixelFormat::Rgba8;
      const Compositor compositor(config);
      const auto resolver = [&source_8bit](const SourceRequest&) { return &source_8bit; };
      const auto milliseconds = TimeMilliseconds(frames, [&](int index) {
        const auto plan = compiler.Compile(sequence, RationalTime(index % 1000, 25));
        const auto output = compositor.Compose(plan, resolver);
        if (!output.valid()) throw std::runtime_error("invalid frame");
      });
      Report(std::to_string(tracks) + " track(s), 3 effects each", milliseconds);
    }

    std::cout << "\n== Composite (float sources, no conversion) ===================\n";
    for (const int tracks : {1, 2, 4}) {
      const auto sequence = BuildSequence(tracks, 2);
      CompositorConfig config;
      config.output_format = PixelFormat::Rgba8;
      const Compositor compositor(config);
      const auto resolver = [&source_float](const SourceRequest&) { return &source_float; };
      const auto milliseconds = TimeMilliseconds(frames, [&](int index) {
        const auto plan = compiler.Compile(sequence, RationalTime(index % 1000, 25));
        const auto output = compositor.Compose(plan, resolver);
        if (!output.valid()) throw std::runtime_error("invalid frame");
      });
      Report(std::to_string(tracks) + " track(s), 3 effects each", milliseconds);
    }

    std::cout << "\n== Effect stack depth (1 track) ===============================\n";
    for (const int effects : {0, 4, 16}) {
      const auto sequence = BuildSequence(1, effects);
      CompositorConfig config;
      config.output_format = PixelFormat::Rgba8;
      const Compositor compositor(config);
      const auto resolver = [&source_float](const SourceRequest&) { return &source_float; };
      const auto milliseconds = TimeMilliseconds(frames, [&](int index) {
        const auto plan = compiler.Compile(sequence, RationalTime(index % 1000, 25));
        return compositor.Compose(plan, resolver);
      });
      Report(std::to_string(effects + 1) + " effect(s)", milliseconds);
    }

    std::cout << "\n== Spatial effects (1 float track) ============================\n";
    const auto measure_effect = [&](const std::string& label, Effect effect) {
      auto sequence = BuildSequence(1, 0);
      sequence.tracks[0].clips[0].effects.push_back(std::move(effect));
      CompositorConfig config;
      config.output_format = PixelFormat::Rgba8;
      const Compositor compositor(config);
      const auto resolver = [&source_float](const SourceRequest&) { return &source_float; };
      const auto milliseconds = TimeMilliseconds(frames, [&](int index) {
        const auto plan = compiler.Compile(sequence, RationalTime(index % 1000, 25));
        const auto output = compositor.Compose(plan, resolver);
        if (!output.valid()) throw std::runtime_error("invalid spatial-effect frame");
      });
      Report(label, milliseconds);
    };
    measure_effect("blur, radius 12 px",
                   MakeEffect("bench:blur", "blur", {{"radius", Value::Scalar(12.0)}}, 0));
    measure_effect("sharpen, amount 1",
                   MakeEffect("bench:sharpen", "sharpen", {{"amount", Value::Scalar(1.0)}}, 0));
    measure_effect("vignette",
                   MakeEffect("bench:vignette", "vignette",
                              {{"amount", Value::Scalar(0.75)}, {"midpoint", Value::Scalar(0.4)},
                               {"feather", Value::Scalar(0.4)}},
                              0));
    measure_effect("lens correction",
                   MakeEffect("bench:lens", "lens_correction", {{"distortion", Value::Scalar(0.15)}}, 0));

    std::cout << "\n== Mix (1024-frame blocks, 48 kHz stereo) =====================\n";
    {
      using cutline::timeline::Clip;
      using cutline::timeline::Effect;
      using cutline::timeline::Parameter;
      using cutline::timeline::Sequence;
      using cutline::timeline::Track;

      // Resolver that returns silence of the right size: the mixer's own cost, with
      // no decode in it.
      const auto resolver = [](const std::string&, const RationalTime&, std::int64_t rate, int channels,
                               std::int64_t count) -> std::optional<cutline::media::AudioBuffer> {
        return cutline::media::AudioBuffer::Allocate(rate, channels, count);
      };

      const auto make_clip = [](const std::string& id, std::int64_t start_seconds) {
        Clip clip;
        clip.id = id;
        clip.source_id = "media-1";
        clip.source_in = RationalTime(0, 1);
        clip.source_out = RationalTime(1, 1);
        clip.timeline_start = RationalTime(start_seconds, 1);
        clip.start_ticks = clip.timeline_start.ToTicks();
        clip.end_ticks = clip.end().ToTicks();
        return clip;
      };
      // `tracks` audio tracks of `clips` one-second clips each; `variant` shapes
      // the clips: 0 plain, 1 retimed 3/2, 2 reversed and retimed, 3 gain keyframed.
      const auto build = [&](int tracks, int clips, int variant) {
        Sequence sequence;
        sequence.id = "seq";
        sequence.frame_rate = {25, 1};
        sequence.width = 64;
        sequence.height = 36;
        sequence.sample_rate = 48000;
        for (int t = 0; t < tracks; ++t) {
          Track track;
          track.id = "a" + std::to_string(t);
          track.kind = cutline::model::TrackKind::Audio;
          track.order = t;
          for (int c = 0; c < clips; ++c) {
            auto clip = make_clip(track.id + "-" + std::to_string(c), c);
            if (variant == 1 || variant == 2) {
              clip.playback_rate = RationalTime(3, 2);
              clip.source_out = RationalTime(3, 2);
              clip.start_ticks = clip.timeline_start.ToTicks();
              clip.end_ticks = clip.end().ToTicks();
            }
            clip.reversed = variant == 2;
            if (variant == 3) {
              Effect gain;
              gain.id = clip.id + "-fx";
              gain.effect_type = "gain";
              Parameter value;
              value.name = "value";
              cutline::anim::AnimatedValue curve;
              curve.SetKeyframe({RationalTime(0, 1), cutline::anim::Value::Scalar(0.0), cutline::anim::Interpolation::Linear, {}, {}});
              curve.SetKeyframe({RationalTime(1, 1), cutline::anim::Value::Scalar(1.0), cutline::anim::Interpolation::Linear, {}, {}});
              value.value = curve;
              gain.parameters.push_back(std::move(value));
              clip.effects.push_back(std::move(gain));
            }
            track.clips.push_back(std::move(clip));
          }
          if (variant == 4) {
            // Processors with memory: an equaliser, a compressor and a limiter in turn.
            for (const char* type : {"eq", "compressor", "limiter"}) {
              Effect effect;
              effect.id = track.id + "-" + type;
              effect.effect_type = type;
              track.effects.push_back(std::move(effect));
            }
          }
          sequence.tracks.push_back(std::move(track));
        }
        cutline::timeline::SequenceGraph graph;
        graph.sequences.push_back(std::move(sequence));
        return graph;
      };

      const cutline::audio::AudioMixer mixer(cutline::audio::MixerConfig{48000, 2, true});
      const auto measure = [&](const std::string& label, const cutline::timeline::SequenceGraph& graph, int seconds) {
        const auto milliseconds = TimeMilliseconds(frames * 10, [&](int index) {
          const auto mixed = mixer.MixSamples(graph, static_cast<std::int64_t>(index % (seconds * 40)) * 1200 + 17, 1024, resolver);
          if (mixed.frames() != 1024) throw std::runtime_error("short block");
        });
        Report(label, milliseconds);
      };
      measure("4 tracks, plain clips", build(4, 40, 0), 40);
      measure("4 tracks, retimed 3/2 (interpolated)", build(4, 40, 1), 40);
      measure("4 tracks, reversed and retimed", build(4, 40, 2), 40);
      measure("4 tracks, gain keyframed per sample", build(4, 40, 3), 40);
      measure("1 track of 10,000 clips", build(1, 10000, 0), 9000);
      {
        // Stateful effects: each block reaches back and ahead of itself, and the cell
        // cache is what keeps that from being paid on every block.
        const auto graph = build(4, 40, 4);
        const auto run = [&](const std::string& label, const cutline::audio::AudioMixer& m) {
          const auto milliseconds = TimeMilliseconds(frames * 10, [&](int index) {
            const auto mixed = m.MixSamples(graph, static_cast<std::int64_t>(index % (40 * 40)) * 1200 + 17, 1024, resolver);
            if (mixed.frames() != 1024) throw std::runtime_error("short block");
          });
          Report(label, milliseconds);
        };
        run("4 tracks, eq+compressor+limiter, no cache", mixer);
        cutline::audio::MixerConfig cached{48000, 2, true};
        cached.stretch_cache = std::make_shared<cutline::audio::StretchCache>();
        run("4 tracks, eq+compressor+limiter, cell cache", cutline::audio::AudioMixer(cached));
      }
    }

    std::cout << "\n== Compile scaling: clips in the sequence ======================\n";
    {
      for (const std::int64_t clips : {10, 1000, 10000, 50000}) {
        const auto sequence = BuildLongSequence(clips);
        cutline::timeline::SequenceGraph graph;
        graph.sequences.push_back(sequence);

        const auto in_place = TimeMilliseconds(frames * 20, [&](int index) {
          const auto plan = compiler.Compile(sequence, RationalTime(index % clips, 1));
          if (plan.video.size() != 1) throw std::runtime_error("expected one active clip");
        });
        const auto via_graph = TimeMilliseconds(frames * 20, [&](int index) {
          const auto plan = compiler.Compile(graph, RationalTime(index % clips, 1));
          if (plan.video.size() != 1) throw std::runtime_error("expected one active clip");
        });
        Report("Compile(sequence), " + std::to_string(clips) + " clips", in_place);
        Report("Compile(graph),    " + std::to_string(clips) + " clips", via_graph);
      }
    }

    std::cout << "\n== Load one sequence from a project with many ===============\n";
    {
      for (const int others : {0, 20, 80}) {
        auto store = BuildAnimatedProject(others);
        cutline::timeline::LoadStatistics last;
        const auto milliseconds = TimeMilliseconds(40, [&](int) {
          cutline::timeline::LoadStatistics statistics;
          const std::lock_guard<std::mutex> lock(store->mutex());
          const auto sequence = cutline::timeline::LoadSequence(store->connection(), "target", &statistics);
          if (sequence.tracks.empty()) throw std::runtime_error("empty load");
          last = statistics;
        });
        Report("load 'target' beside " + std::to_string(others) + " animated sequences", milliseconds);
        std::cout << "      read " << last.effects << " effects, " << last.parameters << " parameters, "
                  << last.keyframes << " keyframes\n";
      }
    }

    std::cout << "\n== Decode and seek (real files, no store) ==================\n";
    MeasureDecode();

    std::cout << "\n== Playback read-ahead (H.264 320x180, random seeks) =======\n";
    MeasureReadAhead();

    std::cout << "\n== Edit command latency (store only; excludes UI and rendering) ===\n";
    {
      MeasureEditLatency("in-memory database", false, 250);
      MeasureLinkedEdits(250);
      MeasureEditLatency("on-disk package (WAL + journal files)", true, 250);
      MeasureEditLatency("on-disk package, every commit forced to disk (synchronous=FULL)", true, 250, true);
    }

    std::cout << "\n== Time arithmetic ===========================================\n";
    {
      // The tick conversion sits in the compile and edit paths, so its cost
      // matters even though a single call is tiny.
      constexpr int kCalls = 2'000'000;
      std::int64_t sink = 0;
      const auto start = std::chrono::steady_clock::now();
      for (int index = 0; index < kCalls; ++index) {
        sink += RationalTime::FromFrames(100000 + index, {30000, 1001}).ToTicks();
      }
      const auto nanoseconds =
          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
      std::cout << "  FromFrames+ToTicks at 29.97, ~1 hour in      " << std::fixed << std::setprecision(1)
                << nanoseconds << " ns/call   (checksum " << (sink & 0xFFFF) << ")\n";
    }

    std::cout << "\nA 1920x1080 frame holds " << (kWidth * kHeight) << " pixels; the software compositor is the\n"
              << "reference implementation, not the interactive one. A GPU backend is what\n"
              << "makes these numbers a frame budget rather than a throughput figure.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cutline_bench failed: " << error.what() << "\n";
    return 1;
  }
}

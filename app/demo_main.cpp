// Cutline end-to-end walkthrough.
//
// Builds a real project package on disk, ingests media, edits through the
// command bus, then decodes, composites, and mixes actual frames and samples.
// It touches every layer that exists, so running it is the fastest way to see
// what does and does not work.
//
//   cutline_demo [output-directory]
//
// Rendered frames are written as PPM next to the project; scripts/ppm-to-png.js
// converts them for viewing.

#include "audio/AudioSink.h"
#include "exporter/ExportWorker.h"
#include "core/commands/Command.h"
#include "core/project/ProjectStore.h"
#include "media/Ingest.h"
#include "media/Providers.h"
#include "media/SyntheticSource.h"
#include "playback/PlaybackEngine.h"
#include "timeline/SequenceLoader.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace commands = cutline::commands;
namespace model = cutline::model;
namespace rates = cutline::time;
using cutline::anim::Interpolation;
using cutline::anim::Value;
using cutline::media::SyntheticPattern;
using cutline::media::SyntheticSpec;
using cutline::project::ProjectStore;
using cutline::time::RationalTime;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

void Heading(const std::string& text) {
  std::cout << "\n== " << text << " " << std::string(text.size() < 60 ? 60 - text.size() : 0, '=') << "\n";
}

// Drives the store the way the editor will: carries the project id, tracks the
// revision for optimistic concurrency, and issues unique command ids.
class Session final {
 public:
  Session(ProjectStore& store, std::string project_id) : store_(store), project_id_(std::move(project_id)) {}

  void Run(commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter_);
    command.project_id = project_id_;
    command.author_id = "demo";
    command.base_revision = store_.CurrentRevision();
    command.timestamp_utc = "2026-10-05T12:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter_);
    const auto result = store_.Execute(command);
    std::cout << "  r" << std::setw(3) << std::left << result.revision << " " << commands::Label(command) << "\n";
  }

 private:
  ProjectStore& store_;
  std::string project_id_;
  int counter_{0};
};

commands::InsertClipPayload MediaClip(std::string id, std::string track, std::string media_id, std::int64_t start,
                                      std::int64_t in, std::int64_t out, std::string linked_group) {
  commands::InsertClipPayload clip;
  clip.id = std::move(id);
  clip.track_id = std::move(track);
  clip.source_kind = model::SourceKind::Media;
  clip.media_id = std::move(media_id);
  clip.source_in = Seconds(in);
  clip.source_out = Seconds(out);
  clip.timeline_start = Seconds(start);
  clip.linked_group = std::move(linked_group);
  return clip;
}

void WritePpm(const std::filesystem::path& path, const cutline::media::VideoFrame& frame) {
  const auto rgba = frame.format() == cutline::media::PixelFormat::Rgba8
                        ? frame.Clone()
                        : cutline::media::ConvertFrame(frame, cutline::media::PixelFormat::Rgba8);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << "P6\n" << rgba.width() << " " << rgba.height() << "\n255\n";
  std::vector<char> row(static_cast<std::size_t>(rgba.width()) * 3);
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* pixels = rgba.row_u8(y);
    for (int x = 0; x < rgba.width(); ++x) {
      row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<char>(pixels[static_cast<std::size_t>(x) * 4 + 0]);
      row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<char>(pixels[static_cast<std::size_t>(x) * 4 + 1]);
      row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<char>(pixels[static_cast<std::size_t>(x) * 4 + 2]);
    }
    file.write(row.data(), static_cast<std::streamsize>(row.size()));
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    cutline::media::RegisterAllProviders();

    const std::filesystem::path output =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::temp_directory_path() / "cutline-demo.vproj";
    std::filesystem::remove_all(output);

    Heading("Environment");
    std::cout << "  file decoding   " << (cutline::media::HasFileDecoding() ? "FFmpeg" : "none (synthetic only)")
              << "\n"
              << "  audio device    " << (cutline::audio::HasAudioDevice() ? "available" : "offline only") << "\n"
              << "  providers       ";
    for (const auto& name : cutline::media::SourceRegistry::Instance().ProviderNames()) std::cout << name << " ";
    std::cout << "\n";

    Heading("Create project package");
    commands::CommandEnvelope create;
    create.command_id = "cmd-create";
    create.project_id = ProjectStore::GenerateProjectUuid();
    create.author_id = "demo";
    create.base_revision = 0;
    create.timestamp_utc = "2026-10-05T12:00:00Z";
    create.type = commands::CommandType::CreateProject;
    create.payload = commands::CreateProjectPayload{"Demo Cut"};
    create.idempotency_key = "key-create";

    auto store = ProjectStore::CreatePackage(output, create);
    std::cout << "  package   " << output.string() << "\n"
              << "  projectId " << store->ProjectId() << "\n"
              << "  journal   " << store->JournalMode() << "\n";

    Session session(*store, create.project_id);

    Heading("Ingest media");
    session.Run(commands::CommandType::CreateBin, commands::CreateBinPayload{"bin-1", std::nullopt, "Footage", 0});

    // Two synthetic sources: a counter so the rendered frames state which source
    // frame they came from, and colour bars for the B-roll.
    SyntheticSpec take_one;
    take_one.pattern = SyntheticPattern::Counter;
    take_one.width = 320;
    take_one.height = 180;
    take_one.frame_rate = {30, 1};
    take_one.duration = Seconds(30);
    take_one.tone_hz = 440.0;
    take_one.tone_amplitude = 0.4f;

    SyntheticSpec take_two = take_one;
    take_two.pattern = SyntheticPattern::Bars;
    take_two.tone_hz = 660.0;

    std::unordered_map<std::string, std::string> media_paths;
    for (const auto& [id, spec] : std::vector<std::pair<std::string, SyntheticSpec>>{{"media-1", take_one},
                                                                                     {"media-2", take_two}}) {
      cutline::media::IngestRequest request;
      request.project_id = create.project_id;
      request.author_id = "demo";
      request.timestamp_utc = "2026-10-05T12:00:00Z";
      request.media_id = id;
      request.bin_id = "bin-1";
      request.path = spec.ToPath();
      request.display_name = id == "media-1" ? "A001_C003 (counter)" : "A001_C004 (bars)";
      request.base_revision = store->CurrentRevision();
      const auto ingested = cutline::media::IngestFile(*store, request);
      media_paths.emplace(ingested.media_id, request.path);
      std::cout << "  r" << std::setw(3) << std::left << ingested.revision << " Import " << request.display_name
                << "  " << ingested.probe.streams.size() << " stream(s), fingerprint "
                << ingested.fingerprint.substr(0, 12) << "...\n";
    }

    Heading("Build a sequence");
    commands::CreateSequencePayload sequence;
    sequence.id = "seq-1";
    sequence.settings.name = "Main";
    sequence.settings.frame_rate = {30, 1};
    sequence.settings.width = 320;
    sequence.settings.height = 180;
    sequence.settings.sample_rate = 48000;
    sequence.settings.working_color_space = "rec709";
    session.Run(commands::CommandType::CreateSequence, sequence);

    session.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
    session.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
    session.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});

    // Linked A/V, as a camera clip arrives.
    session.Run(commands::CommandType::InsertClip, MediaClip("v-1", "v1", "media-1", 0, 0, 6, "take-1"));
    session.Run(commands::CommandType::InsertClip, MediaClip("a-1", "a1", "media-1", 0, 0, 6, "take-1"));
    session.Run(commands::CommandType::InsertClip, MediaClip("v-2", "v1", "media-2", 6, 10, 16, "take-2"));
    session.Run(commands::CommandType::InsertClip, MediaClip("a-2", "a1", "media-2", 6, 10, 16, "take-2"));

    // An upper-track inset with a keyframed opacity ramp and a transform.
    session.Run(commands::CommandType::InsertClip, MediaClip("v-inset", "v2", "media-2", 2, 20, 8 + 20, ""));

    commands::AddEffectPayload motion;
    motion.id = "fx-motion";
    motion.owner_kind = model::EffectOwner::Clip;
    motion.owner_id = "v-inset";
    motion.effect_type = "motion";
    motion.intrinsic = true;
    motion.order = 0;
    motion.parameters = {{"fx-motion:scale", "scale", Value::Vec2(40.0, 40.0)},
                         {"fx-motion:position", "position", Value::Vec2(80.0, -40.0)},
                         {"fx-motion:rotation", "rotation", Value::Scalar(0.0)}};
    session.Run(commands::CommandType::AddEffect, motion);

    commands::AddEffectPayload opacity;
    opacity.id = "fx-opacity";
    opacity.owner_kind = model::EffectOwner::Clip;
    opacity.owner_id = "v-inset";
    opacity.effect_type = "opacity";
    opacity.intrinsic = true;
    opacity.order = 1;
    opacity.parameters = {{"fx-opacity:value", "value", Value::Scalar(1.0)}};
    session.Run(commands::CommandType::AddEffect, opacity);

    for (const auto& [at, level] : std::vector<std::pair<std::int64_t, double>>{{0, 0.0}, {2, 1.0}}) {
      commands::SetKeyframePayload keyframe;
      keyframe.parameter_id = "fx-opacity:value";
      keyframe.keyframe = {Seconds(at), Value::Scalar(level), Interpolation::EaseInOut, {}, {}};
      session.Run(commands::CommandType::SetKeyframe, keyframe);
    }

    // A grade on the lower track, and a dissolve across the cut at 6s.
    commands::AddEffectPayload grade;
    grade.id = "fx-grade";
    grade.owner_kind = model::EffectOwner::Track;
    grade.owner_id = "v1";
    grade.effect_type = "lumetri";
    grade.parameters = {{"fx-grade:exposure", "exposure", Value::Scalar(-0.3)},
                        {"fx-grade:saturation", "saturation", Value::Scalar(85.0)}};
    session.Run(commands::CommandType::AddEffect, grade);

    commands::AddTransitionPayload dissolve;
    dissolve.id = "t-1";
    dissolve.track_id = "v1";
    dissolve.kind = "cross_dissolve";
    dissolve.from_clip_id = "v-1";
    dissolve.to_clip_id = "v-2";
    dissolve.timeline_start = Seconds(5);
    dissolve.duration = Seconds(2);
    session.Run(commands::CommandType::AddTransition, dissolve);

    Heading("Render frames");
    const auto graph = cutline::timeline::LoadSequenceGraph(*store, "seq-1");
    cutline::playback::EngineConfig engine_config;
    engine_config.compositor.output_format = cutline::media::PixelFormat::Rgba8;
    cutline::playback::PlaybackEngine engine(
        graph,
        [&media_paths](const std::string& media_id) {
          const auto found = media_paths.find(media_id);
          return found == media_paths.end() ? std::string{} : found->second;
        },
        engine_config);

    const auto frames_directory = output / "frames";
    std::filesystem::create_directories(frames_directory);
    for (const std::int64_t frame_number : {30, 90, 180, 210, 300}) {
      const auto at = RationalTime::FromFrames(frame_number, {30, 1});
      const auto picture = engine.RenderFrame(at);
      const auto name = "frame-" + std::to_string(frame_number) + ".ppm";
      WritePpm(frames_directory / name, picture);
      std::cout << "  " << at.FormatTimecode({30, 1}) << "  " << picture.width() << "x" << picture.height()
                << "  source frame " << cutline::media::ReadFrameCounter(picture) << "  -> " << name << "\n";
    }

    Heading("Mix audio");
    cutline::audio::OfflineSink sink({48000, 2, 4800});
    engine.Play(sink);
    const auto blocks = sink.RenderBlocks(40);
    std::cout << "  rendered    " << blocks << " blocks of 100 ms\n"
              << "  clock       " << sink.clock().Now().FormatTimecode({30, 1}) << "\n"
              << "  position    " << engine.position().FormatTimecode({30, 1}) << "\n"
              << "  underruns   " << sink.underruns() << "\n";
    engine.Pause();

    const auto block = engine.RenderAudio(Seconds(1), 4800);
    std::cout << "  peak at 1s  " << std::fixed << std::setprecision(4) << block.Peak(0) << " / "
              << block.Peak(1) << "\n";

    Heading("Export");
    if (!cutline::media::HasFileEncoding()) {
      std::cout << "  skipped: this build has no encoder\n";
    } else {
      const auto delivery = output / "export.mkv";
      cutline::exporter::ExportRequest request;
      request.output_path = delivery.string();
      // Lossless, so the exported file can be compared against the monitor
      // frame for frame -- which is exactly what the export tests do.
      request.video.codec = "ffv1";
      request.video.pixel_format = "bgr0";
      request.video.color_range = model::ColorRange::Full;
      request.audio.codec = "pcm_s16le";
      request.out = Seconds(8);

      std::int64_t last_decile = -1;
      const auto result = cutline::exporter::Export(
          engine, request, [&](const cutline::exporter::ExportProgress& progress) {
            const auto decile = progress.frames_total > 0 ? progress.frames_written * 10 / progress.frames_total : 0;
            if (decile != last_decile) {
              last_decile = decile;
              std::cout << "  " << std::setw(3) << (decile * 10) << "%  " << progress.frames_written << "/"
                        << progress.frames_total << " frames\n";
            }
            return true;
          });
      std::cout << "  wrote      " << result.video_frames << " frames, " << result.audio_frames
                << " audio samples\n"
                << "  duration   " << result.duration.FormatTimecode({30, 1}) << "\n"
                << "  file       " << delivery.string() << " ("
                << (std::filesystem::file_size(delivery) / 1024) << " KB)\n";

      // Read it back and confirm it holds the pictures the monitor rendered.
      auto decoded = cutline::media::SourceRegistry::Instance().Open(delivery.string());
      if (decoded != nullptr) {
        const auto at = RationalTime::FromFrames(60, {30, 1});
        const auto exported = decoded->ReadVideo(at);
        const auto monitored = engine.RenderFrame(at);
        std::cout << "  round trip source frame "
                  << (exported.has_value() ? cutline::media::ReadFrameCounter(*exported) : -1) << " from the file vs "
                  << cutline::media::ReadFrameCounter(monitored) << " from the monitor\n";
      }
    }

    Heading("Undo and redo");
    std::cout << "  history depth " << store->HistoryLabels().size() << ", " << store->HistoryBytes()
              << " bytes of changesets\n"
              << "  most recent   "
              << (store->HistoryLabels().empty() ? "(none)" : store->HistoryLabels().back()) << "\n";
    for (int step = 0; step < 3 && store->CanUndo(); ++step) {
      const auto result = store->Undo("demo", "2026-10-05T13:00:00Z");
      std::cout << "  r" << result.revision << " " << result.summary << "\n";
    }
    while (store->CanRedo()) {
      const auto result = store->Redo("demo", "2026-10-05T14:00:00Z");
      std::cout << "  r" << result.revision << " " << result.summary << "\n";
    }

    Heading("Verify");
    store->ValidateDatabase();
    const auto& statistics = engine.statistics();
    std::cout << "  integrity    ok\n"
              << "  revision     " << store->CurrentRevision() << "\n"
              << "  journal      " << store->JournalCount() << " entries\n"
              << "  frames       " << statistics.frames_rendered << " rendered, " << statistics.cache_hits
              << " cache hits, " << statistics.cache_misses << " misses\n"
              << "  audio        " << statistics.audio_blocks_mixed << " blocks mixed\n"
              << "  media        " << statistics.offline_media << " offline, " << statistics.decode_failures
              << " decode failures\n";

    std::cout << "\nProject written to " << output.string() << "\n"
              << "Frames in " << frames_directory.string()
              << " (node scripts/ppm-to-png.js <dir> 2 to view)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cutline_demo failed: " << error.what() << "\n";
    return 1;
  }
}

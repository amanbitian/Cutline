// Export: encode, mux, and the round trip back.
//
// The point of these is the round trip. Checking that an export produced a file
// of about the right length would pass with the picture upside down; decoding
// the result and comparing it against what the monitor rendered for the same
// timecode is what actually says the export is correct. A lossless codec makes
// that comparison nearly exact.

#include "exporter/ExportPresets.h"
#include "exporter/ExportQueue.h"
#include "exporter/ExportValidation.h"
#include "exporter/ExportWorker.h"
#include "media/Providers.h"
#include "media/Source.h"
#include "media/Writer.h"
#include "media/SyntheticSource.h"
#include "playback/PlaybackEngine.h"
#include "tests/native/TestHarness.h"
#include "timeline/TimelineCompiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <filesystem>
#include <vector>
#include <iterator>
#include <fstream>
#include <string>

namespace model = cutline::model;
using cutline::exporter::ExportRequest;
using cutline::media::PixelFormat;
using cutline::media::SyntheticPattern;
using cutline::media::SyntheticSpec;
using cutline::media::VideoFrame;
using cutline::playback::EngineConfig;
using cutline::playback::PlaybackEngine;
using cutline::time::RationalTime;
using cutline::timeline::Clip;
using cutline::timeline::Sequence;
using cutline::timeline::SequenceGraph;
using cutline::timeline::Track;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

SyntheticSpec CounterSpec() {
  SyntheticSpec spec;
  spec.pattern = SyntheticPattern::Counter;
  spec.width = 192;
  spec.height = 108;
  spec.frame_rate = {25, 1};
  spec.duration = Seconds(20);
  spec.tone_hz = 1000.0;
  spec.tone_amplitude = 0.4f;
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

Track MakeTrack(std::string id, model::TrackKind kind) {
  Track track;
  track.id = std::move(id);
  track.kind = kind;
  track.order = 0;
  return track;
}

// One video clip and one audio clip over the same counter source.
SequenceGraph BuildGraph(std::int64_t length = 4, int width = 192, int height = 108) {
  Sequence sequence;
  sequence.id = "seq-1";
  sequence.name = "Export";
  sequence.frame_rate = {25, 1};
  sequence.width = width;
  sequence.height = height;
  sequence.sample_rate = 48000;

  auto video = MakeTrack("v1", model::TrackKind::Video);
  video.clips.push_back(MakeClip("clip-v", "media-1", 0, 0, length));
  auto audio = MakeTrack("a1", model::TrackKind::Audio);
  audio.clips.push_back(MakeClip("clip-a", "media-1", 0, 0, length));
  sequence.tracks.push_back(std::move(video));
  sequence.tracks.push_back(std::move(audio));

  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  return graph;
}

cutline::playback::MediaLocator CounterLocator() {
  const auto path = CounterSpec().ToPath();
  return [path](const std::string&) { return path; };
}

EngineConfig Config() {
  EngineConfig config;
  config.compositor.output_format = PixelFormat::Rgba8;
  return config;
}

std::filesystem::path Scratch(const std::string& name) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-export";
  std::filesystem::create_directories(directory);
  return directory / name;
}

// A lossless video / lossless audio export, so the round trip can be compared
// nearly exactly rather than within a codec's error.
ExportRequest LosslessRequest(const std::filesystem::path& path) {
  ExportRequest request;
  request.output_path = path.string();
  request.video.codec = "ffv1";
  // Full-range RGB through the codec: no chroma subsampling and no range
  // compression, so what comes back is what went in. bgr0 rather than gbrp
  // because that is the 8-bit RGB layout ffv1 actually accepts.
  request.video.pixel_format = "bgr0";
  request.video.color_range = model::ColorRange::Full;
  request.audio.codec = "pcm_s16le";
  return request;
}

struct Rgb final {
  int r{};
  int g{};
  int b{};
};

Rgb SampleAt(const VideoFrame& frame, int x, int y) {
  const auto rgba = frame.format() == PixelFormat::Rgba8 ? frame.Clone()
                                                         : cutline::media::ConvertFrame(frame, PixelFormat::Rgba8);
  const auto* pixel = rgba.row_u8(y) + static_cast<std::size_t>(x) * 4;
  return {pixel[0], pixel[1], pixel[2]};
}


// Frequency of a (mono-compared) signal by counting zero crossings. Independent
// of the code under test, and tolerant of the resampler's small start-up delay.
double MeasureFrequency(const cutline::media::AudioBuffer& audio, int channel, std::int64_t from, std::int64_t count) {
  const auto* samples = audio.channel(channel);
  std::int64_t crossings = 0;
  for (std::int64_t index = from + 1; index < from + count; ++index) {
    if ((samples[index - 1] < 0.0f) != (samples[index] < 0.0f)) ++crossings;
  }
  const double seconds = static_cast<double>(count) / static_cast<double>(audio.sample_rate());
  return static_cast<double>(crossings) / 2.0 / seconds;
}

std::vector<char> ReadAll(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << bytes;
}

// Any sibling of `path` left behind by an export: a temporary that outlived it.
std::vector<std::string> Leftovers(const std::filesystem::path& path) {
  std::vector<std::string> found;
  const auto directory = path.parent_path();
  const auto stem = path.filename().string();
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (name != stem && name.rfind(stem, 0) == 0) found.push_back(name);
  }
  return found;
}

}  // namespace

CUTLINE_TEST(ExportIsRefusedWhenNothingCanWrite) {
  cutline::media::RegisterAllProviders();
  SKIP_INAPPLICABLE(!cutline::media::HasFileEncoding(), "this build can encode, so nothing refuses");
  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  CHECK_THROWS(cutline::exporter::Export(engine, LosslessRequest(Scratch("never.mkv"))));
}

CUTLINE_TEST(ExportSettingsAreResolvedFromTheSequence) {
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());

  ExportRequest request;
  request.output_path = "unused.mkv";
  const auto resolved = cutline::exporter::ResolveRequest(engine, request);
  // Anything the caller left unset comes from the sequence, so an export cannot
  // silently render at the wrong size or rate.
  CHECK_EQ(resolved.video.width, std::int64_t{192});
  CHECK_EQ(resolved.video.height, std::int64_t{108});
  CHECK_EQ(resolved.video.frame_rate.numerator, std::int64_t{25});
  CHECK_EQ(resolved.audio.sample_rate, std::int64_t{48000});
  CHECK_EQ(resolved.audio.channels, 2);
  // An unset out point means the whole sequence.
  CHECK_EQ(resolved.out.Compare(Seconds(4)), 0);
}

CUTLINE_TEST(ExportRangeIsValidated) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());

  auto request = LosslessRequest(Scratch("invalid.mkv"));
  request.in = Seconds(3);
  request.out = Seconds(1);
  CHECK_THROWS(cutline::exporter::Export(engine, request));

  auto empty = LosslessRequest(Scratch("invalid.mkv"));
  empty.include_video = false;
  empty.include_audio = false;
  CHECK_THROWS(cutline::exporter::Export(engine, empty));

  auto unnamed = LosslessRequest(Scratch("invalid.mkv"));
  unnamed.output_path.clear();
  CHECK_THROWS(cutline::exporter::Export(engine, unnamed));
}

CUTLINE_TEST(ExportWritesTheExpectedFrameCount) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("count.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  const auto result = cutline::exporter::Export(engine, LosslessRequest(path));

  // Four seconds at 25 fps.
  CHECK_EQ(result.video_frames, std::int64_t{100});
  CHECK(!result.cancelled);
  CHECK_EQ(result.duration.Compare(Seconds(4)), 0);
  CHECK(std::filesystem::exists(path));
  CHECK(std::filesystem::file_size(path) > 0);
  std::filesystem::remove(path);
}

CUTLINE_TEST(ExportedVideoDecodesBackToWhatTheMonitorRendered) {
  // The round trip. Every exported frame is compared against the frame the
  // monitor produces for the same timecode. A lossless RGB codec means the only
  // tolerance needed is for the encoder's own rounding.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("roundtrip.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  const auto result = cutline::exporter::Export(engine, LosslessRequest(path));
  CHECK_EQ(result.video_frames, std::int64_t{100});

  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(decoded != nullptr);

  const RationalTime frame_duration{1, 25};
  int compared = 0;
  for (const std::int64_t index : {0, 1, 17, 50, 99}) {
    const auto at = frame_duration.Multiply(index);
    const auto expected = engine.RenderFrame(at);
    const auto actual = decoded->ReadVideo(at.Add(frame_duration.Divide(2)));
    CHECK(actual.has_value());
    CHECK_EQ(actual->width(), expected.width());
    CHECK_EQ(actual->height(), expected.height());

    // The counter pattern states which source frame it came from, so this also
    // proves the export put the right picture at the right timecode rather than
    // merely a picture of the right shape.
    CHECK_EQ(cutline::media::ReadFrameCounter(*actual), cutline::media::ReadFrameCounter(expected));

    for (const auto& point : {std::pair{10, 40}, std::pair{95, 54}, std::pair{180, 100}}) {
      const auto want = SampleAt(expected, point.first, point.second);
      const auto got = SampleAt(*actual, point.first, point.second);
      CHECK(std::abs(want.r - got.r) <= 2);
      CHECK(std::abs(want.g - got.g) <= 2);
      CHECK(std::abs(want.b - got.b) <= 2);
    }
    ++compared;
  }
  CHECK_EQ(compared, 5);

  decoded.reset();
  std::filesystem::remove(path);
}

CUTLINE_TEST(ExportUsesOriginalsWhenTheMonitorPrefersAProxy) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  SyntheticSpec original = CounterSpec();
  original.pattern = SyntheticPattern::Solid;
  original.red = 1.0f;
  original.green = 0.0f;
  original.blue = 0.0f;
  original.tone_hz = 0.0;
  SyntheticSpec proxy = original;
  proxy.red = 0.0f;
  proxy.green = 1.0f;

  auto config = Config();
  config.prefer_proxies = true;
  config.proxy_locator = [path = proxy.ToPath()](const std::string&) { return path; };
  PlaybackEngine engine(BuildGraph(1), [path = original.ToPath()](const std::string&) { return path; }, config);
  const auto monitor = SampleAt(engine.RenderFrame(Seconds(0)), 96, 54);
  CHECK(monitor.g > monitor.r);

  const auto path = Scratch("proxy-monitor-original-export.mkv");
  std::filesystem::remove(path);
  auto request = LosslessRequest(path);
  request.include_audio = false;
  request.out = RationalTime(1, 25);
  const auto result = cutline::exporter::Export(engine, request);
  CHECK_EQ(result.video_frames, std::int64_t{1});
  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  const auto frame = decoded->ReadVideo(Seconds(0));
  CHECK(frame.has_value());
  const auto exported = SampleAt(*frame, 96, 54);
  CHECK(exported.r > exported.g);
  decoded.reset();
  std::filesystem::remove(path);
}

CUTLINE_TEST(EveryChannelOfExportedAudioKeepsItsOwnSamples) {
  // The writer queues audio until it has a whole encoder frame. Growing that
  // queue in place used to copy channel 0's new samples over channel 1's old
  // ones before they were moved, so the right channel carried left-channel data.
  // The earlier tests played the same tone on both channels, which cannot show
  // it: this uses a different level on every channel and on every block.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");

  for (const int channels : {2, 6}) {
    const auto path = Scratch("channels-" + std::to_string(channels) + ".mka");
    std::filesystem::remove(path);

    cutline::media::ExportSettings settings;
    settings.path = path.string();
    cutline::media::AudioEncoderSettings audio;
    audio.codec = "pcm_s16le";
    audio.sample_rate = 48000;
    audio.channels = channels;
    settings.audio = audio;

    // Block sizes deliberately do not divide the encoder's frame size, so the
    // queue carries a partial frame across every write.
    const std::int64_t block_sizes[] = {600, 600, 437, 1301};
    std::vector<std::vector<float>> expected(static_cast<std::size_t>(channels));
    {
      auto writer = cutline::media::WriterRegistry::Instance().Open(settings);
      int block_number = 0;
      for (const auto frames : block_sizes) {
        auto buffer = cutline::media::AudioBuffer::Allocate(48000, channels, frames);
        for (int channel = 0; channel < channels; ++channel) {
          // A distinct, exactly representable level per (channel, block).
          const float level = 0.05f * static_cast<float>(1 + channel) + 0.01f * static_cast<float>(block_number);
          for (std::int64_t frame = 0; frame < frames; ++frame) {
            buffer.channel(channel)[frame] = level;
            expected[static_cast<std::size_t>(channel)].push_back(level);
          }
        }
        writer->WriteAudio(buffer);
        ++block_number;
      }
      writer->Finish();
    }

    auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
    CHECK(decoded != nullptr);
    const std::int64_t total = 600 + 600 + 437 + 1301;
    const auto read = decoded->ReadAudio({0, 1}, 48000, channels, total);
    CHECK(read.has_value());
    for (int channel = 0; channel < channels; ++channel) {
      // Sample positions that sit inside each block, away from the joins.
      for (const std::int64_t at : {100, 900, 1400, 2200}) {
        const auto want = expected[static_cast<std::size_t>(channel)][static_cast<std::size_t>(at)];
        CHECK(std::abs(read->channel(channel)[at] - want) < 0.002f);
      }
    }
    decoded.reset();
    std::filesystem::remove(path);
  }
}

CUTLINE_TEST(ExportedAudioStaysLockedToVideo) {
  // At 48 kHz against 25 fps a frame is exactly 1920 samples, so the totals must
  // match to the sample. The accumulation that produces them is the same one a
  // fractional rate relies on, which is checked below.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("sync.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  const auto result = cutline::exporter::Export(engine, LosslessRequest(path));
  CHECK_EQ(result.video_frames, std::int64_t{100});
  CHECK_EQ(result.audio_frames, std::int64_t{100 * 1920});

  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(decoded != nullptr);
  const auto probe = decoded->probe();
  CHECK(probe.PrimaryVideo() != nullptr);
  CHECK(probe.PrimaryAudio() != nullptr);
  CHECK_EQ(probe.PrimaryAudio()->sample_rate, std::int64_t{48000});

  // The tone is still there and at the level the mixer produced.
  const auto block = decoded->ReadAudio(Seconds(1), 48000, 2, 2048);
  CHECK(block.has_value());
  CHECK(std::abs(block->Peak(0) - 0.4f) < 0.02f);

  decoded.reset();
  std::filesystem::remove(path);
}

CUTLINE_TEST(FractionalRatesDoNotDriftOverALongExport) {
  // 48 kHz against 30000/1001 is 1601.6 samples per frame. Rounding each frame
  // independently loses about a sample every three frames, which is a frame of
  // desync every few minutes. Deriving each position from absolute time does
  // not, and this measures that over a thousand frames.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("drift.mkv");
  std::filesystem::remove(path);

  auto graph = BuildGraph(40);
  graph.sequences[0].frame_rate = {30000, 1001};
  PlaybackEngine engine(std::move(graph), CounterLocator(), Config());

  auto request = LosslessRequest(path);
  request.out = RationalTime(1000 * 1001, 30000);  // exactly 1000 frames
  const auto result = cutline::exporter::Export(engine, request);
  CHECK_EQ(result.video_frames, std::int64_t{1000});

  // 1000 frames at 30000/1001 is 1001/30 seconds; at 48 kHz that is 1601600
  // samples exactly. Anything else is accumulated rounding error.
  CHECK_EQ(result.audio_frames, std::int64_t{1601600});
  std::filesystem::remove(path);
}

CUTLINE_TEST(ExportHonoursAnInAndOutPoint) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("range.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  auto request = LosslessRequest(path);
  request.in = Seconds(1);
  request.out = Seconds(3);
  const auto result = cutline::exporter::Export(engine, request);
  CHECK_EQ(result.video_frames, std::int64_t{50});

  // The first exported frame is the one the monitor shows at the in point, not
  // at zero.
  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(decoded != nullptr);
  const auto first = decoded->ReadVideo({1, 50});
  CHECK(first.has_value());
  CHECK_EQ(cutline::media::ReadFrameCounter(*first),
           cutline::media::ReadFrameCounter(engine.RenderFrame(Seconds(1))));

  decoded.reset();
  std::filesystem::remove(path);
}

CUTLINE_TEST(ExportIgnoresMonitoringStateByDefault) {
  // A muted track is usually muted to hear something else while cutting, not to
  // leave it out of the delivery.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");

  auto graph = BuildGraph();
  graph.sequences[0].tracks[0].muted = true;
  PlaybackEngine engine(std::move(graph), CounterLocator(), Config());

  // The monitor shows nothing: the only video track is muted.
  const auto monitored = engine.RenderFrame(Seconds(1));
  CHECK_EQ(SampleAt(monitored, 96, 54).r, 0);

  const auto path = Scratch("muted.mkv");
  std::filesystem::remove(path);
  const auto result = cutline::exporter::Export(engine, LosslessRequest(path));
  CHECK_EQ(result.video_frames, std::int64_t{100});

  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(decoded != nullptr);
  const auto frame = decoded->ReadVideo(Seconds(1));
  CHECK(frame.has_value());
  // The counter clip's mid grey, not black: the muted track was still rendered.
  CHECK(SampleAt(*frame, 96, 54).r > 20);

  decoded.reset();
  std::filesystem::remove(path);
}

CUTLINE_TEST(ExportReportsProgressAndCanBeCancelled) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("cancel.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  std::int64_t last_seen = 0;
  std::int64_t reported_total = 0;
  const auto result = cutline::exporter::Export(engine, LosslessRequest(path),
                                                [&](const cutline::exporter::ExportProgress& progress) {
                                                  last_seen = progress.frames_written;
                                                  reported_total = progress.frames_total;
                                                  return progress.frames_written < 10;
                                                });
  CHECK(result.cancelled);
  CHECK_EQ(last_seen, std::int64_t{10});
  CHECK_EQ(reported_total, std::int64_t{100});
  // A cancelled export leaves no file: a truncated one that still opens looks
  // like a finished render.
  CHECK(!std::filesystem::exists(path));
}

CUTLINE_TEST(AudioOnlyAndVideoOnlyExportsWork) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());

  const auto video_path = Scratch("video-only.mkv");
  std::filesystem::remove(video_path);
  auto video_only = LosslessRequest(video_path);
  video_only.include_audio = false;
  const auto video_result = cutline::exporter::Export(engine, video_only);
  CHECK_EQ(video_result.video_frames, std::int64_t{100});
  CHECK_EQ(video_result.audio_frames, std::int64_t{0});
  {
    auto decoded = cutline::media::SourceRegistry::Instance().Open(video_path.string());
    CHECK(decoded != nullptr);
    CHECK(decoded->probe().PrimaryAudio() == nullptr);
  }
  std::filesystem::remove(video_path);

  const auto audio_path = Scratch("audio-only.mka");
  std::filesystem::remove(audio_path);
  auto audio_only = LosslessRequest(audio_path);
  audio_only.include_video = false;
  const auto audio_result = cutline::exporter::Export(engine, audio_only);
  CHECK_EQ(audio_result.video_frames, std::int64_t{0});
  CHECK(audio_result.audio_frames > 0);
  {
    auto decoded = cutline::media::SourceRegistry::Instance().Open(audio_path.string());
    CHECK(decoded != nullptr);
    CHECK(decoded->probe().PrimaryVideo() == nullptr);
  }
  std::filesystem::remove(audio_path);
}

CUTLINE_TEST(ALossyExportStillCarriesTheRightPictures) {
  // The lossless path proves correctness; this proves the ordinary delivery
  // path works too, with a tolerance a lossy codec earns.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("delivery.mp4");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  auto request = LosslessRequest(path);
  request.video.codec = "libopenh264";
  request.video.pixel_format = "yuv420p";
  request.video.color_range = model::ColorRange::Limited;
  request.video.gop_size = 25;
  request.audio.codec = "aac";

  cutline::exporter::ExportResult result;
  try {
    result = cutline::exporter::Export(engine, request);
  } catch (const std::exception& error) {
    SKIP_UNLESS(false, std::string("lossy encoders unavailable: ") + error.what());
  }
  CHECK_EQ(result.video_frames, std::int64_t{100});

  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(decoded != nullptr);
  const auto frame = decoded->ReadVideo({41, 25});  // frame 41
  CHECK(frame.has_value());
  // The counter blocks are large and high contrast, so they survive 4:2:0 and
  // a lossy quantiser; this is why the pattern uses blocks rather than pixels.
  CHECK_EQ(cutline::media::ReadFrameCounter(*frame),
           cutline::media::ReadFrameCounter(engine.RenderFrame({41, 25})));

  decoded.reset();
  std::filesystem::remove(path);
}


// ------------------------------------------- audio format and delivery safety ----

CUTLINE_TEST(AudioAtAnotherRateIsResampledNotReinterpreted) {
  // The export worker used to size each block for the requested rate but render
  // it at the engine's, and the writer never checked. A 1 kHz tone exported at
  // 44.1 kHz therefore played back near 926 Hz: 48000 samples per second
  // reinterpreted as 44100.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("rate44100.mka");
  std::filesystem::remove(path);

  cutline::media::ExportSettings settings;
  settings.path = path.string();
  cutline::media::AudioEncoderSettings audio;
  audio.codec = "pcm_s16le";
  audio.sample_rate = 44100;
  audio.channels = 2;
  settings.audio = audio;

  {
    auto writer = cutline::media::WriterRegistry::Instance().Open(settings);
    // Two seconds of a 1 kHz tone, handed over at 48 kHz in awkward block sizes.
    std::int64_t position = 0;
    for (const std::int64_t frames : {4800, 1, 7777, 24000, 59422}) {
      auto block = cutline::media::AudioBuffer::Allocate(48000, 2, frames);
      for (std::int64_t frame = 0; frame < frames; ++frame) {
        const float sample = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * static_cast<float>(position + frame) / 48000.0f);
        block.channel(0)[frame] = sample;
        block.channel(1)[frame] = sample;
      }
      position += frames;
      writer->WriteAudio(block);
    }
    writer->Finish();
    // About two seconds at the *output* rate, give or take the resampler's delay.
    CHECK(std::abs(writer->audio_frames_written() - 2 * 44100) < 2000);
  }

  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(decoded != nullptr);
  CHECK_EQ(decoded->probe().PrimaryAudio()->sample_rate, std::int64_t{44100});
  const auto block = decoded->ReadAudio({1, 2}, 44100, 2, 22050);
  CHECK(block.has_value());
  const auto frequency = MeasureFrequency(*block, 0, 0, 22050);
  CHECK(std::abs(frequency - 1000.0) < 15.0);
  decoded.reset();
  std::filesystem::remove(path);
}

CUTLINE_TEST(ExportAtAnotherRateKeepsThePitchEndToEnd) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("export44100.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  auto request = LosslessRequest(path);
  request.audio.sample_rate = 44100;
  const auto result = cutline::exporter::Export(engine, request);
  CHECK_EQ(result.video_frames, std::int64_t{100});
  CHECK(std::abs(result.audio_frames - 4 * 44100) < 2000);

  auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(decoded != nullptr);
  const auto block = decoded->ReadAudio({1, 1}, 44100, 2, 22050);
  CHECK(block.has_value());
  CHECK(std::abs(MeasureFrequency(*block, 0, 0, 22050) - 1000.0) < 15.0);
  decoded.reset();
  std::filesystem::remove(path);
}

CUTLINE_TEST(AudioLayoutIsNegotiatedRatherThanRejected) {
  // A stereo mix delivered as mono or as 5.1 is an ordinary request. It used to
  // fail on the writer's channel-count check; the writer now remixes.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");

  for (const int channels : {1, 6}) {
    const auto path = Scratch("layout-" + std::to_string(channels) + ".mkv");
    std::filesystem::remove(path);
    PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
    auto request = LosslessRequest(path);
    request.audio.channels = channels;
    request.include_video = false;
    CHECK_NO_THROW(cutline::exporter::Export(engine, request));

    auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
    CHECK(decoded != nullptr);
    CHECK_EQ(decoded->probe().PrimaryAudio()->channel_count, std::int64_t{channels});
    const auto block = decoded->ReadAudio({1, 1}, 48000, channels, 4800);
    CHECK(block.has_value());
    // The tone survives the remix on at least the first channel.
    CHECK(block->Peak(0) > 0.1f);
    decoded.reset();
    std::filesystem::remove(path);
  }
}

CUTLINE_TEST(ExportAppliesTheSamePolicyToSoundAsToPicture) {
  // Picture was rendered with the export's mute/solo policy but sound with the
  // monitor's, so a muted audio track exported as silence even when the request
  // said to render every track.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");

  auto graph = BuildGraph();
  for (auto& track : graph.sequences[0].tracks) {
    if (track.kind == model::TrackKind::Audio) track.muted = true;
  }

  const auto peak_of = [&](bool honour) {
    const auto path = Scratch(honour ? "policy-honour.mkv" : "policy-ignore.mkv");
    std::filesystem::remove(path);
    PlaybackEngine engine(graph, CounterLocator(), Config());
    auto request = LosslessRequest(path);
    request.include_video = false;
    request.honour_mute_and_solo = honour;
    const auto result = cutline::exporter::Export(engine, request);
    (void)result;
    auto decoded = cutline::media::SourceRegistry::Instance().Open(path.string());
    const auto block = decoded->ReadAudio({1, 1}, 48000, 2, 4800);
    const auto peak = block.has_value() ? block->Peak(0) : -1.0f;
    decoded.reset();
    std::filesystem::remove(path);
    return peak;
  };

  CHECK(peak_of(false) > 0.3f);   // every track rendered, muted or not
  CHECK(peak_of(true) < 0.001f);  // the monitor's policy, chosen deliberately
}

CUTLINE_TEST(AnExistingDeliveryIsNeverTruncatedByACancelledExport) {
  // The writer opened the destination directly, and cancelling deleted that
  // path -- so cancelling a re-export destroyed the previous delivery.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("keep-me.mkv");
  std::filesystem::remove(path);
  WriteBytes(path, "the previous delivery");

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  auto request = LosslessRequest(path);
  request.overwrite = true;
  const auto result = cutline::exporter::Export(engine, request,
                                                [](const cutline::exporter::ExportProgress& progress) {
                                                  return progress.frames_written < 5;
                                                });
  CHECK(result.cancelled);
  CHECK(std::filesystem::exists(path));
  const auto bytes = ReadAll(path);
  CHECK_EQ(std::string(bytes.begin(), bytes.end()), std::string("the previous delivery"));
  CHECK(Leftovers(path).empty());
  std::filesystem::remove(path);
}

CUTLINE_TEST(AnExistingDeliverySurvivesAWriterFailure) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("survive.mkv");
  std::filesystem::remove(path);
  WriteBytes(path, "keep");

  cutline::media::ExportSettings settings;
  settings.path = path.string();
  settings.overwrite = true;
  cutline::media::VideoEncoderSettings video;
  video.codec = "ffv1";
  video.pixel_format = "bgr0";
  video.width = 64;
  video.height = 36;
  video.frame_rate = {25, 1};
  settings.video = video;
  {
    auto writer = cutline::media::WriterRegistry::Instance().Open(settings);
    writer->WriteVideo(VideoFrame::Allocate(PixelFormat::Rgba8, 64, 36));
    // A frame of the wrong size aborts the export part-way through.
    CHECK_THROWS(writer->WriteVideo(VideoFrame::Allocate(PixelFormat::Rgba8, 32, 18)));
  }  // the writer is abandoned without Finish
  const auto bytes = ReadAll(path);
  CHECK_EQ(std::string(bytes.begin(), bytes.end()), std::string("keep"));
  CHECK(Leftovers(path).empty());
  std::filesystem::remove(path);
}

CUTLINE_TEST(AnExistingFileIsRefusedUnlessOverwriteIsAllowed) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("guarded.mkv");
  std::filesystem::remove(path);
  WriteBytes(path, "precious");

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  CHECK_THROWS(cutline::exporter::Export(engine, LosslessRequest(path)));
  const auto bytes = ReadAll(path);
  CHECK_EQ(std::string(bytes.begin(), bytes.end()), std::string("precious"));
  CHECK(Leftovers(path).empty());

  auto allowed = LosslessRequest(path);
  allowed.overwrite = true;
  CHECK_NO_THROW(cutline::exporter::Export(engine, allowed));
  CHECK(std::filesystem::file_size(path) > 1000);
  std::filesystem::remove(path);
}

CUTLINE_TEST(TheDestinationAppearsOnlyWhenTheExportIsComplete) {
  // A half-written file at the final path looks like a finished render to
  // anything watching the folder. Work happens in a sibling temporary, which
  // is published in one step at the end.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("publish.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  bool existed_early = false;
  bool temporary_seen = false;
  const auto result = cutline::exporter::Export(engine, LosslessRequest(path),
                                                [&](const cutline::exporter::ExportProgress&) {
                                                  if (std::filesystem::exists(path)) existed_early = true;
                                                  if (!Leftovers(path).empty()) temporary_seen = true;
                                                  return true;
                                                });
  CHECK(!result.cancelled);
  CHECK(!existed_early);
  CHECK(temporary_seen);
  CHECK(std::filesystem::exists(path));
  CHECK(Leftovers(path).empty());
  std::filesystem::remove(path);
}

CUTLINE_TEST(ExportAtAnotherSizeIsRefusedBeforeAnythingIsWritten) {
  // Other sizes are not supported yet. That has to be a clear refusal up front,
  // not an exception from deep inside the writer on the first frame after a
  // file has already been created.
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileEncoding(), "built without FFmpeg");
  const auto path = Scratch("resize.mkv");
  std::filesystem::remove(path);

  PlaybackEngine engine(BuildGraph(), CounterLocator(), Config());
  auto request = LosslessRequest(path);
  request.video.width = 96;
  request.video.height = 54;
  bool refused = false;
  try {
    const auto result = cutline::exporter::Export(engine, request);
    (void)result;
  } catch (const std::exception& error) {
    refused = std::string(error.what()).find("size") != std::string::npos;
  }
  CHECK(refused);
  CHECK(!std::filesystem::exists(path));
  CHECK(Leftovers(path).empty());
}

// ------------------------------------------------------- presets, queue, checks ----

namespace {

using cutline::exporter::ExportJob;
using cutline::exporter::ExportQueue;
using cutline::exporter::JobOutcome;
using cutline::exporter::JobProgress;
using cutline::exporter::JobState;
using cutline::exporter::PresetCatalogue;
using cutline::exporter::ResolveOptions;
using cutline::exporter::SequenceFacts;

SequenceFacts Hd25() {
  SequenceFacts facts;
  facts.width = 1920;
  facts.height = 1080;
  facts.frame_rate = {25, 1};
  facts.sample_rate = 48000;
  facts.channels = 2;
  facts.duration = Seconds(60);
  return facts;
}

cutline::exporter::EncoderAvailability Everything() { return [](const std::string&, const std::string&, const std::map<std::string, std::string>&, std::string*) { return true; }; }
cutline::exporter::EncoderAvailability Only(std::set<std::string> codecs) {
  return [codecs](const std::string& codec, const std::string&, const std::map<std::string, std::string>&, std::string* why) {
    if (codecs.count(codec) != 0) return true;
    if (why != nullptr) *why = "no such device";
    return false;
  };
}

ResolveOptions To(const std::string& path, cutline::exporter::EncoderAvailability availability = Everything()) {
  ResolveOptions options;
  options.output_path = path;
  options.availability = std::move(availability);
  return options;
}

bool Encodes() { return cutline::media::HasFileEncoding(); }

}  // namespace

CUTLINE_TEST(EveryPresetNamesRealEncodersAndResolvesToSettingsThatMatchItsPromises) {
  const auto& presets = PresetCatalogue::BuiltIn();
  CHECK(presets.size() >= 18);
  std::set<std::string> ids;
  std::set<std::string> known;
  if (Encodes()) {
    for (const auto& encoder : cutline::media::ListEncoders()) known.insert(encoder.name);
  }
  for (const auto& preset : presets) {
    CHECK(ids.insert(preset.id).second);
    CHECK(!preset.name.empty() && !preset.description.empty() && !preset.extension.empty());
    CHECK(PresetCatalogue::Find(preset.id) == &preset);
    const bool any = !preset.software.empty() || !preset.hardware.empty();
    CHECK(preset.has_video == any);
    CHECK(preset.has_video || preset.has_audio);
    if (known.empty()) continue;   // a build without FFmpeg cannot say what exists
    for (const auto* list : {&preset.software, &preset.hardware}) {
      for (const auto& choice : *list) CHECK(known.count(choice.codec) != 0);
    }
    if (preset.has_audio) CHECK(known.count(preset.audio.codec) != 0);
    // Resolved for 1080p25, with everything available, it is complete and keeps the preset's own promises.
    const auto resolved = cutline::exporter::ResolvePreset(preset, Hd25(), To("C:/out/delivery.dummy"));
    CHECK(resolved.ok);
    CHECK(std::filesystem::path(resolved.output_path).extension() == "." + preset.extension);
    CHECK_EQ(resolved.request.include_video, preset.has_video);
    CHECK(resolved.estimated_bytes > 0);
    if (preset.has_video) {
      CHECK(resolved.request.video.width == 1920 && resolved.request.video.height == 1080 && resolved.request.video.frame_rate.numerator == 25);
      CHECK(!resolved.request.video.codec.empty() && !resolved.request.video.pixel_format.empty());
      CHECK_EQ(resolved.request.high_precision, preset.high_precision);
    }
  }
}

CUTLINE_TEST(ABitrateScalesWithThePictureAndIsClampedAndAProfileCodecIsLeftToItsProfile) {
  const auto* high = PresetCatalogue::Find("web.h264.high");
  const auto* prores = PresetCatalogue::Find("mezzanine.prores.hq");
  const auto* xdcam = PresetCatalogue::Find("broadcast.xdcam422");
  CHECK(high != nullptr && prores != nullptr && xdcam != nullptr);
  // 1080p25 at 0.12 bits per pixel per frame.
  auto resolved = cutline::exporter::ResolvePreset(*high, Hd25(), To("a.mp4", Only({"libopenh264", "aac"})));
  CHECK(resolved.ok && resolved.encoder == "libopenh264" && !resolved.hardware);
  CHECK(std::abs(static_cast<double>(resolved.request.video.bitrate) - 1920.0 * 1080.0 * 25.0 * 0.12) < 2.0);
  CHECK_EQ(resolved.request.video.gop_size, std::int64_t{50});            // a keyframe every two seconds
  CHECK(resolved.request.container_options.at("movflags") == "+faststart");
  // A small picture is held at the floor, a UHD one is not above the ceiling.
  auto small = Hd25();
  small.width = 320;
  small.height = 180;
  CHECK_EQ(cutline::exporter::ResolvePreset(*high, small, To("a.mp4")).request.video.bitrate, std::int64_t{1000000});
  auto big = Hd25();
  big.width = 4096;
  big.height = 2160;
  big.frame_rate = {120, 1};
  CHECK_EQ(cutline::exporter::ResolvePreset(*high, big, To("a.mp4")).request.video.bitrate, std::int64_t{80000000});
  // ProRes: no data rate is passed, the profile decides; the broadcast preset fixes 50 Mbit/s.
  resolved = cutline::exporter::ResolvePreset(*prores, Hd25(), To("a.mov"));
  CHECK(resolved.ok && resolved.request.video.bitrate == 0 && resolved.request.video.pixel_format == "yuv422p10le" && resolved.request.high_precision);
  CHECK(resolved.request.video.options.at("profile") == "hq");
  CHECK(resolved.request.audio.codec == "pcm_s24le");
  resolved = cutline::exporter::ResolvePreset(*xdcam, Hd25(), To("a.mxf"));
  CHECK_EQ(resolved.request.video.bitrate, std::int64_t{50000000});
  CHECK_EQ(resolved.request.audio.sample_rate, std::int64_t{48000});
  // The estimate follows the length: sixty seconds of 1080p25 ProRes HQ at about 5.2 bits a pixel.
  CHECK(std::abs(static_cast<double>(cutline::exporter::ResolvePreset(*prores, Hd25(), To("a.mov")).estimated_bytes) / 1e6 - 1920.0 * 1080.0 * 25.0 * 5.2 * 60.0 / 8.0 / 1e6) < 100.0);
}

CUTLINE_TEST(HardwareEncodersAreUsedWhenTheyWorkSkippedWithAReasonWhenTheyDoNotAndSoftwareStaysAvailable) {
  const auto* high = PresetCatalogue::Find("web.h264.high");
  const auto* hevc = PresetCatalogue::Find("web.hevc");
  // Only the AMD encoder opens: it is chosen, the other two vendors are noted as skipped.
  auto resolved = cutline::exporter::ResolvePreset(*high, Hd25(), To("a.mp4", Only({"h264_amf", "libopenh264", "aac"})));
  CHECK(resolved.ok && resolved.encoder == "h264_amf" && resolved.hardware);
  CHECK(resolved.request.video.pixel_format == "nv12" && resolved.request.video.options.count("usage") == 1);
  int skipped = 0;
  for (const auto& note : resolved.notes) skipped += note.rfind("Skipped ", 0) == 0 ? 1 : 0;
  CHECK_EQ(skipped, 1);   // NVIDIA's is tried first and passed over; Intel's is never reached
  CHECK(resolved.notes[0].find("h264_nvenc") != std::string::npos && resolved.notes[0].find("no such device") != std::string::npos);
  // Preferring software skips the cards altogether.
  auto options = To("a.mp4", Only({"h264_amf", "libopenh264", "aac"}));
  options.prefer_hardware = false;
  resolved = cutline::exporter::ResolvePreset(*high, Hd25(), options);
  CHECK(resolved.ok && resolved.encoder == "libopenh264" && !resolved.hardware && resolved.notes.empty());
  // Nothing works: refused, naming every encoder that was tried and why.
  resolved = cutline::exporter::ResolvePreset(*high, Hd25(), To("a.mp4", Only({})));
  CHECK(!resolved.ok && resolved.refusal.find("h264_amf") != std::string::npos && resolved.refusal.find("libopenh264") != std::string::npos);
  // HEVC has no software encoder in this build: with preferring software it still falls through to the card, and says so.
  options = To("a.mp4", Only({"hevc_amf", "aac"}));
  options.prefer_hardware = false;
  resolved = cutline::exporter::ResolvePreset(*hevc, Hd25(), options);
  CHECK(resolved.ok && resolved.encoder == "hevc_amf");
  CHECK(resolved.notes.back().find("no software encoder") != std::string::npos);
}

CUTLINE_TEST(APresetRefusesASequenceItCannotDeliverAndCorrectsTheFileExtension) {
  const auto* high = PresetCatalogue::Find("web.h264.high");
  const auto* mp3 = PresetCatalogue::Find("audio.mp3");
  auto odd = Hd25();
  odd.width = 1919;
  auto resolved = cutline::exporter::ResolvePreset(*high, odd, To("a.mp4"));
  CHECK(!resolved.ok && resolved.refusal.find("even picture width") != std::string::npos);
  auto huge = Hd25();
  huge.width = 7680;
  huge.height = 4320;
  resolved = cutline::exporter::ResolvePreset(*high, huge, To("a.mp4"));
  CHECK(!resolved.ok && resolved.refusal.find("allows at most") != std::string::npos);
  CHECK(!cutline::exporter::ResolvePreset(*high, Hd25(), To("")).ok);
  // The name follows the format; the person is told.
  resolved = cutline::exporter::ResolvePreset(*high, Hd25(), To("C:/out/film.mov"));
  CHECK(resolved.ok && std::filesystem::path(resolved.output_path).filename() == "film.mp4" && resolved.request.output_path == resolved.output_path);
  CHECK(resolved.notes.back().find("film.mp4") != std::string::npos);
  CHECK(cutline::exporter::ResolvePreset(*high, Hd25(), To("C:/out/film.MP4")).notes.empty());
  // MP3 takes two channels: a surround mix is folded down and the note says so.
  auto surround = Hd25();
  surround.channels = 6;
  resolved = cutline::exporter::ResolvePreset(*mp3, surround, To("a.mp3"));
  CHECK(resolved.ok && resolved.request.audio.channels == 2 && !resolved.request.include_video);
  CHECK(resolved.notes.back().find("Mixed 6 channels down to 2") != std::string::npos);
}

CUTLINE_TEST(EncoderSettingsReachTheEncoderAndAnUnknownOneIsAnErrorNotSilentlyIgnored) {
  SKIP_UNLESS(Encodes(), "this build cannot encode");
  cutline::media::RegisterAllProviders();
  const auto path = Scratch("options.mkv");
  std::filesystem::remove(path);
  const auto frame = [] {
    auto picture = VideoFrame::Allocate(PixelFormat::Rgba8, 128, 72);
    for (int y = 0; y < 72; ++y) for (int x = 0; x < 128 * 4; ++x) picture.row_u8(y)[x] = static_cast<std::uint8_t>(x + y);
    return picture;
  }();
  cutline::media::ExportSettings settings;
  settings.path = path.string();
  settings.overwrite = true;
  cutline::media::VideoEncoderSettings video;
  video.codec = "ffv1";
  video.width = 128;
  video.height = 72;
  video.frame_rate = {25, 1};
  video.pixel_format = "yuv420p";
  video.options = {{"level", "3"}};
  settings.video = video;
  {
    auto writer = cutline::media::WriterRegistry::Instance().Open(settings);
    writer->WriteVideo(frame);
    writer->Finish();
  }
  CHECK(std::filesystem::exists(path));
  settings.video->options = {{"no_such_setting", "1"}};
  CHECK_THROWS(cutline::media::WriterRegistry::Instance().Open(settings));
  settings.video->options.clear();
  settings.container_options = {{"no_such_muxer_setting", "1"}};
  CHECK_THROWS(cutline::media::WriterRegistry::Instance().Open(settings));
  // The encoder catalogue and the working test.
  const auto encoders = cutline::media::ListEncoders();
  CHECK(std::any_of(encoders.begin(), encoders.end(), [](const auto& e) { return e.name == "ffv1" && !e.hardware; }));
  CHECK(std::any_of(encoders.begin(), encoders.end(), [](const auto& e) { return e.name == "h264_amf" && e.hardware; }));
  std::string why;
  CHECK(cutline::media::TestEncoder("ffv1", "yuv420p", &why));
  CHECK(!cutline::media::TestEncoder("no_such_encoder", "yuv420p", &why) && why.find("no encoder") != std::string::npos);
  CHECK(!cutline::media::TestEncoder("ffv1", "no_such_format", &why));
  CHECK(cutline::media::TestEncoder("pcm_s24le", "", &why));
}

CUTLINE_TEST(APictureWithMoreThanEightBitsKeepsThemThroughAProResExport) {
  SKIP_UNLESS(Encodes() && cutline::media::TestEncoder("prores_ks", "yuv422p10le"), "no ProRes encoder");
  cutline::media::RegisterAllProviders();
  const auto path = Scratch("gradient.mov");
  std::filesystem::remove(path);
  constexpr int kWidth = 1280, kHeight = 72;
  // A smooth horizontal ramp, 1280 steps wide: held as a float picture, as the compositor makes it.
  auto ramp = VideoFrame::Allocate(PixelFormat::RgbaF32, kWidth, kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const float v = 0.1f + 0.8f * static_cast<float>(x) / static_cast<float>(kWidth - 1);
      auto* texel = ramp.row_f32(y) + static_cast<std::size_t>(x) * 4;
      texel[0] = texel[1] = texel[2] = v;
      texel[3] = 1.0f;
    }
  }
  cutline::media::ExportSettings settings;
  settings.path = path.string();
  settings.overwrite = true;
  cutline::media::VideoEncoderSettings video;
  video.codec = "prores_ks";
  video.width = kWidth;
  video.height = kHeight;
  video.frame_rate = {25, 1};
  video.pixel_format = "yuv422p10le";
  video.options = {{"profile", "hq"}};
  settings.video = video;
  {
    auto writer = cutline::media::WriterRegistry::Instance().Open(settings);
    for (int i = 0; i < 3; ++i) writer->WriteVideo(ramp);
    writer->Finish();
  }
  auto source = cutline::media::SourceRegistry::Instance().Open(path.string());
  CHECK(source != nullptr);
  const auto decoded = source->ReadVideo(RationalTime(0, 1));
  CHECK(decoded.has_value() && decoded->format() == PixelFormat::Rgba16);   // a 10-bit file decodes to 16-bit pictures
  // Count the distinct levels along one row: an 8-bit pipeline gives at most 256, a 10-bit one many more.
  std::set<int> levels;
  const auto* row = reinterpret_cast<const std::uint16_t*>(decoded->row(36));
  for (int x = 0; x < kWidth; ++x) levels.insert(row[x * 4 + 1]);
  std::fprintf(stderr, "    a %d-step ramp through ProRes 422 HQ decodes to %zu distinct green levels\n", kWidth, levels.size());
  CHECK(levels.size() > 400);   // an 8-bit pipeline gives 205 or fewer for this ramp
  // And the engine an export is made on renders in 16 bits when the preset says so.
  PlaybackEngine monitor(BuildGraph(), CounterLocator(), Config());
  CHECK(monitor.output_format() == PixelFormat::Rgba8);
  CHECK(monitor.ExportClone(PixelFormat::Rgba16)->output_format() == PixelFormat::Rgba16);
}

CUTLINE_TEST(EveryPresetThatWorksOnThisMachineProducesAFileThatPassesTheCheck) {
  SKIP_UNLESS(Encodes(), "this build cannot encode");
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(BuildGraph(4), CounterLocator(), Config());
  SequenceFacts facts;
  facts.width = 192;
  facts.height = 108;
  facts.frame_rate = {25, 1};
  facts.sample_rate = 48000;
  facts.channels = 2;
  facts.duration = Seconds(1);
  int exported = 0, unavailable = 0;
  std::string unavailable_names;
  for (const auto& preset : PresetCatalogue::BuiltIn()) {
    ResolveOptions options;
    options.output_path = Scratch("preset-" + preset.id + ".out").string();
    options.overwrite = true;
    options.in = Seconds(0);
    options.out = Seconds(1);
    auto resolved = cutline::exporter::ResolvePreset(preset, facts, options);
    if (!resolved.ok) {
      ++unavailable;
      unavailable_names += " " + preset.id;
      continue;
    }
    std::string failure;
    try {
      (void)cutline::exporter::Export(engine, resolved.request);
    } catch (const std::exception& error) {
      failure = error.what();
    }
    if (!failure.empty()) {
      // An encoder that opens in a test but will not take this picture is a real failure of the preset, not a skip.
      std::fprintf(stderr, "    %s (%s): %s\n", preset.id.c_str(), resolved.encoder.c_str(), failure.c_str());
    }
    CHECK(failure.empty());
    if (!failure.empty()) continue;
    cutline::exporter::ExportExpectation expected;
    expected.video = preset.has_video;
    expected.audio = preset.has_audio;
    expected.width = facts.width;
    expected.height = facts.height;
    expected.frame_rate = facts.frame_rate;
    expected.duration = Seconds(1);
    expected.sample_rate = resolved.request.audio.sample_rate;
    expected.channels = resolved.request.audio.channels;
    expected.video_codec = resolved.video_stream_codec;
    expected.audio_codec = resolved.audio_stream_codec;
    const auto checked = cutline::exporter::ValidateExport(resolved.output_path, expected);
    if (!checked.ok) {
      for (const auto& problem : checked.problems) std::fprintf(stderr, "    %s: %s\n", preset.id.c_str(), problem.c_str());
    }
    CHECK(checked.ok);
    ++exported;
  }
  std::fprintf(stderr, "    %d presets exported and checked; %d not usable on this machine:%s\n", exported, unavailable, unavailable_names.c_str());
  CHECK(exported >= 12);
}

CUTLINE_TEST(TheCheckOfAFinishedExportNamesWhatIsWrongWithATruncatedWrongOrMissingFile) {
  SKIP_UNLESS(Encodes(), "this build cannot encode");
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(BuildGraph(4), CounterLocator(), Config());
  const auto path = Scratch("checked.mkv");
  std::filesystem::remove(path);
  auto request = LosslessRequest(path);
  request.in = Seconds(0);
  request.out = Seconds(2);
  (void)cutline::exporter::Export(engine, request);
  cutline::exporter::ExportExpectation expected;
  expected.width = 192;
  expected.height = 108;
  expected.frame_rate = {25, 1};
  expected.duration = Seconds(2);
  expected.sample_rate = 48000;
  expected.channels = 2;
  expected.video_codec = "ffv1";
  expected.audio_codec = "pcm_s16le";
  auto good = cutline::exporter::ValidateExport(path.string(), expected);
  CHECK(good.ok && good.bytes > 0 && good.video_codec == "ffv1");
  // Expecting something else names each difference.
  auto wrong = expected;
  wrong.width = 1920;
  wrong.height = 1080;
  wrong.duration = Seconds(5);
  wrong.sample_rate = 44100;
  wrong.channels = 6;
  wrong.video_codec = "h264";
  const auto bad = cutline::exporter::ValidateExport(path.string(), wrong);
  CHECK(!bad.ok);
  const auto mentions = [&](const char* text) { return std::any_of(bad.problems.begin(), bad.problems.end(), [&](const std::string& p) { return p.find(text) != std::string::npos; }); };
  CHECK(mentions("1920x1080") && mentions("long") && mentions("44100") && mentions("6 asked for") && mentions("h264"));
  // No sound asked for but present, and no picture asked for but present, are problems too.
  auto silent = expected;
  silent.audio = false;
  CHECK(!cutline::exporter::ValidateExport(path.string(), silent).ok);
  // A file cut short, and one that is not there.
  const auto bytes = ReadAll(path);
  const auto truncated = Scratch("truncated.mkv");
  WriteBytes(truncated, std::string(bytes.data(), bytes.size() / 3));
  CHECK(!cutline::exporter::ValidateExport(truncated.string(), expected).ok);
  const auto missing = cutline::exporter::ValidateExport(Scratch("nothing-here.mkv").string(), expected);
  CHECK(!missing.ok && missing.problems[0].find("was not created") != std::string::npos);
  const auto empty = Scratch("empty.mkv");
  WriteBytes(empty, "");
  CHECK(!cutline::exporter::ValidateExport(empty.string(), expected).ok);
}

CUTLINE_TEST(TheHardwareAndFinishingPresetsThatNeedAnHdPictureExportAtOneAndPassTheCheck) {
  SKIP_UNLESS(Encodes(), "this build cannot encode");
  cutline::media::RegisterAllProviders();
  PlaybackEngine engine(BuildGraph(3, 1280, 720), CounterLocator(), Config());
  SequenceFacts facts;
  facts.width = 1280;
  facts.height = 720;
  facts.frame_rate = {25, 1};
  facts.sample_rate = 48000;
  facts.channels = 2;
  facts.duration = Seconds(1);
  int exported = 0;
  for (const auto* id : {"web.hevc", "web.hevc.10bit", "web.av1", "mezzanine.dnxhr.sq", "mezzanine.dnxhr.hq", "mezzanine.dnxhr.hqx", "mezzanine.dnxhr.444", "mezzanine.prores.4444", "broadcast.xdcam422"}) {
    const auto* preset = PresetCatalogue::Find(id);
    CHECK(preset != nullptr);
    ResolveOptions options;
    options.output_path = Scratch(std::string("hd-") + id + ".out").string();
    options.overwrite = true;
    options.in = Seconds(0);
    options.out = Seconds(1);
    const auto resolved = cutline::exporter::ResolvePreset(*preset, facts, options);
    if (!resolved.ok) {
      std::fprintf(stderr, "    %s: not on this machine (%s)\n", id, resolved.refusal.substr(0, 150).c_str());
      continue;
    }
    (void)cutline::exporter::Export(engine, resolved.request);
    cutline::exporter::ExportExpectation expected;
    expected.width = 1280;
    expected.height = 720;
    expected.frame_rate = facts.frame_rate;
    expected.duration = Seconds(1);
    expected.sample_rate = resolved.request.audio.sample_rate;
    expected.channels = resolved.request.audio.channels;
    expected.video_codec = resolved.video_stream_codec;
    expected.audio_codec = resolved.audio_stream_codec;
    const auto checked = cutline::exporter::ValidateExport(resolved.output_path, expected);
    for (const auto& problem : checked.problems) std::fprintf(stderr, "    %s: %s\n", id, problem.c_str());
    CHECK(checked.ok);
    std::fprintf(stderr, "    %-24s %-14s %s %.1f MB\n", id, resolved.encoder.c_str(), resolved.hardware ? "(hardware)" : "(software)", static_cast<double>(checked.bytes) / 1e6);
    ++exported;
  }
  CHECK(exported >= 6);   // DNxHR, ProRes 4444, AV1 and XDCAM work in software anywhere this build runs
}

// ------------------------------------------------------------------ the queue ----

namespace {

// An executor that records what ran and takes a controllable time.
struct FakeRun final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::string> started;
  std::set<std::string> fail;
  bool hold{false};   // jobs wait until released
  int frames{4};

  JobOutcome Run(const ExportJob& job, const JobProgress& progress) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      started.push_back(job.id);
      changed.notify_all();
      changed.wait(lock, [&] { return !hold; });
      if (fail.count(job.id) != 0) throw std::runtime_error("the disk is full");
    }
    for (int i = 1; i <= frames; ++i) {
      if (!progress(i, frames)) return {};
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    JobOutcome outcome;
    outcome.encoder = "fake";
    outcome.frames = frames;
    outcome.output_bytes = 1234;
    outcome.notes = {"a note"};
    return outcome;
  }
  bool WaitStarted(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5), [&] { return started.size() >= count; });
  }
  void Release() {
    const std::lock_guard<std::mutex> lock(mutex);
    hold = false;
    changed.notify_all();
  }
};

ExportJob Job(const std::string& id, const std::string& output = "") {
  ExportJob job;
  job.id = id;
  job.name = "Job " + id;
  job.sequence_id = "seq-1";
  job.preset_id = "web.h264.high";
  job.output_path = output.empty() ? "C:/out/" + id + ".mp4" : output;
  return job;
}

JobState StateOf(const ExportQueue& queue, const std::string& id) {
  const auto found = queue.Find(id);
  return found ? found->state : JobState::Failed;
}

}  // namespace

CUTLINE_TEST(QueuedExportsRunOneAtATimeInOrderAndKeepTheirProgressAndOutcome) {
  FakeRun run;
  ExportQueue queue("", [&](const ExportJob& job, const JobProgress& progress) { return run.Run(job, progress); });
  std::atomic<int> notifications{0};
  queue.SetListener([&] { ++notifications; });
  queue.Add(Job("a"));
  queue.Add(Job("b"));
  queue.Add(Job("c"));
  CHECK(queue.WaitIdle(std::chrono::seconds(10)));
  CHECK((run.started == std::vector<std::string>{"a", "b", "c"}));
  for (const auto* id : {"a", "b", "c"}) {
    const auto job = *queue.Find(id);
    CHECK(job.state == JobState::Done && job.frames_done == 4 && job.frames_total == 4 && job.progress() == 1.0);
    CHECK(job.encoder == "fake" && job.output_bytes == 1234 && job.notes.size() == 1 && !job.created.empty() && !job.started.empty() && !job.finished.empty());
  }
  CHECK(notifications.load() > 6);
  // Ids are made when none is given, and a repeated one is not reused for another job.
  ExportJob unnamed = Job("");
  unnamed.id.clear();
  const auto made = queue.Add(unnamed);
  CHECK(!made.empty() && made != "a");
  CHECK(queue.WaitIdle(std::chrono::seconds(10)));
  queue.ClearFinished();
  CHECK(queue.Jobs().empty());
}

CUTLINE_TEST(AFailedExportIsRecordedWithItsReasonAndCanBeRetriedAndACancelledOneStopsPromptly) {
  FakeRun run;
  run.fail = {"bad"};
  ExportQueue queue("", [&](const ExportJob& job, const JobProgress& progress) { return run.Run(job, progress); });
  queue.Add(Job("bad"));
  queue.Add(Job("good"));
  CHECK(queue.WaitIdle(std::chrono::seconds(10)));
  CHECK(StateOf(queue, "bad") == JobState::Failed && queue.Find("bad")->error == "the disk is full");
  CHECK(StateOf(queue, "good") == JobState::Done);   // one failure does not stop the others
  CHECK(!queue.Retry("good"));                       // a finished job is not retried
  {
    const std::lock_guard<std::mutex> lock(run.mutex);
    run.fail.clear();
  }
  CHECK(queue.Retry("bad"));
  CHECK(queue.WaitIdle(std::chrono::seconds(10)));
  CHECK(StateOf(queue, "bad") == JobState::Done && queue.Find("bad")->error.empty());

  // Cancel the one that is running: it stops at its next progress report; the next one then runs.
  run.frames = 100000;
  ExportQueue slow("", [&](const ExportJob& job, const JobProgress& progress) { return run.Run(job, progress); });
  slow.Add(Job("long"));
  slow.Add(Job("after"));
  CHECK(run.WaitStarted(4));   // bad, good and the retried bad ran before; this is the fourth start
  while (StateOf(slow, "long") != JobState::Running) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(slow.Cancel("long"));
  run.frames = 2;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (StateOf(slow, "long") == JobState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK(StateOf(slow, "long") == JobState::Cancelled);
  CHECK(slow.WaitIdle(std::chrono::seconds(10)));
  CHECK(StateOf(slow, "after") == JobState::Done);
  CHECK(!slow.Cancel("after") && !slow.Cancel("nothing"));
}

CUTLINE_TEST(WaitingJobsCanBeCancelledRemovedReorderedAndPausedWithoutDisturbingTheRunningOne) {
  FakeRun run;
  run.hold = true;
  ExportQueue queue("", [&](const ExportJob& job, const JobProgress& progress) { return run.Run(job, progress); });
  queue.Add(Job("one"));
  CHECK(run.WaitStarted(1));            // "one" is running, held
  queue.Add(Job("two"));
  queue.Add(Job("three"));
  queue.Add(Job("four"));
  CHECK(!queue.Remove("one"));          // the running job cannot be removed
  CHECK(queue.Move("four", 0));         // four is next
  CHECK(queue.Cancel("three"));
  CHECK(StateOf(queue, "three") == JobState::Cancelled);
  CHECK(queue.Remove("three"));
  CHECK(!queue.Find("three").has_value());
  queue.Pause();
  CHECK(queue.paused());
  run.Release();                        // "one" finishes; paused, nothing else starts
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (StateOf(queue, "one") != JobState::Done && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK(StateOf(queue, "one") == JobState::Done);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(StateOf(queue, "four") == JobState::Queued && StateOf(queue, "two") == JobState::Queued);
  queue.Resume();
  CHECK(queue.WaitIdle(std::chrono::seconds(10)));
  CHECK((run.started == std::vector<std::string>{"one", "four", "two"}));
}

CUTLINE_TEST(TheQueueSurvivesARestartAJobThatWasRunningIsInterruptedAndADamagedRecordIsKeptAside) {
  const auto file = Scratch("queue/queue.json");
  std::filesystem::remove_all(file.parent_path());
  FakeRun run;
  run.hold = true;
  const auto part = file.parent_path() / "delivery.mp4.cutline-1-1.part";
  {
    ExportQueue queue(file.string(), [&](const ExportJob& job, const JobProgress& progress) { return run.Run(job, progress); });
    ExportJob first = Job("first", (file.parent_path() / "delivery.mp4").string());
    first.in = Seconds(2);
    first.out = Seconds(9);
    first.preset_id = "mezzanine.prores.hq";
    first.prefer_hardware = false;
    queue.Add(first);
    queue.Add(Job("second"));
    CHECK(run.WaitStarted(1));
    WriteBytes(part, "half a file");   // what a crash would leave beside the delivery
    // The queue is destroyed while "first" is running: as a closing application would.
    run.frames = 100000;
    run.Release();
    {
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (StateOf(queue, "first") != JobState::Running && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  CHECK(std::filesystem::exists(file));
  {
    FakeRun idle;
    idle.hold = true;
    ExportQueue reopened(file.string(), [&](const ExportJob& job, const JobProgress& progress) { return idle.Run(job, progress); }, /*start_paused=*/true);
    const auto jobs = reopened.Jobs();
    CHECK_EQ(jobs.size(), std::size_t{2});
    const auto first = *reopened.Find("first");
    CHECK(first.state == JobState::Interrupted && !first.error.empty());
    CHECK(!std::filesystem::exists(file.parent_path() / "delivery.mp4.cutline-1-1.part"));
    CHECK(first.preset_id == "mezzanine.prores.hq" && first.in.Compare(Seconds(2)) == 0 && first.out.Compare(Seconds(9)) == 0 && !first.prefer_hardware);
    CHECK(StateOf(reopened, "second") == JobState::Queued);   // still waiting; nothing started on its own
    CHECK(reopened.paused());
    // The half-written file the dead run left is cleaned up; the person decides about running it again.
    CHECK(reopened.Retry("first"));
    CHECK(StateOf(reopened, "first") == JobState::Queued);
    // Retry went to the back of the line: second would run first.
    const auto order = reopened.Jobs();
    CHECK(order[0].id == "second" && order[1].id == "first");
    idle.Release();
  }
  // A damaged record is set aside, not overwritten, and the queue starts empty.
  WriteBytes(file, "{ this is not json");
  {
    ExportQueue damaged(file.string(), [](const ExportJob&, const JobProgress&) { return JobOutcome{}; }, true);
    CHECK(damaged.Jobs().empty());
    CHECK(std::filesystem::exists(file.string() + ".damaged"));
  }
}

int main() { return cutline::testing::RunAll("export"); }

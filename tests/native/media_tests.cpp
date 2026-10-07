// Frame buffers, audio buffers, timestamp maps, and the synthetic source.

#include "media/AudioBuffer.h"
#include "media/IngestWorkflow.h"
#include "media/Providers.h"
#include "media/ProxyWorkflow.h"
#include "media/Source.h"
#include "media/SyntheticSource.h"
#include "media/TimestampMap.h"
#include "media/VideoFrame.h"
#include "tests/native/TestHarness.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace model = cutline::model;
using cutline::media::AudioBuffer;
using cutline::media::ConvertFrame;
using cutline::media::PixelFormat;
using cutline::media::SourceRegistry;
using cutline::media::SyntheticPattern;
using cutline::media::SyntheticSpec;
using cutline::media::VideoFrame;
using cutline::time::RationalTime;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

bool Near(float actual, float expected, float tolerance = 1e-4f) { return std::abs(actual - expected) < tolerance; }

SyntheticSpec CounterSpec(std::int64_t seconds = 10) {
  SyntheticSpec spec;
  spec.pattern = SyntheticPattern::Counter;
  spec.width = 320;
  spec.height = 180;
  spec.frame_rate = {30, 1};
  spec.duration = Seconds(seconds);
  return spec;
}

}  // namespace

// ------------------------------------------------------------ video frames ----

CUTLINE_TEST(FrameAllocationRejectsEmptySizes) {
  CHECK_THROWS(VideoFrame::Allocate(PixelFormat::Rgba8, 0, 10));
  CHECK_THROWS(VideoFrame::Allocate(PixelFormat::Rgba8, 10, -1));
}

CUTLINE_TEST(FrameRowsAreAlignedAndBoundsChecked) {
  auto frame = VideoFrame::Allocate(PixelFormat::Rgba8, 100, 10);
  CHECK_EQ(frame.width(), 100);
  CHECK(frame.valid());
  // 100 px * 4 bytes is 400, padded up to the 64-byte row alignment.
  CHECK_EQ(frame.stride(), std::ptrdiff_t{448});
  CHECK_THROWS(frame.row(10));
  CHECK_THROWS(frame.row(-1));
  // Typed access refuses a mismatched format rather than reinterpreting bytes.
  CHECK_THROWS(frame.row_f32(0));
}

CUTLINE_TEST(FrameCloneCopiesPixelsAndMetadata) {
  auto frame = VideoFrame::Allocate(PixelFormat::Rgba8, 4, 2);
  frame.row_u8(0)[0] = 42;
  frame.presentation_time = Seconds(3);
  frame.keyframe = true;
  const auto copy = frame.Clone();
  CHECK_EQ(static_cast<int>(copy.row_u8(0)[0]), 42);
  CHECK_EQ(copy.presentation_time.Compare(Seconds(3)), 0);
  CHECK(copy.keyframe);
}

CUTLINE_TEST(EightBitRoundTripsThroughFloatExactly) {
  // Every 8-bit level must survive 8 -> float -> 8 unchanged, or repeated
  // conversions through the compositor would drift.
  auto frame = VideoFrame::Allocate(PixelFormat::Rgba8, 256, 1);
  for (int x = 0; x < 256; ++x) {
    auto* pixel = frame.row_u8(0) + static_cast<std::size_t>(x) * 4;
    pixel[0] = static_cast<std::uint8_t>(x);
    pixel[1] = static_cast<std::uint8_t>(255 - x);
    pixel[2] = static_cast<std::uint8_t>(x);
    pixel[3] = 255;
  }
  const auto round_trip = ConvertFrame(ConvertFrame(frame, PixelFormat::RgbaF32), PixelFormat::Rgba8);
  for (int x = 0; x < 256; ++x) {
    const auto* original = frame.row_u8(0) + static_cast<std::size_t>(x) * 4;
    const auto* restored = round_trip.row_u8(0) + static_cast<std::size_t>(x) * 4;
    CHECK_EQ(static_cast<int>(restored[0]), static_cast<int>(original[0]));
    CHECK_EQ(static_cast<int>(restored[1]), static_cast<int>(original[1]));
  }
}

CUTLINE_TEST(SixteenBitRoundTripsThroughFloatExactly) {
  auto frame = VideoFrame::Allocate(PixelFormat::Rgba16, 4, 1);
  auto* pixels = reinterpret_cast<std::uint16_t*>(frame.row(0));
  pixels[0] = 0;
  pixels[1] = 1;
  pixels[2] = 32768;
  pixels[3] = 65535;
  const auto round_trip = ConvertFrame(ConvertFrame(frame, PixelFormat::RgbaF32), PixelFormat::Rgba16);
  const auto* restored = reinterpret_cast<const std::uint16_t*>(round_trip.row(0));
  for (int index = 0; index < 4; ++index) CHECK_EQ(static_cast<int>(restored[index]), static_cast<int>(pixels[index]));
}

CUTLINE_TEST(FloatConversionClampsOutOfRangeValues) {
  auto frame = VideoFrame::Allocate(PixelFormat::RgbaF32, 2, 1);
  auto* row = frame.row_f32(0);
  row[0] = -0.5f;  // below black
  row[1] = 1.5f;   // above white
  row[2] = 1.0f;
  row[3] = 1.0f;
  const auto converted = ConvertFrame(frame, PixelFormat::Rgba8);
  const auto* pixels = converted.row_u8(0);
  CHECK_EQ(static_cast<int>(pixels[0]), 0);
  CHECK_EQ(static_cast<int>(pixels[1]), 255);
  CHECK_EQ(static_cast<int>(pixels[2]), 255);
}

CUTLINE_TEST(ConvertingAnEmptyFrameFails) {
  const VideoFrame empty;
  CHECK_THROWS(ConvertFrame(empty, PixelFormat::Rgba8));
}

CUTLINE_TEST(LocalProxyGenerationResizesAndCanBeCancelled) {
  cutline::media::RegisterAllProviders();
  SyntheticSpec source;
  source.pattern = SyntheticPattern::Gradient;
  source.width = 96;
  source.height = 54;
  source.frame_rate = {12, 1};
  source.duration = RationalTime(1, 2);

  const auto directory = std::filesystem::temp_directory_path() / "cutline-proxy-generation";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  cutline::media::ProxyGenerationRequest request;
  request.source_path = source.ToPath();
  request.source_fingerprint = "source-fp";
  request.output_path = (directory / "proxy.mkv").string();
  request.preset.max_width = 48;
  request.preset.max_height = 28;
  request.preset.video_codec = "ffv1";
  request.preset.pixel_format = "yuv420p";
  request.preset.video_bitrate = 0;

  std::int64_t progress_frames = 0;
  const auto result = cutline::media::GenerateProxy(
      request, [&](const cutline::media::ProxyGenerationProgress& value) { progress_frames = value.frames_complete; });
  CHECK(!result.cancelled);
  CHECK_EQ(result.width, std::int64_t{48});
  CHECK_EQ(result.height, std::int64_t{26});
  CHECK_EQ(result.video_frames, std::int64_t{6});
  CHECK_EQ(progress_frames, std::int64_t{6});
  CHECK(!result.fingerprint.empty());
  const auto probe = SourceRegistry::Instance().ProbeFile(result.path);
  CHECK_EQ(probe.PrimaryVideo()->width, std::int64_t{48});
  CHECK_EQ(probe.PrimaryVideo()->height, std::int64_t{26});

  request.output_path = (directory / "cancelled.mkv").string();
  const auto cancelled = cutline::media::GenerateProxy(request, {}, [] { return true; });
  CHECK(cancelled.cancelled);
  CHECK(!std::filesystem::exists(request.output_path));
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(ProxyAutomationUsesMediaComplexityPlaybackMissesAndMemoryPressure) {
  cutline::media::ProxyComplexity easy;
  easy.width = 1920;
  easy.height = 1080;
  easy.frames_per_second = 30.0;
  easy.codec = "h264";
  CHECK(!cutline::media::ChooseProxyPreset(easy).generate);

  auto difficult = easy;
  difficult.width = 7680;
  difficult.height = 4320;
  difficult.frames_per_second = 60.0;
  difficult.bit_depth = 10;
  difficult.codec = "av1";
  const auto complex = cutline::media::ChooseProxyPreset(difficult);
  CHECK(complex.generate);
  CHECK_EQ(complex.preset.max_width, std::int64_t{960});

  cutline::media::PlaybackHealth misses;
  misses.requested_frames = 1000;
  misses.deadline_misses = 70;
  misses.average_decode_ms = 36.0;
  const auto adaptive = cutline::media::ChooseProxyPreset(easy, misses);
  CHECK(adaptive.generate);
  CHECK_EQ(adaptive.preset.max_width, std::int64_t{1280});
  CHECK(adaptive.reason.find("misses") != std::string::npos);

  misses = {};
  misses.memory_pressure = cutline::resource::Pressure::Critical;
  const auto pressured = cutline::media::ChooseProxyPreset(easy, misses);
  CHECK(pressured.generate);
  CHECK_EQ(pressured.preset.max_height, std::int64_t{540});
}

// ------------------------------------------------------------ audio buffers ----

CUTLINE_TEST(AudioBufferAllocationIsValidated) {
  CHECK_THROWS(AudioBuffer::Allocate(0, 2, 128));
  CHECK_THROWS(AudioBuffer::Allocate(48000, 0, 128));
  CHECK_THROWS(AudioBuffer::Allocate(48000, 2, -1));
  const auto buffer = AudioBuffer::Allocate(48000, 2, 128);
  CHECK(buffer.valid());
  CHECK_EQ(buffer.frames(), std::int64_t{128});
  CHECK_THROWS(buffer.channel(2));
}

CUTLINE_TEST(MixingSumsChannelsWithGain) {
  auto target = AudioBuffer::Allocate(48000, 2, 4);
  auto source = AudioBuffer::Allocate(48000, 2, 4);
  for (std::int64_t frame = 0; frame < 4; ++frame) {
    source.channel(0)[frame] = 0.5f;
    source.channel(1)[frame] = -0.25f;
  }
  target.MixFrom(source, 1.0f);
  target.MixFrom(source, 0.5f);
  CHECK(Near(target.channel(0)[0], 0.75f));
  CHECK(Near(target.channel(1)[0], -0.375f));
}

CUTLINE_TEST(MixingRefusesMismatchedFormats) {
  auto stereo = AudioBuffer::Allocate(48000, 2, 4);
  const auto mono = AudioBuffer::Allocate(48000, 1, 4);
  const auto other_rate = AudioBuffer::Allocate(44100, 2, 4);
  CHECK_THROWS(stereo.MixFrom(mono, 1.0f));
  CHECK_THROWS(stereo.MixFrom(other_rate, 1.0f));
}

CUTLINE_TEST(SilenceAndPeakBehaveAsExpected) {
  auto buffer = AudioBuffer::Allocate(48000, 1, 8);
  buffer.channel(0)[3] = -0.8f;
  CHECK(Near(buffer.Peak(0), 0.8f));
  buffer.Silence();
  CHECK(Near(buffer.Peak(0), 0.0f));
}

CUTLINE_TEST(DecibelConversionHasASilenceFloor) {
  CHECK(Near(cutline::media::DecibelsToLinear(0.0), 1.0f));
  CHECK(Near(cutline::media::DecibelsToLinear(-6.0), 0.5012f, 1e-3f));
  // At and below the floor the result is exactly zero, so a muted track
  // contributes nothing rather than an inaudible residue.
  CHECK_EQ(cutline::media::DecibelsToLinear(-96.0), 0.0f);
  CHECK_EQ(cutline::media::DecibelsToLinear(-200.0), 0.0f);
}

CUTLINE_TEST(StereoPanHoldsConstantPower) {
  const auto centre = cutline::media::StereoPan(0.0);
  CHECK(Near(centre.left, centre.right));
  // Equal power: the squares sum to one at every position.
  for (const double position : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
    const auto gains = cutline::media::StereoPan(position);
    CHECK(Near(gains.left * gains.left + gains.right * gains.right, 1.0f, 1e-5f));
  }
  const auto left = cutline::media::StereoPan(-1.0);
  CHECK(Near(left.left, 1.0f, 1e-5f));
  CHECK(Near(left.right, 0.0f, 1e-5f));
  // Out-of-range values clamp rather than producing a negative gain.
  CHECK(Near(cutline::media::StereoPan(-5.0).left, 1.0f, 1e-5f));
}

CUTLINE_TEST(ChannelLayoutsMapToCounts) {
  CHECK_EQ(cutline::media::ChannelCountForLayout("mono"), std::int64_t{1});
  CHECK_EQ(cutline::media::ChannelCountForLayout("stereo"), std::int64_t{2});
  CHECK_EQ(cutline::media::ChannelCountForLayout("5.1"), std::int64_t{6});
  CHECK_THROWS(cutline::media::ChannelCountForLayout("ambisonic"));
}

// ----------------------------------------------------------- timestamp maps ----

CUTLINE_TEST(TimestampMapResolvesByPresentationTime) {
  std::vector<cutline::media::FrameTimestamp> frames;
  for (std::int64_t index = 0; index < 5; ++index) {
    frames.push_back({{index, 25}, {1, 25}, index == 0});
  }
  const cutline::media::TimestampMap map(std::move(frames));
  CHECK(map.cadence() == cutline::media::Cadence::Constant);
  CHECK_EQ(map.size(), std::size_t{5});
  // A time inside a frame resolves to that frame, not the next.
  CHECK_EQ(map.AtOrBefore({3, 50}).presentation_time.Compare({1, 25}), 0);
  // Before the first frame clamps to it.
  CHECK_EQ(map.AtOrBefore({-1, 25}).presentation_time.Compare({0, 1}), 0);
}

CUTLINE_TEST(TimestampMapDetectsVariableCadence) {
  std::vector<cutline::media::FrameTimestamp> frames{
      {{0, 25}, {1, 25}, true},
      {{1, 25}, {2, 25}, false},
      {{3, 25}, {1, 25}, false},
  };
  const cutline::media::TimestampMap map(std::move(frames));
  CHECK(map.cadence() == cutline::media::Cadence::Variable);
}

CUTLINE_TEST(TimestampMapRejectsNonsense) {
  CHECK_THROWS(cutline::media::TimestampMap({}));
  CHECK_THROWS(cutline::media::TimestampMap({{{0, 25}, {0, 1}, true}}));
}

// --------------------------------------------------------- synthetic source ----

CUTLINE_TEST(SyntheticPathsRoundTrip) {
  SyntheticSpec spec;
  spec.pattern = SyntheticPattern::Bars;
  spec.width = 640;
  spec.height = 360;
  spec.frame_rate = {30000, 1001};
  spec.duration = Seconds(7);
  spec.tone_hz = 440.0;
  const auto parsed = SyntheticSpec::Parse(spec.ToPath());
  CHECK(parsed.pattern == SyntheticPattern::Bars);
  CHECK_EQ(parsed.width, 640);
  CHECK_EQ(parsed.frame_rate.numerator, std::int64_t{30000});
  CHECK_EQ(parsed.frame_rate.denominator, std::int64_t{1001});
  CHECK_EQ(parsed.duration.Compare(Seconds(7)), 0);
  CHECK(std::abs(parsed.tone_hz - 440.0) < 1e-9);
}

CUTLINE_TEST(SyntheticPathsAreValidated) {
  CHECK_THROWS(SyntheticSpec::Parse("file:///not/synthetic.mov"));
  CHECK_THROWS(SyntheticSpec::Parse("synthetic:nosuchpattern"));
  CHECK_THROWS(SyntheticSpec::Parse("synthetic:bars?novalue"));
}

CUTLINE_TEST(SyntheticProbeDescribesItsStreams) {
  auto spec = CounterSpec();
  spec.tone_hz = 440.0;
  const auto source = cutline::media::OpenSynthetic(spec);
  const auto& probe = source->probe();
  CHECK_EQ(probe.duration.Compare(Seconds(10)), 0);
  CHECK_EQ(probe.streams.size(), std::size_t{2});

  const auto* video = probe.PrimaryVideo();
  CHECK(video != nullptr);
  CHECK_EQ(video->width, std::int64_t{320});
  CHECK_EQ(video->height, std::int64_t{180});
  CHECK(video->cadence == model::Cadence::Constant);

  const auto* audio = probe.PrimaryAudio();
  CHECK(audio != nullptr);
  CHECK_EQ(audio->sample_rate, std::int64_t{48000});
  CHECK_EQ(audio->channel_count, std::int64_t{2});
}

CUTLINE_TEST(CounterFramesIdentifyWhichFrameWasDecoded) {
  // This is the mechanism that makes retime, reverse, and seek behaviour
  // verifiable: the picture states which source frame it is.
  const auto source = cutline::media::OpenSynthetic(CounterSpec());
  for (const std::int64_t frame_index : {0, 1, 29, 100, 299}) {
    const auto at = RationalTime::FromFrames(frame_index, {30, 1});
    const auto frame = source->ReadVideo(at);
    CHECK(frame.has_value());
    CHECK_EQ(cutline::media::ReadFrameCounter(*frame), frame_index);
  }
}

CUTLINE_TEST(ReadingOutsideTheSourceReturnsNothing) {
  const auto source = cutline::media::OpenSynthetic(CounterSpec());
  CHECK(!source->ReadVideo(Seconds(11)).has_value());
  CHECK(!source->ReadVideo({-1, 1}).has_value());
  CHECK(source->ReadVideo(Seconds(9)).has_value());
}

CUTLINE_TEST(VariableFrameRateSourcesHoldFramesForLonger) {
  auto spec = CounterSpec(2);
  spec.variable_frame_rate = true;
  const auto source = cutline::media::OpenSynthetic(spec);
  CHECK(source->timestamps() != nullptr);
  CHECK(source->timestamps()->cadence() == cutline::media::Cadence::Variable);

  // Frame 1 is held for two intervals, so sampling a nominal frame 2 still
  // lands on source frame 1. A constant-rate assumption would return frame 2.
  const auto held = source->ReadVideo(RationalTime::FromFrames(2, {30, 1}));
  CHECK(held.has_value());
  CHECK_EQ(cutline::media::ReadFrameCounter(*held), std::int64_t{1});
}

CUTLINE_TEST(SyntheticAudioIsContinuousAcrossBlocks) {
  auto spec = CounterSpec();
  spec.tone_hz = 1000.0;
  spec.tone_amplitude = 1.0f;
  const auto source = cutline::media::OpenSynthetic(spec);

  // One long read and two adjacent short reads must agree, which is what
  // proves phase comes from absolute time rather than being accumulated.
  const auto whole = source->ReadAudio(Seconds(1), 48000, 2, 256);
  const auto first = source->ReadAudio(Seconds(1), 48000, 2, 128);
  const auto second = source->ReadAudio({48000 + 128, 48000}, 48000, 2, 128);
  CHECK(whole.has_value() && first.has_value() && second.has_value());
  for (std::int64_t frame = 0; frame < 128; ++frame) {
    CHECK(Near(whole->channel(0)[frame], first->channel(0)[frame], 1e-5f));
    CHECK(Near(whole->channel(0)[frame + 128], second->channel(0)[frame], 1e-5f));
  }
}

CUTLINE_TEST(SilentSourcesReturnNoAudio) {
  const auto source = cutline::media::OpenSynthetic(CounterSpec());  // tone_hz is 0
  CHECK(!source->ReadAudio(Seconds(1), 48000, 2, 128).has_value());
}

CUTLINE_TEST(AudioPastTheEndIsSilenceNotARepeat) {
  auto spec = CounterSpec(1);
  spec.tone_hz = 1000.0;
  const auto source = cutline::media::OpenSynthetic(spec);
  // A block straddling the end: the tail must be silent.
  const auto block = source->ReadAudio({47900, 48000}, 48000, 1, 400);
  CHECK(block.has_value());
  CHECK(Near(block->channel(0)[399], 0.0f, 1e-6f));
}

// -------------------------------------------------------- provider registry ----

CUTLINE_TEST(TheRegistryOpensSyntheticPaths) {
  cutline::media::RegisterSyntheticProvider();
  auto& registry = SourceRegistry::Instance();
  CHECK(!registry.empty());

  const auto path = CounterSpec().ToPath();
  const auto source = registry.Open(path);
  CHECK(source != nullptr);
  CHECK_EQ(source->probe().container, std::string("synthetic"));

  const auto probe = registry.ProbeFile(path);
  CHECK_EQ(probe.streams.size(), std::size_t{1});
}

CUTLINE_TEST(TheRegistryRefusesWhatItCannotRead) {
  cutline::media::RegisterSyntheticProvider();
  auto& registry = SourceRegistry::Instance();
  CHECK(registry.Open("/no/such/file.mov") == nullptr);
  CHECK_THROWS(registry.ProbeFile("/no/such/file.mov"));
}

CUTLINE_TEST(RegisteringTheSyntheticProviderTwiceIsHarmless) {
  cutline::media::RegisterSyntheticProvider();
  const auto before = SourceRegistry::Instance().ProviderNames().size();
  cutline::media::RegisterSyntheticProvider();
  CHECK_EQ(SourceRegistry::Instance().ProviderNames().size(), before);
}

// ---------------------------------------------------- FFmpeg decode ----

namespace {

// Fixtures are generated at configure time into the build tree (see
// cmake/Fixtures.cmake). Tests that need one skip rather than fail when it is
// absent, so a build without FFmpeg still reports honestly.
[[nodiscard]] std::optional<std::string> Fixture(const std::string& name) {
  const auto configured = cutline::testing::EnvironmentValue("CUTLINE_FIXTURE_DIR");
  if (configured.empty()) return std::nullopt;
  const auto path = std::filesystem::path(configured) / name;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
  return path.string();
}

// The colour the solid fixtures were generated with: 0x3366CC.
constexpr int kSolidRed = 0x33;
constexpr int kSolidGreen = 0x66;
constexpr int kSolidBlue = 0xCC;

struct Sample final {
  int r{};
  int g{};
  int b{};
  int a{};
};

[[nodiscard]] Sample SampleCentre(const VideoFrame& frame) {
  const auto rgba = frame.format() == PixelFormat::Rgba8 ? frame.Clone() : ConvertFrame(frame, PixelFormat::Rgba8);
  const auto* pixel = rgba.row_u8(rgba.height() / 2) + static_cast<std::size_t>(rgba.width() / 2) * 4;
  return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

}  // namespace

CUTLINE_TEST(FFmpegProviderRegistersWhenAvailable) {
  cutline::media::RegisterAllProviders();
  const auto names = SourceRegistry::Instance().ProviderNames();
  const bool has_ffmpeg = std::find(names.begin(), names.end(), "ffmpeg") != names.end();
  // The flag and the registry must agree, or a caller would be told file
  // decoding exists when no provider can do it.
  CHECK_EQ(has_ffmpeg, cutline::media::HasFileDecoding());
}

CUTLINE_TEST(LosslessRgbDecodesToAUniformFrame) {
  // The fixture is a single flat colour stored losslessly in an RGB layout, so
  // every decoded pixel must be identical. A stride mistake, a row-order
  // mistake, or a half-written buffer all show up here as variation.
  //
  // The exact value is deliberately not asserted: lavfi generates the colour
  // through its own YUV pipeline, so the file does not hold precisely the
  // requested RGB and pinning a constant here would be testing the generator.
  const auto path = Fixture("solid-rgb.mkv");
  SKIP_UNLESS(path.has_value(), "solid-rgb.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  CHECK(source != nullptr);
  const auto frame = source->ReadVideo(Seconds(1));
  CHECK(frame.has_value());

  const auto reference = SampleCentre(*frame);
  CHECK_EQ(reference.a, 255);
  const auto rgba = ConvertFrame(*frame, PixelFormat::Rgba8);
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* row = rgba.row_u8(y);
    for (int x = 0; x < rgba.width(); ++x) {
      const auto* pixel = row + static_cast<std::size_t>(x) * 4;
      CHECK_EQ(static_cast<int>(pixel[0]), reference.r);
      CHECK_EQ(static_cast<int>(pixel[1]), reference.g);
      CHECK_EQ(static_cast<int>(pixel[2]), reference.b);
    }
  }
  // It is still the colour family that was asked for, which catches a wholesale
  // channel swap that uniformity alone would not.
  CHECK(std::abs(reference.r - kSolidRed) <= 4);
  CHECK(std::abs(reference.g - kSolidGreen) <= 4);
  CHECK(std::abs(reference.b - kSolidBlue) <= 4);
}

CUTLINE_TEST(Rec709LimitedYuvDecodesToTheSameColourAsItsRgbTwin) {
  // The real test of the colour path, and the reason both fixtures exist: the
  // same generated colour is stored once as RGB and once as limited-range
  // Rec.709 YUV. Both must decode to the same thing. A wrong matrix or a missed
  // range expansion moves the YUV one and nothing else, with no constant to
  // hardcode and no dependence on what the generator actually produced.
  const auto rgb_path = Fixture("solid-rgb.mkv");
  const auto yuv_path = Fixture("solid-yuv709.mkv");
  SKIP_UNLESS(rgb_path.has_value() && yuv_path.has_value(), "solid colour fixtures not generated");
  cutline::media::RegisterAllProviders();

  auto& registry = SourceRegistry::Instance();
  auto rgb_source = registry.Open(*rgb_path);
  auto yuv_source = registry.Open(*yuv_path);
  CHECK(rgb_source != nullptr && yuv_source != nullptr);

  const auto rgb_frame = rgb_source->ReadVideo(Seconds(1));
  const auto yuv_frame = yuv_source->ReadVideo(Seconds(1));
  CHECK(rgb_frame.has_value() && yuv_frame.has_value());

  const auto expected = SampleCentre(*rgb_frame);
  const auto actual = SampleCentre(*yuv_frame);
  // Only the rounding of one YUV round trip is allowed. Treating the source as
  // bt601, or as full range, would shift these by far more.
  CHECK(std::abs(actual.r - expected.r) <= 3);
  CHECK(std::abs(actual.g - expected.g) <= 3);
  CHECK(std::abs(actual.b - expected.b) <= 3);
  // swscale normalises to full-range RGB whatever the source range was.
  CHECK(yuv_frame->color.range == model::ColorRange::Full);
  CHECK_EQ(yuv_frame->color.matrix, std::string("bt709"));
}

CUTLINE_TEST(ProbeDescribesARealFile) {
  const auto path = Fixture("av-sync.mkv");
  SKIP_UNLESS(path.has_value(), "av-sync.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  const auto probe = SourceRegistry::Instance().ProbeFile(*path);
  CHECK(probe.duration.Compare(Seconds(2)) > 0);
  CHECK(probe.duration.Compare(Seconds(4)) < 0);

  const auto* video = probe.PrimaryVideo();
  CHECK(video != nullptr);
  CHECK_EQ(video->width, std::int64_t{320});
  CHECK_EQ(video->height, std::int64_t{180});
  CHECK_EQ(video->frame_rate.numerator, std::int64_t{25});
  CHECK_EQ(video->frame_rate.denominator, std::int64_t{1});
  CHECK(!video->codec.empty());

  const auto* audio = probe.PrimaryAudio();
  CHECK(audio != nullptr);
  CHECK_EQ(audio->sample_rate, std::int64_t{48000});
  CHECK(audio->channel_count >= 1);
}

CUTLINE_TEST(FractionalFrameRatesAreReportedExactly) {
  const auto path = Fixture("bars-2997.mp4");
  SKIP_UNLESS(path.has_value(), "bars-2997.mp4 fixture not generated");
  cutline::media::RegisterAllProviders();

  const auto probe = SourceRegistry::Instance().ProbeFile(*path);
  const auto* video = probe.PrimaryVideo();
  CHECK(video != nullptr);
  // 30000/1001, not 29.97: rounding it here would put every edit a frame out
  // over a long timeline.
  CHECK_EQ(video->frame_rate.numerator, std::int64_t{30000});
  CHECK_EQ(video->frame_rate.denominator, std::int64_t{1001});
}

CUTLINE_TEST(SeekingBackwardsReturnsTheCorrectFrame) {
  // Long-GOP H.264: a seek lands on a keyframe and has to decode forward from
  // it. Reading the same time twice, with a seek in between, must agree.
  const auto path = Fixture("bars-2997.mp4");
  SKIP_UNLESS(path.has_value(), "bars-2997.mp4 fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  CHECK(source != nullptr);

  const auto first = source->ReadVideo(Seconds(3));
  CHECK(first.has_value());
  const auto early = source->ReadVideo({1, 2});
  CHECK(early.has_value());
  const auto again = source->ReadVideo(Seconds(3));
  CHECK(again.has_value());

  // The same request must decode to the same picture after a backward seek.
  const auto before = SampleCentre(*first);
  const auto after = SampleCentre(*again);
  CHECK(std::abs(before.r - after.r) <= 1);
  CHECK(std::abs(before.g - after.g) <= 1);
  CHECK(std::abs(before.b - after.b) <= 1);
}

CUTLINE_TEST(DecodedFramesCarryTheirPresentationTime) {
  const auto path = Fixture("av-sync.mkv");
  SKIP_UNLESS(path.has_value(), "av-sync.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  const auto frame = source->ReadVideo(Seconds(1));
  CHECK(frame.has_value());
  // At 25 fps the frame covering 1s starts at or just before it, and is one
  // frame long.
  CHECK(frame->presentation_time.Compare(Seconds(1)) <= 0);
  CHECK(frame->presentation_time.Add(frame->duration).Compare(Seconds(1)) > 0);
  CHECK_EQ(frame->duration.Compare({1, 25}), 0);
}

CUTLINE_TEST(ReadingPastTheEndOfAFileReturnsNothing) {
  const auto path = Fixture("solid-rgb.mkv");
  SKIP_UNLESS(path.has_value(), "solid-rgb.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  CHECK(!source->ReadVideo(Seconds(30)).has_value());
}

CUTLINE_TEST(TimestampMapIsBuiltFromRealPackets) {
  const auto path = Fixture("av-sync.mkv");
  SKIP_UNLESS(path.has_value(), "av-sync.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  const auto* map = source->timestamps();
  CHECK(map != nullptr);
  // 3 seconds at 25 fps.
  CHECK_EQ(map->size(), std::size_t{75});
  CHECK(map->cadence() == cutline::media::Cadence::Constant);
  // The map resolves a mid-frame time to the frame containing it.
  const auto& entry = map->AtOrBefore({3, 2});
  CHECK(entry.presentation_time.Compare({3, 2}) <= 0);
}

CUTLINE_TEST(DecodingStillWorksAfterTheTimestampScan) {
  // Building the map rewinds and re-reads the file, which must not leave the
  // decoder pointing somewhere unexpected.
  const auto path = Fixture("av-sync.mkv");
  SKIP_UNLESS(path.has_value(), "av-sync.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  CHECK(source->timestamps() != nullptr);
  const auto frame = source->ReadVideo(Seconds(1));
  CHECK(frame.has_value());
  CHECK_EQ(frame->width(), 320);
}

CUTLINE_TEST(AudioDecodesToTheRequestedRateAndLayout) {
  const auto path = Fixture("tone-48k.mkv");
  SKIP_UNLESS(path.has_value(), "tone-48k.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  CHECK(source != nullptr);

  // The fixture is mono; ask for stereo at a different rate and let the
  // resampler do the work, which is what the mixer will always do.
  const auto block = source->ReadAudio(Seconds(1), 44100, 2, 512);
  CHECK(block.has_value());
  CHECK_EQ(block->sample_rate(), std::int64_t{44100});
  CHECK_EQ(block->channels(), 2);
  CHECK_EQ(block->frames(), std::int64_t{512});
  // A 1 kHz tone at full scale: the block must contain real signal.
  // lavfi's sine source generates at 0.125 full scale, and swresample applies a
  // power-preserving -3 dB when upmixing mono to stereo, so the expected level
  // here is 0.125 / sqrt(2). Asserting the number rather than merely "not
  // silent" means a broken rematrix or a wrong sample-format scale is caught.
  CHECK(std::abs(block->Peak(0) - 0.125f / 1.41421356f) < 0.005f);
  CHECK(std::abs(block->Peak(1) - block->Peak(0)) < 1e-6f);
}

CUTLINE_TEST(AudioReadsAreContinuousAcrossBlocks) {
  // Two adjacent blocks must join without a gap or a repeat, or playback
  // clicks at every buffer boundary.
  const auto path = Fixture("tone-48k.mkv");
  SKIP_UNLESS(path.has_value(), "tone-48k.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  const auto first = source->ReadAudio({24000, 48000}, 48000, 1, 480);
  const auto second = source->ReadAudio({24480, 48000}, 48000, 1, 480);
  CHECK(first.has_value() && second.has_value());

  // A 1 kHz sine at 48 kHz moves at most ~0.13 per sample; a discontinuity at
  // the join would be far larger than that.
  const auto join = std::abs(second->channel(0)[0] - first->channel(0)[479]);
  CHECK(join < 0.25f);
  // Decoded at its own rate and layout, the level is the fixture's own 0.125.
  // This is the check on s16 -> float scaling: a wrong divisor shows up here.
  CHECK(std::abs(first->Peak(0) - 0.125f) < 0.002f);
}

CUTLINE_TEST(AudioAndVideoFromOneFileShareATimeline) {
  const auto path = Fixture("av-sync.mkv");
  SKIP_UNLESS(path.has_value(), "av-sync.mkv fixture not generated");
  cutline::media::RegisterAllProviders();

  auto source = SourceRegistry::Instance().Open(*path);
  const auto frame = source->ReadVideo(Seconds(2));
  const auto block = source->ReadAudio(Seconds(2), 48000, 2, 1024);
  CHECK(frame.has_value());
  CHECK(block.has_value());
  // Both report the same position on the source timeline, which is what the
  // playback engine relies on to keep them together.
  CHECK(frame->presentation_time.Compare(Seconds(2)) <= 0);
  CHECK_EQ(block->presentation_time.Compare(Seconds(2)), 0);
}

CUTLINE_TEST(OpeningSomethingThatIsNotMediaFails) {
  cutline::media::RegisterAllProviders();
  SKIP_UNLESS(cutline::media::HasFileDecoding(), "built without FFmpeg");

  // A real file that is not media: the provider claims it, then fails on open
  // rather than returning a broken source.
  const auto scratch = std::filesystem::temp_directory_path() / "cutline-not-media.txt";
  {
    std::ofstream file(scratch, std::ios::trunc);
    file << "this is not a video\n";
  }
  CHECK_THROWS(SourceRegistry::Instance().Open(scratch.string()));
  std::filesystem::remove(scratch);
}


// ------------------------------------------- demux and decode against references ----
//
// Milestone 1B. These read real files and compare what comes out with values
// that are known independently of the code under test: a frame that says which
// frame it is, and an audio signal given by a closed-form expression that never
// repeats (see cmake/Fixtures.cmake). A decoder that is one sample early, that
// dropped a packet, or that read the wrong stream cannot match them by chance.

namespace {

constexpr double kPi = 3.14159265358979323846;
// av-ref.mkv and its relatives: two channels with different chirps.
double ChirpLeft(double t) { return 0.5 * std::sin(2.0 * kPi * (200.0 * t + 1500.0 * t * t)); }
double ChirpRight(double t) { return 0.4 * std::sin(2.0 * kPi * (300.0 * t + 900.0 * t * t)); }

struct Deviation final {
  double worst{0};
  std::int64_t at{-1};
};

// Compares channel `channel` of `block`, whose first sample is absolute sample
// `first` at the block's own rate, with `signal` evaluated at the matching time.
// The signal starts `offset` seconds into the timeline and lasts `length`
// seconds; outside that the reference is silence. `guard` samples either side of
// each edge are skipped, for comparisons through a resampler whose response
// legitimately smears a step.
template <typename Signal>
[[nodiscard]] Deviation CompareToSignal(const AudioBuffer& block, int channel, std::int64_t first, Signal signal,
                                        double offset = 0.0, double length = 4.0, std::int64_t guard = 0) {
  Deviation result;
  const double rate = static_cast<double>(block.sample_rate());
  const auto start_index = static_cast<std::int64_t>(std::llround(offset * rate));
  const auto end_index = start_index + static_cast<std::int64_t>(std::llround(length * rate));
  for (std::int64_t i = 0; i < block.frames(); ++i) {
    const auto index = first + i;
    if (guard > 0 && (std::llabs(index - start_index) < guard || std::llabs(index - end_index) < guard)) continue;
    const double expected =
        index >= start_index && index < end_index ? signal(static_cast<double>(index - start_index) / rate) : 0.0;
    const double error = std::abs(static_cast<double>(block.channel(channel)[i]) - expected);
    if (error > result.worst) result = {error, index};
  }
  return result;
}

void Expect(bool condition, const std::string& description, int line) {
  if (!condition) cutline::testing::Fail("reference comparison", __FILE__, line, description);
}

[[nodiscard]] std::string Describe(const Deviation& deviation, double tolerance) {
  return "worst error " + std::to_string(deviation.worst) + " at sample " + std::to_string(deviation.at) +
         " (tolerance " + std::to_string(tolerance) + ")";
}

// The index a reference frame encodes: red = N mod 256, green = N / 256.
[[nodiscard]] int FrameIndexOf(const VideoFrame& frame) {
  const auto sample = SampleCentre(frame);
  if (sample.b != 77) return -1;
  return sample.r + 256 * sample.g;
}

[[nodiscard]] std::unique_ptr<cutline::media::Source> OpenReference(const std::string& name) {
  const auto path = Fixture(name);
  if (!path.has_value()) return nullptr;
  cutline::media::RegisterAllProviders();
  return SourceRegistry::Instance().Open(*path);
}

// Deterministic, so a failure can be reproduced exactly.
class Lcg final {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  [[nodiscard]] std::int64_t Below(std::int64_t bound) {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::int64_t>((state_ >> 33) % static_cast<std::uint64_t>(bound));
  }

 private:
  std::uint64_t state_;
};

constexpr double kSameRate = 1e-6;
// Through a resampler. The chirp sweeps up to 12 kHz, so half a sample of
// misalignment is an error of about 0.4; the measured deviation of a correct
// read is below 2e-5.
constexpr double kResampled = 1e-4;

void CheckStereoBlock(const AudioBuffer& block, std::int64_t first, double offset, int line,
                      double length = 4.0) {
  const auto left = CompareToSignal(block, 0, first, ChirpLeft, offset, length);
  const auto right = CompareToSignal(block, 1, first, ChirpRight, offset, length);
  Expect(left.worst < kSameRate, "left: " + Describe(left, kSameRate), line);
  Expect(right.worst < kSameRate, "right: " + Describe(right, kSameRate), line);
}

}  // namespace

CUTLINE_TEST(EveryFrameOfAReferenceFileIdentifiesItself) {
  auto source = OpenReference("av-ref.mkv");
  SKIP_UNLESS(source != nullptr, "av-ref.mkv fixture not generated");

  // Out of order on purpose, so most reads seek. Mid-frame and on-the-boundary
  // times both resolve to the frame that covers them.
  Lcg random(7);
  for (int step = 0; step < 120; ++step) {
    const auto frame_number = random.Below(100);
    const bool midway = random.Below(2) == 0;
    const auto time = midway ? RationalTime(2 * frame_number + 1, 50) : RationalTime(frame_number, 25);
    const auto frame = source->ReadVideo(time);
    Expect(frame.has_value(), "no frame at " + std::to_string(frame_number), __LINE__);
    Expect(FrameIndexOf(*frame) == frame_number,
           "asked for frame " + std::to_string(frame_number) + ", got " + std::to_string(FrameIndexOf(*frame)),
           __LINE__);
    CHECK_EQ(frame->presentation_time.Compare(RationalTime(frame_number, 25)), 0);
  }
  CHECK(!source->ReadVideo(Seconds(4)).has_value());
  CHECK(!source->ReadVideo({-1, 25}).has_value());
}

CUTLINE_TEST(AudioMatchesTheReferenceSignalAtAnyStartSample) {
  auto source = OpenReference("av-ref.mkv");
  SKIP_UNLESS(source != nullptr, "av-ref.mkv fixture not generated");

  // Starts that are not on any codec or container boundary, visited out of
  // order so the reader has to seek backwards as well as forwards.
  for (const std::int64_t start : {59259, 0, 1, 959, 190000, 1023, 100000, 143999, 1024, 2}) {
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 2, 960);
    Expect(block.has_value(), "no audio at sample " + std::to_string(start), __LINE__);
    CheckStereoBlock(*block, start, 0.0, __LINE__);
  }
}

CUTLINE_TEST(ConsecutiveAudioBlocksReproduceTheWholeSignal) {
  auto source = OpenReference("av-ref.mkv");
  SKIP_UNLESS(source != nullptr, "av-ref.mkv fixture not generated");

  // 1000 does not divide any codec frame size, so block edges fall anywhere.
  // The last block runs past the end of the file and must come back
  // zero-padded, not short and not repeated.
  for (std::int64_t start = 0; start < 192000; start += 1000) {
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 2, 1000);
    Expect(block.has_value(), "no audio at sample " + std::to_string(start), __LINE__);
    CheckStereoBlock(*block, start, 0.0, __LINE__);
  }
}

CUTLINE_TEST(AlternatingAudioAndVideoReadsDoNotCorruptEachOther) {
  // The playback pattern: audio is consumed block after block while video is
  // fetched, sometimes behind the audio position. With one demuxer shared by
  // both decoders, each read threw away the other stream's packets.
  auto source = OpenReference("av-ref.mkv");
  SKIP_UNLESS(source != nullptr, "av-ref.mkv fixture not generated");

  std::int64_t audio_position = 0;
  for (int step = 0; step < 36; ++step) {
    const auto block = source->ReadAudio(RationalTime(audio_position, 48000), 48000, 2, 4800);
    Expect(block.has_value(), "no audio at sample " + std::to_string(audio_position), __LINE__);
    CheckStereoBlock(*block, audio_position, 0.0, __LINE__);
    audio_position += 4800;

    const auto frame_number = (step * 7) % 100;
    const auto frame = source->ReadVideo(RationalTime(frame_number, 25));
    Expect(frame.has_value() && FrameIndexOf(*frame) == frame_number,
           "video frame " + std::to_string(frame_number) + " wrong at step " + std::to_string(step), __LINE__);
  }
}

CUTLINE_TEST(ReadOrderDoesNotChangeWhatIsRead) {
  // The strongest statement of independent reader state: a random mix of audio
  // and video reads, anywhere in the file, every one checked against the
  // reference rather than against another read.
  for (const char* name : {"av-ref.mkv", "late-audio.mkv"}) {
    auto source = OpenReference(name);
    SKIP_UNLESS(source != nullptr, "reference fixtures not generated");
    const double offset = std::string(name) == "late-audio.mkv" ? 0.5 : 0.0;
    const double length = 4.0;

    Lcg random(2026);
    for (int step = 0; step < 150; ++step) {
      if (random.Below(2) == 0) {
        const auto start = random.Below(195000);
        const auto frames = 200 + random.Below(3000);
        const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 2, frames);
        if (!block.has_value()) continue;  // wholly past the end of the file
        CheckStereoBlock(*block, start, offset, __LINE__, length);
      } else {
        const auto frame_number = random.Below(100);
        const auto frame = source->ReadVideo(RationalTime(frame_number, 25));
        Expect(frame.has_value() && FrameIndexOf(*frame) == frame_number,
               std::string(name) + ": video frame " + std::to_string(frame_number) + " wrong", __LINE__);
      }
    }
  }
}

CUTLINE_TEST(TwoSourcesOnOneFileKeepSeparateState) {
  auto first = OpenReference("av-ref.mkv");
  auto second = OpenReference("av-ref.mkv");
  SKIP_UNLESS(first != nullptr && second != nullptr, "av-ref.mkv fixture not generated");

  for (int step = 0; step < 20; ++step) {
    const std::int64_t a = step * 7000;
    const std::int64_t b = 180000 - step * 6000;
    const auto block_a = first->ReadAudio(RationalTime(a, 48000), 48000, 2, 1500);
    const auto frame_b = second->ReadVideo(RationalTime(b / 1920, 25));
    const auto block_b = second->ReadAudio(RationalTime(b, 48000), 48000, 2, 1500);
    const auto frame_a = first->ReadVideo(RationalTime(a / 1920, 25));
    Expect(block_a.has_value() && block_b.has_value() && frame_a.has_value() && frame_b.has_value(), "read failed",
           __LINE__);
    CheckStereoBlock(*block_a, a, 0.0, __LINE__);
    CheckStereoBlock(*block_b, b, 0.0, __LINE__);
    CHECK_EQ(FrameIndexOf(*frame_a), static_cast<int>(a / 1920));
    CHECK_EQ(FrameIndexOf(*frame_b), static_cast<int>(b / 1920));
  }
}

CUTLINE_TEST(AStreamThatStartsLateIsPlacedAtItsOwnStartTime) {
  // The audio begins half a second after the picture. Source time zero is the
  // container's earliest timestamp, so the first half second of audio is
  // silence and the signal's own time zero lands at 0.5 s.
  auto source = OpenReference("late-audio.mkv");
  SKIP_UNLESS(source != nullptr, "late-audio.mkv fixture not generated");

  const auto silent = source->ReadAudio({1, 4}, 48000, 2, 4800);
  CHECK(silent.has_value());
  CHECK(silent->Peak(0) == 0.0f && silent->Peak(1) == 0.0f);

  // A block that straddles the start of the audio: silence, then the signal
  // from its very first sample.
  const auto straddling = source->ReadAudio(RationalTime(23000, 48000), 48000, 2, 3000);
  CHECK(straddling.has_value());
  CheckStereoBlock(*straddling, 23000, 0.5, __LINE__, 4.0);
  CHECK(std::abs(straddling->channel(0)[1001]) > 0.0f);

  for (const std::int64_t start : {96000, 24000, 150000, 24001}) {
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 2, 960);
    CHECK(block.has_value());
    CheckStereoBlock(*block, start, 0.5, __LINE__, 4.0);
  }

  const auto frame = source->ReadVideo(Seconds(0));
  CHECK(frame.has_value() && FrameIndexOf(*frame) == 0);
  CHECK(source->probe().duration.Compare({9, 2}) >= 0);
}

CUTLINE_TEST(SourceTimeZeroIsTheFirstPictureNotTheContainersOwnClock) {
  // Both streams start 1.4 s into the container's clock. Source time zero must
  // be the first picture, and audio and video must still agree.
  auto source = OpenReference("offset.mkv");
  SKIP_UNLESS(source != nullptr, "offset.mkv fixture not generated");

  const auto first = source->ReadVideo(Seconds(0));
  Expect(first.has_value(), "no frame at source time zero", __LINE__);
  CHECK_EQ(FrameIndexOf(*first), 0);
  CHECK_EQ(first->presentation_time.Compare(Seconds(0)), 0);

  const auto later = source->ReadVideo(Seconds(2));
  CHECK(later.has_value() && FrameIndexOf(*later) == 50);
  CHECK_EQ(later->presentation_time.Compare(Seconds(2)), 0);

  for (const std::int64_t start : {0, 96000, 5, 150000}) {
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 2, 960);
    Expect(block.has_value(), "no audio", __LINE__);
    CheckStereoBlock(*block, start, 0.0, __LINE__);
  }

  // The content ends at 4 s of source time whatever the container claims.
  CHECK(!source->ReadVideo({21, 5}).has_value());
}

CUTLINE_TEST(AudioAtAnotherRateStaysAlignedWithTheSource) {
  // 44.1 kHz material read at 48 kHz. Aligned means the resampler's own delay
  // has been accounted for: the output is the same signal at the same instants,
  // to within what interpolating a rising chirp costs.
  auto source = OpenReference("chirp-44k.wav");
  SKIP_UNLESS(source != nullptr, "chirp-44k.wav fixture not generated");

  Lcg random(11);
  for (int step = 0; step < 40; ++step) {
    const auto start = 100 + random.Below(190000);
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 1, 1024);
    Expect(block.has_value(), "no audio", __LINE__);
    const auto deviation = CompareToSignal(*block, 0, start, ChirpLeft, 0.0, 4.0, 48);
    Expect(deviation.worst < kResampled, Describe(deviation, kResampled), __LINE__);
  }

  // Sequentially from the very start, which has no earlier material for the
  // resampler to draw on: the first samples must still be in place.
  const auto opening = source->ReadAudio(Seconds(0), 48000, 1, 2048);
  CHECK(opening.has_value());
  const auto head = CompareToSignal(*opening, 0, 0, ChirpLeft, 0.0, 4.0, 24);
  Expect(head.worst < kResampled, "start of file: " + Describe(head, kResampled), __LINE__);
}

CUTLINE_TEST(TheTailOfAResampledFileIsNotDiscarded) {
  // Whatever the resampler is still holding when the file ends has to be
  // flushed out. Read the whole file at the new rate and look at the end.
  auto source = OpenReference("chirp-44k.wav");
  SKIP_UNLESS(source != nullptr, "chirp-44k.wav fixture not generated");

  AudioBuffer end_block;
  for (std::int64_t start = 0; start < 192000; start += 4096) {
    auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 1, 4096);
    Expect(block.has_value(), "no audio", __LINE__);
    const auto deviation = CompareToSignal(*block, 0, start, ChirpLeft, 0.0, 4.0, 48);
    Expect(deviation.worst < kResampled, Describe(deviation, kResampled), __LINE__);
    end_block = std::move(*block);
  }
  // The final block holds samples 188416..192511; the file ends at 192000. The
  // last 48 samples are the ones a resampler is still holding when the input
  // runs out. Flushed, they follow the signal to within the filter's own edge
  // effect (worst measured 0.022, at the very last sample); discarded, they are
  // silence, which is off by up to 0.5.
  const auto last = 192000 - 188416;
  double worst_tail = 0.0;
  for (std::int64_t i = last - 48; i < last; ++i) {
    const double expected = ChirpLeft(static_cast<double>(188416 + i) / 48000.0);
    worst_tail = std::max(worst_tail, std::abs(static_cast<double>(end_block.channel(0)[i]) - expected));
  }
  Expect(worst_tail < 0.05, "tail deviates by " + std::to_string(worst_tail), __LINE__);
  // Past the end of the file there is nothing.
  CHECK(end_block.channel(0)[last + 1] == 0.0f);
}

CUTLINE_TEST(PacketsWithAHoleBetweenThemKeepTheHoleAsSilence) {
  // The audio's timestamps jump by 0.1 s part-way through: 4800 samples that the
  // file does not have. Playing on without noticing would shift everything after
  // the hole 0.1 s early against the picture.
  auto source = OpenReference("gap.mkv");
  SKIP_UNLESS(source != nullptr, "gap.mkv fixture not generated");

  constexpr std::int64_t kHoleStart = 96256;  // 94 packets of 1024 samples
  // The jump is 0.1 s (4800 samples), but the packet after it is stamped 2105 ms,
  // and a millisecond is 48 samples: the file itself says the audio resumes at
  // sample 101040, 16 earlier than the 101056 it was cut for. The container
  // cannot state a position more finely than that, so that is where the
  // reference puts it.
  constexpr std::int64_t kHole = 4784;
  const auto expected = [&](std::int64_t index) {
    if (index < kHoleStart) return ChirpLeft(static_cast<double>(index) / 48000.0);
    if (index < kHoleStart + kHole || index >= 192000 + kHole) return 0.0;
    return ChirpLeft(static_cast<double>(index - kHole) / 48000.0);
  };
  const auto check = [&](const AudioBuffer& block, std::int64_t first) {
    double worst = 0.0;
    std::int64_t where = -1;
    for (std::int64_t i = 0; i < block.frames(); ++i) {
      const double error = std::abs(static_cast<double>(block.channel(0)[i]) - expected(first + i));
      if (error > worst) {
        worst = error;
        where = first + i;
      }
    }
    Expect(worst < kSameRate, "worst error " + std::to_string(worst) + " at sample " + std::to_string(where), __LINE__);
  };

  for (std::int64_t start = 0; start < 196784; start += 1000) {
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 1, 1000);
    Expect(block.has_value(), "no audio at " + std::to_string(start), __LINE__);
    check(*block, start);
  }
  // And arriving in or after the hole by seeking.
  for (const std::int64_t start : {150000, 98000, 96000, 101000, 100000, 20000}) {
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 1, 3000);
    Expect(block.has_value(), "no audio at " + std::to_string(start), __LINE__);
    check(*block, start);
  }
}

CUTLINE_TEST(ResamplingAfterASeekIntoACoarseTimeBaseStreamIsStillAligned) {
  // 44.1 kHz audio in Matroska, whose timestamps are milliseconds (44.1 samples
  // each): the packet index places the lead-in exactly, and the lead-in starts
  // on a sample that is whole at both rates.
  auto source = OpenReference("chirp-44k.mkv");
  SKIP_UNLESS(source != nullptr, "chirp-44k.mkv fixture not generated");

  Lcg random(5);
  for (int step = 0; step < 40; ++step) {
    const auto start = 100 + random.Below(190000);
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 1, 1024);
    Expect(block.has_value(), "no audio", __LINE__);
    const auto deviation = CompareToSignal(*block, 0, start, ChirpLeft, 0.0, 4.0, 48);
    Expect(deviation.worst < kResampled, Describe(deviation, kResampled), __LINE__);
  }
}

CUTLINE_TEST(LossyAudioWithEncoderPrimingIsPlacedOnTheSampleItBelongsTo) {
  // AAC delays the signal by encoder priming that the container trims. Placed
  // correctly, the decoded block correlates best with the reference at lag 0.
  // The comparison is by correlation because a lossy codec cannot match sample
  // for sample.
  auto source = OpenReference("chirp-aac.m4a");
  SKIP_UNLESS(source != nullptr, "chirp-aac.m4a fixture not generated");

  for (const std::int64_t start : {0, 2000, 24000, 96007, 150000, 63}) {
    const auto block = source->ReadAudio(RationalTime(start, 48000), 48000, 1, 4096);
    Expect(block.has_value(), "no audio", __LINE__);

    std::int64_t best_lag = 0;
    double best_score = -2.0;
    for (std::int64_t lag = -1100; lag <= 1100; ++lag) {
      double dot = 0.0, a = 0.0, b = 0.0;
      for (std::int64_t i = 100; i < 3900; ++i) {
        const double expected = ChirpLeft(static_cast<double>(start + i + lag) / 48000.0);
        const double got = block->channel(0)[i];
        dot += expected * got;
        a += expected * expected;
        b += got * got;
      }
      const double score = dot / std::sqrt(a * b + 1e-30);
      if (score > best_score) {
        best_score = score;
        best_lag = lag;
      }
    }
    double reference_energy = 0.0;
    double error_energy = 0.0;
    for (std::int64_t i = 100; i < 3900; ++i) {
      const double expected = ChirpLeft(static_cast<double>(start + i) / 48000.0);
      const double difference = block->channel(0)[i] - expected;
      reference_energy += expected * expected;
      error_energy += difference * difference;
    }
    const double relative = std::sqrt(error_energy / reference_energy);
    Expect(best_lag == 0, "start " + std::to_string(start) + ": best lag " + std::to_string(best_lag), __LINE__);
    Expect(relative < 0.01, "start " + std::to_string(start) + ": relative error " + std::to_string(relative),
           __LINE__);
  }
}

CUTLINE_TEST(InterleavedH264AndAacKeepTheirAudioWhileTheVideoSeeks) {
  auto source = OpenReference("av-aac.mp4");
  SKIP_UNLESS(source != nullptr, "av-aac.mp4 fixture not generated");

  std::int64_t position = 0;
  for (int step = 0; step < 14; ++step) {
    const auto block = source->ReadAudio(RationalTime(position, 48000), 48000, 1, 4096);
    Expect(block.has_value(), "no audio", __LINE__);
    double dot = 0.0, a = 0.0, b = 0.0;
    for (std::int64_t i = 200; i < 3800; ++i) {
      const double expected = ChirpLeft(static_cast<double>(position + i) / 48000.0);
      dot += expected * block->channel(0)[i];
      a += expected * expected;
      b += static_cast<double>(block->channel(0)[i]) * block->channel(0)[i];
    }
    const double correlation = dot / std::sqrt(a * b + 1e-30);
    Expect(correlation > 0.999, "step " + std::to_string(step) + ": correlation " + std::to_string(correlation),
           __LINE__);
    position += 4096;
    // Pull the video somewhere unrelated, backwards as often as forwards.
    CHECK(source->ReadVideo(RationalTime((step * 37) % 100, 25)).has_value());
  }
}

CUTLINE_TEST(ScanningTimestampsDoesNotDisturbAReadInProgress) {
  auto source = OpenReference("av-ref.mkv");
  SKIP_UNLESS(source != nullptr, "av-ref.mkv fixture not generated");

  const auto before = source->ReadAudio(Seconds(0), 48000, 2, 9600);
  CHECK(before.has_value());
  CHECK(source->ReadVideo({1, 2}).has_value());

  const auto* map = source->timestamps();  // walks the file's packets
  Expect(map != nullptr, "no timestamp map", __LINE__);
  CHECK_EQ(map->size(), std::size_t{100});

  // Carries on exactly where the audio left off, and the picture is still right.
  const auto after = source->ReadAudio(RationalTime(9600, 48000), 48000, 2, 9600);
  CHECK(after.has_value());
  CheckStereoBlock(*after, 9600, 0.0, __LINE__);
  const auto frame = source->ReadVideo({3, 2});
  CHECK(frame.has_value() && FrameIndexOf(*frame) == 37);
}

CUTLINE_TEST(TimestampsAreRelativeToSourceTimeZero) {
  auto source = OpenReference("offset.mkv");
  SKIP_UNLESS(source != nullptr, "offset.mkv fixture not generated");
  const auto* map = source->timestamps();
  Expect(map != nullptr, "no timestamp map", __LINE__);
  CHECK_EQ(map->size(), std::size_t{100});
  CHECK_EQ(map->AtOrBefore(Seconds(0)).presentation_time.Compare(Seconds(0)), 0);
  CHECK_EQ(map->AtOrBefore(Seconds(2)).presentation_time.Compare(Seconds(2)), 0);
  CHECK(map->AtOrBefore(Seconds(10)).presentation_time.Compare({99, 25}) == 0);
}

CUTLINE_TEST(AudioReadsRecoverAfterReachingTheEndOfTheFile) {
  auto source = OpenReference("av-ref.mkv");
  SKIP_UNLESS(source != nullptr, "av-ref.mkv fixture not generated");

  const auto tail = source->ReadAudio(RationalTime(191000, 48000), 48000, 2, 4000);
  CHECK(tail.has_value());
  CheckStereoBlock(*tail, 191000, 0.0, __LINE__);
  const auto again = source->ReadAudio(RationalTime(48000, 48000), 48000, 2, 1000);
  CHECK(again.has_value());
  CheckStereoBlock(*again, 48000, 0.0, __LINE__);
  CHECK(!source->ReadAudio(Seconds(5), 48000, 2, 1000).has_value());
}


CUTLINE_TEST(AConstantFrameRateStoredInMillisecondsIsStillConstant) {
  // 30000/1001 fps in Matroska: frames land 33 and 34 ms apart, which is the
  // container rounding, not a variable frame rate. Comparing durations for
  // equality called every such file variable.
  auto source = OpenReference("cfr-2997.mkv");
  SKIP_UNLESS(source != nullptr, "cfr-2997.mkv fixture not generated");
  const auto* map = source->timestamps();
  Expect(map != nullptr, "no timestamp map: " + source->timestamps_problem(), __LINE__);
  CHECK_EQ(map->size(), std::size_t{60});
  CHECK(map->cadence() == cutline::media::Cadence::Constant);
}

CUTLINE_TEST(ARealVariableFrameRateFileIsReportedVariableAndIndexedByItsOwnTimes) {
  // 25 frames at 25 fps (the first second), then frames every 0.1 s.
  auto source = OpenReference("vfr.mkv");
  SKIP_UNLESS(source != nullptr, "vfr.mkv fixture not generated");
  const auto* map = source->timestamps();
  Expect(map != nullptr, "no timestamp map: " + source->timestamps_problem(), __LINE__);
  CHECK(map->cadence() == cutline::media::Cadence::Variable);
  // 0.99 s is the last fast frame (0.96 s); 1.05 s is the first slow one (1.0 s).
  CHECK_EQ(map->AtOrBefore({99, 100}).presentation_time.Compare({24, 25}), 0);
  CHECK_EQ(map->AtOrBefore({105, 100}).presentation_time.Compare({1, 1}), 0);
  CHECK_EQ(map->AtOrBefore({105, 100}).duration.Compare({1, 10}), 0);
  CHECK_EQ(map->AtOrBefore({1, 100}).duration.Compare({1, 25}), 0);
  // And the decoder lands on the frame the map names: it reads frames by time.
  const auto frame = source->ReadVideo({105, 100});
  CHECK(frame.has_value());
  CHECK_EQ(frame->presentation_time.Compare({1, 1}), 0);
  // The file says every frame lasts 40 ms; this one lasts until the next, 100 ms.
  CHECK_EQ(frame->duration.Compare({1, 10}), 0);
}

CUTLINE_TEST(DuplicateTimestampsAreReportedNotHiddenAndDecodingStillWorks) {
  auto source = OpenReference("dup-timestamps.mkv");
  SKIP_UNLESS(source != nullptr, "dup-timestamps.mkv fixture not generated");
  CHECK(source->timestamps() == nullptr);
  const auto problem = source->timestamps_problem();
  Expect(problem.find("share a presentation timestamp") != std::string::npos, "problem was: " + problem, __LINE__);
  // Not having an index does not stop the file being read.
  CHECK(source->ReadVideo({1, 5}).has_value());
}

CUTLINE_TEST(ProbeDurationIsTheContentsNotTheContainersOffsetFigure) {
  const auto seconds = [](const RationalTime& time) {
    return static_cast<double>(time.numerator()) / static_cast<double>(time.denominator());
  };
  // The container claims 5.4 s for 4 s of media that starts 1.4 s in.
  {
    auto source = OpenReference("offset.mkv");
    SKIP_UNLESS(source != nullptr, "offset.mkv fixture not generated");
    Expect(std::abs(seconds(source->probe().duration) - 4.0) < 0.01,
           "duration " + std::to_string(seconds(source->probe().duration)), __LINE__);
  }
  // Files that start at zero are left as the container states them.
  {
    auto source = OpenReference("av-ref.mkv");
    SKIP_UNLESS(source != nullptr, "av-ref.mkv fixture not generated");
    Expect(std::abs(seconds(source->probe().duration) - 4.0) < 0.01, "av-ref duration", __LINE__);
    auto late = OpenReference("late-audio.mkv");
    SKIP_UNLESS(late != nullptr, "late-audio.mkv fixture not generated");
    Expect(std::abs(seconds(late->probe().duration) - 4.5) < 0.01, "late-audio duration", __LINE__);
  }
}


// ------------------------------------------------------------------------- ingest workflow ----

namespace {

std::filesystem::path IngestScratch(const std::string& name) {
  const auto folder = std::filesystem::temp_directory_path() / ("cutline-ingest-" + name);
  std::filesystem::remove_all(folder);
  std::filesystem::create_directories(folder);
  return folder;
}

void WriteBytes(const std::filesystem::path& path, std::size_t size, unsigned seed) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  unsigned state = seed;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525u + 1013904223u;
    out.put(static_cast<char>(state >> 24));
  }
}

}  // namespace

CUTLINE_TEST(ACopyIsOnlyACopyIfReadingItBackGivesTheSameBytesAndEverythingElseLeavesNothingBehind) {
  const auto folder = IngestScratch("copy");
  const auto source = folder / "card" / "clip.mov";
  std::filesystem::create_directories(source.parent_path());
  WriteBytes(source, 9u * 1024 * 1024 + 123, 7);   // more than two blocks, and not a multiple of one
  const auto destination = folder / "project" / "media" / "clip.mov";

  std::uint64_t last_done = 0, last_total = 0;
  cutline::media::CopyOptions options;
  options.progress = [&](std::uint64_t done, std::uint64_t total) {
    CHECK(done >= last_done);
    last_done = done;
    last_total = total;
  };
  const auto good = cutline::media::CopyVerified(source, destination, options);
  CHECK(good.ok && good.verified && good.bytes == 9u * 1024 * 1024 + 123);
  CHECK(!good.source_sha256.empty() && good.source_sha256 == good.copy_sha256);
  CHECK(last_done == good.bytes && last_total == good.bytes);
  CHECK(std::filesystem::exists(destination) && !std::filesystem::exists(destination.string() + ".part"));
  CHECK(std::filesystem::file_size(destination) == good.bytes);
  // The hash is the file's own: an independent reading of the copy gives it.
  {
    std::ifstream a(source, std::ios::binary), b(destination, std::ios::binary);
    CHECK(std::equal(std::istreambuf_iterator<char>(a), {}, std::istreambuf_iterator<char>(b)));
  }

  // A name that is taken is not overwritten.
  const auto again = cutline::media::CopyVerified(source, destination, {});
  CHECK(!again.ok && !again.error.empty() && std::filesystem::file_size(destination) == good.bytes);

  // A copy that comes back different is deleted and says so.
  cutline::media::CopyOptions damaged;
  damaged.after_write = [](const std::filesystem::path& written) {
    std::fstream file(written, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(5 * 1024 * 1024);
    file.put('X');
  };
  const auto bad_destination = folder / "project" / "media" / "damaged.mov";
  const auto bad = cutline::media::CopyVerified(source, bad_destination, damaged);
  CHECK(!bad.ok && !bad.verified && bad.error.find("does not match") != std::string::npos);
  CHECK(bad.source_sha256 != bad.copy_sha256);
  CHECK(!std::filesystem::exists(bad_destination) && !std::filesystem::exists(bad_destination.string() + ".part"));
  // With verification off the same damage goes unnoticed, which is why it is on by default.
  cutline::media::CopyOptions unchecked = damaged;
  unchecked.verify = false;
  CHECK(cutline::media::CopyVerified(source, bad_destination, unchecked).ok);
  std::filesystem::remove(bad_destination);

  // Cancelling part-way leaves neither the copy nor the temporary file.
  cutline::media::CopyOptions stop;
  int polls = 0;
  stop.cancelled = [&] { return ++polls > 2; };
  const auto cancelled_destination = folder / "project" / "media" / "stopped.mov";
  const auto stopped = cutline::media::CopyVerified(source, cancelled_destination, stop);
  CHECK(stopped.cancelled && !stopped.ok);
  CHECK(!std::filesystem::exists(cancelled_destination) && !std::filesystem::exists(cancelled_destination.string() + ".part"));
  // A missing source, and an empty file (which is a real, if useless, file).
  CHECK(!cutline::media::CopyVerified(folder / "nothing.mov", folder / "project" / "media" / "nothing.mov").ok);
  WriteBytes(folder / "card" / "empty.mov", 0, 1);
  const auto empty = cutline::media::CopyVerified(folder / "card" / "empty.mov", folder / "project" / "media" / "empty.mov");
  CHECK(empty.ok && empty.bytes == 0 && std::filesystem::file_size(folder / "project" / "media" / "empty.mov") == 0);
  std::filesystem::remove_all(folder);
}

CUTLINE_TEST(AnIngestPlanNamesEveryCopyWithoutOverwritingAndSaysWhenThereIsNoRoom) {
  const auto folder = IngestScratch("plan");
  std::filesystem::create_directories(folder / "a");
  std::filesystem::create_directories(folder / "b");
  std::filesystem::create_directories(folder / "media");
  WriteBytes(folder / "a" / "shot.mov", 1000, 1);
  WriteBytes(folder / "b" / "shot.mov", 2000, 2);        // the same name from another card
  WriteBytes(folder / "media" / "shot.mov", 10, 3);      // and one already in the destination
  WriteBytes(folder / "media" / "inside.mov", 50, 4);    // a file that is already in the destination folder
  cutline::media::IngestPlanOptions options;
  options.destination_folder = folder / "media";
  options.reserve_bytes = 0;
  auto plan = cutline::media::PlanIngest({folder / "a" / "shot.mov", folder / "b" / "shot.mov", folder / "media" / "inside.mov", folder / "gone.mov"}, options);
  CHECK(plan.ok() && plan.items.size() == 4);
  CHECK(plan.items[0].destination == folder / "media" / "shot-1.mov" && plan.items[0].copy);
  CHECK(plan.items[1].destination == folder / "media" / "shot-2.mov");
  CHECK(!plan.items[2].copy && plan.items[2].destination == folder / "media" / "inside.mov");   // already there: not copied onto itself
  CHECK(!plan.items[3].problem.empty());
  CHECK_EQ(plan.bytes_to_copy, std::uint64_t{3000});
  CHECK(plan.free_bytes > 0 && plan.enough_space);
  // No room: a reserve larger than the disk.
  options.reserve_bytes = std::numeric_limits<std::uint64_t>::max() / 2;
  plan = cutline::media::PlanIngest({folder / "a" / "shot.mov"}, options);
  CHECK(!plan.ok() && !plan.enough_space && plan.problem.find("room") != std::string::npos);
  // In place: nothing is copied and the destination is the source.
  options.copy = false;
  plan = cutline::media::PlanIngest({folder / "a" / "shot.mov"}, options);
  CHECK(plan.ok() && !plan.items[0].copy && plan.items[0].destination == folder / "a" / "shot.mov" && plan.bytes_to_copy == 0);
  // Copying needs somewhere to copy to.
  cutline::media::IngestPlanOptions nowhere;
  CHECK(!cutline::media::PlanIngest({folder / "a" / "shot.mov"}, nowhere).ok());
  std::filesystem::remove_all(folder);
}

CUTLINE_TEST(TheProxyChoiceMapsToPresetsAndAutoLeavesLightMediaAlone) {
  using cutline::media::ProxyComplexity;
  ProxyComplexity hd;
  hd.width = 1920;
  hd.height = 1080;
  hd.frames_per_second = 25.0;
  hd.codec = "h264";
  ProxyComplexity uhd = hd;
  uhd.width = 3840;
  uhd.height = 2160;
  uhd.codec = "hevc";
  uhd.bit_depth = 10;
  CHECK(!cutline::media::ProxyPresetFor("none", uhd).has_value() && !cutline::media::ProxyPresetFor("", uhd).has_value() && !cutline::media::ProxyPresetFor("nonsense", uhd).has_value());
  CHECK(!cutline::media::ProxyPresetFor("auto", hd).has_value());                 // plays well as it is
  const auto heavy = cutline::media::ProxyPresetFor("auto", uhd);
  CHECK(heavy.has_value() && heavy->max_width <= 1280);                            // 10-bit HEVC UHD is made light
  CHECK(!cutline::media::ProxyPresetFor("auto", ProxyComplexity{}).has_value());   // nothing known about it, nothing assumed
  CHECK(cutline::media::ProxyPresetFor("1080", hd)->max_height == 1080 && cutline::media::ProxyPresetFor("720", hd)->max_height == 720 &&
        cutline::media::ProxyPresetFor("540", hd)->max_height == 540);
  CHECK(cutline::media::ProxyPathFor("proxies", "media-ab12").filename() == "media-ab12.mov");
  CHECK(cutline::media::ProxyPathFor("proxies", "a/b:c").filename() == "a_b_c.mov");
}

int main() { return cutline::testing::RunAll("media"); }

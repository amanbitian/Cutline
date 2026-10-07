// Audio mixer, sinks, the master clock, and SHA-256.

#include "audio/AudioCapture.h"
#include "audio/AudioClock.h"
#include "media/Providers.h"
#include "media/Source.h"
#include "audio/AudioMixer.h"
#include "audio/AudioSink.h"
#include "audio/Dsp.h"
#include "audio/TimeStretch.h"
#include "core/util/Sha256.h"
#include "tests/native/TestHarness.h"
#include "timeline/TimelineCompiler.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace model = cutline::model;
namespace rates = cutline::time;
using cutline::anim::Value;
using cutline::audio::AudioClock;
using cutline::audio::AudioMixer;
using cutline::audio::MixerConfig;
using cutline::audio::MixStatistics;
using cutline::audio::OfflineSink;
using cutline::media::AudioBuffer;
using cutline::time::RationalTime;
using cutline::timeline::Clip;
using cutline::timeline::Effect;
using cutline::timeline::Parameter;
using cutline::timeline::Sequence;
using cutline::timeline::SequenceGraph;
using cutline::timeline::TimelineCompiler;
using cutline::timeline::Track;
using cutline::timeline::Transition;

namespace {

constexpr std::int64_t kRate = 48000;
constexpr std::int64_t kBlock = 512;

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

bool Near(float actual, float expected, float tolerance = 1e-4f) { return std::abs(actual - expected) < tolerance; }

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

Track MakeTrack(std::string id, std::int64_t order) {
  Track track;
  track.id = std::move(id);
  track.kind = model::TrackKind::Audio;
  track.order = order;
  return track;
}

Sequence MakeSequence() {
  Sequence sequence;
  sequence.id = "seq-1";
  sequence.name = "Main";
  sequence.frame_rate = rates::kFrameRate25;
  sequence.width = 64;
  sequence.height = 36;
  sequence.sample_rate = kRate;
  return sequence;
}

Effect MakeEffect(std::string id, std::string type, std::vector<std::pair<std::string, Value>> parameters) {
  Effect effect;
  effect.id = std::move(id);
  effect.effect_type = std::move(type);
  for (auto& [name, value] : parameters) {
    Parameter parameter;
    parameter.id = effect.id + ":" + name;
    parameter.name = name;
    parameter.value = cutline::anim::AnimatedValue(value);
    effect.parameters.push_back(std::move(parameter));
  }
  return effect;
}

void Finalise(Sequence& sequence) {
  for (auto& track : sequence.tracks) {
    for (auto& clip : track.clips) {
      clip.start_ticks = clip.timeline_start.ToTicks();
      clip.end_ticks = clip.end().ToTicks();
    }
    for (auto& transition : track.transitions) {
      transition.start_ticks = transition.timeline_start.ToTicks();
      transition.end_ticks = transition.timeline_start.Add(transition.duration).ToTicks();
    }
  }
}

// Serves audio by media id. Two kinds of signal, both with values that say where
// they came from, so a test can state exactly which source sample it expects:
//
//   * a level: the same value at every sample, for sums and gains;
//   * a ramp: source sample s has the value s * 1e-6, silence outside
//     [0, length). It is linear, so a cubic interpolation of it is exact and a
//     retimed read can be compared with a closed-form value to a rounding error.
class Signals final {
 public:
  void Level(const std::string& media, float level) { signals_[media] = {true, level, -1}; }
  // `length` is in samples at the mixer's rate; negative means unbounded.
  void Ramp(const std::string& media, std::int64_t length = -1) { signals_[media] = {false, 0.0f, length}; }

  [[nodiscard]] static float RampValue(std::int64_t sample) { return static_cast<float>(static_cast<double>(sample) * 1e-6); }

  [[nodiscard]] cutline::audio::AudioResolver Resolver() const {
    return [this](const std::string& media, const RationalTime& time, std::int64_t sample_rate, int channels,
                  std::int64_t frames) -> std::optional<AudioBuffer> {
      const auto found = signals_.find(media);
      if (found == signals_.end()) return std::nullopt;
      ++reads;
      auto buffer = AudioBuffer::Allocate(sample_rate, channels, frames);
      const auto first = time.Rescale(sample_rate, rates::RoundingMode::Nearest);
      for (int channel = 0; channel < channels; ++channel) {
        auto* samples = buffer.channel(channel);
        for (std::int64_t n = 0; n < frames; ++n) {
          if (found->second.constant) {
            samples[n] = found->second.level;
          } else {
            const auto s = first + n;
            samples[n] = s >= 0 && (found->second.length < 0 || s < found->second.length) ? RampValue(s) : 0.0f;
          }
        }
      }
      return buffer;
    };
  }

  mutable int reads{0};

 private:
  struct Entry final {
    bool constant{};
    float level{};
    std::int64_t length{};
  };
  std::map<std::string, Entry> signals_;
};

MixerConfig Config() { return MixerConfig{kRate, 2, true}; }

SequenceGraph GraphOf(Sequence sequence) {
  Finalise(sequence);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  return graph;
}

AudioBuffer MixSamples(const SequenceGraph& graph, const Signals& signals, std::int64_t first, std::int64_t frames,
                       MixStatistics* statistics = nullptr, const cutline::timeline::CompileOptions& options = {}) {
  MixStatistics ignored;
  return AudioMixer(Config()).MixSamples(graph, first, frames, signals.Resolver(), statistics ? *statistics : ignored,
                                         options);
}

void Expect(bool condition, const std::string& description, int line) {
  if (!condition) cutline::testing::Fail("audio comparison", __FILE__, line, description);
}

// The first sample of a block at a time in whole seconds.
std::int64_t At(std::int64_t seconds) { return seconds * kRate; }

}  // namespace

// ------------------------------------------------------------- master clock ----

CUTLINE_TEST(AudioClockDerivesTimeFromRenderedSamples) {
  AudioClock clock(48000);
  CHECK_EQ(clock.Now().Compare({0, 1}), 0);
  clock.AdvanceFromAudioCallback(48000);
  CHECK_EQ(clock.Now().Compare(Seconds(1)), 0);
  clock.AdvanceFromAudioCallback(24000);
  CHECK_EQ(clock.Now().Compare({3, 2}), 0);
}

CUTLINE_TEST(AudioClockIgnoresNonPositiveAdvances) {
  AudioClock clock(48000);
  clock.AdvanceFromAudioCallback(0);
  clock.AdvanceFromAudioCallback(-100);
  CHECK_EQ(clock.Now().Compare({0, 1}), 0);
}

CUTLINE_TEST(AudioClockResetsAndReconfigures) {
  AudioClock clock(48000);
  clock.AdvanceFromAudioCallback(96000);
  CHECK_EQ(clock.Now().Compare(Seconds(2)), 0);
  clock.Reset();
  CHECK_EQ(clock.Now().Compare({0, 1}), 0);

  // A device change adopts a new rate; the same sample count is then a
  // different amount of time, which is the whole point.
  clock.Reconfigure(44100);
  CHECK_EQ(clock.sample_rate(), std::int64_t{44100});
  clock.AdvanceFromAudioCallback(44100);
  CHECK_EQ(clock.Now().Compare(Seconds(1)), 0);
  CHECK_THROWS(clock.Reconfigure(0));
}

CUTLINE_TEST(AudioClockRejectsAnImpossibleRate) {
  CHECK_THROWS(AudioClock(0));
  CHECK_THROWS(AudioClock(-48000));
}

// -------------------------------------------------------------------- mixer ----

namespace {

// A clip from `in` to `out` seconds of its media, placed at `start` seconds.
Clip PlacedClip(std::string id, std::string media, std::int64_t in, std::int64_t out, std::int64_t start) {
  auto clip = MakeClip(std::move(id), std::move(media), start, out - in);
  clip.source_in = Seconds(in);
  clip.source_out = Seconds(out);
  return clip;
}

Sequence OneTrack(Clip clip) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  return sequence;
}

Effect Animated(std::string id, std::string type, std::string name,
                std::vector<std::pair<RationalTime, double>> keys) {
  Effect effect;
  effect.id = std::move(id);
  effect.effect_type = std::move(type);
  Parameter parameter;
  parameter.id = effect.id + ":" + name;
  parameter.name = std::move(name);
  cutline::anim::AnimatedValue curve;
  for (const auto& [time, value] : keys) {
    curve.SetKeyframe({time, Value::Scalar(value), cutline::anim::Interpolation::Linear, {}, {}});
  }
  parameter.value = curve;
  effect.parameters.push_back(std::move(parameter));
  return effect;
}

}  // namespace

CUTLINE_TEST(ASingleClipPassesThroughAtUnityGain) {
  const auto graph = GraphOf(OneTrack(MakeClip("clip-1", "media-1", 0, 10)));
  Signals signals;
  signals.Level("media-1", 0.5f);

  MixStatistics statistics;
  const auto mixed = MixSamples(graph, signals, At(1), kBlock, &statistics);
  CHECK_EQ(statistics.sources_mixed, 1);
  CHECK(Near(mixed.channel(0)[0], 0.5f));
  CHECK(Near(mixed.Peak(1), 0.5f));
  CHECK(!statistics.clipped);
}

CUTLINE_TEST(TracksSumRatherThanOcclude) {
  // Unlike picture, an upper audio track does not replace a lower one.
  auto sequence = MakeSequence();
  for (const auto& [id, order] : std::vector<std::pair<std::string, std::int64_t>>{{"a1", 0}, {"a2", 1}}) {
    auto track = MakeTrack(id, order);
    track.clips.push_back(MakeClip("clip-" + id, "media-" + id, 0, 10));
    sequence.tracks.push_back(std::move(track));
  }
  Signals signals;
  signals.Level("media-a1", 0.25f);
  signals.Level("media-a2", 0.5f);

  MixStatistics statistics;
  const auto mixed = MixSamples(GraphOf(std::move(sequence)), signals, At(1), kBlock, &statistics);
  CHECK_EQ(statistics.sources_mixed, 2);
  CHECK(Near(mixed.channel(0)[0], 0.75f));
}

CUTLINE_TEST(MutedAndSoloedTracksBehaveAsTheyDoForPicture) {
  auto sequence = MakeSequence();
  auto muted = MakeTrack("a1", 0);
  muted.muted = true;
  muted.clips.push_back(MakeClip("clip-muted", "media-muted", 0, 10));
  auto audible = MakeTrack("a2", 1);
  audible.clips.push_back(MakeClip("clip-audible", "media-audible", 0, 10));
  sequence.tracks.push_back(std::move(muted));
  sequence.tracks.push_back(std::move(audible));
  Signals signals;
  signals.Level("media-muted", 0.5f);
  signals.Level("media-audible", 0.25f);
  const auto graph = GraphOf(std::move(sequence));

  CHECK(Near(MixSamples(graph, signals, At(1), kBlock).channel(0)[0], 0.25f));
  // An export that asks for every track hears the muted one too.
  cutline::timeline::CompileOptions everything;
  everything.honour_mute_and_solo = false;
  CHECK(Near(MixSamples(graph, signals, At(1), kBlock, nullptr, everything).channel(0)[0], 0.75f));
}

CUTLINE_TEST(TrackGainIsAppliedInDecibels) {
  auto sequence = OneTrack(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks[0].gain_db = -6.0;
  Signals signals;
  signals.Level("media-1", 1.0f);
  // -6 dB is a little over half.
  CHECK(Near(MixSamples(GraphOf(std::move(sequence)), signals, At(1), kBlock).channel(0)[0], 0.5012f, 1e-3f));
}

CUTLINE_TEST(TrackPanHoldsPowerAcrossTheImage) {
  auto sequence = OneTrack(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks[0].pan = -1.0;  // hard left
  Signals signals;
  signals.Level("media-1", 1.0f);
  const auto mixed = MixSamples(GraphOf(std::move(sequence)), signals, At(1), kBlock);
  CHECK(Near(mixed.channel(0)[0], 1.0f, 1e-5f));
  CHECK(Near(mixed.channel(1)[0], 0.0f, 1e-5f));
}

CUTLINE_TEST(ClipVolumeEffectIsAppliedBeforeTheTrackStrip) {
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-vol", "volume", {{"level", Value::Scalar(-6.0)}}));
  auto sequence = OneTrack(std::move(clip));
  sequence.tracks[0].gain_db = -6.0;
  Signals signals;
  signals.Level("media-1", 1.0f);
  // Two -6 dB stages compound to -12 dB.
  CHECK(Near(MixSamples(GraphOf(std::move(sequence)), signals, At(1), kBlock).channel(0)[0], 0.2512f, 1e-3f));
}

CUTLINE_TEST(AutomationIsEvaluatedAtEverySampleNotOncePerBlock) {
  // A gain that ramps from 0 to 1 over four seconds. The old mixer sampled it once
  // at the start of the block, so every sample of a block shared one value and the
  // fade moved in steps of a block. Each sample must follow the line.
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(Animated("fx-fade", "gain", "value", {{Seconds(0), 0.0}, {Seconds(4), 1.0}}));
  Signals signals;
  signals.Level("media-1", 1.0f);
  const auto graph = GraphOf(OneTrack(std::move(clip)));

  const auto mixed = MixSamples(graph, signals, At(1), 4800);
  for (std::int64_t n = 0; n < 4800; ++n) {
    const auto expected = static_cast<float>((static_cast<double>(At(1) + n)) / static_cast<double>(At(4)));
    if (std::abs(mixed.channel(0)[n] - expected) > 1e-6f) {
      cutline::testing::Fail("fade not sample-accurate", __FILE__, __LINE__, "sample " + std::to_string(n));
    }
  }
}

CUTLINE_TEST(ClipEdgesFallOnTheirOwnSample) {
  // A clip that starts 1000 samples into a block. Mixing from a plan made at the
  // block's start put the edge at a block boundary instead.
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.timeline_start = RationalTime(1000, kRate);
  auto sequence = OneTrack(std::move(clip));
  Signals signals;
  signals.Ramp("media-1");

  const auto mixed = MixSamples(GraphOf(std::move(sequence)), signals, 0, 4096);
  for (std::int64_t n = 0; n < 4096; ++n) {
    const auto expected = n < 1000 ? 0.0f : Signals::RampValue(n - 1000);
    if (mixed.channel(0)[n] != expected) {
      cutline::testing::Fail("clip edge misplaced", __FILE__, __LINE__, "sample " + std::to_string(n));
    }
  }
}

CUTLINE_TEST(ClipEndsOnItsOwnSample) {
  auto clip = MakeClip("clip-1", "media-1", 0, 1);  // one second, from zero
  auto sequence = OneTrack(std::move(clip));
  Signals signals;
  signals.Ramp("media-1");
  const auto mixed = MixSamples(GraphOf(std::move(sequence)), signals, kRate - 300, 600);
  CHECK(mixed.channel(0)[299] == Signals::RampValue(kRate - 1));
  CHECK(mixed.channel(0)[300] == 0.0f);
}

CUTLINE_TEST(AClipAtItsOwnRateIsAnExactCopyOfItsSource) {
  auto clip = PlacedClip("clip-1", "media-1", 3, 9, 2);
  auto sequence = OneTrack(std::move(clip));
  Signals signals;
  signals.Ramp("media-1");
  const auto mixed = MixSamples(GraphOf(std::move(sequence)), signals, At(2) + 7, 2000);
  // Timeline second 2 plays source second 3.
  for (std::int64_t n = 0; n < 2000; ++n) {
    if (mixed.channel(0)[n] != Signals::RampValue(At(3) + 7 + n)) {
      cutline::testing::Fail("copy is not exact", __FILE__, __LINE__, "sample " + std::to_string(n));
    }
  }
}

CUTLINE_TEST(AReversedClipPlaysItsSourceBackwards) {
  auto clip = PlacedClip("clip-1", "media-1", 1, 3, 0);
  clip.reversed = true;
  Signals signals;
  signals.Ramp("media-1");
  const auto mixed = MixSamples(GraphOf(OneTrack(std::move(clip))), signals, 0, 1000);
  // The first sample out is the last sample before the out point.
  for (std::int64_t n = 0; n < 1000; ++n) {
    if (mixed.channel(0)[n] != Signals::RampValue(At(3) - 1 - n)) {
      cutline::testing::Fail("reverse is not exact", __FILE__, __LINE__, "sample " + std::to_string(n));
    }
  }
}

CUTLINE_TEST(ARetimedClipReadsItsSourceAtItsRate) {
  // Played at 2x, 1/2x and 3/2x, forwards and reversed. A ramp is linear, so the
  // interpolated value is exact to rounding and each output sample can be compared
  // with the source position it should have read: in + n * rate, or
  // (out - 1) - n * rate.
  struct Case final {
    std::int64_t rate_numerator, rate_denominator;
    bool reversed;
  };
  for (const auto& test : {Case{2, 1, false}, Case{1, 2, false}, Case{3, 2, false}, Case{2, 1, true},
                           Case{1, 2, true}, Case{3, 2, true}, Case{1, 3, false}, Case{5, 4, true}}) {
    auto clip = PlacedClip("clip-1", "media-1", 2, 12, 1);
    clip.playback_rate = RationalTime(test.rate_numerator, test.rate_denominator);
    clip.reversed = test.reversed;
    Signals signals;
    signals.Ramp("media-1");
    const auto mixed = MixSamples(GraphOf(OneTrack(clip)), signals, At(1), 3000);

    const auto rate = static_cast<double>(test.rate_numerator) / static_cast<double>(test.rate_denominator);
    for (std::int64_t n = 0; n < 3000; ++n) {
      const auto position = test.reversed ? static_cast<double>(At(12) - 1) - static_cast<double>(n) * rate
                                          : static_cast<double>(At(2)) + static_cast<double>(n) * rate;
      const auto expected = static_cast<float>(position * 1e-6);
      if (std::abs(mixed.channel(0)[n] - expected) > 1e-6f) {
        cutline::testing::Fail("retimed read is off its source position", __FILE__, __LINE__,
                               "rate " + std::to_string(test.rate_numerator) + "/" + std::to_string(test.rate_denominator) +
                                   (test.reversed ? " reversed" : "") + ", sample " + std::to_string(n));
      }
    }
  }
}

CUTLINE_TEST(AudioFollowsTheSameSpeedRampFreezeAndReverseCurveAsVideo) {
  auto clip = PlacedClip("clip-1", "media-1", 2, 12, 0);
  Effect remap;
  remap.id = "remap";
  remap.effect_type = "time_remap";
  Parameter source;
  source.id = "remap:source";
  source.name = "source_offset";
  cutline::anim::AnimatedValue curve;
  curve.SetKeyframe({Seconds(0), Value::Scalar(0.0), cutline::anim::Interpolation::Linear, {}, {}});
  curve.SetKeyframe({Seconds(1), Value::Scalar(2.0), cutline::anim::Interpolation::Hold, {}, {}});
  curve.SetKeyframe({Seconds(2), Value::Scalar(2.0), cutline::anim::Interpolation::Linear, {}, {}});
  curve.SetKeyframe({Seconds(3), Value::Scalar(1.0), cutline::anim::Interpolation::Linear, {}, {}});
  source.value = std::move(curve);
  remap.parameters.push_back(std::move(source));
  clip.effects.push_back(std::move(remap));
  Signals signals;
  signals.Ramp("media-1");
  MixStatistics statistics;
  const auto graph = GraphOf(OneTrack(std::move(clip)));

  const auto fast = MixSamples(graph, signals, At(0) + kRate / 2, 32, &statistics);
  CHECK(std::abs(fast.channel(0)[0] - Signals::RampValue(At(3))) < 1e-6f);
  const auto frozen = MixSamples(graph, signals, At(1) + kRate / 2, 32, &statistics);
  CHECK(std::abs(frozen.channel(0)[0] - Signals::RampValue(At(4))) < 1e-6f);
  CHECK(std::abs(frozen.channel(0)[31] - frozen.channel(0)[0]) < 1e-7f);
  const auto reverse = MixSamples(graph, signals, At(2) + kRate / 2, 32, &statistics);
  CHECK(std::abs(reverse.channel(0)[0] - Signals::RampValue(At(3) + kRate / 2)) < 1e-6f);
  CHECK(reverse.channel(0)[31] < reverse.channel(0)[0]);
  CHECK_EQ(statistics.time_remap_blocks, 3);
}

CUTLINE_TEST(ARetimedClipIsAPitchShiftNotATimeStretch) {
  // The pitch policy, stated as an observation: a 1 kHz tone played at twice the
  // speed crosses zero twice as often. Zero crossings are counted directly from
  // the mixed samples, independent of the mixer's own position arithmetic.
  class Tone final {
   public:
    [[nodiscard]] cutline::audio::AudioResolver Resolver() const {
      return [](const std::string&, const RationalTime& time, std::int64_t sample_rate, int channels,
                std::int64_t frames) -> std::optional<AudioBuffer> {
        auto buffer = AudioBuffer::Allocate(sample_rate, channels, frames);
        const auto first = time.Rescale(sample_rate, rates::RoundingMode::Nearest);
        for (int channel = 0; channel < channels; ++channel) {
          for (std::int64_t n = 0; n < frames; ++n) {
            buffer.channel(channel)[n] = static_cast<float>(
                std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(first + n) / static_cast<double>(sample_rate)));
          }
        }
        return buffer;
      };
    }
  };
  const auto crossings = [](const AudioBuffer& buffer) {
    int count = 0;
    for (std::int64_t n = 1; n < buffer.frames(); ++n) {
      if ((buffer.channel(0)[n - 1] < 0.0f) != (buffer.channel(0)[n] < 0.0f)) ++count;
    }
    return count;
  };
  for (const auto& [numerator, denominator, expected] :
       std::vector<std::tuple<int, int, int>>{{1, 1, 2000}, {2, 1, 4000}, {1, 2, 1000}}) {
    auto clip = MakeClip("clip-1", "media-1", 0, 10);
    clip.playback_rate = RationalTime(numerator, denominator);
    const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(OneTrack(std::move(clip))), 0, kRate, Tone{}.Resolver());
    CHECK(std::abs(crossings(mixed) - expected) <= 2);
  }
}

CUTLINE_TEST(InterpolationBetweenSamplesIsCubicNotLinear) {
  // A 1 kHz tone at half speed. Linear interpolation of a sine this close to the
  // band edge is off by about 2e-3 at the half-sample points; four-point cubic is
  // two orders of magnitude better. The reference is the sine itself.
  const auto resolver = [](const std::string&, const RationalTime& time, std::int64_t sample_rate, int channels,
                           std::int64_t frames) -> std::optional<AudioBuffer> {
    auto buffer = AudioBuffer::Allocate(sample_rate, channels, frames);
    const auto first = time.Rescale(sample_rate, rates::RoundingMode::Nearest);
    for (int channel = 0; channel < channels; ++channel) {
      for (std::int64_t n = 0; n < frames; ++n) {
        buffer.channel(channel)[n] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(first + n) / static_cast<double>(sample_rate)));
      }
    }
    return buffer;
  };
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.playback_rate = RationalTime(1, 2);
  const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(OneTrack(std::move(clip))), 100, 2000, resolver);
  double worst = 0.0;
  for (std::int64_t n = 0; n < 2000; ++n) {
    const auto position = static_cast<double>(100 + n) * 0.5;
    const auto expected = std::sin(2.0 * 3.14159265358979323846 * 1000.0 * position / static_cast<double>(kRate));
    worst = std::max(worst, std::abs(static_cast<double>(mixed.channel(0)[n]) - expected));
  }
  Expect(worst < 2e-4, "worst interpolation error " + std::to_string(worst), __LINE__);
}

CUTLINE_TEST(BlocksOfAnySizeTileToTheSameAudioAsOneLargeBlock) {
  // The property the engine and the exporter both lean on: cutting a render into
  // blocks anywhere must not change a sample. The project is deliberately awkward:
  // retimed and reversed clips, a crossfade with handles, a nested sequence that is
  // itself retimed, animated gain, and track and sequence effects.
  auto inner = MakeSequence();
  inner.id = "inner";
  {
    auto track = MakeTrack("ia1", 0);
    track.clips.push_back(PlacedClip("inner-clip", "media-inner", 0, 8, 0));
    inner.tracks.push_back(std::move(track));
    inner.effects.push_back(MakeEffect("fx-inner", "volume", {{"level", Value::Scalar(-3.0)}}));
  }

  auto root = MakeSequence();
  {
    auto track = MakeTrack("a1", 0);
    auto first = PlacedClip("clip-a", "media-a", 0, 6, 0);
    first.effects.push_back(Animated("fx-a", "gain", "value", {{Seconds(0), 0.2}, {Seconds(5), 1.0}}));
    auto second = PlacedClip("clip-b", "media-b", 3, 9, 6);
    second.playback_rate = RationalTime(3, 2);
    track.clips.push_back(std::move(first));
    track.clips.push_back(std::move(second));
    Transition dissolve;
    dissolve.id = "t-1";
    dissolve.kind = "constant_power";
    dissolve.from_clip_id = "clip-a";
    dissolve.to_clip_id = "clip-b";
    dissolve.timeline_start = Seconds(5);
    dissolve.duration = Seconds(2);
    track.transitions.push_back(std::move(dissolve));
    root.tracks.push_back(std::move(track));

    auto reversed = MakeTrack("a2", 1);
    auto clip = PlacedClip("clip-r", "media-c", 1, 7, 2);
    clip.reversed = true;
    clip.playback_rate = RationalTime(5, 4);
    reversed.clips.push_back(std::move(clip));
    reversed.gain_db = -4.0;
    reversed.pan = 0.3;
    root.tracks.push_back(std::move(reversed));

    auto nested = MakeTrack("a3", 2);
    auto wrap = MakeClip("clip-n", "inner", 4, 4);
    wrap.source_kind = model::SourceKind::Sequence;
    wrap.source_in = Seconds(1);
    wrap.source_out = Seconds(7);
    wrap.playback_rate = RationalTime(1, 3);
    nested.clips.push_back(std::move(wrap));
    root.tracks.push_back(std::move(nested));
    root.effects.push_back(Animated("fx-master", "volume", "level", {{Seconds(0), -6.0}, {Seconds(14), 0.0}}));
  }
  SequenceGraph graph;
  Finalise(root);
  Finalise(inner);
  graph.sequences.push_back(std::move(root));
  graph.sequences.push_back(std::move(inner));

  Signals signals;
  signals.Ramp("media-a");
  signals.Ramp("media-b");
  signals.Ramp("media-c");
  signals.Ramp("media-inner");

  const std::int64_t total = At(12) + 123;
  const auto whole = MixSamples(graph, signals, 0, total);
  // Peak is well inside full scale, so the clamp stays out of the comparison.
  CHECK(whole.Peak(0) < 1.0f);

  // Block sizes chosen to land mid-clip, on edges, and one sample at a time.
  std::int64_t position = 0;
  const std::vector<std::int64_t> sizes{317, 1000, 1, 4999, 48000, 7, 960, 1920, 11111};
  std::size_t which = 0;
  double worst = 0.0;
  while (position < total) {
    const auto count = std::min(sizes[which++ % sizes.size()], total - position);
    const auto piece = MixSamples(graph, signals, position, count);
    for (int channel = 0; channel < 2; ++channel) {
      for (std::int64_t n = 0; n < count; ++n) {
        worst = std::max(worst, static_cast<double>(std::abs(piece.channel(channel)[n] - whole.channel(channel)[position + n])));
      }
    }
    position += count;
  }
  Expect(worst == 0.0, "blocks differ from one render by " + std::to_string(worst), __LINE__);
}

CUTLINE_TEST(TrackEffectsApplyInTimelineTimeToTheWholeTrack) {
  // A gain animated on the track runs on the sequence's clock, not each clip's: the
  // clip below starts two seconds in, so at timeline second 3 the track's curve
  // (0 at 0 s, 1 at 4 s) reads 0.75 where a clip-local reading would give 0.25.
  auto sequence = OneTrack(MakeClip("clip-1", "media-1", 2, 6));
  sequence.tracks[0].effects.push_back(Animated("fx-track", "gain", "value", {{Seconds(0), 0.0}, {Seconds(4), 1.0}}));
  Signals signals;
  signals.Level("media-1", 1.0f);
  CHECK(Near(MixSamples(GraphOf(std::move(sequence)), signals, At(3), 4).channel(0)[0], 0.75f, 1e-6f));
}

CUTLINE_TEST(NestedSequenceAudioIsMixed) {
  // A clip whose source is another sequence sounds like that sequence: the inner
  // clip, through the inner track's strip, at the inner time the outer clip maps to.
  auto inner = MakeSequence();
  inner.id = "inner";
  auto inner_track = MakeTrack("ia1", 0);
  inner_track.gain_db = -6.0;
  inner_track.clips.push_back(PlacedClip("inner-clip", "media-inner", 0, 10, 0));
  inner.tracks.push_back(std::move(inner_track));

  auto wrap = MakeClip("clip-n", "inner", 2, 4);
  wrap.source_kind = model::SourceKind::Sequence;
  wrap.source_in = Seconds(3);  // starts three seconds into the inner sequence
  wrap.source_out = Seconds(7);
  auto root = OneTrack(std::move(wrap));

  Finalise(root);
  Finalise(inner);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(root));
  graph.sequences.push_back(std::move(inner));

  Signals signals;
  signals.Ramp("media-inner");
  const auto mixed = MixSamples(graph, signals, At(2), 2000);

  // Outer second 2 is inner second 3, at the inner track's -6 dB.
  const auto gain = cutline::media::DecibelsToLinear(-6.0);
  for (std::int64_t n = 0; n < 2000; ++n) {
    const auto expected = Signals::RampValue(At(3) + n) * gain;
    if (std::abs(mixed.channel(0)[n] - expected) > 1e-6f) {
      cutline::testing::Fail("nested audio is wrong", __FILE__, __LINE__, "sample " + std::to_string(n));
    }
  }
  // Before the clip there is nothing.
  CHECK(MixSamples(graph, signals, At(1), 100).Peak(0) == 0.0f);
}

CUTLINE_TEST(NestedAudioFollowsTheOuterClipsRetimingAndDirection) {
  auto inner = MakeSequence();
  inner.id = "inner";
  auto inner_track = MakeTrack("ia1", 0);
  inner_track.clips.push_back(PlacedClip("inner-clip", "media-inner", 0, 20, 0));
  inner.tracks.push_back(std::move(inner_track));

  auto wrap = MakeClip("clip-n", "inner", 0, 4);
  wrap.source_kind = model::SourceKind::Sequence;
  wrap.source_in = Seconds(2);
  wrap.source_out = Seconds(10);
  wrap.playback_rate = RationalTime(2, 1);  // 8 s of inner time in 4 s
  wrap.reversed = true;
  auto root = OneTrack(std::move(wrap));
  Finalise(root);
  Finalise(inner);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(root));
  graph.sequences.push_back(std::move(inner));

  Signals signals;
  signals.Ramp("media-inner");
  const auto mixed = MixSamples(graph, signals, 0, 1500);
  for (std::int64_t n = 0; n < 1500; ++n) {
    const auto expected = Signals::RampValue(At(10) - 1 - 2 * n);
    if (std::abs(mixed.channel(0)[n] - expected) > 1e-6f) {
      cutline::testing::Fail("nested retime is wrong", __FILE__, __LINE__, "sample " + std::to_string(n));
    }
  }
}

CUTLINE_TEST(EffectsStayInsideTheSequenceTheyBelongTo) {
  // A nested sequence's own effects shape its own mix and nothing else. The old
  // compiler appended them to the outer plan, where they were applied to the whole
  // outer mix, including tracks that have nothing to do with the nested clip.
  auto inner = MakeSequence();
  inner.id = "inner";
  auto inner_track = MakeTrack("ia1", 0);
  inner_track.clips.push_back(PlacedClip("inner-clip", "media-inner", 0, 10, 0));
  inner.tracks.push_back(std::move(inner_track));
  inner.effects.push_back(MakeEffect("fx-inner", "gain", {{"value", Value::Scalar(0.5)}}));

  auto root = MakeSequence();
  auto nested_track = MakeTrack("a1", 0);
  auto wrap = MakeClip("clip-n", "inner", 0, 10);
  wrap.source_kind = model::SourceKind::Sequence;
  wrap.source_in = Seconds(0);
  wrap.source_out = Seconds(10);
  wrap.effects.push_back(MakeEffect("fx-outer-clip", "gain", {{"value", Value::Scalar(0.5)}}));
  nested_track.clips.push_back(std::move(wrap));
  auto plain_track = MakeTrack("a2", 1);
  plain_track.clips.push_back(MakeClip("clip-plain", "media-plain", 0, 10));
  root.tracks.push_back(std::move(nested_track));
  root.tracks.push_back(std::move(plain_track));
  Finalise(root);
  Finalise(inner);
  SequenceGraph graph;
  graph.sequences.push_back(std::move(root));
  graph.sequences.push_back(std::move(inner));

  Signals signals;
  signals.Level("media-inner", 0.8f);
  signals.Level("media-plain", 0.1f);
  const auto mixed = MixSamples(graph, signals, At(1), 100);
  // 0.8 through the inner sequence's 0.5 and the outer clip's 0.5, plus 0.1 untouched.
  CHECK(Near(mixed.channel(0)[0], 0.8f * 0.5f * 0.5f + 0.1f, 1e-6f));
}

CUTLINE_TEST(ANestedSequenceThatContainsItselfIsSilentNotAHang) {
  auto loop = MakeSequence();
  loop.id = "seq-1";  // same id as the root
  auto wrap = MakeClip("clip-n", "seq-1", 0, 10);
  wrap.source_kind = model::SourceKind::Sequence;
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(std::move(wrap));
  loop.tracks.push_back(std::move(track));
  Signals signals;
  const auto mixed = MixSamples(GraphOf(std::move(loop)), signals, 0, 480);
  CHECK(mixed.Peak(0) == 0.0f);
}

CUTLINE_TEST(AudioTransitionsCrossFade) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(MakeClip("clip-a", "media-1", 0, 6));
  track.clips.push_back(MakeClip("clip-b", "media-2", 6, 6));
  Transition dissolve;
  dissolve.id = "t-1";
  dissolve.kind = "constant_gain";
  dissolve.from_clip_id = "clip-a";
  dissolve.to_clip_id = "clip-b";
  dissolve.timeline_start = Seconds(5);
  dissolve.duration = Seconds(2);
  track.transitions.push_back(std::move(dissolve));
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));

  Signals signals;
  signals.Level("media-1", 1.0f);
  signals.Level("media-2", 0.0f);

  // At the start of the dissolve the outgoing clip is still at full level.
  CHECK(Near(MixSamples(graph, signals, At(5), kBlock).channel(0)[0], 1.0f));
  // Halfway, half of it remains, and the ramp is per sample: it has already moved
  // within one block.
  MixStatistics statistics;
  const auto middle = MixSamples(graph, signals, At(6), kBlock, &statistics);
  CHECK(Near(middle.channel(0)[0], 0.5f));
  CHECK(Near(middle.channel(0)[kBlock - 1], 0.5f - static_cast<float>(kBlock - 1) / (2.0f * kRate), 1e-6f));
  CHECK_EQ(statistics.transitions_mixed, 1);
  // And it is over by the end.
  CHECK(Near(MixSamples(graph, signals, At(7), kBlock).channel(0)[0], 0.0f));
}
CUTLINE_TEST(ASequenceMadeUnderTheOriginalRulesStillCrossfadesConstantPowerLinearly) {
  // Render version 1 is what every earlier project was made with, and in it a
  // transition named constant_power was a linear ramp. Fixing that for new work must
  // not change the sound of work that was approved with it.
  const auto build = [](std::int64_t version) {
    auto sequence = MakeSequence();
    sequence.render_version = version;
    auto track = MakeTrack("a1", 0);
    track.clips.push_back(MakeClip("clip-a", "media-1", 0, 6));
    track.clips.push_back(MakeClip("clip-b", "media-2", 6, 6));
    Transition dissolve;
    dissolve.id = "t-1";
    dissolve.kind = "constant_power";
    dissolve.from_clip_id = "clip-a";
    dissolve.to_clip_id = "clip-b";
    dissolve.timeline_start = Seconds(5);
    dissolve.duration = Seconds(2);
    track.transitions.push_back(std::move(dissolve));
    sequence.tracks.push_back(std::move(track));
    return GraphOf(std::move(sequence));
  };
  Signals from_only;
  from_only.Level("media-1", 1.0f);
  from_only.Level("media-2", 0.0f);
  CHECK(Near(MixSamples(build(cutline::model::kLegacyRenderVersion), from_only, At(6), 8).channel(0)[0], 0.5f, 1e-5f));
  CHECK(Near(MixSamples(build(cutline::model::kCurrentRenderVersion), from_only, At(6), 8).channel(0)[0], 0.70710678f, 1e-5f));
}


CUTLINE_TEST(AConstantPowerTransitionHoldsLoudnessAcrossTheCut) {
  // Two uncorrelated-power sources of equal level: a constant-power fade sums their
  // squared weights to one, so at the middle each is cos(pi/4).
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(MakeClip("clip-a", "media-1", 0, 6));
  track.clips.push_back(MakeClip("clip-b", "media-2", 6, 6));
  Transition dissolve;
  dissolve.id = "t-1";
  dissolve.kind = "constant_power";
  dissolve.from_clip_id = "clip-a";
  dissolve.to_clip_id = "clip-b";
  dissolve.timeline_start = Seconds(5);
  dissolve.duration = Seconds(2);
  track.transitions.push_back(std::move(dissolve));
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));

  Signals from_only;
  from_only.Level("media-1", 1.0f);
  from_only.Level("media-2", 0.0f);
  Signals to_only;
  to_only.Level("media-1", 0.0f);
  to_only.Level("media-2", 1.0f);
  const auto from = MixSamples(graph, from_only, At(6), 8).channel(0)[0];
  const auto to = MixSamples(graph, to_only, At(6), 8).channel(0)[0];
  CHECK(Near(from, 0.70710678f, 1e-5f));
  CHECK(Near(to, 0.70710678f, 1e-5f));
  CHECK(Near(from * from + to * to, 1.0f, 1e-5f));
}

CUTLINE_TEST(ACrossfadePlaysTheMediaOnEitherSideOfTheCut) {
  // The outgoing clip keeps playing past its out point and the incoming one began
  // before its in point: those samples are real media (the clips' handles), not a
  // held edge. Ramps tell the two apart where levels could not.
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(PlacedClip("clip-a", "media-a", 0, 6, 0));   // plays media-a 0..6 s
  track.clips.push_back(PlacedClip("clip-b", "media-b", 3, 9, 6));   // plays media-b 3..9 s from 6 s on
  Transition dissolve;
  dissolve.id = "t-1";
  dissolve.kind = "constant_gain";
  dissolve.from_clip_id = "clip-a";
  dissolve.to_clip_id = "clip-b";
  dissolve.timeline_start = Seconds(5);
  dissolve.duration = Seconds(2);
  track.transitions.push_back(std::move(dissolve));
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));

  Signals signals;
  signals.Ramp("media-a");
  signals.Ramp("media-b");

  // Timeline 5.5 s: 1/4 of the way through. media-a at 5.5 s (still in the clip),
  // media-b at 2.5 s, which is before its in point.
  const auto before_cut = MixSamples(graph, signals, At(5) + kRate / 2, 4).channel(0)[0];
  const auto expected_before = 0.75f * Signals::RampValue(At(5) + kRate / 2) + 0.25f * Signals::RampValue(At(3) - kRate / 2);
  CHECK(Near(before_cut, expected_before, 1e-5f));
  // Timeline 6.5 s: 3/4 through. media-a at 6.5 s, past its out point at 6 s;
  // media-b at 3.5 s.
  const auto after_cut = MixSamples(graph, signals, At(6) + kRate / 2, 4).channel(0)[0];
  const auto expected_after = 0.25f * Signals::RampValue(At(6) + kRate / 2) + 0.75f * Signals::RampValue(At(3) + kRate / 2);
  CHECK(Near(after_cut, expected_after, 1e-5f));
  // Once the dissolve ends only the incoming clip is left.
  CHECK(MixSamples(graph, signals, At(7), 4).channel(0)[0] == Signals::RampValue(At(4)));
}

CUTLINE_TEST(WhereTheMediaHasNoHandleTheCrossfadeIsSilenceNotAHeldSample) {
  // media-a is exactly six seconds long: the outgoing clip has no handle, so past
  // the cut it contributes nothing rather than repeating its last sample.
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(PlacedClip("clip-a", "media-a", 0, 6, 0));
  track.clips.push_back(PlacedClip("clip-b", "media-b", 0, 6, 6));
  Transition dissolve;
  dissolve.id = "t-1";
  dissolve.kind = "constant_gain";
  dissolve.from_clip_id = "clip-a";
  dissolve.to_clip_id = "clip-b";
  dissolve.timeline_start = Seconds(5);
  dissolve.duration = Seconds(2);
  track.transitions.push_back(std::move(dissolve));
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));

  Signals signals;
  signals.Ramp("media-a", At(6));
  signals.Ramp("media-b", At(6));
  const auto after_cut = MixSamples(graph, signals, At(6) + kRate / 2, 4).channel(0)[0];
  // media-a has run out; media-b is at 0.5 s with weight 3/4.
  CHECK(Near(after_cut, 0.75f * Signals::RampValue(kRate / 2), 1e-5f));
}

CUTLINE_TEST(AFadeOnOneClipHasNoHandle) {
  // A fade-in has only the incoming side. It ramps the clip's own audio and does
  // not invent media before the clip.
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(PlacedClip("clip-b", "media-b", 2, 9, 4));
  Transition fade;
  fade.id = "t-fade";
  fade.kind = "constant_gain";
  fade.to_clip_id = "clip-b";
  fade.timeline_start = Seconds(4);
  fade.duration = Seconds(2);
  track.transitions.push_back(std::move(fade));
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));

  Signals signals;
  signals.Ramp("media-b");
  CHECK(MixSamples(graph, signals, At(3), 100).Peak(0) == 0.0f);  // nothing before the clip
  const auto halfway = MixSamples(graph, signals, At(5), 4).channel(0)[0];
  CHECK(Near(halfway, 0.5f * Signals::RampValue(At(3)), 1e-5f));
}

CUTLINE_TEST(MissingSourcesAreSilenceNotFailure) {
  const auto graph = GraphOf(OneTrack(MakeClip("clip-offline", "media-missing", 0, 10)));
  MixStatistics statistics;
  const auto mixed = MixSamples(graph, Signals{}, At(1), kBlock, &statistics);
  CHECK_EQ(statistics.missing_sources, 1);
  CHECK_EQ(statistics.sources_mixed, 0);
  CHECK(Near(mixed.Peak(0), 0.0f));
}

CUTLINE_TEST(ADisabledClipIsSilent) {
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.enabled = false;
  Signals signals;
  signals.Level("media-1", 0.5f);
  CHECK(MixSamples(GraphOf(OneTrack(std::move(clip))), signals, At(1), kBlock).Peak(0) == 0.0f);
}

CUTLINE_TEST(AnOverloadedMixIsClampedAndReported) {
  auto sequence = MakeSequence();
  Signals signals;
  for (int index = 0; index < 3; ++index) {
    auto track = MakeTrack("a" + std::to_string(index), index);
    track.clips.push_back(MakeClip("clip-" + std::to_string(index), "media-" + std::to_string(index), 0, 10));
    sequence.tracks.push_back(std::move(track));
    signals.Level("media-" + std::to_string(index), 0.5f);
  }
  MixStatistics statistics;
  const auto mixed = MixSamples(GraphOf(std::move(sequence)), signals, At(1), kBlock, &statistics);
  // Three half-scale sources sum to 1.5, which must be clamped rather than allowed
  // to wrap. It is a clamp, not a limiter, and says so.
  CHECK(statistics.clipped);
  CHECK(Near(mixed.channel(0)[0], 1.0f));
  CHECK(statistics.peak > 1.0f);
}

CUTLINE_TEST(PictureEffectsOnAudioClipsAreNotReportedAsSkipped) {
  // A linked A/V clip carries the picture's effects too. Those are not ours to
  // apply, but they are not unknown either, and reporting them would make the
  // skipped list useless.
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-motion", "motion", {{"scale", Value::Vec2(50.0, 50.0)}}));
  clip.effects.push_back(MakeEffect("fx-unknown", "convolution_reverb", {{"mix", Value::Scalar(0.3)}}));
  Signals signals;
  signals.Level("media-1", 1.0f);

  MixStatistics statistics;
  const auto mixed = MixSamples(GraphOf(OneTrack(std::move(clip))), signals, At(1), kBlock, &statistics);
  CHECK_EQ(statistics.skipped_effects.size(), std::size_t{1});
  CHECK_EQ(statistics.skipped_effects[0], std::string("convolution_reverb"));
  CHECK(Near(mixed.channel(0)[0], 1.0f));
  CHECK(cutline::audio::IsBuiltInAudioEffect("volume"));
  CHECK(!cutline::audio::IsBuiltInAudioEffect("convolution_reverb"));
}

CUTLINE_TEST(MixingALongTimelineReadsOnlyTheClipsInTheBlock) {
  // Ten thousand one-second clips on one track: a block near the end reads one or
  // two of them. The earlier mixer scanned every clip and transition in the project
  // for every block.
  auto track = MakeTrack("a1", 0);
  for (int index = 0; index < 10000; ++index) {
    track.clips.push_back(MakeClip("clip-" + std::to_string(index), "media-1", index, 1));
  }
  auto sequence = MakeSequence();
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));

  Signals signals;
  signals.Level("media-1", 0.25f);
  MixStatistics statistics;
  const auto mixed = MixSamples(graph, signals, At(9000) - 100, 200, &statistics);
  CHECK_EQ(statistics.sources_mixed, 2);
  CHECK_EQ(signals.reads, 2);
  CHECK(Near(mixed.channel(0)[0], 0.25f));
  CHECK(Near(mixed.channel(0)[199], 0.25f));
}

// --------------------------------------------------------------------- sink ----

CUTLINE_TEST(TheOfflineSinkPullsBlocksAndAdvancesTheClock) {
  OfflineSink sink({kRate, 2, 256});
  int calls = 0;
  sink.Start([&calls](AudioBuffer& buffer) -> cutline::audio::BlockResult {
    ++calls;
    for (int channel = 0; channel < buffer.channels(); ++channel) {
      for (std::int64_t frame = 0; frame < buffer.frames(); ++frame) buffer.channel(channel)[frame] = 0.25f;
    }
    return calls < 4 ? cutline::audio::BlockResult::Audio : cutline::audio::BlockResult::End;
  });
  CHECK(sink.running());

  const auto rendered = sink.RenderBlocks(10);
  // The callback reports exhaustion on its fourth call, so four blocks carried
  // audio and pulling stops there.
  CHECK_EQ(rendered, 3);
  // The clock counts every block the device consumed, including the last.
  CHECK_EQ(sink.clock().Now().Compare({256 * 4, kRate}), 0);
  sink.Stop();
  CHECK(!sink.running());
}

CUTLINE_TEST(ASinkNeedsARenderCallback) {
  OfflineSink sink;
  CHECK_THROWS(sink.Start(nullptr));
}

CUTLINE_TEST(OpeningTheDefaultSinkAlwaysYieldsSomething) {
  // A machine with no sound card must still open the editor; playback is simply
  // silent. The call therefore never returns null.
  auto sink = cutline::audio::OpenDefaultAudioSink({kRate, 2, 256});
  CHECK(sink != nullptr);
  CHECK(!sink->name().empty());
  CHECK(sink->format().sample_rate > 0);
  CHECK_EQ(sink->underruns(), std::int64_t{0});
}

// ------------------------------------------------------------------ sha-256 ----

CUTLINE_TEST(Sha256MatchesTheKnownAnswers) {
  // The standard test vectors. A hash that is subtly wrong would still look
  // like a hash, so these are not optional.
  CHECK_EQ(cutline::util::Sha256::Of(""),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(cutline::util::Sha256::Of("abc"),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(cutline::util::Sha256::Of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

CUTLINE_TEST(Sha256HandlesTheBlockBoundary) {
  // 55, 56, and 64 bytes exercise the three padding cases.
  CHECK_EQ(cutline::util::Sha256::Of(std::string(55, 'a')),
           std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
  CHECK_EQ(cutline::util::Sha256::Of(std::string(56, 'a')),
           std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
  CHECK_EQ(cutline::util::Sha256::Of(std::string(64, 'a')),
           std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
}

CUTLINE_TEST(Sha256RefusesToBeReused) {
  cutline::util::Sha256 hash;
  hash.Update("data");
  const auto digest = hash.HexDigest();
  CHECK_EQ(digest.size(), std::size_t{64});
  CHECK_THROWS(hash.Update("more"));
  CHECK_THROWS(hash.HexDigest());
}

// ----------------------------------------------- pitch-preserving time stretch ----
//
// A retimed clip can keep its pitch (WSOLA time-stretch) instead of changing it
// (varispeed). The measurements below are independent of the code under test: zero
// crossings counted from the mixed samples, energy at the tone's own frequency, and
// short-time level.

namespace {

// A signal of sines at the given frequencies (equal amplitude, 0.4 in total), as a
// function of the source sample number, so any read of it is exact.
cutline::audio::AudioResolver PartialsResolver(std::vector<double> frequencies) {
  return [frequencies = std::move(frequencies)](const std::string&, const RationalTime& time, std::int64_t sample_rate,
                                                 int channels, std::int64_t frames) -> std::optional<AudioBuffer> {
    auto buffer = AudioBuffer::Allocate(sample_rate, channels, frames);
    const auto first = time.Rescale(sample_rate, rates::RoundingMode::Nearest);
    for (int channel = 0; channel < channels; ++channel) {
      for (std::int64_t n = 0; n < frames; ++n) {
        double value = 0.0;
        for (const auto frequency : frequencies) {
          value += std::sin(2.0 * 3.14159265358979323846 * frequency * static_cast<double>(first + n) / static_cast<double>(sample_rate));
        }
        buffer.channel(channel)[n] = static_cast<float>(0.4 * value / static_cast<double>(frequencies.size()));
      }
    }
    return buffer;
  };
}

// Mixes a ten-second clip of the signal at `speed` (keeping pitch or not) and
// returns the first `seconds` of output.
AudioBuffer MixStretched(const std::vector<double>& frequencies, double speed, bool keep_pitch, std::int64_t frames,
                         bool reversed = false, bool offline = false, MixStatistics* statistics = nullptr) {
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.playback_rate = RationalTime(static_cast<std::int64_t>(speed * 1000.0 + 0.5), 1000);
  clip.maintain_pitch = keep_pitch;
  clip.reversed = reversed;
  cutline::timeline::CompileOptions options;
  options.high_quality_audio = offline;
  MixStatistics local;
  return AudioMixer(Config()).MixSamples(GraphOf(OneTrack(std::move(clip))), 0, frames, PartialsResolver(frequencies),
                                         statistics != nullptr ? *statistics : local, options);
}

// Zero crossings per second of a mono channel over [from, to).
double CrossingsPerSecond(const AudioBuffer& buffer, std::int64_t from, std::int64_t to) {
  int count = 0;
  for (std::int64_t n = from + 1; n < to; ++n) {
    if ((buffer.channel(0)[n - 1] < 0.0f) != (buffer.channel(0)[n] < 0.0f)) ++count;
  }
  return static_cast<double>(count) / (static_cast<double>(to - from) / static_cast<double>(kRate));
}

// The fraction of the energy in [from, to) that is at `frequency` (Goertzel).
double EnergyFractionAt(const AudioBuffer& buffer, std::int64_t from, std::int64_t to, double frequency) {
  double real = 0.0, imaginary = 0.0, total = 0.0;
  for (std::int64_t n = from; n < to; ++n) {
    const auto angle = 2.0 * 3.14159265358979323846 * frequency * static_cast<double>(n) / static_cast<double>(kRate);
    const double sample = buffer.channel(0)[n];
    real += sample * std::cos(angle);
    imaginary += sample * std::sin(angle);
    total += sample * sample;
  }
  const double window = static_cast<double>(to - from);
  const double tone_energy = 2.0 * (real * real + imaginary * imaginary) / window;  // energy of the sinusoid at that frequency
  return total > 0.0 ? tone_energy / total : 0.0;
}

double RmsOf(const AudioBuffer& buffer, std::int64_t from, std::int64_t to) {
  double sum = 0.0;
  for (std::int64_t n = from; n < to; ++n) sum += static_cast<double>(buffer.channel(0)[n]) * buffer.channel(0)[n];
  return std::sqrt(sum / static_cast<double>(to - from));
}

}  // namespace

CUTLINE_TEST(AStretchedClipKeepsItsPitchAtEverySpeedWhileVarispeedChangesIt) {
  // 1 kHz crosses zero 2000 times a second. At 2x, varispeed crosses 4000 times; a
  // stretch keeps 2000. Measured over the middle of the output, away from the edges.
  for (const double speed : {0.5, 0.75, 1.25, 1.5, 2.0, 3.0}) {
    const auto frames = static_cast<std::int64_t>(2.5 * kRate / speed);
    const auto stretched = MixStretched({1000.0}, speed, true, frames);
    const auto from = frames / 5;
    const auto to = frames * 4 / 5;
    const auto stretched_rate = CrossingsPerSecond(stretched, from, to);
    Expect(std::abs(stretched_rate - 2000.0) < 20.0,
           "speed " + std::to_string(speed) + ": " + std::to_string(stretched_rate) + " crossings per second", __LINE__);
    const auto varispeed = MixStretched({1000.0}, speed, false, frames);
    const auto varispeed_rate = CrossingsPerSecond(varispeed, from, to);
    Expect(std::abs(varispeed_rate - 2000.0 * speed) < 20.0 * speed + 5.0,
           "varispeed at " + std::to_string(speed) + ": " + std::to_string(varispeed_rate), __LINE__);
  }
}

CUTLINE_TEST(AStretchedClipsDurationIsExactAndItIsSilentAfterward) {
  for (const double speed : {0.5, 2.0}) {
    auto clip = MakeClip("clip-1", "media-1", 0, 4);  // four seconds of source
    clip.playback_rate = RationalTime(static_cast<std::int64_t>(speed * 1000), 1000);
    clip.maintain_pitch = true;
    const auto expected = static_cast<std::int64_t>(4.0 * kRate / speed);
    const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(OneTrack(std::move(clip))), 0, expected + 4000,
                                                       PartialsResolver({1000.0}));
    // Sound right up to the last few milliseconds, and nothing from the end on.
    Expect(RmsOf(mixed, expected - 960 - 480, expected - 480) > 0.15, "the clip stops early at " + std::to_string(speed), __LINE__);
    CHECK(RmsOf(mixed, expected, expected + 4000) == 0.0);
  }
}

CUTLINE_TEST(AStretchedToneIsStillATone) {
  // Artifact measures on a pure tone and on a chord of two partials, for both
  // quality settings: the energy stays at the original frequencies (the fraction of
  // the output that is *not* the source's own partials stays small) and the level
  // does not pump from window to window.
  struct Fixture final {
    std::vector<double> frequencies;
    double speed;
    bool offline;
  };
  for (const auto& fixture : {Fixture{{1000.0}, 2.0, false}, Fixture{{1000.0}, 0.5, false}, Fixture{{1000.0}, 1.5, true},
                              Fixture{{440.0, 660.0}, 1.25, false}, Fixture{{440.0, 660.0}, 0.8, true}}) {
    const auto frames = static_cast<std::int64_t>(3.0 * kRate / fixture.speed);
    const auto mixed = MixStretched(fixture.frequencies, fixture.speed, true, frames, false, fixture.offline);
    const auto from = frames / 5;
    const auto to = frames * 4 / 5;

    // Locally, in 50 ms windows (50 cycles of a kilohertz), the energy is at the
    // source's own frequencies. Measured over the whole run the fraction would
    // also include the phase steps at chain seams, which are a different thing and
    // are bounded separately below.
    std::vector<double> purity;
    std::vector<double> levels;
    const auto reference = RmsOf(mixed, from, to);
    for (auto start = from; start + 2400 < to; start += 2400) {
      double in_partials = 0.0;
      for (const auto frequency : fixture.frequencies) in_partials += EnergyFractionAt(mixed, start, start + 2400, frequency);
      purity.push_back(in_partials);
      levels.push_back(RmsOf(mixed, start, start + 2400) / reference);
    }
    std::sort(purity.begin(), purity.end());
    std::sort(levels.begin(), levels.end());
    const auto where = std::string(" (speed ") + std::to_string(fixture.speed) + (fixture.offline ? ", offline)" : ", preview)");
    // Typical windows are clean; the worst few (one in a few dozen holds a seam) may not be.
    Expect(purity[purity.size() / 2] > 0.95, "median window purity " + std::to_string(purity[purity.size() / 2]) + where, __LINE__);
    Expect(purity[purity.size() / 10] > 0.80, "tenth-percentile window purity " + std::to_string(purity[purity.size() / 10]) + where, __LINE__);
    // The level does not pump: nearly every window is within 15% of the average.
    Expect(levels[levels.size() / 10] > 0.85 && levels[levels.size() * 9 / 10] < 1.15,
           "level range " + std::to_string(levels[levels.size() / 10]) + " to " + std::to_string(levels[levels.size() * 9 / 10]) + where, __LINE__);
    // A seam may dip the level, but never to silence.
    Expect(levels.front() > 0.3, "worst window level " + std::to_string(levels.front()) + where, __LINE__);
  }
}

CUTLINE_TEST(AReversedStretchedClipKeepsItsPitchToo) {
  const auto frames = static_cast<std::int64_t>(2.0 * kRate / 1.5);
  const auto mixed = MixStretched({1000.0}, 1.5, true, frames, true);
  Expect(std::abs(CrossingsPerSecond(mixed, frames / 5, frames * 4 / 5) - 2000.0) < 20.0, "reversed pitch", __LINE__);
}

CUTLINE_TEST(StretchedAudioIsTheSameWhateverTheBlockSizes) {
  // The property the engine and the exporter lean on, for stretched audio too: the
  // result does not depend on how the render is cut into blocks, with or without a
  // shared cache.
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.playback_rate = RationalTime(3, 2);
  clip.maintain_pitch = true;
  const auto graph = GraphOf(OneTrack(std::move(clip)));
  const auto resolver = PartialsResolver({440.0, 1250.0});
  constexpr std::int64_t kTotal = 3 * kRate;

  for (const bool shared_cache : {false, true}) {
    auto config = Config();
    if (shared_cache) config.stretch_cache = std::make_shared<cutline::audio::StretchCache>();
    const AudioMixer mixer(config);
    const auto whole = mixer.MixSamples(graph, 0, kTotal, resolver);
    std::int64_t position = 0;
    const std::vector<std::int64_t> sizes{317, 1000, 1, 4999, 48000, 7, 960, 11111};
    std::size_t which = 0;
    double worst = 0.0;
    while (position < kTotal) {
      const auto count = std::min(sizes[which++ % sizes.size()], kTotal - position);
      const auto piece = mixer.MixSamples(graph, position, count, resolver);
      for (std::int64_t n = 0; n < count; ++n) {
        worst = std::max(worst, static_cast<double>(std::abs(piece.channel(0)[n] - whole.channel(0)[position + n])));
      }
      position += count;
    }
    Expect(worst == 0.0, std::string(shared_cache ? "with" : "without") + " a shared cache, blocks differ by " + std::to_string(worst), __LINE__);
  }
}

CUTLINE_TEST(StretchedChainsAreComputedOnceAndThenServedFromTheCache) {
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.playback_rate = RationalTime(2, 1);
  clip.maintain_pitch = true;
  const auto graph = GraphOf(OneTrack(std::move(clip)));
  auto config = Config();
  config.stretch_cache = std::make_shared<cutline::audio::StretchCache>();
  const AudioMixer mixer(config);
  const auto resolver = PartialsResolver({1000.0});

  MixStatistics first;
  const auto a = mixer.MixSamples(graph, 20000, 1024, resolver, first);
  CHECK(first.stretch_chains_computed >= 1);
  MixStatistics second;
  const auto b = mixer.MixSamples(graph, 21024, 1024, resolver, second);
  // The next block is in the same chain: nothing is recomputed.
  CHECK_EQ(second.stretch_chains_computed, 0);
  CHECK(second.stretch_cache_hits >= 1);
  CHECK(a.Peak(0) > 0.1f && b.Peak(0) > 0.1f);
  CHECK(config.stretch_cache->bytes() > 0);
}

CUTLINE_TEST(ASpeedTooExtremeToStretchFallsBackToVarispeedAndSaysSo) {
  MixStatistics statistics;
  const auto fallback = MixStretched({1000.0}, 8.0, true, 4000, false, false, &statistics);
  CHECK_EQ(statistics.stretch_fallbacks, 1);
  CHECK_EQ(statistics.stretch_chains_computed, 0);
  const auto varispeed = MixStretched({1000.0}, 8.0, false, 4000);
  double worst = 0.0;
  for (std::int64_t n = 0; n < 4000; ++n) worst = std::max(worst, static_cast<double>(std::abs(fallback.channel(0)[n] - varispeed.channel(0)[n])));
  CHECK(worst == 0.0);
  // And a clip that did not ask for it is never stretched.
  MixStatistics plain;
  const auto untouched = MixStretched({1000.0}, 2.0, false, 4000, false, false, &plain);
  CHECK_EQ(plain.stretch_chains_computed, 0);
  CHECK_EQ(plain.stretch_fallbacks, 0);
  (void)untouched;
}

CUTLINE_TEST(StretchingWithWholeSignalsHelperHasExactLengthAndKeepsAmplitude) {
  auto signal = AudioBuffer::Allocate(kRate, 2, kRate);
  for (std::int64_t n = 0; n < signal.frames(); ++n) {
    signal.channel(0)[n] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 300.0 * static_cast<double>(n) / kRate));
    signal.channel(1)[n] = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 500.0 * static_cast<double>(n) / kRate));
  }
  const auto stretched = cutline::audio::Stretch(signal, 0.5, 2 * kRate, cutline::audio::PreviewStretch());
  CHECK_EQ(stretched.frames(), static_cast<std::int64_t>(2 * kRate));
  CHECK_EQ(stretched.channels(), 2);
  // Each channel keeps its own signal at its own level.
  CHECK(std::abs(RmsOf(stretched, kRate / 2, 3 * kRate / 2) - 0.5 / std::sqrt(2.0)) < 0.03);
  CHECK(stretched.Peak(1) < 0.30f && stretched.Peak(1) > 0.2f);
}

// ------------------------------------------------------------ dsp processors ----
//
// Each processor is checked against what it is specified to do, measured from its
// output: a response curve, a static curve, a step response, a guarantee. None of the
// expected values is read back from the code under test.

namespace {

namespace dsp = cutline::audio::dsp;

AudioBuffer SineBuffer(double frequency, double amplitude, double seconds, int channels = 1, std::int64_t rate = kRate) {
  auto buffer = AudioBuffer::Allocate(rate, channels, static_cast<std::int64_t>(seconds * static_cast<double>(rate)));
  for (int channel = 0; channel < channels; ++channel) {
    for (std::int64_t n = 0; n < buffer.frames(); ++n) {
      buffer.channel(channel)[n] = static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979323846 * frequency * static_cast<double>(n) / static_cast<double>(rate)));
    }
  }
  return buffer;
}

// Deterministic white noise in [-amplitude, amplitude].
AudioBuffer NoiseBuffer(double amplitude, double seconds, std::uint64_t seed, int channels = 1) {
  auto buffer = AudioBuffer::Allocate(kRate, channels, static_cast<std::int64_t>(seconds * kRate));
  std::uint64_t state = seed;
  for (int channel = 0; channel < channels; ++channel) {
    for (std::int64_t n = 0; n < buffer.frames(); ++n) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const double unit = static_cast<double>(state >> 11) / 9007199254740992.0 * 2.0 - 1.0;
      buffer.channel(channel)[n] = static_cast<float>(amplitude * unit);
    }
  }
  return buffer;
}

double LevelDb(const AudioBuffer& buffer, std::int64_t from, std::int64_t to) {
  return dsp::DecibelsOf(RmsOf(buffer, from, to));
}

}  // namespace

CUTLINE_TEST(TheEqualiserMatchesItsDesignedResponseAtEveryBand) {
  dsp::EqSettings settings;
  settings.low_gain_db = 6.0;
  settings.low_frequency = 150.0;
  settings.mid_gain_db = -9.0;
  settings.mid_frequency = 1000.0;
  settings.mid_q = 2.0;
  settings.high_gain_db = 4.0;
  settings.high_frequency = 6000.0;
  for (const double frequency : {50.0, 150.0, 400.0, 1000.0, 2500.0, 6000.0, 12000.0}) {
    auto tone = SineBuffer(frequency, 0.1, 1.0);
    const auto before = LevelDb(tone, kRate / 2, kRate);
    dsp::ApplyEq(tone, settings);
    const auto measured = LevelDb(tone, kRate / 2, kRate) - before;
    const auto fs = static_cast<double>(kRate);
    const auto designed = dsp::ResponseDb(dsp::LowShelf(fs, 150.0, 6.0), fs, frequency) +
                          dsp::ResponseDb(dsp::Peaking(fs, 1000.0, -9.0, 2.0), fs, frequency) +
                          dsp::ResponseDb(dsp::HighShelf(fs, 6000.0, 4.0), fs, frequency);
    Expect(std::abs(measured - designed) < 0.1, "at " + std::to_string(frequency) + " Hz measured " + std::to_string(measured) +
                                                    " dB, designed " + std::to_string(designed), __LINE__);
  }
  // The designs themselves reach their stated gains where they are centred.
  const auto fs = static_cast<double>(kRate);
  CHECK(std::abs(dsp::ResponseDb(dsp::Peaking(fs, 1000.0, 6.0, 1.0), fs, 1000.0) - 6.0) < 0.01);
  CHECK(std::abs(dsp::ResponseDb(dsp::LowShelf(fs, 150.0, 6.0), fs, 20.0) - 6.0) < 0.1);
  CHECK(std::abs(dsp::ResponseDb(dsp::HighShelf(fs, 6000.0, 6.0), fs, 20000.0) - 6.0) < 0.2);
}

CUTLINE_TEST(TheCompressorFollowsItsStaticCurve) {
  // threshold -20 dB, ratio 4:1, no knee: a steady tone at level L above the
  // threshold comes out at threshold + (L - threshold) / 4, plus make-up gain.
  dsp::CompressorSettings settings;
  settings.threshold_db = -20.0;
  settings.ratio = 4.0;
  settings.knee_db = 0.0;
  for (const double input_db : {-40.0, -25.0, -20.0, -12.0, -4.0}) {
    auto tone = SineBuffer(500.0, std::sqrt(2.0) * std::pow(10.0, input_db / 20.0), 1.5);
    const auto detector = tone.Clone();
    dsp::Compress(tone, detector, settings);
    const auto expected = input_db <= -20.0 ? input_db : -20.0 + (input_db + 20.0) / 4.0;
    const auto measured = LevelDb(tone, kRate, tone.frames());
    Expect(std::abs(measured - expected) < 0.35, "in " + std::to_string(input_db) + " dB expected out " + std::to_string(expected) +
                                                     " dB, got " + std::to_string(measured), __LINE__);
  }
  settings.makeup_db = 6.0;
  auto quiet = SineBuffer(500.0, std::sqrt(2.0) * std::pow(10.0, -40.0 / 20.0), 1.0);
  const auto detector = quiet.Clone();
  dsp::Compress(quiet, detector, settings);
  CHECK(std::abs(LevelDb(quiet, kRate / 2, kRate) - (-40.0 + 6.0)) < 0.2);
}

CUTLINE_TEST(TheCompressorsAttackArrivesBeforeTheLoudPartAndItsReleaseIsGradual) {
  dsp::CompressorSettings settings;
  settings.threshold_db = -24.0;
  settings.ratio = 8.0;
  settings.attack_ms = 10.0;
  settings.release_ms = 200.0;
  settings.knee_db = 0.0;
  // 0.5 s quiet, 0.5 s loud (-6 dB), 1.5 s quiet.
  auto signal = AudioBuffer::Allocate(kRate, 1, 5 * kRate / 2);
  for (std::int64_t n = 0; n < signal.frames(); ++n) {
    const double level = n >= kRate / 2 && n < kRate ? std::pow(10.0, -6.0 / 20.0) : std::pow(10.0, -50.0 / 20.0);
    signal.channel(0)[n] = static_cast<float>(level * std::sin(2.0 * 3.14159265358979323846 * 500.0 * static_cast<double>(n) / kRate));
  }
  const auto original = signal.Clone();
  const auto detector = signal.Clone();
  dsp::Compress(signal, detector, settings);

  // Gain actually applied, in dB, in 5 ms windows.
  const auto gain_at = [&](std::int64_t from) { return LevelDb(signal, from, from + 240) - LevelDb(original, from, from + 240); };
  // Well before the loud part nothing is reduced; within the attack time ahead of it
  // the reduction is already arriving (the attack looks ahead), so the first loud
  // cycles are not let through at full level.
  CHECK(std::abs(gain_at(kRate / 4)) < 0.2);
  CHECK(gain_at(kRate / 2 + 240) < -8.0);
  // Settled reduction. A sine at -6 dB peak is -9 dB RMS, which is what the detector
  // reads: (-9 - -24) * (1 - 1/8) = 13.1 dB.
  CHECK(std::abs(gain_at(kRate * 3 / 4) - (-13.1)) < 0.6);
  // Release: not instant, and complete.
  const auto just_after = gain_at(kRate + 480);
  const auto later = gain_at(kRate + 4800);
  const auto much_later = gain_at(kRate + kRate);
  Expect(just_after < -8.0, "the gain snapped back to " + std::to_string(just_after), __LINE__);
  Expect(later > just_after, "the gain did not recover", __LINE__);
  CHECK(std::abs(much_later) < 0.3);
}

CUTLINE_TEST(TheLimiterNeverExceedsItsCeilingAndLeavesQuietAudioAlone) {
  dsp::LimiterSettings settings;
  settings.ceiling_db = -3.0;
  const auto ceiling = std::pow(10.0, settings.ceiling_db / 20.0);
  // Loud noise, a loud sine and a stereo pair with a click: nothing gets through.
  for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
    auto loud = NoiseBuffer(2.5, 2.0, seed, 2);
    dsp::Limit(loud, settings);
    CHECK(loud.Peak(0) <= static_cast<float>(ceiling) + 1e-5f);
    CHECK(loud.Peak(1) <= static_cast<float>(ceiling) + 1e-5f);
  }
  {
    auto sine = SineBuffer(300.0, 4.0, 1.0);
    dsp::Limit(sine, settings);
    CHECK(sine.Peak(0) <= static_cast<float>(ceiling) + 1e-5f);
    CHECK(sine.Peak(0) > 0.6f * static_cast<float>(ceiling));  // limited, not squashed to nothing
  }
  {
    auto clicked = SineBuffer(300.0, 0.2, 1.0, 2);
    clicked.channel(1)[24000] = 3.0f;
    dsp::Limit(clicked, settings);
    CHECK(clicked.Peak(1) <= static_cast<float>(ceiling) + 1e-5f);
  }
  // Below the ceiling, away from anything loud, the signal is untouched.
  auto quiet = SineBuffer(300.0, 0.1, 1.0);
  const auto reference = quiet.Clone();
  dsp::Limit(quiet, settings);
  double worst = 0.0;
  for (std::int64_t n = 0; n < quiet.frames(); ++n) worst = std::max(worst, static_cast<double>(std::abs(quiet.channel(0)[n] - reference.channel(0)[n])));
  CHECK(worst < 1e-6);
}

CUTLINE_TEST(TheGateClosesOnQuietNoiseAndOpensForSpeechLevelSignals) {
  dsp::GateSettings settings;
  settings.threshold_db = -40.0;
  settings.range_db = -60.0;
  // 1 s of room noise (-55 dB), 1 s of a tone (-12 dB), 1 s of noise again.
  auto signal = NoiseBuffer(0.0028, 3.0, 5);
  for (std::int64_t n = kRate; n < 2 * kRate; ++n) {
    signal.channel(0)[n] += static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979323846 * 400.0 * static_cast<double>(n) / kRate));
  }
  const auto original = signal.Clone();
  const auto detector = signal.Clone();
  dsp::Gate(signal, detector, settings);
  // The tone passes at full level; the noise either side is far down.
  CHECK(std::abs(LevelDb(signal, kRate + 2400, 2 * kRate - 2400) - LevelDb(original, kRate + 2400, 2 * kRate - 2400)) < 0.5);
  CHECK(LevelDb(signal, kRate / 4, kRate * 3 / 4) < LevelDb(original, kRate / 4, kRate * 3 / 4) - 45.0);
  CHECK(LevelDb(signal, 5 * kRate / 2, 3 * kRate - 100) < LevelDb(original, 5 * kRate / 2, 3 * kRate - 100) - 45.0);
}

CUTLINE_TEST(TheDuckerLowersTheMusicWhileTheDialogueSpeaksAndRestoresIt) {
  dsp::DuckSettings settings;
  settings.threshold_db = -30.0;
  settings.reduction_db = -12.0;
  settings.attack_ms = 20.0;
  settings.release_ms = 300.0;
  auto music = SineBuffer(200.0, 0.3, 3.0);
  const auto music_before = music.Clone();
  auto dialogue = AudioBuffer::Allocate(kRate, 1, 3 * kRate);
  for (std::int64_t n = kRate; n < 2 * kRate; ++n) {
    dialogue.channel(0)[n] = static_cast<float>(0.2 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(n) / kRate));
  }
  dsp::Duck(music, dialogue, settings);
  const auto reduction_in = [&](std::int64_t a, std::int64_t b) { return LevelDb(music, a, b) - LevelDb(music_before, a, b); };
  CHECK(std::abs(reduction_in(kRate / 4, kRate * 3 / 4)) < 0.1);                    // before: untouched
  CHECK(std::abs(reduction_in(kRate + 4800, 2 * kRate - 2400) - (-12.0)) < 0.6);    // during: ducked
  CHECK(std::abs(reduction_in(5 * kRate / 2, 3 * kRate)) < 0.5);                    // after the release: back
}

CUTLINE_TEST(SpectralProcessingWithNothingToRemoveIsTransparent) {
  // With the amount at zero the short-time transform is analysis and resynthesis and
  // nothing else: it has to give the signal back.
  auto signal = NoiseBuffer(0.4, 1.0, 3);
  const auto original = signal.Clone();
  dsp::DenoiseSettings settings;
  settings.amount = 0.0;
  dsp::Denoise(signal, settings);
  double worst = 0.0;
  for (std::int64_t n = 0; n < signal.frames(); ++n) worst = std::max(worst, static_cast<double>(std::abs(signal.channel(0)[n] - original.channel(0)[n])));
  Expect(worst < 1e-4, "round trip error " + std::to_string(worst), __LINE__);
}

CUTLINE_TEST(DenoiseRaisesTheSignalToNoiseRatioAndKeepsTheTone) {
  // Bursts of a 1 kHz tone (0.3 s on, 0.2 s off) at -14 dB RMS in white noise at
  // -24 dB RMS: 10 dB signal to noise while the tone is on. The noise floor is learned
  // from the gaps, as it is from the pauses in speech; a tone that never stopped would
  // be indistinguishable from a noise floor, and this makes no claim about that.
  const auto tone_amplitude = std::sqrt(2.0) * std::pow(10.0, -14.0 / 20.0);
  const auto noise_amplitude = std::sqrt(3.0) * std::pow(10.0, -24.0 / 20.0);  // uniform noise: rms = a / sqrt(3)
  auto clean = SineBuffer(1000.0, tone_amplitude, 6.0);
  for (std::int64_t n = 0; n < clean.frames(); ++n) {
    if ((n % (kRate / 2)) >= 3 * kRate / 10) clean.channel(0)[n] = 0.0f;
  }
  auto signal = clean.Clone();
  const auto noise = NoiseBuffer(noise_amplitude, 6.0, 11);
  for (std::int64_t n = 0; n < signal.frames(); ++n) signal.channel(0)[n] += noise.channel(0)[n];
  auto cleaned = signal.Clone();
  dsp::DenoiseSettings settings;
  settings.amount = 0.9;
  dsp::Denoise(cleaned, settings);

  // Measured over the last four seconds, after the floor estimate has settled.
  const auto from = 2 * kRate;
  const auto to = 6 * kRate;
  const auto snr = [&](const AudioBuffer& buffer) {
    double tone = 0.0, error = 0.0;
    for (std::int64_t n = from; n < to; ++n) {
      const double reference = clean.channel(0)[n];
      const double difference = buffer.channel(0)[n] - reference;
      tone += reference * reference;
      error += difference * difference;
    }
    return 10.0 * std::log10(tone / error);
  };
  const auto before = snr(signal);
  const auto after = snr(cleaned);
  // 10 dB while on, and on for 60% of the time: 10 + 10 log10(0.6) = 7.8 dB overall.
  Expect(std::abs(before - 7.8) < 0.5, "the fixture should be 7.8 dB, is " + std::to_string(before), __LINE__);
  Expect(after > before + 6.0, "signal to noise went from " + std::to_string(before) + " dB to " + std::to_string(after) + " dB", __LINE__);

  // The tone, while it is on, keeps its level.
  double on_clean = 0.0, on_cleaned = 0.0;
  for (std::int64_t start = from; start + kRate / 2 <= to; start += kRate / 2) {
    on_clean += RmsOf(clean, start + 2400, start + 12000);
    on_cleaned += RmsOf(cleaned, start + 2400, start + 12000);
  }
  Expect(std::abs(dsp::DecibelsOf(on_cleaned / on_clean)) < 1.5, "the tone changed by " + std::to_string(dsp::DecibelsOf(on_cleaned / on_clean)) + " dB", __LINE__);
}

CUTLINE_TEST(DereverbReducesTheTailBetweenBurstsAndKeepsTheBursts) {
  // Noise bursts every 800 ms (100 ms long), each followed by an exponentially
  // decaying noise tail (a crude room, 60 dB down in 500 ms).
  const auto length = 5 * kRate;
  auto dry = AudioBuffer::Allocate(kRate, 1, length);
  auto wet = AudioBuffer::Allocate(kRate, 1, length);
  const auto burst = NoiseBuffer(0.3, 0.1, 21);
  const auto tail_noise = NoiseBuffer(1.0, 1.0, 22);
  for (int start = kRate / 2; start + 4800 < length; start += 38400) {
    for (std::int64_t n = 0; n < burst.frames(); ++n) {
      dry.channel(0)[start + n] += burst.channel(0)[n];
      wet.channel(0)[start + n] += burst.channel(0)[n];
    }
    for (std::int64_t n = 0; n < tail_noise.frames() && start + n < length; ++n) {
      const double envelope = std::pow(10.0, -60.0 / 20.0 * static_cast<double>(n) / (0.5 * kRate));
      wet.channel(0)[start + n] += static_cast<float>(0.15 * envelope * tail_noise.channel(0)[n]);
    }
  }
  auto processed = wet.Clone();
  dsp::DereverbSettings settings;
  settings.amount = 1.0;
  settings.decay_ms = 500.0;
  dsp::Dereverb(processed, settings);

  // Compare the gaps (from 150 ms after a burst's start to the next burst) and the bursts.
  double tail_before = 0.0, tail_after = 0.0, burst_before = 0.0, burst_after = 0.0;
  for (int start = kRate / 2 + 38400; start + 38400 < length; start += 38400) {
    tail_before += RmsOf(wet, start + 9600, start + 33600);
    tail_after += RmsOf(processed, start + 9600, start + 33600);
    burst_before += RmsOf(wet, start, start + 4800);
    burst_after += RmsOf(processed, start, start + 4800);
  }
  Expect(dsp::DecibelsOf(tail_after / tail_before) < -3.0, "the tail only fell by " + std::to_string(dsp::DecibelsOf(tail_after / tail_before)) + " dB", __LINE__);
  Expect(dsp::DecibelsOf(burst_after / burst_before) > -2.0, "the bursts fell by " + std::to_string(dsp::DecibelsOf(burst_after / burst_before)) + " dB", __LINE__);
}

CUTLINE_TEST(LoudnessIsMeasuredAsEbuR128Specifies) {
  // EBU Tech 3341, test case 1: a 1 kHz stereo sine at -23 dBFS reads -23.0 LUFS.
  const auto amplitude = std::pow(10.0, -23.0 / 20.0);
  const auto tone = SineBuffer(1000.0, amplitude, 20.0, 2);
  const auto measured = dsp::MeasureLoudness(tone);
  Expect(std::abs(measured.integrated_lufs - (-23.0)) < 0.1, "integrated " + std::to_string(measured.integrated_lufs), __LINE__);
  Expect(std::abs(measured.momentary_lufs - (-23.0)) < 0.1, "momentary " + std::to_string(measured.momentary_lufs), __LINE__);
  Expect(std::abs(measured.short_term_lufs - (-23.0)) < 0.1, "short-term " + std::to_string(measured.short_term_lufs), __LINE__);
  CHECK(std::abs(measured.sample_peak_db - (-23.0)) < 0.05);

  // Twice the amplitude is 6.02 LU louder.
  const auto louder = dsp::MeasureLoudness(SineBuffer(1000.0, amplitude * 2.0, 20.0, 2));
  CHECK(std::abs((louder.integrated_lufs - measured.integrated_lufs) - 6.0206) < 0.02);

  // The relative gate: five seconds at -23 followed by five at -50 reads as the loud part.
  auto gated = SineBuffer(1000.0, amplitude, 10.0, 2);
  for (int channel = 0; channel < 2; ++channel) {
    for (std::int64_t n = 5 * kRate; n < gated.frames(); ++n) gated.channel(channel)[n] *= static_cast<float>(std::pow(10.0, -27.0 / 20.0));
  }
  Expect(std::abs(dsp::MeasureLoudness(gated).integrated_lufs - (-23.0)) < 0.2, "gated " + std::to_string(dsp::MeasureLoudness(gated).integrated_lufs), __LINE__);

  // Silence has no loudness to report.
  CHECK(dsp::MeasureLoudness(AudioBuffer::Allocate(kRate, 2, 5 * kRate)).integrated_lufs <= -199.0);
  // And the gain that reaches a target.
  CHECK(std::abs(dsp::GainToReachLoudness(tone, -16.0) - 7.0) < 0.1);
}

CUTLINE_TEST(TruePeakSeesTheOvershootBetweenSamples) {
  // A sine at a quarter of the sample rate, 45 degrees off the sample grid: every
  // sample is at 0.7071 of the true peak, which is 1.0.
  auto tone = AudioBuffer::Allocate(kRate, 1, 4800);
  for (std::int64_t n = 0; n < tone.frames(); ++n) {
    tone.channel(0)[n] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.25 * static_cast<double>(n) + 3.14159265358979323846 / 4.0));
  }
  const auto measured = dsp::MeasureLoudness(tone);
  CHECK(std::abs(measured.sample_peak_db - (-3.01)) < 0.05);
  Expect(measured.true_peak_db > -0.5 && measured.true_peak_db < 0.3, "true peak " + std::to_string(measured.true_peak_db), __LINE__);
}

// ------------------------------------------- processors inside the mixer ----

namespace {

// A signal by media id: "tone" is 200 Hz at 0.3; "loud" a 300 Hz tone at 0.9; "speech"
// is a 1 kHz tone only between one and two seconds; "noisy" a tone in white noise.
cutline::audio::AudioResolver ProcessorSignals() {
  return [](const std::string& media, const RationalTime& time, std::int64_t rate, int channels,
            std::int64_t frames) -> std::optional<AudioBuffer> {
    auto buffer = AudioBuffer::Allocate(rate, channels, frames);
    const auto first = time.Rescale(rate, rates::RoundingMode::Nearest);
    for (std::int64_t n = 0; n < frames; ++n) {
      const auto at = first + n;
      const double seconds = static_cast<double>(at) / static_cast<double>(rate);
      double value = 0.0;
      if (media == "tone") value = 0.3 * std::sin(2.0 * 3.14159265358979323846 * 200.0 * seconds);
      else if (media == "loud") value = 0.9 * std::sin(2.0 * 3.14159265358979323846 * 300.0 * seconds);
      else if (media == "speech") value = at >= rate && at < 2 * rate ? 0.2 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * seconds) : 0.0;
      else if (media == "noisy") {
        // Position-keyed noise, so any read of it is the same.
        std::uint64_t state = static_cast<std::uint64_t>(at + 1) * 6364136223846793005ULL + 1442695040888963407ULL;
        state ^= state >> 29;
        state *= 0x9E3779B97F4A7C15ULL;
        value = 0.15 * (static_cast<double>(state >> 11) / 9007199254740992.0 * 2.0 - 1.0);
        if ((at % rate) < rate / 2) value += 0.25 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * seconds);
      } else {
        return std::nullopt;
      }
      for (int channel = 0; channel < channels; ++channel) buffer.channel(channel)[n] = static_cast<float>(value);
    }
    return buffer;
  };
}

Effect ProcessorEffect(const std::string& id, const std::string& type,
                       std::vector<std::pair<std::string, double>> parameters, const std::string& preset = {}) {
  std::vector<std::pair<std::string, Value>> values;
  for (const auto& [name, value] : parameters) values.emplace_back(name, Value::Scalar(value));
  auto effect = MakeEffect(id, type, std::move(values));
  effect.preset_name = preset;
  return effect;
}

SequenceGraph OneClipWithTrackEffect(const std::string& media, Effect effect, double seconds = 6.0) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(MakeClip("clip-1", media, 0, static_cast<std::int64_t>(seconds)));
  track.effects.push_back(std::move(effect));
  sequence.tracks.push_back(std::move(track));
  return GraphOf(std::move(sequence));
}

double WorstDifference(const AudioBuffer& a, const AudioBuffer& b, std::int64_t from_a, std::int64_t from_b, std::int64_t count) {
  double worst = 0.0;
  for (int channel = 0; channel < a.channels(); ++channel) {
    for (std::int64_t n = 0; n < count; ++n) {
      worst = std::max(worst, static_cast<double>(std::abs(a.channel(channel)[from_a + n] - b.channel(channel)[from_b + n])));
    }
  }
  return worst;
}

}  // namespace

CUTLINE_TEST(ACompressorOnATrackFollowsItsCurveThroughTheMixer) {
  // 0.9 amplitude is -3.9 dB RMS; threshold -20, ratio 4: out = -20 + 16.1/4 = -15.97.
  const auto graph = OneClipWithTrackEffect(
      "loud", ProcessorEffect("fx", "compressor", {{"threshold", -20.0}, {"ratio", 4.0}, {"knee", 0.0}, {"attack", 5.0}, {"release", 100.0}}));
  const auto mixed = AudioMixer(Config()).MixSamples(graph, kRate, 2 * kRate, ProcessorSignals());
  const auto expected = -20.0 + (-3.9 + 20.0) / 4.0;
  Expect(std::abs(LevelDb(mixed, 4800, 2 * kRate - 4800) - expected) < 0.4, "level " + std::to_string(LevelDb(mixed, 4800, 2 * kRate - 4800)) + " dB, expected " + std::to_string(expected), __LINE__);
}

CUTLINE_TEST(EveryProcessorGivesTheSameSamplesWhateverTheBlockSizes) {
  // The property the whole design of dsp::* is for: a render is a function of the
  // project, not of how it was cut up. Each processor, on a clip, a track and the
  // sequence, rendered whole and in awkward blocks, with and without a shared cache.
  struct Case final {
    const char* name;
    const char* media;
    const char* type;
    std::vector<std::pair<std::string, double>> parameters;
  };
  const std::vector<Case> cases{
      {"eq", "tone", "eq", {{"low_gain", 6.0}, {"mid_gain", -6.0}, {"mid_frequency", 200.0}, {"high_gain", 3.0}}},
      {"compressor", "loud", "compressor", {{"threshold", -20.0}, {"ratio", 6.0}, {"attack", 15.0}, {"release", 120.0}}},
      {"limiter", "loud", "limiter", {{"ceiling", -6.0}, {"release", 60.0}}},
      {"gate", "noisy", "gate", {{"threshold", -22.0}, {"range", -40.0}, {"hold", 30.0}}},
      {"denoise", "noisy", "denoise", {{"amount", 0.8}}},
      {"dereverb", "noisy", "dereverb", {{"amount", 0.8}}},
  };
  const auto resolver = ProcessorSignals();
  const std::vector<std::int64_t> sizes{317, 1000, 1, 4999, 48000, 7, 960, 4096, 11111};
  constexpr std::int64_t kTotal = kRate + 16123;  // the clip starts at one second

  for (const auto& test : cases) {
    for (const int site : {0, 1, 2}) {  // clip, track, sequence
      auto sequence = MakeSequence();
      auto track = MakeTrack("a1", 0);
      auto clip = MakeClip("clip-1", test.media, 1, 6);  // starts at one second
      auto effect = ProcessorEffect("fx", test.type, test.parameters);
      if (site == 0) clip.effects.push_back(effect);
      if (site == 1) track.effects.push_back(effect);
      if (site == 2) sequence.effects.push_back(effect);
      track.clips.push_back(std::move(clip));
      sequence.tracks.push_back(std::move(track));
      const auto graph = GraphOf(std::move(sequence));

      for (const bool shared_cache : {false, true}) {
        auto config = Config();
        if (shared_cache) config.stretch_cache = std::make_shared<cutline::audio::StretchCache>();
        const AudioMixer mixer(config);
        const auto whole = mixer.MixSamples(graph, 0, kTotal, resolver);
        std::int64_t position = 0;
        std::size_t which = 0;
        double worst = 0.0;
        while (position < kTotal) {
          const auto count = std::min(sizes[which++ % sizes.size()], kTotal - position);
          const auto piece = mixer.MixSamples(graph, position, count, resolver);
          worst = std::max(worst, WorstDifference(piece, whole, 0, position, count));
          position += count;
        }
        Expect(worst == 0.0, std::string(test.name) + (site == 0 ? " on a clip" : site == 1 ? " on a track" : " on the sequence") +
                                 (shared_cache ? " (shared cache)" : "") + ": blocks differ by " + std::to_string(worst), __LINE__);
      }
    }
  }
}

CUTLINE_TEST(ALimiterOnTheSequenceHoldsItsCeilingInEveryBlockOfEverySize) {
  // The guarantee a limiter exists to give, checked through the whole mixer: three
  // loud tracks sum far over full scale, and no block of any size lets more than the
  // ceiling through.
  auto sequence = MakeSequence();
  for (int index = 0; index < 3; ++index) {
    auto track = MakeTrack("a" + std::to_string(index), index);
    track.clips.push_back(MakeClip("clip-" + std::to_string(index), "loud", 0, 8));
    sequence.tracks.push_back(std::move(track));
  }
  sequence.effects.push_back(ProcessorEffect("fx-limit", "limiter", {{"ceiling", -3.0}}));
  const auto graph = GraphOf(std::move(sequence));
  const auto ceiling = std::pow(10.0, -3.0 / 20.0);
  for (const std::int64_t size : {1ll, 7ll, 480ll, 1000ll, 4096ll, 9999ll}) {
    float peak = 0.0f;
    // Every block recomputes the cells it touches (no cache), so small blocks look at
    // fewer of them: enough to cover the loudest, limited stretch.
    const auto reach = std::min<std::int64_t>(3 * kRate, size * 300);
    for (std::int64_t position = 0; position < reach; position += size) {
      const auto block = AudioMixer(Config()).MixSamples(graph, position, std::min<std::int64_t>(size, 3 * kRate - position), ProcessorSignals());
      peak = std::max({peak, block.Peak(0), block.Peak(1)});
    }
    Expect(peak <= static_cast<float>(ceiling) + 1e-5f, "blocks of " + std::to_string(size) + " let " + std::to_string(peak) + " through", __LINE__);
    Expect(peak > 0.5f * static_cast<float>(ceiling), "blocks of " + std::to_string(size) + " were squashed to " + std::to_string(peak), __LINE__);
  }
}

CUTLINE_TEST(AnAutoDuckerKeyedByAnotherTrackLowersTheMusicWhileThatTrackSpeaks) {
  auto sequence = MakeSequence();
  auto music = MakeTrack("music", 0);
  music.clips.push_back(MakeClip("clip-music", "tone", 0, 5));
  music.effects.push_back(ProcessorEffect("fx-duck", "duck", {{"threshold", -30.0}, {"reduction", -12.0}, {"attack", 20.0}, {"release", 300.0}}, "dialogue"));
  auto dialogue = MakeTrack("dialogue", 1);
  dialogue.clips.push_back(MakeClip("clip-speech", "speech", 0, 5));
  // The dialogue is only the key: mute its own contribution to the mix.
  dialogue.gain_db = -96.0;
  sequence.tracks.push_back(std::move(music));
  sequence.tracks.push_back(std::move(dialogue));
  const auto graph = GraphOf(std::move(sequence));

  MixStatistics statistics;
  const auto mixed = AudioMixer(Config()).MixSamples(graph, 0, 4 * kRate, ProcessorSignals(), statistics);
  const auto original = 0.3 / std::sqrt(2.0);
  const auto db = [&](std::int64_t from, std::int64_t to) { return LevelDb(mixed, from, to) - dsp::DecibelsOf(original); };
  CHECK(std::abs(db(kRate / 4, kRate * 3 / 4)) < 0.2);                          // before the speech
  Expect(std::abs(db(kRate + 4800, 2 * kRate - 2400) - (-12.0)) < 0.7, "ducked by " + std::to_string(db(kRate + 4800, 2 * kRate - 2400)) + " dB", __LINE__);          // while it speaks
  CHECK(std::abs(db(5 * kRate / 2, 4 * kRate)) < 0.4);                          // after the release
  CHECK_EQ(statistics.missing_sidechains, 0);

  // Keyed by nothing that exists, the effect still renders and says so.
  auto lonely = MakeSequence();
  auto track = MakeTrack("music", 0);
  track.clips.push_back(MakeClip("clip-music", "tone", 0, 5));
  track.effects.push_back(ProcessorEffect("fx-duck", "duck", {}, "nobody"));
  lonely.tracks.push_back(std::move(track));
  MixStatistics missing;
  const auto result = AudioMixer(Config()).MixSamples(GraphOf(std::move(lonely)), 0, 4800, ProcessorSignals(), missing);
  CHECK(missing.missing_sidechains >= 1);
  CHECK(result.Peak(0) > 0.1f);
}

CUTLINE_TEST(ATrackKeyedByItselfOrInALoopFindsNoSidechainInsteadOfRecursing) {
  auto sequence = MakeSequence();
  auto a = MakeTrack("a", 0);
  a.clips.push_back(MakeClip("clip-a", "tone", 0, 5));
  a.effects.push_back(ProcessorEffect("fx-a", "duck", {}, "b"));
  auto b = MakeTrack("b", 1);
  b.clips.push_back(MakeClip("clip-b", "tone", 0, 5));
  b.effects.push_back(ProcessorEffect("fx-b", "duck", {}, "a"));
  sequence.tracks.push_back(std::move(a));
  sequence.tracks.push_back(std::move(b));
  MixStatistics statistics;
  const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(std::move(sequence)), 0, 4800, ProcessorSignals(), statistics);
  CHECK(statistics.missing_sidechains >= 1);
  CHECK(mixed.Peak(0) > 0.1f);
}

CUTLINE_TEST(AnEffectOnAClipSeesSilenceBeforeTheClipBeganNotTheMediaBeforeIt) {
  // A compressor on a clip that starts at one second, in a block that begins at the
  // clip's first sample: the lookback it asks for lies before the clip and must read
  // silence there, so the first samples are the same as in a render that began earlier.
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  auto clip = MakeClip("clip-1", "loud", 1, 5);
  clip.effects.push_back(ProcessorEffect("fx", "compressor", {{"threshold", -30.0}, {"ratio", 8.0}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));
  const AudioMixer mixer(Config());
  const auto early = mixer.MixSamples(graph, 0, 2 * kRate, ProcessorSignals());
  const auto late = mixer.MixSamples(graph, kRate, 1000, ProcessorSignals());
  CHECK(WorstDifference(late, early, 0, kRate, 1000) == 0.0);
  CHECK(late.Peak(0) > 0.05f);
}

CUTLINE_TEST(StatefulEffectCellsAreComputedOnceAndThenServedFromTheCache) {
  const auto graph = OneClipWithTrackEffect("tone", ProcessorEffect("fx", "eq", {{"mid_gain", 6.0}}));
  auto config = Config();
  config.stretch_cache = std::make_shared<cutline::audio::StretchCache>();
  const AudioMixer mixer(config);
  MixStatistics first;
  (void)mixer.MixSamples(graph, 20000, 1024, ProcessorSignals(), first);
  CHECK(first.dsp_cells_processed >= 1);
  MixStatistics second;
  (void)mixer.MixSamples(graph, 21024, 1024, ProcessorSignals(), second);
  CHECK_EQ(second.dsp_cells_processed, 0);
  CHECK(second.dsp_cache_hits >= 1);
  // A change to the effect's setting is a different cell, never a stale one.
  const auto changed = OneClipWithTrackEffect("tone", ProcessorEffect("fx", "eq", {{"mid_gain", -6.0}}));
  MixStatistics third;
  const auto a = mixer.MixSamples(graph, 20000, 1024, ProcessorSignals(), third);
  MixStatistics fourth;
  const auto b = mixer.MixSamples(changed, 20000, 1024, ProcessorSignals(), fourth);
  CHECK(fourth.dsp_cells_processed >= 1);
  CHECK(std::abs(RmsOf(a, 0, 1024) - RmsOf(b, 0, 1024)) > 0.01);
}

// ------------------------------------------------------ buses, sends, meters ----

namespace {

// Two tracks (a 200 Hz tone and a louder 300 Hz tone), each with one clip, plus a bus.
Sequence TwoTracksAndABus() {
  auto sequence = MakeSequence();
  auto first = MakeTrack("a1", 0);
  first.clips.push_back(MakeClip("clip-1", "tone", 0, 6));
  auto second = MakeTrack("a2", 1);
  second.clips.push_back(MakeClip("clip-2", "loud", 0, 6));
  auto bus = MakeTrack("bus", 2);
  bus.is_bus = true;
  sequence.tracks.push_back(std::move(first));
  sequence.tracks.push_back(std::move(second));
  sequence.tracks.push_back(std::move(bus));
  return sequence;
}

Track& TrackOf(Sequence& sequence, const std::string& id) {
  for (auto& track : sequence.tracks) {
    if (track.id == id) return track;
  }
  throw std::runtime_error("no track " + id);
}

}  // namespace

CUTLINE_TEST(ABusIsTheSumOfItsTracksAtItsOwnFader) {
  // The same two tracks played straight to the master, and through a bus pulled down
  // 6 dB, differ by exactly that fader and nothing else.
  auto direct = TwoTracksAndABus();
  auto routed = TwoTracksAndABus();
  TrackOf(routed, "a1").output_bus_id = "bus";
  TrackOf(routed, "a2").output_bus_id = "bus";
  TrackOf(routed, "bus").gain_db = -6.0;
  // The two tones sum past full scale, so the output clamp is off: it is not what is being compared.
  auto config = Config();
  config.limit_output = false;
  const auto reference = AudioMixer(config).MixSamples(GraphOf(std::move(direct)), 4000, 9000, ProcessorSignals());
  const auto through = AudioMixer(config).MixSamples(GraphOf(std::move(routed)), 4000, 9000, ProcessorSignals());
  const auto scale = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  float worst = 0.0f;
  for (int channel = 0; channel < 2; ++channel) {
    for (std::int64_t n = 0; n < 9000; ++n) {
      worst = std::max(worst, std::abs(through.channel(channel)[n] - reference.channel(channel)[n] * scale));
    }
  }
  CHECK(worst < 1e-6f);
  CHECK(through.Peak(0) > 0.3f);
}

CUTLINE_TEST(ABusRunsItsOwnEffectsOverTheSummedSignal) {
  // A limiter on the bus holds the sum of two loud tracks under its ceiling, which no
  // limiter on either track could.
  auto sequence = TwoTracksAndABus();
  TrackOf(sequence, "a1").clips[0].source_id = "loud";
  TrackOf(sequence, "a1").output_bus_id = "bus";
  TrackOf(sequence, "a2").output_bus_id = "bus";
  TrackOf(sequence, "bus").effects.push_back(ProcessorEffect("fx", "limiter", {{"ceiling", -6.0}}));
  const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(std::move(sequence)), 10000, 20000, ProcessorSignals());
  const auto ceiling = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  CHECK(mixed.Peak(0) <= ceiling + 1e-5f);
  CHECK(mixed.Peak(0) > 0.5f * ceiling);
}

CUTLINE_TEST(AFaderSendTakesTheSignalAfterTheFaderAndAPreFaderSendBefore) {
  // a1 is pulled all the way down and sends to the bus. Post-fader, the send carries
  // nothing; pre-fader, it carries the track as it was before the fader.
  const auto build = [&](bool pre_fader) {
    auto sequence = MakeSequence();
    auto source = MakeTrack("a1", 0);
    source.clips.push_back(MakeClip("clip-1", "tone", 0, 6));
    source.gain_db = -96.0;
    source.sends.push_back({"bus", -6.0, pre_fader});
    auto bus = MakeTrack("bus", 1);
    bus.is_bus = true;
    sequence.tracks.push_back(std::move(source));
    sequence.tracks.push_back(std::move(bus));
    return GraphOf(std::move(sequence));
  };
  const auto post = AudioMixer(Config()).MixSamples(build(false), 4000, 4800, ProcessorSignals());
  const auto pre = AudioMixer(Config()).MixSamples(build(true), 4000, 4800, ProcessorSignals());
  CHECK(post.Peak(0) < 1e-4f);
  // 0.3 at -6 dB.
  CHECK(std::abs(pre.Peak(0) - 0.3f * static_cast<float>(std::pow(10.0, -6.0 / 20.0))) < 0.005f);
}

CUTLINE_TEST(ASendAddsToTheTracksOwnOutputWithoutTakingAnythingFromIt) {
  // A track going to the master and also sending to a bus (which goes to the master)
  // is heard twice: once as it is, once through the send.
  auto sequence = MakeSequence();
  auto source = MakeTrack("a1", 0);
  source.clips.push_back(MakeClip("clip-1", "tone", 0, 6));
  source.sends.push_back({"bus", -6.0, false});
  auto bus = MakeTrack("bus", 1);
  bus.is_bus = true;
  sequence.tracks.push_back(std::move(source));
  sequence.tracks.push_back(std::move(bus));
  const auto graph = GraphOf(std::move(sequence));
  const auto mixed = AudioMixer(Config()).MixSamples(graph, 4000, 4800, ProcessorSignals());
  const auto dry = 0.3f;
  const auto send = 0.3f * static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  CHECK(mixed.Peak(0) <= dry + send + 0.002f);
  CHECK(mixed.Peak(0) > dry + 0.5f * send);
  // The same samples whatever the block size: routing is no more block-dependent than a clip.
  const AudioMixer mixer(Config());
  const auto whole = mixer.MixSamples(graph, 0, 9000, ProcessorSignals());
  double worst = 0.0;
  for (std::int64_t from = 0; from < 9000; from += 777) {
    const auto count = std::min<std::int64_t>(777, 9000 - from);
    worst = std::max(worst, WorstDifference(mixer.MixSamples(graph, from, count, ProcessorSignals()), whole, 0, from, count));
  }
  CHECK(worst == 0.0);
}

CUTLINE_TEST(AMutedBusSilencesWhatIsRoutedToItAndAMutedTrackSendsNothing) {
  auto routed = TwoTracksAndABus();
  TrackOf(routed, "a1").output_bus_id = "bus";
  TrackOf(routed, "a2").output_bus_id = "bus";
  TrackOf(routed, "bus").muted = true;
  CHECK(AudioMixer(Config()).MixSamples(GraphOf(std::move(routed)), 4000, 4800, ProcessorSignals()).Peak(0) == 0.0f);

  auto sends = TwoTracksAndABus();
  TrackOf(sends, "a1").sends.push_back({"bus", 0.0, true});
  TrackOf(sends, "a1").muted = true;
  TrackOf(sends, "a2").muted = true;
  CHECK(AudioMixer(Config()).MixSamples(GraphOf(std::move(sends)), 4000, 4800, ProcessorSignals()).Peak(0) == 0.0f);
}

CUTLINE_TEST(SoloingATrackKeepsItsBusAndSilencesTheOthersFeedingIt) {
  auto sequence = TwoTracksAndABus();
  TrackOf(sequence, "a1").output_bus_id = "bus";
  TrackOf(sequence, "a2").output_bus_id = "bus";
  TrackOf(sequence, "a1").solo = true;
  const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(std::move(sequence)), 4000, 4800, ProcessorSignals());
  // Only the 0.3 tone is left; the 0.9 one is silenced.
  CHECK(mixed.Peak(0) > 0.25f);
  CHECK(mixed.Peak(0) < 0.31f);
}

CUTLINE_TEST(AHandBuiltRoutingLoopIsSilentAndReportedNotEndless) {
  // The store refuses a loop; a snapshot made some other way must still not hang the
  // audio thread.
  auto sequence = MakeSequence();
  // bus-1 feeds bus-2 and bus-2 sends back into bus-1; bus-2 is the one that reaches the master.
  auto first = MakeTrack("bus-1", 0);
  first.is_bus = true;
  first.output_bus_id = "bus-2";
  auto second = MakeTrack("bus-2", 1);
  second.is_bus = true;
  second.sends.push_back({"bus-1", 0.0, false});
  auto source = MakeTrack("a1", 2);
  source.clips.push_back(MakeClip("clip-1", "tone", 0, 6));
  source.output_bus_id = "bus-1";
  sequence.tracks.push_back(std::move(first));
  sequence.tracks.push_back(std::move(second));
  sequence.tracks.push_back(std::move(source));
  MixStatistics statistics;
  const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(std::move(sequence)), 0, 4800, ProcessorSignals(), statistics);
  CHECK(statistics.missing_sidechains >= 1);
  CHECK_EQ(mixed.frames(), std::int64_t{4800});
}

CUTLINE_TEST(AMeterReadsATrackAfterItsFaderAndBeforeTheMaster) {
  auto sequence = TwoTracksAndABus();
  TrackOf(sequence, "a1").gain_db = -6.0;
  TrackOf(sequence, "a1").output_bus_id = "bus";
  TrackOf(sequence, "bus").gain_db = -12.0;
  sequence.effects.push_back(ProcessorEffect("fx", "limiter", {{"ceiling", -30.0}}));
  const auto graph = GraphOf(std::move(sequence));
  const AudioMixer mixer(Config());
  MixStatistics statistics;
  // The track: 0.3 at -6 dB, peak -10.46 dBFS... the master limiter does not touch it.
  const auto track = mixer.MixTrack(graph, "a1", 0, 2 * kRate, ProcessorSignals(), statistics);
  const auto track_reading = cutline::audio::MeasureLevels(track);
  CHECK(std::abs(track_reading.peak_db - dsp::DecibelsOf(0.3 * std::pow(10.0, -6.0 / 20.0))) < 0.05);
  CHECK(std::abs(track_reading.rms_db - (track_reading.peak_db - 3.01)) < 0.1);
  // The bus carries a1 only (a2 goes to the master), at its own -12 dB on top.
  const auto bus_reading = cutline::audio::MeasureLevels(mixer.MixTrack(graph, "bus", 0, 2 * kRate, ProcessorSignals(), statistics));
  CHECK(std::abs(bus_reading.peak_db - (track_reading.peak_db - 12.0)) < 0.1);
  // The master has the limiter's ceiling on it.
  const auto master_reading = cutline::audio::MeasureLevels(mixer.MixSamples(graph, 0, 2 * kRate, ProcessorSignals()));
  CHECK(master_reading.peak_db <= -30.0 + 0.05);
  // Silence reads as silence, and an unknown track is an error rather than a zero.
  CHECK(cutline::audio::MeasureLevels(AudioBuffer::Allocate(kRate, 2, 1000)).peak_db <= -199.0);
  CHECK_THROWS(mixer.MixTrack(graph, "nowhere", 0, 100, ProcessorSignals(), statistics));
}

CUTLINE_TEST(AMeterReportsLoudnessInLufs) {
  // A 997 Hz sine at -20 dBFS peak in both channels: each channel's mean square is 0.005,
  // the two sum to 0.01, i.e. -20 dB, and the K-weighting gain at 997 Hz (+0.691 dB) is exactly
  // the offset in the BS.1770 formula, so the reading is -20.0 LUFS.
  auto sequence = MakeSequence();
  auto track = MakeTrack("a1", 0);
  track.clips.push_back(MakeClip("clip-1", "ref", 0, 20));
  sequence.tracks.push_back(std::move(track));
  const auto graph = GraphOf(std::move(sequence));
  const auto resolver = [](const std::string&, const RationalTime& time, std::int64_t rate, int channels,
                           std::int64_t frames) -> std::optional<AudioBuffer> {
    auto buffer = AudioBuffer::Allocate(rate, channels, frames);
    const auto first = time.Rescale(rate, rates::RoundingMode::Nearest);
    for (std::int64_t n = 0; n < frames; ++n) {
      const auto value = 0.1 * std::sin(2.0 * 3.14159265358979323846 * 997.0 * static_cast<double>(first + n) / static_cast<double>(rate));
      for (int channel = 0; channel < channels; ++channel) buffer.channel(channel)[n] = static_cast<float>(value);
    }
    return buffer;
  };
  const auto reading = cutline::audio::MeasureLevels(AudioMixer(Config()).MixSamples(graph, 0, 5 * kRate, resolver));
  Expect(std::abs(reading.integrated_lufs - (-20.0)) < 0.1, "integrated " + std::to_string(reading.integrated_lufs) + " LUFS", __LINE__);
  CHECK(std::abs(reading.momentary_lufs - reading.integrated_lufs) < 0.3);
  CHECK(std::abs(reading.peak_db - (-20.0)) < 0.05);
}

// ------------------------------------------- pitch kept through a speed ramp ----

namespace {

// A ten-second clip whose source offset follows keys of (output seconds, source seconds).
Clip RemappedClip(const std::vector<std::pair<std::int64_t, std::int64_t>>& keys_in_hundredths, bool keep_pitch) {
  auto clip = PlacedClip("clip-1", "media-1", 0, 10, 0);
  clip.maintain_pitch = keep_pitch;
  Effect remap;
  remap.id = "remap";
  remap.effect_type = "time_remap";
  Parameter source;
  source.id = "remap:source";
  source.name = "source_offset";
  cutline::anim::AnimatedValue curve;
  for (const auto& [output, input] : keys_in_hundredths) {
    curve.SetKeyframe({RationalTime(output, 100), Value::Scalar(static_cast<double>(input) / 100.0), cutline::anim::Interpolation::Linear, {}, {}});
  }
  source.value = std::move(curve);
  remap.parameters.push_back(std::move(source));
  clip.effects.push_back(std::move(remap));
  return clip;
}

}  // namespace

CUTLINE_TEST(APitchKeepingClipRetimedByARampKeepsItsPitchWhereVarispeedFollowsTheSpeed) {
  // Half speed for 1.5 s, normal for 1.5 s, double for 1.5 s: the source advances 0.75, 1.5 and 3.0 s.
  const std::vector<std::pair<std::int64_t, std::int64_t>> keys{{0, 0}, {150, 75}, {300, 225}, {450, 525}};
  const auto resolver = PartialsResolver({1000.0});
  MixStatistics stretched_statistics;
  const auto kept = AudioMixer(Config()).MixSamples(GraphOf(OneTrack(RemappedClip(keys, true))), 0, 4 * kRate, resolver, stretched_statistics);
  const auto varied = AudioMixer(Config()).MixSamples(GraphOf(OneTrack(RemappedClip(keys, false))), 0, 4 * kRate, resolver);
  // The middle of each segment, away from the joins between them.
  const std::vector<std::pair<double, double>> segments{{0.3, 1.2}, {1.8, 2.7}, {3.3, 4.0}};
  const std::vector<double> speeds{0.5, 1.0, 2.0};
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto from = static_cast<std::int64_t>(segments[i].first * kRate);
    const auto to = static_cast<std::int64_t>(segments[i].second * kRate);
    const auto kept_rate = CrossingsPerSecond(kept, from, to);
    const auto varied_rate = CrossingsPerSecond(varied, from, to);
    Expect(std::abs(kept_rate - 2000.0) < 60.0, "segment " + std::to_string(i) + " keeps " + std::to_string(kept_rate) + " crossings a second", __LINE__);
    Expect(std::abs(varied_rate - 2000.0 * speeds[i]) < 60.0 * speeds[i] + 10.0,
           "varispeed segment " + std::to_string(i) + " has " + std::to_string(varied_rate), __LINE__);
    // Locally (50 ms windows, as in the constant-speed tests: over a whole segment the phase steps at chain
    // seams would count against it) the energy is at the tone's own frequency and at about its own loudness.
    std::vector<double> purity;
    for (auto start = from; start + 2400 < to; start += 2400) purity.push_back(EnergyFractionAt(kept, start, start + 2400, 1000.0));
    std::sort(purity.begin(), purity.end());
    Expect(purity[purity.size() / 2] > 0.95, "segment " + std::to_string(i) + " median window purity " + std::to_string(purity[purity.size() / 2]), __LINE__);
    CHECK(RmsOf(kept, from, to) > 0.2 && RmsOf(kept, from, to) < 0.35);    // 0.4 peak is 0.28 rms
  }
  CHECK(stretched_statistics.remap_stretched_blocks > 0);
  CHECK_EQ(stretched_statistics.stretch_fallbacks, 0);
  CHECK(stretched_statistics.stretch_chains_computed > 0);
}

CUTLINE_TEST(ARetimedPitchKeepingClipIsTheSameWhateverBlocksItIsAskedForInAndHoldsAndReversesFallBackAndSaySo) {
  const std::vector<std::pair<std::int64_t, std::int64_t>> keys{{0, 0}, {100, 100}, {200, 300}, {300, 100}, {400, 200}};   // normal, double, then back to the start, then forward again
  const auto resolver = PartialsResolver({700.0, 1500.0});
  const auto graph = GraphOf(OneTrack(RemappedClip({{0, 0}, {100, 50}, {200, 250}, {400, 450}}, true)));
  for (const bool shared_cache : {false, true}) {
    auto config = Config();
    if (shared_cache) config.stretch_cache = std::make_shared<cutline::audio::StretchCache>();
    const AudioMixer mixer(config);
    const std::int64_t total = 4 * kRate;
    const auto whole = mixer.MixSamples(graph, 0, total, resolver);
    double worst = 0.0;
    std::int64_t position = 0;
    const std::vector<std::int64_t> sizes{317, 1000, 1, 4999, 960, 7, 11111};
    std::size_t which = 0;
    while (position < total) {
      const auto count = std::min(sizes[which++ % sizes.size()], total - position);
      const auto piece = mixer.MixSamples(graph, position, count, resolver);
      for (std::int64_t n = 0; n < count; ++n) worst = std::max(worst, static_cast<double>(std::abs(piece.channel(0)[n] - whole.channel(0)[position + n])));
      position += count;
    }
    Expect(worst == 0.0, std::string(shared_cache ? "with" : "without") + " a shared cache, blocks differ by " + std::to_string(worst), __LINE__);
  }

  // A hold and a reverse are not stretched: the blocks that fall in them are varispeed and counted as fallbacks, and
  // the blocks before them are still stretched.
  MixStatistics statistics;
  const auto mixed = AudioMixer(Config()).MixSamples(GraphOf(OneTrack(RemappedClip(keys, true))), 0, 4 * kRate, resolver, statistics);
  CHECK(statistics.remap_stretched_blocks >= 1);
  CHECK(statistics.stretch_fallbacks >= 1);
  CHECK(mixed.Peak(0) > 0.1f);
  // The hold: the source stands still at 3 s for 0 samples long here, so use an explicit freeze.
  MixStatistics frozen;
  const auto during = AudioMixer(Config()).MixSamples(GraphOf(OneTrack(RemappedClip({{0, 0}, {100, 100}, {200, 100}, {300, 200}}, true))), kRate + kRate / 4, 4096, resolver, frozen);
  CHECK_EQ(frozen.remap_stretched_blocks, 0);
  CHECK(frozen.stretch_fallbacks >= 1);
  CHECK(std::abs(during.channel(0)[4095] - during.channel(0)[0]) < 1e-6f);   // a held source plays a held sample
  // A clip that did not ask to keep its pitch is never stretched, ramp or not.
  MixStatistics plain;
  (void)AudioMixer(Config()).MixSamples(GraphOf(OneTrack(RemappedClip(keys, false))), 0, 4096, resolver, plain);
  CHECK_EQ(plain.remap_stretched_blocks, 0);
  CHECK_EQ(plain.stretch_fallbacks, 0);
  CHECK_EQ(plain.stretch_chains_computed, 0);
}

CUTLINE_TEST(AStreamingLoudnessMeterGivesWhatMeasuringTheWholeProgrammeGivesWhateverTheBlockSizes) {
  // Twenty-two seconds of something that is not a tone: two voices of different pitch and level that come and go, a
  // quiet passage below the relative gate, and one short loud transient for the true peak.
  const std::int64_t frames = 22 * kRate;
  auto programme = AudioBuffer::Allocate(kRate, 2, frames);
  unsigned noise = 12345u;
  for (std::int64_t n = 0; n < frames; ++n) {
    const double t = static_cast<double>(n) / static_cast<double>(kRate);
    const double envelope = (t < 6.0 || t > 14.0) ? 0.25 : 0.01;   // loud, then a quiet passage, then loud
    noise = noise * 1664525u + 1013904223u;
    const double hiss = (static_cast<double>(noise >> 8) / 8388608.0 - 1.0) * 0.01;
    programme.channel(0)[n] = static_cast<float>(envelope * std::sin(2.0 * 3.14159265358979 * 220.0 * t) + hiss);
    programme.channel(1)[n] = static_cast<float>(0.6 * envelope * std::sin(2.0 * 3.14159265358979 * 1350.0 * t + 1.0) + hiss);
  }
  programme.channel(0)[9 * kRate + 17] = 0.95f;
  const auto whole = dsp::MeasureLoudness(programme);

  for (const std::int64_t block : {std::int64_t{480}, std::int64_t{4799}, std::int64_t{4800}, std::int64_t{10007}, std::int64_t{kRate}}) {
    dsp::LoudnessMeter meter(kRate, 2);
    for (std::int64_t at = 0; at < frames; at += block) {
      const auto count = std::min(block, frames - at);
      auto piece = AudioBuffer::Allocate(kRate, 2, count);
      for (int channel = 0; channel < 2; ++channel) std::copy(programme.channel(channel) + at, programme.channel(channel) + at + count, piece.channel(channel));
      meter.Push(piece);
    }
    const auto streamed = meter.Result();
    CHECK_EQ(meter.frames(), frames);
    CHECK(std::abs(streamed.integrated_lufs - whole.integrated_lufs) < 1e-3);
    CHECK(std::abs(streamed.momentary_lufs - whole.momentary_lufs) < 1e-3);
    CHECK(std::abs(streamed.loudest_momentary_lufs - whole.loudest_momentary_lufs) < 1e-3);
    CHECK(streamed.sample_peak_db == whole.sample_peak_db);
    CHECK(std::abs(streamed.true_peak_db - whole.true_peak_db) < 1e-9);
  }
  CHECK(whole.integrated_lufs > -40.0 && whole.integrated_lufs < -10.0);   // a number, not the gate's floor

  // Shorter than one block, empty, and silence.
  dsp::LoudnessMeter empty(kRate, 2);
  CHECK(empty.Result().integrated_lufs <= -199.0);
  dsp::LoudnessMeter silent(kRate, 2);
  silent.Push(AudioBuffer::Allocate(kRate, 2, 3 * kRate));
  CHECK(silent.Result().integrated_lufs <= -199.0);
  auto brief = AudioBuffer::Allocate(kRate, 2, 8000);
  for (std::int64_t n = 0; n < 8000; ++n) brief.channel(0)[n] = brief.channel(1)[n] = static_cast<float>(0.3 * std::sin(0.05 * static_cast<double>(n)));
  dsp::LoudnessMeter short_meter(kRate, 2);
  short_meter.Push(brief);
  CHECK(std::abs(short_meter.Result().integrated_lufs - dsp::MeasureLoudness(brief).integrated_lufs) < 1e-3);
}

CUTLINE_TEST(ATakeFromTheDefaultInputIsWrittenAsAWavThatReadsBackAndStopsCleanly) {
  const auto devices = cutline::audio::ListInputDevices();
  SKIP_INAPPLICABLE(!devices.empty(), "this machine has no input device");
  int defaults = 0;
  for (const auto& device : devices) {
    CHECK(!device.id.empty() && !device.name.empty());
    defaults += device.is_default ? 1 : 0;
  }
  CHECK(defaults <= 1);
  const auto path = (std::filesystem::temp_directory_path() / "cutline-take.wav").string();
  std::string why;
  auto recorder = cutline::audio::Recorder::Start("", path, 48000, 2, &why);
  SKIP_INAPPLICABLE(recorder != nullptr, "the default input could not be opened here: " + why);
  const auto started = std::chrono::steady_clock::now();
  while (recorder->frames() < 24000 && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(recorder->frames() >= 24000);   // half a second arrived
  CHECK(std::isfinite(recorder->level_db()) && !recorder->failed());
  const auto result = recorder->Stop();
  CHECK(result.ok && result.frames >= 24000);
  CHECK_EQ(std::filesystem::file_size(path), static_cast<std::uintmax_t>(44 + result.frames * 2 * 4));
  // The file is a float WAV at the rate and layout asked for, and the decoder reads every frame of it back.
  cutline::media::RegisterAllProviders();
  auto source = cutline::media::SourceRegistry::Instance().Open(path);
  CHECK(source != nullptr && source->probe().PrimaryAudio() != nullptr);
  const auto block = source->ReadAudio(cutline::time::RationalTime(0, 1), 48000, 2, 4800);
  CHECK(block.has_value() && block->frames() == 4800 && block->channels() == 2);
  // A second Stop is harmless; a bad device and a bad folder are refused with the reason.
  CHECK(!recorder->Stop().ok);
  std::string refused;
  CHECK(cutline::audio::Recorder::Start("no-such-device", path, 48000, 2, &refused) == nullptr && !refused.empty());
  refused.clear();
  CHECK(cutline::audio::Recorder::Start("", (std::filesystem::temp_directory_path() / "no-such-folder" / "x.wav").string(), 48000, 2, &refused) == nullptr && !refused.empty());
  source.reset();
  std::filesystem::remove(path);
}

int main() { return cutline::testing::RunAll("audio"); }

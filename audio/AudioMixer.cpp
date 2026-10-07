#include "audio/AudioMixer.h"

#include "audio/Dsp.h"
#include "audio/TimeStretch.h"
#include "effects/EffectRegistry.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace cutline::audio {
namespace {

using media::AudioBuffer;
using time::RationalTime;
using timeline::Clip;
using timeline::Effect;
using timeline::Sequence;
using timeline::Track;
using timeline::Transition;

// ------------------------------------------------------------------ helpers ----

[[nodiscard]] std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) {
  auto quotient = value / divisor;
  if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --quotient;
  return quotient;
}

// A time as a count of output samples, to the nearest sample.
[[nodiscard]] std::int64_t ToSample(const RationalTime& time, std::int64_t rate) {
  return time.Rescale(rate, time::RoundingMode::Nearest);
}

struct Context final {
  const model::RenderSemantics semantics;
  const timeline::SequenceGraph& graph;
  const AudioResolver& resolve;
  MixStatistics& statistics;
  const timeline::CompileOptions& options;
  const MixerConfig& config;
  // Chains computed during this mix when the configuration has no cache of its own.
  std::shared_ptr<StretchCache> local_cache;
};

// ------------------------------------------------------------------ effects ----

[[nodiscard]] const anim::AnimatedValue* FindParameter(const Effect& effect, const std::string& name) {
  for (const auto& parameter : effect.parameters) {
    if (parameter.name == name) return &parameter.value;
  }
  return nullptr;
}

// The value of a scalar parameter at each of `count` samples, whose local times
// are (first_local + n) / rate. A constant is evaluated once; a keyframed
// parameter is evaluated at every sample, which is the point: a fade that runs
// across a block has to be a ramp, not one value for the block.
class ParameterTrack final {
 public:
  ParameterTrack(const Effect& effect, const std::string& name, double fallback, std::int64_t first_local,
                 std::int64_t count, std::int64_t rate) {
    const auto* value = FindParameter(effect, name);
    if (value == nullptr) {
      constant_ = fallback;
      return;
    }
    if (!value->animated()) {
      constant_ = value->constant().components[0];
      return;
    }
    values_.resize(static_cast<std::size_t>(count));
    for (std::int64_t n = 0; n < count; ++n) {
      values_[static_cast<std::size_t>(n)] = value->Sample(RationalTime(first_local + n, rate)).components[0];
    }
  }

  [[nodiscard]] double at(std::int64_t n) const {
    return values_.empty() ? constant_ : values_[static_cast<std::size_t>(n)];
  }

 private:
  double constant_{0.0};
  std::vector<double> values_;
};

[[nodiscard]] bool IsPictureEffect(const std::string& type) {
  const auto* descriptor = effects::FindEffect(type);
  return descriptor != nullptr && descriptor->medium == effects::Medium::Video;
}

// One value of a scalar parameter at one local time.
[[nodiscard]] double ScalarAt(const Effect& effect, const std::string& name, double fallback, std::int64_t local_sample,
                              std::int64_t rate) {
  const auto* value = FindParameter(effect, name);
  if (value == nullptr) return fallback;
  return value->animated() ? value->Sample(RationalTime(local_sample, rate)).components[0]
                           : value->constant().components[0];
}

// Effects with memory (filters, dynamics, spectral). They cannot be applied to a block
// on its own: the block's samples depend on what came before and after it. See Dsp.h
// for how they are made to give the same result however a render is cut up.
[[nodiscard]] bool IsStatefulEffect(const std::string& type) {
  return type == "eq" || type == "compressor" || type == "limiter" || type == "gate" || type == "duck" ||
         type == "denoise" || type == "dereverb";
}

// Stateful effects are applied on a fixed grid of cells, and their parameters are
// read once per cell (automation of an equaliser or a threshold moves in steps of a
// cell, about 85 ms; volume, gain and pan, which are per sample, are not affected).
constexpr std::int64_t kCell = 4096;

using Fetch = std::function<AudioBuffer(std::int64_t first, std::int64_t count)>;
// The signal of the track or bus an effect is keyed by, or nullopt when there is none.
using SidechainFetch = std::function<std::optional<AudioBuffer>(const std::string& id, std::int64_t first, std::int64_t count)>;

struct StackInputs final {
  const Fetch& base;
  const SidechainFetch& sidechain;
  // Where the effect owner's own time begins, on the sequence's sample grid (a
  // clip's start; zero for a track or a sequence).
  std::int64_t origin{0};
  // Identifies the owner for caching.
  std::string owner_key;
};

AudioBuffer EvaluateStack(const Context& context, const std::vector<const Effect*>& ordered, std::size_t upto,
                          const StackInputs& inputs, std::int64_t first, std::int64_t count);

// One cell of a stateful effect: parameters, margins, processing.
struct StatefulPlan final {
  dsp::Margins margins;
  std::function<void(AudioBuffer& signal, const AudioBuffer& detector)> run;
  bool wants_sidechain{false};
};

[[nodiscard]] StatefulPlan PlanStateful(const Effect& effect, std::int64_t local_sample, std::int64_t rate_int) {
  const auto rate = static_cast<double>(rate_int);
  const auto get = [&](const char* name, double fallback) { return ScalarAt(effect, name, fallback, local_sample, rate_int); };
  StatefulPlan plan;
  const auto& type = effect.effect_type;
  if (type == "eq") {
    dsp::EqSettings s;
    s.low_gain_db = get("low_gain", 0.0);
    s.low_frequency = get("low_frequency", 120.0);
    s.mid_gain_db = get("mid_gain", 0.0);
    s.mid_frequency = get("mid_frequency", 1000.0);
    s.mid_q = get("mid_q", 1.0);
    s.high_gain_db = get("high_gain", 0.0);
    s.high_frequency = get("high_frequency", 8000.0);
    plan.margins = dsp::EqMargins(s, rate);
    plan.run = [s](AudioBuffer& signal, const AudioBuffer&) { dsp::ApplyEq(signal, s); };
  } else if (type == "compressor") {
    dsp::CompressorSettings s;
    s.threshold_db = get("threshold", -18.0);
    s.ratio = get("ratio", 4.0);
    s.attack_ms = get("attack", 10.0);
    s.release_ms = get("release", 150.0);
    s.makeup_db = get("makeup", 0.0);
    s.knee_db = get("knee", 6.0);
    plan.margins = dsp::CompressorMargins(s, rate);
    plan.wants_sidechain = !effect.preset_name.empty();
    plan.run = [s](AudioBuffer& signal, const AudioBuffer& detector) { dsp::Compress(signal, detector, s); };
  } else if (type == "limiter") {
    dsp::LimiterSettings s;
    s.ceiling_db = get("ceiling", -1.0);
    s.release_ms = get("release", 100.0);
    s.lookahead_ms = get("lookahead", 3.0);
    plan.margins = dsp::LimiterMargins(s, rate);
    plan.run = [s](AudioBuffer& signal, const AudioBuffer&) { dsp::Limit(signal, s); };
  } else if (type == "gate") {
    dsp::GateSettings s;
    s.threshold_db = get("threshold", -50.0);
    s.range_db = get("range", -60.0);
    s.attack_ms = get("attack", 2.0);
    s.hold_ms = get("hold", 40.0);
    s.release_ms = get("release", 120.0);
    plan.margins = dsp::GateMargins(s, rate);
    plan.wants_sidechain = !effect.preset_name.empty();
    plan.run = [s](AudioBuffer& signal, const AudioBuffer& detector) { dsp::Gate(signal, detector, s); };
  } else if (type == "duck") {
    dsp::DuckSettings s;
    s.threshold_db = get("threshold", -35.0);
    s.reduction_db = get("reduction", -12.0);
    s.attack_ms = get("attack", 20.0);
    s.release_ms = get("release", 400.0);
    plan.margins = dsp::DuckMargins(s, rate);
    plan.wants_sidechain = true;
    plan.run = [s](AudioBuffer& signal, const AudioBuffer& detector) { dsp::Duck(signal, detector, s); };
  } else if (type == "denoise") {
    dsp::DenoiseSettings s;
    s.amount = get("amount", 0.7);
    s.sensitivity = get("sensitivity", 1.0);
    plan.margins = dsp::DenoiseMargins(rate);
    plan.run = [s](AudioBuffer& signal, const AudioBuffer&) { dsp::Denoise(signal, s); };
  } else {  // dereverb
    dsp::DereverbSettings s;
    s.amount = get("amount", 0.6);
    s.decay_ms = get("decay", 300.0);
    plan.margins = dsp::DereverbMargins(s, rate);
    plan.run = [s](AudioBuffer& signal, const AudioBuffer&) { dsp::Dereverb(signal, s); };
  }
  return plan;
}

AudioBuffer ApplyStateful(const Context& context, const std::vector<const Effect*>& ordered, std::size_t upto,
                          const StackInputs& inputs, std::int64_t first, std::int64_t count) {
  const auto& effect = *ordered[upto - 1];
  const auto rate = context.config.sample_rate;
  auto output = AudioBuffer::Allocate(rate, context.config.channels, count);
  auto& cache = context.config.stretch_cache != nullptr ? *context.config.stretch_cache : *context.local_cache;

  const auto first_cell = FloorDiv(first, kCell);
  const auto last_cell = FloorDiv(first + count - 1, kCell);
  for (auto cell = first_cell; cell <= last_cell; ++cell) {
    const auto cell_start = cell * kCell;
    // Everything a cell depends on is named in its key: the project revision, where
    // the effect sits, every setting of it (read at the cell's start), the options
    // that change what the sound is, and the cell. The engine empties the cache when
    // the project or its media changes.
    const auto plan = PlanStateful(effect, cell_start - inputs.origin, rate);
    std::string key = "dsp|" + std::to_string(context.graph.root()->source_revision) + "|" + inputs.owner_key + "|" +
                      std::to_string(upto) + "|" + effect.id + "|" + effect.effect_type + "|" + effect.preset_name + "|" +
                      std::to_string(cell) + "|" + std::to_string(rate) + "|" + std::to_string(context.config.channels) + "|" +
                      (context.options.honour_mute_and_solo ? "m" : "a") + (context.options.high_quality_audio ? "q" : "p");
    for (const auto& parameter : effect.parameters) {
      key += "|" + parameter.name + "=" +
             std::to_string(ScalarAt(effect, parameter.name, 0.0, cell_start - inputs.origin, rate));
    }
    std::shared_ptr<const AudioBuffer> done;
    if (context.config.cache_dsp) done = cache.Find(key);
    if (done != nullptr) {
      ++context.statistics.dsp_cache_hits;
    } else {
      const auto in_first = cell_start - plan.margins.memory;
      const auto in_count = kCell + plan.margins.memory + plan.margins.lookahead;
      auto signal = EvaluateStack(context, ordered, upto - 1, inputs, in_first, in_count);
      std::optional<AudioBuffer> side;
      if (plan.wants_sidechain && !effect.preset_name.empty()) side = inputs.sidechain(effect.preset_name, in_first, in_count);
      if (plan.wants_sidechain && !side.has_value()) ++context.statistics.missing_sidechains;
      // With no side-chain the effect listens to its own input, as it was before it changed it.
      const AudioBuffer own = side.has_value() ? AudioBuffer() : signal.Clone();
      plan.run(signal, side.has_value() ? *side : own);
      auto cell_result = std::make_shared<AudioBuffer>(AudioBuffer::Allocate(rate, context.config.channels, kCell));
      for (int channel = 0; channel < signal.channels(); ++channel) {
        std::copy_n(signal.channel(channel) + plan.margins.memory, kCell, cell_result->channel(channel));
      }
      ++context.statistics.dsp_cells_processed;
      if (context.config.cache_dsp) cache.Insert(key, cell_result);
      done = std::move(cell_result);
    }
    const auto from = std::max(first, cell_start);
    const auto to = std::min(first + count, cell_start + kCell);
    for (int channel = 0; channel < output.channels(); ++channel) {
      std::copy_n(done->channel(channel) + (from - cell_start), to - from, output.channel(channel) + (from - first));
    }
  }
  return output;
}

AudioBuffer EvaluateStack(const Context& context, const std::vector<const Effect*>& ordered, std::size_t upto,
                          const StackInputs& inputs, std::int64_t first, std::int64_t count) {
  if (upto == 0) return inputs.base(first, count);
  const auto& effect = *ordered[upto - 1];
  const auto rate = context.config.sample_rate;

  if (IsStatefulEffect(effect.effect_type)) return ApplyStateful(context, ordered, upto, inputs, first, count);

  auto buffer = EvaluateStack(context, ordered, upto - 1, inputs, first, count);
  const auto first_local = first - inputs.origin;
  const auto frames = buffer.frames();
  if (effect.effect_type == "volume") {
    // Authored in decibels, as a mixer strip shows it.
    const ParameterTrack level(effect, "level", 0.0, first_local, frames, rate);
    for (int channel = 0; channel < buffer.channels(); ++channel) {
      auto* samples = buffer.channel(channel);
      for (std::int64_t n = 0; n < frames; ++n) samples[n] *= media::DecibelsToLinear(level.at(n));
    }
  } else if (effect.effect_type == "gain") {
    // A linear multiplier, for cases where the caller already has one.
    const ParameterTrack gain(effect, "value", 1.0, first_local, frames, rate);
    for (int channel = 0; channel < buffer.channels(); ++channel) {
      auto* samples = buffer.channel(channel);
      for (std::int64_t n = 0; n < frames; ++n) samples[n] *= static_cast<float>(gain.at(n));
    }
  } else if (effect.effect_type == "pan") {
    if (buffer.channels() == 2) {
      const ParameterTrack pan(effect, "value", 0.0, first_local, frames, rate);
      for (std::int64_t n = 0; n < frames; ++n) {
        const auto gains = media::StereoPan(pan.at(n));
        buffer.channel(0)[n] *= gains.left;
        buffer.channel(1)[n] *= gains.right;
      }
    }
  } else if (IsPictureEffect(effect.effect_type)) {
    // Picture effects on an audio clip are not an error; they belong to the
    // linked video side and are simply not ours to apply.
  } else {
    context.statistics.skipped_effects.push_back(effect.effect_type);
  }
  return buffer;
}

// An effect stack in order, applied to a signal that can be read over any range.
[[nodiscard]] std::vector<const Effect*> OrderedEnabled(const std::vector<Effect>& effects) {
  std::vector<const Effect*> ordered;
  for (const auto& effect : effects) {
    if (effect.enabled) ordered.push_back(&effect);
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Effect* left, const Effect* right) { return left->order < right->order; });
  return ordered;
}

AudioBuffer ApplyEffectStack(const Context& context, const std::vector<Effect>& effects, const StackInputs& inputs,
                             std::int64_t first, std::int64_t count) {
  const auto ordered = OrderedEnabled(effects);
  return EvaluateStack(context, ordered, ordered.size(), inputs, first, count);
}

// Track gain and pan, applied after the track's own effects.
void ApplyTrackStrip(AudioBuffer& buffer, const Track& track) {
  const auto gain = media::DecibelsToLinear(track.gain_db);
  if (std::abs(gain - 1.0f) > 1e-6f) buffer.ApplyGain(gain);
  if (buffer.channels() == 2 && std::abs(track.pan) > 1e-9) {
    const auto gains = media::StereoPan(track.pan);
    for (std::int64_t n = 0; n < buffer.frames(); ++n) {
      buffer.channel(0)[n] *= gains.left;
      buffer.channel(1)[n] *= gains.right;
    }
  }
}

// ------------------------------------------------------------ source mapping ----

// Where output sample `e` of a clip reads its source, as a whole sample plus a
// fraction. `e` counts from the clip's first sample and may be negative or run
// past the clip's end: a transition plays the media beyond the cut, and the same
// line continues through it.
//
// Forward: in + e * rate. Reversed: (out - 1) - e * rate, so that a reversed clip
// at a rate of one is the exact time-reversal of the same range played forward.
// Everything is integer arithmetic over the rate's own denominator, so a position
// is never a rounded float and never drifts over a long clip.
struct SourceMapping final {
  std::int64_t anchor{};
  bool reversed{false};
  std::int64_t rate_numerator{1};
  std::int64_t rate_denominator{1};
  const anim::AnimatedValue* time_remap{nullptr};
  std::int64_t source_in{};
  std::int64_t sample_rate{48000};

  void At(std::int64_t e, std::int64_t& whole, std::int64_t& fraction_numerator) const {
    if (time_remap != nullptr) {
      constexpr std::int64_t precision = 1'000'000;
      const auto seconds = time_remap->Sample(RationalTime(e, sample_rate)).scalar();
      const auto position = static_cast<long double>(source_in) +
                            static_cast<long double>(seconds) * static_cast<long double>(sample_rate);
      const auto scaled = static_cast<std::int64_t>(std::llround(position * precision));
      whole = FloorDiv(scaled, precision);
      fraction_numerator = scaled - whole * precision;
      return;
    }
    const auto position = reversed ? anchor * rate_denominator - e * rate_numerator
                                   : anchor * rate_denominator + e * rate_numerator;
    whole = FloorDiv(position, rate_denominator);
    fraction_numerator = position - whole * rate_denominator;
  }
  [[nodiscard]] bool integral() const { return rate_denominator == 1; }
};

[[nodiscard]] const anim::AnimatedValue* TimeRemapOf(const Clip& clip) {
  for (const auto& effect : clip.effects) {
    if (!effect.enabled || effect.effect_type != "time_remap") continue;
    for (const auto& parameter : effect.parameters) {
      if (parameter.name == "source_offset") return &parameter.value;
    }
  }
  return nullptr;
}

[[nodiscard]] float Cubic(float before, float at, float after, float beyond, double t) {
  // Catmull-Rom: passes through `at` and `after`, continuous in value and slope.
  const double a = -0.5 * before + 1.5 * at - 1.5 * after + 0.5 * beyond;
  const double b = before - 2.5 * at + 2.0 * after - 0.5 * beyond;
  const double c = -0.5 * before + 0.5 * after;
  return static_cast<float>(((a * t + b) * t + c) * t + at);
}

// ---------------------------------------------------------------- transitions ----

// How a transition touches one clip.
struct TransitionRole final {
  const Transition* transition{};
  bool outgoing{};   // this clip is the `from` side
  bool two_sided{};  // both clips exist, so the media either side of the cut is real
  std::int64_t start{};
  std::int64_t end{};
  bool constant_power{};

  [[nodiscard]] double Progress(std::int64_t sample) const {
    return static_cast<double>(sample - start) / static_cast<double>(std::max<std::int64_t>(end - start, 1));
  }
  // Weight of this clip at `sample`, which must be inside [start, end).
  [[nodiscard]] double Weight(std::int64_t sample) const {
    const auto p = Progress(sample);
    if (constant_power) {
      const auto angle = p * std::numbers::pi / 2.0;
      return outgoing ? std::cos(angle) : std::sin(angle);
    }
    return outgoing ? 1.0 - p : p;
  }
};

// ------------------------------------------------------------- the renderer ----

AudioBuffer RenderSequence(const Context& context, const Sequence& sequence, std::int64_t first_sample,
                           std::int64_t frames, int depth, std::unordered_set<std::string>& visiting,
                           const std::string* only_track = nullptr);

// One clip's contribution to samples [first, first + count) of its sequence's
// timeline, before the clip's effects. Returns nullopt when the source gave
// nothing, and reports whether that matters via `within_clip`.
// Reads `count` samples of the clip's source, at the mixer's rate, starting at
// source sample `first`: from the media, or by rendering a nested sequence.
std::optional<AudioBuffer> ReadSource(const Context& context, const Clip& clip, std::int64_t first, std::int64_t count,
                                      int depth, std::unordered_set<std::string>& visiting) {
  if (clip.source_kind == model::SourceKind::Sequence) {
    const auto* nested = context.graph.Find(clip.source_id);
    if (nested == nullptr || depth >= context.options.max_nesting_depth) return std::nullopt;
    return RenderSequence(context, *nested, first, count, depth + 1, visiting);
  }
  if (clip.source_kind == model::SourceKind::Media) {
    return context.resolve(clip.source_id, RationalTime(first, context.config.sample_rate),
                           context.config.sample_rate, context.config.channels, count);
  }
  return std::nullopt;
}

// What a time-remap curve is, as text, for addressing what was computed from it.
[[nodiscard]] std::string RemapSignature(const anim::AnimatedValue& curve) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](std::uint64_t word) {
    hash ^= word;
    hash *= 1099511628211ull;
  };
  const auto mix_double = [&mix](double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    mix(bits);
  };
  mix_double(curve.constant().components[0]);
  for (const auto& key : curve.keyframes()) {
    mix(static_cast<std::uint64_t>(key.time.numerator()));
    mix(static_cast<std::uint64_t>(key.time.denominator()));
    mix_double(key.value.components[0]);
    mix(static_cast<std::uint64_t>(key.interpolation));
    mix_double(key.out_handle.x);
    mix_double(key.out_handle.y);
    mix_double(key.in_handle.x);
    mix_double(key.in_handle.y);
  }
  char text[17];
  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
  return text;
}

// How much of each output sample of a remapped, pitch-keeping clip comes from the stretch: 1 where the source is
// advancing at a speed WSOLA handles, 0 where it holds, reverses or goes beyond the range (those play varispeed),
// and in between over a few dozen samples so the two do not click at the join. It is a function of the sample's
// position and the curve alone, never of where a block happens to begin, so the mix is the same however it is cut.
[[nodiscard]] std::vector<float> RemapStretchWeights(const anim::AnimatedValue& curve, std::int64_t sample_rate, std::int64_t e,
                                                     std::int64_t count) {
  constexpr std::int64_t kGrid = 8;     // flags are taken every kGrid samples
  constexpr std::int64_t kProbe = 16;   // each over the speed across 2 * kProbe samples
  constexpr std::int64_t kSmooth = 4;   // and averaged over this many grid points either side
  const auto floor_div = [](std::int64_t a, std::int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); };
  const auto first_grid = floor_div(e, kGrid) - kSmooth;
  const auto last_grid = floor_div(e + count - 1, kGrid) + kSmooth;
  std::vector<float> flags(static_cast<std::size_t>(last_grid - first_grid + 1));
  for (auto g = first_grid; g <= last_grid; ++g) {
    const auto centre = g * kGrid;
    // Nothing is read before the clip begins, so the probe never starts there: the opening samples are
    // judged by the first stretch of the clip, not by the standstill of the curve before it.
    const auto low = std::max<std::int64_t>(centre - kProbe, 0);
    const auto high = std::max<std::int64_t>(centre + kProbe, 2 * kProbe);
    const auto before = curve.Sample(RationalTime(low, sample_rate)).scalar();
    const auto after = curve.Sample(RationalTime(high, sample_rate)).scalar();
    const auto speed = (after - before) * static_cast<double>(sample_rate) / static_cast<double>(high - low);
    flags[static_cast<std::size_t>(g - first_grid)] = CanStretch(speed) ? 1.0f : 0.0f;
  }
  std::vector<float> weights(static_cast<std::size_t>(count));
  for (std::int64_t n = 0; n < count; ++n) {
    const auto g = floor_div(e + n, kGrid);
    float sum = 0.0f;
    for (auto k = g - kSmooth; k <= g + kSmooth; ++k) sum += flags[static_cast<std::size_t>(k - first_grid)];
    weights[static_cast<std::size_t>(n)] = sum / static_cast<float>(2 * kSmooth + 1);
  }
  return weights;
}

// A clip played at another speed with its pitch kept. The signal being stretched is
// the clip's source range in playing order (reversed clips read it backwards) and is
// silent outside it; the result is read out chain by chain from the cache.
std::optional<AudioBuffer> ReadStretched(const Context& context, const Clip& clip, std::int64_t clip_start,
                                         std::int64_t clip_in, std::int64_t clip_out, double speed,
                                         const anim::AnimatedValue* remap, std::int64_t first, std::int64_t count,
                                         int depth, std::unordered_set<std::string>& visiting) {
  const auto rate = context.config.sample_rate;
  const auto channels = context.config.channels;
  const auto quality = context.options.high_quality_audio ? OfflineStretch() : PreviewStretch();
  const std::int64_t length = clip_out - clip_in;
  if (length <= 0) return std::nullopt;

  // A remapped clip's chains depend on its whole curve, so the curve is part of the address.
  const std::string key_prefix = clip.id + "|" + clip.source_id + "|" + std::to_string(clip_in) + "|" +
                                 std::to_string(clip_out) + "|" + std::to_string(clip.playback_rate.numerator()) + "/" +
                                 std::to_string(clip.playback_rate.denominator()) + "|" + (clip.reversed ? "r" : "f") +
                                 (remap != nullptr ? "|remap:" + RemapSignature(*remap) : std::string()) +
                                 "|" + std::to_string(rate) + "|" + std::to_string(channels) + "|" +
                                 std::to_string(quality.frame) + "," + std::to_string(quality.search) + "," +
                                 std::to_string(quality.decimation) + "," + std::to_string(quality.chain_frames);

  bool source_missing = false;
  // The signal being stretched, for signal samples [from, from + n).
  const StretchInput read = [&](std::int64_t from, std::int64_t n, AudioBuffer& into) {
    const auto lo = std::max<std::int64_t>(from, 0);
    const auto hi = std::min<std::int64_t>(from + n, length);
    if (hi <= lo) return;
    const auto src_first = clip.reversed ? clip_out - hi : clip_in + lo;
    auto piece = ReadSource(context, clip, src_first, hi - lo, depth, visiting);
    if (!piece.has_value()) {
      source_missing = true;
      return;
    }
    for (int channel = 0; channel < channels; ++channel) {
      auto* to = into.channel(channel) + (lo - from);
      const auto* source = piece->channel(channel);
      const auto available = std::min<std::int64_t>(piece->frames(), hi - lo);
      for (std::int64_t i = 0; i < available; ++i) to[i] = clip.reversed ? source[hi - lo - 1 - i] : source[i];
    }
  };

  // Where in the signal (counted from the clip's in point) output sample e of a remapped clip reads.
  const StretchPosition position = [remap, rate](std::int64_t e) {
    return remap == nullptr ? 0.0 : remap->Sample(RationalTime(e, rate)).scalar() * static_cast<double>(rate);
  };

  auto piece = AudioBuffer::Allocate(rate, channels, count);
  const auto hop = quality.hop();
  const std::int64_t output_length = clip_start >= 0 ? 0 : 0;
  (void)output_length;
  const auto e_first = std::max<std::int64_t>(first - clip_start, 0);
  const auto e_last = first - clip_start + count;  // exclusive
  if (e_last <= e_first) return piece;

  const auto first_chain = std::max<std::int64_t>(ChainOf(e_first, quality) - 1, 0);
  const auto last_chain = ChainOf(e_last - 1, quality);
  for (auto chain = first_chain; chain <= last_chain; ++chain) {
    const auto key = key_prefix + "#" + std::to_string(chain);
    std::shared_ptr<const AudioBuffer> data;
    auto& cache = context.config.stretch_cache != nullptr ? *context.config.stretch_cache : *context.local_cache;
    data = cache.Find(key);
    if (data != nullptr) {
      ++context.statistics.stretch_cache_hits;
    } else {
      auto made = std::make_shared<AudioBuffer>(
          remap != nullptr ? StretchChainMapped(read, rate, channels, position, chain, quality)
                           : StretchChain(read, rate, channels, speed, chain, quality));
      if (source_missing) return std::nullopt;
      ++context.statistics.stretch_chains_computed;
      cache.Insert(key, made);
      data = std::move(made);
    }
    const auto start = ChainStart(chain, quality);
    const auto from = std::max(e_first, start);
    const auto to = std::min(e_last, start + data->frames());
    for (auto e = from; e < to; ++e) {
      // Past the clip's own end the stretched signal is silent: no handles.
      const auto at = e - (first - clip_start);
      for (int channel = 0; channel < channels; ++channel) {
        piece.channel(channel)[at] += data->channel(channel)[e - start];
      }
    }
  }
  (void)hop;
  return piece;
}

std::optional<AudioBuffer> ReadClip(const Context& context, const Clip& clip, std::int64_t clip_start,
                                    std::int64_t clip_in, std::int64_t clip_out, std::int64_t first,
                                    std::int64_t count, int depth, std::unordered_set<std::string>& visiting) {
  const auto rate = context.config.sample_rate;
  SourceMapping mapping;
  mapping.reversed = clip.reversed;
  mapping.anchor = clip.reversed ? clip_out - 1 : clip_in;
  mapping.rate_numerator = clip.playback_rate.numerator();
  mapping.rate_denominator = clip.playback_rate.denominator();
  mapping.time_remap = TimeRemapOf(clip);
  std::vector<float> stretch_weights;   // set for a pitch-keeping remapped clip whose block is partly or wholly stretchable
  mapping.source_in = clip_in;
  mapping.sample_rate = rate;
  if (mapping.time_remap != nullptr) {
    mapping.rate_numerator = 1;
    mapping.rate_denominator = 1'000'000;
    ++context.statistics.time_remap_blocks;
    if (clip.maintain_pitch) {
      // Per sample, how much comes from the stretch (see RemapStretchWeights); the rest is varispeed, below.
      stretch_weights = RemapStretchWeights(*mapping.time_remap, rate, first - clip_start, count);
      for (std::int64_t n = 0; n < count; ++n) {
        // Before the clip's own start there is nothing to stretch: that is a transition's lead-in, varispeed as ever.
        if (first - clip_start + n < 0) stretch_weights[static_cast<std::size_t>(n)] = 0.0f;
      }
      const auto any = std::any_of(stretch_weights.begin(), stretch_weights.end(), [](float w) { return w > 0.0f; });
      const auto all = std::all_of(stretch_weights.begin(), stretch_weights.end(), [](float w) { return w >= 1.0f; });
      if (any) ++context.statistics.remap_stretched_blocks;
      if (!all) ++context.statistics.stretch_fallbacks;
      if (all) {
        return ReadStretched(context, clip, clip_start, clip_in, clip_out, 1.0, mapping.time_remap, first, count, depth, visiting);
      }
      if (!any) stretch_weights.clear();
    }
  }
  if (mapping.rate_numerator <= 0 || mapping.rate_denominator <= 0) return std::nullopt;

  if (mapping.time_remap == nullptr && clip.maintain_pitch && mapping.rate_numerator != mapping.rate_denominator) {
    const double speed = static_cast<double>(mapping.rate_numerator) / static_cast<double>(mapping.rate_denominator);
    if (CanStretch(speed)) {
      return ReadStretched(context, clip, clip_start, clip_in, clip_out, speed, nullptr, first, count, depth, visiting);
    }
    ++context.statistics.stretch_fallbacks;  // varispeed below
  }

  std::int64_t whole_first = 0, whole_last = 0, fraction = 0;
  mapping.At(first - clip_start, whole_first, fraction);
  whole_last = whole_first;
  // A remap may freeze or reverse within a block, so its extrema are not
  // necessarily at the block edges. Scan the positions before one contiguous
  // source read; this keeps the resolver call count independent of ramp shape.
  if (mapping.time_remap != nullptr) {
    for (std::int64_t n = 1; n < count; ++n) {
      std::int64_t position = 0;
      mapping.At(first - clip_start + n, position, fraction);
      whole_first = std::min(whole_first, position);
      whole_last = std::max(whole_last, position);
    }
  } else {
    mapping.At(first - clip_start + count - 1, whole_last, fraction);
  }
  // The cubic reads one sample before and two after the position.
  const auto low = std::min(whole_first, whole_last) - 1;
  const auto high = std::max(whole_first, whole_last) + 2;
  const auto span = high - low + 1;

  auto source = ReadSource(context, clip, low, span, depth, visiting);
  if (!source.has_value()) return std::nullopt;

  auto piece = AudioBuffer::Allocate(rate, context.config.channels, count);
  const auto limit = source->frames();
  const auto fetch = [&](const float* samples, std::int64_t index) {
    const auto offset = index - low;
    return offset >= 0 && offset < limit ? samples[offset] : 0.0f;
  };
  for (int channel = 0; channel < piece.channels(); ++channel) {
    const auto* from = source->channel(channel);
    auto* to = piece.channel(channel);
    for (std::int64_t n = 0; n < count; ++n) {
      std::int64_t whole = 0, remainder = 0;
      mapping.At(first - clip_start + n, whole, remainder);
      if (remainder == 0) {
        to[n] = fetch(from, whole);
      } else {
        to[n] = Cubic(fetch(from, whole - 1), fetch(from, whole), fetch(from, whole + 1), fetch(from, whole + 2),
                      static_cast<double>(remainder) / static_cast<double>(mapping.rate_denominator));
      }
    }
  }
  if (!stretch_weights.empty()) {
    // Where the source runs at a speed that can be stretched, the stretched signal replaces the varispeed one.
    auto stretched = ReadStretched(context, clip, clip_start, clip_in, clip_out, 1.0, mapping.time_remap, first, count, depth, visiting);
    if (!stretched.has_value()) return std::nullopt;
    for (int channel = 0; channel < piece.channels(); ++channel) {
      auto* to = piece.channel(channel);
      const auto* from = stretched->channel(channel);
      for (std::int64_t n = 0; n < count; ++n) {
        const auto weight = stretch_weights[static_cast<std::size_t>(n)];
        if (weight > 0.0f) to[n] = weight * from[n] + (1.0f - weight) * to[n];
      }
    }
  }
  return piece;
}

// How a track's clips are laid out on the sample grid, for one sequence.
struct PlacedClip final {
  const Clip* clip{};
  std::int64_t start{}, end{}, in{}, out{};
  std::int64_t window_start{}, window_end{};
  std::vector<TransitionRole> roles;
};

// Everything on a track that can sound in [first, first + count): the clips, each
// through its own effects, weighted by its transitions, summed.
AudioBuffer SumTrackClips(const Context& context, const Sequence& sequence, const Track& track, std::int64_t first_sample,
                          std::int64_t frames, int depth, std::unordered_set<std::string>& visiting,
                          const SidechainFetch& sidechain) {
  const auto rate = context.config.sample_rate;
  const auto block_end = first_sample + frames;

  // Only the clips that can sound in this block are looked at: a transition
  // lets a clip play at most as far beyond its own edges as the transition is
  // long, so the search window is the block widened by the longest transition.
  std::int64_t reach_ticks = 0;
  for (const auto& transition : track.transitions) {
    reach_ticks = std::max(reach_ticks, transition.end_ticks - transition.start_ticks);
  }
  const auto low_ticks = RationalTime(first_sample - 1, rate).ToTicks() - reach_ticks;
  const auto high_ticks = RationalTime(block_end + 1, rate).ToTicks() + reach_ticks;
  const auto first_candidate = std::partition_point(
      track.clips.begin(), track.clips.end(), [&](const Clip& clip) { return clip.end_ticks <= low_ticks; });

  std::vector<PlacedClip> placed;
  for (auto candidate = first_candidate; candidate != track.clips.end() && candidate->start_ticks < high_ticks;
       ++candidate) {
    const auto& clip = *candidate;
    if (!clip.enabled) continue;
    PlacedClip entry;
    entry.clip = &clip;
    entry.start = ToSample(clip.timeline_start, rate);
    entry.end = ToSample(clip.end(), rate);
    entry.in = ToSample(clip.source_in, rate);
    entry.out = ToSample(clip.source_out, rate);
    entry.window_start = entry.start;
    entry.window_end = entry.end;
    placed.push_back(std::move(entry));
  }

  for (const auto& transition : track.transitions) {
    const auto start = ToSample(transition.timeline_start, rate);
    const auto end = ToSample(transition.timeline_start.Add(transition.duration), rate);
    const bool two_sided = transition.from_clip_id.has_value() && transition.to_clip_id.has_value();
    const bool touches_block = start < block_end && end > first_sample;
    for (auto& entry : placed) {
      const bool outgoing = transition.from_clip_id.has_value() && *transition.from_clip_id == entry.clip->id;
      const bool incoming = transition.to_clip_id.has_value() && *transition.to_clip_id == entry.clip->id;
      if (!outgoing && !incoming) continue;
      TransitionRole role;
      role.transition = &transition;
      role.outgoing = outgoing;
      role.two_sided = two_sided;
      role.start = start;
      role.end = end;
      role.constant_power = transition.kind == "constant_power" && context.semantics.equal_power_crossfades;
      entry.roles.push_back(role);
      // A two-sided transition plays each clip's media beyond the cut.
      if (two_sided) {
        if (outgoing) entry.window_end = std::max(entry.window_end, end);
        if (incoming) entry.window_start = std::min(entry.window_start, start);
      }
    }
    if (touches_block) ++context.statistics.transitions_mixed;
  }

  AudioBuffer track_mix = AudioBuffer::Allocate(rate, context.config.channels, frames);
  for (const auto& entry : placed) {
    const auto a = std::max(entry.window_start, first_sample);
    const auto b = std::min(entry.window_end, block_end);
    if (a >= b) continue;
    const auto count = b - a;

    // The clip's own signal over any range: its source, played at its rate, and
    // nothing outside the span it can sound in. Effects with memory ask for ranges
    // wider than the one being rendered, and what they read before the clip began is
    // silence, which is what a clip's own filter state is at its first sample.
    bool missing = false;
    const Fetch base = [&](std::int64_t from, std::int64_t n) {
      auto out = AudioBuffer::Allocate(rate, context.config.channels, n);
      const auto lo = std::max(from, entry.window_start);
      const auto hi = std::min(from + n, entry.window_end);
      if (hi <= lo) return out;
      auto read = ReadClip(context, *entry.clip, entry.start, entry.in, entry.out, lo, hi - lo, depth, visiting);
      if (!read.has_value()) {
        missing = true;
        return out;
      }
      for (int channel = 0; channel < out.channels(); ++channel) {
        std::copy_n(read->channel(channel), hi - lo, out.channel(channel) + (lo - from));
      }
      return out;
    };
    const StackInputs inputs{base, sidechain, entry.start, sequence.id + "/" + track.id + "/" + entry.clip->id};
    auto piece = ApplyEffectStack(context, entry.clip->effects, inputs, a, count);
    if (missing) {
      // Silence past the media's end is not a missing source; an absent one is.
      if (a < entry.end && b > entry.start) ++context.statistics.missing_sources;
      continue;
    }

    // Per-sample weight: 1 inside the clip, shaped by every transition touching
    // it, and 0 outside the clip unless a two-sided transition is playing its
    // handle.
    for (std::int64_t n = 0; n < count; ++n) {
      const auto sample = a + n;
      const bool inside = sample >= entry.start && sample < entry.end;
      double weight = inside ? 1.0 : 0.0;
      for (const auto& role : entry.roles) {
        if (sample < role.start || sample >= role.end) {
          // Outside the transition: the outgoing clip is gone after it and the
          // incoming one is absent before it.
          if (role.outgoing ? sample >= role.end : sample < role.start) weight = 0.0;
          continue;
        }
        // Inside it. The clip's handle is only real when the transition is
        // two-sided and the sample falls on the far side of the clip's own edge.
        const bool handle = role.two_sided && (role.outgoing ? sample >= entry.end : sample < entry.start);
        weight = (inside || handle) ? (inside ? weight : 1.0) * role.Weight(sample) : 0.0;
      }
      if (weight == 1.0) continue;
      for (int channel = 0; channel < piece.channels(); ++channel) {
        piece.channel(channel)[n] = static_cast<float>(piece.channel(channel)[n] * weight);
      }
    }

    const auto offset = a - first_sample;
    for (int channel = 0; channel < track_mix.channels(); ++channel) {
      auto* to = track_mix.channel(channel) + offset;
      const auto* from = piece.channel(channel);
      for (std::int64_t n = 0; n < count; ++n) to[n] += from[n];
    }
    ++context.statistics.sources_mixed;
  }
  return track_mix;
}

// What is routed into a bus, summed: the tracks whose output it is and their sends to it.
using BusFetch = std::function<AudioBuffer(const Track& bus, std::int64_t first, std::int64_t count)>;

// A track's finished signal: its clips (or, for a bus, what is routed to it), its own
// effects, then its fader and pan. As a side-chain key it is tapped before the fader,
// so a muted or pulled-down dialogue track still keys the music under it.
AudioBuffer RenderTrackOutput(const Context& context, const Sequence& sequence, const Track& track,
                              std::int64_t first_sample, std::int64_t frames, int depth,
                              std::unordered_set<std::string>& visiting, const SidechainFetch& sidechain,
                              const BusFetch& buses, bool pre_fader = false) {
  const Fetch base = [&](std::int64_t from, std::int64_t n) {
    if (track.is_bus) return buses(track, from, n);
    return SumTrackClips(context, sequence, track, from, n, depth, visiting, sidechain);
  };
  const StackInputs inputs{base, sidechain, 0, sequence.id + "/" + track.id};
  auto signal = ApplyEffectStack(context, track.effects, inputs, first_sample, frames);
  if (!pre_fader) {
    ApplyTrackStrip(signal, track);
    // Meter the samples already in hand. Keep this deliberately lighter than the
    // full BS.1770 loudness pass used for delivery analysis: a realtime channel
    // strip needs peak and RMS, and must not decode or mix the track again.
    double energy = 0.0;
    float peak = 0.0f;
    for (int channel = 0; channel < signal.channels(); ++channel) {
      const auto* samples = signal.channel(channel);
      for (std::int64_t frame = 0; frame < signal.frames(); ++frame) {
        peak = std::max(peak, std::abs(samples[frame]));
        energy += static_cast<double>(samples[frame]) * samples[frame];
      }
    }
    TrackLevel level;
    if (peak > 0.0f) level.peak_db = dsp::DecibelsOf(peak);
    const auto count = static_cast<double>(signal.frames()) * signal.channels();
    if (energy > 0.0 && count > 0.0) level.rms_db = dsp::DecibelsOf(std::sqrt(energy / count));
    context.statistics.track_levels[track.id] = level;
  }
  return signal;
}

AudioBuffer RenderSequence(const Context& context, const Sequence& sequence, std::int64_t first_sample,
                           std::int64_t frames, int depth, std::unordered_set<std::string>& visiting,
                           const std::string* only_track) {
  auto output = AudioBuffer::Allocate(context.config.sample_rate, context.config.channels, frames);
  output.presentation_time = RationalTime(first_sample, context.config.sample_rate);
  // Cycles are rejected when the edit is made; this is the second line of defence
  // so a hand-built or corrupted graph cannot hang the audio thread.
  if (!visiting.insert(sequence.id).second) return output;

  // A track other than the one being rendered can be asked for as a side-chain.
  // Asking for one that is already being rendered (a track keyed by itself, or two
  // keyed by each other) finds nothing rather than recursing for ever.
  std::unordered_set<std::string> rendering;
  SidechainFetch sidechain;
  BusFetch buses;
  const auto audible = [&](const Track& track) {
    return track.kind == model::TrackKind::Audio && timeline::TrackContributes(sequence, track, context.options);
  };
  sidechain = [&](const std::string& id, std::int64_t from, std::int64_t n) -> std::optional<AudioBuffer> {
    const auto* source = sequence.FindTrack(id);
    if (source == nullptr || source->kind != model::TrackKind::Audio) return std::nullopt;
    if (!rendering.insert(id).second) return std::nullopt;
    auto signal = RenderTrackOutput(context, sequence, *source, from, n, depth, visiting, sidechain, buses, true);
    rendering.erase(id);
    return signal;
  };

  // A bus is the sum of the tracks routed to it, in track order so the sum is the
  // same every time, plus their sends at the send's gain. A muted track, or one
  // silenced by another's solo, sends nothing either. The store refuses a loop; if
  // one arrives anyway, the bus it would re-enter is silent and the statistics say so.
  buses = [&](const Track& bus, std::int64_t from, std::int64_t n) {
    auto sum = AudioBuffer::Allocate(context.config.sample_rate, context.config.channels, n);
    if (!rendering.insert("bus:" + bus.id).second) {
      ++context.statistics.missing_sidechains;
      return sum;
    }
    const auto add = [&](const AudioBuffer& signal, double gain) {
      for (int channel = 0; channel < sum.channels(); ++channel) {
        auto* to = sum.channel(channel);
        const auto* source = signal.channel(channel);
        for (std::int64_t i = 0; i < n; ++i) to[i] += static_cast<float>(source[i] * gain);
      }
    };
    for (const auto& source : sequence.tracks) {
      if (&source == &bus || source.kind != model::TrackKind::Audio || !audible(source)) continue;
      if (source.output_bus_id == bus.id) {
        rendering.insert(source.id);
        add(RenderTrackOutput(context, sequence, source, from, n, depth, visiting, sidechain, buses), 1.0);
        rendering.erase(source.id);
      }
      for (const auto& send : source.sends) {
        if (send.bus_id != bus.id) continue;
        rendering.insert(source.id);
        add(RenderTrackOutput(context, sequence, source, from, n, depth, visiting, sidechain, buses, send.pre_fader),
            std::pow(10.0, send.gain_db / 20.0));
        rendering.erase(source.id);
      }
    }
    rendering.erase("bus:" + bus.id);
    return sum;
  };

  if (only_track != nullptr) {
    // One track's own signal, as its meter sees it: after its fader, before the master.
    const auto* wanted = sequence.FindTrack(*only_track);
    if (wanted == nullptr || wanted->kind != model::TrackKind::Audio) {
      visiting.erase(sequence.id);
      throw std::invalid_argument("No audio track " + *only_track + " in the sequence");
    }
    if (audible(*wanted)) {
      rendering.insert(wanted->id);
      output = RenderTrackOutput(context, sequence, *wanted, first_sample, frames, depth, visiting, sidechain, buses);
      output.presentation_time = RationalTime(first_sample, context.config.sample_rate);
    }
    visiting.erase(sequence.id);
    return output;
  }
  const Fetch mix = [&](std::int64_t from, std::int64_t n) {
    auto sum = AudioBuffer::Allocate(context.config.sample_rate, context.config.channels, n);
    for (const auto& track : sequence.tracks) {
      if (!audible(track)) continue;
      // Only what is not routed into a bus reaches the master directly.
      if (!track.output_bus_id.empty() && sequence.FindTrack(track.output_bus_id) != nullptr) continue;
      rendering.insert(track.id);
      const auto signal = RenderTrackOutput(context, sequence, track, from, n, depth, visiting, sidechain, buses);
      rendering.erase(track.id);
      for (int channel = 0; channel < sum.channels(); ++channel) {
        auto* to = sum.channel(channel);
        const auto* source = signal.channel(channel);
        for (std::int64_t i = 0; i < n; ++i) to[i] += source[i];
      }
    }
    return sum;
  };

  // Sequence-level effects apply to the finished mix of this sequence and no other.
  const StackInputs inputs{mix, sidechain, 0, sequence.id};
  auto finished = ApplyEffectStack(context, sequence.effects, inputs, first_sample, frames);
  finished.presentation_time = output.presentation_time;
  visiting.erase(sequence.id);
  return finished;
}

}  // namespace

bool IsBuiltInAudioEffect(const std::string& effect_type) {
  const auto* descriptor = effects::FindEffect(effect_type);
  return descriptor != nullptr && descriptor->medium == effects::Medium::Audio && descriptor->cpu_available;
}

media::AudioBuffer AudioMixer::MixSamples(const timeline::SequenceGraph& graph, std::int64_t first_sample,
                                          std::int64_t frames, const AudioResolver& resolve,
                                          MixStatistics& statistics, const timeline::CompileOptions& options) const {
  const auto* root = graph.root();
  if (root == nullptr) throw std::invalid_argument("Sequence graph is empty");
  if (frames <= 0) return AudioBuffer::Allocate(config_.sample_rate, config_.channels, 0);

  // One render, one set of rules: the root sequence's, whatever it nests.
  const Context context{model::RenderSemantics::For(root->render_version), graph, resolve, statistics, options, config_,
                        std::make_shared<StretchCache>()};
  std::unordered_set<std::string> visiting;
  auto output = RenderSequence(context, *root, first_sample, frames, 0, visiting);

  for (int channel = 0; channel < output.channels(); ++channel) {
    statistics.peak = std::max(statistics.peak, output.Peak(channel));
  }
  if (statistics.peak > 1.0f) {
    statistics.clipped = true;
    if (config_.limit_output) {
      for (int channel = 0; channel < output.channels(); ++channel) {
        auto* samples = output.channel(channel);
        for (std::int64_t frame = 0; frame < output.frames(); ++frame) {
          samples[frame] = std::clamp(samples[frame], -1.0f, 1.0f);
        }
      }
    }
  }
  return output;
}

media::AudioBuffer AudioMixer::MixTrack(const timeline::SequenceGraph& graph, const std::string& track_id,
                                        std::int64_t first_sample, std::int64_t frames, const AudioResolver& resolve,
                                        MixStatistics& statistics, const timeline::CompileOptions& options) const {
  const auto* root = graph.root();
  if (root == nullptr) throw std::invalid_argument("Sequence graph is empty");
  if (frames <= 0) return AudioBuffer::Allocate(config_.sample_rate, config_.channels, 0);
  const Context context{model::RenderSemantics::For(root->render_version), graph, resolve, statistics, options, config_,
                        std::make_shared<StretchCache>()};
  std::unordered_set<std::string> visiting;
  return RenderSequence(context, *root, first_sample, frames, 0, visiting, &track_id);
}

LevelReading MeasureLevels(const media::AudioBuffer& buffer) {
  LevelReading reading;
  if (buffer.frames() <= 0) return reading;
  double energy = 0.0;
  for (int channel = 0; channel < buffer.channels(); ++channel) {
    const auto* samples = buffer.channel(channel);
    for (std::int64_t frame = 0; frame < buffer.frames(); ++frame) energy += static_cast<double>(samples[frame]) * samples[frame];
  }
  const auto count = static_cast<double>(buffer.frames()) * buffer.channels();
  if (energy > 0.0) reading.rms_db = dsp::DecibelsOf(std::sqrt(energy / count));
  const auto loudness = dsp::MeasureLoudness(buffer);
  reading.peak_db = loudness.sample_peak_db;
  reading.true_peak_db = loudness.true_peak_db;
  reading.momentary_lufs = loudness.momentary_lufs;
  reading.short_term_lufs = loudness.short_term_lufs;
  reading.integrated_lufs = loudness.integrated_lufs;
  return reading;
}

media::AudioBuffer AudioMixer::MixSamples(const timeline::SequenceGraph& graph, std::int64_t first_sample,
                                          std::int64_t frames, const AudioResolver& resolve) const {
  MixStatistics ignored;
  return MixSamples(graph, first_sample, frames, resolve, ignored);
}

media::AudioBuffer AudioMixer::Mix(const timeline::SequenceGraph& graph, const time::RationalTime& at,
                                   std::int64_t frames, const AudioResolver& resolve, MixStatistics& statistics,
                                   const timeline::CompileOptions& options) const {
  return MixSamples(graph, ToSample(at, config_.sample_rate), frames, resolve, statistics, options);
}

media::AudioBuffer AudioMixer::Mix(const timeline::SequenceGraph& graph, const time::RationalTime& at,
                                   std::int64_t frames, const AudioResolver& resolve) const {
  MixStatistics ignored;
  return Mix(graph, at, frames, resolve, ignored);
}

}  // namespace cutline::audio

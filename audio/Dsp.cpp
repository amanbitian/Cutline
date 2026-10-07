#include "audio/Dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <limits>
#include <numbers>

namespace cutline::audio::dsp {
namespace {

using media::AudioBuffer;

constexpr double kPi = std::numbers::pi;

[[nodiscard]] double ToLinear(double db) { return std::pow(10.0, db / 20.0); }
[[nodiscard]] std::int64_t Samples(double milliseconds, double rate) {
  return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(milliseconds * rate / 1000.0)));
}

// Sliding-window minimum of v over [n - back, n + ahead], with `outside` beyond the ends.
[[nodiscard]] std::vector<double> SlidingMin(const std::vector<double>& v, std::int64_t back, std::int64_t ahead,
                                             double outside) {
  const auto count = static_cast<std::int64_t>(v.size());
  std::vector<double> out(v.size());
  std::deque<std::int64_t> window;
  std::int64_t next = 0;  // next index to push
  const auto value = [&](std::int64_t i) { return i < 0 || i >= count ? outside : v[static_cast<std::size_t>(i)]; };
  for (std::int64_t n = 0; n < count; ++n) {
    const auto high = n + ahead;
    while (next <= high) {
      const auto incoming = value(next);
      while (!window.empty() && value(window.back()) >= incoming) window.pop_back();
      window.push_back(next);
      ++next;
    }
    while (!window.empty() && window.front() < n - back) window.pop_front();
    out[static_cast<std::size_t>(n)] = window.empty() ? outside : value(window.front());
  }
  return out;
}

[[nodiscard]] std::vector<double> SlidingMax(const std::vector<double>& v, std::int64_t back, std::int64_t ahead,
                                             double outside) {
  std::vector<double> negated(v.size());
  for (std::size_t i = 0; i < v.size(); ++i) negated[i] = -v[i];
  auto out = SlidingMin(negated, back, ahead, -outside);
  for (auto& x : out) x = -x;
  return out;
}

// Centred moving average of `window` samples. Beyond the ends the edge value is
// repeated, so that an average over a gain curve never reaches past what the
// curve itself says at the edge (padding with a constant would let it).
[[nodiscard]] std::vector<double> MovingAverage(const std::vector<double>& v, std::int64_t window, double /*unused*/) {
  const auto count = static_cast<std::int64_t>(v.size());
  if (window <= 1 || count == 0) return v;
  const double outside_low = v.front();
  const double outside_high = v.back();
  const auto back = window / 2;
  const auto ahead = window - 1 - back;
  std::vector<double> prefix(static_cast<std::size_t>(count) + 1, 0.0);
  for (std::int64_t i = 0; i < count; ++i) prefix[static_cast<std::size_t>(i) + 1] = prefix[static_cast<std::size_t>(i)] + v[static_cast<std::size_t>(i)];
  std::vector<double> out(v.size());
  for (std::int64_t n = 0; n < count; ++n) {
    const auto lo = n - back;
    const auto hi = n + ahead;  // inclusive
    const auto clamped_lo = std::max<std::int64_t>(lo, 0);
    const auto clamped_hi = std::min<std::int64_t>(hi, count - 1);
    double sum = clamped_hi >= clamped_lo
                     ? prefix[static_cast<std::size_t>(clamped_hi) + 1] - prefix[static_cast<std::size_t>(clamped_lo)]
                     : 0.0;
    sum += outside_low * static_cast<double>(std::max<std::int64_t>(0, -lo));
    sum += outside_high * static_cast<double>(std::max<std::int64_t>(0, hi - (count - 1)));
    out[static_cast<std::size_t>(n)] = sum / static_cast<double>(window);
  }
  return out;
}

// Gain reduction in dB (<= 0) with its release and attack made finite. Release is a
// linear recovery of `release_rate` dB per sample, written as a sliding minimum so
// the value at a sample depends only on the last `span` samples; attack is a centred
// moving average, so the reduction arrives before the event that causes it.
[[nodiscard]] std::vector<double> ShapeReduction(const std::vector<double>& reduction, std::int64_t attack,
                                                 double release_rate, std::int64_t span) {
  const auto count = static_cast<std::int64_t>(reduction.size());
  std::vector<double> shifted(reduction.size());
  for (std::int64_t m = 0; m < count; ++m) shifted[static_cast<std::size_t>(m)] = reduction[static_cast<std::size_t>(m)] - release_rate * static_cast<double>(m);
  // Outside the block there is no reduction: the shifted equivalent of 0 dB.
  auto window = SlidingMin(shifted, span, 0, std::numeric_limits<double>::infinity());
  std::vector<double> released(reduction.size());
  for (std::int64_t n = 0; n < count; ++n) {
    released[static_cast<std::size_t>(n)] =
        std::min(0.0, window[static_cast<std::size_t>(n)] + release_rate * static_cast<double>(n));
  }
  return MovingAverage(released, attack, 0.0);
}

// Level in dB of the detector signal (loudest channel), as RMS over `window`
// samples ending at each sample.
[[nodiscard]] std::vector<double> DetectorLevelDb(const AudioBuffer& detector, std::int64_t window) {
  const auto count = detector.frames();
  std::vector<double> power(static_cast<std::size_t>(count), 0.0);
  for (int channel = 0; channel < detector.channels(); ++channel) {
    const auto* samples = detector.channel(channel);
    for (std::int64_t n = 0; n < count; ++n) {
      power[static_cast<std::size_t>(n)] = std::max(power[static_cast<std::size_t>(n)], static_cast<double>(samples[n]) * samples[n]);
    }
  }
  // Causal window: average of power[n - window + 1 .. n].
  std::vector<double> prefix(static_cast<std::size_t>(count) + 1, 0.0);
  for (std::int64_t n = 0; n < count; ++n) prefix[static_cast<std::size_t>(n) + 1] = prefix[static_cast<std::size_t>(n)] + power[static_cast<std::size_t>(n)];
  std::vector<double> level(static_cast<std::size_t>(count));
  for (std::int64_t n = 0; n < count; ++n) {
    const auto lo = std::max<std::int64_t>(0, n - window + 1);
    const double mean = (prefix[static_cast<std::size_t>(n) + 1] - prefix[static_cast<std::size_t>(lo)]) / static_cast<double>(window);
    level[static_cast<std::size_t>(n)] = 10.0 * std::log10(mean + 1e-12);
  }
  return level;
}

void ApplyGainCurve(AudioBuffer& buffer, const std::vector<double>& gain_db, double extra_db = 0.0) {
  for (int channel = 0; channel < buffer.channels(); ++channel) {
    auto* samples = buffer.channel(channel);
    for (std::int64_t n = 0; n < buffer.frames(); ++n) {
      samples[n] = static_cast<float>(samples[n] * ToLinear(gain_db[static_cast<std::size_t>(n)] + extra_db));
    }
  }
}

}  // namespace

double DecibelsOf(double linear) { return 20.0 * std::log10(std::max(std::abs(linear), 1e-12)); }

// --------------------------------------------------------------- filters ----

Biquad Peaking(double fs, double f, double gain_db, double q) {
  const double a = std::pow(10.0, gain_db / 40.0);
  const double w = 2.0 * kPi * f / fs;
  const double alpha = std::sin(w) / (2.0 * q);
  const double a0 = 1.0 + alpha / a;
  return {(1.0 + alpha * a) / a0, -2.0 * std::cos(w) / a0, (1.0 - alpha * a) / a0, -2.0 * std::cos(w) / a0,
          (1.0 - alpha / a) / a0};
}

Biquad LowShelf(double fs, double f, double gain_db, double slope) {
  const double a = std::pow(10.0, gain_db / 40.0);
  const double w = 2.0 * kPi * f / fs;
  const double cw = std::cos(w);
  const double alpha = std::sin(w) / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0);
  const double beta = 2.0 * std::sqrt(a) * alpha;
  const double a0 = (a + 1.0) + (a - 1.0) * cw + beta;
  return {a * ((a + 1.0) - (a - 1.0) * cw + beta) / a0, 2.0 * a * ((a - 1.0) - (a + 1.0) * cw) / a0,
          a * ((a + 1.0) - (a - 1.0) * cw - beta) / a0, -2.0 * ((a - 1.0) + (a + 1.0) * cw) / a0,
          ((a + 1.0) + (a - 1.0) * cw - beta) / a0};
}

Biquad HighShelf(double fs, double f, double gain_db, double slope) {
  const double a = std::pow(10.0, gain_db / 40.0);
  const double w = 2.0 * kPi * f / fs;
  const double cw = std::cos(w);
  const double alpha = std::sin(w) / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0);
  const double beta = 2.0 * std::sqrt(a) * alpha;
  const double a0 = (a + 1.0) - (a - 1.0) * cw + beta;
  return {a * ((a + 1.0) + (a - 1.0) * cw + beta) / a0, -2.0 * a * ((a - 1.0) + (a + 1.0) * cw) / a0,
          a * ((a + 1.0) + (a - 1.0) * cw - beta) / a0, 2.0 * ((a - 1.0) - (a + 1.0) * cw) / a0,
          ((a + 1.0) - (a - 1.0) * cw - beta) / a0};
}

Biquad LowPass(double fs, double f, double q) {
  const double w = 2.0 * kPi * f / fs;
  const double alpha = std::sin(w) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  return {(1.0 - std::cos(w)) / 2.0 / a0, (1.0 - std::cos(w)) / a0, (1.0 - std::cos(w)) / 2.0 / a0,
          -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
}

Biquad HighPass(double fs, double f, double q) {
  const double w = 2.0 * kPi * f / fs;
  const double alpha = std::sin(w) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  return {(1.0 + std::cos(w)) / 2.0 / a0, -(1.0 + std::cos(w)) / a0, (1.0 + std::cos(w)) / 2.0 / a0,
          -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
}

double ResponseDb(const Biquad& filter, double fs, double frequency) {
  const auto w = 2.0 * kPi * frequency / fs;
  const std::complex<double> z1 = std::polar(1.0, -w);
  const std::complex<double> z2 = std::polar(1.0, -2.0 * w);
  const auto numerator = filter.b0 + filter.b1 * z1 + filter.b2 * z2;
  const auto denominator = 1.0 + filter.a1 * z1 + filter.a2 * z2;
  return 20.0 * std::log10(std::abs(numerator / denominator));
}

void Filter(float* samples, std::int64_t count, const Biquad& f) {
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (std::int64_t n = 0; n < count; ++n) {
    const double x = samples[n];
    const double y = f.b0 * x + f.b1 * x1 + f.b2 * x2 - f.a1 * y1 - f.a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    samples[n] = static_cast<float>(y);
  }
}

// ------------------------------------------------------------- processors ----

void ApplyEq(AudioBuffer& buffer, const EqSettings& s) {
  const double fs = static_cast<double>(buffer.sample_rate());
  std::vector<Biquad> bands;
  if (std::abs(s.low_gain_db) > 1e-6) bands.push_back(LowShelf(fs, s.low_frequency, s.low_gain_db));
  if (std::abs(s.mid_gain_db) > 1e-6) bands.push_back(Peaking(fs, s.mid_frequency, s.mid_gain_db, s.mid_q));
  if (std::abs(s.high_gain_db) > 1e-6) bands.push_back(HighShelf(fs, s.high_frequency, s.high_gain_db));
  for (int channel = 0; channel < buffer.channels(); ++channel) {
    for (const auto& band : bands) Filter(buffer.channel(channel), buffer.frames(), band);
  }
}

namespace {
constexpr double kReductionSpan = 60.0;  // dB of reduction whose recovery is modelled in full
constexpr double kReferenceRecovery = 20.0;  // a release time is the time to recover this many dB
}  // namespace

void Compress(AudioBuffer& buffer, const AudioBuffer& detector, const CompressorSettings& s) {
  const double fs = static_cast<double>(buffer.sample_rate());
  const auto level = DetectorLevelDb(detector, Samples(5.0, fs));
  std::vector<double> reduction(level.size());
  const double knee = std::max(s.knee_db, 0.0);
  const double slope = 1.0 / std::max(s.ratio, 1.0) - 1.0;
  for (std::size_t i = 0; i < level.size(); ++i) {
    const double x = level[i] - s.threshold_db;
    double over;  // how far the level is over the threshold after the curve, minus before
    if (2.0 * x < -knee) {
      over = 0.0;
    } else if (knee > 0.0 && 2.0 * std::abs(x) <= knee) {
      over = slope * (x + knee / 2.0) * (x + knee / 2.0) / (2.0 * knee);
    } else {
      over = slope * x;
    }
    reduction[i] = std::min(0.0, over);
  }
  const double rate = kReferenceRecovery / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto shaped = ShapeReduction(reduction, Samples(s.attack_ms, fs), rate, span);
  ApplyGainCurve(buffer, shaped, s.makeup_db);
}

void Limit(AudioBuffer& buffer, const LimiterSettings& s) {
  const double fs = static_cast<double>(buffer.sample_rate());
  const auto count = buffer.frames();
  const auto look = Samples(s.lookahead_ms, fs);
  std::vector<double> required(static_cast<std::size_t>(count), 0.0);
  for (int channel = 0; channel < buffer.channels(); ++channel) {
    const auto* samples = buffer.channel(channel);
    for (std::int64_t n = 0; n < count; ++n) {
      const double peak_db = DecibelsOf(samples[n]);
      required[static_cast<std::size_t>(n)] = std::min(required[static_cast<std::size_t>(n)], s.ceiling_db - peak_db);
    }
  }
  // Every sample is protected by the deepest reduction anywhere within the lookahead
  // either side of it; recovery is slope-limited; then the whole curve is smoothed
  // over the lookahead. Each averaged value is at most the reduction the sample under
  // it needs, so the ceiling holds.
  const auto protectedCurve = SlidingMin(required, look, look, 0.0);
  const double rate = kReferenceRecovery / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto shaped = ShapeReduction(protectedCurve, look + 1, rate, span);
  ApplyGainCurve(buffer, shaped);
}

void Gate(AudioBuffer& buffer, const AudioBuffer& detector, const GateSettings& s) {
  const double fs = static_cast<double>(buffer.sample_rate());
  const auto level = DetectorLevelDb(detector, Samples(5.0, fs));
  const auto count = static_cast<std::int64_t>(level.size());
  std::vector<double> open(level.size());
  for (std::size_t i = 0; i < level.size(); ++i) open[i] = level[i] > s.threshold_db ? 1.0 : 0.0;
  // Hold: stay open for a while after the signal falls.
  const auto held = SlidingMax(open, Samples(s.hold_ms, fs), 0, 0.0);
  std::vector<double> reduction(level.size());
  for (std::int64_t n = 0; n < count; ++n) reduction[static_cast<std::size_t>(n)] = held[static_cast<std::size_t>(n)] > 0.5 ? 0.0 : std::min(0.0, s.range_db);
  const double depth = std::max(std::abs(s.range_db), 1.0);
  const double rate = depth / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto shaped = ShapeReduction(reduction, Samples(s.attack_ms, fs), rate, span);
  ApplyGainCurve(buffer, shaped);
}

void Duck(AudioBuffer& buffer, const AudioBuffer& detector, const DuckSettings& s) {
  const double fs = static_cast<double>(buffer.sample_rate());
  const auto level = DetectorLevelDb(detector, Samples(10.0, fs));
  std::vector<double> reduction(level.size());
  for (std::size_t i = 0; i < level.size(); ++i) {
    // Full reduction from 6 dB above the threshold, none below it.
    const double t = std::clamp((level[i] - s.threshold_db) / 6.0, 0.0, 1.0);
    reduction[i] = std::min(0.0, s.reduction_db) * t;
  }
  const double depth = std::max(std::abs(s.reduction_db), 1.0);
  const double rate = depth / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto shaped = ShapeReduction(reduction, Samples(s.attack_ms, fs), rate, span);
  ApplyGainCurve(buffer, shaped);
}

// ------------------------------------------------------------- spectral ----

namespace {

constexpr int kFrame = 2048;
constexpr int kFrameHop = 512;

void Fft(std::vector<std::complex<float>>& a, bool inverse) {
  const auto n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t length = 2; length <= n; length <<= 1) {
    const double angle = 2.0 * kPi / static_cast<double>(length) * (inverse ? 1.0 : -1.0);
    const std::complex<float> step(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
    for (std::size_t i = 0; i < n; i += length) {
      std::complex<float> w(1.0f, 0.0f);
      for (std::size_t j = 0; j < length / 2; ++j) {
        const auto u = a[i + j];
        const auto v = a[i + j + length / 2] * w;
        a[i + j] = u + v;
        a[i + j + length / 2] = u - v;
        w *= step;
      }
    }
  }
  if (inverse) {
    for (auto& x : a) x /= static_cast<float>(n);
  }
}

// Runs `gains_for_frame` over the short-time spectrum of one channel and
// overlap-adds the result. `shape` receives, per frame, the power spectrum and
// returns the per-bin gains.
template <typename GainFn>
void SpectralProcess(float* samples, std::int64_t count, GainFn&& gains_for_frame) {
  std::vector<float> window(kFrame);
  for (int i = 0; i < kFrame; ++i) window[static_cast<std::size_t>(i)] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * i / kFrame)));
  std::vector<float> output(static_cast<std::size_t>(count), 0.0f);
  std::vector<std::complex<float>> spectrum(kFrame);
  std::vector<double> power(kFrame / 2 + 1);
  std::int64_t frame_index = 0;
  for (std::int64_t start = -(kFrame - kFrameHop); start < count; start += kFrameHop, ++frame_index) {
    for (int i = 0; i < kFrame; ++i) {
      const auto at = start + i;
      const float x = at >= 0 && at < count ? samples[at] : 0.0f;
      spectrum[static_cast<std::size_t>(i)] = {x * window[static_cast<std::size_t>(i)], 0.0f};
    }
    Fft(spectrum, false);
    for (int k = 0; k <= kFrame / 2; ++k) power[static_cast<std::size_t>(k)] = std::norm(spectrum[static_cast<std::size_t>(k)]);
    const auto gains = gains_for_frame(frame_index, power);
    for (int k = 0; k <= kFrame / 2; ++k) {
      spectrum[static_cast<std::size_t>(k)] *= static_cast<float>(gains[static_cast<std::size_t>(k)]);
      if (k > 0 && k < kFrame / 2) spectrum[static_cast<std::size_t>(kFrame - k)] = std::conj(spectrum[static_cast<std::size_t>(k)]);
    }
    Fft(spectrum, true);
    for (int i = 0; i < kFrame; ++i) {
      const auto at = start + i;
      if (at >= 0 && at < count) output[static_cast<std::size_t>(at)] += spectrum[static_cast<std::size_t>(i)].real() * window[static_cast<std::size_t>(i)] * (static_cast<float>(kFrameHop) / (kFrame / 2.0f)) * 1.0f;
    }
  }
  std::copy(output.begin(), output.end(), samples);
}

}  // namespace

void Denoise(AudioBuffer& buffer, const DenoiseSettings& s) {
  const double fs = static_cast<double>(buffer.sample_rate());
  const auto memory_frames = static_cast<std::size_t>(std::max<std::int64_t>(8, static_cast<std::int64_t>(fs * 0.8) / kFrameHop));
  const double floor_gain = std::pow(10.0, (-6.0 - 24.0 * std::clamp(s.amount, 0.0, 1.0)) / 20.0);
  for (int channel = 0; channel < buffer.channels(); ++channel) {
    std::vector<std::vector<double>> raw;       // power of the last few frames
    std::vector<std::vector<double>> history;  // smoothed power per recent frame
    constexpr std::size_t kSmoothing = 8;       // ~85 ms: enough to keep the floor off single noisy bins
    SpectralProcess(buffer.channel(channel), buffer.frames(), [&](std::int64_t, const std::vector<double>& power) {
      raw.push_back(power);
      if (raw.size() > kSmoothing) raw.erase(raw.begin());
      std::vector<double> smoothed(power.size(), 0.0);
      for (const auto& frame : raw) {
        for (std::size_t k = 0; k < power.size(); ++k) smoothed[k] += frame[k] / static_cast<double>(raw.size());
      }
      history.push_back(smoothed);
      if (history.size() > memory_frames) history.erase(history.begin());
      std::vector<double> gains(power.size(), 1.0);
      for (std::size_t k = 0; k < power.size(); ++k) {
        double floor = std::numeric_limits<double>::max();
        for (const auto& frame : history) floor = std::min(floor, frame[k]);
        // The smallest recent level underestimates the typical noise level (the minimum of
        // many noisy values is well below their mean); kFloorBias puts it back.
        constexpr double kFloorBias = 6.0;
        const double excess = power[k] > 1e-18 ? s.sensitivity * kFloorBias * floor / power[k] : 1.0;
        gains[k] = std::clamp(std::sqrt(std::max(1.0 - std::clamp(s.amount, 0.0, 1.0) * excess, 0.0)), floor_gain, 1.0);
      }
      return gains;
    });
  }
}

void Dereverb(AudioBuffer& buffer, const DereverbSettings& s) {
  const double fs = static_cast<double>(buffer.sample_rate());
  // The tail is what arrived at least ~60 ms ago, decayed by how long ago that was.
  const auto delay = static_cast<std::size_t>(std::max<std::int64_t>(2, static_cast<std::int64_t>(fs * 0.06) / kFrameHop));
  const double frame_seconds = static_cast<double>(kFrameHop) / fs;
  const double per_frame = std::pow(10.0, -6.0 * frame_seconds / (std::max(s.decay_ms, 20.0) / 1000.0));  // power, -60 dB over decay_ms
  const double decayed = std::pow(per_frame, static_cast<double>(delay));
  for (int channel = 0; channel < buffer.channels(); ++channel) {
    std::vector<std::vector<double>> history;
    SpectralProcess(buffer.channel(channel), buffer.frames(), [&](std::int64_t, const std::vector<double>& power) {
      std::vector<double> gains(power.size(), 1.0);
      if (history.size() >= delay) {
        const auto& earlier = history[history.size() - delay];
        for (std::size_t k = 0; k < power.size(); ++k) {
          const double tail = decayed * earlier[k];
          const double excess = power[k] > 1e-18 ? tail / power[k] : 1.0;
          gains[k] = std::clamp(std::sqrt(std::max(1.0 - std::clamp(s.amount, 0.0, 1.0) * excess, 0.0)), 0.25, 1.0);
        }
      }
      history.push_back(power);
      if (history.size() > delay + 1) history.erase(history.begin());
      return gains;
    });
  }
}

// --------------------------------------------------------------- margins ----

Margins CompressorMargins(const CompressorSettings& s, double fs) {
  const double rate = kReferenceRecovery / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto attack = Samples(s.attack_ms, fs);
  return {span + Samples(5.0, fs) + attack, attack};
}
Margins LimiterMargins(const LimiterSettings& s, double fs) {
  const double rate = kReferenceRecovery / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto look = Samples(s.lookahead_ms, fs);
  return {span + 2 * look + 2, 2 * look + 2};
}
Margins GateMargins(const GateSettings& s, double fs) {
  const double depth = std::max(std::abs(s.range_db), 1.0);
  const double rate = depth / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto attack = Samples(s.attack_ms, fs);
  return {span + Samples(s.hold_ms, fs) + Samples(5.0, fs) + attack, attack};
}
Margins DuckMargins(const DuckSettings& s, double fs) {
  const double depth = std::max(std::abs(s.reduction_db), 1.0);
  const double rate = depth / static_cast<double>(Samples(s.release_ms, fs));
  const auto span = static_cast<std::int64_t>(kReductionSpan / rate) + 1;
  const auto attack = Samples(s.attack_ms, fs);
  return {span + Samples(10.0, fs) + attack, attack};
}
Margins EqMargins(const EqSettings&, double fs) { return {static_cast<std::int64_t>(fs * 0.25), 0}; }
Margins DenoiseMargins(double fs) { return {static_cast<std::int64_t>(fs * 0.8) + 12 * kFrame, 2 * kFrame}; }
Margins DereverbMargins(const DereverbSettings&, double fs) { return {static_cast<std::int64_t>(fs * 0.06) + 3 * kFrame, 2 * kFrame}; }

// -------------------------------------------------------------- loudness ----

namespace {

struct KFilter final {
  Biquad shelf;
  Biquad high_pass;
};

KFilter KWeighting(double fs) {
  KFilter k;
  {
    const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    const double kk = std::tan(kPi * f0 / fs);
    const double vh = std::pow(10.0, g / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + kk / q + kk * kk;
    k.shelf = {(vh + vb * kk / q + kk * kk) / a0, 2.0 * (kk * kk - vh) / a0, (vh - vb * kk / q + kk * kk) / a0,
               2.0 * (kk * kk - 1.0) / a0, (1.0 - kk / q + kk * kk) / a0};
  }
  {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double kk = std::tan(kPi * f0 / fs);
    const double a0 = 1.0 + kk / q + kk * kk;
    k.high_pass = {1.0, -2.0, 1.0, 2.0 * (kk * kk - 1.0) / a0, (1.0 - kk / q + kk * kk) / a0};
  }
  return k;
}

[[nodiscard]] double Weight(int channel, int channels) {
  return channels == 5 && channel >= 3 ? 1.41 : 1.0;
}

[[nodiscard]] double BlockLoudness(double power) { return -0.691 + 10.0 * std::log10(power + 1e-20); }

}  // namespace

Loudness MeasureLoudness(const AudioBuffer& buffer) {
  Loudness result;
  const double fs = static_cast<double>(buffer.sample_rate());
  const auto frames = buffer.frames();
  if (frames == 0) return result;

  // Sample and true peak (4x oversampled).
  double peak = 0.0;
  double true_peak = 0.0;
  {
    constexpr int kPhases = 4;
    constexpr int kTaps = 12;
    std::vector<std::vector<double>> kernel(kPhases, std::vector<double>(kTaps));
    for (int phase = 0; phase < kPhases; ++phase) {
      double sum = 0.0;
      for (int tap = 0; tap < kTaps; ++tap) {
        const double x = (tap - kTaps / 2 + 1) - static_cast<double>(phase) / kPhases;
        const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double hann = 0.5 + 0.5 * std::cos(kPi * x / (kTaps / 2 + 1));
        kernel[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] = sinc * hann;
        sum += sinc * hann;
      }
      for (auto& v : kernel[static_cast<std::size_t>(phase)]) v /= sum;
    }
    for (int channel = 0; channel < buffer.channels(); ++channel) {
      const auto* samples = buffer.channel(channel);
      for (std::int64_t n = 0; n < frames; ++n) {
        peak = std::max(peak, static_cast<double>(std::abs(samples[n])));
        for (int phase = 1; phase < kPhases; ++phase) {
          double value = 0.0;
          for (int tap = 0; tap < kTaps; ++tap) {
            const auto at = n + tap - kTaps / 2 + 1;
            if (at >= 0 && at < frames) value += kernel[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] * samples[at];
          }
          true_peak = std::max(true_peak, std::abs(value));
        }
      }
    }
    true_peak = std::max(true_peak, peak);
  }
  result.sample_peak_db = DecibelsOf(peak);
  result.true_peak_db = DecibelsOf(true_peak);

  // K-weight each channel, then mean-square over 400 ms blocks every 100 ms.
  const auto k = KWeighting(fs);
  std::vector<std::vector<float>> weighted(static_cast<std::size_t>(buffer.channels()));
  for (int channel = 0; channel < buffer.channels(); ++channel) {
    weighted[static_cast<std::size_t>(channel)].assign(buffer.channel(channel), buffer.channel(channel) + frames);
    Filter(weighted[static_cast<std::size_t>(channel)].data(), frames, k.shelf);
    Filter(weighted[static_cast<std::size_t>(channel)].data(), frames, k.high_pass);
  }
  const auto block = static_cast<std::int64_t>(std::llround(0.4 * fs));
  const auto step = static_cast<std::int64_t>(std::llround(0.1 * fs));
  const auto window_power = [&](std::int64_t start, std::int64_t length) {
    double total = 0.0;
    for (int channel = 0; channel < buffer.channels(); ++channel) {
      double sum = 0.0;
      for (std::int64_t n = start; n < start + length; ++n) {
        const double x = weighted[static_cast<std::size_t>(channel)][static_cast<std::size_t>(n)];
        sum += x * x;
      }
      total += Weight(channel, buffer.channels()) * sum / static_cast<double>(length);
    }
    return total;
  };

  std::vector<double> powers;
  for (std::int64_t start = 0; start + block <= frames; start += step) powers.push_back(window_power(start, block));
  if (powers.empty()) {
    // Shorter than one block: measure what there is.
    const auto length = std::min<std::int64_t>(frames, block);
    const auto power = window_power(frames - length, length);
    result.momentary_lufs = BlockLoudness(power);
    result.loudest_momentary_lufs = result.momentary_lufs;
    result.short_term_lufs = result.momentary_lufs;
    result.integrated_lufs = result.momentary_lufs > -70.0 ? result.momentary_lufs : -200.0;
    return result;
  }
  result.momentary_lufs = BlockLoudness(powers.back());
  for (const auto power : powers) result.loudest_momentary_lufs = std::max(result.loudest_momentary_lufs, BlockLoudness(power));
  {
    const auto length = std::min<std::int64_t>(frames, static_cast<std::int64_t>(std::llround(3.0 * fs)));
    result.short_term_lufs = BlockLoudness(window_power(frames - length, length));
  }

  // Integrated: absolute gate at -70 LUFS, then relative gate 10 LU below the mean of what passed.
  std::vector<double> gated;
  for (const auto power : powers) {
    if (BlockLoudness(power) > -70.0) gated.push_back(power);
  }
  if (gated.empty()) return result;
  double mean = 0.0;
  for (const auto power : gated) mean += power;
  mean /= static_cast<double>(gated.size());
  const double relative = BlockLoudness(mean) - 10.0;
  double final_sum = 0.0;
  int final_count = 0;
  for (const auto power : gated) {
    if (BlockLoudness(power) > relative) {
      final_sum += power;
      ++final_count;
    }
  }
  if (final_count > 0) result.integrated_lufs = BlockLoudness(final_sum / final_count);
  return result;
}

namespace {

constexpr int kPeakPhases = 4;
constexpr int kPeakTaps = 12;

const std::vector<std::vector<double>>& PeakKernel() {
  static const auto kernel = [] {
    std::vector<std::vector<double>> k(kPeakPhases, std::vector<double>(kPeakTaps));
    for (int phase = 0; phase < kPeakPhases; ++phase) {
      double sum = 0.0;
      for (int tap = 0; tap < kPeakTaps; ++tap) {
        const double x = (tap - kPeakTaps / 2 + 1) - static_cast<double>(phase) / kPeakPhases;
        const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double hann = 0.5 + 0.5 * std::cos(kPi * x / (kPeakTaps / 2 + 1));
        k[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] = sinc * hann;
        sum += sinc * hann;
      }
      for (auto& v : k[static_cast<std::size_t>(phase)]) v /= sum;
    }
    return k;
  }();
  return kernel;
}

double Stage(const Biquad& f, double x, double* state) {
  const double y = f.b0 * x + f.b1 * state[0] + f.b2 * state[1] - f.a1 * state[2] - f.a2 * state[3];
  state[1] = state[0];
  state[0] = x;
  state[3] = state[2];
  state[2] = y;
  return y;
}

}  // namespace

LoudnessMeter::LoudnessMeter(std::int64_t sample_rate, int channels)
    : sample_rate_(sample_rate), step_(std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(0.1 * static_cast<double>(sample_rate))))),
      channels_(static_cast<std::size_t>(std::max(channels, 1))) {}

void LoudnessMeter::Push(const AudioBuffer& block) {
  if (block.frames() == 0) return;
  const auto k = KWeighting(static_cast<double>(sample_rate_));
  const int channels = static_cast<int>(channels_.size());
  const auto frames = block.frames();
  // Loudness: K-weight each channel (state carried from the last block) and sum squares in 100 ms steps.
  std::int64_t position = 0;
  while (position < frames) {
    const auto run = std::min(frames - position, step_ - in_step_);
    for (int c = 0; c < channels; ++c) {
      auto& channel = channels_[static_cast<std::size_t>(c)];
      const float* samples = c < block.channels() ? block.channel(c) : nullptr;
      double sum = 0.0;
      for (std::int64_t n = 0; n < run; ++n) {
        const double x = samples != nullptr ? samples[position + n] : 0.0;
        const double shelved = static_cast<float>(Stage(k.shelf, x, channel.shelf));
        const double weighted = static_cast<float>(Stage(k.high_pass, shelved, channel.high_pass));
        sum += weighted * weighted;
      }
      channel.step_sum += sum;
      channel.total_sum += sum;
    }
    in_step_ += run;
    position += run;
    if (in_step_ == step_) {
      double power = 0.0;
      for (int c = 0; c < channels; ++c) {
        power += Weight(c, channels) * channels_[static_cast<std::size_t>(c)].step_sum / static_cast<double>(step_);
        channels_[static_cast<std::size_t>(c)].step_sum = 0.0;
      }
      powers_.push_back(power);
      in_step_ = 0;
    }
  }
  // Peaks: the sample peak now; the true peak for every sample that has its six samples of lookahead.
  const auto& kernel = PeakKernel();
  for (int c = 0; c < channels; ++c) {
    auto& channel = channels_[static_cast<std::size_t>(c)];
    const float* samples = c < block.channels() ? block.channel(c) : nullptr;
    for (std::int64_t n = 0; n < frames; ++n) {
      const float v = samples != nullptr ? samples[n] : 0.0f;
      peak_ = std::max(peak_, static_cast<double>(std::abs(v)));
      channel.window.push_back(v);
    }
  }
  frames_ += frames;
  const auto ready_until = frames_ - kPeakTaps / 2;   // exclusive: n + 6 must exist
  for (int c = 0; c < channels; ++c) {
    auto& channel = channels_[static_cast<std::size_t>(c)];
    for (std::int64_t n = peak_next_; n < ready_until; ++n) {
      for (int phase = 1; phase < kPeakPhases; ++phase) {
        double value = 0.0;
        for (int tap = 0; tap < kPeakTaps; ++tap) {
          const auto at = n + tap - kPeakTaps / 2 + 1;
          if (at >= 0) value += kernel[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] * channel.window[static_cast<std::size_t>(at - window_start_)];
        }
        true_peak_ = std::max(true_peak_, std::abs(value));
      }
    }
  }
  if (ready_until > peak_next_) peak_next_ = ready_until;
  // Forget what no later sample can need: five samples before the next one to be taken.
  const auto keep_from = std::max<std::int64_t>(window_start_, peak_next_ - (kPeakTaps / 2 - 1));
  if (keep_from > window_start_) {
    for (auto& channel : channels_) channel.window.erase(channel.window.begin(), channel.window.begin() + static_cast<std::ptrdiff_t>(keep_from - window_start_));
    window_start_ = keep_from;
  }
}

Loudness LoudnessMeter::Result() const {
  Loudness result;
  if (frames_ == 0) return result;
  const int channels = static_cast<int>(channels_.size());
  // The peaks of the last six samples, which had no lookahead yet: nothing follows them.
  double true_peak = true_peak_;
  const auto& kernel = PeakKernel();
  for (const auto& channel : channels_) {
    for (std::int64_t n = peak_next_; n < frames_; ++n) {
      for (int phase = 1; phase < kPeakPhases; ++phase) {
        double value = 0.0;
        for (int tap = 0; tap < kPeakTaps; ++tap) {
          const auto at = n + tap - kPeakTaps / 2 + 1;
          if (at >= 0 && at < frames_) value += kernel[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] * channel.window[static_cast<std::size_t>(at - window_start_)];
        }
        true_peak = std::max(true_peak, std::abs(value));
      }
    }
  }
  true_peak = std::max(true_peak, peak_);
  result.sample_peak_db = DecibelsOf(peak_);
  result.true_peak_db = DecibelsOf(true_peak);

  // 400 ms blocks every 100 ms are four steps; a programme shorter than that is measured whole.
  std::vector<double> blocks;
  for (std::size_t i = 0; i + 4 <= powers_.size(); ++i) blocks.push_back((powers_[i] + powers_[i + 1] + powers_[i + 2] + powers_[i + 3]) / 4.0);
  if (blocks.empty()) {
    double power = 0.0;
    for (int c = 0; c < channels; ++c) power += Weight(c, channels) * channels_[static_cast<std::size_t>(c)].total_sum / static_cast<double>(frames_);
    result.momentary_lufs = BlockLoudness(power);
    result.loudest_momentary_lufs = result.momentary_lufs;
    result.short_term_lufs = result.momentary_lufs;
    result.integrated_lufs = result.momentary_lufs > -70.0 ? result.momentary_lufs : -200.0;
    return result;
  }
  result.momentary_lufs = BlockLoudness(blocks.back());
  for (const auto power : blocks) result.loudest_momentary_lufs = std::max(result.loudest_momentary_lufs, BlockLoudness(power));
  {
    const auto count = std::min<std::size_t>(powers_.size(), 30);
    double sum = 0.0;
    for (std::size_t i = powers_.size() - count; i < powers_.size(); ++i) sum += powers_[i];
    result.short_term_lufs = BlockLoudness(sum / static_cast<double>(count));
  }
  std::vector<double> gated;
  for (const auto power : blocks) {
    if (BlockLoudness(power) > -70.0) gated.push_back(power);
  }
  if (gated.empty()) return result;
  double mean = 0.0;
  for (const auto power : gated) mean += power;
  mean /= static_cast<double>(gated.size());
  const double relative = BlockLoudness(mean) - 10.0;
  double final_sum = 0.0;
  int final_count = 0;
  for (const auto power : gated) {
    if (BlockLoudness(power) > relative) {
      final_sum += power;
      ++final_count;
    }
  }
  if (final_count > 0) result.integrated_lufs = BlockLoudness(final_sum / final_count);
  return result;
}

double GainToReachLoudness(const AudioBuffer& buffer, double target_lufs) {
  const auto measured = MeasureLoudness(buffer);
  return measured.integrated_lufs <= -199.0 ? 0.0 : target_lufs - measured.integrated_lufs;
}

}  // namespace cutline::audio::dsp

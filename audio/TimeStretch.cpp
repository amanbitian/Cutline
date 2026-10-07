#include "audio/TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace cutline::audio {

StretchQuality PreviewStretch() { return {1024, 256, 4, 64}; }
StretchQuality OfflineStretch() { return {2048, 1024, 1, 128}; }

std::int64_t ChainStart(std::int64_t chain_index, const StretchQuality& quality) {
  return chain_index * quality.chain_frames * quality.hop();
}
std::int64_t ChainLength(const StretchQuality& quality) {
  return static_cast<std::int64_t>(quality.chain_frames) * quality.hop() + quality.frame;
}
std::int64_t ChainOf(std::int64_t sample, const StretchQuality& quality) {
  const auto span = static_cast<std::int64_t>(quality.chain_frames) * quality.hop();
  auto chain = sample / span;
  if (sample % span != 0 && sample < 0) --chain;
  return chain;
}

namespace {

// How well the window of `mono` starting at `candidate` continues `target`: their
// correlation, normalised by the candidate's energy so a louder stretch of signal
// does not win merely for being loud. `stride` subsamples both.
[[nodiscard]] double Score(const std::vector<float>& mono, const float* target, std::int64_t candidate, int length,
                           int stride) {
  double dot = 0.0;
  double energy = 1e-9;
  const float* window = mono.data() + candidate;
  for (int j = 0; j < length; j += stride) {
    dot += static_cast<double>(target[j]) * window[j];
    energy += static_cast<double>(window[j]) * window[j];
  }
  return dot / std::sqrt(energy);
}

}  // namespace

namespace {

// One chain, given where window k nominally reads (in whole signal samples). Both public entry points
// reduce to this.
media::AudioBuffer ChainFrom(const StretchInput& input, std::int64_t sample_rate, int channels,
                             const std::function<std::int64_t(std::int64_t)>& nominal, std::int64_t chain_index,
                             const StretchQuality& q) {
  const int frame = q.frame;
  const int hop = q.hop();
  const int delta = q.search;
  const int stride = std::max(q.decimation, 1);

  const std::int64_t first_frame = chain_index * q.chain_frames;
  // The span of signal the chain reads: every window's nominal position, widened by the search either side.
  // For a constant speed the first window is the lowest and the last the highest; a ramp is scanned.
  std::int64_t lowest = nominal(first_frame), highest = lowest;
  for (int f = 1; f < q.chain_frames; ++f) {
    const auto position = nominal(first_frame + f);
    lowest = std::min(lowest, position);
    highest = std::max(highest, position);
  }
  const auto in_lo = lowest - delta;
  const auto in_hi = highest + delta + hop + frame;
  const auto span = in_hi - in_lo;

  auto source = media::AudioBuffer::Allocate(sample_rate, channels, span);
  input(in_lo, span, source);

  std::vector<float> mono(static_cast<std::size_t>(span), 0.0f);
  for (int channel = 0; channel < channels; ++channel) {
    const auto* samples = source.channel(channel);
    for (std::int64_t i = 0; i < span; ++i) mono[static_cast<std::size_t>(i)] += samples[i] / static_cast<float>(channels);
  }

  std::vector<float> window(static_cast<std::size_t>(frame));
  for (int n = 0; n < frame; ++n) {
    window[static_cast<std::size_t>(n)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / frame));
  }

  auto output = media::AudioBuffer::Allocate(sample_rate, channels, ChainLength(q));
  std::int64_t previous = 0;
  for (int f = 0; f < q.chain_frames; ++f) {
    const auto nominal_position = nominal(first_frame + f);
    std::int64_t position = nominal_position;
    if (f > 0 && delta > 0) {
      // The window that would continue the previous one naturally begins a hop
      // after it; look for the stretch of signal near the nominal position that
      // matches it best.
      const float* target = mono.data() + (previous + hop - in_lo);
      const auto low = nominal_position - delta;
      const auto high = nominal_position + delta;
      double best = -1e300;
      std::int64_t best_position = nominal_position;
      for (auto p = low; p <= high; p += stride) {
        const auto score = Score(mono, target, p - in_lo, frame, stride);
        if (score > best) {
          best = score;
          best_position = p;
        }
      }
      if (stride > 1) {
        // Refine to the sample around the coarse winner.
        const auto refine_low = std::max(low, best_position - stride + 1);
        const auto refine_high = std::min(high, best_position + stride - 1);
        for (auto p = refine_low; p <= refine_high; ++p) {
          const auto score = Score(mono, target, p - in_lo, frame, stride);
          if (score > best) {
            best = score;
            best_position = p;
          }
        }
      }
      position = best_position;
    }
    previous = position;

    const auto destination = static_cast<std::int64_t>(f) * hop;
    const bool opening_frame = chain_index == 0 && f == 0;
    for (int channel = 0; channel < channels; ++channel) {
      const auto* from = source.channel(channel) + (position - in_lo);
      auto* to = output.channel(channel) + destination;
      for (int n = 0; n < frame; ++n) {
        // The very first window of the whole signal keeps its rising half at full
        // weight, so the start of a clip is not faded in.
        const float weight = opening_frame && n < hop ? 1.0f : window[static_cast<std::size_t>(n)];
        to[n] += from[n] * weight;
      }
    }
  }
  return output;
}

}  // namespace

media::AudioBuffer StretchChain(const StretchInput& input, std::int64_t sample_rate, int channels, double speed,
                                std::int64_t chain_index, const StretchQuality& q) {
  if (!(speed > 0.0)) throw std::invalid_argument("Stretch speed must be positive");
  const double analysis_hop = static_cast<double>(q.hop()) * speed;
  return ChainFrom(input, sample_rate, channels,
                   [analysis_hop](std::int64_t k) { return static_cast<std::int64_t>(std::llround(static_cast<double>(k) * analysis_hop)); },
                   chain_index, q);
}

media::AudioBuffer StretchChainMapped(const StretchInput& input, std::int64_t sample_rate, int channels,
                                      const StretchPosition& position, std::int64_t chain_index, const StretchQuality& q) {
  if (!position) throw std::invalid_argument("A mapped stretch needs a position curve");
  const auto hop = static_cast<std::int64_t>(q.hop());
  return ChainFrom(input, sample_rate, channels,
                   [&position, hop](std::int64_t k) { return static_cast<std::int64_t>(std::llround(position(k * hop))); },
                   chain_index, q);
}

media::AudioBuffer Stretch(const media::AudioBuffer& signal, double speed, std::int64_t out_frames,
                           const StretchQuality& quality) {
  auto output = media::AudioBuffer::Allocate(signal.sample_rate(), signal.channels(), out_frames);
  if (out_frames <= 0) return output;
  const StretchInput read = [&](std::int64_t first, std::int64_t count, media::AudioBuffer& into) {
    for (int channel = 0; channel < signal.channels(); ++channel) {
      auto* to = into.channel(channel);
      const auto* from = signal.channel(channel);
      for (std::int64_t i = 0; i < count; ++i) {
        const auto index = first + i;
        to[i] = index >= 0 && index < signal.frames() ? from[index] : 0.0f;
      }
    }
  };
  const auto last_chain = ChainOf(out_frames - 1, quality);
  for (auto chain = std::max<std::int64_t>(0, 0); chain <= last_chain; ++chain) {
    const auto part = StretchChain(read, signal.sample_rate(), signal.channels(), speed, chain, quality);
    const auto start = ChainStart(chain, quality);
    for (int channel = 0; channel < signal.channels(); ++channel) {
      auto* to = output.channel(channel);
      const auto* from = part.channel(channel);
      for (std::int64_t i = 0; i < part.frames(); ++i) {
        const auto target = start + i;
        if (target >= 0 && target < out_frames) to[target] += from[i];
      }
    }
  }
  return output;
}

}  // namespace cutline::audio

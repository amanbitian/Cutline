#pragma once

// Pitch-preserving time stretch (WSOLA: waveform-similarity overlap-add).
//
// Playing audio at another speed changes its pitch unless something rebuilds the
// signal at the new length: overlapping short windows are laid down at the output
// rate while the reading position advances at the *source* rate, and each window's
// position is nudged to where the waveform best continues the previous one, so the
// joins do not beat or cancel. Pitch, then, is untouched; duration is exact.
//
// Two properties the mixer depends on:
//
//   * The result is a function of the signal and the parameters only, not of how a
//     caller happens to cut its requests into blocks. That is arranged by dividing
//     the output into *chains* of a fixed number of frames. A chain is computed on
//     its own, starting from the nominal reading position, and chains are added
//     where they overlap (the falling half of one chain's last window is the
//     rising half of the next one's first), so any block can be had by computing
//     the chains it touches. The price is a seam once per chain, where the two
//     chains' windows are not aligned to each other: a short, mild phasiness, once
//     every few seconds.
//   * Quality is a choice of parameters, not of algorithm: the preview setting
//     searches coarsely on a decimated copy so it can run beside playback; the
//     offline setting searches the full resolution over a wider range with longer
//     windows. Neither is better by being a different method.

#include "media/AudioBuffer.h"

#include <cstdint>
#include <functional>

namespace cutline::audio {

struct StretchQuality final {
  int frame{1024};         // window length in samples (even)
  int search{256};         // how far, either side, a window may move to find alignment
  int decimation{4};       // the coarse search runs on every nth sample
  int chain_frames{64};    // windows per chain (see above)

  [[nodiscard]] int hop() const { return frame / 2; }
};

[[nodiscard]] StretchQuality PreviewStretch();
[[nodiscard]] StretchQuality OfflineStretch();

// Speeds outside this range are not stretched (WSOLA degrades badly beyond it); the
// caller falls back to varispeed and says so.
inline constexpr double kMinStretchSpeed = 0.25;
inline constexpr double kMaxStretchSpeed = 4.0;
[[nodiscard]] inline bool CanStretch(double speed) { return speed >= kMinStretchSpeed && speed <= kMaxStretchSpeed; }

// Supplies the signal being stretched: fills `count` samples starting at signal
// sample `first` (which may be negative or past the end, where the signal is
// silent) into the planar buffer `into`.
using StretchInput = std::function<void(std::int64_t first, std::int64_t count, media::AudioBuffer& into)>;

// Chain `chain_index` of the stretched signal at `speed` (2.0 plays twice as fast and
// is half as long). The returned buffer's sample 0 is output sample
// chain_index * chain_frames * hop; it holds chain_frames * hop + frame samples, of
// which only the part overlapping the chain's neighbours is partial. Summing the
// chains reproduces the stretched signal.
[[nodiscard]] media::AudioBuffer StretchChain(const StretchInput& input, std::int64_t sample_rate, int channels,
                                              double speed, std::int64_t chain_index, const StretchQuality& quality);

// First output sample covered by a chain, and how many samples its buffer holds.
[[nodiscard]] std::int64_t ChainStart(std::int64_t chain_index, const StretchQuality& quality);
[[nodiscard]] std::int64_t ChainLength(const StretchQuality& quality);
// The chain whose own span (the part not shared with the next chain) contains `sample`.
[[nodiscard]] std::int64_t ChainOf(std::int64_t sample, const StretchQuality& quality);

// Where, in the signal, the output sample `output_sample` is read from, as a sample position (fractional). The
// speed at any point is how fast this advances: a constant speed is position = speed * output_sample, and a
// speed ramp is any curve that never runs backwards. The same chain rules apply as above (chains are computed on
// their own, from this function alone), so the result does not depend on how a caller cuts its requests.
using StretchPosition = std::function<double(std::int64_t output_sample)>;

// Chain `chain_index` of the signal stretched along `position`. The curve must not run backwards over the chain
// (a reverse segment cannot be stretched) and is best kept within kMinStretchSpeed..kMaxStretchSpeed; where it is
// not, the caller is expected to have chosen another way to play it.
[[nodiscard]] media::AudioBuffer StretchChainMapped(const StretchInput& input, std::int64_t sample_rate, int channels,
                                                    const StretchPosition& position, std::int64_t chain_index,
                                                    const StretchQuality& quality);

// Convenience for offline use and tests: stretches a whole signal to exactly
// `out_frames` samples at `speed`.
[[nodiscard]] media::AudioBuffer Stretch(const media::AudioBuffer& signal, double speed, std::int64_t out_frames,
                                         const StretchQuality& quality);

}  // namespace cutline::audio

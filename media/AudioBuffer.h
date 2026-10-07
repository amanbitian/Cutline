#pragma once

// Decoded audio.
//
// Planar 32-bit float throughout: it is what every mixer, resampler, and plug-in
// format wants internally, it has no clipping behaviour to reason about during
// summation, and planar layout means a per-channel operation is a contiguous
// loop. Decoders convert to it on the way out, as they do for video.

#include "core/time/RationalTime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cutline::media {

// Channel order is the interleave order a layout implies; the mixer maps by
// index, so the layout name is what gives those indices meaning.
[[nodiscard]] std::int64_t ChannelCountForLayout(const std::string& layout);

class AudioBuffer final {
 public:
  AudioBuffer() = default;
  AudioBuffer(const AudioBuffer&) = delete;
  AudioBuffer& operator=(const AudioBuffer&) = delete;
  AudioBuffer(AudioBuffer&&) noexcept = default;
  AudioBuffer& operator=(AudioBuffer&&) noexcept = default;

  [[nodiscard]] static AudioBuffer Allocate(std::int64_t sample_rate, int channels, std::int64_t frames);
  [[nodiscard]] AudioBuffer Clone() const;

  [[nodiscard]] bool valid() const noexcept { return sample_rate_ > 0 && channels_ > 0; }
  [[nodiscard]] std::int64_t sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] int channels() const noexcept { return channels_; }
  // Frames, not samples: one frame holds one sample per channel.
  [[nodiscard]] std::int64_t frames() const noexcept { return frames_; }

  [[nodiscard]] float* channel(int index);
  [[nodiscard]] const float* channel(int index) const;

  // Zeroes every channel without reallocating, for reuse in a per-block path.
  void Silence();
  // Sums `other` into this buffer with a linear gain. Layouts must match.
  void MixFrom(const AudioBuffer& other, float gain = 1.0f);
  // Scales every sample in place.
  void ApplyGain(float gain);
  // Peak absolute sample across a channel, for metering and for tests.
  [[nodiscard]] float Peak(int channel_index) const;

  // Presentation time of the first frame, in the source timebase.
  time::RationalTime presentation_time;

 private:
  std::int64_t sample_rate_{0};
  int channels_{0};
  std::int64_t frames_{0};
  // One contiguous allocation, channel-major: channel n starts at n * frames_.
  std::vector<float> samples_;
};

// Converts a decibel gain to a linear multiplier. -inf dB (anything at or below
// the floor) maps to exactly zero so a muted track contributes nothing.
[[nodiscard]] float DecibelsToLinear(double decibels);

// Equal-power stereo pan. -1 is hard left, 0 centre, +1 hard right; the two
// gains sum in power rather than amplitude, so a pan sweep holds loudness.
struct PanGains final {
  float left{1.0f};
  float right{1.0f};
};
[[nodiscard]] PanGains StereoPan(double pan);

}  // namespace cutline::media

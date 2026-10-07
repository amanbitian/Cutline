#include "media/AudioBuffer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace cutline::media {
namespace {

// Below this the gain is treated as silence rather than an inaudible non-zero,
// so a muted track truly contributes nothing to a sum.
constexpr double kSilenceFloorDb = -96.0;

}  // namespace

std::int64_t ChannelCountForLayout(const std::string& layout) {
  if (layout == "mono") return 1;
  if (layout == "stereo") return 2;
  if (layout == "2.1") return 3;
  if (layout == "quad") return 4;
  if (layout == "5.0") return 5;
  if (layout == "5.1") return 6;
  if (layout == "7.1") return 8;
  // "adaptive" tracks carry an explicit channel count elsewhere; refusing here
  // is better than guessing a layout the mixer would then map incorrectly.
  throw std::invalid_argument("Unknown channel layout: " + layout);
}

AudioBuffer AudioBuffer::Allocate(std::int64_t sample_rate, int channels, std::int64_t frames) {
  if (sample_rate <= 0) throw std::invalid_argument("Audio sample rate must be positive");
  if (channels <= 0) throw std::invalid_argument("Audio buffer needs at least one channel");
  if (frames < 0) throw std::invalid_argument("Audio frame count cannot be negative");
  AudioBuffer buffer;
  buffer.sample_rate_ = sample_rate;
  buffer.channels_ = channels;
  buffer.frames_ = frames;
  buffer.samples_.assign(static_cast<std::size_t>(channels) * static_cast<std::size_t>(frames), 0.0f);
  return buffer;
}

AudioBuffer AudioBuffer::Clone() const {
  AudioBuffer copy;
  copy.sample_rate_ = sample_rate_;
  copy.channels_ = channels_;
  copy.frames_ = frames_;
  copy.samples_ = samples_;
  copy.presentation_time = presentation_time;
  return copy;
}

float* AudioBuffer::channel(int index) {
  if (index < 0 || index >= channels_) throw std::out_of_range("Audio channel index is out of range");
  return samples_.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(frames_);
}

const float* AudioBuffer::channel(int index) const {
  if (index < 0 || index >= channels_) throw std::out_of_range("Audio channel index is out of range");
  return samples_.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(frames_);
}

void AudioBuffer::Silence() { std::fill(samples_.begin(), samples_.end(), 0.0f); }

void AudioBuffer::MixFrom(const AudioBuffer& other, float gain) {
  if (other.sample_rate_ != sample_rate_) {
    throw std::invalid_argument("Cannot mix audio at a different sample rate without resampling");
  }
  if (other.channels_ != channels_) throw std::invalid_argument("Cannot mix audio with a different channel count");
  const auto count = static_cast<std::size_t>(std::min(frames_, other.frames_));
  for (int index = 0; index < channels_; ++index) {
    const auto* source = other.channel(index);
    auto* destination = channel(index);
    for (std::size_t frame = 0; frame < count; ++frame) destination[frame] += source[frame] * gain;
  }
}

void AudioBuffer::ApplyGain(float gain) {
  for (auto& sample : samples_) sample *= gain;
}

float AudioBuffer::Peak(int channel_index) const {
  const auto* samples = channel(channel_index);
  float peak = 0.0f;
  for (std::int64_t frame = 0; frame < frames_; ++frame) peak = std::max(peak, std::abs(samples[frame]));
  return peak;
}

float DecibelsToLinear(double decibels) {
  if (decibels <= kSilenceFloorDb) return 0.0f;
  return static_cast<float>(std::pow(10.0, decibels / 20.0));
}

PanGains StereoPan(double pan) {
  const auto clamped = std::clamp(pan, -1.0, 1.0);
  // Map -1..1 onto 0..pi/2 and take cos/sin, which keeps left^2 + right^2 == 1.
  const auto angle = (clamped + 1.0) * 0.25 * std::numbers::pi;
  return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

}  // namespace cutline::media

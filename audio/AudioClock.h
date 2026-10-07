#pragma once

#include "core/time/RationalTime.h"

#include <atomic>
#include <cstdint>

namespace cutline::audio {

class AudioClock final {
 public:
  explicit AudioClock(std::int64_t sample_rate);
  void Reset(std::int64_t sample_position = 0) noexcept;
  // Adopts a new device rate and restarts the count. Used when the output
  // device is opened, and again if the user switches device mid-session.
  void Reconfigure(std::int64_t sample_rate);
  void AdvanceFromAudioCallback(std::int64_t rendered_samples) noexcept;
  [[nodiscard]] std::int64_t sample_rate() const noexcept { return sample_rate_; }
  [[nodiscard]] time::RationalTime Now() const;

 private:
  std::int64_t sample_rate_;
  std::atomic<std::int64_t> sample_position_{0};
};

}  // namespace cutline::audio

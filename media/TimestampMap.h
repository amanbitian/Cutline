#pragma once

#include "core/time/RationalTime.h"

#include <cstddef>
#include <vector>

namespace cutline::media {

struct FrameTimestamp final {
  time::RationalTime presentation_time;
  time::RationalTime duration;
  bool keyframe{};
};

enum class Cadence { Constant, Variable };

class TimestampMap final {
 public:
  // `tolerance` is how far two frame durations may differ and still count as the
  // same cadence. A container that stores timestamps in milliseconds cannot
  // express 30000/1001 fps: its frames are 33 and 34 ms apart, which is rounding,
  // not a variable frame rate. Pass one tick of the container's time base.
  explicit TimestampMap(std::vector<FrameTimestamp> frames, const time::RationalTime& tolerance = {});
  [[nodiscard]] Cadence cadence() const noexcept { return cadence_; }
  [[nodiscard]] const FrameTimestamp& AtOrBefore(const time::RationalTime& source_time) const;
  [[nodiscard]] std::size_t size() const noexcept { return frames_.size(); }

 private:
  std::vector<FrameTimestamp> frames_;
  Cadence cadence_{Cadence::Constant};
};

}  // namespace cutline::media

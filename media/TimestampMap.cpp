#include "media/TimestampMap.h"

#include <algorithm>
#include <stdexcept>

namespace cutline::media {

TimestampMap::TimestampMap(std::vector<FrameTimestamp> frames, const time::RationalTime& tolerance)
    : frames_(std::move(frames)) {
  if (frames_.empty()) throw std::invalid_argument("Timestamp map requires at least one decoded frame timestamp");
  std::sort(frames_.begin(), frames_.end(), [](const auto& left, const auto& right) { return left.presentation_time.Compare(right.presentation_time) < 0; });
  const auto expected_duration = frames_.front().duration;
  for (std::size_t index = 0; index < frames_.size(); ++index) {
    if (frames_[index].duration.Compare({0, 1}) <= 0) throw std::invalid_argument("Frame duration must be positive");
    if (index > 0 && frames_[index].presentation_time.Compare(frames_[index - 1].presentation_time) <= 0) throw std::invalid_argument("Frame timestamps must be strictly increasing");
    auto difference = frames_[index].duration.Subtract(expected_duration);
    if (difference.Compare({0, 1}) < 0) difference = time::RationalTime(0, 1).Subtract(difference);
    if (difference.Compare(tolerance) > 0) cadence_ = Cadence::Variable;
  }
}

const FrameTimestamp& TimestampMap::AtOrBefore(const time::RationalTime& source_time) const {
  const auto iterator = std::upper_bound(frames_.begin(), frames_.end(), source_time, [](const auto& target, const auto& frame) { return target.Compare(frame.presentation_time) < 0; });
  return iterator == frames_.begin() ? frames_.front() : *std::prev(iterator);
}

}  // namespace cutline::media

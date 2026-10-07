#pragma once

#include <cstdint>
#include <string>

namespace cutline::time {

struct FrameRate final { std::int64_t numerator; std::int64_t denominator; };
enum class RoundingMode { Floor, Nearest, Ceil, Exact };

class RationalTime final {
 public:
  RationalTime(std::int64_t numerator = 0, std::int64_t denominator = 1);
  [[nodiscard]] std::int64_t numerator() const noexcept { return numerator_; }
  [[nodiscard]] std::int64_t denominator() const noexcept { return denominator_; }
  [[nodiscard]] static RationalTime FromFrames(std::int64_t frame_count, FrameRate rate);
  [[nodiscard]] std::int64_t ToFrames(FrameRate rate, RoundingMode rounding = RoundingMode::Nearest) const;
  [[nodiscard]] std::int64_t Rescale(std::int64_t target_denominator, RoundingMode rounding = RoundingMode::Exact) const;
  [[nodiscard]] RationalTime Add(const RationalTime& other) const;
  [[nodiscard]] RationalTime Subtract(const RationalTime& other) const;
  [[nodiscard]] RationalTime Multiply(std::int64_t factor) const;
  [[nodiscard]] RationalTime Multiply(const RationalTime& factor) const;
  [[nodiscard]] RationalTime Divide(std::int64_t divisor) const;
  [[nodiscard]] RationalTime Divide(const RationalTime& divisor) const;
  [[nodiscard]] int Compare(const RationalTime& other) const;
  // Cutline stores an exact integer tick value alongside every rational time so
  // that timeline ranges can be indexed and range-queried in SQL, which a
  // numerator/denominator pair cannot be. kTicksPerSecond is divisible by every
  // broadcast frame rate and audio sample rate in use, including the 1001-based
  // rates, so the conversion is lossless rather than a rounding.
  [[nodiscard]] std::int64_t ToTicks() const;
  [[nodiscard]] static RationalTime FromTicks(std::int64_t ticks);
  [[nodiscard]] static RationalTime ParseTimecode(const std::string& timecode, FrameRate rate, bool drop_frame = false);
  // Formats as HH:MM:SS:FF. A timecode names a frame, so a time that falls
  // between frames has to be snapped; the default rounds to the nearest, which
  // is what a timecode display wants. Pass RoundingMode::Exact to assert that
  // the time is already on a frame boundary.
  [[nodiscard]] std::string FormatTimecode(FrameRate rate, bool drop_frame = false,
                                           RoundingMode rounding = RoundingMode::Nearest) const;

 private:
  std::int64_t numerator_;
  std::int64_t denominator_;
};

// 254016000000 = 2^6 * 3^4 * 7^2 * 10^6. Chosen so that frame durations at
// 23.976/29.97/59.94 and sample durations at 44.1/48/96 kHz are all integers.
inline constexpr std::int64_t kTicksPerSecond = 254016000000LL;

inline constexpr FrameRate kFrameRate23976{24000, 1001};
inline constexpr FrameRate kFrameRate24{24, 1};
inline constexpr FrameRate kFrameRate25{25, 1};
inline constexpr FrameRate kFrameRate2997{30000, 1001};
inline constexpr FrameRate kFrameRate30{30, 1};
inline constexpr FrameRate kFrameRate50{50, 1};
inline constexpr FrameRate kFrameRate5994{60000, 1001};
inline constexpr FrameRate kFrameRate60{60, 1};

}  // namespace cutline::time

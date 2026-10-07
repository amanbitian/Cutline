#include "core/time/RationalTime.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <array>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace cutline::time {
namespace {
constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
std::int64_t CheckedMultiply(std::int64_t left, std::int64_t right) {
  if (left == 0 || right == 0) return 0;
  if ((left == -1 && right == kMin) || (right == -1 && left == kMin)) throw std::overflow_error("RationalTime multiplication overflow");
  if ((left > 0 && right > 0 && left > kMax / right) || (left > 0 && right < 0 && right < kMin / left) ||
      (left < 0 && right > 0 && left < kMin / right) || (left < 0 && right < 0 && left < kMax / right)) throw std::overflow_error("RationalTime multiplication overflow");
  return left * right;
}
std::int64_t CheckedAdd(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > kMax - right) || (right < 0 && left < kMin - right)) throw std::overflow_error("RationalTime addition overflow");
  return left + right;
}
std::int64_t Absolute(std::int64_t value) { if (value == kMin) throw std::overflow_error("RationalTime absolute value overflow"); return value < 0 ? -value : value; }
// round(numerator * multiplier / denominator), computed exactly.
//
// The product of a long timeline's numerator and a large multiplier (the tick
// rate, a sample rate) can exceed 64 bits even when the quotient is tiny: one
// hour at 29.97 is a numerator near 1e8, and multiplying that by the tick rate
// of 2.5e11 overflowed although the result is under 1e15. Doing the multiply and
// divide in 128 bits and narrowing once at the end removes that false overflow
// while still reporting a genuine one.
struct WideQuotient final {
  std::uint64_t quotient{};
  std::uint64_t remainder{};
};

[[nodiscard]] WideQuotient MultiplyDivide(std::uint64_t left, std::uint64_t right, std::uint64_t divisor) {
#if defined(_MSC_VER) && defined(_M_X64)
  std::uint64_t high = 0;
  const auto low = _umul128(left, right, &high);
  // The quotient fits in 64 bits only if the high word is below the divisor.
  if (high >= divisor) throw std::overflow_error("RationalTime multiplication overflow");
  std::uint64_t remainder = 0;
  const auto quotient = _udiv128(high, low, divisor, &remainder);
  return {quotient, remainder};
#else
  const auto product = static_cast<unsigned __int128>(left) * right;
  const auto quotient = product / divisor;
  if (quotient > std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("RationalTime multiplication overflow");
  return {static_cast<std::uint64_t>(quotient), static_cast<std::uint64_t>(product % divisor)};
#endif
}

std::int64_t ScaledRound(std::int64_t numerator, std::int64_t multiplier, std::int64_t denominator, RoundingMode mode) {
  if (denominator <= 0) throw std::invalid_argument("Positive denominator required for rescaling");
  if (multiplier < 0) throw std::invalid_argument("Scale factor must not be negative");
  if (numerator == kMin) throw std::overflow_error("RationalTime value is outside the supported range");
  if (numerator == 0 || multiplier == 0) return 0;

  const bool negative = numerator < 0;
  const auto magnitude = static_cast<std::uint64_t>(negative ? -numerator : numerator);
  auto [quotient, remainder] = MultiplyDivide(magnitude, static_cast<std::uint64_t>(multiplier),
                                              static_cast<std::uint64_t>(denominator));

  // Rounding is decided on the magnitude, then the sign is reapplied, so that
  // "away from zero" and floor/ceil mean what they did before this was widened.
  bool increment = false;
  if (remainder != 0) {
    switch (mode) {
      case RoundingMode::Exact: throw std::invalid_argument("RationalTime rescale is not exact");
      case RoundingMode::Floor: increment = negative; break;
      case RoundingMode::Ceil: increment = !negative; break;
      // Half rounds away from zero: the remainder is at least half the divisor.
      case RoundingMode::Nearest:
        increment = remainder >= static_cast<std::uint64_t>(denominator) - remainder;
        break;
    }
  }
  if (increment) {
    if (quotient == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("RationalTime multiplication overflow");
    ++quotient;
  }
  // Narrow once, here. A negative result may reach magnitude 2^63; kMin is not
  // representable as a RationalTime anyway, so it is excluded.
  if (quotient > static_cast<std::uint64_t>(kMax)) throw std::overflow_error("RationalTime multiplication overflow");
  const auto signed_quotient = static_cast<std::int64_t>(quotient);
  return negative ? -signed_quotient : signed_quotient;
}

std::int64_t DivideRounded(std::int64_t numerator, std::int64_t denominator, RoundingMode mode) {
  if (denominator <= 0) throw std::invalid_argument("Positive denominator required for rescaling");
  const auto quotient = numerator / denominator, remainder = numerator % denominator;
  if (remainder == 0) return quotient;
  if (mode == RoundingMode::Exact) throw std::invalid_argument("RationalTime rescale is not exact");
  if (mode == RoundingMode::Floor) return numerator < 0 ? quotient - 1 : quotient;
  if (mode == RoundingMode::Ceil) return numerator > 0 ? quotient + 1 : quotient;
  return Absolute(remainder) >= (denominator + 1) / 2 ? (numerator > 0 ? quotient + 1 : quotient - 1) : quotient;
}
int NominalFrameRate(FrameRate rate) { if (rate.numerator <= 0 || rate.denominator <= 0) throw std::invalid_argument("FrameRate must be positive"); return static_cast<int>((rate.numerator + rate.denominator / 2) / rate.denominator); }
int DropFrames(FrameRate rate) { if (rate.numerator == 30000 && rate.denominator == 1001) return 2; if (rate.numerator == 60000 && rate.denominator == 1001) return 4; throw std::invalid_argument("Drop-frame timecode requires 30000/1001 or 60000/1001"); }
std::array<int, 4> ParseParts(const std::string& value, bool* semicolon) {
  if (value.size() != 11 || value[2] != ':' || value[5] != ':' || (value[8] != ':' && value[8] != ';')) throw std::invalid_argument("Timecode must be HH:MM:SS:FF or HH:MM:SS;FF");
  *semicolon = value[8] == ';'; std::array<int, 4> parts{}; constexpr std::array<int, 4> starts{0, 3, 6, 9};
  for (std::size_t i = 0; i < parts.size(); ++i) { const auto p = starts[i]; if (value[p] < '0' || value[p] > '9' || value[p + 1] < '0' || value[p + 1] > '9') throw std::invalid_argument("Timecode contains non-numeric fields"); parts[i] = (value[p] - '0') * 10 + value[p + 1] - '0'; }
  return parts;
}
std::string TwoDigits(std::int64_t value) { std::ostringstream output; output << std::setw(2) << std::setfill('0') << value; return output.str(); }
}  // namespace

RationalTime::RationalTime(std::int64_t numerator, std::int64_t denominator) : numerator_(numerator), denominator_(denominator) {
  if (denominator_ == 0) throw std::invalid_argument("RationalTime denominator cannot be zero");
  if (numerator_ == kMin || denominator_ == kMin) throw std::overflow_error("RationalTime value is outside the supported range");
  if (denominator_ < 0) { numerator_ = -numerator_; denominator_ = -denominator_; }
  const auto divisor = std::gcd(numerator_, denominator_); numerator_ /= divisor; denominator_ /= divisor;
}
RationalTime RationalTime::FromFrames(std::int64_t frames, FrameRate rate) { if (rate.numerator <= 0 || rate.denominator <= 0) throw std::invalid_argument("FrameRate must be positive"); return {CheckedMultiply(frames, rate.denominator), rate.numerator}; }
std::int64_t RationalTime::ToFrames(FrameRate rate, RoundingMode rounding) const {
  if (rate.numerator <= 0 || rate.denominator <= 0) throw std::invalid_argument("FrameRate must be positive");
  // frames = time * rate.numerator / rate.denominator, with the denominator
  // being this time's own times the rate's.
  return ScaledRound(numerator_, rate.numerator, CheckedMultiply(denominator_, rate.denominator), rounding);
}
std::int64_t RationalTime::Rescale(std::int64_t target, RoundingMode rounding) const {
  if (target <= 0) throw std::invalid_argument("Target denominator must be positive");
  return ScaledRound(numerator_, target, denominator_, rounding);
}
RationalTime RationalTime::Add(const RationalTime& other) const { const auto gcd = std::gcd(denominator_, other.denominator_); const auto ls = other.denominator_ / gcd, rs = denominator_ / gcd; return {CheckedAdd(CheckedMultiply(numerator_, ls), CheckedMultiply(other.numerator_, rs)), CheckedMultiply(denominator_, ls)}; }
RationalTime RationalTime::Subtract(const RationalTime& other) const { if (other.numerator_ == kMin) throw std::overflow_error("RationalTime subtraction overflow"); return Add({-other.numerator_, other.denominator_}); }
RationalTime RationalTime::Multiply(std::int64_t factor) const { return {CheckedMultiply(numerator_, factor), denominator_}; }
RationalTime RationalTime::Multiply(const RationalTime& factor) const { const auto left = std::gcd(Absolute(numerator_), factor.denominator_), right = std::gcd(Absolute(factor.numerator_), denominator_); return {CheckedMultiply(numerator_ / left, factor.numerator_ / right), CheckedMultiply(denominator_ / right, factor.denominator_ / left)}; }
RationalTime RationalTime::Divide(std::int64_t divisor) const { if (divisor == 0) throw std::invalid_argument("RationalTime division by zero"); return Divide({divisor, 1}); }
RationalTime RationalTime::Divide(const RationalTime& divisor) const { if (divisor.numerator_ == 0) throw std::invalid_argument("RationalTime division by zero"); return Multiply({divisor.denominator_, divisor.numerator_}); }
int RationalTime::Compare(const RationalTime& other) const { const auto gcd = std::gcd(denominator_, other.denominator_); const auto left = CheckedMultiply(numerator_, other.denominator_ / gcd), right = CheckedMultiply(other.numerator_, denominator_ / gcd); return left == right ? 0 : (left < right ? -1 : 1); }
std::int64_t RationalTime::ToTicks() const {
  // Lossless for every supported rate, and exact: the tick rate is divisible by
  // each broadcast frame rate and sample rate, so no rounding occurs in practice.
  return ScaledRound(numerator_, kTicksPerSecond, denominator_, RoundingMode::Nearest);
}
RationalTime RationalTime::FromTicks(std::int64_t ticks) { return {ticks, kTicksPerSecond}; }
RationalTime RationalTime::ParseTimecode(const std::string& value, FrameRate rate, bool drop) {
  bool semicolon = false; const auto parts = ParseParts(value, &semicolon); const auto nominal = NominalFrameRate(rate);
  if (parts[1] >= 60 || parts[2] >= 60 || parts[3] >= nominal) throw std::invalid_argument("Timecode field is out of range");
  if (drop != semicolon) throw std::invalid_argument("Timecode delimiter does not match drop-frame mode");
  auto frames = ((static_cast<std::int64_t>(parts[0]) * 60 + parts[1]) * 60 + parts[2]) * nominal + parts[3];
  if (drop) { const auto dropped = DropFrames(rate); if (parts[2] == 0 && parts[1] % 10 != 0 && parts[3] < dropped) throw std::invalid_argument("Drop-frame timecode references a skipped frame number"); const auto minutes = static_cast<std::int64_t>(parts[0]) * 60 + parts[1]; frames -= dropped * (minutes - minutes / 10); }
  return FromFrames(frames, rate);
}
std::string RationalTime::FormatTimecode(FrameRate rate, bool drop, RoundingMode rounding) const {
  auto frames = ToFrames(rate, rounding); const bool negative = frames < 0; if (negative) frames = -frames; const auto nominal = NominalFrameRate(rate);
  if (drop) { const auto dropped = DropFrames(rate), per_minute = nominal * 60 - dropped, per_ten = nominal * 600 - dropped * 9; const auto tens = frames / per_ten, rest = frames % per_ten; frames += static_cast<std::int64_t>(dropped) * 9 * tens; if (rest >= dropped) frames += static_cast<std::int64_t>(dropped) * ((rest - dropped) / per_minute); }
  return std::string(negative ? "-" : "") + TwoDigits(frames / (nominal * 3600)) + ":" + TwoDigits((frames / (nominal * 60)) % 60) + ":" + TwoDigits((frames / nominal) % 60) + (drop ? ";" : ":") + TwoDigits(frames % nominal);
}
}  // namespace cutline::time

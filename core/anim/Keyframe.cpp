#include "core/anim/Keyframe.h"

#include <algorithm>
#include <optional>
#include <cmath>
#include <stdexcept>

namespace cutline::anim {
namespace {

// One coordinate of a cubic bezier whose end points are 0 and 1:
// control values (0, a, b, 1) at parameter t.
double BezierCoordinate(double a, double b, double t) {
  const auto inverse = 1.0 - t;
  return 3.0 * inverse * inverse * t * a + 3.0 * inverse * t * t * b + t * t * t;
}

double BezierSlope(double a, double b, double t) {
  const auto inverse = 1.0 - t;
  return 3.0 * inverse * inverse * a + 6.0 * inverse * t * (b - a) + 3.0 * t * t * (1.0 - b);
}

// The curve parameter at which x(t) == x, for a timing curve with control points
// (0,0), (x1,y1), (x2,y2), (1,1): the standard CSS / After Effects solve.
// Newton-Raphson converges in a handful of iterations for monotonic handles; the
// bisection fallback keeps it correct if a handle makes the curve steep.
double SolveBezierParameter(double x, double x1, double x2) {
  double guess = x;
  for (int iteration = 0; iteration < 8; ++iteration) {
    const auto error = BezierCoordinate(x1, x2, guess) - x;
    if (std::abs(error) < 1e-7) return guess;
    const auto derivative = BezierSlope(x1, x2, guess);
    if (std::abs(derivative) < 1e-9) break;
    guess -= error / derivative;
  }
  double low = 0.0;
  double high = 1.0;
  guess = x;
  for (int iteration = 0; iteration < 32; ++iteration) {
    const auto current = BezierCoordinate(x1, x2, guess);
    if (std::abs(current - x) < 1e-7) break;
    if (current < x) low = guess; else high = guess;
    guess = (low + high) * 0.5;
  }
  return guess;
}

double BezierEase(double x, double x1, double y1, double x2, double y2) {
  return BezierCoordinate(y1, y2, SolveBezierParameter(x, x1, x2));
}

double ApplyEasing(Interpolation interpolation, double t, const Keyframe& from, const Keyframe& to) {
  switch (interpolation) {
    case Interpolation::Hold: return 0.0;
    case Interpolation::Linear: return t;
    case Interpolation::EaseIn: return BezierEase(t, 0.42, 0.0, 1.0, 1.0);
    case Interpolation::EaseOut: return BezierEase(t, 0.0, 0.0, 0.58, 1.0);
    case Interpolation::EaseInOut: return BezierEase(t, 0.42, 0.0, 0.58, 1.0);
    case Interpolation::Bezier:
      return BezierEase(t, from.out_handle.x, from.out_handle.y, to.in_handle.x, to.in_handle.y);
  }
  return t;
}

}  // namespace

std::string ToString(Interpolation interpolation) {
  switch (interpolation) {
    case Interpolation::Hold: return "hold";
    case Interpolation::Linear: return "linear";
    case Interpolation::Bezier: return "bezier";
    case Interpolation::EaseIn: return "ease_in";
    case Interpolation::EaseOut: return "ease_out";
    case Interpolation::EaseInOut: return "ease_in_out";
  }
  throw std::invalid_argument("Unknown interpolation mode");
}

Interpolation ParseInterpolation(const std::string& name) {
  if (name == "hold") return Interpolation::Hold;
  if (name == "linear") return Interpolation::Linear;
  if (name == "bezier") return Interpolation::Bezier;
  if (name == "ease_in") return Interpolation::EaseIn;
  if (name == "ease_out") return Interpolation::EaseOut;
  if (name == "ease_in_out") return Interpolation::EaseInOut;
  throw std::invalid_argument("Unknown interpolation mode: " + name);
}

Value Value::Scalar(double value) { return Value{{value, 0.0, 0.0, 0.0}, 1}; }
Value Value::Vec2(double x, double y) { return Value{{x, y, 0.0, 0.0}, 2}; }
Value Value::Vec3(double x, double y, double z) { return Value{{x, y, z, 0.0}, 3}; }
Value Value::Vec4(double x, double y, double z, double w) { return Value{{x, y, z, w}, 4}; }

bool Value::Equals(const Value& other) const {
  if (dimension != other.dimension) return false;
  for (int index = 0; index < dimension; ++index) {
    if (std::abs(components[static_cast<std::size_t>(index)] - other.components[static_cast<std::size_t>(index)]) > 1e-12) {
      return false;
    }
  }
  return true;
}

Value Lerp(const Value& from, const Value& to, double t) {
  if (from.dimension != to.dimension) throw std::invalid_argument("Cannot interpolate values of different dimension");
  Value result{};
  result.dimension = from.dimension;
  for (int index = 0; index < from.dimension; ++index) {
    const auto slot = static_cast<std::size_t>(index);
    result.components[slot] = from.components[slot] + (to.components[slot] - from.components[slot]) * t;
  }
  return result;
}

AnimatedValue::AnimatedValue(Value constant) : constant_(constant) {}

AnimatedValue::AnimatedValue(std::vector<Keyframe> keyframes) : keyframes_(std::move(keyframes)) {
  std::sort(keyframes_.begin(), keyframes_.end(),
            [](const Keyframe& left, const Keyframe& right) { return left.time.Compare(right.time) < 0; });
  for (std::size_t index = 1; index < keyframes_.size(); ++index) {
    if (keyframes_[index].time.Compare(keyframes_[index - 1].time) == 0) {
      throw std::invalid_argument("Two keyframes share the same time");
    }
  }
  if (!keyframes_.empty()) constant_ = keyframes_.front().value;
}

void AnimatedValue::SetKeyframe(Keyframe keyframe) {
  const auto position = std::lower_bound(
      keyframes_.begin(), keyframes_.end(), keyframe.time,
      [](const Keyframe& candidate, const time::RationalTime& time) { return candidate.time.Compare(time) < 0; });
  if (position != keyframes_.end() && position->time.Compare(keyframe.time) == 0) {
    *position = std::move(keyframe);
    return;
  }
  keyframes_.insert(position, std::move(keyframe));
}

bool AnimatedValue::RemoveKeyframe(const time::RationalTime& at) {
  const auto position = std::lower_bound(
      keyframes_.begin(), keyframes_.end(), at,
      [](const Keyframe& candidate, const time::RationalTime& time) { return candidate.time.Compare(time) < 0; });
  if (position == keyframes_.end() || position->time.Compare(at) != 0) return false;
  keyframes_.erase(position);
  return true;
}

Value AnimatedValue::Sample(const time::RationalTime& at) const {
  if (keyframes_.empty()) return constant_;
  if (keyframes_.size() == 1 || at.Compare(keyframes_.front().time) <= 0) return keyframes_.front().value;
  if (at.Compare(keyframes_.back().time) >= 0) return keyframes_.back().value;

  // First keyframe strictly after `at`; the span is [previous, position].
  const auto position = std::upper_bound(
      keyframes_.begin(), keyframes_.end(), at,
      [](const time::RationalTime& time, const Keyframe& candidate) { return time.Compare(candidate.time) < 0; });
  const auto& to = *position;
  const auto& from = *std::prev(position);

  if (from.interpolation == Interpolation::Hold) return from.value;

  // Normalised position within the span, computed in rational space and only
  // then reduced to a double for the easing solve.
  const auto span = to.time.Subtract(from.time);
  const auto offset = at.Subtract(from.time);
  const auto ratio = offset.Divide(span);
  const auto t = static_cast<double>(ratio.numerator()) / static_cast<double>(ratio.denominator());

  return Lerp(from.value, to.value, ApplyEasing(from.interpolation, t, from, to));
}

namespace {

struct Point final {
  double x{};
  double y{};
};

[[nodiscard]] Point Mix(const Point& from, const Point& to, double t) {
  return {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
}

// The timing curve of a segment as bezier control points, or nullopt for the
// two modes that are not curves.
struct TimingCurve final {
  Point p1;
  Point p2;
};

[[nodiscard]] std::optional<TimingCurve> TimingCurveOf(const Keyframe& from, const Keyframe& to) {
  switch (from.interpolation) {
    case Interpolation::EaseIn: return TimingCurve{{0.42, 0.0}, {1.0, 1.0}};
    case Interpolation::EaseOut: return TimingCurve{{0.0, 0.0}, {0.58, 1.0}};
    case Interpolation::EaseInOut: return TimingCurve{{0.42, 0.0}, {0.58, 1.0}};
    case Interpolation::Bezier:
      return TimingCurve{{from.out_handle.x, from.out_handle.y}, {to.in_handle.x, to.in_handle.y}};
    case Interpolation::Hold:
    case Interpolation::Linear: return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] double AsDouble(const time::RationalTime& value) {
  return static_cast<double>(value.numerator()) / static_cast<double>(value.denominator());
}

// A tiny divisor would blow up a normalisation; fall back to leaving the axis alone.
[[nodiscard]] double SafeReciprocal(double value) { return std::abs(value) > 1e-12 ? 1.0 / value : 1.0; }

}  // namespace

SplitCurves SplitKeyframes(const std::vector<Keyframe>& keyframes, const time::RationalTime& at) {
  SplitCurves result;
  if (keyframes.empty()) return result;
  for (std::size_t index = 1; index < keyframes.size(); ++index) {
    if (keyframes[index].time.Compare(keyframes[index - 1].time) <= 0) {
      throw std::invalid_argument("Keyframes must be sorted by strictly increasing time to be split");
    }
  }

  const auto rebase = [&at](Keyframe key) {
    key.time = key.time.Subtract(at);
    return key;
  };

  // First key strictly after the cut.
  const auto after = static_cast<std::size_t>(
      std::upper_bound(keyframes.begin(), keyframes.end(), at,
                       [](const time::RationalTime& time, const Keyframe& key) { return time.Compare(key.time) < 0; }) -
      keyframes.begin());
  const bool on_a_key = after > 0 && keyframes[after - 1].time.Compare(at) == 0;

  if (after == 0) {
    // Before the first key: sampling holds the first key's value, so the left
    // half is a hold of it, and the right half is the whole curve, rebased.
    result.left.push_back(keyframes.front());
    for (const auto& key : keyframes) result.right.push_back(rebase(key));
    return result;
  }
  if (on_a_key) {
    // The cut is on a key, which both halves keep.
    result.left.assign(keyframes.begin(), keyframes.begin() + static_cast<std::ptrdiff_t>(after));
    for (std::size_t index = after - 1; index < keyframes.size(); ++index) {
      result.right.push_back(rebase(keyframes[index]));
    }
    return result;
  }
  if (after == keyframes.size()) {
    // After the last key: the right half holds its value.
    result.left = keyframes;
    auto held = rebase(keyframes.back());
    held.interpolation = Interpolation::Linear;
    result.right.push_back(held);
    return result;
  }

  // Inside a segment: A is the key before the cut and B the key after it.
  const auto& a = keyframes[after - 1];
  const auto& b = keyframes[after];
  const auto fraction = at.Subtract(a.time).Divide(b.time.Subtract(a.time));
  const auto u = AsDouble(fraction);

  Value boundary_value = a.value;
  const auto timing = TimingCurveOf(a, b);
  if (a.interpolation == Interpolation::Hold) {
    boundary_value = a.value;
  } else if (a.interpolation == Interpolation::Linear) {
    boundary_value = Lerp(a.value, b.value, u);
  } else {
    boundary_value = Lerp(a.value, b.value, BezierEase(u, timing->p1.x, timing->p1.y, timing->p2.x, timing->p2.y));
  }

  result.left.assign(keyframes.begin(), keyframes.begin() + static_cast<std::ptrdiff_t>(after));
  Keyframe left_boundary{at, boundary_value, Interpolation::Linear, {}, {}};
  Keyframe right_boundary{time::RationalTime{0, 1}, boundary_value, a.interpolation, a.out_handle, {}};

  if (timing.has_value()) {
    // De Casteljau at the curve parameter where x equals the cut, giving two
    // curves that together trace the original.
    const Point p0{0.0, 0.0};
    const Point p3{1.0, 1.0};
    const auto t = SolveBezierParameter(u, timing->p1.x, timing->p2.x);
    const auto p01 = Mix(p0, timing->p1, t);
    const auto p12 = Mix(timing->p1, timing->p2, t);
    const auto p23 = Mix(timing->p2, p3, t);
    const auto p012 = Mix(p01, p12, t);
    const auto p123 = Mix(p12, p23, t);
    const auto cut = Mix(p012, p123, t);  // the point on the curve at x == u

    // Left curve: p0, p01, p012, cut. Normalise so it ends at (1,1).
    const auto left_x = SafeReciprocal(cut.x);
    const auto left_y = SafeReciprocal(cut.y);
    // Right curve: cut, p123, p23, p3. Normalise so it runs (0,0) to (1,1).
    const auto right_x = SafeReciprocal(1.0 - cut.x);
    const auto right_y = SafeReciprocal(1.0 - cut.y);

    result.left.back().interpolation = Interpolation::Bezier;
    result.left.back().out_handle = {p01.x * left_x, p01.y * left_y};
    left_boundary.in_handle = {p012.x * left_x, p012.y * left_y};

    right_boundary.interpolation = Interpolation::Bezier;
    right_boundary.out_handle = {(p123.x - cut.x) * right_x, (p123.y - cut.y) * right_y};

    result.right.push_back(right_boundary);
    auto next = rebase(b);
    next.in_handle = {(p23.x - cut.x) * right_x, (p23.y - cut.y) * right_y};
    result.right.push_back(next);
  } else {
    result.right.push_back(right_boundary);
    result.right.push_back(rebase(b));
  }
  result.left.push_back(left_boundary);
  for (std::size_t index = after + 1; index < keyframes.size(); ++index) result.right.push_back(rebase(keyframes[index]));
  return result;
}

}  // namespace cutline::anim

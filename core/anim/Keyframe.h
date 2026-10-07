#pragma once

// Animated parameter values.
//
// Every effect parameter in the project is either a constant or a keyframed
// curve, so this is the type the render graph samples once per frame. Keyframe
// times are exact rational times (never floats) so that a keyframe placed on a
// frame boundary at 23.976 resolves to that same frame after a sequence rate
// change.

#include "core/time/RationalTime.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cutline::anim {

// How a value travels from this keyframe to the next one.
enum class Interpolation {
  Hold,    // step: value stays until the next keyframe
  Linear,
  Bezier,  // cubic bezier using the two handles below
  EaseIn,
  EaseOut,
  EaseInOut,
};

[[nodiscard]] std::string ToString(Interpolation interpolation);
[[nodiscard]] Interpolation ParseInterpolation(const std::string& name);

// A parameter value. Premiere's inspector needs scalars, 2D/3D points, colours
// and booleans; one fixed-width vector covers all of them without a variant per
// parameter kind, and `dimension` records how many components are meaningful.
struct Value final {
  std::array<double, 4> components{};
  int dimension{1};

  [[nodiscard]] static Value Scalar(double value);
  [[nodiscard]] static Value Vec2(double x, double y);
  [[nodiscard]] static Value Vec3(double x, double y, double z);
  [[nodiscard]] static Value Vec4(double x, double y, double z, double w);
  [[nodiscard]] double scalar() const { return components[0]; }
  [[nodiscard]] bool Equals(const Value& other) const;
};

[[nodiscard]] Value Lerp(const Value& from, const Value& to, double t);

// Bezier handles are stored in normalised keyframe space: x is the fraction of
// the span to the neighbouring keyframe, y the fraction of the value delta.
// That keeps a curve's shape intact when a clip is retimed or a keyframe moves.
struct BezierHandle final {
  double x{0.0};
  double y{0.0};
};

struct Keyframe final {
  time::RationalTime time;
  Value value;
  Interpolation interpolation{Interpolation::Linear};
  BezierHandle out_handle{0.33, 0.0};  // leaving this keyframe
  BezierHandle in_handle{0.67, 0.0};   // arriving at this keyframe
};

// An ordered keyframe track for one parameter. Sampling outside the first or
// last keyframe holds the end value, which is what every NLE does.
class AnimatedValue final {
 public:
  AnimatedValue() = default;
  explicit AnimatedValue(Value constant);
  explicit AnimatedValue(std::vector<Keyframe> keyframes);

  // Replaces or inserts the keyframe at this exact time, keeping order.
  void SetKeyframe(Keyframe keyframe);
  // Returns false when no keyframe existed at that time.
  bool RemoveKeyframe(const time::RationalTime& at);

  [[nodiscard]] bool animated() const noexcept { return !keyframes_.empty(); }
  [[nodiscard]] const std::vector<Keyframe>& keyframes() const noexcept { return keyframes_; }
  [[nodiscard]] const Value& constant() const noexcept { return constant_; }
  void set_constant(Value value) { constant_ = value; }

  // Samples the curve. O(log n) in the keyframe count.
  [[nodiscard]] Value Sample(const time::RationalTime& at) const;

 private:
  Value constant_{};
  std::vector<Keyframe> keyframes_;
};

// The result of cutting an animation curve in two.
struct SplitCurves final {
  // Clip-local times, unchanged. Covers [0, at].
  std::vector<Keyframe> left;
  // Rebased so that `at` becomes time zero. Covers [at, end).
  std::vector<Keyframe> right;
};

// Cuts a keyframed curve at `at` (clip-local time) so that each half, sampled in
// its own clip-local time, reproduces the original exactly.
//
// This is what a razor cut needs. Keyframe times are relative to the start of the
// clip, and the right half starts later, so copying keyframes unchanged restarts
// its animation. Rebasing the times is not enough either: a cut inside a segment
// leaves each half with only part of that segment's curve, so a boundary key with
// the sampled value is needed, and for an eased or Bezier segment the *timing
// curve itself* has to be divided (de Casteljau at the parameter where x equals
// the cut) and each piece renormalised, or the two halves would ease over the
// wrong span. Eased segments become explicit Bezier handles in the process.
//
// `keyframes` must be sorted by strictly increasing time and share one dimension.
// An empty input gives two empty results. One case cannot be represented exactly:
// a Bezier that overshoots and returns to its starting value exactly at the cut
// has no unit-square normalisation, so the half affected is approximated flat.
[[nodiscard]] SplitCurves SplitKeyframes(const std::vector<Keyframe>& keyframes, const time::RationalTime& at);

}  // namespace cutline::anim

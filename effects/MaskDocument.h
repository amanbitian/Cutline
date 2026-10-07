#pragma once

// Versioned, effect-owned geometric masks. Coordinates are normalized to the
// picture; feather and expansion are pixels. Animation time is local to the
// effect owner (clip-local for clip effects, sequence time otherwise).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cutline::effects::mask {

inline constexpr std::int64_t kDocumentVersion = 1;

enum class Shape { Rectangle, Ellipse, Bezier };
enum class Combine { Add, Subtract, Intersect };

struct Key final {
  double time{0.0};
  double value{0.0};
};

struct Animation final {
  std::string property;
  std::string interpolation{"linear"};
  std::vector<Key> keys;
};

struct Point final {
  double x{0.0};
  double y{0.0};
  // Absolute cubic control points. A handle equal to the point makes that side
  // a straight segment and keeps polygon masks compact.
  double in_x{0.0};
  double in_y{0.0};
  double out_x{0.0};
  double out_y{0.0};
};

struct Document final {
  std::int64_t schema_version{kDocumentVersion};
  Shape shape{Shape::Rectangle};
  Combine combine{Combine::Add};
  bool inverted{false};
  double center_x{0.5};
  double center_y{0.5};
  double width{0.5};
  double height{0.5};
  double rotation{0.0};
  double feather{0.0};
  double expansion{0.0};
  double opacity{1.0};
  std::vector<Point> points;
  std::vector<Animation> animations;
};

[[nodiscard]] Document Parse(const std::string& json);
[[nodiscard]] std::string ToJson(const Document& document);
void Validate(const Document& document);

[[nodiscard]] Document Evaluate(const Document& document, double seconds);
// Splits local animation at `seconds`; the right side is rebased to zero.
[[nodiscard]] std::pair<Document, Document> Split(const Document& document, double seconds);
// Moves animation in local time. Keys trimmed before zero are replaced by a
// sampled boundary key so the visible result remains continuous.
[[nodiscard]] Document Shift(const Document& document, double seconds);

[[nodiscard]] const char* ToString(Shape shape);
[[nodiscard]] const char* ToString(Combine combine);

}  // namespace cutline::effects::mask

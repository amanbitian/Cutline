#include "effects/MaskDocument.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace cutline::effects::mask {
namespace {

[[nodiscard]] const json::Value* Optional(const json::Value& value, std::string_view name) { return value.Find(name); }

[[nodiscard]] double Number(const json::Value& value, std::string_view name, double fallback) {
  const auto* field = Optional(value, name);
  if (field == nullptr) return fallback;
  if (!field->is_number()) throw std::invalid_argument("Mask property '" + std::string(name) + "' must be a number");
  return field->number;
}

[[nodiscard]] bool Boolean(const json::Value& value, std::string_view name, bool fallback) {
  const auto* field = Optional(value, name);
  if (field == nullptr) return fallback;
  if (field->kind != json::Value::Kind::Bool) throw std::invalid_argument("Mask property '" + std::string(name) + "' must be true or false");
  return field->boolean;
}

[[nodiscard]] Shape ParseShape(const std::string& value) {
  if (value == "rectangle") return Shape::Rectangle;
  if (value == "ellipse") return Shape::Ellipse;
  if (value == "bezier") return Shape::Bezier;
  throw std::invalid_argument("Unknown mask shape: " + value);
}

[[nodiscard]] Combine ParseCombine(const std::string& value) {
  if (value == "add") return Combine::Add;
  if (value == "subtract") return Combine::Subtract;
  if (value == "intersect") return Combine::Intersect;
  throw std::invalid_argument("Unknown mask combine mode: " + value);
}

[[nodiscard]] bool KnownInterpolation(const std::string& value) {
  return value == "linear" || value == "hold" || value == "ease_in" || value == "ease_out" || value == "ease_in_out";
}

[[nodiscard]] bool PointProperty(const std::string& property, std::size_t point_count) {
  if (property.rfind("point_", 0) != 0) return false;
  const auto separator = property.find('_', 6);
  if (separator == std::string::npos) return false;
  std::size_t used = 0;
  std::size_t index = 0;
  try {
    index = static_cast<std::size_t>(std::stoull(property.substr(6, separator - 6), &used));
  } catch (...) {
    return false;
  }
  if (used != separator - 6 || index >= point_count) return false;
  const auto component = property.substr(separator + 1);
  return component == "x" || component == "y" || component == "in_x" || component == "in_y" ||
         component == "out_x" || component == "out_y";
}

[[nodiscard]] bool KnownProperty(const std::string& property, std::size_t point_count) {
  static const std::set<std::string> scalar{"center_x", "center_y", "width", "height", "rotation", "feather", "expansion", "opacity"};
  return scalar.count(property) != 0 || PointProperty(property, point_count);
}

[[nodiscard]] double Ease(const std::string& interpolation, double t) {
  if (interpolation == "ease_in") return t * t;
  if (interpolation == "ease_out") return 1.0 - (1.0 - t) * (1.0 - t);
  if (interpolation == "ease_in_out") return t * t * (3.0 - 2.0 * t);
  return t;
}

[[nodiscard]] double Sample(const Animation& animation, double seconds) {
  if (animation.keys.empty()) return 0.0;
  if (seconds <= animation.keys.front().time) return animation.keys.front().value;
  if (seconds >= animation.keys.back().time) return animation.keys.back().value;
  const auto right = std::upper_bound(animation.keys.begin(), animation.keys.end(), seconds,
                                      [](double time, const Key& key) { return time < key.time; });
  const auto left = std::prev(right);
  if (animation.interpolation == "hold") return left->value;
  auto t = (seconds - left->time) / (right->time - left->time);
  t = Ease(animation.interpolation, std::clamp(t, 0.0, 1.0));
  return left->value + (right->value - left->value) * t;
}

void SetProperty(Document& document, const std::string& property, double value) {
  if (property == "center_x") document.center_x = value;
  else if (property == "center_y") document.center_y = value;
  else if (property == "width") document.width = value;
  else if (property == "height") document.height = value;
  else if (property == "rotation") document.rotation = value;
  else if (property == "feather") document.feather = value;
  else if (property == "expansion") document.expansion = value;
  else if (property == "opacity") document.opacity = value;
  else {
    const auto separator = property.find('_', 6);
    const auto index = static_cast<std::size_t>(std::stoull(property.substr(6, separator - 6)));
    auto& point = document.points[index];
    const auto component = property.substr(separator + 1);
    if (component == "x") point.x = value;
    else if (component == "y") point.y = value;
    else if (component == "in_x") point.in_x = value;
    else if (component == "in_y") point.in_y = value;
    else if (component == "out_x") point.out_x = value;
    else if (component == "out_y") point.out_y = value;
  }
}

[[nodiscard]] std::string KeyJson(const Key& key) {
  return json::Object().Add("time", key.time).Add("value", key.value).Build();
}

[[nodiscard]] std::string AnimationJson(const Animation& animation) {
  std::vector<std::string> keys;
  for (const auto& key : animation.keys) keys.push_back(KeyJson(key));
  return json::Object().Add("property", animation.property).Add("interpolation", animation.interpolation)
      .AddRaw("keys", json::Array(keys)).Build();
}

}  // namespace

const char* ToString(Shape shape) {
  switch (shape) {
    case Shape::Rectangle: return "rectangle";
    case Shape::Ellipse: return "ellipse";
    case Shape::Bezier: return "bezier";
  }
  return "rectangle";
}

const char* ToString(Combine combine) {
  switch (combine) {
    case Combine::Add: return "add";
    case Combine::Subtract: return "subtract";
    case Combine::Intersect: return "intersect";
  }
  return "add";
}

void Validate(const Document& document) {
  if (document.schema_version < 1 || document.schema_version > kDocumentVersion) {
    throw std::invalid_argument("Mask document version is not supported");
  }
  const double values[]{document.center_x, document.center_y, document.width, document.height, document.rotation,
                        document.feather, document.expansion, document.opacity};
  for (double value : values) if (!std::isfinite(value)) throw std::invalid_argument("Mask geometry must be finite");
  if (document.width < 0.0 || document.height < 0.0 || document.feather < 0.0 ||
      document.opacity < 0.0 || document.opacity > 1.0) {
    throw std::invalid_argument("Mask size, feather or opacity is invalid");
  }
  if (document.shape == Shape::Bezier && document.points.size() < 3) {
    throw std::invalid_argument("A Bezier mask needs at least three points");
  }
  if (document.points.size() > 512) throw std::invalid_argument("Mask has too many points");
  for (const auto& point : document.points) {
    const double coordinates[]{point.x, point.y, point.in_x, point.in_y, point.out_x, point.out_y};
    for (double value : coordinates) if (!std::isfinite(value)) throw std::invalid_argument("Mask point must be finite");
  }
  std::set<std::string> animated;
  for (const auto& animation : document.animations) {
    if (!KnownProperty(animation.property, document.points.size())) throw std::invalid_argument("Mask cannot animate '" + animation.property + "'");
    if (!animated.insert(animation.property).second) throw std::invalid_argument("Mask animates '" + animation.property + "' twice");
    if (!KnownInterpolation(animation.interpolation)) throw std::invalid_argument("Unknown mask interpolation: " + animation.interpolation);
    if (animation.keys.empty() || animation.keys.size() > 512) throw std::invalid_argument("Mask animation needs between 1 and 512 keys");
    for (std::size_t index = 0; index < animation.keys.size(); ++index) {
      const auto& key = animation.keys[index];
      if (!std::isfinite(key.time) || !std::isfinite(key.value) || key.time < 0.0) throw std::invalid_argument("Mask key is invalid");
      if (index > 0 && key.time <= animation.keys[index - 1].time) throw std::invalid_argument("Mask keys must be strictly increasing");
    }
  }
}

Document Parse(const std::string& text) {
  const auto root = json::Parse(text);
  if (!root.is_object()) throw std::invalid_argument("Mask document must be an object");
  Document document;
  document.schema_version = root.Integer("schema_version");
  document.shape = ParseShape(root.String("shape"));
  document.combine = ParseCombine(root.String("combine"));
  document.inverted = Boolean(root, "inverted", false);
  document.center_x = Number(root, "center_x", 0.5);
  document.center_y = Number(root, "center_y", 0.5);
  document.width = Number(root, "width", 0.5);
  document.height = Number(root, "height", 0.5);
  document.rotation = Number(root, "rotation", 0.0);
  document.feather = Number(root, "feather", 0.0);
  document.expansion = Number(root, "expansion", 0.0);
  document.opacity = Number(root, "opacity", 1.0);
  if (const auto* points = Optional(root, "points")) {
    if (!points->is_array()) throw std::invalid_argument("Mask points must be an array");
    for (const auto& value : points->items) {
      if (!value.is_object()) throw std::invalid_argument("Mask point must be an object");
      Point point;
      point.x = value.Number("x"); point.y = value.Number("y");
      point.in_x = Number(value, "in_x", point.x); point.in_y = Number(value, "in_y", point.y);
      point.out_x = Number(value, "out_x", point.x); point.out_y = Number(value, "out_y", point.y);
      document.points.push_back(point);
    }
  }
  if (const auto* animations = Optional(root, "animations")) {
    if (!animations->is_array()) throw std::invalid_argument("Mask animations must be an array");
    for (const auto& value : animations->items) {
      if (!value.is_object()) throw std::invalid_argument("Mask animation must be an object");
      Animation animation;
      animation.property = value.String("property");
      animation.interpolation = value.String("interpolation");
      const auto& keys = value.Require("keys");
      if (!keys.is_array()) throw std::invalid_argument("Mask animation keys must be an array");
      for (const auto& key : keys.items) {
        if (!key.is_object()) throw std::invalid_argument("Mask key must be an object");
        animation.keys.push_back({key.Number("time"), key.Number("value")});
      }
      document.animations.push_back(std::move(animation));
    }
  }
  Validate(document);
  return document;
}

std::string ToJson(const Document& document) {
  Validate(document);
  std::vector<std::string> points;
  for (const auto& point : document.points) {
    points.push_back(json::Object().Add("x", point.x).Add("y", point.y).Add("in_x", point.in_x).Add("in_y", point.in_y)
                         .Add("out_x", point.out_x).Add("out_y", point.out_y).Build());
  }
  std::vector<std::string> animations;
  for (const auto& animation : document.animations) animations.push_back(AnimationJson(animation));
  return json::Object().Add("schema_version", document.schema_version).Add("shape", ToString(document.shape))
      .Add("combine", ToString(document.combine)).Add("inverted", document.inverted).Add("center_x", document.center_x)
      .Add("center_y", document.center_y).Add("width", document.width).Add("height", document.height)
      .Add("rotation", document.rotation).Add("feather", document.feather).Add("expansion", document.expansion)
      .Add("opacity", document.opacity).AddRaw("points", json::Array(points)).AddRaw("animations", json::Array(animations)).Build();
}

Document Evaluate(const Document& document, double seconds) {
  auto result = document;
  result.animations.clear();
  for (const auto& animation : document.animations) SetProperty(result, animation.property, Sample(animation, seconds));
  Validate(result);
  return result;
}

std::pair<Document, Document> Split(const Document& document, double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) throw std::invalid_argument("Mask split time is invalid");
  auto left = document;
  auto right = document;
  left.animations.clear();
  right.animations.clear();
  for (const auto& animation : document.animations) {
    const auto boundary = Sample(animation, seconds);
    Animation before{animation.property, animation.interpolation, {}};
    Animation after{animation.property, animation.interpolation, {}};
    for (const auto& key : animation.keys) {
      if (key.time < seconds) before.keys.push_back(key);
      if (key.time > seconds) after.keys.push_back({key.time - seconds, key.value});
    }
    before.keys.push_back({seconds, boundary});
    after.keys.insert(after.keys.begin(), {0.0, boundary});
    left.animations.push_back(std::move(before));
    right.animations.push_back(std::move(after));
  }
  Validate(left);
  Validate(right);
  return {std::move(left), std::move(right)};
}

Document Shift(const Document& document, double seconds) {
  if (!std::isfinite(seconds)) throw std::invalid_argument("Mask shift is invalid");
  if (seconds < 0.0) return Split(document, -seconds).second;
  auto result = document;
  for (auto& animation : result.animations) {
    for (auto& key : animation.keys) key.time += seconds;
  }
  Validate(result);
  return result;
}

}  // namespace cutline::effects::mask

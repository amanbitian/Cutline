#include "effects/GraphicsDocument.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cutline::render::graphics {
namespace {

constexpr std::size_t kMaximumKeys = 512;
constexpr std::size_t kMaximumElements = 2000;

[[nodiscard]] const json::Value* Optional(const json::Value& object, std::string_view name) { return object.Find(name); }

[[nodiscard]] double Number(const json::Value& object, std::string_view name, double fallback) {
  const auto* value = Optional(object, name);
  if (value == nullptr) return fallback;
  if (!value->is_number()) throw std::invalid_argument("Graphic property '" + std::string(name) + "' must be a number");
  return value->number;
}

[[nodiscard]] std::string String(const json::Value& object, std::string_view name, std::string fallback = {}) {
  const auto* value = Optional(object, name);
  if (value == nullptr) return fallback;
  if (!value->is_string()) throw std::invalid_argument("Graphic property '" + std::string(name) + "' must be text");
  return value->text;
}

[[nodiscard]] bool Boolean(const json::Value& object, std::string_view name, bool fallback) {
  const auto* value = Optional(object, name);
  if (value == nullptr) return fallback;
  if (value->kind != json::Value::Kind::Bool) throw std::invalid_argument("Graphic property '" + std::string(name) + "' must be true or false");
  return value->boolean;
}

[[nodiscard]] std::array<double, 4> Colour(const json::Value& object, std::string_view name, std::array<double, 4> fallback) {
  const auto* value = Optional(object, name);
  if (value == nullptr) return fallback;
  if (!value->is_array() || value->items.size() != 4) throw std::invalid_argument("Graphic colour must have four components");
  for (std::size_t index = 0; index < 4; ++index) {
    if (!value->items[index].is_number()) throw std::invalid_argument("Graphic colour components must be numbers");
    fallback[index] = std::clamp(value->items[index].number, 0.0, 1.0);
  }
  return fallback;
}

[[nodiscard]] ElementType ParseType(const std::string& type) {
  if (type == "text") return ElementType::Text;
  if (type == "rectangle") return ElementType::Rectangle;
  if (type == "ellipse") return ElementType::Ellipse;
  if (type == "image") return ElementType::Image;
  throw std::invalid_argument("Unknown graphic element type: " + type);
}

[[nodiscard]] const char* TypeName(ElementType type) {
  switch (type) {
    case ElementType::Text: return "text";
    case ElementType::Rectangle: return "rectangle";
    case ElementType::Ellipse: return "ellipse";
    case ElementType::Image: return "image";
  }
  return "rectangle";
}

[[nodiscard]] bool KnownProperty(const std::string& property) {
  static const std::set<std::string> known{"x", "y", "width", "height", "opacity", "font_size", "fill_r", "fill_g", "fill_b", "fill_a",
                                           "rotation", "corner_radius", "stroke_width", "shadow_x", "shadow_y", "shadow_blur"};
  return known.count(property) != 0;
}

// Animatable properties that only schema 3 knows.
[[nodiscard]] bool EffectProperty(const std::string& property) {
  return property == "rotation" || property == "corner_radius" || property == "stroke_width" || property == "shadow_x" ||
         property == "shadow_y" || property == "shadow_blur";
}

[[nodiscard]] bool KnownInterpolation(const std::string& name) {
  return name == "linear" || name == "hold" || name == "ease_in" || name == "ease_out" || name == "ease_in_out";
}

[[nodiscard]] bool KnownAnchorX(const std::string& anchor) {
  return anchor == "none" || anchor == "left" || anchor == "center" || anchor == "right" || anchor == "stretch";
}
[[nodiscard]] bool KnownAnchorY(const std::string& anchor) {
  return anchor == "none" || anchor == "top" || anchor == "middle" || anchor == "bottom" || anchor == "stretch";
}

[[nodiscard]] Animation ParseAnimation(const json::Value& value) {
  if (!value.is_object()) throw std::invalid_argument("A graphic animation must be an object");
  Animation animation;
  animation.property = value.String("property");
  animation.interpolation = String(value, "interpolation", "linear");
  const auto& keys = value.Require("keys");
  if (!keys.is_array()) throw std::invalid_argument("Graphic animation keys must be an array");
  for (const auto& key : keys.items) {
    if (!key.is_object()) throw std::invalid_argument("A graphic animation key must be an object");
    animation.keys.push_back({key.Number("time"), key.Number("value")});
  }
  return animation;
}

[[nodiscard]] std::string ColourJson(const std::array<double, 4>& colour) {
  return json::Array({json::Number(colour[0]), json::Number(colour[1]), json::Number(colour[2]), json::Number(colour[3])});
}

[[nodiscard]] std::string AnimationJson(const Animation& animation) {
  std::vector<std::string> keys;
  for (const auto& key : animation.keys) keys.push_back(json::Object().Add("time", key.time).Add("value", key.value).Build());
  return json::Object().Add("property", animation.property).Add("interpolation", animation.interpolation).AddRaw("keys", json::Array(keys)).Build();
}

[[nodiscard]] std::string ElementJson(const Element& element) {
  auto object = json::Object();
  object.Add("id", element.id).Add("type", TypeName(element.type)).Add("x", element.x).Add("y", element.y)
      .Add("width", element.width).Add("height", element.height).Add("opacity", element.opacity)
      .AddRaw("fill", ColourJson(element.fill)).Add("text", element.text).Add("font", element.font)
      .Add("font_size", element.font_size).Add("bold", element.bold).Add("italic", element.italic)
      .Add("align", element.align).Add("asset", element.asset);
  // Written only when used, so a plain document stays what an older build wrote.
  if (element.anchor_x != "none") object.Add("anchor_x", element.anchor_x);
  if (element.anchor_y != "none") object.Add("anchor_y", element.anchor_y);
  if (element.rotation != 0.0) object.Add("rotation", element.rotation);
  if (element.corner_radius != 0.0) object.Add("corner_radius", element.corner_radius);
  if (element.stroke_width != 0.0) object.Add("stroke_width", element.stroke_width).AddRaw("stroke", ColourJson(element.stroke));
  if (element.shadow[3] > 0.0) {
    object.AddRaw("shadow", ColourJson(element.shadow)).Add("shadow_x", element.shadow_x).Add("shadow_y", element.shadow_y)
        .Add("shadow_blur", element.shadow_blur);
  }
  if (!element.animations.empty()) {
    std::vector<std::string> animations;
    for (const auto& animation : element.animations) animations.push_back(AnimationJson(animation));
    object.AddRaw("animations", json::Array(animations));
  }
  return object.Build();
}

[[nodiscard]] Document ParseDocumentValue(const json::Value& root) {
  if (!root.is_object()) throw std::invalid_argument("Graphic document must be a JSON object");
  Document result;
  result.schema_version = root.Integer("schema_version");
  result.width = static_cast<int>(root.Integer("width"));
  result.height = static_cast<int>(root.Integer("height"));
  const auto& elements = root.Require("elements");
  if (!elements.is_array()) throw std::invalid_argument("Graphic elements must be an array");
  for (const auto& value : elements.items) {
    if (!value.is_object()) throw std::invalid_argument("A graphic element must be an object");
    Element item;
    item.id = value.String("id");
    item.type = ParseType(value.String("type"));
    item.x = Number(value, "x", 0.0);
    item.y = Number(value, "y", 0.0);
    item.width = Number(value, "width", 0.0);
    item.height = Number(value, "height", 0.0);
    item.opacity = Number(value, "opacity", 1.0);
    item.fill = Colour(value, "fill", item.fill);
    item.text = String(value, "text");
    item.font = String(value, "font", "Arial");
    item.font_size = Number(value, "font_size", 0.08);
    item.bold = Boolean(value, "bold", false);
    item.italic = Boolean(value, "italic", false);
    item.align = String(value, "align", "center");
    item.asset = String(value, "asset");
    item.anchor_x = String(value, "anchor_x", "none");
    item.anchor_y = String(value, "anchor_y", "none");
    item.rotation = Number(value, "rotation", 0.0);
    item.corner_radius = Number(value, "corner_radius", 0.0);
    item.stroke_width = Number(value, "stroke_width", 0.0);
    item.stroke = Colour(value, "stroke", item.stroke);
    item.shadow = Colour(value, "shadow", item.shadow);
    item.shadow_x = Number(value, "shadow_x", 0.0);
    item.shadow_y = Number(value, "shadow_y", 0.0);
    item.shadow_blur = Number(value, "shadow_blur", 0.0);
    if (const auto* animations = Optional(value, "animations")) {
      if (!animations->is_array()) throw std::invalid_argument("Graphic animations must be an array");
      for (const auto& animation : animations->items) item.animations.push_back(ParseAnimation(animation));
    }
    result.elements.push_back(std::move(item));
  }
  Validate(result);
  return result;
}

[[nodiscard]] std::string ReadFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Could not open graphics package: " + path);
  std::ostringstream content;
  content << stream.rdbuf();
  return content.str();
}

// "r,g,b,a" in 0..1 (alpha optional), or "#RRGGBB" / "#RRGGBBAA".
[[nodiscard]] std::array<double, 4> ParseColourValue(const std::string& text, const std::string& name) {
  std::array<double, 4> colour{0.0, 0.0, 0.0, 1.0};
  if (!text.empty() && text.front() == '#') {
    if (text.size() != 7 && text.size() != 9) throw std::invalid_argument("Graphics template colour is not #RRGGBB or #RRGGBBAA: " + name);
    for (std::size_t index = 0; index < (text.size() - 1) / 2; ++index) {
      std::size_t used = 0;
      const auto byte = std::stoi(text.substr(1 + index * 2, 2), &used, 16);
      if (used != 2) throw std::invalid_argument("Graphics template colour is not hexadecimal: " + name);
      colour[index] = byte / 255.0;
    }
    return colour;
  }
  std::istringstream stream(text);
  std::string part;
  std::size_t count = 0;
  while (std::getline(stream, part, ',')) {
    if (count == 4) throw std::invalid_argument("Graphics template colour has too many components: " + name);
    std::size_t used = 0;
    const auto parsed = std::stod(part, &used);
    if (used != part.size() || !std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0) {
      throw std::invalid_argument("Graphics template colour components must be numbers from 0 to 1: " + name);
    }
    colour[count++] = parsed;
  }
  if (count < 3) throw std::invalid_argument("Graphics template colour needs red, green and blue: " + name);
  return colour;
}

[[nodiscard]] double EaseFor(const std::string& interpolation, double t) {
  if (interpolation == "ease_in") return t * t;
  if (interpolation == "ease_out") return 1.0 - (1.0 - t) * (1.0 - t);
  if (interpolation == "ease_in_out") return t * t * (3.0 - 2.0 * t);
  return t;
}

}  // namespace

void Validate(const Document& document) {
  if (document.schema_version > kDocumentVersion) throw std::invalid_argument("Graphic document is newer than this build");
  if (document.schema_version < 1 || document.width <= 0 || document.height <= 0) throw std::invalid_argument("Graphic document header is invalid");
  if (document.elements.size() > kMaximumElements) throw std::invalid_argument("Graphic document has too many elements");
  std::set<std::string> ids;
  for (const auto& element : document.elements) {
    if (element.id.empty() || !ids.insert(element.id).second) throw std::invalid_argument("Graphic element ids must be present and unique");
    if (!std::isfinite(element.x) || !std::isfinite(element.y) || !std::isfinite(element.width) || !std::isfinite(element.height) ||
        !std::isfinite(element.font_size)) {
      throw std::invalid_argument("Graphic element geometry must be finite");
    }
    if (element.width < 0.0 || element.height < 0.0 || element.opacity < 0.0 || element.opacity > 1.0) {
      throw std::invalid_argument("Graphic element geometry or opacity is invalid");
    }
    if (!std::isfinite(element.rotation) || !std::isfinite(element.shadow_x) || !std::isfinite(element.shadow_y) ||
        !std::isfinite(element.corner_radius) || !std::isfinite(element.stroke_width) || !std::isfinite(element.shadow_blur)) {
      throw std::invalid_argument("Graphic element rotation, outline or shadow must be finite");
    }
    if (element.corner_radius < 0.0 || element.stroke_width < 0.0 || element.stroke_width > 1.0 || element.shadow_blur < 0.0 ||
        element.shadow_blur > 1.0 || std::abs(element.shadow_x) > 1.0 || std::abs(element.shadow_y) > 1.0) {
      throw std::invalid_argument("Graphic element outline, shadow or corner radius is out of range");
    }
    if (element.type == ElementType::Image && element.asset.empty()) throw std::invalid_argument("Image graphic has no asset");
    if (!KnownAnchorX(element.anchor_x)) throw std::invalid_argument("Graphic anchor_x is not none, left, center, right or stretch");
    if (!KnownAnchorY(element.anchor_y)) throw std::invalid_argument("Graphic anchor_y is not none, top, middle, bottom or stretch");
    std::set<std::string> animated;
    for (const auto& animation : element.animations) {
      if (!KnownProperty(animation.property)) throw std::invalid_argument("A graphic cannot animate '" + animation.property + "'");
      if (!animated.insert(animation.property).second) throw std::invalid_argument("A graphic animates '" + animation.property + "' twice");
      if (!KnownInterpolation(animation.interpolation)) throw std::invalid_argument("Unknown graphic interpolation: " + animation.interpolation);
      if (animation.keys.empty() || animation.keys.size() > kMaximumKeys) throw std::invalid_argument("A graphic animation needs between 1 and 512 keys");
      for (std::size_t index = 0; index < animation.keys.size(); ++index) {
        const auto& key = animation.keys[index];
        if (!std::isfinite(key.time) || !std::isfinite(key.value) || key.time < 0.0) {
          throw std::invalid_argument("A graphic animation key must have a finite value at a time from 0");
        }
        if (index > 0 && key.time <= animation.keys[index - 1].time) throw std::invalid_argument("Graphic animation keys must be in increasing time");
      }
    }
  }
}

void Validate(const TemplatePackage& package) {
  if (package.schema_version > kTemplateVersion || package.schema_version < 1) throw std::invalid_argument("Unsupported graphics template version");
  if (package.id.empty() || package.version < 1) throw std::invalid_argument("Graphics template identity is invalid");
  Validate(package.document);
  std::set<std::string> names;
  for (const auto& control : package.controls) {
    if (control.name.empty() || !names.insert(control.name).second) throw std::invalid_argument("Template control names must be unique");
    const auto element = std::find_if(package.document.elements.begin(), package.document.elements.end(),
                                      [&](const Element& item) { return item.id == control.element_id; });
    if (element == package.document.elements.end()) throw std::invalid_argument("Graphics template control '" + control.name + "' refers to a missing element");
    static const std::set<std::string> properties{"text", "asset", "x", "y", "width", "height", "opacity", "font_size", "fill",
                                                  "rotation", "corner_radius", "stroke_width", "stroke", "shadow", "shadow_x", "shadow_y",
                                                  "shadow_blur"};
    if (properties.count(control.property) == 0) throw std::invalid_argument("Unsupported graphics template property: " + control.property);
  }
}

bool UsesEffects(const Element& element) {
  return element.rotation != 0.0 || element.corner_radius != 0.0 || element.stroke_width != 0.0 || element.shadow[3] > 0.0;
}

std::int64_t RequiredVersion(const Document& document) {
  std::int64_t version = 1;
  for (const auto& element : document.elements) {
    if (UsesEffects(element)) return 3;
    for (const auto& animation : element.animations) {
      if (EffectProperty(animation.property)) return 3;
    }
    if (element.anchor_x != "none" || element.anchor_y != "none" || !element.animations.empty()) version = 2;
  }
  return version;
}

Document ParseDocument(const std::string& source) { return ParseDocumentValue(json::Parse(source)); }

TemplatePackage ParseTemplate(const std::string& source) {
  const auto root = json::Parse(source);
  if (!root.is_object()) throw std::invalid_argument("Graphics template must be a JSON object");
  TemplatePackage package;
  package.schema_version = root.Integer("schema_version");
  if (package.schema_version > kTemplateVersion || package.schema_version < 1) throw std::invalid_argument("Unsupported graphics template version");
  package.id = root.String("template_id");
  package.version = root.Integer("template_version");
  if (package.id.empty() || package.version < 1) throw std::invalid_argument("Graphics template identity is invalid");
  package.name = String(root, "name");
  package.description = String(root, "description");
  package.document = ParseDocumentValue(root.Require("document"));
  const auto& controls = root.Require("controls");
  if (!controls.is_array()) throw std::invalid_argument("Template controls must be an array");
  for (const auto& value : controls.items) {
    Control control{value.String("name"), value.String("element_id"), value.String("property"), value.String("default")};
    control.label = String(value, "label");
    control.minimum = Number(value, "minimum", 0.0);
    control.maximum = Number(value, "maximum", -1.0);
    package.controls.push_back(std::move(control));
  }
  Validate(package);
  return package;
}

Document LoadDocument(const std::string& path) { return ParseDocument(ReadFile(path)); }
TemplatePackage LoadTemplate(const std::string& path) { return ParseTemplate(ReadFile(path)); }

std::string ToJson(const Document& document) {
  Validate(document);
  std::vector<std::string> elements;
  for (const auto& element : document.elements) elements.push_back(ElementJson(element));
  return json::Object().Add("schema_version", std::max(document.schema_version, RequiredVersion(document)))
      .Add("width", static_cast<std::int64_t>(document.width)).Add("height", static_cast<std::int64_t>(document.height))
      .AddRaw("elements", json::Array(elements)).Build();
}

std::string ToJson(const TemplatePackage& package) {
  std::vector<std::string> controls;
  for (const auto& control : package.controls) {
    auto object = json::Object().Add("name", control.name).Add("element_id", control.element_id).Add("property", control.property)
                      .Add("default", control.default_value);
    if (!control.label.empty()) object.Add("label", control.label);
    if (control.maximum >= control.minimum) object.Add("minimum", control.minimum).Add("maximum", control.maximum);
    controls.push_back(object.Build());
  }
  auto object = json::Object().Add("schema_version", package.schema_version).Add("template_id", package.id)
                    .Add("template_version", package.version);
  if (!package.name.empty()) object.Add("name", package.name);
  if (!package.description.empty()) object.Add("description", package.description);
  return object.AddRaw("document", ToJson(package.document)).AddRaw("controls", json::Array(controls)).Build();
}

Document Instantiate(const TemplatePackage& package, const std::map<std::string, std::string>& values) {
  auto result = package.document;
  for (const auto& [name, supplied] : values) {
    const auto control = std::find_if(package.controls.begin(), package.controls.end(), [&](const auto& item) { return item.name == name; });
    if (control == package.controls.end()) throw std::invalid_argument("Unknown graphics template control: " + name);
    const auto element = std::find_if(result.elements.begin(), result.elements.end(), [&](const auto& item) { return item.id == control->element_id; });
    if (element == result.elements.end()) throw std::invalid_argument("Graphics template control refers to a missing element");
    const auto& value = supplied.empty() ? control->default_value : supplied;
    const auto numeric = [&]() {
      std::size_t used = 0;
      double parsed = 0.0;
      try {
        parsed = std::stod(value, &used);
      } catch (const std::exception&) {
        throw std::invalid_argument("Graphics template value is not numeric: " + name);
      }
      if (used != value.size() || !std::isfinite(parsed)) throw std::invalid_argument("Graphics template value is not numeric: " + name);
      if (control->maximum >= control->minimum && (parsed < control->minimum || parsed > control->maximum)) {
        throw std::invalid_argument("Graphics template value is outside its range: " + name);
      }
      return parsed;
    };
    if (control->property == "text") element->text = value;
    else if (control->property == "asset") element->asset = value;
    else if (control->property == "x") element->x = numeric();
    else if (control->property == "y") element->y = numeric();
    else if (control->property == "width") element->width = numeric();
    else if (control->property == "height") element->height = numeric();
    else if (control->property == "opacity") element->opacity = numeric();
    else if (control->property == "font_size") element->font_size = numeric();
    else if (control->property == "fill") element->fill = ParseColourValue(value, name);
    else if (control->property == "stroke") element->stroke = ParseColourValue(value, name);
    else if (control->property == "shadow") element->shadow = ParseColourValue(value, name);
    else if (control->property == "rotation") element->rotation = numeric();
    else if (control->property == "corner_radius") element->corner_radius = numeric();
    else if (control->property == "stroke_width") element->stroke_width = numeric();
    else if (control->property == "shadow_x") element->shadow_x = numeric();
    else if (control->property == "shadow_y") element->shadow_y = numeric();
    else if (control->property == "shadow_blur") element->shadow_blur = numeric();
    else throw std::invalid_argument("Unsupported graphics template property: " + control->property);
  }
  Validate(result);
  return result;
}

double Evaluate(const Animation& animation, double seconds) {
  const auto& keys = animation.keys;
  if (keys.empty()) return 0.0;
  if (seconds <= keys.front().time) return keys.front().value;
  if (seconds >= keys.back().time) return keys.back().value;
  const auto after = std::upper_bound(keys.begin(), keys.end(), seconds, [](double time, const AnimationKey& key) { return time < key.time; });
  const auto& to = *after;
  const auto& from = *std::prev(after);
  if (animation.interpolation == "hold") return from.value;
  const auto t = (seconds - from.time) / (to.time - from.time);
  return from.value + (to.value - from.value) * EaseFor(animation.interpolation, t);
}

Element Evaluate(const Element& element, double seconds) {
  auto result = element;
  for (const auto& animation : element.animations) {
    const auto value = Evaluate(animation, seconds);
    if (animation.property == "x") result.x = value;
    else if (animation.property == "y") result.y = value;
    else if (animation.property == "width") result.width = std::max(0.0, value);
    else if (animation.property == "height") result.height = std::max(0.0, value);
    else if (animation.property == "opacity") result.opacity = std::clamp(value, 0.0, 1.0);
    else if (animation.property == "font_size") result.font_size = std::max(0.0, value);
    else if (animation.property == "fill_r") result.fill[0] = std::clamp(value, 0.0, 1.0);
    else if (animation.property == "fill_g") result.fill[1] = std::clamp(value, 0.0, 1.0);
    else if (animation.property == "fill_b") result.fill[2] = std::clamp(value, 0.0, 1.0);
    else if (animation.property == "fill_a") result.fill[3] = std::clamp(value, 0.0, 1.0);
    else if (animation.property == "rotation") result.rotation = value;
    else if (animation.property == "corner_radius") result.corner_radius = std::max(0.0, value);
    else if (animation.property == "stroke_width") result.stroke_width = std::clamp(value, 0.0, 1.0);
    else if (animation.property == "shadow_x") result.shadow_x = std::clamp(value, -1.0, 1.0);
    else if (animation.property == "shadow_y") result.shadow_y = std::clamp(value, -1.0, 1.0);
    else if (animation.property == "shadow_blur") result.shadow_blur = std::clamp(value, 0.0, 1.0);
  }
  return result;
}

Placement Place(const Document& document, const Element& element, int picture_width, int picture_height) {
  const double lw = picture_width, lh = picture_height;
  const double dw = document.width, dh = document.height;
  const bool constrained = element.anchor_x != "none" || element.anchor_y != "none";
  Placement placement;
  if (!constrained) {
    placement.x = static_cast<int>(std::lround(element.x * lw));
    placement.y = static_cast<int>(std::lround(element.y * lh));
    placement.width = std::max(0, static_cast<int>(std::lround(element.width * lw)));
    placement.height = std::max(0, static_cast<int>(std::lround(element.height * lh)));
    placement.font_pixels = element.font_size * lh;
    placement.unit = lh;
    return placement;
  }
  // The document fitted inside the picture: one scale for both axes, so a circle stays a circle and a
  // title stays the size it was designed against the height of the page.
  const auto scale = std::min(lw / dw, lh / dh);
  // One axis: the start and length in pixels from the margins and size in document units, the anchor
  // and the picture's length along it.
  const auto axis = [&](const std::string& anchor, double start, double length, double document_length, double picture_length,
                        const char* low, const char* middle, const char* high) {
    const auto start_d = start * document_length;
    const auto length_d = length * document_length;
    const auto end_margin_d = (1.0 - start - length) * document_length;
    double out_start = 0.0, out_length = 0.0;
    if (anchor == low) {
      out_length = length_d * scale;
      out_start = start_d * scale;
    } else if (anchor == high) {
      out_length = length_d * scale;
      out_start = picture_length - end_margin_d * scale - out_length;
    } else if (anchor == middle) {
      out_length = length_d * scale;
      out_start = picture_length / 2.0 + (start_d + length_d / 2.0 - document_length / 2.0) * scale - out_length / 2.0;
    } else if (anchor == "stretch") {
      out_start = start_d * scale;
      out_length = std::max(0.0, picture_length - (start_d + end_margin_d) * scale);
    } else {
      // No constraint on this axis: it scales with the picture.
      out_start = start * picture_length;
      out_length = length * picture_length;
    }
    return std::pair{static_cast<int>(std::lround(out_start)), std::max(0, static_cast<int>(std::lround(out_length)))};
  };
  const auto horizontal = axis(element.anchor_x, element.x, element.width, dw, lw, "left", "center", "right");
  const auto vertical = axis(element.anchor_y, element.y, element.height, dh, lh, "top", "middle", "bottom");
  placement.x = horizontal.first;
  placement.width = horizontal.second;
  placement.y = vertical.first;
  placement.height = vertical.second;
  placement.font_pixels = element.font_size * dh * scale;
  placement.unit = dh * scale;
  return placement;
}

std::string ValuesToJson(const std::map<std::string, std::string>& values) {
  auto object = json::Object();
  for (const auto& [name, value] : values) object.Add(name, value);
  return object.Build();
}

std::map<std::string, std::string> ValuesFromJson(const std::string& text) {
  const auto root = json::Parse(text);
  if (!root.is_object()) throw std::invalid_argument("Template values must be a JSON object");
  std::map<std::string, std::string> values;
  for (const auto& [name, value] : root.members) {
    if (!value.is_string()) throw std::invalid_argument("Template value '" + name + "' must be text");
    values[name] = value.text;
  }
  return values;
}

std::string MakeGraphicBundle(const std::string& document_json) {
  return json::Object().Add("kind", "graphic").AddRaw("document", document_json).Build();
}

std::string MakeTemplateBundle(const std::string& package_json, const std::map<std::string, std::string>& values) {
  return json::Object().Add("kind", "template").AddRaw("package", package_json).AddRaw("values", ValuesToJson(values)).Build();
}

Document ResolveBundle(const std::string& bundle_json) {
  const auto root = json::Parse(bundle_json);
  if (!root.is_object()) throw std::invalid_argument("A graphic bundle must be a JSON object");
  const auto& kind = root.String("kind");
  if (kind == "graphic") return ParseDocumentValue(root.Require("document"));
  if (kind == "template") {
    const auto package = ParseTemplate(root.Require("package").raw);
    return Instantiate(package, ValuesFromJson(root.Require("values").raw));
  }
  throw std::invalid_argument("A graphic bundle is a graphic or a template, not '" + kind + "'");
}

}  // namespace cutline::render::graphics

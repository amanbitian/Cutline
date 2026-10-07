#include "ui/GraphicsDesigner.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cutline::ui {
namespace {

using commands::CommandType;
using gfx::Element;
using gfx::ElementType;

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinimumSide = 0.01;

const char* TypeName(ElementType type) {
  switch (type) {
    case ElementType::Text: return "text";
    case ElementType::Rectangle: return "rectangle";
    case ElementType::Ellipse: return "ellipse";
    case ElementType::Image: return "image";
  }
  return "rectangle";
}

std::string Number(double value) {
  std::ostringstream out;
  out << std::setprecision(6) << value;
  return out.str();
}

double ParseNumber(const std::string& text, const std::string& name) {
  std::size_t used = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &used);
  } catch (const std::exception&) {
    throw std::invalid_argument(name + " needs a number");
  }
  if (used != text.size() || !std::isfinite(value)) throw std::invalid_argument(name + " needs a number");
  return value;
}

bool ParseToggle(const std::string& text, const std::string& name) {
  if (text == "true" || text == "1" || text == "on") return true;
  if (text == "false" || text == "0" || text == "off") return false;
  throw std::invalid_argument(name + " is on or off");
}

// An element's box in document pixels, its middle and its turn.
struct Frame final {
  double cx{0.0}, cy{0.0}, half_width{0.0}, half_height{0.0}, radians{0.0};
};

// At the document's own size an anchored element lands where its fractions say (the anchors only matter when the picture is
// another shape), so the designer works from the fractions, exactly, and not from the rounded pixels of the renderer.
Frame FrameOf(const gfx::Document& document, const Element& element) {
  Frame frame;
  frame.half_width = element.width * document.width / 2.0;
  frame.half_height = element.height * document.height / 2.0;
  frame.cx = element.x * document.width + frame.half_width;
  frame.cy = element.y * document.height + frame.half_height;
  frame.radians = element.rotation * kPi / 180.0;
  return frame;
}

// Points between document pixels and the element's own axes (x along its width, y along its height).
void ToLocal(const Frame& frame, double x, double y, double& lx, double& ly) {
  const auto c = std::cos(frame.radians), s = std::sin(frame.radians);
  const auto dx = x - frame.cx, dy = y - frame.cy;
  lx = dx * c + dy * s;
  ly = -dx * s + dy * c;
}
void ToDocument(const Frame& frame, double lx, double ly, double& x, double& y) {
  const auto c = std::cos(frame.radians), s = std::sin(frame.radians);
  x = frame.cx + lx * c - ly * s;
  y = frame.cy + lx * s + ly * c;
}

// Stores a frame back in the element, as fractions of the document. An element with an anchor keeps it: its
// geometry is still the fraction of the document it was authored in, so a designer working at the document's own
// size writes the same fractions it read.
void StoreFrame(const gfx::Document& document, Element& element, const Frame& frame) {
  element.x = (frame.cx - frame.half_width) / document.width;
  element.y = (frame.cy - frame.half_height) / document.height;
  element.width = frame.half_width * 2.0 / document.width;
  element.height = frame.half_height * 2.0 / document.height;
}

bool HasId(const gfx::Document& document, const std::string& id) {
  return std::any_of(document.elements.begin(), document.elements.end(), [&](const Element& e) { return e.id == id; });
}

std::string UniqueId(const gfx::Document& document, const std::string& base) {
  for (int n = 1;; ++n) {
    const auto id = base + "-" + std::to_string(n);
    if (!HasId(document, id)) return id;
  }
}

const std::vector<std::string> kAlign{"left", "center", "right"};
const std::vector<std::string> kAnchorX{"none", "left", "center", "right", "stretch"};
const std::vector<std::string> kAnchorY{"none", "top", "middle", "bottom", "stretch"};

PropertyInfo Prop(std::string name, std::string label, std::string group, std::string kind, double minimum = 0.0, double maximum = 1.0, double step = 0.01,
                  std::vector<std::string> choices = {}) {
  PropertyInfo info;
  info.name = std::move(name);
  info.label = std::move(label);
  info.group = std::move(group);
  info.kind = std::move(kind);
  info.minimum = minimum;
  info.maximum = maximum;
  info.step = step;
  info.choices = std::move(choices);
  return info;
}

Element Text(const std::string& id, const std::string& text, double x, double y, double width, double height, double size, bool bold = false,
             const std::string& align = "center") {
  Element element;
  element.id = id;
  element.type = ElementType::Text;
  element.text = text;
  element.x = x;
  element.y = y;
  element.width = width;
  element.height = height;
  element.font_size = size;
  element.bold = bold;
  element.align = align;
  return element;
}

Element Shape(const std::string& id, double x, double y, double width, double height, std::array<double, 4> fill, ElementType type = ElementType::Rectangle) {
  Element element;
  element.id = id;
  element.type = type;
  element.x = x;
  element.y = y;
  element.width = width;
  element.height = height;
  element.fill = fill;
  return element;
}

gfx::Animation Slide(const std::string& property, double from, double to, double seconds = 0.5) {
  return {property, "ease_out", {{0.0, from}, {seconds, to}}};
}

gfx::TemplatePackage Package(const std::string& id, const std::string& name, const std::string& description, gfx::Document document) {
  auto controls = AutoControls(document);
  return MakeTemplate(document, id, name, description, std::move(controls));
}

}  // namespace

// -------------------------------------------------------------------------- document ----

gfx::Document NewGraphicDocument(int width, int height) {
  gfx::Document document;
  document.width = width;
  document.height = height;
  return document;
}

std::string AddElement(gfx::Document& document, gfx::ElementType type) {
  Element element;
  element.type = type;
  element.id = UniqueId(document, TypeName(type));
  switch (type) {
    case ElementType::Text:
      element.text = "Text";
      element.x = 0.2;
      element.y = 0.4;
      element.width = 0.6;
      element.height = 0.2;
      element.font_size = 0.1;
      break;
    case ElementType::Image:
      element.x = 0.35;
      element.y = 0.25;
      element.width = 0.3;
      element.height = 0.5;
      break;
    default:
      element.x = 0.35;
      element.y = 0.3;
      element.width = type == ElementType::Ellipse ? 0.3 : 0.3;
      element.height = 0.4;
      element.fill = {0.95, 0.35, 0.2, 1.0};
      break;
  }
  document.elements.push_back(std::move(element));
  return document.elements.back().id;
}

bool RemoveElement(gfx::Document& document, const std::string& id) {
  const auto found = std::find_if(document.elements.begin(), document.elements.end(), [&](const Element& e) { return e.id == id; });
  if (found == document.elements.end()) return false;
  document.elements.erase(found);
  return true;
}

std::string DuplicateElement(gfx::Document& document, const std::string& id) {
  const auto* original = FindElement(document, id);
  if (original == nullptr) return {};
  auto copy = *original;
  copy.id = UniqueId(document, TypeName(copy.type));
  copy.x = std::min(copy.x + 0.02, 1.0);
  copy.y = std::min(copy.y + 0.03, 1.0);
  document.elements.push_back(std::move(copy));
  return document.elements.back().id;
}

bool Restack(gfx::Document& document, const std::string& id, Stacking where) {
  auto& elements = document.elements;
  const auto found = std::find_if(elements.begin(), elements.end(), [&](const Element& e) { return e.id == id; });
  if (found == elements.end()) return false;
  const auto index = static_cast<std::size_t>(found - elements.begin());
  std::size_t target = index;
  switch (where) {
    case Stacking::Front: target = elements.size() - 1; break;
    case Stacking::Back: target = 0; break;
    case Stacking::Forward: target = std::min(index + 1, elements.size() - 1); break;
    case Stacking::Backward: target = index == 0 ? 0 : index - 1; break;
  }
  if (target == index) return false;
  auto moved = std::move(elements[index]);
  elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(index));
  elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(target), std::move(moved));
  return true;
}

gfx::Element* FindElement(gfx::Document& document, const std::string& id) {
  for (auto& element : document.elements) {
    if (element.id == id) return &element;
  }
  return nullptr;
}

const gfx::Element* FindElement(const gfx::Document& document, const std::string& id) {
  for (const auto& element : document.elements) {
    if (element.id == id) return &element;
  }
  return nullptr;
}

std::vector<PropertyInfo> PropertiesOf(gfx::ElementType type) {
  std::vector<PropertyInfo> list;
  if (type == ElementType::Text) {
    list.push_back(Prop("text", "Text", "Text", "text"));
    list.push_back(Prop("font", "Font", "Text", "text"));
    list.push_back(Prop("font_size", "Size", "Text", "number", 0.01, 1.0, 0.005));
    list.push_back(Prop("bold", "Bold", "Text", "toggle"));
    list.push_back(Prop("italic", "Italic", "Text", "toggle"));
    list.push_back(Prop("align", "Align", "Text", "choice", 0, 0, 0, kAlign));
  }
  if (type == ElementType::Image) list.push_back(Prop("asset", "Picture", "Picture", "asset"));
  list.push_back(Prop("x", "X", "Position", "number", -1.0, 2.0, 0.005));
  list.push_back(Prop("y", "Y", "Position", "number", -1.0, 2.0, 0.005));
  list.push_back(Prop("width", "Width", "Position", "number", 0.0, 2.0, 0.005));
  list.push_back(Prop("height", "Height", "Position", "number", 0.0, 2.0, 0.005));
  list.push_back(Prop("rotation", "Rotation", "Position", "number", -3600.0, 3600.0, 1.0));
  list.push_back(Prop("opacity", "Opacity", "Appearance", "number", 0.0, 1.0, 0.01));
  if (type != ElementType::Image) list.push_back(Prop("fill", type == ElementType::Text ? "Colour" : "Fill", "Appearance", "colour"));
  if (type == ElementType::Rectangle || type == ElementType::Image) list.push_back(Prop("corner_radius", "Corner radius", "Appearance", "number", 0.0, 0.5, 0.005));
  list.push_back(Prop("stroke_width", "Outline width", "Outline", "number", 0.0, 0.2, 0.001));
  list.push_back(Prop("stroke", "Outline colour", "Outline", "colour"));
  list.push_back(Prop("shadow", "Shadow colour", "Shadow", "colour"));
  list.push_back(Prop("shadow_x", "Shadow X", "Shadow", "number", -0.5, 0.5, 0.002));
  list.push_back(Prop("shadow_y", "Shadow Y", "Shadow", "number", -0.5, 0.5, 0.002));
  list.push_back(Prop("shadow_blur", "Shadow blur", "Shadow", "number", 0.0, 0.5, 0.002));
  list.push_back(Prop("anchor_x", "Pin across", "Layout", "choice", 0, 0, 0, kAnchorX));
  list.push_back(Prop("anchor_y", "Pin down", "Layout", "choice", 0, 0, 0, kAnchorY));
  return list;
}

const std::vector<std::string>& EntranceKinds() {
  static const std::vector<std::string> kinds{"none", "fade", "slide_left", "slide_right", "slide_up", "slide_down", "pop"};
  return kinds;
}

namespace {
bool EntranceProperty(const std::string& property) { return property == "x" || property == "y" || property == "opacity" || property == "width" || property == "height"; }
const gfx::Animation* AnimationOf(const Element& element, const std::string& property) {
  for (const auto& animation : element.animations) {
    if (animation.property == property) return &animation;
  }
  return nullptr;
}
}  // namespace

void SetEntrance(gfx::Element& element, const std::string& kind, double seconds) {
  const auto& kinds = EntranceKinds();
  if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) throw std::invalid_argument("An entrance is none, fade, slide_left, slide_right, slide_up, slide_down or pop");
  if (!(seconds > 0.0) || seconds > 60.0) throw std::invalid_argument("An entrance takes between 0 and 60 seconds");
  element.animations.erase(std::remove_if(element.animations.begin(), element.animations.end(), [](const gfx::Animation& a) { return EntranceProperty(a.property); }),
                           element.animations.end());
  const auto add = [&](const char* property, double from, double to) {
    element.animations.push_back({property, "ease_out", {{0.0, from}, {seconds, to}}});
  };
  if (kind == "fade") add("opacity", 0.0, element.opacity);
  else if (kind == "slide_left") { add("x", element.x - 0.3, element.x); add("opacity", 0.0, element.opacity); }
  else if (kind == "slide_right") { add("x", element.x + 0.3, element.x); add("opacity", 0.0, element.opacity); }
  else if (kind == "slide_up") { add("y", element.y + 0.2, element.y); add("opacity", 0.0, element.opacity); }
  else if (kind == "slide_down") { add("y", element.y - 0.2, element.y); add("opacity", 0.0, element.opacity); }
  else if (kind == "pop") {
    // Grows from its middle.
    add("x", element.x + element.width / 2.0, element.x);
    add("y", element.y + element.height / 2.0, element.y);
    add("width", 0.0, element.width);
    add("height", 0.0, element.height);
    add("opacity", 0.0, element.opacity);
  }
}

std::string EntranceOf(const gfx::Element& element, double* seconds) {
  const auto* x = AnimationOf(element, "x");
  const auto* y = AnimationOf(element, "y");
  const auto* opacity = AnimationOf(element, "opacity");
  const auto* width = AnimationOf(element, "width");
  double length = 0.0;
  for (const auto& animation : element.animations) {
    if (EntranceProperty(animation.property) && !animation.keys.empty()) length = std::max(length, animation.keys.back().time);
  }
  if (seconds != nullptr) *seconds = length;
  const auto rising = [](const gfx::Animation* a) { return a != nullptr && a->keys.size() >= 2 && a->keys.front().value < a->keys.back().value; };
  const auto falling = [](const gfx::Animation* a) { return a != nullptr && a->keys.size() >= 2 && a->keys.front().value > a->keys.back().value; };
  if (width != nullptr && rising(width)) return "pop";
  if (rising(x) && opacity != nullptr) return "slide_left";
  if (falling(x) && opacity != nullptr) return "slide_right";
  if (falling(y) && opacity != nullptr) return "slide_up";
  if (rising(y) && opacity != nullptr) return "slide_down";
  if (rising(opacity) && x == nullptr && y == nullptr) return "fade";
  return "none";
}

std::string ColourText(const std::array<double, 4>& colour) {
  static const char* digits = "0123456789ABCDEF";
  std::string text = "#";
  for (const auto component : colour) {
    const auto byte = static_cast<int>(std::lround(std::clamp(component, 0.0, 1.0) * 255.0));
    text += digits[byte >> 4];
    text += digits[byte & 15];
  }
  return text;
}

std::array<double, 4> ParseColour(const std::string& text) {
  std::array<double, 4> colour{0.0, 0.0, 0.0, 1.0};
  if (!text.empty() && text.front() == '#') {
    if (text.size() != 7 && text.size() != 9) throw std::invalid_argument("A colour is #RRGGBB or #RRGGBBAA");
    for (std::size_t index = 0; index < (text.size() - 1) / 2; ++index) {
      std::size_t used = 0;
      int byte = 0;
      try {
        byte = std::stoi(text.substr(1 + index * 2, 2), &used, 16);
      } catch (const std::exception&) {
        throw std::invalid_argument("A colour is #RRGGBB or #RRGGBBAA");
      }
      if (used != 2) throw std::invalid_argument("A colour is #RRGGBB or #RRGGBBAA");
      colour[index] = byte / 255.0;
    }
    return colour;
  }
  std::istringstream stream(text);
  std::string part;
  std::size_t count = 0;
  while (std::getline(stream, part, ',')) {
    if (count == 4) throw std::invalid_argument("A colour has red, green, blue and optionally alpha");
    const auto value = ParseNumber(part, "A colour component");
    if (value < 0.0 || value > 1.0) throw std::invalid_argument("A colour component is from 0 to 1");
    colour[count++] = value;
  }
  if (count < 3) throw std::invalid_argument("A colour has red, green and blue");
  return colour;
}

std::string PropertyValue(const gfx::Element& e, const std::string& name) {
  if (name == "text") return e.text;
  if (name == "font") return e.font;
  if (name == "font_size") return Number(e.font_size);
  if (name == "bold") return e.bold ? "true" : "false";
  if (name == "italic") return e.italic ? "true" : "false";
  if (name == "align") return e.align;
  if (name == "asset") return e.asset;
  if (name == "x") return Number(e.x);
  if (name == "y") return Number(e.y);
  if (name == "width") return Number(e.width);
  if (name == "height") return Number(e.height);
  if (name == "rotation") return Number(e.rotation);
  if (name == "opacity") return Number(e.opacity);
  if (name == "fill") return ColourText(e.fill);
  if (name == "corner_radius") return Number(e.corner_radius);
  if (name == "stroke_width") return Number(e.stroke_width);
  if (name == "stroke") return ColourText(e.stroke);
  if (name == "shadow") return ColourText(e.shadow);
  if (name == "shadow_x") return Number(e.shadow_x);
  if (name == "shadow_y") return Number(e.shadow_y);
  if (name == "shadow_blur") return Number(e.shadow_blur);
  if (name == "anchor_x") return e.anchor_x;
  if (name == "anchor_y") return e.anchor_y;
  throw std::invalid_argument("A graphic element has no property called " + name);
}

void SetProperty(gfx::Element& element, const std::string& name, const std::string& value) {
  auto next = element;
  if (name == "text") next.text = value;
  else if (name == "font") next.font = value.empty() ? "Arial" : value;
  else if (name == "font_size") next.font_size = ParseNumber(value, name);
  else if (name == "bold") next.bold = ParseToggle(value, name);
  else if (name == "italic") next.italic = ParseToggle(value, name);
  else if (name == "align") {
    if (std::find(kAlign.begin(), kAlign.end(), value) == kAlign.end()) throw std::invalid_argument("align is left, center or right");
    next.align = value;
  } else if (name == "asset") next.asset = value;
  else if (name == "x") next.x = ParseNumber(value, name);
  else if (name == "y") next.y = ParseNumber(value, name);
  else if (name == "width") next.width = ParseNumber(value, name);
  else if (name == "height") next.height = ParseNumber(value, name);
  else if (name == "rotation") next.rotation = ParseNumber(value, name);
  else if (name == "opacity") next.opacity = std::clamp(ParseNumber(value, name), 0.0, 1.0);
  else if (name == "fill") next.fill = ParseColour(value);
  else if (name == "corner_radius") next.corner_radius = ParseNumber(value, name);
  else if (name == "stroke_width") next.stroke_width = ParseNumber(value, name);
  else if (name == "stroke") next.stroke = ParseColour(value);
  else if (name == "shadow") next.shadow = ParseColour(value);
  else if (name == "shadow_x") next.shadow_x = ParseNumber(value, name);
  else if (name == "shadow_y") next.shadow_y = ParseNumber(value, name);
  else if (name == "shadow_blur") next.shadow_blur = ParseNumber(value, name);
  else if (name == "anchor_x") {
    if (std::find(kAnchorX.begin(), kAnchorX.end(), value) == kAnchorX.end()) throw std::invalid_argument("anchor_x is none, left, center, right or stretch");
    next.anchor_x = value;
  } else if (name == "anchor_y") {
    if (std::find(kAnchorY.begin(), kAnchorY.end(), value) == kAnchorY.end()) throw std::invalid_argument("anchor_y is none, top, middle, bottom or stretch");
    next.anchor_y = value;
  } else {
    throw std::invalid_argument("A graphic element has no property called " + name);
  }
  // What the document would refuse is refused here, so a bad value never reaches it.
  gfx::Document check;
  check.elements.push_back(next);
  // An image with no picture yet is a draft, which Problems reports, not a bad value.
  if (next.type == ElementType::Image && next.asset.empty()) check.elements.back().asset = "-";
  gfx::Validate(check);
  element = std::move(next);
}

std::string ElementLabel(const gfx::Element& element) {
  if (element.type == ElementType::Text && !element.text.empty()) {
    auto text = element.text;
    std::replace(text.begin(), text.end(), '\n', ' ');
    if (text.size() > 28) text = text.substr(0, 27) + "...";
    return text;
  }
  return std::string(TypeName(element.type)) + " " + element.id;
}

// ---------------------------------------------------------------------------- canvas ----

ElementBox ElementBoxOf(const gfx::Document&, const gfx::Element& element) { return {element.x, element.y, element.width, element.height}; }

std::optional<std::string> HitElement(const gfx::Document& document, DocPoint at) {
  const auto px = at.x * document.width, py = at.y * document.height;
  for (auto it = document.elements.rbegin(); it != document.elements.rend(); ++it) {
    const auto frame = FrameOf(document, *it);
    double lx = 0.0, ly = 0.0;
    ToLocal(frame, px, py, lx, ly);
    if (std::abs(lx) <= frame.half_width && std::abs(ly) <= frame.half_height) return it->id;
  }
  return std::nullopt;
}

std::vector<GripPosition> GripsOf(const gfx::Document& document, const gfx::Element& element) {
  const auto frame = FrameOf(document, element);
  struct Spec { Grip grip; double sx, sy; };
  static const Spec specs[] = {{Grip::TopLeft, -1, -1}, {Grip::Top, 0, -1},    {Grip::TopRight, 1, -1}, {Grip::Right, 1, 0},
                               {Grip::BottomRight, 1, 1}, {Grip::Bottom, 0, 1}, {Grip::BottomLeft, -1, 1}, {Grip::Left, -1, 0}};
  std::vector<GripPosition> grips;
  for (const auto& spec : specs) {
    double x = 0.0, y = 0.0;
    ToDocument(frame, spec.sx * frame.half_width, spec.sy * frame.half_height, x, y);
    grips.push_back({spec.grip, {x / document.width, y / document.height}});
  }
  double x = 0.0, y = 0.0;
  ToDocument(frame, 0.0, -frame.half_height - 0.04 * document.height, x, y);
  grips.push_back({Grip::Turn, {x / document.width, y / document.height}});
  return grips;
}

Grip GripAt(const gfx::Document& document, const gfx::Element& element, DocPoint at, double reach) {
  const auto reach_px = reach * document.height;
  for (const auto& grip : GripsOf(document, element)) {
    if (std::hypot((grip.at.x - at.x) * document.width, (grip.at.y - at.y) * document.height) <= reach_px) return grip.grip;
  }
  const auto frame = FrameOf(document, element);
  double lx = 0.0, ly = 0.0;
  ToLocal(frame, at.x * document.width, at.y * document.height, lx, ly);
  return std::abs(lx) <= frame.half_width && std::abs(ly) <= frame.half_height ? Grip::Body : Grip::None;
}

std::vector<Guide> MoveBy(gfx::Document& document, const std::string& id, double dx, double dy, bool snap, double threshold) {
  std::vector<Guide> guides;
  auto* element = FindElement(document, id);
  if (element == nullptr) return guides;
  auto frame = FrameOf(document, *element);
  frame.cx += dx * document.width;
  frame.cy += dy * document.height;
  if (snap) {
    // The edges and middle of the turned element's bounding box against the document's edges and middle.
    const auto c = std::abs(std::cos(frame.radians)), s = std::abs(std::sin(frame.radians));
    const auto reach_x = frame.half_width * c + frame.half_height * s, reach_y = frame.half_width * s + frame.half_height * c;
    const auto snap_axis = [&](double& centre, double reach, double length, bool vertical) {
      double best = threshold * length + 1.0, shift = 0.0, line = 0.0;
      const double candidates[3][2] = {{centre - reach, 0.0}, {centre, length / 2.0}, {centre + reach, length}};
      for (const auto& candidate : candidates) {
        const auto distance = std::abs(candidate[1] - candidate[0]);
        if (distance <= threshold * length && distance < best) {
          best = distance;
          shift = candidate[1] - candidate[0];
          line = candidate[1] / length;
        }
      }
      if (best <= threshold * length) {
        centre += shift;
        guides.push_back({vertical, line});
      }
    };
    snap_axis(frame.cx, reach_x, document.width, true);
    snap_axis(frame.cy, reach_y, document.height, false);
  }
  StoreFrame(document, *element, frame);
  return guides;
}

void ResizeBy(gfx::Document& document, const std::string& id, Grip grip, double dx, double dy, bool keep_proportions) {
  auto* element = FindElement(document, id);
  if (element == nullptr || grip == Grip::None || grip == Grip::Body || grip == Grip::Turn) return;
  auto frame = FrameOf(document, *element);
  double lx = 0.0, ly = 0.0;
  // The drag as the element's own axes see it.
  {
    const auto c = std::cos(frame.radians), s = std::sin(frame.radians);
    const auto px = dx * document.width, py = dy * document.height;
    lx = px * c + py * s;
    ly = -px * s + py * c;
  }
  double sx = 0.0, sy = 0.0;
  switch (grip) {
    case Grip::TopLeft: sx = -1; sy = -1; break;
    case Grip::Top: sy = -1; break;
    case Grip::TopRight: sx = 1; sy = -1; break;
    case Grip::Right: sx = 1; break;
    case Grip::BottomRight: sx = 1; sy = 1; break;
    case Grip::Bottom: sy = 1; break;
    case Grip::BottomLeft: sx = -1; sy = 1; break;
    case Grip::Left: sx = -1; break;
    default: break;
  }
  const auto minimum_w = kMinimumSide * document.width, minimum_h = kMinimumSide * document.height;
  const auto width = frame.half_width * 2.0, height = frame.half_height * 2.0;
  auto new_width = sx != 0.0 ? std::max(minimum_w, width + sx * lx) : width;
  auto new_height = sy != 0.0 ? std::max(minimum_h, height + sy * ly) : height;
  if (keep_proportions && sx != 0.0 && sy != 0.0 && width > 0.0 && height > 0.0) {
    const auto scale = std::max(new_width / width, new_height / height);
    new_width = std::max(minimum_w, width * scale);
    new_height = std::max(minimum_h, height * scale);
  }
  // The edge or corner opposite the grip stays where it is: the middle moves by half of what the size changed, away from it.
  const auto shift_x = sx * (new_width - width) / 2.0, shift_y = sy * (new_height - height) / 2.0;
  double x = 0.0, y = 0.0;
  ToDocument(frame, shift_x, shift_y, x, y);
  frame.cx = x;
  frame.cy = y;
  frame.half_width = new_width / 2.0;
  frame.half_height = new_height / 2.0;
  StoreFrame(document, *element, frame);
}

void TurnTo(gfx::Document& document, const std::string& id, DocPoint at, double snap_degrees) {
  auto* element = FindElement(document, id);
  if (element == nullptr) return;
  const auto frame = FrameOf(document, *element);
  const auto dx = at.x * document.width - frame.cx, dy = at.y * document.height - frame.cy;
  if (dx == 0.0 && dy == 0.0) return;
  // The top of the element points at the pointer: straight up is 0 degrees, clockwise is positive.
  auto degrees = std::atan2(dx, -dy) * 180.0 / kPi;
  if (snap_degrees > 0.0) {
    const auto nearest = std::round(degrees / snap_degrees) * snap_degrees;
    if (std::abs(nearest - degrees) <= 3.0) degrees = nearest;
  }
  element->rotation = degrees == 0.0 ? 0.0 : degrees;
}

void AlignElement(gfx::Document& document, const std::string& id, Alignment alignment) {
  auto* element = FindElement(document, id);
  if (element == nullptr) return;
  auto frame = FrameOf(document, *element);
  const auto c = std::abs(std::cos(frame.radians)), s = std::abs(std::sin(frame.radians));
  const auto reach_x = frame.half_width * c + frame.half_height * s, reach_y = frame.half_width * s + frame.half_height * c;
  switch (alignment) {
    case Alignment::Left: frame.cx = reach_x; break;
    case Alignment::Centre: frame.cx = document.width / 2.0; break;
    case Alignment::Right: frame.cx = document.width - reach_x; break;
    case Alignment::Top: frame.cy = reach_y; break;
    case Alignment::Middle: frame.cy = document.height / 2.0; break;
    case Alignment::Bottom: frame.cy = document.height - reach_y; break;
  }
  StoreFrame(document, *element, frame);
}

std::vector<std::string> Problems(const gfx::Document& document) {
  std::vector<std::string> problems;
  for (const auto& element : document.elements) {
    if (element.type == ElementType::Image && element.asset.empty()) problems.push_back("Choose a picture for " + element.id + " or delete it");
  }
  return problems;
}

gfx::Document ForPreview(const gfx::Document& document) {
  auto copy = document;
  for (auto& element : copy.elements) {
    if (element.type != ElementType::Image || !element.asset.empty()) continue;
    // A placeholder where the picture will be.
    element.type = ElementType::Rectangle;
    element.fill = {0.35, 0.37, 0.42, 0.8};
    element.stroke_width = std::max(element.stroke_width, 0.004);
    element.stroke = {0.7, 0.72, 0.78, 1.0};
  }
  return copy;
}

// -------------------------------------------------------------------------- templates ----

std::vector<gfx::Control> AutoControls(const gfx::Document& document) {
  std::vector<gfx::Control> controls;
  std::set<std::string> names;
  const auto add = [&](const std::string& base, const std::string& suffix, const Element& element, const std::string& property,
                       const std::string& value, const std::string& label) {
    auto name = base + suffix;
    for (int n = 2; !names.insert(name).second; ++n) name = base + suffix + std::to_string(n);
    gfx::Control control{name, element.id, property, value};
    control.label = label;
    controls.push_back(std::move(control));
  };
  for (const auto& element : document.elements) {
    // Control names are the element's id, which a person chose or the designer numbered.
    switch (element.type) {
      case ElementType::Text:
        add(element.id, "", element, "text", element.text, "Text");
        add(element.id, "_colour", element, "fill", ColourText(element.fill), "Text colour");
        break;
      case ElementType::Image:
        add(element.id, "", element, "asset", element.asset, "Picture");
        break;
      default:
        add(element.id, "_colour", element, "fill", ColourText(element.fill), "Colour");
        break;
    }
  }
  return controls;
}

gfx::TemplatePackage MakeTemplate(const gfx::Document& document, const std::string& id, const std::string& name, const std::string& description,
                                  std::vector<gfx::Control> controls) {
  gfx::TemplatePackage package;
  package.id = id;
  package.version = 1;
  package.name = name;
  package.description = description;
  package.document = document;
  package.controls = std::move(controls);
  gfx::Validate(package);
  return package;
}

std::vector<gfx::TemplatePackage> BuiltInTemplates() {
  std::vector<gfx::TemplatePackage> templates;
  const std::array<double, 4> ink{0.08, 0.09, 0.12, 0.92}, white{1, 1, 1, 1}, accent{0.95, 0.35, 0.2, 1};

  {  // A name and a line under it, sliding in from the left.
    auto document = NewGraphicDocument();
    auto bar = Shape("bar", 0.05, 0.74, 0.42, 0.14, ink);
    bar.corner_radius = 0.01;
    bar.anchor_x = "left";
    bar.anchor_y = "bottom";
    bar.animations.push_back(Slide("x", -0.5, 0.05));
    auto stripe = Shape("stripe", 0.05, 0.74, 0.008, 0.14, accent);
    stripe.anchor_x = "left";
    stripe.anchor_y = "bottom";
    stripe.animations.push_back(Slide("x", -0.5, 0.05));
    auto name = Text("name", "Name Surname", 0.07, 0.755, 0.38, 0.07, 0.05, true, "left");
    name.anchor_x = "left";
    name.anchor_y = "bottom";
    name.animations.push_back(Slide("x", -0.5, 0.07));
    auto role = Text("role", "Role or place", 0.07, 0.82, 0.38, 0.05, 0.03, false, "left");
    role.fill = {0.8, 0.82, 0.88, 1};
    role.anchor_x = "left";
    role.anchor_y = "bottom";
    role.animations.push_back(Slide("x", -0.5, 0.07, 0.6));
    document.elements = {bar, stripe, name, role};
    templates.push_back(Package("lower-third", "Lower third", "A name and a line under it, sliding in from the left.", document));
  }
  {  // A big centred title that fades in and has a soft shadow.
    auto document = NewGraphicDocument();
    auto title = Text("title", "Title", 0.1, 0.38, 0.8, 0.24, 0.16, true);
    title.shadow = {0, 0, 0, 0.7};
    title.shadow_y = 0.006;
    title.shadow_blur = 0.012;
    title.anchor_x = "center";
    title.anchor_y = "middle";
    title.animations.push_back({"opacity", "ease_out", {{0.0, 0.0}, {0.6, 1.0}}});
    document.elements = {title};
    templates.push_back(Package("title-card", "Title card", "One large centred title that fades in, with a soft shadow.", document));
  }
  {  // A rounded tag with an outline.
    auto document = NewGraphicDocument();
    auto tag = Shape("tag", 0.06, 0.06, 0.22, 0.09, {0.1, 0.3, 0.8, 1});
    tag.corner_radius = 0.045;
    tag.stroke_width = 0.004;
    tag.stroke = white;
    tag.anchor_x = "left";
    tag.anchor_y = "top";
    auto label = Text("label", "LIVE", 0.06, 0.082, 0.22, 0.06, 0.045, true);
    label.anchor_x = "left";
    label.anchor_y = "top";
    document.elements = {tag, label};
    templates.push_back(Package("name-tag", "Rounded tag", "A rounded, outlined tag in the corner.", document));
  }
  {  // A quotation with its source.
    auto document = NewGraphicDocument();
    auto quote = Text("quote", "A line worth reading.", 0.12, 0.3, 0.76, 0.3, 0.075, false, "left");
    quote.shadow = {0, 0, 0, 0.6};
    quote.shadow_y = 0.004;
    quote.shadow_blur = 0.01;
    quote.anchor_x = "center";
    quote.anchor_y = "middle";
    auto source = Text("source", "Someone, somewhere", 0.12, 0.62, 0.76, 0.08, 0.04, true, "left");
    source.fill = {1, 0.8, 0.3, 1};
    source.anchor_x = "center";
    source.anchor_y = "middle";
    document.elements = {quote, source};
    templates.push_back(Package("quote", "Quotation", "A quotation and the name of whoever said it.", document));
  }
  {  // A full-width caption bar.
    auto document = NewGraphicDocument();
    auto band = Shape("band", 0.0, 0.84, 1.0, 0.16, {0, 0, 0, 0.65});
    band.anchor_x = "stretch";
    band.anchor_y = "bottom";
    auto line = Text("line", "A line of text across the bottom", 0.04, 0.87, 0.92, 0.1, 0.05, false, "center");
    line.anchor_x = "stretch";
    line.anchor_y = "bottom";
    document.elements = {band, line};
    templates.push_back(Package("caption-bar", "Caption bar", "A dark band across the bottom with a line of text that stretches with the picture.", document));
  }
  {  // A turned badge.
    auto document = NewGraphicDocument();
    auto badge = Shape("badge", 0.72, 0.1, 0.2, 0.2, accent, ElementType::Ellipse);
    badge.stroke_width = 0.006;
    badge.stroke = white;
    badge.rotation = -12.0;
    badge.shadow = {0, 0, 0, 0.5};
    badge.shadow_y = 0.008;
    badge.shadow_blur = 0.01;
    badge.anchor_x = "right";
    badge.anchor_y = "top";
    auto text = Text("text", "NEW", 0.72, 0.17, 0.2, 0.08, 0.065, true);
    text.rotation = -12.0;
    text.anchor_x = "right";
    text.anchor_y = "top";
    document.elements = {badge, text};
    templates.push_back(Package("badge", "Badge", "A turned round badge with a word on it.", document));
  }
  return templates;
}

// ----------------------------------------------------------------------------- plans ----

EditPlan PlanCreateGraphic(const EditContext& ctx, const std::string& name, const gfx::Document& document, std::string* id_out) {
  if (!ctx.new_id) return EditPlan::Refuse("There is no way to name the graphic");
  std::string json;
  try {
    json = gfx::ToJson(document);
  } catch (const std::exception& error) {
    return EditPlan::Refuse(error.what());
  }
  commands::CreateGraphicPayload payload;
  payload.id = ctx.new_id("graphic");
  payload.name = name.empty() ? "Graphic" : name;
  payload.kind = "graphic";
  payload.document_json = std::move(json);
  if (id_out != nullptr) *id_out = payload.id;
  EditPlan plan;
  plan.ok = true;
  plan.label = "New Graphic";
  plan.commands.push_back({CommandType::CreateGraphic, std::move(payload)});
  return plan;
}

EditPlan PlanSaveGraphic(const EditContext&, const std::string& id, const gfx::Document& document) {
  std::string json;
  try {
    json = gfx::ToJson(document);
  } catch (const std::exception& error) {
    return EditPlan::Refuse(error.what());
  }
  commands::UpdateGraphicPayload payload;
  payload.id = id;
  payload.document_json = std::move(json);
  EditPlan plan;
  plan.ok = true;
  plan.label = "Edit Graphic";
  plan.commands.push_back({CommandType::UpdateGraphic, std::move(payload)});
  return plan;
}

EditPlan PlanRenameGraphic(const EditContext&, const std::string& id, const std::string& name) {
  if (name.empty()) return EditPlan::Refuse("A graphic needs a name");
  commands::UpdateGraphicPayload payload;
  payload.id = id;
  payload.name = name;
  EditPlan plan;
  plan.ok = true;
  plan.label = "Rename Graphic";
  plan.commands.push_back({CommandType::UpdateGraphic, std::move(payload)});
  return plan;
}

EditPlan PlanDeleteGraphic(const EditContext& ctx, const std::string& id) {
  if (ctx.sequence != nullptr) {
    const auto reference = "project:" + id;
    for (const auto& track : ctx.sequence->tracks) {
      for (const auto& clip : track.clips) {
        for (const auto& effect : clip.effects) {
          if (effect.preset_name == reference) return EditPlan::Refuse("The graphic is on the timeline (clip " + clip.name + "); remove the clips first");
        }
      }
    }
  }
  EditPlan plan;
  plan.ok = true;
  plan.label = "Delete Graphic";
  plan.commands.push_back({CommandType::DeleteGraphic, commands::DeleteGraphicPayload{id}});
  return plan;
}

EditPlan PlanCreateFromTemplate(const EditContext& ctx, const std::string& name, const std::string& template_id, std::int64_t version,
                                const std::map<std::string, std::string>& values, std::string* id_out) {
  if (!ctx.new_id) return EditPlan::Refuse("There is no way to name the graphic");
  commands::CreateGraphicPayload payload;
  payload.id = ctx.new_id("graphic");
  payload.name = name.empty() ? "Title" : name;
  payload.kind = "template";
  payload.template_id = template_id;
  payload.template_version = version;
  payload.values = values;
  if (id_out != nullptr) *id_out = payload.id;
  EditPlan plan;
  plan.ok = true;
  plan.label = "New Title from Template";
  plan.commands.push_back({CommandType::CreateGraphic, std::move(payload)});
  return plan;
}

EditPlan PlanSetTemplateValues(const EditContext&, const std::string& id, const std::map<std::string, std::string>& values) {
  commands::UpdateGraphicPayload payload;
  payload.id = id;
  payload.values = values;
  EditPlan plan;
  plan.ok = true;
  plan.label = "Change Title";
  plan.commands.push_back({CommandType::UpdateGraphic, std::move(payload)});
  return plan;
}

EditPlan PlanInstallTemplate(const EditContext&, const gfx::TemplatePackage& package) {
  std::string json;
  try {
    json = gfx::ToJson(package);
    (void)gfx::ParseTemplate(json);
  } catch (const std::exception& error) {
    return EditPlan::Refuse(error.what());
  }
  EditPlan plan;
  plan.ok = true;
  plan.label = "Add Title Template";
  plan.commands.push_back({CommandType::InstallGraphicTemplate, commands::InstallGraphicTemplatePayload{std::move(json)}});
  return plan;
}

EditPlan PlanPlaceGraphic(const EditContext& ctx, const std::string& graphic_id, const std::string& name, const RationalTime& at,
                          const RationalTime& duration, const std::string& track_id) {
  if (ctx.sequence == nullptr || !ctx.new_id) return EditPlan::Refuse("There is no sequence");
  if (duration.Compare(RationalTime(0, 1)) <= 0) return EditPlan::Refuse("A graphic needs a length");
  const auto end = at.Add(duration);
  const auto free_at = [&](const timeline::Track& track) {
    return std::none_of(track.clips.begin(), track.clips.end(), [&](const timeline::Clip& clip) {
      return clip.timeline_start.Compare(end) < 0 && clip.end().Compare(at) > 0;
    });
  };
  const timeline::Track* chosen = nullptr;
  if (!track_id.empty()) {
    chosen = ctx.sequence->FindTrack(track_id);
    if (chosen == nullptr || chosen->kind != model::TrackKind::Video || chosen->is_bus) return EditPlan::Refuse("Graphics go on picture tracks");
    if (chosen->locked) return EditPlan::Refuse("The track " + chosen->id + " is locked");
    if (!free_at(*chosen)) return EditPlan::Refuse("Something else is on that track then");
  } else {
    for (const auto& track : ctx.sequence->tracks) {
      if (track.kind != model::TrackKind::Video || track.is_bus || track.locked || !free_at(track)) continue;
      if (chosen == nullptr || track.order > chosen->order) chosen = &track;
    }
  }
  EditPlan plan;
  plan.ok = true;
  plan.label = "Add Graphic to Timeline";
  std::string target;
  if (chosen != nullptr) {
    target = chosen->id;
  } else {
    // Every picture track is taken or locked at that time: a new one above them all.
    std::int64_t order = 0;
    for (const auto& track : ctx.sequence->tracks) {
      if (track.kind == model::TrackKind::Video && !track.is_bus) order = std::max(order, track.order + 1);
    }
    commands::AddTrackPayload track;
    track.id = ctx.new_id("v");
    track.sequence_id = ctx.sequence->id;
    track.order = order;
    track.name = "V" + std::to_string(order + 1);
    target = track.id;
    plan.commands.push_back({CommandType::AddVideoTrack, std::move(track)});
    plan.notes.push_back("A new picture track was added for the graphic");
  }
  commands::AddGraphicClipPayload clip;
  clip.clip_id = ctx.new_id("clip");
  clip.track_id = target;
  clip.graphic_id = graphic_id;
  clip.timeline_start = at;
  clip.duration = duration;
  clip.name = name;
  plan.commands.push_back({CommandType::AddGraphicClip, std::move(clip)});
  plan.result_time = at;
  return plan;
}

}  // namespace cutline::ui

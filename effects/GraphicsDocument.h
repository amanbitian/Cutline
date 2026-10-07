#pragma once

// Title and graphic documents and the reusable template packages built on them, as data: parsing,
// validation, animation, layout and the bundle the project stores. Drawing them is render/Graphics.h.
//
// Coordinates and sizes are normalised to the document, so one package can render at any sequence
// size. Everything here is pure computation on text and numbers, which is why the project store can
// check a graphic before it accepts it.
//
// Versions. A document is schema 1 until it uses per-element animation or anchoring, which only this
// build understands; such a document is written as schema 2, and an older build refuses it instead
// of drawing it without its animation. Rotation, outlines, drop shadows and rounded corners are schema 3
// for the same reason.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cutline::render::graphics {

inline constexpr std::int64_t kBaseDocumentVersion = 1;
inline constexpr std::int64_t kDocumentVersion = 3;  // the newest this build reads
inline constexpr std::int64_t kTemplateVersion = 1;

enum class ElementType { Text, Rectangle, Ellipse, Image };

// One property of an element moving over time. Times are seconds from the start of the clip showing
// the graphic; before the first key the value is the first key's, after the last it is the last's.
struct AnimationKey final {
  double time{0.0};
  double value{0.0};
};

struct Animation final {
  // x, y, width, height, opacity, font_size, fill_r, fill_g, fill_b or fill_a; and from schema 3 rotation,
  // corner_radius, stroke_width, shadow_x, shadow_y or shadow_blur.
  std::string property;
  // linear, hold, ease_in, ease_out or ease_in_out: how the value travels from a key to the next.
  std::string interpolation{"linear"};
  std::vector<AnimationKey> keys;
};

struct Element final {
  std::string id;
  ElementType type{ElementType::Rectangle};
  double x{0.0}, y{0.0}, width{0.0}, height{0.0};
  double opacity{1.0};
  std::array<double, 4> fill{1.0, 1.0, 1.0, 1.0};
  std::string text;
  // One family, or several separated by commas in order of preference ("Segoe UI, Arial"): the first
  // installed on the machine is used.
  std::string font{"Arial"};
  double font_size{0.08};
  bool bold{false};
  bool italic{false};
  std::string align{"center"};
  std::string asset;
  // Responsive constraints. "none" scales the element with the picture (the position and size stay the
  // same fraction of it, so a different shape of picture stretches it). Any other value keeps the element's
  // size in proportion to the document fitted inside the picture and pins it: left/center/right and
  // top/middle/bottom keep the margin or centre it had, "stretch" keeps both margins and lets the size
  // flex. An element with an anchor on either axis is laid out this way on both.
  std::string anchor_x{"none"};
  std::string anchor_y{"none"};
  std::vector<Animation> animations;
  // Schema 3. Lengths are fractions of the document's height, like font_size, so they scale with the
  // picture. rotation is in degrees, clockwise, about the middle of the element.
  double rotation{0.0};
  // Rectangles and images: how far the corners are rounded.
  double corner_radius{0.0};
  // An outline in stroke: inside the edge of a shape or an image, outside the letters of a text. Drawn
  // only when stroke_width is above 0.
  double stroke_width{0.0};
  std::array<double, 4> stroke{0.0, 0.0, 0.0, 1.0};
  // A drop shadow of shadow's colour, shown only when its alpha is above 0, offset by shadow_x/shadow_y
  // and softened over shadow_blur. It falls in the picture's own directions, whatever the rotation.
  std::array<double, 4> shadow{0.0, 0.0, 0.0, 0.0};
  double shadow_x{0.0}, shadow_y{0.0}, shadow_blur{0.0};
};

struct Document final {
  std::int64_t schema_version{kBaseDocumentVersion};
  int width{1920};
  int height{1080};
  std::vector<Element> elements;
};

struct Control final {
  std::string name;
  std::string element_id;
  // text, asset, x, y, width, height, opacity, font_size, rotation, corner_radius, stroke_width,
  // shadow_x, shadow_y, shadow_blur, or a colour: fill, stroke or shadow ("r,g,b,a" in 0..1 or "#RRGGBB[AA]").
  std::string property;
  std::string default_value;
  // What the control is called in an interface, and the range a numeric value must stay in. A
  // maximum below the minimum means no limit.
  std::string label;
  double minimum{0.0};
  double maximum{-1.0};
};

struct TemplatePackage final {
  std::int64_t schema_version{kTemplateVersion};
  std::string id;
  std::int64_t version{1};
  std::string name;
  std::string description;
  Document document;
  std::vector<Control> controls;
};

[[nodiscard]] Document ParseDocument(const std::string& json);
[[nodiscard]] TemplatePackage ParseTemplate(const std::string& json);
[[nodiscard]] Document LoadDocument(const std::string& path);
[[nodiscard]] TemplatePackage LoadTemplate(const std::string& path);
[[nodiscard]] std::string ToJson(const Document& document);
[[nodiscard]] std::string ToJson(const TemplatePackage& package);

// Throws std::invalid_argument for a document or package that could not be drawn faithfully.
void Validate(const Document& document);
void Validate(const TemplatePackage& package);

// Whether an element draws anything beyond a plain upright fill (rotation, an outline, a shadow or rounded corners).
[[nodiscard]] bool UsesEffects(const Element& element);

// The schema version a document needs given what it uses.
[[nodiscard]] std::int64_t RequiredVersion(const Document& document);

// Applies named user values to a copy of the package's document. Unknown controls, elements, malformed
// or out-of-range values are rejected.
[[nodiscard]] Document Instantiate(const TemplatePackage& package, const std::map<std::string, std::string>& values);

// The element as it is `seconds` into the clip: every animated property at its value then.
[[nodiscard]] Element Evaluate(const Element& element, double seconds);
[[nodiscard]] double Evaluate(const Animation& animation, double seconds);

// Where an element lands, in pixels, on a picture of the given size.
struct Placement final {
  int x{0}, y{0}, width{0}, height{0};
  // The height of the em in pixels, for text.
  double font_pixels{0.0};
  // Pixels in a length of 1.0 (the document's height as it lands): stroke widths, shadow offsets and corner
  // radii are fractions of it.
  double unit{0.0};
};
[[nodiscard]] Placement Place(const Document& document, const Element& evaluated, int picture_width, int picture_height);

// What the project stores for a graphic, as the one text the compositor reads:
//   {"kind":"graphic","document":{...}}
//   {"kind":"template","package":{...},"values":{"control":"value"}}
[[nodiscard]] std::string MakeGraphicBundle(const std::string& document_json);
[[nodiscard]] std::string MakeTemplateBundle(const std::string& package_json, const std::map<std::string, std::string>& values);
// The document a bundle describes (a template's after its values are applied).
[[nodiscard]] Document ResolveBundle(const std::string& bundle_json);

// The values of a template instance as the project stores them.
[[nodiscard]] std::string ValuesToJson(const std::map<std::string, std::string>& values);
[[nodiscard]] std::map<std::string, std::string> ValuesFromJson(const std::string& json);

}  // namespace cutline::render::graphics

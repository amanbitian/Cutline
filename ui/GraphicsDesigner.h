#pragma once

// Designing titles and graphics: a graphic document (effects/GraphicsDocument.h) edited as a picture.
//
// Everything here is pure computation on the document, so the designer's rules (what a click selects, how a corner
// handle resizes a turned element, where an edge snaps) are tested without a window. The window passes pointer
// positions in as fractions of the document and draws what it is given back. Geometry is worked in the document's own
// pixels (its width and height) so that a turned element keeps its shape while it is dragged.
//
// The project side is plans (ui/EditPlanner.h EditPlan): creating, saving and deleting graphics, putting one on the
// timeline, installing a template package, making an instance of one. Each is one undo step.

#include "effects/GraphicsDocument.h"
#include "ui/EditPlanner.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

namespace gfx = render::graphics;

// ------------------------------------------------------------------ the document ----

[[nodiscard]] gfx::Document NewGraphicDocument(int width = 1920, int height = 1080);

// A new element with sensible defaults for its type (text says "Text", a shape is a quarter of the document), added
// on top. The id is the type name with a number that no element has. Returns the id.
std::string AddElement(gfx::Document& document, gfx::ElementType type);
bool RemoveElement(gfx::Document& document, const std::string& id);
// A copy just beside the original, on top. Returns its id, or empty if there was no such element.
std::string DuplicateElement(gfx::Document& document, const std::string& id);

enum class Stacking { Front, Forward, Backward, Back };
// Elements are drawn in order, so later ones are on top. False when it is already as far as it can go.
bool Restack(gfx::Document& document, const std::string& id, Stacking where);

[[nodiscard]] gfx::Element* FindElement(gfx::Document& document, const std::string& id);
[[nodiscard]] const gfx::Element* FindElement(const gfx::Document& document, const std::string& id);

// What a person can set on an element, grouped for a panel.
struct PropertyInfo final {
  std::string name;
  std::string label;
  std::string group;
  // number, text, colour, toggle, choice or asset.
  std::string kind;
  double minimum{0.0}, maximum{1.0}, step{0.01};
  std::vector<std::string> choices;
};
[[nodiscard]] std::vector<PropertyInfo> PropertiesOf(gfx::ElementType type);

// How an element arrives: none, fade, slide_left (comes in from the left), slide_right, slide_up (rises from below),
// slide_down, or pop (grows from nothing). It replaces the element's animation of the properties it uses; animations
// of other properties are kept.
[[nodiscard]] const std::vector<std::string>& EntranceKinds();
void SetEntrance(gfx::Element& element, const std::string& kind, double seconds);
// The entrance the element's animation makes up (none when it makes up none of the above) and how long it takes.
[[nodiscard]] std::string EntranceOf(const gfx::Element& element, double* seconds = nullptr);

// A property as text ("0.25", "#FF8800FF", "true"), and set from text. SetProperty throws std::invalid_argument for an
// unknown property or a value that is not one (it changes nothing then).
[[nodiscard]] std::string PropertyValue(const gfx::Element& element, const std::string& name);
void SetProperty(gfx::Element& element, const std::string& name, const std::string& value);

[[nodiscard]] std::string ColourText(const std::array<double, 4>& colour);
[[nodiscard]] std::array<double, 4> ParseColour(const std::string& text);

// A short name for a list: the text of a text element, else the type and id.
[[nodiscard]] std::string ElementLabel(const gfx::Element& element);

// What stops the document being saved (an image with no picture chosen yet), in words for the person.
[[nodiscard]] std::vector<std::string> Problems(const gfx::Document& document);
// The document as the designer draws it: an image with no picture is shown as a grey placeholder.
[[nodiscard]] gfx::Document ForPreview(const gfx::Document& document);

// ------------------------------------------------------------------------ canvas ----

// A point as fractions of the document (0 to 1 across and down).
struct DocPoint final {
  double x{0.0}, y{0.0};
};

// Where the element's box lies (its own, before turning), as fractions of the document.
struct ElementBox final {
  double x{0.0}, y{0.0}, width{0.0}, height{0.0};
};
[[nodiscard]] ElementBox ElementBoxOf(const gfx::Document& document, const gfx::Element& element);

// The top-most element under the point, allowing for its turn; text counts as its box.
[[nodiscard]] std::optional<std::string> HitElement(const gfx::Document& document, DocPoint at);

enum class Grip { None, Body, TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left, Turn };
struct GripPosition final {
  Grip grip{Grip::None};
  DocPoint at;
};
// The eight resize grips and the turn grip (above the top edge), as fractions of the document, turned with the element.
[[nodiscard]] std::vector<GripPosition> GripsOf(const gfx::Document& document, const gfx::Element& element);
// The grip under the point within `reach` (a fraction of the document's height), else Body if the point is inside the
// element, else None.
[[nodiscard]] Grip GripAt(const gfx::Document& document, const gfx::Element& element, DocPoint at, double reach = 0.015);

struct Guide final {
  bool vertical{false};
  // Where it lies, as a fraction of the document.
  double at{0.0};
};

// Moves the element by (dx, dy) fractions of the document; with `snap` its edges and middle fall on the document's
// edges and middle (and stay off by nothing) when within `threshold`. Returns the guides it landed on.
std::vector<Guide> MoveBy(gfx::Document& document, const std::string& id, double dx, double dy, bool snap = true, double threshold = 0.008);
// Drags one grip by (dx, dy): the opposite edge or corner stays where it is, however the element is turned. A corner
// keeps the proportions with `keep_proportions`. The Turn grip is not handled here (see TurnTo). A box never gets
// smaller than a hundredth of the document.
void ResizeBy(gfx::Document& document, const std::string& id, Grip grip, double dx, double dy, bool keep_proportions = false);
// Turns the element so its top points at the point; `snap_degrees` rounds to a multiple of it when within three degrees
// of one (0 turns snapping off).
void TurnTo(gfx::Document& document, const std::string& id, DocPoint at, double snap_degrees = 15.0);

enum class Alignment { Left, Centre, Right, Top, Middle, Bottom };
void AlignElement(gfx::Document& document, const std::string& id, Alignment alignment);

// ------------------------------------------------------------------------ templates ----

// A control for each thing a person would change in the document: the text of each text element, the picture of each
// image, the colour of each text and shape. Named by the element ("headline", "headline_colour").
[[nodiscard]] std::vector<gfx::Control> AutoControls(const gfx::Document& document);
[[nodiscard]] gfx::TemplatePackage MakeTemplate(const gfx::Document& document, const std::string& id, const std::string& name,
                                                const std::string& description, std::vector<gfx::Control> controls);

// The templates that come with the application.
[[nodiscard]] std::vector<gfx::TemplatePackage> BuiltInTemplates();

// ------------------------------------------------------------------------- plans ----

// Makes a graphic of the document. `id_out` receives its id.
[[nodiscard]] EditPlan PlanCreateGraphic(const EditContext& ctx, const std::string& name, const gfx::Document& document, std::string* id_out = nullptr);
[[nodiscard]] EditPlan PlanSaveGraphic(const EditContext& ctx, const std::string& id, const gfx::Document& document);
[[nodiscard]] EditPlan PlanRenameGraphic(const EditContext& ctx, const std::string& id, const std::string& name);
[[nodiscard]] EditPlan PlanDeleteGraphic(const EditContext& ctx, const std::string& id);
// Makes an instance of an installed template; `values` are its controls.
[[nodiscard]] EditPlan PlanCreateFromTemplate(const EditContext& ctx, const std::string& name, const std::string& template_id,
                                              std::int64_t version, const std::map<std::string, std::string>& values,
                                              std::string* id_out = nullptr);
[[nodiscard]] EditPlan PlanSetTemplateValues(const EditContext& ctx, const std::string& id, const std::map<std::string, std::string>& values);
// Adds a package to the project's library (a version already there with the same content is left as it is).
[[nodiscard]] EditPlan PlanInstallTemplate(const EditContext& ctx, const gfx::TemplatePackage& package);

// Puts the graphic on the timeline at `at` for `duration`, on the top picture track that is free then; a track is added
// above the others when there is none. `track_id` names a track to use instead.
[[nodiscard]] EditPlan PlanPlaceGraphic(const EditContext& ctx, const std::string& graphic_id, const std::string& name, const RationalTime& at,
                                        const RationalTime& duration, const std::string& track_id = {});

}  // namespace cutline::ui

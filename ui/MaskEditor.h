#pragma once

// Drawing and editing masks on the picture: what can be grabbed, what a drag does to it, how a shape is begun, how its
// points are added, removed and smoothed, how its values are keyed over time, and how it is made to follow a track.
//
// A mask is an effects::mask::Document (effects/MaskDocument.h): a rectangle, an ellipse or a closed cubic Bezier path in
// coordinates normalised to the picture, with feather and expansion in pixels. Everything here works on the document as it
// is *at one moment*: the editor shows and drags the evaluated shape, and `ApplyEdit` writes the change back to the stored
// document the way an animator expects: a value that is keyed gets a key at this time, one that is not changes for the
// whole clip.
//
// Nothing here knows about Qt or the monitor; the monitor turns the pointer into normalised picture coordinates.

#include "effects/MaskDocument.h"
#include "render/Tracking.h"
#include "ui/EditPlanner.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cutline::ui {

using MaskDocument = effects::mask::Document;

// The size of the picture the mask sits on, in pixels. Rotation and a circle's roundness are measured in pixels, so the
// editor needs it to turn normalised distances into the same ones the compositor uses.
struct PictureSize final {
  double width{1920};
  double height{1080};
};

struct MaskHandle final {
  enum class Kind {
    None,
    Body,         // anywhere inside the shape: move it
    EdgeLeft,     // rectangle and ellipse: the four sides, in the shape's own (rotated) directions
    EdgeRight,
    EdgeTop,
    EdgeBottom,
    Rotate,       // the knob beyond the top edge
    Point,        // path: a vertex
    TangentIn,    // path: the handle on the way into the selected vertex
    TangentOut,   // path: the handle on the way out
    Segment,      // path: on the line between two vertices (to add a vertex there)
  } kind{Kind::None};
  int index{0};  // the vertex, or for a segment the one it starts at
  [[nodiscard]] bool operator==(const MaskHandle& other) const { return kind == other.kind && index == other.index; }
};

// A grabbable spot, for drawing: in normalised coordinates.
struct HandlePosition final {
  MaskHandle handle;
  double x{0};
  double y{0};
};

// ---------------------------------------------------------------- the shape ----

// The outline as a polyline (closed: the last point joins the first), in normalised coordinates, for drawing.
[[nodiscard]] std::vector<std::pair<double, double>> Outline(const MaskDocument& document, PictureSize picture, int steps_per_segment = 16);
// Every handle the shape offers, in normalised coordinates. `selected_point` is the vertex whose tangents are shown (-1: none).
[[nodiscard]] std::vector<HandlePosition> Handles(const MaskDocument& document, PictureSize picture, int selected_point = -1);
// What is under a point: the nearest handle within `tolerance_pixels`, else the body if inside, else nothing.
[[nodiscard]] MaskHandle HitTest(const MaskDocument& document, PictureSize picture, double x, double y, double tolerance_pixels, int selected_point = -1);
[[nodiscard]] bool Inside(const MaskDocument& document, PictureSize picture, double x, double y);

// ----------------------------------------------------------------- dragging ----

struct DragOptions final {
  bool constrain{false};  // keep a rectangle square / a circle round, rotation to 15 degrees, a handle on an axis
  bool break_tangent{false};  // move one tangent without its partner following
};
// The shape as it is after dragging `handle` from where the pointer was pressed to where it is now. `before` is the shape
// as it was at the press, so the whole drag is computed from it and never accumulates.
[[nodiscard]] MaskDocument Drag(const MaskDocument& before, const MaskHandle& handle, double press_x, double press_y, double x, double y,
                                PictureSize picture, const DragOptions& options = {});

// ------------------------------------------------------------- beginning one ----

[[nodiscard]] MaskDocument NewRectangle(double x0, double y0, double x1, double y1, PictureSize picture);
[[nodiscard]] MaskDocument NewEllipse(double x0, double y0, double x1, double y1, PictureSize picture);
// A closed path through the points, with straight sides; drag handles out of the vertices to curve it.
[[nodiscard]] MaskDocument NewPath(const std::vector<std::pair<double, double>>& points);
// A rounded (smooth) version of a vertex: tangents a third of the way to each neighbour, in line with them.
[[nodiscard]] MaskDocument Smooth(const MaskDocument& document, int index);
[[nodiscard]] MaskDocument Corner(const MaskDocument& document, int index);  // tangents back onto the vertex
[[nodiscard]] bool IsSmooth(const MaskDocument& document, int index);

// Adds a vertex on the segment nearest the point (within the tolerance) without changing the curve; unchanged when none is
// near. A new vertex on a path invalidates animation of its vertices, which is dropped.
[[nodiscard]] MaskDocument InsertPoint(const MaskDocument& document, PictureSize picture, double x, double y, double tolerance_pixels);
// Takes a vertex away (a path keeps at least three).
[[nodiscard]] MaskDocument RemovePoint(const MaskDocument& document, int index);

// ---------------------------------------------------------- values over time ----

// The names a document's values go by when keyed: center_x, width, feather, point_3_x, ...
[[nodiscard]] std::vector<std::string> PropertyNames(const MaskDocument& document);
[[nodiscard]] bool IsAnimated(const MaskDocument& document, const std::string& property);
[[nodiscard]] double ValueOf(const MaskDocument& document, const std::string& property);
// Writes `edited` (the shape at `seconds`, after a drag) back into `stored`: every value that differs from `evaluated` (the
// shape at that moment before the drag) is set at this time, as a key when the value is animated and as the base value when
// it is not. Shape, combine and invert are not animated and are copied.
[[nodiscard]] MaskDocument ApplyEdit(const MaskDocument& stored, const MaskDocument& evaluated, const MaskDocument& edited, double seconds);
// Starts animating a value: a key at this time holding what it is now. Stopping removes the animation and leaves the value
// it has at this time.
[[nodiscard]] MaskDocument StartAnimating(const MaskDocument& document, const std::string& property, double seconds);
[[nodiscard]] MaskDocument StopAnimating(const MaskDocument& document, const std::string& property, double seconds);
// Removes the key at this time (and the animation with it when it was the last).
[[nodiscard]] MaskDocument RemoveKey(const MaskDocument& document, const std::string& property, double seconds);

// ------------------------------------------------------------------- tracking ----

// Makes the mask follow a point track: the shape stays where it is at `reference_seconds` and moves by what the track moved
// since then, keyed at every sample the track found (lost samples are skipped). Rectangles and ellipses animate their
// centre; paths animate every vertex and handle. Returns the document unchanged if the track has fewer than two usable
// samples.
[[nodiscard]] MaskDocument FollowTrack(const MaskDocument& document, const render::tracking::TrackResult& track, double reference_seconds, PictureSize picture);

// ---------------------------------------------------------------------- plans ----

// Masks belong to an effect on a clip; these are single undo steps on the command bus.
[[nodiscard]] EditPlan PlanAddMask(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const MaskDocument& document);
[[nodiscard]] EditPlan PlanUpdateMask(const EditContext& ctx, const std::string& clip_id, const std::string& mask_id, const MaskDocument& document, const std::string& label = "Edit Mask");
[[nodiscard]] EditPlan PlanRemoveMask(const EditContext& ctx, const std::string& clip_id, const std::string& mask_id);

}  // namespace cutline::ui

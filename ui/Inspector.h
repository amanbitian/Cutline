#pragma once

// The effect controls of a clip: what the inspector shows (each effect, each parameter with its value at the
// playhead, its range, whether it is animated and whether a keyframe sits at this very time) and the edits it
// makes (change a value, switch animation on or off, add or remove a keyframe, add, remove, reorder or disable
// an effect), each as an EditPlan to run as one undoable step.
//
// Parameter metadata comes from the effect registry; an effect the registry does not know (from a plug-in or a
// newer version) is still shown with whatever its parameters hold, just without ranges.

#include "effects/EffectRegistry.h"
#include "timeline/Sequence.h"
#include "ui/EditPlanner.h"

#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

struct ParameterRow final {
  std::string effect_id;
  std::string parameter_id;
  std::string name;          // the registry's id for it ("radius")
  std::string display_name;  // "Radius"
  int dimension{1};
  anim::Value value;          // at the time shown
  anim::Value default_value;
  std::optional<double> minimum, maximum;
  effects::Unit unit{effects::Unit::None};
  bool keyframeable{true};
  bool keyframed{false};
  bool key_here{false};
  std::vector<RationalTime> key_times;
};

struct EffectRow final {
  std::string id;
  std::string type;
  std::string display_name;
  std::string category;
  bool enabled{true};
  bool intrinsic{false};
  bool known{false};
  std::int64_t order{0};
  std::string preset_name;
  std::string asset_kind;
  std::vector<ParameterRow> parameters;
};

struct InspectorState final {
  std::string clip_id;
  std::string clip_name;
  std::vector<EffectRow> effects;
};

// The clip's effects as the inspector shows them at `local_time`, which is the time within the clip (the playhead
// minus the clip's start): keyframes are kept on that clock.
[[nodiscard]] InspectorState BuildInspector(const timeline::Clip& clip, const RationalTime& local_time);

// ---------------------------------------------------------------------- the catalogue ----

struct CatalogueEntry final {
  std::string id;
  std::string name;
  std::string category;
  bool needs_asset{false};
  std::string asset_kind;   // ".cube" for a LUT, "colorspace" for a colour space name, ...
};

// The effects that can be added to a clip (video effects this build can render), by category then name, narrowed to
// those whose name or category contains the query.
[[nodiscard]] std::vector<CatalogueEntry> EffectCatalogue(const std::string& query = {});

// ------------------------------------------------------------------------- edits ----

[[nodiscard]] EditPlan PlanAddEffect(const EditContext& ctx, const std::string& clip_id, const std::string& effect_type, const std::string& preset_name = {});
// Puts an effect that analysis produced (its parameters, with their keyframes, already keyed to times within the clip)
// on the clip as one step. The effect id is replaced by a new one.
[[nodiscard]] EditPlan PlanAddAnalysedEffect(const EditContext& ctx, const std::string& clip_id, const timeline::Effect& effect, const std::string& label);
[[nodiscard]] EditPlan PlanRemoveEffect(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id);
[[nodiscard]] EditPlan PlanSetEffectEnabled(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, bool enabled);
// Moves the effect one place earlier (negative) or later (positive) in the clip's stack.
[[nodiscard]] EditPlan PlanMoveEffect(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, int places);

// Changes a parameter's value. If the parameter is animated this sets a keyframe at `local_time` (keeping the
// interpolation of the key before it, else linear); otherwise it sets the value for the whole clip. The value is
// checked against the effect's declaration, and refused if it does not fit.
[[nodiscard]] EditPlan PlanSetParameter(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name,
                                        const anim::Value& value, const RationalTime& local_time);
// The same value, brought inside the parameter's range.
[[nodiscard]] anim::Value ClampToRange(const ParameterRow& row, const anim::Value& value);
// The stopwatch: from steady to animated (a key at `local_time` holding the current value), or back to steady
// (every key removed, the value at `local_time` kept).
[[nodiscard]] EditPlan PlanToggleAnimation(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name,
                                           const RationalTime& local_time);
// A key at `local_time` holding the current value, or removal of the key there.
[[nodiscard]] EditPlan PlanToggleKeyframe(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name,
                                          const RationalTime& local_time);
[[nodiscard]] EditPlan PlanResetParameter(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name);
// The nearest key after / before `local_time` among the parameter's keys, as a time within the clip.
[[nodiscard]] std::optional<RationalTime> NextKeyframe(const ParameterRow& row, const RationalTime& local_time);
[[nodiscard]] std::optional<RationalTime> PreviousKeyframe(const ParameterRow& row, const RationalTime& local_time);

}  // namespace cutline::ui

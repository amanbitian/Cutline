#pragma once

// Putting transitions on cuts, changing them and taking them off: the editing rules.
//
// A transition lives on one track and joins the clip going out to the clip coming in at a cut (the end of one is the start of
// the next), or fades one clip in from, or out to, whatever is beneath it. Where it sits against the cut is its alignment:
// centred (half before the cut, half after), starting at the cut, or ending at it. A transition needs picture the cut does not
// show: a centred one of length d plays d/2 of the outgoing clip's media after the cut and d/2 of the incoming clip's before
// it (its handles), a starting one d of the outgoing, an ending one d of the incoming. The plan keeps the length to what the
// handles allow, and says so, rather than letting the picture run out in the middle of the transition.

#include "ui/EditPlanner.h"

#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

// A place a transition can go: on `track_id`, between `from_clip` and `to_clip` (both set for a cut; one set at the edge of
// a clip with nothing touching it).
struct TransitionSite final {
  std::string track_id;
  std::optional<std::string> from_clip;
  std::optional<std::string> to_clip;
  RationalTime at;   // the cut, or the clip's edge
  [[nodiscard]] bool cut() const { return from_clip.has_value() && to_clip.has_value(); }
};

// The sites on video tracks within `window` of `around`, nearest first. Cuts are listed before lone edges at the same distance.
// With `track_id` only that track.
[[nodiscard]] std::vector<TransitionSite> TransitionSitesNear(const timeline::Sequence& sequence, const RationalTime& around, const RationalTime& window,
                                                              const std::string& track_id = {});

struct TransitionRequest final {
  std::string kind{"cross_dissolve"};
  RationalTime duration{1, 1};
  model::TransitionAlignment alignment{model::TransitionAlignment::Center};
};

// The handles of a clip, as timeline time: how much more picture there is before its first frame and after its last.
struct ClipHandles final {
  RationalTime head{0, 1}, tail{0, 1};
  bool known{false};
};
[[nodiscard]] ClipHandles HandlesOf(const EditContext& ctx, const timeline::Clip& clip);

// Adds a transition at the site. The length is held to the handles and to the clips, to whole frames, and the plan's notes say
// when it was; refused when the site has no room (no handle), the track is locked, a clip has a speed ramp, or another
// transition is already there. At a lone edge the alignment follows the edge (a clip's start fades in, its end fades out).
[[nodiscard]] EditPlan PlanAddTransition(const EditContext& ctx, const TransitionSite& site, const TransitionRequest& request);

// Changes a transition's kind, length and alignment (as the same rules would for a new one): one undo step.
[[nodiscard]] EditPlan PlanChangeTransition(const EditContext& ctx, const std::string& transition_id, const TransitionRequest& request);
[[nodiscard]] EditPlan PlanRemoveTransition(const EditContext& ctx, const std::string& transition_id);

}  // namespace cutline::ui

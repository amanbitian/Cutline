#include "ui/Transitions.h"

#include "render/Transitions.h"

#include <algorithm>
#include <cmath>

namespace cutline::ui {
namespace {

using commands::CommandType;

const RationalTime kZero(0, 1);
const RationalTime kUnbounded(1000000, 1);

RationalTime Min(const RationalTime& a, const RationalTime& b) { return a.Compare(b) <= 0 ? a : b; }

const timeline::Clip* FindClip(const timeline::Sequence& sequence, const std::string& id, const timeline::Track** track = nullptr) {
  for (const auto& t : sequence.tracks) {
    for (const auto& clip : t.clips) {
      if (clip.id == id) {
        if (track != nullptr) *track = &t;
        return &clip;
      }
    }
  }
  return nullptr;
}

bool Ramped(const timeline::Clip& clip) {
  return std::any_of(clip.effects.begin(), clip.effects.end(), [](const timeline::Effect& e) { return e.effect_type == "time_remap"; });
}

RationalTime Frame(const timeline::Sequence& sequence) { return RationalTime(sequence.frame_rate.denominator, sequence.frame_rate.numerator); }

// `d` rounded down to a whole number of frames (an even number when `even`, so that half of it is whole too).
RationalTime WholeFrames(const timeline::Sequence& sequence, const RationalTime& d, bool even) {
  auto frames = d.ToFrames(sequence.frame_rate, time::RoundingMode::Floor);
  if (even && frames % 2 != 0) --frames;
  return RationalTime::FromFrames(std::max<std::int64_t>(frames, 0), sequence.frame_rate);
}

EditPlan BuildAdd(const EditContext& ctx, const TransitionSite& site, TransitionRequest request, const std::string& replacing) {
  if (ctx.sequence == nullptr || !ctx.new_id) return EditPlan::Refuse("There is no sequence");
  const auto& sequence = *ctx.sequence;
  if (!render::IsKnownTransition(request.kind)) return EditPlan::Refuse("This build has no transition called " + request.kind);
  const timeline::Track* track = sequence.FindTrack(site.track_id);
  if (track == nullptr || track->kind != model::TrackKind::Video) return EditPlan::Refuse("Transitions go on picture tracks");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  const timeline::Clip* from = site.from_clip ? FindClip(sequence, *site.from_clip) : nullptr;
  const timeline::Clip* to = site.to_clip ? FindClip(sequence, *site.to_clip) : nullptr;
  if ((site.from_clip && from == nullptr) || (site.to_clip && to == nullptr) || (from == nullptr && to == nullptr)) return EditPlan::Refuse("The clips this transition joins are not in the sequence");
  if ((from != nullptr && Ramped(*from)) || (to != nullptr && Ramped(*to))) return EditPlan::Refuse("A clip with a speed ramp has no handles to make a transition from");
  if (from != nullptr && to != nullptr && from->end().Compare(to->timeline_start) != 0) return EditPlan::Refuse("The clips do not touch: a transition joins a cut");

  EditPlan plan;
  plan.ok = true;
  plan.label = "Add Transition";
  auto alignment = request.alignment;
  RationalTime cut = to != nullptr ? to->timeline_start : from->end();
  // The most the length can be, from what each side has to give.
  RationalTime limit = kUnbounded;
  if (from != nullptr && to != nullptr) {
    const auto out = HandlesOf(ctx, *from), in = HandlesOf(ctx, *to);
    switch (alignment) {
      case model::TransitionAlignment::Start:
        limit = Min(out.tail, to->duration());
        break;
      case model::TransitionAlignment::End:
        limit = Min(from->duration(), in.head);
        break;
      default:
        alignment = model::TransitionAlignment::Center;
        limit = Min(Min(from->duration(), out.tail), Min(in.head, to->duration())).Multiply(2);
        break;
    }
  } else if (to != nullptr) {
    alignment = model::TransitionAlignment::Start;   // a clip's start fades in from what is beneath
    limit = to->duration();
    if (request.alignment != alignment) plan.notes.push_back("At the start of a clip a transition begins at its first frame");
  } else {
    alignment = model::TransitionAlignment::End;
    limit = from->duration();
    if (request.alignment != alignment) plan.notes.push_back("At the end of a clip a transition finishes at its last frame");
  }
  auto length = WholeFrames(sequence, request.duration, alignment == model::TransitionAlignment::Center);
  if (length.Compare(limit) > 0) {
    length = WholeFrames(sequence, limit, alignment == model::TransitionAlignment::Center);
    plan.notes.push_back("The transition was shortened to what the clips' handles and lengths allow");
  }
  if (length.Compare(Frame(sequence)) < 0) return EditPlan::Refuse("There is no room for a transition here: the clips have no spare picture beyond the cut");

  RationalTime start = cut;
  if (alignment == model::TransitionAlignment::Center) start = cut.Subtract(length.Divide(2));
  else if (alignment == model::TransitionAlignment::End) start = cut.Subtract(length);
  const auto end = start.Add(length);
  if (start.Compare(kZero) < 0) return EditPlan::Refuse("The transition would begin before the start of the sequence");
  for (const auto& existing : track->transitions) {
    if (existing.id == replacing) continue;
    const auto existing_end = existing.timeline_start.Add(existing.duration);
    if (existing.timeline_start.Compare(end) < 0 && existing_end.Compare(start) > 0) return EditPlan::Refuse("There is already a transition here");
  }
  commands::AddTransitionPayload add;
  add.id = ctx.new_id("transition");
  add.track_id = track->id;
  add.kind = request.kind;
  add.alignment = alignment;
  if (from != nullptr) add.from_clip_id = from->id;
  if (to != nullptr) add.to_clip_id = to->id;
  add.timeline_start = start;
  add.duration = length;
  plan.commands.push_back({CommandType::AddTransition, add});
  plan.result_time = start;
  return plan;
}

}  // namespace

ClipHandles HandlesOf(const EditContext& ctx, const timeline::Clip& clip) {
  ClipHandles handles;
  handles.head = handles.tail = kUnbounded;
  std::optional<RationalTime> media_length;
  if (clip.source_kind == model::SourceKind::Media && ctx.media_duration) media_length = ctx.media_duration(clip.source_id);
  if (clip.source_kind == model::SourceKind::Sequence && ctx.sequence_duration) media_length = ctx.sequence_duration(clip.source_id);
  if (!media_length) return handles;   // a generator or an adjustment has no end to its picture
  handles.known = true;
  const auto before = clip.source_in;                                    // media before the first source frame used
  auto after = media_length->Subtract(clip.source_out);                  // and after the last
  if (after.Compare(kZero) < 0) after = kZero;
  const auto head_source = clip.reversed ? after : before, tail_source = clip.reversed ? before : after;
  handles.head = head_source.Divide(clip.playback_rate);
  handles.tail = tail_source.Divide(clip.playback_rate);
  return handles;
}

std::vector<TransitionSite> TransitionSitesNear(const timeline::Sequence& sequence, const RationalTime& around, const RationalTime& window, const std::string& track_id) {
  struct Candidate {
    TransitionSite site;
    RationalTime distance;
  };
  std::vector<Candidate> found;
  const auto distance = [&](const RationalTime& at) { return at.Compare(around) >= 0 ? at.Subtract(around) : around.Subtract(at); };
  for (const auto& track : sequence.tracks) {
    if (track.kind != model::TrackKind::Video || track.is_bus || (!track_id.empty() && track.id != track_id)) continue;
    std::vector<const timeline::Clip*> clips;
    for (const auto& clip : track.clips) clips.push_back(&clip);
    std::sort(clips.begin(), clips.end(), [](const auto* a, const auto* b) { return a->timeline_start.Compare(b->timeline_start) < 0; });
    for (std::size_t i = 0; i < clips.size(); ++i) {
      const auto* clip = clips[i];
      const bool touches_before = i > 0 && clips[i - 1]->end().Compare(clip->timeline_start) == 0;
      const bool touches_after = i + 1 < clips.size() && clips[i + 1]->timeline_start.Compare(clip->end()) == 0;
      if (touches_after) found.push_back({{track.id, clip->id, clips[i + 1]->id, clip->end()}, distance(clip->end())});
      if (!touches_before) found.push_back({{track.id, std::nullopt, clip->id, clip->timeline_start}, distance(clip->timeline_start)});
      if (!touches_after) found.push_back({{track.id, clip->id, std::nullopt, clip->end()}, distance(clip->end())});
    }
  }
  found.erase(std::remove_if(found.begin(), found.end(), [&](const Candidate& c) { return c.distance.Compare(window) > 0; }), found.end());
  std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
    const int c = a.distance.Compare(b.distance);
    if (c != 0) return c < 0;
    return a.site.cut() && !b.site.cut();
  });
  std::vector<TransitionSite> sites;
  for (auto& candidate : found) sites.push_back(std::move(candidate.site));
  return sites;
}

EditPlan PlanAddTransition(const EditContext& ctx, const TransitionSite& site, const TransitionRequest& request) { return BuildAdd(ctx, site, request, {}); }

EditPlan PlanChangeTransition(const EditContext& ctx, const std::string& transition_id, const TransitionRequest& request) {
  if (ctx.sequence == nullptr) return EditPlan::Refuse("There is no sequence");
  const timeline::Track* track = nullptr;
  const timeline::Transition* existing = nullptr;
  for (const auto& t : ctx.sequence->tracks) {
    for (const auto& transition : t.transitions) {
      if (transition.id == transition_id) {
        track = &t;
        existing = &transition;
      }
    }
  }
  if (existing == nullptr) return EditPlan::Refuse("The transition is not in the sequence");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  TransitionSite site;
  site.track_id = track->id;
  site.from_clip = existing->from_clip_id;
  site.to_clip = existing->to_clip_id;
  const auto* to = site.to_clip ? FindClip(*ctx.sequence, *site.to_clip) : nullptr;
  const auto* from = site.from_clip ? FindClip(*ctx.sequence, *site.from_clip) : nullptr;
  site.at = to != nullptr ? to->timeline_start : (from != nullptr ? from->end() : existing->timeline_start);
  auto added = BuildAdd(ctx, site, request, transition_id);
  if (!added.ok) return added;
  EditPlan plan = std::move(added);
  plan.label = "Change Transition";
  plan.commands.insert(plan.commands.begin(), {CommandType::RemoveTransition, commands::RemoveTransitionPayload{transition_id}});
  return plan;
}

EditPlan PlanRemoveTransition(const EditContext& ctx, const std::string& transition_id) {
  if (ctx.sequence == nullptr) return EditPlan::Refuse("There is no sequence");
  for (const auto& track : ctx.sequence->tracks) {
    for (const auto& transition : track.transitions) {
      if (transition.id != transition_id) continue;
      if (track.locked) return EditPlan::Refuse("The track " + track.id + " is locked");
      EditPlan plan;
      plan.ok = true;
      plan.label = "Remove Transition";
      plan.commands.push_back({CommandType::RemoveTransition, commands::RemoveTransitionPayload{transition_id}});
      return plan;
    }
  }
  return EditPlan::Refuse("The transition is not in the sequence");
}

}  // namespace cutline::ui

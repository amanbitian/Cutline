#include "ui/EditPlanner.h"

#include <algorithm>
#include <map>

namespace cutline::ui {
namespace {

using commands::CommandType;
using timeline::Clip;
using timeline::Track;

const RationalTime kZero(0, 1);

// A clip as the planner sees it while it works out a sequence of edits to it: the same arithmetic the store applies.
struct VClip {
  std::string id;
  std::string track_id;
  RationalTime start, in, out, rate{1, 1};
  bool reversed{false};

  [[nodiscard]] RationalTime duration() const { return out.Subtract(in).Divide(rate); }
  [[nodiscard]] RationalTime end() const { return start.Add(duration()); }

  [[nodiscard]] VClip TrimmedHead(const RationalTime& to) const {
    VClip c = *this;
    const auto shift = to.Subtract(start).Multiply(rate);
    if (reversed) c.out = out.Subtract(shift);
    else c.in = in.Add(shift);
    c.start = to;
    return c;
  }
  [[nodiscard]] VClip TrimmedTail(const RationalTime& to) const {
    VClip c = *this;
    const auto shift = to.Subtract(end()).Multiply(rate);
    if (reversed) c.in = in.Subtract(shift);
    else c.out = out.Add(shift);
    return c;
  }
  // The halves when cut at `at`: a reversed clip's left half holds the tail of its source.
  [[nodiscard]] std::pair<VClip, VClip> SplitAt(const RationalTime& at, const std::string& right_id) const {
    VClip left = *this, right = *this;
    right.id = right_id;
    right.start = at;
    const auto shift = at.Subtract(start).Multiply(rate);
    if (reversed) {
      left.in = out.Subtract(shift);
      right.out = out.Subtract(shift);
    } else {
      left.out = in.Add(shift);
      right.in = in.Add(shift);
    }
    return {left, right};
  }
};

VClip View(const Track& track, const Clip& clip) {
  return {clip.id, track.id, clip.timeline_start, clip.source_in, clip.source_out, clip.playback_rate, clip.reversed};
}

bool Ramped(const Clip& clip) {
  return std::any_of(clip.effects.begin(), clip.effects.end(), [](const timeline::Effect& e) { return e.effect_type == "time_remap"; });
}

struct Found {
  const Track* track{nullptr};
  const Clip* clip{nullptr};
};

Found FindClip(const timeline::Sequence& sequence, const std::string& id) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id == id) return {&track, &clip};
    }
  }
  return {};
}

RationalTime FrameLength(const EditContext& ctx) {
  const auto rate = ctx.sequence->frame_rate;
  return RationalTime(rate.denominator, rate.numerator);
}

std::string Name(const Found& f) { return f.clip->name.empty() ? f.clip->id : f.clip->name; }

PlannedCommand Planned(CommandType type, commands::CommandPayload payload) { return {type, std::move(payload)}; }

commands::TrimClipPayload TrimPayload(const VClip& c, bool propagate) {
  commands::TrimClipPayload payload;
  payload.id = c.id;
  payload.source_in = c.in;
  payload.source_out = c.out;
  payload.timeline_start = c.start;
  payload.propagate_links = propagate;
  return payload;
}

// The longest the media allows a clip's source range to run, if known.
std::optional<RationalTime> MediaLength(const EditContext& ctx, const Clip& clip) {
  if (clip.source_kind == model::SourceKind::Media && ctx.media_duration) return ctx.media_duration(clip.source_id);
  if (clip.source_kind == model::SourceKind::Sequence && ctx.sequence_duration) return ctx.sequence_duration(clip.source_id);
  return std::nullopt;
}

// The members edited together with a clip: the clip and, if linked editing is on, the clips linked to it.
std::vector<Found> Members(const EditContext& ctx, const Found& clip) {
  std::vector<Found> members{clip};
  if (!ctx.linked || clip.clip->linked_group.empty()) return members;
  for (const auto& track : ctx.sequence->tracks) {
    for (const auto& other : track.clips) {
      if (other.id != clip.clip->id && other.linked_group == clip.clip->linked_group) members.push_back({&track, &other});
    }
  }
  return members;
}

RationalTime Min(const RationalTime& a, const RationalTime& b) { return a.Compare(b) <= 0 ? a : b; }
RationalTime Max(const RationalTime& a, const RationalTime& b) { return a.Compare(b) >= 0 ? a : b; }
RationalTime Clamp(const RationalTime& v, const RationalTime& lo, const RationalTime& hi) { return Max(lo, Min(v, hi)); }

// The end of the clip before `clip` on its track (zero if none), and the start of the one after (nullopt if none).
RationalTime PreviousEnd(const Track& track, const Clip& clip, const std::set<std::string>& skip = {}) {
  RationalTime end = kZero;
  for (const auto& other : track.clips) {
    if (other.id == clip.id || skip.count(other.id) != 0) continue;
    if (other.timeline_start.Compare(clip.timeline_start) < 0) end = Max(end, other.end());
  }
  return end;
}

std::optional<RationalTime> NextStart(const Track& track, const Clip& clip, const std::set<std::string>& skip = {}) {
  std::optional<RationalTime> start;
  for (const auto& other : track.clips) {
    if (other.id == clip.id || skip.count(other.id) != 0) continue;
    if (other.timeline_start.Compare(clip.timeline_start) > 0 && (!start || other.timeline_start.Compare(*start) < 0)) start = other.timeline_start;
  }
  return start;
}

bool TrackEditable(const Track& track) { return !track.locked && !track.is_bus; }

// ----------------------------------------------------------- clearing and making room ----

struct Builder {
  const EditContext& ctx;
  std::vector<PlannedCommand> commands;
  std::vector<std::string> notes;

  [[nodiscard]] std::string NewId(const std::string& prefix) const { return ctx.new_id ? ctx.new_id(prefix) : prefix + "-" + std::to_string(commands.size()); }
  void Add(CommandType type, commands::CommandPayload payload) { commands.push_back(Planned(type, std::move(payload))); }
  void Note(std::string text) { notes.push_back(std::move(text)); }

  void Move(const VClip& c, const std::string& track, const RationalTime& to, bool propagate = false) {
    commands::MoveClipPayload payload;
    payload.id = c.id;
    payload.track_id = track;
    payload.timeline_start = to;
    payload.propagate_links = propagate;
    Add(CommandType::MoveClip, payload);
  }
  void Trim(const VClip& c, bool propagate = false) { Add(CommandType::TrimClip, TrimPayload(c, propagate)); }
  void Delete(const std::string& id, bool propagate = false) { Add(CommandType::DeleteClip, commands::DeleteClipPayload{id, propagate}); }
  std::string Split(const std::string& id, const RationalTime& at, bool propagate = false) {
    commands::SplitClipPayload payload;
    payload.id = id;
    payload.new_clip_id = NewId("clip");
    payload.at = at;
    payload.propagate_links = propagate;
    const auto right = payload.new_clip_id;
    Add(CommandType::SplitClip, payload);
    return right;
  }

  // Removes whatever is in [s, e) from a track, except the clips named in `exclude`. The pieces of clips that are left
  // after the range (a clip that started inside it, the right half of one that spanned it) are added to `after`, as they
  // stand now at `e`, for an extract to move up.
  void ClearRange(const Track& track, const RationalTime& s, const RationalTime& e, const std::set<std::string>& exclude, std::vector<VClip>* after = nullptr) {
    for (const auto& clip : track.clips) {
      if (exclude.count(clip.id) != 0) continue;
      const auto start = clip.timeline_start, end = clip.end();
      if (end.Compare(s) <= 0 || start.Compare(e) >= 0) continue;
      const bool starts_inside = start.Compare(s) >= 0;
      const bool ends_inside = end.Compare(e) <= 0;
      const auto view = View(track, clip);
      if (starts_inside && ends_inside) {
        Delete(clip.id);
        Note((clip.name.empty() ? clip.id : clip.name) + " was removed to make room");
      } else if (starts_inside) {
        Trim(view.TrimmedHead(e));
        if (after != nullptr) after->push_back(view.TrimmedHead(e));
        Note((clip.name.empty() ? clip.id : clip.name) + " was shortened at its start");
      } else if (ends_inside) {
        Trim(view.TrimmedTail(s));
        Note((clip.name.empty() ? clip.id : clip.name) + " was shortened at its end");
      } else {
        // It spans the whole range: cut at the start, then take the piece that is inside off the front of the right half.
        const auto right_id = Split(clip.id, s);
        auto halves = view.SplitAt(s, right_id);
        Trim(halves.second.TrimmedHead(e));
        if (after != nullptr) after->push_back(halves.second.TrimmedHead(e));
        Note((clip.name.empty() ? clip.id : clip.name) + " was cut to make room");
      }
    }
  }

  // Opens a gap of `length` at `at` on the tracks: clips that span `at` are cut there, and every clip at or after
  // it (other than those excluded) moves later by `length`.
  void MakeRoom(const std::vector<const Track*>& tracks, const RationalTime& at, const RationalTime& length, const std::set<std::string>& exclude) {
    for (const auto* track : tracks) {
      struct Shift {
        std::string id;
        VClip view;
        RationalTime start;
      };
      std::vector<Shift> shifts;
      for (const auto& clip : track->clips) {
        if (exclude.count(clip.id) != 0) continue;
        const auto start = clip.timeline_start, end = clip.end();
        if (start.Compare(at) < 0 && end.Compare(at) > 0) {
          const auto right_id = Split(clip.id, at);
          const auto halves = View(*track, clip).SplitAt(at, right_id);
          shifts.push_back({right_id, halves.second, at});
        } else if (start.Compare(at) >= 0) {
          shifts.push_back({clip.id, View(*track, clip), start});
        }
      }
      // Later ones first, so a clip never lands on one that has not moved yet.
      std::sort(shifts.begin(), shifts.end(), [](const Shift& a, const Shift& b) { return a.start.Compare(b.start) > 0; });
      for (const auto& shift : shifts) Move(shift.view, track->id, shift.start.Add(length));
    }
  }
};

EditPlan Finish(Builder& b, std::string label) {
  EditPlan plan;
  plan.ok = true;
  plan.label = std::move(label);
  plan.commands = std::move(b.commands);
  plan.notes = std::move(b.notes);
  return plan;
}

std::vector<const Track*> EditableTracks(const EditContext& ctx, const std::set<std::string>& only = {}) {
  std::vector<const Track*> tracks;
  for (const auto& track : ctx.sequence->tracks) {
    if (!TrackEditable(track)) continue;
    if (!only.empty() && only.count(track.id) == 0) continue;
    tracks.push_back(&track);
  }
  return tracks;
}

}  // namespace

// ------------------------------------------------------------------------ moving ----

EditPlan PlanMove(const EditContext& ctx, const std::set<std::string>& clip_ids, const std::string& primary_clip_id, const RationalTime& delta_in,
                  int track_delta, OverlapMode mode) {
  const auto& sequence = *ctx.sequence;
  const auto primary = FindClip(sequence, primary_clip_id);
  if (primary.clip == nullptr) return EditPlan::Refuse("The clip is not in the sequence");
  auto ids = ctx.linked ? WithLinked(sequence, clip_ids) : clip_ids;
  ids.insert(primary_clip_id);

  struct Move {
    Found from;
    std::string to_track;
    RationalTime new_start;
    VClip view;
  };
  Builder b{ctx, {}, {}};

  // Tracks of the primary's kind, in order, so a track offset means a row up or down.
  std::vector<const Track*> same_kind;
  for (const auto& track : sequence.tracks) {
    if (track.kind == primary.track->kind && !track.is_bus) same_kind.push_back(&track);
  }
  std::sort(same_kind.begin(), same_kind.end(), [](const Track* a, const Track* c) { return a->order < c->order; });

  auto delta = delta_in;
  // No clip may start before zero: clamp the move to what the earliest clip allows.
  for (const auto& id : ids) {
    const auto f = FindClip(sequence, id);
    if (f.clip == nullptr) return EditPlan::Refuse("A selected clip is not in the sequence");
    const auto earliest = f.clip->timeline_start.Add(delta);
    if (earliest.Compare(kZero) < 0) {
      delta = kZero.Subtract(f.clip->timeline_start);
      b.Note("The move was stopped at the start of the sequence");
    }
  }

  std::vector<Move> moves;
  for (const auto& id : ids) {
    const auto f = FindClip(sequence, id);
    if (!TrackEditable(*f.track)) return EditPlan::Refuse("The track " + f.track->id + " is locked");
    Move m{f, f.track->id, f.clip->timeline_start.Add(delta), View(*f.track, *f.clip)};
    if (track_delta != 0 && f.track->kind == primary.track->kind) {
      const auto at = std::find(same_kind.begin(), same_kind.end(), f.track);
      const auto index = static_cast<int>(at - same_kind.begin()) + track_delta;
      if (index < 0 || index >= static_cast<int>(same_kind.size())) return EditPlan::Refuse("There is no track there");
      m.to_track = same_kind[static_cast<std::size_t>(index)]->id;
      if (!TrackEditable(*same_kind[static_cast<std::size_t>(index)])) return EditPlan::Refuse("The track " + m.to_track + " is locked");
    }
    moves.push_back(std::move(m));
  }
  if (delta.Compare(kZero) == 0 && track_delta == 0) return EditPlan::Refuse("Nothing to move");

  // The moved clips must not land on each other.
  for (std::size_t i = 0; i < moves.size(); ++i) {
    for (std::size_t j = i + 1; j < moves.size(); ++j) {
      if (moves[i].to_track != moves[j].to_track) continue;
      const auto a_end = moves[i].new_start.Add(moves[i].view.duration());
      const auto b_end = moves[j].new_start.Add(moves[j].view.duration());
      if (moves[i].new_start.Compare(b_end) < 0 && moves[j].new_start.Compare(a_end) < 0) return EditPlan::Refuse("The moved clips would land on each other");
    }
  }

  // What is under the moved clips at their new places.
  const std::set<std::string> moved_ids(ids.begin(), ids.end());
  std::map<std::string, std::vector<std::pair<RationalTime, RationalTime>>> landing;
  for (const auto& m : moves) landing[m.to_track].emplace_back(m.new_start, m.new_start.Add(m.view.duration()));
  for (auto& [track_id, intervals] : landing) {
    std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& c) { return a.first.Compare(c.first) < 0; });
  }
  const auto collides = [&](const Track& track, const RationalTime& s, const RationalTime& e) {
    for (const auto& clip : track.clips) {
      if (moved_ids.count(clip.id) != 0) continue;
      if (clip.timeline_start.Compare(e) < 0 && clip.end().Compare(s) > 0) return true;
    }
    return false;
  };

  bool parking = false;
  std::vector<std::pair<std::string, std::vector<std::pair<RationalTime, RationalTime>>>> clears;
  for (const auto& [track_id, intervals] : landing) {
    const auto* track = sequence.FindTrack(track_id);
    for (const auto& [s, e] : intervals) {
      if (!collides(*track, s, e)) continue;
      if (mode == OverlapMode::Refuse) return EditPlan::Refuse("The clips would overlap other clips");
    }
  }

  if (mode == OverlapMode::Overwrite) {
    for (const auto& [track_id, intervals] : landing) {
      const auto* track = sequence.FindTrack(track_id);
      for (const auto& [s, e] : intervals) b.ClearRange(*track, s, e, moved_ids);
    }
  } else if (mode == OverlapMode::Insert) {
    // Insert: the moved clips are taken out first (parked past the end), room is made, and they are placed.
    parking = true;
    for (const auto& [track_id, intervals] : landing) {
      for (std::size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].first.Compare(intervals[i - 1].second) != 0) return EditPlan::Refuse("Insert needs the moved clips to be touching");
      }
    }
  }

  // Order the moves so none lands on a clip that has not moved yet: the way the clips are going decides who goes first.
  const bool rightward = delta.Compare(kZero) > 0;
  std::sort(moves.begin(), moves.end(), [&](const Move& a, const Move& c) {
    const std::int64_t pa = a.from.track->order, pc = c.from.track->order;
    if (track_delta != 0 && a.from.track->kind == primary.track->kind && pa != pc) return track_delta > 0 ? pa > pc : pa < pc;
    return rightward ? a.view.start.Compare(c.view.start) > 0 : a.view.start.Compare(c.view.start) < 0;
  });

  if (parking) {
    RationalTime far = kZero;
    for (const auto& track : sequence.tracks) {
      for (const auto& clip : track.clips) far = Max(far, clip.end());
    }
    far = far.Add(RationalTime(60, 1));
    for (const auto& m : moves) {
      const auto offset = m.new_start.Subtract(landing[m.to_track].front().first);
      b.Move(m.view, m.to_track, far.Add(offset));
    }
    for (const auto& [track_id, intervals] : landing) {
      const auto s = intervals.front().first, e = intervals.back().second;
      std::vector<const Track*> tracks;
      if (ctx.ripple_all_tracks) tracks = EditableTracks(ctx);
      else tracks.push_back(sequence.FindTrack(track_id));
      b.MakeRoom(tracks, s, e.Subtract(s), moved_ids);
    }
    for (const auto& m : moves) b.Move(m.view, m.to_track, m.new_start);
  } else {
    for (const auto& m : moves) b.Move(m.view, m.to_track, m.new_start);
  }
  auto plan = Finish(b, moves.size() == 1 ? "Move Clip" : "Move Clips");
  plan.result_time = primary.clip->timeline_start.Add(delta);
  return plan;
}

// -------------------------------------------------------------------------- trimming ----

namespace {

// The range of timeline shifts d that moving a clip's head by d allows: [lo, hi].
struct Range {
  RationalTime lo, hi;
};

Range HeadRange(const EditContext& ctx, const Found& f, bool normal_mode, const std::set<std::string>& group) {
  const auto& clip = *f.clip;
  const auto frame = FrameLength(ctx);
  Range r{RationalTime(-1000000, 1), clip.duration().Subtract(frame)};
  // Media: the source range must stay inside it.
  const auto length = MediaLength(ctx, clip);
  if (!clip.reversed) {
    r.lo = Max(r.lo, kZero.Subtract(clip.source_in).Divide(clip.playback_rate));
  } else if (length) {
    r.lo = Max(r.lo, kZero.Subtract(length->Subtract(clip.source_out)).Divide(clip.playback_rate));
  }
  // In Normal mode the head cannot go back into the clip before it; a ripple, roll or slide moves what is there.
  if (normal_mode) r.lo = Max(r.lo, PreviousEnd(*f.track, clip, group).Subtract(clip.timeline_start));
  return r;
}

Range TailRange(const EditContext& ctx, const Found& f, bool normal_mode, const std::set<std::string>& group) {
  const auto& clip = *f.clip;
  const auto frame = FrameLength(ctx);
  Range r{kZero.Subtract(clip.duration()).Add(frame), RationalTime(1000000, 1)};
  const auto length = MediaLength(ctx, clip);
  if (!clip.reversed) {
    if (length) r.hi = Min(r.hi, length->Subtract(clip.source_out).Divide(clip.playback_rate));
  } else {
    r.hi = Min(r.hi, clip.source_in.Divide(clip.playback_rate));
  }
  if (normal_mode) {
    if (const auto next = NextStart(*f.track, clip, group)) r.hi = Min(r.hi, next->Subtract(clip.end()));
  }
  return r;
}

Range Intersect(const Range& a, const Range& b) { return {Max(a.lo, b.lo), Min(a.hi, b.hi)}; }

}  // namespace

EditPlan PlanTrim(const EditContext& ctx, const std::string& clip_id, TrimEdge edge, const RationalTime& to, TrimMode mode) {
  const auto& sequence = *ctx.sequence;
  const auto clip = FindClip(sequence, clip_id);
  if (clip.clip == nullptr) return EditPlan::Refuse("The clip is not in the sequence");
  const auto members = Members(ctx, clip);
  std::set<std::string> group;
  for (const auto& m : members) group.insert(m.clip->id);
  for (const auto& m : members) {
    if (Ramped(*m.clip)) return EditPlan::Refuse("A clip with a speed ramp cannot be trimmed; remove the ramp or change it");
    if (!TrackEditable(*m.track)) return EditPlan::Refuse("The track " + m.track->id + " is locked");
  }
  Builder b{ctx, {}, {}};
  const bool normal = mode == TrimMode::Normal;

  // The shift of the edge asked for, then limited by every clip that moves with it.
  const auto asked = edge == TrimEdge::Head ? to.Subtract(clip.clip->timeline_start) : to.Subtract(clip.clip->end());
  Range allowed{RationalTime(-1000000, 1), RationalTime(1000000, 1)};
  for (const auto& m : members) {
    allowed = Intersect(allowed, edge == TrimEdge::Head ? HeadRange(ctx, m, normal, group) : TailRange(ctx, m, normal, group));
  }
  if (allowed.lo.Compare(allowed.hi) > 0) return EditPlan::Refuse("There is no room to trim there");
  auto d = Clamp(asked, allowed.lo, allowed.hi);
  if (d.Compare(asked) != 0) b.Note("The trim was stopped where the media or a neighbouring clip ends");

  // A ripple trim moves what follows by the change in the clip's end; it is limited by what is in the way.
  struct Later {
    const Track* track;
    const Clip* clip;
  };
  std::vector<Later> later;
  RationalTime end_change = kZero;
  if (!normal) {
    // Head ripple keeps the clip where it starts and moves its end by -d; tail ripple moves its end by d.
    end_change = edge == TrimEdge::Head ? kZero.Subtract(d) : d;
    const auto pivot = clip.clip->end();
    for (const auto& track : sequence.tracks) {
      if (track.is_bus) continue;
      const bool own = std::any_of(members.begin(), members.end(), [&](const Found& m) { return m.track == &track; });
      if (!own && !ctx.ripple_all_tracks) continue;
      if (track.locked) {
        if (!own) b.Note("The locked track " + track.id + " was not moved with the edit");
        continue;
      }
      for (const auto& other : track.clips) {
        if (group.count(other.id) != 0) continue;
        if (other.timeline_start.Compare(pivot) >= 0) later.push_back({&track, &other});
      }
    }
    if (end_change.Compare(kZero) < 0) {
      // Closing up: each track may only move as far as the gap before its first later clip allows.
      RationalTime room = RationalTime(1000000, 1);
      std::map<const Track*, RationalTime> first;
      for (const auto& l : later) {
        auto found = first.find(l.track);
        if (found == first.end() || l.clip->timeline_start.Compare(found->second) < 0) first[l.track] = l.clip->timeline_start;
      }
      for (const auto& [track, first_start] : first) {
        RationalTime before = kZero;
        for (const auto& other : track->clips) {
          if (other.timeline_start.Compare(first_start) < 0) {
            const auto other_end = group.count(other.id) != 0 ? other.end().Add(end_change) : other.end();
            before = Max(before, other_end);
          }
        }
        room = Min(room, first_start.Subtract(before));
      }
      const auto closing = kZero.Subtract(end_change);
      if (closing.Compare(room) > 0) {
        b.Note("The ripple was stopped where a clip on another track is in the way");
        end_change = kZero.Subtract(room);
        d = edge == TrimEdge::Head ? room : kZero.Subtract(room);
      }
    }
  }
  if (d.Compare(kZero) == 0) return EditPlan::Refuse("Nothing to trim");

  const auto trimmed = [&](const Found& f) {
    const auto view = View(*f.track, *f.clip);
    if (edge == TrimEdge::Head) return normal ? view.TrimmedHead(f.clip->timeline_start.Add(d)) : [&] {
      // Ripple head: the clip stays where it starts, its source in moves, and it gets shorter or longer.
      auto moved = view.TrimmedHead(view.start.Add(d));
      moved.start = view.start;
      return moved;
    }();
    return view.TrimmedTail(view.end().Add(d));
  };

  const auto push_later = [&] {
    std::sort(later.begin(), later.end(), [&](const Later& a, const Later& c) {
      return end_change.Compare(kZero) > 0 ? a.clip->timeline_start.Compare(c.clip->timeline_start) > 0
                                           : a.clip->timeline_start.Compare(c.clip->timeline_start) < 0;
    });
    for (const auto& l : later) b.Move(View(*l.track, *l.clip), l.track->id, l.clip->timeline_start.Add(end_change));
  };
  // Growing: make room first. Shrinking: trim first, then close up.
  if (!normal && end_change.Compare(kZero) > 0) push_later();
  b.Trim(trimmed(clip), ctx.linked);
  if (!normal && end_change.Compare(kZero) < 0) push_later();

  auto plan = Finish(b, std::string(normal ? "Trim " : "Ripple Trim ") + (edge == TrimEdge::Head ? "Start" : "End"));
  plan.result_time = edge == TrimEdge::Head ? clip.clip->timeline_start.Add(d) : clip.clip->end().Add(d);
  return plan;
}

EditPlan PlanRoll(const EditContext& ctx, const std::string& before_id, const std::string& after_id, const RationalTime& to) {
  const auto& sequence = *ctx.sequence;
  const auto a = FindClip(sequence, before_id);
  const auto c = FindClip(sequence, after_id);
  if (a.clip == nullptr || c.clip == nullptr) return EditPlan::Refuse("The clips are not in the sequence");
  if (a.track != c.track || a.clip->end().Compare(c.clip->timeline_start) != 0) return EditPlan::Refuse("A rolling edit needs two clips that touch");
  if (Ramped(*a.clip) || Ramped(*c.clip)) return EditPlan::Refuse("A clip with a speed ramp cannot be trimmed");
  if (!TrackEditable(*a.track)) return EditPlan::Refuse("The track " + a.track->id + " is locked");
  Builder b{ctx, {}, {}};
  const std::set<std::string> both{before_id, after_id};
  const auto tail = TailRange(ctx, a, false, both);
  const auto head = HeadRange(ctx, c, false, both);
  const auto allowed = Intersect(tail, head);
  const auto asked = to.Subtract(c.clip->timeline_start);
  const auto d = Clamp(asked, allowed.lo, allowed.hi);
  if (d.Compare(asked) != 0) b.Note("The roll was stopped where the media ends");
  if (d.Compare(kZero) == 0) return EditPlan::Refuse("Nothing to roll");
  const auto first = View(*a.track, *a.clip).TrimmedTail(a.clip->end().Add(d));
  const auto second = View(*c.track, *c.clip).TrimmedHead(c.clip->timeline_start.Add(d));
  // Whichever clip gets shorter goes first, so the other has room to grow.
  if (d.Compare(kZero) < 0) {
    b.Trim(first, ctx.linked);
    b.Trim(second, ctx.linked);
  } else {
    b.Trim(second, ctx.linked);
    b.Trim(first, ctx.linked);
  }
  auto plan = Finish(b, "Rolling Edit");
  plan.result_time = c.clip->timeline_start.Add(d);
  return plan;
}

EditPlan PlanSlip(const EditContext& ctx, const std::string& clip_id, const RationalTime& delta) {
  const auto clip = FindClip(*ctx.sequence, clip_id);
  if (clip.clip == nullptr) return EditPlan::Refuse("The clip is not in the sequence");
  if (Ramped(*clip.clip)) return EditPlan::Refuse("A clip with a speed ramp cannot be slipped");
  if (!TrackEditable(*clip.track)) return EditPlan::Refuse("The track " + clip.track->id + " is locked");
  if (clip.clip->source_kind == model::SourceKind::Adjustment) return EditPlan::Refuse("There is no media to slip");
  Builder b{ctx, {}, {}};
  auto shift = delta;
  const auto length = MediaLength(ctx, *clip.clip);
  const auto lo = kZero.Subtract(clip.clip->source_in);
  RationalTime hi = RationalTime(1000000, 1);
  if (length) hi = length->Subtract(clip.clip->source_out);
  const auto clamped = Clamp(shift, lo, Max(lo, hi));
  if (clamped.Compare(shift) != 0) b.Note("The slip was stopped where the media ends");
  shift = clamped;
  if (shift.Compare(kZero) == 0) return EditPlan::Refuse("Nothing to slip");
  auto view = View(*clip.track, *clip.clip);
  view.in = view.in.Add(shift);
  view.out = view.out.Add(shift);
  b.Trim(view, false);  // a slip never moves linked clips
  return Finish(b, "Slip");
}

EditPlan PlanSlide(const EditContext& ctx, const std::string& clip_id, const RationalTime& delta) {
  const auto& sequence = *ctx.sequence;
  const auto clip = FindClip(sequence, clip_id);
  if (clip.clip == nullptr) return EditPlan::Refuse("The clip is not in the sequence");
  if (!TrackEditable(*clip.track)) return EditPlan::Refuse("The track " + clip.track->id + " is locked");
  const Clip* before = nullptr;
  const Clip* after = nullptr;
  for (const auto& other : clip.track->clips) {
    if (other.end().Compare(clip.clip->timeline_start) == 0) before = &other;
    if (other.timeline_start.Compare(clip.clip->end()) == 0) after = &other;
  }
  if (before == nullptr || after == nullptr) return EditPlan::Refuse("A slide needs a clip touching each side");
  if (Ramped(*before) || Ramped(*after)) return EditPlan::Refuse("A clip with a speed ramp cannot be trimmed");
  const Found a{clip.track, before}, c{clip.track, after};
  const std::set<std::string> three{before->id, clip_id, after->id};
  Builder b{ctx, {}, {}};
  const auto allowed = Intersect(TailRange(ctx, a, false, three), HeadRange(ctx, c, false, three));
  const auto d = Clamp(delta, allowed.lo, allowed.hi);
  if (d.Compare(delta) != 0) b.Note("The slide was stopped where a neighbour or its media ends");
  if (d.Compare(kZero) == 0) return EditPlan::Refuse("Nothing to slide");
  const auto first = View(*a.track, *a.clip).TrimmedTail(before->end().Add(d));
  const auto last = View(*c.track, *c.clip).TrimmedHead(after->timeline_start.Add(d));
  const auto self = View(*clip.track, *clip.clip);
  if (d.Compare(kZero) > 0) {
    b.Trim(last, ctx.linked);
    b.Move(self, clip.track->id, self.start.Add(d), ctx.linked);
    b.Trim(first, ctx.linked);
  } else {
    b.Trim(first, ctx.linked);
    b.Move(self, clip.track->id, self.start.Add(d), ctx.linked);
    b.Trim(last, ctx.linked);
  }
  auto plan = Finish(b, "Slide");
  plan.result_time = clip.clip->timeline_start.Add(d);
  return plan;
}

// --------------------------------------------------------------------- cutting, removing ----

EditPlan PlanSplit(const EditContext& ctx, const RationalTime& at, const std::set<std::string>& track_ids) {
  const auto& sequence = *ctx.sequence;
  Builder b{ctx, {}, {}};
  std::set<std::string> handled_groups;
  int cuts = 0;
  for (const auto& track : sequence.tracks) {
    if (track.is_bus || (!track_ids.empty() && track_ids.count(track.id) == 0)) continue;
    for (const auto& clip : track.clips) {
      if (clip.timeline_start.Compare(at) >= 0 || clip.end().Compare(at) <= 0) continue;
      if (track.locked) {
        b.Note("The locked track " + track.id + " was not cut");
        continue;
      }
      if (ctx.linked && !clip.linked_group.empty() && !handled_groups.insert(clip.linked_group).second) continue;
      (void)b.Split(clip.id, at, ctx.linked);
      ++cuts;
    }
  }
  if (cuts == 0) return EditPlan::Refuse("There is no clip under the playhead to cut");
  auto plan = Finish(b, "Add Edit");
  plan.result_time = at;
  return plan;
}

EditPlan PlanSplitClips(const EditContext& ctx, const RationalTime& at, const std::set<std::string>& clip_ids) {
  Builder b{ctx, {}, {}};
  std::set<std::string> handled_groups;
  int cuts = 0;
  for (const auto& track : ctx.sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (clip_ids.count(clip.id) == 0) continue;
      if (clip.timeline_start.Compare(at) >= 0 || clip.end().Compare(at) <= 0) continue;
      if (track.locked) return EditPlan::Refuse("The track " + track.id + " is locked");
      if (ctx.linked && !clip.linked_group.empty() && !handled_groups.insert(clip.linked_group).second) continue;
      (void)b.Split(clip.id, at, ctx.linked);
      ++cuts;
    }
  }
  if (cuts == 0) return EditPlan::Refuse("The selected clips do not reach the cut");
  return Finish(b, "Add Edit");
}

EditPlan PlanDelete(const EditContext& ctx, const std::set<std::string>& clip_ids, bool ripple) {
  const auto& sequence = *ctx.sequence;
  const auto ids = ctx.linked ? WithLinked(sequence, clip_ids) : clip_ids;
  if (ids.empty()) return EditPlan::Refuse("Nothing is selected");
  struct Target {
    Found f;
  };
  std::vector<Found> targets;
  for (const auto& id : ids) {
    const auto f = FindClip(sequence, id);
    if (f.clip == nullptr) return EditPlan::Refuse("A selected clip is not in the sequence");
    if (!TrackEditable(*f.track)) return EditPlan::Refuse("The track " + f.track->id + " is locked");
    targets.push_back(f);
  }
  Builder b{ctx, {}, {}};
  if (!ripple) {
    for (const auto& f : targets) b.Delete(f.clip->id, false);
    return Finish(b, targets.size() == 1 ? "Clear" : "Clear Clips");
  }
  // Ripple: the last first, so what is earlier has not moved. A linked group is removed once, through whichever of
  // its members comes first; the store closes the gap on every member's track.
  std::sort(targets.begin(), targets.end(), [](const Found& a, const Found& c) { return a.clip->timeline_start.Compare(c.clip->timeline_start) > 0; });
  std::set<std::string> done;
  for (const auto& f : targets) {
    if (done.count(f.clip->id) != 0) continue;
    done.insert(f.clip->id);
    if (ctx.linked && !f.clip->linked_group.empty()) {
      for (const auto& other : targets) {
        if (other.clip->linked_group == f.clip->linked_group) done.insert(other.clip->id);
      }
    }
    b.Add(CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{f.clip->id, ctx.linked});
  }
  return Finish(b, targets.size() == 1 ? "Ripple Delete" : "Ripple Delete Clips");
}

EditPlan PlanLift(const EditContext& ctx, const RationalTime& in, const RationalTime& out, const std::set<std::string>& track_ids) {
  if (out.Compare(in) <= 0) return EditPlan::Refuse("The marked range is empty");
  Builder b{ctx, {}, {}};
  for (const auto* track : EditableTracks(ctx, track_ids)) b.ClearRange(*track, in, out, {});
  if (b.commands.empty()) return EditPlan::Refuse("There is nothing in the marked range");
  return Finish(b, "Lift");
}

EditPlan PlanExtract(const EditContext& ctx, const RationalTime& in, const RationalTime& out, const std::set<std::string>& track_ids) {
  if (out.Compare(in) <= 0) return EditPlan::Refuse("The marked range is empty");
  Builder b{ctx, {}, {}};
  const auto length = out.Subtract(in);
  const auto tracks = EditableTracks(ctx, track_ids);
  std::vector<std::vector<VClip>> remainders(tracks.size());
  for (std::size_t i = 0; i < tracks.size(); ++i) b.ClearRange(*tracks[i], in, out, {}, &remainders[i]);
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    const auto* track = tracks[i];
    // What is left of the clips the range cut into comes up first, then every clip that began at or after the range's end.
    for (const auto& piece : remainders[i]) b.Move(piece, track->id, piece.start.Subtract(length));
    // In time order, so a clip moving up never lands on one that has not moved yet.
    std::vector<const Clip*> later;
    for (const auto& clip : track->clips) {
      if (clip.timeline_start.Compare(out) >= 0) later.push_back(&clip);
    }
    std::sort(later.begin(), later.end(), [](const Clip* a, const Clip* c) { return a->timeline_start.Compare(c->timeline_start) < 0; });
    for (const auto* clip : later) b.Move(View(*track, *clip), track->id, clip->timeline_start.Subtract(length));
  }
  if (b.commands.empty()) return EditPlan::Refuse("There is nothing in the marked range");
  return Finish(b, "Extract");
}

EditPlan PlanRemoveRanges(const EditContext& ctx, std::vector<TimeRange> ranges, bool ripple, const std::string& label) {
  ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [](const TimeRange& r) { return r.out.Compare(r.in) <= 0; }), ranges.end());
  if (ranges.empty()) return EditPlan::Refuse("There is nothing to remove");
  std::sort(ranges.begin(), ranges.end(), [](const TimeRange& x, const TimeRange& y) { return x.in.Compare(y.in) < 0; });
  std::vector<TimeRange> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.in.Compare(merged.back().out) <= 0) {
      if (range.out.Compare(merged.back().out) > 0) merged.back().out = range.out;
    } else {
      merged.push_back(range);
    }
  }
  // removed_before[i] is the length of the first i ranges: how far a ripple moves what comes after them.
  std::vector<RationalTime> removed_before(merged.size() + 1, kZero);
  for (std::size_t i = 0; i < merged.size(); ++i) removed_before[i + 1] = removed_before[i].Add(merged[i].out.Subtract(merged[i].in));
  const auto shift_at = [&](const RationalTime& at) {
    // The ranges that end at or before `at`.
    const auto it = std::partition_point(merged.begin(), merged.end(), [&](const TimeRange& r) { return r.out.Compare(at) <= 0; });
    return removed_before[static_cast<std::size_t>(std::distance(merged.begin(), it))];
  };

  // Each clip is planned once, whatever number of ranges cross it: what survives of it is cut out with splits and trims
  // (the original clip keeps its first surviving piece), and on a ripple every surviving piece is moved to its place once.
  // That is a plan of work in proportion to the clips and the ranges, not their product.
  Builder b{ctx, {}, {}};
  int removed_whole = 0;
  for (const auto* track : EditableTracks(ctx)) {
    struct Piece {
      VClip clip;
      RationalTime to;
    };
    std::vector<Piece> survivors;
    std::vector<const Clip*> clips;
    for (const auto& clip : track->clips) clips.push_back(&clip);
    std::sort(clips.begin(), clips.end(), [](const Clip* x, const Clip* y) { return x->timeline_start.Compare(y->timeline_start) < 0; });
    for (const auto* clip : clips) {
      const auto start = clip->timeline_start, end = clip->end();
      auto it = std::partition_point(merged.begin(), merged.end(), [&](const TimeRange& r) { return r.out.Compare(start) <= 0; });
      std::vector<std::pair<RationalTime, RationalTime>> segments;
      auto cursor = start;
      bool touched = false;
      for (; it != merged.end() && it->in.Compare(end) < 0; ++it) {
        touched = true;
        if (it->in.Compare(cursor) > 0) segments.emplace_back(cursor, it->in);
        cursor = Max(cursor, it->out);
      }
      if (!touched) {
        if (ripple && shift_at(start).Compare(kZero) > 0) survivors.push_back({View(*track, *clip), start.Subtract(shift_at(start))});
        continue;
      }
      if (cursor.Compare(end) < 0) segments.emplace_back(cursor, end);
      if (segments.empty()) {
        b.Delete(clip->id);
        ++removed_whole;
        continue;
      }
      const auto original = View(*track, *clip);
      auto current = original;
      if (segments.front().first.Compare(start) > 0) current = current.TrimmedHead(segments.front().first);
      if (segments.back().second.Compare(end) < 0) current = current.TrimmedTail(segments.back().second);
      if (current.start.Compare(original.start) != 0 || current.in.Compare(original.in) != 0 || current.out.Compare(original.out) != 0) b.Trim(current);
      for (std::size_t k = 0; k < segments.size(); ++k) {
        if (k + 1 < segments.size()) {
          // The gap after this piece: cut at its start, and take its length off the front of the right half.
          const auto gap_in = segments[k].second, gap_out = segments[k + 1].first;
          const auto right_id = b.Split(current.id, gap_in);
          auto halves = current.SplitAt(gap_in, right_id);
          survivors.push_back({halves.first, segments[k].first.Subtract(shift_at(segments[k].first))});
          current = halves.second.TrimmedHead(gap_out);
          b.Trim(current);
        } else {
          survivors.push_back({current, segments[k].first.Subtract(shift_at(segments[k].first))});
        }
      }
    }
    if (ripple) {
      // In time order, so a piece never lands on one that has not moved yet.
      for (const auto& piece : survivors) {
        if (piece.to.Compare(piece.clip.start) != 0) b.Move(piece.clip, track->id, piece.to);
      }
    }
  }
  if (b.commands.empty()) return EditPlan::Refuse("There is nothing in the ranges to remove");
  if (removed_whole > 0) b.Note(std::to_string(removed_whole) + (removed_whole == 1 ? " clip was removed whole" : " clips were removed whole"));
  auto plan = Finish(b, label);
  plan.result_time = merged.front().in;
  return plan;
}

// -------------------------------------------------------------------- placing clips ----

std::optional<ClipSpec> SpecOf(const timeline::Sequence& sequence, const std::string& clip_id, const RationalTime& relative_to) {
  const auto f = FindClip(sequence, clip_id);
  if (f.clip == nullptr) return std::nullopt;
  ClipSpec spec;
  spec.source_kind = f.clip->source_kind;
  if (f.clip->source_kind == model::SourceKind::Media) spec.media_id = f.clip->source_id;
  if (f.clip->source_kind == model::SourceKind::Sequence) spec.nested_sequence_id = f.clip->source_id;
  spec.source_in = f.clip->source_in;
  spec.source_out = f.clip->source_out;
  spec.playback_rate = f.clip->playback_rate;
  spec.reversed = f.clip->reversed;
  spec.maintain_pitch = f.clip->maintain_pitch;
  spec.audio_role = f.clip->audio_role;
  spec.track_id = f.track->id;
  spec.offset = f.clip->timeline_start.Subtract(relative_to);
  spec.name = f.clip->name;
  spec.link_key = f.clip->linked_group;
  spec.effects = f.clip->effects;
  return spec;
}

EditPlan PlanPlace(const EditContext& ctx, const std::vector<ClipSpec>& specs, const RationalTime& at, OverlapMode mode, const std::string& label) {
  const auto& sequence = *ctx.sequence;
  if (specs.empty()) return EditPlan::Refuse("There is nothing to place");
  Builder b{ctx, {}, {}};
  RationalTime lowest = specs.front().offset, highest = specs.front().offset.Add(specs.front().duration());
  for (const auto& spec : specs) {
    const auto* track = sequence.FindTrack(spec.track_id);
    if (track == nullptr || track->is_bus) return EditPlan::Refuse("There is no track " + spec.track_id);
    if (track->locked) return EditPlan::Refuse("The track " + spec.track_id + " is locked");
    if (spec.duration().Compare(kZero) <= 0) return EditPlan::Refuse("A clip to place has no length");
    if (at.Add(spec.offset).Compare(kZero) < 0) return EditPlan::Refuse("The clips would start before the sequence");
    if (spec.source_kind == model::SourceKind::Media) {
      if (spec.media_id.empty()) return EditPlan::Refuse("A clip to place names no media");
      if (ctx.media_duration) {
        if (const auto length = ctx.media_duration(spec.media_id)) {
          if (spec.source_in.Compare(kZero) < 0 || spec.source_out.Compare(*length) > 0) return EditPlan::Refuse("The range is outside the media");
        }
      }
    }
    lowest = Min(lowest, spec.offset);
    highest = Max(highest, spec.offset.Add(spec.duration()));
  }

  // Making room, per the mode.
  std::map<std::string, std::vector<std::pair<RationalTime, RationalTime>>> per_track;
  for (const auto& spec : specs) per_track[spec.track_id].emplace_back(at.Add(spec.offset), at.Add(spec.offset).Add(spec.duration()));
  if (mode == OverlapMode::Insert) {
    std::vector<const Track*> tracks;
    if (ctx.ripple_all_tracks) tracks = EditableTracks(ctx);
    else {
      for (const auto& [id, intervals] : per_track) tracks.push_back(sequence.FindTrack(id));
    }
    b.MakeRoom(tracks, at.Add(lowest), highest.Subtract(lowest), {});
  } else {
    for (const auto& [id, intervals] : per_track) {
      const auto* track = sequence.FindTrack(id);
      for (const auto& [s, e] : intervals) {
        if (mode == OverlapMode::Refuse) {
          for (const auto& clip : track->clips) {
            if (clip.timeline_start.Compare(e) < 0 && clip.end().Compare(s) > 0) return EditPlan::Refuse("The clips would overlap other clips");
          }
        } else {
          b.ClearRange(*track, s, e, {});
        }
      }
    }
  }

  std::map<std::string, std::string> links;
  for (const auto& spec : specs) {
    commands::InsertClipPayload clip;
    clip.id = b.NewId("clip");
    clip.track_id = spec.track_id;
    clip.source_kind = spec.source_kind;
    if (spec.source_kind == model::SourceKind::Media) clip.media_id = spec.media_id;
    if (spec.source_kind == model::SourceKind::Sequence) clip.nested_sequence_id = spec.nested_sequence_id;
    clip.source_in = spec.source_in;
    clip.source_out = spec.source_out;
    clip.timeline_start = at.Add(spec.offset);
    clip.playback_rate = spec.playback_rate;
    clip.reversed = spec.reversed;
    clip.maintain_pitch = spec.maintain_pitch;
    clip.audio_role = spec.audio_role;
    clip.name = spec.name;
    if (!spec.link_key.empty()) {
      auto found = links.find(spec.link_key);
      if (found == links.end()) found = links.emplace(spec.link_key, b.NewId("link")).first;
      clip.linked_group = found->second;
    }
    const auto clip_id = clip.id;
    b.Add(CommandType::InsertClip, clip);
    for (const auto& effect : spec.effects) {
      commands::AddEffectPayload add;
      add.id = b.NewId("fx");
      add.owner_kind = model::EffectOwner::Clip;
      add.owner_id = clip_id;
      add.effect_type = effect.effect_type;
      add.order = effect.order;
      add.intrinsic = effect.intrinsic;
      add.preset_name = effect.preset_name;
      std::vector<commands::SetKeyframePayload> keys;
      for (const auto& parameter : effect.parameters) {
        commands::EffectParameter init;
        init.id = add.id + ":" + parameter.name;
        init.name = parameter.name;
        init.value = parameter.value.constant();
        add.parameters.push_back(init);
        for (const auto& key : parameter.value.keyframes()) keys.push_back({init.id, key});
      }
      b.Add(CommandType::AddEffect, add);
      for (const auto& key : keys) b.Add(CommandType::SetKeyframe, key);
      if (!effect.enabled) b.Add(CommandType::SetEffectEnabled, commands::SetEffectEnabledPayload{add.id, false});
    }
  }
  auto plan = Finish(b, label);
  plan.result_time = at.Add(lowest);
  return plan;
}

EditPlan PlanInsertEdit(const EditContext& ctx, const SourceRange& source, const RationalTime& at, const std::string& video_track_id,
                        const std::string& audio_track_id, OverlapMode mode) {
  std::vector<ClipSpec> specs;
  const auto make = [&](const std::string& track) {
    ClipSpec spec;
    spec.media_id = source.media_id;
    spec.source_in = source.in;
    spec.source_out = source.out;
    spec.track_id = track;
    spec.name = source.name;
    return spec;
  };
  if (source.has_video && !video_track_id.empty()) specs.push_back(make(video_track_id));
  if (source.has_audio && !audio_track_id.empty()) specs.push_back(make(audio_track_id));
  if (specs.size() == 2) specs[0].link_key = specs[1].link_key = "av";
  if (specs.empty()) return EditPlan::Refuse("There is no track to place the clip on");
  return PlanPlace(ctx, specs, at, mode, mode == OverlapMode::Insert ? "Insert" : "Overwrite");
}

ThreePointResult ResolveThreePoint(const ThreePointInput& input) {
  ThreePointResult result;
  const auto refuse = [&](const std::string& why) {
    result.ok = false;
    result.refusal = why;
    return result;
  };
  std::optional<RationalTime> sequence_length;
  if (input.sequence_in && input.sequence_out) {
    if (input.sequence_out->Compare(*input.sequence_in) <= 0) return refuse("The sequence in and out points are the wrong way round");
    sequence_length = input.sequence_out->Subtract(*input.sequence_in);
  }
  auto in = input.source_in;
  auto out = input.source_out;
  if (in && out && out->Compare(*in) <= 0) return refuse("The source in and out points are the wrong way round");
  if (!in && !out) {
    in = kZero;
    out = sequence_length ? Min(input.source_duration, *sequence_length) : input.source_duration;
  } else if (in && !out) {
    out = sequence_length ? Min(input.source_duration, in->Add(*sequence_length)) : input.source_duration;
  } else if (!in && out) {
    in = sequence_length ? Max(kZero, out->Subtract(*sequence_length)) : kZero;
  } else if (sequence_length) {
    // All four: the source is trimmed to the space it is to fill.
    out = Min(input.source_duration, in->Add(*sequence_length));
  }
  if (in->Compare(kZero) < 0 || out->Compare(input.source_duration) > 0 || out->Compare(*in) <= 0) return refuse("The source range is outside the media");
  const auto length = out->Subtract(*in);
  RationalTime at = input.playhead;
  if (input.sequence_in) at = *input.sequence_in;
  else if (input.sequence_out) at = input.sequence_out->Subtract(length);
  if (at.Compare(kZero) < 0) return refuse("The edit would start before the sequence");
  result.ok = true;
  result.source_in = *in;
  result.source_out = *out;
  result.at = at;
  return result;
}

// --------------------------------------------------------------------- other edits ----

EditPlan PlanSetEnabled(const EditContext& ctx, const std::set<std::string>& clip_ids, bool enabled) {
  if (clip_ids.empty()) return EditPlan::Refuse("Nothing is selected");
  Builder b{ctx, {}, {}};
  const auto ids = ctx.linked ? WithLinked(*ctx.sequence, clip_ids) : clip_ids;
  for (const auto& id : ids) {
    const auto f = FindClip(*ctx.sequence, id);
    if (f.clip == nullptr) return EditPlan::Refuse("A selected clip is not in the sequence");
    if (f.clip->enabled == enabled) continue;
    if (!TrackEditable(*f.track)) return EditPlan::Refuse("The track " + f.track->id + " is locked");
    b.Add(CommandType::SetClipEnabled, commands::SetClipEnabledPayload{id, enabled, false});
  }
  if (b.commands.empty()) return EditPlan::Refuse(enabled ? "The clips are already enabled" : "The clips are already disabled");
  return Finish(b, enabled ? "Enable Clips" : "Disable Clips");
}

EditPlan PlanLink(const EditContext& ctx, const std::set<std::string>& clip_ids) {
  if (clip_ids.size() < 2) return EditPlan::Refuse("Select at least two clips to link");
  Builder b{ctx, {}, {}};
  commands::LinkClipsPayload payload;
  payload.group_id = b.NewId("link");
  for (const auto& id : clip_ids) {
    if (FindClip(*ctx.sequence, id).clip == nullptr) return EditPlan::Refuse("A selected clip is not in the sequence");
    payload.clip_ids.push_back(id);
  }
  b.Add(CommandType::LinkClips, payload);
  return Finish(b, "Link Clips");
}

EditPlan PlanUnlink(const EditContext& ctx, const std::set<std::string>& clip_ids) {
  Builder b{ctx, {}, {}};
  commands::UnlinkClipsPayload payload;
  for (const auto& id : WithLinked(*ctx.sequence, clip_ids)) {
    const auto f = FindClip(*ctx.sequence, id);
    if (f.clip != nullptr && !f.clip->linked_group.empty()) payload.clip_ids.push_back(id);
  }
  if (payload.clip_ids.empty()) return EditPlan::Refuse("None of the selected clips is linked");
  b.Add(CommandType::UnlinkClips, payload);
  return Finish(b, "Unlink Clips");
}

std::vector<commands::CommandEnvelope> ToEnvelopes(const EditPlan& plan, const EnvelopeFactory& factory, std::int64_t first_serial) {
  std::vector<commands::CommandEnvelope> envelopes;
  std::int64_t serial = first_serial;
  for (const auto& planned : plan.commands) {
    commands::CommandEnvelope command;
    command.command_id = factory.key_prefix + "-" + std::to_string(serial);
    command.idempotency_key = factory.key_prefix + "-key-" + std::to_string(serial);
    command.project_id = factory.project_id;
    command.author_id = factory.author_id;
    command.timestamp_utc = factory.timestamp_utc;
    command.base_revision = factory.base_revision;
    command.type = planned.type;
    command.payload = planned.payload;
    envelopes.push_back(std::move(command));
    ++serial;
  }
  return envelopes;
}

}  // namespace cutline::ui

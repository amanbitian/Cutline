#include "ui/TimelineView.h"

#include <algorithm>
#include <cmath>

namespace cutline::ui {
namespace {

double Seconds(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()); }

double FrameSeconds(time::FrameRate rate) {
  return rate.numerator > 0 ? static_cast<double>(rate.denominator) / static_cast<double>(rate.numerator) : 1.0 / 25.0;
}

RationalTime FrameTime(double seconds, time::FrameRate rate) {
  const auto frames = static_cast<std::int64_t>(std::llround(seconds / FrameSeconds(rate)));
  return RationalTime::FromFrames(std::max<std::int64_t>(frames, 0), rate);
}

bool IsRetimed(const timeline::Clip& clip) {
  if (clip.reversed || clip.playback_rate.Compare(RationalTime(1, 1)) != 0) return true;
  return std::any_of(clip.effects.begin(), clip.effects.end(), [](const timeline::Effect& e) { return e.effect_type == "time_remap"; });
}

bool IsGraphic(const timeline::Clip& clip) {
  return std::any_of(clip.effects.begin(), clip.effects.end(),
                     [](const timeline::Effect& e) { return e.effect_type == "graphic" || e.effect_type == "motion_graphics_template"; });
}

}  // namespace

// ----------------------------------------------------------------- viewport ----

double TimelineViewport::TimeToX(const RationalTime& t) const { return SecondsToX(Seconds(t)); }

RationalTime TimelineViewport::XToTime(double x, time::FrameRate rate) const { return FrameTime(XToSeconds(x), rate); }

void TimelineViewport::ZoomAt(double anchor_x, double factor) {
  if (!(factor > 0.0) || !std::isfinite(factor)) return;
  const double anchor_seconds = XToSeconds(anchor_x);
  pixels_per_second = std::clamp(pixels_per_second * factor, kMinPixelsPerSecond, kMaxPixelsPerSecond);
  scroll_seconds = std::max(0.0, anchor_seconds - anchor_x / pixels_per_second);
}

void TimelineViewport::ScrollBySeconds(double seconds) { scroll_seconds = std::max(0.0, scroll_seconds + seconds); }

void TimelineViewport::FitRange(double start_seconds, double end_seconds, double margin_px) {
  const double span = std::max(end_seconds - start_seconds, 0.04);
  const double usable = std::max(width_px - 2.0 * margin_px, 10.0);
  pixels_per_second = std::clamp(usable / span, kMinPixelsPerSecond, kMaxPixelsPerSecond);
  scroll_seconds = std::max(0.0, start_seconds - margin_px / pixels_per_second);
}

bool TimelineViewport::EnsureVisible(double seconds, double margin_px) {
  const double margin = margin_px / pixels_per_second;
  const double before = scroll_seconds;
  if (seconds < scroll_seconds + margin) {
    scroll_seconds = std::max(0.0, seconds - margin);
  } else if (seconds > EndSeconds() - margin) {
    scroll_seconds = std::max(0.0, seconds - (VisibleSeconds() - margin));
  }
  return scroll_seconds != before;
}

bool TimelineViewport::FollowPlayhead(double seconds) {
  const double margin = 40.0 / pixels_per_second;
  if (seconds >= scroll_seconds && seconds <= EndSeconds() - margin) return false;
  scroll_seconds = std::max(0.0, seconds - margin);
  return true;
}

std::vector<RulerTick> BuildRuler(const TimelineViewport& viewport, time::FrameRate rate, bool drop_frame, double min_major_px) {
  std::vector<RulerTick> ticks;
  if (viewport.pixels_per_second <= 0.0 || viewport.width_px <= 0.0) return ticks;
  const double frame = FrameSeconds(rate);
  std::vector<double> steps;
  for (const double frames : {1.0, 2.0, 5.0, 10.0}) steps.push_back(frames * frame);
  for (const double seconds : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0, 7200.0, 14400.0}) {
    if (seconds > 10.0 * frame) steps.push_back(seconds);
  }
  double step = steps.back();
  for (const auto candidate : steps) {
    if (candidate * viewport.pixels_per_second >= min_major_px) {
      step = candidate;
      break;
    }
  }
  int subdivisions = 1;
  for (const int divisor : {10, 5, 4, 2}) {
    const double minor = step / divisor;
    if (minor * viewport.pixels_per_second < 10.0 || minor < frame * (1.0 - 1e-9)) continue;
    if (step < 1.0) {
      const double in_frames = minor / frame;
      if (std::abs(in_frames - std::round(in_frames)) > 1e-6) continue;
    }
    subdivisions = divisor;
    break;
  }
  const double minor_step = step / subdivisions;
  const auto first = static_cast<std::int64_t>(std::floor(viewport.scroll_seconds / minor_step));
  const auto last = static_cast<std::int64_t>(std::ceil(viewport.EndSeconds() / minor_step));
  for (std::int64_t k = std::max<std::int64_t>(first, 0); k <= last; ++k) {
    RulerTick tick;
    tick.seconds = static_cast<double>(k) * minor_step;
    tick.x = viewport.SecondsToX(tick.seconds);
    tick.major = k % subdivisions == 0;
    if (tick.major) tick.label = FrameTime(tick.seconds, rate).FormatTimecode(rate, drop_frame);
    ticks.push_back(std::move(tick));
  }
  return ticks;
}

// -------------------------------------------------------------------- layout ----

TimelineLayout BuildTimelineLayout(const timeline::Sequence& sequence, const TimelineViewport& viewport, const LayoutOptions& options) {
  TimelineLayout layout;
  std::vector<const timeline::Track*> video, audio;
  for (const auto& track : sequence.tracks) {
    if (track.is_bus) continue;
    (track.kind == model::TrackKind::Video ? video : audio).push_back(&track);
  }
  std::sort(video.begin(), video.end(), [](const auto* a, const auto* b) { return a->order > b->order; });
  std::sort(audio.begin(), audio.end(), [](const auto* a, const auto* b) { return a->order < b->order; });
  double y = 0.0;
  const auto place = [&](const timeline::Track& track) {
    TrackRow row;
    row.id = track.id;
    row.kind = track.kind;
    row.name = track.name;
    row.y = y;
    row.height = track.kind == model::TrackKind::Video ? options.video_track_height : options.audio_track_height;
    row.locked = track.locked;
    row.muted = track.muted;
    row.solo = track.solo;
    row.order = track.order;
    const bool row_visible = row.y + row.height >= viewport.vertical_scroll_px &&
                             row.y <= viewport.VerticalEndPx();
    if (row_visible) layout.tracks.push_back(row);
    const double lo = viewport.scroll_seconds, hi = viewport.EndSeconds();
    const auto lo_ticks = RationalTime(static_cast<std::int64_t>(std::floor(lo * time::kTicksPerSecond)),
                                       time::kTicksPerSecond).ToTicks();
    auto clip = std::lower_bound(track.clips.begin(), track.clips.end(), lo_ticks,
                                 [](const timeline::Clip& candidate, std::int64_t ticks) {
                                   return candidate.end_ticks <= ticks;
                                 });
    for (; row_visible && clip != track.clips.end(); ++clip) {
      if (clip->start_ticks > RationalTime(static_cast<std::int64_t>(std::ceil(hi * time::kTicksPerSecond)),
                                           time::kTicksPerSecond).ToTicks()) break;
      const double start = Seconds(clip->timeline_start), end = Seconds(clip->end());
      if (end < lo || start > hi) continue;
      ClipBox box;
      box.clip_id = clip->id;
      box.track_id = track.id;
      box.kind = track.kind;
      box.source_kind = clip->source_kind;
      box.x = viewport.SecondsToX(start);
      box.width = std::max((end - start) * viewport.pixels_per_second, 1.0);
      box.y = row.y + 1.0;
      box.height = row.height - 2.0;
      box.start = clip->timeline_start;
      box.end = clip->end();
      box.name = clip->name;
      box.enabled = clip->enabled;
      box.linked = !clip->linked_group.empty();
      box.retimed = IsRetimed(*clip);
      box.graphic = IsGraphic(*clip);
      box.has_effects = std::any_of(clip->effects.begin(), clip->effects.end(), [](const timeline::Effect& e) {
        return e.enabled && !e.intrinsic && e.effect_type != "time_remap" && e.effect_type != "graphic" && e.effect_type != "motion_graphics_template";
      });
      layout.clips.push_back(std::move(box));
    }
    for (const auto& transition : track.transitions) {
      if (!row_visible) break;
      const double start = Seconds(transition.timeline_start), end = Seconds(transition.timeline_start.Add(transition.duration));
      if (end < lo || start > hi) continue;
      TransitionBox box;
      box.id = transition.id;
      box.track_id = track.id;
      box.kind = transition.kind;
      box.x = viewport.SecondsToX(start);
      box.width = std::max((end - start) * viewport.pixels_per_second, 1.0);
      box.y = row.y + 1.0;
      box.height = row.height - 2.0;
      layout.transitions.push_back(std::move(box));
    }
    y += row.height + options.track_gap;
  };
  for (const auto* track : video) place(*track);
  for (const auto* track : audio) place(*track);
  layout.content_height = y;
  layout.sequence_seconds = Seconds(sequence.Duration());
  return layout;
}

Hit HitTest(const TimelineLayout& layout, const TimelineViewport& viewport, const LayoutOptions& options, double x, double y, double edge_px) {
  (void)options;
  Hit hit;
  hit.seconds = viewport.XToSeconds(x);
  if (y < 0.0) {
    hit.kind = HitKind::Ruler;
    return hit;
  }
  const TrackRow* row = nullptr;
  for (const auto& candidate : layout.tracks) {
    if (y >= candidate.y && y < candidate.y + candidate.height) row = &candidate;
  }
  if (row == nullptr) return hit;
  hit.track_id = row->id;
  hit.kind = HitKind::TrackEmpty;

  std::vector<const ClipBox*> clips;
  for (const auto& box : layout.clips) {
    if (box.track_id == row->id) clips.push_back(&box);
  }
  std::sort(clips.begin(), clips.end(), [](const auto* a, const auto* b) { return a->x < b->x; });

  // A transition's own box first: it is drawn over the cut it belongs to.
  for (const auto& transition : layout.transitions) {
    if (transition.track_id == row->id && transition.width >= edge_px && x >= transition.x && x < transition.x + transition.width) {
      hit.kind = HitKind::Transition;
      hit.transition_id = transition.id;
      return hit;
    }
  }
  // Edit points: where one clip ends and the next begins.
  for (std::size_t i = 0; i + 1 < clips.size(); ++i) {
    const auto* a = clips[i];
    const auto* b = clips[i + 1];
    if (a->end.Compare(b->start) != 0) continue;
    const double boundary = b->x;
    if (std::abs(x - boundary) <= edge_px) {
      hit.kind = HitKind::EditPoint;
      hit.clip_id = b->clip_id;
      hit.other_clip_id = a->clip_id;
      return hit;
    }
  }
  for (const auto* box : clips) {
    if (x < box->x || x >= box->x + box->width) continue;
    const double edge = std::min(edge_px, box->width / 3.0);
    hit.clip_id = box->clip_id;
    if (x < box->x + edge) hit.kind = HitKind::ClipHead;
    else if (x >= box->x + box->width - edge) hit.kind = HitKind::ClipTail;
    else hit.kind = HitKind::ClipBody;
    return hit;
  }
  return hit;
}

// ----------------------------------------------------------------- selection ----

std::set<std::string> WithLinked(const timeline::Sequence& sequence, const std::set<std::string>& ids) {
  std::set<std::string> groups;
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (ids.count(clip.id) != 0 && !clip.linked_group.empty()) groups.insert(clip.linked_group);
    }
  }
  auto result = ids;
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (!clip.linked_group.empty() && groups.count(clip.linked_group) != 0) result.insert(clip.id);
    }
  }
  return result;
}

void Selection::Select(const timeline::Sequence& sequence, const std::string& clip_id, Mode mode, bool linked) {
  std::set<std::string> ids{clip_id};
  if (linked) ids = WithLinked(sequence, ids);
  switch (mode) {
    case Mode::Replace: clips_ = std::move(ids); break;
    case Mode::Add: clips_.insert(ids.begin(), ids.end()); break;
    case Mode::Toggle:
      if (clips_.count(clip_id) != 0) {
        for (const auto& id : ids) clips_.erase(id);
      } else {
        clips_.insert(ids.begin(), ids.end());
      }
      break;
  }
}

void Selection::SelectRect(const TimelineLayout& layout, double x0, double y0, double x1, double y1, Mode mode, const timeline::Sequence* linked_from) {
  const double left = std::min(x0, x1), right = std::max(x0, x1), top = std::min(y0, y1), bottom = std::max(y0, y1);
  std::set<std::string> ids;
  for (const auto& box : layout.clips) {
    if (box.x < right && box.x + box.width > left && box.y < bottom && box.y + box.height > top) ids.insert(box.clip_id);
  }
  if (linked_from != nullptr) ids = WithLinked(*linked_from, ids);
  if (mode == Mode::Replace) clips_.clear();
  if (mode == Mode::Toggle) {
    for (const auto& id : ids) {
      if (!clips_.erase(id)) clips_.insert(id);
    }
  } else {
    clips_.insert(ids.begin(), ids.end());
  }
}

void Selection::SelectTrackForward(const timeline::Sequence& sequence, const std::string& track_id, const RationalTime& from, Mode mode) {
  const auto* track = sequence.FindTrack(track_id);
  if (mode == Mode::Replace) clips_.clear();
  if (track == nullptr) return;
  for (const auto& clip : track->clips) {
    if (clip.end().Compare(from) > 0) clips_.insert(clip.id);
  }
}

void Selection::SelectAll(const timeline::Sequence& sequence) {
  clips_.clear();
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) clips_.insert(clip.id);
  }
}

void Selection::Prune(const timeline::Sequence& sequence) {
  std::set<std::string> alive;
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clips_.count(clip.id) != 0) alive.insert(clip.id);
    }
  }
  clips_ = std::move(alive);
}

// ------------------------------------------------------------------ snapping ----

std::vector<SnapPoint> CollectSnapPoints(const timeline::Sequence& sequence, const SnapSources& sources, const std::set<std::string>& exclude) {
  std::vector<SnapPoint> points;
  points.push_back({RationalTime(0, 1), SnapKind::SequenceStart, {}});
  points.push_back({sources.playhead, SnapKind::Playhead, {}});
  if (sources.mark_in) points.push_back({*sources.mark_in, SnapKind::In, {}});
  if (sources.mark_out) points.push_back({*sources.mark_out, SnapKind::Out, {}});
  for (const auto& marker : sources.markers) points.push_back({marker, SnapKind::Marker, {}});
  for (const auto& track : sequence.tracks) {
    if (track.is_bus) continue;
    for (const auto& clip : track.clips) {
      if (exclude.count(clip.id) != 0) continue;
      points.push_back({clip.timeline_start, SnapKind::ClipStart, clip.id});
      points.push_back({clip.end(), SnapKind::ClipEnd, clip.id});
    }
  }
  std::stable_sort(points.begin(), points.end(), [](const SnapPoint& a, const SnapPoint& b) { return a.time.ToTicks() < b.time.ToTicks(); });
  return points;
}

namespace {

int Priority(SnapKind kind) {
  switch (kind) {
    case SnapKind::Playhead: return 0;
    case SnapKind::Marker: return 1;
    case SnapKind::In:
    case SnapKind::Out: return 2;
    case SnapKind::SequenceStart: return 3;
    default: return 4;
  }
}

}  // namespace

SnapResult Snap(const RationalTime& time, const std::vector<SnapPoint>& points, const TimelineViewport& viewport, double threshold_px) {
  SnapResult result;
  result.time = time;
  if (points.empty() || viewport.pixels_per_second <= 0.0) return result;
  const auto ticks = time.ToTicks();
  const double threshold_ticks = threshold_px / viewport.pixels_per_second * static_cast<double>(time::kTicksPerSecond);
  const auto first = std::lower_bound(points.begin(), points.end(), ticks - static_cast<std::int64_t>(threshold_ticks) - 1,
                                      [](const SnapPoint& p, std::int64_t value) { return p.time.ToTicks() < value; });
  double best = threshold_ticks + 1.0;
  int best_priority = 99;
  for (auto it = first; it != points.end() && static_cast<double>(it->time.ToTicks() - ticks) <= threshold_ticks; ++it) {
    const double distance = std::abs(static_cast<double>(it->time.ToTicks() - ticks));
    if (distance > threshold_ticks) continue;
    const int priority = Priority(it->kind);
    if (distance < best - 1.0 || (std::abs(distance - best) <= 1.0 && priority < best_priority)) {
      best = distance;
      best_priority = priority;
      result.snapped = true;
      result.point = *it;
      result.time = it->time;
    }
  }
  return result;
}

SpanSnap SnapSpan(const RationalTime& start, const RationalTime& duration, const std::vector<SnapPoint>& points, const TimelineViewport& viewport,
                  double threshold_px) {
  SpanSnap result;
  result.start = start;
  const auto by_start = Snap(start, points, viewport, threshold_px);
  const auto end = start.Add(duration);
  const auto by_end = Snap(end, points, viewport, threshold_px);
  const auto distance = [](const RationalTime& a, const RationalTime& b) { return std::abs(a.ToTicks() - b.ToTicks()); };
  if (by_start.snapped && (!by_end.snapped || distance(by_start.time, start) <= distance(by_end.time, end))) {
    result.start = by_start.time;
    result.snapped = true;
    result.point = by_start.point;
  } else if (by_end.snapped) {
    result.start = by_end.time.Subtract(duration);
    result.snapped = true;
    result.end_snapped = true;
    result.point = by_end.point;
  }
  return result;
}

}  // namespace cutline::ui

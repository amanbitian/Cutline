#include "ui/Ramp.h"

#include <algorithm>
#include <cmath>

namespace cutline::ui {
namespace {

using time::RationalTime;

double Sec(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()); }

RationalTime Frame(time::FrameRate rate) { return RationalTime(rate.denominator, rate.numerator); }

// The nearest whole number of frames, at least one.
RationalTime SnapFrames(const RationalTime& t, time::FrameRate rate) {
  const auto frames = std::max<std::int64_t>(1, t.ToFrames(rate, time::RoundingMode::Nearest));
  return RationalTime::FromFrames(frames, rate);
}

constexpr double kMaxSpeed = 100.0;

// Ramer-Douglas-Peucker on (time, speed): the fewest points that stay within `epsilon` of the series.
void Simplify(const std::vector<std::pair<double, double>>& p, std::size_t first, std::size_t last, double epsilon, std::vector<bool>& keep) {
  if (last <= first + 1) return;
  double worst = -1.0;
  std::size_t at = first;
  for (std::size_t i = first + 1; i < last; ++i) {
    const double t = (p[i].first - p[first].first) / (p[last].first - p[first].first);
    const double line = p[first].second + (p[last].second - p[first].second) * t;
    const double error = std::abs(p[i].second - line);
    if (error > worst) {
      worst = error;
      at = i;
    }
  }
  if (worst > epsilon) {
    keep[at] = true;
    Simplify(p, first, at, epsilon, keep);
    Simplify(p, at, last, epsilon, keep);
  }
}

}  // namespace

RampModel RampModel::Constant(const RationalTime& duration, double speed, time::FrameRate rate) {
  RampModel model;
  model.rate_ = rate;
  model.segments_.push_back({SnapFrames(duration, rate), speed, speed});
  return model;
}

RampModel RampModel::FromClip(const timeline::Clip& clip, time::FrameRate rate) {
  const anim::AnimatedValue* curve = nullptr;
  for (const auto& effect : clip.effects) {
    if (effect.effect_type != "time_remap") continue;
    for (const auto& parameter : effect.parameters) {
      if (parameter.name == "source_offset" && parameter.value.keyframes().size() >= 2) curve = &parameter.value;
    }
  }
  if (curve == nullptr) {
    const double steady = Sec(clip.playback_rate) * (clip.reversed ? -1.0 : 1.0);
    return Constant(clip.duration(), steady, rate);
  }
  const auto& keys = curve->keyframes();
  // The speed over each interval between keys, placed at the interval's middle.
  std::vector<std::pair<double, double>> series;
  for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
    const double t0 = Sec(keys[i].time), t1 = Sec(keys[i + 1].time);
    if (t1 <= t0) continue;
    series.emplace_back((t0 + t1) / 2.0, (keys[i + 1].value.scalar() - keys[i].value.scalar()) / (t1 - t0));
  }
  RampModel model;
  model.rate_ = rate;
  const double total = Sec(keys.back().time) - Sec(keys.front().time);
  if (series.empty() || total <= 0.0) return Constant(clip.duration(), 1.0, rate);
  std::vector<bool> keep(series.size(), false);
  keep.front() = keep.back() = true;
  Simplify(series, 0, series.size() - 1, 0.02, keep);
  std::vector<std::pair<double, double>> points;
  for (std::size_t i = 0; i < series.size(); ++i) {
    if (keep[i]) points.push_back(series[i]);
  }
  // Straight lines between the kept points; the first starts at the clip's start and the last ends at its end, with the
  // speed carried out to them.
  // The first and last points sit half a frame in from the ends; carry them out to the ends.
  points.back().first = total;
  if (points.size() > 1) points.front().first = 0.0;
  const double frame = Sec(Frame(rate));
  std::vector<commands::SpeedSegment> out;
  double previous_time = 0.0;
  double previous_speed = points.front().second;
  // Boundaries are snapped to frames as running totals, so the lengths always add up to the whole.
  const auto at = [&](double seconds) { return SnapFrames(RationalTime(static_cast<std::int64_t>(std::llround(seconds * 1e6)), 1000000), rate); };
  auto snapped_previous = RationalTime(0, 1);
  for (std::size_t i = 0; i < points.size(); ++i) {
    const bool last = i + 1 == points.size();
    const double t = last ? total : points[i].first;
    const double v = points[i].second;
    if (t - previous_time <= 0.0) {
      previous_speed = v;
      continue;
    }
    auto snapped = at(t);
    if (!last && snapped.Compare(snapped_previous) <= 0) continue;
    if (last && snapped.Compare(snapped_previous) <= 0) {
      if (!out.empty()) out.back().end_speed = v;
      continue;
    }
    out.push_back({snapped.Subtract(snapped_previous), previous_speed, v});
    snapped_previous = snapped;
    previous_time = t;
    previous_speed = v;
  }
  // A stretch of a frame or less between two very different speeds is a jump, not a ramp: let its neighbours meet.
  for (std::size_t i = 0; i < out.size();) {
    const bool tiny = Sec(out[i].duration) <= frame * 1.01 && out.size() > 1 && std::abs(out[i].end_speed - out[i].start_speed) > 0.2;
    if (!tiny) {
      ++i;
      continue;
    }
    // Its frame goes to the segment before (or after), so the total length stays.
    if (i > 0) out[i - 1].duration = out[i - 1].duration.Add(out[i].duration);
    else out[i + 1].duration = out[i + 1].duration.Add(out[i].duration);
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
  }
  // A frame between two segments whose speeds nearly meet is the rounding of a corner, not a segment of its own.
  for (std::size_t i = 1; i + 1 < out.size();) {
    const auto& mid = out[i];
    const double scale = std::max(1.0, std::max(std::abs(mid.start_speed), std::abs(mid.end_speed)));
    const bool corner = Sec(mid.duration) <= frame * 1.01 && std::abs(mid.end_speed - mid.start_speed) <= 0.05 * scale;
    if (!corner) {
      ++i;
      continue;
    }
    const double speed = (mid.start_speed + mid.end_speed) / 2.0;
    out[i - 1].end_speed = speed;
    out[i - 1].duration = out[i - 1].duration.Add(mid.duration);
    out[i + 1].start_speed = speed;
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
  }
  // Segments that join with the same straight line are one.
  for (std::size_t i = 0; i + 1 < out.size();) {
    const auto& a = out[i];
    const auto& b = out[i + 1];
    const bool continuous = std::abs(a.end_speed - b.start_speed) < 0.02;
    const double slope_a = (a.end_speed - a.start_speed) / Sec(a.duration);
    const double slope_b = (b.end_speed - b.start_speed) / Sec(b.duration);
    if (continuous && std::abs(slope_a - slope_b) * (Sec(a.duration) + Sec(b.duration)) < 0.03) {
      out[i].end_speed = b.end_speed;
      out[i].duration = a.duration.Add(b.duration);
      out.erase(out.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    } else {
      ++i;
    }
  }
  model.segments_ = std::move(out);
  return model;
}

RationalTime RampModel::duration() const {
  RationalTime total(0, 1);
  for (const auto& s : segments_) total = total.Add(s.duration);
  return total;
}

RationalTime RampModel::BoundaryTime(int boundary) const {
  RationalTime t(0, 1);
  for (int i = 0; i < boundary && i < static_cast<int>(segments_.size()); ++i) t = t.Add(segments_[static_cast<std::size_t>(i)].duration);
  return t;
}

int RampModel::SegmentAt(const RationalTime& t) const {
  if (t.Compare(RationalTime(0, 1)) < 0) return -1;
  RationalTime start(0, 1);
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    const auto end = start.Add(segments_[i].duration);
    if (t.Compare(end) < 0) return static_cast<int>(i);
    start = end;
  }
  return -1;
}

double RampModel::SpeedAt(const RationalTime& t) const {
  const auto i = SegmentAt(t);
  if (i < 0) return segments_.empty() ? 0.0 : segments_.back().end_speed;
  const auto& s = segments_[static_cast<std::size_t>(i)];
  const double local = Sec(t) - Sec(BoundaryTime(i));
  return s.start_speed + (s.end_speed - s.start_speed) * local / Sec(s.duration);
}

double RampModel::SourceSecondsAt(const RationalTime& t) const {
  double position = 0.0;
  double start = 0.0;
  const double target = Sec(t);
  for (const auto& s : segments_) {
    const double length = Sec(s.duration);
    const double span = std::clamp(target - start, 0.0, length);
    position += s.start_speed * span + (s.end_speed - s.start_speed) * span * span / (2.0 * length);
    if (target <= start + length) break;
    start += length;
  }
  return position;
}

std::pair<double, double> RampModel::SourceExtent() const {
  double position = 0.0, low = 0.0, high = 0.0;
  for (const auto& s : segments_) {
    const double length = Sec(s.duration);
    if ((s.start_speed < 0.0) != (s.end_speed < 0.0) && s.start_speed != s.end_speed) {
      const double turn = -s.start_speed * length / (s.end_speed - s.start_speed);
      if (turn > 0.0 && turn < length) {
        const double value = position + s.start_speed * turn + (s.end_speed - s.start_speed) * turn * turn / (2.0 * length);
        low = std::min(low, value);
        high = std::max(high, value);
      }
    }
    position += (s.start_speed + s.end_speed) * length / 2.0;
    low = std::min(low, position);
    high = std::max(high, position);
  }
  return {low, high};
}

bool RampModel::SplitAt(const RationalTime& at) {
  const auto t = SnapFrames(at, rate_);
  const auto i = SegmentAt(t);
  if (i < 0) return false;
  const auto start = BoundaryTime(i);
  const auto& s = segments_[static_cast<std::size_t>(i)];
  if (t.Compare(start) <= 0 || t.Compare(start.Add(s.duration)) >= 0) return false;
  const double speed = SpeedAt(t);
  commands::SpeedSegment first{t.Subtract(start), s.start_speed, speed};
  commands::SpeedSegment second{start.Add(s.duration).Subtract(t), speed, s.end_speed};
  segments_[static_cast<std::size_t>(i)] = first;
  segments_.insert(segments_.begin() + i + 1, second);
  return true;
}

bool RampModel::RemoveBoundary(int boundary) {
  if (boundary < 1 || boundary >= static_cast<int>(segments_.size())) return false;
  auto& a = segments_[static_cast<std::size_t>(boundary) - 1];
  const auto b = segments_[static_cast<std::size_t>(boundary)];
  a.end_speed = b.end_speed;
  a.duration = a.duration.Add(b.duration);
  segments_.erase(segments_.begin() + boundary);
  return true;
}

bool RampModel::MoveBoundary(int boundary, const RationalTime& to) {
  if (boundary < 1 || boundary >= static_cast<int>(segments_.size())) return false;
  auto& a = segments_[static_cast<std::size_t>(boundary) - 1];
  auto& b = segments_[static_cast<std::size_t>(boundary)];
  const auto start = BoundaryTime(boundary - 1);
  const auto end = BoundaryTime(boundary + 1);
  const auto frame = Frame(rate_);
  auto t = RationalTime::FromFrames(std::max<std::int64_t>(0, to.ToFrames(rate_, time::RoundingMode::Nearest)), rate_);
  const auto lo = start.Add(frame), hi = end.Subtract(frame);
  if (lo.Compare(hi) > 0) return false;
  if (t.Compare(lo) < 0) t = lo;
  if (t.Compare(hi) > 0) t = hi;
  a.duration = t.Subtract(start);
  b.duration = end.Subtract(t);
  return true;
}

bool RampModel::SetBoundarySpeed(int boundary, double speed, Side side) {
  if (!std::isfinite(speed) || std::abs(speed) > kMaxSpeed) return false;
  const int n = static_cast<int>(segments_.size());
  if (boundary < 0 || boundary > n) return false;
  if (boundary > 0 && side != Side::After) segments_[static_cast<std::size_t>(boundary) - 1].end_speed = speed;
  if (boundary < n && side != Side::Before) segments_[static_cast<std::size_t>(boundary)].start_speed = speed;
  return true;
}

bool RampModel::SetSegmentSpeed(int segment, double start_speed, double end_speed) {
  if (segment < 0 || segment >= static_cast<int>(segments_.size())) return false;
  if (!std::isfinite(start_speed) || !std::isfinite(end_speed) || std::abs(start_speed) > kMaxSpeed || std::abs(end_speed) > kMaxSpeed) return false;
  segments_[static_cast<std::size_t>(segment)].start_speed = start_speed;
  segments_[static_cast<std::size_t>(segment)].end_speed = end_speed;
  return true;
}

bool RampModel::Freeze(int segment) { return SetSegmentSpeed(segment, 0.0, 0.0); }

bool RampModel::Reverse(int segment) {
  if (segment < 0 || segment >= static_cast<int>(segments_.size())) return false;
  auto& s = segments_[static_cast<std::size_t>(segment)];
  s.start_speed = -s.start_speed;
  s.end_speed = -s.end_speed;
  return true;
}

bool RampModel::SetEase(int segment, RampEase ease) {
  if (segment < 0 || segment >= static_cast<int>(segments_.size())) return false;
  const auto s = segments_[static_cast<std::size_t>(segment)];
  if (ease == RampEase::Linear) return true;
  const auto frames = s.duration.ToFrames(rate_, time::RoundingMode::Nearest);
  const int pieces = static_cast<int>(std::clamp<std::int64_t>(frames / 2, 2, 12));
  if (frames < pieces) return false;
  const auto shape = [&](double f) {
    switch (ease) {
      case RampEase::EaseIn: return f * f;
      case RampEase::EaseOut: return 1.0 - (1.0 - f) * (1.0 - f);
      case RampEase::EaseInOut: return f * f * (3.0 - 2.0 * f);
      default: return f;
    }
  };
  std::vector<commands::SpeedSegment> pieces_out;
  std::int64_t used = 0;
  for (int k = 0; k < pieces; ++k) {
    // Whole frames per piece, the remainder spread from the front.
    const std::int64_t length = frames / pieces + (k < frames % pieces ? 1 : 0);
    const double f0 = static_cast<double>(k) / pieces, f1 = static_cast<double>(k + 1) / pieces;
    pieces_out.push_back({RationalTime::FromFrames(length, rate_), s.start_speed + (s.end_speed - s.start_speed) * shape(f0),
                          s.start_speed + (s.end_speed - s.start_speed) * shape(f1)});
    used += length;
  }
  (void)used;
  segments_.erase(segments_.begin() + segment);
  segments_.insert(segments_.begin() + segment, pieces_out.begin(), pieces_out.end());
  return true;
}

bool RampModel::SetLastDuration(const RationalTime& d) {
  if (segments_.empty()) return false;
  segments_.back().duration = SnapFrames(d, rate_);
  return true;
}

bool RampModel::ScaleSpeeds(double factor) {
  if (!(factor > 0.0) || !std::isfinite(factor)) return false;
  for (const auto& s : segments_) {
    if (std::abs(s.start_speed * factor) > kMaxSpeed || std::abs(s.end_speed * factor) > kMaxSpeed) return false;
  }
  for (auto& s : segments_) {
    s.start_speed *= factor;
    s.end_speed *= factor;
  }
  return true;
}

std::vector<std::pair<double, double>> RampModel::GraphPoints() const {
  std::vector<std::pair<double, double>> points;
  double t = 0.0;
  for (const auto& s : segments_) {
    points.emplace_back(t, s.start_speed);
    t += Sec(s.duration);
    points.emplace_back(t, s.end_speed);
  }
  return points;
}

std::string RampModel::Check(const RationalTime& source_in, const std::optional<RationalTime>& media_length) const {
  if (segments_.empty()) return "The ramp has no segments";
  for (const auto& s : segments_) {
    if (s.duration.Compare(RationalTime(0, 1)) <= 0) return "A segment has no length";
    if (!std::isfinite(s.start_speed) || !std::isfinite(s.end_speed) || std::abs(s.start_speed) > kMaxSpeed || std::abs(s.end_speed) > kMaxSpeed) {
      return "A speed is beyond 100 times";
    }
  }
  const auto [low, high] = SourceExtent();
  if (high - low < 0.001) return "The ramp does not move through the source";
  if (Sec(source_in) + low < -1e-9) return "The ramp runs back past the start of the media";
  if (media_length && Sec(source_in) + high > Sec(*media_length) + 1e-9) return "The ramp runs on past the end of the media";
  return {};
}

EditPlan PlanApplyRamp(const EditContext& ctx, const std::string& clip_id, const RampModel& model) {
  const timeline::Clip* clip = nullptr;
  const timeline::Track* track = nullptr;
  for (const auto& t : ctx.sequence->tracks) {
    for (const auto& c : t.clips) {
      if (c.id == clip_id) {
        clip = &c;
        track = &t;
      }
    }
  }
  if (clip == nullptr) return EditPlan::Refuse("The clip is not in the sequence");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  // Where the clip reads at its first frame: the start of its window, or, with a ramp already, where that ramp starts.
  RationalTime anchor = clip->source_in;
  for (const auto& effect : clip->effects) {
    if (effect.effect_type != "time_remap") continue;
    for (const auto& parameter : effect.parameters) {
      if (parameter.name == "source_offset" && !parameter.value.keyframes().empty()) {
        const double offset = parameter.value.keyframes().front().value.scalar();
        anchor = clip->source_in.Add(RationalTime(static_cast<std::int64_t>(std::llround(offset * 1e6)), 1000000));
      }
    }
  }
  std::optional<RationalTime> length;
  if (clip->source_kind == model::SourceKind::Media && ctx.media_duration) length = ctx.media_duration(clip->source_id);
  if (clip->source_kind == model::SourceKind::Sequence && ctx.sequence_duration) length = ctx.sequence_duration(clip->source_id);
  if (const auto problem = model.Check(anchor, length); !problem.empty()) return EditPlan::Refuse(problem);
  commands::SetSpeedRampPayload payload;
  payload.clip_id = clip_id;
  payload.segments = model.segments();
  payload.propagate_links = ctx.linked;
  EditPlan plan;
  plan.ok = true;
  plan.label = "Speed Ramp";
  plan.commands.push_back({commands::CommandType::SetSpeedRamp, payload});
  plan.result_time = clip->timeline_start.Add(model.duration());
  return plan;
}

EditPlan PlanClearRamp(const EditContext& ctx, const std::string& clip_id) {
  for (const auto& t : ctx.sequence->tracks) {
    for (const auto& c : t.clips) {
      if (c.id != clip_id) continue;
      const bool has = std::any_of(c.effects.begin(), c.effects.end(), [](const timeline::Effect& e) { return e.effect_type == "time_remap"; });
      if (!has) return EditPlan::Refuse("The clip has no speed ramp");
      if (t.locked) return EditPlan::Refuse("The track " + t.id + " is locked");
      EditPlan plan;
      plan.ok = true;
      plan.label = "Remove Speed Ramp";
      plan.commands.push_back({commands::CommandType::ClearSpeedRamp, commands::ClearSpeedRampPayload{clip_id, ctx.linked}});
      return plan;
    }
  }
  return EditPlan::Refuse("The clip is not in the sequence");
}

}  // namespace cutline::ui

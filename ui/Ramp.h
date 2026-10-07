#pragma once

// The speed ramp as a person edits it: a speed graph over the clip's own time, made of segments that each go from one
// speed to another in a straight line (1 is normal, 2 twice as fast, 0 a freeze, negative backwards), with the height
// of the graph at any time being how fast the picture is moving through the source then. This is the same shape the
// SetSpeedRamp command takes, so what is edited is what is stored.
//
// A ramp read back from a clip is recovered from the stored source-time curve by looking at how fast it advances from
// frame to frame and joining stretches that are straight; so a ramp that was written from segments comes back as
// those segments (to the accuracy of a frame), and a ramp made some other way comes back as the nearest straight-line
// description of it.

#include "core/commands/Command.h"
#include "timeline/Sequence.h"
#include "ui/EditPlanner.h"

#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

enum class RampEase { Linear, EaseIn, EaseOut, EaseInOut };

class RampModel final {
 public:
  RampModel() = default;
  // One segment of constant speed: the starting point for a clip that has no ramp yet.
  [[nodiscard]] static RampModel Constant(const time::RationalTime& duration, double speed, time::FrameRate rate);
  // What the clip plays now: its ramp if it has one, else its steady speed (and direction).
  [[nodiscard]] static RampModel FromClip(const timeline::Clip& clip, time::FrameRate rate);

  [[nodiscard]] const std::vector<commands::SpeedSegment>& segments() const { return segments_; }
  [[nodiscard]] time::FrameRate rate() const { return rate_; }
  [[nodiscard]] time::RationalTime duration() const;
  [[nodiscard]] bool empty() const { return segments_.empty(); }
  // The segment holding a time (a boundary belongs to the segment after it), or -1 outside the ramp.
  [[nodiscard]] int SegmentAt(const time::RationalTime& t) const;
  [[nodiscard]] time::RationalTime BoundaryTime(int boundary) const;  // boundary 0 is the start, segments().size() the end
  [[nodiscard]] double SpeedAt(const time::RationalTime& t) const;
  // How far into the source (from the clip's own source in point) the picture is at a time: the integral of the speed.
  [[nodiscard]] double SourceSecondsAt(const time::RationalTime& t) const;
  // The lowest and highest source positions the ramp reaches, relative to where the clip starts.
  [[nodiscard]] std::pair<double, double> SourceExtent() const;

  // ---- edits. Each returns false (changing nothing) when it cannot be done.
  // Cuts the segment at a time into two with the speed it had there, so the shape is unchanged.
  bool SplitAt(const time::RationalTime& t);
  // Joins the two segments either side of an inner boundary into one straight one.
  bool RemoveBoundary(int boundary);
  // Moves an inner boundary in time; the two segments next to it trade length, each keeping at least a frame.
  bool MoveBoundary(int boundary, const time::RationalTime& to);
  // Sets the speed at a boundary: the end of the segment before and the start of the one after, together, or only one
  // side to make a jump.
  enum class Side { Both, Before, After };
  bool SetBoundarySpeed(int boundary, double speed, Side side = Side::Both);
  // A whole segment at one speed, held still, or run backwards.
  bool SetSegmentSpeed(int segment, double start_speed, double end_speed);
  bool Freeze(int segment);
  bool Reverse(int segment);
  // Changes how a segment gets from its start speed to its end speed, as steps that follow the curve.
  bool SetEase(int segment, RampEase ease);
  // Makes the last segment longer or shorter, which makes the whole ramp so.
  bool SetLastDuration(const time::RationalTime& duration);
  // Scales every speed.
  bool ScaleSpeeds(double factor);

  // The graph as points for drawing: (seconds, speed), with a pair at the same time wherever the speed jumps.
  [[nodiscard]] std::vector<std::pair<double, double>> GraphPoints() const;
  // Empty when the ramp can be applied to the clip, else why not: a speed out of range, or a sweep of the source that
  // leaves the media.
  [[nodiscard]] std::string Check(const time::RationalTime& source_in, const std::optional<time::RationalTime>& media_length) const;

 private:
  std::vector<commands::SpeedSegment> segments_;
  time::FrameRate rate_{time::kFrameRate25};
};

// Apply the ramp to a clip as one undoable step (linked clips follow), or take it off.
[[nodiscard]] EditPlan PlanApplyRamp(const EditContext& ctx, const std::string& clip_id, const RampModel& model);
[[nodiscard]] EditPlan PlanClearRamp(const EditContext& ctx, const std::string& clip_id);

}  // namespace cutline::ui

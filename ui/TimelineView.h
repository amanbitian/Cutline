#pragma once

// The timeline's view: how time maps to pixels, the ruler, where each track and clip is drawn, what is under
// the pointer, what is selected, and what a dragged edge or clip snaps to. Pure geometry over a sequence
// snapshot; the window that draws it (and the editing rules in EditPlanner.h) sit on top.

#include "core/model/Types.h"
#include "core/time/RationalTime.h"
#include "timeline/Sequence.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cutline::ui {

using time::RationalTime;

// ----------------------------------------------------------------- viewport ----

struct TimelineViewport final {
  static constexpr double kMinPixelsPerSecond = 0.02;
  static constexpr double kMaxPixelsPerSecond = 40000.0;

  double pixels_per_second{100.0};
  double scroll_seconds{0.0};  // the time at the left edge
  double width_px{1000.0};
  double vertical_scroll_px{0.0};
  double height_px{1.0e9};

  [[nodiscard]] double SecondsToX(double seconds) const { return (seconds - scroll_seconds) * pixels_per_second; }
  [[nodiscard]] double XToSeconds(double x) const { return scroll_seconds + x / pixels_per_second; }
  [[nodiscard]] double TimeToX(const RationalTime& t) const;
  // The frame boundary nearest the pixel, never before zero.
  [[nodiscard]] RationalTime XToTime(double x, time::FrameRate rate) const;
  [[nodiscard]] double VisibleSeconds() const { return width_px / pixels_per_second; }
  [[nodiscard]] double EndSeconds() const { return scroll_seconds + VisibleSeconds(); }
  [[nodiscard]] double VerticalEndPx() const { return vertical_scroll_px + height_px; }

  // Zooms by `factor` (above one zooms in) keeping the time under pixel `anchor_x` where it is.
  void ZoomAt(double anchor_x, double factor);
  void ScrollBySeconds(double seconds);
  void ScrollByPixels(double pixels) { ScrollBySeconds(pixels / pixels_per_second); }
  // Zooms and scrolls so [start, end] seconds fill the width with a margin at each side.
  void FitRange(double start_seconds, double end_seconds, double margin_px = 24.0);
  // Scrolls just enough to bring `seconds` into view (leaving `margin_px`); returns whether it moved.
  bool EnsureVisible(double seconds, double margin_px = 40.0);
  // While playing: keep the playhead in view by paging, as editors do, when it reaches the right edge.
  bool FollowPlayhead(double seconds);
};

struct RulerTick final {
  double seconds{0.0};
  double x{0.0};
  bool major{false};
  std::string label;  // timecode, on major ticks
};

// Ticks for the visible span. The step is the smallest of 1, 2, 5, 10 frames and then 1, 2, 5, 10, 15, 30 seconds,
// 1, 2, 5, 10, 15, 30 minutes and hours that keeps major ticks at least `min_major_px` apart; minor ticks
// subdivide it while they stay at least 10 pixels apart.
[[nodiscard]] std::vector<RulerTick> BuildRuler(const TimelineViewport& viewport, time::FrameRate rate, bool drop_frame, double min_major_px = 90.0);

// -------------------------------------------------------------------- layout ----

struct LayoutOptions final {
  double ruler_height{28.0};
  double video_track_height{56.0};
  double audio_track_height{40.0};
  double track_gap{2.0};
  double header_width{120.0};
};

struct TrackRow final {
  std::string id;
  model::TrackKind kind{model::TrackKind::Video};
  std::string name;
  double y{0}, height{0};
  bool locked{false}, muted{false}, solo{false};
  std::int64_t order{0};
};

struct ClipBox final {
  std::string clip_id, track_id;
  model::TrackKind kind{model::TrackKind::Video};
  model::SourceKind source_kind{model::SourceKind::Media};
  double x{0}, y{0}, width{0}, height{0};
  RationalTime start, end;
  std::string name;
  bool enabled{true};
  bool linked{false};
  // Played at another speed, backwards, or through a speed ramp.
  bool retimed{false};
  bool has_effects{false};
  bool graphic{false};
};

struct TransitionBox final {
  std::string id, track_id, kind;
  double x{0}, y{0}, width{0}, height{0};
};

struct TimelineLayout final {
  std::vector<TrackRow> tracks;
  std::vector<ClipBox> clips;
  std::vector<TransitionBox> transitions;
  double content_height{0};  // below the ruler
  double sequence_seconds{0};
};

// Video tracks from the highest order down, then audio tracks from the lowest order down, as editors show them.
// Buses are not shown. Tracks and clips outside the viewport are left out;
// clip lookup starts by binary search, so layout cost follows what is visible
// instead of total project length.
[[nodiscard]] TimelineLayout BuildTimelineLayout(const timeline::Sequence& sequence, const TimelineViewport& viewport,
                                                 const LayoutOptions& options = {});

enum class HitKind { None, Ruler, TrackEmpty, ClipBody, ClipHead, ClipTail, EditPoint, Transition };

struct Hit final {
  HitKind kind{HitKind::None};
  std::string clip_id;        // the clip; for an edit point, the clip that starts there
  std::string other_clip_id;  // for an edit point, the clip that ends there
  std::string track_id;
  std::string transition_id;
  double seconds{0.0};  // the time under the pointer
};

// What is at a point (relative to the top-left of the track area, below the header). Clip edges within `edge_px`
// (at most a third of the clip's width) are trim handles; where one clip ends and the next begins, the pair is an
// edit point and the zone straddles the join.
[[nodiscard]] Hit HitTest(const TimelineLayout& layout, const TimelineViewport& viewport, const LayoutOptions& options,
                          double x, double y, double edge_px = 6.0);

// ----------------------------------------------------------------- selection ----

class Selection final {
 public:
  enum class Mode { Replace, Toggle, Add };

  [[nodiscard]] const std::set<std::string>& clips() const { return clips_; }
  [[nodiscard]] bool Contains(const std::string& clip_id) const { return clips_.count(clip_id) != 0; }
  [[nodiscard]] bool empty() const { return clips_.empty(); }
  // Selects a clip (and, if `linked`, the clips linked to it).
  void Select(const timeline::Sequence& sequence, const std::string& clip_id, Mode mode, bool linked);
  // Selects every clip whose box meets the rectangle (in layout coordinates).
  void SelectRect(const TimelineLayout& layout, double x0, double y0, double x1, double y1, Mode mode, const timeline::Sequence* linked_from = nullptr);
  // Every clip of the track from a time onward (the track-select tool).
  void SelectTrackForward(const timeline::Sequence& sequence, const std::string& track_id, const RationalTime& from, Mode mode);
  void SelectAll(const timeline::Sequence& sequence);
  void Clear() { clips_.clear(); }
  // Drops ids that no longer exist after an edit.
  void Prune(const timeline::Sequence& sequence);

 private:
  std::set<std::string> clips_;
};

// The clips linked to any of `ids` (and `ids` themselves).
[[nodiscard]] std::set<std::string> WithLinked(const timeline::Sequence& sequence, const std::set<std::string>& ids);

// ------------------------------------------------------------------ snapping ----

enum class SnapKind { ClipStart, ClipEnd, Playhead, Marker, In, Out, SequenceStart };

struct SnapPoint final {
  RationalTime time;
  SnapKind kind{SnapKind::ClipStart};
  std::string source;  // the clip or marker it belongs to
};

struct SnapSources final {
  RationalTime playhead;
  std::optional<RationalTime> mark_in, mark_out;
  std::vector<RationalTime> markers;
};

// Every point a drag can snap to, in time order, leaving out the clips being dragged (a clip does not snap to itself).
[[nodiscard]] std::vector<SnapPoint> CollectSnapPoints(const timeline::Sequence& sequence, const SnapSources& sources,
                                                       const std::set<std::string>& exclude_clips = {});

struct SnapResult final {
  RationalTime time;
  bool snapped{false};
  SnapPoint point;
};

// The nearest point within `threshold_px` of `time` at this zoom, else the time itself.
[[nodiscard]] SnapResult Snap(const RationalTime& time, const std::vector<SnapPoint>& points, const TimelineViewport& viewport, double threshold_px);

// Snaps a span that moves as one (a dragged clip): its start or its end, whichever finds a point nearer. The result is
// the amount to add to the proposed start.
struct SpanSnap final {
  RationalTime start;   // the start after snapping
  bool snapped{false};
  SnapPoint point;
  bool end_snapped{false};  // the end, not the start, found the point
};
[[nodiscard]] SpanSnap SnapSpan(const RationalTime& start, const RationalTime& duration, const std::vector<SnapPoint>& points,
                                const TimelineViewport& viewport, double threshold_px);

}  // namespace cutline::ui

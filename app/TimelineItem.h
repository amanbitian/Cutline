#pragma once

// The timeline as a Qt Quick item: it paints the ruler, the track headers, the clips and the playhead from the layout
// that ui/TimelineView.h works out, and turns the mouse into gestures whose meaning is in ui/EditPlanner.h: scrub,
// select, drag to move, drag an edge to trim (ripple, roll), slip, slide, razor, zoom and pan. While a gesture is in
// progress it shows what would happen (ghost clips, the edge, the snap line, and red where the edit is refused);
// releasing runs it as one undoable step.

#include "app/Session.h"
#include "ui/EditPlanner.h"
#include "ui/Ramp.h"
#include "ui/TimelineView.h"

#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <optional>

namespace cutline::app {

class TimelineItem : public QQuickPaintedItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(TimelineItem)
  Q_PROPERTY(cutline::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
  Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond NOTIFY viewChanged)

 public:
  explicit TimelineItem(QQuickItem* parent = nullptr);
  [[nodiscard]] Session* session() const { return session_; }
  void setSession(Session* session);
  [[nodiscard]] double pixelsPerSecond() const { return viewport_.pixels_per_second; }
  void paint(QPainter* painter) override;

  Q_INVOKABLE void zoom(double factor);
  Q_INVOKABLE void zoomToFit();
  // Where a handle of a clip speed ramp is drawn, in this item coordinates (invalid if it is not shown); for tests and tools.
  Q_INVOKABLE QPointF rampHandlePosition(const QString& clip_id, int boundary, bool after) const;
  Q_INVOKABLE bool dropMedia(double x, double y, const QString& media_id, bool insert);

 signals:
  void sessionChanged();
  void viewChanged();

 protected:
  void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void hoverMoveEvent(QHoverEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  enum class Drag { None, Scrub, Marquee, Move, Trim, Roll, Slip, Slide, Pan, RampHandle };
  struct DragState {
    Drag kind{Drag::None};
    QPointF press;
    QPointF current;
    std::string clip_id;        // the clip pressed
    std::string other_clip_id;  // for a roll: the clip before the cut
    ui::TrimEdge edge{ui::TrimEdge::Tail};
    ui::TrimMode trim_mode{ui::TrimMode::Normal};
    bool linked{true};
    bool insert{false};
    ui::Selection::Mode marquee_mode{ui::Selection::Mode::Replace};
    double press_scroll_seconds{0};
    double press_scroll_y{0};
    ui::EditPlan plan;
    std::optional<time::RationalTime> snap_at;
    std::optional<time::RationalTime> result_time;
    int track_delta{0};
    time::RationalTime delta{0, 1};
    // A speed-ramp handle being dragged: the ramp as it was at the press, the one being shaped, and the speed scale of the
    // band, which stays put while dragging so the handle follows the pointer.
    ui::RampModel ramp_before;
    ui::RampModel ramp;
    int ramp_boundary{0};
    ui::RampModel::Side ramp_side{ui::RampModel::Side::Both};
    double range_lo{-1}, range_hi{2};
    QRectF band;
  };

  [[nodiscard]] ui::LayoutOptions Options() const;
  [[nodiscard]] double TrackAreaX(double x) const { return x - header_width_; }
  [[nodiscard]] double TrackAreaY(double y) const { return y - ruler_height_ + scroll_y_; }
  [[nodiscard]] time::FrameRate Rate() const;
  void Rebuild();
  void Clamp();
  void BeginDrag(QMouseEvent* event);
  void UpdateDrag(QMouseEvent* event);
  void EndDrag(QMouseEvent* event);
  [[nodiscard]] ui::EditContext Context(bool linked) const;
  [[nodiscard]] std::vector<ui::SnapPoint> SnapPoints(const std::set<std::string>& exclude) const;
  [[nodiscard]] time::RationalTime SnappedTime(double x, const std::set<std::string>& exclude, std::optional<time::RationalTime>& snapped_at) const;
  [[nodiscard]] const ui::TrackRow* RowAtY(double y) const;
  void SeekToX(double x);
  // The speed band drawn over the lower part of a clip that has a ramp, and the handles on it.
  [[nodiscard]] bool HitRampHandle(const QPointF& pos);
  void RefreshRamps();
  [[nodiscard]] static QColor ClipColour(const ui::ClipBox& box);

  Session* session_{nullptr};
  ui::TimelineViewport viewport_;
  ui::TimelineLayout layout_;
  double scroll_y_{0};
  double header_width_{132};
  double ruler_height_{30};
  DragState drag_;
  std::map<std::string, ui::RampModel> ramps_;
  bool fitted_once_{false};
  bool had_content_{false};
};

}  // namespace cutline::app

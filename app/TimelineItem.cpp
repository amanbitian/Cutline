#include "app/TimelineItem.h"

#include <QCursor>
#include <QFont>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cutline::app {
namespace {

using time::RationalTime;

constexpr double kEdgePx = 7.0;

double SecondsOf(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()); }

// The draggable points of a ramp graph: each segment's two ends, one point where they meet at the same speed.
struct RampHandleSpec {
  int boundary;
  ui::RampModel::Side side;
  double seconds;
  double speed;
};

std::vector<RampHandleSpec> RampHandles(const ui::RampModel& ramp) {
  std::vector<RampHandleSpec> list;
  double start = 0.0;
  const auto& segments = ramp.segments();
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& s = segments[i];
    const bool joined = i > 0 && std::abs(segments[i - 1].end_speed - s.start_speed) < 1e-9;
    if (joined) list.back().side = ui::RampModel::Side::Both;
    else list.push_back({static_cast<int>(i), ui::RampModel::Side::After, start, s.start_speed});
    start += SecondsOf(s.duration);
    list.push_back({static_cast<int>(i) + 1, ui::RampModel::Side::Before, start, s.end_speed});
  }
  return list;
}

std::pair<double, double> RampSpeedRange(const ui::RampModel& ramp) {
  double lo = -1.0, hi = 2.0;
  for (const auto& s : ramp.segments()) {
    lo = std::min({lo, s.start_speed, s.end_speed});
    hi = std::max({hi, s.start_speed, s.end_speed});
  }
  return {lo - 0.25, hi + 0.25};
}

QRectF RampBand(const QRectF& clip) { return QRectF(clip.left(), clip.top() + 18.0, clip.width(), std::max(0.0, clip.height() - 20.0)); }

bool HasRamp(const timeline::Clip& clip) {
  for (const auto& effect : clip.effects) {
    if (effect.effect_type == "time_remap") return true;
  }
  return false;
}

RationalTime FramesToTime(double seconds, time::FrameRate rate) {
  const auto frames = static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator)));
  return RationalTime::FromFrames(frames, rate);
}

}  // namespace

TimelineItem::TimelineItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
  setAcceptHoverEvents(true);
  setAntialiasing(true);
  setOpaquePainting(true);
  setFlag(ItemIsFocusScope, false);
}

void TimelineItem::setSession(Session* session) {
  if (session_ == session) return;
  session_ = session;
  if (session_ != nullptr) {
    const auto refresh = [this] {
      Rebuild();
      update();
    };
    connect(session_, &Session::sequenceChanged, this, refresh);
    connect(session_, &Session::selectionChanged, this, [this] { update(); });
    connect(session_, &Session::marksChanged, this, [this] { update(); });
    connect(session_, &Session::playheadChanged, this, [this] { update(); });
    connect(session_, &Session::snapChanged, this, [this] { update(); });
    connect(session_, &Session::projectChanged, this, [this] {
      fitted_once_ = false;
      had_content_ = false;
      scroll_y_ = 0;
      Rebuild();
      update();
    });
    connect(session_, &Session::timelineAction, this, [this](const QString& action) {
      if (action == "zoom_in") zoom(1.5);
      else if (action == "zoom_out") zoom(1.0 / 1.5);
      else if (action == "zoom_fit") zoomToFit();
      else if (action == "follow") {
        if (viewport_.FollowPlayhead(session_->playheadSeconds())) {
          Rebuild();
          update();
          emit viewChanged();
        }
      }
    });
  }
  Rebuild();
  emit sessionChanged();
}

time::FrameRate TimelineItem::Rate() const {
  const auto* sequence = session_ != nullptr ? session_->sequence() : nullptr;
  return sequence != nullptr ? sequence->frame_rate : time::kFrameRate25;
}

ui::LayoutOptions TimelineItem::Options() const {
  ui::LayoutOptions options;
  options.header_width = header_width_;
  options.ruler_height = ruler_height_;
  if (session_ != nullptr) {
    options.video_track_height = static_cast<double>(session_->prefs().GetInt("timeline.video_track_height"));
    options.audio_track_height = static_cast<double>(session_->prefs().GetInt("timeline.audio_track_height"));
  }
  return options;
}

void TimelineItem::Clamp() {
  viewport_.scroll_seconds = std::max(0.0, viewport_.scroll_seconds);
  const double visible = std::max(0.0, height() - ruler_height_);
  scroll_y_ = std::clamp(scroll_y_, 0.0, std::max(0.0, layout_.content_height - visible));
}

void TimelineItem::RefreshRamps() {
  ramps_.clear();
  const auto* sequence = session_ != nullptr ? session_->sequence() : nullptr;
  if (sequence == nullptr) return;
  for (const auto& box : layout_.clips) {
    if (!box.retimed || box.height < 34.0) continue;
    for (const auto& track : sequence->tracks) {
      for (const auto& clip : track.clips) {
        if (clip.id == box.clip_id && HasRamp(clip)) ramps_[clip.id] = ui::RampModel::FromClip(clip, sequence->frame_rate);
      }
    }
  }
}

void TimelineItem::Rebuild() {
  viewport_.width_px = std::max(1.0, width() - header_width_);
  const auto* sequence = session_ != nullptr ? session_->sequence() : nullptr;
  if (sequence != nullptr) {
    layout_ = ui::BuildTimelineLayout(*sequence, viewport_, Options());
    // The first time there is something to see, fit it; after that the view is the person's.
    if (!fitted_once_ && width() > 200 && sequence->Duration().numerator() > 0) {
      viewport_.FitRange(0.0, std::max(SecondsOf(sequence->Duration()) * 1.05, 5.0));
      layout_ = ui::BuildTimelineLayout(*sequence, viewport_, Options());
      fitted_once_ = true;
    }
    had_content_ = had_content_ || sequence->Duration().numerator() > 0;
  } else {
    layout_ = {};
  }
  Clamp();
  RefreshRamps();
}

void TimelineItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
  QQuickPaintedItem::geometryChange(new_geometry, old_geometry);
  Rebuild();
  update();
}

void TimelineItem::zoom(double factor) {
  const double playhead_x = viewport_.SecondsToX(session_ != nullptr ? session_->playheadSeconds() : 0.0);
  const double anchor = playhead_x >= 0 && playhead_x <= viewport_.width_px ? playhead_x : viewport_.width_px / 2.0;
  viewport_.ZoomAt(anchor, factor);
  Rebuild();
  update();
  emit viewChanged();
}

void TimelineItem::zoomToFit() {
  const auto* sequence = session_ != nullptr ? session_->sequence() : nullptr;
  const double end = sequence != nullptr ? SecondsOf(sequence->Duration()) : 10.0;
  viewport_.FitRange(0.0, std::max(end, 1.0));
  Rebuild();
  update();
  emit viewChanged();
}

QColor TimelineItem::ClipColour(const ui::ClipBox& box) {
  QColor colour;
  if (box.graphic) colour = QColor("#7f4fa8");
  else if (box.source_kind == model::SourceKind::Adjustment) colour = QColor("#7a7a7a");
  else if (box.source_kind == model::SourceKind::Sequence) colour = QColor("#9a7430");
  else if (box.kind == model::TrackKind::Audio) colour = QColor("#2f8f5f");
  else colour = QColor("#2f6f8f");
  if (!box.enabled) colour.setAlpha(90);
  return colour;
}

const ui::TrackRow* TimelineItem::RowAtY(double y) const {
  for (const auto& row : layout_.tracks) {
    if (y >= row.y && y < row.y + row.height) return &row;
  }
  return nullptr;
}

std::vector<ui::SnapPoint> TimelineItem::SnapPoints(const std::set<std::string>& exclude) const {
  ui::SnapSources sources;
  sources.playhead = session_->playheadTime();
  sources.mark_in = session_->markInTime();
  sources.mark_out = session_->markOutTime();
  sources.markers = session_->markers();
  return ui::CollectSnapPoints(*session_->sequence(), sources, exclude);
}

RationalTime TimelineItem::SnappedTime(double x, const std::set<std::string>& exclude, std::optional<RationalTime>& snapped_at) const {
  const auto time = viewport_.XToTime(TrackAreaX(x), Rate());
  snapped_at.reset();
  if (!session_->snap()) return time;
  const auto result = ui::Snap(time, SnapPoints(exclude), viewport_, static_cast<double>(session_->prefs().GetInt("timeline.snap_distance_px")));
  if (result.snapped) snapped_at = result.time;
  return result.time;
}

ui::EditContext TimelineItem::Context(bool linked) const {
  auto ctx = session_->EditContextFor();
  ctx.linked = linked && ctx.linked;
  return ctx;
}

// ------------------------------------------------------------------------ painting ----

void TimelineItem::paint(QPainter* painter) {
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->fillRect(boundingRect(), QColor("#141515"));
  if (session_ == nullptr || session_->sequence() == nullptr) {
    painter->setPen(QColor("#5c6462"));
    painter->drawText(boundingRect(), Qt::AlignCenter, tr("No sequence"));
    return;
  }
  const auto* sequence = session_->sequence();
  const QRectF track_area(header_width_, ruler_height_, std::max(0.0, width() - header_width_), std::max(0.0, height() - ruler_height_));
  QFont small("Segoe UI", 8);
  QFont normal("Segoe UI", 9);

  // Track rows and clips, clipped to the area below the ruler and right of the headers.
  painter->save();
  painter->setClipRect(track_area);
  for (const auto& row : layout_.tracks) {
    const double y = ruler_height_ + row.y - scroll_y_;
    if (y + row.height < ruler_height_ || y > height()) continue;
    painter->fillRect(QRectF(header_width_, y, track_area.width(), row.height), row.kind == model::TrackKind::Video ? QColor("#1a1c1c") : QColor("#171a19"));
    painter->setPen(QColor("#252828"));
    painter->drawLine(QPointF(header_width_, y + row.height), QPointF(width(), y + row.height));
  }
  const bool names = session_->prefs().GetBool("timeline.show_clip_names");
  for (const auto& box : layout_.clips) {
    const double y = ruler_height_ + box.y - scroll_y_;
    const QRectF rect(header_width_ + box.x, y, box.width, box.height);
    if (rect.right() < header_width_ || rect.left() > width() || rect.bottom() < ruler_height_ || rect.top() > height()) continue;
    const bool selected = session_->selection().Contains(box.clip_id);
    auto colour = ClipColour(box);
    QPainterPath path;
    path.addRoundedRect(rect, 3, 3);
    painter->fillPath(path, colour);
    // A lighter title strip, the way editors show a clip's label.
    painter->save();
    painter->setClipPath(path);
    painter->fillRect(QRectF(rect.left(), rect.top(), rect.width(), std::min(16.0, rect.height())), colour.lighter(125));
    painter->restore();
    painter->setPen(selected ? QPen(QColor("#d9ff62"), 2.0) : QPen(colour.darker(160), 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
    if (box.linked) {
      painter->setPen(QPen(QColor(255, 255, 255, 120), 1.0));
      painter->drawLine(QPointF(rect.left() + 3, rect.top() + 16.5), QPointF(rect.left() + std::min(rect.width() - 3, 40.0), rect.top() + 16.5));
    }
    if (names && rect.width() > 28) {
      painter->setFont(normal);
      painter->setPen(QColor(box.enabled ? "#f4f5f1" : "#9aa09e"));
      const QFontMetricsF metrics(normal);
      QString label = QString::fromStdString(box.name.empty() ? box.clip_id : box.name);
      label = metrics.elidedText(label, Qt::ElideRight, rect.width() - 30);
      painter->drawText(QRectF(rect.left() + 5, rect.top() + 1, rect.width() - 8, 15), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    // Badges: effects, retime.
    painter->setFont(small);
    double badge_x = rect.right() - 4;
    const auto badge = [&](const QString& text, const QColor& fill) {
      const QFontMetricsF metrics(small);
      const double w = metrics.horizontalAdvance(text) + 6;
      if (badge_x - w < rect.left() + 20) return;
      const QRectF r(badge_x - w, rect.top() + 2, w, 12);
      painter->fillRect(r, fill);
      painter->setPen(QColor("#101111"));
      painter->drawText(r, Qt::AlignCenter, text);
      badge_x -= w + 2;
    };
    if (box.has_effects) badge("fx", QColor("#d9ff62"));
    if (box.retimed) badge("R", QColor("#ffb36b"));
    // The speed over the clip's own time, with a handle at each cut and end.
    const bool dragging_this = drag_.kind == Drag::RampHandle && drag_.clip_id == box.clip_id;
    const auto ramp_it = ramps_.find(box.clip_id);
    if (dragging_this || ramp_it != ramps_.end()) {
      const auto& ramp = dragging_this ? drag_.ramp : ramp_it->second;
      const auto band = RampBand(rect);
      const auto range = dragging_this ? std::pair<double, double>{drag_.range_lo, drag_.range_hi} : RampSpeedRange(ramp);
      if (band.height() >= 14.0 && !ramp.empty()) {
        const double total = SecondsOf(ramp.duration());
        const auto px = [&](double t) { return band.left() + (total > 0 ? t / total : 0.0) * band.width(); };
        const auto py = [&](double v) { return band.top() + (range.second - v) / (range.second - range.first) * band.height(); };
        painter->save();
        painter->setClipRect(rect.intersected(band));
        painter->fillRect(band, QColor(0, 0, 0, 90));
        painter->setPen(QPen(QColor(255, 255, 255, 60), 1.0, Qt::DotLine));
        painter->drawLine(QPointF(band.left(), py(0.0)), QPointF(band.right(), py(0.0)));
        painter->setPen(QPen(QColor(255, 255, 255, 40), 1.0, Qt::DotLine));
        painter->drawLine(QPointF(band.left(), py(1.0)), QPointF(band.right(), py(1.0)));
        QPainterPath line;
        bool first = true;
        for (const auto& [t, v] : ramp.GraphPoints()) {
          if (first) line.moveTo(px(t), py(v));
          else line.lineTo(px(t), py(v));
          first = false;
        }
        painter->setPen(QPen(QColor("#ffb36b"), 1.6));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(line);
        painter->restore();
        if (band.height() >= 24.0 && band.width() >= 40.0) {
          painter->setPen(QPen(QColor("#ffb36b"), 1.5));
          for (const auto& handle : RampHandles(ramp)) {
            painter->setBrush(QColor("#101111"));
            painter->drawEllipse(QPointF(px(handle.seconds), py(handle.speed)), 3.5, 3.5);
          }
        }
      }
    }
  }
  for (const auto& transition : layout_.transitions) {
    const double y = ruler_height_ + transition.y - scroll_y_;
    const QRectF rect(header_width_ + transition.x, y, transition.width, transition.height);
    painter->fillRect(rect, QColor(255, 255, 255, 60));
    painter->setPen(QColor(255, 255, 255, 140));
    painter->drawLine(rect.bottomLeft(), rect.topRight());
  }

  // What a gesture in progress would do.
  const auto row_by_id = [&](const std::string& id) -> const ui::TrackRow* {
    for (const auto& r : layout_.tracks) {
      if (r.id == id) return &r;
    }
    return nullptr;
  };
  if (drag_.kind == Drag::Move) {
    const bool ok = drag_.plan.ok;
    const std::set<std::string> moved = session_->selection().clips();
    const auto delta_seconds = SecondsOf(drag_.delta);
    for (const auto& box : layout_.clips) {
      if (moved.count(box.clip_id) == 0) continue;
      const auto* row = row_by_id(box.track_id);
      double y = ruler_height_ + box.y - scroll_y_;
      if (drag_.track_delta != 0 && row != nullptr) {
        // The row the clip would land on, counting rows of its own kind.
        std::vector<const ui::TrackRow*> same;
        for (const auto& r : layout_.tracks) {
          if (r.kind == row->kind) same.push_back(&r);
        }
        const auto index = std::find(same.begin(), same.end(), row) - same.begin();
        const int target = static_cast<int>(index) + (row->kind == model::TrackKind::Video ? -drag_.track_delta : drag_.track_delta);
        if (target >= 0 && target < static_cast<int>(same.size())) y = ruler_height_ + same[static_cast<std::size_t>(target)]->y + 1.0 - scroll_y_;
      }
      QRectF ghost(header_width_ + box.x + delta_seconds * viewport_.pixels_per_second, y, box.width, box.height);
      painter->setPen(QPen(ok ? QColor("#d9ff62") : QColor("#ff6b6b"), 2.0, Qt::DashLine));
      painter->setBrush(ok ? QColor(217, 255, 98, 40) : QColor(255, 107, 107, 50));
      painter->drawRoundedRect(ghost, 3, 3);
    }
  }
  if ((drag_.kind == Drag::Trim || drag_.kind == Drag::Roll) && drag_.plan.ok == false && !drag_.plan.refusal.empty() && drag_.result_time == std::nullopt) {
    // nothing to draw: the refusal shows in the status line
  }
  if (drag_.result_time) {
    const double x = header_width_ + viewport_.TimeToX(*drag_.result_time);
    painter->setPen(QPen(drag_.plan.ok ? QColor("#ffd24d") : QColor("#ff6b6b"), 2.0));
    painter->drawLine(QPointF(x, ruler_height_), QPointF(x, height()));
  }
  if (drag_.snap_at) {
    const double x = header_width_ + viewport_.TimeToX(*drag_.snap_at);
    painter->setPen(QPen(QColor("#ffffff"), 1.0, Qt::DotLine));
    painter->drawLine(QPointF(x, ruler_height_), QPointF(x, height()));
  }
  if (drag_.kind == Drag::Marquee) {
    const QRectF rect = QRectF(drag_.press, drag_.current).normalized();
    painter->setPen(QPen(QColor("#d9ff62"), 1.0));
    painter->setBrush(QColor(217, 255, 98, 30));
    painter->drawRect(rect);
  }

  // The playhead through every track.
  const double playhead_x = header_width_ + viewport_.SecondsToX(session_->playheadSeconds());
  if (playhead_x >= header_width_ && playhead_x <= width()) {
    painter->setPen(QPen(QColor("#ff5a4f"), 1.5));
    painter->drawLine(QPointF(playhead_x, ruler_height_), QPointF(playhead_x, height()));
  }
  painter->restore();

  // Track headers.
  painter->fillRect(QRectF(0, ruler_height_, header_width_, height() - ruler_height_), QColor("#191b1b"));
  painter->save();
  painter->setClipRect(QRectF(0, ruler_height_, header_width_, height() - ruler_height_));
  for (const auto& row : layout_.tracks) {
    const double y = ruler_height_ + row.y - scroll_y_;
    if (y + row.height < ruler_height_ || y > height()) continue;
    painter->setPen(QColor("#2a2d2d"));
    painter->drawLine(QPointF(0, y + row.height), QPointF(header_width_, y + row.height));
    painter->setFont(normal);
    painter->setPen(QColor("#d6dad8"));
    painter->drawText(QRectF(8, y, 56, row.height), Qt::AlignLeft | Qt::AlignVCenter, QString::fromStdString(row.name.empty() ? row.id : row.name));
    const char* letters[3] = {"M", "S", "L"};
    const bool flags[3] = {row.muted, row.solo, row.locked};
    const QColor on[3] = {QColor("#ff9b4f"), QColor("#ffd24d"), QColor("#9aa0ff")};
    for (int i = 0; i < 3; ++i) {
      const QRectF button(header_width_ - 24.0 * (3 - i) - 2, y + (row.height - 18) / 2.0, 20, 18);
      painter->setPen(QColor("#3a3d3d"));
      painter->setBrush(flags[i] ? on[i] : QColor("#222424"));
      painter->drawRoundedRect(button, 3, 3);
      painter->setPen(flags[i] ? QColor("#101111") : QColor("#8a9290"));
      painter->setFont(small);
      painter->drawText(button, Qt::AlignCenter, letters[i]);
    }
  }
  painter->restore();

  // The ruler.
  painter->fillRect(QRectF(0, 0, width(), ruler_height_), QColor("#1c1e1e"));
  painter->save();
  painter->setClipRect(QRectF(header_width_, 0, width() - header_width_, ruler_height_));
  if (session_->markInTime() || session_->markOutTime()) {
    const double a = header_width_ + viewport_.SecondsToX(session_->markInTime() ? SecondsOf(*session_->markInTime()) : 0.0);
    const double b = session_->markOutTime() ? header_width_ + viewport_.SecondsToX(SecondsOf(*session_->markOutTime())) : width();
    painter->fillRect(QRectF(a, ruler_height_ - 8, std::max(1.0, b - a), 8), QColor(80, 140, 255, 110));
  }
  painter->setFont(small);
  for (const auto& tick : ui::BuildRuler(viewport_, Rate(), sequence->drop_frame)) {
    const double x = header_width_ + tick.x;
    painter->setPen(tick.major ? QColor("#8a9290") : QColor("#454a49"));
    painter->drawLine(QPointF(x, ruler_height_ - (tick.major ? 14 : 6)), QPointF(x, ruler_height_));
    if (tick.major) {
      painter->setPen(QColor("#9aa09e"));
      painter->drawText(QPointF(x + 3, 11), QString::fromStdString(tick.label));
    }
  }
  for (const auto& marker : session_->markers()) {
    const double x = header_width_ + viewport_.TimeToX(marker);
    QPainterPath diamond;
    diamond.moveTo(x, ruler_height_ - 12);
    diamond.lineTo(x + 5, ruler_height_ - 7);
    diamond.lineTo(x, ruler_height_ - 2);
    diamond.lineTo(x - 5, ruler_height_ - 7);
    diamond.closeSubpath();
    painter->fillPath(diamond, QColor("#ffd24d"));
  }
  if (playhead_x >= header_width_ && playhead_x <= width()) {
    QPainterPath head;
    head.moveTo(playhead_x - 6, 2);
    head.lineTo(playhead_x + 6, 2);
    head.lineTo(playhead_x + 6, 12);
    head.lineTo(playhead_x, ruler_height_ - 2);
    head.lineTo(playhead_x - 6, 12);
    head.closeSubpath();
    painter->fillPath(head, QColor("#ff5a4f"));
  }
  painter->restore();
  painter->setPen(QColor("#2a2d2d"));
  painter->drawLine(QPointF(0, ruler_height_), QPointF(width(), ruler_height_));
  painter->drawLine(QPointF(header_width_, 0), QPointF(header_width_, height()));
  // Timecode in the corner above the headers.
  painter->setFont(QFont("Consolas", 11));
  painter->setPen(QColor("#d9ff62"));
  painter->drawText(QRectF(6, 0, header_width_ - 8, ruler_height_), Qt::AlignLeft | Qt::AlignVCenter, session_->timecode());
}

// ------------------------------------------------------------------------- mouse ----

void TimelineItem::SeekToX(double x) {
  const auto time = viewport_.XToTime(TrackAreaX(x), Rate());
  session_->SeekTo(time);
}

void TimelineItem::hoverMoveEvent(QHoverEvent* event) {
  if (session_ == nullptr || session_->sequence() == nullptr) return;
  const auto pos = event->position();
  Qt::CursorShape shape = Qt::ArrowCursor;
  const auto tool = session_->tool();
  if (pos.x() >= header_width_ && pos.y() >= ruler_height_) {
    const auto hit = ui::HitTest(layout_, viewport_, Options(), TrackAreaX(pos.x()), TrackAreaY(pos.y()), kEdgePx);
    if (tool == "razor") shape = Qt::CrossCursor;
    else if (tool == "hand") shape = Qt::OpenHandCursor;
    else if (hit.kind == ui::HitKind::ClipHead || hit.kind == ui::HitKind::ClipTail || hit.kind == ui::HitKind::EditPoint) shape = Qt::SizeHorCursor;
    else if (hit.kind == ui::HitKind::ClipBody) shape = Qt::SizeAllCursor;
  }
  setCursor(shape);
}

void TimelineItem::mousePressEvent(QMouseEvent* event) {
  if (session_ == nullptr || session_->sequence() == nullptr) return;
  forceActiveFocus();
  drag_ = {};
  drag_.press = drag_.current = event->position();
  BeginDrag(event);
  update();
}

void TimelineItem::BeginDrag(QMouseEvent* event) {
  const auto pos = event->position();
  const auto* sequence = session_->sequence();
  const auto modifiers = event->modifiers();
  const auto tool = session_->tool();
  const bool linked = !(modifiers & Qt::AltModifier);

  if (event->button() == Qt::MiddleButton || tool == "hand") {
    drag_.kind = Drag::Pan;
    drag_.press_scroll_seconds = viewport_.scroll_seconds;
    drag_.press_scroll_y = scroll_y_;
    setCursor(Qt::ClosedHandCursor);
    return;
  }
  // Header buttons.
  if (pos.x() < header_width_ && pos.y() >= ruler_height_) {
    const double y = TrackAreaY(pos.y());
    if (const auto* row = RowAtY(y)) {
      const double row_top = ruler_height_ + row->y - scroll_y_;
      const char* flags[3] = {"muted", "solo", "locked"};
      const bool states[3] = {row->muted, row->solo, row->locked};
      for (int i = 0; i < 3; ++i) {
        const QRectF button(header_width_ - 24.0 * (3 - i) - 2, row_top + (row->height - 18) / 2.0, 20, 18);
        if (button.contains(pos)) {
          session_->setTrackFlag(QString::fromStdString(row->id), flags[i], !states[i]);
          return;
        }
      }
    }
    return;
  }
  if (pos.y() < ruler_height_) {
    if (pos.x() >= header_width_) {
      drag_.kind = Drag::Scrub;
      SeekToX(pos.x());
    }
    return;
  }
  if (tool != "razor" && tool != "zoom" && tool != "track_select" && HitRampHandle(pos)) return;
  const double tx = TrackAreaX(pos.x());
  const double ty = TrackAreaY(pos.y());
  const auto hit = ui::HitTest(layout_, viewport_, Options(), tx, ty, kEdgePx);

  if (tool == "zoom") {
    viewport_.ZoomAt(tx, (modifiers & Qt::AltModifier) ? 0.5 : 2.0);
    Rebuild();
    emit viewChanged();
    return;
  }
  if (tool == "razor") {
    if (!hit.clip_id.empty() && hit.kind != ui::HitKind::EditPoint) {
      std::optional<RationalTime> snapped;
      const auto at = SnappedTime(pos.x(), {}, snapped);
      (void)session_->Apply(ui::PlanSplitClips(Context(linked), at, {hit.clip_id}));
    }
    return;
  }
  if (tool == "track_select") {
    if (!hit.track_id.empty()) {
      const auto mode = (modifiers & Qt::ShiftModifier) ? ui::Selection::Mode::Add : ui::Selection::Mode::Replace;
      session_->selection().SelectTrackForward(*sequence, hit.track_id, viewport_.XToTime(tx, Rate()), mode);
      session_->NotifySelectionChanged();
    }
    return;
  }

  const bool toggle = (modifiers & Qt::ShiftModifier) != 0;
  const bool link_selection = session_->prefs().GetBool("timeline.linked_selection") && linked;
  switch (hit.kind) {
    case ui::HitKind::ClipBody:
    case ui::HitKind::ClipHead:
    case ui::HitKind::ClipTail:
    case ui::HitKind::EditPoint: {
      const bool edge_gesture = hit.kind != ui::HitKind::ClipBody && tool != "slip" && tool != "slide";
      if (!edge_gesture || hit.kind == ui::HitKind::EditPoint) {
        // Select first, so a drag moves what is selected.
        if (!session_->selection().Contains(hit.clip_id) || toggle) {
          session_->selection().Select(*sequence, hit.clip_id, toggle ? ui::Selection::Mode::Toggle : ui::Selection::Mode::Replace, link_selection);
          session_->NotifySelectionChanged();
        }
      } else if (!session_->selection().Contains(hit.clip_id)) {
        session_->selection().Select(*sequence, hit.clip_id, ui::Selection::Mode::Replace, link_selection);
        session_->NotifySelectionChanged();
      }
      if (toggle && !session_->selection().Contains(hit.clip_id)) return;
      drag_.clip_id = hit.clip_id;
      drag_.linked = linked;
      drag_.insert = (modifiers & Qt::ControlModifier) != 0;
      if (hit.kind == ui::HitKind::EditPoint) {
        drag_.other_clip_id = hit.other_clip_id;
        if (tool == "ripple") {
          // Ripple at a cut trims whichever side the pointer is on.
          drag_.kind = Drag::Trim;
          drag_.trim_mode = ui::TrimMode::Ripple;
          const auto cut = viewport_.TimeToX(sequence->FindTrack(hit.track_id) != nullptr ? RationalTime(0, 1) : RationalTime(0, 1));
          (void)cut;
          const double boundary_x = [&] {
            for (const auto& box : layout_.clips) {
              if (box.clip_id == hit.clip_id) return box.x;
            }
            return tx;
          }();
          if (tx < boundary_x) {
            drag_.clip_id = hit.other_clip_id;
            drag_.edge = ui::TrimEdge::Tail;
          } else {
            drag_.edge = ui::TrimEdge::Head;
          }
        } else {
          drag_.kind = Drag::Roll;
        }
      } else if (hit.kind == ui::HitKind::ClipHead || hit.kind == ui::HitKind::ClipTail) {
        if (tool == "slip") drag_.kind = Drag::Slip;
        else if (tool == "slide") drag_.kind = Drag::Slide;
        else {
          drag_.kind = Drag::Trim;
          drag_.edge = hit.kind == ui::HitKind::ClipHead ? ui::TrimEdge::Head : ui::TrimEdge::Tail;
          drag_.trim_mode = tool == "ripple" ? ui::TrimMode::Ripple : ui::TrimMode::Normal;
        }
      } else {
        drag_.kind = tool == "slip" ? Drag::Slip : tool == "slide" ? Drag::Slide : Drag::Move;
        if (tool == "ripple" || tool == "roll") drag_.kind = Drag::Move;
      }
      break;
    }
    case ui::HitKind::Transition:
      break;
    default:
      // Empty space: a rectangle to select with, and a click clears.
      drag_.kind = Drag::Marquee;
      drag_.marquee_mode = toggle ? ui::Selection::Mode::Add : ui::Selection::Mode::Replace;
      if (!toggle) {
        session_->selection().Clear();
        session_->NotifySelectionChanged();
      }
      break;
  }
}

QPointF TimelineItem::rampHandlePosition(const QString& clip_id, int boundary, bool after) const {
  const auto it = ramps_.find(clip_id.toStdString());
  if (it == ramps_.end() || it->second.empty()) return QPointF(-1, -1);
  for (const auto& box : layout_.clips) {
    if (box.clip_id != clip_id.toStdString()) continue;
    const QRectF rect(header_width_ + box.x, ruler_height_ + box.y - scroll_y_, box.width, box.height);
    const auto band = RampBand(rect);
    const auto range = RampSpeedRange(it->second);
    const double total = SecondsOf(it->second.duration());
    for (const auto& handle : RampHandles(it->second)) {
      if (handle.boundary != boundary) continue;
      if (handle.side != ui::RampModel::Side::Both && (handle.side == ui::RampModel::Side::After) != after) continue;
      return QPointF(band.left() + handle.seconds / total * band.width(), band.top() + (range.second - handle.speed) / (range.second - range.first) * band.height());
    }
  }
  return QPointF(-1, -1);
}

bool TimelineItem::HitRampHandle(const QPointF& pos) {
  for (const auto& box : layout_.clips) {
    const auto it = ramps_.find(box.clip_id);
    if (it == ramps_.end() || it->second.empty()) continue;
    const QRectF rect(header_width_ + box.x, ruler_height_ + box.y - scroll_y_, box.width, box.height);
    const auto band = RampBand(rect);
    if (band.height() < 24.0 || band.width() < 40.0 || !rect.adjusted(-6, -6, 6, 6).contains(pos)) continue;
    const auto range = RampSpeedRange(it->second);
    const double total = SecondsOf(it->second.duration());
    for (const auto& handle : RampHandles(it->second)) {
      const QPointF at(band.left() + handle.seconds / total * band.width(), band.top() + (range.second - handle.speed) / (range.second - range.first) * band.height());
      if (std::abs(at.x() - pos.x()) > 6.0 || std::abs(at.y() - pos.y()) > 6.0) continue;
      drag_.kind = Drag::RampHandle;
      drag_.clip_id = box.clip_id;
      drag_.ramp_before = it->second;
      drag_.ramp = it->second;
      drag_.ramp_boundary = handle.boundary;
      drag_.ramp_side = handle.side;
      drag_.range_lo = range.first;
      drag_.range_hi = range.second;
      drag_.band = band;
      if (!session_->selection().Contains(box.clip_id)) {
        session_->selection().Select(*session_->sequence(), box.clip_id, ui::Selection::Mode::Replace, false);
        session_->NotifySelectionChanged();
      }
      return true;
    }
  }
  return false;
}

void TimelineItem::mouseDoubleClickEvent(QMouseEvent* event) {
  if (session_ == nullptr || session_->sequence() == nullptr) return;
  const auto pos = event->position();
  if (pos.x() < header_width_ || pos.y() < ruler_height_) return;
  const auto hit = ui::HitTest(layout_, viewport_, Options(), TrackAreaX(pos.x()), TrackAreaY(pos.y()), kEdgePx);
  // A double-click on a transition opens it.
  if (hit.kind == ui::HitKind::Transition && !hit.transition_id.empty()) {
    session_->openTransition(QString::fromStdString(hit.transition_id));
    return;
  }
  if (hit.clip_id.empty() || ramps_.find(hit.clip_id) == ramps_.end()) return;
  // A double-click on the speed band of a clip that has a ramp opens its graph.
  session_->selection().Select(*session_->sequence(), hit.clip_id, ui::Selection::Mode::Replace, false);
  session_->NotifySelectionChanged();
  (void)session_->rampOpen();
}

void TimelineItem::mouseMoveEvent(QMouseEvent* event) {
  if (session_ == nullptr || drag_.kind == Drag::None) return;
  drag_.current = event->position();
  UpdateDrag(event);
  update();
}

void TimelineItem::UpdateDrag(QMouseEvent* event) {
  const auto* sequence = session_->sequence();
  const auto rate = Rate();
  const double dx_px = drag_.current.x() - drag_.press.x();
  const auto linked = drag_.linked;
  drag_.snap_at.reset();
  drag_.result_time.reset();
  switch (drag_.kind) {
    case Drag::Pan: {
      viewport_.scroll_seconds = std::max(0.0, drag_.press_scroll_seconds - dx_px / viewport_.pixels_per_second);
      scroll_y_ = drag_.press_scroll_y - (drag_.current.y() - drag_.press.y());
      Rebuild();
      emit viewChanged();
      break;
    }
    case Drag::Scrub:
      SeekToX(std::clamp(drag_.current.x(), header_width_, width()));
      // Dragging past an edge scrolls.
      if (drag_.current.x() > width() - 20) viewport_.ScrollBySeconds(0.1 * 40.0 / viewport_.pixels_per_second * 4.0);
      else if (drag_.current.x() < header_width_ + 20) viewport_.ScrollBySeconds(-0.4 * 40.0 / viewport_.pixels_per_second);
      Rebuild();
      break;
    case Drag::Marquee: {
      const double x0 = TrackAreaX(drag_.press.x()), y0 = TrackAreaY(drag_.press.y());
      const double x1 = TrackAreaX(drag_.current.x()), y1 = TrackAreaY(drag_.current.y());
      session_->selection().SelectRect(layout_, x0, y0, x1, y1, drag_.marquee_mode, session_->prefs().GetBool("timeline.linked_selection") ? sequence : nullptr);
      session_->NotifySelectionChanged();
      break;
    }
    case Drag::Move: {
      auto delta = FramesToTime(dx_px / viewport_.pixels_per_second, rate);
      // Alt drags the clip alone, leaving what is linked to it where it is.
      const auto selected = linked ? session_->selection().clips() : std::set<std::string>{drag_.clip_id};
      // Snap by the clip being dragged: its start or its end, whichever finds a point.
      const ui::ClipBox* primary = nullptr;
      for (const auto& box : layout_.clips) {
        if (box.clip_id == drag_.clip_id) primary = &box;
      }
      if (primary != nullptr && session_->snap()) {
        const auto duration = primary->end.Subtract(primary->start);
        const auto result = ui::SnapSpan(primary->start.Add(delta), duration, SnapPoints(ui::WithLinked(*sequence, selected)), viewport_,
                                         static_cast<double>(session_->prefs().GetInt("timeline.snap_distance_px")));
        if (result.snapped) {
          delta = result.start.Subtract(primary->start);
          drag_.snap_at = result.end_snapped ? result.start.Add(duration) : result.start;
        }
      }
      // Rows moved, counted in the primary clip's own kind.
      int track_delta = 0;
      const auto* press_row = RowAtY(TrackAreaY(drag_.press.y()));
      const auto* now_row = RowAtY(TrackAreaY(drag_.current.y()));
      if (press_row != nullptr && now_row != nullptr && press_row->kind == now_row->kind) {
        std::vector<const ui::TrackRow*> same;
        for (const auto& r : layout_.tracks) {
          if (r.kind == press_row->kind) same.push_back(&r);
        }
        const auto a = std::find(same.begin(), same.end(), press_row) - same.begin();
        const auto b = std::find(same.begin(), same.end(), now_row) - same.begin();
        const int rows = static_cast<int>(b - a);
        track_delta = press_row->kind == model::TrackKind::Video ? -rows : rows;
      }
      drag_.delta = delta;
      drag_.track_delta = track_delta;
      drag_.plan = ui::PlanMove(Context(linked), selected, drag_.clip_id, delta, track_delta, drag_.insert ? ui::OverlapMode::Insert : ui::OverlapMode::Overwrite);
      break;
    }
    case Drag::Trim: {
      std::optional<RationalTime> snapped;
      const auto at = SnappedTime(drag_.current.x(), {drag_.clip_id}, snapped);
      drag_.snap_at = snapped;
      drag_.plan = ui::PlanTrim(Context(linked), drag_.clip_id, drag_.edge, at, drag_.trim_mode);
      drag_.result_time = drag_.plan.result_time;
      break;
    }
    case Drag::Roll: {
      std::optional<RationalTime> snapped;
      const auto at = SnappedTime(drag_.current.x(), {drag_.clip_id, drag_.other_clip_id}, snapped);
      drag_.snap_at = snapped;
      drag_.plan = ui::PlanRoll(Context(linked), drag_.other_clip_id, drag_.clip_id, at);
      drag_.result_time = drag_.plan.result_time;
      break;
    }
    case Drag::Slip: {
      // Dragging right shows earlier media, as the picture seems to slide with the pointer.
      const auto timeline_delta = FramesToTime(dx_px / viewport_.pixels_per_second, rate);
      time::RationalTime rate_of_clip(1, 1);
      for (const auto& track : sequence->tracks) {
        for (const auto& clip : track.clips) {
          if (clip.id == drag_.clip_id) rate_of_clip = clip.playback_rate;
        }
      }
      drag_.plan = ui::PlanSlip(Context(false), drag_.clip_id, time::RationalTime(0, 1).Subtract(timeline_delta.Multiply(rate_of_clip)));
      break;
    }
    case Drag::Slide: {
      const auto delta = FramesToTime(dx_px / viewport_.pixels_per_second, rate);
      drag_.delta = delta;
      drag_.plan = ui::PlanSlide(Context(linked), drag_.clip_id, delta);
      break;
    }
    case Drag::RampHandle: {
      // Sideways moves the cut (an end stays where it is); up and down sets the speed there, to a twentieth.
      auto ramp = drag_.ramp_before;
      const double total = SecondsOf(ramp.duration());
      const bool inner = drag_.ramp_boundary > 0 && drag_.ramp_boundary < static_cast<int>(ramp.segments().size());
      if (inner && drag_.band.width() > 0.0) {
        const double seconds = (drag_.current.x() - drag_.band.left()) / drag_.band.width() * total;
        (void)ramp.MoveBoundary(drag_.ramp_boundary, FramesToTime(seconds, rate));
      }
      if (drag_.band.height() > 0.0) {
        const double speed = drag_.range_hi - (drag_.current.y() - drag_.band.top()) / drag_.band.height() * (drag_.range_hi - drag_.range_lo);
        (void)ramp.SetBoundarySpeed(drag_.ramp_boundary, std::round(speed * 20.0) / 20.0, drag_.ramp_side);
      }
      drag_.ramp = ramp;
      break;
    }
    case Drag::None:
      break;
  }
  (void)event;
}

void TimelineItem::mouseReleaseEvent(QMouseEvent* event) {
  if (session_ == nullptr) return;
  drag_.current = event->position();
  EndDrag(event);
  drag_ = {};
  setCursor(Qt::ArrowCursor);
  update();
}

void TimelineItem::EndDrag(QMouseEvent*) {
  switch (drag_.kind) {
    case Drag::Move:
    case Drag::Trim:
    case Drag::Roll:
    case Drag::Slip:
    case Drag::Slide:
      // A click without a drag does nothing; a drag runs what the rules planned, or says why not.
      if (std::abs(drag_.current.x() - drag_.press.x()) >= 2.0 || std::abs(drag_.current.y() - drag_.press.y()) >= 6.0) {
        if (!drag_.plan.commands.empty() || !drag_.plan.ok) (void)session_->Apply(drag_.plan);
      }
      break;
    case Drag::RampHandle:
      // One undoable step, and only if the pointer went anywhere.
      if (std::abs(drag_.current.x() - drag_.press.x()) >= 2.0 || std::abs(drag_.current.y() - drag_.press.y()) >= 2.0) (void)session_->ApplyRamp(drag_.clip_id, drag_.ramp);
      break;
    case Drag::Marquee:
      session_->NotifySelectionChanged();
      break;
    default:
      break;
  }
}

void TimelineItem::wheelEvent(QWheelEvent* event) {
  if (session_ == nullptr) return;
  const double delta = event->angleDelta().y();
  if (event->modifiers() & Qt::ControlModifier) {
    viewport_.ZoomAt(TrackAreaX(event->position().x()), delta > 0 ? 1.25 : 0.8);
  } else if (event->modifiers() & Qt::ShiftModifier) {
    viewport_.ScrollByPixels(-delta);
  } else {
    scroll_y_ -= delta / 2.0;
    // A sideways wheel (a trackpad) scrolls time.
    if (event->angleDelta().x() != 0) viewport_.ScrollByPixels(-event->angleDelta().x());
  }
  Rebuild();
  update();
  emit viewChanged();
}

bool TimelineItem::dropMedia(double x, double y, const QString& media_id, bool insert) {
  if (session_ == nullptr || session_->sequence() == nullptr) return false;
  const auto time = viewport_.XToTime(TrackAreaX(x), Rate());
  const auto* row = RowAtY(TrackAreaY(y));
  std::string video, audio;
  std::int64_t video_order = 1 << 30, audio_order = 1 << 30;
  for (const auto& track : session_->sequence()->tracks) {
    if (track.locked || track.is_bus) continue;
    if (track.kind == model::TrackKind::Video && track.order < video_order) { video = track.id; video_order = track.order; }
    if (track.kind == model::TrackKind::Audio && track.order < audio_order) { audio = track.id; audio_order = track.order; }
  }
  if (row != nullptr) {
    // Dropped on a row: that row takes its kind of the clip, and the other kind goes to the default track.
    if (row->kind == model::TrackKind::Video) video = row->id;
    else audio = row->id;
  }
  return session_->Apply(session_->InsertPlanAt(media_id.toStdString(), time, video, audio, insert));
}

}  // namespace cutline::app

// Masks in the application: choosing the effect a mask belongs to, drawing a rectangle, an ellipse or a path on the monitor,
// dragging its handles, keying its values, and making it follow a track. What a drag does to a shape is ui/MaskEditor.h;
// this keeps the gesture in progress, turns the finished one into one undoable edit, and answers the monitor's questions.

#include "app/Session.h"

#include "core/db/Sql.h"
#include "render/TrackingData.h"
#include "ui/MaskEditor.h"

#include <QMetaObject>

#include <algorithm>
#include <cmath>

namespace cutline::app {

using time::RationalTime;

namespace {

double SecondsOf(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(std::max<std::int64_t>(1, t.denominator())); }

struct Found final {
  const timeline::Clip* clip{nullptr};
  const timeline::Effect* effect{nullptr};
  const timeline::EffectMask* mask{nullptr};
};

Found FindMaskIn(const timeline::Sequence& sequence, const std::string& mask_id) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      for (const auto& effect : clip.effects) {
        for (const auto& mask : effect.masks) {
          if (mask.id == mask_id) return {&clip, &effect, &mask};
        }
      }
    }
  }
  return {};
}

QVariantList PairList(const std::vector<std::pair<double, double>>& points) {
  QVariantList list;
  for (const auto& [x, y] : points) list << QVariant(QVariantList{x, y});   // wrapped: appending a list to a list would flatten it
  return list;
}

}  // namespace

// ------------------------------------------------------------------ what is shown ----

ui::PictureSize Session::MaskPicture() const {
  const auto* sequence = graph_.root();
  return sequence == nullptr ? ui::PictureSize{} : ui::PictureSize{static_cast<double>(sequence->width), static_cast<double>(sequence->height)};
}

double Session::MaskLocalSeconds(const timeline::Clip& clip) const {
  const auto local = transport_.position().Subtract(clip.timeline_start);
  return local.Compare(RationalTime(0, 1)) < 0 ? 0.0 : SecondsOf(local);
}

QVariantMap Session::maskMap(const timeline::EffectMask& mask, const timeline::Clip& clip) const {
  const auto evaluated = effects::mask::Evaluate(mask.document, MaskLocalSeconds(clip));
  QVariantList animated;
  QVariantList key_here;
  for (const auto& animation : mask.document.animations) {
    animated << QString::fromStdString(animation.property);
    for (const auto& key : animation.keys) {
      if (std::abs(key.time - MaskLocalSeconds(clip)) < 1e-3) key_here << QString::fromStdString(animation.property);
    }
  }
  QVariantMap map;
  map["id"] = QString::fromStdString(mask.id);
  map["shape"] = effects::mask::ToString(mask.document.shape);
  map["combine"] = effects::mask::ToString(mask.document.combine);
  map["inverted"] = mask.document.inverted;
  map["feather"] = evaluated.feather;
  map["expansion"] = evaluated.expansion;
  map["opacity"] = evaluated.opacity;
  map["animated"] = animated;
  map["keyHere"] = key_here;
  map["points"] = static_cast<int>(mask.document.points.size());
  map["active"] = mask.id == mask_id_;
  return map;
}

QVariantList Session::masksOf(const std::string& effect_id) const {
  QVariantList list;
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return list;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      for (const auto& effect : clip.effects) {
        if (effect.id != effect_id) continue;
        for (const auto& mask : effect.masks) list << maskMap(mask, clip);
      }
    }
  }
  return list;
}

QVariantMap Session::maskOverlay() const {
  QVariantMap overlay;
  overlay["tool"] = mask_tool_;
  overlay["penPoints"] = PairList(pen_points_);
  if (gesture_.drawing) {
    overlay["outline"] = PairList(ui::Outline(gesture_.preview_evaluated, MaskPicture()));
    overlay["active"] = true;
    return overlay;
  }
  const auto* sequence = graph_.root();
  if (mask_id_.empty() || sequence == nullptr) {
    overlay["active"] = false;
    return overlay;
  }
  const auto found = FindMaskIn(*sequence, mask_id_);
  if (found.mask == nullptr) {
    overlay["active"] = false;
    return overlay;
  }
  const auto evaluated = gesture_.active ? gesture_.preview_evaluated : effects::mask::Evaluate(found.mask->document, MaskLocalSeconds(*found.clip));
  overlay["active"] = true;
  overlay["outline"] = PairList(ui::Outline(evaluated, MaskPicture()));
  QVariantList handles;
  for (const auto& h : ui::Handles(evaluated, MaskPicture(), mask_point_)) {
    QVariantMap map;
    map["kind"] = static_cast<int>(h.handle.kind);
    map["index"] = h.handle.index;
    map["x"] = h.x;
    map["y"] = h.y;
    map["selected"] = h.handle.kind == ui::MaskHandle::Kind::Point && h.handle.index == mask_point_;
    handles << map;
  }
  overlay["handles"] = handles;
  overlay["smooth"] = ui::IsSmooth(evaluated, mask_point_);
  return overlay;
}

bool Session::maskInteractive() const { return mask_tool_ != "none" || !mask_id_.empty() || !pen_points_.empty(); }

void Session::maskSetTool(const QString& tool) {
  if (mask_tool_ == tool) return;
  mask_tool_ = tool;
  pen_points_.clear();
  gesture_ = {};
  emit maskChanged();
}

void Session::maskSelect(const QString& id) {
  mask_id_ = id.toStdString();
  mask_point_ = -1;
  if (!id.isEmpty() && mask_tool_ == "none") mask_tool_ = "select";
  gesture_ = {};
  RefreshInspector();
  emit maskChanged();
}

void Session::maskSetTarget(const QString& effect_id) {
  mask_effect_ = effect_id.toStdString();
  emit maskChanged();
}

// --------------------------------------------------------------------- the edits ----

bool Session::MaskApply(const ui::EditPlan& plan) {
  const bool ok = Apply(plan);
  emit maskChanged();
  return ok;
}

namespace {
// The effect new masks go on: the one asked for, else the clip's first effect that is not one of its own controls.
const timeline::Effect* TargetEffect(const timeline::Clip& clip, const std::string& wanted) {
  for (const auto& effect : clip.effects) {
    if (effect.id == wanted) return &effect;
  }
  for (const auto& effect : clip.effects) {
    if (!effect.intrinsic) return &effect;
  }
  return nullptr;
}
}  // namespace

bool Session::maskAdd(const QString& effect_id, const QString& shape) {
  const auto clip_id = PrimaryClip();
  const auto* sequence = graph_.root();
  if (!clip_id || sequence == nullptr) {
    ShowStatus(tr("Select a clip to mask"));
    return false;
  }
  const timeline::Clip* clip = nullptr;
  for (const auto& track : sequence->tracks) {
    for (const auto& candidate : track.clips) {
      if (candidate.id == *clip_id) clip = &candidate;
    }
  }
  const auto* target = clip == nullptr ? nullptr : TargetEffect(*clip, effect_id.toStdString());
  if (target == nullptr) {
    ShowStatus(tr("Add an effect to the clip first: a mask limits where an effect applies"));
    return false;
  }
  const auto picture = MaskPicture();
  ui::MaskDocument document = shape == "ellipse" ? ui::NewEllipse(0.3, 0.3, 0.7, 0.7, picture)
                              : shape == "path"  ? ui::NewPath({{0.3, 0.3}, {0.7, 0.3}, {0.7, 0.7}, {0.3, 0.7}})
                                                 : ui::NewRectangle(0.3, 0.3, 0.7, 0.7, picture);
  const auto plan = ui::PlanAddMask(EditContextFor(), *clip_id, target->id, document);
  if (!plan.ok) {
    ShowStatus(QString::fromStdString(plan.refusal));
    return false;
  }
  const auto created = std::get<commands::AddMaskPayload>(plan.commands[0].payload).id;
  if (!MaskApply(plan)) return false;
  maskSelect(QString::fromStdString(created));
  return true;
}

bool Session::maskRemove(const QString& id) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return false;
  const auto found = FindMaskIn(*sequence, id.toStdString());
  if (found.mask == nullptr) return false;
  const bool ok = MaskApply(ui::PlanRemoveMask(EditContextFor(), found.clip->id, id.toStdString()));
  if (ok && mask_id_ == id.toStdString()) maskSelect({});
  return ok;
}

bool Session::MaskEdit(const std::string& mask_id, const std::function<ui::MaskDocument(const ui::MaskDocument& stored, const ui::MaskDocument& evaluated, double seconds)>& change, const std::string& label) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return false;
  const auto found = FindMaskIn(*sequence, mask_id);
  if (found.mask == nullptr) return false;
  const auto seconds = MaskLocalSeconds(*found.clip);
  const auto evaluated = effects::mask::Evaluate(found.mask->document, seconds);
  ui::MaskDocument edited;
  try {
    edited = change(found.mask->document, evaluated, seconds);
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
    return false;
  }
  return MaskApply(ui::PlanUpdateMask(EditContextFor(), found.clip->id, mask_id, edited, label));
}

bool Session::maskSetNumber(const QString& id, const QString& property, double value) {
  const auto name = property.toStdString();
  return MaskEdit(id.toStdString(), [&](const ui::MaskDocument& stored, const ui::MaskDocument& evaluated, double seconds) {
    auto edited = evaluated;
    if (name == "feather") edited.feather = std::max(0.0, value);
    else if (name == "expansion") edited.expansion = value;
    else if (name == "opacity") edited.opacity = std::clamp(value, 0.0, 1.0);
    else throw std::invalid_argument("A mask has no value called " + name);
    return ui::ApplyEdit(stored, evaluated, edited, seconds);
  }, "Edit Mask");
}

bool Session::maskSetMode(const QString& id, const QString& combine, bool inverted) {
  return MaskEdit(id.toStdString(), [&](const ui::MaskDocument& stored, const ui::MaskDocument&, double) {
    auto edited = stored;
    edited.combine = combine == "subtract" ? effects::mask::Combine::Subtract : combine == "intersect" ? effects::mask::Combine::Intersect : effects::mask::Combine::Add;
    edited.inverted = inverted;
    return edited;
  }, "Edit Mask");
}

bool Session::maskToggleKey(const QString& id, const QString& property) {
  const auto name = property.toStdString();
  return MaskEdit(id.toStdString(), [&](const ui::MaskDocument& stored, const ui::MaskDocument& evaluated, double seconds) {
    if (!ui::IsAnimated(stored, name)) return ui::StartAnimating(stored, name, seconds);
    // Animated: a key at this time is removed (the last one ends the animation); otherwise one is added holding what the
    // value is now.
    auto result = stored;
    for (auto& animation : result.animations) {
      if (animation.property != name) continue;
      for (const auto& key : animation.keys) {
        if (std::abs(key.time - seconds) < 1e-3) return ui::RemoveKey(stored, name, key.time);
      }
      animation.keys.push_back({seconds, ui::ValueOf(evaluated, name)});
      std::sort(animation.keys.begin(), animation.keys.end(), [](const effects::mask::Key& x, const effects::mask::Key& y) { return x.time < y.time; });
    }
    return result;
  }, "Key Mask Value");
}

bool Session::maskStopAnimating(const QString& id, const QString& property) {
  const auto name = property.toStdString();
  return MaskEdit(id.toStdString(), [&](const ui::MaskDocument& stored, const ui::MaskDocument&, double seconds) { return ui::StopAnimating(stored, name, seconds); }, "Stop Animating Mask");
}

// ------------------------------------------------------------------ the gestures ----

namespace {
ui::MaskHandle ToolHit(const ui::MaskDocument& evaluated, ui::PictureSize picture, double x, double y, double tolerance, int selected) { return ui::HitTest(evaluated, picture, x, y, tolerance, selected); }
}  // namespace

bool Session::maskPress(double x, double y, double tolerance_pixels, int modifiers) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || !maskInteractive()) return false;
  const auto picture = MaskPicture();
  const bool control = (modifiers & Qt::ControlModifier) != 0;
  (void)control;
  if (mask_tool_ == "rectangle" || mask_tool_ == "ellipse") {
    gesture_ = {};
    gesture_.drawing = true;
    gesture_.press_x = gesture_.x = x;
    gesture_.press_y = gesture_.y = y;
    gesture_.preview_evaluated = mask_tool_ == "ellipse" ? ui::NewEllipse(x, y, x, y, picture) : ui::NewRectangle(x, y, x, y, picture);
    emit maskChanged();
    return true;
  }
  if (mask_tool_ == "pen") {
    // Close the path by clicking on its first point.
    if (pen_points_.size() >= 3 && std::hypot((x - pen_points_[0].first) * picture.width, (y - pen_points_[0].second) * picture.height) <= tolerance_pixels * 1.5) {
      maskFinishPen();
      return true;
    }
    pen_points_.push_back({x, y});
    emit maskChanged();
    return true;
  }
  if (mask_id_.empty()) return false;
  const auto found = FindMaskIn(*sequence, mask_id_);
  if (found.mask == nullptr) return false;
  const auto seconds = MaskLocalSeconds(*found.clip);
  const auto evaluated = effects::mask::Evaluate(found.mask->document, seconds);
  const auto handle = ToolHit(evaluated, picture, x, y, tolerance_pixels, mask_point_);
  if (handle.kind == ui::MaskHandle::Kind::None) {
    return false;   // a click away from the mask: the monitor goes on being a monitor
  }
  if (handle.kind == ui::MaskHandle::Kind::Point) mask_point_ = handle.index;
  gesture_ = {};
  gesture_.active = true;
  gesture_.handle = handle;
  gesture_.press_x = gesture_.x = x;
  gesture_.press_y = gesture_.y = y;
  gesture_.stored = found.mask->document;
  gesture_.evaluated = evaluated;
  gesture_.preview_evaluated = evaluated;
  gesture_.clip_id = found.clip->id;
  gesture_.seconds = seconds;
  emit maskChanged();
  return true;
}

bool Session::maskMove(double x, double y, int modifiers) {
  if (gesture_.drawing) {
    gesture_.x = x;
    gesture_.y = y;
    const auto picture = MaskPicture();
    const bool square = (modifiers & Qt::ShiftModifier) != 0;
    double ex = x, ey = y;
    if (square) {
      const double size = std::max(std::abs(x - gesture_.press_x) * picture.width, std::abs(y - gesture_.press_y) * picture.height);
      ex = gesture_.press_x + (x >= gesture_.press_x ? 1 : -1) * size / picture.width;
      ey = gesture_.press_y + (y >= gesture_.press_y ? 1 : -1) * size / picture.height;
    }
    gesture_.preview_evaluated = mask_tool_ == "ellipse" ? ui::NewEllipse(gesture_.press_x, gesture_.press_y, ex, ey, picture) : ui::NewRectangle(gesture_.press_x, gesture_.press_y, ex, ey, picture);
    emit maskChanged();
    return true;
  }
  if (!gesture_.active) return false;
  ui::DragOptions options;
  options.constrain = (modifiers & Qt::ShiftModifier) != 0;
  options.break_tangent = (modifiers & Qt::AltModifier) != 0;
  gesture_.x = x;
  gesture_.y = y;
  gesture_.preview_evaluated = ui::Drag(gesture_.evaluated, gesture_.handle, gesture_.press_x, gesture_.press_y, x, y, MaskPicture(), options);
  gesture_.moved = true;
  emit maskChanged();
  return true;
}

bool Session::maskRelease(double x, double y, int modifiers) {
  // Letting go where it was pressed is a click, not a drag of no distance (which would write an unchanged shape back).
  if (gesture_.drawing || std::abs(x - gesture_.x) > 1e-9 || std::abs(y - gesture_.y) > 1e-9) (void)maskMove(x, y, modifiers);
  if (gesture_.drawing) {
    const auto drawn = gesture_.preview_evaluated;
    const auto picture = MaskPicture();
    gesture_ = {};
    const bool big_enough = drawn.width * picture.width >= 4.0 || drawn.height * picture.height >= 4.0;
    emit maskChanged();
    if (!big_enough) return true;   // a click with the shape tool draws nothing
    const auto clip_id = PrimaryClip();
    const auto* sequence = graph_.root();
    if (!clip_id || sequence == nullptr) return true;
    const timeline::Clip* clip = nullptr;
    for (const auto& track : sequence->tracks) {
      for (const auto& candidate : track.clips) {
        if (candidate.id == *clip_id) clip = &candidate;
      }
    }
    const auto* target = clip == nullptr ? nullptr : TargetEffect(*clip, mask_effect_);
    if (target == nullptr) {
      ShowStatus(tr("Add an effect to the clip first: a mask limits where an effect applies"));
      return true;
    }
    const auto plan = ui::PlanAddMask(EditContextFor(), *clip_id, target->id, drawn);
    const auto created = plan.ok ? std::get<commands::AddMaskPayload>(plan.commands[0].payload).id : std::string();
    if (MaskApply(plan) && !created.empty()) {
      mask_tool_ = "select";
      maskSelect(QString::fromStdString(created));
    }
    return true;
  }
  if (!gesture_.active) return false;
  const auto gesture = gesture_;
  gesture_ = {};
  emit maskChanged();
  if (!gesture.moved) return true;   // a click on a handle only selects it
  const auto edited = ui::ApplyEdit(gesture.stored, gesture.evaluated, gesture.preview_evaluated, gesture.seconds);
  const auto plan = ui::PlanUpdateMask(EditContextFor(), gesture.clip_id, mask_id_, edited, "Edit Mask");
  return MaskApply(plan);
}

bool Session::maskDoubleClick(double x, double y, double tolerance_pixels) {
  gesture_ = {};   // the second press of a double-click began a gesture of its own; the double-click replaces it
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return false;
  if (mask_tool_ == "pen" && pen_points_.size() >= 3) {
    maskFinishPen();
    return true;
  }
  if (mask_id_.empty()) return false;
  const auto found = FindMaskIn(*sequence, mask_id_);
  if (found.mask == nullptr) return false;
  const auto picture = MaskPicture();
  const auto seconds = MaskLocalSeconds(*found.clip);
  const auto evaluated = effects::mask::Evaluate(found.mask->document, seconds);
  const auto handle = ui::HitTest(evaluated, picture, x, y, tolerance_pixels, mask_point_);
  if (handle.kind == ui::MaskHandle::Kind::Segment) {
    const auto grown = ui::InsertPoint(evaluated, picture, x, y, tolerance_pixels);
    if (grown.points.size() == evaluated.points.size()) return false;
    // A new vertex means the stored vertex animation no longer lines up; the editor drops it, and so does this.
    auto edited = ui::ApplyEdit(found.mask->document, evaluated, evaluated, seconds);
    edited.points = grown.points;
    edited.animations.erase(std::remove_if(edited.animations.begin(), edited.animations.end(), [](const effects::mask::Animation& a) { return a.property.rfind("point_", 0) == 0; }), edited.animations.end());
    return MaskApply(ui::PlanUpdateMask(EditContextFor(), found.clip->id, mask_id_, edited, "Add Mask Point"));
  }
  if (handle.kind == ui::MaskHandle::Kind::Point) {
    // On a vertex: round it off, or back to a corner if it already is round.
    const bool smooth = ui::IsSmooth(evaluated, handle.index);
    auto edited = found.mask->document;
    const auto shaped = smooth ? ui::Corner(evaluated, handle.index) : ui::Smooth(evaluated, handle.index);
    edited = ui::ApplyEdit(found.mask->document, evaluated, shaped, seconds);
    return MaskApply(ui::PlanUpdateMask(EditContextFor(), found.clip->id, mask_id_, edited, smooth ? "Sharpen Mask Point" : "Smooth Mask Point"));
  }
  return false;
}

bool Session::maskDeletePoint() {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || mask_id_.empty() || mask_point_ < 0) return false;
  const auto found = FindMaskIn(*sequence, mask_id_);
  if (found.mask == nullptr) return false;
  const auto shaped = ui::RemovePoint(found.mask->document, mask_point_);
  if (shaped.points.size() == found.mask->document.points.size()) {
    ShowStatus(tr("A path needs at least three points"));
    return false;
  }
  mask_point_ = -1;
  return MaskApply(ui::PlanUpdateMask(EditContextFor(), found.clip->id, mask_id_, shaped, "Remove Mask Point"));
}

void Session::maskFinishPen() {
  const auto points = pen_points_;
  pen_points_.clear();
  emit maskChanged();
  if (points.size() < 3) return;
  const auto clip_id = PrimaryClip();
  const auto* sequence = graph_.root();
  if (!clip_id || sequence == nullptr) return;
  const timeline::Clip* clip = nullptr;
  for (const auto& track : sequence->tracks) {
    for (const auto& candidate : track.clips) {
      if (candidate.id == *clip_id) clip = &candidate;
    }
  }
  const auto* target = clip == nullptr ? nullptr : TargetEffect(*clip, mask_effect_);
  if (target == nullptr) {
    ShowStatus(tr("Add an effect to the clip first: a mask limits where an effect applies"));
    return;
  }
  const auto plan = ui::PlanAddMask(EditContextFor(), *clip_id, target->id, ui::NewPath(points));
  const auto created = plan.ok ? std::get<commands::AddMaskPayload>(plan.commands[0].payload).id : std::string();
  if (MaskApply(plan) && !created.empty()) {
    mask_tool_ = "select";
    maskSelect(QString::fromStdString(created));
  }
}

void Session::maskCancelPen() {
  pen_points_.clear();
  gesture_ = {};
  emit maskChanged();
}

// ------------------------------------------------------------------- tracking ----

QVariantList Session::maskTracks() const {
  QVariantList list;
  const auto* sequence = graph_.root();
  if (sequence == nullptr || mask_id_.empty() || store_ == nullptr) return list;
  const auto found = FindMaskIn(*sequence, mask_id_);
  if (found.clip == nullptr) return list;
  const std::lock_guard<std::mutex> lock(store_->mutex());
  db::Statement statement(store_->connection(), "SELECT id, name, kind FROM tracking_data WHERE clip_id = ? AND kind = 'point' ORDER BY id;");
  statement.Bind(1, found.clip->id);
  while (statement.Step()) {
    QVariantMap map;
    map["id"] = QString::fromStdString(statement.ColumnText(0));
    map["name"] = QString::fromStdString(statement.ColumnText(1));
    list << map;
  }
  return list;
}

bool Session::maskFollowTrack(const QString& mask_id, const QString& track_id) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || store_ == nullptr) return false;
  const auto found = FindMaskIn(*sequence, mask_id.toStdString());
  if (found.mask == nullptr) return false;
  std::string json;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT data_json FROM tracking_data WHERE id = ? AND clip_id = ? AND kind = 'point';");
    statement.Bind(1, track_id.toStdString()).Bind(2, found.clip->id);
    if (statement.Step()) json = statement.ColumnText(0);
  }
  if (json.empty()) {
    ShowStatus(tr("That track is not on this clip"));
    return false;
  }
  render::tracking::TrackResult track;
  try {
    track = render::tracking::TrackResultFromJson(json);
  } catch (const std::exception& error) {
    ShowStatus(tr("The track could not be read: %1").arg(error.what()));
    return false;
  }
  const auto seconds = MaskLocalSeconds(*found.clip);
  const auto followed = ui::FollowTrack(found.mask->document, track, seconds, {static_cast<double>(track.source_width > 0 ? track.source_width : sequence->width), static_cast<double>(track.source_height > 0 ? track.source_height : sequence->height)});
  if (followed.animations.empty()) {
    ShowStatus(tr("The track has too few usable points to follow"));
    return false;
  }
  return MaskApply(ui::PlanUpdateMask(EditContextFor(), found.clip->id, mask_id.toStdString(), followed, "Track Mask"));
}

void Session::maskTrack(const QString& mask_id) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || store_ == nullptr) return;
  const auto found = FindMaskIn(*sequence, mask_id.toStdString());
  if (found.mask == nullptr || found.clip->source_kind != model::SourceKind::Media) {
    ShowStatus(tr("Only a mask on a clip of video media can be tracked"));
    return;
  }
  if (found.clip->playback_rate.Compare(RationalTime(1, 1)) != 0 || found.clip->reversed) {
    ShowStatus(tr("Track a clip at normal speed: the track is measured frame by frame of the clip"));
    return;
  }
  std::string path, fingerprint;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT original_path, fingerprint FROM media WHERE id = ?;");
    statement.Bind(1, found.clip->source_id);
    if (statement.Step()) {
      path = statement.ColumnText(0);
      fingerprint = statement.ColumnText(1);
    }
  }
  if (path.empty()) {
    ShowStatus(tr("The media is offline"));
    return;
  }
  const auto document = effects::mask::Evaluate(found.mask->document, MaskLocalSeconds(*found.clip));
  // The point to follow: the middle of the shape, as a fraction of the picture, which is the source's picture when the clip
  // is not moved or cropped.
  double cx = document.center_x, cy = document.center_y;
  if (document.shape == effects::mask::Shape::Bezier && !document.points.empty()) {
    cx = cy = 0.0;
    for (const auto& p : document.points) {
      cx += p.x;
      cy += p.y;
    }
    cx /= static_cast<double>(document.points.size());
    cy /= static_cast<double>(document.points.size());
  }
  const auto clip_id = found.clip->id;
  const auto source_in = found.clip->source_in;
  const auto clip_frames = found.clip->duration();
  const auto rate = sequence->frame_rate;
  const auto start_frame = static_cast<std::size_t>(std::max<std::int64_t>(0, transport_.position().Subtract(found.clip->timeline_start).ToFrames(rate, time::RoundingMode::Floor)));
  const auto limit = analysis_limit_;
  const auto id = mask_id.toStdString();
  const auto title = std::string("Tracking a mask on ") + (found.clip->name.empty() ? found.clip->id : found.clip->name);
  runner_->Run("track", title, [this, path, fingerprint, clip_id, source_in, clip_frames, rate, start_frame, cx, cy, limit, id](ui::JobContext& context) {
    auto source = media::SourceRegistry::Instance().Open(path);
    if (source == nullptr) throw std::runtime_error("The media could not be opened");
    const auto total = static_cast<std::size_t>(std::min<std::int64_t>(clip_frames.ToFrames(rate, time::RoundingMode::Nearest), static_cast<std::int64_t>(limit)));
    if (start_frame + 2 >= total) throw std::runtime_error("There are too few frames after the playhead to track");
    std::map<std::size_t, media::VideoFrame> kept;
    const auto resolve = [&](std::size_t index) -> const media::VideoFrame* {
      if (const auto found_frame = kept.find(index); found_frame != kept.end()) return &found_frame->second;
      auto frame = source->ReadVideo(source_in.Add(time::RationalTime::FromFrames(static_cast<std::int64_t>(index), rate)));
      if (!frame) return nullptr;
      auto converted = frame->format() == media::PixelFormat::RgbaF32 ? std::move(*frame) : media::ConvertFrame(*frame, media::PixelFormat::RgbaF32);
      for (auto it = kept.begin(); it != kept.end();) it = (index > 4 && it->first + 4 < index) ? kept.erase(it) : std::next(it);
      return &kept.emplace(index, std::move(converted)).first->second;
    };
    const auto first = resolve(start_frame);
    if (first == nullptr) throw std::runtime_error("The frame at the playhead could not be read");
    render::tracking::PointTrackerConfig config;
    config.analysis_width = 960;
    const auto result = render::tracking::TrackPoint(total, rate, resolve, start_frame, cx * first->width(), cy * first->height(), config, [&] { return context.cancelled(); },
                                                     [&](std::size_t done, std::size_t all) { context.Progress(done, all); });
    if (result.cancelled || context.cancelled()) return;
    commands::SaveTrackingDataPayload save;
    save.id = "track-" + id;
    save.clip_id = clip_id;
    save.kind = "point";
    save.name = "Mask track";
    save.algorithm = result.algorithm;
    save.source_fingerprint = fingerprint;
    save.data_json = render::tracking::ToJson(result);
    QMetaObject::invokeMethod(this, [this, save, id, result]() {
      ui::EditPlan plan;
      plan.ok = true;
      plan.label = "Track Mask";
      plan.commands.push_back({commands::CommandType::SaveTrackingData, save});
      (void)Apply(plan);
      (void)maskFollowTrack(QString::fromStdString(id), QString::fromStdString(save.id));
    }, Qt::QueuedConnection);
  });
}

}  // namespace cutline::app

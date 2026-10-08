// The multicam side of the session: choosing the clips and how to line them up, the angle monitor, live cutting
// against a running clock, and refining the cuts. What the edits mean is in ui/Multicam.h; this holds the group that
// is open, the clock, and the picture of every angle that the monitor shows.

#include "app/Session.h"

#include "core/db/Sql.h"
#include "media/Source.h"
#include "ui/Multicam.h"

#include <QMetaObject>

#include <algorithm>
#include <cmath>

namespace cutline::app {

using time::RationalTime;
namespace mc = timeline::multicam;

namespace {

constexpr int kGridWidth = 960;
constexpr int kGridHeight = 540;
constexpr std::int64_t kEnvelopeSeconds = 90;  // a clap is found well inside this

QImage GridImage(const media::VideoFrame& frame) {
  const auto rgba = media::ConvertFrame(frame, media::PixelFormat::Rgba8);
  QImage image(rgba.width(), rgba.height(), QImage::Format_RGBA8888);
  for (int y = 0; y < rgba.height(); ++y) std::memcpy(image.scanLine(y), rgba.row_u8(y), static_cast<std::size_t>(rgba.width()) * 4);
  return image;
}

RationalTime FrameAt(double seconds, time::FrameRate rate) {
  const auto frames = static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator)));
  return RationalTime::FromFrames(std::max<std::int64_t>(frames, 0), rate);
}

}  // namespace

time::FrameRate Session::McRate() const { return graph_.root() != nullptr ? graph_.root()->frame_rate : time::kFrameRate25; }

// ---------------------------------------------------------------- what is open ----

QStringList Session::trackIds(const QString& kind) const {
  QStringList ids;
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return ids;
  const auto wanted = kind == "audio" ? model::TrackKind::Audio : model::TrackKind::Video;
  for (const auto& track : sequence->tracks) {
    if (track.kind == wanted) ids << QString::fromStdString(track.id);
  }
  return ids;
}

QVariantList Session::multicamGroups() const {
  QVariantList list;
  if (store_ == nullptr) return list;
  for (const auto& id : mc::ListGroupIds(*store_)) {
    try {
      const auto group = mc::LoadGroup(*store_, id);
      QVariantMap map;
      map["id"] = QString::fromStdString(id);
      map["name"] = QString::fromStdString(group.name);
      map["angles"] = static_cast<int>(group.angles.size());
      map["duration"] = Seconds(group.duration);
      list << map;
    } catch (const std::exception&) {
    }
  }
  return list;
}

QVariantList Session::multicamCandidates() const {
  QVariantList list;
  if (store_ == nullptr) return list;
  const std::lock_guard<std::mutex> lock(store_->mutex());
  db::Statement statement(store_->connection(), R"sql(
    SELECT m.id, m.display_name, m.duration_num, m.duration_den,
           EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = m.id AND s.kind = 'video'),
           EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = m.id AND s.kind = 'audio'),
           m.start_timecode_num, m.start_timecode_den
      FROM media m ORDER BY m.display_name;
  )sql");
  while (statement.Step()) {
    QVariantMap map;
    map["id"] = QString::fromStdString(statement.ColumnText(0));
    map["name"] = QString::fromStdString(statement.ColumnText(1));
    map["duration"] = Seconds(RationalTime(statement.ColumnInt(2), statement.ColumnInt(3)));
    map["hasVideo"] = statement.ColumnInt(4) != 0;
    map["hasAudio"] = statement.ColumnInt(5) != 0;
    map["timecode"] = Seconds(RationalTime(statement.ColumnInt(6), std::max<std::int64_t>(1, statement.ColumnInt(7))));
    list << map;
  }
  return list;
}

QVariantList Session::multicamAngles() const {
  QVariantList list;
  const auto group = McGroup();
  if (!group) return list;
  const auto* showing = mc::ActiveAngle(*group, mc_position_);
  int index = 0;
  for (const auto& angle : group->angles) {
    QVariantMap map;
    map["index"] = index;
    map["id"] = QString::fromStdString(angle.id);
    map["name"] = QString::fromStdString(angle.name);
    map["active"] = showing != nullptr && showing->id == angle.id;
    map["present"] = ui::HasPictureAt(angle, mc_position_);
    map["offset"] = Seconds(angle.source_offset);
    map["key"] = index < 9 ? QString::number(index + 1) : QString();
    list << map;
    ++index;
  }
  return list;
}

QVariantList Session::multicamCuts() const {
  QVariantList list;
  const auto group = McGroup();
  if (!group) return list;
  int index = 0;
  for (const auto& cut : group->switches) {
    QVariantMap map;
    map["index"] = index++;
    map["time"] = Seconds(cut.timeline_time);
    map["angle"] = QString::fromStdString(cut.angle_id);
    for (std::size_t i = 0; i < group->angles.size(); ++i) {
      if (group->angles[i].id == cut.angle_id) {
        map["angleIndex"] = static_cast<int>(i);
        map["angleName"] = QString::fromStdString(group->angles[i].name);
      }
    }
    list << map;
  }
  return list;
}

double Session::multicamDuration() const {
  const auto group = McGroup();
  return group ? Seconds(group->duration) : 0.0;
}

QString Session::multicamSync() const {
  const auto group = McGroup();
  if (!group) return {};
  return QString("%1, %2%").arg(QString::fromStdString(group->sync_method)).arg(static_cast<int>(group->sync_confidence * 100.0 + 0.5));
}

QString Session::multicamName() const {
  const auto group = McGroup();
  return group ? QString::fromStdString(group->name) : QString();
}

std::shared_ptr<const mc::Group> Session::McGroup() const {
  const std::lock_guard<std::mutex> lock(mc_mutex_);
  return mc_group_;
}

void Session::RefreshMulticam() {
  if (store_ == nullptr || mc_id_.empty()) return;
  try {
    auto group = std::make_shared<const mc::Group>(mc::LoadGroup(*store_, mc_id_));
    const std::lock_guard<std::mutex> lock(mc_mutex_);
    mc_group_ = std::move(group);
  } catch (const std::exception&) {
    // The group is gone (an undo took it away): close it.
    mc_id_.clear();
    {
      const std::lock_guard<std::mutex> lock(mc_mutex_);
      mc_group_.reset();
    }
    StopMulticamClock();
  }
  emit multicamChanged();
  RequestMulticamFrame();
}

bool Session::multicamOpen(const QString& group_id) {
  if (store_ == nullptr) return false;
  mc_id_ = group_id.toStdString();
  mc_position_ = RationalTime(0, 1);
  StopMulticamClock();
  if (!mc_presenter_) StartMulticamMonitor();
  RefreshMulticam();
  if (mc_id_.empty()) {
    ShowStatus(tr("There is no multicam group %1").arg(group_id));
    return false;
  }
  emit multicamPositionChanged();
  return true;
}

void Session::multicamClose() {
  StopMulticamClock();
  mc_id_.clear();
  {
    const std::lock_guard<std::mutex> lock(mc_mutex_);
    mc_group_.reset();
  }
  mc_frame_ = {};
  emit multicamChanged();
  emit multicamFrameReady();
}

void Session::StartMulticamMonitor() {
  auto* store = store_.get();
  const bool proxies = prefs_.GetBool("media.use_proxies");
  mc_monitor_ = std::make_unique<ui::AngleMonitor>([store, proxies](const std::string& media_id) {
    const std::lock_guard<std::mutex> lock(store->mutex());
    if (proxies) {
      db::Statement proxy(store->connection(), "SELECT p.path FROM media_proxies p JOIN media m ON m.id = p.media_id WHERE p.media_id = ? AND p.source_fingerprint = m.fingerprint;");
      proxy.Bind(1, media_id);
      if (proxy.Step() && !proxy.ColumnText(0).empty()) return proxy.ColumnText(0);
    }
    db::Statement statement(store->connection(), "SELECT original_path FROM media WHERE id = ?;");
    statement.Bind(1, media_id);
    return statement.Step() ? statement.ColumnText(0) : std::string();
  });
  mc_presenter_ = std::make_unique<ui::FramePresenter>(
      [this](const RationalTime& at, ui::SizePx size) {
        const auto group = McGroup();
        if (!group || group->angles.empty()) return media::VideoFrame{};
        render::AngleMonitorConfig config;
        config.width = std::max(64, size.width);
        config.height = std::max(36, size.height);
        // Keep every tile at least a few pixels wide however many angles there are.
        return mc_monitor_->Render(*group, at, config);
      },
      [this](ui::PresentedFrame presented, const RationalTime&, std::uint64_t) {
        auto frame = std::move(presented.pixels);
        if (!frame.valid()) return;
        auto image = GridImage(frame);
        QMetaObject::invokeMethod(this, [this, image] {
          mc_frame_ = image;
          emit multicamFrameReady();
        }, Qt::QueuedConnection);
      });
  connect(&mc_tick_, &QTimer::timeout, this, [this] { OnMulticamTick(); });
  mc_tick_.setInterval(20);
}

void Session::RequestMulticamFrame() {
  if (!mc_presenter_ || mc_id_.empty()) return;
  (void)mc_presenter_->Request(mc_position_, ui::SizePx{kGridWidth, kGridHeight});
}

// ----------------------------------------------------------------------- clock ----

void Session::StopMulticamClock() {
  if (!mc_playing_) return;
  mc_playing_ = false;
  mc_tick_.stop();
  emit multicamPositionChanged();
}

void Session::multicamPlay(bool on) {
  const auto group = McGroup();
  if (!group || on == mc_playing_) return;
  if (!on) {
    StopMulticamClock();
    return;
  }
  if (mc_position_.Compare(group->duration) >= 0) mc_position_ = RationalTime(0, 1);
  mc_playing_ = true;
  mc_clock_base_ = Seconds(mc_position_);
  mc_clock_.start();
  mc_tick_.start();
  emit multicamPositionChanged();
}

void Session::OnMulticamTick() {
  const auto group = McGroup();
  if (!group) {
    StopMulticamClock();
    return;
  }
  const double seconds = mc_clock_base_ + static_cast<double>(mc_clock_.nsecsElapsed()) * 1e-9;
  if (seconds >= Seconds(group->duration)) {
    mc_position_ = group->duration;
    StopMulticamClock();
  } else {
    mc_position_ = FrameAt(seconds, McRate());
  }
  emit multicamPositionChanged();
  RequestMulticamFrame();
}

void Session::multicamSeek(double seconds) {
  const auto group = McGroup();
  if (!group) return;
  auto to = FrameAt(seconds, McRate());
  if (to.Compare(group->duration) >= 0) to = group->duration;
  mc_position_ = to;
  if (mc_playing_) {
    mc_clock_base_ = Seconds(to);
    mc_clock_.restart();
  }
  emit multicamPositionChanged();
  RequestMulticamFrame();
}

// ------------------------------------------------------------------- the edits ----

bool Session::McApply(const ui::EditPlan& plan) {
  if (!Apply(plan)) return false;
  RefreshMulticam();
  return true;
}

bool Session::multicamCut(int angle_index) {
  const auto group = McGroup();
  if (!group) {
    ShowStatus(tr("Open a multicam group to cut"));
    return false;
  }
  // Cut at the frame the clock is on now, which is what makes a key press while playing land where it was pressed.
  return McApply(ui::PlanCutTo(*group, angle_index, mc_position_, McRate()));
}

bool Session::multicamCutAtPoint(double x, double y, double width, double height) {
  const auto group = McGroup();
  if (!group || width <= 0 || height <= 0) return false;
  const int px = static_cast<int>(x / width * kGridWidth), py = static_cast<int>(y / height * kGridHeight);
  render::AngleMonitorConfig config;
  config.width = kGridWidth;
  config.height = kGridHeight;
  const int angle = ui::AngleMonitor::AngleAt(group->angles.size(), px, py, config);
  return angle >= 0 && multicamCut(angle);
}

bool Session::multicamMoveCut(int cut, double to_seconds) {
  const auto group = McGroup();
  if (!group || cut < 0 || cut >= static_cast<int>(group->switches.size())) return false;
  return McApply(ui::PlanMoveCut(*group, group->switches[static_cast<std::size_t>(cut)].timeline_time, FrameAt(to_seconds, McRate()), McRate()));
}

bool Session::multicamNudgeCut(int cut, int frames) {
  const auto group = McGroup();
  if (!group || cut < 0 || cut >= static_cast<int>(group->switches.size())) return false;
  return McApply(ui::PlanNudgeCut(*group, group->switches[static_cast<std::size_t>(cut)].timeline_time, frames, McRate()));
}

bool Session::multicamChangeCut(int cut, int angle_index) {
  const auto group = McGroup();
  if (!group || cut < 0 || cut >= static_cast<int>(group->switches.size())) return false;
  return McApply(ui::PlanChangeCutAngle(*group, group->switches[static_cast<std::size_t>(cut)].timeline_time, angle_index));
}

bool Session::multicamRemoveCut(int cut) {
  const auto group = McGroup();
  if (!group || cut < 0 || cut >= static_cast<int>(group->switches.size())) return false;
  return McApply(ui::PlanRemoveCut(*group, group->switches[static_cast<std::size_t>(cut)].timeline_time));
}

bool Session::multicamRenameAngle(int angle_index, const QString& name) {
  const auto group = McGroup();
  return group && McApply(ui::PlanRenameAngle(*group, angle_index, name.toStdString()));
}

bool Session::multicamNudgeSync(int angle_index, int frames) {
  const auto group = McGroup();
  return group && McApply(ui::PlanNudgeSync(*group, angle_index, RationalTime::FromFrames(frames, McRate())));
}

bool Session::multicamFlatten(const QString& video_track, const QString& audio_track, bool audio_from_one_angle, int audio_angle) {
  const auto group = McGroup();
  if (!group) return false;
  const auto at = transport_.position();
  const auto plan = ui::PlanFlatten(EditContextFor(), *group, video_track.toStdString(), audio_track.toStdString(),
                                    audio_from_one_angle ? ui::AudioSource::OneAngle : ui::AudioSource::FollowsPicture, audio_angle, at);
  return Apply(plan);
}

bool Session::multicamDelete() {
  const auto group = McGroup();
  if (!group) return false;
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Delete Multicam Group";
  plan.commands.push_back({commands::CommandType::DeleteMulticamGroup, commands::DeleteMulticamGroupPayload{mc_id_}});
  const bool ok = Apply(plan);
  if (ok) multicamClose();
  return ok;
}

// ---------------------------------------------------------------------- set up ----

QString Session::multicamDemo() {
  QVariantList angles;
  for (const auto& candidate : multicamCandidates()) {
    if (angles.size() >= 3) break;
    if (!candidate.toMap()["hasVideo"].toBool()) continue;
    QVariantMap angle;
    angle["mediaId"] = candidate.toMap()["id"];
    angle["offset"] = 0.0;
    angles << angle;
  }
  return multicamCreate(angles, tr("Demo multicam"), "manual", 0);
}

QString Session::multicamCreate(const QVariantList& angles, const QString& name, const QString& sync, int reference) {
  if (store_ == nullptr) return {};
  const auto candidates = multicamCandidates();
  const auto find = [&](const QString& id) -> QVariantMap {
    for (const auto& candidate : candidates) {
      if (candidate.toMap()["id"].toString() == id) return candidate.toMap();
    }
    return {};
  };
  ui::GroupSetup setup;
  setup.name = name.toStdString();
  setup.reference = reference;
  setup.sync = sync == "timecode" ? ui::SyncChoice::Timecode : sync == "marker" ? ui::SyncChoice::Marker : sync == "audio" ? ui::SyncChoice::Audio : ui::SyncChoice::Manual;
  std::vector<std::string> clip_names;
  for (const auto& entry : angles) {
    const auto wanted = entry.toMap();
    const auto media = find(wanted["mediaId"].toString());
    if (media.isEmpty()) {
      ShowStatus(tr("A chosen clip is not in the project"));
      return {};
    }
    ui::AngleDraft draft;
    draft.media_id = wanted["mediaId"].toString().toStdString();
    draft.name = wanted["name"].toString().toStdString();
    draft.duration = RationalTime(static_cast<std::int64_t>(std::llround(media["duration"].toDouble() * 1000.0)), 1000);
    draft.has_video = media["hasVideo"].toBool();
    draft.has_audio = media["hasAudio"].toBool();
    draft.timecode_start = RationalTime(static_cast<std::int64_t>(std::llround(media["timecode"].toDouble() * 1000.0)), 1000);
    if (wanted.contains("marker") && wanted["marker"].isValid() && !wanted["marker"].toString().isEmpty()) {
      draft.marker = RationalTime(static_cast<std::int64_t>(std::llround(wanted["marker"].toDouble() * 1000.0)), 1000);
    }
    draft.offset = RationalTime(static_cast<std::int64_t>(std::llround(wanted["offset"].toDouble() * 1000.0)), 1000);
    clip_names.push_back(media["name"].toString().toStdString());
    setup.angles.push_back(std::move(draft));
  }
  if (setup.sync == ui::SyncChoice::Audio) {
    // The sound of each clip, as a hundredth-second loudness line; a clip with no sound has none and is reported by the plan.
    for (auto& draft : setup.angles) {
      std::string path;
      {
        const std::lock_guard<std::mutex> lock(store_->mutex());
        db::Statement statement(store_->connection(), "SELECT original_path FROM media WHERE id = ?;");
        statement.Bind(1, draft.media_id);
        if (statement.Step()) path = statement.ColumnText(0);
      }
      if (path.empty()) continue;
      if (auto source = media::SourceRegistry::Instance().Open(path)) draft.envelope = ui::AudioEnvelope(*source, RationalTime(kEnvelopeSeconds, 1));
    }
  }
  const auto result = ui::PlanCreateGroup(setup, clip_names, [this](const std::string& prefix) { return NewId(prefix); });
  if (!Apply(result.plan)) return {};
  if (!result.plan.notes.empty()) ShowStatus(QString::fromStdString(result.plan.notes.front()));
  const auto id = QString::fromStdString(result.group_id);
  (void)multicamOpen(id);
  return id;
}

void Session::MulticamCommand(const std::string& id) {
  if (id == "multicam.play") {
    multicamPlay(!mc_playing_);
    return;
  }
  if (id.rfind("multicam.cut_", 0) == 0) {
    const int number = std::atoi(id.c_str() + std::strlen("multicam.cut_"));
    // The digits belong to the multicam monitor only while a group is open in it.
    if (!McGroup()) return;
    (void)multicamCut(number - 1);
    return;
  }
  if (id == "multicam.next_cut" || id == "multicam.previous_cut") {
    const auto group = McGroup();
    if (!group) return;
    const bool next = id == "multicam.next_cut";
    std::optional<RationalTime> target;
    for (const auto& cut : group->switches) {
      if (next && cut.timeline_time.Compare(mc_position_) > 0 && !target) target = cut.timeline_time;
      if (!next && cut.timeline_time.Compare(mc_position_) < 0) target = cut.timeline_time;
    }
    if (target) multicamSeek(Seconds(*target));
  }
}

}  // namespace cutline::app

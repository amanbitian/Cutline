#include "app/Session.h"
#include "media/ProxyWorkflow.h"
#include "render/ColorManagement.h"

#include "core/db/Sql.h"
#include "core/util/Json.h"
#include "media/Ingest.h"
#include "media/Providers.h"
#include "media/SyntheticSource.h"
#include "render/RenderCache.h"
#include "render/Tracking.h"
#include "render/FlowCache.h"
#include "media/Source.h"
#include "timeline/SequenceLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cutline::app {
namespace {

using commands::CommandType;
using time::RationalTime;

std::string ToStd(const QString& text) { return text.toStdString(); }

QString LocalPath(const QString& url_or_path) {
  const QUrl url(url_or_path);
  if (url.isLocalFile()) return url.toLocalFile();
  return url_or_path;
}

std::string ReadText(const QString& path) {
  std::ifstream in(path.toStdString(), std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void WriteText(const QString& path, const std::string& text) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  std::ofstream out(path.toStdString(), std::ios::binary | std::ios::trunc);
  out << text;
}

QImage ToImage(media::VideoFrame frame) {
  if (frame.format() != media::PixelFormat::Rgba8) {
    frame = media::ConvertFrame(frame, media::PixelFormat::Rgba8);
  }
  // QImage can retain external immutable storage. Give it the VideoFrame
  // itself as the cleanup owner so presenting a 4K frame does not copy another
  // 33 MB merely to cross the Qt boundary. If Qt ever requests writable bits,
  // QImage detaches as usual.
  auto* owner = new media::VideoFrame(std::move(frame));
  const auto* bytes = reinterpret_cast<const uchar*>(static_cast<const media::VideoFrame&>(*owner).data());
  return QImage(bytes, owner->width(), owner->height(), owner->stride(), QImage::Format_RGBA8888,
                [](void* value) { delete static_cast<media::VideoFrame*>(value); }, owner);
}

// Qt's key to the name the shortcut table uses; empty when it is not a key a shortcut can have.
std::string KeyName(int key) {
  if (key >= Qt::Key_A && key <= Qt::Key_Z) return std::string(1, static_cast<char>('A' + (key - Qt::Key_A)));
  if (key >= Qt::Key_0 && key <= Qt::Key_9) return std::string(1, static_cast<char>('0' + (key - Qt::Key_0)));
  if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return "F" + std::to_string(key - Qt::Key_F1 + 1);
  switch (key) {
    case Qt::Key_Space: return "Space";
    case Qt::Key_Left: return "Left";
    case Qt::Key_Right: return "Right";
    case Qt::Key_Up: return "Up";
    case Qt::Key_Down: return "Down";
    case Qt::Key_Home: return "Home";
    case Qt::Key_End: return "End";
    case Qt::Key_PageUp: return "PageUp";
    case Qt::Key_PageDown: return "PageDown";
    case Qt::Key_Delete: return "Delete";
    case Qt::Key_Backspace: return "Backspace";
    case Qt::Key_Return:
    case Qt::Key_Enter: return "Enter";
    case Qt::Key_Escape: return "Escape";
    case Qt::Key_Tab: return "Tab";
    case Qt::Key_Insert: return "Insert";
    case Qt::Key_Equal:
    case Qt::Key_Plus: return "Equal";
    case Qt::Key_Minus: return "Minus";
    case Qt::Key_Comma: return "Comma";
    case Qt::Key_Period: return "Period";
    case Qt::Key_Slash: return "Slash";
    case Qt::Key_Backslash: return "Backslash";
    case Qt::Key_Semicolon: return "Semicolon";
    case Qt::Key_Apostrophe: return "Quote";
    case Qt::Key_BracketLeft: return "BracketLeft";
    case Qt::Key_BracketRight: return "BracketRight";
    case Qt::Key_QuoteLeft: return "Backtick";
    default: return {};
  }
}

QVariantMap ValueMap(const anim::Value& value) {
  QVariantList components;
  for (int i = 0; i < value.dimension; ++i) components.push_back(value.components[static_cast<std::size_t>(i)]);
  return {{"dimension", value.dimension}, {"components", components}};
}

}  // namespace

Session::Session(QObject* parent) : QObject(parent), keymap_(ui::BuiltInCommands()), runner_(std::make_unique<ui::JobRunner>(jobs_)) {
  media::RegisterAllProviders();
  jobs_.Observe([this](const ui::JobInfo&) { QMetaObject::invokeMethod(this, [this] { emit jobsChanged(); }, Qt::QueuedConnection); });
  tick_.setInterval(16);
  connect(&tick_, &QTimer::timeout, this, &Session::OnTick);
  connect(this, &Session::playheadChanged, this, [this] { UpdateTranscriptWord(); });
  setScopeMode("waveform");
  prefs_.Observe([this](const std::string& key, const ui::PreferenceValue&) {
    if (key == "timeline.snap") emit snapChanged();
    if (key.rfind("monitor.", 0) == 0) emit monitorChanged();
    if (key == "playback.loop") emit transportChanged();
  });
}

Session::~Session() {
  tick_.stop();
  presenter_.reset();
  {
    const std::lock_guard<std::mutex> lock(scopes_mutex_);
    scopes_.reset();
  }
  if (engine_ != nullptr) engine_->Pause();
  runner_.reset();
  SaveConfiguration();
}

// ------------------------------------------------------------------ configuration ----

void Session::SetConfigDirectory(const QString& directory) {
  config_dir_ = directory;
  std::vector<std::string> warnings;
  prefs_.Load((directory + "/preferences.json").toStdString(), &warnings);
  if (QFile::exists(directory + "/keymap.json")) {
    try {
      keymap_.FromJson(ReadText(directory + "/keymap.json"), &warnings);
    } catch (const std::exception& e) {
      warnings.push_back(std::string("The saved shortcuts could not be read: ") + e.what());
    }
  }
  if (QFile::exists(directory + "/workspaces.json")) {
    try {
      workspaces_.FromJson(ReadText(directory + "/workspaces.json"), &warnings);
    } catch (const std::exception& e) {
      warnings.push_back(std::string("The saved workspaces could not be read: ") + e.what());
    }
  }
  if (const auto q = ui::ParseMonitorQuality(prefs_.GetText("playback.default_quality"))) quality_ = *q;
  transport_.SetLooping(prefs_.GetBool("playback.loop"));
  // The looks that ship with the application, and a folder beside them for the person's own .cube files.
  (void)ui::WriteBuiltInLooks(std::filesystem::path(directory.toStdString()) / "luts" / "Cutline Looks");
  luts_.SetFolders({std::filesystem::path(directory.toStdString()) / "luts"});
  luts_.Rescan();
  if (!warnings.empty()) ShowStatus(QString::fromStdString(warnings.front()));
}

void Session::SaveConfiguration() {
  if (config_dir_.isEmpty()) return;
  try {
    prefs_.Save((config_dir_ + "/preferences.json").toStdString());
    WriteText(config_dir_ + "/keymap.json", keymap_.ToJson());
    WriteText(config_dir_ + "/workspaces.json", workspaces_.ToJson());
  } catch (const std::exception&) {
    // A configuration that cannot be written is not worth stopping an edit for.
  }
}

// ------------------------------------------------------------------------ reading ----

QString Session::projectName() const { return store_ ? QString::fromStdString(store_->ProjectName()) : QString(); }
double Session::playheadSeconds() const { return Seconds(transport_.position()); }

QString Session::timecode() const {
  const auto* sequence = graph_.root();
  const auto rate = sequence != nullptr ? sequence->frame_rate : time::kFrameRate25;
  return QString::fromStdString(transport_.position().FormatTimecode(rate, sequence != nullptr && sequence->drop_frame));
}

double Session::durationSeconds() const { return graph_.root() != nullptr ? Seconds(graph_.root()->Duration()) : 0.0; }

double Session::frameRate() const {
  const auto* sequence = graph_.root();
  return sequence != nullptr && sequence->frame_rate.denominator != 0 ? static_cast<double>(sequence->frame_rate.numerator) / static_cast<double>(sequence->frame_rate.denominator) : 25.0;
}

QStringList Session::history() const {
  QStringList labels;
  if (store_ == nullptr) return labels;
  for (const auto& label : store_->HistorySteps()) labels << QString::fromStdString(label);
  return labels;
}

int Session::appliedSteps() const { return store_ == nullptr ? 0 : static_cast<int>(store_->AppliedStepCount()); }

QVariantList Session::jobs() const {
  QVariantList list;
  for (const auto& job : jobs_.Snapshot()) {
    list.push_back(QVariantMap{{"id", job.id},
                               {"kind", QString::fromStdString(job.kind)},
                               {"title", QString::fromStdString(job.title)},
                               {"state", QString::fromStdString(ui::ToString(job.state))},
                               {"progress", job.progress},
                               {"message", QString::fromStdString(job.message)},
                               {"error", QString::fromStdString(job.error)},
                               {"active", !ui::Finished(job.state)}});
  }
  return list;
}

QSize Session::currentFrameSequenceSize() const {
  const auto* sequence = graph_.root();
  return sequence != nullptr ? QSize(static_cast<int>(sequence->width), static_cast<int>(sequence->height)) : QSize(1920, 1080);
}

void Session::ShowStatus(const QString& message) {
  status_ = message;
  emit statusChanged();
}

void Session::setTool(const QString& tool) {
  if (tool_ == tool) return;
  tool_ = tool;
  emit toolChanged();
}

void Session::setSnap(bool on) {
  prefs_.Set("timeline.snap", on);
  SaveConfiguration();
}

void Session::setLoop(bool on) {
  transport_.SetLooping(on);
  prefs_.Set("playback.loop", on);
  emit transportChanged();
}

void Session::setMonitorQuality(const QString& quality) {
  const auto parsed = ui::ParseMonitorQuality(quality.toStdString());
  if (!parsed) return;
  quality_ = *parsed;
  prefs_.Set("playback.default_quality", ui::ToString(quality_));
  emit monitorChanged();
  RequestFrame();
}

void Session::setSafeMargins(bool on) { prefs_.Set("monitor.show_safe_margins", on); }

void Session::SetMonitorDisplay(int width, int height) {
  if (displayed_.width == width && displayed_.height == height) return;
  displayed_ = {width, height};
  RequestFrame();
}

// -------------------------------------------------------------------- project life ----

bool Session::Open(std::unique_ptr<project::ProjectStore> store) {
  export_queue_.reset();   // a running export stops (and is recorded as interrupted) before its project goes
  multicamClose();
  mc_presenter_.reset();
  mc_monitor_.reset();
  tick_.stop();
  presenter_.reset();
  if (engine_ != nullptr) engine_->Pause();
  engine_.reset();
  transport_.Stop();
  store_ = std::move(store);
  project_id_ = store_->ProjectId();
  // The sequence to edit: the one made for a new project, or the first the project has.
  sequence_id_ = "seq-1";
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT id FROM sequences ORDER BY (id = 'seq-1') DESC, rowid LIMIT 1;");
    if (statement.Step()) sequence_id_ = statement.ColumnText(0);
  }
  selection_.Clear();
  mark_in_.reset();
  mark_out_.reset();
  {
    const std::lock_guard<std::mutex> lock(frame_mutex_);
    frame_ = QImage();
    gpu_frame_ = {};
  }
  const auto scope_mode = scope_mode_;
  scope_mode_.clear();
  setScopeMode(scope_mode);
  package_folder_ = store_->PackagePath().string();
  tx_cache_.clear();
  tx_working_.clear();
  tx_media_.clear();
  tx_signature_.clear();
  flow_cache_ = std::make_shared<render::FlowCache>(render::FlowCacheConfig{
      256u << 20, store_->PackagePath().empty() ? std::filesystem::path() : store_->PackagePath() / "cache" / "flow", 1ull << 30});
  Reload();
  StartEngine();
  StartExportQueue();
  RefreshMedia();
  tick_.start();
  emit projectChanged();
  emit marksChanged();
  emit selectionChanged();
  RequestFrame();
  ShowStatus(tr("Opened %1").arg(projectName()));
  return true;
}

bool Session::newProject(const QString& folder_url, const QString& name) {
  try {
    const auto folder = LocalPath(folder_url);
    const auto package = std::filesystem::path(folder.toStdString()) / (name.toStdString() + ".cutline");
    commands::CommandEnvelope create;
    create.command_id = "create-" + NewId("p");
    create.project_id = project::ProjectStore::GenerateProjectUuid();
    create.author_id = "user";
    create.timestamp_utc = "2026-01-01T00:00:00Z";
    create.type = CommandType::CreateProject;
    create.payload = commands::CreateProjectPayload{name.toStdString()};
    create.idempotency_key = "create-key-" + NewId("p");
    auto store = project::ProjectStore::CreatePackage(package, create);
    const auto id = store->ProjectId();
    const auto run = [&](CommandType type, commands::CommandPayload payload) {
      commands::CommandEnvelope command;
      command.command_id = "cmd-" + NewId("n");
      command.project_id = id;
      command.author_id = "user";
      command.base_revision = store->CurrentRevision();
      command.timestamp_utc = "2026-01-01T00:00:00Z";
      command.type = type;
      command.payload = std::move(payload);
      command.idempotency_key = "key-" + NewId("n");
      (void)store->Execute(command);
    };
    commands::CreateSequencePayload sequence;
    sequence.id = "seq-1";
    sequence.settings.name = "Sequence 1";
    sequence.settings.frame_rate = time::kFrameRate25;
    sequence.settings.width = 1920;
    sequence.settings.height = 1080;
    sequence.settings.sample_rate = 48000;
    run(CommandType::CreateSequence, sequence);
    run(CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
    run(CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
    run(CommandType::AddVideoTrack, commands::AddTrackPayload{"v3", "seq-1", 2, "stereo", "V3"});
    run(CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});
    run(CommandType::AddAudioTrack, commands::AddTrackPayload{"a2", "seq-1", 1, "stereo", "A2"});
    store->ClearHistory();
    return Open(std::move(store));
  } catch (const std::exception& error) {
    ShowStatus(tr("Could not create the project: %1").arg(error.what()));
    return false;
  }
}

bool Session::newDemoProject() {
  const auto folder = QDir::temp().filePath("cutline-demo-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
  QDir().mkpath(folder);
  if (!newProject(folder, "Demo")) return false;
  // Synthetic media, imported through the same ingest as a file: bars with a tone, a counter, and a gradient.
  const auto make = [&](const std::string& id, media::SyntheticPattern pattern, double tone, int seconds, float r, float g, float b) {
    media::SyntheticSpec spec;
    spec.pattern = pattern;
    spec.width = 640;
    spec.height = 360;
    spec.frame_rate = time::kFrameRate25;
    spec.duration = RationalTime(seconds, 1);
    spec.tone_hz = tone;
    spec.red = r;
    spec.green = g;
    spec.blue = b;
    media::IngestRequest request;
    request.project_id = project_id_;
    request.author_id = "user";
    request.timestamp_utc = "2026-01-01T00:00:00Z";
    request.media_id = id;
    request.path = spec.ToPath();
    request.display_name = id;
    request.base_revision = static_cast<std::int64_t>(store_->CurrentRevision());
    (void)media::IngestFile(*store_, request);
  };
  try {
    make("Bars", media::SyntheticPattern::Bars, 440.0, 20, 0, 0, 0);
    make("Counter", media::SyntheticPattern::Counter, 330.0, 20, 0, 0, 0);
    make("Gradient", media::SyntheticPattern::Gradient, 0.0, 20, 0, 0, 0);
    make("Sunset", media::SyntheticPattern::Solid, 220.0, 20, 0.9f, 0.45f, 0.2f);
    selectMedia("Bars");
    store_->ClearHistory();
    // A first cut: bars, counter, then the solid, with the sound linked.
    const auto place = [&](const std::string& media_id, std::int64_t at, std::int64_t in, std::int64_t out) {
      media::SyntheticSpec spec;  // (only to keep the compiler honest about the unused helper above)
      (void)spec;
      ui::SourceRange source{media_id, media_id, RationalTime(in, 1), RationalTime(out, 1), true, true};
      auto ctx = EditContextFor();
      const auto plan = ui::PlanInsertEdit(ctx, source, RationalTime(at, 1), "v1", "a1", ui::OverlapMode::Overwrite);
      Apply(plan);
    };
    place("Bars", 0, 0, 6);
    place("Counter", 6, 2, 10);
    place("Sunset", 14, 0, 5);
    RefreshMedia();
    transport_.Seek(RationalTime(3, 1));
    RequestFrame();
    emit playheadChanged();
    emit projectChanged();  // so the timeline fits what the demo has put on it
  } catch (const std::exception& error) {
    ShowStatus(tr("The demo project could not be built: %1").arg(error.what()));
  }
  return true;
}

bool Session::openProject(const QString& folder_url) {
  try {
    const auto path = LocalPath(folder_url).toStdString();
    return Open(project::ProjectStore::OpenPackage(path));
  } catch (const std::exception& error) {
    ShowStatus(tr("Could not open the project: %1").arg(error.what()));
    return false;
  }
}

void Session::closeProject() {
  export_queue_.reset();
  designerClose();
  multicamClose();
  mc_presenter_.reset();
  mc_monitor_.reset();
  tick_.stop();
  presenter_.reset();
  engine_.reset();
  store_.reset();
  graph_ = {};
  caption_track_.clear();
  {
    const std::lock_guard<std::mutex> lock(frame_mutex_);
    frame_ = QImage();
    gpu_frame_ = {};
  }
  scope_result_.reset();
  media_.clear();
  inspector_.clear();
  emit projectChanged();
  emit sequenceChanged();
  emit mediaChanged();
  emit inspectorChanged();
  emit frameReady();
  emit scopeChanged();
  emit captionsChanged();
}

void Session::StartEngine() {
  playback::EngineConfig config;
  config.sample_rate = 48000;
  config.channels = 2;
  config.read_ahead_frames = static_cast<std::size_t>(prefs_.GetInt("playback.read_ahead_frames"));
  config.frame_cache_bytes = static_cast<std::size_t>(prefs_.GetInt("playback.frame_cache_mb")) * 1024u * 1024u;
  config.compositor.output_format = media::PixelFormat::Rgba8;
  config.compositor.flow_cache = flow_cache_;
  config.render_cache = std::make_shared<render::RenderCache>(render::RenderCacheConfig{256u * 1024u * 1024u, {}, 0});
  config.prefer_proxies = prefs_.GetBool("media.use_proxies");
  config.use_gpu = prefs_.GetBool("playback.use_gpu");
  config.hardware_decode = prefs_.GetBool("playback.hardware_decode");
  config.gpu_external_device = presentation_device_;
  config.gpu_external_context = presentation_context_;
  auto* store = store_.get();
  const auto lookup = [store](const std::string& sql, const std::string& id) {
    const std::lock_guard<std::mutex> lock(store->mutex());
    db::Statement statement(store->connection(), sql);
    statement.Bind(1, id);
    return statement.Step() ? statement.ColumnText(0) : std::string();
  };
  const playback::MediaLocator locator = [lookup](const std::string& id) { return lookup("SELECT original_path FROM media WHERE id = ?;", id); };
  config.proxy_locator = [lookup](const std::string& id) {
    return lookup("SELECT p.path FROM media_proxies p JOIN media m ON m.id = p.media_id WHERE p.media_id = ? AND p.source_fingerprint = m.fingerprint;", id);
  };
  engine_ = std::make_unique<playback::PlaybackEngine>(graph_, locator, config);

  auto* engine = engine_.get();
  presenter_ = std::make_unique<ui::FramePresenter>(
      [this, engine](const RationalTime& at, ui::SizePx size) -> ui::PresentedFrame {
        engine->SetOutputSize(size.width, size.height);
        // Scopes need CPU pixels, but not at the monitor's full frame rate. One
        // sampled readback every meter refresh keeps them live while the other
        // frames remain native textures end to end.
        const bool sample_scopes = scope_sample_due_.exchange(false, std::memory_order_relaxed);
        if (presentation_device_ != nullptr && !sample_scopes) {
          auto presented = engine->RenderForPresentation(at);
          if (presented.on_gpu()) return ui::PresentedFrame(std::move(presented.texture));
          return ui::PresentedFrame(std::move(presented.pixels));
        }
        return engine->RenderFrame(at);
      },
      [this](ui::PresentedFrame presented, const RationalTime&, std::uint64_t serial) {
        const auto info = QString("%1 x %2").arg(presented.width()).arg(presented.height());
        QImage image;
        if (!presented.on_gpu()) {
          {
            const std::lock_guard<std::mutex> lock(scopes_mutex_);
            if (scopes_ != nullptr) (void)scopes_->Submit(presented.pixels.Share());
          }
          image = ToImage(std::move(presented.pixels));
        }
        auto gpu_frame = std::move(presented.texture);
        QMetaObject::invokeMethod(
            this,
            [this, image, gpu_frame = std::move(gpu_frame), serial, info]() mutable {
              OnFrame(image, std::move(gpu_frame), serial, info);
            },
            Qt::QueuedConnection);
      });
}

void Session::SetPresentationDevice(void* device, void* context) {
  if (device == nullptr || (presentation_device_ == device && presentation_context_ == context)) return;
  if (store_ == nullptr || graph_.root() == nullptr) {
    presentation_device_ = device;
    presentation_context_ = context;
    return;
  }
  const bool was_playing = transport_.playing();
  if (was_playing) StopPlayback();
  // Join the presenter before changing the device pointers captured by its
  // worker callback. The replacement engine is then built on the new device.
  presenter_.reset();
  engine_.reset();
  presentation_device_ = device;
  presentation_context_ = context;
  StartEngine();
  RequestFrame();
  if (was_playing) StartPlayback();
}

QString Session::gpuInfo() const {
  if (engine_ == nullptr) return {};
  const auto name = engine_->gpu_device_name();
  if (name.empty()) {
    const auto why = engine_->gpu_unavailable_reason();
    return why.empty() ? tr("Software renderer") : tr("Software renderer (%1)").arg(QString::fromStdString(why));
  }
  const auto stats = engine_->statistics();
  QString text = tr("GPU: %1").arg(QString::fromStdString(name));
  if (stats.gpu_frames > 0) text += tr(", %1 frames (%2 ms each on the card)").arg(stats.gpu_frames).arg(static_cast<double>(stats.gpu_device_microseconds) / static_cast<double>(stats.gpu_frames) / 1000.0, 0, 'f', 2);
  if (stats.hardware_pictures > 0) text += tr(", %1 pictures decoded on the card").arg(stats.hardware_pictures);
  if (stats.gpu_fallback_frames > 0) text += tr(", %1 frames in software").arg(stats.gpu_fallback_frames);
  return text;
}

QVariantList Session::audioBuses() const {
  QVariantList buses;
  buses.push_back(QVariantMap{{"id", QString()}, {"name", tr("Master")}});
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return buses;
  for (const auto& track : sequence->tracks) {
    if (track.kind == model::TrackKind::Audio && track.is_bus) {
      buses.push_back(QVariantMap{{"id", QString::fromStdString(track.id)},
                                  {"name", QString::fromStdString(track.name)}});
    }
  }
  return buses;
}

QVariantList Session::audioMixer() const {
  QVariantList rows;
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return rows;
  const auto levels = engine_ != nullptr ? engine_->audio_levels() : std::map<std::string, audio::TrackLevel>{};
  for (const auto& track : sequence->tracks) {
    if (track.kind != model::TrackKind::Audio) continue;
    QVariantList sends;
    for (const auto& send : track.sends) {
      const auto* bus = sequence->FindTrack(send.bus_id);
      sends.push_back(QVariantMap{{"busId", QString::fromStdString(send.bus_id)},
                                  {"busName", bus != nullptr ? QString::fromStdString(bus->name) : QString::fromStdString(send.bus_id)},
                                  {"gainDb", send.gain_db}, {"preFader", send.pre_fader}});
    }
    auto level = audio::TrackLevel{};
    if (const auto found = levels.find(track.id); found != levels.end()) level = found->second;
    rows.push_back(QVariantMap{{"id", QString::fromStdString(track.id)},
                               {"name", QString::fromStdString(track.name)},
                               {"gainDb", track.gain_db}, {"pan", track.pan},
                               {"muted", track.muted}, {"solo", track.solo},
                               {"locked", track.locked}, {"isBus", track.is_bus}, {"master", false},
                               {"outputBusId", QString::fromStdString(track.output_bus_id)},
                               {"channelLayout", QString::fromStdString(track.channel_layout)},
                               {"peakDb", level.peak_db}, {"rmsDb", level.rms_db}, {"sends", sends},
                               {"autoMode", audioAutomationMode(QString::fromStdString(track.id))},
                               {"autoVolume", !ui::AutomationOf(track, ui::AutomationTarget::Volume).empty()},
                               {"autoPan", !ui::AutomationOf(track, ui::AutomationTarget::Pan).empty()},
                               {"liveGainDb", ui::AutomationValueAt(track, ui::AutomationTarget::Volume, transport_.position()).value_or(track.gain_db)},
                               {"livePan", ui::AutomationValueAt(track, ui::AutomationTarget::Pan, transport_.position()).value_or(track.pan)}});
  }
  auto master = audio::TrackLevel{};
  if (const auto found = levels.find("master"); found != levels.end()) master = found->second;
  rows.push_back(QVariantMap{{"id", "master"}, {"name", tr("Master")},
                             {"gainDb", 0.0}, {"pan", 0.0}, {"muted", false}, {"solo", false},
                             {"locked", true}, {"isBus", true}, {"master", true},
                             {"outputBusId", QString()}, {"channelLayout", QString::fromStdString(sequence->channel_layout)},
                             {"peakDb", master.peak_db}, {"rmsDb", master.rms_db}, {"sends", QVariantList{}},
                             {"autoMode", "read"}, {"autoVolume", false}, {"autoPan", false}, {"liveGainDb", 0.0}, {"livePan", 0.0}});
  return rows;
}

void Session::setScopeMode(const QString& requested) {
  const auto mode = requested == "parade" || requested == "vectorscope" || requested == "histogram"
                        ? requested
                        : QString("waveform");
  if (scope_mode_ == mode) return;
  render::AsyncScopeOptions options;
  options.measure_waveform = mode == "waveform" || mode == "parade";
  options.waveform_mode = mode == "parade" ? render::WaveformMode::Parade : render::WaveformMode::Luma;
  options.measure_vectorscope = mode == "vectorscope";
  options.measure_histogram = mode == "histogram";
  {
    const std::lock_guard<std::mutex> lock(scopes_mutex_);
    scopes_ = std::make_unique<render::AsyncScopes>(options);
  }
  scope_mode_ = mode;
  scope_sample_due_.store(true, std::memory_order_relaxed);
  scope_result_.reset();
  scope_generation_ = 0;
  emit scopeChanged();
  RequestFrame();
}

void Session::RefreshScopes() {
  std::optional<render::AsyncScopeResult> latest;
  {
    const std::lock_guard<std::mutex> lock(scopes_mutex_);
    if (scopes_ != nullptr) latest = scopes_->Latest();
  }
  if (!latest || latest->generation == scope_generation_) return;
  scope_generation_ = latest->generation;
  scope_result_ = std::move(latest);
  emit scopeChanged();
}

void Session::OnFrame(QImage image, render::gpu::PresentationFrame gpu_frame, std::uint64_t serial, const QString& info) {
  latest_serial_ = serial;
  {
    const std::lock_guard<std::mutex> lock(frame_mutex_);
    frame_ = std::move(image);
    gpu_frame_ = std::move(gpu_frame);
  }
  const auto stats = presenter_ != nullptr ? presenter_->statistics() : ui::PresenterStatistics{};
  render_info_ = QString("%1  %2 ms").arg(info).arg(stats.last_render_ms, 0, 'f', 1);
  emit frameReady();
}

void Session::Reload(bool invalidate_media) {
  if (store_ == nullptr) return;
  graph_ = timeline::LoadSequenceGraph(*store_, sequence_id_);
  if (const auto* sequence = graph_.root()) {
    transport_.SetFrameRate(sequence->frame_rate);
    transport_.SetDuration(sequence->Duration());
    selection_.Prune(*sequence);
    const auto selected_caption = std::find_if(sequence->caption_tracks.begin(), sequence->caption_tracks.end(),
                                               [&](const captions::Track& track) { return track.id == caption_track_; });
    if (selected_caption == sequence->caption_tracks.end()) {
      caption_track_ = sequence->caption_tracks.empty() ? std::string{} : sequence->caption_tracks.front().id;
    }
  }
  // Markers on the sequence, for snapping and the ruler.
  markers_.clear();
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT start_num, start_den FROM markers WHERE owner_kind = 'sequence' AND owner_id = ? ORDER BY start_ticks;");
    statement.Bind(1, sequence_id_);
    while (statement.Step()) markers_.emplace_back(statement.ColumnInt(0), statement.ColumnInt(1));
  }
  if (engine_ != nullptr) engine_->UpdateSequence(graph_, invalidate_media);
  RefreshHistory();
  RefreshInspector();
  if (!mc_id_.empty()) RefreshMulticam();
  RefreshTranscript();
  ++graphics_revision_;
  emit graphicsChanged();
  emit audioMixerChanged();
  emit captionsChanged();
  emit sequenceChanged();
  emit selectionChanged();
  emit playheadChanged();
  RequestFrame();
}

void Session::RefreshHistory() { emit historyChanged(); }

void Session::RefreshMedia() {
  media_.clear();
  if (store_ == nullptr) {
    emit mediaChanged();
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), R"sql(
      SELECT m.id, m.display_name, m.duration_num, m.duration_den, m.original_path, m.missing,
             EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = m.id AND s.kind = 'video'),
             EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = m.id AND s.kind = 'audio'),
             EXISTS(SELECT 1 FROM media_proxies p WHERE p.media_id = m.id)
        FROM media m ORDER BY m.display_name, m.id;
    )sql");
    while (statement.Step()) {
      const RationalTime duration(statement.ColumnInt(2), statement.ColumnInt(3));
      media_.push_back(QVariantMap{{"id", QString::fromStdString(statement.ColumnText(0))},
                                   {"name", QString::fromStdString(statement.ColumnText(1))},
                                   {"duration", Seconds(duration)},
                                   {"path", QString::fromStdString(statement.ColumnText(4))},
                                   {"missing", statement.ColumnInt(5) != 0},
                                   {"video", statement.ColumnInt(6) != 0},
                                   {"audio", statement.ColumnInt(7) != 0},
                                   {"proxy", statement.ColumnInt(8) != 0}});
    }
  }
  // Whether a proxy can be used (it is out of date if the original has changed since) and how far one being made has got.
  for (auto& entry : media_) {
    auto map = entry.toMap();
    const auto id = map["id"].toString().toStdString();
    const auto record = media::FindProxy(*store_, id);
    map["proxyStatus"] = record.status == media::ProxyStatus::Ready ? "ready" : record.status == media::ProxyStatus::Stale ? "stale" : record.status == media::ProxyStatus::Missing ? "missing" : "none";
    const auto busy = proxy_progress_.find(id);
    map["proxyBusy"] = busy != proxy_progress_.end();
    map["proxyProgress"] = busy != proxy_progress_.end() ? busy->second : 0.0;
    entry = map;
  }
  emit mediaChanged();
}

std::optional<std::string> Session::PrimaryClip() const {
  if (selection_.empty()) return std::nullopt;
  // The picture clip of a selected pair, when there is one: its sound is edited with it, and the inspector, effects and masks
  // belong to what is seen. (The set is ordered by id, which says nothing about which of the two comes first.)
  if (const auto* sequence = graph_.root()) {
    for (const auto& track : sequence->tracks) {
      if (track.kind != model::TrackKind::Video) continue;
      for (const auto& clip : track.clips) {
        if (selection_.Contains(clip.id)) return clip.id;
      }
    }
  }
  return *selection_.clips().begin();
}

void Session::NotifySelectionChanged() {
  RefreshInspector();
  RefreshTranscript();
  emit selectionChanged();
}

void Session::RefreshInspector() {
  inspector_.clear();
  inspector_clip_.clear();
  const auto* sequence = graph_.root();
  const auto id = PrimaryClip();
  if (sequence != nullptr && id) {
    for (const auto& track : sequence->tracks) {
      for (const auto& clip : track.clips) {
        if (clip.id != *id) continue;
        const auto local = transport_.position().Subtract(clip.timeline_start);
        const auto state = ui::BuildInspector(clip, local.Compare(RationalTime(0, 1)) < 0 ? RationalTime(0, 1) : local);
        inspector_clip_ = QString::fromStdString(state.clip_name);
        for (const auto& effect : state.effects) {
          QVariantList parameters;
          for (const auto& p : effect.parameters) {
            QVariantList keys;
            for (const auto& k : p.key_times) keys.push_back(Seconds(k));
            parameters.push_back(QVariantMap{{"name", QString::fromStdString(p.name)},
                                             {"label", QString::fromStdString(p.display_name)},
                                             {"value", ValueMap(p.value)},
                                             {"default", ValueMap(p.default_value)},
                                             {"hasMin", p.minimum.has_value()},
                                             {"min", p.minimum.value_or(0.0)},
                                             {"hasMax", p.maximum.has_value()},
                                             {"max", p.maximum.value_or(1.0)},
                                             {"unit", QString::fromStdString(effects::ToString(p.unit))},
                                             {"dimension", p.dimension},
                                             {"keyframeable", p.keyframeable},
                                             {"keyframed", p.keyframed},
                                             {"keyHere", p.key_here},
                                             {"keys", keys}});
          }
          inspector_.push_back(QVariantMap{{"id", QString::fromStdString(effect.id)},
                                           {"type", QString::fromStdString(effect.type)},
                                           {"name", QString::fromStdString(effect.display_name)},
                                           {"category", QString::fromStdString(effect.category)},
                                           {"enabled", effect.enabled},
                                           {"intrinsic", effect.intrinsic},
                                           {"preset", QString::fromStdString(effect.preset_name)},
                                           {"assetKind", QString::fromStdString(effect.asset_kind)},
                                           {"parameters", parameters},
                                           {"masks", masksOf(effect.id)}});
        }
      }
    }
  }
  emit inspectorChanged();
}

// ---------------------------------------------------------------------- editing ----

std::string Session::NewId(const std::string& prefix) const {
  return prefix + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toStdString();
}

ui::EditContext Session::EditContextFor() {
  ui::EditContext ctx;
  ctx.sequence = graph_.root();
  ctx.new_id = [this](const std::string& prefix) { return NewId(prefix); };
  auto* store = store_.get();
  ctx.media_duration = [store](const std::string& id) -> std::optional<RationalTime> {
    if (store == nullptr) return std::nullopt;
    const std::lock_guard<std::mutex> lock(store->mutex());
    db::Statement statement(store->connection(), "SELECT duration_num, duration_den FROM media WHERE id = ?;");
    statement.Bind(1, id);
    if (!statement.Step()) return std::nullopt;
    return RationalTime(statement.ColumnInt(0), statement.ColumnInt(1));
  };
  ctx.ripple_all_tracks = prefs_.GetBool("timeline.ripple_all_tracks");
  ctx.linked = prefs_.GetBool("timeline.linked_selection");
  return ctx;
}

commands::CommandEnvelope Session::Envelope(CommandType type, commands::CommandPayload payload) {
  commands::CommandEnvelope command;
  command.command_id = "cmd-" + NewId("c");
  command.project_id = project_id_;
  command.author_id = "user";
  command.base_revision = store_->CurrentRevision();
  command.timestamp_utc = "2026-01-01T00:00:00Z";
  command.type = type;
  command.payload = std::move(payload);
  command.idempotency_key = "key-" + NewId("k");
  return command;
}

namespace {

bool ChangesMedia(CommandType type) {
  switch (type) {
    case CommandType::ImportMedia:
    case CommandType::RemoveMedia:
    case CommandType::RelinkMedia:
    case CommandType::SetMediaStreams:
    case CommandType::AttachProxy:
    case CommandType::DetachProxy:
      return true;
    default:
      return false;
  }
}

}  // namespace

void Session::Run(CommandType type, commands::CommandPayload payload) {
  if (store_ == nullptr) return;
  try {
    (void)store_->Execute(Envelope(type, std::move(payload)));
    Reload(ChangesMedia(type));
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
  }
}

bool Session::Apply(const ui::EditPlan& plan) {
  if (store_ == nullptr || graph_.root() == nullptr) return false;
  if (!plan.ok) {
    ShowStatus(QString::fromStdString(plan.refusal));
    return false;
  }
  std::vector<commands::CommandEnvelope> envelopes;
  for (const auto& planned : plan.commands) envelopes.push_back(Envelope(planned.type, planned.payload));
  try {
    (void)store_->ExecuteGroup(std::move(envelopes), plan.label);
  } catch (const std::exception& error) {
    ShowStatus(tr("%1: %2").arg(QString::fromStdString(plan.label), error.what()));
    Reload();
    return false;
  }
  Reload(std::any_of(plan.commands.begin(), plan.commands.end(),
                     [](const auto& command) { return ChangesMedia(command.type); }));
  ShowStatus(plan.notes.empty() ? QString::fromStdString(plan.label) : QString::fromStdString(plan.label + " - " + plan.notes.front()));
  return true;
}

void Session::undoTo(int step) {
  if (store_ == nullptr) return;
  try {
    while (static_cast<int>(store_->AppliedStepCount()) > step && store_->CanUndo()) (void)store_->Undo("user", "2026-01-01T00:00:00Z");
    while (static_cast<int>(store_->AppliedStepCount()) < step && store_->CanRedo()) (void)store_->Redo("user", "2026-01-01T00:00:00Z");
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
  }
  Reload();
}

ui::EditPlan Session::InsertPlanAt(const std::string& media_id, const RationalTime& at, const std::string& video_track, const std::string& audio_track, bool insert) {
  if (store_ == nullptr || graph_.root() == nullptr) return ui::EditPlan::Refuse("There is no project");
  bool has_video = true, has_audio = true;
  RationalTime duration(0, 1);
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), R"sql(
      SELECT m.duration_num, m.duration_den,
             EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = m.id AND s.kind = 'video'),
             EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = m.id AND s.kind = 'audio')
        FROM media m WHERE m.id = ?;
    )sql");
    statement.Bind(1, media_id);
    if (!statement.Step()) return ui::EditPlan::Refuse("That media is not in the project");
    duration = RationalTime(statement.ColumnInt(0), statement.ColumnInt(1));
    has_video = statement.ColumnInt(2) != 0;
    has_audio = statement.ColumnInt(3) != 0;
  }
  ui::SourceRange source{media_id, media_id, RationalTime(0, 1), duration, has_video, has_audio};
  for (const auto& entry : media_) {
    const auto map = entry.toMap();
    if (map["id"].toString().toStdString() == media_id) source.name = map["name"].toString().toStdString();
  }
  return ui::PlanInsertEdit(EditContextFor(), source, at, video_track, audio_track, insert ? ui::OverlapMode::Insert : ui::OverlapMode::Overwrite);
}

ui::EditPlan Session::InsertPlanFor(const std::string& media_id, bool insert) {
  if (store_ == nullptr || graph_.root() == nullptr) return ui::EditPlan::Refuse("There is no project");
  // The lowest video track and the lowest audio track that are not locked.
  const auto* sequence = graph_.root();
  std::string video, audio;
  std::int64_t video_order = 1 << 30, audio_order = 1 << 30;
  for (const auto& track : sequence->tracks) {
    if (track.locked || track.is_bus) continue;
    if (track.kind == model::TrackKind::Video && track.order < video_order) { video = track.id; video_order = track.order; }
    if (track.kind == model::TrackKind::Audio && track.order < audio_order) { audio = track.id; audio_order = track.order; }
  }
  // Marked points decide what is placed and where; without them it is the whole media at the playhead.
  RationalTime duration(0, 1);
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT duration_num, duration_den FROM media WHERE id = ?;");
    statement.Bind(1, media_id);
    if (!statement.Step()) return ui::EditPlan::Refuse("That media is not in the project");
    duration = RationalTime(statement.ColumnInt(0), statement.ColumnInt(1));
  }
  ui::ThreePointInput input;
  input.playhead = transport_.position();
  input.sequence_in = mark_in_;
  input.sequence_out = mark_out_;
  input.source_duration = duration;
  const auto resolved = ui::ResolveThreePoint(input);
  if (!resolved.ok) return ui::EditPlan::Refuse(resolved.refusal);
  auto plan = InsertPlanAt(media_id, resolved.at, video, audio, insert);
  return plan;
}

void Session::insertMedia(const QString& media_id, bool insert) { (void)Apply(InsertPlanFor(media_id.toStdString(), insert)); }

void Session::selectMedia(const QString& media_id) { source_media_ = media_id.toStdString(); }

void Session::importMedia(const QStringList& urls) {
  if (store_ == nullptr) {
    ShowStatus(tr("Open or create a project first"));
    return;
  }
  for (const auto& url : urls) {
    const auto path = LocalPath(url).toStdString();
    const auto title = QFileInfo(QString::fromStdString(path)).fileName();
    runner_->Run("import", ("Importing " + title).toStdString(), [this, path](ui::JobContext& context) {
      media::IngestRequest request;
      request.project_id = project_id_;
      request.author_id = "user";
      request.timestamp_utc = "2026-01-01T00:00:00Z";
      request.media_id = NewId("media");
      request.path = path;
      request.base_revision = store_->CurrentRevision();
      context.Progress(0, 0, "Probing");
      const auto result = media::IngestFile(*store_, request);
      context.Progress(1, 1, result.already_present ? "Already in the project" : "Imported");
    });
  }
  // The list is refreshed when each job ends.
  jobs_.Observe([this](const ui::JobInfo& info) {
    if (info.kind == "import" && info.state == ui::JobState::Succeeded) {
      QMetaObject::invokeMethod(this, [this] { RefreshMedia(); }, Qt::QueuedConnection);
    }
  });
}

std::set<std::string> Session::SelectedOrUnderPlayhead() const {
  std::set<std::string> ids = selection_.clips();
  return ids;
}

void Session::setClipSpeed(double percent, bool reversed, bool maintain_pitch) {
  if (selection_.empty() || !(percent > 0.0)) return;
  std::vector<ui::PlannedCommand> planned;
  const auto rate = RationalTime(static_cast<std::int64_t>(percent * 100.0 + 0.5), 10000);
  for (const auto& id : selection_.clips()) {
    commands::SetClipSpeedPayload payload;
    payload.id = id;
    payload.playback_rate = rate;
    payload.reversed = reversed;
    payload.maintain_pitch = maintain_pitch;
    payload.propagate_links = false;
    planned.push_back({CommandType::SetClipSpeed, payload});
  }
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Change Speed";
  plan.commands = std::move(planned);
  (void)Apply(plan);
}

void Session::setTrackFlag(const QString& track_id, const QString& flag, bool on) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return;
  const auto* track = sequence->FindTrack(track_id.toStdString());
  if (track == nullptr) return;
  commands::SetTrackStatePayload payload{track->id, track->locked, track->muted, track->solo, track->gain_db, track->pan, track->name};
  if (flag == "locked") payload.locked = on;
  else if (flag == "muted") payload.muted = on;
  else if (flag == "solo") payload.solo = on;
  else return;
  Run(CommandType::SetTrackState, payload);
}

bool Session::audioSetLevel(const QString& track_id, double gain_db, double pan) {
  const auto* sequence = graph_.root();
  const auto* track = sequence != nullptr ? sequence->FindTrack(track_id.toStdString()) : nullptr;
  if (track == nullptr || track->kind != model::TrackKind::Audio || track->locked) return false;
  commands::SetTrackStatePayload payload{track->id, track->locked, track->muted, track->solo,
                                         std::clamp(gain_db, -96.0, 24.0), std::clamp(pan, -1.0, 1.0), track->name};
  Run(CommandType::SetTrackState, payload);
  return true;
}

bool Session::audioRename(const QString& track_id, const QString& name) {
  const auto* sequence = graph_.root();
  const auto* track = sequence != nullptr ? sequence->FindTrack(track_id.toStdString()) : nullptr;
  const auto trimmed = name.trimmed();
  if (track == nullptr || track->kind != model::TrackKind::Audio || track->locked || trimmed.isEmpty()) return false;
  Run(CommandType::SetTrackState, commands::SetTrackStatePayload{track->id, track->locked, track->muted, track->solo,
                                                                  track->gain_db, track->pan, trimmed.toStdString()});
  return true;
}

bool Session::audioSetOutput(const QString& track_id, const QString& bus_id) {
  const auto* sequence = graph_.root();
  const auto* track = sequence != nullptr ? sequence->FindTrack(track_id.toStdString()) : nullptr;
  if (track == nullptr || track->kind != model::TrackKind::Audio || track->locked ||
      (track->is_bus && bus_id == track_id)) return false;
  Run(CommandType::SetTrackRouting,
      commands::SetTrackRoutingPayload{track->id, track->is_bus, bus_id.toStdString(),
                                       [&] { std::vector<commands::TrackSend> value; for (const auto& s : track->sends) value.push_back({s.bus_id, s.gain_db, s.pre_fader}); return value; }()});
  return true;
}

bool Session::audioSetSend(const QString& track_id, const QString& bus_id, double gain_db, bool pre_fader, bool enabled) {
  const auto* sequence = graph_.root();
  const auto* track = sequence != nullptr ? sequence->FindTrack(track_id.toStdString()) : nullptr;
  const auto* bus = sequence != nullptr ? sequence->FindTrack(bus_id.toStdString()) : nullptr;
  if (track == nullptr || bus == nullptr || track->kind != model::TrackKind::Audio || !bus->is_bus || track->locked || track == bus) return false;
  std::vector<commands::TrackSend> sends;
  for (const auto& send : track->sends) {
    if (send.bus_id != bus_id.toStdString()) sends.push_back({send.bus_id, send.gain_db, send.pre_fader});
  }
  if (enabled) sends.push_back({bus_id.toStdString(), std::clamp(gain_db, -96.0, 24.0), pre_fader});
  Run(CommandType::SetTrackRouting, commands::SetTrackRoutingPayload{track->id, track->is_bus, track->output_bus_id, std::move(sends)});
  return true;
}

QString Session::audioAddBus(const QString& name) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return {};
  std::int64_t order = 0;
  for (const auto& track : sequence->tracks) if (track.kind == model::TrackKind::Audio) order = std::max(order, track.order + 1);
  const auto id = NewId("bus");
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Add Audio Bus";
  plan.commands.push_back({CommandType::AddAudioTrack,
                           commands::AddTrackPayload{id, sequence_id_, order, "stereo", name.trimmed().isEmpty() ? "Bus" : name.trimmed().toStdString()}});
  plan.commands.push_back({CommandType::SetTrackRouting, commands::SetTrackRoutingPayload{id, true, {}, {}}});
  return Apply(plan) ? QString::fromStdString(id) : QString();
}

bool Session::audioRemoveBus(const QString& track_id) {
  const auto* sequence = graph_.root();
  const auto* track = sequence != nullptr ? sequence->FindTrack(track_id.toStdString()) : nullptr;
  if (track == nullptr || !track->is_bus || track->locked) return false;
  for (const auto& source : sequence->tracks) {
    if (source.output_bus_id == track->id) {
      ShowStatus(tr("Route %1 to another output before removing %2")
                     .arg(QString::fromStdString(source.name), QString::fromStdString(track->name)));
      return false;
    }
    if (std::any_of(source.sends.begin(), source.sends.end(), [&](const timeline::TrackSend& send) {
          return send.bus_id == track->id;
        })) {
      ShowStatus(tr("Remove sends to %1 before deleting it").arg(QString::fromStdString(track->name)));
      return false;
    }
  }
  Run(CommandType::RemoveTrack, commands::RemoveTrackPayload{track->id});
  return graph_.root() == nullptr || graph_.root()->FindTrack(track_id.toStdString()) == nullptr;
}

// ------------------------------------------------------------------- transport ----

void Session::SeekTo(const RationalTime& to) {
  const bool was_audio = transport_.audio_driven();
  transport_.Seek(to);
  if (engine_ != nullptr && was_audio) engine_->Seek(transport_.position());
  emit playheadChanged();
  RefreshInspector();
  RequestFrame();
}

void Session::seek(double seconds) {
  const auto rate = graph_.root() != nullptr ? graph_.root()->frame_rate : time::kFrameRate25;
  const auto frames = static_cast<std::int64_t>(std::llround(seconds * rate.numerator / rate.denominator));
  SeekTo(RationalTime::FromFrames(std::max<std::int64_t>(frames, 0), rate));
}

void Session::RequestFrame(ui::PresentationMode mode) {
  if (presenter_ == nullptr || graph_.root() == nullptr) return;
  const auto* sequence = graph_.root();
  if (sequence->width <= 0 || sequence->height <= 0) return;
  const ui::SizePx full{static_cast<int>(sequence->width), static_cast<int>(sequence->height)};
  (void)presenter_->Request(transport_.position(), ui::RenderSizeFor(full, quality_, displayed_), mode);
  // RenderFrame schedules the following frames during playback. Direct priming is reserved for discontinuous jumps.
  if (engine_ != nullptr && mode == ui::PresentationMode::LatestOnly) engine_->PrimeReadAhead(transport_.position());
}

void Session::StartPlayback() {
  if (engine_ == nullptr || !audio_enabled_) return;
  if (transport_.audio_driven()) {
    if (sink_ == nullptr) sink_ = audio::OpenDefaultAudioSink();
    if (!sink_->drives_clock()) {
      ShowStatus(tr("No audio output is available; playback will continue without sound"));
      return;
    }
    engine_->Seek(transport_.position());
    engine_->Play(*sink_);
  } else {
    engine_->Pause();
  }
}

void Session::StopPlayback() {
  if (engine_ != nullptr) engine_->Pause();
}

void Session::SyncAudioToTransport() {
  if (engine_ == nullptr) return;
  if (transport_.audio_driven() && !engine_->playing()) StartPlayback();
  else if (!transport_.audio_driven() && engine_->playing()) StopPlayback();
}

void Session::OnTick() {
  if (engine_ == nullptr) return;
  // A pass of automation ends when the transport does.
  if (!automation_passes_.empty() && !transport_.playing()) FlushAutomation(transport_.position());
  if (++audio_meter_ticks_ >= 6) {
    audio_meter_ticks_ = 0;
    scope_sample_due_.store(true, std::memory_order_relaxed);
    emit audioMixerChanged();
    RefreshScopes();
  }
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const auto dt = std::chrono::duration<double>(now - last).count();
  last = now;
  if (!transport_.playing()) return;
  ui::Transport::Tick tick;
  if (transport_.audio_driven() && engine_->playing() && audio_enabled_) {
    const auto state = engine_->playback_state();
    if (state == audio::SinkState::Failed || state == audio::SinkState::Stopped) {
      // A device can disappear after it opened (Bluetooth and HDMI endpoints do
      // this in normal use). Do not leave the transport following its frozen
      // clock: stop audio and let the wall clock drive the rest of this play.
      const auto error = engine_->playback_error();
      StopPlayback();
      sink_.reset();
      ShowStatus(error.empty()
                     ? tr("Audio output stopped; playback will continue without sound")
                     : tr("Audio output failed; playback will continue without sound: %1")
                           .arg(QString::fromStdString(error)));
      tick = transport_.Advance(dt);
    } else {
      // The audio clock leads: the playhead is wherever the sound is.
      const auto position = engine_->position();
      const auto before = transport_.position();
      transport_.Seek(position);
      tick.moved = position.Compare(before) != 0;
      if (state == audio::SinkState::Finished || position.Compare(graph_.root()->Duration()) >= 0) {
        if (transport_.looping()) {
          transport_.Seek(mark_in_.value_or(RationalTime(0, 1)));
          engine_->Seek(transport_.position());
          engine_->Play(*sink_);
          tick.looped = true;
        } else {
          transport_.Stop();
          StopPlayback();
          emit transportChanged();
        }
      } else if (transport_.looping() && mark_out_ && position.Compare(*mark_out_) >= 0) {
        transport_.Seek(mark_in_.value_or(RationalTime(0, 1)));
        engine_->Seek(transport_.position());
        tick.looped = true;
      }
    }
  } else {
    tick = transport_.Advance(dt);
    if (tick.reached_boundary) {
      StopPlayback();
      emit transportChanged();
    }
  }
  if (tick.moved) {
    emit playheadChanged();
    RequestFrame(tick.looped ? ui::PresentationMode::LatestOnly : ui::PresentationMode::Playback);
    if (prefs_.GetBool("timeline.scroll_follows_playhead")) emit timelineAction("follow");
  }
}

// -------------------------------------------------------------------- inspector edits ----

void Session::addEffect(const QString& effect_type, const QString& preset) {
  const auto id = PrimaryClip();
  if (!id) {
    ShowStatus(tr("Select a clip first"));
    return;
  }
  (void)Apply(ui::PlanAddEffect(EditContextFor(), *id, effect_type.toStdString(), preset.toStdString()));
}

void Session::removeEffect(const QString& effect_id) {
  if (const auto id = PrimaryClip()) (void)Apply(ui::PlanRemoveEffect(EditContextFor(), *id, effect_id.toStdString()));
}

void Session::setEffectEnabled(const QString& effect_id, bool enabled) {
  if (const auto id = PrimaryClip()) (void)Apply(ui::PlanSetEffectEnabled(EditContextFor(), *id, effect_id.toStdString(), enabled));
}

void Session::moveEffect(const QString& effect_id, int places) {
  if (const auto id = PrimaryClip()) (void)Apply(ui::PlanMoveEffect(EditContextFor(), *id, effect_id.toStdString(), places));
}

namespace {

RationalTime LocalTimeOf(const timeline::Sequence& sequence, const std::string& clip_id, const RationalTime& playhead) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id == clip_id) {
        const auto local = playhead.Subtract(clip.timeline_start);
        return local.Compare(RationalTime(0, 1)) < 0 ? RationalTime(0, 1) : local;
      }
    }
  }
  return RationalTime(0, 1);
}

}  // namespace

void Session::setParameter(const QString& effect_id, const QString& name, const QVariantList& components) {
  const auto id = PrimaryClip();
  if (!id || graph_.root() == nullptr) return;
  anim::Value value;
  value.dimension = static_cast<int>(std::min<qsizetype>(components.size(), 4));
  for (int i = 0; i < value.dimension; ++i) value.components[static_cast<std::size_t>(i)] = components[i].toDouble();
  (void)Apply(ui::PlanSetParameter(EditContextFor(), *id, effect_id.toStdString(), name.toStdString(), value, LocalTimeOf(*graph_.root(), *id, transport_.position())));
}

void Session::toggleAnimation(const QString& effect_id, const QString& name) {
  if (const auto id = PrimaryClip(); id && graph_.root() != nullptr) {
    (void)Apply(ui::PlanToggleAnimation(EditContextFor(), *id, effect_id.toStdString(), name.toStdString(), LocalTimeOf(*graph_.root(), *id, transport_.position())));
  }
}

void Session::toggleKeyframe(const QString& effect_id, const QString& name) {
  if (const auto id = PrimaryClip(); id && graph_.root() != nullptr) {
    (void)Apply(ui::PlanToggleKeyframe(EditContextFor(), *id, effect_id.toStdString(), name.toStdString(), LocalTimeOf(*graph_.root(), *id, transport_.position())));
  }
}

void Session::resetParameter(const QString& effect_id, const QString& name) {
  if (const auto id = PrimaryClip()) (void)Apply(ui::PlanResetParameter(EditContextFor(), *id, effect_id.toStdString(), name.toStdString()));
}

void Session::jumpToKeyframe(const QString& effect_id, const QString& name, bool next) {
  const auto id = PrimaryClip();
  if (!id || graph_.root() == nullptr) return;
  for (const auto& track : graph_.root()->tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id != *id) continue;
      const auto local = LocalTimeOf(*graph_.root(), *id, transport_.position());
      const auto state = ui::BuildInspector(clip, local);
      for (const auto& effect : state.effects) {
        if (effect.id != effect_id.toStdString()) continue;
        for (const auto& parameter : effect.parameters) {
          if (parameter.name != name.toStdString()) continue;
          const auto target = next ? ui::NextKeyframe(parameter, local) : ui::PreviousKeyframe(parameter, local);
          if (target) SeekTo(clip.timeline_start.Add(*target));
        }
      }
    }
  }
}

QVariantList Session::effectCatalogue(const QString& query) const {
  QVariantList list;
  for (const auto& entry : ui::EffectCatalogue(query.toStdString())) {
    list.push_back(QVariantMap{{"id", QString::fromStdString(entry.id)},
                               {"name", QString::fromStdString(entry.name)},
                               {"category", QString::fromStdString(entry.category)},
                               {"needsAsset", entry.needs_asset},
                               {"assetKind", QString::fromStdString(entry.asset_kind)}});
  }
  return list;
}

QVariantList Session::colorSpaces() const {
  struct Entry { const char* id; const char* label; const char* group; };
  static const Entry entries[] = {
      {"rec709", "Rec.709", "Display"}, {"srgb", "sRGB", "Display"}, {"p3-d65", "Display P3", "Display"}, {"rec2020", "Rec.2020", "Display"},
      {"rec2020-pq", "Rec.2020 PQ (HDR10)", "HDR"}, {"rec2020-hlg", "Rec.2020 HLG", "HDR"},
      {"linear-rec709", "Linear Rec.709", "Linear"}, {"linear-p3", "Linear P3", "Linear"}, {"linear-rec2020", "Linear Rec.2020", "Linear"},
      {"acescg", "ACEScg (AP1, linear)", "ACES"}, {"aces2065-1", "ACES2065-1 (AP0, linear)", "ACES"}, {"acescct", "ACEScct", "ACES"}, {"acescc", "ACEScc", "ACES"},
      {"arri-logc3", "ARRI LogC3 / Wide Gamut 3", "Camera log"}, {"sony-slog3", "Sony S-Log3 / S-Gamut3.Cine", "Camera log"},
      {"panasonic-vlog", "Panasonic V-Log / V-Gamut", "Camera log"}, {"red-log3g10", "RED Log3G10 / REDWideGamutRGB", "Camera log"},
  };
  QVariantList list;
  for (const auto& entry : entries) list << QVariantMap{{"id", entry.id}, {"label", entry.label}, {"group", entry.group}};
  return list;
}

void Session::addInputColorSpace(const QString& space) {
  if (!render::color::ParseSpace(space.toStdString())) {
    ShowStatus(tr("%1 is not a colour space this build defines").arg(space));
    return;
  }
  addEffect("input_colorspace", space);
}

QVariantMap Session::sequenceColor() const {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return {};
  return QVariantMap{{"working", QString::fromStdString(sequence->working_color_space)},
                     {"display", QString::fromStdString(sequence->display_color_space)},
                     {"managed", model::RenderSemantics::For(sequence->render_version).color_managed},
                     {"renderVersion", static_cast<int>(sequence->render_version)}};
}

bool Session::setSequenceColor(const QString& working, const QString& display) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || store_ == nullptr) return false;
  if (!render::color::ParseSpace(working.toStdString()) || !render::color::ParseSpace(display.toStdString())) {
    ShowStatus(tr("Choose colour spaces this build defines"));
    return false;
  }
  commands::UpdateSequenceSettingsPayload payload;
  payload.id = sequence->id;
  payload.settings.name = sequence->name;
  payload.settings.frame_rate = sequence->frame_rate;
  payload.settings.width = sequence->width;
  payload.settings.height = sequence->height;
  payload.settings.pixel_aspect = sequence->pixel_aspect;
  payload.settings.sample_rate = sequence->sample_rate;
  payload.settings.channel_layout = sequence->channel_layout;
  payload.settings.working_color_space = working.toStdString();
  payload.settings.display_color_space = display.toStdString();
  payload.settings.field_order = sequence->field_order;
  payload.settings.drop_frame = sequence->drop_frame;
  // The spaces mean nothing under the rules of a sequence made before colour management: asking for them upgrades it.
  if (!model::RenderSemantics::For(sequence->render_version).color_managed) payload.settings.render_version = model::kCurrentRenderVersion;
  Run(CommandType::UpdateSequenceSettings, payload);
  return true;
}

void Session::setLutFolders(const QStringList& folders) {
  std::vector<std::filesystem::path> list;
  // The application's own folder is always there; the person's are added to it.
  if (!config_dir_.isEmpty()) list.emplace_back(std::filesystem::path(config_dir_.toStdString()) / "luts");
  for (const auto& folder : folders) list.emplace_back(LocalPath(folder).toStdString());
  luts_.SetFolders(std::move(list));
  luts_.Rescan();
}

QString Session::lutFolder() const {
  return config_dir_.isEmpty() ? QString() : config_dir_ + "/luts";
}

QVariantList Session::lutEntries(const QString& query) const {
  QVariantList list;
  for (const auto* entry : luts_.Search(query.toStdString())) {
    list.push_back(QVariantMap{{"name", QString::fromStdString(entry->name)},
                               {"path", QString::fromStdString(entry->path.string())},
                               {"size", entry->size},
                               {"curves", entry->curves},
                               {"group", QString::fromStdString(entry->path.parent_path().filename().string())},
                               {"folder", QString::fromStdString(entry->folder)}});
  }
  return list;
}

void Session::selectClipNamed(const QString& name) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (clip.name == name.toStdString()) {
        selection_.Select(*sequence, clip.id, ui::Selection::Mode::Replace, prefs_.GetBool("timeline.linked_selection"));
        NotifySelectionChanged();
        return;
      }
    }
  }
}

bool Session::ctrlHeld() const { return QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier); }

void Session::analyseClip(const QString& kind) {
  const auto id = PrimaryClip();
  const auto* sequence = graph_.root();
  if (!id || sequence == nullptr || store_ == nullptr) {
    ShowStatus(tr("Select a clip to analyse"));
    return;
  }
  const timeline::Clip* clip = nullptr;
  for (const auto& track : sequence->tracks) {
    for (const auto& candidate : track.clips) {
      if (candidate.id == *id) clip = &candidate;
    }
  }
  if (clip == nullptr || clip->source_kind != model::SourceKind::Media) {
    ShowStatus(tr("Only a clip of video media can be analysed"));
    return;
  }
  const auto media_id = clip->source_id;
  std::string path;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT original_path FROM media WHERE id = ?;");
    statement.Bind(1, media_id);
    if (statement.Step()) path = statement.ColumnText(0);
  }
  if (path.empty()) {
    ShowStatus(tr("The media is offline"));
    return;
  }
  const auto clip_id = *id;
  const auto source_in = clip->source_in, source_out = clip->source_out;
  const auto rate = sequence->frame_rate;
  const bool stabilise = kind == "stabilize";
  const auto title = (stabilise ? std::string("Stabilising ") : std::string("Analysing motion in ")) + (clip->name.empty() ? clip->id : clip->name);
  const auto limit = analysis_limit_;
  runner_->Run(ToStd(kind), title, [this, path, clip_id, source_in, source_out, rate, stabilise, limit](ui::JobContext& context) {
    auto source = media::SourceRegistry::Instance().Open(path);
    if (source == nullptr) throw std::runtime_error("The media could not be opened");
    // The frame rate of the media itself: for slow motion, pairs of its own pictures are what get interpolated between.
    time::FrameRate source_rate = rate;
    for (const auto& stream : source->probe().streams) {
      if (stream.kind == model::StreamKind::Video && stream.frame_rate.numerator > 0) source_rate = stream.frame_rate;
    }
    const auto analysis_rate = stabilise ? rate : source_rate;
    const auto span = source_out.Subtract(source_in);
    const auto frames = static_cast<std::size_t>(std::min<std::int64_t>(span.ToFrames(analysis_rate, time::RoundingMode::Nearest), static_cast<std::int64_t>(limit)));
    if (frames < 2) throw std::runtime_error("The clip is too short to analyse");
    // Frames are read in order and a few kept, which is how both analyses ask for them.
    std::map<std::size_t, media::VideoFrame> kept;
    const auto resolve = [&](std::size_t index) -> const media::VideoFrame* {
      if (const auto found = kept.find(index); found != kept.end()) return &found->second;
      auto frame = source->ReadVideo(source_in.Add(time::RationalTime::FromFrames(static_cast<std::int64_t>(index), analysis_rate)));
      if (!frame) return nullptr;
      auto converted = frame->format() == media::PixelFormat::RgbaF32 ? std::move(*frame) : media::ConvertFrame(*frame, media::PixelFormat::RgbaF32);
      for (auto it = kept.begin(); it != kept.end();) it = (index > 6 && it->first + 6 < index) ? kept.erase(it) : std::next(it);
      return &kept.emplace(index, std::move(converted)).first->second;
    };
    const auto cancel = context.cancel_token();
    if (stabilise) {
      const auto result = render::tracking::AnalyzeSimilarityStabilization(
          frames, rate, resolve, {}, cancel, [&](std::size_t done, std::size_t total) { context.Progress(done, total); });
      if (result.cancelled || context.cancelled()) return;
      auto effect = render::tracking::MakeSimilarityStabilizerEffect(result, "analysed");
      QMetaObject::invokeMethod(this, [this, clip_id, effect]() { (void)Apply(ui::PlanAddAnalysedEffect(EditContextFor(), clip_id, effect, "Stabilize")); }, Qt::QueuedConnection);
    } else {
      render::FlowAnalysis flow(flow_cache_, resolve, frames);
      flow.Observe([&](const render::FlowAnalysisProgress& p) { context.Progress(p.done, p.total_pairs, "pair " + std::to_string(p.done)); });
      flow.Start();
      while (true) {
        if (context.cancelled()) {
          flow.Cancel();
          break;
        }
        if (flow.Progress().finished) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      flow.Wait();
      const auto done = flow.Progress();
      if (!done.error.empty()) throw std::runtime_error(done.error);
    }
  });
}

void Session::cancelJob(int id) { jobs_.RequestCancel(id); }
void Session::clearFinishedJobs() {
  jobs_.ClearFinished();
  emit jobsChanged();
}

// ------------------------------------------------------------- commands and keys ----

QVariantList Session::commandList() const {
  QVariantList list;
  for (const auto& command : ui::BuiltInCommands().All()) {
    list.push_back(QVariantMap{{"id", QString::fromStdString(command.id)},
                               {"label", QString::fromStdString(command.label)},
                               {"category", QString::fromStdString(command.category)},
                               {"description", QString::fromStdString(command.description)},
                               {"shortcut", shortcutText(QString::fromStdString(command.id))}});
  }
  return list;
}

QVariantList Session::searchCommands(const QString& query) const {
  QVariantList list;
  for (const auto* command : ui::BuiltInCommands().Search(query.toStdString())) {
    list.push_back(QVariantMap{{"id", QString::fromStdString(command->id)},
                               {"label", QString::fromStdString(command->label)},
                               {"category", QString::fromStdString(command->category)},
                               {"shortcut", shortcutText(QString::fromStdString(command->id))}});
  }
  return list;
}

QString Session::shortcutText(const QString& command_id) const {
  QStringList parts;
  for (const auto& shortcut : keymap_.ShortcutsFor(command_id.toStdString())) parts << QString::fromStdString(ui::ToString(shortcut));
  return parts.join(", ");
}

bool Session::bindShortcut(const QString& command_id, const QString& chord) {
  const auto parsed = ui::ParseShortcut(chord.toStdString());
  if (!parsed || ui::BuiltInCommands().Find(command_id.toStdString()) == nullptr) return false;
  const auto displaced = keymap_.Bind(command_id.toStdString(), *parsed);
  SaveConfiguration();
  if (!displaced.empty()) ShowStatus(tr("%1 now runs %2; it was %3's").arg(chord, command_id, QString::fromStdString(displaced.front())));
  return true;
}

void Session::resetShortcuts() {
  keymap_.ResetAll();
  SaveConfiguration();
}

QVariantList Session::preferenceList() const {
  QVariantList list;
  for (const auto& def : prefs_.schema().All()) {
    QVariant value;
    const auto& v = prefs_.Get(def.key);
    if (const auto* b = std::get_if<bool>(&v)) value = *b;
    else if (const auto* i = std::get_if<std::int64_t>(&v)) value = static_cast<qlonglong>(*i);
    else if (const auto* d = std::get_if<double>(&v)) value = *d;
    else value = QString::fromStdString(std::get<std::string>(v));
    QStringList choices;
    for (const auto& choice : def.choices) choices << QString::fromStdString(choice);
    list.push_back(QVariantMap{{"key", QString::fromStdString(def.key)},
                               {"kind", static_cast<int>(def.kind)},
                               {"category", QString::fromStdString(def.category)},
                               {"label", QString::fromStdString(def.label)},
                               {"description", QString::fromStdString(def.description)},
                               {"value", value},
                               {"min", def.minimum},
                               {"max", def.maximum},
                               {"choices", choices},
                               {"restart", def.needs_restart},
                               {"isDefault", prefs_.IsDefault(def.key)}});
  }
  return list;
}

bool Session::setPreference(const QString& key, const QVariant& value) {
  const auto* def = prefs_.schema().Find(key.toStdString());
  if (def == nullptr) return false;
  try {
    switch (def->kind) {
      case ui::PreferenceKind::Bool: prefs_.Set(def->key, value.toBool()); break;
      case ui::PreferenceKind::Integer: prefs_.Set(def->key, static_cast<std::int64_t>(value.toLongLong())); break;
      case ui::PreferenceKind::Real: prefs_.Set(def->key, value.toDouble()); break;
      default: prefs_.Set(def->key, value.toString().toStdString()); break;
    }
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
    return false;
  }
  SaveConfiguration();
  if (def->key == "playback.default_quality") {
    if (const auto q = ui::ParseMonitorQuality(prefs_.GetText(def->key))) quality_ = *q;
    RequestFrame();
  }
  return true;
}

void Session::resetPreference(const QString& key) {
  try {
    prefs_.Reset(key.toStdString());
    SaveConfiguration();
  } catch (const std::exception&) {
  }
}

bool Session::handleKey(int qt_key, int modifiers, const QString& text, bool pressed, bool auto_repeat) {
  (void)text;
  const auto name = KeyName(qt_key);
  if (name.empty()) return false;
  // K held changes what J and L do.
  if (name == "K" && !(modifiers & Qt::ControlModifier)) {
    slow_modifier_ = pressed;
    transport_.SetSlowModifier(pressed);
    if (pressed && !auto_repeat) DoCommand("transport.stop");
    return true;
  }
  if (!pressed) return false;
  ui::Shortcut shortcut;
  shortcut.ctrl = (modifiers & Qt::ControlModifier) != 0;
  shortcut.shift = (modifiers & Qt::ShiftModifier) != 0;
  shortcut.alt = (modifiers & Qt::AltModifier) != 0;
  shortcut.key = name;
  // Plus is Shift+Equal: look it up as the plain key too.
  auto command = keymap_.Lookup(shortcut);
  if (!command && qt_key == Qt::Key_Plus) {
    shortcut.shift = false;
    command = keymap_.Lookup(shortcut);
  }
  if (!command) return false;
  const bool repeatable = *command == "transport.step_back" || *command == "transport.step_forward" || *command == "transport.step_back_many" ||
                          *command == "transport.step_forward_many" || *command == "timeline.zoom_in" || *command == "timeline.zoom_out" ||
                          *command == "timeline.next_edit" || *command == "timeline.previous_edit";
  if (auto_repeat && !repeatable) return true;
  DoCommand(*command);
  return true;
}

void Session::trigger(const QString& command_id) { DoCommand(command_id.toStdString()); }

void Session::DoCommand(const std::string& id) {
  if (id.rfind("multicam.", 0) == 0) {
    MulticamCommand(id);
    return;
  }
  const auto* sequence = graph_.root();
  const auto position = transport_.position();
  const auto ctx = [this] { return EditContextFor(); };
  if (id == "file.save") {
    if (store_) {
      try {
        store_->CreateSnapshot();
        ShowStatus(tr("Saved"));
      } catch (const std::exception& e) {
        ShowStatus(e.what());
      }
    }
  } else if (id == "file.open_project" || id == "file.new_project") {
    emit openRequested();
  } else if (id == "file.import_media") {
    emit importRequested();
  } else if (id == "file.ingest_media") {
    emit ingestRequested();
  } else if (id == "file.export_media") {
    emit exportRequested();
  } else if (id == "file.quit") {
    emit quitRequested();
  } else if (id == "edit.undo") {
    if (store_ && store_->CanUndo()) {
      try {
        (void)store_->Undo("user", "2026-01-01T00:00:00Z");
      } catch (const std::exception& e) {
        ShowStatus(e.what());
      }
      Reload();
    }
  } else if (id == "edit.redo") {
    if (store_ && store_->CanRedo()) {
      try {
        (void)store_->Redo("user", "2026-01-01T00:00:00Z");
      } catch (const std::exception& e) {
        ShowStatus(e.what());
      }
      Reload();
    }
  } else if (id == "edit.select_all") {
    if (sequence) selection_.SelectAll(*sequence);
    NotifySelectionChanged();
  } else if (id == "edit.deselect_all") {
    selection_.Clear();
    NotifySelectionChanged();
  } else if (id == "edit.delete" || id == "edit.ripple_delete") {
    (void)Apply(ui::PlanDelete(ctx(), selection_.clips(), id == "edit.ripple_delete"));
  } else if (id == "edit.copy" || id == "edit.cut") {
    clipboard_.clear();
    if (sequence && !selection_.empty()) {
      RationalTime first(1000000, 1);
      for (const auto& track : sequence->tracks) {
        for (const auto& clip : track.clips) {
          if (selection_.Contains(clip.id) && clip.timeline_start.Compare(first) < 0) first = clip.timeline_start;
        }
      }
      for (const auto& clip_id : selection_.clips()) {
        if (const auto spec = ui::SpecOf(*sequence, clip_id, first)) clipboard_.push_back(*spec);
      }
      ShowStatus(tr("Copied %1 clip(s)").arg(clipboard_.size()));
      if (id == "edit.cut") (void)Apply(ui::PlanDelete(ctx(), selection_.clips(), false));
    }
  } else if (id == "edit.paste" || id == "edit.paste_insert") {
    if (clipboard_.empty()) ShowStatus(tr("There is nothing to paste"));
    else (void)Apply(ui::PlanPlace(ctx(), clipboard_, position, id == "edit.paste" ? ui::OverlapMode::Overwrite : ui::OverlapMode::Insert, "Paste"));
  } else if (id == "edit.duplicate") {
    if (sequence && !selection_.empty()) {
      std::vector<ui::ClipSpec> specs;
      RationalTime first(1000000, 1), last(0, 1);
      for (const auto& track : sequence->tracks) {
        for (const auto& clip : track.clips) {
          if (!selection_.Contains(clip.id)) continue;
          if (clip.timeline_start.Compare(first) < 0) first = clip.timeline_start;
          if (clip.end().Compare(last) > 0) last = clip.end();
        }
      }
      for (const auto& clip_id : selection_.clips()) {
        if (const auto spec = ui::SpecOf(*sequence, clip_id, first)) specs.push_back(*spec);
      }
      (void)Apply(ui::PlanPlace(ctx(), specs, last, ui::OverlapMode::Overwrite, "Duplicate"));
    }
  } else if (id == "edit.preferences") {
    emit preferencesRequested();
  } else if (id == "edit.command_palette") {
    emit commandPaletteRequested();
  } else if (id == "help.shortcuts") {
    emit shortcutsRequested();
  } else if (id == "timeline.add_edit" || id == "timeline.add_edit_all") {
    if (id == "timeline.add_edit" && !selection_.empty()) (void)Apply(ui::PlanSplitClips(ctx(), position, selection_.clips()));
    else (void)Apply(ui::PlanSplit(ctx(), position));
  } else if (id == "timeline.insert" || id == "timeline.overwrite") {
    std::string media = source_media_;
    if (media.empty() && !media_.isEmpty()) media = media_.front().toMap()["id"].toString().toStdString();
    if (media.empty()) ShowStatus(tr("Select media in the project panel first"));
    else (void)Apply(InsertPlanFor(media, id == "timeline.insert"));
  } else if (id == "timeline.lift" || id == "timeline.extract") {
    if (!mark_in_ || !mark_out_) ShowStatus(tr("Mark an in and an out point first"));
    else (void)Apply(id == "timeline.lift" ? ui::PlanLift(ctx(), *mark_in_, *mark_out_) : ui::PlanExtract(ctx(), *mark_in_, *mark_out_));
  } else if (id == "timeline.link") {
    const auto linked = sequence && !selection_.empty() && ui::PlanUnlink(ctx(), selection_.clips()).ok;
    (void)Apply(linked ? ui::PlanUnlink(ctx(), selection_.clips()) : ui::PlanLink(ctx(), selection_.clips()));
  } else if (id == "timeline.enable") {
    bool any_enabled = false;
    if (sequence) {
      for (const auto& track : sequence->tracks) {
        for (const auto& clip : track.clips) any_enabled = any_enabled || (selection_.Contains(clip.id) && clip.enabled);
      }
    }
    (void)Apply(ui::PlanSetEnabled(ctx(), selection_.clips(), !any_enabled));
  } else if (id == "timeline.speed") {
    emit speedRequested();
  } else if (id == "timeline.speed_ramp") {
    (void)rampOpen();
  } else if (id == "timeline.next_edit" || id == "timeline.previous_edit") {
    if (sequence) {
      std::optional<RationalTime> best;
      const bool next = id == "timeline.next_edit";
      for (const auto& track : sequence->tracks) {
        for (const auto& clip : track.clips) {
          for (const auto& point : {clip.timeline_start, clip.end()}) {
            if (next ? point.Compare(position) > 0 : point.Compare(position) < 0) {
              if (!best || (next ? point.Compare(*best) < 0 : point.Compare(*best) > 0)) best = point;
            }
          }
        }
      }
      if (best) SeekTo(*best);
    }
  } else if (id == "timeline.mark_in") {
    mark_in_ = position;
    if (mark_out_ && mark_out_->Compare(position) <= 0) mark_out_.reset();
    transport_.SetLoopRange(mark_in_, mark_out_);
    emit marksChanged();
  } else if (id == "timeline.mark_out") {
    mark_out_ = position;
    if (mark_in_ && mark_in_->Compare(position) >= 0) mark_in_.reset();
    transport_.SetLoopRange(mark_in_, mark_out_);
    emit marksChanged();
  } else if (id == "timeline.clear_in" || id == "timeline.clear_out" || id == "timeline.clear_in_out") {
    if (id != "timeline.clear_out") mark_in_.reset();
    if (id != "timeline.clear_in") mark_out_.reset();
    transport_.SetLoopRange(mark_in_, mark_out_);
    emit marksChanged();
  } else if (id == "timeline.go_in") {
    if (mark_in_) SeekTo(*mark_in_);
  } else if (id == "timeline.go_out") {
    if (mark_out_) SeekTo(*mark_out_);
  } else if (id == "timeline.add_marker") {
    commands::AddMarkerPayload marker;
    marker.id = NewId("marker");
    marker.owner_kind = model::MarkerOwner::Sequence;
    marker.owner_id = sequence_id_;
    marker.start = position;
    marker.end = position;
    marker.label = "Marker";
    Run(CommandType::AddMarker, marker);
  } else if (id == "timeline.snap") {
    setSnap(!snap());
  } else if (id == "timeline.zoom_in" || id == "timeline.zoom_out" || id == "timeline.zoom_fit") {
    emit timelineAction(QString::fromStdString(id.substr(9)));
  } else if (id == "timeline.add_video_track" || id == "timeline.add_audio_track") {
    if (sequence) {
      const bool video = id == "timeline.add_video_track";
      std::int64_t order = 0;
      std::size_t count = 0;
      for (const auto& track : sequence->tracks) {
        if (track.kind == (video ? model::TrackKind::Video : model::TrackKind::Audio) && !track.is_bus) {
          order = std::max(order, track.order + 1);
          ++count;
        }
      }
      const auto track_id = NewId(video ? "v" : "a");
      Run(video ? CommandType::AddVideoTrack : CommandType::AddAudioTrack,
          commands::AddTrackPayload{track_id, sequence_id_, order, "stereo", (video ? "V" : "A") + std::to_string(count + 1)});
    }
  } else if (id.rfind("tool.", 0) == 0) {
    setTool(QString::fromStdString(id.substr(5)));
  } else if (id == "transport.play_pause") {
    transport_.TogglePlay();
    SyncAudioToTransport();
    emit transportChanged();
  } else if (id == "transport.play_forward") {
    transport_.PressForward();
    SyncAudioToTransport();
    emit transportChanged();
  } else if (id == "transport.play_reverse") {
    transport_.PressReverse();
    SyncAudioToTransport();
    emit transportChanged();
  } else if (id == "transport.stop") {
    transport_.PressStop();
    StopPlayback();
    emit transportChanged();
  } else if (id == "transport.step_back" || id == "transport.step_forward" || id == "transport.step_back_many" || id == "transport.step_forward_many") {
    StopPlayback();
    transport_.StepFrames(id == "transport.step_back" ? -1 : id == "transport.step_forward" ? 1 : id == "transport.step_back_many" ? -5 : 5);
    emit playheadChanged();
    emit transportChanged();
    RefreshInspector();
    RequestFrame();
  } else if (id == "transport.go_start" || id == "transport.go_end") {
    StopPlayback();
    transport_.Stop();
    if (id == "transport.go_start") transport_.GoToStart();
    else transport_.GoToEnd();
    emit playheadChanged();
    emit transportChanged();
    RefreshInspector();
    RequestFrame();
  } else if (id == "transport.loop") {
    setLoop(!transport_.looping());
  } else if (id.rfind("monitor.quality_", 0) == 0) {
    setMonitorQuality(QString::fromStdString(id.substr(16)));
  } else if (id == "monitor.safe_margins") {
    setSafeMargins(!safeMargins());
  } else if (id == "monitor.fullscreen" || id == "monitor.zoom_fit" || id == "monitor.zoom_100") {
    emit monitorAction(QString::fromStdString(id.substr(8)));
  } else if (id.rfind("workspace.", 0) == 0 || id.rfind("panel.", 0) == 0) {
    emit workspaceCommand(QString::fromStdString(id));
  }
}

void Session::setRenderFrameForTest(const QString& path) {
  QImage image(path);
  if (!image.isNull()) {
    {
      const std::lock_guard<std::mutex> lock(frame_mutex_);
      frame_ = image.convertToFormat(QImage::Format_RGBA8888);
      gpu_frame_ = {};
    }
    emit frameReady();
  }
}

bool Session::saveScreenshot(const QString& path) {
  QQuickWindow* best = nullptr;
  for (auto* window : QGuiApplication::allWindows()) {
    auto* quick = qobject_cast<QQuickWindow*>(window);
    if (quick == nullptr || !quick->isVisible()) continue;
    if (best == nullptr || quick->width() * quick->height() > best->width() * best->height()) best = quick;
  }
  return best != nullptr && best->grabWindow().save(path);
}


// ------------------------------------------------------------------- speed ramp ----

namespace {

RationalTime SnapSeconds(double seconds, const timeline::Sequence& sequence) {
  const auto rate = sequence.frame_rate;
  const auto frames = static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator)));
  return RationalTime::FromFrames(frames, rate);
}

const timeline::Clip* FindClipIn(const timeline::Sequence& sequence, const std::string& id) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id == id) return &clip;
    }
  }
  return nullptr;
}

QVariantMap SegmentMap(const commands::SpeedSegment& segment, double start) {
  QVariantMap map;
  map["start"] = start;
  map["duration"] = static_cast<double>(segment.duration.numerator()) / static_cast<double>(segment.duration.denominator());
  map["startSpeed"] = segment.start_speed;
  map["endSpeed"] = segment.end_speed;
  return map;
}

}  // namespace

ui::RampModel Session::RampModelFor(const std::string& clip_id) const {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return {};
  const auto* clip = FindClipIn(*sequence, clip_id);
  return clip == nullptr ? ui::RampModel{} : ui::RampModel::FromClip(*clip, sequence->frame_rate);
}

bool Session::ApplyRamp(const std::string& clip_id, const ui::RampModel& model) {
  const bool ok = Apply(ui::PlanApplyRamp(EditContextFor(), clip_id, model));
  if (ok && clip_id == ramp_clip_) {
    ramp_ = RampModelFor(clip_id);
    ramp_dirty_ = false;
    emit rampChanged();
  }
  return ok;
}

bool Session::rampOpen() {
  // A picture clip is preferred over the sound linked to it; the ramp reaches the sound either way.
  auto id = PrimaryClip();
  if (graph_.root() != nullptr) {
    for (const auto& track : graph_.root()->tracks) {
      if (track.kind != model::TrackKind::Video) continue;
      for (const auto& clip : track.clips) {
        if (selection_.Contains(clip.id)) {
          id = clip.id;
          break;
        }
      }
    }
  }
  if (!id || graph_.root() == nullptr) {
    ShowStatus(tr("Select a clip to ramp its speed"));
    return false;
  }
  ramp_clip_ = *id;
  ramp_ = RampModelFor(*id);
  ramp_dirty_ = false;
  emit rampChanged();
  emit rampRequested();
  return !ramp_.empty();
}

void Session::rampClose() {
  ramp_clip_.clear();
  ramp_ = {};
  ramp_dirty_ = false;
  emit rampChanged();
}

void Session::setRampLive(bool live) {
  if (ramp_live_ == live) return;
  ramp_live_ = live;
  emit rampChanged();
}

double Session::rampClipStart() const {
  const auto* sequence = graph_.root();
  const auto* clip = sequence == nullptr ? nullptr : FindClipIn(*sequence, ramp_clip_);
  return clip == nullptr ? 0.0 : Seconds(clip->timeline_start);
}

QVariantList Session::rampSegments() const {
  QVariantList list;
  double start = 0.0;
  for (const auto& segment : ramp_.segments()) {
    list << SegmentMap(segment, start);
    start += Seconds(segment.duration);
  }
  return list;
}

QVariantList Session::rampGraph() const {
  QVariantList list;
  for (const auto& [t, v] : ramp_.GraphPoints()) list << QVariant(QVariantList{t, v});
  return list;
}

double Session::rampDuration() const { return ramp_.empty() ? 0.0 : Seconds(ramp_.duration()); }

QVariantList Session::rampExtent() const {
  const auto extent = ramp_.SourceExtent();
  return QVariantList{extent.first, extent.second};
}

QString Session::rampProblem() const {
  if (ramp_clip_.empty() || graph_.root() == nullptr || ramp_.empty()) return {};
  if (FindClipIn(*graph_.root(), ramp_clip_) == nullptr) return tr("The clip is no longer in the sequence");
  // The same check applying runs, so the dialog can say so before the button is pressed.
  const auto plan = ui::PlanApplyRamp(const_cast<Session*>(this)->EditContextFor(), ramp_clip_, ramp_);
  return plan.ok ? QString() : QString::fromStdString(plan.refusal);
}

#define CUTLINE_RAMP_EDIT(expression)     \
  do {                                    \
    if (ramp_clip_.empty()) return false; \
    if (!(expression)) return false;      \
    ramp_dirty_ = true;                   \
    emit rampChanged();                   \
    if (ramp_live_ && !ramp_dragging_) return rampApply(); \
    return true;                          \
  } while (false)

bool Session::rampSplit(double seconds) {
  if (graph_.root() == nullptr) return false;
  CUTLINE_RAMP_EDIT(ramp_.SplitAt(SnapSeconds(seconds, *graph_.root())));
}

bool Session::rampRemoveBoundary(int boundary) { CUTLINE_RAMP_EDIT(ramp_.RemoveBoundary(boundary)); }

bool Session::rampMoveBoundary(int boundary, double seconds) {
  if (graph_.root() == nullptr) return false;
  CUTLINE_RAMP_EDIT(ramp_.MoveBoundary(boundary, SnapSeconds(seconds, *graph_.root())));
}

bool Session::rampSetBoundarySpeed(int boundary, double speed, const QString& side) {
  const auto which = side == "before" ? ui::RampModel::Side::Before : side == "after" ? ui::RampModel::Side::After : ui::RampModel::Side::Both;
  CUTLINE_RAMP_EDIT(ramp_.SetBoundarySpeed(boundary, speed, which));
}

bool Session::rampSetSegmentSpeed(int segment, double start_speed, double end_speed) { CUTLINE_RAMP_EDIT(ramp_.SetSegmentSpeed(segment, start_speed, end_speed)); }
bool Session::rampFreeze(int segment) { CUTLINE_RAMP_EDIT(ramp_.Freeze(segment)); }
bool Session::rampReverse(int segment) { CUTLINE_RAMP_EDIT(ramp_.Reverse(segment)); }

bool Session::rampEase(int segment, const QString& ease) {
  const auto kind = ease == "in" ? ui::RampEase::EaseIn : ease == "out" ? ui::RampEase::EaseOut : ease == "inout" ? ui::RampEase::EaseInOut : ui::RampEase::Linear;
  CUTLINE_RAMP_EDIT(ramp_.SetEase(segment, kind));
}

bool Session::rampScale(double factor) { CUTLINE_RAMP_EDIT(ramp_.ScaleSpeeds(factor)); }

bool Session::rampSetDuration(double seconds) {
  if (graph_.root() == nullptr) return false;
  CUTLINE_RAMP_EDIT(ramp_.SetLastDuration(SnapSeconds(seconds, *graph_.root())));
}

#undef CUTLINE_RAMP_EDIT

bool Session::rampApply() {
  if (ramp_clip_.empty()) return false;
  return ApplyRamp(ramp_clip_, ramp_);
}

bool Session::rampClear() {
  if (ramp_clip_.empty()) return false;
  const bool ok = Apply(ui::PlanClearRamp(EditContextFor(), ramp_clip_));
  if (ok) {
    ramp_ = RampModelFor(ramp_clip_);
    ramp_dirty_ = false;
    emit rampChanged();
  }
  return ok;
}

void Session::rampDrag(bool dragging) {
  const bool released = ramp_dragging_ && !dragging;
  ramp_dragging_ = dragging;
  if (released && ramp_live_ && ramp_dirty_) (void)rampApply();
}

void Session::rampReset() {
  if (ramp_clip_.empty()) return;
  ramp_ = RampModelFor(ramp_clip_);
  ramp_dirty_ = false;
  emit rampChanged();
}

// Puts the playhead on the picture a time into the ramp shows, so a boundary can be looked at while it is adjusted.
void Session::rampPreview(double ramp_seconds) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || ramp_clip_.empty()) return;
  const auto* clip = FindClipIn(*sequence, ramp_clip_);
  if (clip == nullptr) return;
  SeekTo(clip->timeline_start.Add(SnapSeconds(std::clamp(ramp_seconds, 0.0, std::max(0.0, rampDuration() - 0.001)), *sequence)));
}

}  // namespace cutline::app

// Exporting from the application: choosing a preset, seeing what it will do, queueing it, and watching the queue.
//
// What an export means (the preset, the encoder chosen, the checks afterwards) is in exporter/. This holds the queue for
// the open project, builds the engine each job renders with, and reports the queue to the interface.

#include "app/Session.h"

#include "exporter/ExportPresets.h"
#include "exporter/ExportQueue.h"
#include "exporter/ExportValidation.h"
#include "exporter/ExportWorker.h"
#include "core/db/Sql.h"
#include "timeline/SequenceLoader.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <chrono>

namespace cutline::app {

using time::RationalTime;

namespace {

QString StateText(exporter::JobState state) {
  switch (state) {
    case exporter::JobState::Queued: return QObject::tr("Waiting");
    case exporter::JobState::Running: return QObject::tr("Exporting");
    case exporter::JobState::Done: return QObject::tr("Done");
    case exporter::JobState::Failed: return QObject::tr("Failed");
    case exporter::JobState::Cancelled: return QObject::tr("Cancelled");
    case exporter::JobState::Interrupted: return QObject::tr("Interrupted");
  }
  return {};
}

QStringList ToList(const std::vector<std::string>& lines) {
  QStringList list;
  for (const auto& line : lines) list << QString::fromStdString(line);
  return list;
}

double Seconds(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(std::max<std::int64_t>(1, t.denominator())); }

}  // namespace

QVariantList Session::exportPresets() const {
  QVariantList list;
  for (const auto& preset : exporter::PresetCatalogue::BuiltIn()) {
    QVariantMap map;
    map["id"] = QString::fromStdString(preset.id);
    map["name"] = QString::fromStdString(preset.name);
    map["description"] = QString::fromStdString(preset.description);
    map["category"] = QString::fromUtf8(exporter::ToString(preset.category));
    map["extension"] = QString::fromStdString(preset.extension);
    map["video"] = preset.has_video;
    list << map;
  }
  return list;
}

exporter::SequenceFacts Session::ExportFacts(const RationalTime& in, const RationalTime& out) const {
  exporter::SequenceFacts facts;
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return facts;
  facts.width = sequence->width;
  facts.height = sequence->height;
  facts.frame_rate = sequence->frame_rate;
  facts.pixel_aspect = sequence->pixel_aspect;
  facts.sample_rate = sequence->sample_rate;
  facts.channels = static_cast<int>(media::ChannelCountForLayout(sequence->channel_layout));
  const auto end = out.Compare(RationalTime(0, 1)) > 0 ? out : sequence->Duration();
  facts.duration = end.Compare(in) > 0 ? end.Subtract(in) : RationalTime(0, 1);
  return facts;
}

QVariantMap Session::exportPlan(const QString& preset_id, const QString& output_path, bool prefer_hardware, bool use_marks) const {
  QVariantMap map;
  const auto* preset = exporter::PresetCatalogue::Find(preset_id.toStdString());
  if (preset == nullptr || graph_.root() == nullptr) {
    map["ok"] = false;
    map["refusal"] = preset == nullptr ? tr("Choose a preset") : tr("There is no sequence to export");
    return map;
  }
  RationalTime in(0, 1), out(0, 1);
  if (use_marks) {
    if (mark_in_) in = *mark_in_;
    if (mark_out_) out = *mark_out_;
  }
  exporter::ResolveOptions options;
  options.output_path = output_path.toStdString();
  options.prefer_hardware = prefer_hardware;
  options.in = in;
  options.out = out;
  const auto resolved = exporter::ResolvePreset(*preset, ExportFacts(in, out), options);
  map["ok"] = resolved.ok;
  map["refusal"] = QString::fromStdString(resolved.refusal);
  map["encoder"] = QString::fromStdString(resolved.encoder);
  map["hardware"] = resolved.hardware;
  map["audioEncoder"] = QString::fromStdString(resolved.audio_encoder);
  map["outputPath"] = QString::fromStdString(resolved.output_path);
  map["notes"] = ToList(resolved.notes);
  map["estimatedBytes"] = static_cast<double>(resolved.estimated_bytes);
  map["duration"] = Seconds(ExportFacts(in, out).duration);
  if (resolved.ok && preset->has_video) {
    map["summary"] = tr("%1 x %2, %3 fps").arg(resolved.request.video.width).arg(resolved.request.video.height)
                         .arg(static_cast<double>(resolved.request.video.frame_rate.numerator) / static_cast<double>(resolved.request.video.frame_rate.denominator), 0, 'g', 5);
  }
  return map;
}

QString Session::exportQueueAdd(const QString& preset_id, const QString& output_path, const QString& name, bool overwrite, bool prefer_hardware, bool use_marks) {
  if (export_queue_ == nullptr || graph_.root() == nullptr) {
    ShowStatus(tr("Open a project to export it"));
    return {};
  }
  const auto plan = exportPlan(preset_id, output_path, prefer_hardware, use_marks);
  if (!plan["ok"].toBool()) {
    ShowStatus(plan["refusal"].toString());
    return {};
  }
  exporter::ExportJob job;
  job.name = name.isEmpty() ? QFileInfo(plan["outputPath"].toString()).completeBaseName().toStdString() : name.toStdString();
  job.sequence_id = sequence_id_;
  job.preset_id = preset_id.toStdString();
  job.output_path = plan["outputPath"].toString().toStdString();
  job.overwrite = overwrite;
  job.prefer_hardware = prefer_hardware;
  if (use_marks) {
    if (mark_in_) job.in = *mark_in_;
    if (mark_out_) job.out = *mark_out_;
  }
  const auto id = QString::fromStdString(export_queue_->Add(std::move(job)));
  ShowStatus(tr("Queued %1").arg(QFileInfo(plan["outputPath"].toString()).fileName()));
  return id;
}

QVariantList Session::exportJobs() const {
  QVariantList list;
  if (export_queue_ == nullptr) return list;
  for (const auto& job : export_queue_->Jobs()) {
    QVariantMap map;
    map["id"] = QString::fromStdString(job.id);
    map["name"] = QString::fromStdString(job.name);
    map["preset"] = QString::fromStdString(job.preset_id);
    if (const auto* preset = exporter::PresetCatalogue::Find(job.preset_id)) map["presetName"] = QString::fromStdString(preset->name);
    map["output"] = QString::fromStdString(job.output_path);
    map["state"] = QString::fromUtf8(exporter::ToString(job.state));
    map["stateText"] = StateText(job.state);
    map["progress"] = job.progress();
    map["framesDone"] = static_cast<double>(job.frames_done);
    map["framesTotal"] = static_cast<double>(job.frames_total);
    map["error"] = QString::fromStdString(job.error);
    map["encoder"] = QString::fromStdString(job.encoder);
    map["hardware"] = job.hardware;
    map["notes"] = ToList(job.notes);
    map["problems"] = ToList(job.problems);
    map["bytes"] = static_cast<double>(job.output_bytes);
    map["seconds"] = job.seconds;
    map["active"] = job.state == exporter::JobState::Running;
    map["waiting"] = job.state == exporter::JobState::Queued;
    map["canRetry"] = job.state == exporter::JobState::Failed || job.state == exporter::JobState::Cancelled || job.state == exporter::JobState::Interrupted;
    list << map;
  }
  return list;
}

bool Session::exportQueuePaused() const { return export_queue_ != nullptr && export_queue_->paused(); }

bool Session::exportCancel(const QString& id) { return export_queue_ != nullptr && export_queue_->Cancel(id.toStdString()); }
bool Session::exportRetry(const QString& id) { return export_queue_ != nullptr && export_queue_->Retry(id.toStdString()); }
bool Session::exportRemove(const QString& id) { return export_queue_ != nullptr && export_queue_->Remove(id.toStdString()); }
bool Session::exportMove(const QString& id, int position) { return export_queue_ != nullptr && export_queue_->Move(id.toStdString(), position); }
void Session::exportClearFinished() {
  if (export_queue_ != nullptr) export_queue_->ClearFinished();
}
void Session::exportPause(bool paused) {
  if (export_queue_ == nullptr) return;
  if (paused) export_queue_->Pause();
  else export_queue_->Resume();
}
bool Session::exportWaitIdle(int milliseconds) const { return export_queue_ == nullptr || export_queue_->WaitIdle(std::chrono::milliseconds(milliseconds)); }

QString Session::exportFolder() const {
  auto folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  if (folder.isEmpty()) folder = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
  return folder;
}

void Session::showExports() { emit workspaceCommand("panelshow.exports"); }

void Session::exportReveal(const QString& path) {
  const QFileInfo info(path);
  const auto folder = info.exists() ? info.absolutePath() : info.dir().absolutePath();
  QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void Session::StartExportQueue() {
  export_queue_.reset();
  if (store_ == nullptr) return;
  QDir().mkpath(QString::fromStdString(package_folder_) + "/exports");
  const auto file = package_folder_.empty() ? std::string() : package_folder_ + "/exports/queue.json";
  export_queue_ = std::make_unique<exporter::ExportQueue>(file, [this](const exporter::ExportJob& job, const exporter::JobProgress& progress) { return RunExportJob(job, progress); });
  export_queue_->SetListener([this] { QMetaObject::invokeMethod(this, [this] { emit exportsChanged(); }, Qt::QueuedConnection); });
  emit exportsChanged();
}

// Runs on the queue's thread. Renders the sequence as it is in the project now, on an engine of its own that decodes the
// original media in software and composes on the CPU: a delivery is made by the reference path.
exporter::JobOutcome Session::RunExportJob(const exporter::ExportJob& job, const exporter::JobProgress& progress) {
  if (store_ == nullptr) throw std::runtime_error("The project is closed");
  const auto* preset = exporter::PresetCatalogue::Find(job.preset_id);
  if (preset == nullptr) throw std::runtime_error("There is no preset " + job.preset_id);
  auto graph = timeline::LoadSequenceGraph(*store_, job.sequence_id);
  const auto* sequence = graph.root();
  if (sequence == nullptr) throw std::runtime_error("The sequence " + job.sequence_id + " is no longer in the project");

  exporter::SequenceFacts facts;
  facts.width = sequence->width;
  facts.height = sequence->height;
  facts.frame_rate = sequence->frame_rate;
  facts.pixel_aspect = sequence->pixel_aspect;
  facts.sample_rate = sequence->sample_rate;
  facts.channels = static_cast<int>(media::ChannelCountForLayout(sequence->channel_layout));
  const auto out = job.out.Compare(RationalTime(0, 1)) > 0 ? job.out : sequence->Duration();
  facts.duration = out.Subtract(job.in);
  exporter::ResolveOptions options;
  options.output_path = job.output_path;
  options.overwrite = job.overwrite;
  options.prefer_hardware = job.prefer_hardware;
  options.in = job.in;
  options.out = job.out;
  const auto resolved = exporter::ResolvePreset(*preset, facts, options);
  if (!resolved.ok) throw std::runtime_error(resolved.refusal);

  auto* store = store_.get();
  const auto lookup = [store](const std::string& id) {
    const std::lock_guard<std::mutex> lock(store->mutex());
    db::Statement statement(store->connection(), "SELECT original_path FROM media WHERE id = ?;");
    statement.Bind(1, id);
    return statement.Step() ? statement.ColumnText(0) : std::string();
  };
  playback::EngineConfig config;
  config.sample_rate = sequence->sample_rate;
  config.channels = static_cast<int>(media::ChannelCountForLayout(sequence->channel_layout));
  config.compositor.output_format = resolved.request.high_precision ? media::PixelFormat::Rgba16 : media::PixelFormat::Rgba8;
  config.decode_workers = 0;       // a delivery reads in order; read-ahead would only compete with the monitor
  config.read_ahead_frames = 0;
  config.use_gpu = false;
  playback::PlaybackEngine engine(std::move(graph), lookup, config);

  exporter::JobOutcome outcome;
  outcome.encoder = resolved.encoder;
  outcome.hardware = resolved.hardware;
  outcome.notes = resolved.notes;
  std::int64_t last_step = 0;   // steps are frame periods, for sound-only exports too
  const auto result = exporter::Export(engine, resolved.request, [&](const exporter::ExportProgress& step) {
    last_step = step.frames_written;
    return progress(step.frames_written, step.frames_total);
  });
  outcome.frames = last_step;
  if (result.cancelled) return outcome;

  exporter::ExportExpectation expected;
  expected.video = preset->has_video;
  expected.audio = preset->has_audio;
  expected.width = facts.width;
  expected.height = facts.height;
  expected.frame_rate = facts.frame_rate;
  expected.duration = result.duration;
  expected.sample_rate = resolved.request.audio.sample_rate;
  expected.channels = resolved.request.audio.channels;
  expected.video_codec = resolved.video_stream_codec;
  expected.audio_codec = resolved.audio_stream_codec;
  const auto checked = exporter::ValidateExport(resolved.output_path, expected);
  outcome.problems = checked.problems;
  outcome.output_bytes = checked.bytes;
  return outcome;
}

}  // namespace cutline::app

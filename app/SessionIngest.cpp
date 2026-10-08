// Ingest and proxies in the application: choosing files, copying them into the project exactly, importing them, and making
// the lighter pictures that play them in real time. The rules are media/IngestWorkflow.h and media/ProxyWorkflow.h; this
// runs them as background jobs the person can see and stop, and says what happened.

#include "app/Session.h"

#include "media/Ingest.h"
#include "media/IngestWorkflow.h"
#include "media/ProxyWorkflow.h"
#include "media/Source.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>

namespace cutline::app {

namespace fs = std::filesystem;

namespace {

fs::path LocalFile(const QString& url_or_path) {
  const QUrl url(url_or_path);
  return fs::path((url.isLocalFile() ? url.toLocalFile() : url_or_path).toStdWString());
}

}  // namespace

void Session::setUseProxies(bool on) {
  if (useProxies() == on) return;
  prefs_.Set("media.use_proxies", on);
  if (engine_ != nullptr) engine_->SetPreferProxies(on);
  emit mediaChanged();
  RequestFrame();
}

QString Session::ingestFolder() const {
  return package_folder_.empty() ? QString() : QString::fromStdString(package_folder_) + "/media";
}

QVariantList Session::proxyChoices() const {
  return QVariantList{QVariantMap{{"id", "auto"}, {"label", tr("Automatic: only for media that is heavy to play")}},
                      QVariantMap{{"id", "1080"}, {"label", tr("Full HD proxy (1920 x 1080)")}},
                      QVariantMap{{"id", "720"}, {"label", tr("HD proxy (1280 x 720)")}},
                      QVariantMap{{"id", "540"}, {"label", tr("Light proxy (960 x 540)")}},
                      QVariantMap{{"id", "none"}, {"label", tr("No proxy")}}};
}

QVariantMap Session::ingestPlan(const QStringList& urls, bool copy, const QString& destination) const {
  std::vector<fs::path> sources;
  for (const auto& url : urls) sources.push_back(LocalFile(url));
  media::IngestPlanOptions options;
  options.copy = copy;
  options.destination_folder = destination.isEmpty() ? fs::path(ingestFolder().toStdWString()) : LocalFile(destination);
  const auto plan = media::PlanIngest(sources, options);
  QVariantList items;
  for (const auto& item : plan.items) {
    items << QVariantMap{{"name", QString::fromStdString(item.source.filename().string())},
                         {"bytes", static_cast<double>(item.bytes)},
                         {"destination", QString::fromStdString(item.destination.string())},
                         {"copy", item.copy},
                         {"problem", QString::fromStdString(item.problem)}};
  }
  return QVariantMap{{"ok", plan.ok()}, {"problem", QString::fromStdString(plan.problem)}, {"bytes", static_cast<double>(plan.bytes_to_copy)},
                     {"freeBytes", static_cast<double>(plan.free_bytes)}, {"items", items}};
}

void Session::ingestFiles(const QStringList& urls, bool copy, bool verify, const QString& destination, const QString& proxy) {
  if (store_ == nullptr) {
    ShowStatus(tr("Open or create a project first"));
    return;
  }
  const auto plan = ingestPlan(urls, copy, destination);
  if (!plan["ok"].toBool()) {
    ShowStatus(plan["problem"].toString());
    return;
  }
  std::vector<fs::path> sources;
  for (const auto& url : urls) sources.push_back(LocalFile(url));
  media::IngestPlanOptions options;
  options.copy = copy;
  options.destination_folder = destination.isEmpty() ? fs::path(ingestFolder().toStdWString()) : LocalFile(destination);
  const auto real_plan = media::PlanIngest(sources, options);
  const auto choice = proxy.toStdString();
  for (const auto& item : real_plan.items) {
    if (!item.problem.empty()) {
      ShowStatus(QString::fromStdString(item.source.filename().string() + ": " + item.problem));
      continue;
    }
    const auto title = std::string(item.copy ? "Copying and importing " : "Importing ") + item.source.filename().string();
    runner_->Run("ingest", title, [this, item, verify, choice](ui::JobContext& context) {
      std::unique_lock<std::timed_mutex> serial(ingest_mutex_, std::defer_lock);
      while (!serial.try_lock_for(std::chrono::milliseconds(100))) {
        if (context.cancelled()) return;
      }
      if (context.cancelled()) return;
      std::string path = item.source.string();
      if (item.copy) {
        media::CopyOptions copy_options;
        copy_options.verify = verify;
        copy_options.cancelled = context.cancel_token();
        copy_options.progress = [&](std::uint64_t done, std::uint64_t total) { context.Progress(done, std::max<std::uint64_t>(total, 1), "Copying"); };
        const auto copied = media::CopyVerified(item.source, item.destination, copy_options);
        if (copied.cancelled) return;
        if (!copied.ok) throw std::runtime_error(copied.error);
        path = item.destination.string();
      }
      media::IngestRequest request;
      request.project_id = project_id_;
      request.author_id = "user";
      request.timestamp_utc = "2026-01-01T00:00:00Z";
      request.media_id = NewId("media");
      request.path = path;
      request.base_revision = store_->CurrentRevision();
      context.Progress(0, 0, "Reading the file");
      const auto result = media::IngestFile(*store_, request);
      QMetaObject::invokeMethod(this, [this] { RefreshMedia(); }, Qt::QueuedConnection);
      if (result.already_present) {
        // The project already has this media: the new copy would only be clutter.
        if (item.copy) {
          std::error_code ignored;
          fs::remove(item.destination, ignored);
        }
        return;
      }
      if (!choice.empty() && choice != "none" && !context.cancelled()) MakeProxy(context, result.media_id, choice);
    });
  }
}

// Runs on a job's thread.
void Session::MakeProxy(ui::JobContext& context, const std::string& media_id, const std::string& choice) {
  const auto facts = MediaFactsOf(media_id);
  if (facts.path.empty()) throw std::runtime_error("The media is offline");
  const auto probe = media::SourceRegistry::Instance().ProbeFile(facts.path);
  const auto* video = probe.PrimaryVideo();
  if (video == nullptr) return;   // sound only: nothing to make a picture of
  media::ProxyComplexity complexity;
  complexity.width = video->width;
  complexity.height = video->height;
  complexity.frames_per_second = video->frame_rate.denominator > 0 ? static_cast<double>(video->frame_rate.numerator) / static_cast<double>(video->frame_rate.denominator) : 0.0;
  complexity.bit_depth = static_cast<int>(video->bit_depth);
  complexity.codec = video->codec;
  const auto preset = media::ProxyPresetFor(choice, complexity);
  if (!preset) {
    QMetaObject::invokeMethod(this, [this] { ShowStatus(tr("This media plays well as it is: no proxy was made")); }, Qt::QueuedConnection);
    return;
  }
  const fs::path folder = package_folder_.empty() ? fs::temp_directory_path() / "cutline-proxies" : fs::path(package_folder_) / "proxies";
  std::error_code ignored;
  fs::create_directories(folder, ignored);
  media::ProxyGenerationRequest request;
  request.source_path = facts.path;
  request.source_fingerprint = facts.fingerprint;
  request.output_path = media::ProxyPathFor(folder, media_id).string();
  request.preset = *preset;
  const auto set_progress = [this, media_id](double value, bool done) {
    QMetaObject::invokeMethod(this, [this, media_id, value, done] {
      if (done) proxy_progress_.erase(media_id);
      else proxy_progress_[media_id] = value;
      RefreshMedia();
    }, Qt::QueuedConnection);
  };
  set_progress(0.0, false);
  double last_reported = 0.0;
  media::ProxyGenerationResult generated;
  try {
    generated = media::GenerateProxy(request, [&](const media::ProxyGenerationProgress& step) {
      const double fraction = step.frames_total > 0 ? static_cast<double>(step.frames_complete) / static_cast<double>(step.frames_total) : 0.0;
      context.Progress(static_cast<std::uint64_t>(std::max<std::int64_t>(step.frames_complete, 0)), static_cast<std::uint64_t>(std::max<std::int64_t>(step.frames_total, 1)), "Making the proxy");
      if (fraction - last_reported >= 0.05) {
        last_reported = fraction;
        set_progress(fraction, false);
      }
    }, context.cancel_token());
  } catch (...) {
    set_progress(0.0, true);
    fs::remove(request.output_path, ignored);
    throw;
  }
  if (generated.cancelled) {
    set_progress(0.0, true);
    fs::remove(request.output_path, ignored);
    return;
  }
  media::AttachGeneratedProxyRequest attach;
  attach.command_id = "cmd-" + NewId("proxy");
  attach.project_id = project_id_;
  attach.author_id = "user";
  attach.timestamp_utc = "2026-01-01T00:00:00Z";
  attach.media_id = media_id;
  attach.base_revision = store_->CurrentRevision();
  (void)media::AttachGeneratedProxy(*store_, attach, generated);
  set_progress(1.0, true);
  QMetaObject::invokeMethod(this, [this] { RequestFrame(); }, Qt::QueuedConnection);
}

void Session::proxyCreate(const QString& media_id, const QString& choice) {
  if (store_ == nullptr) return;
  const auto id = media_id.toStdString();
  if (proxy_progress_.count(id) != 0) return;
  const auto facts = MediaFactsOf(id);
  if (facts.path.empty()) {
    ShowStatus(tr("That media is offline"));
    return;
  }
  const auto pick = choice.isEmpty() ? std::string("720") : choice.toStdString();
  runner_->Run("proxy", "Making a proxy of " + facts.name, [this, id, pick](ui::JobContext& context) { MakeProxy(context, id, pick); });
}

bool Session::proxyRemove(const QString& media_id) {
  if (store_ == nullptr) return false;
  const auto record = media::FindProxy(*store_, media_id.toStdString());
  if (record.status == media::ProxyStatus::None) return false;
  Run(commands::CommandType::DetachProxy, commands::DetachProxyPayload{media_id.toStdString()});
  // The file goes too, but only if it is one this project made.
  std::error_code ignored;
  const fs::path proxies = fs::path(package_folder_) / "proxies";
  if (!package_folder_.empty() && fs::path(record.path).parent_path() == proxies) fs::remove(record.path, ignored);
  RefreshMedia();
  if (engine_ != nullptr) RequestFrame();
  return true;
}

}  // namespace cutline::app

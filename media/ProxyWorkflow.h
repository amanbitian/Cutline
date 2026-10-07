#pragma once

// Local proxy generation and project association.
//
// Proxies never replace originals. The project stores the fingerprint of the
// original used to make each proxy, and playback resolves a proxy only while
// that fingerprint still matches. Generation uses the in-process media source
// and writer registries, so no service, upload, or internet connection exists
// in this path.

#include "core/project/ProjectStore.h"
#include "core/resource/QuotaManager.h"
#include "core/time/RationalTime.h"
#include "playback/PlaybackEngine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace cutline::media {

enum class ProxyStatus { None, Ready, Missing, Stale };

struct ProxyRecord final {
  std::string media_id;
  std::string path;
  std::string fingerprint;
  std::string source_fingerprint;
  std::string codec;
  std::int64_t width{};
  std::int64_t height{};
  ProxyStatus status{ProxyStatus::None};
};

[[nodiscard]] ProxyRecord FindProxy(const project::ProjectStore& store, const std::string& media_id);

// Suitable for EngineConfig::proxy_locator. Only Ready proxies resolve; stale
// and missing records return an empty path, causing playback to use originals.
[[nodiscard]] playback::MediaLocator ProjectProxyLocator(const project::ProjectStore& store);

struct ProxyPreset final {
  std::int64_t max_width{1280};
  std::int64_t max_height{720};
  std::string video_codec{"prores_ks"};
  std::string pixel_format{"yuv422p10le"};
  std::string audio_codec{"pcm_s16le"};
  std::int64_t video_bitrate{45'000'000};
};

struct ProxyComplexity final {
  std::int64_t width{};
  std::int64_t height{};
  double frames_per_second{30.0};
  int bit_depth{8};
  std::string codec;
  bool intra_only{false};
};

struct PlaybackHealth final {
  std::uint64_t requested_frames{};
  std::uint64_t deadline_misses{};
  double average_decode_ms{};
  double frame_budget_ms{33.333};
  resource::Pressure memory_pressure{resource::Pressure::Normal};
};

struct ProxyDecision final {
  bool generate{false};
  ProxyPreset preset;
  double complexity_score{};
  double miss_rate{};
  std::string reason;
};

// Deterministic offline policy used by ingest and playback telemetry. It never
// contacts a service and always chooses codecs shipped with the application.
[[nodiscard]] ProxyDecision ChooseProxyPreset(const ProxyComplexity& media,
                                              const PlaybackHealth& playback = {});

struct ProxyGenerationRequest final {
  std::string source_path;
  std::string source_fingerprint;
  std::string output_path;
  ProxyPreset preset;
};

struct ProxyGenerationProgress final {
  std::int64_t frames_complete{};
  std::int64_t frames_total{};
};

using ProxyProgress = std::function<void(const ProxyGenerationProgress&)>;
using ProxyCancel = std::function<bool()>;

struct ProxyGenerationResult final {
  std::string path;
  std::string fingerprint;
  std::string source_fingerprint;
  std::string codec;
  std::int64_t width{};
  std::int64_t height{};
  std::int64_t video_frames{};
  bool cancelled{false};
};

[[nodiscard]] ProxyGenerationResult GenerateProxy(const ProxyGenerationRequest& request,
                                                   const ProxyProgress& progress = {},
                                                   const ProxyCancel& cancel = {});

// A small ownership wrapper for running GenerateProxy away from the UI thread.
// Destruction cancels and joins the worker, so it cannot outlive its callbacks.
class ProxyJob final {
 public:
  explicit ProxyJob(ProxyGenerationRequest request, ProxyProgress progress = {});
  ~ProxyJob();
  ProxyJob(const ProxyJob&) = delete;
  ProxyJob& operator=(const ProxyJob&) = delete;
  ProxyJob(ProxyJob&&) noexcept;
  ProxyJob& operator=(ProxyJob&&) noexcept;

  void Cancel();
  [[nodiscard]] bool Ready() const;
  [[nodiscard]] ProxyGenerationResult Get();

 private:
  std::shared_ptr<std::atomic_bool> cancel_;
  std::future<ProxyGenerationResult> future_;
};

struct AttachGeneratedProxyRequest final {
  std::string command_id;
  std::string project_id;
  std::string author_id;
  std::string timestamp_utc;
  std::string media_id;
  std::int64_t base_revision{};
};

// Journals the association after generation succeeds. Regeneration uses the
// same command and atomically replaces the prior association.
[[nodiscard]] project::CommandResult AttachGeneratedProxy(project::ProjectStore& store,
                                                           const AttachGeneratedProxyRequest& request,
                                                           const ProxyGenerationResult& generated);

}  // namespace cutline::media

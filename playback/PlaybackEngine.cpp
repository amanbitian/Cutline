#include "playback/PlaybackEngine.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cutline::playback {
namespace {

// Raw decoded pictures are independent of the clip that asks for them. Keying
// by media lets duplicate clips and nested uses share one decode.
[[nodiscard]] std::string CacheKey(const timeline::SourceRequest& request) {
  return request.source_id + "@" + std::to_string(request.source_time.ToTicks());
}

}  // namespace

PlaybackEngine::PlaybackEngine(timeline::SequenceGraph graph, MediaLocator locator, EngineConfig config)
    : locator_(std::move(locator)),
      config_(config),
      compositor_(config.compositor),
      stretch_cache_(std::make_shared<audio::StretchCache>()),
      mixer_(audio::MixerConfig{config.sample_rate, config.channels, true, stretch_cache_}) {
  prefer_proxies_.store(config.prefer_proxies, std::memory_order_relaxed);
  if (!locator_) throw std::invalid_argument("A playback engine needs a media locator");
  if (graph.root() == nullptr) throw std::invalid_argument("A playback engine needs a sequence");
  if (config_.frame_cache_bytes == 0) throw std::invalid_argument("The frame cache needs a positive byte budget");
  if (config_.decode_workers != 0 && config_.read_ahead_frames != 0 && config_.max_pending_decodes == 0) {
    throw std::invalid_argument("The read-ahead queue needs a positive capacity");
  }
  if (config_.decode_workers != 0 && config_.read_ahead_frames != 0 && config_.worker_source_limit == 0) {
    throw std::invalid_argument("Each read-ahead worker needs a positive source limit");
  }
  graph_ = std::make_shared<const timeline::SequenceGraph>(std::move(graph));
  if (config_.use_gpu) {
    render::gpu::D3D11Compositor::Options options;
    options.adapter_id = config_.gpu_adapter_id;
    options.allow_software = config_.gpu_allow_software;
    gpu_ = render::gpu::D3D11Compositor::Create(options, &gpu_unavailable_reason_);
  }
  if (config_.decode_workers != 0 && config_.read_ahead_frames != 0) {
    decode_pool_ = std::make_unique<DecodePool>(
        config_.decode_workers, config_.max_pending_decodes, config_.worker_source_limit,
        read_ahead_generation_.load(),
        [this](DecodeJob job, std::optional<media::VideoFrame> frame) {
          if (!frame.has_value() || job.generation != read_ahead_generation_.load(std::memory_order_acquire)) return;
          const std::lock_guard<std::mutex> picture(video_mutex_);
          if (job.generation != read_ahead_generation_.load(std::memory_order_relaxed) ||
              cache_index_.count(job.cache_key) != 0) {
            return;
          }
          (void)InsertCachedFrame(std::move(job.cache_key), std::move(*frame), true);
          counters_.read_ahead_completed.fetch_add(1, std::memory_order_relaxed);
        });
    prime_worker_ = std::thread([this] { RunReadAheadPrimer(); });
  }
}

PlaybackEngine::~PlaybackEngine() {
  Pause();
  {
    const std::lock_guard<std::mutex> lock(prime_mutex_);
    prime_stop_ = true;
    prime_pending_.reset();
  }
  prime_wake_.notify_all();
  if (prime_worker_.joinable()) prime_worker_.join();
  if (decode_pool_ != nullptr) decode_pool_->Stop();
}

PlaybackEngine::Graph PlaybackEngine::Snapshot() const {
  const std::lock_guard<std::mutex> lock(graph_mutex_);
  return graph_;
}

std::string PlaybackEngine::ResolveMedia(const std::string& media_id) const {
  if (prefer_proxies_.load(std::memory_order_relaxed) && config_.proxy_locator) {
    try {
      auto proxy = config_.proxy_locator(media_id);
      if (!proxy.empty()) return proxy;
    } catch (const std::exception&) {
      // Proxy media is disposable. A database/read error must not hide a valid
      // original from the editor.
    }
  }
  return locator_(media_id);
}

std::shared_ptr<const timeline::SequenceGraph> PlaybackEngine::graph() const { return Snapshot(); }

std::string PlaybackEngine::gpu_device_name() const {
  const std::lock_guard<std::mutex> picture(video_mutex_);
  return gpu_ != nullptr ? gpu_->device().name : std::string();
}

std::string PlaybackEngine::gpu_unavailable_reason() const {
  const std::lock_guard<std::mutex> picture(video_mutex_);
  return gpu_ != nullptr || !config_.use_gpu ? std::string() : gpu_unavailable_reason_;
}

std::unique_ptr<PlaybackEngine> PlaybackEngine::OriginalQualityClone() const { return ExportClone(config_.compositor.output_format); }

std::unique_ptr<PlaybackEngine> PlaybackEngine::ExportClone(media::PixelFormat output_format) const {
  EngineConfig config;
  {
    const std::lock_guard<std::mutex> picture(video_mutex_);
    config = config_;
    // GPU rounding may differ from the reference output. Never reuse preview
    // pictures for a reference export, including after a device failure.
    if (config_.use_gpu) config.render_cache.reset();
  }
  config.compositor.output_format = output_format;
  config.compositor.width = 0;
  config.compositor.height = 0;
  config.prefer_proxies = false;
  config.proxy_locator = {};
  // A delivery is rendered by the reference compositor, picture for picture the same on any machine.
  config.use_gpu = false;
  // Export reads sequentially and should not compete with monitor read-ahead.
  config.decode_workers = 0;
  config.read_ahead_frames = 0;
  return std::make_unique<PlaybackEngine>(*Snapshot(), locator_, std::move(config));
}

EngineStatistics PlaybackEngine::statistics() const {
  EngineStatistics snapshot;
  snapshot.frames_rendered = counters_.frames_rendered.load(std::memory_order_relaxed);
  snapshot.render_cache_hits = counters_.render_cache_hits.load(std::memory_order_relaxed);
  snapshot.render_cache_stores = counters_.render_cache_stores.load(std::memory_order_relaxed);
  snapshot.audio_blocks_mixed = counters_.audio_blocks_mixed.load(std::memory_order_relaxed);
  snapshot.cache_hits = counters_.cache_hits.load(std::memory_order_relaxed);
  snapshot.cache_misses = counters_.cache_misses.load(std::memory_order_relaxed);
  snapshot.cache_evictions = counters_.cache_evictions.load(std::memory_order_relaxed);
  snapshot.decode_failures = counters_.decode_failures.load(std::memory_order_relaxed);
  snapshot.offline_media = counters_.offline_media.load(std::memory_order_relaxed);
  snapshot.read_ahead_queued = counters_.read_ahead_queued.load(std::memory_order_relaxed);
  snapshot.read_ahead_completed = counters_.read_ahead_completed.load(std::memory_order_relaxed);
  snapshot.read_ahead_cache_hits = counters_.read_ahead_cache_hits.load(std::memory_order_relaxed);
  snapshot.read_ahead_queue_full = counters_.read_ahead_queue_full.load(std::memory_order_relaxed);
  snapshot.total_frame_render_microseconds =
      counters_.total_frame_render_microseconds.load(std::memory_order_relaxed);
  snapshot.max_frame_render_microseconds = counters_.max_frame_render_microseconds.load(std::memory_order_relaxed);
  snapshot.audio_blocks_clipped = counters_.audio_blocks_clipped.load(std::memory_order_relaxed);
  snapshot.audio_missing_sources = counters_.audio_missing_sources.load(std::memory_order_relaxed);
  snapshot.audio_blocks_dropped = counters_.audio_blocks_dropped.load(std::memory_order_relaxed);
  snapshot.gpu_frames = counters_.gpu_frames.load(std::memory_order_relaxed);
  snapshot.gpu_fallback_frames = counters_.gpu_fallback_frames.load(std::memory_order_relaxed);
  snapshot.gpu_failures = counters_.gpu_failures.load(std::memory_order_relaxed);
  snapshot.gpu_microseconds = counters_.gpu_microseconds.load(std::memory_order_relaxed);
  snapshot.gpu_device_microseconds = counters_.gpu_device_microseconds.load(std::memory_order_relaxed);
  snapshot.hardware_pictures = counters_.hardware_pictures.load(std::memory_order_relaxed);
  snapshot.uploaded_pictures = counters_.uploaded_pictures.load(std::memory_order_relaxed);
  return snapshot;
}

std::shared_ptr<media::Source> PlaybackEngine::SourceFor(const std::string& media_id) {
  {
    const std::lock_guard<std::mutex> lock(sources_mutex_);
    if (const auto found = sources_.find(media_id); found != sources_.end()) return found->second.source;
    if (offline_.count(media_id) != 0) return nullptr;
  }

  // Locating and opening can be slow (a database lookup, a file open) and are done
  // with no lock held, so a slow open of one item never stalls the other thread's
  // reads of another.
  const auto path = ResolveMedia(media_id);
  const auto mark_offline = [&](bool failed_to_decode) {
    const std::lock_guard<std::mutex> lock(sources_mutex_);
    if (offline_.emplace(media_id, true).second) {
      if (failed_to_decode) {
        counters_.decode_failures.fetch_add(1, std::memory_order_relaxed);
      } else {
        counters_.offline_media.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  if (path.empty()) {
    mark_offline(false);
    return nullptr;
  }
  std::shared_ptr<media::Source> opened;
  try {
    opened = media::SourceRegistry::Instance().Open(path);
  } catch (const std::exception&) {
    // A file that exists but cannot be decoded is offline for our purposes. It
    // is remembered so the failure costs one attempt, not one per frame.
    mark_offline(true);
    return nullptr;
  }
  if (opened == nullptr) {
    mark_offline(false);
    return nullptr;
  }
  const std::lock_guard<std::mutex> lock(sources_mutex_);
  // The other thread may have opened the same media while this one was; keep one.
  const auto inserted = sources_.emplace(media_id, OpenSource{path, std::move(opened)});
  return inserted.first->second.source;
}

std::shared_ptr<media::Source> PlaybackEngine::DeviceSourceFor(const std::string& media_id) {
  if (const auto found = device_sources_.find(media_id); found != device_sources_.end()) return found->second;
  std::shared_ptr<media::Source> opened;
  try {
    const auto path = ResolveMedia(media_id);
    if (!path.empty()) {
      media::OpenOptions options;
      options.d3d11_device = gpu_->native_device();
      opened = media::SourceRegistry::Instance().Open(path, options);
      if (opened != nullptr && !opened->device_decode_available()) opened.reset();
    }
  } catch (const std::exception&) {
    opened.reset();
  }
  device_sources_.emplace(media_id, opened);
  return opened;
}

media::DeviceFrame PlaybackEngine::DeviceFrameFor(const timeline::SourceRequest& request) {
  if (gpu_ == nullptr || !config_.hardware_decode || request.source_kind != model::SourceKind::Media) return {};
  const auto source = DeviceSourceFor(request.source_id);
  if (source == nullptr) return {};
  try {
    auto frame = source->ReadDeviceVideo(request.source_time);
    // A source can open a hardware decoder and reject the stream only when the first picture is requested. Remember
    // that permanent failure as a null device source so software read-ahead resumes instead of forcing every later
    // frame through synchronous CPU decode.
    if (!frame.has_value() && !source->device_decode_available()) device_sources_[request.source_id].reset();
    return frame.has_value() ? std::move(*frame) : media::DeviceFrame{};
  } catch (const std::exception&) {
    device_sources_[request.source_id].reset();
    return {};
  }
}

const media::VideoFrame* PlaybackEngine::FrameFor(const timeline::SourceRequest& request) {
  if (request.source_kind == model::SourceKind::Adjustment) return nullptr;

  if (request.source_kind == model::SourceKind::Sequence) {
    // Nested sequences are composed recursively and handed back as if they were
    // decoded media, which is what keeps the compositor unaware of nesting.
    const auto found = nested_frames_.find(request.clip_id);
    return found == nested_frames_.end() ? nullptr : &found->second;
  }

  const auto key = CacheKey(request);
  if (const auto found = cache_index_.find(key); found != cache_index_.end()) {
    // Move to the front: this is the least-recently-used eviction order.
    cache_.splice(cache_.begin(), cache_, found->second);
    counters_.cache_hits.fetch_add(1, std::memory_order_relaxed);
    if (cache_.front().read_ahead) counters_.read_ahead_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return &cache_.front().frame;
  }

  const auto source = SourceFor(request.source_id);
  if (source == nullptr) return nullptr;

  std::optional<media::VideoFrame> decoded;
  try {
    decoded = source->ReadVideo(request.source_time);
  } catch (const std::exception&) {
    counters_.decode_failures.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  if (!decoded.has_value()) return nullptr;

  counters_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return InsertCachedFrame(key, std::move(*decoded), false);
}

const media::VideoFrame* PlaybackEngine::InsertCachedFrame(std::string key, media::VideoFrame frame,
                                                           bool read_ahead) {
  if (const auto found = cache_index_.find(key); found != cache_index_.end()) return &found->second->frame;
  const auto bytes = frame.size_bytes();
  cache_.push_front({std::move(key), std::move(frame), read_ahead});
  cache_index_.emplace(cache_.front().key, cache_.begin());
  cache_bytes_ += bytes;
  // Always keep the frame just decoded, even when it alone exceeds the budget:
  // returning a pointer into something immediately evicted would dangle.
  while (cache_.size() > 1 && cache_bytes_ > config_.frame_cache_bytes) {
    cache_bytes_ -= cache_.back().frame.size_bytes();
    cache_index_.erase(cache_.back().key);
    cache_.pop_back();
    counters_.cache_evictions.fetch_add(1, std::memory_order_relaxed);
  }
  return &cache_.front().frame;
}

void PlaybackEngine::ScheduleReadAhead(const Graph& graph, const time::RationalTime& at,
                                       const timeline::CompileOptions& options, bool include_current,
                                       std::uint64_t generation) {
  if (decode_pool_ == nullptr || graph == nullptr || graph->root() == nullptr) return;
  const auto* root = graph->root();
  if (generation == 0) generation = read_ahead_generation_.load(std::memory_order_acquire);
  if (generation != read_ahead_generation_.load(std::memory_order_acquire)) return;
  auto first_frame = at.ToFrames(root->frame_rate, time::RoundingMode::Floor);
  if (!include_current) ++first_frame;
  if (first_frame < 0) first_frame = 0;

  std::unordered_set<std::string> offered;
  std::unordered_map<std::string, std::string> paths;
  for (std::size_t offset = 0; offset < config_.read_ahead_frames; ++offset) {
    if (generation != read_ahead_generation_.load(std::memory_order_acquire)) return;
    const auto timeline_time = time::RationalTime::FromFrames(first_frame + static_cast<std::int64_t>(offset),
                                                              root->frame_rate);
    if (timeline_time.Compare(root->Duration()) >= 0) break;
    auto plan = compiler_.Compile(*graph, timeline_time, options);
    for (const auto& request : plan.video) {
      if (generation != read_ahead_generation_.load(std::memory_order_acquire)) return;
      if (request.source_kind != model::SourceKind::Media) continue;
      auto key = CacheKey(request);
      if (!offered.insert(key).second) continue;
      bool decoded_on_device = false;
      {
        const std::lock_guard<std::mutex> picture(video_mutex_);
        if (cache_index_.count(key) != 0) continue;
        // A hardware decoder already supplies this media directly to the compositor. A parallel software decode cannot
        // satisfy that path and only competes for CPU, storage bandwidth, and RAM.
        if (gpu_ != nullptr && config_.hardware_decode) {
          const auto device = device_sources_.find(request.source_id);
          decoded_on_device = device != device_sources_.end() && device->second != nullptr;
        }
      }
      if (decoded_on_device) continue;

      auto path = paths.find(request.source_id);
      if (path == paths.end()) {
        std::string resolved;
        try {
          resolved = ResolveMedia(request.source_id);
        } catch (const std::exception&) {
          continue;
        }
        path = paths.emplace(request.source_id, std::move(resolved)).first;
      }
      if (path->second.empty()) continue;

      const auto result = decode_pool_->Enqueue(
          DecodeJob{std::move(key), request.source_id, path->second, request.source_time, generation});
      if (result == EnqueueResult::Queued) {
        counters_.read_ahead_queued.fetch_add(1, std::memory_order_relaxed);
      } else if (result == EnqueueResult::Full) {
        counters_.read_ahead_queue_full.fetch_add(1, std::memory_order_relaxed);
        return;  // The nearest requests are already queued; do not churn on the rest.
      }
    }
  }
}

std::uint64_t PlaybackEngine::AdvanceReadAheadGeneration() {
  const auto generation = read_ahead_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (decode_pool_ != nullptr) decode_pool_->AdvanceGeneration(generation);
  return generation;
}

void PlaybackEngine::PrimeReadAhead(const time::RationalTime& at) {
  // A direct prime represents the newest scrub position. Discard queued work
  // from the prior position before offering this one at the front of an empty
  // FIFO; ordinary forward playback uses ScheduleReadAhead without advancing.
  const auto generation = AdvanceReadAheadGeneration();
  if (decode_pool_ == nullptr) return;
  auto graph = Snapshot();
  {
    const std::lock_guard<std::mutex> lock(prime_mutex_);
    prime_pending_ = PrimeRequest{std::move(graph), at, config_.compile, generation};
  }
  prime_wake_.notify_one();
}

void PlaybackEngine::RunReadAheadPrimer() {
  for (;;) {
    PrimeRequest request;
    {
      std::unique_lock<std::mutex> lock(prime_mutex_);
      prime_wake_.wait(lock, [this] { return prime_stop_ || prime_pending_.has_value(); });
      if (prime_stop_) return;
      request = std::move(*prime_pending_);
      prime_pending_.reset();
    }
    ScheduleReadAhead(request.graph, request.at, request.options, true, request.generation);
  }
}

render::SourceVersions PlaybackEngine::Versions() const {
  // A file's identity is its location, size and modification time: enough to tell that a picture
  // made from it is out of date, without reading it.
  const auto stamp = [](const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return path;  // a synthetic: URI, or something not on disk: its name is its version
    const auto written = std::filesystem::last_write_time(path, error).time_since_epoch().count();
    return path + "|" + std::to_string(size) + "|" + std::to_string(written);
  };
  render::SourceVersions versions;
  versions.media = [this, stamp](const std::string& media_id) { return stamp(ResolveMedia(media_id)); };
  versions.asset = [this, stamp](const std::string& reference) {
    std::filesystem::path path(reference);
    if (path.is_relative() && !config_.compositor.asset_root.empty()) path = std::filesystem::path(config_.compositor.asset_root) / path;
    return stamp(path.string());
  };
  return versions;
}

media::VideoFrame PlaybackEngine::ComposeSequence(const timeline::SequenceGraph& graph,
                                                  const timeline::Sequence& sequence, const time::RationalTime& at,
                                                  int depth, const timeline::CompileOptions& options) {
  const auto plan = compiler_.Compile(sequence, at, options);

  // The address of this picture, if there is a cache to look it up in.
  render::RenderKey key;
  const auto versions = Versions();
  if (config_.render_cache != nullptr) {
    std::vector<render::RenderKey> nested_keys;
    if (depth < options.max_nesting_depth) {
      for (const auto& request : plan.video) {
        if (request.depth != 0 || request.source_kind != model::SourceKind::Sequence) continue;
        const auto* nested = graph.Find(request.source_id);
        nested_keys.push_back(nested == nullptr ? render::RenderKey("missing")
                                                : render::KeyForTime(compiler_, graph, *nested, request.source_time, options,
                                                                     config_.compositor, versions, depth + 1));
      }
    }
    key = render::KeyForPlan(plan, config_.compositor, versions, nested_keys);
    if (const auto cached = config_.render_cache->Find(key)) {
      counters_.render_cache_hits.fetch_add(1, std::memory_order_relaxed);
      auto frame = cached->Clone();
      frame.presentation_time = plan.sequence_time;  // the same picture, at this time
      return frame;
    }
  }

  // Compose any nested sequences first, so their results are available when the
  // compositor asks for the clip that wraps them.
  if (depth < options.max_nesting_depth) {
    for (const auto& request : plan.video) {
      if (request.depth != 0 || request.source_kind != model::SourceKind::Sequence) continue;
      const auto* nested = graph.Find(request.source_id);
      if (nested == nullptr) continue;
      auto composed = ComposeSequence(graph, *nested, request.source_time, depth + 1, options);
      nested_frames_.insert_or_assign(request.clip_id, std::move(composed));
    }
  }

  render::Statistics composed;
  const render::FrameResolver resolve = [this](const timeline::SourceRequest& request) { return FrameFor(request); };
  media::VideoFrame frame;
  bool composed_on_gpu = false;
  if (gpu_ != nullptr && gpu_->Supports(plan, config_.compositor)) {
    try {
      render::gpu::GpuStatistics device_stats;
      const render::gpu::DeviceFrameResolver on_device = [this](const timeline::SourceRequest& request) { return DeviceFrameFor(request); };
      frame = gpu_->Compose(plan, config_.compositor, resolve, composed, &device_stats, on_device);
      composed_on_gpu = true;
      counters_.gpu_frames.fetch_add(1, std::memory_order_relaxed);
      counters_.gpu_microseconds.fetch_add(static_cast<std::int64_t>(device_stats.total_ms * 1000.0), std::memory_order_relaxed);
      counters_.gpu_device_microseconds.fetch_add(static_cast<std::int64_t>(device_stats.gpu_ms * 1000.0), std::memory_order_relaxed);
      counters_.hardware_pictures.fetch_add(device_stats.sources_on_device, std::memory_order_relaxed);
      counters_.uploaded_pictures.fetch_add(device_stats.sources_uploaded, std::memory_order_relaxed);
    } catch (const render::gpu::Unsupported&) {
      // Something that turned up only with the pictures (a colour conversion): the software compositor does this frame.
      counters_.gpu_fallback_frames.fetch_add(1, std::memory_order_relaxed);
      composed = {};
    } catch (const std::exception& error) {
      // The device failed (removed, reset, out of memory). Stay in software from here on rather than fail every frame.
      counters_.gpu_failures.fetch_add(1, std::memory_order_relaxed);
      gpu_unavailable_reason_ = std::string("the GPU failed: ") + error.what();
      device_sources_.clear();
      gpu_.reset();
      composed = {};
    }
  } else if (gpu_ != nullptr) {
    counters_.gpu_fallback_frames.fetch_add(1, std::memory_order_relaxed);
  }
  if (!composed_on_gpu) frame = compositor_.Compose(plan, resolve, composed);
  // Only a complete picture is worth keeping: one with a source still decoding has a hole that the
  // next render would fill.
  if (config_.render_cache != nullptr && composed.missing_frames == 0) {
    config_.render_cache->Store(key, frame);
    counters_.render_cache_stores.fetch_add(1, std::memory_order_relaxed);
  }
  return frame;
}

PlaybackEngine::PreRenderResult PlaybackEngine::PreRender(const time::RationalTime& in, const time::RationalTime& out,
                                                          const std::function<bool()>& cancel,
                                                          const std::function<void(std::int64_t, std::int64_t)>& progress) {
  PreRenderResult result;
  const auto graph = Snapshot();
  const auto* root = graph->root();
  if (root == nullptr) throw std::logic_error("Playback engine has no sequence");
  const auto first = in.ToFrames(root->frame_rate, time::RoundingMode::Nearest);
  const auto last = out.ToFrames(root->frame_rate, time::RoundingMode::Nearest);
  const auto versions = Versions();
  for (std::int64_t frame = first; frame < last; ++frame) {
    if (cancel && cancel()) {
      result.cancelled = true;
      break;
    }
    const auto at = time::RationalTime::FromFrames(frame, root->frame_rate);
    const bool held = config_.render_cache != nullptr &&
                      config_.render_cache->Contains(render::KeyForTime(compiler_, *graph, *root, at, config_.compile, config_.compositor, versions));
    if (held) {
      ++result.already_cached;
    } else {
      (void)RenderFrame(at);
      ++result.rendered;
    }
    if (progress) progress(frame - first + 1, last - first);
  }
  return result;
}

std::vector<bool> PlaybackEngine::CachedFrames(const time::RationalTime& in, std::int64_t count) {
  if (config_.render_cache == nullptr) return std::vector<bool>(static_cast<std::size_t>(std::max<std::int64_t>(count, 0)), false);
  return render::CachedFrames(*config_.render_cache, *Snapshot(), in, count, config_.compile, config_.compositor, Versions());
}

media::VideoFrame PlaybackEngine::RenderFrame(const time::RationalTime& at) {
  return RenderFrame(at, config_.compile);
}

media::VideoFrame PlaybackEngine::RenderFrame(const time::RationalTime& at,
                                              const timeline::CompileOptions& options) {
  const auto started = std::chrono::steady_clock::now();
  const auto graph = Snapshot();
  const auto* root = graph->root();
  if (root == nullptr) throw std::logic_error("Playback engine has no sequence");
  media::VideoFrame frame;
  {
    const std::lock_guard<std::mutex> picture(video_mutex_);
    // Nested composites are per-frame; holding them across frames would serve
    // stale pictures after a seek.
    nested_frames_.clear();
    frame = ComposeSequence(*graph, *root, at, 0, options);
  }
  counters_.frames_rendered.fetch_add(1, std::memory_order_relaxed);
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  counters_.total_frame_render_microseconds.fetch_add(elapsed, std::memory_order_relaxed);
  auto maximum = counters_.max_frame_render_microseconds.load(std::memory_order_relaxed);
  while (elapsed > maximum && !counters_.max_frame_render_microseconds.compare_exchange_weak(
                                  maximum, elapsed, std::memory_order_relaxed)) {
  }
  ScheduleReadAhead(graph, at, options, false);
  return frame;
}

media::AudioBuffer PlaybackEngine::RenderAudio(const time::RationalTime& at, std::int64_t frames) {
  return RenderAudio(at, frames, config_.compile);
}

media::AudioBuffer PlaybackEngine::RenderAudio(const time::RationalTime& at, std::int64_t frames,
                                               const timeline::CompileOptions& options) {
  return RenderAudioAt(at.Rescale(config_.sample_rate, time::RoundingMode::Nearest), frames, options);
}

media::AudioBuffer PlaybackEngine::RenderAudioAt(std::int64_t first_sample, std::int64_t frames,
                                                 const timeline::CompileOptions& options) {
  if (frames <= 0) return media::AudioBuffer::Allocate(config_.sample_rate, config_.channels, 0);
  return MixBlock(mixer_, first_sample, frames, options);
}

media::AudioBuffer PlaybackEngine::MixBlock(const audio::AudioMixer& mixer, std::int64_t first_sample,
                                            std::int64_t frames, const timeline::CompileOptions& options) {
  const auto graph = Snapshot();
  if (graph->root() == nullptr) throw std::logic_error("Playback engine has no sequence");

  // The mixer places every clip edge, transition and keyframe at its own sample,
  // so a block is rendered whole rather than in pieces between boundaries.
  audio::MixStatistics mix;
  const std::lock_guard<std::mutex> sound(audio_mutex_);
  auto mixed = mixer.MixSamples(
      *graph, first_sample, frames,
      [this](const std::string& media_id, const time::RationalTime& at, std::int64_t sample_rate, int channels,
             std::int64_t block) -> std::optional<media::AudioBuffer> {
        const auto source = SourceFor(media_id);
        if (source == nullptr) return std::nullopt;
        try {
          return source->ReadAudio(at, sample_rate, channels, block);
        } catch (const std::exception&) {
          counters_.decode_failures.fetch_add(1, std::memory_order_relaxed);
          return std::nullopt;
        }
      },
      mix, options);
  counters_.audio_blocks_mixed.fetch_add(1, std::memory_order_relaxed);
  if (mix.clipped) counters_.audio_blocks_clipped.fetch_add(1, std::memory_order_relaxed);
  counters_.audio_missing_sources.fetch_add(mix.missing_sources, std::memory_order_relaxed);
  const auto master = audio::MeasureLevels(mixed);
  mix.track_levels["master"] = audio::TrackLevel{master.peak_db, master.rms_db};
  {
    const std::lock_guard<std::mutex> lock(audio_levels_mutex_);
    audio_levels_ = std::move(mix.track_levels);
  }
  return mixed;
}

std::map<std::string, audio::TrackLevel> PlaybackEngine::audio_levels() const {
  const std::lock_guard<std::mutex> lock(audio_levels_mutex_);
  return audio_levels_;
}

void PlaybackEngine::SetOutputSize(int width, int height) {
  const std::lock_guard<std::mutex> picture(video_mutex_);
  if (config_.compositor.width == width && config_.compositor.height == height) return;
  config_.compositor.width = width;
  config_.compositor.height = height;
  compositor_ = render::Compositor(config_.compositor);
}

void PlaybackEngine::SetPreferProxies(bool on) {
  if (prefer_proxies_.exchange(on, std::memory_order_relaxed) == on) return;
  // Every open source is asked where its media is now, as after a relink.
  UpdateSequence(*Snapshot());
}

void PlaybackEngine::UpdateSequence(timeline::SequenceGraph graph, bool invalidate_media) {
  auto replacement = std::make_shared<const timeline::SequenceGraph>(std::move(graph));
  {
    const std::lock_guard<std::mutex> lock(graph_mutex_);
    graph_ = std::move(replacement);
  }
  (void)AdvanceReadAheadGeneration();

  if (!invalidate_media) {
    const std::lock_guard<std::mutex> picture(video_mutex_);
    stretch_cache_->Clear();
    nested_frames_.clear();
    return;
  }

  // Relinking changes where a media item lives without changing its id, so an open
  // decoder keyed by id would go on reading the old file, and an item that was
  // offline would stay offline however many times it was fixed. Ask the locator
  // again, once per edit rather than once per frame. A render that already holds a
  // decoder keeps it until it is done.
  std::vector<std::string> ids;
  {
    const std::lock_guard<std::mutex> lock(sources_mutex_);
    for (const auto& entry : sources_) ids.push_back(entry.first);
  }
  std::vector<std::pair<std::string, std::string>> located;
  for (const auto& id : ids) located.emplace_back(id, ResolveMedia(id));
  {
    const std::lock_guard<std::mutex> lock(sources_mutex_);
    for (const auto& [id, path] : located) {
      const auto found = sources_.find(id);
      if (found != sources_.end() && found->second.path != path) sources_.erase(found);
    }
    offline_.clear();
  }

  // An edit can change what any clip resolves to, so cached pictures are no longer
  // trustworthy.
  const std::lock_guard<std::mutex> picture(video_mutex_);
  stretch_cache_->Clear();
  cache_.clear();
  cache_index_.clear();
  cache_bytes_ = 0;
  nested_frames_.clear();
  device_sources_.clear();  // hardware decoders are reopened on the next frame, at the path the locator now gives
}

void PlaybackEngine::Play(audio::AudioSink& sink) {
  {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    if (playing_) return;
    sink_ = &sink;
    play_origin_ = audio_cursor_;
    playing_ = true;
  }
  sink.clock().Reset();

  // The sound is rendered at the device's own rate and layout. Rendering at the
  // engine's and handing the device's block size to it played every file whose
  // device ran at another rate at the wrong pitch.
  const auto format = sink.format();
  auto mixer = std::make_shared<audio::AudioMixer>(
      audio::MixerConfig{format.sample_rate, format.channels, true, stretch_cache_});

  // The sink pulls on its own thread. The cursor advances by exactly the number
  // of frames handed over, so the audio timeline cannot drift from the device.
  auto callback = [this, mixer](media::AudioBuffer& buffer) -> audio::BlockResult {
    std::uint64_t generation = 0;
    time::RationalTime at;
    {
      const std::lock_guard<std::mutex> lock(transport_mutex_);
      generation = generation_;
      at = audio_cursor_;
    }
    const auto rate = buffer.sample_rate();
    auto mixed = MixBlock(*mixer, at.Rescale(rate, time::RoundingMode::Nearest), buffer.frames(), config_.compile);
    for (int channel = 0; channel < std::min(buffer.channels(), mixed.channels()); ++channel) {
      std::copy_n(mixed.channel(channel), std::min(buffer.frames(), mixed.frames()), buffer.channel(channel));
    }
    const auto advanced = at.Add(time::RationalTime(buffer.frames(), rate));
    {
      const std::lock_guard<std::mutex> lock(transport_mutex_);
      // A seek while this block was being rendered: it belongs to the old
      // position. Committing it would move the cursor back from where the seek put
      // it, and playing it would be a few milliseconds of the wrong audio.
      if (generation_ != generation) {
        counters_.audio_blocks_dropped.fetch_add(1, std::memory_order_relaxed);
        return audio::BlockResult::Dropped;
      }
      audio_cursor_ = advanced;
    }
    // Report whether *this* block held audio, not whether another one will.
    // Testing the advanced cursor instead would mark the block containing the
    // final samples as empty and cut the last one short.
    const auto graph = Snapshot();
    return at.Compare(graph->root()->Duration()) < 0 ? audio::BlockResult::Audio : audio::BlockResult::End;
  };
  {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    render_callback_ = callback;
  }
  sink.Start(std::move(callback));
}

void PlaybackEngine::Pause() {
  audio::AudioSink* sink = nullptr;
  {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    if (!playing_) return;
    sink = sink_;
    playing_ = false;
  }
  // Stop joins the producer, which may be inside the callback waiting for the
  // transport lock: it must not be held here.
  if (sink != nullptr) sink->Stop();
  const std::lock_guard<std::mutex> lock(transport_mutex_);
  sink_ = nullptr;
  render_callback_ = nullptr;
}

void PlaybackEngine::Seek(const time::RationalTime& to) {
  audio::AudioSink* sink = nullptr;
  audio::RenderCallback restart;
  {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    audio_cursor_ = to;
    play_origin_ = to;
    ++generation_;
    sink = sink_;
    restart = render_callback_;
  }
  PrimeReadAhead(to);
  if (sink == nullptr) return;
  // Outside the lock: the sink waits for its producer, which may be waiting for it.
  if (sink->state() == audio::SinkState::Finished && restart) {
    // The end of the timeline was reached and played out; the producer has gone.
    // Seeking back means play again from there.
    sink->Stop();
    sink->clock().Reset();
    sink->Start(std::move(restart));
    return;
  }
  sink->Flush();
}

bool PlaybackEngine::playing() const {
  const std::lock_guard<std::mutex> lock(transport_mutex_);
  return playing_;
}

audio::SinkState PlaybackEngine::playback_state() const {
  audio::AudioSink* sink = nullptr;
  {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    sink = sink_;
  }
  return sink != nullptr ? sink->state() : audio::SinkState::Stopped;
}

std::string PlaybackEngine::playback_error() const {
  audio::AudioSink* sink = nullptr;
  {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    sink = sink_;
  }
  return sink != nullptr ? sink->error() : std::string();
}

time::RationalTime PlaybackEngine::position() const {
  time::RationalTime position;
  {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    if (!playing_ || sink_ == nullptr) {
      position = audio_cursor_;
    } else {
      // Where the device has actually reached, which trails what has been queued by
      // the output latency. Rendering picture for the queued position instead would
      // put the image ahead of the sound by the size of the device buffer.
      const auto elapsed = sink_->clock().Now();
      const auto audible = elapsed.Subtract(sink_->latency());
      position = play_origin_.Add(audible);
      if (position.Compare({0, 1}) < 0) position = play_origin_;
    }
  }
  // Neither a sink that keeps its clock running through the silence after the end,
  // nor a cursor that a callback advanced past it, is a position on the timeline.
  const auto end = Snapshot()->root()->Duration();
  return position.Compare(end) > 0 ? end : position;
}

}  // namespace cutline::playback

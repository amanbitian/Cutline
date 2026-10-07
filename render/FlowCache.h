#pragma once

// Optical-flow analysis, kept so it is done once.
//
// Estimating flow between two frames is the expensive part of a motion-compensated slow-down (about
// a second at 1080p on the reference implementation), and a clip played at a quarter of its speed
// asks for the same pair of source frames four times over. The preview, an export of the same
// timeline and a background analysis pass all ask for the same pairs. The cache makes them one piece
// of work.
//
// Addressed by content. The key is a hash of the two frames' pixels (as the float pictures the
// estimator reads), their size, the estimator's settings and its algorithm version. Nothing is
// tracked or invalidated: a different frame, a different setting or a newer algorithm is a different
// key, and the old entry is never asked for again. Because the key is the content, it does not matter
// which of preview, export or background analysis computed it.
//
// Two tiers like the render cache: memory to a byte budget, least recently used out first, and
// optionally disk (one file per pair, written under a temporary name and renamed, checksummed so a
// damaged file is a miss rather than a wrong field).

#include "media/VideoFrame.h"
#include "core/resource/QuotaManager.h"
#include "render/OpticalFlow.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace cutline::render {

// Bumped whenever EstimateOpticalFlow would give a different answer for the same input.
inline constexpr int kFlowAlgorithmVersion = 1;

using FlowKey = std::string;  // 32 hex characters

[[nodiscard]] FlowKey MakeFlowKey(const media::VideoFrame& first, const media::VideoFrame& second, const OpticalFlowConfig& config);

struct FlowCacheConfig final {
  std::size_t memory_bytes{128u << 20};
  // Empty: memory only.
  std::filesystem::path directory;
  // Disk budget; the oldest files go first. 0 means unlimited.
  std::uint64_t disk_bytes{1ull << 30};
  std::shared_ptr<resource::QuotaManager> quota_manager;
  double memory_recompute_cost{100.0};
  double disk_recompute_cost{60.0};
};

struct FlowCacheStats final {
  std::uint64_t memory_hits{0};
  std::uint64_t disk_hits{0};
  std::uint64_t misses{0};
  std::uint64_t computed{0};
  std::uint64_t evictions{0};
  std::uint64_t disk_writes{0};
  // Files that were there but could not be trusted (short, wrong checksum, wrong size) and were removed.
  std::uint64_t disk_rejected{0};
  std::uint64_t shared_quota_evictions{0};
  std::size_t entries{0};
  std::size_t bytes{0};
};

enum class FlowSource { Memory, Disk, Computed };

struct FlowLookup final {
  std::shared_ptr<const FlowField> field;
  FlowSource source{FlowSource::Computed};
};

class FlowCache final {
 public:
  explicit FlowCache(FlowCacheConfig config = {});
  ~FlowCache();
  FlowCache(const FlowCache&) = delete;
  FlowCache& operator=(const FlowCache&) = delete;

  // Memory, then disk. Null when the pair has not been analysed.
  [[nodiscard]] std::shared_ptr<const FlowField> Find(const FlowKey& key);
  void Put(const FlowKey& key, std::shared_ptr<const FlowField> field);

  // The field for a pair: found, or computed here. When two threads ask for the same pair at once one
  // computes and the other waits for it, so the work is never done twice.
  [[nodiscard]] FlowLookup GetOrCompute(const FlowKey& key, const std::function<FlowField()>& compute);

  [[nodiscard]] FlowCacheStats Stats() const;
  // Drops memory and disk entries.
  void Clear();

 private:
  struct Entry final {
    FlowKey key;
    std::shared_ptr<const FlowField> field;
    std::size_t bytes{0};
  };
  [[nodiscard]] std::filesystem::path PathFor(const FlowKey& key) const;
  void InsertLocked(const FlowKey& key, std::shared_ptr<const FlowField> field);
  void WriteDisk(const FlowKey& key, const FlowField& field);
  [[nodiscard]] std::shared_ptr<const FlowField> ReadDisk(const FlowKey& key);
  void TrimDisk();
  void EvictSharedMemory(const FlowKey& key);
  void EvictSharedDisk(const FlowKey& key);

  FlowCacheConfig config_;
  mutable std::mutex mutex_;
  std::list<Entry> lru_;
  std::unordered_map<FlowKey, std::list<Entry>::iterator> index_;
  std::unordered_map<FlowKey, std::shared_future<std::shared_ptr<const FlowField>>> in_flight_;
  FlowCacheStats stats_;
  std::size_t bytes_{0};
  std::uint64_t writes_since_trim_{0};
  std::string quota_owner_;
};

// The flow between two frames through the cache.
[[nodiscard]] FlowLookup EstimateOpticalFlowCached(FlowCache& cache, const media::VideoFrame& first, const media::VideoFrame& second,
                                                   const OpticalFlowConfig& config = {});

// ---------------------------------------------------------------- background analysis ----

struct FlowAnalysisProgress final {
  std::size_t total_pairs{0};
  // Pairs finished, of whichever kind below.
  std::size_t done{0};
  // Already in the cache.
  std::size_t reused{0};
  // Analysed by this job.
  std::size_t computed{0};
  // Not analysable: a frame was missing or the two differ in size.
  std::size_t skipped{0};
  bool finished{false};
  bool cancelled{false};
  // Set if the job stopped on an unexpected failure.
  std::string error;
};

// Analyses every consecutive pair of a run of frames on its own thread, filling the cache so playback
// and export find the work done. It can be cancelled, reports progress, and stops when destroyed.
class FlowAnalysis final {
 public:
  // The frame at an index, or null if it cannot be had. Called from the analysis thread only, one
  // index at a time in increasing order; the picture must stay valid until the next call.
  using Frames = std::function<const media::VideoFrame*(std::size_t index)>;
  using Observer = std::function<void(const FlowAnalysisProgress&)>;

  FlowAnalysis(std::shared_ptr<FlowCache> cache, Frames frames, std::size_t frame_count, OpticalFlowConfig config = {});
  ~FlowAnalysis();
  FlowAnalysis(const FlowAnalysis&) = delete;
  FlowAnalysis& operator=(const FlowAnalysis&) = delete;

  // Called from the analysis thread after each pair. Set before Start.
  void Observe(Observer observer) { observer_ = std::move(observer); }
  void Start();
  void Cancel();
  // Returns when the job has finished or been cancelled.
  void Wait();
  [[nodiscard]] FlowAnalysisProgress Progress() const;

 private:
  void Run();

  std::shared_ptr<FlowCache> cache_;
  Frames frames_;
  std::size_t frame_count_;
  OpticalFlowConfig config_;
  Observer observer_;
  std::atomic<bool> cancel_{false};
  mutable std::mutex mutex_;
  FlowAnalysisProgress progress_;
  std::thread worker_;
};

}  // namespace cutline::render

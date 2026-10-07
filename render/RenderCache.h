#pragma once

// A cache of rendered pictures, addressed by what they are made from.
//
// Invalidation by address. The usual way to keep a render cache honest is to track edits and
// throw away what they touch, and that is where such caches go wrong: an edit nobody thought to
// hook leaves a stale frame on screen. Here a frame's address is a hash of everything the picture
// is a function of: the compiled plan for that instant (each clip's source, source time, speed and
// direction, every effect's parameters as sampled at that instant, transitions and their progress,
// the sequence's own effects), the versions of the media and assets it reads, the render version,
// and the output format. Change any of those and the address changes, so the old entry is simply
// never asked for; change none and the entry is found. An edit therefore invalidates exactly the
// frames it affects: a keyframe moved at 10 s leaves 5 s alone because the frame at 5 s is the same
// function of the same inputs as before.
//
// Two tiers. Memory holds recent frames to a byte budget, least recently used out first. Disk,
// optionally, holds more under its own budget, one file per frame, written to a temporary name and
// renamed so a crash leaves nothing half-written, and checksummed so a damaged file reads as a
// miss rather than as a wrong picture.
//
// Only complete pictures belong here. A frame composed while a source was not yet decoded has a
// hole in it that a later render would fill, so callers store a frame only when nothing was missing.

#include "media/VideoFrame.h"
#include "core/resource/QuotaManager.h"
#include "render/Compositor.h"
#include "timeline/Sequence.h"
#include "timeline/TimelineCompiler.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cutline::render {

// 128 bits of a SHA-256, as 32 hex characters.
using RenderKey = std::string;

// What a picture reads that the plan does not contain: the identity of media and of assets
// (look-up tables). Each returns a string that changes whenever the thing it names changes.
struct SourceVersions final {
  std::function<std::string(const std::string& media_id)> media;
  std::function<std::string(const std::string& asset_reference)> asset;
};

// The address of the picture a plan composes to. `nested` are the addresses of the nested
// sequences' own pictures, in the order the plan's nested clips appear.
[[nodiscard]] RenderKey KeyForPlan(const timeline::PlaybackPlan& plan, const CompositorConfig& config, const SourceVersions& versions,
                                   const std::vector<RenderKey>& nested = {});

// The address of the picture of `sequence` at `at`, following nested sequences the way rendering does.
[[nodiscard]] RenderKey KeyForTime(const timeline::TimelineCompiler& compiler, const timeline::SequenceGraph& graph,
                                   const timeline::Sequence& sequence, const time::RationalTime& at,
                                   const timeline::CompileOptions& options, const CompositorConfig& config,
                                   const SourceVersions& versions, int depth = 0);

struct RenderCacheConfig final {
  std::size_t memory_bytes{512u * 1024u * 1024u};
  // Empty: memory only.
  std::filesystem::path disk_directory;
  std::size_t disk_bytes{4096ull * 1024u * 1024u};
  // Optional cross-cache budget. Local budgets remain hard per-cache caps;
  // this one prevents their sum from exhausting the machine.
  std::shared_ptr<resource::QuotaManager> quota_manager;
  double memory_recompute_cost{8.0};
  double disk_recompute_cost{4.0};
};

struct RenderCacheStatistics final {
  std::int64_t hits{0};
  std::int64_t misses{0};
  std::int64_t stores{0};
  std::int64_t memory_evictions{0};
  std::int64_t disk_hits{0};
  std::int64_t disk_writes{0};
  std::int64_t disk_evictions{0};
  std::int64_t corrupt_files{0};
  std::int64_t shared_quota_evictions{0};
  std::size_t memory_bytes{0};
  std::size_t disk_bytes{0};
  std::size_t entries{0};
};

class RenderCache final {
 public:
  explicit RenderCache(RenderCacheConfig config = {});
  ~RenderCache();

  // The picture, or nullptr. A hit promotes it to most recent; a disk hit also brings it into memory.
  [[nodiscard]] std::shared_ptr<const media::VideoFrame> Find(const RenderKey& key);
  // Whether it is held, in memory or on disk, without counting as a use.
  [[nodiscard]] bool Contains(const RenderKey& key) const;
  void Store(const RenderKey& key, const media::VideoFrame& frame);
  void Clear();

  [[nodiscard]] RenderCacheStatistics statistics() const;
  [[nodiscard]] const RenderCacheConfig& config() const noexcept { return config_; }

 private:
  struct Entry final {
    std::shared_ptr<const media::VideoFrame> frame;
    std::size_t bytes{0};
    std::list<RenderKey>::iterator position;
  };
  struct DiskEntry final {
    std::size_t bytes{0};
    std::filesystem::file_time_type time;
  };

  void EvictMemoryLocked();
  void EvictDiskLocked();
  [[nodiscard]] std::filesystem::path PathFor(const RenderKey& key) const;
  void WriteDiskLocked(const RenderKey& key, const media::VideoFrame& frame);
  [[nodiscard]] std::shared_ptr<const media::VideoFrame> ReadDiskLocked(const RenderKey& key);
  void InsertMemoryLocked(const RenderKey& key, std::shared_ptr<const media::VideoFrame> frame);
  void EvictShared(const RenderKey& key, resource::Tier tier);

  RenderCacheConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<RenderKey, Entry> memory_;
  std::list<RenderKey> recency_;  // front is most recent
  std::map<RenderKey, DiskEntry> disk_;
  RenderCacheStatistics stats_;
  std::string quota_owner_;
};

// Which frames of [first, first + count) at the sequence's rate the cache already holds: the cache
// indicator above a timeline, and what render-in-to-out would skip.
[[nodiscard]] std::vector<bool> CachedFrames(const RenderCache& cache, const timeline::SequenceGraph& graph,
                                             const time::RationalTime& first, std::int64_t count,
                                             const timeline::CompileOptions& options, const CompositorConfig& config,
                                             const SourceVersions& versions);

}  // namespace cutline::render

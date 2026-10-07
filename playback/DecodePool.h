#pragma once

#include "core/time/RationalTime.h"
#include "media/VideoFrame.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace cutline::playback {

// One raw media frame to decode. Jobs contain a resolved path so workers never
// need to call project/database code, and a generation so edits and seeks can
// cheaply invalidate queued and in-flight work.
struct DecodeJob final {
  std::string cache_key;
  std::string media_id;
  std::string path;
  time::RationalTime source_time;
  std::uint64_t generation{};
};

enum class EnqueueResult { Queued, Duplicate, Full, Stale, Stopped };

// A bounded pool of independent media decoders. A Source has seek state and
// permits only one video reader, so every worker owns its own Source instances.
// The callback runs on a worker thread and must remain short.
class DecodePool final {
 public:
  using Completion = std::function<void(DecodeJob, std::optional<media::VideoFrame>)>;

  DecodePool(std::size_t workers, std::size_t capacity, std::size_t source_capacity,
             std::uint64_t generation, Completion completion);
  ~DecodePool();
  DecodePool(const DecodePool&) = delete;
  DecodePool& operator=(const DecodePool&) = delete;

  [[nodiscard]] EnqueueResult Enqueue(DecodeJob job);
  // Drops queued work and makes in-flight results from older generations inert.
  void AdvanceGeneration(std::uint64_t generation);
  void Stop();

 private:
  [[nodiscard]] static std::string Identity(const DecodeJob& job);
  void WorkerLoop();

  const std::size_t capacity_;
  const std::size_t source_capacity_;
  Completion completion_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<DecodeJob> queue_;
  std::unordered_set<std::string> pending_;
  std::vector<std::thread> workers_;
  std::uint64_t generation_{};
  bool stopping_{false};
};

}  // namespace cutline::playback

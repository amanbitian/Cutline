#pragma once

// One bounded scheduler for latency-sensitive playback and background work.
// Callers keep domain-specific progress reporting; this class owns ordering,
// deduplication, generation cancellation and background concurrency limits.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cutline::jobs {

enum class Priority : int {
  RealtimeAudio = 0,
  VisibleFrame = 1,
  Presentation = 2,
  Interactive = 3,
  ReadAhead = 4,
  Scopes = 5,
  Export = 6,
  CacheRender = 7,
  Proxy = 8,
  Analysis = 9,
};

class CancellationToken final {
 public:
  CancellationToken() = default;
  [[nodiscard]] bool cancelled() const noexcept;

 private:
  explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}
  std::shared_ptr<std::atomic<bool>> flag_;
  friend class PriorityScheduler;
};

struct SchedulerConfig final {
  std::size_t workers{4};
  std::size_t queue_capacity{256};
  // At most workers-reserved workers run export/proxy/cache/analysis work.
  // The reserved workers remain available for visible and interactive work.
  std::size_t reserved_interactive_workers{1};
};

enum class SubmitResult { Queued, ReplacedBackgroundWork, Duplicate, Full, Stale, Stopped };

struct Submission final {
  SubmitResult result{SubmitResult::Stopped};
  std::uint64_t id{};
};

struct SchedulerStatistics final {
  std::uint64_t submitted{};
  std::uint64_t executed{};
  std::uint64_t failed{};
  std::uint64_t cancelled{};
  std::uint64_t replaced{};
  std::uint64_t rejected{};
  std::size_t queue_peak{};
  std::size_t queued{};
  std::size_t running{};
};

class PriorityScheduler final {
 public:
  using Work = std::function<void(const CancellationToken&)>;

  explicit PriorityScheduler(SchedulerConfig config = {});
  ~PriorityScheduler();
  PriorityScheduler(const PriorityScheduler&) = delete;
  PriorityScheduler& operator=(const PriorityScheduler&) = delete;

  [[nodiscard]] Submission Submit(std::string key, Priority priority, std::uint64_t generation, Work work);
  [[nodiscard]] bool Cancel(std::uint64_t id);
  // Cancels queued and running jobs from every older generation.
  void AdvanceGeneration(std::uint64_t generation);
  [[nodiscard]] bool WaitIdle(std::chrono::milliseconds timeout);
  [[nodiscard]] SchedulerStatistics statistics() const;
  void Stop();

 private:
  struct Job;
  [[nodiscard]] static bool Background(Priority priority) noexcept;
  [[nodiscard]] std::size_t BackgroundLimit() const noexcept;
  [[nodiscard]] std::size_t BestRunnableLocked() const;
  void WorkerLoop();

  SchedulerConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable idle_;
  std::vector<std::shared_ptr<Job>> queue_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Job>> running_;
  std::unordered_set<std::string> identities_;
  std::vector<std::thread> workers_;
  SchedulerStatistics statistics_;
  std::uint64_t next_id_{1};
  std::uint64_t sequence_{};
  std::uint64_t generation_{};
  bool generation_enforced_{};
  std::size_t running_background_{};
  bool stopping_{};
};

}  // namespace cutline::jobs

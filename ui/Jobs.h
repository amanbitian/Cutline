#pragma once

// Background jobs the person should be able to see and stop: tracking and stabilisation analysis, optical-flow
// analysis, proxy generation, pre-rendering, exports. A tracker holds what each is doing (state, progress,
// message) and a cancel request each worker polls; a runner starts a job on a thread of its own and reports into
// the tracker. Neither knows what a job does, so every kind of work shows up in one place and is stopped the same
// way.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cutline::ui {

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };

[[nodiscard]] std::string ToString(JobState state);
[[nodiscard]] inline bool Finished(JobState state) { return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled; }

struct JobInfo final {
  int id{0};
  std::string kind;   // "tracking", "optical_flow", "proxy", "export", ...
  std::string title;  // "Analysing Clip 3"
  JobState state{JobState::Queued};
  double progress{0.0};  // 0 to 1; negative when it cannot say
  std::string message;
  std::string error;
  bool cancel_requested{false};
};

class JobTracker final {
 public:
  using Observer = std::function<void(const JobInfo&)>;

  int Create(std::string kind, std::string title);
  void Start(int id);
  // done of total; a total of zero means progress is unknown.
  void Progress(int id, std::uint64_t done, std::uint64_t total, const std::string& message = {});
  void Succeed(int id, const std::string& message = {});
  void Fail(int id, const std::string& error);
  // Marks the job as stopped on request.
  void MarkCancelled(int id);
  // Asks a job to stop; the worker sees it through CancelToken and finishes. A job that has not started is
  // cancelled at once.
  void RequestCancel(int id);
  [[nodiscard]] bool CancelRequested(int id) const;
  [[nodiscard]] std::function<bool()> CancelToken(int id) const;

  [[nodiscard]] std::vector<JobInfo> Snapshot() const;
  [[nodiscard]] std::optional<JobInfo> Find(int id) const;
  // Jobs that have not finished.
  [[nodiscard]] int ActiveCount() const;
  // Forgets jobs that have finished.
  void ClearFinished();
  // Called after every change, on the thread that made it, outside the tracker's lock.
  void Observe(Observer observer);

 private:
  void Notify(const JobInfo& info);

  mutable std::mutex mutex_;
  std::vector<JobInfo> jobs_;
  int next_id_{1};
  std::vector<Observer> observers_;
};

// What a running job sees of its own record.
class JobContext final {
 public:
  JobContext(JobTracker& tracker, int id) : tracker_(tracker), id_(id) {}
  [[nodiscard]] int id() const { return id_; }
  void Progress(std::uint64_t done, std::uint64_t total, const std::string& message = {}) { tracker_.Progress(id_, done, total, message); }
  [[nodiscard]] bool cancelled() const { return tracker_.CancelRequested(id_); }
  [[nodiscard]] std::function<bool()> cancel_token() const { return tracker_.CancelToken(id_); }

 private:
  JobTracker& tracker_;
  int id_;
};

// Runs jobs on threads of its own and waits for them when destroyed. The function returns normally on success and
// throws to fail; it polls `context.cancelled()` to find out whether to stop, and if it stops for that reason it
// returns and the job is recorded as cancelled.
class JobRunner final {
 public:
  explicit JobRunner(JobTracker& tracker) : tracker_(tracker) {}
  ~JobRunner();
  JobRunner(const JobRunner&) = delete;
  JobRunner& operator=(const JobRunner&) = delete;

  int Run(std::string kind, std::string title, std::function<void(JobContext&)> work);
  // Waits for every job started so far.
  void WaitAll();

 private:
  JobTracker& tracker_;
  std::vector<std::thread> threads_;
};

}  // namespace cutline::ui

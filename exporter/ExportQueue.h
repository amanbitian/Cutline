#pragma once

// The export queue: exports waiting to run, running, and finished, kept on disk so that closing the application, or the
// application dying, loses nothing.
//
// Jobs run one at a time, in order, on a worker thread of their own. What a job *does* is the executor's business (the
// application builds the engine for its sequence and calls exporter::Export); the queue only owns the order, the state, the
// progress, and the record. After a restart a job that was running is shown as interrupted, with a way to run it again,
// and the jobs still waiting go on waiting; nothing starts a delivery the person did not queue.
//
// A job writes its file through the export worker, which publishes only a finished file, so an interrupted or cancelled
// job leaves what was at the destination untouched.

#include "core/time/RationalTime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cutline::exporter {

enum class JobState { Queued, Running, Done, Failed, Cancelled, Interrupted };

[[nodiscard]] const char* ToString(JobState state);
[[nodiscard]] std::optional<JobState> ParseJobState(const std::string& text);

struct ExportJob final {
  std::string id;
  std::string name;
  std::string sequence_id;
  std::string preset_id;
  std::string output_path;
  bool overwrite{false};
  bool prefer_hardware{true};
  time::RationalTime in{0, 1};
  time::RationalTime out{0, 1};  // zero: to the end of the sequence

  JobState state{JobState::Queued};
  std::int64_t frames_done{0};
  std::int64_t frames_total{0};
  std::string error;
  std::string encoder;  // what encoded it, filled in when it runs
  bool hardware{false};
  std::vector<std::string> notes;
  std::vector<std::string> problems;  // what the check of the finished file found
  std::int64_t output_bytes{0};
  double seconds{0};  // how long it took
  std::string created, started, finished;  // UTC, ISO 8601

  [[nodiscard]] double progress() const { return frames_total > 0 ? static_cast<double>(frames_done) / static_cast<double>(frames_total) : 0.0; }
  [[nodiscard]] bool finished_state() const { return state == JobState::Done || state == JobState::Failed || state == JobState::Cancelled; }
};

struct JobOutcome final {
  std::string encoder;
  bool hardware{false};
  std::vector<std::string> notes;
  std::vector<std::string> problems;
  std::int64_t output_bytes{0};
  std::int64_t frames{0};
};

// Reports how far the job is; returns false when the job has been cancelled (or the queue is closing) and should stop.
using JobProgress = std::function<bool(std::int64_t done, std::int64_t total)>;
// Runs a job. Throws on failure (the message becomes the job's error). Returns normally when the job finished, or when
// `progress` told it to stop (the queue then records it as cancelled).
using JobExecutor = std::function<JobOutcome(const ExportJob& job, const JobProgress& progress)>;

class ExportQueue final {
 public:
  // `state_file` is where the queue is kept (created with its folder); empty keeps it in memory only.
  ExportQueue(std::string state_file, JobExecutor executor, bool start_paused = false);
  ~ExportQueue();
  ExportQueue(const ExportQueue&) = delete;
  ExportQueue& operator=(const ExportQueue&) = delete;

  // Queues a job (an id and the creation time are filled in when empty). Returns its id.
  std::string Add(ExportJob job);
  // Stops a queued job, or asks the running one to stop. False when there is no such job or it has already finished.
  bool Cancel(const std::string& id);
  // Takes a job off the list. Not the running one.
  bool Remove(const std::string& id);
  // Puts a failed, cancelled or interrupted job back at the end of the queue.
  bool Retry(const std::string& id);
  // Moves a waiting job to a place among the waiting jobs (0 is next).
  bool Move(const std::string& id, int position);
  void Pause();   // lets the running job finish, starts no more
  void Resume();
  [[nodiscard]] bool paused() const;
  void ClearFinished();

  [[nodiscard]] std::vector<ExportJob> Jobs() const;
  [[nodiscard]] std::optional<ExportJob> Find(const std::string& id) const;
  [[nodiscard]] std::string state_file() const { return state_file_; }

  // Called after any change, from whichever thread made it.
  void SetListener(std::function<void()> listener);
  // Returns when nothing is queued or running, or after the timeout; true when idle.
  [[nodiscard]] bool WaitIdle(std::chrono::milliseconds timeout) const;

 private:
  void Run();
  void Save();           // under mutex_
  void Load();
  void Changed();        // under mutex_: save, wake, notify
  [[nodiscard]] ExportJob* FindUnlocked(const std::string& id);

  std::string state_file_;
  JobExecutor executor_;
  mutable std::mutex mutex_;
  mutable std::condition_variable wake_;
  mutable std::condition_variable idle_;
  std::vector<ExportJob> jobs_;
  std::function<void()> listener_;
  std::string running_id_;
  bool cancel_running_{false};
  bool paused_{false};
  bool stopping_{false};
  std::uint64_t counter_{0};
  std::thread worker_;
};

}  // namespace cutline::exporter

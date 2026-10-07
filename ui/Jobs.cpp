#include "ui/Jobs.h"

#include <algorithm>

namespace cutline::ui {

std::string ToString(JobState state) {
  switch (state) {
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
  }
  return "queued";
}

namespace {

JobInfo* Locate(std::vector<JobInfo>& jobs, int id) {
  for (auto& job : jobs) {
    if (job.id == id) return &job;
  }
  return nullptr;
}

}  // namespace

void JobTracker::Notify(const JobInfo& info) {
  std::vector<Observer> observers;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    observers = observers_;
  }
  for (const auto& observer : observers) observer(info);
}

int JobTracker::Create(std::string kind, std::string title) {
  JobInfo info;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    info.id = next_id_++;
    info.kind = std::move(kind);
    info.title = std::move(title);
    info.progress = 0.0;
    jobs_.push_back(info);
  }
  Notify(info);
  return info.id;
}

void JobTracker::Start(int id) {
  std::optional<JobInfo> info;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto* job = Locate(jobs_, id); job != nullptr && job->state == JobState::Queued) {
      job->state = JobState::Running;
      info = *job;
    }
  }
  if (info) Notify(*info);
}

void JobTracker::Progress(int id, std::uint64_t done, std::uint64_t total, const std::string& message) {
  std::optional<JobInfo> info;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto* job = Locate(jobs_, id); job != nullptr && !Finished(job->state)) {
      job->progress = total == 0 ? -1.0 : std::min(1.0, static_cast<double>(done) / static_cast<double>(total));
      if (!message.empty()) job->message = message;
      info = *job;
    }
  }
  if (info) Notify(*info);
}

void JobTracker::Succeed(int id, const std::string& message) {
  std::optional<JobInfo> info;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto* job = Locate(jobs_, id); job != nullptr && !Finished(job->state)) {
      job->state = JobState::Succeeded;
      job->progress = 1.0;
      if (!message.empty()) job->message = message;
      info = *job;
    }
  }
  if (info) Notify(*info);
}

void JobTracker::Fail(int id, const std::string& error) {
  std::optional<JobInfo> info;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto* job = Locate(jobs_, id); job != nullptr && !Finished(job->state)) {
      job->state = JobState::Failed;
      job->error = error;
      info = *job;
    }
  }
  if (info) Notify(*info);
}

void JobTracker::MarkCancelled(int id) {
  std::optional<JobInfo> info;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto* job = Locate(jobs_, id); job != nullptr && !Finished(job->state)) {
      job->state = JobState::Cancelled;
      info = *job;
    }
  }
  if (info) Notify(*info);
}

void JobTracker::RequestCancel(int id) {
  std::optional<JobInfo> info;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto* job = Locate(jobs_, id); job != nullptr && !Finished(job->state)) {
      job->cancel_requested = true;
      if (job->state == JobState::Queued) job->state = JobState::Cancelled;
      info = *job;
    }
  }
  if (info) Notify(*info);
}

bool JobTracker::CancelRequested(int id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& job : jobs_) {
    if (job.id == id) return job.cancel_requested;
  }
  return false;
}

std::function<bool()> JobTracker::CancelToken(int id) const {
  return [this, id]() { return CancelRequested(id); };
}

std::vector<JobInfo> JobTracker::Snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return jobs_;
}

std::optional<JobInfo> JobTracker::Find(int id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& job : jobs_) {
    if (job.id == id) return job;
  }
  return std::nullopt;
}

int JobTracker::ActiveCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(), [](const JobInfo& j) { return !Finished(j.state); }));
}

void JobTracker::ClearFinished() {
  const std::lock_guard<std::mutex> lock(mutex_);
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const JobInfo& j) { return Finished(j.state); }), jobs_.end());
}

void JobTracker::Observe(Observer observer) {
  const std::lock_guard<std::mutex> lock(mutex_);
  observers_.push_back(std::move(observer));
}

JobRunner::~JobRunner() { WaitAll(); }

int JobRunner::Run(std::string kind, std::string title, std::function<void(JobContext&)> work) {
  const auto id = tracker_.Create(std::move(kind), std::move(title));
  threads_.emplace_back([this, id, work = std::move(work)]() {
    // Cancelled before it began: it is already recorded as such.
    if (tracker_.CancelRequested(id)) return;
    tracker_.Start(id);
    JobContext context(tracker_, id);
    try {
      work(context);
      if (tracker_.CancelRequested(id)) tracker_.MarkCancelled(id);
      else tracker_.Succeed(id);
    } catch (const std::exception& error) {
      tracker_.Fail(id, error.what());
    } catch (...) {
      tracker_.Fail(id, "The job failed");
    }
  });
  return id;
}

void JobRunner::WaitAll() {
  for (auto& thread : threads_) {
    if (thread.joinable()) thread.join();
  }
  threads_.clear();
}

}  // namespace cutline::ui

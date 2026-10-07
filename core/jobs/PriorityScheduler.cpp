#include "core/jobs/PriorityScheduler.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cutline::jobs {

struct PriorityScheduler::Job final {
  std::uint64_t id{};
  std::uint64_t sequence{};
  std::uint64_t generation{};
  std::string key;
  std::string identity;
  Priority priority{Priority::Analysis};
  Work work;
  std::shared_ptr<std::atomic<bool>> cancelled{std::make_shared<std::atomic<bool>>(false)};
};

bool CancellationToken::cancelled() const noexcept {
  return flag_ != nullptr && flag_->load(std::memory_order_relaxed);
}

PriorityScheduler::PriorityScheduler(SchedulerConfig config) : config_(config) {
  if (config_.workers == 0 || config_.queue_capacity == 0) {
    throw std::invalid_argument("The scheduler needs workers and queue capacity");
  }
  config_.reserved_interactive_workers = std::min(config_.reserved_interactive_workers, config_.workers);
  workers_.reserve(config_.workers);
  for (std::size_t index = 0; index < config_.workers; ++index) workers_.emplace_back([this] { WorkerLoop(); });
}

PriorityScheduler::~PriorityScheduler() { Stop(); }

bool PriorityScheduler::Background(Priority priority) noexcept {
  return static_cast<int>(priority) >= static_cast<int>(Priority::Export);
}

std::size_t PriorityScheduler::BackgroundLimit() const noexcept {
  if (config_.reserved_interactive_workers >= config_.workers) return 1;
  return config_.workers - config_.reserved_interactive_workers;
}

std::size_t PriorityScheduler::BestRunnableLocked() const {
  auto best = queue_.size();
  for (std::size_t index = 0; index < queue_.size(); ++index) {
    const auto& candidate = queue_[index];
    if (Background(candidate->priority) && running_background_ >= BackgroundLimit()) continue;
    if (best == queue_.size() || static_cast<int>(candidate->priority) < static_cast<int>(queue_[best]->priority) ||
        (candidate->priority == queue_[best]->priority && candidate->sequence < queue_[best]->sequence)) {
      best = index;
    }
  }
  return best;
}

Submission PriorityScheduler::Submit(std::string key, Priority priority, std::uint64_t generation, Work work) {
  if (key.empty() || !work) throw std::invalid_argument("A scheduled job needs an identity and work");
  const auto identity = std::to_string(generation) + ":" + key;
  SubmitResult result = SubmitResult::Queued;
  std::uint64_t id = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return {SubmitResult::Stopped, 0};
    if (generation_enforced_ && generation != generation_) return {SubmitResult::Stale, 0};
    if (identities_.count(identity) != 0) return {SubmitResult::Duplicate, 0};
    if (queue_.size() >= config_.queue_capacity) {
      auto worst = queue_.end();
      for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (static_cast<int>((*it)->priority) <= static_cast<int>(priority)) continue;
        if (worst == queue_.end() || static_cast<int>((*it)->priority) > static_cast<int>((*worst)->priority) ||
            ((*it)->priority == (*worst)->priority && (*it)->sequence < (*worst)->sequence)) {
          worst = it;
        }
      }
      if (worst == queue_.end()) {
        ++statistics_.rejected;
        return {SubmitResult::Full, 0};
      }
      (*worst)->cancelled->store(true, std::memory_order_relaxed);
      identities_.erase((*worst)->identity);
      queue_.erase(worst);
      ++statistics_.cancelled;
      ++statistics_.replaced;
      result = SubmitResult::ReplacedBackgroundWork;
    }
    auto job = std::make_shared<Job>();
    job->id = next_id_++;
    job->sequence = sequence_++;
    job->generation = generation;
    job->key = std::move(key);
    job->identity = identity;
    job->priority = priority;
    job->work = std::move(work);
    id = job->id;
    identities_.insert(identity);
    queue_.push_back(std::move(job));
    ++statistics_.submitted;
    statistics_.queue_peak = std::max(statistics_.queue_peak, queue_.size());
  }
  ready_.notify_all();
  return {result, id};
}

bool PriorityScheduler::Cancel(std::uint64_t id) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto queued = std::find_if(queue_.begin(), queue_.end(), [id](const auto& job) { return job->id == id; });
    if (queued != queue_.end()) {
      (*queued)->cancelled->store(true, std::memory_order_relaxed);
      identities_.erase((*queued)->identity);
      queue_.erase(queued);
      ++statistics_.cancelled;
      if (queue_.empty() && running_.empty()) idle_.notify_all();
      return true;
    }
    const auto running = running_.find(id);
    if (running == running_.end()) return false;
    running->second->cancelled->store(true, std::memory_order_relaxed);
  }
  return true;
}

void PriorityScheduler::AdvanceGeneration(std::uint64_t generation) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    generation_enforced_ = true;
    for (auto it = queue_.begin(); it != queue_.end();) {
      if ((*it)->generation == generation) {
        ++it;
        continue;
      }
      (*it)->cancelled->store(true, std::memory_order_relaxed);
      identities_.erase((*it)->identity);
      it = queue_.erase(it);
      ++statistics_.cancelled;
    }
    for (auto& [id, job] : running_) {
      static_cast<void>(id);
      if (job->generation != generation) job->cancelled->store(true, std::memory_order_relaxed);
    }
    if (queue_.empty() && running_.empty()) idle_.notify_all();
  }
  ready_.notify_all();
}

bool PriorityScheduler::WaitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return idle_.wait_for(lock, timeout, [this] { return queue_.empty() && running_.empty(); });
}

SchedulerStatistics PriorityScheduler::statistics() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto result = statistics_;
  result.queued = queue_.size();
  result.running = running_.size();
  return result;
}

void PriorityScheduler::Stop() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    for (const auto& job : queue_) job->cancelled->store(true, std::memory_order_relaxed);
    for (const auto& [id, job] : running_) {
      static_cast<void>(id);
      job->cancelled->store(true, std::memory_order_relaxed);
    }
    statistics_.cancelled += queue_.size();
    queue_.clear();
    identities_.clear();
  }
  ready_.notify_all();
  for (auto& worker : workers_) if (worker.joinable()) worker.join();
  workers_.clear();
}

void PriorityScheduler::WorkerLoop() {
  while (true) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || BestRunnableLocked() != queue_.size(); });
      if (stopping_) return;
      const auto selected = BestRunnableLocked();
      job = queue_[selected];
      queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(selected));
      running_.emplace(job->id, job);
      if (Background(job->priority)) ++running_background_;
    }

    bool failed = false;
    try {
      job->work(CancellationToken{job->cancelled});
    } catch (...) {
      failed = true;
    }

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (Background(job->priority)) --running_background_;
      running_.erase(job->id);
      identities_.erase(job->identity);
      if (job->cancelled->load(std::memory_order_relaxed)) ++statistics_.cancelled;
      else if (failed) ++statistics_.failed;
      else ++statistics_.executed;
      if (queue_.empty() && running_.empty()) idle_.notify_all();
    }
    ready_.notify_all();
  }
}

}  // namespace cutline::jobs

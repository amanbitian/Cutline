#include "playback/DecodePool.h"

#include "media/Source.h"

#include <exception>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>

namespace cutline::playback {
namespace {

struct WorkerSource final {
  std::string media_id;
  std::string path;
  std::unique_ptr<media::Source> source;
};

}  // namespace

DecodePool::DecodePool(std::size_t workers, std::size_t capacity, std::size_t source_capacity,
                       std::uint64_t generation, Completion completion)
    : capacity_(capacity), source_capacity_(source_capacity), completion_(std::move(completion)), generation_(generation) {
  workers_.reserve(workers);
  for (std::size_t index = 0; index < workers; ++index) workers_.emplace_back([this] { WorkerLoop(); });
}

DecodePool::~DecodePool() { Stop(); }

std::string DecodePool::Identity(const DecodeJob& job) {
  return std::to_string(job.generation) + ":" + job.cache_key;
}

EnqueueResult DecodePool::Enqueue(DecodeJob job) {
  const auto identity = Identity(job);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return EnqueueResult::Stopped;
    if (job.generation != generation_) return EnqueueResult::Stale;
    if (pending_.count(identity) != 0) return EnqueueResult::Duplicate;
    if (queue_.size() >= capacity_) return EnqueueResult::Full;
    pending_.insert(identity);
    queue_.push_back(std::move(job));
  }
  ready_.notify_one();
  return EnqueueResult::Queued;
}

void DecodePool::AdvanceGeneration(std::uint64_t generation) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    for (const auto& job : queue_) pending_.erase(Identity(job));
    queue_.clear();
  }
  ready_.notify_all();
}

void DecodePool::Stop() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    queue_.clear();
    pending_.clear();
  }
  ready_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
}

void DecodePool::WorkerLoop() {
  std::list<WorkerSource> sources;
  std::unordered_map<std::string, std::list<WorkerSource>::iterator> source_index;
  while (true) {
    DecodeJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }

    std::optional<media::VideoFrame> frame;
    try {
      auto found = source_index.find(job.media_id);
      if (found != source_index.end() && found->second->path != job.path) {
        sources.erase(found->second);
        source_index.erase(found);
        found = source_index.end();
      }
      if (found == source_index.end()) {
        auto opened = media::SourceRegistry::Instance().Open(job.path);
        sources.push_front(WorkerSource{job.media_id, job.path, std::move(opened)});
        found = source_index.emplace(job.media_id, sources.begin()).first;
        while (sources.size() > source_capacity_) {
          source_index.erase(sources.back().media_id);
          sources.pop_back();
        }
      } else {
        sources.splice(sources.begin(), sources, found->second);
        found->second = sources.begin();
      }
      if (found->second->source != nullptr) frame = found->second->source->ReadVideo(job.source_time);
    } catch (const std::exception&) {
      // Read-ahead is opportunistic. The synchronous render path reports a
      // persistent open/decode failure if the frame is actually requested.
    }

    bool deliver = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(Identity(job));
      deliver = !stopping_ && job.generation == generation_;
    }
    if (deliver && completion_) completion_(std::move(job), std::move(frame));
  }
}

}  // namespace cutline::playback

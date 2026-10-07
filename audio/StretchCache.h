#pragma once

// A byte-bounded cache of stretched chains (see TimeStretch.h), shared by the
// mixers of one engine. A chain is a pure function of its key, so a hit is as good
// as a computation; the cache exists because a block of 1024 samples touches a chain
// of tens of thousands, and recomputing it per block would cost far more than playing
// it. Thread-safe.

#include "media/AudioBuffer.h"

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cutline::audio {

class StretchCache final {
 public:
  explicit StretchCache(std::size_t byte_budget = 96u * 1024u * 1024u) : budget_(byte_budget) {}

  [[nodiscard]] std::shared_ptr<const media::AudioBuffer> Find(const std::string& key) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = index_.find(key);
    if (found == index_.end()) return nullptr;
    entries_.splice(entries_.begin(), entries_, found->second);
    return found->second->second;
  }

  void Insert(const std::string& key, std::shared_ptr<const media::AudioBuffer> chain) {
    const auto bytes = static_cast<std::size_t>(chain->frames()) * static_cast<std::size_t>(chain->channels()) * sizeof(float);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = index_.find(key); found != index_.end()) {
      held_ -= EntryBytes(*found->second->second);
      entries_.erase(found->second);
      index_.erase(found);
    }
    entries_.emplace_front(key, std::move(chain));
    index_[key] = entries_.begin();
    held_ += bytes;
    // Always keep the newest entry, even one larger than the budget.
    while (entries_.size() > 1 && held_ > budget_) {
      held_ -= EntryBytes(*entries_.back().second);
      index_.erase(entries_.back().first);
      entries_.pop_back();
    }
  }

  void Clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    index_.clear();
    held_ = 0;
  }

  [[nodiscard]] std::size_t bytes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return held_;
  }
  [[nodiscard]] std::size_t budget() const { return budget_; }

 private:
  static std::size_t EntryBytes(const media::AudioBuffer& buffer) {
    return static_cast<std::size_t>(buffer.frames()) * static_cast<std::size_t>(buffer.channels()) * sizeof(float);
  }

  using Entry = std::pair<std::string, std::shared_ptr<const media::AudioBuffer>>;
  std::size_t budget_;
  mutable std::mutex mutex_;
  std::list<Entry> entries_;
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
  std::size_t held_{0};
};

}  // namespace cutline::audio

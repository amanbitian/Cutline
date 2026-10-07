#pragma once

// Runs the rows of a picture on every core.
//
// Almost everything the software compositor does to a picture is a pure function of one pixel (or of pixels read
// from another picture) written to its own place, so rows can be handed to any thread in any order and the result is
// bit for bit what one thread would make. That is the only kind of work this is for: a body must not write outside the
// row it was given and must not touch shared state that is not read-only.
//
// The workers are made once and kept. A call takes rows in blocks off a shared counter (the calling thread works too,
// so a call costs no more than the serial loop on a one-core machine), and returns when every block is done. If the
// pool is already busy with another call (an export and the monitor composing at the same moment), the second caller
// runs its rows itself rather than waiting: the two never slow each other beyond sharing cores.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cutline::util {

class RowPool final {
 public:
  static RowPool& Instance() {
    static RowPool pool;
    return pool;
  }

  RowPool(const RowPool&) = delete;
  RowPool& operator=(const RowPool&) = delete;

  [[nodiscard]] int workers() const { return static_cast<int>(threads_.size()); }

  // Calls body(first_block .. last_block exclusive) for disjoint ranges that together cover [0, blocks).
  void Run(int blocks, const std::function<void(int)>& body) {
    if (blocks <= 0) return;
    std::unique_lock<std::mutex> own(run_mutex_, std::try_to_lock);
    if (blocks == 1 || threads_.empty() || !own.owns_lock()) {
      for (int block = 0; block < blocks; ++block) body(block);
      return;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      body_ = &body;
      blocks_ = blocks;
      next_.store(0, std::memory_order_relaxed);
      active_ = static_cast<int>(threads_.size());
      ++generation_;
    }
    wake_.notify_all();
    Work();
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return active_ == 0; });
    body_ = nullptr;
  }

 private:
  RowPool() {
    const unsigned count = std::thread::hardware_concurrency();
    const unsigned workers = count > 1 ? std::min(count, 32u) - 1 : 0;
    for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { Loop(); });
  }

  ~RowPool() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    for (auto& thread : threads_) thread.join();
  }

  void Work() {
    for (;;) {
      const int block = next_.fetch_add(1, std::memory_order_relaxed);
      if (block >= blocks_) return;
      (*body_)(block);
    }
  }

  void Loop() {
    std::uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
      }
      Work();
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (--active_ == 0) done_.notify_one();
      }
    }
  }

  std::vector<std::thread> threads_;
  std::mutex run_mutex_;  // one call at a time owns the workers
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  const std::function<void(int)>* body_{nullptr};
  int blocks_{0};
  std::atomic<int> next_{0};
  int active_{0};
  std::uint64_t generation_{0};
  bool stop_{false};
};

// Calls row(y) for every y in [first, last], on every core when there are enough rows to be worth it.
template <typename Function>
void ParallelRows(int first, int last, Function&& row) {
  const int rows = last - first + 1;
  if (rows <= 0) return;
  // Under about forty rows the hand-off costs more than it saves.
  if (rows < 40) {
    for (int y = first; y <= last; ++y) row(y);
    return;
  }
  constexpr int kRowsPerBlock = 8;
  const int blocks = (rows + kRowsPerBlock - 1) / kRowsPerBlock;
  RowPool::Instance().Run(blocks, [&](int block) {
    const int begin = first + block * kRowsPerBlock;
    const int end = std::min(begin + kRowsPerBlock - 1, last);
    for (int y = begin; y <= end; ++y) row(y);
  });
}

}  // namespace cutline::util

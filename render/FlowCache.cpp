#include "render/FlowCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace cutline::render {
namespace {

std::atomic<std::uint64_t> g_flow_cache_ids{0};

// A fast 128-bit mix over words. It addresses a cache, it does not guard against an adversary.
class Hasher final {
 public:
  void Word(std::uint64_t word) {
    a_ = (a_ ^ word) * 0x9E3779B185EBCA87ull;
    a_ = (a_ << 31) | (a_ >> 33);
    b_ = (b_ + word) * 0xC2B2AE3D27D4EB4Full;
    b_ ^= b_ >> 29;
  }
  void Bytes(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size >= 8) {
      std::uint64_t word;
      std::memcpy(&word, bytes, 8);
      Word(word);
      bytes += 8;
      size -= 8;
    }
    if (size > 0) {
      std::uint64_t word = 0;
      std::memcpy(&word, bytes, size);
      Word(word ^ (static_cast<std::uint64_t>(size) << 56));
    }
  }
  void Float(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    Word(bits);
  }
  [[nodiscard]] std::uint64_t Fold() const { return a_ ^ (b_ * 0x9E3779B97F4A7C15ull); }
  [[nodiscard]] std::string Hex() const {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto lane : {a_ ^ (b_ >> 17), b_ ^ (a_ << 13)}) {
      auto mixed = lane;
      mixed ^= mixed >> 33;
      mixed *= 0xFF51AFD7ED558CCDull;
      mixed ^= mixed >> 33;
      for (int shift = 60; shift >= 0; shift -= 4) out.push_back(digits[(mixed >> shift) & 0xF]);
    }
    return out;
  }

 private:
  std::uint64_t a_{0x243F6A8885A308D3ull};
  std::uint64_t b_{0x13198A2E03707344ull};
};

void HashFrame(Hasher& hasher, const media::VideoFrame& frame) {
  hasher.Word(static_cast<std::uint64_t>(frame.width()));
  hasher.Word(static_cast<std::uint64_t>(frame.height()));
  const auto convert = frame.format() != media::PixelFormat::RgbaF32;
  const auto converted = convert ? media::ConvertFrame(frame, media::PixelFormat::RgbaF32) : media::VideoFrame{};
  const auto& source = convert ? converted : frame;
  for (int y = 0; y < source.height(); ++y) {
    hasher.Bytes(source.row_f32(y), static_cast<std::size_t>(source.width()) * 4 * sizeof(float));
  }
}

constexpr char kMagic[8] = {'C', 'U', 'T', 'F', 'L', 'O', 'W', '1'};

template <typename T>
void PutValue(std::vector<char>& out, T value) {
  const auto* bytes = reinterpret_cast<const char*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool TakeValue(const std::vector<char>& in, std::size_t& at, T& value) {
  if (at + sizeof(T) > in.size()) return false;
  std::memcpy(&value, in.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}

[[nodiscard]] std::size_t FieldBytes(const FlowField& field) { return sizeof(FlowField) + field.samples.size() * sizeof(FlowSample); }

[[nodiscard]] std::uint64_t Checksum(const char* data, std::size_t size) {
  Hasher hasher;
  hasher.Bytes(data, size);
  return hasher.Fold();
}

}  // namespace

FlowKey MakeFlowKey(const media::VideoFrame& first, const media::VideoFrame& second, const OpticalFlowConfig& config) {
  Hasher hasher;
  hasher.Word(static_cast<std::uint64_t>(kFlowAlgorithmVersion));
  hasher.Word(static_cast<std::uint64_t>(config.block_size));
  hasher.Word(static_cast<std::uint64_t>(config.search_radius));
  hasher.Float(config.occlusion_tolerance);
  hasher.Float(config.minimum_confidence);
  HashFrame(hasher, first);
  hasher.Word(0xF10F10F10ull);  // between the pictures, so (a, b) and (b, a) differ
  HashFrame(hasher, second);
  return hasher.Hex();
}

FlowCache::FlowCache(FlowCacheConfig config)
    : config_(std::move(config)), quota_owner_("flow-cache-" + std::to_string(++g_flow_cache_ids)) {
  if (!config_.directory.empty()) {
    std::error_code error;
    std::filesystem::create_directories(config_.directory, error);
    if (config_.quota_manager && !error) {
      for (const auto& entry : std::filesystem::directory_iterator(config_.directory, error)) {
        if (error || entry.path().extension() != ".cutflow") continue;
        const auto bytes = static_cast<std::size_t>(entry.file_size(error));
        if (error || bytes == 0) continue;
        const auto key = entry.path().stem().string();
        config_.quota_manager->Track(quota_owner_, key, resource::Tier::Disk, bytes,
                                     config_.disk_recompute_cost,
                                     [this, key] { EvictSharedDisk(key); });
      }
      config_.quota_manager->Enforce(resource::Tier::Disk);
    }
  }
}

FlowCache::~FlowCache() {
  if (config_.quota_manager) config_.quota_manager->ForgetOwner(quota_owner_);
}

std::filesystem::path FlowCache::PathFor(const FlowKey& key) const { return config_.directory / (key + ".cutflow"); }

void FlowCache::InsertLocked(const FlowKey& key, std::shared_ptr<const FlowField> field) {
  if (const auto found = index_.find(key); found != index_.end()) {
    bytes_ -= found->second->bytes;
    lru_.erase(found->second);
    index_.erase(found);
  }
  const auto bytes = FieldBytes(*field);
  lru_.push_front({key, std::move(field), bytes});
  index_[key] = lru_.begin();
  bytes_ += bytes;
  if (config_.quota_manager) {
    config_.quota_manager->Track(quota_owner_, key, resource::Tier::Ram, bytes,
                                 config_.memory_recompute_cost,
                                 [this, key] { EvictSharedMemory(key); });
  }
  // The newest entry stays even if it alone is over the budget: it is what is being asked for now.
  while (bytes_ > config_.memory_bytes && lru_.size() > 1) {
    auto& oldest = lru_.back();
    bytes_ -= oldest.bytes;
    if (config_.quota_manager) config_.quota_manager->Forget(quota_owner_, oldest.key, resource::Tier::Ram);
    index_.erase(oldest.key);
    lru_.pop_back();
    ++stats_.evictions;
  }
}

void FlowCache::WriteDisk(const FlowKey& key, const FlowField& field) {
  if (config_.directory.empty()) return;
  std::vector<char> out;
  out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
  PutValue<std::int32_t>(out, field.width);
  PutValue<std::int32_t>(out, field.height);
  PutValue<std::int32_t>(out, field.block_size);
  PutValue<std::int32_t>(out, field.columns);
  PutValue<std::int32_t>(out, field.rows);
  for (const auto& sample : field.samples) {
    PutValue<float>(out, sample.dx);
    PutValue<float>(out, sample.dy);
    PutValue<float>(out, sample.confidence);
    PutValue<std::uint8_t>(out, sample.occluded ? 1 : 0);
  }
  PutValue<std::uint64_t>(out, Checksum(out.data(), out.size()));

  const auto path = PathFor(key);
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return;
    stream.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!stream) {
      stream.close();
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return;
  }
  if (config_.quota_manager) {
    config_.quota_manager->Track(quota_owner_, key, resource::Tier::Disk, out.size(),
                                 config_.disk_recompute_cost,
                                 [this, key] { EvictSharedDisk(key); });
    config_.quota_manager->Enforce(resource::Tier::Disk);
  }
  bool trim = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.disk_writes;
    trim = ++writes_since_trim_ >= 64;
    if (trim) writes_since_trim_ = 0;
  }
  if (trim) TrimDisk();
}

std::shared_ptr<const FlowField> FlowCache::ReadDisk(const FlowKey& key) {
  if (config_.directory.empty()) return nullptr;
  const auto path = PathFor(key);
  std::vector<char> in;
  {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return nullptr;
    const auto size = static_cast<std::size_t>(stream.tellg());
    in.resize(size);
    stream.seekg(0);
    stream.read(in.data(), static_cast<std::streamsize>(size));
    if (!stream) in.clear();
  }
  const auto reject = [&]() -> std::shared_ptr<const FlowField> {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (config_.quota_manager) config_.quota_manager->Forget(quota_owner_, key, resource::Tier::Disk);
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.disk_rejected;
    return nullptr;
  };
  if (in.size() < sizeof(kMagic) + 8 || std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0) return reject();
  std::uint64_t stored = 0;
  std::memcpy(&stored, in.data() + in.size() - 8, 8);
  if (stored != Checksum(in.data(), in.size() - 8)) return reject();
  auto field = std::make_shared<FlowField>();
  std::size_t at = sizeof(kMagic);
  std::int32_t width, height, block, columns, rows;
  if (!TakeValue(in, at, width) || !TakeValue(in, at, height) || !TakeValue(in, at, block) || !TakeValue(in, at, columns) || !TakeValue(in, at, rows)) return reject();
  if (width <= 0 || height <= 0 || block <= 0 || columns <= 0 || rows <= 0) return reject();
  const auto count = static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
  constexpr std::size_t kSampleBytes = 3 * sizeof(float) + 1;
  if (at + count * kSampleBytes + 8 != in.size()) return reject();
  field->width = width;
  field->height = height;
  field->block_size = block;
  field->columns = columns;
  field->rows = rows;
  field->samples.resize(count);
  for (auto& sample : field->samples) {
    std::uint8_t occluded = 0;
    if (!TakeValue(in, at, sample.dx) || !TakeValue(in, at, sample.dy) || !TakeValue(in, at, sample.confidence) || !TakeValue(in, at, occluded)) return reject();
    sample.occluded = occluded != 0;
  }
  return field;
}

void FlowCache::TrimDisk() {
  if (config_.directory.empty() || config_.disk_bytes == 0) return;
  struct File final {
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
    std::uint64_t size;
  };
  std::vector<File> files;
  std::uint64_t total = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(config_.directory, error)) {
    if (entry.path().extension() != ".cutflow") continue;
    std::error_code file_error;
    const auto size = entry.file_size(file_error);
    const auto modified = entry.last_write_time(file_error);
    if (file_error) continue;
    files.push_back({entry.path(), modified, size});
    total += size;
  }
  if (total <= config_.disk_bytes) return;
  std::sort(files.begin(), files.end(), [](const File& l, const File& r) { return l.modified < r.modified; });
  for (const auto& file : files) {
    if (total <= config_.disk_bytes) break;
    std::error_code remove_error;
    if (std::filesystem::remove(file.path, remove_error)) {
      total -= file.size;
      if (config_.quota_manager) {
        config_.quota_manager->Forget(quota_owner_, file.path.stem().string(), resource::Tier::Disk);
      }
    }
  }
}

std::shared_ptr<const FlowField> FlowCache::Find(const FlowKey& key) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = index_.find(key); found != index_.end()) {
      lru_.splice(lru_.begin(), lru_, found->second);
      ++stats_.memory_hits;
      if (config_.quota_manager) config_.quota_manager->Touch(quota_owner_, key, resource::Tier::Ram);
      return found->second->field;
    }
  }
  auto field = ReadDisk(key);
  if (field) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.disk_hits;
      InsertLocked(key, field);
      if (config_.quota_manager) config_.quota_manager->Touch(quota_owner_, key, resource::Tier::Disk);
    }
  }
  if (field && config_.quota_manager) config_.quota_manager->Enforce(resource::Tier::Ram);
  if (field) return field;
  const std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.misses;
  return nullptr;
}

void FlowCache::Put(const FlowKey& key, std::shared_ptr<const FlowField> field) {
  if (field == nullptr || !field->valid()) return;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    InsertLocked(key, field);
  }
  if (config_.quota_manager) config_.quota_manager->Enforce(resource::Tier::Ram);
  WriteDisk(key, *field);
}

FlowLookup FlowCache::GetOrCompute(const FlowKey& key, const std::function<FlowField()>& compute) {
  for (;;) {
    std::promise<std::shared_ptr<const FlowField>> promise;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (const auto found = index_.find(key); found != index_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second);
        ++stats_.memory_hits;
        if (config_.quota_manager) config_.quota_manager->Touch(quota_owner_, key, resource::Tier::Ram);
        return {found->second->field, FlowSource::Memory};
      }
      if (const auto pending = in_flight_.find(key); pending != in_flight_.end()) {
        auto future = pending->second;
        lock.unlock();
        try {
          return {future.get(), FlowSource::Memory};
        } catch (const FlowCancelled&) {
          // Whoever was computing it was told to stop; this caller still needs the answer.
          continue;
        }
      }
      in_flight_[key] = promise.get_future().share();
    }
    const auto finish = [&]() {
      const std::lock_guard<std::mutex> lock(mutex_);
      in_flight_.erase(key);
    };
    try {
      if (auto field = ReadDisk(key)) {
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          ++stats_.disk_hits;
          InsertLocked(key, field);
        }
        if (config_.quota_manager) config_.quota_manager->Enforce(resource::Tier::Ram);
        promise.set_value(field);
        finish();
        return {field, FlowSource::Disk};
      }
      auto field = std::make_shared<const FlowField>(compute());
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.misses;
        ++stats_.computed;
        InsertLocked(key, field);
      }
      if (config_.quota_manager) config_.quota_manager->Enforce(resource::Tier::Ram);
      WriteDisk(key, *field);
      promise.set_value(field);
      finish();
      return {field, FlowSource::Computed};
    } catch (...) {
      promise.set_exception(std::current_exception());
      finish();
      throw;
    }
  }
}

FlowCacheStats FlowCache::Stats() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto stats = stats_;
  stats.entries = lru_.size();
  stats.bytes = bytes_;
  return stats;
}

void FlowCache::Clear() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
    bytes_ = 0;
  }
  if (config_.quota_manager) config_.quota_manager->ForgetOwner(quota_owner_);
  if (config_.directory.empty()) return;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(config_.directory, error)) {
    if (entry.path().extension() == ".cutflow") std::filesystem::remove(entry.path(), error);
  }
}

void FlowCache::EvictSharedMemory(const FlowKey& key) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto found = index_.find(key);
  if (found == index_.end()) return;
  bytes_ -= found->second->bytes;
  lru_.erase(found->second);
  index_.erase(found);
  ++stats_.shared_quota_evictions;
}

void FlowCache::EvictSharedDisk(const FlowKey& key) {
  std::error_code error;
  if (std::filesystem::remove(PathFor(key), error)) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.shared_quota_evictions;
  }
}

FlowLookup EstimateOpticalFlowCached(FlowCache& cache, const media::VideoFrame& first, const media::VideoFrame& second,
                                     const OpticalFlowConfig& config) {
  const auto key = MakeFlowKey(first, second, config);
  return cache.GetOrCompute(key, [&]() { return EstimateOpticalFlow(first, second, config); });
}

// ---------------------------------------------------------------- background analysis ----

FlowAnalysis::FlowAnalysis(std::shared_ptr<FlowCache> cache, Frames frames, std::size_t frame_count, OpticalFlowConfig config)
    : cache_(std::move(cache)), frames_(std::move(frames)), frame_count_(frame_count), config_(std::move(config)) {
  if (cache_ == nullptr) throw std::invalid_argument("Flow analysis needs a cache to fill");
  if (!frames_) throw std::invalid_argument("Flow analysis has no frames to read");
  progress_.total_pairs = frame_count_ > 1 ? frame_count_ - 1 : 0;
}

FlowAnalysis::~FlowAnalysis() {
  Cancel();
  if (worker_.joinable()) worker_.join();
}

void FlowAnalysis::Start() {
  if (worker_.joinable()) throw std::logic_error("Flow analysis has already started");
  worker_ = std::thread([this]() { Run(); });
}

void FlowAnalysis::Cancel() { cancel_.store(true); }

void FlowAnalysis::Wait() {
  if (worker_.joinable()) worker_.join();
}

FlowAnalysisProgress FlowAnalysis::Progress() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return progress_;
}

void FlowAnalysis::Run() {
  const auto publish = [&]() {
    FlowAnalysisProgress snapshot;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      snapshot = progress_;
    }
    if (observer_) observer_(snapshot);
  };
  const auto stopping = [&]() { return cancel_.load(); };
  auto config = config_;
  config.cancel = stopping;
  try {
    media::VideoFrame held;
    bool have_held = false;
    for (std::size_t index = 0; index < frame_count_ && !stopping(); ++index) {
      const auto* frame = frames_(index);
      const bool usable = frame != nullptr && frame->valid();
      if (index > 0) {
        bool skipped = true;
        FlowSource source = FlowSource::Computed;
        if (usable && have_held && frame->width() == held.width() && frame->height() == held.height()) {
          try {
            source = EstimateOpticalFlowCached(*cache_, held, *frame, config).source;
            skipped = false;
          } catch (const FlowCancelled&) {
            break;
          }
        }
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          ++progress_.done;
          if (skipped) ++progress_.skipped;
          else if (source == FlowSource::Computed) ++progress_.computed;
          else ++progress_.reused;
        }
        publish();
      }
      if (usable) {
        held = frame->Clone();
        have_held = true;
      } else {
        have_held = false;
      }
    }
  } catch (const std::exception& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    progress_.error = error.what();
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    progress_.cancelled = cancel_.load() && progress_.done < progress_.total_pairs;
    progress_.finished = true;
  }
  publish();
}

}  // namespace cutline::render

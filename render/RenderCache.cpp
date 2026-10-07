#include "render/RenderCache.h"

#include "core/util/Sha256.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <stdexcept>

namespace cutline::render {
namespace {

std::atomic<std::uint64_t> g_render_cache_ids{0};

// Everything that goes into an address, as an unambiguous byte string: each field is tagged and
// carries its length, so that ("ab", "c") and ("a", "bc") hash differently.
class KeyBuilder final {
 public:
  void Field(std::string_view tag, std::string_view value) {
    buffer_.append(tag);
    buffer_.push_back('=');
    buffer_.append(std::to_string(value.size()));
    buffer_.push_back(':');
    buffer_.append(value);
    buffer_.push_back(';');
  }
  void Field(std::string_view tag, std::int64_t value) { Field(tag, std::to_string(value)); }
  void Field(std::string_view tag, bool value) { Field(tag, std::string_view(value ? "1" : "0")); }
  void Field(std::string_view tag, double value) {
    // The bits, not a rounded text: two parameters that differ in the last place are different pictures.
    std::uint64_t bits;
    static_assert(sizeof bits == sizeof value);
    std::memcpy(&bits, &value, sizeof bits);
    Field(tag, std::to_string(bits));
  }
  void Field(std::string_view tag, const time::RationalTime& value) {
    Field(tag, std::to_string(value.numerator()) + "/" + std::to_string(value.denominator()));
  }
  [[nodiscard]] RenderKey Finish() const { return util::Sha256::Of(buffer_).substr(0, 32); }

 private:
  std::string buffer_;
};

void AddEffects(KeyBuilder& key, const std::vector<timeline::SampledEffect>& effects, const SourceVersions& versions) {
  key.Field("effects", static_cast<std::int64_t>(effects.size()));
  for (const auto& effect : effects) {
    key.Field("type", effect.effect_type);
    key.Field("order", effect.order);
    if (!effect.preset_name.empty()) {
      key.Field("preset", effect.preset_name);
      // An asset such as a look-up table is a file whose contents can change under the same name.
      if (versions.asset && effect.inline_asset.empty()) key.Field("asset", versions.asset(effect.preset_name));
    }
    // A graphic that lives in the project is part of the picture: a different one is a different key.
    if (!effect.inline_asset.empty()) key.Field("inline", effect.inline_asset);
    for (const auto& parameter : effect.parameters) {
      key.Field("p", parameter.name);
      key.Field("n", static_cast<std::int64_t>(parameter.value.dimension));
      for (int i = 0; i < parameter.value.dimension; ++i) key.Field("v", parameter.value.components[static_cast<std::size_t>(i)]);
    }
  }
}

constexpr std::uint32_t kMagic = 0x43524C43;  // "CLRC"
constexpr std::uint32_t kVersion = 1;

std::uint64_t Fnv1a(const std::uint8_t* data, std::size_t size, std::uint64_t seed = 1469598103934665603ull) {
  auto hash = seed;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

template <typename T>
void Put(std::vector<std::uint8_t>& out, const T& value) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}
void PutString(std::vector<std::uint8_t>& out, const std::string& text) {
  Put(out, static_cast<std::uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

class Reader final {
 public:
  Reader(const std::vector<std::uint8_t>& data) : data_(data) {}
  template <typename T>
  bool Get(T& value) {
    if (position_ + sizeof(T) > data_.size()) return false;
    std::memcpy(&value, data_.data() + position_, sizeof(T));
    position_ += sizeof(T);
    return true;
  }
  bool GetString(std::string& text) {
    std::uint32_t size = 0;
    if (!Get(size) || size > 1024 || position_ + size > data_.size()) return false;
    text.assign(reinterpret_cast<const char*>(data_.data() + position_), size);
    position_ += size;
    return true;
  }
  [[nodiscard]] const std::uint8_t* current() const { return data_.data() + position_; }
  [[nodiscard]] std::size_t remaining() const { return data_.size() - position_; }
  void Skip(std::size_t n) { position_ += n; }

 private:
  const std::vector<std::uint8_t>& data_;
  std::size_t position_{0};
};

}  // namespace

// --------------------------------------------------------------------- keys ----

RenderKey KeyForPlan(const timeline::PlaybackPlan& plan, const CompositorConfig& config, const SourceVersions& versions,
                     const std::vector<RenderKey>& nested) {
  KeyBuilder key;
  key.Field("format", static_cast<std::int64_t>(1));  // the layout of this key; a change here retires every old entry
  key.Field("width", static_cast<std::int64_t>(config.width > 0 ? config.width : plan.width));
  key.Field("height", static_cast<std::int64_t>(config.height > 0 ? config.height : plan.height));
  key.Field("output", static_cast<std::int64_t>(config.output_format));
  key.Field("renderVersion", plan.render_version);
  key.Field("working", plan.working_color_space);
  key.Field("display", plan.display_color_space);
  key.Field("bgR", static_cast<double>(config.background_red));
  key.Field("bgG", static_cast<double>(config.background_green));
  key.Field("bgB", static_cast<double>(config.background_blue));
  key.Field("assets", config.asset_root);
  key.Field("frameRate", std::to_string(plan.frame_rate.numerator) + "/" + std::to_string(plan.frame_rate.denominator));

  std::size_t next_nested = 0;
  key.Field("requests", static_cast<std::int64_t>(plan.video.size()));
  for (const auto& request : plan.video) {
    key.Field("depth", static_cast<std::int64_t>(request.depth));
    key.Field("track", request.track_order);
    key.Field("trackId", request.track_id);
    key.Field("clip", request.clip_id);
    key.Field("kind", static_cast<std::int64_t>(request.source_kind));
    key.Field("source", request.source_id);
    if (request.source_kind == model::SourceKind::Media && versions.media) key.Field("version", versions.media(request.source_id));
    key.Field("at", request.source_time);
    key.Field("rate", request.playback_rate);
    key.Field("reversed", request.reversed);
    AddEffects(key, request.effects, versions);
    if (request.source_kind == model::SourceKind::Sequence && request.depth == 0 && next_nested < nested.size()) {
      key.Field("nested", nested[next_nested++]);
    }
  }
  key.Field("transitions", static_cast<std::int64_t>(plan.transitions.size()));
  for (const auto& transition : plan.transitions) {
    key.Field("kind", transition.kind);
    key.Field("track", transition.track_id);
    key.Field("from", transition.from_clip_id.value_or(""));
    key.Field("to", transition.to_clip_id.value_or(""));
    key.Field("progress", transition.progress);
    AddEffects(key, transition.effects, versions);
  }
  AddEffects(key, plan.sequence_effects, versions);
  key.Field("captions", static_cast<std::int64_t>(plan.captions.size()));
  for (const auto& caption : plan.captions) {
    key.Field("captionTrack", caption.track_id);
    key.Field("captionText", caption.text);
    key.Field("captionStyle", caption.style_json);
  }
  return key.Finish();
}

RenderKey KeyForTime(const timeline::TimelineCompiler& compiler, const timeline::SequenceGraph& graph,
                     const timeline::Sequence& sequence, const time::RationalTime& at, const timeline::CompileOptions& options,
                     const CompositorConfig& config, const SourceVersions& versions, int depth) {
  const auto plan = compiler.Compile(sequence, at, options);
  std::vector<RenderKey> nested;
  if (depth < options.max_nesting_depth) {
    for (const auto& request : plan.video) {
      if (request.depth != 0 || request.source_kind != model::SourceKind::Sequence) continue;
      const auto* inner = graph.Find(request.source_id);
      nested.push_back(inner == nullptr ? RenderKey("missing") : KeyForTime(compiler, graph, *inner, request.source_time, options, config, versions, depth + 1));
    }
  }
  return KeyForPlan(plan, config, versions, nested);
}

// --------------------------------------------------------------------- cache ----

RenderCache::RenderCache(RenderCacheConfig config)
    : config_(std::move(config)), quota_owner_("render-cache-" + std::to_string(++g_render_cache_ids)) {
  if (config_.disk_directory.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(config_.disk_directory, error);
  if (error) {
    config_.disk_directory.clear();  // no disk tier: the cache still works in memory
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(config_.disk_directory, error)) {
    if (error) break;
    const auto& path = entry.path();
    if (path.extension() == ".tmp") {
      std::filesystem::remove(path, error);  // a write that did not finish
      continue;
    }
    if (path.extension() != ".clrc") continue;
    DiskEntry disk;
    disk.bytes = static_cast<std::size_t>(entry.file_size(error));
    disk.time = entry.last_write_time(error);
    disk_[path.stem().string()] = disk;
    stats_.disk_bytes += disk.bytes;
    if (config_.quota_manager) {
      const auto key = path.stem().string();
      config_.quota_manager->Track(quota_owner_, key, resource::Tier::Disk, disk.bytes,
                                   config_.disk_recompute_cost,
                                   [this, key] { EvictShared(key, resource::Tier::Disk); });
    }
  }
  if (config_.quota_manager) config_.quota_manager->Enforce(resource::Tier::Disk);
}

RenderCache::~RenderCache() {
  if (config_.quota_manager) config_.quota_manager->ForgetOwner(quota_owner_);
}

std::filesystem::path RenderCache::PathFor(const RenderKey& key) const { return config_.disk_directory / (key + ".clrc"); }

void RenderCache::InsertMemoryLocked(const RenderKey& key, std::shared_ptr<const media::VideoFrame> frame) {
  const auto bytes = frame->size_bytes();
  if (bytes > config_.memory_bytes) return;  // larger than the whole budget: not held
  const auto existing = memory_.find(key);
  if (existing != memory_.end()) {
    stats_.memory_bytes -= existing->second.bytes;
    recency_.erase(existing->second.position);
    memory_.erase(existing);
  }
  recency_.push_front(key);
  memory_[key] = {std::move(frame), bytes, recency_.begin()};
  stats_.memory_bytes += bytes;
  if (config_.quota_manager) {
    config_.quota_manager->Track(quota_owner_, key, resource::Tier::Ram, bytes, config_.memory_recompute_cost,
                                 [this, key] { EvictShared(key, resource::Tier::Ram); });
  }
  EvictMemoryLocked();
  stats_.entries = memory_.size();
}

void RenderCache::EvictMemoryLocked() {
  while (stats_.memory_bytes > config_.memory_bytes && !recency_.empty()) {
    const auto& oldest = recency_.back();
    const auto found = memory_.find(oldest);
    stats_.memory_bytes -= found->second.bytes;
    if (config_.quota_manager) config_.quota_manager->Forget(quota_owner_, oldest, resource::Tier::Ram);
    memory_.erase(found);
    recency_.pop_back();
    ++stats_.memory_evictions;
  }
}

void RenderCache::EvictDiskLocked() {
  while (stats_.disk_bytes > config_.disk_bytes && !disk_.empty()) {
    auto oldest = disk_.begin();
    for (auto it = disk_.begin(); it != disk_.end(); ++it) {
      if (it->second.time < oldest->second.time) oldest = it;
    }
    std::error_code error;
    std::filesystem::remove(PathFor(oldest->first), error);
    stats_.disk_bytes -= oldest->second.bytes;
    if (config_.quota_manager) config_.quota_manager->Forget(quota_owner_, oldest->first, resource::Tier::Disk);
    disk_.erase(oldest);
    ++stats_.disk_evictions;
  }
}

void RenderCache::WriteDiskLocked(const RenderKey& key, const media::VideoFrame& frame) {
  std::vector<std::uint8_t> out;
  Put(out, kMagic);
  Put(out, kVersion);
  PutString(out, key);
  Put(out, static_cast<std::int32_t>(frame.width()));
  Put(out, static_cast<std::int32_t>(frame.height()));
  Put(out, static_cast<std::int32_t>(frame.format()));
  Put(out, frame.presentation_time.numerator());
  Put(out, frame.presentation_time.denominator());
  PutString(out, frame.color.primaries);
  PutString(out, frame.color.transfer);
  PutString(out, frame.color.matrix);
  Put(out, static_cast<std::int32_t>(frame.color.range));
  const auto row_bytes = static_cast<std::size_t>(frame.width()) * media::BytesPerPixel(frame.format());
  Put(out, static_cast<std::uint64_t>(row_bytes * static_cast<std::size_t>(frame.height())));
  for (int y = 0; y < frame.height(); ++y) {
    const auto* row = reinterpret_cast<const std::uint8_t*>(frame.row(y));
    out.insert(out.end(), row, row + row_bytes);
  }
  Put(out, Fnv1a(out.data(), out.size()));

  const auto final_path = PathFor(key);
  auto temporary = final_path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
      file.close();
      std::error_code ignore;
      std::filesystem::remove(temporary, ignore);
      return;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, final_path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return;
  }
  const auto previous = disk_.find(key);
  if (previous != disk_.end()) stats_.disk_bytes -= previous->second.bytes;
  disk_[key] = {out.size(), std::filesystem::last_write_time(final_path, error)};
  stats_.disk_bytes += out.size();
  if (config_.quota_manager) {
    config_.quota_manager->Track(quota_owner_, key, resource::Tier::Disk, out.size(), config_.disk_recompute_cost,
                                 [this, key] { EvictShared(key, resource::Tier::Disk); });
  }
  ++stats_.disk_writes;
  EvictDiskLocked();
}

std::shared_ptr<const media::VideoFrame> RenderCache::ReadDiskLocked(const RenderKey& key) {
  const auto known = disk_.find(key);
  if (known == disk_.end()) return nullptr;
  const auto discard = [&]() {
    std::error_code error;
    std::filesystem::remove(PathFor(key), error);
    stats_.disk_bytes -= known->second.bytes;
    if (config_.quota_manager) config_.quota_manager->Forget(quota_owner_, key, resource::Tier::Disk);
    disk_.erase(known);
    ++stats_.corrupt_files;
    return std::shared_ptr<const media::VideoFrame>{};
  };
  std::ifstream file(PathFor(key), std::ios::binary);
  if (!file) return discard();
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  file.close();  // a file cannot be removed while it is open, and a damaged one is about to be
  if (data.size() < sizeof(std::uint64_t)) return discard();
  std::uint64_t stored_checksum;
  std::memcpy(&stored_checksum, data.data() + data.size() - sizeof stored_checksum, sizeof stored_checksum);
  if (Fnv1a(data.data(), data.size() - sizeof stored_checksum) != stored_checksum) return discard();

  Reader reader(data);
  std::uint32_t magic = 0, version = 0;
  std::string stored_key, primaries, transfer, matrix;
  std::int32_t width = 0, height = 0, format = 0, range = 0;
  std::int64_t pts_n = 0, pts_d = 1;
  std::uint64_t payload = 0;
  if (!reader.Get(magic) || !reader.Get(version) || magic != kMagic || version != kVersion || !reader.GetString(stored_key) ||
      stored_key != key || !reader.Get(width) || !reader.Get(height) || !reader.Get(format) || !reader.Get(pts_n) || !reader.Get(pts_d) ||
      !reader.GetString(primaries) || !reader.GetString(transfer) || !reader.GetString(matrix) || !reader.Get(range) || !reader.Get(payload)) {
    return discard();
  }
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || pts_d <= 0 || format < 0 || format > 2) return discard();
  const auto pixel_format = static_cast<media::PixelFormat>(format);
  const auto row_bytes = static_cast<std::size_t>(width) * media::BytesPerPixel(pixel_format);
  if (payload != row_bytes * static_cast<std::size_t>(height) || reader.remaining() != payload + sizeof(std::uint64_t)) return discard();

  auto frame = std::make_shared<media::VideoFrame>(media::VideoFrame::Allocate(pixel_format, width, height));
  for (int y = 0; y < height; ++y) {
    std::memcpy(frame->row(y), reader.current(), row_bytes);
    reader.Skip(row_bytes);
  }
  frame->presentation_time = time::RationalTime(pts_n, pts_d);
  frame->color.primaries = primaries;
  frame->color.transfer = transfer;
  frame->color.matrix = matrix;
  frame->color.range = static_cast<model::ColorRange>(range);
  return frame;
}

std::shared_ptr<const media::VideoFrame> RenderCache::Find(const RenderKey& key) {
  std::shared_ptr<const media::VideoFrame> result;
  bool promoted = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = memory_.find(key);
    if (found != memory_.end()) {
      recency_.splice(recency_.begin(), recency_, found->second.position);
      ++stats_.hits;
      result = found->second.frame;
    } else if (!config_.disk_directory.empty()) {
      if (auto frame = ReadDiskLocked(key)) {
        InsertMemoryLocked(key, frame);
        ++stats_.hits;
        ++stats_.disk_hits;
        result = std::move(frame);
        promoted = true;
      }
    }
    if (!result) ++stats_.misses;
  }
  if (result && config_.quota_manager) {
    config_.quota_manager->Touch(quota_owner_, key, resource::Tier::Ram);
    if (promoted) config_.quota_manager->Enforce(resource::Tier::Ram);
  }
  return result;
}

bool RenderCache::Contains(const RenderKey& key) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return memory_.count(key) != 0 || disk_.count(key) != 0;
}

void RenderCache::Store(const RenderKey& key, const media::VideoFrame& frame) {
  if (!frame.valid()) return;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.stores;
    InsertMemoryLocked(key, std::make_shared<media::VideoFrame>(frame.Clone()));
    if (!config_.disk_directory.empty() && disk_.count(key) == 0) WriteDiskLocked(key, frame);
  }
  if (config_.quota_manager) {
    config_.quota_manager->Enforce(resource::Tier::Ram);
    config_.quota_manager->Enforce(resource::Tier::Disk);
  }
}

void RenderCache::Clear() {
  const std::lock_guard<std::mutex> lock(mutex_);
  memory_.clear();
  recency_.clear();
  stats_.memory_bytes = 0;
  stats_.entries = 0;
  for (const auto& [key, entry] : disk_) {
    std::error_code error;
    std::filesystem::remove(PathFor(key), error);
  }
  disk_.clear();
  stats_.disk_bytes = 0;
  if (config_.quota_manager) config_.quota_manager->ForgetOwner(quota_owner_);
}

void RenderCache::EvictShared(const RenderKey& key, resource::Tier tier) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (tier == resource::Tier::Ram) {
    const auto found = memory_.find(key);
    if (found == memory_.end()) return;
    stats_.memory_bytes -= found->second.bytes;
    recency_.erase(found->second.position);
    memory_.erase(found);
    stats_.entries = memory_.size();
  } else if (tier == resource::Tier::Disk) {
    const auto found = disk_.find(key);
    if (found == disk_.end()) return;
    std::error_code error;
    std::filesystem::remove(PathFor(key), error);
    stats_.disk_bytes -= found->second.bytes;
    disk_.erase(found);
  } else {
    return;
  }
  ++stats_.shared_quota_evictions;
}

RenderCacheStatistics RenderCache::statistics() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto copy = stats_;
  copy.entries = memory_.size();
  return copy;
}

std::vector<bool> CachedFrames(const RenderCache& cache, const timeline::SequenceGraph& graph, const time::RationalTime& first,
                               std::int64_t count, const timeline::CompileOptions& options, const CompositorConfig& config,
                               const SourceVersions& versions) {
  std::vector<bool> out;
  const auto* root = graph.root();
  if (root == nullptr || count <= 0) return out;
  const timeline::TimelineCompiler compiler;
  const auto start_frame = first.ToFrames(root->frame_rate, time::RoundingMode::Nearest);
  for (std::int64_t i = 0; i < count; ++i) {
    const auto at = time::RationalTime::FromFrames(start_frame + i, root->frame_rate);
    out.push_back(cache.Contains(KeyForTime(compiler, graph, *root, at, options, config, versions)));
  }
  return out;
}

}  // namespace cutline::render

#include "media/IngestWorkflow.h"

#include "core/util/Sha256.h"

#include <algorithm>
#include <fstream>
#include <string_view>
#include <set>
#include <system_error>

namespace cutline::media {
namespace fs = std::filesystem;
namespace {

constexpr std::size_t kBlock = 4u << 20;

std::string Key(const fs::path& path) {
  std::error_code ignored;
  auto canonical = fs::weakly_canonical(path, ignored);
  auto text = (ignored ? path : canonical).generic_string();
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

}  // namespace

IngestPlan PlanIngest(const std::vector<fs::path>& sources, const IngestPlanOptions& options) {
  IngestPlan plan;
  std::error_code error;
  const bool copying = options.copy;
  if (copying && options.destination_folder.empty()) {
    plan.problem = "Choose a folder to copy the media into";
    return plan;
  }
  std::set<std::string> taken;   // destination names claimed in this plan
  const auto folder_key = copying ? Key(options.destination_folder) : std::string();
  for (const auto& source : sources) {
    IngestPlanItem item;
    item.source = source;
    item.copy = copying;
    if (!fs::is_regular_file(source, error)) {
      item.problem = "The file is not there";
      plan.items.push_back(std::move(item));
      continue;
    }
    item.bytes = fs::file_size(source, error);
    if (!copying) {
      item.destination = source;
      plan.items.push_back(std::move(item));
      continue;
    }
    // Already in the destination folder: nothing to copy.
    if (Key(source.parent_path()) == folder_key) {
      item.copy = false;
      item.destination = source;
      plan.items.push_back(std::move(item));
      continue;
    }
    const auto stem = source.stem().string(), extension = source.extension().string();
    auto candidate = options.destination_folder / source.filename();
    for (int n = 1; fs::exists(candidate, error) || taken.count(Key(candidate)) != 0; ++n) {
      candidate = options.destination_folder / (stem + "-" + std::to_string(n) + extension);
    }
    taken.insert(Key(candidate));
    item.destination = candidate;
    plan.bytes_to_copy += item.bytes;
    plan.items.push_back(std::move(item));
  }
  if (copying && plan.bytes_to_copy > 0) {
    // The nearest folder that exists says how much room there is.
    auto probe = options.destination_folder;
    while (!probe.empty() && !fs::exists(probe, error)) probe = probe.parent_path();
    const auto info = fs::space(probe.empty() ? fs::current_path() : probe, error);
    if (!error) {
      plan.free_bytes = info.available;
      plan.enough_space = info.available >= plan.bytes_to_copy + options.reserve_bytes;
      if (!plan.enough_space) plan.problem = "There is not enough room in the destination for these files";
    }
  }
  return plan;
}

CopyResult CopyVerified(const fs::path& source, const fs::path& destination, const CopyOptions& options) {
  CopyResult result;
  std::error_code error;
  if (!fs::is_regular_file(source, error)) {
    result.error = "The file to copy is not there";
    return result;
  }
  const auto total = fs::file_size(source, error);
  if (destination.has_parent_path()) fs::create_directories(destination.parent_path(), error);
  if (fs::exists(destination, error)) {
    result.error = "A file with that name is already in the destination";
    return result;
  }
  const auto temporary = fs::path(destination.string() + ".part");
  const auto discard = [&] { fs::remove(temporary, error); };
  {
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
      result.error = !in ? "The source could not be opened" : "The destination could not be written";
      discard();
      return result;
    }
    util::Sha256 hash;
    std::vector<char> buffer(kBlock);
    std::uint64_t done = 0;
    while (in) {
      if (options.cancelled && options.cancelled()) {
        out.close();
        discard();
        result.cancelled = true;
        return result;
      }
      in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto got = in.gcount();
      if (got <= 0) break;
      hash.Update(std::string_view(buffer.data(), static_cast<std::size_t>(got)));
      out.write(buffer.data(), got);
      if (!out) {
        result.error = "The destination could not be written (is the disk full?)";
        out.close();
        discard();
        return result;
      }
      done += static_cast<std::uint64_t>(got);
      if (options.progress) options.progress(done, total);
    }
    out.flush();
    if (!out || done != total) {
      result.error = done != total ? "The source ended before its size said it would" : "The destination could not be written";
      out.close();
      discard();
      return result;
    }
    result.bytes = done;
    result.source_sha256 = hash.HexDigest();
  }
  if (options.after_write) options.after_write(temporary);
  if (options.verify) {
    std::ifstream back(temporary, std::ios::binary);
    util::Sha256 hash;
    std::vector<char> buffer(kBlock);
    std::uint64_t seen = 0;
    while (back) {
      if (options.cancelled && options.cancelled()) {
        back.close();
        discard();
        result.cancelled = true;
        return result;
      }
      back.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto got = back.gcount();
      if (got <= 0) break;
      hash.Update(std::string_view(buffer.data(), static_cast<std::size_t>(got)));
      seen += static_cast<std::uint64_t>(got);
    }
    back.close();
    result.copy_sha256 = hash.HexDigest();
    if (seen != result.bytes || result.copy_sha256 != result.source_sha256) {
      result.error = "The copy does not match the original (checked by reading it back); it was deleted";
      discard();
      return result;
    }
    result.verified = true;
  }
  fs::rename(temporary, destination, error);
  if (error) {
    result.error = "The copy could not be put in place: " + error.message();
    discard();
    return result;
  }
  result.ok = true;
  return result;
}

std::optional<ProxyPreset> ProxyPresetFor(const std::string& choice, const ProxyComplexity& media) {
  if (choice.empty() || choice == "none") return std::nullopt;
  ProxyPreset preset;
  if (choice == "auto") {
    if (media.width <= 0 || media.height <= 0 || media.frames_per_second <= 0.0) return std::nullopt;
    auto decision = ChooseProxyPreset(media);
    if (!decision.generate) return std::nullopt;
    return decision.preset;
  }
  if (choice == "1080") {
    preset.max_width = 1920;
    preset.max_height = 1080;
    preset.video_bitrate = 60'000'000;
  } else if (choice == "720") {
    preset.max_width = 1280;
    preset.max_height = 720;
    preset.video_bitrate = 30'000'000;
  } else if (choice == "540") {
    preset.max_width = 960;
    preset.max_height = 540;
    preset.video_bitrate = 18'000'000;
  } else {
    return std::nullopt;
  }
  return preset;
}

fs::path ProxyPathFor(const fs::path& proxy_folder, const std::string& media_id) {
  std::string safe = media_id;
  for (auto& c : safe) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.')) c = '_';
  }
  return proxy_folder / (safe + ".mov");
}

}  // namespace cutline::media

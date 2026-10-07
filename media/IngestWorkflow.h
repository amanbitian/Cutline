#pragma once

// Bringing media into a project from somewhere else: planning where each file goes, copying it exactly, and choosing the
// proxy that goes with it.
//
// A copy is made so that the project does not depend on a card, a drive that goes away or a folder someone tidies. It is
// only a copy if it is the same file, so this does what a careful assistant editor does by hand: it hashes the source while
// it reads it, writes to a temporary name beside the destination, reads the written file back and hashes that, and only
// when the two agree renames it into place. A file that does not come back the same is deleted and reported, never left
// looking like a copy; a cancelled copy leaves nothing. Nothing is sent anywhere, and the original is never modified.

#include "media/ProxyWorkflow.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cutline::media {

struct IngestPlanItem final {
  std::filesystem::path source;
  std::filesystem::path destination;   // where the copy will be; the source itself when nothing is copied
  std::uint64_t bytes{0};
  bool copy{true};
  std::string problem;                 // why it cannot be ingested, empty when it can
};

struct IngestPlanOptions final {
  bool copy{true};
  std::filesystem::path destination_folder;
  // Space to leave free on the destination after the copies, in bytes.
  std::uint64_t reserve_bytes{256ull << 20};
};

struct IngestPlan final {
  std::vector<IngestPlanItem> items;
  std::uint64_t bytes_to_copy{0};
  std::uint64_t free_bytes{0};
  bool enough_space{true};
  std::string problem;                 // about the plan as a whole (no room, no destination)
  [[nodiscard]] bool ok() const { return problem.empty(); }
};

// Works out where each file goes. Files in the destination already are not copied onto themselves; two sources with the same
// name get "name-1.ext" and so on, and a name already taken in the folder is never overwritten.
[[nodiscard]] IngestPlan PlanIngest(const std::vector<std::filesystem::path>& sources, const IngestPlanOptions& options);

struct CopyOptions final {
  bool verify{true};
  std::function<bool()> cancelled;
  std::function<void(std::uint64_t done, std::uint64_t total)> progress;
  // For tests: runs after the copy is written and before it is read back.
  std::function<void(const std::filesystem::path& written)> after_write;
};

struct CopyResult final {
  bool ok{false};
  bool cancelled{false};
  std::uint64_t bytes{0};
  std::string source_sha256;
  std::string copy_sha256;   // empty when not verified
  bool verified{false};
  std::string error;
};

// Copies `source` to `destination` (whose folder is made) as above. With `verify` off it is a plain streaming copy and
// the source hash is still taken, for the record.
[[nodiscard]] CopyResult CopyVerified(const std::filesystem::path& source, const std::filesystem::path& destination, const CopyOptions& options = {});

// The proxy a person asked for. "auto" follows the offline policy on the media's own complexity (nothing when the original
// plays well); "1080", "720" and "540" are the three sizes; "none" is no proxy.
[[nodiscard]] std::optional<ProxyPreset> ProxyPresetFor(const std::string& choice, const ProxyComplexity& media);

// Where a proxy of this media goes, beside the project: <folder>/<media id>.mov.
[[nodiscard]] std::filesystem::path ProxyPathFor(const std::filesystem::path& proxy_folder, const std::string& media_id);

}  // namespace cutline::media

#pragma once

// Collecting a project: copying everything it needs into one folder, proving the copies are exact,
// and leaving a project that opens from that folder and nowhere else.
//
// What it does, in order. It works out which media items the chosen sequences use (following nested
// sequences), copies each file into the destination while hashing it, reads the copy back and hashes
// that too, and calls the copy good only when the two agree; a file that cannot be read, or that
// does not come back the same, is deleted from the destination and reported, never left looking
// like a copy. It writes a manifest naming each item, where its copy is (relative to the
// destination, so the folder can be moved) and its SHA-256. It copies the project package beside
// them and, in that copy only, relinks every item to its copy. The original project and media are
// not touched.
//
// What it does not do. Trimming media to the part used plus handles, and transcoding to another
// format, need an encoder; this library has none of its own. A caller that has one passes it as
// `transcode`, and it is used in place of a plain copy for the items it accepts. Without one, those
// are not done and the report says so, rather than quietly copying more than was asked for.

#include "core/project/ProjectStore.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cutline::project {

struct MediaUse final {
  std::string id;
  std::string name;
  std::filesystem::path source;
  std::string fingerprint;
  time::RationalTime duration;
  // How much of the media the chosen sequences use, as the sum of their source ranges (a range used
  // twice counts twice); zero for media nothing uses.
  time::RationalTime used;
  bool used_by_selection{false};
};

struct ConsolidateOptions final {
  std::filesystem::path destination;
  // Empty: every sequence.
  std::vector<std::string> sequence_ids;
  // Also copy media that no chosen sequence uses.
  bool include_unused_media{false};
  std::string media_directory{"media"};
  // Also copy the project package and relink the copy.
  bool copy_package{true};
  // Replaces a plain copy for an item: writes the item's replacement at the given path and returns
  // true, or returns false with a reason. Used for transcoding and trimming.
  std::function<bool(const MediaUse& media, const std::filesystem::path& destination, std::string& reason)> transcode;
  std::function<bool()> cancel;
  std::function<void(std::size_t done, std::size_t total)> progress;
};

struct ConsolidatedMedia final {
  std::string id;
  std::string name;
  std::filesystem::path source;
  // Relative to the destination.
  std::filesystem::path copy;
  std::uint64_t bytes{0};
  std::string sha256;
  bool verified{false};
  bool transcoded{false};
  bool used{false};
  std::string status;  // "copied", "already there", or what went wrong
};

struct ConsolidationReport final {
  std::vector<ConsolidatedMedia> media;
  std::vector<std::string> missing;       // media whose file could not be found
  std::vector<std::string> failed;        // media that could not be copied exactly
  std::vector<std::string> not_copied;    // unused media left out
  std::vector<std::string> notes;
  std::uint64_t bytes_copied{0};
  bool cancelled{false};
  std::filesystem::path manifest;
  std::filesystem::path package;

  [[nodiscard]] bool complete() const { return !cancelled && missing.empty() && failed.empty(); }
  [[nodiscard]] std::string ToText() const;
};

[[nodiscard]] std::vector<MediaUse> PlanConsolidation(const ProjectStore& store, const std::vector<std::string>& sequence_ids = {});

[[nodiscard]] ConsolidationReport Consolidate(ProjectStore& store, const ConsolidateOptions& options);

// SHA-256 of a file, as lowercase hex; empty when it cannot be read.
[[nodiscard]] std::string HashFile(const std::filesystem::path& path);

struct ManifestCheck final {
  std::vector<std::string> mismatched;  // ids whose file differs from the manifest
  std::vector<std::string> missing;     // ids whose file is not there
  int checked{0};
  [[nodiscard]] bool ok() const { return mismatched.empty() && missing.empty(); }
};

// Reads a manifest and checks every file it names under `root` against its recorded SHA-256.
[[nodiscard]] ManifestCheck VerifyManifest(const std::filesystem::path& manifest, const std::filesystem::path& root);

// Points every media item of an open project at its copy under `root`, per the manifest: how a collected
// folder that has been moved or copied elsewhere is made to open again. Returns how many were relinked.
int RelinkFromManifest(ProjectStore& store, const std::filesystem::path& manifest, const std::filesystem::path& root);

}  // namespace cutline::project

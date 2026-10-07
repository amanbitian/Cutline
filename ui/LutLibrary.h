#pragma once

// The look-up tables a person can pick from: .cube files found in the folders they have named (the project's own,
// the application's, their own), checked by reading them, searchable by name, and previewable on a test chart so a
// look can be judged before it is applied.

#include "media/VideoFrame.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cutline::ui {

struct LutEntry final {
  std::filesystem::path path;
  std::string name;    // the file name without its extension
  std::string folder;  // the folder it was found in, as given
  int size{0};         // lattice points per side of a cube, or rows of a 1D table
  bool curves{false};  // a 1D table (three curves) rather than a cube
  std::string error;   // why it cannot be used, when it cannot
  std::uintmax_t bytes{0};
};

class LutLibrary final {
 public:
  void SetFolders(std::vector<std::filesystem::path> folders) { folders_ = std::move(folders); }
  [[nodiscard]] const std::vector<std::filesystem::path>& folders() const { return folders_; }
  // Looks through the folders (and the folders inside them) and reads each .cube file to be sure it is one. Entries
  // are sorted by name; a file that does not parse is listed with the reason.
  void Rescan();
  [[nodiscard]] const std::vector<LutEntry>& entries() const { return entries_; }
  // Entries whose name contains every word of the query, case-insensitive; unusable entries are left out unless
  // `include_broken`.
  [[nodiscard]] std::vector<const LutEntry*> Search(const std::string& query, bool include_broken = false) const;
  [[nodiscard]] const LutEntry* Find(const std::filesystem::path& path) const;

 private:
  std::vector<std::filesystem::path> folders_;
  std::vector<LutEntry> entries_;
};

// The reference to store on an effect: relative to `asset_root` when the file is inside it (so the project can move),
// otherwise the full path.
[[nodiscard]] std::string AssetReferenceFor(const std::filesystem::path& file, const std::filesystem::path& asset_root);

// A test chart as a picture: a grey ramp, a saturated hue ramp, a lightened one, and skin, foliage, sky, shadow,
// highlight and mid-grey patches.
[[nodiscard]] media::VideoFrame TestChart(int width, int height);
// The picture through the look-up table at `strength` 0..1. Throws if the file cannot be read.
[[nodiscard]] media::VideoFrame ApplyLutTo(const media::VideoFrame& picture, const std::filesystem::path& lut, float strength = 1.0f);

}  // namespace cutline::ui

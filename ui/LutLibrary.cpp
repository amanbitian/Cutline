#include "ui/LutLibrary.h"

#include "render/CubeLut.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <system_error>

namespace cutline::ui {
namespace {

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

void SetPixel(media::VideoFrame& frame, int x, int y, float r, float g, float b) {
  auto* texel = frame.row_f32(y) + static_cast<std::size_t>(x) * 4;
  texel[0] = r;
  texel[1] = g;
  texel[2] = b;
  texel[3] = 1.0f;
}

void Hue(float h, float& r, float& g, float& b) {
  const float x = 1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f);
  const int sector = static_cast<int>(h * 6.0f) % 6;
  const float table[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
  r = table[sector][0];
  g = table[sector][1];
  b = table[sector][2];
}

}  // namespace

void LutLibrary::Rescan() {
  entries_.clear();
  std::set<std::filesystem::path> seen;
  for (const auto& folder : folders_) {
    std::error_code folder_error;
    if (!std::filesystem::is_directory(folder, folder_error)) continue;
    for (std::filesystem::recursive_directory_iterator it(
             folder, std::filesystem::directory_options::skip_permission_denied, folder_error),
         end;
         it != end;) {
      const auto path = it->path();
      std::error_code increment_error;
      it.increment(increment_error);
      std::error_code entry_error;
      if (!std::filesystem::is_regular_file(path, entry_error) || Lower(path.extension().string()) != ".cube") continue;
      auto canonical = std::filesystem::weakly_canonical(path, entry_error);
      if (entry_error) {
        entry_error.clear();
        canonical = std::filesystem::absolute(path, entry_error);
      }
      if (entry_error) canonical = path.lexically_normal();
      if (!seen.insert(canonical).second) continue;
      LutEntry entry;
      entry.path = path;
      entry.name = path.stem().string();
      entry.folder = folder.string();
      entry.bytes = std::filesystem::file_size(path, entry_error);
      if (entry_error) entry.bytes = 0;
      try {
        const auto lut = render::CubeLut::Load(path);
        entry.size = lut.size();
        entry.curves = lut.kind() == render::CubeLut::Kind::Curves1D;
      } catch (const std::exception& e) {
        entry.error = e.what();
      }
      entries_.push_back(std::move(entry));
    }
  }
  std::sort(entries_.begin(), entries_.end(), [](const LutEntry& a, const LutEntry& b) {
    const auto la = Lower(a.name), lb = Lower(b.name);
    return la != lb ? la < lb : a.path < b.path;
  });
}

std::vector<const LutEntry*> LutLibrary::Search(const std::string& query, bool include_broken) const {
  std::vector<std::string> words;
  std::istringstream stream(Lower(query));
  std::string word;
  while (stream >> word) words.push_back(word);
  std::vector<const LutEntry*> found;
  for (const auto& entry : entries_) {
    if (!entry.error.empty() && !include_broken) continue;
    const auto name = Lower(entry.name);
    if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return name.find(w) != std::string::npos; })) found.push_back(&entry);
  }
  return found;
}

const LutEntry* LutLibrary::Find(const std::filesystem::path& path) const {
  for (const auto& entry : entries_) {
    if (entry.path == path) return &entry;
  }
  return nullptr;
}

std::string AssetReferenceFor(const std::filesystem::path& file, const std::filesystem::path& asset_root) {
  if (!asset_root.empty()) {
    // This reference is also created before a LUT is copied into a project's
    // asset directory.  std::filesystem::relative() may fail for that
    // non-existent destination on Windows because it resolves both paths via
    // the filesystem.  A lexical comparison is sufficient here: both paths
    // come from the same asset-selection operation and no filesystem lookup is
    // needed to decide whether the destination is below the asset root.
    const auto relative = file.lexically_normal().lexically_relative(asset_root.lexically_normal());
    if (!relative.empty() && *relative.begin() != "..") return relative.generic_string();
  }
  return file.generic_string();
}

media::VideoFrame TestChart(int width, int height) {
  auto frame = media::VideoFrame::Allocate(media::PixelFormat::RgbaF32, std::max(width, 8), std::max(height, 8));
  const int w = frame.width(), h = frame.height();
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float u = static_cast<float>(x) / static_cast<float>(w - 1);
      float r, g, b;
      if (y < h / 4) {
        r = g = b = u;  // grey ramp
      } else if (y < h / 2) {
        Hue(std::min(u, 0.999f), r, g, b);  // saturated hues
      } else if (y < h * 3 / 4) {
        // The hues lightened, to show what a look does to pastels.
        Hue(std::min(u, 0.999f), r, g, b);
        const float mix = 0.55f;
        r = r * (1.0f - mix) + mix;
        g = g * (1.0f - mix) + mix;
        b = b * (1.0f - mix) + mix;
      } else {
        static const float patches[6][3] = {{0.78f, 0.57f, 0.47f}, {0.30f, 0.45f, 0.20f}, {0.35f, 0.50f, 0.75f},
                                            {0.06f, 0.06f, 0.08f}, {0.92f, 0.92f, 0.90f}, {0.5f, 0.5f, 0.5f}};
        const auto& p = patches[std::min(5, static_cast<int>(u * 6.0f))];
        r = p[0];
        g = p[1];
        b = p[2];
      }
      SetPixel(frame, x, y, r, g, b);
    }
  }
  return frame;
}

media::VideoFrame ApplyLutTo(const media::VideoFrame& picture, const std::filesystem::path& lut_path, float strength) {
  const auto lut = render::CubeLut::Load(lut_path);
  auto out = picture.format() == media::PixelFormat::RgbaF32 ? picture.Clone() : media::ConvertFrame(picture, media::PixelFormat::RgbaF32);
  const float k = std::clamp(strength, 0.0f, 1.0f);
  for (int y = 0; y < out.height(); ++y) {
    auto* row = out.row_f32(y);
    for (int x = 0; x < out.width(); ++x) {
      auto* texel = row + static_cast<std::size_t>(x) * 4;
      const auto graded = lut.Sample(texel[0], texel[1], texel[2]);
      for (int c = 0; c < 3; ++c) texel[c] = texel[c] * (1.0f - k) + graded[static_cast<std::size_t>(c)] * k;
    }
  }
  return out;
}

}  // namespace cutline::ui

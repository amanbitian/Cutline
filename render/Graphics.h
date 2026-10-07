#pragma once

// Drawing title and graphic documents (effects/GraphicsDocument.h) into a layer. Coordinates and font
// sizes are normalised to the document, so one package renders at any sequence size.

#include "effects/GraphicsDocument.h"
#include "media/VideoFrame.h"
#include "render/Layer.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cutline::render::graphics {

using ImageResolver = std::function<const media::VideoFrame*(const std::string& asset)>;

// The pictures image elements show, decoded from files (any still the media layer opens) the first time they are
// needed and kept; a file that changes on disk is read again. An asset is a path, absolute or relative to the root.
class StillCache final {
 public:
  explicit StillCache(std::string root = {}) : root_(std::move(root)) {}
  // Null when the file is missing or cannot be decoded.
  [[nodiscard]] const media::VideoFrame* Find(const std::string& asset);
  [[nodiscard]] ImageResolver Resolver() {
    return [this](const std::string& asset) { return Find(asset); };
  }

 private:
  struct Entry final {
    std::filesystem::file_time_type modified{};
    std::unique_ptr<media::VideoFrame> frame;
  };
  std::string root_;
  std::map<std::string, Entry> entries_;
};
struct DrawResult final {
  int elements_drawn{0};
  std::vector<std::string> warnings;
};

// Draws the document as it is `seconds` into the clip showing it (animated properties are evaluated
// then). A font that is not installed is reported in the warnings, together with the one used instead.
[[nodiscard]] DrawResult Draw(Layer& layer, const Document& document, const ImageResolver& images = {}, double seconds = 0.0);

// The families the document names that no installed font answers to: for each text element, the font
// list is satisfied by any one of its families, so an element is missing only when none of them is installed.
[[nodiscard]] std::vector<std::string> MissingFonts(const Document& document);

// The family a font list resolves to on this machine: its first installed family, else its first entry.
[[nodiscard]] std::string ResolveFontFamily(const std::string& font_list);

}  // namespace cutline::render::graphics

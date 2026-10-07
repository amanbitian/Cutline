#pragma once

// Text as a picture: a string, a typeface and a size in, a bitmap of coverage out.
//
// This is the one place the project turns characters into pixels, for captions burned into a
// picture and for titles alike, so that a caption looks the same in the monitor and in the export:
// both call this. The rasteriser is the operating system's: on Windows the GDI text engine, which
// knows the installed fonts, shapes text correctly for the scripts it supports, breaks lines at
// word boundaries and anti-aliases. Elsewhere there is no rasteriser (Available() is false) and
// text draws as nothing, with the caller told, rather than as a wrong approximation.
//
// What is and is not reproducible. The result depends on the typeface installed on the machine; a
// font that is not installed is replaced by the system's choice and the bitmap says so
// (`substituted`), so a project that names a font its machine lacks can say so instead of quietly
// looking different. Within one machine and one font the output is deterministic.

#include <string>
#include <vector>

namespace cutline::render::text {

enum class Align { Left, Center, Right };

struct Style final {
  std::string family{"Arial"};
  // Height of the em in pixels.
  double size{48.0};
  bool bold{false};
  bool italic{false};
  Align align{Align::Center};
  // Extra space between letters, in pixels.
  double letter_spacing{0.0};
};

struct Bitmap final {
  int width{0};
  int height{0};
  // Coverage 0..1, row by row; empty when nothing was drawn.
  std::vector<float> coverage;
  // The family actually used, and whether it is not the one asked for.
  std::string resolved_family;
  bool substituted{false};
  // Lines laid out, after wrapping.
  int lines{0};

  [[nodiscard]] bool empty() const { return coverage.empty() || width <= 0 || height <= 0; }
  [[nodiscard]] float at(int x, int y) const { return coverage[static_cast<std::size_t>(y) * width + x]; }
};

// Whether this build can rasterise text at all.
[[nodiscard]] bool Available();

// Lays `utf8` out in a box `wrap_width` pixels wide (0: one line per line break, no wrapping) and draws it.
// The bitmap is as large as the text needs.
[[nodiscard]] Bitmap Rasterize(const std::string& utf8, const Style& style, int wrap_width = 0);

// The typeface families installed, sorted, without duplicates.
[[nodiscard]] std::vector<std::string> InstalledFamilies();
[[nodiscard]] bool HasFamily(const std::string& family);

}  // namespace cutline::render::text

#pragma once

// Golden-frame comparison.
//
// For a media application this is the only test that can tell you a refactor
// did not change the picture. Unit tests on the compositor's helpers would all
// still pass if, say, the premultiply were dropped or the track order inverted;
// a golden image would not.
//
// How it works:
//   * Goldens are PPM (P6) files under tests/golden/. PPM because the format is
//     eight lines of code to read and write, has no compression to disagree
//     about between zlib versions, and diffs legibly as a binary blob.
//   * Comparison is per-channel with a tolerance, because bilinear sampling and
//     float rounding differ in the last bit between compilers. The tolerance is
//     tight enough that a real change in output fails.
//   * A failure writes the actual image and a difference map next to the golden
//     so the change can be looked at rather than guessed about.
//   * Setting CUTLINE_UPDATE_GOLDEN=1 rewrites goldens instead of comparing.
//     That is how a deliberate change is accepted, and it is deliberately an
//     environment variable so it cannot be left switched on in the source.

#include "media/VideoFrame.h"
#include "tests/native/TestHarness.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cutline::testing {

struct ImageDifference final {
  bool matches{false};
  int differing_pixels{0};
  int worst_channel_delta{0};
  int worst_x{-1};
  int worst_y{-1};
  std::string detail;
};

// Where goldens live. Resolved from the source tree so the tests can run from
// any build directory.
[[nodiscard]] inline std::filesystem::path GoldenDirectory() {
  // CUTLINE_GOLDEN_DIR is set by CMake to the source tree's tests/golden.
  const auto configured = EnvironmentValue("CUTLINE_GOLDEN_DIR");
  if (!configured.empty()) return std::filesystem::path(configured);
  return std::filesystem::current_path() / "tests" / "golden";
}

[[nodiscard]] inline bool UpdatingGoldens() {
  const auto flag = EnvironmentValue("CUTLINE_UPDATE_GOLDEN");
  return flag == "1" || flag == "y" || flag == "Y" || flag == "yes";
}

// Writes an RGB PPM. Alpha is composited over the checker-free black background
// that the compositor already flattened onto, so the golden records exactly what
// a viewer would see.
inline void WritePpm(const std::filesystem::path& path, const media::VideoFrame& frame) {
  const auto rgba = frame.format() == media::PixelFormat::Rgba8
                        ? frame.Clone()
                        : media::ConvertFrame(frame, media::PixelFormat::Rgba8);
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) throw std::runtime_error("Unable to write image " + path.string());
  file << "P6\n" << rgba.width() << " " << rgba.height() << "\n255\n";
  std::vector<char> row(static_cast<std::size_t>(rgba.width()) * 3);
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* pixels = rgba.row_u8(y);
    for (int x = 0; x < rgba.width(); ++x) {
      row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<char>(pixels[static_cast<std::size_t>(x) * 4 + 0]);
      row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<char>(pixels[static_cast<std::size_t>(x) * 4 + 1]);
      row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<char>(pixels[static_cast<std::size_t>(x) * 4 + 2]);
    }
    file.write(row.data(), static_cast<std::streamsize>(row.size()));
  }
}

struct LoadedImage final {
  int width{0};
  int height{0};
  std::vector<std::uint8_t> rgb;
};

[[nodiscard]] inline LoadedImage ReadPpm(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Unable to read image " + path.string());
  std::string magic;
  file >> magic;
  if (magic != "P6") throw std::runtime_error("Not a P6 PPM: " + path.string());
  LoadedImage image;
  int maximum = 0;
  file >> image.width >> image.height >> maximum;
  if (maximum != 255) throw std::runtime_error("Unsupported PPM depth in " + path.string());
  file.get();  // the single whitespace byte before the pixel data
  image.rgb.resize(static_cast<std::size_t>(image.width) * image.height * 3);
  file.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
  if (!file) throw std::runtime_error("PPM is truncated: " + path.string());
  return image;
}

// Per-channel comparison. `tolerance` is the largest absolute 8-bit difference
// accepted on any channel.
[[nodiscard]] inline ImageDifference CompareToGolden(const media::VideoFrame& actual, const std::string& name,
                                                     int tolerance = 2) {
  const auto golden_path = GoldenDirectory() / (name + ".ppm");

  if (UpdatingGoldens()) {
    WritePpm(golden_path, actual);
    return {true, 0, 0, -1, -1, "golden updated"};
  }

  if (!std::filesystem::exists(golden_path)) {
    // Write what we produced so a new golden can be reviewed and committed,
    // but fail: a missing golden is an unverified test, not a passing one.
    WritePpm(GoldenDirectory() / (name + ".actual.ppm"), actual);
    return {false, 0, 0, -1, -1,
            "no golden at " + golden_path.string() +
                "; the produced image was written alongside it as ." + name +
                ".actual.ppm. Review it, then re-run with CUTLINE_UPDATE_GOLDEN=1"};
  }

  const auto golden = ReadPpm(golden_path);
  const auto rgba = actual.format() == media::PixelFormat::Rgba8
                        ? actual.Clone()
                        : media::ConvertFrame(actual, media::PixelFormat::Rgba8);

  if (golden.width != rgba.width() || golden.height != rgba.height()) {
    WritePpm(GoldenDirectory() / (name + ".actual.ppm"), actual);
    return {false, 0, 0, -1, -1,
            "size changed: golden is " + std::to_string(golden.width) + "x" + std::to_string(golden.height) +
                ", produced " + std::to_string(rgba.width()) + "x" + std::to_string(rgba.height())};
  }

  ImageDifference difference;
  // The difference map is amplified so a one-level change is visible rather
  // than being an invisible near-black image.
  auto map = media::VideoFrame::Allocate(media::PixelFormat::Rgba8, rgba.width(), rgba.height());
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* actual_row = rgba.row_u8(y);
    auto* map_row = map.row_u8(y);
    for (int x = 0; x < rgba.width(); ++x) {
      const auto golden_index = (static_cast<std::size_t>(y) * golden.width + x) * 3;
      int worst = 0;
      for (int channel = 0; channel < 3; ++channel) {
        const int produced = actual_row[static_cast<std::size_t>(x) * 4 + channel];
        const int expected = golden.rgb[golden_index + static_cast<std::size_t>(channel)];
        worst = std::max(worst, std::abs(produced - expected));
      }
      if (worst > tolerance) {
        ++difference.differing_pixels;
        if (worst > difference.worst_channel_delta) {
          difference.worst_channel_delta = worst;
          difference.worst_x = x;
          difference.worst_y = y;
        }
      }
      const auto amplified = static_cast<std::uint8_t>(std::min(255, worst * 16));
      map_row[static_cast<std::size_t>(x) * 4 + 0] = amplified;
      map_row[static_cast<std::size_t>(x) * 4 + 1] = amplified;
      map_row[static_cast<std::size_t>(x) * 4 + 2] = amplified;
      map_row[static_cast<std::size_t>(x) * 4 + 3] = 255;
    }
  }

  difference.matches = difference.differing_pixels == 0;
  if (!difference.matches) {
    WritePpm(GoldenDirectory() / (name + ".actual.ppm"), actual);
    WritePpm(GoldenDirectory() / (name + ".diff.ppm"), map);
    difference.detail = std::to_string(difference.differing_pixels) + " pixel(s) differ by more than " +
                        std::to_string(tolerance) + "; worst delta " +
                        std::to_string(difference.worst_channel_delta) + " at (" +
                        std::to_string(difference.worst_x) + "," + std::to_string(difference.worst_y) +
                        "). Wrote " + name + ".actual.ppm and " + name + ".diff.ppm";
  }
  return difference;
}

}  // namespace cutline::testing

// Asserts a frame matches its golden. Named so the golden file is predictable
// from the test that uses it.
#define CHECK_GOLDEN(frame, name)                                                        \
  do {                                                                                   \
    const auto golden_result = cutline::testing::CompareToGolden((frame), (name));        \
    if (!golden_result.matches) {                                                         \
      cutline::testing::Fail("CHECK_GOLDEN(" #frame ", " name ")", __FILE__, __LINE__,     \
                             golden_result.detail);                                       \
    }                                                                                     \
  } while (false)

#define CHECK_GOLDEN_TOLERANCE(frame, name, tolerance)                                   \
  do {                                                                                   \
    const auto golden_result = cutline::testing::CompareToGolden((frame), (name), (tolerance)); \
    if (!golden_result.matches) {                                                         \
      cutline::testing::Fail("CHECK_GOLDEN(" #frame ", " name ")", __FILE__, __LINE__,     \
                             golden_result.detail);                                       \
    }                                                                                     \
  } while (false)

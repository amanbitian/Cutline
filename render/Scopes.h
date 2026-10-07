#pragma once

// Video scopes: waveform, parade, vectorscope and histogram, measured from a frame.
//
// A scope is a measurement, not a picture. Each function here returns the measured
// densities as numbers, with the coordinate system written down, so a test can look up
// where a known value must land and a display can draw the same numbers however it
// likes. Nothing here touches the frame, allocates per pixel, or depends on a window;
// a UI that wants scopes calls these from a worker on a copy of the displayed frame and
// is free to drop frames it has no time for.
//
// Conventions:
//   * Input is the frame's straight (un-premultiplied) RGB, display-referred, 0..1.
//     Values outside that range (float frames can carry them) are counted, not hidden:
//     see `below` and `above`.
//   * Luma and chroma use the chosen matrix (BT.709 by default).
//   * Waveform and parade images are `height` rows by `width` columns; row 0 is the
//     top, the highest value. A scope column is a band of source columns, so the scope
//     can be narrower or wider than the picture. Each cell holds the fraction of the
//     picture's rows in that band whose value fell in that row's level: a column holding
//     one flat value reads 1.0 in one cell.
//   * The vectorscope is `size` by `size`: x = Cb, y = Cr, both -0.5..+0.5, with
//     +Cr up (row 0 is the most red-ish). Cells hold the fraction of all pixels.
//   * The scale says how a scope's axis is labelled (percent, 8-bit or 10-bit code
//     values); it does not change where anything is measured.

#include "media/VideoFrame.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace cutline::render {

enum class ScopeMatrix { Rec709, Rec601, Rec2020 };
enum class ScopeScale { Percent, Code8, Code10 };
enum class WaveformMode { Luma, Rgb, Parade };

// The luma coefficients of a matrix.
[[nodiscard]] std::array<float, 3> LumaCoefficients(ScopeMatrix matrix);

// A level 0..1 expressed on an axis scale: 0..100, 0..255 or 0..1023.
[[nodiscard]] double ScaleLevel(double level, ScopeScale scale);

// The row of a `height`-row scope on which a level 0..1 is drawn (0 at the top =
// level 1), and its inverse for a row.
[[nodiscard]] int LevelToRow(double level, int height);
[[nodiscard]] double RowToLevel(int row, int height);

struct ScopeOptions final {
  ScopeMatrix matrix{ScopeMatrix::Rec709};
  int width{256};   // waveform / parade columns
  int height{256};  // waveform / parade levels
};

struct Waveform final {
  int width{0};
  int height{0};
  int channels{1};  // 1 for luma; 3 for Rgb (overlaid) and Parade (side by side in one image)
  WaveformMode mode{WaveformMode::Luma};
  std::vector<float> density;  // [channel][row][column]
  // Pixels below 0 and above 1 on any measured channel, so an out-of-range picture is visible.
  long long below{0};
  long long above{0};

  [[nodiscard]] float at(int channel, int row, int column) const {
    return density[(static_cast<std::size_t>(channel) * height + row) * width + column];
  }
};

[[nodiscard]] Waveform MeasureWaveform(const media::VideoFrame& frame, WaveformMode mode, const ScopeOptions& options = {});

struct Vectorscope final {
  int size{0};
  std::vector<float> density;  // [row][column]
  long long pixels{0};

  [[nodiscard]] float at(int row, int column) const { return density[static_cast<std::size_t>(row) * size + column]; }
};

[[nodiscard]] Vectorscope MeasureVectorscope(const media::VideoFrame& frame, int size = 256,
                                             ScopeMatrix matrix = ScopeMatrix::Rec709);

// The cell a Cb/Cr pair lands in on a `size`-wide vectorscope.
struct ScopeCell final {
  int row{0};
  int column{0};
};
[[nodiscard]] ScopeCell VectorscopeCell(double cb, double cr, int size);
// Cb and Cr of straight RGB under a matrix.
[[nodiscard]] std::array<double, 2> ChromaOf(double r, double g, double b, ScopeMatrix matrix);

struct Histogram final {
  int bins{0};
  // Fractions of the picture's pixels in each bin, for red, green, blue and luma.
  std::array<std::vector<float>, 4> channels;
  long long below{0};
  long long above{0};
  long long pixels{0};
};

[[nodiscard]] Histogram MeasureHistogram(const media::VideoFrame& frame, int bins = 256,
                                         ScopeMatrix matrix = ScopeMatrix::Rec709);

struct AsyncScopeOptions final {
  ScopeOptions waveform;
  WaveformMode waveform_mode{WaveformMode::Luma};
  int vectorscope_size{256};
  int histogram_bins{256};
  // The worker downsamples before measuring. This bounds CPU and memory cost
  // independently of whether the displayed picture is HD, 4K or 8K.
  int max_input_width{640};
  int max_input_height{360};
  bool measure_waveform{true};
  bool measure_vectorscope{true};
  bool measure_histogram{true};
};

struct AsyncScopeResult final {
  std::uint64_t generation{};
  std::optional<Waveform> waveform;
  std::optional<Vectorscope> vectorscope;
  std::optional<Histogram> histogram;
};

struct AsyncScopeStatistics final {
  std::uint64_t submitted{};
  std::uint64_t completed{};
  std::uint64_t dropped{};
};

// A single-slot latest-frame worker. Playback never waits for scopes: when it
// submits faster than measurement, the stale queued frame is replaced.
class AsyncScopes final {
 public:
  explicit AsyncScopes(AsyncScopeOptions options = {});
  ~AsyncScopes();
  AsyncScopes(const AsyncScopes&) = delete;
  AsyncScopes& operator=(const AsyncScopes&) = delete;

  [[nodiscard]] std::uint64_t Submit(media::VideoFrame frame);
  [[nodiscard]] std::optional<AsyncScopeResult> Latest() const;
  [[nodiscard]] bool WaitFor(std::uint64_t generation, std::chrono::milliseconds timeout);
  [[nodiscard]] AsyncScopeStatistics statistics() const;

 private:
  struct Pending final { std::uint64_t generation{}; media::VideoFrame frame; };
  void Run();

  AsyncScopeOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable completed_;
  std::optional<Pending> pending_;
  std::optional<AsyncScopeResult> latest_;
  AsyncScopeStatistics statistics_;
  std::thread worker_;
  bool stopping_{false};
};

}  // namespace cutline::render

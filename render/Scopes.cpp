#include "render/Scopes.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace cutline::render {
namespace {

struct Rgb final {
  float r, g, b;
};

// Reads a pixel as straight RGB. The compositor's frames are straight-alpha float; an
// 8-bit frame is converted once, up front, by the callers below.
[[nodiscard]] Rgb ReadPixel(const float* texel) { return {texel[0], texel[1], texel[2]}; }

[[nodiscard]] media::VideoFrame AsFloat(const media::VideoFrame& frame) {
  if (!frame.valid()) throw std::invalid_argument("A scope needs a picture");
  if (frame.format() == media::PixelFormat::RgbaF32) return frame.Clone();
  return media::ConvertFrame(frame, media::PixelFormat::RgbaF32);
}

[[nodiscard]] float LumaOf(const Rgb& c, const std::array<float, 3>& k) { return c.r * k[0] + c.g * k[1] + c.b * k[2]; }

[[nodiscard]] media::VideoFrame Downsample(const media::VideoFrame& input, int max_width, int max_height) {
  if (max_width <= 0 || max_height <= 0) throw std::invalid_argument("Scope downsample size must be positive");
  const auto scale = std::min({1.0, static_cast<double>(max_width) / input.width(),
                              static_cast<double>(max_height) / input.height()});
  if (scale >= 1.0) return input.Clone();
  const auto source = AsFloat(input);
  const auto width = std::max(1, static_cast<int>(std::floor(input.width() * scale)));
  const auto height = std::max(1, static_cast<int>(std::floor(input.height() * scale)));
  auto output = media::VideoFrame::Allocate(media::PixelFormat::RgbaF32, width, height);
  output.color = input.color;
  for (int y = 0; y < height; ++y) {
    const auto sy = std::min(input.height() - 1, static_cast<int>((static_cast<long long>(y) * input.height()) / height));
    auto* target = output.row_f32(y);
    const auto* source_row = source.row_f32(sy);
    for (int x = 0; x < width; ++x) {
      const auto sx = std::min(input.width() - 1, static_cast<int>((static_cast<long long>(x) * input.width()) / width));
      for (int channel = 0; channel < 4; ++channel) {
        target[static_cast<std::size_t>(x) * 4 + channel] = source_row[static_cast<std::size_t>(sx) * 4 + channel];
      }
    }
  }
  return output;
}

}  // namespace

std::array<float, 3> LumaCoefficients(ScopeMatrix matrix) {
  switch (matrix) {
    case ScopeMatrix::Rec601:
      return {0.299f, 0.587f, 0.114f};
    case ScopeMatrix::Rec2020:
      return {0.2627f, 0.6780f, 0.0593f};
    case ScopeMatrix::Rec709:
      break;
  }
  return {0.2126f, 0.7152f, 0.0722f};
}

double ScaleLevel(double level, ScopeScale scale) {
  switch (scale) {
    case ScopeScale::Code8:
      return level * 255.0;
    case ScopeScale::Code10:
      return level * 1023.0;
    case ScopeScale::Percent:
      break;
  }
  return level * 100.0;
}

int LevelToRow(double level, int height) {
  const auto clamped = std::clamp(level, 0.0, 1.0);
  return static_cast<int>(std::lround((1.0 - clamped) * (height - 1)));
}

double RowToLevel(int row, int height) {
  return height <= 1 ? 0.0 : 1.0 - static_cast<double>(row) / static_cast<double>(height - 1);
}

std::array<double, 2> ChromaOf(double r, double g, double b, ScopeMatrix matrix) {
  const auto k = LumaCoefficients(matrix);
  const auto y = k[0] * r + k[1] * g + k[2] * b;
  // Cb = (B - Y) / (2 (1 - Kb)), Cr = (R - Y) / (2 (1 - Kr)): each spans -0.5..+0.5.
  return {(b - y) / (2.0 * (1.0 - k[2])), (r - y) / (2.0 * (1.0 - k[0]))};
}

ScopeCell VectorscopeCell(double cb, double cr, int size) {
  const auto column = static_cast<int>(std::lround(std::clamp(cb + 0.5, 0.0, 1.0) * (size - 1)));
  const auto row = static_cast<int>(std::lround((1.0 - std::clamp(cr + 0.5, 0.0, 1.0)) * (size - 1)));
  return {row, column};
}

Waveform MeasureWaveform(const media::VideoFrame& input, WaveformMode mode, const ScopeOptions& options) {
  if (options.width < 1 || options.height < 2) throw std::invalid_argument("A waveform needs a size");
  const auto frame = AsFloat(input);
  const auto k = LumaCoefficients(options.matrix);

  // A parade is three waveforms side by side, each a third of the width.
  const bool parade = mode == WaveformMode::Parade;
  const auto columns = parade ? std::max(1, options.width / 3) : options.width;

  Waveform scope;
  scope.width = parade ? columns * 3 : options.width;
  scope.height = options.height;
  scope.mode = mode;
  scope.channels = mode == WaveformMode::Luma ? 1 : 3;
  scope.density.assign(static_cast<std::size_t>(scope.channels) * scope.height * scope.width, 0.0f);

  const auto note_range = [&](double value) {
    if (value < 0.0) ++scope.below;
    else if (value > 1.0) ++scope.above;
  };
  // Out-of-range values are counted once per pixel, whatever the scope's width.
  for (int y = 0; y < frame.height(); ++y) {
    const auto* row = frame.row_f32(y);
    for (int x = 0; x < frame.width(); ++x) {
      const auto c = ReadPixel(row + static_cast<std::size_t>(x) * 4);
      if (mode == WaveformMode::Luma) {
        note_range(LumaOf(c, k));
      } else {
        note_range(c.r);
        note_range(c.g);
        note_range(c.b);
      }
    }
  }

  // Each scope column reads a band of source columns (at least one, so a scope wider than
  // the picture has no gaps) and records where each pixel's level falls.
  for (int column = 0; column < columns; ++column) {
    const auto first = static_cast<int>(static_cast<long long>(column) * frame.width() / columns);
    const auto last = std::max(first + 1, static_cast<int>((static_cast<long long>(column + 1) * frame.width() + columns - 1) / columns));
    const auto samples = static_cast<float>(std::min(last, frame.width()) - first) * static_cast<float>(frame.height());
    for (int x = first; x < std::min(last, frame.width()); ++x) {
      for (int y = 0; y < frame.height(); ++y) {
        const auto c = ReadPixel(frame.row_f32(y) + static_cast<std::size_t>(x) * 4);
        const float values[3] = {c.r, c.g, c.b};
        const int measured = mode == WaveformMode::Luma ? 1 : 3;
        for (int channel = 0; channel < measured; ++channel) {
          const auto level = mode == WaveformMode::Luma ? LumaOf(c, k) : values[channel];
          const auto row = LevelToRow(level, scope.height);
          const auto where = parade ? column + channel * columns : column;
          scope.density[(static_cast<std::size_t>(channel) * scope.height + row) * scope.width + where] += 1.0f / samples;
        }
      }
    }
  }
  return scope;
}

Vectorscope MeasureVectorscope(const media::VideoFrame& input, int size, ScopeMatrix matrix) {
  if (size < 2) throw std::invalid_argument("A vectorscope needs a size");
  const auto frame = AsFloat(input);
  Vectorscope scope;
  scope.size = size;
  scope.density.assign(static_cast<std::size_t>(size) * size, 0.0f);
  for (int y = 0; y < frame.height(); ++y) {
    const auto* row = frame.row_f32(y);
    for (int x = 0; x < frame.width(); ++x) {
      const auto c = ReadPixel(row + static_cast<std::size_t>(x) * 4);
      const auto chroma = ChromaOf(c.r, c.g, c.b, matrix);
      const auto cell = VectorscopeCell(chroma[0], chroma[1], size);
      scope.density[static_cast<std::size_t>(cell.row) * size + cell.column] += 1.0f;
      ++scope.pixels;
    }
  }
  if (scope.pixels > 0) {
    const auto inverse = 1.0f / static_cast<float>(scope.pixels);
    for (auto& cell : scope.density) cell *= inverse;
  }
  return scope;
}

Histogram MeasureHistogram(const media::VideoFrame& input, int bins, ScopeMatrix matrix) {
  if (bins < 2) throw std::invalid_argument("A histogram needs at least two bins");
  const auto frame = AsFloat(input);
  const auto k = LumaCoefficients(matrix);
  Histogram histogram;
  histogram.bins = bins;
  for (auto& channel : histogram.channels) channel.assign(static_cast<std::size_t>(bins), 0.0f);
  const auto bin_of = [bins](double value) {
    return static_cast<std::size_t>(std::clamp(static_cast<int>(std::floor(std::clamp(value, 0.0, 1.0) * bins)), 0, bins - 1));
  };
  for (int y = 0; y < frame.height(); ++y) {
    const auto* row = frame.row_f32(y);
    for (int x = 0; x < frame.width(); ++x) {
      const auto c = ReadPixel(row + static_cast<std::size_t>(x) * 4);
      const float values[4] = {c.r, c.g, c.b, LumaOf(c, k)};
      for (int channel = 0; channel < 4; ++channel) {
        histogram.channels[static_cast<std::size_t>(channel)][bin_of(values[channel])] += 1.0f;
        if (channel < 3) {
          if (values[channel] < 0.0f) ++histogram.below;
          else if (values[channel] > 1.0f) ++histogram.above;
        }
      }
      ++histogram.pixels;
    }
  }
  if (histogram.pixels > 0) {
    const auto inverse = 1.0f / static_cast<float>(histogram.pixels);
    for (auto& channel : histogram.channels) {
      for (auto& bin : channel) bin *= inverse;
    }
  }
  return histogram;
}

AsyncScopes::AsyncScopes(AsyncScopeOptions options) : options_(options) {
  if (options_.max_input_width <= 0 || options_.max_input_height <= 0) {
    throw std::invalid_argument("Async scopes need a positive input bound");
  }
  worker_ = std::thread([this] { Run(); });
}

AsyncScopes::~AsyncScopes() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    pending_.reset();
  }
  ready_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::uint64_t AsyncScopes::Submit(media::VideoFrame frame) {
  if (!frame.valid()) throw std::invalid_argument("Async scopes need a valid frame");
  std::uint64_t generation = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) throw std::logic_error("Async scopes are stopping");
    generation = ++statistics_.submitted;
    if (pending_) ++statistics_.dropped;
    pending_ = Pending{generation, std::move(frame)};
  }
  ready_.notify_one();
  return generation;
}

std::optional<AsyncScopeResult> AsyncScopes::Latest() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

bool AsyncScopes::WaitFor(std::uint64_t generation, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return completed_.wait_for(lock, timeout, [&] {
    return stopping_ || (latest_.has_value() && latest_->generation >= generation);
  }) && latest_.has_value() && latest_->generation >= generation;
}

AsyncScopeStatistics AsyncScopes::statistics() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return statistics_;
}

void AsyncScopes::Run() {
  while (true) {
    Pending job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
      if (stopping_) return;
      job = std::move(*pending_);
      pending_.reset();
    }
    const auto input = Downsample(job.frame, options_.max_input_width, options_.max_input_height);
    AsyncScopeResult result;
    result.generation = job.generation;
    if (options_.measure_waveform) result.waveform = MeasureWaveform(input, options_.waveform_mode, options_.waveform);
    if (options_.measure_vectorscope) result.vectorscope = MeasureVectorscope(input, options_.vectorscope_size, options_.waveform.matrix);
    if (options_.measure_histogram) result.histogram = MeasureHistogram(input, options_.histogram_bins, options_.waveform.matrix);
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      latest_ = std::move(result);
      ++statistics_.completed;
    }
    completed_.notify_all();
  }
}

}  // namespace cutline::render

#include "render/AngleMonitor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cutline::render {
namespace {

void Validate(const AngleMonitorConfig& config) {
  if (config.width < 16 || config.height < 16) throw std::invalid_argument("The angle monitor is too small");
  if (config.gap < 0 || config.border < 0 || config.columns < 0) throw std::invalid_argument("The angle monitor layout is invalid");
}

void Fill(media::VideoFrame& frame, const TileRect& rect, const std::array<float, 3>& colour) {
  for (int y = std::max(0, rect.y); y < std::min(frame.height(), rect.y + rect.height); ++y) {
    auto* row = frame.row_f32(y);
    for (int x = std::max(0, rect.x); x < std::min(frame.width(), rect.x + rect.width); ++x) {
      auto* texel = row + static_cast<std::size_t>(x) * 4;
      texel[0] = colour[0];
      texel[1] = colour[1];
      texel[2] = colour[2];
      texel[3] = 1.0f;
    }
  }
}

// Averages the source pixels each destination pixel covers.
void Fit(const media::VideoFrame& source, media::VideoFrame& destination, const TileRect& rect) {
  const double scale = std::min(static_cast<double>(rect.width) / source.width(), static_cast<double>(rect.height) / source.height());
  const int width = std::max(1, static_cast<int>(std::floor(source.width() * scale)));
  const int height = std::max(1, static_cast<int>(std::floor(source.height() * scale)));
  const int left = rect.x + (rect.width - width) / 2;
  const int top = rect.y + (rect.height - height) / 2;
  for (int y = 0; y < height; ++y) {
    const int y0 = static_cast<int>(static_cast<std::int64_t>(y) * source.height() / height);
    const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<std::int64_t>(y + 1) * source.height() / height));
    auto* out = destination.row_f32(top + y);
    for (int x = 0; x < width; ++x) {
      const int x0 = static_cast<int>(static_cast<std::int64_t>(x) * source.width() / width);
      const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<std::int64_t>(x + 1) * source.width() / width));
      double sum[3]{};
      int count = 0;
      for (int sy = y0; sy < std::min(y1, source.height()); ++sy) {
        const auto* row = source.row_f32(sy);
        for (int sx = x0; sx < std::min(x1, source.width()); ++sx) {
          const auto* texel = row + static_cast<std::size_t>(sx) * 4;
          // Straight colour over black, so a transparent picture shows dark rather than its hidden colour.
          const double alpha = std::clamp(static_cast<double>(texel[3]), 0.0, 1.0);
          sum[0] += texel[0] * alpha;
          sum[1] += texel[1] * alpha;
          sum[2] += texel[2] * alpha;
          ++count;
        }
      }
      auto* texel = out + static_cast<std::size_t>(left + x) * 4;
      const double n = std::max(1, count);
      texel[0] = static_cast<float>(sum[0] / n);
      texel[1] = static_cast<float>(sum[1] / n);
      texel[2] = static_cast<float>(sum[2] / n);
      texel[3] = 1.0f;
    }
  }
}

}  // namespace

std::vector<TileRect> AngleMonitorLayout(std::size_t count, const AngleMonitorConfig& config) {
  Validate(config);
  if (count == 0) return {};
  const int columns = config.columns > 0 ? std::min<int>(config.columns, static_cast<int>(count))
                                         : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
  const int rows = static_cast<int>((count + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
  const int cell_width = (config.width - config.gap * (columns + 1)) / columns;
  const int cell_height = (config.height - config.gap * (rows + 1)) / rows;
  if (cell_width < 4 || cell_height < 4) throw std::invalid_argument("The angle monitor has no room for that many angles");
  std::vector<TileRect> rects;
  for (std::size_t index = 0; index < count; ++index) {
    const int column = static_cast<int>(index % static_cast<std::size_t>(columns));
    const int row = static_cast<int>(index / static_cast<std::size_t>(columns));
    rects.push_back({config.gap + column * (cell_width + config.gap), config.gap + row * (cell_height + config.gap), cell_width, cell_height});
  }
  return rects;
}

int AngleAtPoint(std::size_t count, int x, int y, const AngleMonitorConfig& config) {
  const auto rects = AngleMonitorLayout(count, config);
  for (std::size_t index = 0; index < rects.size(); ++index) {
    if (rects[index].Contains(x, y)) return static_cast<int>(index);
  }
  return -1;
}

media::VideoFrame ComposeAngleMonitor(const std::vector<MonitorTile>& tiles, const AngleMonitorConfig& config) {
  const auto rects = AngleMonitorLayout(tiles.size(), config);
  auto output = media::VideoFrame::Allocate(media::PixelFormat::RgbaF32, config.width, config.height);
  Fill(output, {0, 0, config.width, config.height}, config.background);
  for (std::size_t index = 0; index < tiles.size(); ++index) {
    auto rect = rects[index];
    if (tiles[index].active) {
      Fill(output, rect, config.active_border);
      const int inset = std::min({config.border, rect.width / 4, rect.height / 4});
      rect = {rect.x + inset, rect.y + inset, rect.width - 2 * inset, rect.height - 2 * inset};
    }
    Fill(output, rect, config.empty);
    const auto* frame = tiles[index].frame;
    if (frame == nullptr || !frame->valid()) continue;
    if (frame->format() == media::PixelFormat::RgbaF32) {
      Fit(*frame, output, rect);
    } else {
      const auto converted = media::ConvertFrame(*frame, media::PixelFormat::RgbaF32);
      Fit(converted, output, rect);
    }
  }
  return output;
}

}  // namespace cutline::render

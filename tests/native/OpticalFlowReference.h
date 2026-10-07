#pragma once

// Exhaustive pre-optimization search kept as an independent regression oracle.
#include "render/OpticalFlow.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace flow_reference {
namespace media = cutline::media;
using cutline::render::FlowField;
using cutline::render::OpticalFlowConfig;
using cutline::render::FlowCancelled;

[[nodiscard]] float Luma(const media::VideoFrame& frame, int x, int y) {
  const auto* pixel = frame.row_f32(y) + static_cast<std::size_t>(x) * 4;
  return 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
}

[[nodiscard]] FlowField EstimateOneWay(const media::VideoFrame& first, const media::VideoFrame& second,
                                       const OpticalFlowConfig& config) {
  FlowField field;
  field.width = first.width();
  field.height = first.height();
  field.block_size = std::max(2, config.block_size);
  field.columns = (field.width + field.block_size - 1) / field.block_size;
  field.rows = (field.height + field.block_size - 1) / field.block_size;
  field.samples.resize(static_cast<std::size_t>(field.columns * field.rows));

  const auto search = std::max(0, config.search_radius);
  for (int block_y = 0; block_y < field.rows; ++block_y) {
    if (config.cancel && config.cancel()) throw FlowCancelled();
    const auto y0 = block_y * field.block_size;
    const auto y1 = std::min(y0 + field.block_size, field.height);
    for (int block_x = 0; block_x < field.columns; ++block_x) {
      const auto x0 = block_x * field.block_size;
      const auto x1 = std::min(x0 + field.block_size, field.width);
      double best = std::numeric_limits<double>::infinity();
      double second_best = std::numeric_limits<double>::infinity();
      int best_dx = 0;
      int best_dy = 0;
      int best_count = 0;

      for (int dy = -search; dy <= search; ++dy) {
        for (int dx = -search; dx <= search; ++dx) {
          double error = 0.0;
          int count = 0;
          for (int y = y0; y < y1; ++y) {
            const auto sy = y + dy;
            if (sy < 0 || sy >= field.height) continue;
            for (int x = x0; x < x1; ++x) {
              const auto sx = x + dx;
              if (sx < 0 || sx >= field.width) continue;
              error += std::abs(static_cast<double>(Luma(first, x, y) - Luma(second, sx, sy)));
              ++count;
            }
          }
          if (count < std::max(1, (x1 - x0) * (y1 - y0) / 2)) continue;
          const auto average = error / static_cast<double>(count);
          // A small distance preference resolves flat/periodic areas without
          // pulling motion away from a clearly better match.
          const auto score = average + 1e-5 * static_cast<double>(dx * dx + dy * dy);
          if (score < best) {
            second_best = best;
            best = score;
            best_dx = dx;
            best_dy = dy;
            best_count = count;
          } else if (score < second_best) {
            second_best = score;
          }
        }
      }

      auto& sample = field.samples[static_cast<std::size_t>(block_y * field.columns + block_x)];
      sample.dx = static_cast<float>(best_dx);
      sample.dy = static_cast<float>(best_dy);
      if (best_count > 0 && std::isfinite(second_best)) {
        sample.confidence = static_cast<float>(std::clamp((second_best - best) / (second_best + 1e-6), 0.0, 1.0));
      }
    }
  }
  return field;
}

}  // namespace flow_reference

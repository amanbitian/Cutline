#pragma once

// Deterministic CPU optical flow used for frame interpolation and as the
// reference output for a future compute implementation.

#include "media/VideoFrame.h"

#include <functional>
#include <stdexcept>
#include <vector>

namespace cutline::render {

struct FlowSample final {
  float dx{0.0f};
  float dy{0.0f};
  float confidence{0.0f};
  bool occluded{false};
};

struct FlowField final {
  int width{0};
  int height{0};
  int block_size{8};
  int columns{0};
  int rows{0};
  std::vector<FlowSample> samples;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const FlowSample& at(int column, int row) const;
  [[nodiscard]] FlowSample sample(float x, float y) const;
};

// Thrown by EstimateOpticalFlow when its cancel check says to stop.
class FlowCancelled final : public std::runtime_error {
 public:
  FlowCancelled() : std::runtime_error("Optical-flow estimation was cancelled") {}
};

struct OpticalFlowConfig final {
  int block_size{8};
  int search_radius{12};
  float occlusion_tolerance{1.5f};
  float minimum_confidence{0.05f};
  // Polled between rows of blocks; true makes the estimate throw FlowCancelled. Not part of the
  // result, so not part of a cache key.
  std::function<bool()> cancel;
};

// Estimates first->second motion. Frames must have equal dimensions; any RGBA
// format accepted by ConvertFrame is allowed.
[[nodiscard]] FlowField EstimateOpticalFlow(const media::VideoFrame& first, const media::VideoFrame& second,
                                            const OpticalFlowConfig& config = {});

// Synthesises a point between the frames (0=first, 1=second). Low-confidence
// and occluded regions fall back to a cross-dissolve to avoid stretched edges.
[[nodiscard]] media::VideoFrame InterpolateOpticalFlow(const media::VideoFrame& first,
                                                       const media::VideoFrame& second, float progress,
                                                       const FlowField& forward,
                                                       const FlowField* backward = nullptr);

}  // namespace cutline::render

#pragma once

// Offline translational video stabilisation. Analysis is intentionally separate
// from frame rendering: it may inspect many frames, records deterministic
// keyframes, and playback then remains a pure per-frame operation.

#include "media/VideoFrame.h"
#include "timeline/Sequence.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cutline::render {

struct StabilizerConfig final {
  int analysis_width{160};
  int max_translation_pixels{48};
  int smoothing_radius_frames{12};
  float minimum_improvement{0.01f};
  float crop_safety{1.02f};
  float maximum_scale{1.25f};
};

struct StabilizerSample final {
  time::RationalTime time;
  float correction_x{};
  float correction_y{};
};

struct StabilizationResult final {
  std::vector<StabilizerSample> samples;
  float auto_scale{1.0f};
  std::string algorithm{"translation_sad_v1"};
  int source_width{};
  int source_height{};
};

using StabilizerFrameResolver = std::function<const media::VideoFrame*(std::size_t frame_index)>;

// Resolves each frame once, estimates inter-frame camera translation on luma
// thumbnails, smooths the camera path, and returns the inverse correction.
[[nodiscard]] StabilizationResult AnalyzeStabilization(std::size_t frame_count, time::FrameRate frame_rate,
                                                        const StabilizerFrameResolver& resolve,
                                                        const StabilizerConfig& config = {});

// Turns the result into an ordinary keyframed effect suitable for AddEffect /
// SetKeyframe persistence. Removing the effect reverses stabilization without
// touching the source media.
[[nodiscard]] timeline::Effect MakeStabilizerEffect(const StabilizationResult& result, std::string effect_id,
                                                    std::int64_t order = 0);

}  // namespace cutline::render

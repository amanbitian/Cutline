#include "render/Stabilizer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cutline::render {
namespace {

struct LumaImage final {
  int width{};
  int height{};
  float scale_x{1.0f};
  float scale_y{1.0f};
  std::vector<float> pixels;

  [[nodiscard]] float at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
};

[[nodiscard]] LumaImage MakeThumbnail(const media::VideoFrame& input, int requested_width) {
  if (!input.valid()) throw std::invalid_argument("Stabilizer received an invalid frame");
  media::VideoFrame converted;
  const media::VideoFrame* frame = &input;
  if (input.format() != media::PixelFormat::RgbaF32) {
    converted = media::ConvertFrame(input, media::PixelFormat::RgbaF32);
    frame = &converted;
  }
  const auto width = std::max(16, std::min(requested_width, frame->width()));
  const auto height = std::max(16, static_cast<int>(std::lround(static_cast<double>(frame->height()) * width /
                                                                static_cast<double>(frame->width()))));
  LumaImage result;
  result.width = width;
  result.height = height;
  result.scale_x = static_cast<float>(frame->width()) / width;
  result.scale_y = static_cast<float>(frame->height()) / height;
  result.pixels.resize(static_cast<std::size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    const auto source_y = std::min(frame->height() - 1, static_cast<int>((y + 0.5f) * result.scale_y));
    const auto* row = frame->row_f32(source_y);
    for (int x = 0; x < width; ++x) {
      const auto source_x = std::min(frame->width() - 1, static_cast<int>((x + 0.5f) * result.scale_x));
      const auto* pixel = row + static_cast<std::size_t>(source_x) * 4;
      result.pixels[static_cast<std::size_t>(y) * width + x] =
          pixel[0] * 0.2126f + pixel[1] * 0.7152f + pixel[2] * 0.0722f;
    }
  }
  return result;
}

[[nodiscard]] std::pair<float, float> EstimateTranslation(const LumaImage& previous, const LumaImage& current,
                                                           const StabilizerConfig& config) {
  if (previous.width != current.width || previous.height != current.height) {
    throw std::invalid_argument("Stabilizer frame dimensions changed during analysis");
  }
  const auto search_x = std::clamp(static_cast<int>(std::ceil(config.max_translation_pixels / current.scale_x)), 1,
                                   std::max(1, previous.width / 4 - 2));
  const auto search_y = std::clamp(static_cast<int>(std::ceil(config.max_translation_pixels / current.scale_y)), 1,
                                   std::max(1, previous.height / 4 - 2));
  const auto margin_x = search_x + 2;
  const auto margin_y = search_y + 2;
  if (previous.width - margin_x * 2 < 8 || previous.height - margin_y * 2 < 8) {
    throw std::invalid_argument("Stabilizer analysis frame is too small for the search range");
  }

  const auto cost = [&](int dx, int dy) {
    double error = 0.0;
    std::size_t samples = 0;
    // A two-pixel stride is sufficient on the deliberately small luma proxy and
    // cuts analysis work by four without changing the integer-pixel result.
    for (int y = margin_y; y < previous.height - margin_y; y += 2) {
      for (int x = margin_x; x < previous.width - margin_x; x += 2) {
        error += std::abs(previous.at(x, y) - current.at(x + dx, y + dy));
        ++samples;
      }
    }
    return static_cast<float>(error / static_cast<double>(samples));
  };

  const auto zero_cost = cost(0, 0);
  auto best_cost = std::numeric_limits<float>::max();
  int best_x = 0;
  int best_y = 0;
  for (int dy = -search_y; dy <= search_y; ++dy) {
    for (int dx = -search_x; dx <= search_x; ++dx) {
      const auto candidate = cost(dx, dy);
      const auto candidate_distance = std::abs(dx) + std::abs(dy);
      const auto best_distance = std::abs(best_x) + std::abs(best_y);
      if (candidate < best_cost - 1e-7f ||
          (std::abs(candidate - best_cost) <= 1e-7f && candidate_distance < best_distance)) {
        best_cost = candidate;
        best_x = dx;
        best_y = dy;
      }
    }
  }
  if (zero_cost <= 1e-7f || best_cost > zero_cost * (1.0f - std::clamp(config.minimum_improvement, 0.0f, 0.99f))) {
    return {0.0f, 0.0f};
  }
  return {best_x * current.scale_x, best_y * current.scale_y};
}

}  // namespace

StabilizationResult AnalyzeStabilization(std::size_t frame_count, time::FrameRate frame_rate,
                                         const StabilizerFrameResolver& resolve, const StabilizerConfig& config) {
  if (frame_count == 0) throw std::invalid_argument("Stabilizer needs at least one frame");
  if (!resolve) throw std::invalid_argument("Stabilizer has no frame resolver");
  if (frame_rate.numerator <= 0 || frame_rate.denominator <= 0) {
    throw std::invalid_argument("Stabilizer has an invalid frame rate");
  }
  if (config.analysis_width < 16 || config.max_translation_pixels < 1 || config.smoothing_radius_frames < 0) {
    throw std::invalid_argument("Stabilizer configuration is invalid");
  }

  const auto* first_frame = resolve(0);
  if (first_frame == nullptr || !first_frame->valid()) throw std::runtime_error("Stabilizer could not resolve frame 0");
  StabilizationResult result;
  result.source_width = first_frame->width();
  result.source_height = first_frame->height();
  auto previous = MakeThumbnail(*first_frame, config.analysis_width);
  std::vector<std::pair<float, float>> path(frame_count);
  for (std::size_t index = 1; index < frame_count; ++index) {
    const auto* frame = resolve(index);
    if (frame == nullptr || !frame->valid()) {
      throw std::runtime_error("Stabilizer could not resolve frame " + std::to_string(index));
    }
    if (frame->width() != result.source_width || frame->height() != result.source_height) {
      throw std::invalid_argument("Stabilizer frame dimensions changed during analysis");
    }
    auto current = MakeThumbnail(*frame, config.analysis_width);
    const auto motion = EstimateTranslation(previous, current, config);
    path[index] = {path[index - 1].first + motion.first, path[index - 1].second + motion.second};
    previous = std::move(current);
  }

  const auto radius = config.smoothing_radius_frames;
  result.samples.reserve(frame_count);
  float maximum_x = 0.0f;
  float maximum_y = 0.0f;
  std::vector<std::pair<float, float>> correction(frame_count);
  for (std::size_t index = 0; index < frame_count; ++index) {
    double sum_x = 0.0;
    double sum_y = 0.0;
    const auto signed_index = static_cast<std::int64_t>(index);
    for (int offset = -radius; offset <= radius; ++offset) {
      const auto sample = std::clamp<std::int64_t>(signed_index + offset, 0,
                                                   static_cast<std::int64_t>(frame_count - 1));
      sum_x += path[static_cast<std::size_t>(sample)].first;
      sum_y += path[static_cast<std::size_t>(sample)].second;
    }
    const auto count = static_cast<double>(radius * 2 + 1);
    correction[index] = {static_cast<float>(sum_x / count - path[index].first),
                         static_cast<float>(sum_y / count - path[index].second)};
  }
  // Anchor the first frame so applying stabilization never creates an initial
  // jump relative to the edit that was approved before analysis.
  const auto anchor = correction.front();
  for (std::size_t index = 0; index < frame_count; ++index) {
    correction[index].first -= anchor.first;
    correction[index].second -= anchor.second;
    maximum_x = std::max(maximum_x, std::abs(correction[index].first));
    maximum_y = std::max(maximum_y, std::abs(correction[index].second));
    result.samples.push_back({time::RationalTime::FromFrames(static_cast<std::int64_t>(index), frame_rate),
                              correction[index].first, correction[index].second});
  }

  const auto safe_width = std::max(1.0f, result.source_width - maximum_x * 2.0f);
  const auto safe_height = std::max(1.0f, result.source_height - maximum_y * 2.0f);
  const auto required = std::max(result.source_width / safe_width, result.source_height / safe_height) *
                        std::max(config.crop_safety, 1.0f);
  result.auto_scale = std::clamp(required, 1.0f, std::max(config.maximum_scale, 1.0f));
  return result;
}

timeline::Effect MakeStabilizerEffect(const StabilizationResult& result, std::string effect_id, std::int64_t order) {
  if (result.samples.empty()) throw std::invalid_argument("Cannot build a stabilizer effect from no samples");
  timeline::Effect effect;
  effect.id = std::move(effect_id);
  effect.effect_type = "stabilizer";
  effect.order = order;

  timeline::Parameter position;
  position.id = effect.id + ":position";
  position.name = "position";
  std::vector<anim::Keyframe> keys;
  keys.reserve(result.samples.size());
  for (const auto& sample : result.samples) {
    keys.push_back({sample.time, anim::Value::Vec2(sample.correction_x, sample.correction_y),
                    anim::Interpolation::Linear, {}, {}});
  }
  position.value = anim::AnimatedValue(std::move(keys));
  effect.parameters.push_back(std::move(position));

  timeline::Parameter scale;
  scale.id = effect.id + ":scale";
  scale.name = "scale";
  scale.value = anim::AnimatedValue(anim::Value::Vec2(result.auto_scale * 100.0f, result.auto_scale * 100.0f));
  effect.parameters.push_back(std::move(scale));
  return effect;
}

}  // namespace cutline::render

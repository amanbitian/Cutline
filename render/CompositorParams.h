#pragma once

// What the compositor reads out of a clip's effect stack to place and grade its picture: the motion transform, the crop
// and the primary grade. Shared by the CPU compositor and the GPU one so both read a clip the same way.

#include "render/Layer.h"

#include <cmath>
#include <string>
#include <vector>

namespace cutline::render {

// Geometry for the motion effect, resolved into the inverse mapping the
// rasteriser needs (destination pixel -> source pixel).
struct Transform2D final {
  float scale_x{1.0f};
  float scale_y{1.0f};
  float rotation_radians{0.0f};
  float translate_x{0.0f};
  float translate_y{0.0f};
  float anchor_x{0.5f};
  float anchor_y{0.5f};
  bool identity{true};
};

[[nodiscard]] inline Transform2D ReadTransform(const SampledEffect& effect) {
  Transform2D transform;
  // Scale is a percentage, as every NLE inspector shows it.
  const auto scale = Vec2Parameter(effect, "scale", 100.0f, 100.0f);
  transform.scale_x = scale[0] / 100.0f;
  transform.scale_y = scale[1] / 100.0f;
  const auto position = Vec2Parameter(effect, "position", 0.0f, 0.0f);
  transform.translate_x = position[0];
  transform.translate_y = position[1];
  transform.rotation_radians = ScalarParameter(effect, "rotation", 0.0f) * 3.14159265358979323846f / 180.0f;
  const auto anchor = Vec2Parameter(effect, "anchor", 0.5f, 0.5f);
  transform.anchor_x = anchor[0];
  transform.anchor_y = anchor[1];
  transform.identity = std::abs(transform.scale_x - 1.0f) < 1e-6f && std::abs(transform.scale_y - 1.0f) < 1e-6f &&
                       std::abs(transform.rotation_radians) < 1e-6f && std::abs(transform.translate_x) < 1e-6f &&
                       std::abs(transform.translate_y) < 1e-6f;
  return transform;
}

struct CropRect final {
  float left{0.0f};
  float top{0.0f};
  float right{0.0f};
  float bottom{0.0f};
  [[nodiscard]] bool empty() const { return left <= 0.0f && top <= 0.0f && right <= 0.0f && bottom <= 0.0f; }
};

[[nodiscard]] inline CropRect ReadCrop(const SampledEffect& effect) {
  // Fractions of the source, matching how a crop effect is normally authored.
  return {ScalarParameter(effect, "left", 0.0f), ScalarParameter(effect, "top", 0.0f),
          ScalarParameter(effect, "right", 0.0f), ScalarParameter(effect, "bottom", 0.0f)};
}

// A basic primary grade. Applied to premultiplied values, so each operation
// un-premultiplies, works on colour, and re-premultiplies -- otherwise a
// semi-transparent pixel would be graded differently from an opaque one.
struct Grade final {
  float exposure_stops{0.0f};
  float contrast{1.0f};
  float saturation{1.0f};
  float temperature{0.0f};
  [[nodiscard]] bool identity() const {
    return std::abs(exposure_stops) < 1e-6f && std::abs(contrast - 1.0f) < 1e-6f &&
           std::abs(saturation - 1.0f) < 1e-6f && std::abs(temperature) < 1e-6f;
  }
};

[[nodiscard]] inline Grade ReadGrade(const SampledEffect& effect) {
  Grade grade;
  grade.exposure_stops = ScalarParameter(effect, "exposure", 0.0f);
  // Contrast and saturation are percentages in the inspector; 100 is neutral.
  grade.contrast = ScalarParameter(effect, "contrast", 100.0f) / 100.0f;
  grade.saturation = ScalarParameter(effect, "saturation", 100.0f) / 100.0f;
  grade.temperature = ScalarParameter(effect, "temperature", 0.0f);
  return grade;
}

[[nodiscard]] inline Transform2D TransformFor(const std::vector<SampledEffect>& effects) {
  Transform2D result;
  bool has_primary = false;
  for (const auto& effect : effects) {
    if (!has_primary && (effect.effect_type == "motion" || effect.effect_type == "transform")) {
      result = ReadTransform(effect);
      has_primary = true;
    }
  }
  // Stabilizer analysis produces correction keyframes in output pixels plus an
  // auto-crop scale. Compose those with the clip's ordinary Motion effect so
  // the user's framing remains editable and removing stabilizer is reversible.
  for (const auto& effect : effects) {
    if (effect.effect_type != "stabilizer") continue;
    const auto correction = ReadTransform(effect);
    result.scale_x *= correction.scale_x;
    result.scale_y *= correction.scale_y;
    result.rotation_radians += correction.rotation_radians;
    result.translate_x += correction.translate_x;
    result.translate_y += correction.translate_y;
  }
  result.identity = std::abs(result.scale_x - 1.0f) < 1e-6f && std::abs(result.scale_y - 1.0f) < 1e-6f &&
                    std::abs(result.rotation_radians) < 1e-6f && std::abs(result.translate_x) < 1e-6f &&
                    std::abs(result.translate_y) < 1e-6f;
  return result;
}

[[nodiscard]] inline CropRect CropFor(const std::vector<SampledEffect>& effects) {
  for (const auto& effect : effects) {
    if (effect.effect_type == "crop") return ReadCrop(effect);
  }
  return {};
}

[[nodiscard]] inline const SampledEffect* FindEffect(const std::vector<SampledEffect>& effects, const std::string& type) {
  for (const auto& effect : effects) {
    if (effect.effect_type == type) return &effect;
  }
  return nullptr;
}


}  // namespace cutline::render

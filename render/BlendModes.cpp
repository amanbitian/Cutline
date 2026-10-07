#include "render/BlendModes.h"

#include <algorithm>
#include <cmath>

namespace cutline::render {
namespace {

[[nodiscard]] float Channel(float backdrop, float source, BlendMode mode) {
  switch (mode) {
    case BlendMode::Normal: return source;
    case BlendMode::Multiply: return backdrop * source;
    case BlendMode::Screen: return backdrop + source - backdrop * source;
    case BlendMode::Overlay:
      return backdrop <= 0.5f ? 2.0f * backdrop * source
                              : 1.0f - 2.0f * (1.0f - backdrop) * (1.0f - source);
    case BlendMode::Darken: return std::min(backdrop, source);
    case BlendMode::Lighten: return std::max(backdrop, source);
    case BlendMode::ColorDodge:
      return source >= 1.0f ? 1.0f : std::min(1.0f, backdrop / (1.0f - source));
    case BlendMode::ColorBurn:
      return source <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - backdrop) / source);
    case BlendMode::HardLight:
      return source <= 0.5f ? 2.0f * backdrop * source
                            : 1.0f - 2.0f * (1.0f - backdrop) * (1.0f - source);
    case BlendMode::SoftLight: {
      const auto d = backdrop <= 0.25f ? ((16.0f * backdrop - 12.0f) * backdrop + 4.0f) * backdrop
                                       : std::sqrt(std::max(0.0f, backdrop));
      return source <= 0.5f ? backdrop - (1.0f - 2.0f * source) * backdrop * (1.0f - backdrop)
                            : backdrop + (2.0f * source - 1.0f) * (d - backdrop);
    }
    case BlendMode::Difference: return std::abs(backdrop - source);
    case BlendMode::Exclusion: return backdrop + source - 2.0f * backdrop * source;
  }
  return source;
}

}  // namespace

BlendMode BlendModeFromIndex(int index) noexcept {
  if (index < static_cast<int>(BlendMode::Normal) || index > static_cast<int>(BlendMode::Exclusion)) {
    return BlendMode::Normal;
  }
  return static_cast<BlendMode>(index);
}

std::string_view BlendModeName(BlendMode mode) noexcept {
  switch (mode) {
    case BlendMode::Normal: return "normal";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Screen: return "screen";
    case BlendMode::Overlay: return "overlay";
    case BlendMode::Darken: return "darken";
    case BlendMode::Lighten: return "lighten";
    case BlendMode::ColorDodge: return "color_dodge";
    case BlendMode::ColorBurn: return "color_burn";
    case BlendMode::HardLight: return "hard_light";
    case BlendMode::SoftLight: return "soft_light";
    case BlendMode::Difference: return "difference";
    case BlendMode::Exclusion: return "exclusion";
  }
  return "normal";
}

void CompositeBlend(const Layer& source, Layer& destination, BlendMode mode, float opacity) {
  if (source.empty()) return;
  const auto strength = std::clamp(opacity, 0.0f, 1.0f);
  if (strength <= 0.0f) return;

  for (int y = source.min_y(); y <= source.max_y(); ++y) {
    for (int x = source.min_x(); x <= source.max_x(); ++x) {
      const auto& top = source.at(x, y);
      if (top.a <= 0.0f) continue;
      auto& bottom = destination.at(x, y);
      const auto source_alpha = std::clamp(top.a * strength, 0.0f, 1.0f);
      const auto backdrop_alpha = std::clamp(bottom.a, 0.0f, 1.0f);
      const auto source_inverse = top.a > 0.0f ? 1.0f / top.a : 0.0f;
      const auto backdrop_inverse = backdrop_alpha > 0.0f ? 1.0f / backdrop_alpha : 0.0f;

      const float source_rgb[3] = {top.r * source_inverse, top.g * source_inverse, top.b * source_inverse};
      const float backdrop_rgb[3] = {bottom.r * backdrop_inverse, bottom.g * backdrop_inverse,
                                     bottom.b * backdrop_inverse};
      float* output[3] = {&bottom.r, &bottom.g, &bottom.b};
      for (int channel = 0; channel < 3; ++channel) {
        const auto blended = Channel(backdrop_rgb[channel], source_rgb[channel], mode);
        *output[channel] = (1.0f - source_alpha) * (backdrop_rgb[channel] * backdrop_alpha) +
                           (1.0f - backdrop_alpha) * (source_rgb[channel] * source_alpha) +
                           source_alpha * backdrop_alpha * blended;
      }
      bottom.a = source_alpha + backdrop_alpha * (1.0f - source_alpha);
    }
  }
  destination.MarkDirty(source.min_x(), source.min_y(), source.max_x(), source.max_y());
}

}  // namespace cutline::render

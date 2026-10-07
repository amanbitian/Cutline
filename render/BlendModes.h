#pragma once

// Photoshop/NLE style blend compositing in the compositor's premultiplied
// working space. The CPU implementation is the reference contract that a GPU
// backend must match.

#include "render/Layer.h"

#include <string_view>

namespace cutline::render {

enum class BlendMode {
  Normal = 0,
  Multiply,
  Screen,
  Overlay,
  Darken,
  Lighten,
  ColorDodge,
  ColorBurn,
  HardLight,
  SoftLight,
  Difference,
  Exclusion,
};

[[nodiscard]] BlendMode BlendModeFromIndex(int index) noexcept;
[[nodiscard]] std::string_view BlendModeName(BlendMode mode) noexcept;

// Composites `source` over `destination`. Opacity scales the complete source
// contribution. Both buffers contain premultiplied RGBA.
void CompositeBlend(const Layer& source, Layer& destination, BlendMode mode, float opacity = 1.0f);

}  // namespace cutline::render

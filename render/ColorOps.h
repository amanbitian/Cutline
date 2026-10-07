#pragma once

// The colour tools as a graphics card runs them: each tool reduced to the constants its shader needs.
//
// The software compositor (render/ColorEffects.cpp, render/Filters.cpp) is the definition of what each tool does; the
// card's shader (render/D3D11Compositor.cpp) must give the same picture. To keep the two from drifting, the constants
// are derived here, beside the software code that reads the same parameters, with the same defaults, the same clamps and
// the same rule for when a tool is the identity and leaves the picture alone.

#include "render/Layer.h"

#include <optional>
#include <vector>

namespace cutline::render {

// Operation numbers in the shader's operation block (kinds 1 and 2, opacity and basic grade, and 3, a LUT, are made by
// the compositor itself).
enum class GpuColorKind : int {
  ColorWheels = 4,
  Curves = 5,
  ChannelMixer = 6,
  Tint = 7,
  BlackAndWhite = 8,
  ColorAdjust = 9,
  HueCurves = 10,
  HslSecondary = 11,
};

struct GpuColorOp final {
  GpuColorKind kind{GpuColorKind::ColorWheels};
  // The operation block: six float4. [0].x is set by the compositor to the kind. Hue curves and HSL secondary use all
  // twenty-four values in order (the kind first), the others their own rows (see the shader).
  float block[6][4]{};
  // For curves: five tables of 256 entries (master, red, green, blue, luma), the ones in use first marked in block[0].y.
  std::vector<float> tables;
};

// Whether the effect type is one a card can run through DescribeColorOp.
[[nodiscard]] bool IsGpuColorEffect(const std::string& effect_type);
// The shader constants for the effect, or nothing when the software compositor would leave the picture untouched (a
// neutral tool): the caller then draws nothing for it, as the software does.
[[nodiscard]] std::optional<GpuColorOp> DescribeColorOp(const SampledEffect& effect);

}  // namespace cutline::render

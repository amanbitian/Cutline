#pragma once

// Pixel effects beyond the compositor's original set: production filters (FX-003),
// keying (KEY-001) and grading tools (COLOR-002).
//
// Each is a pure function over a premultiplied layer: no I/O, no hidden state, and
// the same output for the same parameters. That is what lets preview and export
// share them, and what lets the tests pin their results to numbers.
//
// Conventions:
//   * Colour maths is done on straight (un-premultiplied) values and written back
//     premultiplied, so a half-transparent edge keeps its hue.
//   * Where an effect reaches beyond the layer's written rectangle (blur, glow, a
//     shadow) it widens the dirty rectangle, clamped to the canvas.
//   * Gaussian blur is three successive box blurs sized to the requested sigma. The
//     result is within a few per cent of a true Gaussian and costs the same at any
//     radius, which a direct kernel does not at 1080p.

#include "render/Layer.h"

#include <string>
#include <vector>

namespace cutline::render {

// Applies the effect when it is one of the effects defined here and returns true; returns
// false, touching nothing, for any other effect type.
[[nodiscard]] bool ApplyFilterEffect(Layer& layer, Layer& scratch, const SampledEffect& effect);
[[nodiscard]] bool ApplyColorEffect(Layer& layer, Layer& scratch, const SampledEffect& effect);

// The effect types ApplyFilterEffect and ApplyColorEffect handle.
[[nodiscard]] const std::vector<std::string>& FilterEffectTypes();

// Building blocks shared by the effects and exposed for the tests.
//
// Blurs the layer in place with a gaussian of the given sigma, in pixels (0 to 128).
void GaussianBlur(Layer& layer, float sigma);

// A tone curve through (0,0), (0.25,a), (0.5,b), (0.75,c), (1,1): the monotone cubic
// (Fritsch-Carlson) interpolant, sampled into `table` (256 entries, input 0..1).
void BuildCurveTable(float a, float b, float c, float* table);

}  // namespace cutline::render

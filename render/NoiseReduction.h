#pragma once

// Video noise reduction (FX-002): a temporal pass that averages a pixel with the same pixel in the
// neighbouring frames where they agree, then a spatial pass that smooths what remains without
// crossing edges.
//
// Temporal. For each pixel, each neighbouring frame contributes with a weight that falls away as
// the 3 x 3 neighbourhood of that pixel in the neighbour stops looking like the neighbourhood in
// the current frame. Where nothing moved the weights are near one and the noise averages out; where
// something moved they are near zero and the neighbour is left out, so there is no ghost. The
// window is at most two frames either side, which bounds the memory it needs to four extra layers.
//
// Spatial. A bilateral filter: each pixel becomes the average of the pixels around it, weighted by
// how close they are and how close their values are, so a flat area is smoothed and an edge is not.
// It runs on luma and chroma separately; chroma noise is less visible and is smoothed over a wider
// area, guided by the luma so colour does not bleed across an edge. The `detail` setting narrows
// what counts as "close in value" and so keeps more texture at the price of less smoothing.
//
// Both passes work on straight (un-premultiplied) colour and leave alpha alone.

#include "render/Layer.h"

#include <vector>

namespace cutline::render {

struct NoiseSettings final {
  float luma{0.0f};      // 0..1
  float chroma{0.0f};    // 0..1
  float temporal{0.0f};  // 0..1
  float detail{0.5f};    // 0..1, how much texture to protect
  int radius{1};         // frames either side to use, 1 or 2

  [[nodiscard]] bool spatial() const { return luma > 1e-6f || chroma > 1e-6f; }
  [[nodiscard]] bool active() const { return spatial() || temporal > 1e-6f; }
};

[[nodiscard]] NoiseSettings ReadNoiseSettings(const SampledEffect& effect);

// Reduces the noise in `layer`. `neighbours` are the surrounding frames drawn the same way as the
// layer (any may be absent), and are ignored when temporal is zero.
void ReduceNoise(Layer& layer, Layer& scratch, const NoiseSettings& settings, const std::vector<const Layer*>& neighbours);

}  // namespace cutline::render

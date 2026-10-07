#pragma once

// General grid warp plus a fast scan-line model for rolling-shutter repair.
// Offsets use inverse mapping: each output pixel samples input at output+offset.

#include "render/Layer.h"

#include <vector>

namespace cutline::render {

struct MeshOffset final {
  float x{0.0f};
  float y{0.0f};
};

struct WarpMesh final {
  int columns{0};
  int rows{0};
  std::vector<MeshOffset> offsets;

  [[nodiscard]] bool valid() const noexcept {
    return columns >= 2 && rows >= 2 && offsets.size() == static_cast<std::size_t>(columns * rows);
  }
};

struct RollingShutterSettings final {
  float horizontal{0.0f};
  float vertical{0.0f};
  float rotation_degrees{0.0f};
  float curve{0.0f};
  bool bottom_to_top{false};
};

void ApplyMeshWarp(Layer& layer, Layer& scratch, const WarpMesh& mesh);
void ApplyRollingShutter(Layer& layer, Layer& scratch, const RollingShutterSettings& settings);

}  // namespace cutline::render

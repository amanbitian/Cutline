#pragma once

// A decoded picture that stays on the GPU.
//
// A hardware decoder (Direct3D 11 video acceleration through FFmpeg) writes its pictures into planar video textures
// on the same device the compositor draws with. Handing such a picture over as a VideoFrame would mean reading it
// back; this carries a reference to the texture instead, and the compositor reads the planes where they are. The
// reference keeps the decoder's surface from being reused until the compositor is done with it.

#include "core/time/RationalTime.h"

#include <memory>

namespace cutline::media {

enum class DeviceFormat {
  Nv12,  // 8-bit 4:2:0: a luma plane and an interleaved chroma plane
  P010,  // 10-bit 4:2:0, stored in the high bits of 16
};

struct DeviceFrame final {
  // The native texture (an ID3D11Texture2D*, usually an array whose slice is `array_index`) and the device it lives on.
  void* texture{nullptr};
  void* device{nullptr};
  int array_index{0};
  int width{0};
  int height{0};
  DeviceFormat format{DeviceFormat::Nv12};
  // How to turn the planes into colour.
  bool full_range{false};
  bool bt709{true};  // false: BT.601
  bool bt2020{false};
  time::RationalTime presentation_time;
  time::RationalTime duration;
  // Whatever keeps the surface valid (an AVFrame reference); released when the last holder lets go.
  std::shared_ptr<void> keep_alive;
  [[nodiscard]] bool valid() const noexcept { return texture != nullptr && width > 0 && height > 0; }
};

}  // namespace cutline::media

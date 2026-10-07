#pragma once

// The GPU compositor for Windows, on Direct3D 11 compute shaders.
//
// It renders what the software compositor (render/Compositor.h) renders, for the plans it can: layers per track,
// bottom to top, with the motion transform, crop, opacity and primary grade, solid generators, adjustment clips,
// dissolves and sequence-level effects, premultiplied source-over in half float. Anything else in a plan (masks,
// blur and the other filters, blend modes, graphics, captions, colour management, optical-flow interpolation) makes
// `Supports` say no and the caller renders that frame with the software compositor, which stays the reference:
// every number in the shaders is the software compositor's, and tests/native/gpu_tests.cpp compares the two picture
// by picture.
//
// Pictures reach the GPU in one of two ways. A frame the decoder handed over in memory is uploaded into a pooled
// texture. A frame a hardware decoder left on the device (render/DeviceFrame.h) is read where it is, as video
// planes, and converted to colour in the same shader that places it: no upload and no copy.

#include "render/Compositor.h"
#include "render/DeviceFrame.h"
#include "render/GpuDevice.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cutline::render::gpu {

struct GpuStatistics final {
  double total_ms{0};      // from the call to the picture being in memory
  double upload_ms{0};     // copying pictures to the device
  double gpu_ms{0};        // the device executing the passes (a timestamp query)
  double readback_ms{0};   // waiting for and copying the finished picture back
  int sources_uploaded{0};
  int sources_on_device{0};  // read in place from a hardware decoder
  int passes{0};
  std::size_t texture_bytes{0};  // device memory this compositor holds
};

// Thrown by Compose when a picture turns out to need something only the software compositor does (a colour conversion
// between spaces), after Supports said yes because that could not be known until the picture arrived. The caller renders
// the frame with the software compositor.
class Unsupported final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class D3D11Compositor final {
 public:
  struct Options final {
    // Choose this adapter (an id from EnumerateDevices) rather than the best one.
    std::string adapter_id;
    // Allow the software rasteriser (WARP) when no hardware adapter qualifies. For machines without a GPU and for tests.
    bool allow_software{false};
  };

  // Every adapter the system lists, as the selector sees it.
  [[nodiscard]] static std::vector<DeviceDescriptor> EnumerateDevices();
  // Null when there is no Direct3D 11 device to be had (a non-Windows build, no adapter, a driver that refuses); `reason`
  // then says why.
  [[nodiscard]] static std::unique_ptr<D3D11Compositor> Create(const Options& options = {}, std::string* reason = nullptr);
  ~D3D11Compositor();
  D3D11Compositor(const D3D11Compositor&) = delete;
  D3D11Compositor& operator=(const D3D11Compositor&) = delete;

  [[nodiscard]] const DeviceDescriptor& device() const;
  // The ID3D11Device, for decoders that must put their pictures on this device (render/DeviceFrame.h).
  [[nodiscard]] void* native_device() const;

  // Whether this compositor can render the plan exactly as the software one would. When it cannot, `reason` names the
  // first thing in the way.
  [[nodiscard]] bool Supports(const timeline::PlaybackPlan& plan, const CompositorConfig& config, std::string* reason = nullptr) const;

  // Renders a plan `Supports` accepted. `resolve` supplies in-memory pictures; `device_resolve`, if given, is asked
  // first and supplies pictures that are already on the device. One caller at a time.
  [[nodiscard]] media::VideoFrame Compose(const timeline::PlaybackPlan& plan, const CompositorConfig& config, const FrameResolver& resolve,
                                          Statistics& statistics, GpuStatistics* gpu = nullptr, const DeviceFrameResolver& device_resolve = {});

  // Drops pooled textures and targets (a size change, a memory squeeze).
  void ReleaseResources();

 private:
  D3D11Compositor();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::render::gpu

#pragma once

// The compositor's view of a picture that stays on the GPU (media/DeviceFrame.h).

#include "media/DeviceFrame.h"
#include "timeline/TimelineCompiler.h"

#include <functional>

namespace cutline::render::gpu {

using media::DeviceFormat;
using media::DeviceFrame;

// Supplies the device-resident picture for a source request, or an invalid one when the request must be served from memory.
using DeviceFrameResolver = std::function<DeviceFrame(const timeline::SourceRequest&)>;

}  // namespace cutline::render::gpu

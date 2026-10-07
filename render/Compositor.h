#pragma once

// Software compositor: turns a PlaybackPlan plus decoded frames into one output
// picture.
//
// This is the reference implementation of Cutline's render semantics. A GPU
// backend will replace it for interactive playback, but this one defines what
// "correct" means, and the golden-frame tests pin it. Keeping a CPU path is not
// a stopgap: an export that must be bit-reproducible, and a test suite that must
// run on any machine, both need a renderer that does not depend on a driver.
//
// Composition model:
//   * One layer per track, bottom to top by track order.
//   * Layers are composited with premultiplied alpha, source-over.
//   * A track carrying an active transition contributes a single layer: its two
//     sides mixed by the transition's progress.
//   * An adjustment clip has no source of its own; its effect stack is applied
//     to everything already composited beneath it.
//   * Nested sequences are resolved by the caller's resolver, which composes
//     them recursively. The compositor itself handles one sequence level, so
//     nesting depth does not complicate this code.

#include "media/VideoFrame.h"
#include "render/ColorManagement.h"
#include "render/FlowCache.h"
#include "timeline/TimelineCompiler.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cutline::render {

// Supplies the decoded picture for a source request, or nullptr when the frame
// is not available (a missing file, a decode error, or a still-loading cache).
// A missing frame composites as nothing rather than failing the whole frame, so
// one offline clip does not black out the monitor.
using FrameResolver = std::function<const media::VideoFrame*(const timeline::SourceRequest&)>;

struct CompositorConfig final {
  // Output size. Defaults to the plan's sequence size when left at zero.
  int width{0};
  int height{0};
  // Background shown wherever nothing is composited.
  float background_red{0.0f};
  float background_green{0.0f};
  float background_blue{0.0f};
  // Output format. Rgba8 for display and export; RgbaF32 to keep headroom when
  // the result feeds another stage.
  media::PixelFormat output_format{media::PixelFormat::Rgba8};
  // Base directory for relative asset references such as .cube LUTs. Empty
  // means the process working directory.
  std::string asset_root;
  // Where optical-flow analysis is kept, so the same pair of source frames is analysed once whether
  // the preview, an export or a background pass asks. Empty: analysed every time it is needed.
  std::shared_ptr<FlowCache> flow_cache;
};

// Effects the compositor implements natively. Anything else is passed over, and
// `Statistics::skipped_effects` records it so a caller can tell the difference
// between "applied" and "silently ignored".
[[nodiscard]] bool IsBuiltInEffect(const std::string& effect_type);
[[nodiscard]] std::vector<std::string> BuiltInEffectTypes();

struct Statistics final {
  int layers_composited{0};
  int transitions_mixed{0};
  int adjustment_layers{0};
  int missing_frames{0};
  std::vector<std::string> skipped_effects;
  // Recoverable asset or parse failures. The frame still renders with that
  // effect bypassed, so one bad LUT cannot take down playback or export.
  std::vector<std::string> effect_errors;
  // Colour management (render version 3 and later): pixels converted from a frame's own colour
  // space into the working space, and whether the finished picture was converted for display.
  // Noise reduction: neighbouring frames drawn and used by the temporal pass.
  int noise_frames_used{0};
  // Fractional-frame synthesis selected by a time-remapped clip.
  int interpolated_frames{0};
  int optical_flow_frames{0};
  // Of those, how many reused an analysis already done (in memory or on disk).
  int optical_flow_reused{0};
  int graphics_elements_drawn{0};
  // Captions drawn into the picture.
  int captions_drawn{0};
  int color_input_conversions{0};
  bool color_output_conversion{false};
};

class Compositor final {
 public:
  explicit Compositor(CompositorConfig config = {});
  ~Compositor();
  // Moveable, not copyable: the scratch buffers are large and sharing them
  // between two compositors would make neither thread-safe.
  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;
  Compositor(Compositor&&) noexcept;
  Compositor& operator=(Compositor&&) noexcept;

  [[nodiscard]] media::VideoFrame Compose(const timeline::PlaybackPlan& plan, const FrameResolver& resolve) const;
  // Same, reporting what it did. Used by tests and by the demo.
  [[nodiscard]] media::VideoFrame Compose(const timeline::PlaybackPlan& plan, const FrameResolver& resolve,
                                          Statistics& statistics) const;

  [[nodiscard]] const CompositorConfig& config() const noexcept { return config_; }

 private:
  // Reusable layer buffers. A 1920x1080 layer is 33 MB, so allocating one per
  // clip per frame dominated the render cost; these persist across frames.
  // Holding them means one Compositor belongs to one render thread.
  struct Workspace;
  CompositorConfig config_;
  std::unique_ptr<Workspace> workspace_;
};

}  // namespace cutline::render

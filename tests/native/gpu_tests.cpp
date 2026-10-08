// The GPU compositor against the software one: the same plans rendered both ways must give the same pictures, and
// what the GPU cannot render must be refused so the software compositor does it. These need a Direct3D 11 adapter; with
// no hardware adapter they use the software rasteriser (WARP) so the arithmetic is still checked, and only the timing
// test is skipped.

#include "tests/native/TestHarness.h"

#include "effects/EffectRegistry.h"
#include "media/Providers.h"
#include "media/SyntheticSource.h"
#include "playback/PlaybackEngine.h"
#include "media/Source.h"
#include "media/VideoFrame.h"
#include "render/Compositor.h"
#include "render/D3D11Compositor.h"
#include "timeline/TimelineCompiler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <vector>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace model = cutline::model;
namespace gpu = cutline::render::gpu;
using cutline::anim::Value;
using cutline::media::PixelFormat;
using cutline::media::VideoFrame;
using cutline::render::Compositor;
using cutline::render::CompositorConfig;
using cutline::render::Statistics;
using cutline::time::RationalTime;
using cutline::timeline::Clip;
using cutline::timeline::Effect;
using cutline::timeline::Parameter;
using cutline::timeline::Sequence;
using cutline::timeline::SourceRequest;
using cutline::timeline::TimelineCompiler;
using cutline::timeline::Track;
using cutline::timeline::Transition;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

Clip MakeClip(std::string id, std::string source, std::int64_t start, std::int64_t duration) {
  Clip clip;
  clip.id = std::move(id);
  clip.source_kind = model::SourceKind::Media;
  clip.source_id = std::move(source);
  clip.source_in = Seconds(0);
  clip.source_out = Seconds(duration);
  clip.timeline_start = Seconds(start);
  return clip;
}

Track MakeTrack(std::string id, std::int64_t order) {
  Track track;
  track.id = std::move(id);
  track.kind = model::TrackKind::Video;
  track.order = order;
  return track;
}

Sequence MakeSequence(int width, int height) {
  Sequence sequence;
  sequence.id = "seq-1";
  sequence.name = "Main";
  sequence.frame_rate = cutline::time::kFrameRate25;
  sequence.width = width;
  sequence.height = height;
  sequence.sample_rate = 48000;
  return sequence;
}

Effect MakeEffect(std::string id, std::string type, std::vector<std::pair<std::string, Value>> parameters) {
  Effect effect;
  effect.id = std::move(id);
  effect.effect_type = std::move(type);
  for (auto& [name, value] : parameters) {
    Parameter parameter;
    parameter.id = effect.id + ":" + name;
    parameter.name = name;
    parameter.value = cutline::anim::AnimatedValue(value);
    effect.parameters.push_back(std::move(parameter));
  }
  return effect;
}

void Finalise(Sequence& sequence) {
  for (auto& track : sequence.tracks) {
    for (auto& clip : track.clips) {
      clip.start_ticks = clip.timeline_start.ToTicks();
      clip.end_ticks = clip.end().ToTicks();
    }
    for (auto& transition : track.transitions) {
      transition.start_ticks = transition.timeline_start.ToTicks();
      transition.end_ticks = transition.timeline_start.Add(transition.duration).ToTicks();
    }
  }
}

// A picture with something in every part of it: smooth gradients, hard edges, a checker, and a half-transparent band, so a
// wrong sampling position, premultiplication or edge rule shows up as a difference.
VideoFrame TexturedFrame(int width, int height, PixelFormat format, int seed) {
  auto frame = VideoFrame::Allocate(PixelFormat::RgbaF32, width, height);
  for (int y = 0; y < height; ++y) {
    auto* row = frame.row_f32(y);
    for (int x = 0; x < width; ++x) {
      auto* texel = row + static_cast<std::size_t>(x) * 4;
      const float u = static_cast<float>(x) / static_cast<float>(width - 1), v = static_cast<float>(y) / static_cast<float>(height - 1);
      const bool checker = ((x / 6) + (y / 6) + seed) % 2 == 0;
      texel[0] = checker ? 0.2f + 0.7f * u : 0.9f - 0.6f * v;
      texel[1] = 0.1f + 0.8f * (seed % 2 == 0 ? v : u);
      texel[2] = (x > width / 2) != (y > height / 2) ? 0.85f : 0.15f + 0.1f * static_cast<float>(seed % 3);
      texel[3] = (y > height / 3 && y < height / 3 + height / 8) ? 0.5f : 1.0f;
    }
  }
  return format == PixelFormat::RgbaF32 ? std::move(frame) : cutline::media::ConvertFrame(frame, format);
}

class Frames final {
 public:
  void Add(const std::string& clip_id, VideoFrame frame) { frames_.emplace(clip_id, std::move(frame)); }
  [[nodiscard]] cutline::render::FrameResolver Resolver() const {
    return [this](const SourceRequest& request) -> const VideoFrame* {
      const auto found = frames_.find(request.clip_id);
      return found == frames_.end() ? nullptr : &found->second;
    };
  }
 private:
  std::map<std::string, VideoFrame> frames_;
};

struct Pair final {
  VideoFrame cpu;
  VideoFrame gpu;
  int different{0};   // pixels with a channel more than two levels apart
  int largest{0};
  double mean{0};
  int total{0};
  // How far the effects moved the picture from the same picture with none (mean levels): a tool that does nothing
  // agrees with itself, so this is what makes an agreement mean something.
  double change{0};
};

// The GPU compositor, or nothing when the machine has no Direct3D 11 at all (even the software rasteriser).
gpu::D3D11Compositor* Device() {
  static std::unique_ptr<gpu::D3D11Compositor> compositor = [] {
    gpu::D3D11Compositor::Options options;
    options.allow_software = true;
    return gpu::D3D11Compositor::Create(options);
  }();
  return compositor.get();
}

bool HaveHardware() { return Device() != nullptr && Device()->device().vendor != gpu::Vendor::Software; }

Pair Render(const Sequence& sequence, std::int64_t at_seconds, const Frames& frames, PixelFormat output = PixelFormat::Rgba8) {
  CompositorConfig config;
  config.output_format = output;
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(at_seconds));
  std::string reason;
  CHECK(Device()->Supports(plan, config, &reason));
  Statistics cpu_stats, gpu_stats;
  Pair pair;
  pair.cpu = Compositor(config).Compose(plan, frames.Resolver(), cpu_stats);
  pair.gpu = Device()->Compose(plan, config, frames.Resolver(), gpu_stats);
  CHECK(pair.cpu.width() == pair.gpu.width() && pair.cpu.height() == pair.gpu.height() && pair.cpu.format() == pair.gpu.format());
  CHECK_EQ(cpu_stats.layers_composited, gpu_stats.layers_composited);
  CHECK_EQ(cpu_stats.transitions_mixed, gpu_stats.transitions_mixed);
  CHECK_EQ(cpu_stats.adjustment_layers, gpu_stats.adjustment_layers);
  CHECK_EQ(cpu_stats.missing_frames, gpu_stats.missing_frames);
  double sum = 0;
  for (int y = 0; y < pair.cpu.height(); ++y) {
    for (int x = 0; x < pair.cpu.width(); ++x) {
      int worst = 0;
      for (int c = 0; c < 4; ++c) {
        int a, b;
        if (output == PixelFormat::Rgba8) {
          a = pair.cpu.row_u8(y)[x * 4 + c];
          b = pair.gpu.row_u8(y)[x * 4 + c];
        } else {
          a = static_cast<int>(std::lround(pair.cpu.row_f32(y)[x * 4 + c] * 255.0f));
          b = static_cast<int>(std::lround(pair.gpu.row_f32(y)[x * 4 + c] * 255.0f));
        }
        worst = std::max(worst, std::abs(a - b));
        sum += std::abs(a - b);
      }
      pair.largest = std::max(pair.largest, worst);
      if (worst > 2) ++pair.different;
      ++pair.total;
    }
  }
  pair.mean = sum / (static_cast<double>(pair.total) * 4.0);
  return pair;
}

// Pictures agree when nearly every pixel is within two levels of 255 and none is wildly off. The odd pixel on a rotated
// edge may fall either side of a half-pixel boundary because a GPU divides slightly differently, which is why a small
// fraction is allowed to differ.
void CheckParity(const Pair& pair, const char* what) {
  const double fraction = static_cast<double>(pair.different) / static_cast<double>(pair.total);
  if (fraction > 0.002 || pair.mean > 0.5) {
    std::fprintf(stderr, "    %s: %d of %d pixels differ by more than 2 levels (largest %d, mean %.3f)\n", what, pair.different, pair.total, pair.largest, pair.mean);
  }
  CHECK(fraction <= 0.002);
  CHECK(pair.mean <= 0.5);   // half-float intermediates flip the odd channel by one level
}

}  // namespace

CUTLINE_TEST(TheAdaptersAreListedWithCapabilitiesAndOneIsChosenByTheSelector) {
  const auto devices = gpu::D3D11Compositor::EnumerateDevices();
  SKIP_INAPPLICABLE(!devices.empty(), "Windows lists no graphics adapter");
  for (const auto& device : devices) {
    CHECK(device.backend == gpu::Backend::D3D11 && !device.id.empty() && !device.name.empty());
    CHECK(gpu::HasAll(device.capabilities, gpu::Flag(gpu::Capability::Compositor)));
  }
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device can be created");
  CHECK(Device()->device().backend == gpu::Backend::D3D11);
  // With hardware present it is a hardware adapter that is chosen, never the software rasteriser.
  const bool any_hardware = std::any_of(devices.begin(), devices.end(), [](const auto& d) { return d.vendor != gpu::Vendor::Software; });
  if (any_hardware) CHECK(HaveHardware());
  std::fprintf(stderr, "    adapter: %s (%s), %zu MB dedicated\n", Device()->device().name.c_str(), gpu::ToString(Device()->device().vendor),
               Device()->device().dedicated_memory_bytes >> 20);
  // Asking for software explicitly and refusing it are both honoured.
  gpu::D3D11Compositor::Options none;
  none.adapter_id = "d3d11:nonsense";
  std::string reason;
  CHECK(gpu::D3D11Compositor::Create(none, &reason) == nullptr && !reason.empty());
}

CUTLINE_TEST(LayersOpacityAndGradeMatchTheSoftwareCompositorOnPicturesWithEdgesAndTransparency) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  auto sequence = MakeSequence(160, 90);
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("a", "m", 0, 10));
  auto middle = MakeTrack("v2", 1);
  auto b = MakeClip("b", "m", 0, 10);
  b.effects.push_back(MakeEffect("o", "opacity", {{"value", Value::Scalar(0.6)}}));
  middle.clips.push_back(std::move(b));
  auto upper = MakeTrack("v3", 2);
  auto c = MakeClip("c", "m", 0, 10);
  c.effects.push_back(MakeEffect("g", "grade", {{"exposure", Value::Scalar(0.4)}, {"contrast", Value::Scalar(130.0)}, {"saturation", Value::Scalar(70.0)}, {"temperature", Value::Scalar(25.0)}}));
  c.effects.push_back(MakeEffect("o2", "opacity", {{"value", Value::Scalar(0.8)}}));
  upper.clips.push_back(std::move(c));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(middle));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);
  Frames frames;
  frames.Add("a", TexturedFrame(160, 90, PixelFormat::RgbaF32, 0));
  frames.Add("b", TexturedFrame(160, 90, PixelFormat::Rgba8, 1));
  frames.Add("c", TexturedFrame(160, 90, PixelFormat::Rgba16, 2));
  CheckParity(Render(sequence, 1, frames), "three layers (float, 8-bit and 16-bit sources)");
  // Floating-point output agrees as well, to within what half-float intermediates can hold.
  const auto pair = Render(sequence, 1, frames, PixelFormat::RgbaF32);
  CheckParity(pair, "float output");
}

CUTLINE_TEST(MotionCropAndAnchorPlaceAPictureTheSameWayOnBothCompositorsForAnySourceShape) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  struct Case { const char* name; int source_w, source_h; double scale; double rotation; double px, py; double ax, ay; double crop_l, crop_t, crop_r, crop_b; };
  const Case cases[] = {
      {"scaled down and offset", 128, 72, 55.0, 0.0, 12.0, -7.0, 0.5, 0.5, 0, 0, 0, 0},
      {"rotated", 128, 72, 80.0, 17.0, 0.0, 0.0, 0.5, 0.5, 0, 0, 0, 0},
      {"rotated about a corner", 128, 72, 70.0, -33.0, 10.0, 5.0, 0.0, 0.0, 0, 0, 0, 0},
      {"scaled up", 128, 72, 180.0, 5.0, -20.0, 9.0, 0.5, 0.5, 0, 0, 0, 0},
      {"cropped", 128, 72, 100.0, 0.0, 0.0, 0.0, 0.5, 0.5, 0.1, 0.2, 0.15, 0.05},
      {"cropped and rotated", 128, 72, 90.0, 28.0, 3.0, -4.0, 0.5, 0.5, 0.2, 0.1, 0.1, 0.2},
      {"a 4:3 picture on a 16:9 timeline", 96, 72, 100.0, 0.0, 0.0, 0.0, 0.5, 0.5, 0, 0, 0, 0},
      {"a tall picture", 36, 64, 100.0, 90.0, 0.0, 0.0, 0.5, 0.5, 0, 0, 0, 0},
      {"a picture larger than the timeline", 400, 225, 100.0, 11.0, 0.0, 0.0, 0.5, 0.5, 0, 0, 0, 0},
      {"mirrored", 128, 72, -100.0, 0.0, 0.0, 0.0, 0.5, 0.5, 0, 0, 0, 0},
  };
  for (const auto& test : cases) {
    auto sequence = MakeSequence(160, 90);
    auto track = MakeTrack("v1", 0);
    auto clip = MakeClip("a", "m", 0, 10);
    clip.effects.push_back(MakeEffect("m", "motion", {{"scale", Value::Vec2(test.scale, std::abs(test.scale))}, {"position", Value::Vec2(test.px, test.py)},
                                                      {"rotation", Value::Scalar(test.rotation)}, {"anchor", Value::Vec2(test.ax, test.ay)}}));
    if (test.crop_l + test.crop_t + test.crop_r + test.crop_b > 0) {
      clip.effects.push_back(MakeEffect("c", "crop", {{"left", Value::Scalar(test.crop_l)}, {"top", Value::Scalar(test.crop_t)}, {"right", Value::Scalar(test.crop_r)}, {"bottom", Value::Scalar(test.crop_b)}}));
    }
    track.clips.push_back(std::move(clip));
    sequence.tracks.push_back(std::move(track));
    Finalise(sequence);
    Frames frames;
    frames.Add("a", TexturedFrame(test.source_w, test.source_h, PixelFormat::Rgba8, 3));
    CheckParity(Render(sequence, 1, frames), test.name);
  }
}

CUTLINE_TEST(DissolvesAdjustmentClipsGeneratorsAndSequenceEffectsMatchTheSoftwareCompositor) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  auto sequence = MakeSequence(128, 72);
  auto base = MakeTrack("v1", 0);
  base.clips.push_back(MakeClip("a", "m", 0, 6));
  base.clips.push_back(MakeClip("b", "m", 6, 6));
  Transition dissolve;
  dissolve.id = "t";
  dissolve.kind = "cross_dissolve";
  dissolve.from_clip_id = "a";
  dissolve.to_clip_id = "b";
  dissolve.timeline_start = Seconds(5);
  dissolve.duration = Seconds(2);
  base.transitions.push_back(std::move(dissolve));
  // A generator above it, then an adjustment clip that re-grades everything beneath.
  auto solid_track = MakeTrack("v2", 1);
  auto solid = MakeClip("s", "", 0, 12);
  solid.source_kind = model::SourceKind::Media;
  solid.effects.push_back(MakeEffect("solid", "solid", {{"color", Value::Vec3(0.8, 0.3, 0.1)}}));
  solid.effects.push_back(MakeEffect("m", "motion", {{"scale", Value::Vec2(40.0, 40.0)}, {"position", Value::Vec2(30.0, 10.0)}}));
  solid.effects.push_back(MakeEffect("o", "opacity", {{"value", Value::Scalar(0.7)}}));
  solid_track.clips.push_back(std::move(solid));
  auto adjust_track = MakeTrack("v3", 2);
  auto adjust = MakeClip("adj", "", 0, 12);
  adjust.source_kind = model::SourceKind::Adjustment;
  adjust.effects.push_back(MakeEffect("g", "grade", {{"exposure", Value::Scalar(-0.3)}, {"contrast", Value::Scalar(120.0)}, {"saturation", Value::Scalar(140.0)}}));
  adjust_track.clips.push_back(std::move(adjust));
  sequence.tracks.push_back(std::move(base));
  sequence.tracks.push_back(std::move(solid_track));
  sequence.tracks.push_back(std::move(adjust_track));
  sequence.effects.push_back(MakeEffect("se", "opacity", {{"value", Value::Scalar(0.9)}}));
  Finalise(sequence);
  Frames frames;
  frames.Add("a", TexturedFrame(128, 72, PixelFormat::Rgba8, 4));
  frames.Add("b", TexturedFrame(128, 72, PixelFormat::Rgba8, 5));
  // Ordinary 8-bit output does not reserve float output/staging or the two transition layers. The layers are added
  // only when a transition frame first needs them.
  {
    CompositorConfig config;
    Statistics statistics;
    gpu::GpuStatistics device_statistics;
    Device()->ReleaseResources();
    (void)Device()->Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)), config, frames.Resolver(), statistics,
                            &device_statistics);
    CHECK_EQ(device_statistics.texture_bytes, static_cast<std::size_t>(128 * 72 * 24));
    (void)Device()->Compose(TimelineCompiler{}.Compile(sequence, Seconds(6)), config, frames.Resolver(), statistics,
                            &device_statistics);
    CHECK_EQ(device_statistics.texture_bytes, static_cast<std::size_t>(128 * 72 * 40));
  }
  for (const std::int64_t at : {1, 5, 6, 8}) {
    auto pair = Render(sequence, at, frames);
    CheckParity(pair, ("at " + std::to_string(at) + " s").c_str());
  }
  // A frame that is missing is reported the same way and leaves what is beneath.
  Frames none;
  none.Add("b", TexturedFrame(128, 72, PixelFormat::Rgba8, 5));
  CheckParity(Render(sequence, 1, none), "a missing source");
}

CUTLINE_TEST(WhatTheGpuCannotRenderIsRefusedWithAReasonSoTheSoftwareCompositorDoesIt) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  CompositorConfig config;
  const auto supports = [&](const Sequence& sequence, std::string& why) {
    return Device()->Supports(TimelineCompiler{}.Compile(sequence, Seconds(1)), config, &why);
  };
  const auto with_effect = [&](const char* type, std::vector<std::pair<std::string, Value>> parameters) {
    auto sequence = MakeSequence(64, 36);
    auto track = MakeTrack("v1", 0);
    auto clip = MakeClip("a", "m", 0, 10);
    clip.effects.push_back(MakeEffect("e", type, std::move(parameters)));
    track.clips.push_back(std::move(clip));
    sequence.tracks.push_back(std::move(track));
    Finalise(sequence);
    return sequence;
  };
  std::string why;
  CHECK(supports(with_effect("opacity", {{"value", Value::Scalar(0.5)}}), why));
  CHECK(supports(with_effect("motion", {{"scale", Value::Vec2(50.0, 50.0)}}), why));
  CHECK(supports(with_effect("blur", {{"radius", Value::Scalar(0.0)}}), why));
  CHECK(supports(with_effect("blur", {{"radius", Value::Scalar(4.0)}}), why));
  CHECK(!supports(with_effect("blend_mode", {{"mode", Value::Scalar(1.0)}}), why) && why.find("blend") != std::string::npos);
  CHECK(supports(with_effect("vignette", {{"amount", Value::Scalar(0.5)}}), why));
  CHECK(supports(with_effect("gaussian_blur", {{"radius", Value::Scalar(3.0)}}), why));
  CHECK(supports(with_effect("directional_blur", {{"length", Value::Scalar(8.0)}}), why));
  CHECK(supports(with_effect("lens_correction", {{"distortion", Value::Scalar(0.2)}}), why));
  CHECK(supports(with_effect("wave_warp", {{"amplitude", Value::Scalar(5.0)}}), why));
  CHECK(supports(with_effect("bulge", {{"amount", Value::Scalar(0.3)}}), why));
  CHECK(supports(with_effect("rolling_shutter", {{"horizontal", Value::Scalar(7.0)}}), why));
  CHECK(supports(with_effect("posterize", {{"levels", Value::Scalar(6.0)}}), why));
  CHECK(!supports(with_effect("frame_interpolation", {{"mode", Value::Scalar(2.0)}}), why));
  // A mask on an otherwise supported effect.
  {
    auto sequence = with_effect("opacity", {{"value", Value::Scalar(0.5)}});
    cutline::timeline::EffectMask mask;
    mask.id = "mask";
    sequence.tracks[0].clips[0].effects[0].masks.push_back(mask);
    Finalise(sequence);
    CHECK(!supports(sequence, why) && why.find("mask") != std::string::npos);
  }
  // Colour management and the output format the shaders do not write.
  {
    auto sequence = with_effect("opacity", {{"value", Value::Scalar(0.5)}});
    sequence.render_version = 3;
    sequence.working_color_space = "linear";
    CHECK(!supports(sequence, why));
  }
  config.output_format = PixelFormat::Rgba16;
  CHECK(!supports(with_effect("opacity", {{"value", Value::Scalar(0.5)}}), why));
}

CUTLINE_TEST(SpatialEffectsStayOnTheGpuAndMatchTheSoftwareReference) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  const std::vector<std::pair<const char*, Effect>> cases = {
      {"box blur", MakeEffect("e", "blur", {{"radius", Value::Scalar(4.0)}})},
      {"gaussian blur", MakeEffect("e", "gaussian_blur", {{"radius", Value::Scalar(3.0)}})},
      {"directional blur", MakeEffect("e", "directional_blur", {{"length", Value::Scalar(8.0)}, {"angle", Value::Scalar(27.0)}})},
      {"sharpen", MakeEffect("e", "sharpen", {{"amount", Value::Scalar(0.7)}})},
      {"vignette", MakeEffect("e", "vignette", {{"amount", Value::Scalar(0.6)}, {"midpoint", Value::Scalar(0.35)}, {"feather", Value::Scalar(0.4)}})},
      {"lens correction", MakeEffect("e", "lens_correction", {{"distortion", Value::Scalar(0.18)}, {"quadratic", Value::Scalar(-0.06)}, {"scale", Value::Scalar(105.0)}})},
      {"wave warp", MakeEffect("e", "wave_warp", {{"amplitude", Value::Scalar(5.0)}, {"wavelength", Value::Scalar(31.0)}, {"phase", Value::Scalar(20.0)}})},
      {"bulge", MakeEffect("e", "bulge", {{"amount", Value::Scalar(0.35)}, {"radius", Value::Scalar(0.8)}})},
      {"rolling shutter", MakeEffect("e", "rolling_shutter", {{"horizontal", Value::Scalar(6.0)}, {"vertical", Value::Scalar(-2.0)}, {"rotation", Value::Scalar(3.0)}, {"curve", Value::Scalar(0.4)}, {"direction", Value::Scalar(1.0)}})},
      {"posterize", MakeEffect("e", "posterize", {{"levels", Value::Scalar(6.0)}})},
  };
  for (const auto& [name, effect] : cases) {
    auto sequence = MakeSequence(128, 72);
    auto track = MakeTrack("v1", 0);
    auto clip = MakeClip("a", "m", 0, 10);
    clip.effects.push_back(effect);
    track.clips.push_back(std::move(clip));
    sequence.tracks.push_back(std::move(track));
    Finalise(sequence);
    Frames frames;
    frames.Add("a", TexturedFrame(128, 72, PixelFormat::Rgba8, 9));
    CheckParity(Render(sequence, 1, frames), name);
  }

  // Spatial and colour passes keep their order, including on an inset whose
  // transparent boundary is part of the filter input.
  auto sequence = MakeSequence(128, 72);
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("a", "m", 0, 10);
  clip.effects.push_back(MakeEffect("m", "motion", {{"scale", Value::Vec2(65.0, 65.0)}, {"rotation", Value::Scalar(7.0)}}));
  clip.effects.push_back(MakeEffect("g1", "grade", {{"exposure", Value::Scalar(0.3)}}));
  clip.effects.push_back(MakeEffect("b", "blur", {{"radius", Value::Scalar(3.0)}}));
  clip.effects.push_back(MakeEffect("g2", "grade", {{"saturation", Value::Scalar(60.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  Frames frames;
  frames.Add("a", TexturedFrame(128, 72, PixelFormat::Rgba8, 12));
  CheckParity(Render(sequence, 1, frames), "motion, grade, blur, grade chain");
}

CUTLINE_TEST(PresentationTexturesAvoidReadbackStayAliveAndCanUseTheUiDevice) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  gpu::D3D11Compositor::Options options;
  options.external_device = Device()->native_device();
  options.allow_software = true;
  std::string why;
  auto compositor = gpu::D3D11Compositor::Create(options, &why);
  CHECK(compositor != nullptr);
  CHECK(compositor->native_device() == Device()->native_device());

  auto sequence = MakeSequence(128, 72);
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("a", "m", 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  Frames frames;
  frames.Add("a", TexturedFrame(128, 72, PixelFormat::Rgba8, 4));
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  CompositorConfig config;
  Statistics stats;
  gpu::GpuStatistics device_stats;
  gpu::PresentationFrame first;
  const auto no_pixels = compositor->Compose(plan, config, frames.Resolver(), stats, &device_stats, {}, &first);
  CHECK(!no_pixels.valid());
  CHECK(first.valid());
  CHECK(first.device == Device()->native_device());
  CHECK(device_stats.readback_ms == 0.0);

  const auto first_texture = first.texture;
  gpu::PresentationFrame second;
  (void)compositor->Compose(plan, config, frames.Resolver(), stats, &device_stats, {}, &second);
  CHECK(second.valid());
  CHECK(second.texture != first_texture);  // the first texture is still leased by the scene graph
  first = {};
  gpu::PresentationFrame third;
  (void)compositor->Compose(plan, config, frames.Resolver(), stats, &device_stats, {}, &third);
  CHECK(third.texture == first_texture);   // released slots are recycled without allocating every frame
}

CUTLINE_TEST(TheGpuPathIsFasterThanTheSoftwarePathAtFullHdAndSaysWhereTheTimeGoes) {
  SKIP_INAPPLICABLE(HaveHardware(), "no hardware Direct3D 11 adapter: timing the software rasteriser says nothing");
  auto sequence = MakeSequence(1920, 1080);
  for (int t = 0; t < 3; ++t) {
    auto track = MakeTrack("v" + std::to_string(t + 1), t);
    auto clip = MakeClip("c" + std::to_string(t), "m", 0, 10);
    clip.effects.push_back(MakeEffect("m", "motion", {{"scale", Value::Vec2(100.0 - 15.0 * t, 100.0 - 15.0 * t)}, {"rotation", Value::Scalar(4.0 * t)}}));
    clip.effects.push_back(MakeEffect("g", "grade", {{"exposure", Value::Scalar(0.1)}, {"contrast", Value::Scalar(110.0)}}));
    track.clips.push_back(std::move(clip));
    sequence.tracks.push_back(std::move(track));
  }
  Finalise(sequence);
  Frames frames;
  for (int t = 0; t < 3; ++t) frames.Add("c" + std::to_string(t), TexturedFrame(1920, 1080, PixelFormat::Rgba8, t));
  CompositorConfig config;
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  Statistics stats;
  Compositor cpu(config);
  (void)cpu.Compose(plan, frames.Resolver(), stats);   // warm both
  gpu::GpuStatistics gpu_stats;
  (void)Device()->Compose(plan, config, frames.Resolver(), stats, &gpu_stats);
  constexpr int kRuns = 5;
  double cpu_ms = 0, gpu_ms = 0, direct_ms = 0, gpu_device_ms = 0, readback_ms = 0;
  gpu::PresentationFrame presented;
  for (int i = 0; i < kRuns; ++i) {
    auto start = std::chrono::steady_clock::now();
    (void)cpu.Compose(plan, frames.Resolver(), stats);
    cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    (void)Device()->Compose(plan, config, frames.Resolver(), stats, &gpu_stats);
    gpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    gpu_device_ms += gpu_stats.gpu_ms;
    readback_ms = gpu_stats.readback_ms;
    start = std::chrono::steady_clock::now();
    (void)Device()->Compose(plan, config, frames.Resolver(), stats, &gpu_stats, {}, &presented);
    direct_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }
  std::fprintf(stderr, "    1080p, 3 layers with motion and grade (8-bit sources, %s):\n    software %.1f ms   GPU readback %.1f ms (upload %.1f, device %.2f, readback %.1f)   direct texture %.1f ms, %.1fx vs CPU\n",
               Device()->device().name.c_str(), cpu_ms / kRuns, gpu_ms / kRuns, gpu_stats.upload_ms,
               gpu_device_ms / kRuns, readback_ms, direct_ms / kRuns, cpu_ms / direct_ms);
  CHECK(gpu_ms < cpu_ms);
  CHECK(direct_ms < gpu_ms);
}

// ------------------------------------------------------------ hardware decode ----

namespace {

std::optional<std::string> Fixture(const std::string& name) {
  const auto configured = cutline::testing::EnvironmentValue("CUTLINE_FIXTURE_DIR");
  if (configured.empty()) return std::nullopt;
  const auto path = std::filesystem::path(configured) / name;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
  return path.string();
}

// One clip of a file on one track, as the compositor sees it, and the two ways of getting its picture.
struct OneClip final {
  Sequence sequence;
  std::unique_ptr<cutline::media::Source> software;
  std::unique_ptr<cutline::media::Source> hardware;
};

OneClip OpenClip(const std::string& path, int width, int height) {
  cutline::media::RegisterAllProviders();
  OneClip clip;
  clip.sequence = MakeSequence(width, height);
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("c", "m", 0, 4));
  clip.sequence.tracks.push_back(std::move(track));
  Finalise(clip.sequence);
  clip.software = cutline::media::SourceRegistry::Instance().Open(path);
  cutline::media::OpenOptions options;
  options.d3d11_device = Device()->native_device();
  clip.hardware = cutline::media::SourceRegistry::Instance().Open(path, options);
  return clip;
}

}  // namespace

CUTLINE_TEST(HardwareDecodedPicturesAreComposedWhereTheyAreAndMatchTheSoftwareDecode) {
  SKIP_INAPPLICABLE(HaveHardware(), "no hardware Direct3D 11 adapter");
  const auto path = Fixture("bars-2997.mp4");
  SKIP_UNLESS(path.has_value(), "bars-2997.mp4 fixture not generated");
  auto clip = OpenClip(*path, 320, 180);
  SKIP_UNLESS(clip.software != nullptr && clip.hardware != nullptr, "FFmpeg is not available");
  CHECK(clip.hardware->device_decode_available());
  // The picture comes back as planes on the device, not as pixels in memory.
  const auto probe = clip.hardware->ReadDeviceVideo(RationalTime(1, 1));
  SKIP_UNLESS(probe.has_value(), "this adapter has no hardware decoder for H.264");
  CHECK(probe->valid() && probe->format == cutline::media::DeviceFormat::Nv12 && probe->width == 320 && probe->height == 180);
  CHECK(probe->device == Device()->native_device());

  CompositorConfig config;
  double sum = 0;
  int bad = 0, total = 0, largest = 0, on_device = 0;
  for (const double seconds : {0.0, 0.5, 1.0, 2.5, 3.2}) {
    const RationalTime at(static_cast<std::int64_t>(std::llround(seconds * 1000.0)), 1000);
    auto plan = TimelineCompiler{}.Compile(clip.sequence, at);
    plan.video[0].source_time = at;
    auto software_frame = clip.software->ReadVideo(at);
    CHECK(software_frame.has_value());
    const cutline::render::FrameResolver resolve = [&](const SourceRequest&) { return &*software_frame; };
    Statistics cpu_stats, gpu_stats;
    gpu::GpuStatistics device_stats;
    const auto cpu = Compositor(config).Compose(plan, resolve, cpu_stats);
    const gpu::DeviceFrameResolver on_gpu = [&](const SourceRequest& request) {
      auto frame = clip.hardware->ReadDeviceVideo(request.source_time);
      return frame ? *frame : gpu::DeviceFrame{};
    };
    const auto device = Device()->Compose(plan, config, resolve, gpu_stats, &device_stats, on_gpu);
    on_device += device_stats.sources_on_device;
    {
      double local = 0;
      for (int y = 0; y < cpu.height(); ++y) for (int x = 0; x < cpu.width(); ++x) for (int c = 0; c < 3; ++c) local += std::abs(static_cast<int>(cpu.row_u8(y)[x * 4 + c]) - static_cast<int>(device.row_u8(y)[x * 4 + c]));
      std::fprintf(stderr, "    t=%.1f software frame at %.4f s: mean difference %.2f\n", seconds,
                   static_cast<double>(software_frame->presentation_time.numerator()) / static_cast<double>(software_frame->presentation_time.denominator()),
                   local / (cpu.width() * cpu.height() * 3.0));
    }
    if (!cutline::testing::EnvironmentValue("CUTLINE_DUMP").empty() && seconds == 1.0) {
      for (const auto& pair : {std::pair<const char*, const VideoFrame*>{"cpu", &cpu}, {"gpu", &device}}) {
        std::FILE* out = std::fopen((cutline::testing::EnvironmentValue("CUTLINE_DUMP") + pair.first + ".ppm").c_str(), "wb");
        std::fprintf(out, "P6\n%d %d\n255\n", pair.second->width(), pair.second->height());
        for (int y = 0; y < pair.second->height(); ++y) for (int x = 0; x < pair.second->width(); ++x) std::fwrite(pair.second->row_u8(y) + x * 4, 1, 3, out);
        std::fclose(out);
      }
    }
    CHECK_EQ(device_stats.sources_uploaded, 0);   // nothing was copied to the device
    // The two differ in how chroma is stretched back to full size (the filter and where the chroma samples are taken to
    // sit), which shows only at colour edges. Averaged over 8x8 blocks that cancels out, so what is left is the matrix, the
    // range and the plane reading: a wrong one is tens of levels everywhere.
    constexpr int kBlock = 8;
    for (int by = 0; by + kBlock <= cpu.height(); by += kBlock) {
      for (int bx = 0; bx + kBlock <= cpu.width(); bx += kBlock) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
          double a = 0, b = 0;
          for (int y = by; y < by + kBlock; ++y) {
            for (int x = bx; x < bx + kBlock; ++x) {
              a += cpu.row_u8(y)[x * 4 + c];
              b += device.row_u8(y)[x * 4 + c];
            }
          }
          const int difference = static_cast<int>(std::lround(std::abs(a - b) / (kBlock * kBlock)));
          worst = std::max(worst, difference);
          sum += std::abs(a - b) / (kBlock * kBlock);
        }
        largest = std::max(largest, worst);
        if (worst > 4) ++bad;
        ++total;
      }
    }
  }
  CHECK_EQ(on_device, 5);
  const double mean = sum / (static_cast<double>(total) * 3.0), fraction = static_cast<double>(bad) / static_cast<double>(total);
  std::fprintf(stderr, "    hardware decode vs software decode, 5 pictures, 8x8 block averages: mean difference %.2f levels, %.2f%% of blocks more than 4 levels apart (largest %d)\n", mean, fraction * 100.0, largest);
  CHECK(mean < 1.0);
  CHECK(fraction < 0.03);
}

CUTLINE_TEST(HardwareDecodeAtFullHdSkipsTheCopiesAndTheColourConversionOfTheSoftwarePath) {
  SKIP_INAPPLICABLE(HaveHardware(), "no hardware Direct3D 11 adapter");
  std::filesystem::path binary = cutline::testing::EnvironmentValue("CUTLINE_FFMPEG_BINARY");
  if (binary.empty()) {
    for (const auto* candidate : {"../../.tools/ffmpeg/bin/ffmpeg.exe", "../.tools/ffmpeg/bin/ffmpeg.exe", ".tools/ffmpeg/bin/ffmpeg.exe"}) {
      if (std::filesystem::exists(candidate)) binary = candidate;
    }
  }
  SKIP_UNLESS(!binary.empty(), "no ffmpeg binary to make a full-HD file with");
  const auto file = std::filesystem::temp_directory_path() / "cutline-hd-test.mp4";
  if (!std::filesystem::exists(file)) {
    const auto command = "\"" + binary.string() + "\" -v error -y -f lavfi -i testsrc2=s=1920x1080:r=25:d=4 -c:v libopenh264 -b:v 12M -g 25 -pix_fmt yuv420p \"" + file.string() + "\"";
    const int status = std::system(("\"" + command + "\"").c_str());
    SKIP_UNLESS(status == 0 && std::filesystem::exists(file), "could not encode a full-HD file");
  }
  auto clip = OpenClip(file.string(), 1920, 1080);
  SKIP_UNLESS(clip.software != nullptr && clip.hardware != nullptr, "FFmpeg is not available");
  SKIP_UNLESS(clip.hardware->ReadDeviceVideo(RationalTime(0, 1)).has_value(), "this adapter has no hardware decoder for H.264");

  CompositorConfig config;
  constexpr int kFrames = 50;
  const auto time_at = [](int frame) { return RationalTime(frame, 25); };
  Statistics stats;
  gpu::GpuStatistics device_stats;
  // Software: decode to memory, then the GPU compositor uploads it.
  auto start = std::chrono::steady_clock::now();
  double upload_ms = 0;
  for (int f = 0; f < kFrames; ++f) {
    auto plan = TimelineCompiler{}.Compile(clip.sequence, time_at(f));
    plan.video[0].source_time = time_at(f);
    auto picture = clip.software->ReadVideo(time_at(f));
    const cutline::render::FrameResolver resolve = [&](const SourceRequest&) { return &*picture; };
    (void)Device()->Compose(plan, config, resolve, stats, &device_stats);
    upload_ms += device_stats.upload_ms;
  }
  const double software_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
  // Hardware: decode on the device, composite from its planes.
  start = std::chrono::steady_clock::now();
  int on_device = 0;
  for (int f = 0; f < kFrames; ++f) {
    auto plan = TimelineCompiler{}.Compile(clip.sequence, time_at(f));
    plan.video[0].source_time = time_at(f);
    const cutline::render::FrameResolver nothing = [](const SourceRequest&) { return nullptr; };
    const gpu::DeviceFrameResolver on_gpu = [&](const SourceRequest& request) {
      auto frame = clip.hardware->ReadDeviceVideo(request.source_time);
      return frame ? *frame : gpu::DeviceFrame{};
    };
    (void)Device()->Compose(plan, config, nothing, stats, &device_stats, on_gpu);
    on_device += device_stats.sources_on_device;
  }
  const double hardware_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / kFrames;
  std::fprintf(stderr, "    1080p H.264, %d frames in sequence: software decode + upload + GPU compose %.2f ms/frame (upload %.2f); hardware decode + GPU compose %.2f ms/frame\n",
               kFrames, software_ms, upload_ms / kFrames, hardware_ms);
  CHECK_EQ(on_device, kFrames);
  CHECK(hardware_ms < software_ms);
}

// ----------------------------------------------------------------- the engine ----

namespace {

cutline::timeline::SequenceGraph OneClipGraph(int width, int height, const std::string& media, std::int64_t seconds, std::vector<Effect> effects = {}) {
  auto sequence = MakeSequence(width, height);
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("c", media, 0, seconds);
  clip.effects = std::move(effects);
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  cutline::timeline::SequenceGraph graph;
  graph.sequences.push_back(std::move(sequence));
  return graph;
}

cutline::playback::EngineConfig EngineSettings(bool use_gpu, bool hardware = true) {
  cutline::playback::EngineConfig config;
  config.compositor.output_format = PixelFormat::Rgba8;
  config.decode_workers = 0;   // synchronous: the point here is which compositor drew the frame
  config.use_gpu = use_gpu;
  config.gpu_allow_software = true;
  config.hardware_decode = hardware;
  return config;
}

double MeanDifference(const VideoFrame& a, const VideoFrame& b) {
  double sum = 0;
  for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) for (int c = 0; c < 3; ++c) sum += std::abs(static_cast<int>(a.row_u8(y)[x * 4 + c]) - static_cast<int>(b.row_u8(y)[x * 4 + c]));
  return sum / (static_cast<double>(a.width()) * a.height() * 3.0);
}

}  // namespace

CUTLINE_TEST(AnEngineAskedForTheGpuComposesOnItAndRendersTheSamePicturesAsTheSoftwareEngine) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  cutline::media::RegisterAllProviders();
  cutline::media::SyntheticSpec spec;
  spec.pattern = cutline::media::SyntheticPattern::Counter;
  spec.width = 320;
  spec.height = 180;
  spec.frame_rate = {25, 1};
  spec.duration = Seconds(10);
  const auto path = spec.ToPath();
  const cutline::playback::MediaLocator locator = [path](const std::string&) { return path; };
  std::vector<Effect> effects;
  effects.push_back(MakeEffect("m", "motion", {{"scale", Value::Vec2(70.0, 70.0)}, {"rotation", Value::Scalar(8.0)}}));
  effects.push_back(MakeEffect("g", "grade", {{"saturation", Value::Scalar(120.0)}}));
  cutline::playback::PlaybackEngine software(OneClipGraph(160, 90, "m", 10, effects), locator, EngineSettings(false));
  cutline::playback::PlaybackEngine on_gpu(OneClipGraph(160, 90, "m", 10, effects), locator, EngineSettings(true));
  CHECK(!on_gpu.gpu_device_name().empty());
  CHECK(software.gpu_device_name().empty());
  for (const std::int64_t at : {0, 1, 2, 5}) {
    const auto a = software.RenderFrame(Seconds(at));
    const auto b = on_gpu.RenderFrame(Seconds(at));
    CHECK(MeanDifference(a, b) < 0.5);
  }
  const auto stats = on_gpu.statistics();
  CHECK_EQ(stats.gpu_frames, 4);
  CHECK_EQ(stats.gpu_fallback_frames, 0);
  CHECK_EQ(stats.uploaded_pictures, 4);   // a synthetic source is not decoded in hardware
  CHECK_EQ(stats.hardware_pictures, 0);
  CHECK(software.statistics().gpu_frames == 0);
}

CUTLINE_TEST(TheMonitorEngineReturnsANativeTextureWhenItSharesThePresentationDevice) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  cutline::media::RegisterAllProviders();
  cutline::media::SyntheticSpec spec;
  spec.pattern = cutline::media::SyntheticPattern::Counter;
  spec.width = 160;
  spec.height = 90;
  spec.duration = Seconds(2);
  const auto path = spec.ToPath();
  const cutline::playback::MediaLocator locator = [path](const std::string&) { return path; };
  auto config = EngineSettings(true, false);
  config.gpu_external_device = Device()->native_device();
  cutline::playback::PlaybackEngine engine(OneClipGraph(160, 90, "m", 2), locator, config);
  const auto frame = engine.RenderForPresentation(Seconds(1));
  CHECK(frame.on_gpu());
  CHECK(frame.texture.device == Device()->native_device());
  CHECK(!frame.pixels.valid());
  CHECK_EQ(engine.statistics().gpu_frames, std::int64_t{1});
}

CUTLINE_TEST(AFrameTheGpuCannotRenderIsComposedInSoftwareAndCountedAndAnExportEngineNeverUsesTheGpu) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  cutline::media::RegisterAllProviders();
  cutline::media::SyntheticSpec spec;
  spec.pattern = cutline::media::SyntheticPattern::Counter;
  spec.width = 160;
  spec.height = 90;
  spec.duration = Seconds(10);
  const auto path = spec.ToPath();
  const cutline::playback::MediaLocator locator = [path](const std::string&) { return path; };
  std::vector<Effect> effects;
  effects.push_back(MakeEffect("b", "drop_shadow", {{"opacity", Value::Scalar(0.7)}, {"radius", Value::Scalar(3.0)}}));
  cutline::playback::PlaybackEngine software(OneClipGraph(160, 90, "m", 10, effects), locator, EngineSettings(false));
  cutline::playback::PlaybackEngine asked(OneClipGraph(160, 90, "m", 10, effects), locator, EngineSettings(true));
  const auto a = software.RenderFrame(Seconds(1));
  const auto b = asked.RenderFrame(Seconds(1));
  CHECK(MeanDifference(a, b) == 0.0);   // the software compositor drew both
  const auto stats = asked.statistics();
  CHECK_EQ(stats.gpu_frames, 0);
  CHECK_EQ(stats.gpu_fallback_frames, 1);
  // The clone that exports renders in software whatever the monitor does.
  const auto exporter = asked.OriginalQualityClone();
  (void)exporter->RenderFrame(Seconds(1));
  CHECK(exporter->gpu_device_name().empty());
  CHECK_EQ(exporter->statistics().gpu_frames, 0);
  // Not asking for the GPU leaves it untouched, and asking on a machine that cannot is reported rather than hidden.
  CHECK(software.gpu_unavailable_reason().empty());
}

CUTLINE_TEST(AnEngineDecodesH264OnTheGpuAndComposesItWithoutUploadingAPicture) {
  SKIP_INAPPLICABLE(HaveHardware(), "no hardware Direct3D 11 adapter");
  const auto path = Fixture("bars-2997.mp4");
  SKIP_UNLESS(path.has_value(), "bars-2997.mp4 fixture not generated");
  cutline::media::RegisterAllProviders();
  const cutline::playback::MediaLocator locator = [file = *path](const std::string&) { return file; };
  cutline::playback::PlaybackEngine software(OneClipGraph(320, 180, "m", 4), locator, EngineSettings(false));
  cutline::playback::PlaybackEngine hardware(OneClipGraph(320, 180, "m", 4), locator, EngineSettings(true));
  cutline::playback::PlaybackEngine uploaded(OneClipGraph(320, 180, "m", 4), locator, EngineSettings(true, false));
  SKIP_UNLESS(hardware.gpu_device_name() != "", "the GPU compositor did not start");
  double hardware_difference = 0, upload_difference = 0;
  for (const std::int64_t at : {0, 1, 2, 3}) {
    const auto reference = software.RenderFrame(Seconds(at));
    hardware_difference += MeanDifference(reference, hardware.RenderFrame(Seconds(at)));
    upload_difference += MeanDifference(reference, uploaded.RenderFrame(Seconds(at)));
  }
  const auto stats = hardware.statistics();
  SKIP_UNLESS(stats.hardware_pictures > 0, "this adapter has no hardware decoder for H.264");
  CHECK_EQ(stats.hardware_pictures, 4);
  CHECK_EQ(stats.uploaded_pictures, 0);
  CHECK_EQ(uploaded.statistics().hardware_pictures, 0);
  CHECK_EQ(uploaded.statistics().uploaded_pictures, 4);
  std::fprintf(stderr, "    against the software engine, mean difference per picture: hardware decode %.2f levels, software decode uploaded %.2f levels\n", hardware_difference / 4, upload_difference / 4);
  CHECK(upload_difference / 4 < 1.0);   // the same pixels, composed on the other device
  CHECK(hardware_difference / 4 < 3.0);  // the same picture; the chroma of an edge may land differently
}

CUTLINE_TEST(SoftwareReadAheadSkipsMediaAlreadyDecodedOnTheGpu) {
  SKIP_INAPPLICABLE(HaveHardware(), "no hardware Direct3D 11 adapter");
  const auto path = Fixture("bars-2997.mp4");
  SKIP_UNLESS(path.has_value(), "bars-2997.mp4 fixture not generated");
  cutline::media::RegisterAllProviders();
  const cutline::playback::MediaLocator locator = [file = *path](const std::string&) { return file; };
  auto settings = EngineSettings(true);
  settings.decode_workers = 1;
  settings.read_ahead_frames = 4;
  settings.max_pending_decodes = 4;
  cutline::playback::PlaybackEngine engine(OneClipGraph(320, 180, "m", 4), locator, settings);

  (void)engine.RenderFrame(Seconds(0));
  const auto stats = engine.statistics();
  SKIP_UNLESS(stats.hardware_pictures > 0, "this adapter has no hardware decoder for H.264");
  CHECK_EQ(stats.read_ahead_queued, 0);
}

namespace {

constexpr const char* kFailingDevicePath = "test:device-decoder-falls-back";

class FailingDeviceSource final : public cutline::media::Source {
 public:
  explicit FailingDeviceSource(bool device) : device_(device) {
    cutline::commands::MediaStream stream;
    stream.kind = model::StreamKind::Video;
    stream.width = 16;
    stream.height = 16;
    stream.frame_rate = {25, 1};
    probe_.duration = Seconds(4);
    probe_.streams.push_back(std::move(stream));
  }

  [[nodiscard]] const cutline::media::Probe& probe() const override { return probe_; }
  [[nodiscard]] std::optional<VideoFrame> ReadVideo(const RationalTime& time) override {
    auto frame = VideoFrame::Allocate(PixelFormat::Rgba8, 16, 16);
    frame.presentation_time = time;
    frame.duration = {1, 25};
    return frame;
  }
  [[nodiscard]] std::optional<cutline::media::DeviceFrame> ReadDeviceVideo(const RationalTime&) override {
    device_ = false;
    return std::nullopt;
  }
  [[nodiscard]] bool device_decode_available() const override { return device_; }
  [[nodiscard]] std::optional<cutline::media::AudioBuffer> ReadAudio(const RationalTime&, std::int64_t, int,
                                                                     std::int64_t) override {
    return std::nullopt;
  }
  [[nodiscard]] const cutline::media::TimestampMap* timestamps() const override { return nullptr; }

 private:
  cutline::media::Probe probe_;
  bool device_{false};
};

class FailingDeviceProvider final : public cutline::media::SourceProvider {
 public:
  [[nodiscard]] std::string name() const override { return "failing-device-test"; }
  [[nodiscard]] bool CanOpen(const std::string& path) const override { return path == kFailingDevicePath; }
  [[nodiscard]] std::unique_ptr<cutline::media::Source> Open(const std::string&) override {
    return std::make_unique<FailingDeviceSource>(false);
  }
  [[nodiscard]] std::unique_ptr<cutline::media::Source> Open(
      const std::string&, const cutline::media::OpenOptions& options) override {
    return std::make_unique<FailingDeviceSource>(options.d3d11_device != nullptr);
  }
  [[nodiscard]] cutline::media::Probe ProbeFile(const std::string&) override {
    return FailingDeviceSource(false).probe();
  }
};

void RegisterFailingDeviceProvider() {
  static std::once_flag once;
  std::call_once(once, [] {
    cutline::media::SourceRegistry::Instance().Register(std::make_unique<FailingDeviceProvider>());
  });
}

}  // namespace

CUTLINE_TEST(AHardwareDecoderFailureRestoresSoftwareReadAhead) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 adapter");
  RegisterFailingDeviceProvider();
  auto settings = EngineSettings(true);
  settings.decode_workers = 1;
  settings.read_ahead_frames = 3;
  settings.max_pending_decodes = 3;
  cutline::playback::PlaybackEngine engine(
      OneClipGraph(16, 16, "m", 4), [](const std::string&) { return std::string(kFailingDevicePath); }, settings);

  const auto frame = engine.RenderFrame(Seconds(0));
  CHECK(frame.valid());
  const auto stats = engine.statistics();
  CHECK_EQ(stats.hardware_pictures, 0);
  CHECK(stats.uploaded_pictures > 0);
  CHECK(stats.read_ahead_queued > 0);
}

// ------------------------------------------------------------------ colour tools on the card ----

namespace {

// One clip carrying the effects, on a textured picture with edges and a half-transparent band.
Sequence ClipWith(std::vector<Effect> effects, int width = 160, int height = 90) {
  auto sequence = MakeSequence(width, height);
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("a", "m", 0, 10);
  for (auto& effect : effects) clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  return sequence;
}

Pair RenderTextured(const Sequence& sequence, const std::string& asset_root = {}, PixelFormat source = PixelFormat::RgbaF32) {
  Frames frames;
  frames.Add("a", TexturedFrame(static_cast<int>(sequence.width), static_cast<int>(sequence.height), source, 2));
  CompositorConfig config;
  config.output_format = PixelFormat::Rgba8;
  config.asset_root = asset_root;
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  std::string reason;
  CHECK(Device()->Supports(plan, config, &reason));
  Statistics cpu_stats, gpu_stats;
  Pair pair;
  pair.cpu = Compositor(config).Compose(plan, frames.Resolver(), cpu_stats);
  pair.gpu = Device()->Compose(plan, config, frames.Resolver(), gpu_stats);
  CHECK(cpu_stats.effect_errors.empty() && gpu_stats.effect_errors.empty());
  {
    auto bare = sequence;
    for (auto& track : bare.tracks) {
      for (auto& clip : track.clips) clip.effects.clear();
    }
    const auto plain = Compositor(config).Compose(TimelineCompiler{}.Compile(bare, Seconds(1)), frames.Resolver(), cpu_stats);
    double moved = 0;
    for (int y = 0; y < plain.height(); ++y) {
      for (int x = 0; x < plain.width() * 4; ++x) moved += std::abs(static_cast<int>(plain.row_u8(y)[x]) - static_cast<int>(pair.cpu.row_u8(y)[x]));
    }
    pair.change = moved / (static_cast<double>(plain.width()) * plain.height() * 4.0);
  }
  double sum = 0;
  for (int y = 0; y < pair.cpu.height(); ++y) {
    for (int x = 0; x < pair.cpu.width(); ++x) {
      int worst = 0;
      for (int c = 0; c < 4; ++c) {
        const int a = pair.cpu.row_u8(y)[x * 4 + c], b = pair.gpu.row_u8(y)[x * 4 + c];
        worst = std::max(worst, std::abs(a - b));
        sum += std::abs(a - b);
      }
      pair.largest = std::max(pair.largest, worst);
      if (worst > 2) ++pair.different;
      ++pair.total;
    }
  }
  pair.mean = sum / (static_cast<double>(pair.total) * 4.0);
  return pair;
}

// A cube whose output depends on all three inputs differently, so a swapped axis or a wrong index order cannot give the
// same picture.
std::filesystem::path WriteCube(const std::filesystem::path& directory, const std::string& name, int size, const std::string& header = {}) {
  std::filesystem::create_directories(directory);
  const auto path = directory / name;
  std::ofstream out(path, std::ios::trunc);
  out << header << "LUT_3D_SIZE " << size << "\n";
  for (int b = 0; b < size; ++b) {
    for (int g = 0; g < size; ++g) {
      for (int r = 0; r < size; ++r) {
        const double x = r / double(size - 1), y = g / double(size - 1), z = b / double(size - 1);
        out << std::min(1.0, 0.85 * x + 0.20 * z * z) << " " << (0.6 * y + 0.3 * x * (1 - y)) << " " << std::min(1.0, 0.1 + 0.7 * z + 0.2 * y * x) << "\n";
      }
    }
  }
  return path;
}

}  // namespace

CUTLINE_TEST(EveryColourToolOnTheCardGivesTheSamePictureAsTheSoftwareCompositor) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  struct Case {
    const char* name;
    const char* type;
    std::vector<std::pair<std::string, Value>> parameters;
  };
  const Case cases[] = {
      {"colour wheels", "color_wheels", {{"lift", Value::Vec4(0.03, 0.0, -0.02, 0.01)}, {"gamma", Value::Vec4(1.2, 0.95, 0.9, 1.05)}, {"gain", Value::Vec4(1.0, 1.1, 1.25, 0.95)},
                                         {"shadows", Value::Vec3(0.02, 0.0, 0.08)}, {"midtones", Value::Vec3(0.05, 0.0, -0.03)}, {"highlights", Value::Vec3(0.0, -0.04, 0.0)}}},
      {"colour wheels, lift only", "color_wheels", {{"lift", Value::Vec4(0.1, 0.1, 0.1, 0.0)}}},
      {"curves", "curves", {{"master", Value::Vec3(0.18, 0.55, 0.85)}, {"red", Value::Vec3(0.3, 0.5, 0.7)}, {"green", Value::Vec3(0.25, 0.5, 0.75)}, {"blue", Value::Vec3(0.2, 0.45, 0.8)}, {"luma", Value::Vec3(0.3, 0.5, 0.72)}}},
      {"curves, luma only", "curves", {{"luma", Value::Vec3(0.15, 0.5, 0.9)}}},
      {"channel mixer", "channel_mixer", {{"red", Value::Vec3(0.8, 0.2, 0.0)}, {"green", Value::Vec3(0.1, 0.9, 0.0)}, {"blue", Value::Vec3(0.3, 0.0, 0.7)}}},
      {"tint", "tint", {{"map_black", Value::Vec3(0.05, 0.0, 0.2)}, {"map_white", Value::Vec3(1.0, 0.9, 0.7)}, {"amount", Value::Scalar(0.8)}}},
      {"black and white", "black_and_white", {{"weights", Value::Vec3(0.4, 0.4, 0.2)}, {"amount", Value::Scalar(0.9)}}},
      {"colour adjust", "color_adjust", {{"temperature", Value::Scalar(20.0)}, {"tint", Value::Scalar(-10.0)}, {"vibrance", Value::Scalar(40.0)}, {"shadows", Value::Scalar(30.0)}, {"highlights", Value::Scalar(-25.0)}}},
      {"hue curves", "hue_curves", {{"hue_vs_hue_a", Value::Vec3(25.0, -30.0, 10.0)}, {"hue_vs_hue_b", Value::Vec3(0.0, 40.0, -20.0)}, {"hue_vs_sat_a", Value::Vec3(0.3, 0.0, -0.4)},
                                    {"hue_vs_sat_b", Value::Vec3(0.2, 0.1, 0.0)}, {"hue_vs_luma_a", Value::Vec3(0.0, 0.2, -0.2)}, {"hue_vs_luma_b", Value::Vec3(-0.1, 0.0, 0.15)}}},
      {"HSL secondary", "hsl_secondary", {{"hue_center", Value::Scalar(20.0)}, {"hue_width", Value::Scalar(120.0)}, {"hue_softness", Value::Scalar(30.0)}, {"sat_min", Value::Scalar(0.1)},
                                          {"sat_softness", Value::Scalar(0.1)}, {"luma_max", Value::Scalar(0.9)}, {"luma_softness", Value::Scalar(0.1)}, {"hue_shift", Value::Scalar(40.0)},
                                          {"sat_gain", Value::Scalar(1.4)}, {"lightness", Value::Scalar(0.1)}}},
      {"HSL secondary, inverted", "hsl_secondary", {{"hue_center", Value::Scalar(200.0)}, {"hue_width", Value::Scalar(90.0)}, {"invert", Value::Scalar(1.0)}, {"sat_gain", Value::Scalar(0.4)}}},
      {"HSL secondary, matte view", "hsl_secondary", {{"hue_center", Value::Scalar(120.0)}, {"hue_width", Value::Scalar(200.0)}, {"hue_softness", Value::Scalar(20.0)}, {"view", Value::Scalar(1.0)}}},
      {"basic colour", "grade", {{"exposure", Value::Scalar(0.3)}, {"contrast", Value::Scalar(120.0)}, {"saturation", Value::Scalar(80.0)}}},
  };
  for (const auto& test : cases) {
    std::vector<Effect> effects;
    effects.push_back(MakeEffect("e", test.type, test.parameters));
    const auto pair = RenderTextured(ClipWith(std::move(effects)));
    CheckParity(pair, test.name);
    CHECK(pair.change > 1.0);   // the tool did something to the picture
  }
  // Several in a row, in order, with an opacity among them, on an 8-bit source: the order of the operations matters.
  {
    std::vector<Effect> effects;
    effects.push_back(MakeEffect("1", "color_adjust", {{"temperature", Value::Scalar(15.0)}, {"vibrance", Value::Scalar(30.0)}}));
    effects.push_back(MakeEffect("2", "color_wheels", {{"gain", Value::Vec4(1.1, 1.0, 0.9, 1.0)}, {"midtones", Value::Vec3(0.04, 0.0, 0.0)}}));
    effects.push_back(MakeEffect("3", "opacity", {{"value", Value::Scalar(0.75)}}));
    effects.push_back(MakeEffect("4", "curves", {{"master", Value::Vec3(0.2, 0.5, 0.8)}}));
    effects.push_back(MakeEffect("5", "tint", {{"map_black", Value::Vec3(0.0, 0.0, 0.1)}, {"amount", Value::Scalar(0.5)}}));
    CheckParity(RenderTextured(ClipWith(std::move(effects)), {}, PixelFormat::Rgba8), "a chain of five");
  }
  // A tool left at its neutral values draws nothing on either side, and nothing about the picture changes.
  {
    std::vector<Effect> effects;
    effects.push_back(MakeEffect("e", "color_wheels", {}));
    effects.push_back(MakeEffect("f", "curves", {}));
    effects.push_back(MakeEffect("g", "color_adjust", {}));
    effects.push_back(MakeEffect("h", "tint", {{"amount", Value::Scalar(0.0)}}));
    const auto pair = RenderTextured(ClipWith(std::move(effects)));
    CheckParity(pair, "neutral tools");
    CHECK(pair.largest <= 1 && pair.change < 0.01);
  }
  // A normal grading stack can exceed the old eight-operation constant-buffer limit without leaving the card.
  {
    std::vector<Effect> effects;
    for (int index = 0; index < 9; ++index) {
      effects.push_back(MakeEffect("grade-" + std::to_string(index), "grade",
                                   {{"exposure", Value::Scalar(index % 2 == 0 ? 0.03 : -0.02)}}));
    }
    CheckParity(RenderTextured(ClipWith(std::move(effects)), {}, PixelFormat::Rgba8), "nine grades");
  }
}

CUTLINE_TEST(ALutOnTheCardGivesTheSamePictureAsTheSoftwareCompositorForCubesCurvesDomainsAndIntensity) {
  SKIP_INAPPLICABLE(Device() != nullptr, "no Direct3D 11 device");
  const auto directory = std::filesystem::temp_directory_path() / "cutline-gpu-lut";
  std::filesystem::remove_all(directory);
  const auto lut_effect = [](const std::string& file, double intensity) {
    auto effect = MakeEffect("lut", "lut", {{"intensity", Value::Scalar(intensity)}});
    effect.preset_name = file;
    return effect;
  };
  WriteCube(directory, "small.cube", 17);
  WriteCube(directory, "large.cube", 33, "TITLE \"Large\"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n");
  WriteCube(directory, "wide.cube", 9, "DOMAIN_MIN -0.25 -0.25 -0.25\nDOMAIN_MAX 1.5 1.5 1.5\n");
  {
    std::ofstream curves(directory / "curve.cube", std::ios::trunc);
    curves << "LUT_1D_SIZE 5\n0 0 0\n0.1 0.3 0.2\n0.45 0.55 0.5\n0.8 0.7 0.9\n1 1 1\n";
  }
  const auto root = directory.string();
  for (const auto& [file, intensity, name] : {std::tuple<const char*, double, const char*>{"small.cube", 1.0, "a 17 point cube"},
                                              {"large.cube", 1.0, "a 33 point cube"},
                                              {"wide.cube", 1.0, "a cube with another domain"},
                                              {"curve.cube", 1.0, "a 1D table"},
                                              {"small.cube", 0.4, "a cube at 40 percent"}}) {
    std::vector<Effect> effects;
    effects.push_back(lut_effect(file, intensity));
    const auto pair = RenderTextured(ClipWith(std::move(effects)), root);
    CheckParity(pair, name);
    CHECK(pair.change > 2.0);
  }
  // With a grade before it and an opacity after: the LUT sees what the grade made.
  {
    std::vector<Effect> effects;
    effects.push_back(MakeEffect("g", "grade", {{"exposure", Value::Scalar(0.25)}, {"saturation", Value::Scalar(120.0)}}));
    effects.push_back(lut_effect("small.cube", 1.0));
    effects.push_back(MakeEffect("o", "opacity", {{"value", Value::Scalar(0.7)}}));
    CheckParity(RenderTextured(ClipWith(std::move(effects)), root, PixelFormat::Rgba8), "grade, LUT, opacity");
  }

  // Several LUTs remain on the card and preserve their authored order.
  {
    std::vector<Effect> two;
    two.push_back(lut_effect("small.cube", 0.7));
    two.push_back(lut_effect("large.cube", 0.4));
    CheckParity(RenderTextured(ClipWith(std::move(two)), root, PixelFormat::Rgba8), "two LUTs");
  }

  // What the card cannot take is refused with a reason, so the software compositor reports the fallback.
  CompositorConfig config;
  config.asset_root = root;
  const auto refuses = [&](std::vector<Effect> effects, const char* word) {
    std::string why;
    const auto plan = TimelineCompiler{}.Compile(ClipWith(std::move(effects)), Seconds(1));
    return !Device()->Supports(plan, config, &why) && why.find(word) != std::string::npos;
  };
  {
    std::vector<Effect> missing;
    missing.push_back(lut_effect("nothing-here.cube", 1.0));
    CHECK(refuses(std::move(missing), "lut"));
  }
  {
    std::ofstream broken(directory / "broken.cube", std::ios::trunc);
    broken << "LUT_3D_SIZE 2\n1 1 1\n";
  }
  {
    std::vector<Effect> broken;
    broken.push_back(lut_effect("broken.cube", 1.0));
    CHECK(refuses(std::move(broken), "entry count"));
  }
  // A file that appears (or is repaired) is taken up again without the compositor being remade.
  {
    std::vector<Effect> late;
    late.push_back(lut_effect("late.cube", 1.0));
    CHECK(refuses(late, "lut"));
    WriteCube(directory, "late.cube", 5);
    std::string why;
    CHECK(Device()->Supports(TimelineCompiler{}.Compile(ClipWith(std::move(late)), Seconds(1)), config, &why));
  }
  std::filesystem::remove_all(directory);
}

int main() { return cutline::testing::RunAll("gpu"); }

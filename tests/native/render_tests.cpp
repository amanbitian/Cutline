// Compositor: blend maths, effects, transitions, and whole-frame goldens.
//
// The numeric cases pin the maths exactly. The golden cases catch the changes a
// numeric test cannot see -- a dropped premultiply, an inverted track order, a
// half-pixel sampling shift -- by comparing every pixel of the output.

#include "media/SyntheticSource.h"
#include "media/Providers.h"
#include "media/VideoFrame.h"
#include "effects/EffectRegistry.h"
#include "render/ColorManagement.h"
#include "render/AngleMonitor.h"
#include "render/BlendModes.h"
#include "render/Compositor.h"
#include "render/TextRaster.h"
#include "captions/Captions.h"
#include "render/Filters.h"
#include "render/CubeLut.h"
#include "core/util/ParallelRows.h"
#include "render/Graphics.h"
#include "render/GpuDevice.h"
#include "render/MeshWarp.h"
#include "render/FlowCache.h"
#include "render/OpticalFlow.h"
#include "render/RenderCache.h"
#include "render/RenderGraph.h"
#include "render/RenderGraphBuilder.h"
#include "render/Scopes.h"
#include "render/RollingShutter.h"
#include "render/Tracking.h"
#include "render/Transitions.h"
#include "render/TrackingData.h"
#include "render/Stabilizer.h"
#include "tests/native/GoldenImage.h"
#include "tests/native/OpticalFlowReference.h"
#include "tests/native/TestHarness.h"
#include "timeline/TimelineCompiler.h"

#include <atomic>
#include <cstdio>
#include <set>
#include <cmath>
#include <limits>
#include <sstream>
#include <thread>
#include <complex>
#include <filesystem>
#include <fstream>
#include <map>
#include <chrono>
#include <string>
#include <thread>

namespace model = cutline::model;
namespace rates = cutline::time;
using cutline::anim::Interpolation;
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

constexpr int kWidth = 64;
constexpr int kHeight = 36;

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

// A flat colour frame, which makes the expected blend result arithmetic the
// test can state exactly.
VideoFrame SolidFrame(float red, float green, float blue, float alpha = 1.0f, int width = kWidth,
                      int height = kHeight) {
  auto frame = VideoFrame::Allocate(PixelFormat::RgbaF32, width, height);
  for (int y = 0; y < height; ++y) {
    auto* row = frame.row_f32(y);
    for (int x = 0; x < width; ++x) {
      auto* texel = row + static_cast<std::size_t>(x) * 4;
      texel[0] = red;
      texel[1] = green;
      texel[2] = blue;
      texel[3] = alpha;
    }
  }
  return frame;
}

Clip MakeClip(std::string id, std::string source, std::int64_t start, std::int64_t duration) {
  Clip clip;
  clip.id = std::move(id);
  clip.source_kind = model::SourceKind::Media;
  clip.source_id = std::move(source);
  clip.source_in = Seconds(0);
  clip.source_out = Seconds(duration);
  clip.timeline_start = Seconds(start);
  clip.start_ticks = clip.timeline_start.ToTicks();
  clip.end_ticks = clip.end().ToTicks();
  return clip;
}

Track MakeTrack(std::string id, std::int64_t order) {
  Track track;
  track.id = std::move(id);
  track.kind = model::TrackKind::Video;
  track.order = order;
  return track;
}

Sequence MakeSequence() {
  Sequence sequence;
  sequence.id = "seq-1";
  sequence.name = "Main";
  sequence.frame_rate = rates::kFrameRate25;
  sequence.width = kWidth;
  sequence.height = kHeight;
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

// Serves a frame per clip id.
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

// Reads the centre pixel, where a flat-colour composite is unambiguous.
struct Rgba final {
  float r{};
  float g{};
  float b{};
  float a{};
};

Rgba Centre(const VideoFrame& frame) {
  const auto* row = frame.row_f32(frame.height() / 2);
  const auto* texel = row + static_cast<std::size_t>(frame.width() / 2) * 4;
  return {texel[0], texel[1], texel[2], texel[3]};
}

Rgba At(const VideoFrame& frame, int x, int y) {
  const auto* row = frame.row_f32(y);
  const auto* texel = row + static_cast<std::size_t>(x) * 4;
  return {texel[0], texel[1], texel[2], texel[3]};
}

bool Near(float actual, float expected, float tolerance = 1e-4f) { return std::abs(actual - expected) < tolerance; }

CompositorConfig FloatOutput() {
  CompositorConfig config;
  config.output_format = PixelFormat::RgbaF32;
  return config;
}

}  // namespace

// ------------------------------------------------------------------- basics ----

CUTLINE_TEST(ASingleOpaqueClipFillsTheFrame) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 0.0f, 0.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  const auto pixel = Centre(output);
  CHECK(Near(pixel.r, 1.0f));
  CHECK(Near(pixel.g, 0.0f));
  CHECK(Near(pixel.b, 0.0f));
  CHECK(Near(pixel.a, 1.0f));
}

CUTLINE_TEST(AnEmptyPlanProducesTheBackground) {
  auto sequence = MakeSequence();
  sequence.tracks.push_back(MakeTrack("v1", 0));
  Finalise(sequence);

  auto config = FloatOutput();
  config.background_red = 0.25f;
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(config).Compose(plan, Frames{}.Resolver());
  const auto pixel = Centre(output);
  CHECK(Near(pixel.r, 0.25f));
  CHECK(Near(pixel.a, 1.0f));
}

CUTLINE_TEST(AMissingFrameIsReportedAndLeavesTheBackground) {
  // One offline clip must not black out the monitor or abort the frame.
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-1", "media-missing", 0, 10));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  Statistics statistics;
  const auto output = Compositor(FloatOutput()).Compose(plan, Frames{}.Resolver(), statistics);
  CHECK_EQ(statistics.missing_frames, 1);
  CHECK(Near(Centre(output).r, 0.0f));
}

CUTLINE_TEST(AFrameMissingBetweenFullAndInsetPicturesCannotLeakTheOldPicture) {
  auto full = MakeSequence();
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  full.tracks.push_back(std::move(track));
  Finalise(full);
  auto inset = full;
  inset.tracks.front().clips.front().effects.push_back(
      MakeEffect("motion", "motion", {{"scale", Value::Vec2(50.0, 50.0)}}));

  auto red = SolidFrame(1.0f, 0.0f, 0.0f);
  auto blue = SolidFrame(0.0f, 0.0f, 1.0f);
  const VideoFrame* current = &red;
  const auto resolver = [&](const SourceRequest&) { return current; };
  Compositor compositor(FloatOutput());
  (void)compositor.Compose(TimelineCompiler{}.Compile(full, Seconds(1)), resolver);
  current = nullptr;
  (void)compositor.Compose(TimelineCompiler{}.Compile(full, Seconds(1)), resolver);
  current = &blue;
  const auto output = compositor.Compose(TimelineCompiler{}.Compile(inset, Seconds(1)), resolver);

  // This pixel is in the transform's conservative dirty rectangle but outside
  // the half-size picture. It catches stale storage that a missing frame could
  // otherwise make visible on the next partial draw.
  CHECK(Near(At(output, 15, 8).r, 0.0f));
  CHECK(Near(At(output, 15, 8).b, 0.0f));
  CHECK(Centre(output).b > 0.99f);
}

CUTLINE_TEST(ComposeRejectsAPlanWithNoSize) {
  auto sequence = MakeSequence();
  sequence.width = 0;
  sequence.tracks.push_back(MakeTrack("v1", 0));
  // The compiler validates sizes, so build the plan by hand to reach the
  // compositor's own guard.
  cutline::timeline::PlaybackPlan plan;
  plan.sequence_id = "seq-1";
  CHECK_THROWS(Compositor(FloatOutput()).Compose(plan, Frames{}.Resolver()));
}

// ------------------------------------------------------- compositing order ----

CUTLINE_TEST(UpperTracksCompositeOverLowerOnes) {
  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("clip-lower", "media-1", 0, 10));
  auto upper = MakeTrack("v2", 1);
  upper.clips.push_back(MakeClip("clip-upper", "media-2", 0, 10));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-lower", SolidFrame(1.0f, 0.0f, 0.0f));
  frames.Add("clip-upper", SolidFrame(0.0f, 1.0f, 0.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  // The upper track is opaque, so it wins outright.
  const auto pixel = Centre(output);
  CHECK(Near(pixel.r, 0.0f));
  CHECK(Near(pixel.g, 1.0f));
}

CUTLINE_TEST(SemiTransparentUpperTrackBlendsWithWhatIsBeneath) {
  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("clip-lower", "media-1", 0, 10));
  auto upper = MakeTrack("v2", 1);
  upper.clips.push_back(MakeClip("clip-upper", "media-2", 0, 10));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-lower", SolidFrame(1.0f, 0.0f, 0.0f));
  frames.Add("clip-upper", SolidFrame(0.0f, 1.0f, 0.0f, 0.5f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  const auto pixel = Centre(output);
  // Source-over with 0.5 alpha: half green over full red.
  CHECK(Near(pixel.r, 0.5f));
  CHECK(Near(pixel.g, 0.5f));
  CHECK(Near(pixel.a, 1.0f));
}

CUTLINE_TEST(ProductionBlendModesUseStraightColourAndPremultipliedCoverage) {
  auto sequence = MakeSequence();
  auto bottom = MakeTrack("v1", 0);
  bottom.clips.push_back(MakeClip("bottom", "media-a", 0, 10));
  auto top = MakeTrack("v2", 1);
  auto top_clip = MakeClip("top", "media-b", 0, 10);
  top_clip.effects.push_back(MakeEffect("blend", "blend_mode",
                                        {{"mode", Value::Scalar(1.0)}, {"opacity", Value::Scalar(0.5)}}));
  top.clips.push_back(std::move(top_clip));
  sequence.tracks.push_back(std::move(bottom));
  sequence.tracks.push_back(std::move(top));
  Finalise(sequence);
  Frames frames;
  frames.Add("bottom", SolidFrame(0.8f, 0.4f, 0.2f));
  frames.Add("top", SolidFrame(0.5f, 0.25f, 1.0f));
  const auto pixel = Centre(Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)),
                                                               frames.Resolver()));
  CHECK(Near(pixel.r, 0.6f));  // half normal backdrop, half multiply(0.8, 0.5)
  CHECK(Near(pixel.g, 0.25f));
  CHECK(Near(pixel.b, 0.2f));
  CHECK(Near(pixel.a, 1.0f));
  CHECK(cutline::render::BlendModeName(cutline::render::BlendModeFromIndex(11)) == "exclusion");
  CHECK(cutline::render::BlendModeFromIndex(99) == cutline::render::BlendMode::Normal);
}

CUTLINE_TEST(FrameInterpolationSynthesisesAFractionalTimeRequestedByARemap) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip", "media", 0, 10);
  clip.effects.push_back(MakeEffect("interpolation", "frame_interpolation", {{"mode", Value::Scalar(1.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  auto first = SolidFrame(1.0f, 0.0f, 0.0f);
  first.presentation_time = {0, 1};
  first.duration = {1, 25};
  auto second = SolidFrame(0.0f, 0.0f, 1.0f);
  second.presentation_time = {1, 25};
  second.duration = {1, 25};
  const auto resolver = [&](const SourceRequest& request) -> const VideoFrame* {
    return request.source_time.Compare(RationalTime(1, 25)) < 0 ? &first : &second;
  };
  Statistics statistics;
  const auto output = Compositor(FloatOutput()).Compose(
      TimelineCompiler{}.Compile(sequence, RationalTime(1, 50)), resolver, statistics);
  const auto pixel = Centre(output);
  CHECK(Near(pixel.r, 0.5f));
  CHECK(Near(pixel.b, 0.5f));
  CHECK_EQ(statistics.interpolated_frames, 1);
  CHECK_EQ(statistics.optical_flow_frames, 0);
}

// ---------------------------------------------------------------- opacity ----

CUTLINE_TEST(OpacityEffectScalesContribution) {
  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("clip-lower", "media-1", 0, 10));
  auto upper = MakeTrack("v2", 1);
  auto top = MakeClip("clip-upper", "media-2", 0, 10);
  top.effects.push_back(MakeEffect("fx-op", "opacity", {{"value", Value::Scalar(0.25)}}));
  upper.clips.push_back(std::move(top));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-lower", SolidFrame(0.0f, 0.0f, 0.0f));
  frames.Add("clip-upper", SolidFrame(1.0f, 1.0f, 1.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  CHECK(Near(Centre(output).r, 0.25f));
}

CUTLINE_TEST(GeometricMaskLimitsAnEffectToItsRectangleAndFeather) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  auto effect = MakeEffect("fx-grade", "grade", {{"exposure", Value::Scalar(1.0)}});
  cutline::effects::mask::Document document;
  document.shape = cutline::effects::mask::Shape::Rectangle;
  document.width = 0.5;
  document.height = 0.5;
  document.feather = 4.0;
  effect.masks.push_back({"mask-1", 0, document});
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.25f, 0.25f, 0.25f));
  const auto output = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)),
                                                        frames.Resolver());
  CHECK(Near(Centre(output).r, 0.5f));
  CHECK(Near(At(output, 2, 2).r, 0.25f));
  const auto edge = At(output, kWidth / 4, kHeight / 2).r;
  CHECK(edge > 0.25f);
  CHECK(edge < 0.5f);
}

CUTLINE_TEST(AnimatedEllipseMaskIsSampledInClipLocalTime) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  auto effect = MakeEffect("fx-grade", "grade", {{"exposure", Value::Scalar(1.0)}});
  cutline::effects::mask::Document document;
  document.shape = cutline::effects::mask::Shape::Ellipse;
  document.center_x = 0.25;
  document.width = 0.3;
  document.height = 0.5;
  document.animations.push_back({"center_x", "linear", {{0.0, 0.25}, {2.0, 0.75}}});
  effect.masks.push_back({"mask-1", 0, document});
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.25f, 0.25f, 0.25f));
  Compositor compositor(FloatOutput());
  const auto left = compositor.Compose(TimelineCompiler{}.Compile(sequence, Seconds(0)), frames.Resolver());
  const auto right = compositor.Compose(TimelineCompiler{}.Compile(sequence, Seconds(2)), frames.Resolver());
  CHECK(Near(At(left, kWidth / 4, kHeight / 2).r, 0.5f));
  CHECK(Near(At(left, 3 * kWidth / 4, kHeight / 2).r, 0.25f));
  CHECK(Near(At(right, kWidth / 4, kHeight / 2).r, 0.25f));
  CHECK(Near(At(right, 3 * kWidth / 4, kHeight / 2).r, 0.5f));
}

CUTLINE_TEST(KeyframedOpacityRampsOverTheClip) {
  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("clip-lower", "media-1", 0, 10));
  auto upper = MakeTrack("v2", 1);
  auto top = MakeClip("clip-upper", "media-2", 0, 10);

  Effect fade;
  fade.id = "fx-fade";
  fade.effect_type = "opacity";
  Parameter value;
  value.id = "fx-fade:value";
  value.name = "value";
  cutline::anim::AnimatedValue curve;
  curve.SetKeyframe({Seconds(0), Value::Scalar(0.0), Interpolation::Linear, {}, {}});
  curve.SetKeyframe({Seconds(4), Value::Scalar(1.0), Interpolation::Linear, {}, {}});
  value.value = curve;
  fade.parameters.push_back(std::move(value));
  top.effects.push_back(std::move(fade));

  upper.clips.push_back(std::move(top));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-lower", SolidFrame(0.0f, 0.0f, 0.0f));
  frames.Add("clip-upper", SolidFrame(1.0f, 1.0f, 1.0f));

  const TimelineCompiler compiler;
  const Compositor compositor(FloatOutput());
  CHECK(Near(Centre(compositor.Compose(compiler.Compile(sequence, Seconds(0)), frames.Resolver())).r, 0.0f));
  CHECK(Near(Centre(compositor.Compose(compiler.Compile(sequence, Seconds(1)), frames.Resolver())).r, 0.25f));
  CHECK(Near(Centre(compositor.Compose(compiler.Compile(sequence, Seconds(2)), frames.Resolver())).r, 0.5f));
  CHECK(Near(Centre(compositor.Compose(compiler.Compile(sequence, Seconds(4)), frames.Resolver())).r, 1.0f));
}

// ------------------------------------------------------------ transitions ----

CUTLINE_TEST(CrossDissolveMixesByProgress) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-a", "media-1", 0, 6));
  track.clips.push_back(MakeClip("clip-b", "media-2", 6, 6));
  Transition dissolve;
  dissolve.id = "t-1";
  dissolve.kind = "cross_dissolve";
  dissolve.from_clip_id = "clip-a";
  dissolve.to_clip_id = "clip-b";
  dissolve.timeline_start = Seconds(5);
  dissolve.duration = Seconds(2);
  track.transitions.push_back(std::move(dissolve));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-a", SolidFrame(1.0f, 0.0f, 0.0f));
  frames.Add("clip-b", SolidFrame(0.0f, 0.0f, 1.0f));

  const TimelineCompiler compiler;
  const Compositor compositor(FloatOutput());

  // At the start of the dissolve the outgoing clip is still fully present.
  const auto start = Centre(compositor.Compose(compiler.Compile(sequence, Seconds(5)), frames.Resolver()));
  CHECK(Near(start.r, 1.0f));
  CHECK(Near(start.b, 0.0f));

  // Halfway: an even mix.
  Statistics statistics;
  const auto middle =
      Centre(compositor.Compose(compiler.Compile(sequence, Seconds(6)), frames.Resolver(), statistics));
  CHECK(Near(middle.r, 0.5f));
  CHECK(Near(middle.b, 0.5f));
  CHECK_EQ(statistics.transitions_mixed, 1);
  // The two sides contribute one layer between them, not two.
  CHECK_EQ(statistics.layers_composited, 1);
}

CUTLINE_TEST(AOneSidedTransitionFadesFromWhatIsBeneath) {
  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("clip-bg", "media-bg", 0, 10));
  auto upper = MakeTrack("v2", 1);
  upper.clips.push_back(MakeClip("clip-in", "media-in", 0, 10));
  Transition fade;
  fade.id = "t-fade";
  fade.kind = "cross_dissolve";
  fade.alignment = model::TransitionAlignment::Start;
  fade.to_clip_id = "clip-in";
  fade.timeline_start = Seconds(0);
  fade.duration = Seconds(4);
  upper.transitions.push_back(std::move(fade));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-bg", SolidFrame(1.0f, 0.0f, 0.0f));
  frames.Add("clip-in", SolidFrame(0.0f, 1.0f, 0.0f));

  const TimelineCompiler compiler;
  const Compositor compositor(FloatOutput());
  // At the start the incoming clip is fully transparent, so the lower track
  // shows through untouched.
  const auto begin = Centre(compositor.Compose(compiler.Compile(sequence, Seconds(0)), frames.Resolver()));
  CHECK(Near(begin.r, 1.0f));
  CHECK(Near(begin.g, 0.0f));
  // Halfway through, half of each.
  const auto middle = Centre(compositor.Compose(compiler.Compile(sequence, Seconds(2)), frames.Resolver()));
  CHECK(Near(middle.r, 0.5f));
  CHECK(Near(middle.g, 0.5f));
}

// ------------------------------------------------------------------ grade ----

CUTLINE_TEST(ExposureIsMeasuredInStops) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-grade", "grade", {{"exposure", Value::Scalar(1.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.25f, 0.25f, 0.25f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  // One stop is a doubling.
  CHECK(Near(Centre(output).r, 0.5f));
}

CUTLINE_TEST(SaturationPivotsAroundRec709Luma) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-grade", "grade", {{"saturation", Value::Scalar(0.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 0.0f, 0.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  // Fully desaturated pure red collapses to its Rec.709 luma, 0.2126.
  const auto pixel = Centre(output);
  CHECK(Near(pixel.r, 0.2126f, 1e-3f));
  CHECK(Near(pixel.g, 0.2126f, 1e-3f));
  CHECK(Near(pixel.b, 0.2126f, 1e-3f));
}

CUTLINE_TEST(ContrastPivotsAroundMidGrey) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-grade", "grade", {{"contrast", Value::Scalar(200.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  // Mid grey is the pivot, so it must not move.
  frames.Add("clip-1", SolidFrame(0.5f, 0.5f, 0.5f));
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  CHECK(Near(Centre(Compositor(FloatOutput()).Compose(plan, frames.Resolver())).r, 0.5f));

  Frames brighter;
  brighter.Add("clip-1", SolidFrame(0.6f, 0.6f, 0.6f));
  // 0.6 is 0.1 above the pivot; doubling contrast puts it 0.2 above.
  CHECK(Near(Centre(Compositor(FloatOutput()).Compose(plan, brighter.Resolver())).r, 0.7f, 1e-3f));
}

CUTLINE_TEST(GradeIgnoresFullyTransparentPixels) {
  // Grading a transparent pixel would otherwise lift it off zero and leave a
  // halo once it is composited.
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-grade", "grade", {{"exposure", Value::Scalar(2.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.5f, 0.5f, 0.5f, 0.0f));
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  // A transparent source contributes nothing, so the background shows through
  // unlifted. Were the grade applied to premultiplied zeros it would raise this
  // off black and leave a halo once composited.
  CHECK(Near(Centre(output).r, 0.0f));
}

// --------------------------------------------------------- spatial effects ----

CUTLINE_TEST(BlurUsesASeparableNormalisedKernel) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-blur", "blur", {{"radius", Value::Scalar(1.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  auto impulse = SolidFrame(0.0f, 0.0f, 0.0f);
  auto* texel = impulse.row_f32(kHeight / 2) + static_cast<std::size_t>(kWidth / 2) * 4;
  texel[0] = texel[1] = texel[2] = 1.0f;
  Frames frames;
  frames.Add("clip-1", std::move(impulse));

  const auto output = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)),
                                                         frames.Resolver());
  // A radius-one separable box kernel spreads one impulse over a 3x3 square.
  CHECK(Near(Centre(output).r, 1.0f / 9.0f, 1e-3f));
  CHECK(Near(At(output, kWidth / 2 + 1, kHeight / 2).r, 1.0f / 9.0f, 1e-3f));
  CHECK(Near(At(output, kWidth / 2 + 2, kHeight / 2).r, 0.0f));
}

CUTLINE_TEST(SharpenRaisesLocalContrastWithoutChangingAlpha) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-sharpen", "sharpen", {{"amount", Value::Scalar(1.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  auto frame = SolidFrame(0.5f, 0.5f, 0.5f);
  auto* texel = frame.row_f32(kHeight / 2) + static_cast<std::size_t>(kWidth / 2) * 4;
  texel[0] = texel[1] = texel[2] = 0.6f;
  Frames frames;
  frames.Add("clip-1", std::move(frame));

  const auto output = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)),
                                                         frames.Resolver());
  CHECK(Near(Centre(output).r, 0.7f, 1e-3f));
  CHECK(Near(Centre(output).a, 1.0f));
}

CUTLINE_TEST(VignetteDarkensCornersAndPreservesTheCentre) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-vignette", "vignette",
                                    {{"amount", Value::Scalar(1.0)}, {"midpoint", Value::Scalar(0.2)},
                                     {"feather", Value::Scalar(0.5)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 1.0f, 1.0f));
  const auto output = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)),
                                                         frames.Resolver());
  CHECK(Centre(output).r > 0.99f);
  CHECK(At(output, 0, 0).r < 0.01f);
  CHECK(Near(At(output, 0, 0).a, 1.0f));
}

CUTLINE_TEST(LensCorrectionRemapsPixelsRadially) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(
      MakeEffect("fx-lens", "lens_correction", {{"distortion", Value::Scalar(0.4)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  auto gradient = SolidFrame(0.0f, 0.0f, 0.0f);
  for (int y = 0; y < gradient.height(); ++y) {
    auto* row = gradient.row_f32(y);
    for (int x = 0; x < gradient.width(); ++x) row[static_cast<std::size_t>(x) * 4] = x / 63.0f;
  }
  Frames frames;
  frames.Add("clip-1", std::move(gradient));
  const auto output = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)),
                                                         frames.Resolver());
  // Positive distortion samples farther from the optical centre at the right.
  CHECK(At(output, 48, kHeight / 2).r > 48.0f / 63.0f);
  CHECK(Near(Centre(output).r, 32.0f / 63.0f, 1e-3f));
  CHECK(cutline::render::IsBuiltInEffect("wide_angle"));
}

CUTLINE_TEST(CubeLutLoadsOnceAndBlendsByIntensity) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-render-lut";
  std::filesystem::create_directories(directory);
  const auto path = directory / "invert.cube";
  {
    std::ofstream lut(path, std::ios::trunc);
    lut << "TITLE \"Invert\"\n"
           "LUT_3D_SIZE 2\n"
           "DOMAIN_MIN 0 0 0\n"
           "DOMAIN_MAX 1 1 1\n"
           "1 1 1\n0 1 1\n1 0 1\n0 0 1\n"
           "1 1 0\n0 1 0\n1 0 0\n0 0 0\n";
  }

  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  auto effect = MakeEffect("fx-lut", "lut", {{"intensity", Value::Scalar(0.5)}});
  effect.preset_name = "invert.cube";
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.2f, 0.4f, 0.8f));
  auto config = FloatOutput();
  config.asset_root = directory.string();
  const auto output = Compositor(config).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)), frames.Resolver());
  // Halfway between the source and its inverted LUT result.
  CHECK(Near(Centre(output).r, 0.5f, 1e-3f));
  CHECK(Near(Centre(output).g, 0.5f, 1e-3f));
  CHECK(Near(Centre(output).b, 0.5f, 1e-3f));
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(OneDimensionalCubeTablesAreCurvesPerChannelAndAdobeRangesAreRead) {
  using cutline::render::CubeLut;
  std::istringstream curves(
      "TITLE \"curves\"\n"
      "LUT_1D_SIZE 3\n"
      "0 0 0\n"
      "0.8 0.2 0.5\n"
      "1 1 1\n");
  const auto lut = CubeLut::Parse(curves);
  CHECK(lut.kind() == CubeLut::Kind::Curves1D && lut.size() == 3 && lut.entry_count() == 3);
  // Each channel is looked up in its own column, between the rows either side.
  auto out = lut.Sample(0.25f, 0.25f, 0.25f);
  CHECK(Near(out[0], 0.4f) && Near(out[1], 0.1f) && Near(out[2], 0.25f));
  out = lut.Sample(0.75f, 1.0f, 0.5f);
  CHECK(Near(out[0], 0.9f) && Near(out[1], 1.0f) && Near(out[2], 0.5f));
  out = lut.Sample(-3.0f, 7.0f, std::numeric_limits<float>::quiet_NaN());   // outside the domain: held at its edges; a NaN is not an index
  CHECK(Near(out[0], 0.0f) && Near(out[1], 1.0f) && Near(out[2], 0.0f));

  // A domain other than 0..1, and Adobe's spelling of it.
  std::istringstream ranged("LUT_1D_SIZE 2\nLUT_1D_INPUT_RANGE -1 3\n0 0 0\n1 1 1\n");
  const auto wide = CubeLut::Parse(ranged);
  CHECK(Near(wide.Sample(1.0f, 1.0f, 1.0f)[0], 0.5f) && Near(wide.domain_max()[2], 3.0f));

  // What cannot be read says why.
  const auto fails = [](const char* text) {
    std::istringstream in(text);
    try {
      (void)CubeLut::Parse(in);
    } catch (const std::exception&) {
      return true;
    }
    return false;
  };
  CHECK(fails("LUT_1D_SIZE 2\nLUT_3D_SIZE 2\n0 0 0\n1 1 1\n"));        // both kinds in one file
  CHECK(fails("LUT_1D_SIZE 3\n0 0 0\n1 1 1\n"));                       // too few rows
  CHECK(fails("LUT_1D_SIZE 1\n0 0 0\n"));                               // not a table
  CHECK(fails("LUT_3D_SIZE 2\n0 0 0 0\n"));                            // a row of four
  CHECK(fails("0 0 0\n"));                                              // data before the size
  CHECK(fails("LUT_1D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n0 0 0\n1 1 1\n"));
}

CUTLINE_TEST(TheLutSamplerMapsEveryColourTheSameWhicheverCallIsUsedAndDecimalsDoNotDependOnTheLocale) {
  using cutline::render::CubeLut;
  // A 5-point table with a different curve in every channel and a domain that is not 0..1.
  std::ostringstream text;
  text << "LUT_3D_SIZE 5\nDOMAIN_MIN -0.1 0 0\nDOMAIN_MAX 1.2 1 1.5\n";
  for (int b = 0; b < 5; ++b) {
    for (int g = 0; g < 5; ++g) {
      for (int r = 0; r < 5; ++r) text << (r * r) / 16.0 << " " << (g + 2 * r) / 12.0 << " " << std::sqrt(b / 4.0) * (1 + g) / 5.0 << "\n";
    }
  }
  std::istringstream in(text.str());
  const auto lut = CubeLut::Parse(in);
  float worst = 0.0f;
  for (int i = 0; i < 400; ++i) {
    const float rgb[3] = {-0.3f + 1.8f * ((i * 7) % 41) / 40.0f, ((i * 13) % 37) / 36.0f, 1.7f * ((i * 3) % 43) / 42.0f};
    float mapped[3];
    lut.Map(rgb, mapped);
    const auto sampled = lut.Sample(rgb[0], rgb[1], rgb[2]);
    for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(mapped[c] - sampled[static_cast<std::size_t>(c)]));
  }
  CHECK(worst == 0.0f);
  // At a lattice point the table's own entry comes back exactly.
  const auto corner = lut.Sample(1.2f, 1.0f, 1.5f);
  CHECK(Near(corner[0], 16.0f / 16.0f) && Near(corner[1], (4 + 8) / 12.0f));
  // The same trilinear value as written out by hand for one point inside a cell.
  const auto inside = lut.Sample(0.275f, 0.5f, 0.75f);   // r: (0.275+0.1)/1.3*4 = 1.1538, g: 2, b: 2
  const float fr = (0.275f + 0.1f) / 1.3f * 4.0f - 1.0f;
  const float r_low = 1.0f / 16.0f, r_high = 4.0f / 16.0f;
  CHECK(Near(inside[0], r_low + (r_high - r_low) * fr, 1e-5f));
}

CUTLINE_TEST(RowsRunOnEveryCoreEachExactlyOnceAndCallersThatMeetDoNotWaitForEachOther) {
  using cutline::util::ParallelRows;
  constexpr int kRows = 1500;
  std::vector<std::atomic<int>> seen(kRows);
  ParallelRows(0, kRows - 1, [&](int y) { seen[static_cast<std::size_t>(y)].fetch_add(1); });
  for (int y = 0; y < kRows; ++y) CHECK_EQ(seen[static_cast<std::size_t>(y)].load(), 1);
  // A range that is not a multiple of a block, one that is short, one that is empty.
  std::atomic<int> total{0};
  ParallelRows(7, 7 + 99, [&](int) { total.fetch_add(1); });
  ParallelRows(0, 5, [&](int) { total.fetch_add(1); });
  ParallelRows(5, 4, [&](int) { total.fetch_add(1000); });
  CHECK_EQ(total.load(), 106);
  // Two threads calling at once (an export and the monitor): both finish, every row of both is done once.
  std::vector<std::atomic<int>> first(kRows), second(kRows);
  std::thread other([&] {
    for (int round = 0; round < 20; ++round) ParallelRows(0, kRows - 1, [&](int y) { second[static_cast<std::size_t>(y)].fetch_add(1); });
  });
  for (int round = 0; round < 20; ++round) ParallelRows(0, kRows - 1, [&](int y) { first[static_cast<std::size_t>(y)].fetch_add(1); });
  other.join();
  for (int y = 0; y < kRows; ++y) CHECK(first[static_cast<std::size_t>(y)].load() == 20 && second[static_cast<std::size_t>(y)].load() == 20);
}

CUTLINE_TEST(MissingLutIsReportedAndBypassed) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-render-lut-relink";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  auto effect = MakeEffect("fx-lut", "lut", {{"intensity", Value::Scalar(1.0)}});
  effect.preset_name = "late.cube";
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.2f, 0.4f, 0.8f));
  auto config = FloatOutput();
  config.asset_root = directory.string();
  Compositor compositor(config);
  Statistics statistics;
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = compositor.Compose(plan, frames.Resolver(), statistics);
  CHECK_EQ(statistics.effect_errors.size(), std::size_t{1});
  CHECK(Near(Centre(output).r, 0.2f));
  CHECK(cutline::render::IsBuiltInEffect("lut"));

  // The same compositor retries when an offline asset appears; playback does
  // not have to be restarted after relinking or restoring a project package.
  {
    std::ofstream lut(directory / "late.cube", std::ios::trunc);
    lut << "LUT_3D_SIZE 2\n"
           "1 1 1\n0 1 1\n1 0 1\n0 0 1\n"
           "1 1 0\n0 1 0\n1 0 0\n0 0 0\n";
  }
  Statistics recovered_statistics;
  const auto recovered = compositor.Compose(plan, frames.Resolver(), recovered_statistics);
  CHECK(recovered_statistics.effect_errors.empty());
  CHECK(Near(Centre(recovered).r, 0.8f, 1e-3f));
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(StabilizerAnalysisFindsAndSmoothsCameraShake) {
  const auto shifted = [](int shift) {
    auto frame = SolidFrame(0.0f, 0.0f, 0.0f);
    for (int y = 0; y < kHeight; ++y) {
      auto* row = frame.row_f32(y);
      for (int x = 0; x < kWidth; ++x) {
        const auto source_x = x - shift;
        if (source_x < 0 || source_x >= kWidth) continue;
        const auto value = static_cast<float>(((source_x * 37 + y * 17 + source_x * y * 3) % 101) / 100.0);
        auto* pixel = row + static_cast<std::size_t>(x) * 4;
        pixel[0] = pixel[1] = pixel[2] = value;
      }
    }
    return frame;
  };
  std::vector<VideoFrame> frames;
  frames.push_back(shifted(0));
  frames.push_back(shifted(3));
  frames.push_back(shifted(0));
  cutline::render::StabilizerConfig config;
  config.analysis_width = kWidth;
  config.max_translation_pixels = 4;
  config.smoothing_radius_frames = 1;
  config.crop_safety = 1.0f;
  const auto result = cutline::render::AnalyzeStabilization(
      frames.size(), rates::kFrameRate25, [&frames](std::size_t index) { return &frames[index]; }, config);
  CHECK_EQ(result.samples.size(), std::size_t{3});
  CHECK(Near(result.samples[0].correction_x, 0.0f));
  CHECK(Near(result.samples[1].correction_x, -3.0f, 0.1f));
  CHECK(Near(result.samples[2].correction_x, 0.0f, 0.1f));
  CHECK(result.auto_scale > 1.0f);

  const auto effect = cutline::render::MakeStabilizerEffect(result, "fx-stabilizer");
  CHECK_EQ(effect.effect_type, std::string("stabilizer"));
  CHECK_EQ(effect.parameters.size(), std::size_t{2});
  CHECK_EQ(effect.parameters[0].value.keyframes().size(), std::size_t{3});
}

CUTLINE_TEST(StabilizerCorrectionIsAppliedAsReversibleMotion) {
  cutline::render::StabilizationResult analysis;
  analysis.source_width = kWidth;
  analysis.source_height = kHeight;
  analysis.auto_scale = 1.0f;
  analysis.samples = {{Seconds(0), 0.0f, 0.0f}, {Seconds(1), -5.0f, 0.0f}};

  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(cutline::render::MakeStabilizerEffect(analysis, "fx-stabilizer"));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 1.0f, 1.0f));
  const auto output = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)),
                                                         frames.Resolver());
  CHECK(Near(Centre(output).r, 1.0f));
  CHECK(Near(At(output, kWidth - 1, kHeight / 2).r, 0.0f));
  CHECK(cutline::render::IsBuiltInEffect("stabilizer"));
}

// -------------------------------------------------------------- geometry ----

CUTLINE_TEST(ScalingShrinksTheImageAboutTheCentre) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-motion", "motion", {{"scale", Value::Vec2(50.0, 50.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 1.0f, 1.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  // The output canvas is opaque, so coverage is read from colour: a white
  // source over the black background shows where the image landed.
  CHECK(Near(Centre(output).r, 1.0f));
  CHECK(Near(At(output, 1, 1).r, 0.0f));
}

CUTLINE_TEST(PositionTranslatesTheImage) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect(
      "fx-motion", "motion", {{"scale", Value::Vec2(50.0, 50.0)}, {"position", Value::Vec2(-16.0, 0.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 1.0f, 1.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  // Shifted 16 px left: present left of centre, gone well right of it.
  CHECK(Near(At(output, kWidth / 2 - 16, kHeight / 2).r, 1.0f));
  CHECK(Near(At(output, kWidth - 2, kHeight / 2).r, 0.0f));
}

CUTLINE_TEST(CropRemovesPartOfTheSource) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  // Drop the left half of the source.
  clip.effects.push_back(MakeEffect("fx-crop", "crop", {{"left", Value::Scalar(0.5)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 1.0f, 1.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  CHECK(Near(At(output, 2, kHeight / 2).r, 0.0f));
  CHECK(Near(At(output, kWidth - 3, kHeight / 2).r, 1.0f));
}

// ------------------------------------------------------- adjustment layers ----

CUTLINE_TEST(AnAdjustmentClipGradesEverythingBeneathIt) {
  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));

  auto upper = MakeTrack("v2", 1);
  Clip adjustment;
  adjustment.id = "adj-1";
  adjustment.source_kind = model::SourceKind::Adjustment;
  adjustment.source_in = Seconds(0);
  adjustment.source_out = Seconds(10);
  adjustment.timeline_start = Seconds(0);
  adjustment.effects.push_back(MakeEffect("fx-adj", "grade", {{"exposure", Value::Scalar(1.0)}}));
  upper.clips.push_back(std::move(adjustment));

  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.25f, 0.25f, 0.25f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  Statistics statistics;
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver(), statistics);
  CHECK_EQ(statistics.adjustment_layers, 1);
  // The clip beneath has been pushed up one stop.
  CHECK(Near(Centre(output).r, 0.5f));
}

CUTLINE_TEST(SequenceEffectsApplyToTheFinishedComposite) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(track));
  sequence.effects.push_back(MakeEffect("fx-master", "grade", {{"exposure", Value::Scalar(-1.0)}}));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(0.5f, 0.5f, 0.5f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver());
  CHECK(Near(Centre(output).r, 0.25f));
}

CUTLINE_TEST(UnknownEffectsAreReportedRatherThanSilentlyIgnored) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx-warp", "warp_stabilizer", {{"smoothness", Value::Scalar(50.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", SolidFrame(1.0f, 1.0f, 1.0f));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  Statistics statistics;
  const auto output = Compositor(FloatOutput()).Compose(plan, frames.Resolver(), statistics);
  CHECK_EQ(statistics.skipped_effects.size(), std::size_t{1});
  CHECK_EQ(statistics.skipped_effects[0], std::string("warp_stabilizer"));
  // The clip still renders; only the unimplemented effect is absent.
  CHECK(Near(Centre(output).r, 1.0f));
  CHECK(!cutline::render::IsBuiltInEffect("warp_stabilizer"));
  CHECK(cutline::render::IsBuiltInEffect("opacity"));
}

CUTLINE_TEST(ASolidGeneratorNeedsNoDecodedSource) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  Clip clip;
  clip.id = "clip-solid";
  clip.source_kind = model::SourceKind::Adjustment;
  clip.source_in = Seconds(0);
  clip.source_out = Seconds(10);
  clip.timeline_start = Seconds(0);
  clip.effects.push_back(MakeEffect("fx-solid", "solid", {{"color", Value::Vec3(0.0, 0.0, 1.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  // Adjustment clips apply to what is beneath, so put the generator on its own
  // track above a black base to show the fill.
  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  Statistics statistics;
  const auto output = Compositor(FloatOutput()).Compose(plan, Frames{}.Resolver(), statistics);
  // No decode was attempted for a generator.
  CHECK_EQ(statistics.missing_frames, 0);
  CHECK_EQ(output.width(), kWidth);
}

// --------------------------------------------------------- golden frames ----

CUTLINE_TEST(GoldenColourBars) {
  // Catches channel order, stride, and row-orientation mistakes that a
  // flat-colour test cannot see.
  auto source = cutline::media::OpenSynthetic(
      cutline::media::SyntheticSpec{cutline::media::SyntheticPattern::Bars, kWidth, kHeight, {25, 1}, Seconds(4)});
  auto decoded = source->ReadVideo(Seconds(1));
  CHECK(decoded.has_value());

  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 4));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", std::move(*decoded));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor{}.Compose(plan, frames.Resolver());
  CHECK_GOLDEN(output, "bars");
}

CUTLINE_TEST(GoldenDissolveMidpoint) {
  auto bars = cutline::media::OpenSynthetic(
      cutline::media::SyntheticSpec{cutline::media::SyntheticPattern::Bars, kWidth, kHeight, {25, 1}, Seconds(12)});
  cutline::media::SyntheticSpec gradient_spec;
  gradient_spec.pattern = cutline::media::SyntheticPattern::Gradient;
  gradient_spec.width = kWidth;
  gradient_spec.height = kHeight;
  gradient_spec.duration = Seconds(12);
  auto gradient = cutline::media::OpenSynthetic(gradient_spec);

  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-a", "media-1", 0, 6));
  track.clips.push_back(MakeClip("clip-b", "media-2", 6, 6));
  Transition dissolve;
  dissolve.id = "t-1";
  dissolve.kind = "cross_dissolve";
  dissolve.from_clip_id = "clip-a";
  dissolve.to_clip_id = "clip-b";
  dissolve.timeline_start = Seconds(5);
  dissolve.duration = Seconds(2);
  track.transitions.push_back(std::move(dissolve));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-a", *bars->ReadVideo(Seconds(5)));
  frames.Add("clip-b", *gradient->ReadVideo(Seconds(0)));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(6));
  const auto output = Compositor{}.Compose(plan, frames.Resolver());
  CHECK_GOLDEN(output, "dissolve-midpoint");
}

CUTLINE_TEST(GoldenTransformedPictureInPicture) {
  // Scale, offset, and rotation together: the case where a half-pixel or
  // inverted-matrix error shows up immediately.
  cutline::media::SyntheticSpec spec;
  spec.pattern = cutline::media::SyntheticPattern::Bars;
  spec.width = kWidth;
  spec.height = kHeight;
  spec.duration = Seconds(4);
  auto source = cutline::media::OpenSynthetic(spec);

  cutline::media::SyntheticSpec base_spec;
  base_spec.pattern = cutline::media::SyntheticPattern::Gradient;
  base_spec.width = kWidth;
  base_spec.height = kHeight;
  base_spec.duration = Seconds(4);
  auto base = cutline::media::OpenSynthetic(base_spec);

  auto sequence = MakeSequence();
  auto lower = MakeTrack("v1", 0);
  lower.clips.push_back(MakeClip("clip-base", "media-base", 0, 4));
  auto upper = MakeTrack("v2", 1);
  auto inset = MakeClip("clip-inset", "media-inset", 0, 4);
  inset.effects.push_back(MakeEffect("fx-motion", "motion",
                                     {{"scale", Value::Vec2(40.0, 40.0)},
                                      {"position", Value::Vec2(12.0, -6.0)},
                                      {"rotation", Value::Scalar(15.0)}}));
  inset.effects.push_back(MakeEffect("fx-op", "opacity", {{"value", Value::Scalar(0.85)}}));
  upper.clips.push_back(std::move(inset));
  sequence.tracks.push_back(std::move(lower));
  sequence.tracks.push_back(std::move(upper));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-base", *base->ReadVideo(Seconds(1)));
  frames.Add("clip-inset", *source->ReadVideo(Seconds(1)));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor{}.Compose(plan, frames.Resolver());
  CHECK_GOLDEN(output, "transform-pip");
}

CUTLINE_TEST(GoldenGradedBars) {
  cutline::media::SyntheticSpec spec;
  spec.pattern = cutline::media::SyntheticPattern::Bars;
  spec.width = kWidth;
  spec.height = kHeight;
  spec.duration = Seconds(4);
  auto source = cutline::media::OpenSynthetic(spec);

  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 4);
  clip.effects.push_back(MakeEffect("fx-grade", "lumetri",
                                    {{"exposure", Value::Scalar(-0.5)},
                                     {"contrast", Value::Scalar(130.0)},
                                     {"saturation", Value::Scalar(60.0)},
                                     {"temperature", Value::Scalar(25.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  Frames frames;
  frames.Add("clip-1", *source->ReadVideo(Seconds(1)));

  const auto plan = TimelineCompiler{}.Compile(sequence, Seconds(1));
  const auto output = Compositor{}.Compose(plan, frames.Resolver());
  CHECK_GOLDEN(output, "graded-bars");
}

// ----------------------------------------- production filters, keying, grading ----

namespace {

void SetPixel(VideoFrame& frame, int x, int y, float r, float g, float b, float a = 1.0f) {
  auto* texel = frame.row_f32(y) + static_cast<std::size_t>(x) * 4;
  texel[0] = r;
  texel[1] = g;
  texel[2] = b;
  texel[3] = a;
}

using Parameters = std::vector<std::pair<std::string, Value>>;

void Expect(bool condition, const std::string& description, int line) {
  if (!condition) cutline::testing::Fail("comparison", __FILE__, line, description);
}

// One clip carrying one effect, composed over a background.
VideoFrame ApplyEffect(const std::string& type, Parameters parameters, const VideoFrame& input, float background = 0.0f,
                       Statistics* statistics = nullptr) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx", type, std::move(parameters)));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  Frames frames;
  frames.Add("clip-1", input.Clone());
  auto config = FloatOutput();
  config.background_red = config.background_green = config.background_blue = background;
  Statistics ignored;
  return Compositor(config).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)), frames.Resolver(),
                                    statistics != nullptr ? *statistics : ignored);
}

VideoFrame Impulse(float value = 1.0f) {
  auto frame = SolidFrame(0.0f, 0.0f, 0.0f);
  SetPixel(frame, kWidth / 2, kHeight / 2, value, value, value);
  return frame;
}

// The red channel climbs from 0 at the left edge to 1 at the right.
VideoFrame HorizontalRamp() {
  auto frame = SolidFrame(0.0f, 0.0f, 0.0f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) SetPixel(frame, x, y, static_cast<float>(x) / static_cast<float>(kWidth - 1), 0.0f, 0.0f);
  }
  return frame;
}

double SumRed(const VideoFrame& frame) {
  double sum = 0.0;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) sum += At(frame, x, y).r;
  }
  return sum;
}

Value Vec3Of(double x, double y, double z) { return Value::Vec3(x, y, z); }

}  // namespace

CUTLINE_TEST(GaussianBlurConservesLightAndSpreadsItWithTheRequestedSigma) {
  const double sigma = 3.0;
  const auto output = ApplyEffect("gaussian_blur", {{"radius", Value::Scalar(sigma)}}, Impulse());
  CHECK(std::abs(SumRed(output) - 1.0) < 1e-3);
  // The variance of the spread along a row is sigma squared, to within what a three-box
  // approximation gives (a few per cent).
  double mass = 0.0, mean = 0.0, second = 0.0;
  for (int x = 0; x < kWidth; ++x) {
    const auto value = At(output, x, kHeight / 2).r;
    mass += value;
    mean += value * x;
  }
  mean /= mass;
  for (int x = 0; x < kWidth; ++x) second += At(output, x, kHeight / 2).r * (x - mean) * (x - mean);
  second /= mass;
  CHECK(std::abs(second - sigma * sigma) < 0.12 * sigma * sigma);
  // Symmetric about the impulse.
  CHECK(Near(At(output, kWidth / 2 - 3, kHeight / 2).r, At(output, kWidth / 2 + 3, kHeight / 2).r, 1e-6f));
  CHECK(Near(At(output, kWidth / 2, kHeight / 2 - 2).r, At(output, kWidth / 2, kHeight / 2 + 2).r, 1e-6f));
  // And at zero it does nothing.
  const auto untouched = ApplyEffect("gaussian_blur", {{"radius", Value::Scalar(0.0)}}, Impulse());
  CHECK(Near(Centre(untouched).r, 1.0f, 1e-6f));
}

CUTLINE_TEST(ADirectionalBlurSmearsAlongItsAngleAndNowhereElse) {
  const auto horizontal = ApplyEffect("directional_blur", {{"length", Value::Scalar(8.0)}, {"angle", Value::Scalar(0.0)}}, Impulse());
  CHECK(std::abs(SumRed(horizontal) - 1.0) < 2e-2);
  CHECK(At(horizontal, kWidth / 2 + 3, kHeight / 2).r > 0.02f);
  CHECK(At(horizontal, kWidth / 2 - 3, kHeight / 2).r > 0.02f);
  CHECK(At(horizontal, kWidth / 2 + 7, kHeight / 2).r < 1e-6f);
  CHECK(At(horizontal, kWidth / 2, kHeight / 2 + 1).r < 0.02f);
  const auto vertical = ApplyEffect("directional_blur", {{"length", Value::Scalar(8.0)}, {"angle", Value::Scalar(90.0)}}, Impulse());
  CHECK(At(vertical, kWidth / 2, kHeight / 2 + 3).r > 0.02f);
  CHECK(At(vertical, kWidth / 2 + 3, kHeight / 2).r < 0.02f);
}

CUTLINE_TEST(UnsharpMaskOvershootsAnEdgeAndLeavesFlatAreasAlone) {
  auto edge = SolidFrame(0.3f, 0.3f, 0.3f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = kWidth / 2; x < kWidth; ++x) SetPixel(edge, x, y, 0.7f, 0.7f, 0.7f);
  }
  const Parameters strong{{"amount", Value::Scalar(1.5)}, {"radius", Value::Scalar(2.0)}};
  const auto sharpened = ApplyEffect("unsharp_mask", strong, edge);
  CHECK(At(sharpened, kWidth / 2 - 1, kHeight / 2).r < 0.3f - 0.02f);  // the dark side goes darker
  CHECK(At(sharpened, kWidth / 2, kHeight / 2).r > 0.7f + 0.02f);      // the light side lighter
  CHECK(Near(At(sharpened, 4, kHeight / 2).r, 0.3f, 1e-4f));            // far from the edge nothing moves
  CHECK(Near(At(sharpened, kWidth - 4, kHeight / 2).r, 0.7f, 1e-4f));
  // Below the threshold, nothing is touched.
  const auto gated = ApplyEffect("unsharp_mask", {{"amount", Value::Scalar(1.5)}, {"radius", Value::Scalar(2.0)}, {"threshold", Value::Scalar(0.9)}}, edge);
  CHECK(Near(At(gated, kWidth / 2, kHeight / 2).r, 0.7f, 1e-6f));
}

CUTLINE_TEST(GlowAddsLightAroundBrightAreasAndNeverPastFullScale) {
  auto frame = SolidFrame(0.05f, 0.05f, 0.05f);
  for (int y = 16; y < 20; ++y) {
    for (int x = 30; x < 34; ++x) SetPixel(frame, x, y, 1.0f, 1.0f, 1.0f);
  }
  const auto output = ApplyEffect("glow", {{"threshold", Value::Scalar(0.5)}, {"radius", Value::Scalar(3.0)}, {"intensity", Value::Scalar(1.0)}}, frame);
  CHECK(At(output, 29, 17).r > 0.05f + 0.05f);  // beside the square: lit
  CHECK(Near(At(output, 4, 4).r, 0.05f, 1e-4f));  // far away: untouched
  float peak = 0.0f;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) peak = std::max(peak, At(output, x, y).r);
  }
  CHECK(peak <= 1.0f);
  CHECK(Near(At(output, 31, 17).r, 1.0f, 1e-4f));
  // Intensity zero is no glow.
  const auto none = ApplyEffect("glow", {{"threshold", Value::Scalar(0.5)}, {"radius", Value::Scalar(3.0)}, {"intensity", Value::Scalar(0.0)}}, frame);
  CHECK(Near(At(none, 29, 17).r, 0.05f, 1e-6f));
}

CUTLINE_TEST(ADropShadowFallsBehindTheSubjectInItsDirection) {
  // An opaque 8x8 square on a transparent frame; a red shadow six pixels to the right.
  auto frame = SolidFrame(0.0f, 0.0f, 0.0f, 0.0f);
  for (int y = 14; y < 22; ++y) {
    for (int x = 20; x < 28; ++x) SetPixel(frame, x, y, 1.0f, 1.0f, 1.0f, 1.0f);
  }
  const auto output = ApplyEffect("drop_shadow",
                                  {{"color", Vec3Of(1.0, 0.0, 0.0)}, {"opacity", Value::Scalar(1.0)}, {"distance", Value::Scalar(6.0)},
                                   {"angle", Value::Scalar(0.0)}, {"softness", Value::Scalar(0.0)}},
                                  frame);
  CHECK(Near(At(output, 23, 17).g, 1.0f, 1e-4f));    // the subject stays on top, white
  CHECK(Near(At(output, 30, 17).r, 1.0f, 1e-4f));    // the shadow to its right is red
  CHECK(Near(At(output, 30, 17).g, 0.0f, 1e-4f));
  CHECK(Near(At(output, 18, 17).r, 0.0f, 1e-4f));    // nothing on the other side
  CHECK(Near(At(output, 23, 25).r, 0.0f, 1e-4f));    // nothing below
  // Opacity scales it; zero means no shadow.
  const auto faint = ApplyEffect("drop_shadow", {{"color", Vec3Of(1.0, 0.0, 0.0)}, {"opacity", Value::Scalar(0.5)}, {"distance", Value::Scalar(6.0)}, {"angle", Value::Scalar(0.0)}, {"softness", Value::Scalar(0.0)}}, frame);
  CHECK(Near(At(faint, 30, 17).r, 0.5f, 1e-4f));
}

CUTLINE_TEST(ChannelMixerRoutesChannelsByTheMatrix) {
  const auto output = ApplyEffect("channel_mixer",
                                  {{"red", Vec3Of(0.0, 0.0, 1.0)}, {"green", Vec3Of(0.0, 1.0, 0.0)}, {"blue", Vec3Of(1.0, 0.0, 0.0)}},
                                  SolidFrame(1.0f, 0.5f, 0.0f));
  CHECK(Near(Centre(output).r, 0.0f));
  CHECK(Near(Centre(output).g, 0.5f));
  CHECK(Near(Centre(output).b, 1.0f));
  // A row of weights mixes: red takes half of red and half of green.
  const auto mixed = ApplyEffect("channel_mixer", {{"red", Vec3Of(0.5, 0.5, 0.0)}}, SolidFrame(1.0f, 0.0f, 0.0f));
  CHECK(Near(Centre(mixed).r, 0.5f));
}

CUTLINE_TEST(BlackAndWhiteWeighsTheChannelsItIsGiven) {
  const auto luma = ApplyEffect("black_and_white", {}, SolidFrame(1.0f, 0.0f, 0.0f));
  CHECK(Near(Centre(luma).r, 0.2126f, 1e-4f));
  CHECK(Near(Centre(luma).g, 0.2126f, 1e-4f));
  const auto red_filter = ApplyEffect("black_and_white", {{"weights", Vec3Of(1.0, 0.0, 0.0)}}, SolidFrame(1.0f, 0.3f, 0.0f));
  CHECK(Near(Centre(red_filter).g, 1.0f, 1e-4f));
  const auto half = ApplyEffect("black_and_white", {{"amount", Value::Scalar(0.5)}}, SolidFrame(1.0f, 0.0f, 0.0f));
  CHECK(Near(Centre(half).r, 0.5f * (1.0f + 0.2126f), 1e-4f));
  CHECK(Near(Centre(half).g, 0.5f * 0.2126f, 1e-4f));
}

CUTLINE_TEST(TintMapsTheTonalRangeOntoTwoColours) {
  const Parameters map{{"map_black", Vec3Of(0.0, 0.0, 0.2)}, {"map_white", Vec3Of(1.0, 0.8, 0.2)}};
  const auto output = ApplyEffect("tint", map, SolidFrame(0.5f, 0.5f, 0.5f));
  CHECK(Near(Centre(output).r, 0.5f, 1e-4f));
  CHECK(Near(Centre(output).g, 0.4f, 1e-4f));
  CHECK(Near(Centre(output).b, 0.2f, 1e-4f));
  const auto dark = ApplyEffect("tint", map, SolidFrame(0.0f, 0.0f, 0.0f));
  CHECK(Near(Centre(dark).b, 0.2f, 1e-4f));
  auto half = map;
  half.emplace_back("amount", Value::Scalar(0.0));
  CHECK(Near(Centre(ApplyEffect("tint", half, SolidFrame(0.5f, 0.5f, 0.5f))).b, 0.5f, 1e-4f));
}

CUTLINE_TEST(PosterizeSnapsEachChannelToTheNearestLevel) {
  const auto four = ApplyEffect("posterize", {{"levels", Value::Scalar(4.0)}}, SolidFrame(0.4f, 0.9f, 0.1f));
  CHECK(Near(Centre(four).r, 1.0f / 3.0f, 1e-4f));
  CHECK(Near(Centre(four).g, 1.0f, 1e-4f));
  CHECK(Near(Centre(four).b, 0.0f, 1e-4f));
  const auto off = ApplyEffect("posterize", {{"levels", Value::Scalar(256.0)}}, SolidFrame(0.4f, 0.9f, 0.1f));
  CHECK(Near(Centre(off).r, 0.4f, 1e-6f));
}

CUTLINE_TEST(AWaveWarpDisplacesRowsByTheSineAndKeepsTheRestOfThePicture) {
  const double amplitude = 4.0, wavelength = 36.0;
  const auto output = ApplyEffect("wave_warp", {{"amplitude", Value::Scalar(amplitude)}, {"wavelength", Value::Scalar(wavelength)}}, HorizontalRamp());
  const double tau = 6.283185307179586;
  for (const int y : {0, 8, 18, 27, 35}) {
    const int x = 30;
    const auto sx = x + 0.5 + amplitude * std::sin(tau * (y + 0.5) / wavelength);
    CHECK(Near(At(output, x, y).r, static_cast<float>((sx - 0.5) / (kWidth - 1)), 2e-3f));
  }
  // Vertical waves displace columns instead.
  const auto vertical = ApplyEffect("wave_warp", {{"amplitude", Value::Scalar(amplitude)}, {"wavelength", Value::Scalar(wavelength)}, {"vertical", Value::Scalar(1.0)}}, HorizontalRamp());
  // The ramp does not change down a column, so a vertical displacement leaves it as it was.
  CHECK(Near(At(vertical, 30, 10).r, 30.0f / (kWidth - 1), 2e-3f));
}

CUTLINE_TEST(AMeshWarpInterpolatesControlOffsetsAcrossThePicture) {
  cutline::render::Layer layer;
  cutline::render::Layer scratch;
  layer.Reset(8, 8);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) layer.at(x, y) = {static_cast<float>(x) / 7.0f, 0.0f, 0.0f, 1.0f};
  }
  layer.MarkWhole();
  cutline::render::WarpMesh mesh;
  mesh.columns = 2;
  mesh.rows = 2;
  mesh.offsets = {{0.0f, 0.0f}, {0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 0.0f}};
  cutline::render::ApplyMeshWarp(layer, scratch, mesh);
  CHECK(Near(layer.at(3, 0).r, 3.0f / 7.0f));
  CHECK(Near(layer.at(3, 7).r, 5.0f / 7.0f));
  CHECK(layer.at(7, 7).a == 0.0f);  // inverse map falls beyond the source
}

CUTLINE_TEST(TheMeshWarpEffectBendsTheGridPointsItIsGivenAndLeavesTheRestAlone) {
  // Pull the bottom edge's points two pixels sideways: the top row stays, the bottom row moves, in between is a blend.
  Parameters shifted;
  for (int column = 0; column < 4; ++column) shifted.emplace_back("point_3_" + std::to_string(column), Value::Vec2(2.0, 0.0));
  const auto frame = HorizontalRamp();
  const auto output = ApplyEffect("mesh_warp", shifted, frame);
  CHECK(Near(At(output, 5, 0).r, At(frame, 5, 0).r, 1e-5f));
  CHECK(Near(At(output, 5, kHeight - 1).r, At(frame, 7, kHeight - 1).r, 1e-4f));
  const auto middle = At(output, 5, (kHeight - 1) * 5 / 6).r;
  CHECK(middle > At(frame, 5, (kHeight - 1) * 5 / 6).r && middle < At(frame, 7, (kHeight - 1) * 5 / 6).r);
}

CUTLINE_TEST(RollingShutterRepairChangesEachScanLineByItsCaptureTime) {
  cutline::render::Layer layer;
  cutline::render::Layer scratch;
  layer.Reset(8, 5);
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 8; ++x) layer.at(x, y) = {x == 4 ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f};
  }
  layer.MarkWhole();
  cutline::render::RollingShutterSettings settings;
  settings.horizontal = 2.0f;
  cutline::render::ApplyRollingShutter(layer, scratch, settings);
  CHECK(Near(layer.at(4, 0).r, 1.0f));
  CHECK(Near(layer.at(2, 4).r, 1.0f));
  CHECK(Near(layer.at(4, 4).r, 0.0f));
}

CUTLINE_TEST(ABulgeMagnifiesTheCentreAndLeavesEverythingOutsideItsRadius) {
  const auto base = HorizontalRamp();
  const auto output = ApplyEffect("bulge", {{"amount", Value::Scalar(0.5)}, {"radius", Value::Scalar(1.0)}}, base);
  // The radius is half the shorter side: 18 px around the centre (32, 18).
  const float radius = 18.0f;
  const int x = 41;  // 9 px right of centre, on the row through it
  const float dx = static_cast<float>(x) + 0.5f - 32.0f;
  const float falloff = 1.0f - std::abs(dx) / radius;
  const float source = 32.0f + dx * (1.0f - 0.5f * falloff * falloff);
  CHECK(Near(At(output, x, kHeight / 2 - 1).r, (source - 0.5f) / (kWidth - 1), 3e-3f));
  // The middle reads from nearer the middle than it is: magnified.
  CHECK(At(output, x, kHeight / 2 - 1).r < static_cast<float>(x) / (kWidth - 1));
  // Outside the radius, and at zero amount, nothing changes.
  CHECK(Near(At(output, 60, 2).r, 60.0f / (kWidth - 1), 1e-6f));
  const auto none = ApplyEffect("bulge", {{"amount", Value::Scalar(0.0)}}, base);
  CHECK(Near(At(none, x, 10).r, static_cast<float>(x) / (kWidth - 1), 1e-6f));
}

// --------------------------------------------------------------------- keying ----

namespace {

// A green screen (lit unevenly, with a darker lower half) with a skin-toned subject in
// the middle third and, either side of it, an edge column that is 35% subject and 65% screen.
VideoFrame GreenScreen() {
  auto frame = SolidFrame(0.0f, 0.8f, 0.0f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      if (y >= kHeight / 2) SetPixel(frame, x, y, 0.0f, 0.4f, 0.0f);  // a shadowed half of the screen
    }
  }
  for (int y = 6; y < 30; ++y) {
    for (int x = 22; x < 42; ++x) SetPixel(frame, x, y, 0.8f, 0.55f, 0.45f);
    // Edge pixels: the subject blended 50% with whatever screen is behind.
    const float screen_g = y >= kHeight / 2 ? 0.4f : 0.8f;
    const float mixed_g = 0.35f * 0.55f + 0.65f * screen_g;
    SetPixel(frame, 21, y, 0.35f * 0.8f, mixed_g, 0.35f * 0.45f);
    SetPixel(frame, 42, y, 0.35f * 0.8f, mixed_g, 0.35f * 0.45f);
  }
  return frame;
}

}  // namespace

CUTLINE_TEST(AChromaKeyRemovesAScreenOfAnyBrightnessAndKeepsTheSubject) {
  const auto matte = ApplyEffect("chroma_key", {{"view", Value::Scalar(1.0)}}, GreenScreen());
  CHECK(Near(At(matte, 4, 4).r, 0.0f, 1e-3f));    // lit screen
  CHECK(Near(At(matte, 4, 30).r, 0.0f, 1e-3f));   // shadowed screen
  CHECK(Near(At(matte, 32, 18).r, 1.0f, 1e-3f));  // subject
  CHECK(Near(At(matte, 32, 10).r, 1.0f, 1e-3f));
  // Composited over blue, the screen is gone and the subject is as it was.
  const auto over_black = ApplyEffect("chroma_key", {}, GreenScreen(), 0.0f);
  CHECK(Near(At(over_black, 4, 4).g, 0.0f, 1e-3f));
  CHECK(Near(At(over_black, 32, 18).r, 0.8f, 1e-3f));
  CHECK(Near(At(over_black, 32, 18).g, 0.55f, 1e-3f));
  const auto over_grey = ApplyEffect("chroma_key", {}, GreenScreen(), 0.5f);
  CHECK(Near(At(over_grey, 4, 4).g, 0.5f, 1e-3f));  // the screen shows what is behind it
}

CUTLINE_TEST(ChromaKeySuppressesSpillSoNoGreenHaloRemainsAtTheEdge) {
  // The edge column is half subject, half screen. Its matte is partial, and its colour,
  // once the spill is pulled out, must not be greener than a balance of its red and blue.
  const auto matte = ApplyEffect("chroma_key", {{"view", Value::Scalar(1.0)}}, GreenScreen());
  const auto edge_alpha = At(matte, 21, 10).r;
  CHECK(edge_alpha > 0.05f && edge_alpha < 0.95f);  // a partial matte, not a hard cut
  const auto despilled = ApplyEffect("chroma_key", {{"spill", Value::Scalar(1.0)}}, GreenScreen());
  const auto edge = At(despilled, 21, 10);
  {
    const float r = edge.r / edge_alpha, g = edge.g / edge_alpha, b = edge.b / edge_alpha;
    CHECK(g <= (r + b) * 0.5f + 1e-3f);
  }
  // With no suppression the same edge is greener: the halo is what the control removes.
  const auto raw = ApplyEffect("chroma_key", {{"spill", Value::Scalar(0.0)}}, GreenScreen());
  CHECK(At(raw, 21, 10).g > edge.g + 0.01f);
}

CUTLINE_TEST(GarbageAndCoreMattesOverrideTheKey) {
  // Garbage: everything left of a quarter of the width is out, subject or not.
  // Core: a rectangle inside the green is always kept.
  const auto matte = ApplyEffect("chroma_key",
                                 {{"view", Value::Scalar(1.0)},
                                  {"garbage", Value::Vec4(0.5, 0.0, 1.0, 1.0)},
                                  {"core", Value::Vec4(0.0, 0.0, 0.25, 0.25)}},
                                 GreenScreen());
  CHECK(Near(At(matte, 24, 18).r, 0.0f, 1e-3f));   // subject pixel, left of the garbage edge: keyed out
  CHECK(Near(At(matte, 36, 18).r, 1.0f, 1e-3f));   // subject pixel to the right: kept
  CHECK(Near(At(matte, 4, 4).r, 1.0f, 1e-3f));     // green, but inside the core: kept
  CHECK(Near(At(matte, 4, 30).r, 0.0f, 1e-3f));    // green outside the core: gone
}

CUTLINE_TEST(ShrinkAndFeatherCleanUpTheMatteEdge) {
  auto frame = SolidFrame(0.0f, 0.8f, 0.0f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth / 2; ++x) SetPixel(frame, x, y, 0.8f, 0.55f, 0.45f);
  }
  const auto hard = ApplyEffect("chroma_key", {{"view", Value::Scalar(1.0)}}, frame);
  CHECK(Near(At(hard, 31, 18).r, 1.0f, 1e-3f));
  CHECK(Near(At(hard, 32, 18).r, 0.0f, 1e-3f));
  const auto shrunk = ApplyEffect("chroma_key", {{"view", Value::Scalar(1.0)}, {"shrink", Value::Scalar(2.0)}}, frame);
  CHECK(Near(At(shrunk, 29, 18).r, 1.0f, 1e-3f));
  CHECK(Near(At(shrunk, 30, 18).r, 0.0f, 1e-3f));
  const auto grown = ApplyEffect("chroma_key", {{"view", Value::Scalar(1.0)}, {"shrink", Value::Scalar(-2.0)}}, frame);
  CHECK(Near(At(grown, 33, 18).r, 1.0f, 1e-3f));
  CHECK(Near(At(grown, 34, 18).r, 0.0f, 1e-3f));
  const auto soft = ApplyEffect("chroma_key", {{"view", Value::Scalar(1.0)}, {"feather", Value::Scalar(2.0)}}, frame);
  CHECK(At(soft, 31, 18).r > 0.05f && At(soft, 31, 18).r < 0.95f);
  CHECK(At(soft, 32, 18).r > 0.05f && At(soft, 32, 18).r < 0.95f);
  CHECK(Near(At(soft, 10, 18).r, 1.0f, 1e-3f));
}

CUTLINE_TEST(ALumaKeyRemovesTheDarksOrWithInvertTheLights) {
  auto frame = SolidFrame(0.2f, 0.2f, 0.2f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = kWidth / 2; x < kWidth; ++x) SetPixel(frame, x, y, 0.8f, 0.8f, 0.8f);
  }
  const Parameters sharp{{"threshold", Value::Scalar(0.5)}, {"softness", Value::Scalar(0.0)}, {"view", Value::Scalar(1.0)}};
  const auto dark_out = ApplyEffect("luma_key", sharp, frame);
  CHECK(Near(At(dark_out, 10, 10).r, 0.0f, 1e-3f));
  CHECK(Near(At(dark_out, 50, 10).r, 1.0f, 1e-3f));
  auto inverted = sharp;
  inverted.emplace_back("invert", Value::Scalar(1.0));
  const auto light_out = ApplyEffect("luma_key", inverted, frame);
  CHECK(Near(At(light_out, 10, 10).r, 1.0f, 1e-3f));
  CHECK(Near(At(light_out, 50, 10).r, 0.0f, 1e-3f));
  // A soft key gives intermediate alpha across the softness range.
  const auto soft = ApplyEffect("luma_key", {{"threshold", Value::Scalar(0.1)}, {"softness", Value::Scalar(0.2)}, {"view", Value::Scalar(1.0)}}, SolidFrame(0.2f, 0.2f, 0.2f));
  CHECK(Near(Centre(soft).r, 0.5f, 1e-3f));
}

// ------------------------------------------------------------------- grading ----

CUTLINE_TEST(LiftGammaAndGainFollowTheirDefinitions) {
  const auto gain = ApplyEffect("color_wheels", {{"gain", Value::Vec4(1.0, 1.0, 1.0, 1.6)}}, SolidFrame(0.5f, 0.25f, 0.5f));
  CHECK(Near(Centre(gain).r, 0.8f, 1e-4f));
  CHECK(Near(Centre(gain).g, 0.4f, 1e-4f));
  const auto lift = ApplyEffect("color_wheels", {{"lift", Value::Vec4(0.0, 0.0, 0.0, 0.1)}}, SolidFrame(0.5f, 0.0f, 1.0f));
  CHECK(Near(Centre(lift).r, 0.55f, 1e-4f));  // 0.5 + 0.1 * (1 - 0.5)
  CHECK(Near(Centre(lift).g, 0.1f, 1e-4f));   // black rises by the lift
  CHECK(Near(Centre(lift).b, 1.0f, 1e-4f));   // white does not move
  const auto gamma = ApplyEffect("color_wheels", {{"gamma", Value::Vec4(1.0, 1.0, 1.0, 2.0)}}, SolidFrame(0.25f, 0.0f, 1.0f));
  CHECK(Near(Centre(gamma).r, 0.5f, 1e-4f));  // 0.25 ^ (1/2)
  CHECK(Near(Centre(gamma).g, 0.0f, 1e-4f));
  CHECK(Near(Centre(gamma).b, 1.0f, 1e-4f));
  // Per-channel values tint one channel only.
  const auto red_gain = ApplyEffect("color_wheels", {{"gain", Value::Vec4(1.5, 1.0, 1.0, 1.0)}}, SolidFrame(0.4f, 0.4f, 0.4f));
  CHECK(Near(Centre(red_gain).r, 0.6f, 1e-4f));
  CHECK(Near(Centre(red_gain).g, 0.4f, 1e-4f));
}

CUTLINE_TEST(TonalWheelsTintOnlyTheirPartOfTheTonalRange) {
  const Parameters shadows{{"shadows", Vec3Of(0.2, 0.0, 0.0)}};
  const auto dark = ApplyEffect("color_wheels", shadows, SolidFrame(0.0f, 0.0f, 0.0f));
  CHECK(Near(Centre(dark).r, 0.2f, 1e-4f));  // black is all shadow
  const auto bright = ApplyEffect("color_wheels", shadows, SolidFrame(0.9f, 0.9f, 0.9f));
  CHECK(Near(Centre(bright).r, 0.9f, 1e-4f));  // a highlight is not touched
  const auto mid = ApplyEffect("color_wheels", {{"midtones", Vec3Of(0.0, 0.0, 0.1)}}, SolidFrame(0.5f, 0.5f, 0.5f));
  CHECK(Near(Centre(mid).b, 0.6f, 1e-4f));
  CHECK(Near(Centre(mid).r, 0.5f, 1e-4f));
  const auto highlight = ApplyEffect("color_wheels", {{"highlights", Vec3Of(0.0, 0.0, -0.1)}}, SolidFrame(1.0f, 1.0f, 1.0f));
  CHECK(Near(Centre(highlight).b, 0.9f, 1e-4f));
}

CUTLINE_TEST(CurveTablesAreMonotoneAndPassThroughTheirPoints) {
  float table[256];
  cutline::render::BuildCurveTable(0.25f, 0.5f, 0.75f, table);  // the identity
  for (int i = 0; i < 256; ++i) CHECK(Near(table[i], static_cast<float>(i) / 255.0f, 1e-3f));
  cutline::render::BuildCurveTable(0.1f, 0.5f, 0.9f, table);  // an S
  CHECK(Near(table[0], 0.0f, 1e-6f));
  CHECK(Near(table[255], 1.0f, 1e-6f));
  for (int i = 1; i < 256; ++i) CHECK(table[i] >= table[i - 1] - 1e-6f);
  CHECK(Near(table[64], 0.1f, 0.01f));   // x = 0.2510
  CHECK(Near(table[128], 0.5f, 0.01f));  // x = 0.5020
  CHECK(Near(table[191], 0.9f, 0.01f));  // x = 0.7490
  // Points out of order in height (a dip) must not make the curve overshoot or fold back.
  cutline::render::BuildCurveTable(0.6f, 0.3f, 0.9f, table);
  for (int i = 0; i < 256; ++i) CHECK(table[i] >= 0.0f && table[i] <= 1.0f);
}

CUTLINE_TEST(CurvesReshapeTheTonesAndTheLumaCurveKeepsHue) {
  // An RGB curve that darkens the middle.
  const auto dark = ApplyEffect("curves", {{"master", Vec3Of(0.2, 0.3, 0.7)}}, SolidFrame(0.5f, 0.5f, 0.5f));
  CHECK(Centre(dark).r < 0.4f);
  CHECK(Near(Centre(dark).r, Centre(dark).g, 1e-6f));
  // A single channel's curve moves only that channel.
  const auto red = ApplyEffect("curves", {{"red", Vec3Of(0.4, 0.7, 0.9)}}, SolidFrame(0.5f, 0.5f, 0.5f));
  CHECK(Centre(red).r > 0.6f);
  CHECK(Near(Centre(red).g, 0.5f, 1e-6f));
  // The luma curve scales the colour to its new brightness without changing its balance.
  const auto lifted = ApplyEffect("curves", {{"luma", Vec3Of(0.4, 0.6, 0.8)}}, SolidFrame(0.4f, 0.2f, 0.2f));
  const auto pixel = Centre(lifted);
  CHECK(pixel.r > 0.4f);
  CHECK(Near(pixel.r / pixel.g, 2.0f, 1e-3f));
  CHECK(Near(pixel.g, pixel.b, 1e-6f));
}

CUTLINE_TEST(HueCurvesMoveOnlyTheHuesTheyAreSetFor) {
  // Green (hue 120) is the third knot of the first vector: shift it 60 degrees, to cyan.
  const Parameters shift{{"hue_vs_hue_a", Vec3Of(0.0, 0.0, 60.0)}};
  const auto green = ApplyEffect("hue_curves", shift, SolidFrame(0.0f, 1.0f, 0.0f));
  CHECK(Near(Centre(green).r, 0.0f, 1e-3f));
  CHECK(Near(Centre(green).g, 1.0f, 1e-3f));
  CHECK(Near(Centre(green).b, 1.0f, 1e-3f));
  // Red (hue 0), whose knot is zero, and a grey, which has no hue, stay where they were.
  const auto red = ApplyEffect("hue_curves", shift, SolidFrame(1.0f, 0.0f, 0.0f));
  CHECK(Near(Centre(red).r, 1.0f, 1e-3f));
  CHECK(Near(Centre(red).g, 0.0f, 1e-3f));
  const auto grey = ApplyEffect("hue_curves", shift, SolidFrame(0.5f, 0.5f, 0.5f));
  CHECK(Near(Centre(grey).r, 0.5f, 1e-4f));
  // Saturation: raising it at 240 (the second vector's second knot) deepens a muted blue.
  const auto deeper = ApplyEffect("hue_curves", {{"hue_vs_sat_b", Vec3Of(0.0, 0.5, 0.0)}}, SolidFrame(0.4f, 0.4f, 0.6f));
  CHECK(Centre(deeper).b - Centre(deeper).r > 0.2f);
}

CUTLINE_TEST(ColorAdjustWarmsTheWhiteBalanceAndShapesVibranceAndTone) {
  const auto warm = ApplyEffect("color_adjust", {{"temperature", Value::Scalar(100.0)}}, SolidFrame(0.4f, 0.4f, 0.4f));
  CHECK(Near(Centre(warm).r, 0.5f, 1e-4f));
  CHECK(Near(Centre(warm).g, 0.4f, 1e-4f));
  CHECK(Near(Centre(warm).b, 0.3f, 1e-4f));
  const auto magenta = ApplyEffect("color_adjust", {{"tint", Value::Scalar(100.0)}}, SolidFrame(0.4f, 0.4f, 0.4f));
  CHECK(Near(Centre(magenta).g, 0.3f, 1e-4f));
  // Vibrance lifts a muted colour by more than a saturated one.
  const auto muted_before = SolidFrame(0.5f, 0.4f, 0.4f);
  const auto vivid_before = SolidFrame(1.0f, 0.1f, 0.1f);
  const auto spread = [](const Rgba& c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); };
  const auto muted = ApplyEffect("color_adjust", {{"vibrance", Value::Scalar(100.0)}}, muted_before);
  const auto vivid = ApplyEffect("color_adjust", {{"vibrance", Value::Scalar(100.0)}}, vivid_before);
  CHECK(spread(Centre(muted)) / spread(At(muted_before, 0, 0)) > spread(Centre(vivid)) / spread(At(vivid_before, 0, 0)));
  // Shadows lift the darks and not the lights; highlights pull the lights down.
  const auto lifted = ApplyEffect("color_adjust", {{"shadows", Value::Scalar(100.0)}}, SolidFrame(0.0f, 0.0f, 0.0f));
  CHECK(Near(Centre(lifted).r, 0.25f, 1e-4f));
  const auto bright = ApplyEffect("color_adjust", {{"shadows", Value::Scalar(100.0)}}, SolidFrame(0.9f, 0.9f, 0.9f));
  CHECK(Near(Centre(bright).r, 0.9f, 1e-4f));
  const auto pulled = ApplyEffect("color_adjust", {{"highlights", Value::Scalar(-100.0)}}, SolidFrame(1.0f, 1.0f, 1.0f));
  CHECK(Near(Centre(pulled).r, 0.75f, 1e-4f));
}

CUTLINE_TEST(AnHslSecondaryCorrectsOnlyWhatItsQualifierSelects) {
  // Left half red, right half blue.
  auto frame = SolidFrame(0.0f, 0.0f, 0.8f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth / 2; ++x) SetPixel(frame, x, y, 0.8f, 0.0f, 0.0f);
  }
  const Parameters red_only{{"hue_center", Value::Scalar(0.0)}, {"hue_width", Value::Scalar(60.0)}};
  auto desaturate = red_only;
  desaturate.emplace_back("sat_gain", Value::Scalar(0.0));
  const auto output = ApplyEffect("hsl_secondary", desaturate, frame);
  CHECK(Near(At(output, 10, 10).r, At(output, 10, 10).b, 1e-3f));   // red went grey
  CHECK(Near(At(output, 50, 10).b, 0.8f, 1e-4f));                   // blue is as it was
  CHECK(Near(At(output, 50, 10).r, 0.0f, 1e-4f));
  // The matte view shows the selection, and invert swaps it.
  auto view = red_only;
  view.emplace_back("view", Value::Scalar(1.0));
  const auto matte = ApplyEffect("hsl_secondary", view, frame);
  CHECK(Near(At(matte, 10, 10).r, 1.0f, 1e-4f));
  CHECK(Near(At(matte, 50, 10).r, 0.0f, 1e-4f));
  view.emplace_back("invert", Value::Scalar(1.0));
  CHECK(Near(At(ApplyEffect("hsl_secondary", view, frame), 50, 10).r, 1.0f, 1e-4f));
  // A hue range that straddles zero degrees selects red from either side of it.
  auto wrap = frame.Clone();
  SetPixel(wrap, 40, 5, 0.8f, 0.0f, 0.1f);  // hue ~ 352
  auto straddle = Parameters{{"hue_center", Value::Scalar(350.0)}, {"hue_width", Value::Scalar(40.0)}, {"view", Value::Scalar(1.0)}};
  const auto straddled = ApplyEffect("hsl_secondary", straddle, wrap);
  CHECK(Near(At(straddled, 40, 5).r, 1.0f, 1e-4f));
  CHECK(Near(At(straddled, 10, 10).r, 1.0f, 1e-4f));  // exactly 0 degrees: 10 from the centre
  CHECK(Near(At(straddled, 50, 10).r, 0.0f, 1e-4f));
  // Luma and saturation qualifiers narrow it: only mid-saturation, not this fully saturated red.
  const auto narrow = ApplyEffect("hsl_secondary", {{"sat_min", Value::Scalar(0.0)}, {"sat_max", Value::Scalar(0.5)}, {"view", Value::Scalar(1.0)}}, frame);
  CHECK(Near(At(narrow, 10, 10).r, 0.0f, 1e-4f));
}

CUTLINE_TEST(EveryNewEffectHasADescriptorAndItsDefaultsLeaveThePictureAloneWhereTheyShould) {
  // Effects whose defaults are a no-op: a new effect dropped on a clip must not change it.
  const char* neutral[] = {"gaussian_blur", "directional_blur", "unsharp_mask", "glow",        "drop_shadow",
                           "channel_mixer", "posterize",        "wave_warp",    "bulge",       "color_wheels",
                           "curves",        "hue_curves",       "color_adjust", "hsl_secondary", "mesh_warp"};
  auto frame = HorizontalRamp();
  SetPixel(frame, 10, 10, 0.9f, 0.2f, 0.4f);
  for (const auto* type : neutral) {
    const auto* descriptor = cutline::effects::FindEffect(type);
    CHECK(descriptor != nullptr);
    if (descriptor == nullptr) continue;
    Parameters defaults;
    for (const auto& parameter : descriptor->parameters) defaults.emplace_back(parameter.id, parameter.default_value);
    const auto output = ApplyEffect(type, defaults, frame);
    float worst = 0.0f;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) worst = std::max({worst, std::abs(At(output, x, y).r - At(frame, x, y).r), std::abs(At(output, x, y).g - At(frame, x, y).g), std::abs(At(output, x, y).b - At(frame, x, y).b)});
    }
    Expect(worst < 1e-5f, std::string(type) + " with its defaults changes the picture by " + std::to_string(worst), __LINE__);
  }
  // None of the effects is reported as skipped: all are implemented, not merely registered.
  for (const auto& type : cutline::render::FilterEffectTypes()) {
    Statistics statistics;
    (void)ApplyEffect(type, {}, SolidFrame(0.3f, 0.5f, 0.2f), 0.0f, &statistics);
    Expect(statistics.skipped_effects.empty(), type + " was skipped by the compositor", __LINE__);
    CHECK(cutline::render::IsBuiltInEffect(type));
  }
}

CUTLINE_TEST(FilterParametersCanBeKeyframedAcrossAClip) {
  // A blur that grows over the clip: the compositor evaluates it per frame like any effect.
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  Effect blur = MakeEffect("fx", "gaussian_blur", {{"radius", Value::Scalar(0.0)}});
  cutline::anim::AnimatedValue curve(Value::Scalar(0.0));
  curve.SetKeyframe({Seconds(0), Value::Scalar(0.0), Interpolation::Linear, {}, {}});
  curve.SetKeyframe({Seconds(10), Value::Scalar(6.0), Interpolation::Linear, {}, {}});
  blur.parameters[0].value = curve;
  clip.effects.push_back(std::move(blur));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  Frames frames;
  frames.Add("clip-1", Impulse());
  const Compositor compositor(FloatOutput());
  const auto early = compositor.Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)), frames.Resolver());
  const auto late = compositor.Compose(TimelineCompiler{}.Compile(sequence, Seconds(9)), frames.Resolver());
  CHECK(Centre(early).r > Centre(late).r);
  CHECK(Centre(late).r < 0.1f);
}

// --------------------------------------------------------------------- scopes ----

CUTLINE_TEST(ScopeAxesMapLevelsToRowsAndScales) {
  CHECK_EQ(cutline::render::LevelToRow(1.0, 256), 0);
  CHECK_EQ(cutline::render::LevelToRow(0.0, 256), 255);
  CHECK_EQ(cutline::render::LevelToRow(0.5, 101), 50);
  CHECK_EQ(cutline::render::LevelToRow(2.0, 256), 0);    // beyond the range clamps to the edge
  CHECK_EQ(cutline::render::LevelToRow(-1.0, 256), 255);
  CHECK(Near(static_cast<float>(cutline::render::RowToLevel(0, 256)), 1.0f, 1e-6f));
  CHECK(Near(static_cast<float>(cutline::render::RowToLevel(255, 256)), 0.0f, 1e-6f));
  using cutline::render::ScopeScale;
  CHECK(cutline::render::ScaleLevel(1.0, ScopeScale::Percent) == 100.0);
  CHECK(cutline::render::ScaleLevel(1.0, ScopeScale::Code8) == 255.0);
  CHECK(cutline::render::ScaleLevel(0.5, ScopeScale::Code10) == 511.5);
}

CUTLINE_TEST(ALumaWaveformPutsEachColumnAtItsOwnLevel) {
  // A grey ramp: column x is x/63.
  auto ramp = SolidFrame(0.0f, 0.0f, 0.0f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const auto v = static_cast<float>(x) / static_cast<float>(kWidth - 1);
      SetPixel(ramp, x, y, v, v, v);
    }
  }
  cutline::render::ScopeOptions options;
  options.width = kWidth;
  options.height = 101;
  const auto scope = cutline::render::MeasureWaveform(ramp, cutline::render::WaveformMode::Luma, options);
  CHECK_EQ(scope.channels, 1);
  CHECK_EQ(scope.width, kWidth);
  for (const int x : {0, 1, 10, 31, 32, 50, 63}) {
    const auto level = static_cast<double>(x) / (kWidth - 1);
    const auto row = cutline::render::LevelToRow(level, options.height);
    CHECK(Near(scope.at(0, row, x), 1.0f, 1e-5f));  // every pixel of the column is at that level
    // and nowhere else in the column
    float column_total = 0.0f;
    for (int r = 0; r < options.height; ++r) column_total += scope.at(0, r, x);
    CHECK(Near(column_total, 1.0f, 1e-4f));
  }
  CHECK_EQ(scope.below, 0LL);
  CHECK_EQ(scope.above, 0LL);
}

CUTLINE_TEST(AParadeSeparatesTheChannelsAndAnRgbWaveformOverlaysThem) {
  // Left third pure red, middle pure green, right third pure blue.
  auto bars = SolidFrame(0.0f, 0.0f, 0.0f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < 21; ++x) SetPixel(bars, x, y, 1.0f, 0.0f, 0.0f);
    for (int x = 21; x < 42; ++x) SetPixel(bars, x, y, 0.0f, 1.0f, 0.0f);
    for (int x = 42; x < kWidth; ++x) SetPixel(bars, x, y, 0.0f, 0.0f, 1.0f);
  }
  cutline::render::ScopeOptions options;
  options.width = 3 * 63;  // each panel is 63 columns for the 64-pixel picture
  options.height = 101;
  const auto parade = cutline::render::MeasureWaveform(bars, cutline::render::WaveformMode::Parade, options);
  CHECK_EQ(parade.channels, 3);
  CHECK_EQ(parade.width, 189);
  const int top = 0, bottom = options.height - 1;
  // Red panel (columns 0..62): full on the left, off elsewhere.
  CHECK(parade.at(0, top, 5) > 0.9f);
  CHECK(parade.at(0, bottom, 5) < 1e-6f);
  CHECK(parade.at(0, bottom, 40) > 0.9f);
  CHECK(parade.at(0, top, 40) < 1e-6f);
  // Green panel (63..125): full in the middle only.
  CHECK(parade.at(1, top, 63 + 31) > 0.9f);
  CHECK(parade.at(1, bottom, 63 + 5) > 0.9f);
  // Blue panel (126..188): full on the right only.
  CHECK(parade.at(2, top, 126 + 55) > 0.9f);
  CHECK(parade.at(2, bottom, 126 + 5) > 0.9f);

  // Overlaid, the three channels share columns: at the left edge red is high, green and blue low.
  options.width = kWidth;
  const auto overlay = cutline::render::MeasureWaveform(bars, cutline::render::WaveformMode::Rgb, options);
  CHECK_EQ(overlay.width, kWidth);
  CHECK(Near(overlay.at(0, top, 2), 1.0f, 1e-5f));
  CHECK(Near(overlay.at(1, bottom, 2), 1.0f, 1e-5f));
  CHECK(Near(overlay.at(2, bottom, 2), 1.0f, 1e-5f));
  CHECK(Near(overlay.at(1, top, 30), 1.0f, 1e-5f));
}

CUTLINE_TEST(WaveformsAndHistogramsCountOutOfRangeValuesInsteadOfHidingThem) {
  // A float picture can carry values past the legal range: half super-white, a quarter sub-black.
  auto frame = SolidFrame(0.5f, 0.5f, 0.5f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth / 2; ++x) SetPixel(frame, x, y, 1.2f, 1.2f, 1.2f);
  }
  for (int y = 0; y < kHeight / 2; ++y) {
    for (int x = kWidth / 2; x < kWidth; ++x) SetPixel(frame, x, y, -0.1f, -0.1f, -0.1f);
  }
  const auto scope = cutline::render::MeasureWaveform(frame, cutline::render::WaveformMode::Luma);
  const long long half = static_cast<long long>(kWidth / 2) * kHeight;
  CHECK_EQ(scope.above, half);
  CHECK_EQ(scope.below, half / 2);
  // They sit on the scope's edges: the top row for the super-white, the bottom for the sub-black.
  CHECK(scope.at(0, 0, 4) > 0.9f);
  CHECK(scope.at(0, scope.height - 1, scope.width - 1) > 0.0f);
  const auto histogram = cutline::render::MeasureHistogram(frame);
  CHECK_EQ(histogram.above, half * 3);  // counted over red, green and blue
  CHECK_EQ(histogram.below, (half / 2) * 3);
  CHECK(Near(histogram.channels[0][255], 0.5f, 1e-5f));  // the super-white lands in the top bin
  CHECK(Near(histogram.channels[0][0], 0.25f, 1e-5f));
}

CUTLINE_TEST(AVectorscopePlacesColoursAtTheirHueAndSaturation) {
  using cutline::render::ChromaOf;
  using cutline::render::ScopeMatrix;
  const auto red = ChromaOf(1.0, 0.0, 0.0, ScopeMatrix::Rec709);
  CHECK(Near(static_cast<float>(red[1]), 0.5f, 1e-5f));            // pure red is at full Cr
  CHECK(Near(static_cast<float>(red[0]), -0.114572f, 1e-5f));
  const auto blue = ChromaOf(0.0, 0.0, 1.0, ScopeMatrix::Rec709);
  CHECK(Near(static_cast<float>(blue[0]), 0.5f, 1e-5f));           // pure blue is at full Cb
  const auto grey = ChromaOf(0.4, 0.4, 0.4, ScopeMatrix::Rec709);
  CHECK(Near(static_cast<float>(grey[0]), 0.0f, 1e-6f));
  CHECK(Near(static_cast<float>(grey[1]), 0.0f, 1e-6f));

  // Solid red: every pixel in the one cell, at the top of the scope.
  const auto scope = cutline::render::MeasureVectorscope(SolidFrame(1.0f, 0.0f, 0.0f), 256);
  const auto cell = cutline::render::VectorscopeCell(red[0], red[1], 256);
  CHECK_EQ(cell.row, 0);
  CHECK(Near(scope.at(cell.row, cell.column), 1.0f, 1e-6f));
  CHECK_EQ(scope.pixels, static_cast<long long>(kWidth) * kHeight);
  // A grey frame lands in the middle however bright.
  const auto centre = cutline::render::VectorscopeCell(0.0, 0.0, 256);
  CHECK(Near(cutline::render::MeasureVectorscope(SolidFrame(0.8f, 0.8f, 0.8f), 256).at(centre.row, centre.column), 1.0f, 1e-6f));
  // The six colours of a bars signal are in six different cells, in hue order around the centre.
  const double hues[6][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};
  std::vector<std::pair<int, int>> cells;
  for (const auto& c : hues) {
    const auto chroma = ChromaOf(c[0], c[1], c[2], ScopeMatrix::Rec709);
    const auto where = cutline::render::VectorscopeCell(chroma[0], chroma[1], 256);
    cells.emplace_back(where.row, where.column);
  }
  for (std::size_t i = 0; i < cells.size(); ++i) {
    for (std::size_t j = i + 1; j < cells.size(); ++j) CHECK(cells[i] != cells[j]);
  }
}

CUTLINE_TEST(HistogramBinsHoldTheFractionOfThePictureAtEachLevel) {
  auto frame = SolidFrame(0.25f, 0.25f, 0.25f);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = kWidth / 2; x < kWidth; ++x) SetPixel(frame, x, y, 0.75f, 0.75f, 0.75f);
  }
  const auto histogram = cutline::render::MeasureHistogram(frame, 64);
  CHECK_EQ(histogram.bins, 64);
  CHECK(Near(histogram.channels[0][16], 0.5f, 1e-6f));  // 0.25 * 64
  CHECK(Near(histogram.channels[0][48], 0.5f, 1e-6f));  // 0.75 * 64
  CHECK(Near(histogram.channels[3][16], 0.5f, 1e-6f));  // luma of a grey is the grey
  float total = 0.0f;
  for (const auto bin : histogram.channels[1]) total += bin;
  CHECK(Near(total, 1.0f, 1e-5f));
  CHECK_EQ(histogram.below, 0LL);
}

CUTLINE_TEST(TheScopesUseTheChosenMatrixAndAcceptEightBitFrames) {
  using cutline::render::ScopeMatrix;
  CHECK(Near(cutline::render::LumaCoefficients(ScopeMatrix::Rec709)[1], 0.7152f));
  CHECK(Near(cutline::render::LumaCoefficients(ScopeMatrix::Rec601)[1], 0.587f));
  CHECK(Near(cutline::render::LumaCoefficients(ScopeMatrix::Rec2020)[1], 0.678f));
  // The same green reads at different luma levels under the two matrices.
  const auto green = SolidFrame(0.0f, 1.0f, 0.0f);
  cutline::render::ScopeOptions options;
  options.width = 4;
  options.height = 101;
  const auto row_of = [&](ScopeMatrix matrix) {
    options.matrix = matrix;
    const auto scope = cutline::render::MeasureWaveform(green, cutline::render::WaveformMode::Luma, options);
    for (int r = 0; r < scope.height; ++r) {
      if (scope.at(0, r, 0) > 0.5f) return r;
    }
    return -1;
  };
  CHECK_EQ(row_of(ScopeMatrix::Rec709), cutline::render::LevelToRow(0.7152, 101));
  CHECK_EQ(row_of(ScopeMatrix::Rec601), cutline::render::LevelToRow(0.587, 101));
  // An 8-bit frame measures the same as its float equivalent.
  auto eight = cutline::media::ConvertFrame(SolidFrame(0.5f, 0.5f, 0.5f), PixelFormat::Rgba8);
  const auto histogram = cutline::render::MeasureHistogram(eight, 256);
  CHECK(Near(histogram.channels[3][127] + histogram.channels[3][128], 1.0f, 1e-5f));
  CHECK_THROWS(cutline::render::MeasureWaveform(VideoFrame{}, cutline::render::WaveformMode::Luma));
}

CUTLINE_TEST(AsyncScopesDownsampleAndPublishWithoutBlockingPlayback) {
  cutline::render::AsyncScopeOptions options;
  options.max_input_width = 64;
  options.max_input_height = 36;
  options.measure_waveform = false;
  options.measure_vectorscope = false;
  options.measure_histogram = true;
  options.histogram_bins = 32;
  cutline::render::AsyncScopes scopes(options);

  const auto generation = scopes.Submit(SolidFrame(0.25f, 0.5f, 0.75f, 1.0f, 320, 180));
  CHECK(scopes.WaitFor(generation, std::chrono::seconds(2)));
  const auto result = scopes.Latest();
  CHECK(result.has_value());
  CHECK_EQ(result->generation, generation);
  CHECK(!result->waveform.has_value());
  CHECK(!result->vectorscope.has_value());
  CHECK(result->histogram.has_value());
  CHECK_EQ(result->histogram->pixels, static_cast<long long>(64 * 36));
  CHECK_EQ(scopes.statistics().completed, std::uint64_t{1});
}

// ----------------------------------------------------------- colour management ----

namespace {

namespace cm = cutline::render::color;

// One flat frame tagged with a colour space, composed in a sequence with the given spaces.
VideoFrame ComposeFlat(float r, float g, float b, const std::string& primaries, const std::string& transfer,
                       const std::string& working, const std::string& display, std::int64_t render_version,
                       Statistics& statistics, float opacity = 1.0f) {
  auto sequence = MakeSequence();
  sequence.working_color_space = working;
  sequence.display_color_space = display;
  sequence.render_version = render_version;
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  if (opacity < 1.0f) clip.effects.push_back(MakeEffect("fx", "opacity", {{"value", Value::Scalar(opacity)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  auto frame = SolidFrame(r, g, b);
  frame.color.primaries = primaries;
  frame.color.transfer = transfer;
  Frames frames;
  frames.Add("clip-1", std::move(frame));
  return Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)), frames.Resolver(), statistics);
}

}  // namespace

CUTLINE_TEST(TransferFunctionsMatchTheirPublishedReferenceValues) {
  // SMPTE ST 2084: 100 nits is 0.5081, 203 nits (reference white) 0.5807, 1000 nits 0.7518, 10000 nits 1.
  CHECK(Near(static_cast<float>(cm::NitsToPq(0.0)), 0.0f, 1e-6f));
  CHECK(Near(static_cast<float>(cm::NitsToPq(100.0)), 0.5081f, 1e-4f));
  CHECK(Near(static_cast<float>(cm::NitsToPq(203.0)), 0.5807f, 1e-4f));
  CHECK(Near(static_cast<float>(cm::NitsToPq(1000.0)), 0.7518f, 1e-4f));
  CHECK(Near(static_cast<float>(cm::NitsToPq(10000.0)), 1.0f, 1e-6f));
  for (const double nits : {0.01, 1.0, 50.0, 203.0, 4000.0}) CHECK(std::abs(cm::PqToNits(cm::NitsToPq(nits)) - nits) < 1e-6 * std::max(1.0, nits));
  // BT.2100 HLG: scene light 1/12 is signal 0.5, full scene light is signal 1.
  CHECK(Near(static_cast<float>(cm::HlgOetf(1.0 / 12.0)), 0.5f, 1e-6f));
  CHECK(Near(static_cast<float>(cm::HlgOetf(1.0)), 1.0f, 1e-5f));
  CHECK(Near(static_cast<float>(cm::HlgInverseOetf(0.5)), 1.0f / 12.0f, 1e-6f));
  for (const double e : {0.0, 0.01, 1.0 / 12.0, 0.3, 0.9}) CHECK(std::abs(cm::HlgInverseOetf(cm::HlgOetf(e)) - e) < 1e-9);
  // sRGB: code 0.5 is 0.2140 linear. BT.709: linear 0.018 is code 0.081 (the end of the linear segment).
  CHECK(Near(static_cast<float>(cm::ToLinear(cm::Transfer::Srgb, 0.5)), 0.2140f, 1e-4f));
  CHECK(Near(static_cast<float>(cm::FromLinear(cm::Transfer::Bt709, 0.018)), 0.081f, 1e-4f));
  CHECK(Near(static_cast<float>(cm::FromLinear(cm::Transfer::Bt709, 0.5)), 0.7054f, 1e-3f));
  CHECK(Near(static_cast<float>(cm::ToLinear(cm::Transfer::Gamma24, 0.5)), 0.1895f, 1e-3f));
  // Every SDR curve is its own inverse's inverse across the range.
  for (const auto transfer : {cm::Transfer::Bt709, cm::Transfer::Srgb, cm::Transfer::Gamma24, cm::Transfer::Linear}) {
    for (int i = 0; i <= 20; ++i) {
      const double v = i / 20.0;
      CHECK(std::abs(cm::FromLinear(transfer, cm::ToLinear(transfer, v)) - v) < 1e-9);
    }
  }
}

CUTLINE_TEST(PrimariesRotateBetweenGamutsAsTheStandardsSay) {
  const auto to2020 = cm::Rotation(cm::Primaries::Bt709, cm::Primaries::Bt2020);
  const double expected2020[3][3] = {{0.6274, 0.3293, 0.0433}, {0.0691, 0.9195, 0.0114}, {0.0164, 0.0880, 0.8956}};
  const auto toP3 = cm::Rotation(cm::Primaries::Bt709, cm::Primaries::DisplayP3);
  const double expectedP3[3][3] = {{0.8225, 0.1774, 0.0}, {0.0332, 0.9669, 0.0}, {0.0171, 0.0724, 0.9108}};
  for (int i = 0; i < 3; ++i) {
    double row2020 = 0.0, rowP3 = 0.0;
    for (int j = 0; j < 3; ++j) {
      CHECK(std::abs(to2020[i][j] - expected2020[i][j]) < 1e-3);
      CHECK(std::abs(toP3[i][j] - expectedP3[i][j]) < 1e-3);
      row2020 += to2020[i][j];
      rowP3 += toP3[i][j];
    }
    CHECK(std::abs(row2020 - 1.0) < 1e-9);  // white stays white
    CHECK(std::abs(rowP3 - 1.0) < 1e-9);
  }
  const auto luma709 = cm::LumaCoefficients(cm::Primaries::Bt709);
  CHECK(std::abs(luma709[0] - 0.2126) < 1e-4 && std::abs(luma709[1] - 0.7152) < 1e-4 && std::abs(luma709[2] - 0.0722) < 1e-4);
  const auto luma2020 = cm::LumaCoefficients(cm::Primaries::Bt2020);
  CHECK(std::abs(luma2020[0] - 0.2627) < 1e-4 && std::abs(luma2020[1] - 0.6780) < 1e-4 && std::abs(luma2020[2] - 0.0593) < 1e-4);
  const auto back = cm::Rotation(cm::Primaries::Bt2020, cm::Primaries::Bt709);
  for (int i = 0; i < 3; ++i) {
    double identity = 0.0;
    for (int j = 0; j < 3; ++j) identity += back[i][j] * to2020[j][i];
    CHECK(std::abs(identity - 1.0) < 1e-9);
  }
}

CUTLINE_TEST(ColourSpacesAreNamedAndFrameTagsAreUnderstood) {
  CHECK(cm::ParseSpace("rec709").has_value() && *cm::ParseSpace("rec709") == cm::Space{});
  CHECK(cm::ParseSpace("REC2020-PQ").has_value());
  CHECK(cm::ParseSpace("linear-p3").has_value() && cm::ParseSpace("linear-p3")->linear());
  CHECK(!cm::ParseSpace("aces-rrt-odt").has_value());
  for (const auto* name : {"rec709", "srgb", "p3-d65", "rec2020", "rec2020-pq", "rec2020-hlg", "linear-rec709", "linear-p3", "linear-rec2020"}) {
    CHECK_EQ(cm::Name(*cm::ParseSpace(name)), std::string(name));
  }
  // What a decoder reports.
  const auto pq = cm::SpaceFromTags("bt2020", "smpte2084");
  CHECK(pq.has_value() && pq->hdr() && pq->primaries == cm::Primaries::Bt2020);
  const auto hlg = cm::SpaceFromTags("bt2020", "arib-std-b67");
  CHECK(hlg.has_value() && hlg->transfer == cm::Transfer::Hlg);
  CHECK(cm::SpaceFromTags("smpte432", "iec61966-2-1")->primaries == cm::Primaries::DisplayP3);
  CHECK(*cm::SpaceFromTags("unspecified", "unspecified") == cm::Space{});  // untagged footage is Rec.709
  CHECK(!cm::SpaceFromTags("film", "bt709").has_value());
  CHECK(!cm::SpaceFromTags("bt709", "smpte428").has_value());
  const auto tags = cm::TagsOf(*pq);
  CHECK_EQ(tags.primaries, std::string("bt2020"));
  CHECK_EQ(tags.transfer, std::string("smpte2084"));
}

CUTLINE_TEST(ATransformConvertsGamutsAndHoldsGreysAndRoundTrips) {
  const cm::Space rec709{}, rec2020{cm::Primaries::Bt2020, cm::Transfer::Bt709}, p3{cm::Primaries::DisplayP3, cm::Transfer::Srgb};
  CHECK(cm::Transform(rec709, rec709).identity());
  // A grey is the same grey through any primaries (they share a white point).
  for (const auto& target : {rec2020, p3}) {
    const auto out = cm::Transform(rec709, target).Apply(0.4, 0.4, 0.4);
    CHECK(std::abs(out[0] - out[1]) < 1e-9 && std::abs(out[1] - out[2]) < 1e-9);
  }
  // An in-gamut colour comes back through a wider space unchanged.
  const auto forward = cm::Transform(rec709, rec2020);
  const auto reverse = cm::Transform(rec2020, rec709);
  for (const double r : {0.1, 0.5, 0.9}) {
    for (const double g : {0.2, 0.6}) {
      const auto wide = forward.Apply(r, g, 0.3);
      const auto narrow = reverse.Apply(wide[0], wide[1], wide[2]);
      CHECK(std::abs(narrow[0] - r) < 1e-6 && std::abs(narrow[1] - g) < 1e-6 && std::abs(narrow[2] - 0.3) < 1e-6);
    }
  }
  // Pure P3 red is outside Rec.709. Clipped, it is 709 red; desaturated, it keeps its luminance.
  cm::Options clip;
  clip.gamut = cm::GamutMapping::Clip;
  const auto clipped = cm::Transform(p3, rec709, clip).Apply(1.0, 0.0, 0.0);
  CHECK(std::abs(clipped[0] - 1.0) < 1e-9 && clipped[1] == 0.0 && clipped[2] == 0.0);
  const auto soft = cm::Transform(p3, rec709).Apply(1.0, 0.0, 0.0);
  for (const auto c : soft) CHECK(c >= 0.0 && c <= 1.0);
  const auto linear = [](double v) { return cm::ToLinear(cm::Transfer::Bt709, v); };
  const auto k709 = cm::LumaCoefficients(cm::Primaries::Bt709);  // as derived from the primaries, not the rounded 0.2126
  const auto luma_out = k709[0] * linear(soft[0]) + k709[1] * linear(soft[1]) + k709[2] * linear(soft[2]);
  const auto p3_luma = cm::LumaCoefficients(cm::Primaries::DisplayP3)[0];  // red's luminance share in P3
  Expect(std::abs(luma_out - p3_luma) < 1e-6, "luma " + std::to_string(luma_out) + " vs " + std::to_string(p3_luma), __LINE__);
}

CUTLINE_TEST(HdrLightIsFittedIntoSdrByASoftKneeAndNothingBelowTheKneeMoves) {
  // The curve: identity to three quarters of white, a smooth shoulder, and the source's peak lands on 1.
  const double peak = 1000.0 / 203.0;
  for (const double l : {0.0, 0.2, 0.5, 0.75}) CHECK(std::abs(cm::Transform::SoftKnee(l, peak) - l) < 1e-12);
  CHECK(std::abs(cm::Transform::SoftKnee(peak, peak) - 1.0) < 1e-12);
  double previous = 0.75;
  for (double l = 0.76; l <= peak; l += 0.01) {
    const auto mapped = cm::Transform::SoftKnee(l, peak);
    CHECK(mapped > previous && mapped <= 1.0);
    previous = mapped;
  }
  // No kink at the knee: the slope just above it is the slope just below (1).
  CHECK(std::abs((cm::Transform::SoftKnee(0.7501, peak) - cm::Transform::SoftKnee(0.75, peak)) / 0.0001 - 1.0) < 1e-3);

  // Through a transform: a PQ signal at 1000 nits is full SDR white; 50 nits is not changed in light.
  const cm::Space pq{cm::Primaries::Bt2020, cm::Transfer::Pq}, rec2020{cm::Primaries::Bt2020, cm::Transfer::Bt709};
  const auto peak_code = cm::NitsToPq(1000.0);
  const auto white = cm::Transform(pq, rec2020).Apply(peak_code, peak_code, peak_code);
  for (const auto c : white) CHECK(std::abs(c - 1.0) < 1e-6);
  const auto low_code = cm::NitsToPq(50.0);
  const auto low = cm::Transform(pq, rec2020).Apply(low_code, low_code, low_code);
  CHECK(std::abs(cm::ToLinear(cm::Transfer::Bt709, low[0]) - 50.0 / 203.0) < 1e-6);
  // And with tone mapping off, an SDR target simply clips what it cannot show.
  cm::Options none;
  none.tone_map = false;
  const auto clipped = cm::Transform(pq, rec2020, none).Apply(peak_code, peak_code, peak_code);
  CHECK(std::abs(clipped[0] - 1.0) < 1e-9);
  const auto mid_code = cm::NitsToPq(500.0);
  CHECK(cm::Transform(pq, rec2020).Apply(mid_code, mid_code, mid_code)[0] < cm::Transform(pq, rec2020, none).Apply(mid_code, mid_code, mid_code)[0] + 1e-12);
}

CUTLINE_TEST(HlgAndPqAgreeOnTheLightTheyCarryAndRoundTrip) {
  const cm::Space pq{cm::Primaries::Bt2020, cm::Transfer::Pq}, hlg{cm::Primaries::Bt2020, cm::Transfer::Hlg};
  // HLG signal 0.5 is scene light 1/12, which a 1000-nit display shows as 1000 * (1/12)^1.2 nits.
  const auto nits = 1000.0 * std::pow(1.0 / 12.0, 1.2);
  const auto as_pq = cm::Transform(hlg, pq).Apply(0.5, 0.5, 0.5);
  CHECK(std::abs(as_pq[0] - cm::NitsToPq(nits)) < 1e-6);
  CHECK(std::abs(as_pq[0] - as_pq[1]) < 1e-12);
  // Each way and back.
  for (const double v : {0.1, 0.3, 0.5, 0.75, 0.9}) {
    const auto there = cm::Transform(hlg, pq).Apply(v, v * 0.8, v * 0.5);
    const auto back = cm::Transform(pq, hlg).Apply(there[0], there[1], there[2]);
    CHECK(std::abs(back[0] - v) < 1e-6 && std::abs(back[1] - v * 0.8) < 1e-6 && std::abs(back[2] - v * 0.5) < 1e-6);
  }
}

CUTLINE_TEST(FramesInTheSequenceSpaceAreLeftAloneAndOtherSpacesAreConvertedUnderTheCurrentVersion) {
  // Rec.709 footage in a Rec.709 sequence: nothing is touched, and nothing is counted.
  Statistics same;
  const auto plain = ComposeFlat(0.3f, 0.5f, 0.7f, "bt709", "bt709", "rec709", "rec709", cutline::model::kCurrentRenderVersion, same);
  CHECK(Near(Centre(plain).r, 0.3f) && Near(Centre(plain).g, 0.5f) && Near(Centre(plain).b, 0.7f));
  CHECK_EQ(same.color_input_conversions, 0);
  CHECK(!same.color_output_conversion);

  // Display P3 footage in that sequence is converted into Rec.709 on the way in.
  Statistics converted;
  const auto p3 = ComposeFlat(1.0f, 0.0f, 0.0f, "smpte432", "iec61966-2-1", "rec709", "rec709", cutline::model::kCurrentRenderVersion, converted);
  CHECK_EQ(converted.color_input_conversions, 1);
  const auto expected = cm::Transform(*cm::ParseSpace("p3-d65"), cm::Space{}).Apply(1.0, 0.0, 0.0);
  CHECK(Near(Centre(p3).r, static_cast<float>(expected[0]), 1e-4f));
  CHECK(Near(Centre(p3).g, static_cast<float>(expected[1]), 1e-4f));
  CHECK(Near(Centre(p3).b, static_cast<float>(expected[2]), 1e-4f));

  // The same footage in a sequence made before render version 3 is exactly as it was: the setting
  // was recorded and ignored, and an old project must still look the way it did.
  Statistics legacy;
  const auto old = ComposeFlat(1.0f, 0.0f, 0.0f, "smpte432", "iec61966-2-1", "rec709", "rec709", 2, legacy);
  CHECK(Near(Centre(old).r, 1.0f) && Near(Centre(old).g, 0.0f) && Near(Centre(old).b, 0.0f));
  CHECK_EQ(legacy.color_input_conversions, 0);
}

CUTLINE_TEST(ThePictureIsConvertedToTheDisplaySpaceAndTaggedAsItsOutput) {
  Statistics statistics;
  // SDR white shown on a PQ display is reference white: 203 nits, code 0.5807, on every channel.
  const auto pq = ComposeFlat(1.0f, 1.0f, 1.0f, "bt709", "bt709", "rec709", "rec2020-pq", cutline::model::kCurrentRenderVersion, statistics);
  CHECK(statistics.color_output_conversion);
  CHECK(Near(Centre(pq).r, 0.5807f, 1e-3f) && Near(Centre(pq).g, 0.5807f, 1e-3f) && Near(Centre(pq).b, 0.5807f, 1e-3f));
  CHECK_EQ(pq.color.primaries, std::string("bt2020"));
  CHECK_EQ(pq.color.transfer, std::string("smpte2084"));

  // To sRGB the Rec.709 signal is re-encoded: the same light, a different curve.
  Statistics to_srgb;
  const auto srgb = ComposeFlat(0.5f, 0.5f, 0.5f, "bt709", "bt709", "rec709", "srgb", cutline::model::kCurrentRenderVersion, to_srgb);
  const auto linear = cm::ToLinear(cm::Transfer::Bt709, 0.5);
  CHECK(Near(Centre(srgb).r, static_cast<float>(cm::FromLinear(cm::Transfer::Srgb, linear)), 1e-4f));
  CHECK_EQ(srgb.color.transfer, std::string("iec61966-2-1"));
}

CUTLINE_TEST(ALinearWorkingSpaceBlendsInLinearLight) {
  // Half-opaque white over black. Blended as encoded numbers it is 0.5. In a linear working space
  // the 0.5 is light, and encoded for the display it is 0.7054.
  Statistics encoded;
  const auto in_display = ComposeFlat(1.0f, 1.0f, 1.0f, "bt709", "bt709", "rec709", "rec709", cutline::model::kCurrentRenderVersion, encoded, 0.5f);
  CHECK(Near(Centre(in_display).r, 0.5f, 1e-4f));
  Statistics linear;
  const auto in_linear = ComposeFlat(1.0f, 1.0f, 1.0f, "bt709", "bt709", "linear-rec709", "rec709", cutline::model::kCurrentRenderVersion, linear, 0.5f);
  CHECK(Near(Centre(in_linear).r, 0.7054f, 1e-3f));
  CHECK_EQ(linear.color_input_conversions, 1);
  CHECK(linear.color_output_conversion);
  // Fully opaque footage comes through the round trip unchanged.
  Statistics opaque;
  const auto round_trip = ComposeFlat(0.3f, 0.6f, 0.9f, "bt709", "bt709", "linear-rec709", "rec709", cutline::model::kCurrentRenderVersion, opaque);
  CHECK(Near(Centre(round_trip).r, 0.3f, 1e-4f) && Near(Centre(round_trip).g, 0.6f, 1e-4f) && Near(Centre(round_trip).b, 0.9f, 1e-4f));
}

// --------------------------------------------------------- scene-referred colour (ACES, camera log) ----

CUTLINE_TEST(CameraLogAndAcesTransfersMatchTheirPublishedGreyPointsAndRoundTripAcrossTheirRange) {
  // 18 percent grey, and black, as each maker publishes them.
  CHECK(std::abs(cm::FromLinear(cm::Transfer::AcesCct, 0.18) - 0.4135884) < 1e-6);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::AcesCc, 0.18) - 0.4135884) < 1e-6);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::LogC3, 0.18) - 0.391007) < 1e-5);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::SLog3, 0.18) - 420.0 / 1023.0) < 1e-9);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::SLog3, 0.0) - 95.0 / 1023.0) < 1e-9);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::VLog, 0.18) - 0.423311) < 1e-5);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::VLog, 0.0) - 0.125) < 1e-12);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::Log3G10, 0.18) - 1.0 / 3.0) < 1e-4);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::Log3G10, 0.0) - 0.091551) < 1e-5);
  // The ACEScct toe is a straight line below its cut, and joins the log part without a step.
  CHECK(std::abs(cm::FromLinear(cm::Transfer::AcesCct, 0.0078125) - 0.155251141552511) < 1e-9);
  CHECK(std::abs(cm::FromLinear(cm::Transfer::AcesCct, 0.0) - 0.0729055341958355) < 1e-12);
  // Encode and decode agree over the whole range a camera records: deep shadow to four stops over white and beyond.
  for (const auto transfer : {cm::Transfer::AcesCc, cm::Transfer::AcesCct, cm::Transfer::LogC3, cm::Transfer::SLog3, cm::Transfer::VLog, cm::Transfer::Log3G10}) {
    for (const double linear : {0.0001, 0.001, 0.005, 0.02, 0.18, 0.5, 1.0, 4.0, 16.0}) {
      const auto back = cm::ToLinear(transfer, cm::FromLinear(transfer, linear));
      CHECK(std::abs(back - linear) <= 1e-6 * std::max(1.0, linear));
    }
    // and they rise, so a brighter scene never gets a lower code value
    double previous = -1e9;
    for (double linear = 0.0; linear < 32.0; linear += 0.05) {
      const auto code = cm::FromLinear(transfer, linear);
      CHECK(code >= previous - 1e-5);
      previous = code;
    }
  }
}

CUTLINE_TEST(AcesAndCameraGamutsRotateByTheirPublishedMatricesAndKeepWhiteWhite) {
  // ACEScg to Rec.709 (Bradford-adapted from the ACES white): the matrix colour-science and the ACES documents give.
  const auto ap1_to_709 = cm::Rotation(cm::Primaries::AcesAp1, cm::Primaries::Bt709);
  const double expected[3][3] = {{1.70505, -0.62179, -0.08326}, {-0.13026, 1.14080, -0.01055}, {-0.02400, -0.12897, 1.15297}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) CHECK(std::abs(ap1_to_709[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] - expected[i][j]) < 2e-4);
  }
  // ACES2065-1 to ACEScg (both share a white, so nothing is adapted): the published AP0 to AP1 matrix.
  const auto ap0_to_ap1 = cm::Rotation(cm::Primaries::AcesAp0, cm::Primaries::AcesAp1);
  const double ap0[3][3] = {{1.4514393161, -0.2365107469, -0.2149285693}, {-0.0765537734, 1.1762296998, -0.0996759264}, {0.0083161484, -0.0060324498, 0.9977163014}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) CHECK(std::abs(ap0_to_ap1[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] - ap0[i][j]) < 1e-6);
  }
  // Every set maps equal red, green and blue to the same white, and to white in any other set.
  const cm::Primaries sets[] = {cm::Primaries::Bt709, cm::Primaries::DisplayP3, cm::Primaries::Bt2020, cm::Primaries::AcesAp0, cm::Primaries::AcesAp1,
                                cm::Primaries::ArriWideGamut3, cm::Primaries::SGamut3Cine, cm::Primaries::VGamut, cm::Primaries::RedWideGamut};
  for (const auto from : sets) {
    for (const auto to : sets) {
      const auto m = cm::Rotation(from, to);
      for (int row = 0; row < 3; ++row) CHECK(std::abs(m[static_cast<std::size_t>(row)][0] + m[static_cast<std::size_t>(row)][1] + m[static_cast<std::size_t>(row)][2] - 1.0) < 1e-9);
    }
    // and the luminance of equal channels is one, whatever the white a set was defined with
    const auto luma = cm::LumaCoefficients(from);
    CHECK(std::abs(luma[0] + luma[1] + luma[2] - 1.0) < 1e-9);
  }
  // Going there and back is the identity.
  const auto there = cm::Rotation(cm::Primaries::ArriWideGamut3, cm::Primaries::Bt709);
  const auto back = cm::Rotation(cm::Primaries::Bt709, cm::Primaries::ArriWideGamut3);
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) sum += there[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] * back[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)];
      CHECK(std::abs(sum - (i == j ? 1.0 : 0.0)) < 1e-9);
    }
  }
}

CUTLINE_TEST(SceneReferredSpacesAreNamedAndLogFootageReachesTheDisplayNeutralGreyAtGreyAndHighlightsUnclipped) {
  for (const auto* name : {"acescg", "aces2065-1", "acescc", "acescct", "arri-logc3", "sony-slog3", "panasonic-vlog", "red-log3g10", "linear-arri-wide-gamut3", "linear-sgamut3cine", "linear-vgamut", "linear-rwg"}) {
    const auto space = cm::ParseSpace(name);
    CHECK(space.has_value());
    CHECK(cm::Name(*space) == name);
    CHECK(space->scene_referred() || cm::Name(*space).rfind("linear-", 0) == 0);
  }
  CHECK(cm::ParseSpace("LogC").has_value() && cm::ParseSpace("S-Log3").has_value() && !cm::ParseSpace("acesdp").has_value());
  CHECK(cm::ParseSpace("acescg")->scene_referred() && !cm::ParseSpace("linear-rec709")->scene_referred());

  const auto rec709 = *cm::ParseSpace("rec709");
  for (const auto* name : {"arri-logc3", "sony-slog3", "panasonic-vlog", "red-log3g10", "acescct", "acescc"}) {
    const auto source = *cm::ParseSpace(name);
    const cm::Transform view(source, rec709);
    // 18 percent grey as the camera records it comes out as 18 percent grey on the display: the Rec.709 code for 0.18.
    const auto code = cm::FromLinear(source.transfer, 0.18);
    const auto grey = view.Apply(code, code, code);
    const auto display_grey = cm::FromLinear(cm::Transfer::Bt709, 0.18);
    CHECK(std::abs(grey[0] - display_grey) < 2e-4 && std::abs(grey[0] - grey[1]) < 2e-4 && std::abs(grey[1] - grey[2]) < 2e-4);
    // Highlights above white are fitted under 1 rather than clipped together: four and eight times white stay apart.
    const auto four = cm::FromLinear(source.transfer, 4.0), eight = cm::FromLinear(source.transfer, 8.0);
    const auto bright4 = view.Apply(four, four, four)[0], bright8 = view.Apply(eight, eight, eight)[0];
    CHECK(bright4 < bright8 && bright8 <= 1.0 && bright4 > grey[0]);
    // Black stays black.
    const auto black_code = cm::FromLinear(source.transfer, 0.0);
    CHECK(view.Apply(black_code, black_code, black_code)[0] < 0.02);
  }
  // The same linear light in ACEScg and in Rec.709 primaries is the same grey, and a saturated ACEScg colour that Rec.709 cannot show is brought in range.
  const auto acescg = *cm::ParseSpace("acescg");
  const auto grey = cm::Transform(acescg, rec709).Apply(0.18, 0.18, 0.18);
  CHECK(std::abs(grey[0] - cm::FromLinear(cm::Transfer::Bt709, 0.18)) < 2e-4);
  const auto pure = cm::Transform(acescg, rec709).Apply(0.0, 1.0, 0.0);
  for (const auto channel : pure) CHECK(channel >= 0.0 && channel <= 1.0);
  // The tags a container can carry do not name these spaces.
  CHECK(cm::TagsOf(*cm::ParseSpace("arri-logc3")).transfer == "unknown" && cm::TagsOf(acescg).primaries == "unknown");
}

CUTLINE_TEST(AClipCanSayWhatItsPictureIsAndLogFootageIsReadAsLogEvenWhenTheFileSaysNothing) {
  const auto compose = [](bool declared, const std::string& space, Statistics& statistics) {
    auto sequence = MakeSequence();
    sequence.working_color_space = "rec709";
    sequence.display_color_space = "rec709";
    sequence.render_version = cutline::model::kCurrentRenderVersion;
    auto track = MakeTrack("v1", 0);
    auto clip = MakeClip("clip-1", "media-1", 0, 10);
    if (declared) {
      auto effect = MakeEffect("fx-space", "input_colorspace", {});
      effect.preset_name = space;
      clip.effects.push_back(std::move(effect));
    }
    track.clips.push_back(std::move(clip));
    sequence.tracks.push_back(std::move(track));
    Finalise(sequence);
    // LogC3 code value of 18 percent grey, in a picture whose file carries no colour tags at all.
    const auto code = static_cast<float>(cm::FromLinear(cm::Transfer::LogC3, 0.18));
    Frames frames;
    frames.Add("clip-1", SolidFrame(code, code, code));
    return Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)), frames.Resolver(), statistics);
  };
  Statistics undeclared, declared, wrong;
  const auto as_is = compose(false, {}, undeclared);
  CHECK(Near(Centre(as_is).r, 0.391007f, 1e-5f) && undeclared.color_input_conversions == 0);   // taken to be Rec.709: the log picture stays flat
  const auto graded = compose(true, "arri-logc3", declared);
  CHECK_EQ(declared.color_input_conversions, 1);
  CHECK(Near(Centre(graded).r, static_cast<float>(cm::FromLinear(cm::Transfer::Bt709, 0.18)), 5e-4f));   // grey is grey again
  CHECK(std::abs(Centre(graded).r - Centre(graded).g) < 5e-4f && std::abs(Centre(graded).g - Centre(graded).b) < 5e-4f);
  // A space it does not know is reported and the picture is read by its tags.
  const auto ignored = compose(true, "no-such-space", wrong);
  CHECK(Near(Centre(ignored).r, 0.391007f, 1e-5f));
  CHECK(!wrong.effect_errors.empty() && wrong.effect_errors[0].find("input_colorspace") != std::string::npos);
  CHECK(cutline::render::IsBuiltInEffect("input_colorspace"));
}

// ------------------------------------------------------------ the transition library ----

namespace {

cutline::render::Layer FilledLayer(int width, int height, float r, float g, float b, bool gradient = false) {
  cutline::render::Layer layer;
  layer.Reset(width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float ramp = gradient ? static_cast<float>(x) / static_cast<float>(width) : 1.0f;
      layer.at(x, y) = {r * ramp, g * ramp, b * ramp, 1.0f};
    }
  }
  layer.MarkWhole();
  return layer;
}

double ShareOfIncoming(const std::string& kind, float progress) {
  const auto from = FilledLayer(64, 36, 1, 0, 0), to = FilledLayer(64, 36, 0, 0, 1);
  cutline::render::Layer mixed;
  cutline::render::MixTransition(kind, from, to, progress, mixed);
  int blue = 0;
  for (int y = 0; y < 36; ++y) {
    for (int x = 0; x < 64; ++x) blue += mixed.at(x, y).b > 0.5f ? 1 : 0;
  }
  return static_cast<double>(blue) / (64.0 * 36.0);
}

}  // namespace

CUTLINE_TEST(EveryTransitionStartsAsTheOutgoingPictureAndEndsAsTheIncomingOneAndNeverLeavesTheRange) {
  const auto from = FilledLayer(64, 36, 0.9f, 0.3f, 0.1f, true), to = FilledLayer(64, 36, 0.1f, 0.4f, 0.8f, true);
  const auto& library = cutline::render::TransitionLibrary();
  CHECK(library.size() >= std::size_t{20});
  std::set<std::string> ids;
  for (const auto& info : library) {
    CHECK(ids.insert(info.id).second && cutline::render::IsKnownTransition(info.id) && !info.name.empty() && !info.category.empty() && !info.description.empty());
    cutline::render::Layer mixed;
    for (const float progress : {0.0f, 1.0f}) {
      cutline::render::MixTransition(info.id, from, to, progress, mixed);
      const auto& expected = progress == 0.0f ? from : to;
      float worst = 0.0f;
      for (int y = 0; y < 36; ++y) {
        for (int x = 0; x < 64; ++x) {
          const auto& a = mixed.at(x, y);
          const auto& b = expected.at(x, y);
          worst = std::max({worst, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
        }
      }
      if (worst > 2e-4f) std::fprintf(stderr, "    %s at %.0f: off by %f\n", info.id.c_str(), progress, worst);
      CHECK(worst <= 2e-4f);
    }
    // In between nothing leaves 0..1, and a picture is always whole (alpha stays 1 over opaque pictures).
    for (const float progress : {0.1f, 0.33f, 0.5f, 0.77f, 0.95f}) {
      cutline::render::MixTransition(info.id, from, to, progress, mixed);
      for (int y = 0; y < 36; ++y) {
        for (int x = 0; x < 64; ++x) {
          const auto& p = mixed.at(x, y);
          CHECK(p.r >= -1e-6f && p.r <= 1.0001f && p.g >= -1e-6f && p.g <= 1.0001f && p.b >= -1e-6f && p.b <= 1.0001f && std::abs(p.a - 1.0f) < 1e-4f);
        }
      }
    }
  }
  // A name that is not in the library is a cross dissolve, and says it is not known.
  cutline::render::Layer unknown, straight;
  cutline::render::MixTransition("spin_zoom_dream", from, to, 0.4f, unknown);
  cutline::render::Layer::Mix(from, to, 0.4f, straight);
  CHECK(!cutline::render::IsKnownTransition("spin_zoom_dream") && cutline::render::IsStraightDissolve("spin_zoom_dream"));
  CHECK(cutline::render::IsStraightDissolve("cross_dissolve") && cutline::render::IsStraightDissolve("constant_power") && !cutline::render::IsStraightDissolve("wipe_left"));
  for (int x = 0; x < 64; ++x) CHECK(unknown.at(x, 5).r == straight.at(x, 5).r && unknown.at(x, 5).b == straight.at(x, 5).b);
}

CUTLINE_TEST(WipesIrisesAndTheClockRevealTheIncomingPictureInProportionToTheProgressFromTheSideTheyName) {
  // The share of the picture that has changed follows the progress, and grows steadily.
  for (const auto* kind : {"wipe_left", "wipe_right", "wipe_up", "wipe_down", "barn_door_h", "barn_door_v", "clock_wipe"}) {
    double previous = -1.0;
    for (const float progress : {0.2f, 0.4f, 0.5f, 0.6f, 0.8f}) {
      const auto share = ShareOfIncoming(kind, progress);
      // The edge is feathered, so the picture is wiped from a little before the start to a little past the end: 1.1 p - 0.05.
      CHECK(std::abs(share - (1.1 * progress - 0.05)) < 0.04);
      CHECK(share > previous);
      previous = share;
    }
  }
  // A box iris grows with the square of its progress; a circle is somewhere between nothing and everything and grows.
  CHECK(std::abs(ShareOfIncoming("iris_box", 0.5f) - 0.25) < 0.04);
  const auto circle_small = ShareOfIncoming("iris_circle", 0.3f), circle_mid = ShareOfIncoming("iris_circle", 0.6f), circle_big = ShareOfIncoming("iris_circle", 0.9f);
  CHECK(circle_small > 0.02 && circle_small < circle_mid && circle_mid < circle_big && circle_big < 1.0);

  // Which side they come from. At 40 percent a wipe_right has the left of the picture changed and the right not.
  const auto from = FilledLayer(64, 36, 1, 0, 0), to = FilledLayer(64, 36, 0, 0, 1);
  cutline::render::Layer mixed;
  const auto blue = [&](const char* kind, float progress, int x, int y) {
    cutline::render::MixTransition(kind, from, to, progress, mixed);
    return mixed.at(x, y).b > 0.5f;
  };
  CHECK(blue("wipe_right", 0.4f, 5, 18) && !blue("wipe_right", 0.4f, 58, 18));
  CHECK(!blue("wipe_left", 0.4f, 5, 18) && blue("wipe_left", 0.4f, 58, 18));
  CHECK(blue("wipe_down", 0.4f, 32, 3) && !blue("wipe_down", 0.4f, 32, 32));
  CHECK(!blue("wipe_up", 0.4f, 32, 3) && blue("wipe_up", 0.4f, 32, 32));
  CHECK(blue("barn_door_h", 0.4f, 32, 18) && !blue("barn_door_h", 0.4f, 3, 18) && !blue("barn_door_h", 0.4f, 61, 18));
  CHECK(blue("iris_circle", 0.4f, 32, 18) && !blue("iris_circle", 0.4f, 2, 2));
  // The clock hand starts at twelve and goes round clockwise: at 30 percent the upper right has gone, the upper left has not.
  CHECK(blue("clock_wipe", 0.3f, 48, 9) && !blue("clock_wipe", 0.3f, 16, 9) && !blue("clock_wipe", 0.3f, 16, 27));
  // The edge is soft: a column or two between the pictures is a mixture, not a step.
  cutline::render::MixTransition("wipe_right", from, to, 0.5f, mixed);
  int mixed_columns = 0;
  for (int x = 0; x < 64; ++x) mixed_columns += (mixed.at(x, 18).b > 0.05f && mixed.at(x, 18).b < 0.95f) ? 1 : 0;
  CHECK(mixed_columns >= 3 && mixed_columns <= 12);
}

CUTLINE_TEST(PushesSlidesAndDipsMoveAndDarkenTheWayTheirNamesSayAndStaySharp) {
  const auto from = FilledLayer(64, 36, 1, 0, 0, true), to = FilledLayer(64, 36, 0, 0, 1, true);   // brightness rises left to right
  cutline::render::Layer mixed;
  // Push left at half: the outgoing picture has moved 32 pixels left, and the incoming one fills where it was.
  cutline::render::MixTransition("push_left", from, to, 0.5f, mixed);
  CHECK(mixed.at(0, 7).r == from.at(32, 7).r && mixed.at(31, 7).r == from.at(63, 7).r);
  CHECK(mixed.at(32, 7).b == to.at(0, 7).b && mixed.at(63, 7).b == to.at(31, 7).b);   // whole pixels, no blur
  cutline::render::MixTransition("push_right", from, to, 0.25f, mixed);
  CHECK(mixed.at(16, 7).r == from.at(0, 7).r && mixed.at(0, 7).b == to.at(48, 7).b);
  cutline::render::MixTransition("push_up", from, to, 0.5f, mixed);
  CHECK(mixed.at(10, 0).r == from.at(10, 18).r && mixed.at(10, 35).b == to.at(10, 17).b);
  // Slides: the outgoing picture stays where it is while the incoming one covers it from the named side.
  cutline::render::MixTransition("slide_left", from, to, 0.5f, mixed);
  CHECK(mixed.at(10, 5).r == from.at(10, 5).r && mixed.at(40, 5).b == to.at(8, 5).b && mixed.at(40, 5).r == 0.0f);
  cutline::render::MixTransition("slide_right", from, to, 0.5f, mixed);
  CHECK(mixed.at(10, 5).b == to.at(42, 5).b && mixed.at(50, 5).r == from.at(50, 5).r);
  cutline::render::MixTransition("slide_down", from, to, 0.5f, mixed);
  CHECK(mixed.at(5, 4).b == to.at(5, 22).b && mixed.at(5, 30).r == from.at(5, 30).r);
  // Dips: black (or white) in the middle, and the outgoing and incoming pictures at their quarter points.
  const auto flat_from = FilledLayer(64, 36, 1, 0, 0), flat_to = FilledLayer(64, 36, 0, 0, 1);
  cutline::render::MixTransition("dip_to_black", flat_from, flat_to, 0.5f, mixed);
  CHECK(mixed.at(10, 10).r == 0.0f && mixed.at(10, 10).b == 0.0f && mixed.at(10, 10).a == 1.0f);
  cutline::render::MixTransition("dip_to_black", flat_from, flat_to, 0.25f, mixed);
  CHECK(Near(mixed.at(10, 10).r, 0.5f));
  cutline::render::MixTransition("dip_to_white", flat_from, flat_to, 0.5f, mixed);
  CHECK(Near(mixed.at(10, 10).r, 1.0f) && Near(mixed.at(10, 10).g, 1.0f) && Near(mixed.at(10, 10).b, 1.0f));
  cutline::render::MixTransition("dip_to_white", flat_from, flat_to, 0.75f, mixed);
  CHECK(Near(mixed.at(10, 10).r, 0.5f) && Near(mixed.at(10, 10).g, 0.5f) && Near(mixed.at(10, 10).b, 1.0f));   // white easing into blue
  // Additive: both pictures are full in the middle; film: mixed in light, so the middle is brighter than the straight mix.
  cutline::render::MixTransition("additive_dissolve", flat_from, flat_to, 0.5f, mixed);
  CHECK(Near(mixed.at(3, 3).r, 1.0f) && Near(mixed.at(3, 3).b, 1.0f));
  cutline::render::MixTransition("film_dissolve", flat_from, flat_to, 0.5f, mixed);
  CHECK(Near(mixed.at(3, 3).r, std::pow(0.5f, 1.0f / 2.2f), 1e-4f) && mixed.at(3, 3).r > 0.5f);
  // Zoom: the outgoing picture grows, so the picture a quarter of the way from the middle is further out in it.
  cutline::render::MixTransition("zoom_dissolve", from, from, 0.5f, mixed);
  CHECK(mixed.at(48, 18).r < from.at(48, 18).r);   // a point right of centre now shows what was nearer the centre
  CHECK(Near(mixed.at(32, 18).r, from.at(32, 18).r, 0.02f));
}

CUTLINE_TEST(TheCompositorDrawsTheNamedTransitionAtTheProgressOfTheFrameAndTellsOfOneItDoesNotHave) {
  const auto compose = [](const std::string& kind, double at_seconds, Statistics& statistics) {
    auto sequence = MakeSequence();
    auto track = MakeTrack("v1", 0);
    track.clips.push_back(MakeClip("clip-a", "media-1", 0, 6));
    track.clips.push_back(MakeClip("clip-b", "media-2", 6, 6));
    Transition transition;
    transition.id = "t-1";
    transition.kind = kind;
    transition.from_clip_id = "clip-a";
    transition.to_clip_id = "clip-b";
    transition.timeline_start = Seconds(5);
    transition.duration = Seconds(2);
    track.transitions.push_back(std::move(transition));
    sequence.tracks.push_back(std::move(track));
    Finalise(sequence);
    Frames frames;
    frames.Add("clip-a", SolidFrame(1.0f, 0.0f, 0.0f));
    frames.Add("clip-b", SolidFrame(0.0f, 0.0f, 1.0f));
    return Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, Seconds(static_cast<std::int64_t>(at_seconds))), frames.Resolver(), statistics);
  };
  Statistics wiping;
  const auto wiped = compose("wipe_right", 6, wiping);   // halfway
  CHECK_EQ(wiping.transitions_mixed, 1);
  CHECK(At(wiped, 2, kHeight / 2).b > 0.9f && At(wiped, kWidth - 3, kHeight / 2).r > 0.9f);   // left changed, right not yet
  Statistics dipping;
  const auto dipped = compose("dip_to_black", 6, dipping);
  CHECK(Near(Centre(dipped).r, 0.0f) && Near(Centre(dipped).b, 0.0f));
  Statistics unknown;
  const auto fallback = compose("page_curl_deluxe", 6, unknown);
  CHECK(Near(Centre(fallback).r, 0.5f) && Near(Centre(fallback).b, 0.5f));   // a dissolve
  CHECK(!unknown.effect_errors.empty() && unknown.effect_errors[0].find("page_curl_deluxe") != std::string::npos);
  Statistics known;
  (void)compose("cross_dissolve", 6, known);
  CHECK(known.effect_errors.empty());
}

CUTLINE_TEST(ColourSpacesThisBuildDoesNotDefineAreReportedAndTreatedAsRec709) {
  Statistics statistics;
  const auto output = ComposeFlat(0.3f, 0.5f, 0.7f, "bt709", "bt709", "rec709", "aces-rrt-odt", cutline::model::kCurrentRenderVersion, statistics);
  CHECK(Near(Centre(output).r, 0.3f));
  bool reported = false;
  for (const auto& error : statistics.effect_errors) reported = reported || error.find("aces-rrt-odt") != std::string::npos;
  CHECK(reported);
  Statistics odd_tags;
  const auto odd = ComposeFlat(0.3f, 0.5f, 0.7f, "film", "bt709", "rec709", "rec709", cutline::model::kCurrentRenderVersion, odd_tags);
  CHECK(Near(Centre(odd).g, 0.5f));
  CHECK(!odd_tags.effect_errors.empty());
}

// ------------------------------------------------------------------- tracking ----

namespace {

namespace trk = cutline::render::tracking;

double Hash01(int x, int y, int seed) {
  std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + static_cast<std::uint32_t>(seed) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<double>(h & 0xFFFFFF) / 16777216.0;
}

// A picture that exists everywhere, so a frame of it under any camera is exact rather than a resampling of a resampling.
double Scene(double x, double y) {
  double value = 0.0, amplitude = 0.5, total = 0.0;
  for (int octave = 0; octave < 4; ++octave) {
    const double cell = 6.0 * (1 << octave);
    const double fx = x / cell, fy = y / cell;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
    double tx = fx - ix, ty = fy - iy;
    tx = tx * tx * (3.0 - 2.0 * tx);
    ty = ty * ty * (3.0 - 2.0 * ty);
    const double a = Hash01(ix, iy, octave), b = Hash01(ix + 1, iy, octave), c = Hash01(ix, iy + 1, octave), d = Hash01(ix + 1, iy + 1, octave);
    value += amplitude * ((a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty);
    total += amplitude;
    amplitude *= 0.6;
  }
  return value / total;
}

// The camera: the scene point z lands at c + t + s R (z - c).
trk::Warp Camera(double scale, double theta, double tx, double ty, double cx, double cy) {
  const auto rotation = trk::Warp::Similarity(scale, theta, 0.0, 0.0);
  const auto centre = rotation.Apply(cx, cy);
  return trk::Warp::Similarity(scale, theta, cx + tx - centre.first, cy + ty - centre.second);
}

VideoFrame SceneFrame(int width, int height, const trk::Warp& camera, float gain = 1.0f, float offset = 0.0f) {
  const auto inverse = camera.Inverse();
  auto frame = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto source = inverse.Apply(x, y);
      const auto v = static_cast<float>(Scene(source.first, source.second)) * gain + offset;
      SetPixel(frame, x, y, v, v, v);
    }
  }
  return frame;
}

struct ShakeSample final {
  double scale, theta, tx, ty;
};
ShakeSample Shake(int k) {
  return {1.0 + 0.012 * std::sin(0.55 * k), 0.018 * std::sin(0.4 * k), 5.0 * std::sin(0.6 * k), 3.5 * std::cos(0.5 * k) - 3.5};
}

class FrameSet final {
 public:
  void Add(VideoFrame frame) { frames_.push_back(std::move(frame)); }
  [[nodiscard]] trk::FrameResolver Resolver() const {
    return [this](std::size_t index) -> const VideoFrame* { return index < frames_.size() ? &frames_[index] : nullptr; };
  }
  [[nodiscard]] std::size_t size() const { return frames_.size(); }
  [[nodiscard]] const VideoFrame& at(std::size_t index) const { return frames_[index]; }

 private:
  std::vector<VideoFrame> frames_;
};

}  // namespace

CUTLINE_TEST(APointTrackFollowsAFeatureToAFractionOfAPixel) {
  FrameSet frames;
  std::vector<std::pair<double, double>> truth;
  for (int k = 0; k < 24; ++k) {
    const auto tx = 2.7 * k + 1.3 * std::sin(0.5 * k), ty = -1.1 * k + 2.0 * std::cos(0.4 * k) - 2.0;
    frames.Add(SceneFrame(256, 160, trk::Warp::Translation(tx, ty)));
    truth.emplace_back(128.0 + tx, 80.0 + ty);
  }
  const auto result = trk::TrackPoint(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 0, 128.0, 80.0);
  CHECK_EQ(result.samples.size(), std::size_t{24});
  CHECK(!result.cancelled);
  CHECK_EQ(result.lost_at, std::int64_t{-1});
  double worst = 0.0;
  for (std::size_t k = 0; k < result.samples.size(); ++k) {
    CHECK(result.samples[k].valid);
    CHECK(result.samples[k].confidence > 0.95);
    worst = std::max(worst, std::hypot(result.samples[k].x - truth[k].first, result.samples[k].y - truth[k].second));
  }
  Expect(worst < 0.1, "worst error " + std::to_string(worst) + " px", __LINE__);
  CHECK(result.samples[5].time.Compare(RationalTime(5, 25)) == 0);
}

CUTLINE_TEST(AChangeOfExposureDoesNotPullATrackButAnObstructionIsReportedAndRecoveredFrom) {
  FrameSet frames;
  for (int k = 0; k < 20; ++k) {
    const auto tx = 2.0 * k;
    // Exposure steps at frame 8; the point is hidden by a blank frame at 12 to 14.
    auto frame = (k >= 12 && k <= 14) ? SolidFrame(0.5f, 0.5f, 0.5f, 1.0f, 256, 160)
                                      : SceneFrame(256, 160, trk::Warp::Translation(tx, 0.0), k >= 8 ? 0.6f : 1.0f, k >= 8 ? 0.15f : 0.0f);
    frames.Add(std::move(frame));
  }
  const auto result = trk::TrackPoint(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 0, 100.0, 80.0);
  for (std::size_t k = 1; k < 12; ++k) CHECK(result.samples[k].valid);   // the gain and offset are no obstacle
  for (std::size_t k = 12; k <= 14; ++k) CHECK(!result.samples[k].valid);  // nothing to see
  for (std::size_t k = 15; k < 20; ++k) {                                 // found again where it should be
    CHECK(result.samples[k].valid);
    CHECK(std::abs(result.samples[k].x - (100.0 + 2.0 * k)) < 0.3);
  }
  CHECK_EQ(result.lost_at, std::int64_t{-1});

  // A track that is never found again says where it was lost.
  FrameSet gone;
  for (int k = 0; k < 14; ++k) gone.Add(k < 9 ? SceneFrame(256, 160, trk::Warp::Translation(2.0 * k, 0.0)) : SolidFrame(0.4f, 0.4f, 0.4f, 1.0f, 256, 160));
  const auto lost = trk::TrackPoint(gone.size(), cutline::time::kFrameRate25, gone.Resolver(), 0, 100.0, 80.0);
  CHECK_EQ(lost.lost_at, std::int64_t{9});
}

CUTLINE_TEST(TrackingCanBeCancelledAndReportsItsProgressAndRefusesWhatIsNotTrackable) {
  FrameSet frames;
  for (int k = 0; k < 12; ++k) frames.Add(SceneFrame(192, 128, trk::Warp::Translation(1.5 * k, 0.0)));
  std::size_t reported = 0;
  int polls = 0;
  const auto cancelled = trk::TrackPoint(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 0, 96.0, 64.0, {},
                                         [&]() { return ++polls > 5; }, [&](std::size_t done, std::size_t) { reported = done; });
  CHECK(cancelled.cancelled);
  CHECK_EQ(cancelled.samples.size(), std::size_t{6});  // the first frame and the five followed before cancelling
  CHECK_EQ(reported, std::size_t{6});                  // progress counts frames done, the first included

  CHECK_THROWS(trk::TrackPoint(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 12, 96.0, 64.0));
  CHECK_THROWS(trk::TrackPoint(frames.size(), cutline::time::kFrameRate25, {}, 0, 96.0, 64.0));
  CHECK_THROWS(trk::TrackPlane(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 0, trk::Region{10, 10, 4, 4}));
  const auto beyond = [](std::size_t) -> const VideoFrame* { return nullptr; };
  CHECK_THROWS(trk::TrackPoint(3, cutline::time::kFrameRate25, beyond, 0, 10.0, 10.0));
  CHECK(trk::IsStale("fp-old", "fp-new"));
  CHECK(!trk::IsStale("fp", "fp"));
}

CUTLINE_TEST(APlanarTrackRecoversTranslationRotationAndScaleOfAPlane) {
  FrameSet frames;
  std::vector<trk::Warp> truth;
  for (int k = 0; k < 20; ++k) {
    const auto camera = Camera(1.0 + 0.006 * k, 0.012 * k, 1.8 * k, -0.9 * k, 160.0, 100.0);
    frames.Add(SceneFrame(320, 200, camera));
    truth.push_back(camera);
  }
  const trk::Region region{90, 50, 140, 100};
  const auto result = trk::TrackPlane(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 0, region);
  CHECK_EQ(result.samples.size(), std::size_t{20});
  CHECK_EQ(result.lost_at, std::int64_t{-1});
  double corner_error = 0.0, rotation_error = 0.0, scale_error = 0.0;
  for (std::size_t k = 0; k < result.samples.size(); ++k) {
    CHECK(result.samples[k].valid);
    CHECK(result.samples[k].confidence > 0.95);
    for (const auto& corner : {std::pair{90.0, 50.0}, std::pair{230.0, 50.0}, std::pair{90.0, 150.0}, std::pair{230.0, 150.0}}) {
      const auto a = result.samples[k].warp.Apply(corner.first, corner.second);
      const auto b = truth[k].Apply(corner.first, corner.second);
      corner_error = std::max(corner_error, std::hypot(a.first - b.first, a.second - b.second));
    }
    rotation_error = std::max(rotation_error, std::abs(result.samples[k].warp.Rotation() - truth[k].Rotation()));
    scale_error = std::max(scale_error, std::abs(result.samples[k].warp.Scale() - truth[k].Scale()));
  }
  Expect(corner_error < 0.15, "corner error " + std::to_string(corner_error) + " px", __LINE__);
  Expect(rotation_error < 0.002, "rotation error " + std::to_string(rotation_error) + " rad", __LINE__);
  Expect(scale_error < 0.002, "scale error " + std::to_string(scale_error), __LINE__);

  // An affine model also recovers a shear, which a similarity cannot.
  FrameSet sheared;
  std::vector<trk::Warp> shear_truth;
  for (int k = 0; k < 12; ++k) {
    trk::Warp warp;
    warp.a12 = 0.01 * k;
    warp.tx = -warp.a12 * 100.0 + 1.0 * k;  // shear about the row through the middle of the region
    warp.ty = 0.5 * k;
    sheared.Add(SceneFrame(320, 200, warp));
    shear_truth.push_back(warp);
  }
  trk::PlanarTrackerConfig affine;
  affine.model = trk::Model::Affine;
  const auto sheared_result = trk::TrackPlane(sheared.size(), cutline::time::kFrameRate25, sheared.Resolver(), 0, region, affine);
  double shear_error = 0.0;
  for (std::size_t k = 0; k < sheared_result.samples.size(); ++k) {
    CHECK(sheared_result.samples[k].valid);
    const auto a = sheared_result.samples[k].warp.Apply(230.0, 150.0);
    const auto b = shear_truth[k].Apply(230.0, 150.0);
    shear_error = std::max(shear_error, std::hypot(a.first - b.first, a.second - b.second));
  }
  Expect(shear_error < 0.25, "shear error " + std::to_string(shear_error) + " px", __LINE__);
}

CUTLINE_TEST(ATrackBecomesAnOrdinaryMotionEffectThatMovesALayerWithIt) {
  FrameSet frames;
  for (int k = 0; k < 10; ++k) frames.Add(SceneFrame(256, 160, trk::Warp::Translation(3.0 * k, 1.0 * k)));
  const auto track = trk::TrackPoint(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 0, 128.0, 80.0);
  const auto effect = trk::MakeTrackedMotionEffect(track, "fx-track");
  CHECK_EQ(effect.effect_type, std::string("motion"));
  CHECK_EQ(effect.parameters.size(), std::size_t{1});
  const auto& keys = effect.parameters[0].value.keyframes();
  CHECK_EQ(keys.size(), std::size_t{10});
  CHECK(std::abs(keys[0].value.components[0]) < 1e-9);           // starts where it is
  CHECK(std::abs(keys[9].value.components[0] - 27.0) < 0.2);     // 9 frames at 3 px
  CHECK(std::abs(keys[9].value.components[1] - 9.0) < 0.2);
  CHECK(keys[9].time.Compare(RationalTime(9, 25)) == 0);

  // The effect is the registered one's shape: it validates and the compositor moves a layer by it.
  CHECK(cutline::effects::ValidateParameter("motion", "position", keys[9].value).empty());
  auto sequence = MakeSequence();
  sequence.width = 256;
  sequence.height = 160;
  auto layer_track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(effect);
  layer_track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(layer_track));
  Finalise(sequence);
  auto marker_frame = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, 256, 160);
  for (int y = 70; y < 90; ++y) {
    for (int x = 118; x < 138; ++x) SetPixel(marker_frame, x, y, 1.0f, 1.0f, 1.0f);
  }
  Frames layer_frames;
  layer_frames.Add("clip-1", std::move(marker_frame));
  const auto composed = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(9, 25)), layer_frames.Resolver());
  // 27 px right and 9 down: the square's centre (128, 80) is now at about (155, 89).
  CHECK(At(composed, 155, 89).r > 0.9f);
  CHECK(At(composed, 128, 80).r < 0.1f);
}

CUTLINE_TEST(SimilarityStabilisationMeasuresTheCamerasRotationZoomAndShift) {
  const int w = 320, h = 180, count = 22;
  FrameSet frames;
  std::vector<ShakeSample> truth;
  for (int k = 0; k < count; ++k) {
    const auto s = Shake(k);
    truth.push_back(s);
    frames.Add(SceneFrame(w, h, Camera(s.scale, s.theta, s.tx, s.ty, w / 2.0, h / 2.0)));
  }
  const auto result = trk::AnalyzeSimilarityStabilization(frames.size(), cutline::time::kFrameRate25, frames.Resolver());
  CHECK_EQ(result.samples.size(), std::size_t{22});
  CHECK_EQ(result.camera_path.size(), std::size_t{22});
  CHECK(result.uncertain_frames.empty());
  double translation_error = 0.0, rotation_error = 0.0, scale_error = 0.0;
  int fewest = 1000;
  for (int k = 0; k < count; ++k) {
    const auto& measured = result.camera_path[static_cast<std::size_t>(k)];
    translation_error = std::max(translation_error, std::hypot(measured.translation_x - truth[static_cast<std::size_t>(k)].tx, measured.translation_y - truth[static_cast<std::size_t>(k)].ty));
    rotation_error = std::max(rotation_error, std::abs(measured.rotation_degrees - truth[static_cast<std::size_t>(k)].theta * 180.0 / 3.14159265358979323846));
    scale_error = std::max(scale_error, std::abs(measured.scale - truth[static_cast<std::size_t>(k)].scale));
    if (k > 0) fewest = std::min(fewest, result.samples[static_cast<std::size_t>(k)].inliers);
  }
  Expect(translation_error < 0.4, "camera shift error " + std::to_string(translation_error) + " px", __LINE__);
  Expect(rotation_error < 0.08, "camera rotation error " + std::to_string(rotation_error) + " deg", __LINE__);
  Expect(scale_error < 0.004, "camera zoom error " + std::to_string(scale_error), __LINE__);
  Expect(fewest >= 20, "only " + std::to_string(fewest) + " features agreed on some frame", __LINE__);

  // The first frame is left where it is, and the same frames give the same numbers.
  CHECK(std::abs(result.samples[0].translation_x) < 1e-9 && std::abs(result.samples[0].rotation_degrees) < 1e-9 &&
        std::abs(result.samples[0].scale - 1.0) < 1e-9);
  const auto again = trk::AnalyzeSimilarityStabilization(frames.size(), cutline::time::kFrameRate25, frames.Resolver());
  bool identical = again.auto_scale == result.auto_scale;
  for (std::size_t k = 0; k < result.samples.size(); ++k) {
    identical = identical && again.samples[k].translation_x == result.samples[k].translation_x &&
                again.samples[k].rotation_degrees == result.samples[k].rotation_degrees && again.samples[k].scale == result.samples[k].scale;
  }
  CHECK(identical);
}

CUTLINE_TEST(AStabilisedPictureHoldsStillAndTheCropHidesEveryBorder) {
  const int w = 320, h = 180, count = 22;
  FrameSet frames;
  for (int k = 0; k < count; ++k) {
    const auto s = Shake(k);
    frames.Add(SceneFrame(w, h, Camera(s.scale, s.theta, s.tx, s.ty, w / 2.0, h / 2.0)));
  }
  const auto result = trk::AnalyzeSimilarityStabilization(frames.size(), cutline::time::kFrameRate25, frames.Resolver());
  CHECK(result.auto_scale > 1.0 && result.auto_scale <= 1.35);

  // Render each frame through the compositor with the stabiliser effect, and compare the middle of
  // the picture with the first frame: the corrections have to be the right way round to help.
  auto sequence = MakeSequence();
  sequence.width = w;
  sequence.height = h;
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(trk::MakeSimilarityStabilizerEffect(result, "fx-stab"));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  const auto difference = [&](const VideoFrame& a, const VideoFrame& b) {
    double sum = 0.0;
    int n = 0;
    for (int y = h / 4; y < 3 * h / 4; y += 2) {
      for (int x = w / 4; x < 3 * w / 4; x += 2) {
        sum += std::abs(At(a, x, y).r - At(b, x, y).r);
        ++n;
      }
    }
    return sum / n;
  };
  // What stabilising is for: the picture should hold still from one frame to the next. Compare how
  // far each frame is from the one before, before and after. (Resampling softens both alike.)
  std::vector<VideoFrame> stabilised;
  for (int k = 0; k < count; ++k) {
    Frames current;
    current.Add("clip-1", frames.at(static_cast<std::size_t>(k)).Clone());
    stabilised.push_back(Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(k, 25)), current.Resolver()));
  }
  double before = 0.0, after = 0.0;
  for (int k = 1; k < count; ++k) {
    after += difference(stabilised[static_cast<std::size_t>(k)], stabilised[static_cast<std::size_t>(k - 1)]);
    before += difference(frames.at(static_cast<std::size_t>(k)), frames.at(static_cast<std::size_t>(k - 1)));
  }
  Expect(after < 0.5 * before, "frame to frame the picture moves " + std::to_string(after / (count - 1)) + " after correction against " + std::to_string(before / (count - 1)) + " before", __LINE__);

  // No border: the corners of the output, taken back through each frame's correction (and the crop),
  // fall inside the source.
  const double half_w = w / 2.0, half_h = h / 2.0;
  for (const auto& sample : result.samples) {
    const std::complex<double> a = std::polar(sample.scale * result.auto_scale, sample.rotation_degrees * 3.14159265358979323846 / 180.0);
    const std::complex<double> t{sample.translation_x, sample.translation_y};
    for (const auto& corner : {std::complex<double>{-half_w, -half_h}, {half_w, -half_h}, {-half_w, half_h}, {half_w, half_h}}) {
      const auto source = (corner - t) / a;
      CHECK(std::abs(source.real()) <= half_w + 1e-6 && std::abs(source.imag()) <= half_h + 1e-6);
    }
  }

  // The policies: no crop, or a fixed one.
  trk::SimilarityStabilizerConfig none;
  none.crop = trk::CropPolicy::None;
  CHECK(trk::AnalyzeSimilarityStabilization(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), none).auto_scale == 1.0);
  trk::SimilarityStabilizerConfig fixed;
  fixed.crop = trk::CropPolicy::Fixed;
  fixed.fixed_scale = 1.08;
  CHECK(trk::AnalyzeSimilarityStabilization(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), fixed).auto_scale == 1.08);
}

CUTLINE_TEST(StabilisationOfAFrameWithNothingToFollowSaysSoAndLeavesItAlone) {
  FrameSet frames;
  for (int k = 0; k < 6; ++k) frames.Add(SolidFrame(0.4f, 0.4f, 0.4f, 1.0f, 320, 180));
  const auto result = trk::AnalyzeSimilarityStabilization(frames.size(), cutline::time::kFrameRate25, frames.Resolver());
  CHECK_EQ(result.uncertain_frames.size(), std::size_t{5});
  for (const auto& sample : result.samples) {
    CHECK(std::abs(sample.translation_x) < 1e-9 && std::abs(sample.translation_y) < 1e-9);
    CHECK(std::abs(sample.rotation_degrees) < 1e-9 && std::abs(sample.scale - 1.0) < 1e-9);
  }
  CHECK(Near(static_cast<float>(result.auto_scale), 1.0f, 1e-6f) || result.auto_scale >= 1.0);
  CHECK_THROWS(trk::AnalyzeSimilarityStabilization(0, cutline::time::kFrameRate25, frames.Resolver()));

  // Cancelling returns what was done.
  FrameSet shaky;
  for (int k = 0; k < 8; ++k) {
    const auto s = Shake(k);
    shaky.Add(SceneFrame(320, 180, Camera(s.scale, s.theta, s.tx, s.ty, 160.0, 90.0)));
  }
  int polls = 0;
  const auto stopped = trk::AnalyzeSimilarityStabilization(shaky.size(), cutline::time::kFrameRate25, shaky.Resolver(), {}, [&]() { return ++polls > 3; });
  CHECK(stopped.cancelled);
  CHECK_EQ(stopped.samples.size(), std::size_t{4});  // the first frame and the three analysed before cancelling
}

CUTLINE_TEST(EachKindOfShakeIsCorrectedTheRightWayRoundThroughTheCompositor) {
  // Rotation alone, zoom alone, and shift alone, each large enough that a correction applied the wrong
  // way round (or not at all) makes the picture move more, not less.
  struct Case final {
    const char* name;
    ShakeSample (*shake)(int);
  };
  const Case cases[] = {
      {"rotation", [](int k) { return ShakeSample{1.0, 0.05 * std::sin(0.5 * k), 0.0, 0.0}; }},
      {"zoom", [](int k) { return ShakeSample{1.0 + 0.04 * std::sin(0.5 * k), 0.0, 0.0, 0.0}; }},
      {"shift", [](int k) { return ShakeSample{1.0, 0.0, 6.0 * std::sin(0.5 * k), -4.0 * std::sin(0.45 * k)}; }},
  };
  const int w = 320, h = 180, count = 20;
  for (const auto& test : cases) {
    FrameSet frames;
    for (int k = 0; k < count; ++k) {
      const auto s = test.shake(k);
      frames.Add(SceneFrame(w, h, Camera(s.scale, s.theta, s.tx, s.ty, w / 2.0, h / 2.0)));
    }
    const auto result = trk::AnalyzeSimilarityStabilization(frames.size(), cutline::time::kFrameRate25, frames.Resolver());
    auto sequence = MakeSequence();
    sequence.width = w;
    sequence.height = h;
    auto track = MakeTrack("v1", 0);
    auto clip = MakeClip("clip-1", "media-1", 0, 10);
    clip.effects.push_back(trk::MakeSimilarityStabilizerEffect(result, "fx-stab"));
    track.clips.push_back(std::move(clip));
    sequence.tracks.push_back(std::move(track));
    Finalise(sequence);
    const auto difference = [&](const VideoFrame& a, const VideoFrame& b) {
      double sum = 0.0;
      int n = 0;
      for (int y = h / 4; y < 3 * h / 4; y += 2) {
        for (int x = w / 4; x < 3 * w / 4; x += 2) {
          sum += std::abs(At(a, x, y).r - At(b, x, y).r);
          ++n;
        }
      }
      return sum / n;
    };
    std::vector<VideoFrame> stabilised;
    for (int k = 0; k < count; ++k) {
      Frames current;
      current.Add("clip-1", frames.at(static_cast<std::size_t>(k)).Clone());
      stabilised.push_back(Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(k, 25)), current.Resolver()));
    }
    double before = 0.0, after = 0.0;
    for (int k = 1; k < count; ++k) {
      after += difference(stabilised[static_cast<std::size_t>(k)], stabilised[static_cast<std::size_t>(k - 1)]);
      before += difference(frames.at(static_cast<std::size_t>(k)), frames.at(static_cast<std::size_t>(k - 1)));
    }
    Expect(after < 0.45 * before, std::string(test.name) + " shake: the picture moves " + std::to_string(after / (count - 1)) + " frame to frame after correction against " + std::to_string(before / (count - 1)) + " before", __LINE__);
  }
}

CUTLINE_TEST(TrackingResultsWriteAsReadableJsonAndReadBackExactly) {
  FrameSet frames;
  for (int k = 0; k < 8; ++k) frames.Add(SceneFrame(256, 160, trk::Warp::Translation(2.5 * k + 0.1 * k * k, -1.0 * k)));
  const auto point = trk::TrackPoint(frames.size(), cutline::time::kFrameRate2997, frames.Resolver(), 0, 128.0, 80.0);
  const auto text = trk::ToJson(point);
  CHECK(text.find("\"kind\":\"point\"") != std::string::npos);
  const auto back = trk::TrackResultFromJson(text);
  CHECK_EQ(back.samples.size(), point.samples.size());
  bool identical = back.algorithm == point.algorithm && back.lost_at == point.lost_at && back.source_width == point.source_width;
  for (std::size_t k = 0; k < point.samples.size(); ++k) {
    identical = identical && back.samples[k].x == point.samples[k].x && back.samples[k].y == point.samples[k].y &&
                back.samples[k].confidence == point.samples[k].confidence && back.samples[k].valid == point.samples[k].valid &&
                back.samples[k].time.Compare(point.samples[k].time) == 0;
  }
  CHECK(identical);

  const auto plane = trk::TrackPlane(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), 0, trk::Region{60, 30, 120, 90});
  const auto plane_back = trk::PlanarResultFromJson(trk::ToJson(plane));
  CHECK_EQ(plane_back.samples.size(), plane.samples.size());
  CHECK(plane_back.region.x == plane.region.x && plane_back.region.height == plane.region.height);
  bool same_warps = true;
  for (std::size_t k = 0; k < plane.samples.size(); ++k) {
    same_warps = same_warps && plane_back.samples[k].warp.a11 == plane.samples[k].warp.a11 && plane_back.samples[k].warp.tx == plane.samples[k].warp.tx &&
                 plane_back.samples[k].warp.a21 == plane.samples[k].warp.a21 && plane_back.samples[k].confidence == plane.samples[k].confidence;
  }
  CHECK(same_warps);

  FrameSet shaky;
  for (int k = 0; k < 6; ++k) {
    const auto s = Shake(k);
    shaky.Add(SceneFrame(320, 180, Camera(s.scale, s.theta, s.tx, s.ty, 160.0, 90.0)));
  }
  const auto stabilisation = trk::AnalyzeSimilarityStabilization(shaky.size(), cutline::time::kFrameRate25, shaky.Resolver());
  const auto stabilisation_back = trk::SimilarityResultFromJson(trk::ToJson(stabilisation));
  CHECK_EQ(stabilisation_back.samples.size(), stabilisation.samples.size());
  CHECK_EQ(stabilisation_back.camera_path.size(), stabilisation.camera_path.size());
  CHECK(stabilisation_back.auto_scale == stabilisation.auto_scale);
  bool same_corrections = true;
  for (std::size_t k = 0; k < stabilisation.samples.size(); ++k) {
    same_corrections = same_corrections && stabilisation_back.samples[k].translation_x == stabilisation.samples[k].translation_x &&
                       stabilisation_back.samples[k].rotation_degrees == stabilisation.samples[k].rotation_degrees &&
                       stabilisation_back.samples[k].scale == stabilisation.samples[k].scale;
  }
  CHECK(same_corrections);

  CHECK_THROWS(trk::TrackResultFromJson("{\"samples\": 3}"));
  CHECK_THROWS(trk::PlanarResultFromJson("not json"));
}

// ------------------------------------------------------------- noise reduction ----

namespace {

constexpr int kNoiseW = 128, kNoiseH = 96;

// Roughly Gaussian noise, a function of position, frame and channel only.
double GaussianAt(int x, int y, int frame, int channel) {
  const double u1 = std::max(Hash01(x, y, frame * 7 + channel * 131 + 1), 1e-9);
  const double u2 = Hash01(x + 977, y + 4001, frame * 11 + channel * 17 + 5);
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

// Flat areas, a hard edge, a gradient, and a patch of fine stripes: what noise reduction must smooth,
// keep, and not flatten.
float CleanValue(int x, int y, int box_x = 40) {
  float v = 0.3f + 0.2f * static_cast<float>(x) / kNoiseW;
  if (x >= box_x && x < box_x + 24 && y >= 20 && y < 60) v = 0.8f;
  if (x >= 90 && x < 120 && y >= 60 && y < 90) v = ((x / 2) % 2 == 0) ? 0.6f : 0.4f;  // stripes two pixels wide
  return v;
}

VideoFrame NoisyFrame(int frame, float sigma, int box_x = 40, bool colour_noise = true, bool luma_noise = true) {
  auto out = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, kNoiseW, kNoiseH);
  for (int y = 0; y < kNoiseH; ++y) {
    for (int x = 0; x < kNoiseW; ++x) {
      const float base = CleanValue(x, y, box_x);
      // Noise on each channel: a common part (luma) and a per-channel part (chroma).
      const float common = luma_noise ? static_cast<float>(GaussianAt(x, y, frame, 3)) * sigma : 0.0f;
      const float cr = colour_noise ? static_cast<float>(GaussianAt(x, y, frame, 0)) * sigma : 0.0f;
      const float cb = colour_noise ? static_cast<float>(GaussianAt(x, y, frame, 2)) * sigma : 0.0f;
      SetPixel(out, x, y, base + common + cr, base + common - 0.5f * (cr + cb), base + common + cb);
    }
  }
  return out;
}

VideoFrame CleanFrame(int box_x = 40) { return NoisyFrame(0, 0.0f, box_x); }

double Psnr(const VideoFrame& a, const VideoFrame& b) {
  double sum = 0.0;
  int n = 0;
  for (int y = 0; y < kNoiseH; ++y) {
    for (int x = 0; x < kNoiseW; ++x) {
      const auto p = At(a, x, y), q = At(b, x, y);
      sum += (p.r - q.r) * (p.r - q.r) + (p.g - q.g) * (p.g - q.g) + (p.b - q.b) * (p.b - q.b);
      n += 3;
    }
  }
  return 10.0 * std::log10(1.0 / (sum / n + 1e-12));
}

// The error over a box of the picture.
double Mse(const VideoFrame& a, const VideoFrame& b, int x0, int y0, int x1, int y1) {
  double sum = 0.0;
  int n = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const auto p = At(a, x, y), q = At(b, x, y);
      sum += (p.g - q.g) * (p.g - q.g);
      ++n;
    }
  }
  return sum / n;
}

// A clip with noise reduction, shown at a frame of a stack of source frames.
VideoFrame ComposeAt(const std::vector<VideoFrame>& frames, int index, Parameters parameters, Statistics& statistics) {
  auto sequence = MakeSequence();
  sequence.width = kNoiseW;
  sequence.height = kNoiseH;
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(MakeEffect("fx", "noise_reduction", std::move(parameters)));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  const cutline::render::FrameResolver resolver = [&](const SourceRequest& request) -> const VideoFrame* {
    const auto at = request.source_time.ToFrames(cutline::time::kFrameRate25, cutline::time::RoundingMode::Nearest);
    return at >= 0 && at < static_cast<std::int64_t>(frames.size()) ? &frames[static_cast<std::size_t>(at)] : nullptr;
  };
  return Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(index, 25)), resolver, statistics);
}

VideoFrame ComposeOne(const VideoFrame& frame, Parameters parameters) {
  std::vector<VideoFrame> one;
  one.push_back(frame.Clone());
  Statistics ignored;
  return ComposeAt(one, 0, std::move(parameters), ignored);
}

}  // namespace

CUTLINE_TEST(SpatialNoiseReductionRaisesTheSignalToNoiseRatioAndKeepsAnEdge) {
  const auto clean = CleanFrame();
  const auto noisy = NoisyFrame(0, 0.06f);
  const auto filtered = ComposeOne(noisy, {{"luma", Value::Scalar(0.7)}, {"chroma", Value::Scalar(0.7)}});
  const auto before = Psnr(noisy, clean), after = Psnr(filtered, clean);
  Expect(after > before + 4.0, "PSNR " + std::to_string(before) + " -> " + std::to_string(after) + " dB", __LINE__);
  // A flat patch is much quieter.
  CHECK(Mse(filtered, clean, 5, 5, 30, 18) < 0.4 * Mse(noisy, clean, 5, 5, 30, 18));
  // The edge of the bright box (x = 40, rows 25 to 55) is still an edge: its step across four pixels keeps most of its size.
  const auto step = [&](const VideoFrame& f) {
    double sum = 0.0;
    for (int y = 25; y < 55; ++y) sum += At(f, 42, y).g - At(f, 37, y).g;
    return sum / 30.0;
  };
  Expect(step(filtered) > 0.85 * step(clean), "edge step " + std::to_string(step(filtered)) + " of " + std::to_string(step(clean)), __LINE__);
  // Alpha is left alone and the effect is deterministic.
  CHECK(Near(At(filtered, 10, 10).a, 1.0f));
  const auto again = ComposeOne(noisy, {{"luma", Value::Scalar(0.7)}, {"chroma", Value::Scalar(0.7)}});
  CHECK(Psnr(again, filtered) > 100.0);
}

CUTLINE_TEST(ChromaNoiseReductionSmoothsColourAndLeavesLuminanceAlone) {
  const auto clean = CleanFrame();
  const auto noisy = NoisyFrame(0, 0.06f, 40, true, false);  // colour noise only
  const auto filtered = ComposeOne(noisy, {{"chroma", Value::Scalar(0.8)}});
  // The colour error is the red-blue difference from clean; the luma error is left as it was.
  const auto colour_error = [&](const VideoFrame& f) {
    double sum = 0.0;
    int n = 0;
    for (int y = 5; y < 18; ++y) {
      for (int x = 5; x < 30; ++x) {
        const auto p = At(f, x, y), q = At(clean, x, y);
        const double d = (p.r - p.b) - (q.r - q.b);
        sum += d * d;
        ++n;
      }
    }
    return sum / n;
  };
  CHECK(colour_error(filtered) < 0.25 * colour_error(noisy));
  // Luma (Rec.709) is untouched by a chroma-only pass, to within the conversion's own rounding.
  const auto luma = [](const Rgba& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; };
  double drift = 0.0;
  for (int y = 0; y < kNoiseH; y += 3) {
    for (int x = 0; x < kNoiseW; x += 3) drift = std::max(drift, std::abs(luma(At(filtered, x, y)) - luma(At(noisy, x, y))));
  }
  Expect(drift < 0.02, "luma moved by " + std::to_string(drift), __LINE__);
}

CUTLINE_TEST(DetailPreservationKeepsFineTextureThatStrongSmoothingWouldFlatten) {
  const auto noisy = NoisyFrame(0, 0.02f);
  const auto stripes = [&](const VideoFrame& f) {
    double sum = 0.0;
    int n = 0;
    for (int y = 65; y < 85; ++y) {
      for (int x = 94; x < 116; x += 4) {
        sum += std::abs(At(f, x, y).g - At(f, x + 2, y).g);  // across a light and a dark stripe
        ++n;
      }
    }
    return sum / n;
  };
  const auto protecting = ComposeOne(noisy, {{"luma", Value::Scalar(1.0)}, {"detail", Value::Scalar(1.0)}});
  const auto flattening = ComposeOne(noisy, {{"luma", Value::Scalar(1.0)}, {"detail", Value::Scalar(0.0)}});
  Expect(stripes(protecting) > 0.15 && stripes(protecting) > 1.5 * stripes(flattening),
         "stripes (0.2 apart) keep " + std::to_string(stripes(protecting)) + " with detail and " + std::to_string(stripes(flattening)) + " without", __LINE__);
}

CUTLINE_TEST(TemporalNoiseReductionAveragesStillAreasAcrossFrames) {
  const auto clean = CleanFrame();
  std::vector<VideoFrame> frames;
  for (int k = 0; k < 6; ++k) frames.push_back(NoisyFrame(k, 0.06f));
  Statistics one, two, none;
  const auto radius_one = ComposeAt(frames, 2, {{"temporal", Value::Scalar(0.6)}}, one);
  const auto radius_two = ComposeAt(frames, 2, {{"temporal", Value::Scalar(0.6)}, {"radius", Value::Scalar(2.0)}}, two);
  const auto untouched = ComposeAt(frames, 2, {}, none);
  const auto base = Psnr(frames[2], clean);
  CHECK(Psnr(untouched, clean) > base - 1e-6 && Psnr(untouched, clean) < base + 1e-6);
  Expect(Psnr(radius_one, clean) > base + 2.2, "one frame either side: " + std::to_string(base) + " -> " + std::to_string(Psnr(radius_one, clean)) + " dB", __LINE__);
  Expect(Psnr(radius_two, clean) > Psnr(radius_one, clean) + 1.0, "two frames either side: " + std::to_string(Psnr(radius_two, clean)) + " dB", __LINE__);
  // The memory it used is bounded: two neighbours, or four, and none when it is off.
  CHECK_EQ(one.noise_frames_used, 2);
  CHECK_EQ(two.noise_frames_used, 4);
  CHECK_EQ(none.noise_frames_used, 0);
  // At the start of the clip there is nothing before it, and it still works with what there is.
  Statistics edge;
  const auto first = ComposeAt(frames, 0, {{"temporal", Value::Scalar(0.6)}}, edge);
  CHECK_EQ(edge.noise_frames_used, 1);
  CHECK(Psnr(first, clean) > Psnr(frames[0], clean) + 1.0);
  Statistics last;
  (void)ComposeAt(frames, 5, {{"temporal", Value::Scalar(0.6)}}, last);
  CHECK_EQ(last.noise_frames_used, 1);  // no frame after the last
}

CUTLINE_TEST(TemporalNoiseReductionLeavesNoGhostBehindAMovingObject) {
  // A bright box moving 5 pixels a frame over a still, noisy background.
  std::vector<VideoFrame> frames;
  std::vector<VideoFrame> clean_frames;
  for (int k = 0; k < 5; ++k) {
    frames.push_back(NoisyFrame(k, 0.04f, 30 + 5 * k));
    clean_frames.push_back(CleanFrame(30 + 5 * k));
  }
  Statistics statistics;
  const auto filtered = ComposeAt(frames, 2, {{"temporal", Value::Scalar(0.5)}}, statistics);
  CHECK_EQ(statistics.noise_frames_used, 2);
  // At frame 2 the box is at x 40..64. Where it was in frame 1 (35..59) but is not now, x 35..40, a ghost
  // would lift the background; where it will be in frame 3 (45..69) but is not yet, x 64..69, likewise.
  const auto level = [&](const VideoFrame& f, int x0, int x1) {
    double sum = 0.0;
    int n = 0;
    for (int y = 25; y < 55; ++y) {
      for (int x = x0; x < x1; ++x) {
        sum += At(f, x, y).g;
        ++n;
      }
    }
    return sum / n;
  };
  const auto clean_trail = level(clean_frames[2], 35, 40), clean_lead = level(clean_frames[2], 64, 69);
  Expect(level(filtered, 35, 40) < clean_trail + 0.04, "a ghost trails the box: " + std::to_string(level(filtered, 35, 40)) + " against " + std::to_string(clean_trail), __LINE__);
  Expect(level(filtered, 64, 69) < clean_lead + 0.04, "a ghost leads the box: " + std::to_string(level(filtered, 64, 69)), __LINE__);
  // The box itself is intact and the still background is quieter.
  CHECK(level(filtered, 46, 58) > 0.74);
  const auto still = [&](const VideoFrame& f) { return Mse(f, clean_frames[2], 100, 5, 125, 18); };
  CHECK(still(filtered) < 0.6 * still(frames[2]));
}

CUTLINE_TEST(NoiseReductionDoesNothingUntilItIsAskedTo) {
  const auto noisy = NoisyFrame(0, 0.05f);
  const auto same = ComposeOne(noisy, {});
  CHECK(Psnr(same, noisy) > 100.0);
  // Its registered defaults are all zero strength.
  const auto* descriptor = cutline::effects::FindEffect("noise_reduction");
  CHECK(descriptor != nullptr);
  if (descriptor != nullptr) {
    Parameters defaults;
    for (const auto& parameter : descriptor->parameters) defaults.emplace_back(parameter.id, parameter.default_value);
    CHECK(Psnr(ComposeOne(noisy, defaults), noisy) > 100.0);
  }
  // As a sequence-level effect there are no neighbouring frames, and the spatial pass still works.
  auto sequence = MakeSequence();
  sequence.width = kNoiseW;
  sequence.height = kNoiseH;
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(track));
  sequence.effects.push_back(MakeEffect("fx", "noise_reduction", {{"luma", Value::Scalar(0.7)}, {"chroma", Value::Scalar(0.7)}, {"temporal", Value::Scalar(0.9)}}));
  Finalise(sequence);
  Frames frames;
  frames.Add("clip-1", noisy.Clone());
  Statistics statistics;
  const auto output = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(1, 25)), frames.Resolver(), statistics);
  Expect(Psnr(output, CleanFrame()) > Psnr(noisy, CleanFrame()) + 3.0, "PSNR " + std::to_string(Psnr(noisy, CleanFrame())) + " -> " + std::to_string(Psnr(output, CleanFrame())), __LINE__);
  CHECK_EQ(statistics.noise_frames_used, 0);
}

CUTLINE_TEST(OpticalFlowMatchesExhaustiveSearchIncludingConfidenceAndOcclusion) {
  // Uneven edge blocks, tiny pictures, flat ties, independent noise, motion,
  // negative/HDR float values, and 8-bit conversion exercise search pruning.
  for (const auto dimensions : {std::pair{1, 1}, std::pair{13, 9}, std::pair{24, 17}}) {
    for (int pattern = 0; pattern < 5; ++pattern) {
      auto first = SolidFrame(0.2f, 0.2f, 0.2f, 1.0f, dimensions.first, dimensions.second);
      auto second = first.Clone();
      if (pattern != 0) {
        for (int y = 0; y < first.height(); ++y) {
          for (int x = 0; x < first.width(); ++x) {
            for (int channel = 0; channel < 3; ++channel) {
              const auto texture = [&](int u, int offset) {
                const auto noise = static_cast<unsigned>((u + 31) * 17 + y * 31 + (u + 31) * y * 3 +
                                                         channel * 23 + offset * (x * 13 + y * 7 + 11));
                const auto value = static_cast<float>(noise % 101) / 100.0f;
                return pattern == 3 ? value * 5.0f - 2.0f : value;
              };
              first.row_f32(y)[x * 4 + channel] = texture(x, 0);
              second.row_f32(y)[x * 4 + channel] = texture(x - 2, pattern == 2 ? 1 : 0);
            }
          }
        }
      }
      if (pattern == 4) {
        first = cutline::media::ConvertFrame(first, PixelFormat::Rgba8);
        second = cutline::media::ConvertFrame(second, PixelFormat::Rgba8);
      }
      const auto a = first.format() == PixelFormat::RgbaF32 ? first.Clone()
                      : cutline::media::ConvertFrame(first, PixelFormat::RgbaF32);
      const auto b = second.format() == PixelFormat::RgbaF32 ? second.Clone()
                      : cutline::media::ConvertFrame(second, PixelFormat::RgbaF32);
      for (const auto settings : {std::pair{2, 0}, std::pair{4, 3}, std::pair{8, 12}}) {
        cutline::render::OpticalFlowConfig config;
        config.block_size = settings.first;
        config.search_radius = settings.second;
        auto expected = flow_reference::EstimateOneWay(a, b, config);
        const auto backward = flow_reference::EstimateOneWay(b, a, config);
        for (int row = 0; row < expected.rows; ++row) {
          for (int column = 0; column < expected.columns; ++column) {
            auto& motion = expected.samples[static_cast<std::size_t>(row * expected.columns + column)];
            const auto reverse = backward.sample((column + 0.5f) * expected.block_size + motion.dx,
                                                 (row + 0.5f) * expected.block_size + motion.dy);
            motion.occluded = motion.confidence < config.minimum_confidence ||
                std::hypot(motion.dx + reverse.dx, motion.dy + reverse.dy) > config.occlusion_tolerance;
          }
        }
        const auto actual = cutline::render::EstimateOpticalFlow(first, second, config);
        CHECK_EQ(actual.samples.size(), expected.samples.size());
        for (std::size_t index = 0; index < expected.samples.size(); ++index) {
          CHECK_EQ(actual.samples[index].dx, expected.samples[index].dx);
          CHECK_EQ(actual.samples[index].dy, expected.samples[index].dy);
          CHECK_EQ(actual.samples[index].confidence, expected.samples[index].confidence);
          CHECK_EQ(actual.samples[index].occluded, expected.samples[index].occluded);
        }
      }
    }
  }
}

CUTLINE_TEST(OpticalFlowFindsTranslationAndSynthesisesTheMiddleFrame) {
  constexpr int width = 40;
  constexpr int height = 24;
  auto first = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, width, height);
  auto second = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, width, height);
  for (int y = 4; y < height - 4; ++y) {
    for (int x = 4; x < width - 7; ++x) {
      const auto value = static_cast<float>((x * 17 + y * 31 + x * y * 3) % 101) / 100.0f;
      auto* a = first.row_f32(y) + static_cast<std::size_t>(x) * 4;
      auto* b = second.row_f32(y) + static_cast<std::size_t>(x + 3) * 4;
      a[0] = a[1] = a[2] = value;
      b[0] = b[1] = b[2] = value;
    }
  }
  cutline::render::OpticalFlowConfig config;
  config.block_size = 8;
  config.search_radius = 5;
  config.minimum_confidence = 0.0f;
  const auto forward = cutline::render::EstimateOpticalFlow(first, second, config);
  const auto middle_motion = forward.at(2, 1);
  CHECK(std::abs(middle_motion.dx - 3.0f) < 0.1f);
  CHECK(std::abs(middle_motion.dy) < 0.1f);
  CHECK(middle_motion.confidence > 0.1f);

  const auto midpoint = cutline::render::InterpolateOpticalFlow(first, second, 0.5f, forward);
  // The texture that began at x=12 and ends at x=15 appears midway.
  const auto expected = At(first, 12, 10).r;
  CHECK(std::abs(At(midpoint, 13, 10).r - expected) < 0.25f ||
        std::abs(At(midpoint, 14, 10).r - expected) < 0.25f);
}

CUTLINE_TEST(OpticalFlowRejectsMismatchedFramesAndMarksUncertainBlocks) {
  const auto small = SolidFrame(0.2f, 0.2f, 0.2f, 1.0f, 16, 16);
  const auto different = SolidFrame(0.2f, 0.2f, 0.2f, 1.0f, 17, 16);
  CHECK_THROWS(cutline::render::EstimateOpticalFlow(small, different));
  auto changed = SolidFrame(0.8f, 0.8f, 0.8f, 1.0f, 16, 16);
  cutline::render::OpticalFlowConfig config;
  config.block_size = 8;
  config.search_radius = 2;
  config.minimum_confidence = 0.2f;
  const auto field = cutline::render::EstimateOpticalFlow(small, changed, config);
  bool has_occlusion = false;
  for (const auto& sample : field.samples) has_occlusion = has_occlusion || sample.occluded;
  CHECK(has_occlusion);
}

// -------------------------------------------------------------------- render cache ----

namespace {

using cutline::render::RenderCache;
using cutline::render::RenderCacheConfig;
using cutline::render::RenderKey;
using cutline::render::SourceVersions;

SourceVersions Versions(const std::string& media_version = "v1") {
  SourceVersions versions;
  versions.media = [media_version](const std::string& id) { return id + "@" + media_version; };
  versions.asset = [](const std::string& reference) { return reference + "@asset1"; };
  return versions;
}

// A one-clip sequence whose clip carries an animated opacity: 1.0 at 2 s, 0.2 at 4 s, and a third key at
// `late_value` at 8 s.
Sequence CacheSequence(double late_value = 0.6) {
  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  auto effect = MakeEffect("fx", "opacity", {{"value", Value::Scalar(1.0)}});
  cutline::anim::AnimatedValue curve(Value::Scalar(1.0));
  curve.SetKeyframe({Seconds(2), Value::Scalar(1.0), Interpolation::Linear, {}, {}});
  curve.SetKeyframe({Seconds(4), Value::Scalar(0.2), Interpolation::Linear, {}, {}});
  curve.SetKeyframe({Seconds(8), Value::Scalar(late_value), Interpolation::Linear, {}, {}});
  effect.parameters[0].value = curve;
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  return sequence;
}

RenderKey KeyAt(const Sequence& sequence, std::int64_t numerator, std::int64_t denominator, const SourceVersions& versions,
                const CompositorConfig& config = {}) {
  cutline::timeline::SequenceGraph graph;
  graph.sequences.push_back(sequence);
  return cutline::render::KeyForTime(TimelineCompiler{}, graph, graph.sequences[0], RationalTime(numerator, denominator), {}, config, versions);
}

VideoFrame Flat(float value, int width = 64, int height = 36) { return SolidFrame(value, value, value, 1.0f, width, height); }

}  // namespace

CUTLINE_TEST(APicturesAddressChangesExactlyWhenSomethingItIsMadeFromChanges) {
  const auto versions = Versions();
  const auto base = CacheSequence();
  const auto key = KeyAt(base, 3, 1, versions);
  CHECK_EQ(key.size(), std::size_t{32});
  CHECK(KeyAt(CacheSequence(), 3, 1, versions) == key);  // the same inputs, the same address

  // An edit far from the time looked at leaves its address alone: the third key moves the curve after 4 s only.
  CHECK(KeyAt(CacheSequence(0.9), 3, 1, versions) == key);
  CHECK(KeyAt(CacheSequence(0.9), 1, 1, versions) == KeyAt(base, 1, 1, versions));
  // And changes the addresses it does touch.
  CHECK(KeyAt(CacheSequence(0.9), 6, 1, versions) != KeyAt(base, 6, 1, versions));
  // A different time gives a different picture of the media.
  CHECK(KeyAt(base, 3, 1, versions) != KeyAt(base, 5, 2, versions));

  // The media changing under the clip, the render version, the output size, the background.
  CHECK(KeyAt(base, 3, 1, Versions("v2")) != key);
  auto older = base;
  older.render_version = 2;
  CHECK(KeyAt(older, 3, 1, versions) != key);
  CompositorConfig bigger;
  bigger.width = 128;
  bigger.height = 72;
  CHECK(KeyAt(base, 3, 1, versions, bigger) != key);
  CompositorConfig grey;
  grey.background_red = 0.5f;
  CHECK(KeyAt(base, 3, 1, versions, grey) != key);
  CompositorConfig bytes;
  bytes.output_format = PixelFormat::RgbaF32;
  CHECK(KeyAt(base, 3, 1, versions, bytes) != key);

  // A look-up table's contents are part of the picture.
  auto lut = CacheSequence();
  auto lut_effect = MakeEffect("fx-lut", "lut", {{"intensity", Value::Scalar(1.0)}});
  lut_effect.preset_name = "looks/warm.cube";
  lut.tracks[0].clips[0].effects.push_back(lut_effect);
  SourceVersions changed = versions;
  changed.asset = [](const std::string& reference) { return reference + "@asset2"; };
  CHECK(KeyAt(lut, 3, 1, versions) != KeyAt(lut, 3, 1, changed));
  CHECK(KeyAt(lut, 3, 1, versions) == KeyAt(lut, 3, 1, versions));
}

CUTLINE_TEST(TheMemoryTierHoldsToItsBudgetAndDropsTheLeastRecentlyUsed) {
  const auto one = Flat(0.1f).size_bytes();
  RenderCacheConfig config;
  config.memory_bytes = one * 2 + one / 2;  // room for two
  RenderCache cache(config);
  cache.Store("a", Flat(0.1f));
  cache.Store("b", Flat(0.2f));
  CHECK(cache.Find("a") != nullptr);  // a is now the most recent
  cache.Store("c", Flat(0.3f));       // b, the least recent, goes
  CHECK(cache.Find("b") == nullptr);
  CHECK(cache.Find("a") != nullptr && cache.Find("c") != nullptr);
  const auto stats = cache.statistics();
  CHECK_EQ(stats.entries, std::size_t{2});
  CHECK(stats.memory_bytes <= config.memory_bytes);
  CHECK_EQ(stats.memory_evictions, std::int64_t{1});
  CHECK_EQ(stats.hits, std::int64_t{3});
  CHECK_EQ(stats.misses, std::int64_t{1});
  // A picture bigger than the whole budget is not held, and does not push everything else out.
  cache.Store("huge", Flat(0.5f, 640, 360));
  CHECK(cache.Find("huge") == nullptr);
  CHECK(cache.Find("a") != nullptr);
  // The same address twice is one entry.
  cache.Store("a", Flat(0.9f));
  CHECK_EQ(cache.statistics().entries, std::size_t{2});
  CHECK(Near(At(*cache.Find("a"), 3, 3).r, 0.9f));
  cache.Clear();
  CHECK(cache.Find("a") == nullptr);
  CHECK_EQ(cache.statistics().memory_bytes, std::size_t{0});
}

CUTLINE_TEST(TheDiskTierSurvivesARestartAndDistrustsAnythingDamaged) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-render-cache-test";
  std::filesystem::remove_all(directory);
  RenderCacheConfig config;
  config.disk_directory = directory;
  const auto key = std::string(32, 'a');
  auto frame = SolidFrame(0.25f, 0.5f, 0.75f, 1.0f, 40, 24);
  frame.presentation_time = RationalTime(7, 25);
  frame.color.primaries = "bt2020";
  frame.color.transfer = "smpte2084";
  {
    RenderCache cache(config);
    cache.Store(key, frame);
    CHECK_EQ(cache.statistics().disk_writes, std::int64_t{1});
    CHECK(std::filesystem::exists(directory / (key + ".clrc")));
  }
  {
    // A new cache over the same directory: nothing in memory, the picture is on disk, exactly.
    RenderCache cache(config);
    CHECK(cache.Contains(key));
    const auto found = cache.Find(key);
    CHECK(found != nullptr);
    if (found != nullptr) {
      CHECK_EQ(found->width(), 40);
      CHECK(Near(At(*found, 10, 10).g, 0.5f, 1e-7f) && Near(At(*found, 39, 23).b, 0.75f, 1e-7f));
      CHECK(found->presentation_time.Compare(RationalTime(7, 25)) == 0);
      CHECK_EQ(found->color.transfer, std::string("smpte2084"));
    }
    CHECK_EQ(cache.statistics().disk_hits, std::int64_t{1});
    CHECK(cache.Find(key) != nullptr);  // and now from memory
    CHECK_EQ(cache.statistics().disk_hits, std::int64_t{1});
  }
  // Damage: one flipped byte in the middle of the pixels.
  {
    std::fstream file(directory / (key + ".clrc"), std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(200);
    char byte = 0;
    file.read(&byte, 1);
    file.seekp(200);
    byte = static_cast<char>(byte ^ 0x5A);
    file.write(&byte, 1);
  }
  {
    RenderCache cache(config);
    CHECK(cache.Find(key) == nullptr);
    CHECK_EQ(cache.statistics().corrupt_files, std::int64_t{1});
    CHECK(!std::filesystem::exists(directory / (key + ".clrc")));  // not left to fail again
  }
  // A file cut short, and one left half-written by a crash.
  {
    RenderCache cache(config);
    cache.Store(key, frame);
  }
  std::filesystem::resize_file(directory / (key + ".clrc"), 100);
  { std::ofstream(directory / (std::string(32, 'b') + ".clrc.tmp"), std::ios::binary) << "half a picture"; }
  {
    RenderCache cache(config);
    CHECK(cache.Find(key) == nullptr);
    CHECK(!std::filesystem::exists(directory / (std::string(32, 'b') + ".clrc.tmp")));
  }
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(TheDiskTierStaysWithinItsBudgetByDroppingTheOldest) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-render-cache-budget";
  std::filesystem::remove_all(directory);
  RenderCacheConfig config;
  config.disk_directory = directory;
  config.memory_bytes = 1;  // everything has to be found on disk
  const auto one = Flat(0.1f, 32, 18).size_bytes();
  config.disk_bytes = one * 2 + 600;  // room for two files with their headers
  RenderCache cache(config);
  for (int i = 0; i < 4; ++i) {
    cache.Store(std::string(31, 'x') + static_cast<char>('a' + i), Flat(0.1f * (i + 1), 32, 18));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  const auto stats = cache.statistics();
  CHECK(stats.disk_bytes <= config.disk_bytes);
  CHECK(stats.disk_evictions >= 2);
  CHECK(!cache.Contains(std::string(31, 'x') + "a"));
  CHECK(cache.Contains(std::string(31, 'x') + "d"));
  CHECK(Near(At(*cache.Find(std::string(31, 'x') + "d"), 1, 1).r, 0.4f, 1e-6f));
  std::filesystem::remove_all(directory);
}

// ------------------------------------------------------------------------ text ----

namespace {

cutline::render::text::Style TextStyle(double size, const std::string& family = "Arial") {
  cutline::render::text::Style style;
  style.family = family;
  style.size = size;
  return style;
}

}  // namespace

CUTLINE_TEST(TextIsRasterisedAtTheSizeAskedWrappedAndAlignedAndAFontThatIsMissingIsSaid) {
  namespace txt = cutline::render::text;
  SKIP_INAPPLICABLE(txt::Available(), "this build has no text rasteriser");
  const auto small = txt::Rasterize("Hello world", TextStyle(24.0));
  const auto large = txt::Rasterize("Hello world", TextStyle(48.0));
  CHECK(!small.empty() && !large.empty());
  CHECK(large.width > static_cast<int>(1.7 * small.width) && large.width < static_cast<int>(2.3 * small.width));
  CHECK(large.height > static_cast<int>(1.6 * small.height));
  // Coverage is a coverage: in range, and present.
  float peak = 0.0f, total = 0.0f;
  for (const auto c : large.coverage) {
    peak = std::max(peak, c);
    total += c;
    CHECK(c >= 0.0f && c <= 1.0f);
  }
  CHECK(peak > 0.9f && total > 100.0f);

  // Wrapped to a width, it is taller and no wider than the width.
  const std::string sentence = "The quick brown fox jumps over the lazy dog and keeps running";
  const auto one_line = txt::Rasterize(sentence, TextStyle(32.0));
  const auto wrapped = txt::Rasterize(sentence, TextStyle(32.0), 300);
  CHECK(wrapped.width <= 302 && wrapped.lines >= 3 && wrapped.height > 2 * one_line.height);
  CHECK_EQ(one_line.lines, 1);

  // Alignment moves the shorter line within a two-line block.
  const auto centroid_of_first_row_band = [](const txt::Bitmap& b, int y0, int y1) {
    double sum = 0.0, weight = 0.0;
    for (int y = y0; y < y1; ++y) {
      for (int x = 0; x < b.width; ++x) {
        sum += x * b.at(x, y);
        weight += b.at(x, y);
      }
    }
    return weight > 0.0 ? sum / weight : 0.0;
  };
  auto left_style = TextStyle(32.0);
  left_style.align = txt::Align::Left;
  auto right_style = left_style;
  right_style.align = txt::Align::Right;
  const auto left = txt::Rasterize("A much longer first line\nShort", left_style);
  const auto right = txt::Rasterize("A much longer first line\nShort", right_style);
  const auto bottom_band = [](const txt::Bitmap& b) { return std::pair{b.height * 3 / 5, b.height}; };
  CHECK(centroid_of_first_row_band(right, bottom_band(right).first, bottom_band(right).second) >
        centroid_of_first_row_band(left, bottom_band(left).first, bottom_band(left).second) + 20.0);

  // Bold is wider; non-Latin text draws; the same text twice is the same bitmap.
  auto bold = TextStyle(32.0);
  bold.bold = true;
  CHECK(txt::Rasterize("Weight", bold).width > txt::Rasterize("Weight", TextStyle(32.0)).width);
  CHECK(!txt::Rasterize("Caf\xC3\xA9 \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", TextStyle(32.0)).empty());
  const auto again = txt::Rasterize("Hello world", TextStyle(48.0));
  CHECK(again.coverage == large.coverage);
  CHECK(txt::Rasterize("", TextStyle(32.0)).empty());
  CHECK(txt::Rasterize("   ", TextStyle(32.0)).empty());

  // Fonts: the installed ones are listed, and one that is not installed is substituted and says so.
  const auto families = txt::InstalledFamilies();
  CHECK(!families.empty());
  CHECK(std::is_sorted(families.begin(), families.end()));
  CHECK(std::adjacent_find(families.begin(), families.end()) == families.end());
  const auto missing = txt::Rasterize("Hello", TextStyle(32.0, "Zzyzx Not A Real Font Name"));
  CHECK(missing.substituted && !missing.resolved_family.empty() && !txt::HasFamily("Zzyzx Not A Real Font Name"));
  if (txt::HasFamily("Arial")) CHECK(!txt::Rasterize("Hello", TextStyle(32.0, "Arial")).substituted);
}

CUTLINE_TEST(GraphicsDocumentsDrawShapesImagesAndRoundTripTheirVersionedFormat) {
  cutline::render::graphics::Document document;
  document.width = 100;
  document.height = 100;
  cutline::render::graphics::Element rectangle;
  rectangle.id = "box";
  rectangle.type = cutline::render::graphics::ElementType::Rectangle;
  rectangle.x = 0.1;
  rectangle.y = 0.2;
  rectangle.width = 0.4;
  rectangle.height = 0.3;
  rectangle.fill = {1.0, 0.0, 0.0, 1.0};
  document.elements.push_back(rectangle);
  cutline::render::graphics::Element image;
  image.id = "logo";
  image.type = cutline::render::graphics::ElementType::Image;
  image.x = 0.6;
  image.y = 0.1;
  image.width = 0.2;
  image.height = 0.2;
  image.asset = "logo";
  document.elements.push_back(image);

  const auto encoded = cutline::render::graphics::ToJson(document);
  const auto decoded = cutline::render::graphics::ParseDocument(encoded);
  CHECK_EQ(decoded.elements.size(), std::size_t{2});
  cutline::render::Layer canvas;
  canvas.Reset(100, 100);
  const auto logo = SolidFrame(0.0f, 1.0f, 0.0f, 1.0f, 4, 4);
  const auto result = cutline::render::graphics::Draw(canvas, decoded, [&](const std::string& asset) {
    return asset == "logo" ? &logo : static_cast<const VideoFrame*>(nullptr);
  });
  CHECK_EQ(result.elements_drawn, 2);
  CHECK(Near(canvas.at(20, 30).r, 1.0f));
  CHECK(Near(canvas.at(65, 15).g, 1.0f));
}

CUTLINE_TEST(MotionGraphicsTemplatesExposeVersionedReusableControls) {
  cutline::render::graphics::TemplatePackage package;
  package.id = "lower-third";
  package.version = 3;
  cutline::render::graphics::Element title;
  title.id = "title";
  title.type = cutline::render::graphics::ElementType::Text;
  title.text = "Default";
  title.width = 0.8;
  title.height = 0.2;
  package.document.elements.push_back(title);
  package.controls.push_back({"headline", "title", "text", "Default"});
  package.controls.push_back({"alpha", "title", "opacity", "1"});
  const auto round_trip = cutline::render::graphics::ParseTemplate(cutline::render::graphics::ToJson(package));
  const auto instance = cutline::render::graphics::Instantiate(round_trip, {{"headline", "Offline News"}, {"alpha", "0.5"}});
  CHECK_EQ(instance.elements[0].text, std::string("Offline News"));
  CHECK(Near(static_cast<float>(instance.elements[0].opacity), 0.5f));
  CHECK_THROWS(cutline::render::graphics::Instantiate(round_trip, {{"unknown", "x"}}));
}

CUTLINE_TEST(GraphicGeneratorPackagesRenderWithoutADecodedMediaFrame) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-graphic-generator";
  std::filesystem::create_directories(directory);
  const auto path = directory / "card.cutgraphic";
  cutline::render::graphics::Document document;
  cutline::render::graphics::Element card;
  card.id = "card";
  card.type = cutline::render::graphics::ElementType::Rectangle;
  card.width = 1.0;
  card.height = 1.0;
  card.fill = {0.2, 0.4, 0.8, 1.0};
  document.elements.push_back(card);
  { std::ofstream out(path, std::ios::binary); out << cutline::render::graphics::ToJson(document); }

  auto sequence = MakeSequence();
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("graphic", "", 0, 2);
  auto effect = MakeEffect("graphic-fx", "graphic", {});
  effect.preset_name = path.filename().string();
  clip.effects.push_back(std::move(effect));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  auto config = FloatOutput();
  config.asset_root = directory.string();
  Statistics statistics;
  const auto output = Compositor(config).Compose(TimelineCompiler{}.Compile(sequence, Seconds(1)), Frames{}.Resolver(), statistics);
  CHECK(Near(Centre(output).b, 0.8f));
  CHECK_EQ(statistics.graphics_elements_drawn, 1);
  CHECK_EQ(statistics.missing_frames, 0);
}

// ------------------------------------------------------------- burned-in captions ----

namespace {

cutline::timeline::Sequence CaptionedSequence(const std::string& track_style, const std::vector<std::pair<std::string, std::string>>& cues_and_styles) {
  auto sequence = MakeSequence();
  sequence.width = 640;
  sequence.height = 360;
  auto track = MakeTrack("v1", 0);
  track.clips.push_back(MakeClip("clip-1", "media-1", 0, 10));
  sequence.tracks.push_back(std::move(track));
  cutline::captions::Track captions_track;
  captions_track.id = "cap";
  captions_track.language = "en";
  captions_track.style_json = track_style;
  int n = 0;
  for (const auto& [text, style] : cues_and_styles) {
    cutline::captions::Cue cue;
    cue.id = "cue-" + std::to_string(++n);
    cue.start = Seconds(0);
    cue.end = Seconds(5);
    cue.text = text;
    cue.style_json = style;
    cue.resolved_style_json = cutline::captions::MergeStyles(track_style, style);
    captions_track.cues.push_back(std::move(cue));
  }
  sequence.caption_tracks.push_back(std::move(captions_track));
  Finalise(sequence);
  return sequence;
}

VideoFrame ComposeCaptioned(const cutline::timeline::Sequence& sequence, bool captions, std::int64_t at_ms, Statistics& statistics,
                            float background = 0.5f) {
  Frames frames;
  frames.Add("clip-1", SolidFrame(background, background, background, 1.0f, 640, 360));
  cutline::timeline::CompileOptions options;
  options.include_captions = captions;
  return Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(at_ms, 1000), options), frames.Resolver(), statistics);
}

struct Box final {
  int x0{1 << 30}, y0{1 << 30}, x1{-1}, y1{-1};
  bool empty() const { return x1 < 0; }
};

// Where the picture differs from a plain one.
Box ChangedBox(const VideoFrame& a, float plain) {
  Box box;
  for (int y = 0; y < a.height(); ++y) {
    for (int x = 0; x < a.width(); ++x) {
      const auto p = At(a, x, y);
      if (std::abs(p.r - plain) > 0.004f || std::abs(p.g - plain) > 0.004f || std::abs(p.b - plain) > 0.004f) {
        box.x0 = std::min(box.x0, x);
        box.y0 = std::min(box.y0, y);
        box.x1 = std::max(box.x1, x);
        box.y1 = std::max(box.y1, y);
      }
    }
  }
  return box;
}

}  // namespace

CUTLINE_TEST(CaptionsAreBurnedIntoThePictureOnlyWhenAskedAndSitInTheTitleSafeAreaAtTheBottom) {
  SKIP_INAPPLICABLE(cutline::render::text::Available(), "this build has no text rasteriser");
  const auto sequence = CaptionedSequence("{}", {{"Hello, captions", "{}"}});
  Statistics plain_stats, captioned_stats, after_stats;
  const auto plain = ComposeCaptioned(sequence, false, 1000, plain_stats);
  CHECK(ChangedBox(plain, 0.5f).empty());
  CHECK_EQ(plain_stats.captions_drawn, 0);
  const auto captioned = ComposeCaptioned(sequence, true, 1000, captioned_stats);
  CHECK_EQ(captioned_stats.captions_drawn, 1);
  const auto box = ChangedBox(captioned, 0.5f);
  CHECK(!box.empty());
  // Bottom of the frame, inside the title-safe area (the middle 80 percent), centred.
  CHECK(box.y0 > 360 / 2);
  CHECK(box.y1 <= 360 - 36);
  CHECK(box.x0 >= 64 && box.x1 <= 640 - 64);
  CHECK(std::abs((box.x0 + box.x1) / 2 - 320) < 8);
  // After the cue has ended there is nothing to draw.
  const auto after = ComposeCaptioned(sequence, true, 5000, after_stats);
  CHECK(ChangedBox(after, 0.5f).empty());
  CHECK_EQ(after_stats.captions_drawn, 0);
  // Drawn the same way every time.
  Statistics again_stats;
  const auto again = ComposeCaptioned(sequence, true, 1000, again_stats);
  bool same = true;
  for (int y = 0; y < 360 && same; y += 3) {
    for (int x = 0; x < 640; x += 3) same = same && At(again, x, y).g == At(captioned, x, y).g;
  }
  CHECK(same);
}

CUTLINE_TEST(ACaptionsStyleDecidesItsColourBackgroundPositionWidthAndOutline) {
  SKIP_INAPPLICABLE(cutline::render::text::Available(), "this build has no text rasteriser");
  const auto brightest_red = [](const VideoFrame& f, const Box& box) {
    float best = 0.0f;
    for (int y = box.y0; y <= box.y1; ++y) {
      for (int x = box.x0; x <= box.x1; ++x) {
        const auto p = At(f, x, y);
        if (p.r - p.g > best) best = p.r - p.g;
      }
    }
    return best;
  };
  Statistics s;
  // Colour: red text on a plain background shows red.
  const auto red = ComposeCaptioned(CaptionedSequence("{\"color\":[1,0,0,1],\"background\":[0,0,0,0]}", {{"Red words", "{}"}}), true, 100, s);
  CHECK(brightest_red(red, ChangedBox(red, 0.5f)) > 0.4f);

  // A background box darkens the picture behind the text, over a box bigger than the text.
  const auto boxed = ComposeCaptioned(CaptionedSequence("{\"background\":[0,0,0,0.8]}", {{"Boxed", "{}"}}), true, 100, s);
  const auto boxed_region = ChangedBox(boxed, 0.5f);
  const auto unboxed = ComposeCaptioned(CaptionedSequence("{\"background\":[0,0,0,0]}", {{"Boxed", "{}"}}), true, 100, s);
  const auto text_region = ChangedBox(unboxed, 0.5f);
  CHECK(boxed_region.x1 - boxed_region.x0 > text_region.x1 - text_region.x0 + 8);
  CHECK(boxed_region.y1 - boxed_region.y0 > text_region.y1 - text_region.y0 + 8);
  CHECK(At(boxed, boxed_region.x0 + 2, boxed_region.y0 + 2).g < 0.2f);  // a corner of the box: 0.5 under 80 percent black

  // Position: top is above the middle which is above the bottom.
  const auto top = ChangedBox(ComposeCaptioned(CaptionedSequence("{\"position\":\"top\"}", {{"Up", "{}"}}), true, 100, s), 0.5f);
  const auto middle = ChangedBox(ComposeCaptioned(CaptionedSequence("{\"position\":\"middle\"}", {{"Mid", "{}"}}), true, 100, s), 0.5f);
  const auto bottom = ChangedBox(ComposeCaptioned(CaptionedSequence("{}", {{"Down", "{}"}}), true, 100, s), 0.5f);
  CHECK(top.y1 < middle.y0 && middle.y1 < bottom.y0);
  CHECK(top.y0 >= 36 && std::abs((middle.y0 + middle.y1) / 2 - 180) < 12);

  // Alignment: left hugs the safe edge, right the other.
  const auto left = ChangedBox(ComposeCaptioned(CaptionedSequence("{\"align\":\"left\"}", {{"Left", "{}"}}), true, 100, s), 0.5f);
  const auto right = ChangedBox(ComposeCaptioned(CaptionedSequence("{\"align\":\"right\"}", {{"Right", "{}"}}), true, 100, s), 0.5f);
  CHECK(left.x0 < 64 + 20 && right.x1 > 640 - 64 - 20);

  // A long line wraps within the maximum width, and a narrower maximum gives more lines.
  const std::string long_text = "This is a long caption that cannot possibly fit on a single line of a small picture";
  const auto wide = ChangedBox(ComposeCaptioned(CaptionedSequence("{\"background\":[0,0,0,0]}", {{long_text, "{}"}}), true, 100, s), 0.5f);
  CHECK(wide.x1 - wide.x0 <= static_cast<int>(0.8 * 640) + 4);
  const auto narrow = ChangedBox(ComposeCaptioned(CaptionedSequence("{\"background\":[0,0,0,0],\"maxWidth\":0.4}", {{long_text, "{}"}}), true, 100, s), 0.5f);
  CHECK(narrow.x1 - narrow.x0 <= static_cast<int>(0.4 * 640) + 4);
  CHECK(narrow.y1 - narrow.y0 > wide.y1 - wide.y0);

  // An outline puts a ring of its colour around white letters on a mid grey.
  const auto outlined = ComposeCaptioned(CaptionedSequence("{\"background\":[0,0,0,0],\"outline\":[0,0,0,1],\"outlineWidth\":0.008}", {{"Ring", "{}"}}), true, 100, s);
  const auto flat = ComposeCaptioned(CaptionedSequence("{\"background\":[0,0,0,0]}", {{"Ring", "{}"}}), true, 100, s);
  float darkest_outlined = 1.0f, darkest_flat = 1.0f;
  for (int y = 0; y < 360; ++y) {
    for (int x = 0; x < 640; ++x) {
      darkest_outlined = std::min(darkest_outlined, At(outlined, x, y).g);
      darkest_flat = std::min(darkest_flat, At(flat, x, y).g);
    }
  }
  CHECK(darkest_outlined < 0.1f && darkest_flat > 0.45f);
}

CUTLINE_TEST(CuesShowingTogetherStackAndAMissingFontIsReported) {
  SKIP_INAPPLICABLE(cutline::render::text::Available(), "this build has no text rasteriser");
  Statistics one_stats, two_stats;
  const auto one = ChangedBox(ComposeCaptioned(CaptionedSequence("{}", {{"First", "{}"}}), true, 100, one_stats), 0.5f);
  const auto two = ChangedBox(ComposeCaptioned(CaptionedSequence("{}", {{"First", "{}"}, {"Second", "{}"}}), true, 100, two_stats), 0.5f);
  CHECK_EQ(two_stats.captions_drawn, 2);
  CHECK(two.y1 == one.y1 || std::abs(two.y1 - one.y1) <= 2);  // the first stays where it was, nearest the edge
  CHECK(two.y1 - two.y0 > (one.y1 - one.y0) * 3 / 2);          // and the second sits above it

  Statistics font_stats;
  (void)ComposeCaptioned(CaptionedSequence("{\"family\":\"Zzyzx Not A Real Font Name\"}", {{"Words", "{}"}}), true, 100, font_stats);
  bool said = false;
  for (const auto& message : font_stats.effect_errors) said = said || message.find("not installed") != std::string::npos;
  CHECK(said);
}

// ----------------------------------------------------------- rolling-shutter analysis ----

namespace {

// A pan across a scene at (vx, vy) pixels a frame, captured row by row: row y of frame k is read at
// k + readout * y / (H - 1) frames. With bottom_to_top the rows are read in the other order.
VideoFrame RollingFrame(int w, int h, int k, double vx, double vy, double readout, bool bottom_to_top = false) {
  auto frame = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, w, h);
  for (int y = 0; y < h; ++y) {
    auto scan = static_cast<double>(y) / (h - 1);
    if (bottom_to_top) scan = 1.0 - scan;
    const double t = k + readout * scan;
    for (int x = 0; x < w; ++x) {
      const auto v = static_cast<float>(Scene(x - vx * t, y - vy * t));
      SetPixel(frame, x, y, v, v, v);
    }
  }
  return frame;
}

VideoFrame GlobalFrame(int w, int h, int k, double vx, double vy) { return RollingFrame(w, h, k, vx, vy, 0.0); }

double CentralDifference(const VideoFrame& a, const VideoFrame& b) {
  double sum = 0.0;
  int n = 0;
  for (int y = a.height() / 8; y < a.height() * 7 / 8; y += 2) {
    for (int x = a.width() / 8; x < a.width() * 7 / 8; x += 2) {
      sum += std::abs(At(a, x, y).r - At(b, x, y).r);
      ++n;
    }
  }
  return sum / n;
}

}  // namespace

CUTLINE_TEST(RollingShutterAnalysisMeasuresThePanAndTurnsItIntoTheScanLineCorrection) {
  const int w = 320, h = 180, count = 8;
  const double vx = 5.0, vy = 1.2, readout = 0.6;
  FrameSet frames;
  for (int k = 0; k < count; ++k) frames.Add(RollingFrame(w, h, k, vx, vy, readout));
  const auto profile = *trk::FindProfile(trk::BuiltInCameraProfiles(), "cmos-typical");
  CHECK(std::abs(profile.readout - readout) < 1e-12);
  const auto result = trk::AnalyzeRollingShutter(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), profile);
  CHECK_EQ(result.samples.size(), std::size_t{8});
  double worst_velocity = 0.0, worst_correction = 0.0;
  for (const auto& sample : result.samples) {
    CHECK(sample.valid && sample.confidence > 0.9);
    worst_velocity = std::max({worst_velocity, std::abs(sample.velocity_x - vx), std::abs(sample.velocity_y - vy)});
    worst_correction = std::max({worst_correction, std::abs(sample.horizontal - vx * readout), std::abs(sample.vertical - vy * readout)});
  }
  Expect(worst_velocity < 0.15, "velocity error " + std::to_string(worst_velocity) + " px/frame", __LINE__);
  Expect(worst_correction < 0.1, "correction error " + std::to_string(worst_correction) + " px", __LINE__);
  CHECK(result.samples[3].time.Compare(RationalTime(3, 25)) == 0);

  // Through the compositor: the repaired frame is the global-shutter frame.
  auto sequence = MakeSequence();
  sequence.width = w;
  sequence.height = h;
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(trk::MakeRollingShutterEffect(result, "fx-rs"));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  double before = 0.0, after = 0.0;
  for (int k = 1; k < count - 1; ++k) {
    Frames current;
    current.Add("clip-1", frames.at(static_cast<std::size_t>(k)).Clone());
    const auto repaired = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(k, 25)), current.Resolver());
    const auto ideal = GlobalFrame(w, h, k, vx, vy);
    before += CentralDifference(frames.at(static_cast<std::size_t>(k)), ideal);
    after += CentralDifference(repaired, ideal);
  }
  Expect(after < 0.3 * before, "skew left after repair " + std::to_string(after / (count - 2)) + " against " + std::to_string(before / (count - 2)) + " before", __LINE__);
  Expect(after / (count - 2) < 0.03, "residual " + std::to_string(after / (count - 2)), __LINE__);
}

CUTLINE_TEST(AGlobalShutterNeedsNoRepairAndTheOtherScanDirectionIsRepairedToo) {
  const int w = 320, h = 180, count = 6;
  FrameSet global;
  for (int k = 0; k < count; ++k) global.Add(GlobalFrame(w, h, k, 4.0, 0.0));
  const auto none = trk::AnalyzeRollingShutter(global.size(), cutline::time::kFrameRate25, global.Resolver(), *trk::FindProfile(trk::BuiltInCameraProfiles(), "global"));
  for (const auto& sample : none.samples) CHECK(sample.horizontal == 0.0 && sample.vertical == 0.0 && sample.valid);

  // A sensor read from the bottom up: the profile says so, the effect carries it, and the repair holds.
  trk::CameraProfile upside_down{"flipped", "Read bottom to top", 0.5, true, ""};
  FrameSet frames;
  for (int k = 0; k < count; ++k) frames.Add(RollingFrame(w, h, k, 4.0, 0.0, 0.5, true));
  const auto result = trk::AnalyzeRollingShutter(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), upside_down);
  const auto effect = trk::MakeRollingShutterEffect(result, "fx");
  CHECK_EQ(effect.effect_type, std::string("rolling_shutter"));
  const auto direction = std::find_if(effect.parameters.begin(), effect.parameters.end(), [](const auto& p) { return p.name == "direction"; });
  CHECK(direction != effect.parameters.end() && direction->value.constant().scalar() == 1.0);
  auto sequence = MakeSequence();
  sequence.width = w;
  sequence.height = h;
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip-1", "media-1", 0, 10);
  clip.effects.push_back(effect);
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);
  Frames current;
  current.Add("clip-1", frames.at(2).Clone());
  const auto repaired = Compositor(FloatOutput()).Compose(TimelineCompiler{}.Compile(sequence, RationalTime(2, 25)), current.Resolver());
  const auto ideal = GlobalFrame(w, h, 2, 4.0, 0.0);
  CHECK(CentralDifference(repaired, ideal) < 0.3 * CentralDifference(frames.at(2), ideal));
}

CUTLINE_TEST(CameraProfilesAreDataThatIsValidatedAndFramesWithNothingToMeasureAreHeldNotGuessed) {
  // Profiles round trip and are checked.
  const auto json = trk::ProfilesToJson(trk::BuiltInCameraProfiles());
  const auto back = trk::ProfilesFromJson(json);
  CHECK_EQ(back.size(), trk::BuiltInCameraProfiles().size());
  CHECK(trk::FindProfile(back, "cmos-slow") != nullptr && trk::FindProfile(back, "nothing") == nullptr);
  CHECK(trk::BuiltInCameraProfiles()[2].notes.find("not a measurement") != std::string::npos);  // honest about what they are
  CHECK_THROWS(trk::ProfilesFromJson("{\"profiles\":[{\"id\":\"x\",\"name\":\"X\",\"readout\":1.5,\"bottomToTop\":false}]}"));
  CHECK_THROWS(trk::ProfilesFromJson("{\"profiles\":[{\"id\":\"\",\"name\":\"X\",\"readout\":0.5,\"bottomToTop\":false}]}"));
  CHECK_THROWS(trk::ProfilesFromJson("{\"profiles\":[{\"id\":\"a\",\"name\":\"A\",\"readout\":0.5,\"bottomToTop\":false},{\"id\":\"a\",\"name\":\"B\",\"readout\":0.5,\"bottomToTop\":false}]}"));
  const auto measured = trk::ProfilesFromJson("{\"profiles\":[{\"id\":\"mine\",\"name\":\"My camera, measured\",\"readout\":0.47,\"bottomToTop\":false}]}");
  CHECK(std::abs(measured[0].readout - 0.47) < 1e-12);

  // A flat grey frame in the middle of a pan: the two frames whose motion is measured against it cannot be
  // measured, and each holds the correction before it.
  FrameSet frames;
  for (int k = 0; k < 6; ++k) frames.Add(k == 3 ? SolidFrame(0.4f, 0.4f, 0.4f, 1.0f, 320, 180) : RollingFrame(320, 180, k, 4.0, 0.0, 0.6));
  const auto result = trk::AnalyzeRollingShutter(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), measured[0]);
  CHECK(!result.samples[2].valid && !result.samples[4].valid);
  CHECK(result.samples[1].valid && result.samples[3].valid);
  const auto effect = trk::MakeRollingShutterEffect(result, "fx");
  const auto& keys = effect.parameters[0].value.keyframes();
  CHECK_EQ(keys.size(), std::size_t{6});
  CHECK(std::abs(keys[2].value.scalar() - keys[1].value.scalar()) < 1e-12);
  CHECK(std::abs(keys[4].value.scalar() - keys[3].value.scalar()) < 1e-12);

  // Progress, cancellation and bad input.
  std::size_t reported = 0;
  int polls = 0;
  const auto stopped = trk::AnalyzeRollingShutter(frames.size(), cutline::time::kFrameRate25, frames.Resolver(), measured[0], {},
                                                  [&]() { return ++polls > 2; }, [&](std::size_t done, std::size_t) { reported = done; });
  CHECK(stopped.cancelled);
  CHECK_EQ(stopped.samples.size(), std::size_t{2});
  CHECK_EQ(reported, std::size_t{2});
  CHECK_THROWS(trk::AnalyzeRollingShutter(1, cutline::time::kFrameRate25, frames.Resolver(), measured[0]));
  CHECK_THROWS(trk::MakeRollingShutterEffect(trk::RollingShutterResult{}, "fx"));
}

// ------------------------------------------------------------ angle monitor ----

CUTLINE_TEST(TheAngleMonitorTilesEveryAngleKeepsEachPicturesShapeAndMarksTheLiveOne) {
  cutline::render::AngleMonitorConfig config;
  config.width = 320;
  config.height = 180;
  const auto rects = cutline::render::AngleMonitorLayout(3, config);
  CHECK_EQ(rects.size(), std::size_t{3});
  // Inside the frame and not overlapping.
  for (std::size_t i = 0; i < rects.size(); ++i) {
    CHECK(rects[i].x >= 0 && rects[i].y >= 0 && rects[i].x + rects[i].width <= config.width && rects[i].y + rects[i].height <= config.height);
    for (std::size_t j = i + 1; j < rects.size(); ++j) {
      const bool apart = rects[i].x + rects[i].width <= rects[j].x || rects[j].x + rects[j].width <= rects[i].x ||
                         rects[i].y + rects[i].height <= rects[j].y || rects[j].y + rects[j].height <= rects[i].y;
      CHECK(apart);
    }
  }
  // A click resolves to the angle under it, and the gap and the unused fourth cell to none.
  for (std::size_t i = 0; i < rects.size(); ++i) {
    CHECK_EQ(cutline::render::AngleAtPoint(3, rects[i].x + rects[i].width / 2, rects[i].y + rects[i].height / 2, config), static_cast<int>(i));
  }
  CHECK_EQ(cutline::render::AngleAtPoint(3, 0, 0, config), -1);
  CHECK_EQ(cutline::render::AngleAtPoint(3, rects[2].x + rects[2].width + rects[2].width / 2, rects[2].y + 5, config), -1);

  const auto red = SolidFrame(1.0f, 0.0f, 0.0f, 1.0f, 64, 32);   // wide: leaves bars above and below in a 16:9 cell... a 2:1 picture in a near-16:9 cell
  const auto green = SolidFrame(0.0f, 1.0f, 0.0f, 1.0f, 40, 40);  // square: leaves bars at the sides
  std::vector<cutline::render::MonitorTile> tiles{{&red, false}, {&green, true}, {nullptr, false}};
  const auto output = cutline::render::ComposeAngleMonitor(tiles, config);
  CHECK(output.width() == 320 && output.height() == 180 && output.format() == cutline::media::PixelFormat::RgbaF32);
  const auto centre = [&](const cutline::render::TileRect& r) { return At(output, r.x + r.width / 2, r.y + r.height / 2); };
  CHECK(Near(centre(rects[0]).r, 1.0f) && Near(centre(rects[0]).g, 0.0f));
  CHECK(Near(centre(rects[1]).g, 1.0f) && Near(centre(rects[1]).r, 0.0f));
  // The live angle has a border in the marker colour just inside its cell; the others do not.
  CHECK(Near(At(output, rects[1].x + 1, rects[1].y + rects[1].height / 2).r, config.active_border[0]));
  CHECK(!Near(At(output, rects[0].x + 1, rects[0].y + rects[0].height / 2).r, config.active_border[0], 0.05f));
  // The square picture keeps its shape: the cell's left edge inside the border is the empty colour, not the picture.
  CHECK(Near(At(output, rects[1].x + config.border + 1, rects[1].y + rects[1].height / 2).g, config.empty[1]));
  // An angle with no picture is shown as an empty tile, not as the one before it.
  CHECK(Near(centre(rects[2]).r, config.empty[0]) && Near(centre(rects[2]).g, config.empty[1]));
  // Reduction averages: a half-and-half picture reads as grey in a small tile.
  auto split = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, 64, 32);
  for (int y = 0; y < 32; ++y) for (int x = 0; x < 31; ++x) SetPixel(split, x, y, 1.0f, 1.0f, 1.0f);
  cutline::render::AngleMonitorConfig tiny;
  tiny.width = 40;
  tiny.height = 24;
  tiny.gap = 2;
  const auto small = cutline::render::ComposeAngleMonitor({{&split, false}}, tiny);
  const auto middle = At(small, 19, 12);
  CHECK(middle.r > 0.2f && middle.r < 0.8f);
  // Bad configuration is refused.
  cutline::render::AngleMonitorConfig bad;
  bad.width = 8;
  CHECK_THROWS(cutline::render::AngleMonitorLayout(2, bad));
  CHECK(cutline::render::AngleMonitorLayout(0, config).empty());
}

// ----------------------------------------------------- graphic animation and layout ----

namespace {

namespace gfx = cutline::render::graphics;

gfx::Element PlainBox(const std::string& id, double x, double y, double w, double h) {
  gfx::Element box;
  box.id = id;
  box.type = gfx::ElementType::Rectangle;
  box.x = x;
  box.y = y;
  box.width = w;
  box.height = h;
  box.fill = {1.0, 0.0, 0.0, 1.0};
  return box;
}

}  // namespace

CUTLINE_TEST(AnimatedGraphicPropertiesFollowTheirKeysAndEachInterpolationKeepsItsEndpoints) {
  gfx::Animation linear{"x", "linear", {{1.0, 0.2}, {3.0, 0.6}}};
  CHECK(Near(static_cast<float>(gfx::Evaluate(linear, 0.0)), 0.2f));    // before the first key: the first value
  CHECK(Near(static_cast<float>(gfx::Evaluate(linear, 1.0)), 0.2f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(linear, 2.0)), 0.4f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(linear, 3.0)), 0.6f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(linear, 9.0)), 0.6f));    // after the last: the last value
  gfx::Animation hold{"x", "hold", {{0.0, 0.0}, {2.0, 1.0}}};
  CHECK(Near(static_cast<float>(gfx::Evaluate(hold, 1.99)), 0.0f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(hold, 2.0)), 1.0f));
  gfx::Animation ease_in{"x", "ease_in", {{0.0, 0.0}, {2.0, 1.0}}};
  gfx::Animation ease_out{"x", "ease_out", {{0.0, 0.0}, {2.0, 1.0}}};
  gfx::Animation smooth{"x", "ease_in_out", {{0.0, 0.0}, {2.0, 1.0}}};
  CHECK(Near(static_cast<float>(gfx::Evaluate(ease_in, 1.0)), 0.25f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(ease_out, 1.0)), 0.75f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(smooth, 1.0)), 0.5f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(smooth, 0.0)), 0.0f) && Near(static_cast<float>(gfx::Evaluate(smooth, 2.0)), 1.0f));

  // On a drawn element: a box slides, fades and changes colour; opacity and colour are held to their ranges.
  gfx::Document document;
  document.width = 100;
  document.height = 100;
  auto box = PlainBox("box", 0.0, 0.4, 0.2, 0.2);
  box.animations = {{"x", "linear", {{0.0, 0.0}, {2.0, 0.8}}}, {"opacity", "linear", {{0.0, 1.0}, {2.0, 3.0}}}, {"fill_g", "linear", {{0.0, 0.0}, {2.0, 1.0}}}};
  document.elements.push_back(box);
  cutline::render::Layer layer;
  layer.Reset(100, 100);
  (void)gfx::Draw(layer, document, {}, 1.0);
  CHECK(Near(layer.at(50, 50).r, 1.0f) && Near(layer.at(50, 50).g, 0.5f));    // x = 0.4: columns 40 to 59; half way to green
  CHECK(layer.at(10, 50).a == 0.0f);
  layer.Reset(100, 100);
  (void)gfx::Draw(layer, document, {}, 0.0);
  CHECK(layer.at(10, 50).a > 0.99f && layer.at(50, 50).a == 0.0f);

  // The document round trips with its animation, and is written as the newer schema only because it needs it.
  const auto encoded = gfx::ToJson(document);
  CHECK(encoded.find("\"schema_version\":2") != std::string::npos);
  const auto decoded = gfx::ParseDocument(encoded);
  CHECK_EQ(decoded.elements[0].animations.size(), std::size_t{3});
  CHECK(Near(static_cast<float>(gfx::Evaluate(decoded.elements[0].animations[0], 1.0)), 0.4f));
  gfx::Document plain;
  plain.elements.push_back(PlainBox("a", 0, 0, 1, 1));
  CHECK(gfx::ToJson(plain).find("\"schema_version\":1") != std::string::npos);
  CHECK(gfx::ToJson(plain).find("animations") == std::string::npos);
  // A build that does not know a newer schema must refuse it rather than draw it without its animation.
  auto future = encoded;
  future.replace(future.find("\"schema_version\":2"), 18, "\"schema_version\":4");
  CHECK_THROWS(gfx::ParseDocument(future));

  // What cannot be drawn is refused.
  const auto refused = [&](auto change) {
    auto bad = decoded;
    change(bad.elements[0]);
    return [&] {
      try {
        gfx::Validate(bad);
      } catch (const std::invalid_argument&) {
        return true;
      }
      return false;
    }();
  };
  CHECK(refused([](gfx::Element& e) { e.animations[0].property = "hue"; }));
  CHECK(refused([](gfx::Element& e) { e.animations[1].property = "x"; }));    // twice
  CHECK(refused([](gfx::Element& e) { e.animations[0].keys = {{1.0, 0.0}, {1.0, 1.0}}; }));   // not increasing
  CHECK(refused([](gfx::Element& e) { e.animations[0].keys = {{-1.0, 0.0}}; }));
  CHECK(refused([](gfx::Element& e) { e.animations[0].keys.clear(); }));
  CHECK(refused([](gfx::Element& e) { e.animations[0].keys[0].value = std::nan(""); }));
  CHECK(refused([](gfx::Element& e) { e.animations[0].interpolation = "bounce"; }));
  CHECK(refused([](gfx::Element& e) { e.anchor_x = "top"; }));
  CHECK(refused([](gfx::Element& e) { e.anchor_y = "left"; }));
  CHECK(!refused([](gfx::Element&) {}));
}

CUTLINE_TEST(AnchoredGraphicElementsKeepTheirMarginsAndSizeWhenThePictureChangesShape) {
  gfx::Document document;
  document.width = 1920;
  document.height = 1080;
  // A lower third: 5% in from the left, its bottom 10% up from the bottom edge, 40% of the width, 10% of the height.
  auto lower = PlainBox("lower", 0.05, 0.8, 0.4, 0.1);
  lower.anchor_x = "left";
  lower.anchor_y = "bottom";
  auto centred = PlainBox("centred", 0.4, 0.45, 0.2, 0.1);
  centred.anchor_x = "center";
  centred.anchor_y = "middle";
  auto bar = PlainBox("bar", 0.1, 0.0, 0.8, 0.05);
  bar.anchor_x = "stretch";
  bar.anchor_y = "top";
  auto free = PlainBox("free", 0.1, 0.1, 0.3, 0.3);
  document.elements = {lower, centred, bar, free};

  // On the shape it was designed for the anchors change nothing.
  for (const auto& element : document.elements) {
    const auto same = gfx::Place(document, element, 1920, 1080);
    auto unanchored = element;
    unanchored.anchor_x = "none";
    unanchored.anchor_y = "none";
    const auto plain = gfx::Place(document, unanchored, 1920, 1080);
    CHECK(same.x == plain.x && same.y == plain.y && same.width == plain.width && same.height == plain.height);
  }

  // On a tall picture (1080 by 1920) the document is fitted inside it: everything is 1080/1920 of its size.
  const double s = 1080.0 / 1920.0;
  const auto tall = gfx::Place(document, lower, 1080, 1920);
  CHECK(std::abs(tall.width - 0.4 * 1920 * s) <= 1.0);                       // 432
  CHECK(std::abs(tall.height - 0.1 * 1080 * s) <= 1.0);                      // 60.75
  CHECK(std::abs(tall.x - 0.05 * 1920 * s) <= 1.0);                          // the same margin from the left, scaled: 54
  CHECK(std::abs((1920 - tall.y - tall.height) - 0.1 * 1080 * s) <= 1.5);    // and from the bottom: 60.75
  const auto mid = gfx::Place(document, centred, 1080, 1920);
  CHECK(std::abs((mid.x + mid.width / 2.0) - 540.0) <= 1.0 && std::abs((mid.y + mid.height / 2.0) - 960.0) <= 1.0);
  const auto stretched = gfx::Place(document, bar, 1080, 1920);
  CHECK(std::abs(stretched.x - 0.1 * 1920 * s) <= 1.0);                      // left margin kept
  CHECK(std::abs((1080 - stretched.x - stretched.width) - 0.1 * 1920 * s) <= 1.5);   // right margin kept, width flexes
  CHECK(stretched.y <= 1);
  // Not anchored: it simply scales with the picture, so it is stretched taller.
  const auto loose = gfx::Place(document, document.elements[3], 1080, 1920);
  CHECK(loose.x == 108 && loose.y == 192 && loose.width == 324 && loose.height == 576);
  // Text size follows the same fit when anchored, and the height of the picture when not.
  auto words = lower;
  words.font_size = 0.1;
  CHECK(std::abs(gfx::Place(document, words, 1080, 1920).font_pixels - 0.1 * 1080 * s) < 1e-6);
  words.anchor_x = "none";
  words.anchor_y = "none";
  CHECK(std::abs(gfx::Place(document, words, 1080, 1920).font_pixels - 0.1 * 1920) < 1e-6);

  // And it draws there.
  cutline::render::Layer layer;
  layer.Reset(108, 192);
  gfx::Document one;
  one.elements = {lower};
  (void)gfx::Draw(layer, one);
  CHECK(layer.at(30, 183).a > 0.99f);   // inside the lower third: columns 5 to 47, rows 180 to 185
  CHECK(layer.at(30, 120).a == 0.0f);   // the loose design would have put it higher
}

CUTLINE_TEST(TemplateControlsTakeColoursAndRangesAndFontListsFallBackToWhatIsInstalled) {
  gfx::TemplatePackage package;
  package.id = "lower-third";
  package.version = 2;
  package.document.elements.push_back(PlainBox("bar", 0.0, 0.0, 1.0, 1.0));
  gfx::Control colour{"colour", "bar", "fill", "0,1,0"};
  gfx::Control opacity{"opacity", "bar", "opacity", "1"};
  opacity.minimum = 0.0;
  opacity.maximum = 1.0;
  package.controls = {colour, opacity};
  const auto round_trip = gfx::ParseTemplate(gfx::ToJson(package));
  CHECK(round_trip.controls[1].maximum == 1.0);
  const auto green = gfx::Instantiate(round_trip, {{"colour", "0,1,0"}});
  CHECK(green.elements[0].fill[0] == 0.0 && green.elements[0].fill[1] == 1.0);
  const auto hex = gfx::Instantiate(round_trip, {{"colour", "#0080FF80"}});
  CHECK(std::abs(hex.elements[0].fill[1] - 128.0 / 255.0) < 1e-9 && std::abs(hex.elements[0].fill[3] - 128.0 / 255.0) < 1e-9);
  CHECK_THROWS(gfx::Instantiate(round_trip, {{"colour", "2,0,0"}}));       // outside 0..1
  CHECK_THROWS(gfx::Instantiate(round_trip, {{"colour", "red"}}));
  CHECK_THROWS(gfx::Instantiate(round_trip, {{"colour", "#12345"}}));
  CHECK_THROWS(gfx::Instantiate(round_trip, {{"opacity", "1.5"}}));        // outside the control's range
  CHECK_THROWS(gfx::Instantiate(round_trip, {{"opacity", "abc"}}));
  // A package whose control names a missing element, or a property it cannot set, is refused when read.
  auto broken = package;
  broken.controls[0].element_id = "nothing";
  CHECK_THROWS(gfx::ParseTemplate(gfx::ToJson(broken)));
  broken = package;
  broken.controls[0].property = "bold";
  CHECK_THROWS(gfx::ParseTemplate(gfx::ToJson(broken)));

  // The bundle the project stores resolves to the same document the template gives.
  const auto bundle = gfx::MakeTemplateBundle(gfx::ToJson(package), {{"colour", "0,1,0"}});
  CHECK(gfx::ResolveBundle(bundle).elements[0].fill[1] == 1.0);
  CHECK(gfx::ResolveBundle(gfx::MakeGraphicBundle(gfx::ToJson(green))).elements.size() == 1);
  CHECK_THROWS(gfx::ResolveBundle("{\"kind\":\"movie\"}"));
  CHECK_THROWS(gfx::ResolveBundle("[]"));

  // Fonts: a list is satisfied by any installed family, and only a list none of which is installed is missing.
  gfx::Document words;
  auto title = PlainBox("t", 0, 0, 1, 1);
  title.type = gfx::ElementType::Text;
  title.text = "Hello";
  title.font = "No Such Font One, No Such Font Two";
  words.elements = {title};
  CHECK_EQ(gfx::ResolveFontFamily(title.font), std::string("No Such Font One"));   // nothing installed: the first, so the warning names it
  if (cutline::render::text::Available() && cutline::render::text::HasFamily("Arial")) {
    CHECK_EQ(gfx::MissingFonts(words).size(), std::size_t{1});
    title.font = "No Such Font One, Arial";
    words.elements = {title};
    CHECK_EQ(gfx::ResolveFontFamily(title.font), std::string("Arial"));
    CHECK(gfx::MissingFonts(words).empty());
    // Drawing a missing font says so, and still draws something.
    title.font = "No Such Font One";
    words.elements = {title};
    cutline::render::Layer layer;
    layer.Reset(200, 100);
    const auto drawn = gfx::Draw(layer, words);
    CHECK(drawn.elements_drawn == 1);
    bool warned = false;
    for (const auto& warning : drawn.warnings) warned = warned || warning.find("No Such Font One") != std::string::npos;
    CHECK(warned);
  }
}

// ------------------------------------------------------------- optical-flow cache ----

namespace {

using cutline::render::FlowCache;
using cutline::render::FlowCacheConfig;
using cutline::render::FlowSource;

// A textured picture that has moved `shift` pixels to the right.
VideoFrame MovingTexture(int shift, int width = 40, int height = 24) {
  auto frame = SolidFrame(0.0f, 0.0f, 0.0f, 1.0f, width, height);
  for (int y = 2; y < height - 2; ++y) {
    for (int x = 2; x < width - 2; ++x) {
      const int u = x - shift;
      const auto value = static_cast<float>((u * 17 + y * 31 + u * y * 3) % 101) / 100.0f;
      SetPixel(frame, x, y, value, value, value);
    }
  }
  return frame;
}

std::filesystem::path FlowDirectory(const std::string& name) {
  const auto directory = std::filesystem::temp_directory_path() / ("cutline-flow-" + name);
  std::filesystem::remove_all(directory);
  return directory;
}

cutline::render::OpticalFlowConfig FastFlow() {
  cutline::render::OpticalFlowConfig config;
  config.search_radius = 5;
  return config;
}

}  // namespace

CUTLINE_TEST(AFlowKeyIsTheContentOfTheTwoPicturesAndTheSettingsAndNothingElse) {
  const auto a = MovingTexture(0);
  const auto b = MovingTexture(3);
  const auto config = FastFlow();
  const auto key = cutline::render::MakeFlowKey(a, b, config);
  CHECK_EQ(key.size(), std::size_t{32});
  CHECK_EQ(cutline::render::MakeFlowKey(a.Clone(), b.Clone(), config), key);                 // the same pictures, wherever they are held
  CHECK(cutline::render::MakeFlowKey(b, a, config) != key);                                  // direction matters
  auto touched = a.Clone();
  SetPixel(touched, 10, 10, 0.123f, 0.5f, 0.5f);
  CHECK(cutline::render::MakeFlowKey(touched, b, config) != key);                            // one pixel
  auto finer = config;
  finer.block_size = 4;
  CHECK(cutline::render::MakeFlowKey(a, b, finer) != key);                                   // another setting
  auto cancelling = config;
  cancelling.cancel = []() { return false; };
  CHECK_EQ(cutline::render::MakeFlowKey(a, b, cancelling), key);                             // a cancel check is not part of the answer
  // The same pixels held as 8-bit read as the same pictures once converted to float, so they share entries.
  const auto a8 = cutline::media::ConvertFrame(a, cutline::media::PixelFormat::Rgba8);
  const auto b8 = cutline::media::ConvertFrame(b, cutline::media::PixelFormat::Rgba8);
  CHECK_EQ(cutline::render::MakeFlowKey(a8, b8, config), cutline::render::MakeFlowKey(cutline::media::ConvertFrame(a8, cutline::media::PixelFormat::RgbaF32),
                                                                                      cutline::media::ConvertFrame(b8, cutline::media::PixelFormat::RgbaF32), config));
}

CUTLINE_TEST(AnAnalysisIsDoneOnceThenFoundInMemoryThenOnDiskAndADamagedFileIsNotBelieved) {
  const auto directory = FlowDirectory("tiers");
  const auto a = MovingTexture(0);
  const auto b = MovingTexture(3);
  const auto config = FastFlow();
  FlowCacheConfig cache_config;
  cache_config.directory = directory;
  {
    FlowCache cache(cache_config);
    const auto first = cutline::render::EstimateOpticalFlowCached(cache, a, b, config);
    CHECK(first.source == FlowSource::Computed);
    // What was cached is what the estimator gives.
    const auto direct = cutline::render::EstimateOpticalFlow(a, b, config);
    CHECK_EQ(first.field->samples.size(), direct.samples.size());
    for (std::size_t i = 0; i < direct.samples.size(); ++i) {
      CHECK(first.field->samples[i].dx == direct.samples[i].dx && first.field->samples[i].dy == direct.samples[i].dy);
      CHECK(first.field->samples[i].occluded == direct.samples[i].occluded);
    }
    CHECK(std::abs(first.field->at(2, 1).dx - 3.0f) < 0.1f);
    const auto again = cutline::render::EstimateOpticalFlowCached(cache, a, b, config);
    CHECK(again.source == FlowSource::Memory && again.field == first.field);
    const auto stats = cache.Stats();
    CHECK_EQ(stats.computed, std::uint64_t{1});
    CHECK_EQ(stats.memory_hits, std::uint64_t{1});
    CHECK_EQ(stats.disk_writes, std::uint64_t{1});
  }
  {
    // A later run, a new process: the disk has it, and nothing is computed.
    FlowCache cache(cache_config);
    const auto found = cutline::render::EstimateOpticalFlowCached(cache, a, b, config);
    CHECK(found.source == FlowSource::Disk);
    CHECK(std::abs(found.field->at(2, 1).dx - 3.0f) < 0.1f);
    CHECK_EQ(cache.Stats().computed, std::uint64_t{0});
    CHECK_EQ(cache.Stats().disk_hits, std::uint64_t{1});
  }
  // Damage the file: a flipped byte, then a short file. Each is refused, removed and recomputed.
  const auto key = cutline::render::MakeFlowKey(a, b, config);
  const auto path = directory / (key + ".cutflow");
  CHECK(std::filesystem::exists(path));
  for (const auto damage : {0, 1}) {
    std::vector<char> bytes;
    {
      std::ifstream in(path, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (damage == 0) {
      bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x55);
    } else {
      bytes.resize(bytes.size() / 2);
    }
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    FlowCache cache(cache_config);
    const auto recovered = cutline::render::EstimateOpticalFlowCached(cache, a, b, config);
    CHECK(recovered.source == FlowSource::Computed);
    CHECK(std::abs(recovered.field->at(2, 1).dx - 3.0f) < 0.1f);
    CHECK_EQ(cache.Stats().disk_rejected, std::uint64_t{1});
    CHECK(std::filesystem::exists(path));   // and a good copy has been written back
  }
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(TheFlowCacheKeepsToItsMemoryBudgetAndNeverComputesAPairTwiceAtOnce) {
  // Budget for about two fields: the third pair pushes the oldest out of memory, and it is still on disk.
  const auto directory = FlowDirectory("budget");
  const auto config = FastFlow();
  const auto first = cutline::render::EstimateOpticalFlow(MovingTexture(0), MovingTexture(1), config);
  FlowCacheConfig cache_config;
  cache_config.directory = directory;
  cache_config.memory_bytes = 2 * (sizeof(cutline::render::FlowField) + first.samples.size() * sizeof(cutline::render::FlowSample)) + 8;
  FlowCache cache(cache_config);
  std::vector<std::string> keys;
  for (int shift = 0; shift < 3; ++shift) {
    const auto a = MovingTexture(shift);
    const auto b = MovingTexture(shift + 1);
    keys.push_back(cutline::render::MakeFlowKey(a, b, config));
    (void)cutline::render::EstimateOpticalFlowCached(cache, a, b, config);
  }
  CHECK_EQ(cache.Stats().entries, std::size_t{2});
  CHECK_EQ(cache.Stats().evictions, std::uint64_t{1});
  CHECK(cache.Find(keys[2]) != nullptr && cache.Find(keys[1]) != nullptr);
  const auto before = cache.Stats().disk_hits;
  CHECK(cache.Find(keys[0]) != nullptr);                       // out of memory, back from disk
  CHECK_EQ(cache.Stats().disk_hits, before + 1);
  CHECK(cache.Find("00000000000000000000000000000000") == nullptr);
  cache.Clear();
  CHECK_EQ(cache.Stats().entries, std::size_t{0});
  CHECK(cache.Find(keys[0]) == nullptr);                       // cleared on disk too

  // Several threads asking for one pair at once: one computes, the rest wait for it.
  FlowCache shared;
  std::atomic<int> computations{0};
  const auto a = MovingTexture(0);
  const auto b = MovingTexture(2);
  const auto key = cutline::render::MakeFlowKey(a, b, config);
  std::vector<std::thread> threads;
  std::vector<std::shared_ptr<const cutline::render::FlowField>> results(6);
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back([&, i]() {
      results[static_cast<std::size_t>(i)] = shared.GetOrCompute(key, [&]() {
        ++computations;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return cutline::render::EstimateOpticalFlow(a, b, config);
      }).field;
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK_EQ(computations.load(), 1);
  for (const auto& result : results) CHECK(result == results[0] && result != nullptr);

  // A computation that fails leaves nothing behind and the next caller can try again.
  CHECK_THROWS(shared.GetOrCompute("ffffffffffffffffffffffffffffffff", []() -> cutline::render::FlowField { throw std::runtime_error("no"); }));
  CHECK(shared.GetOrCompute("ffffffffffffffffffffffffffffffff", [&]() { return cutline::render::EstimateOpticalFlow(a, b, config); }).field != nullptr);
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(SharedQuotaEvictsCheapRenderFramesBeforeExpensiveFlowAnalysis) {
  const auto flow = std::make_shared<const cutline::render::FlowField>(
      cutline::render::EstimateOpticalFlow(MovingTexture(0), MovingTexture(1), FastFlow()));
  const auto flow_bytes = sizeof(cutline::render::FlowField) +
                          flow->samples.size() * sizeof(cutline::render::FlowSample);
  const auto frame = SolidFrame(0.2f, 0.3f, 0.4f, 1.0f, 16, 16);
  cutline::resource::QuotaConfig quota_config;
  quota_config.ram_bytes = flow_bytes + frame.size_bytes() - 1;
  auto quota = std::make_shared<cutline::resource::QuotaManager>(quota_config);

  FlowCacheConfig flow_config;
  flow_config.memory_bytes = flow_bytes * 2;
  flow_config.quota_manager = quota;
  FlowCache flow_cache(flow_config);
  flow_cache.Put("flow", flow);

  cutline::render::RenderCacheConfig render_config;
  render_config.memory_bytes = frame.size_bytes() * 2;
  render_config.quota_manager = quota;
  cutline::render::RenderCache render_cache(render_config);
  render_cache.Store("frame", frame);

  CHECK(!render_cache.Contains("frame"));
  CHECK(flow_cache.Find("flow") != nullptr);
  CHECK_EQ(render_cache.statistics().shared_quota_evictions, std::int64_t{1});
  CHECK(quota->statistics().ram_bytes <= quota_config.ram_bytes);
}

CUTLINE_TEST(ResourcePressureShrinksEveryBudgetAndRunsCallbacksOutsideTheManagerLock) {
  cutline::resource::QuotaConfig config;
  config.ram_bytes = 100;
  config.vram_bytes = 100;
  config.disk_bytes = 100;
  cutline::resource::QuotaManager quota(config);
  bool cheap_evicted = false;
  bool expensive_evicted = false;
  quota.Track("render", "cheap", cutline::resource::Tier::Ram, 60, 1.0,
              [&] { cheap_evicted = true; });
  quota.Track("flow", "expensive", cutline::resource::Tier::Ram, 60, 100.0,
              [&] { expensive_evicted = true; });
  quota.Enforce(cutline::resource::Tier::Ram);
  CHECK(cheap_evicted);
  CHECK(!expensive_evicted);
  CHECK_EQ(quota.statistics().ram_bytes, std::size_t{60});

  quota.SetPressure(cutline::resource::Pressure::Critical);
  CHECK(expensive_evicted);
  CHECK_EQ(quota.statistics().ram_bytes, std::size_t{0});
  CHECK_EQ(quota.statistics().evictions, std::uint64_t{2});
}

CUTLINE_TEST(TheFlowCacheDiskTierStaysWithinItsBudgetKeepingTheNewest) {
  const auto directory = FlowDirectory("trim");
  FlowCacheConfig cache_config;
  cache_config.directory = directory;
  cache_config.disk_bytes = 1000;
  std::vector<std::string> keys;
  {
    FlowCache cache(cache_config);
    for (int i = 0; i < 70; ++i) {
      cutline::render::FlowField field;
      field.width = 8;
      field.height = 8;
      field.block_size = 8;
      field.columns = 1;
      field.rows = 1;
      field.samples = {{static_cast<float>(i), 0.0f, 1.0f, false}};
      char name[40];
      std::snprintf(name, sizeof(name), "%032d", i);
      keys.push_back(name);
      cache.Put(keys.back(), std::make_shared<const cutline::render::FlowField>(std::move(field)));
      // Distinct modification times, so "oldest" is well defined.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  std::uint64_t total = 0;
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    total += entry.file_size();
    ++files;
  }
  // The trim runs every 64 writes: at that point the oldest went, and six more files were written after it.
  CHECK(files < 70);
  CHECK(total <= 1000 + 6 * 64);
  FlowCache reopened(cache_config);
  CHECK(reopened.Find(keys.back()) != nullptr);   // the newest survived
  CHECK(reopened.Find(keys.front()) == nullptr);  // the oldest did not
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(ABackgroundAnalysisFillsTheCacheReportsProgressAndCanBeStoppedAndPlaybackFindsTheWorkDone) {
  const auto config = FastFlow();
  FrameSet frames;
  for (int k = 0; k < 6; ++k) frames.Add(MovingTexture(2 * k));
  const auto resolver = frames.Resolver();
  auto cache = std::make_shared<FlowCache>();

  std::vector<std::size_t> reported;
  cutline::render::FlowAnalysis analysis(cache, [&](std::size_t index) { return resolver(index); }, frames.size(), config);
  analysis.Observe([&](const cutline::render::FlowAnalysisProgress& progress) { reported.push_back(progress.done); });
  analysis.Start();
  analysis.Wait();
  auto progress = analysis.Progress();
  CHECK(progress.finished && !progress.cancelled && progress.error.empty());
  CHECK_EQ(progress.total_pairs, std::size_t{5});
  CHECK_EQ(progress.done, std::size_t{5});
  CHECK_EQ(progress.computed, std::size_t{5});
  CHECK_EQ(progress.reused, std::size_t{0});
  CHECK(reported.size() >= 5 && reported.back() == 5);
  for (std::size_t i = 1; i < reported.size(); ++i) CHECK(reported[i] >= reported[i - 1]);

  // Playback now finds each pair already analysed.
  for (std::size_t k = 0; k + 1 < frames.size(); ++k) {
    CHECK(cutline::render::EstimateOpticalFlowCached(*cache, frames.at(k), frames.at(k + 1), config).source != FlowSource::Computed);
  }
  // A second pass over the same footage computes nothing.
  cutline::render::FlowAnalysis again(cache, [&](std::size_t index) { return resolver(index); }, frames.size(), config);
  again.Start();
  again.Wait();
  CHECK_EQ(again.Progress().reused, std::size_t{5});
  CHECK_EQ(again.Progress().computed, std::size_t{0});

  // A frame that cannot be had, and one of another size, skip their two pairs and no more.
  const auto odd = MovingTexture(0, 48, 24);
  auto cache_two = std::make_shared<FlowCache>();
  cutline::render::FlowAnalysis gaps(cache_two, [&](std::size_t index) -> const VideoFrame* {
    if (index == 2) return nullptr;
    if (index == 4) return &odd;
    return &frames.at(index);
  }, frames.size(), config);
  gaps.Start();
  gaps.Wait();
  CHECK_EQ(gaps.Progress().skipped, std::size_t{4});     // pairs 1-2, 2-3, 3-4 and 4-5
  CHECK_EQ(gaps.Progress().computed, std::size_t{1});    // 0-1

  // Stopping: the job halts between pairs, says it was cancelled, and what it had done stays in the cache.
  auto cache_three = std::make_shared<FlowCache>();
  FrameSet long_run;
  for (int k = 0; k < 40; ++k) long_run.Add(MovingTexture(k % 7, 96, 64));
  const auto long_resolver = long_run.Resolver();
  cutline::render::FlowAnalysis stopping(cache_three, [&](std::size_t index) { return long_resolver(index); }, long_run.size(), config);
  stopping.Observe([&](const cutline::render::FlowAnalysisProgress& p) {
    if (p.done >= 2) stopping.Cancel();
  });
  stopping.Start();
  stopping.Wait();
  const auto stopped = stopping.Progress();
  CHECK(stopped.finished && stopped.cancelled);
  CHECK(stopped.done >= 2 && stopped.done < 39);
  CHECK_EQ(cache_three->Stats().computed, static_cast<std::uint64_t>(stopped.computed));

  // Destroying a running job stops it, and a job needs a cache and frames.
  {
    cutline::render::FlowAnalysis abandoned(std::make_shared<FlowCache>(), [&](std::size_t index) { return long_resolver(index); }, long_run.size(), config);
    abandoned.Start();
  }
  CHECK_THROWS(cutline::render::FlowAnalysis(nullptr, [](std::size_t) { return static_cast<const VideoFrame*>(nullptr); }, 2));
  CHECK_THROWS(cutline::render::FlowAnalysis(std::make_shared<FlowCache>(), {}, 2));
}

CUTLINE_TEST(TheCompositorReusesAnAnalysisForEveryFractionalFrameBetweenTwoSourceFrames) {
  auto sequence = MakeSequence();
  sequence.width = 40;
  sequence.height = 24;
  auto track = MakeTrack("v1", 0);
  auto clip = MakeClip("clip", "media", 0, 10);
  clip.effects.push_back(MakeEffect("interpolation", "frame_interpolation", {{"mode", Value::Scalar(2.0)}}));
  track.clips.push_back(std::move(clip));
  sequence.tracks.push_back(std::move(track));
  Finalise(sequence);

  auto first = MovingTexture(0);
  first.presentation_time = {0, 1};
  first.duration = {1, 25};
  auto second = MovingTexture(3);
  second.presentation_time = {1, 25};
  second.duration = {1, 25};
  const auto resolver = [&](const SourceRequest& request) -> const VideoFrame* {
    return request.source_time.Compare(RationalTime(1, 25)) < 0 ? &first : &second;
  };
  auto config = FloatOutput();
  config.flow_cache = std::make_shared<FlowCache>();
  const Compositor compositor(config);
  // A quarter, half and three quarters of the way between the two source frames: one analysis, three pictures.
  std::vector<VideoFrame> pictures;
  int reused = 0;
  for (const auto quarter : {1, 2, 3}) {
    Statistics statistics;
    pictures.push_back(compositor.Compose(TimelineCompiler{}.Compile(sequence, RationalTime(quarter, 100)), resolver, statistics));
    CHECK_EQ(statistics.optical_flow_frames, 1);
    reused += statistics.optical_flow_reused;
  }
  CHECK_EQ(reused, 2);
  CHECK_EQ(config.flow_cache->Stats().computed, std::uint64_t{1});
  // And the pictures are the same as without a cache.
  const Compositor uncached(FloatOutput());
  const auto direct = uncached.Compose(TimelineCompiler{}.Compile(sequence, RationalTime(2, 100)), resolver);
  float worst = 0.0f;
  for (int y = 0; y < direct.height(); ++y) {
    for (int x = 0; x < direct.width(); ++x) worst = std::max(worst, std::abs(At(direct, x, y).r - At(pictures[1], x, y).r));
  }
  CHECK(worst == 0.0f);
}

CUTLINE_TEST(RenderGraphEliminatesDeadWorkFusesUnaryNodesAndKeepsStableContentAddresses) {
  namespace graph = cutline::render::graph;
  graph::RenderGraph first{
      {
          {"source", graph::NodeKind::Source, {}, "asset=shot-a;frame=42", {0, 0, 1920, 1080}},
          {"transform", graph::NodeKind::Transform, {"source"}, "x=12;y=4;scale=1", {0, 0, 1920, 1080}},
          {"grade", graph::NodeKind::Color, {"transform"}, "exposure=0.5", {0, 0, 1920, 1080}},
          {"output", graph::NodeKind::Output, {"grade"}, "display=rec709", {0, 0, 1920, 1080}},
          {"dead-blur", graph::NodeKind::Blur, {"source"}, "radius=64", {0, 0, 1920, 1080}},
      },
      {"output"}};

  const auto compiled = graph::Compile(first);
  CHECK_EQ(compiled.statistics.input_nodes, std::size_t{5});
  CHECK_EQ(compiled.statistics.live_nodes, std::size_t{4});
  CHECK_EQ(compiled.statistics.dead_nodes, std::size_t{1});
  CHECK_EQ(compiled.statistics.fused_nodes, std::size_t{1});
  CHECK_EQ(compiled.nodes.size(), std::size_t{3});
  CHECK_EQ(compiled.nodes[1].source_ids.size(), std::size_t{2});
  CHECK_EQ(compiled.nodes[1].operations.size(), std::size_t{2});
  CHECK_EQ(compiled.nodes[1].operations[0].parameters, std::string{"x=12;y=4;scale=1"});
  CHECK_EQ(compiled.nodes[1].operations[1].parameters, std::string{"exposure=0.5"});
  CHECK(compiled.hashes.find("dead-blur") == compiled.hashes.end());

  auto changed = first;
  changed.nodes[2].parameters = "exposure=0.75";
  const auto recompiled = graph::Compile(changed);
  CHECK_EQ(compiled.hashes.at("source"), recompiled.hashes.at("source"));
  CHECK_EQ(compiled.hashes.at("transform"), recompiled.hashes.at("transform"));
  CHECK(compiled.hashes.at("grade") != recompiled.hashes.at("grade"));
  CHECK(compiled.output_hashes.front() != recompiled.output_hashes.front());
}

CUTLINE_TEST(RenderGraphInvalidatesOnlyDependentBranchesAndRejectsInvalidGraphs) {
  namespace graph = cutline::render::graph;
  graph::RenderGraph graph_with_branches{
      {
          {"source", graph::NodeKind::Source, {}, "asset=shot-a;frame=7", {0, 0, 1280, 720}},
          {"left", graph::NodeKind::Color, {"source"}, "temperature=4", {0, 0, 640, 720}},
          {"right", graph::NodeKind::Color, {"source"}, "tint=2", {640, 0, 640, 720}},
          {"blend", graph::NodeKind::Blend, {"left", "right"}, "mode=screen", {0, 0, 1280, 720}},
          {"output", graph::NodeKind::Output, {"blend"}, "", {0, 0, 1280, 720}},
      },
      {"output"}};
  const auto before = graph::Compile(graph_with_branches);
  graph_with_branches.nodes[1].parameters = "temperature=8";
  const auto after = graph::Compile(graph_with_branches);
  CHECK(before.hashes.at("left") != after.hashes.at("left"));
  CHECK_EQ(before.hashes.at("right"), after.hashes.at("right"));
  CHECK(before.hashes.at("blend") != after.hashes.at("blend"));

  graph::RenderGraph missing{{{"output", graph::NodeKind::Output, {"missing"}, "", {}}}, {"output"}};
  CHECK_THROWS(graph::Compile(missing));
  graph::RenderGraph cyclic{{
      {"a", graph::NodeKind::Filter, {"b"}, "", {}},
      {"b", graph::NodeKind::Filter, {"a"}, "", {}},
  }, {"a"}};
  CHECK_THROWS(graph::Compile(cyclic));
}

CUTLINE_TEST(GpuSelectionIsCapabilityBasedAcrossAmdNvidiaIntelAndApple) {
  namespace gpu = cutline::render::gpu;
  constexpr auto gib = std::size_t{1024} * 1024 * 1024;
  const auto common = gpu::Capability::Compositor | gpu::Capability::Float16;
  const std::vector<gpu::DeviceDescriptor> devices{
      {"amd", "Radeon", gpu::Backend::D3D12, gpu::Vendor::Amd,
       common | gpu::Flag(gpu::Capability::TemporalNoiseReduction), 8 * gib},
      {"nvidia", "GeForce", gpu::Backend::Vulkan, gpu::Vendor::Nvidia,
       common | gpu::Flag(gpu::Capability::OpticalFlow), 8 * gib},
      {"intel", "Arc", gpu::Backend::D3D12, gpu::Vendor::Intel, common, 12 * gib},
      {"apple", "Apple GPU", gpu::Backend::Metal, gpu::Vendor::Apple,
       common | gpu::Flag(gpu::Capability::OpticalFlow) | gpu::Flag(gpu::Capability::TemporalNoiseReduction),
       0, 16 * gib, true},
  };

  gpu::DeviceRequirements optical_flow;
  optical_flow.required = gpu::Flag(gpu::Capability::Compositor) | gpu::Flag(gpu::Capability::OpticalFlow);
  optical_flow.preferred = gpu::Flag(gpu::Capability::TemporalNoiseReduction);
  const auto selected = gpu::SelectDevice(devices, optical_flow);
  CHECK(selected.has_value());
  CHECK(selected->device.vendor == gpu::Vendor::Apple);
  CHECK(!selected->used_cpu_fallback);

  gpu::DeviceRequirements noise_reduction;
  noise_reduction.required = gpu::Flag(gpu::Capability::Compositor) |
                             gpu::Flag(gpu::Capability::TemporalNoiseReduction);
  noise_reduction.preferred = gpu::Flag(gpu::Capability::OpticalFlow);
  const auto selected_for_nr = gpu::SelectDevice(devices, noise_reduction);
  CHECK(selected_for_nr.has_value());
  CHECK_EQ(selected_for_nr->device.id, std::string{"apple"});
}

CUTLINE_TEST(GpuSelectionUsesCpuForSupportedWorkAndRefusesImpossibleZeroCopy) {
  namespace gpu = cutline::render::gpu;
  const std::vector<gpu::DeviceDescriptor> unavailable{
      {"disabled", "Disabled GPU", gpu::Backend::D3D12, gpu::Vendor::Amd,
       gpu::Flag(gpu::Capability::Compositor), 0, 0, false, false}};
  const auto fallback = gpu::SelectDevice(unavailable);
  CHECK(fallback.has_value());
  CHECK(fallback->used_cpu_fallback);
  CHECK(fallback->device.backend == gpu::Backend::Cpu);

  gpu::DeviceRequirements zero_copy;
  zero_copy.required = gpu::Flag(gpu::Capability::Compositor) | gpu::Flag(gpu::Capability::ZeroCopyDecode);
  CHECK(!gpu::SelectDevice(unavailable, zero_copy).has_value());
  zero_copy.allow_cpu_fallback = false;
  CHECK(!gpu::SelectDevice(std::span<const gpu::DeviceDescriptor>{}, zero_copy).has_value());
}

CUTLINE_TEST(PlaybackPlansLowerToAStableRenderGraphWithSourceVersionInvalidation) {
  namespace graph = cutline::render::graph;
  cutline::timeline::PlaybackPlan plan;
  plan.width = 1920;
  plan.height = 1080;
  plan.working_color_space = "rec709";
  plan.display_color_space = "srgb";
  cutline::timeline::SourceRequest lower;
  lower.clip_id = "lower";
  lower.track_id = "v1";
  lower.source_id = "media-a";
  lower.track_order = 0;
  lower.effects.push_back({"grade-a", "grade", "", "", 0, false,
                           {{"exposure", Value::Scalar(0.5)}}, {}});
  cutline::timeline::SourceRequest upper;
  upper.clip_id = "upper";
  upper.track_id = "v2";
  upper.source_id = "media-b";
  upper.track_order = 1;
  upper.effects.push_back({"opacity-b", "opacity", "", "", 0, false,
                           {{"value", Value::Scalar(0.75)}}, {}});
  plan.video = {lower, upper};

  graph::BuildOptions options;
  options.source_versions = {{"media-a", "sha-a1"}, {"media-b", "sha-b1"}};
  const auto first = graph::Compile(graph::Build(plan, options));
  options.source_versions["media-a"] = "sha-a2";
  const auto source_changed = graph::Compile(graph::Build(plan, options));
  CHECK(first.hashes.at("layer/0/source") != source_changed.hashes.at("layer/0/source"));
  CHECK_EQ(first.hashes.at("layer/1/source"), source_changed.hashes.at("layer/1/source"));
  CHECK(first.output_hashes.front() != source_changed.output_hashes.front());

  plan.video[0].effects[0].parameters[0].value = Value::Scalar(1.0);
  const auto effect_changed = graph::Compile(graph::Build(plan, options));
  CHECK_EQ(source_changed.hashes.at("layer/0/source"), effect_changed.hashes.at("layer/0/source"));
  CHECK(source_changed.hashes.at("layer/0/effect/0") != effect_changed.hashes.at("layer/0/effect/0"));
  CHECK_EQ(source_changed.hashes.at("layer/1/effect/0"), effect_changed.hashes.at("layer/1/effect/0"));
}

// ------------------------------------------- graphic rotation, outlines, shadows and rounded corners ----

namespace {

gfx::Document OneElement(const gfx::Element& element, int size = 100) {
  gfx::Document document;
  document.width = size;
  document.height = size;
  document.elements.push_back(element);
  return document;
}

cutline::render::Layer DrawOnto(const gfx::Document& document, int size = 100, const gfx::ImageResolver& images = {}, double seconds = 0.0) {
  cutline::render::Layer canvas;
  canvas.Reset(size, size);
  (void)gfx::Draw(canvas, document, images, seconds);
  return canvas;
}

}  // namespace

CUTLINE_TEST(AGraphicElementTurnsAboutItsMiddleAndTheDocumentNeedsTheNewestSchemaOnlyWhenItUsesIt) {
  auto bar = PlainBox("bar", 0.4, 0.1, 0.2, 0.6);   // 40..60 across, 10..70 down; its middle is (50, 40)
  auto upright = DrawOnto(OneElement(bar));
  CHECK(Near(upright.at(50, 15).r, 1.0f) && Near(upright.at(30, 40).a, 0.0f));
  CHECK_EQ(gfx::RequiredVersion(OneElement(bar)), std::int64_t{1});

  bar.rotation = 90.0;
  const auto document = OneElement(bar);
  CHECK_EQ(gfx::RequiredVersion(document), std::int64_t{3});
  const auto turned = DrawOnto(document);
  // A quarter turn makes it 60 across and 20 down, about the same middle.
  CHECK(Near(turned.at(30, 40).r, 1.0f) && Near(turned.at(70, 40).r, 1.0f));
  CHECK(Near(turned.at(50, 15).a, 0.0f) && Near(turned.at(50, 65).a, 0.0f));
  // Clockwise: a bar turned 45 degrees has its right end lower, not higher.
  auto arm = PlainBox("arm", 0.2, 0.48, 0.6, 0.04);
  arm.rotation = 45.0;
  const auto swung = DrawOnto(OneElement(arm));
  CHECK(Near(swung.at(68, 68).a, 1.0f) && Near(swung.at(68, 32).a, 0.0f));
  // The turn is kept by the format, and a plain document is still written as the oldest schema.
  const auto round_trip = gfx::ParseDocument(gfx::ToJson(document));
  CHECK(Near(static_cast<float>(round_trip.elements[0].rotation), 90.0f));
  CHECK(gfx::ToJson(document).find("\"schema_version\":3") != std::string::npos);
  CHECK(gfx::ToJson(OneElement(PlainBox("plain", 0.1, 0.1, 0.2, 0.2))).find("\"schema_version\":1") != std::string::npos);
  // A document from a newer build is refused, not drawn without what it asks for.
  auto future = gfx::ToJson(document);
  future.replace(future.find("\"schema_version\":3"), 18, "\"schema_version\":9");
  CHECK_THROWS(gfx::ParseDocument(future));
  // Not finite or out of range is refused.
  bar.rotation = std::numeric_limits<double>::infinity();
  CHECK_THROWS(gfx::Validate(OneElement(bar)));
  bar.rotation = 0.0;
  bar.stroke_width = 2.0;
  CHECK_THROWS(gfx::Validate(OneElement(bar)));
}

CUTLINE_TEST(AnOutlineIsDrawnInsideAShapeAndRoundedCornersCutTheCornersAway) {
  auto box = PlainBox("box", 0.2, 0.2, 0.6, 0.6);   // 20..80
  box.stroke_width = 0.05;                           // 5 px
  box.stroke = {0.0, 0.0, 1.0, 1.0};
  const auto outlined = DrawOnto(OneElement(box));
  CHECK(Near(outlined.at(22, 50).b, 1.0f) && Near(outlined.at(22, 50).r, 0.0f));   // in the band
  CHECK(Near(outlined.at(50, 50).r, 1.0f) && Near(outlined.at(50, 50).b, 0.0f));   // the fill inside it
  CHECK(Near(outlined.at(10, 50).a, 0.0f));                                         // nothing outside the edge

  box.stroke_width = 0.0;
  box.corner_radius = 0.2;                           // 20 px: a quarter circle at each corner
  const auto rounded = DrawOnto(OneElement(box));
  CHECK(Near(rounded.at(21, 21).a, 0.0f));
  CHECK(Near(rounded.at(50, 21).a, 1.0f) && Near(rounded.at(21, 50).a, 1.0f));
  // The edge is smooth: a pixel on the arc is only partly covered.
  bool partial = false;
  for (int y = 20; y < 40 && !partial; ++y) for (int x = 20; x < 40; ++x) {
    const auto alpha = rounded.at(x, y).a;
    if (alpha > 0.05f && alpha < 0.95f) { partial = true; break; }
  }
  CHECK(partial);
  // Outline and rounding together, on an ellipse.
  auto ellipse = PlainBox("oval", 0.1, 0.3, 0.8, 0.4);
  ellipse.type = gfx::ElementType::Ellipse;
  ellipse.stroke_width = 0.03;
  ellipse.stroke = {0.0, 1.0, 0.0, 1.0};
  const auto oval = DrawOnto(OneElement(ellipse));
  CHECK(Near(oval.at(50, 50).r, 1.0f) && oval.at(11, 50).g > 0.9f && Near(oval.at(2, 2).a, 0.0f));
}

CUTLINE_TEST(AShadowFollowsTheElementInThePicturesOwnDirectionsAndSoftensWithItsBlur) {
  auto box = PlainBox("box", 0.2, 0.2, 0.3, 0.3);   // 20..50
  box.shadow = {0.0, 0.0, 0.0, 1.0};
  box.shadow_x = 0.1;                                 // 10 px right and down
  box.shadow_y = 0.1;
  const auto hard = DrawOnto(OneElement(box));
  CHECK(Near(hard.at(35, 35).r, 1.0f));                                  // the element is on top of its shadow
  CHECK(Near(hard.at(55, 55).a, 1.0f) && Near(hard.at(55, 55).r, 0.0f));   // the shadow shows beyond its corner
  CHECK(Near(hard.at(15, 15).a, 0.0f) && Near(hard.at(70, 70).a, 0.0f));

  box.shadow_blur = 0.1;
  const auto soft = DrawOnto(OneElement(box));
  const auto edge = soft.at(60, 55).a;
  CHECK(edge > 0.05f && edge < 0.95f);             // the shadow's edge is spread
  CHECK(soft.at(55, 55).a > 0.4f);

  // The shadow is a quarter of the element's opacity when the element is.
  box.opacity = 0.5;
  const auto faded = DrawOnto(OneElement(box));
  CHECK(faded.at(55, 55).a < soft.at(55, 55).a + 1e-4f);
  CHECK(faded.at(55, 55).a > 0.0f);

  // A shadow whose colour is transparent is not drawn, and the document stays at the oldest schema.
  auto none = PlainBox("none", 0.2, 0.2, 0.3, 0.3);
  none.shadow_x = 0.1;
  CHECK_EQ(gfx::RequiredVersion(OneElement(none)), std::int64_t{1});
  CHECK(Near(DrawOnto(OneElement(none)).at(55, 55).a, 0.0f));
}

CUTLINE_TEST(RotationOutlinesAndShadowsAnimateAndAreTemplateControls) {
  auto box = PlainBox("box", 0.4, 0.1, 0.2, 0.6);
  box.animations.push_back({"rotation", "linear", {{0.0, 0.0}, {2.0, 90.0}}});
  const auto document = OneElement(box);
  CHECK_EQ(gfx::RequiredVersion(document), std::int64_t{3});
  CHECK(Near(DrawOnto(document, 100, {}, 0.0).at(50, 15).a, 1.0f));
  CHECK(Near(DrawOnto(document, 100, {}, 2.0).at(50, 15).a, 0.0f) && Near(DrawOnto(document, 100, {}, 2.0).at(30, 40).a, 1.0f));
  CHECK(Near(static_cast<float>(gfx::Evaluate(box, 1.0).rotation), 45.0f));
  const auto parsed = gfx::ParseDocument(gfx::ToJson(document));
  CHECK_EQ(parsed.elements[0].animations.size(), std::size_t{1});

  gfx::TemplatePackage package;
  package.id = "badge";
  package.version = 1;
  package.document = OneElement(PlainBox("box", 0.2, 0.2, 0.4, 0.4));
  package.controls.push_back({"turn", "box", "rotation", "0"});
  package.controls.push_back({"outline", "box", "stroke_width", "0"});
  package.controls.push_back({"outline_colour", "box", "stroke", "#0000FF"});
  package.controls.push_back({"glow", "box", "shadow", "0,0,0,0"});
  package.controls.push_back({"glow_blur", "box", "shadow_blur", "0"});
  const auto round_trip = gfx::ParseTemplate(gfx::ToJson(package));
  const auto instance = gfx::Instantiate(round_trip, {{"turn", "30"}, {"outline", "0.02"}, {"outline_colour", ""}, {"glow", "0,0,0,0.5"}, {"glow_blur", "0.05"}});
  CHECK(Near(static_cast<float>(instance.elements[0].rotation), 30.0f) && Near(static_cast<float>(instance.elements[0].stroke_width), 0.02f));
  CHECK(Near(static_cast<float>(instance.elements[0].stroke[2]), 1.0f) && Near(static_cast<float>(instance.elements[0].shadow[3]), 0.5f));
  CHECK_EQ(gfx::RequiredVersion(instance), std::int64_t{3});
}

CUTLINE_TEST(TextAndPicturesTakeOutlinesAndRoundedCornersToo) {
  // A picture, rounded and bound to the element's box, with bilinear scaling of a 2 x 2 source.
  auto picture = PlainBox("logo", 0.2, 0.2, 0.6, 0.6);
  picture.type = gfx::ElementType::Image;
  picture.asset = "logo";
  picture.corner_radius = 0.3;
  const auto logo = SolidFrame(0.0f, 1.0f, 0.0f, 1.0f, 2, 2);
  const auto drawn = DrawOnto(OneElement(picture), 100, [&](const std::string& asset) {
    return asset == "logo" ? &logo : static_cast<const VideoFrame*>(nullptr);
  });
  CHECK(Near(drawn.at(50, 50).g, 1.0f) && Near(drawn.at(21, 21).a, 0.0f));

  if (!cutline::render::text::Available()) return;
  gfx::Element title;
  title.id = "title";
  title.type = gfx::ElementType::Text;
  title.text = "W";
  title.x = 0.1;
  title.y = 0.1;
  title.width = 0.8;
  title.height = 0.8;
  title.font_size = 0.6;
  title.fill = {1.0, 1.0, 1.0, 1.0};
  title.stroke_width = 0.03;
  title.stroke = {1.0, 0.0, 0.0, 1.0};
  const auto plain_text = [&] { auto copy = title; copy.stroke_width = 0.0; return DrawOnto(OneElement(copy)); }();
  const auto outlined = DrawOnto(OneElement(title));
  int red = 0, white = 0, plain_red = 0;
  for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) {
    const auto& p = outlined.at(x, y);
    if (p.a > 0.99f && p.r > 0.99f && p.g < 0.01f) ++red;
    if (p.a > 0.99f && p.g > 0.99f) ++white;
    if (plain_text.at(x, y).g < 0.01f && plain_text.at(x, y).a > 0.99f) ++plain_red;
  }
  CHECK(red > 50 && white > 50 && plain_red == 0);
  // Shadowed and turned text still draws, and the layer's dirty area covers it.
  title.stroke_width = 0.0;
  title.rotation = 15.0;
  title.shadow = {0.0, 0.0, 0.0, 0.8};
  title.shadow_x = 0.03;
  title.shadow_y = 0.03;
  title.shadow_blur = 0.04;
  CHECK(!DrawOnto(OneElement(title)).empty());
}

CUTLINE_TEST(StillPicturesAreDecodedOnceFromFilesAndAMissingOneIsNull) {
  cutline::media::RegisterAllProviders();
  cutline::render::graphics::StillCache cache;
  CHECK(cache.Find("") == nullptr && cache.Find("no-such-file.png") == nullptr);
  const auto fixtures = cutline::testing::EnvironmentValue("CUTLINE_FIXTURE_DIR");
  if (fixtures.empty()) return;
  const auto path = (std::filesystem::path(fixtures) / "bars-2997.mp4").string();
  if (!std::filesystem::exists(path)) return;
  const auto* first = cache.Find(path);
  CHECK(first != nullptr && first->valid() && first->width() > 0);
  CHECK(cache.Find(path) == first);   // kept, not decoded again
  const auto resolver = cache.Resolver();
  CHECK(resolver(path) == first);
}

int main() { return cutline::testing::RunAll("render"); }

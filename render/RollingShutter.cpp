#include "render/RollingShutter.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace cutline::render::tracking {

const std::vector<CameraProfile>& BuiltInCameraProfiles() {
  static const std::vector<CameraProfile> profiles{
      {"global", "Global shutter", 0.0, false, "Every row is captured at once: nothing to repair."},
      {"cmos-fast", "CMOS, fast readout (nominal)", 0.3, false, "A class of camera, not a measurement: readout about a third of the frame interval."},
      {"cmos-typical", "CMOS, typical readout (nominal)", 0.6, false, "A class of camera, not a measurement: readout about 60 percent of the frame interval."},
      {"cmos-slow", "CMOS, slow readout (nominal)", 0.9, false, "A class of camera, not a measurement: readout nearly the whole frame interval."},
  };
  return profiles;
}

std::string ProfilesToJson(const std::vector<CameraProfile>& profiles) {
  std::vector<std::string> items;
  for (const auto& p : profiles) {
    items.push_back(json::Object().Add("id", p.id).Add("name", p.name).Add("readout", p.readout).Add("bottomToTop", p.bottom_to_top).Add("notes", p.notes).Build());
  }
  return json::Object().Add("version", static_cast<std::int64_t>(1)).AddRaw("profiles", json::Array(items)).Build();
}

std::vector<CameraProfile> ProfilesFromJson(const std::string& text) {
  const auto root = json::Parse(text);
  std::vector<CameraProfile> out;
  std::set<std::string> seen;
  for (const auto& item : root.Require("profiles").items) {
    CameraProfile p;
    p.id = item.String("id");
    p.name = item.String("name");
    p.readout = item.Number("readout");
    p.bottom_to_top = item.Bool("bottomToTop");
    if (const auto* notes = item.Find("notes"); notes != nullptr && notes->is_string()) p.notes = notes->text;
    if (p.id.empty()) throw std::invalid_argument("A camera profile needs an id");
    if (!seen.insert(p.id).second) throw std::invalid_argument("The camera profile " + p.id + " appears twice");
    if (!(p.readout >= 0.0 && p.readout <= 1.0)) throw std::invalid_argument("The readout of " + p.id + " must be between 0 and 1");
    out.push_back(std::move(p));
  }
  return out;
}

const CameraProfile* FindProfile(const std::vector<CameraProfile>& profiles, const std::string& id) {
  for (const auto& p : profiles) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

RollingShutterResult AnalyzeRollingShutter(std::size_t frame_count, time::FrameRate rate, const FrameResolver& resolve,
                                           const CameraProfile& profile, const RollingShutterConfig& config, const CancelCheck& cancel,
                                           const ProgressCallback& progress) {
  if (frame_count < 2) throw std::invalid_argument("Rolling-shutter analysis needs at least two frames");
  if (!resolve) throw std::invalid_argument("Rolling-shutter analysis has no frame resolver");
  if (config.region_fraction < 0.2 || config.region_fraction > 1.0) throw std::invalid_argument("The analysis region is a fraction between 0.2 and 1");
  RollingShutterResult result;
  result.profile = profile;

  const auto prepare = [&](std::size_t index) {
    const auto* frame = resolve(index);
    if (frame == nullptr || !frame->valid()) throw std::runtime_error("Rolling-shutter analysis could not resolve frame " + std::to_string(index));
    if (index == 0) {
      result.source_width = frame->width();
      result.source_height = frame->height();
    } else if (frame->width() != result.source_width || frame->height() != result.source_height) {
      throw std::invalid_argument("Rolling-shutter analysis frame size changed");
    }
    const auto gray = ToGray(*frame, config.analysis_width);
    return std::pair{BuildPyramid(gray, config.align.pyramid_levels), static_cast<double>(gray.width) / frame->width()};
  };

  // The velocity at frame k is half the motion from frame k-1 to k+1 (the first and last frames use the one
  // neighbour they have). Each frame's pyramid is built once and dropped when no later frame needs it.
  std::map<std::size_t, std::pair<Pyramid, double>> cache;
  const auto get = [&](std::size_t index) -> const std::pair<Pyramid, double>& {
    auto found = cache.find(index);
    if (found == cache.end()) found = cache.emplace(index, prepare(index)).first;
    return found->second;
  };
  for (std::size_t k = 0; k < frame_count; ++k) {
    if (cancel && cancel()) {
      result.cancelled = true;
      break;
    }
    const auto first = k == 0 ? 0 : k - 1;
    const auto last = k + 1 < frame_count ? k + 1 : k;
    const double span = static_cast<double>(last - first);
    const auto& earlier = get(first);
    const auto& later = get(last);
    const auto width = earlier.first.levels.front().width, height = earlier.first.levels.front().height;
    const auto fraction = config.region_fraction;
    const Region region{width * (1.0 - fraction) * 0.5, height * (1.0 - fraction) * 0.5, width * fraction, height * fraction};
    const auto aligned = Align(earlier.first, later.first, region, Model::Translation, Warp{}, config.align);

    RollingShutterSample sample;
    sample.time = time::RationalTime::FromFrames(static_cast<std::int64_t>(k), rate);
    sample.confidence = aligned.confidence;
    sample.valid = aligned.confidence >= config.min_confidence;
    if (sample.valid) {
      sample.velocity_x = aligned.warp.tx / earlier.second / span;
      sample.velocity_y = aligned.warp.ty / earlier.second / span;
      sample.horizontal = sample.velocity_x * profile.readout;
      sample.vertical = sample.velocity_y * profile.readout;
    }
    result.samples.push_back(sample);
    if (progress) progress(k + 1, frame_count);
    // Frame k-1 is not needed again.
    if (k >= 1) cache.erase(k - 1);
  }
  return result;
}

timeline::Effect MakeRollingShutterEffect(const RollingShutterResult& result, std::string effect_id, std::int64_t order) {
  if (result.samples.empty()) throw std::invalid_argument("Cannot build a rolling-shutter effect from no samples");
  timeline::Effect effect;
  effect.id = std::move(effect_id);
  effect.effect_type = "rolling_shutter";
  effect.order = order;
  // Measured corrections, held across the frames that could not be measured; frames before the first
  // measurement take that first value.
  std::vector<anim::Keyframe> horizontal, vertical;
  double last_h = 0.0, last_v = 0.0;
  for (const auto& s : result.samples) {
    if (s.valid) {
      last_h = s.horizontal;
      last_v = s.vertical;
      break;
    }
  }
  for (const auto& s : result.samples) {
    if (s.valid) {
      last_h = s.horizontal;
      last_v = s.vertical;
    }
    horizontal.push_back({s.time, anim::Value::Scalar(last_h), anim::Interpolation::Linear, {}, {}});
    vertical.push_back({s.time, anim::Value::Scalar(last_v), anim::Interpolation::Linear, {}, {}});
  }
  const auto add = [&](const std::string& name, anim::AnimatedValue value) {
    timeline::Parameter parameter;
    parameter.id = effect.id + ":" + name;
    parameter.name = name;
    parameter.value = std::move(value);
    effect.parameters.push_back(std::move(parameter));
  };
  add("horizontal", anim::AnimatedValue(std::move(horizontal)));
  add("vertical", anim::AnimatedValue(std::move(vertical)));
  add("direction", anim::AnimatedValue(anim::Value::Scalar(result.profile.bottom_to_top ? 1.0 : 0.0)));
  return effect;
}

}  // namespace cutline::render::tracking

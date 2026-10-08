#include "effects/EffectRegistry.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace cutline::effects {
namespace {

using anim::Value;

ParameterDescriptor Scalar(std::string id, std::string name, double value, Unit unit = Unit::None,
                           std::optional<double> minimum = {}, std::optional<double> maximum = {}) {
  return {std::move(id), std::move(name), 1, Value::Scalar(value), minimum, maximum, unit, true};
}

ParameterDescriptor Vec2(std::string id, std::string name, double x, double y, Unit unit = Unit::None,
                         std::optional<double> minimum = {}, std::optional<double> maximum = {}) {
  return {std::move(id), std::move(name), 2, Value::Vec2(x, y), minimum, maximum, unit, true};
}

ParameterDescriptor Vec3(std::string id, std::string name, double x, double y, double z, Unit unit = Unit::None,
                         std::optional<double> minimum = {}, std::optional<double> maximum = {}) {
  return {std::move(id), std::move(name), 3, Value::Vec3(x, y, z), minimum, maximum, unit, true};
}

ParameterDescriptor Vec4(std::string id, std::string name, double x, double y, double z, double w, Unit unit = Unit::None,
                         std::optional<double> minimum = {}, std::optional<double> maximum = {}) {
  return {std::move(id), std::move(name), 4, Value::Vec4(x, y, z, w), minimum, maximum, unit, true};
}

EffectDescriptor Video(std::string id, std::string name, std::string category,
                       std::vector<ParameterDescriptor> parameters, std::string asset_kind = {}) {
  return {std::move(id), std::move(name), std::move(category), Medium::Video, true, false,
          std::move(asset_kind), std::move(parameters)};
}

EffectDescriptor GpuVideo(std::string id, std::string name, std::string category,
                          std::vector<ParameterDescriptor> parameters, std::string asset_kind = {}) {
  auto effect = Video(std::move(id), std::move(name), std::move(category), std::move(parameters),
                      std::move(asset_kind));
  effect.gpu_available = true;
  return effect;
}

EffectDescriptor Audio(std::string id, std::string name, std::string category,
                       std::vector<ParameterDescriptor> parameters, std::string asset_kind = {}) {
  return {std::move(id), std::move(name), std::move(category), Medium::Audio, true, false, std::move(asset_kind),
          std::move(parameters)};
}

// The mesh warp's control grid: four by four points, each an offset in pixels (inverse mapping, as MeshWarp).
std::vector<ParameterDescriptor> MeshWarpParameters() {
  std::vector<ParameterDescriptor> parameters;
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      parameters.push_back(Vec2("point_" + std::to_string(row) + "_" + std::to_string(column),
                                "Point " + std::to_string(row + 1) + "," + std::to_string(column + 1), 0.0, 0.0, Unit::Pixels, -4000.0, 4000.0));
    }
  }
  return parameters;
}

}  // namespace

const std::vector<EffectDescriptor>& BuiltInEffects() {
  static const std::vector<EffectDescriptor> effects{
      GpuVideo("opacity", "Opacity", "Transform", {Scalar("value", "Opacity", 1.0, Unit::Normalized, 0.0, 1.0)}),
      GpuVideo("motion", "Motion", "Transform",
            {Vec2("scale", "Scale", 100.0, 100.0, Unit::Percent, 0.01, 10000.0),
             Vec2("position", "Position", 0.0, 0.0, Unit::Pixels),
             Scalar("rotation", "Rotation", 0.0, Unit::Degrees),
             Vec2("anchor", "Anchor", 0.5, 0.5, Unit::Normalized, 0.0, 1.0)}),
      GpuVideo("transform", "Transform", "Transform",
            {Vec2("scale", "Scale", 100.0, 100.0, Unit::Percent, 0.01, 10000.0),
             Vec2("position", "Position", 0.0, 0.0, Unit::Pixels),
             Scalar("rotation", "Rotation", 0.0, Unit::Degrees),
             Vec2("anchor", "Anchor", 0.5, 0.5, Unit::Normalized, 0.0, 1.0)}),
      GpuVideo("crop", "Crop", "Transform",
            {Scalar("left", "Left", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("top", "Top", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("right", "Right", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("bottom", "Bottom", 0.0, Unit::Normalized, 0.0, 1.0)}),
      GpuVideo("grade", "Basic Color", "Color",
            {Scalar("exposure", "Exposure", 0.0, Unit::Stops, -20.0, 20.0),
             Scalar("contrast", "Contrast", 100.0, Unit::Percent, 0.0, 400.0),
             Scalar("saturation", "Saturation", 100.0, Unit::Percent, 0.0, 400.0),
             Scalar("temperature", "Temperature", 0.0, Unit::Percent, -100.0, 100.0)}),
      GpuVideo("lumetri", "Basic Color", "Color",
            {Scalar("exposure", "Exposure", 0.0, Unit::Stops, -20.0, 20.0),
             Scalar("contrast", "Contrast", 100.0, Unit::Percent, 0.0, 400.0),
             Scalar("saturation", "Saturation", 100.0, Unit::Percent, 0.0, 400.0),
             Scalar("temperature", "Temperature", 0.0, Unit::Percent, -100.0, 100.0)}),
      GpuVideo("solid", "Solid Color", "Generate",
            {Vec3("color", "Color", 0.0, 0.0, 0.0, Unit::Normalized, 0.0, 1.0)}),
      Video("blur", "Box Blur", "Blur & Sharpen", {Scalar("radius", "Radius", 0.0, Unit::Pixels, 0.0, 128.0)}),
      Video("sharpen", "Sharpen", "Blur & Sharpen", {Scalar("amount", "Amount", 0.0, Unit::Multiplier, 0.0, 5.0)}),
      Video("vignette", "Vignette", "Stylize",
            {Scalar("amount", "Amount", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("midpoint", "Midpoint", 0.5, Unit::Normalized, 0.0, 1.0),
             Scalar("feather", "Feather", 0.5, Unit::Normalized, 0.0, 1.0),
             Vec2("center", "Center", 0.5, 0.5, Unit::Normalized, 0.0, 1.0)}),
      Video("lens_correction", "Lens Correction", "Distort",
            {Scalar("distortion", "Distortion", 0.0, Unit::Normalized, -1.0, 1.0),
             Scalar("quadratic", "Quadratic", 0.0, Unit::Normalized, -1.0, 1.0),
             Scalar("scale", "Scale", 100.0, Unit::Percent, 1.0, 400.0),
             Vec2("center", "Optical Center", 0.5, 0.5, Unit::Normalized, 0.0, 1.0)}),
      Video("wide_angle", "Wide-Angle Correction", "Distort",
            {Scalar("distortion", "Distortion", 0.0, Unit::Normalized, -1.0, 1.0),
             Scalar("quadratic", "Quadratic", 0.0, Unit::Normalized, -1.0, 1.0),
             Scalar("scale", "Scale", 100.0, Unit::Percent, 1.0, 400.0),
             Vec2("center", "Optical Center", 0.5, 0.5, Unit::Normalized, 0.0, 1.0)}),
      // What the clip's picture is, when the file does not say (log footage is usually untagged): the colour space the
      // picture is brought from into the sequence's working space. The name is in the preset (see render/ColorManagement.h).
      Video("input_colorspace", "Input Colour Space", "Color", {}, "colorspace"),
      GpuVideo("lut", "Creative LUT", "Color",
            {Scalar("intensity", "Intensity", 1.0, Unit::Normalized, 0.0, 1.0)}, ".cube"),
      GpuVideo("stabilizer", "Stabilizer", "Distort",
            {Vec2("position", "Correction", 0.0, 0.0, Unit::Pixels),
             Vec2("scale", "Auto Scale", 100.0, 100.0, Unit::Percent, 1.0, 400.0),
             Scalar("rotation", "Rotation", 0.0, Unit::Degrees)}),
      Video("rolling_shutter", "Rolling-Shutter Repair", "Distort",
            {Scalar("horizontal", "Horizontal Skew", 0.0, Unit::Pixels, -2000.0, 2000.0),
             Scalar("vertical", "Vertical Skew", 0.0, Unit::Pixels, -2000.0, 2000.0),
             Scalar("rotation", "Scan Rotation", 0.0, Unit::Degrees, -45.0, 45.0),
             Scalar("curve", "Scan Curve", 0.0, Unit::Normalized, -1.0, 1.0),
             Scalar("direction", "Scan Direction", 0.0, Unit::None, 0.0, 1.0)}),
      Video("mesh_warp", "Mesh Warp", "Distort", MeshWarpParameters()),
      Video("blend_mode", "Blend Mode", "Composite",
            {Scalar("mode", "Mode", 0.0, Unit::None, 0.0, 11.0),
             Scalar("opacity", "Blend Opacity", 1.0, Unit::Normalized, 0.0, 1.0)}),
      GpuVideo("time_remap", "Time Remap", "Time",
            {Scalar("source_offset", "Source Time Offset", 0.0, Unit::None, 0.0)}),
      Video("frame_interpolation", "Frame Interpolation", "Time",
            {Scalar("mode", "Mode", 0.0, Unit::None, 0.0, 2.0)}),
      Video("graphic", "Title / Graphic", "Generate", {}, ".cutgraphic"),
      Video("motion_graphics_template", "Motion Graphics Template", "Generate", {}, ".cuttemplate"),
      // Production filters (FX-003).
      Video("gaussian_blur", "Gaussian Blur", "Blur & Sharpen", {Scalar("radius", "Radius", 0.0, Unit::Pixels, 0.0, 128.0)}),
      Video("directional_blur", "Directional Blur", "Blur & Sharpen",
            {Scalar("length", "Length", 0.0, Unit::Pixels, 0.0, 256.0), Scalar("angle", "Angle", 0.0, Unit::Degrees)}),
      Video("unsharp_mask", "Unsharp Mask", "Blur & Sharpen",
            {Scalar("amount", "Amount", 0.0, Unit::Multiplier, 0.0, 5.0),
             Scalar("radius", "Radius", 2.0, Unit::Pixels, 0.0, 64.0),
             Scalar("threshold", "Threshold", 0.0, Unit::Normalized, 0.0, 1.0)}),
      Video("glow", "Glow", "Stylize",
            {Scalar("threshold", "Threshold", 0.7, Unit::Normalized, 0.0, 1.0),
             Scalar("radius", "Radius", 0.0, Unit::Pixels, 0.0, 128.0),
             Scalar("intensity", "Intensity", 0.0, Unit::Multiplier, 0.0, 4.0)}),
      Video("drop_shadow", "Drop Shadow", "Stylize",
            {Vec3("color", "Color", 0.0, 0.0, 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("opacity", "Opacity", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("distance", "Distance", 10.0, Unit::Pixels, 0.0, 500.0),
             Scalar("angle", "Direction", 45.0, Unit::Degrees),
             Scalar("softness", "Softness", 5.0, Unit::Pixels, 0.0, 128.0)}),
      Video("noise_reduction", "Noise Reduction", "Restoration",
            {Scalar("luma", "Luma", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("chroma", "Chroma", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("temporal", "Temporal", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("detail", "Detail Preservation", 0.5, Unit::Normalized, 0.0, 1.0),
             Scalar("radius", "Temporal Frames", 1.0, Unit::None, 1.0, 2.0)}),
      GpuVideo("channel_mixer", "Channel Mixer", "Color",
            {Vec3("red", "Red Output", 1.0, 0.0, 0.0, Unit::Multiplier, -4.0, 4.0),
             Vec3("green", "Green Output", 0.0, 1.0, 0.0, Unit::Multiplier, -4.0, 4.0),
             Vec3("blue", "Blue Output", 0.0, 0.0, 1.0, Unit::Multiplier, -4.0, 4.0)}),
      GpuVideo("black_and_white", "Black & White", "Color",
            {Vec3("weights", "Channel Weights", 0.2126, 0.7152, 0.0722, Unit::Multiplier, -2.0, 2.0),
             Scalar("amount", "Amount", 1.0, Unit::Normalized, 0.0, 1.0)}),
      GpuVideo("tint", "Tint", "Color",
            {Vec3("map_black", "Map Black To", 0.0, 0.0, 0.0, Unit::Normalized, 0.0, 1.0),
             Vec3("map_white", "Map White To", 1.0, 1.0, 1.0, Unit::Normalized, 0.0, 1.0),
             Scalar("amount", "Amount", 1.0, Unit::Normalized, 0.0, 1.0)}),
      Video("posterize", "Posterize", "Stylize", {Scalar("levels", "Levels", 256.0, Unit::None, 2.0, 256.0)}),
      Video("wave_warp", "Wave Warp", "Distort",
            {Scalar("amplitude", "Amplitude", 0.0, Unit::Pixels, 0.0, 200.0),
             Scalar("wavelength", "Wavelength", 100.0, Unit::Pixels, 4.0, 4000.0),
             Scalar("phase", "Phase", 0.0, Unit::Degrees),
             Scalar("vertical", "Vertical", 0.0, Unit::Normalized, 0.0, 1.0)}),
      Video("bulge", "Bulge", "Distort",
            {Scalar("amount", "Amount", 0.0, Unit::Normalized, -1.0, 1.0),
             Scalar("radius", "Radius", 0.5, Unit::Normalized, 0.01, 2.0),
             Vec2("center", "Center", 0.5, 0.5, Unit::Normalized, 0.0, 1.0)}),
      // Keying (KEY-001). The garbage and core rectangles are left, top, right, bottom.
      Video("chroma_key", "Chroma Key", "Keying",
            {Vec3("color", "Key Color", 0.0, 1.0, 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("tolerance", "Tolerance", 0.25, Unit::Normalized, 0.0, 1.0),
             Scalar("softness", "Softness", 0.15, Unit::Normalized, 0.0, 1.0),
             Scalar("spill", "Spill Suppression", 0.5, Unit::Normalized, 0.0, 1.0),
             Scalar("shrink", "Shrink Matte", 0.0, Unit::Pixels, -20.0, 20.0),
             Scalar("feather", "Feather", 0.0, Unit::Pixels, 0.0, 64.0),
             Vec4("garbage", "Garbage Matte", 0.0, 0.0, 1.0, 1.0, Unit::Normalized, 0.0, 1.0),
             Vec4("core", "Core Matte", 0.0, 0.0, 0.0, 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("view", "View Matte", 0.0, Unit::Normalized, 0.0, 1.0)}),
      Video("luma_key", "Luma Key", "Keying",
            {Scalar("threshold", "Threshold", 0.1, Unit::Normalized, 0.0, 1.0),
             Scalar("softness", "Softness", 0.1, Unit::Normalized, 0.0, 1.0),
             Scalar("invert", "Invert", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("shrink", "Shrink Matte", 0.0, Unit::Pixels, -20.0, 20.0),
             Scalar("feather", "Feather", 0.0, Unit::Pixels, 0.0, 64.0),
             Vec4("garbage", "Garbage Matte", 0.0, 0.0, 1.0, 1.0, Unit::Normalized, 0.0, 1.0),
             Vec4("core", "Core Matte", 0.0, 0.0, 0.0, 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("view", "View Matte", 0.0, Unit::Normalized, 0.0, 1.0)}),
      // Grading tools (COLOR-002). Wheels and gains are red, green, blue, then master.
      GpuVideo("color_wheels", "Color Wheels", "Color",
            {Vec4("lift", "Lift", 0.0, 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0),
             Vec4("gamma", "Gamma", 1.0, 1.0, 1.0, 1.0, Unit::Multiplier, 0.1, 4.0),
             Vec4("gain", "Gain", 1.0, 1.0, 1.0, 1.0, Unit::Multiplier, 0.0, 4.0),
             Vec3("shadows", "Shadows", 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0),
             Vec3("midtones", "Midtones", 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0),
             Vec3("highlights", "Highlights", 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0)}),
      GpuVideo("curves", "Curves", "Color",
            {Vec3("master", "RGB Curve", 0.25, 0.5, 0.75, Unit::Normalized, 0.0, 1.0),
             Vec3("red", "Red Curve", 0.25, 0.5, 0.75, Unit::Normalized, 0.0, 1.0),
             Vec3("green", "Green Curve", 0.25, 0.5, 0.75, Unit::Normalized, 0.0, 1.0),
             Vec3("blue", "Blue Curve", 0.25, 0.5, 0.75, Unit::Normalized, 0.0, 1.0),
             Vec3("luma", "Luma Curve", 0.25, 0.5, 0.75, Unit::Normalized, 0.0, 1.0)}),
      GpuVideo("hue_curves", "Hue Curves", "Color",
            {Vec3("hue_vs_hue_a", "Hue Shift 0/60/120", 0.0, 0.0, 0.0, Unit::Degrees, -180.0, 180.0),
             Vec3("hue_vs_hue_b", "Hue Shift 180/240/300", 0.0, 0.0, 0.0, Unit::Degrees, -180.0, 180.0),
             Vec3("hue_vs_sat_a", "Saturation 0/60/120", 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0),
             Vec3("hue_vs_sat_b", "Saturation 180/240/300", 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0),
             Vec3("hue_vs_luma_a", "Lightness 0/60/120", 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0),
             Vec3("hue_vs_luma_b", "Lightness 180/240/300", 0.0, 0.0, 0.0, Unit::Normalized, -1.0, 1.0)}),
      GpuVideo("color_adjust", "Color Adjust", "Color",
            {Scalar("temperature", "Temperature", 0.0, Unit::Percent, -100.0, 100.0),
             Scalar("tint", "Tint", 0.0, Unit::Percent, -100.0, 100.0),
             Scalar("vibrance", "Vibrance", 0.0, Unit::Percent, -100.0, 100.0),
             Scalar("shadows", "Shadows", 0.0, Unit::Percent, -100.0, 100.0),
             Scalar("highlights", "Highlights", 0.0, Unit::Percent, -100.0, 100.0)}),
      GpuVideo("hsl_secondary", "HSL Secondary", "Color",
            {Scalar("hue_center", "Hue Center", 0.0, Unit::Degrees, 0.0, 360.0),
             Scalar("hue_width", "Hue Width", 360.0, Unit::Degrees, 0.0, 360.0),
             Scalar("hue_softness", "Hue Softness", 0.0, Unit::Degrees, 0.0, 180.0),
             Scalar("sat_min", "Saturation Min", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("sat_max", "Saturation Max", 1.0, Unit::Normalized, 0.0, 1.0),
             Scalar("sat_softness", "Saturation Softness", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("luma_min", "Luma Min", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("luma_max", "Luma Max", 1.0, Unit::Normalized, 0.0, 1.0),
             Scalar("luma_softness", "Luma Softness", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("hue_shift", "Hue Shift", 0.0, Unit::Degrees, -180.0, 180.0),
             Scalar("sat_gain", "Saturation Gain", 1.0, Unit::Multiplier, 0.0, 4.0),
             Scalar("lightness", "Lightness", 0.0, Unit::Normalized, -1.0, 1.0),
             Scalar("view", "View Matte", 0.0, Unit::Normalized, 0.0, 1.0),
             Scalar("invert", "Invert", 0.0, Unit::Normalized, 0.0, 1.0)}),
      Audio("volume", "Volume", "Amplitude", {Scalar("level", "Level", 0.0, Unit::Decibels, -96.0, 24.0)}),
      Audio("gain", "Gain", "Amplitude", {Scalar("value", "Gain", 1.0, Unit::Multiplier, 0.0, 16.0)}),
      Audio("pan", "Pan", "Stereo", {Scalar("value", "Pan", 0.0, Unit::Normalized, -1.0, 1.0)}),
      Audio("eq", "Equalizer", "Tone",
            {Scalar("low_gain", "Low Gain", 0.0, Unit::Decibels, -24.0, 24.0),
             Scalar("low_frequency", "Low Frequency", 120.0, Unit::None, 20.0, 1000.0),
             Scalar("mid_gain", "Mid Gain", 0.0, Unit::Decibels, -24.0, 24.0),
             Scalar("mid_frequency", "Mid Frequency", 1000.0, Unit::None, 100.0, 10000.0),
             Scalar("mid_q", "Mid Q", 1.0, Unit::None, 0.1, 10.0),
             Scalar("high_gain", "High Gain", 0.0, Unit::Decibels, -24.0, 24.0),
             Scalar("high_frequency", "High Frequency", 8000.0, Unit::None, 1000.0, 20000.0)}),
      Audio("compressor", "Compressor", "Dynamics",
            {Scalar("threshold", "Threshold", -18.0, Unit::Decibels, -60.0, 0.0),
             Scalar("ratio", "Ratio", 4.0, Unit::None, 1.0, 20.0),
             Scalar("attack", "Attack", 10.0, Unit::None, 0.1, 200.0),
             Scalar("release", "Release", 150.0, Unit::None, 5.0, 2000.0),
             Scalar("makeup", "Make-up Gain", 0.0, Unit::Decibels, -12.0, 24.0),
             Scalar("knee", "Knee", 6.0, Unit::Decibels, 0.0, 24.0)}),
      Audio("limiter", "Limiter", "Dynamics",
            {Scalar("ceiling", "Ceiling", -1.0, Unit::Decibels, -24.0, 0.0),
             Scalar("release", "Release", 100.0, Unit::None, 5.0, 1000.0),
             Scalar("lookahead", "Lookahead", 3.0, Unit::None, 0.5, 10.0)}),
      Audio("gate", "Noise Gate", "Dynamics",
            {Scalar("threshold", "Threshold", -50.0, Unit::Decibels, -90.0, 0.0),
             Scalar("range", "Range", -60.0, Unit::Decibels, -90.0, 0.0),
             Scalar("attack", "Attack", 2.0, Unit::None, 0.1, 100.0),
             Scalar("hold", "Hold", 40.0, Unit::None, 0.0, 1000.0),
             Scalar("release", "Release", 120.0, Unit::None, 5.0, 2000.0)}),
      Audio("duck", "Auto Duck", "Dynamics",
            {Scalar("threshold", "Threshold", -35.0, Unit::Decibels, -80.0, 0.0),
             Scalar("reduction", "Reduction", -12.0, Unit::Decibels, -60.0, 0.0),
             Scalar("attack", "Attack", 20.0, Unit::None, 1.0, 500.0),
             Scalar("release", "Release", 400.0, Unit::None, 10.0, 5000.0)},
            "sidechain"),
      Audio("denoise", "Noise Reduction", "Restoration",
            {Scalar("amount", "Amount", 0.7, Unit::Normalized, 0.0, 1.0),
             Scalar("sensitivity", "Sensitivity", 1.0, Unit::Multiplier, 0.25, 4.0)}),
      Audio("dereverb", "Reverb Reduction", "Restoration",
            {Scalar("amount", "Amount", 0.6, Unit::Normalized, 0.0, 1.0),
             Scalar("decay", "Decay", 300.0, Unit::None, 50.0, 3000.0)}),
  };
  return effects;
}

const EffectDescriptor* FindEffect(std::string_view effect_id) {
  for (const auto& effect : BuiltInEffects()) {
    if (effect.id == effect_id) return &effect;
  }
  return nullptr;
}

const ParameterDescriptor* FindParameter(const EffectDescriptor& effect, std::string_view parameter_id) {
  for (const auto& parameter : effect.parameters) {
    if (parameter.id == parameter_id) return &parameter;
  }
  return nullptr;
}

std::string ToString(Medium medium) { return medium == Medium::Video ? "video" : "audio"; }

std::string ToString(Unit unit) {
  switch (unit) {
    case Unit::None: return "none";
    case Unit::Normalized: return "normalized";
    case Unit::Percent: return "percent";
    case Unit::Pixels: return "pixels";
    case Unit::Degrees: return "degrees";
    case Unit::Stops: return "stops";
    case Unit::Decibels: return "decibels";
    case Unit::Multiplier: return "multiplier";
  }
  throw std::invalid_argument("Unhandled effect parameter unit");
}

std::string ValidateParameter(std::string_view effect_id, std::string_view parameter_id, const anim::Value& value) {
  const auto* effect = FindEffect(effect_id);
  if (effect == nullptr) return {};
  const auto* parameter = FindParameter(*effect, parameter_id);
  if (parameter == nullptr) {
    return "Effect " + std::string(effect_id) + " has no parameter named " + std::string(parameter_id);
  }
  if (value.dimension != parameter->dimension) {
    return "Effect " + std::string(effect_id) + " parameter " + std::string(parameter_id) + " expects " +
           std::to_string(parameter->dimension) + " component(s), received " + std::to_string(value.dimension);
  }
  for (int component = 0; component < value.dimension; ++component) {
    const auto sample = value.components[static_cast<std::size_t>(component)];
    if (!std::isfinite(sample)) {
      return "Effect " + std::string(effect_id) + " parameter " + std::string(parameter_id) +
             " must contain finite values";
    }
    if (parameter->minimum.has_value() && sample < *parameter->minimum) {
      return "Effect " + std::string(effect_id) + " parameter " + std::string(parameter_id) + " is below its minimum";
    }
    if (parameter->maximum.has_value() && sample > *parameter->maximum) {
      return "Effect " + std::string(effect_id) + " parameter " + std::string(parameter_id) + " is above its maximum";
    }
  }
  return {};
}

std::string ValidateAssetReference(std::string_view effect_id, std::string_view asset_reference) {
  const auto* effect = FindEffect(effect_id);
  if (effect == nullptr || effect->asset_kind.empty() || !asset_reference.empty()) return {};
  return "Effect " + std::string(effect_id) + " requires a " + effect->asset_kind + " asset reference";
}

}  // namespace cutline::effects

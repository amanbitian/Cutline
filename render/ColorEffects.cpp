#include "render/Filters.h"
#include "render/ColorOps.h"
#include "render/ParallelRows.h"

#include <algorithm>
#include <array>
#include <cmath>

// Grading tools: lift/gamma/gain and tonal wheels, tone curves, hue-keyed curves,
// white balance and tone adjustments, and HSL secondary correction.
//
// They are written for the display-referred working range the compositor uses today
// (0..1, clamped after each effect). A scene-referred, linear-light pipeline is the
// subject of COLOR-001 and would change what these operate on, not how they are
// specified; the render version is how that change will be kept from altering a
// project made before it.

namespace cutline::render {
namespace {

constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

struct Color final {
  float r{0.0f}, g{0.0f}, b{0.0f};
};

[[nodiscard]] Color Straight(const Pixel& pixel) {
  if (pixel.a <= 0.0f) return {};
  if (pixel.a >= 1.0f) return {pixel.r, pixel.g, pixel.b};
  const auto inverse = 1.0f / pixel.a;
  return {pixel.r * inverse, pixel.g * inverse, pixel.b * inverse};
}

[[nodiscard]] float Luma(const Color& c) { return c.r * kLumaR + c.g * kLumaG + c.b * kLumaB; }

[[nodiscard]] float Smoothstep(float edge0, float edge1, float value) {
  if (edge1 <= edge0) return value >= edge1 ? 1.0f : 0.0f;
  const auto t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] float Param(const SampledEffect& effect, const std::string& name, float fallback) {
  return ScalarParameter(effect, name, fallback);
}

[[nodiscard]] std::array<float, 3> Vec3(const SampledEffect& effect, const std::string& name, float x, float y,
                                        float z) {
  const auto value = FindParameter(effect, name);
  if (!value.has_value()) return {x, y, z};
  return {static_cast<float>(value->components[0]), static_cast<float>(value->components[1]),
          static_cast<float>(value->components[2])};
}

[[nodiscard]] std::array<float, 4> Vec4(const SampledEffect& effect, const std::string& name, float x, float y, float z,
                                        float w) {
  const auto value = FindParameter(effect, name);
  if (!value.has_value()) return {x, y, z, w};
  return {static_cast<float>(value->components[0]), static_cast<float>(value->components[1]),
          static_cast<float>(value->components[2]), static_cast<float>(value->components[3])};
}

template <typename Function>
void ForEachColour(Layer& layer, Function&& function) {
  // Rows run on every core: each pixel depends only on itself, so the result is what one thread would make.
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      const auto result = function(Straight(pixel));
      const auto alpha = pixel.a;
      pixel = {std::clamp(result.r, 0.0f, 1.0f) * alpha, std::clamp(result.g, 0.0f, 1.0f) * alpha,
               std::clamp(result.b, 0.0f, 1.0f) * alpha, alpha};
    }
  });
}

// ------------------------------------------------------------------- HSL ----

struct Hsl final {
  float h{0.0f};  // degrees, 0..360
  float s{0.0f};
  float l{0.0f};
};

[[nodiscard]] Hsl ToHsl(const Color& c) {
  const auto maximum = std::max({c.r, c.g, c.b});
  const auto minimum = std::min({c.r, c.g, c.b});
  Hsl result;
  result.l = (maximum + minimum) * 0.5f;
  const auto delta = maximum - minimum;
  if (delta <= 1e-7f) return result;
  result.s = delta / (1.0f - std::abs(2.0f * result.l - 1.0f) + 1e-12f);
  result.s = std::clamp(result.s, 0.0f, 1.0f);
  float hue;
  if (maximum == c.r) hue = std::fmod((c.g - c.b) / delta, 6.0f);
  else if (maximum == c.g) hue = (c.b - c.r) / delta + 2.0f;
  else hue = (c.r - c.g) / delta + 4.0f;
  result.h = hue * 60.0f;
  if (result.h < 0.0f) result.h += 360.0f;
  return result;
}

[[nodiscard]] Color FromHsl(const Hsl& hsl) {
  const auto chroma = (1.0f - std::abs(2.0f * hsl.l - 1.0f)) * hsl.s;
  auto h = std::fmod(hsl.h, 360.0f);
  if (h < 0.0f) h += 360.0f;
  const auto sector = h / 60.0f;
  const auto x = chroma * (1.0f - std::abs(std::fmod(sector, 2.0f) - 1.0f));
  float r = 0.0f, g = 0.0f, b = 0.0f;
  if (sector < 1.0f) { r = chroma; g = x; }
  else if (sector < 2.0f) { r = x; g = chroma; }
  else if (sector < 3.0f) { g = chroma; b = x; }
  else if (sector < 4.0f) { g = x; b = chroma; }
  else if (sector < 5.0f) { r = x; b = chroma; }
  else { r = chroma; b = x; }
  const auto m = hsl.l - chroma * 0.5f;
  return {r + m, g + m, b + m};
}

// ----------------------------------------------------------- lift/gamma/gain ----

void ApplyColorWheels(Layer& layer, const SampledEffect& effect) {
  const auto lift = Vec4(effect, "lift", 0.0f, 0.0f, 0.0f, 0.0f);
  const auto gamma = Vec4(effect, "gamma", 1.0f, 1.0f, 1.0f, 1.0f);
  const auto gain = Vec4(effect, "gain", 1.0f, 1.0f, 1.0f, 1.0f);
  const auto shadows = Vec3(effect, "shadows", 0.0f, 0.0f, 0.0f);
  const auto midtones = Vec3(effect, "midtones", 0.0f, 0.0f, 0.0f);
  const auto highlights = Vec3(effect, "highlights", 0.0f, 0.0f, 0.0f);

  std::array<float, 3> lifts, inverse_gammas, gains;
  bool identity = true;
  for (int i = 0; i < 3; ++i) {
    lifts[static_cast<std::size_t>(i)] = std::clamp(lift[static_cast<std::size_t>(i)] + lift[3], -1.0f, 1.0f);
    const auto g = std::clamp(gamma[static_cast<std::size_t>(i)] * gamma[3], 0.1f, 4.0f);
    inverse_gammas[static_cast<std::size_t>(i)] = 1.0f / g;
    gains[static_cast<std::size_t>(i)] = std::clamp(gain[static_cast<std::size_t>(i)] * gain[3], 0.0f, 4.0f);
    if (std::abs(lifts[static_cast<std::size_t>(i)]) > 1e-6f || std::abs(g - 1.0f) > 1e-6f ||
        std::abs(gains[static_cast<std::size_t>(i)] - 1.0f) > 1e-6f ||
        std::abs(shadows[static_cast<std::size_t>(i)]) > 1e-6f || std::abs(midtones[static_cast<std::size_t>(i)]) > 1e-6f ||
        std::abs(highlights[static_cast<std::size_t>(i)]) > 1e-6f) {
      identity = false;
    }
  }
  if (identity) return;

  ForEachColour(layer, [&](Color c) {
    float v[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i) {
      const auto k = static_cast<std::size_t>(i);
      auto value = gains[k] * (v[i] + lifts[k] * (1.0f - v[i]));
      value = std::max(value, 0.0f);
      v[i] = inverse_gammas[k] == 1.0f ? value : std::pow(value, inverse_gammas[k]);
    }
    // Tonal wheels: a tint added to the shadows, midtones or highlights, each weighted by
    // where the pixel sits in the tonal range. The three weights always sum to one.
    const auto l = Luma({v[0], v[1], v[2]});
    const auto weight_shadows = 1.0f - Smoothstep(0.0f, 0.5f, l);
    const auto weight_highlights = Smoothstep(0.5f, 1.0f, l);
    const auto weight_midtones = 1.0f - weight_shadows - weight_highlights;
    for (int i = 0; i < 3; ++i) {
      const auto k = static_cast<std::size_t>(i);
      v[i] += shadows[k] * weight_shadows + midtones[k] * weight_midtones + highlights[k] * weight_highlights;
    }
    return Color{v[0], v[1], v[2]};
  });
}

// -------------------------------------------------------------------- curves ----

[[nodiscard]] float Lookup(const float* table, float value) {
  const auto position = std::clamp(value, 0.0f, 1.0f) * 255.0f;
  const auto index = static_cast<int>(position);
  if (index >= 255) return table[255];
  const auto t = position - static_cast<float>(index);
  return table[index] + (table[index + 1] - table[index]) * t;
}

bool IsDefaultCurve(const std::array<float, 3>& points) {
  return std::abs(points[0] - 0.25f) < 1e-6f && std::abs(points[1] - 0.5f) < 1e-6f &&
         std::abs(points[2] - 0.75f) < 1e-6f;
}

void ApplyCurves(Layer& layer, const SampledEffect& effect) {
  const auto master = Vec3(effect, "master", 0.25f, 0.5f, 0.75f);
  const auto red = Vec3(effect, "red", 0.25f, 0.5f, 0.75f);
  const auto green = Vec3(effect, "green", 0.25f, 0.5f, 0.75f);
  const auto blue = Vec3(effect, "blue", 0.25f, 0.5f, 0.75f);
  const auto luma = Vec3(effect, "luma", 0.25f, 0.5f, 0.75f);
  const bool use_master = !IsDefaultCurve(master), use_red = !IsDefaultCurve(red), use_green = !IsDefaultCurve(green),
             use_blue = !IsDefaultCurve(blue), use_luma = !IsDefaultCurve(luma);
  if (!use_master && !use_red && !use_green && !use_blue && !use_luma) return;

  float master_table[256], red_table[256], green_table[256], blue_table[256], luma_table[256];
  BuildCurveTable(master[0], master[1], master[2], master_table);
  BuildCurveTable(red[0], red[1], red[2], red_table);
  BuildCurveTable(green[0], green[1], green[2], green_table);
  BuildCurveTable(blue[0], blue[1], blue[2], blue_table);
  BuildCurveTable(luma[0], luma[1], luma[2], luma_table);

  ForEachColour(layer, [&](Color c) {
    if (use_master) {
      c.r = Lookup(master_table, c.r);
      c.g = Lookup(master_table, c.g);
      c.b = Lookup(master_table, c.b);
    }
    if (use_red) c.r = Lookup(red_table, c.r);
    if (use_green) c.g = Lookup(green_table, c.g);
    if (use_blue) c.b = Lookup(blue_table, c.b);
    if (use_luma) {
      // The luma curve moves brightness and leaves hue alone: scale the colour to the
      // new luma, or, at black where there is no colour to scale, lift it as grey.
      const auto l = Luma(c);
      const auto target = Lookup(luma_table, l);
      if (l > 1e-5f) {
        const auto ratio = target / l;
        c.r *= ratio;
        c.g *= ratio;
        c.b *= ratio;
      } else {
        c.r = c.g = c.b = target;
      }
    }
    return c;
  });
}

// --------------------------------------------------------------- hue curves ----

// Periodic Catmull-Rom through six knots 60 degrees apart.
[[nodiscard]] float PeriodicCurve(const std::array<float, 6>& knots, float hue) {
  const auto position = std::fmod(hue, 360.0f) / 60.0f;
  const auto base = static_cast<int>(std::floor(position));
  const auto t = position - static_cast<float>(base);
  const auto at = [&](int index) { return knots[static_cast<std::size_t>(((index % 6) + 6) % 6)]; };
  const auto p0 = at(base - 1), p1 = at(base), p2 = at(base + 1), p3 = at(base + 2);
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
}

[[nodiscard]] std::array<float, 6> Knots(const SampledEffect& effect, const std::string& name) {
  const auto a = Vec3(effect, name + "_a", 0.0f, 0.0f, 0.0f);
  const auto b = Vec3(effect, name + "_b", 0.0f, 0.0f, 0.0f);
  return {a[0], a[1], a[2], b[0], b[1], b[2]};
}

[[nodiscard]] bool AllZero(const std::array<float, 6>& knots) {
  return std::all_of(knots.begin(), knots.end(), [](float v) { return std::abs(v) < 1e-6f; });
}

void ApplyHueCurves(Layer& layer, const SampledEffect& effect) {
  const auto hue_shift = Knots(effect, "hue_vs_hue");
  const auto saturation = Knots(effect, "hue_vs_sat");
  const auto lightness = Knots(effect, "hue_vs_luma");
  if (AllZero(hue_shift) && AllZero(saturation) && AllZero(lightness)) return;
  ForEachColour(layer, [&](Color c) {
    auto hsl = ToHsl(c);
    // A grey has no hue to look up: the curves fade in with saturation.
    const auto weight = Smoothstep(0.0f, 0.1f, hsl.s);
    if (weight <= 0.0f) return c;
    const auto shift = PeriodicCurve(hue_shift, hsl.h) * weight;
    const auto sat = PeriodicCurve(saturation, hsl.h) * weight;
    const auto lum = PeriodicCurve(lightness, hsl.h) * weight;
    hsl.h += shift;
    hsl.s = std::clamp(hsl.s * (1.0f + sat), 0.0f, 1.0f);
    hsl.l = lum >= 0.0f ? hsl.l + (1.0f - hsl.l) * std::min(lum, 1.0f) : hsl.l * (1.0f + std::max(lum, -1.0f));
    return FromHsl(hsl);
  });
}

// ------------------------------------------------------------------ adjust ----

void ApplyColorAdjust(Layer& layer, const SampledEffect& effect) {
  const auto temperature = std::clamp(Param(effect, "temperature", 0.0f), -100.0f, 100.0f) / 100.0f;
  const auto tint = std::clamp(Param(effect, "tint", 0.0f), -100.0f, 100.0f) / 100.0f;
  const auto vibrance = std::clamp(Param(effect, "vibrance", 0.0f), -100.0f, 100.0f) / 100.0f;
  const auto shadows = std::clamp(Param(effect, "shadows", 0.0f), -100.0f, 100.0f) / 100.0f;
  const auto highlights = std::clamp(Param(effect, "highlights", 0.0f), -100.0f, 100.0f) / 100.0f;
  if (std::abs(temperature) < 1e-6f && std::abs(tint) < 1e-6f && std::abs(vibrance) < 1e-6f &&
      std::abs(shadows) < 1e-6f && std::abs(highlights) < 1e-6f) {
    return;
  }
  // White balance as channel gains: warmer raises red and lowers blue, a positive tint
  // moves toward magenta by lowering green.
  const auto gain_r = 1.0f + 0.25f * temperature;
  const auto gain_g = 1.0f - 0.25f * tint;
  const auto gain_b = 1.0f - 0.25f * temperature;
  ForEachColour(layer, [&](Color c) {
    c.r *= gain_r;
    c.g *= gain_g;
    c.b *= gain_b;
    if (std::abs(vibrance) > 1e-6f) {
      // More for the muted colours than the saturated ones, so skin and sky do not clip.
      const auto maximum = std::max({c.r, c.g, c.b});
      const auto saturation = maximum > 1e-6f ? (maximum - std::min({c.r, c.g, c.b})) / maximum : 0.0f;
      const auto factor = std::max(0.0f, 1.0f + vibrance * (1.0f - saturation));
      const auto l = Luma(c);
      c.r = l + (c.r - l) * factor;
      c.g = l + (c.g - l) * factor;
      c.b = l + (c.b - l) * factor;
    }
    if (std::abs(shadows) > 1e-6f || std::abs(highlights) > 1e-6f) {
      const auto l = std::clamp(Luma(c), 0.0f, 1.0f);
      const auto delta = 0.25f * (shadows * (1.0f - Smoothstep(0.0f, 0.5f, l)) + highlights * Smoothstep(0.5f, 1.0f, l));
      c.r += delta;
      c.g += delta;
      c.b += delta;
    }
    return c;
  });
}

// --------------------------------------------------------------- secondary ----

[[nodiscard]] float Band(float value, float low, float high, float softness) {
  if (softness <= 0.0f) return value >= low && value <= high ? 1.0f : 0.0f;
  return Smoothstep(low - softness, low, value) * (1.0f - Smoothstep(high, high + softness, value));
}

void ApplyHslSecondary(Layer& layer, const SampledEffect& effect) {
  const auto hue_centre = Param(effect, "hue_center", 0.0f);
  const auto hue_width = std::clamp(Param(effect, "hue_width", 360.0f), 0.0f, 360.0f);
  const auto hue_softness = std::clamp(Param(effect, "hue_softness", 0.0f), 0.0f, 180.0f);
  const auto sat_min = Param(effect, "sat_min", 0.0f);
  const auto sat_max = Param(effect, "sat_max", 1.0f);
  const auto sat_softness = std::clamp(Param(effect, "sat_softness", 0.0f), 0.0f, 1.0f);
  const auto luma_min = Param(effect, "luma_min", 0.0f);
  const auto luma_max = Param(effect, "luma_max", 1.0f);
  const auto luma_softness = std::clamp(Param(effect, "luma_softness", 0.0f), 0.0f, 1.0f);
  const auto hue_shift = Param(effect, "hue_shift", 0.0f);
  const auto sat_gain = std::clamp(Param(effect, "sat_gain", 1.0f), 0.0f, 4.0f);
  const auto lightness = std::clamp(Param(effect, "lightness", 0.0f), -1.0f, 1.0f);
  const bool view_matte = Param(effect, "view", 0.0f) >= 0.5f;
  const bool invert = Param(effect, "invert", 0.0f) >= 0.5f;
  const bool corrects = std::abs(hue_shift) > 1e-6f || std::abs(sat_gain - 1.0f) > 1e-6f || std::abs(lightness) > 1e-6f;
  if (!corrects && !view_matte) return;

  ForEachColour(layer, [&](Color c) {
    const auto hsl = ToHsl(c);
    float hue_matte = 1.0f;
    if (hue_width < 359.99f) {
      if (hsl.s < 1e-4f) {
        hue_matte = 0.0f;  // a grey has no hue to be inside a range of
      } else {
        auto distance = std::abs(std::fmod(hsl.h - hue_centre, 360.0f));
        if (distance > 180.0f) distance = 360.0f - distance;
        const auto half = hue_width * 0.5f;
        hue_matte = hue_softness <= 0.0f ? (distance <= half ? 1.0f : 0.0f) : 1.0f - Smoothstep(half, half + hue_softness, distance);
      }
    }
    auto matte = hue_matte * Band(hsl.s, sat_min, sat_max, sat_softness) * Band(hsl.l, luma_min, luma_max, luma_softness);
    if (invert) matte = 1.0f - matte;
    if (view_matte) return Color{matte, matte, matte};
    auto corrected = hsl;
    corrected.h += hue_shift;
    corrected.s = std::clamp(corrected.s * sat_gain, 0.0f, 1.0f);
    corrected.l = lightness >= 0.0f ? corrected.l + (1.0f - corrected.l) * lightness : corrected.l * (1.0f + lightness);
    const auto result = FromHsl(corrected);
    return Color{c.r + (result.r - c.r) * matte, c.g + (result.g - c.g) * matte, c.b + (result.b - c.b) * matte};
  });
}

}  // namespace

void BuildCurveTable(float a, float b, float c, float* table) {
  // Monotone cubic Hermite interpolation (Fritsch-Carlson) through five equally spaced
  // knots. Monotone so that a curve through monotone points never overshoots and
  // inverts part of the tonal range.
  constexpr int kKnots = 5;
  const float x[kKnots] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
  const float y[kKnots] = {0.0f, std::clamp(a, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), std::clamp(c, 0.0f, 1.0f), 1.0f};
  constexpr float h = 0.25f;
  float delta[kKnots - 1];
  for (int k = 0; k < kKnots - 1; ++k) delta[k] = (y[k + 1] - y[k]) / h;
  float m[kKnots];
  const auto sign = [](float v) { return v > 0.0f ? 1 : (v < 0.0f ? -1 : 0); };
  for (int k = 1; k < kKnots - 1; ++k) {
    m[k] = sign(delta[k - 1]) * sign(delta[k]) > 0 ? 2.0f * delta[k - 1] * delta[k] / (delta[k - 1] + delta[k]) : 0.0f;
  }
  // Ends: the one-sided three-point estimate, kept monotone.
  const auto end_slope = [&](float d0, float d1) {
    auto slope = (3.0f * d0 - d1) * 0.5f;
    if (sign(slope) != sign(d0)) return 0.0f;
    if (sign(d0) != sign(d1) && std::abs(slope) > 3.0f * std::abs(d0)) return 3.0f * d0;
    return slope;
  };
  m[0] = end_slope(delta[0], delta[1]);
  m[kKnots - 1] = end_slope(delta[kKnots - 2], delta[kKnots - 3]);

  for (int i = 0; i < 256; ++i) {
    const auto input = static_cast<float>(i) / 255.0f;
    int segment = std::min(static_cast<int>(input / h), kKnots - 2);
    const auto t = (input - x[segment]) / h;
    const auto t2 = t * t;
    const auto t3 = t2 * t;
    const auto value = (2.0f * t3 - 3.0f * t2 + 1.0f) * y[segment] + (t3 - 2.0f * t2 + t) * h * m[segment] +
                       (-2.0f * t3 + 3.0f * t2) * y[segment + 1] + (t3 - t2) * h * m[segment + 1];
    table[i] = std::clamp(value, 0.0f, 1.0f);
  }
}

// -------------------------------------------------------------- on the card ----

bool IsGpuColorEffect(const std::string& type) {
  return type == "color_wheels" || type == "curves" || type == "channel_mixer" || type == "tint" || type == "black_and_white" || type == "color_adjust" ||
         type == "hue_curves" || type == "hsl_secondary";
}

std::optional<GpuColorOp> DescribeColorOp(const SampledEffect& effect) {
  const auto& type = effect.effect_type;
  GpuColorOp op;
  auto& b = op.block;
  const auto put3 = [&](int row, int at, const std::array<float, 3>& v) {
    for (int i = 0; i < 3; ++i) b[row][at + i] = v[static_cast<std::size_t>(i)];
  };
  if (type == "color_wheels") {
    // The same derivation as ApplyColorWheels, including its idea of neutral.
    const auto lift = Vec4(effect, "lift", 0.0f, 0.0f, 0.0f, 0.0f);
    const auto gamma = Vec4(effect, "gamma", 1.0f, 1.0f, 1.0f, 1.0f);
    const auto gain = Vec4(effect, "gain", 1.0f, 1.0f, 1.0f, 1.0f);
    const auto shadows = Vec3(effect, "shadows", 0.0f, 0.0f, 0.0f);
    const auto midtones = Vec3(effect, "midtones", 0.0f, 0.0f, 0.0f);
    const auto highlights = Vec3(effect, "highlights", 0.0f, 0.0f, 0.0f);
    bool identity = true;
    std::array<float, 3> lifts{}, inverse_gammas{}, gains{};
    for (std::size_t i = 0; i < 3; ++i) {
      lifts[i] = std::clamp(lift[i] + lift[3], -1.0f, 1.0f);
      const auto g = std::clamp(gamma[i] * gamma[3], 0.1f, 4.0f);
      inverse_gammas[i] = 1.0f / g;
      gains[i] = std::clamp(gain[i] * gain[3], 0.0f, 4.0f);
      if (std::abs(lifts[i]) > 1e-6f || std::abs(g - 1.0f) > 1e-6f || std::abs(gains[i] - 1.0f) > 1e-6f || std::abs(shadows[i]) > 1e-6f ||
          std::abs(midtones[i]) > 1e-6f || std::abs(highlights[i]) > 1e-6f) {
        identity = false;
      }
    }
    if (identity) return std::nullopt;
    op.kind = GpuColorKind::ColorWheels;
    put3(0, 1, lifts);
    put3(1, 0, inverse_gammas);
    put3(2, 0, gains);
    put3(3, 0, shadows);
    put3(4, 0, midtones);
    put3(5, 0, highlights);
    return op;
  }
  if (type == "curves") {
    const char* const names[5] = {"master", "red", "green", "blue", "luma"};
    int flags = 0;
    op.tables.assign(5 * 256, 0.0f);
    for (int i = 0; i < 5; ++i) {
      const auto points = Vec3(effect, names[i], 0.25f, 0.5f, 0.75f);
      BuildCurveTable(points[0], points[1], points[2], op.tables.data() + static_cast<std::size_t>(i) * 256);
      if (!IsDefaultCurve(points)) flags |= 1 << i;
    }
    if (flags == 0) return std::nullopt;
    op.kind = GpuColorKind::Curves;
    b[0][1] = static_cast<float>(flags);
    return op;
  }
  if (type == "channel_mixer") {
    // Filters.cpp: no neutral shortcut, the tool also clamps what it touches.
    put3(0, 1, Vec3(effect, "red", 1.0f, 0.0f, 0.0f));
    put3(1, 0, Vec3(effect, "green", 0.0f, 1.0f, 0.0f));
    put3(2, 0, Vec3(effect, "blue", 0.0f, 0.0f, 1.0f));
    op.kind = GpuColorKind::ChannelMixer;
    return op;
  }
  if (type == "black_and_white") {
    put3(0, 1, Vec3(effect, "weights", kLumaR, kLumaG, kLumaB));
    b[1][0] = std::clamp(Param(effect, "amount", 1.0f), 0.0f, 1.0f);
    op.kind = GpuColorKind::BlackAndWhite;
    return op;
  }
  if (type == "tint") {
    const auto amount = std::clamp(Param(effect, "amount", 1.0f), 0.0f, 1.0f);
    if (amount <= 1e-6f) return std::nullopt;
    put3(0, 1, Vec3(effect, "map_black", 0.0f, 0.0f, 0.0f));
    put3(1, 0, Vec3(effect, "map_white", 1.0f, 1.0f, 1.0f));
    b[1][3] = amount;
    op.kind = GpuColorKind::Tint;
    return op;
  }
  if (type == "hue_curves") {
    const auto hue_shift = Knots(effect, "hue_vs_hue");
    const auto saturation = Knots(effect, "hue_vs_sat");
    const auto lightness = Knots(effect, "hue_vs_luma");
    if (AllZero(hue_shift) && AllZero(saturation) && AllZero(lightness)) return std::nullopt;
    // Eighteen knots after the kind: hue shift, saturation, lightness, six each, in the block's order.
    float* flat = &b[0][0];
    for (std::size_t i = 0; i < 6; ++i) {
      flat[1 + i] = hue_shift[i];
      flat[7 + i] = saturation[i];
      flat[13 + i] = lightness[i];
    }
    op.kind = GpuColorKind::HueCurves;
    return op;
  }
  if (type == "hsl_secondary") {
    const auto hue_width = std::clamp(Param(effect, "hue_width", 360.0f), 0.0f, 360.0f);
    const auto sat_gain = std::clamp(Param(effect, "sat_gain", 1.0f), 0.0f, 4.0f);
    const auto lightness = std::clamp(Param(effect, "lightness", 0.0f), -1.0f, 1.0f);
    const auto hue_shift = Param(effect, "hue_shift", 0.0f);
    const bool view_matte = Param(effect, "view", 0.0f) >= 0.5f;
    const bool corrects = std::abs(hue_shift) > 1e-6f || std::abs(sat_gain - 1.0f) > 1e-6f || std::abs(lightness) > 1e-6f;
    if (!corrects && !view_matte) return std::nullopt;
    float* flat = &b[0][0];
    flat[1] = Param(effect, "hue_center", 0.0f);
    flat[2] = hue_width;
    flat[3] = std::clamp(Param(effect, "hue_softness", 0.0f), 0.0f, 180.0f);
    flat[4] = Param(effect, "sat_min", 0.0f);
    flat[5] = Param(effect, "sat_max", 1.0f);
    flat[6] = std::clamp(Param(effect, "sat_softness", 0.0f), 0.0f, 1.0f);
    flat[7] = Param(effect, "luma_min", 0.0f);
    flat[8] = Param(effect, "luma_max", 1.0f);
    flat[9] = std::clamp(Param(effect, "luma_softness", 0.0f), 0.0f, 1.0f);
    flat[10] = hue_shift;
    flat[11] = sat_gain;
    flat[12] = lightness;
    flat[13] = view_matte ? 1.0f : 0.0f;
    flat[14] = Param(effect, "invert", 0.0f) >= 0.5f ? 1.0f : 0.0f;
    op.kind = GpuColorKind::HslSecondary;
    return op;
  }
  if (type == "color_adjust") {
    const auto temperature = std::clamp(Param(effect, "temperature", 0.0f), -100.0f, 100.0f) / 100.0f;
    const auto tint = std::clamp(Param(effect, "tint", 0.0f), -100.0f, 100.0f) / 100.0f;
    const auto vibrance = std::clamp(Param(effect, "vibrance", 0.0f), -100.0f, 100.0f) / 100.0f;
    const auto shadows = std::clamp(Param(effect, "shadows", 0.0f), -100.0f, 100.0f) / 100.0f;
    const auto highlights = std::clamp(Param(effect, "highlights", 0.0f), -100.0f, 100.0f) / 100.0f;
    if (std::abs(temperature) < 1e-6f && std::abs(tint) < 1e-6f && std::abs(vibrance) < 1e-6f && std::abs(shadows) < 1e-6f && std::abs(highlights) < 1e-6f) {
      return std::nullopt;
    }
    b[0][1] = 1.0f + 0.25f * temperature;
    b[0][2] = 1.0f - 0.25f * tint;
    b[0][3] = 1.0f - 0.25f * temperature;
    b[1][0] = vibrance;
    b[1][1] = shadows;
    b[1][2] = highlights;
    op.kind = GpuColorKind::ColorAdjust;
    return op;
  }
  return std::nullopt;
}

bool ApplyColorEffect(Layer& layer, Layer&, const SampledEffect& effect) {
  const auto& type = effect.effect_type;
  if (type == "color_wheels") ApplyColorWheels(layer, effect);
  else if (type == "curves") ApplyCurves(layer, effect);
  else if (type == "hue_curves") ApplyHueCurves(layer, effect);
  else if (type == "color_adjust") ApplyColorAdjust(layer, effect);
  else if (type == "hsl_secondary") ApplyHslSecondary(layer, effect);
  else return false;
  return true;
}

}  // namespace cutline::render

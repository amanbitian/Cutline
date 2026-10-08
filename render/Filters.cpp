#include "render/Filters.h"
#include "render/MeshWarp.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

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

[[nodiscard]] Pixel Premultiplied(const Color& colour, float alpha) {
  return {colour.r * alpha, colour.g * alpha, colour.b * alpha, alpha};
}

[[nodiscard]] float Luma(const Color& c) { return c.r * kLumaR + c.g * kLumaG + c.b * kLumaB; }

[[nodiscard]] float Smoothstep(float edge0, float edge1, float value) {
  if (edge1 <= edge0) return value >= edge1 ? 1.0f : 0.0f;
  const auto t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] std::array<float, 3> Vec3Param(const SampledEffect& effect, const std::string& name, float x, float y,
                                             float z) {
  const auto value = FindParameter(effect, name);
  if (!value.has_value()) return {x, y, z};
  return {static_cast<float>(value->components[0]), static_cast<float>(value->components[1]),
          static_cast<float>(value->components[2])};
}

[[nodiscard]] std::array<float, 4> Vec4Param(const SampledEffect& effect, const std::string& name, float x, float y,
                                             float z, float w) {
  const auto value = FindParameter(effect, name);
  if (!value.has_value()) return {x, y, z, w};
  return {static_cast<float>(value->components[0]), static_cast<float>(value->components[1]),
          static_cast<float>(value->components[2]), static_cast<float>(value->components[3])};
}

void CopyInto(const Layer& source, Layer& destination) {
  destination.Reset(source.width(), source.height());
  if (source.empty()) return;
  ParallelRows(source.min_y(), source.max_y(), [&](int y) {
    for (int x = source.min_x(); x <= source.max_x(); ++x) destination.at(x, y) = source.at(x, y);
  });
  destination.MarkDirty(source.min_x(), source.min_y(), source.max_x(), source.max_y());
}

// ---------------------------------------------------------------- blurring ----

// One box of the given radius over a line, treating everything beyond its ends as
// transparent. Double accumulators keep a long window from drifting.
void BoxLine(const Pixel* in, Pixel* out, int count, int radius) {
  double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_a = 0.0;
  const auto add = [&](int index, double sign) {
    if (index < 0 || index >= count) return;
    sum_r += sign * in[index].r;
    sum_g += sign * in[index].g;
    sum_b += sign * in[index].b;
    sum_a += sign * in[index].a;
  };
  for (int index = -radius; index <= radius; ++index) add(index, 1.0);
  const auto inverse = 1.0 / static_cast<double>(2 * radius + 1);
  for (int index = 0; index < count; ++index) {
    out[index] = {static_cast<float>(sum_r * inverse), static_cast<float>(sum_g * inverse),
                  static_cast<float>(sum_b * inverse), static_cast<float>(sum_a * inverse)};
    add(index - radius, -1.0);
    add(index + radius + 1, 1.0);
  }
}

void BoxBlurHorizontal(Layer& layer, int radius) {
  if (radius <= 0 || layer.empty()) return;
  const auto lo = std::max(0, layer.min_x() - radius);
  const auto hi = std::min(layer.width() - 1, layer.max_x() + radius);
  const auto y0 = layer.min_y();
  const auto y1 = layer.max_y();
  const auto count = hi - lo + 1;
  ParallelRows(y0, y1, [&](int y) {
    thread_local std::vector<Pixel> in, out;
    in.resize(static_cast<std::size_t>(count));
    out.resize(static_cast<std::size_t>(count));
    for (int x = 0; x < count; ++x) in[static_cast<std::size_t>(x)] = layer.at(lo + x, y);
    BoxLine(in.data(), out.data(), count, radius);
    for (int x = 0; x < count; ++x) layer.at(lo + x, y) = out[static_cast<std::size_t>(x)];
  });
  layer.MarkDirty(lo, y0, hi, y1);
}

void BoxBlurVertical(Layer& layer, int radius) {
  if (radius <= 0 || layer.empty()) return;
  const auto lo = std::max(0, layer.min_y() - radius);
  const auto hi = std::min(layer.height() - 1, layer.max_y() + radius);
  const auto x0 = layer.min_x();
  const auto x1 = layer.max_x();
  const auto count = hi - lo + 1;
  ParallelRows(x0, x1, [&](int x) {
    thread_local std::vector<Pixel> in, out;
    in.resize(static_cast<std::size_t>(count));
    out.resize(static_cast<std::size_t>(count));
    for (int y = 0; y < count; ++y) in[static_cast<std::size_t>(y)] = layer.at(x, lo + y);
    BoxLine(in.data(), out.data(), count, radius);
    for (int y = 0; y < count; ++y) layer.at(x, lo + y) = out[static_cast<std::size_t>(y)];
  });
  layer.MarkDirty(x0, lo, x1, hi);
}

// Box radii whose three successive passes have the variance of a gaussian of sigma. A
// box of odd width w has variance (w^2 - 1) / 12 and the variances of successive passes
// add, so this picks, from the odd widths around the ideal one, the triple whose total
// is closest to sigma^2 (the choice is deterministic: the first of equals wins).
[[nodiscard]] std::array<int, 3> GaussianRadii(float sigma) {
  const auto target = static_cast<double>(sigma) * sigma;
  const auto ideal = std::sqrt(12.0 * target / 3.0 + 1.0);
  auto centre = static_cast<int>(std::floor(ideal));
  if (centre % 2 == 0) --centre;
  const auto first = std::max(1, centre - 6);
  const auto last = centre + 6;
  double best_error = 1e300;
  std::array<int, 3> best{1, 1, 1};
  for (int a = first; a <= last; a += 2) {
    for (int b = a; b <= last; b += 2) {
      for (int c = b; c <= last; c += 2) {
        const auto variance = (static_cast<double>(a) * a + static_cast<double>(b) * b + static_cast<double>(c) * c - 3.0) / 12.0;
        const auto error = std::abs(variance - target);
        if (error < best_error) {
          best_error = error;
          best = {a, b, c};
        }
      }
    }
  }
  return {(best[0] - 1) / 2, (best[1] - 1) / 2, (best[2] - 1) / 2};
}

}  // namespace

void GaussianBlur(Layer& layer, float sigma) {
  sigma = std::clamp(sigma, 0.0f, 128.0f);
  if (sigma < 0.3f || layer.empty()) return;
  for (const auto radius : GaussianRadii(sigma)) {
    BoxBlurHorizontal(layer, radius);
    BoxBlurVertical(layer, radius);
  }
}

namespace {

// ------------------------------------------------------------------ sampling ----

// Bilinear sample of a premultiplied layer at a continuous position (pixel centres at
// +0.5), transparent outside the canvas.
[[nodiscard]] Pixel SampleLayer(const Layer& layer, float x, float y) {
  const auto fx = x - 0.5f;
  const auto fy = y - 0.5f;
  const auto x0 = static_cast<int>(std::floor(fx));
  const auto y0 = static_cast<int>(std::floor(fy));
  const auto tx = fx - static_cast<float>(x0);
  const auto ty = fy - static_cast<float>(y0);
  const auto fetch = [&](int px, int py) -> Pixel {
    if (px < 0 || py < 0 || px >= layer.width() || py >= layer.height()) return {};
    return layer.at(px, py);
  };
  const auto a = fetch(x0, y0);
  const auto b = fetch(x0 + 1, y0);
  const auto c = fetch(x0, y0 + 1);
  const auto d = fetch(x0 + 1, y0 + 1);
  const auto mix = [&](float va, float vb, float vc, float vd) {
    return (va * (1.0f - tx) + vb * tx) * (1.0f - ty) + (vc * (1.0f - tx) + vd * tx) * ty;
  };
  return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
}

// ------------------------------------------------------------- blur & sharpen ----

void ApplyGaussianBlur(Layer& layer, const SampledEffect& effect) {
  GaussianBlur(layer, ScalarParameter(effect, "radius", 0.0f));
}

void ApplyDirectionalBlur(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto length = std::clamp(ScalarParameter(effect, "length", 0.0f), 0.0f, 256.0f);
  if (length < 1.0f || layer.empty()) return;
  const auto angle = ScalarParameter(effect, "angle", 0.0f) * std::numbers::pi_v<float> / 180.0f;
  const auto dx = std::cos(angle);
  const auto dy = std::sin(angle);
  const auto taps = static_cast<int>(std::lround(length)) + 1;
  const auto reach = static_cast<int>(std::ceil(length * 0.5f)) + 1;
  const auto x0 = std::max(0, layer.min_x() - reach);
  const auto x1 = std::min(layer.width() - 1, layer.max_x() + reach);
  const auto y0 = std::max(0, layer.min_y() - reach);
  const auto y1 = std::min(layer.height() - 1, layer.max_y() + reach);

  scratch.Reset(layer.width(), layer.height());
  ParallelRows(y0, y1, [&](int y) {
    for (int x = x0; x <= x1; ++x) {
      Pixel sum;
      for (int tap = 0; tap < taps; ++tap) {
        const auto offset = (static_cast<float>(tap) / static_cast<float>(taps - 1) - 0.5f) * length;
        const auto sample = SampleLayer(layer, static_cast<float>(x) + 0.5f + dx * offset, static_cast<float>(y) + 0.5f + dy * offset);
        sum.r += sample.r;
        sum.g += sample.g;
        sum.b += sample.b;
        sum.a += sample.a;
      }
      const auto inverse = 1.0f / static_cast<float>(taps);
      scratch.at(x, y) = {sum.r * inverse, sum.g * inverse, sum.b * inverse, sum.a * inverse};
    }
  });
  scratch.MarkDirty(x0, y0, x1, y1);
  CopyInto(scratch, layer);
}

void ApplyUnsharpMask(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto amount = std::clamp(ScalarParameter(effect, "amount", 0.0f), 0.0f, 5.0f);
  const auto radius = std::clamp(ScalarParameter(effect, "radius", 2.0f), 0.0f, 64.0f);
  const auto threshold = std::clamp(ScalarParameter(effect, "threshold", 0.0f), 0.0f, 1.0f);
  if (amount <= 1e-6f || radius < 0.3f || layer.empty()) return;
  CopyInto(layer, scratch);
  GaussianBlur(scratch, radius);
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      // The blur is transparent beyond the canvas, so near its edge it is dimmer than the
      // picture without there being any detail: compare the colours, not the light.
      const auto& blurred_pixel = scratch.at(x, y);
      const auto blurred = Straight(blurred_pixel);
      const auto dr = pixel.r - blurred.r * pixel.a;
      const auto dg = pixel.g - blurred.g * pixel.a;
      const auto db = pixel.b - blurred.b * pixel.a;
      if (std::max({std::abs(dr), std::abs(dg), std::abs(db)}) <= threshold) continue;
      pixel.r = std::clamp(pixel.r + amount * dr, 0.0f, pixel.a);
      pixel.g = std::clamp(pixel.g + amount * dg, 0.0f, pixel.a);
      pixel.b = std::clamp(pixel.b + amount * db, 0.0f, pixel.a);
    }
  });
}

void ApplyGlow(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto threshold = std::clamp(ScalarParameter(effect, "threshold", 0.7f), 0.0f, 1.0f);
  const auto radius = std::clamp(ScalarParameter(effect, "radius", 0.0f), 0.0f, 128.0f);
  const auto intensity = std::clamp(ScalarParameter(effect, "intensity", 0.0f), 0.0f, 4.0f);
  if (intensity <= 1e-6f || radius < 0.3f || layer.empty()) return;

  // The light that glows: what is brighter than the threshold, fading in over the
  // top of the range so the edge of the selection does not show as a contour.
  scratch.Reset(layer.width(), layer.height());
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      const auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      const auto weight = Smoothstep(threshold, std::min(1.0f, threshold + 0.25f), Luma(Straight(pixel)));
      scratch.at(x, y) = {pixel.r * weight, pixel.g * weight, pixel.b * weight, pixel.a * weight};
    }
  });
  scratch.MarkDirty(layer.min_x(), layer.min_y(), layer.max_x(), layer.max_y());
  GaussianBlur(scratch, radius);

  // Screen the glow over the picture: 1 - (1 - a)(1 - b), which cannot exceed full scale.
  ParallelRows(scratch.min_y(), scratch.max_y(), [&](int y) {
    for (int x = scratch.min_x(); x <= scratch.max_x(); ++x) {
      const auto& glow = scratch.at(x, y);
      if (glow.a <= 0.0f && glow.r <= 0.0f && glow.g <= 0.0f && glow.b <= 0.0f) continue;
      auto& pixel = layer.at(x, y);
      const auto screen = [](float base, float light) { return base + light - base * light; };
      pixel.r = screen(pixel.r, std::clamp(glow.r * intensity, 0.0f, 1.0f));
      pixel.g = screen(pixel.g, std::clamp(glow.g * intensity, 0.0f, 1.0f));
      pixel.b = screen(pixel.b, std::clamp(glow.b * intensity, 0.0f, 1.0f));
      pixel.a = screen(pixel.a, std::clamp(glow.a * intensity, 0.0f, 1.0f));
    }
  });
  layer.MarkDirty(scratch.min_x(), scratch.min_y(), scratch.max_x(), scratch.max_y());
}

void ApplyDropShadow(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto opacity = std::clamp(ScalarParameter(effect, "opacity", 0.0f), 0.0f, 1.0f);
  if (opacity <= 1e-6f || layer.empty()) return;
  const auto colour = Vec3Param(effect, "color", 0.0f, 0.0f, 0.0f);
  const auto distance = std::clamp(ScalarParameter(effect, "distance", 10.0f), 0.0f, 500.0f);
  const auto angle = ScalarParameter(effect, "angle", 45.0f) * std::numbers::pi_v<float> / 180.0f;
  const auto softness = std::clamp(ScalarParameter(effect, "softness", 5.0f), 0.0f, 128.0f);
  const auto offset_x = static_cast<int>(std::lround(std::cos(angle) * distance));
  const auto offset_y = static_cast<int>(std::lround(std::sin(angle) * distance));

  scratch.Reset(layer.width(), layer.height());
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    const auto ty = y + offset_y;
    if (ty < 0 || ty >= layer.height()) return;
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      const auto tx = x + offset_x;
      if (tx < 0 || tx >= layer.width()) continue;
      const auto alpha = layer.at(x, y).a * opacity;
      if (alpha <= 0.0f) continue;
      scratch.at(tx, ty) = {colour[0] * alpha, colour[1] * alpha, colour[2] * alpha, alpha};
    }
  });
  scratch.MarkDirty(std::max(0, layer.min_x() + offset_x), std::max(0, layer.min_y() + offset_y),
                    std::min(layer.width() - 1, layer.max_x() + offset_x),
                    std::min(layer.height() - 1, layer.max_y() + offset_y));
  GaussianBlur(scratch, softness);

  // The picture over its shadow.
  ParallelRows(scratch.min_y(), scratch.max_y(), [&](int y) {
    for (int x = scratch.min_x(); x <= scratch.max_x(); ++x) {
      const auto& shadow = scratch.at(x, y);
      auto& pixel = layer.at(x, y);
      const auto keep = 1.0f - pixel.a;
      pixel.r += shadow.r * keep;
      pixel.g += shadow.g * keep;
      pixel.b += shadow.b * keep;
      pixel.a += shadow.a * keep;
    }
  });
  layer.MarkDirty(scratch.min_x(), scratch.min_y(), scratch.max_x(), scratch.max_y());
}

// ----------------------------------------------------------------- per pixel ----

template <typename Function>
void ForEachColour(Layer& layer, Function&& function) {
  // Pure per-pixel work: rows run on every core, and the result is what one thread would make.
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      const auto result = function(Straight(pixel), x, y);
      pixel = Premultiplied({std::clamp(result.r, 0.0f, 1.0f), std::clamp(result.g, 0.0f, 1.0f),
                             std::clamp(result.b, 0.0f, 1.0f)},
                            pixel.a);
    }
  });
}

void ApplyChannelMixer(Layer& layer, const SampledEffect& effect) {
  const auto red = Vec3Param(effect, "red", 1.0f, 0.0f, 0.0f);
  const auto green = Vec3Param(effect, "green", 0.0f, 1.0f, 0.0f);
  const auto blue = Vec3Param(effect, "blue", 0.0f, 0.0f, 1.0f);
  ForEachColour(layer, [&](Color c, int, int) {
    return Color{c.r * red[0] + c.g * red[1] + c.b * red[2], c.r * green[0] + c.g * green[1] + c.b * green[2],
                 c.r * blue[0] + c.g * blue[1] + c.b * blue[2]};
  });
}

void ApplyBlackAndWhite(Layer& layer, const SampledEffect& effect) {
  const auto weights = Vec3Param(effect, "weights", kLumaR, kLumaG, kLumaB);
  const auto amount = std::clamp(ScalarParameter(effect, "amount", 1.0f), 0.0f, 1.0f);
  ForEachColour(layer, [&](Color c, int, int) {
    const auto grey = c.r * weights[0] + c.g * weights[1] + c.b * weights[2];
    return Color{c.r + (grey - c.r) * amount, c.g + (grey - c.g) * amount, c.b + (grey - c.b) * amount};
  });
}

void ApplyTint(Layer& layer, const SampledEffect& effect) {
  const auto black = Vec3Param(effect, "map_black", 0.0f, 0.0f, 0.0f);
  const auto white = Vec3Param(effect, "map_white", 1.0f, 1.0f, 1.0f);
  const auto amount = std::clamp(ScalarParameter(effect, "amount", 1.0f), 0.0f, 1.0f);
  if (amount <= 1e-6f) return;
  ForEachColour(layer, [&](Color c, int, int) {
    const auto l = std::clamp(Luma(c), 0.0f, 1.0f);
    const Color mapped{black[0] + (white[0] - black[0]) * l, black[1] + (white[1] - black[1]) * l,
                       black[2] + (white[2] - black[2]) * l};
    return Color{c.r + (mapped.r - c.r) * amount, c.g + (mapped.g - c.g) * amount, c.b + (mapped.b - c.b) * amount};
  });
}

void ApplyPosterize(Layer& layer, const SampledEffect& effect) {
  const auto levels = static_cast<int>(std::lround(std::clamp(ScalarParameter(effect, "levels", 256.0f), 2.0f, 256.0f)));
  if (levels >= 256) return;  // 256 is the picture's own precision: nothing to remove
  const auto steps = static_cast<float>(levels - 1);
  const auto snap = [steps](float value) { return std::round(std::clamp(value, 0.0f, 1.0f) * steps) / steps; };
  ForEachColour(layer, [&](Color c, int, int) { return Color{snap(c.r), snap(c.g), snap(c.b)}; });
}

// ------------------------------------------------------------------- warping ----

void ApplyWaveWarp(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto amplitude = std::clamp(ScalarParameter(effect, "amplitude", 0.0f), 0.0f, 200.0f);
  if (amplitude < 0.01f || layer.empty()) return;
  const auto wavelength = std::clamp(ScalarParameter(effect, "wavelength", 100.0f), 4.0f, 4000.0f);
  const auto phase = ScalarParameter(effect, "phase", 0.0f) * std::numbers::pi_v<float> / 180.0f;
  const bool vertical = ScalarParameter(effect, "vertical", 0.0f) >= 0.5f;
  const auto reach = static_cast<int>(std::ceil(amplitude)) + 1;
  const auto x0 = std::max(0, layer.min_x() - (vertical ? 0 : reach));
  const auto x1 = std::min(layer.width() - 1, layer.max_x() + (vertical ? 0 : reach));
  const auto y0 = std::max(0, layer.min_y() - (vertical ? reach : 0));
  const auto y1 = std::min(layer.height() - 1, layer.max_y() + (vertical ? reach : 0));

  scratch.Reset(layer.width(), layer.height());
  const auto tau = 2.0f * std::numbers::pi_v<float>;
  ParallelRows(y0, y1, [&](int y) {
    for (int x = x0; x <= x1; ++x) {
      const auto cx = static_cast<float>(x) + 0.5f;
      const auto cy = static_cast<float>(y) + 0.5f;
      // Output (x, y) reads the picture where the wave has moved it from.
      const auto sx = vertical ? cx : cx + amplitude * std::sin(tau * cy / wavelength + phase);
      const auto sy = vertical ? cy + amplitude * std::sin(tau * cx / wavelength + phase) : cy;
      scratch.at(x, y) = SampleLayer(layer, sx, sy);
    }
  });
  scratch.MarkDirty(x0, y0, x1, y1);
  CopyInto(scratch, layer);
}

void ApplyBulge(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto amount = std::clamp(ScalarParameter(effect, "amount", 0.0f), -1.0f, 1.0f);
  if (std::abs(amount) < 1e-4f || layer.empty()) return;
  const auto centre = Vec2Parameter(effect, "center", 0.5f, 0.5f);
  const auto radius_fraction = std::clamp(ScalarParameter(effect, "radius", 0.5f), 0.01f, 2.0f);
  const auto cx = centre[0] * static_cast<float>(layer.width());
  const auto cy = centre[1] * static_cast<float>(layer.height());
  const auto radius = radius_fraction * static_cast<float>(std::min(layer.width(), layer.height())) * 0.5f;

  const auto x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
  const auto x1 = std::min(layer.width() - 1, static_cast<int>(std::ceil(cx + radius)));
  const auto y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
  const auto y1 = std::min(layer.height() - 1, static_cast<int>(std::ceil(cy + radius)));
  if (x1 < x0 || y1 < y0) return;
  CopyInto(layer, scratch);
  ParallelRows(y0, y1, [&](int y) {
    for (int x = x0; x <= x1; ++x) {
      const auto dx = static_cast<float>(x) + 0.5f - cx;
      const auto dy = static_cast<float>(y) + 0.5f - cy;
      const auto distance = std::sqrt(dx * dx + dy * dy);
      if (distance >= radius) continue;
      // Positive amounts magnify the middle: an output pixel near the centre reads
      // from closer in than it is. The mapping is the identity at the rim.
      const auto falloff = 1.0f - distance / radius;
      const auto source_scale = 1.0f - amount * falloff * falloff;
      layer.at(x, y) = SampleLayer(scratch, cx + dx * source_scale, cy + dy * source_scale);
    }
  });
  layer.MarkDirty(x0, y0, x1, y1);
}

// ------------------------------------------------------------------- keying ----

// Shrinks (positive) or grows (negative) the matte held in a layer's alpha channel by
// a square window of the given radius: a minimum or maximum filter, separably.
void MorphAlpha(Layer& matte, int radius) {
  if (radius == 0 || matte.empty()) return;
  const bool erode = radius > 0;
  const auto r = std::abs(radius);
  const auto x0 = std::max(0, matte.min_x() - (erode ? 0 : r));
  const auto x1 = std::min(matte.width() - 1, matte.max_x() + (erode ? 0 : r));
  const auto y0 = std::max(0, matte.min_y() - (erode ? 0 : r));
  const auto y1 = std::min(matte.height() - 1, matte.max_y() + (erode ? 0 : r));
  const auto pick = [erode](float a, float b) { return erode ? std::min(a, b) : std::max(a, b); };
  ParallelRows(y0, y1, [&](int y) {
    thread_local std::vector<float> line, filtered;
    line.assign(static_cast<std::size_t>(x1 - x0 + 1), 0.0f);
    for (int x = x0; x <= x1; ++x) line[static_cast<std::size_t>(x - x0)] = matte.at(x, y).a;
    filtered = line;
    for (int x = x0; x <= x1; ++x) {
      // Beyond the canvas the matte is empty: an eroding window sees zero there.
      auto value = line[static_cast<std::size_t>(x - x0)];
      for (int tap = -r; tap <= r; ++tap) {
        const auto position = x + tap;
        const auto sample = position < x0 || position > x1 ? 0.0f : line[static_cast<std::size_t>(position - x0)];
        value = pick(value, sample);
      }
      filtered[static_cast<std::size_t>(x - x0)] = value;
    }
    for (int x = x0; x <= x1; ++x) matte.at(x, y).a = filtered[static_cast<std::size_t>(x - x0)];
  });
  ParallelRows(x0, x1, [&](int x) {
    thread_local std::vector<float> line, filtered;
    line.assign(static_cast<std::size_t>(y1 - y0 + 1), 0.0f);
    for (int y = y0; y <= y1; ++y) line[static_cast<std::size_t>(y - y0)] = matte.at(x, y).a;
    filtered = line;
    for (int y = y0; y <= y1; ++y) {
      auto value = line[static_cast<std::size_t>(y - y0)];
      for (int tap = -r; tap <= r; ++tap) {
        const auto position = y + tap;
        const auto sample = position < y0 || position > y1 ? 0.0f : line[static_cast<std::size_t>(position - y0)];
        value = pick(value, sample);
      }
      filtered[static_cast<std::size_t>(y - y0)] = value;
    }
    for (int y = y0; y <= y1; ++y) matte.at(x, y).a = filtered[static_cast<std::size_t>(y - y0)];
  });
  matte.MarkDirty(x0, y0, x1, y1);
}

// What every key shares once it has a raw matte in `matte`'s alpha: the garbage and
// core rectangles, cleanup, and either the keyed picture or the matte to look at.
struct KeyCleanup final {
  std::array<float, 4> garbage{0.0f, 0.0f, 1.0f, 1.0f};  // outside this rectangle is always keyed out
  std::array<float, 4> core{0.0f, 0.0f, 0.0f, 0.0f};     // inside this rectangle is always kept
  float shrink{0.0f};                                    // pixels; negative grows the matte
  float feather{0.0f};                                   // pixels of blur on the matte edge
  bool view_matte{false};
};

[[nodiscard]] KeyCleanup ReadCleanup(const SampledEffect& effect) {
  KeyCleanup cleanup;
  cleanup.garbage = Vec4Param(effect, "garbage", 0.0f, 0.0f, 1.0f, 1.0f);
  cleanup.core = Vec4Param(effect, "core", 0.0f, 0.0f, 0.0f, 0.0f);
  cleanup.shrink = std::clamp(ScalarParameter(effect, "shrink", 0.0f), -20.0f, 20.0f);
  cleanup.feather = std::clamp(ScalarParameter(effect, "feather", 0.0f), 0.0f, 64.0f);
  cleanup.view_matte = ScalarParameter(effect, "view", 0.0f) >= 0.5f;
  return cleanup;
}

void ApplyMatte(Layer& layer, Layer& matte, const KeyCleanup& cleanup,
                const std::function<Color(Color)>& recolour) {
  const auto w = static_cast<float>(layer.width());
  const auto h = static_cast<float>(layer.height());
  const auto inside = [&](const std::array<float, 4>& rect, int x, int y) {
    const auto u = (static_cast<float>(x) + 0.5f) / w;
    const auto v = (static_cast<float>(y) + 0.5f) / h;
    return u >= rect[0] && u < rect[2] && v >= rect[1] && v < rect[3];
  };
  const bool has_garbage = cleanup.garbage[0] > 0.0f || cleanup.garbage[1] > 0.0f || cleanup.garbage[2] < 1.0f ||
                           cleanup.garbage[3] < 1.0f;
  if (has_garbage) {
    ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
      for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
        if (!inside(cleanup.garbage, x, y)) matte.at(x, y).a = 0.0f;
      }
    });
  }
  MorphAlpha(matte, static_cast<int>(std::lround(cleanup.shrink)));
  GaussianBlur(matte, cleanup.feather);
  const bool has_core = cleanup.core[2] > cleanup.core[0] && cleanup.core[3] > cleanup.core[1];

  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      auto alpha = matte.at(x, y).a;
      if (has_core && inside(cleanup.core, x, y)) alpha = 1.0f;
      alpha = std::clamp(alpha, 0.0f, 1.0f);
      if (cleanup.view_matte) {
        pixel = {alpha * pixel.a, alpha * pixel.a, alpha * pixel.a, pixel.a};
        continue;
      }
      const auto colour = recolour(Straight(pixel));
      pixel = Premultiplied({std::clamp(colour.r, 0.0f, 1.0f), std::clamp(colour.g, 0.0f, 1.0f),
                             std::clamp(colour.b, 0.0f, 1.0f)},
                            pixel.a * alpha);
    }
  });
}

// How far a colour is from the key colour, ignoring brightness: both are scaled so their
// brightest channel is one, and the distance between the results is taken (0 for the
// same hue and saturation, about 1 for opposite). That is what makes one key hold across
// a lit and a shadowed part of the same screen. Near black the ratio is noise, and a
// pixel that dark is called far from the key (kept) rather than keyed on that noise.
[[nodiscard]] float KeyDistance(const Color& c, const Color& key_normalised) {
  const auto brightest = std::max({c.r, c.g, c.b});
  if (brightest < 0.02f) return 1.0f;
  const auto inverse = 1.0f / brightest;
  const auto dr = c.r * inverse - key_normalised.r;
  const auto dg = c.g * inverse - key_normalised.g;
  const auto db = c.b * inverse - key_normalised.b;
  return std::sqrt(dr * dr + dg * dg + db * db) * 0.70710678f;
}

// Pulls the key's dominant channel down toward the mean of the other two wherever it
// exceeds them: green spill on a skin tone, blue on a white shirt.
[[nodiscard]] Color Despill(Color c, int dominant, float amount) {
  if (amount <= 0.0f) return c;
  float* channels[3] = {&c.r, &c.g, &c.b};
  const auto a = *channels[(dominant + 1) % 3];
  const auto b = *channels[(dominant + 2) % 3];
  const auto limit = (a + b) * 0.5f;
  auto& key = *channels[dominant];
  if (key > limit) key = key + (limit - key) * amount;
  return c;
}

void ApplyChromaKey(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  if (layer.empty()) return;
  const auto key = Vec3Param(effect, "color", 0.0f, 1.0f, 0.0f);
  const auto tolerance = std::clamp(ScalarParameter(effect, "tolerance", 0.25f), 0.0f, 1.0f);
  const auto softness = std::clamp(ScalarParameter(effect, "softness", 0.15f), 0.0f, 1.0f);
  const auto spill = std::clamp(ScalarParameter(effect, "spill", 0.5f), 0.0f, 1.0f);
  const auto cleanup = ReadCleanup(effect);
  const auto key_brightest = std::max({key[0], key[1], key[2], 1e-6f});
  const Color key_normalised{key[0] / key_brightest, key[1] / key_brightest, key[2] / key_brightest};
  const int dominant = key[0] >= key[1] && key[0] >= key[2] ? 0 : (key[1] >= key[2] ? 1 : 2);

  scratch.Reset(layer.width(), layer.height());
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      const auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      const auto distance = KeyDistance(Straight(pixel), key_normalised);
      scratch.at(x, y).a = Smoothstep(tolerance, tolerance + softness, distance);
    }
  });
  scratch.MarkDirty(layer.min_x(), layer.min_y(), layer.max_x(), layer.max_y());
  ApplyMatte(layer, scratch, cleanup, [&](Color c) { return Despill(c, dominant, spill); });
}

void ApplyLumaKey(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  if (layer.empty()) return;
  const auto threshold = std::clamp(ScalarParameter(effect, "threshold", 0.1f), 0.0f, 1.0f);
  const auto softness = std::clamp(ScalarParameter(effect, "softness", 0.1f), 0.0f, 1.0f);
  const bool invert = ScalarParameter(effect, "invert", 0.0f) >= 0.5f;
  const auto cleanup = ReadCleanup(effect);

  scratch.Reset(layer.width(), layer.height());
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      const auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      // By default what is darker than the threshold is keyed out; inverted, what is lighter.
      const auto kept = Smoothstep(threshold, threshold + softness, Luma(Straight(pixel)));
      scratch.at(x, y).a = invert ? 1.0f - kept : kept;
    }
  });
  scratch.MarkDirty(layer.min_x(), layer.min_y(), layer.max_x(), layer.max_y());
  ApplyMatte(layer, scratch, cleanup, [](Color c) { return c; });
}

}  // namespace

const std::vector<std::string>& FilterEffectTypes() {
  static const std::vector<std::string> types{
      "gaussian_blur", "directional_blur", "unsharp_mask", "glow",         "drop_shadow", "channel_mixer",
      "black_and_white", "tint",           "posterize",    "wave_warp",    "bulge",       "chroma_key",
      "luma_key",      "color_wheels",     "curves",       "hue_curves",   "color_adjust", "hsl_secondary",
      "rolling_shutter", "mesh_warp",
  };
  return types;
}

bool ApplyFilterEffect(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto& type = effect.effect_type;
  if (type == "gaussian_blur") ApplyGaussianBlur(layer, effect);
  else if (type == "directional_blur") ApplyDirectionalBlur(layer, scratch, effect);
  else if (type == "unsharp_mask") ApplyUnsharpMask(layer, scratch, effect);
  else if (type == "glow") ApplyGlow(layer, scratch, effect);
  else if (type == "drop_shadow") ApplyDropShadow(layer, scratch, effect);
  else if (type == "channel_mixer") ApplyChannelMixer(layer, effect);
  else if (type == "black_and_white") ApplyBlackAndWhite(layer, effect);
  else if (type == "tint") ApplyTint(layer, effect);
  else if (type == "posterize") ApplyPosterize(layer, effect);
  else if (type == "wave_warp") ApplyWaveWarp(layer, scratch, effect);
  else if (type == "bulge") ApplyBulge(layer, scratch, effect);
  else if (type == "chroma_key") ApplyChromaKey(layer, scratch, effect);
  else if (type == "luma_key") ApplyLumaKey(layer, scratch, effect);
  else if (type == "rolling_shutter") {
    RollingShutterSettings settings;
    settings.horizontal = ScalarParameter(effect, "horizontal", 0.0f);
    settings.vertical = ScalarParameter(effect, "vertical", 0.0f);
    settings.rotation_degrees = ScalarParameter(effect, "rotation", 0.0f);
    settings.curve = ScalarParameter(effect, "curve", 0.0f);
    settings.bottom_to_top = ScalarParameter(effect, "direction", 0.0f) >= 0.5f;
    ApplyRollingShutter(layer, scratch, settings);
  }
  else if (type == "mesh_warp") {
    WarpMesh mesh;
    mesh.columns = 4;
    mesh.rows = 4;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) {
        const auto point = Vec2Parameter(effect, "point_" + std::to_string(row) + "_" + std::to_string(column), 0.0f, 0.0f);
        mesh.offsets.push_back({point[0], point[1]});
      }
    }
    ApplyMeshWarp(layer, scratch, mesh);
  }
  else return false;
  return true;
}

}  // namespace cutline::render

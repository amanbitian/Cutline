#include "render/Compositor.h"
#include "render/BlendModes.h"
#include "render/CubeLut.h"
#include "render/ParallelRows.h"
#include "render/Transitions.h"
#include "render/Filters.h"
#include "render/Graphics.h"
#include "render/CaptionRender.h"
#include "render/ColorManagement.h"
#include "render/CompositorParams.h"
#include "render/Layer.h"
#include "render/NoiseReduction.h"
#include "render/OpticalFlow.h"
#include "core/model/RenderVersion.h"
#include "effects/EffectRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace cutline::render {
namespace {

using media::PixelFormat;
using media::VideoFrame;
using timeline::SampledEffect;
using timeline::SourceRequest;

struct Vec2 final { float x{}; float y{}; };

struct EffectResources final {
  struct LutFailure final {
    std::string message;
    bool existed{false};
    std::filesystem::file_time_type modified{};
  };
  std::string asset_root;
  std::unordered_map<std::string, CubeLut> lut_cache;
  std::unordered_map<std::string, LutFailure> lut_failures;
  std::unordered_map<std::string, graphics::Document> graphics_cache;
  graphics::StillCache stills;
  std::shared_ptr<FlowCache> flow_cache;
};

// Bilinear sample of a straight-alpha RGBA float frame, premultiplied on the way
// out. Outside the frame returns fully transparent, so a scaled-down layer has
// clean edges instead of clamped smear.
[[nodiscard]] Pixel SampleBilinear(const VideoFrame& frame, float x, float y) {
  if (x < -0.5f || y < -0.5f || x > static_cast<float>(frame.width()) - 0.5f ||
      y > static_cast<float>(frame.height()) - 0.5f) {
    return {};
  }
  const auto clamp_x = [&frame](int value) { return std::clamp(value, 0, frame.width() - 1); };
  const auto clamp_y = [&frame](int value) { return std::clamp(value, 0, frame.height() - 1); };

  const auto fx = std::floor(x);
  const auto fy = std::floor(y);
  const auto tx = x - fx;
  const auto ty = y - fy;
  const auto x0 = clamp_x(static_cast<int>(fx));
  const auto x1 = clamp_x(static_cast<int>(fx) + 1);
  const auto y0 = clamp_y(static_cast<int>(fy));
  const auto y1 = clamp_y(static_cast<int>(fy) + 1);

  const auto fetch = [&frame](int px, int py) {
    const auto* row = frame.row_f32(py);
    const auto* texel = row + static_cast<std::size_t>(px) * 4;
    // Premultiply as we read: all interior maths is premultiplied.
    const auto alpha = texel[3];
    return Pixel{texel[0] * alpha, texel[1] * alpha, texel[2] * alpha, alpha};
  };

  const auto p00 = fetch(x0, y0);
  const auto p10 = fetch(x1, y0);
  const auto p01 = fetch(x0, y1);
  const auto p11 = fetch(x1, y1);

  const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  return {lerp(lerp(p00.r, p10.r, tx), lerp(p01.r, p11.r, tx), ty),
          lerp(lerp(p00.g, p10.g, tx), lerp(p01.g, p11.g, tx), ty),
          lerp(lerp(p00.b, p10.b, tx), lerp(p01.b, p11.b, tx), ty),
          lerp(lerp(p00.a, p10.a, tx), lerp(p01.a, p11.a, tx), ty)};
}

// Bilinear sample of a premultiplied layer. Unlike the source-frame sampler,
// this does not need to premultiply on read. Pixels outside the canvas are
// transparent, which gives spatial filters clean edges.
[[nodiscard]] Pixel SampleBilinear(const Layer& layer, float x, float y) {
  if (x < -0.5f || y < -0.5f || x > static_cast<float>(layer.width()) - 0.5f ||
      y > static_cast<float>(layer.height()) - 0.5f) {
    return {};
  }
  const auto clamp_x = [&layer](int value) { return std::clamp(value, 0, layer.width() - 1); };
  const auto clamp_y = [&layer](int value) { return std::clamp(value, 0, layer.height() - 1); };
  const auto fx = std::floor(x);
  const auto fy = std::floor(y);
  const auto tx = x - fx;
  const auto ty = y - fy;
  const auto x0 = clamp_x(static_cast<int>(fx));
  const auto x1 = clamp_x(static_cast<int>(fx) + 1);
  const auto y0 = clamp_y(static_cast<int>(fy));
  const auto y1 = clamp_y(static_cast<int>(fy) + 1);
  const auto& p00 = layer.at(x0, y0);
  const auto& p10 = layer.at(x1, y0);
  const auto& p01 = layer.at(x0, y1);
  const auto& p11 = layer.at(x1, y1);
  const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  return {lerp(lerp(p00.r, p10.r, tx), lerp(p01.r, p11.r, tx), ty),
          lerp(lerp(p00.g, p10.g, tx), lerp(p01.g, p11.g, tx), ty),
          lerp(lerp(p00.b, p10.b, tx), lerp(p01.b, p11.b, tx), ty),
          lerp(lerp(p00.a, p10.a, tx), lerp(p01.a, p11.a, tx), ty)};
}

// ----------------------------------------------------------------- effects ----

void ApplyGrade(Layer& layer, const Grade& grade) {
  if (grade.identity() || layer.empty()) return;
  const auto exposure = std::pow(2.0f, grade.exposure_stops);
  // Rec.709 luma weights, used for the saturation pivot.
  constexpr float kLumaR = 0.2126f;
  constexpr float kLumaG = 0.7152f;
  constexpr float kLumaB = 0.0722f;
  // Temperature is a simple warm/cool tilt: positive warms by lifting red and
  // dropping blue. A real white-balance would go through a chromatic adaptation
  // in a known colour space, which is the colour-managed stage's job.
  const auto warm = grade.temperature / 100.0f;

  // Only the rectangle this layer has written can hold anything to grade.
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      // Fully opaque is the common case, and there the premultiplied value is
      // already the colour: no reciprocal needed.
      const auto opaque = pixel.a >= 1.0f;
      const auto inverse_alpha = opaque ? 1.0f : 1.0f / pixel.a;
      auto red = pixel.r * inverse_alpha;
      auto green = pixel.g * inverse_alpha;
      auto blue = pixel.b * inverse_alpha;

      red *= exposure;
      green *= exposure;
      blue *= exposure;

      red = (red - 0.5f) * grade.contrast + 0.5f;
      green = (green - 0.5f) * grade.contrast + 0.5f;
      blue = (blue - 0.5f) * grade.contrast + 0.5f;

      const auto luma = red * kLumaR + green * kLumaG + blue * kLumaB;
      red = luma + (red - luma) * grade.saturation;
      green = luma + (green - luma) * grade.saturation;
      blue = luma + (blue - luma) * grade.saturation;

      red += warm * 0.1f;
      blue -= warm * 0.1f;

      red = std::clamp(red, 0.0f, 1.0f);
      green = std::clamp(green, 0.0f, 1.0f);
      blue = std::clamp(blue, 0.0f, 1.0f);

      pixel.r = red * pixel.a;
      pixel.g = green * pixel.a;
      pixel.b = blue * pixel.a;
    }
  });
}

void ApplyOpacity(Layer& layer, float opacity) {
  const auto scale = std::clamp(opacity, 0.0f, 1.0f);
  if (std::abs(scale - 1.0f) < 1e-6f || layer.empty()) return;
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      // Premultiplied, so scaling all four components is the whole operation.
      pixel.r *= scale;
      pixel.g *= scale;
      pixel.b *= scale;
      pixel.a *= scale;
    }
  });
}

void CopyLayer(const Layer& source, Layer& destination) {
  destination.Reset(source.width(), source.height());
  if (source.empty()) return;
  ParallelRows(source.min_y(), source.max_y(), [&](int y) {
    for (int x = source.min_x(); x <= source.max_x(); ++x) destination.at(x, y) = source.at(x, y);
  });
  destination.MarkDirty(source.min_x(), source.min_y(), source.max_x(), source.max_y());
}

[[nodiscard]] float SegmentDistance(Vec2 point, Vec2 a, Vec2 b) {
  const auto dx = b.x - a.x;
  const auto dy = b.y - a.y;
  const auto length_squared = dx * dx + dy * dy;
  const auto t = length_squared <= 1e-12f ? 0.0f :
      std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length_squared, 0.0f, 1.0f);
  return std::hypot(point.x - (a.x + dx * t), point.y - (a.y + dy * t));
}

struct PreparedMask final {
  const effects::mask::Document* document{};
  std::vector<Vec2> polygon;
};

[[nodiscard]] std::vector<Vec2> FlattenBezier(const effects::mask::Document& document, int width, int height) {
  std::vector<Vec2> result;
  if (document.points.size() < 3) return result;
  constexpr int kSteps = 12;
  result.reserve(document.points.size() * kSteps);
  for (std::size_t index = 0; index < document.points.size(); ++index) {
    const auto& a = document.points[index];
    const auto& b = document.points[(index + 1) % document.points.size()];
    for (int step = 0; step < kSteps; ++step) {
      const auto t = static_cast<float>(step) / kSteps;
      const auto u = 1.0f - t;
      const auto x = u * u * u * a.x + 3.0f * u * u * t * a.out_x +
                     3.0f * u * t * t * b.in_x + t * t * t * b.x;
      const auto y = u * u * u * a.y + 3.0f * u * u * t * a.out_y +
                     3.0f * u * t * t * b.in_y + t * t * t * b.y;
      result.push_back({static_cast<float>(x * width), static_cast<float>(y * height)});
    }
  }
  return result;
}

[[nodiscard]] float MaskCoverage(const PreparedMask& mask, float x, float y, int width, int height) {
  const auto& document = *mask.document;
  float signed_distance = -std::numeric_limits<float>::infinity();
  if (document.shape == effects::mask::Shape::Rectangle || document.shape == effects::mask::Shape::Ellipse) {
    const auto centre_x = static_cast<float>(document.center_x * width);
    const auto centre_y = static_cast<float>(document.center_y * height);
    const auto radians = static_cast<float>(-document.rotation * 3.14159265358979323846 / 180.0);
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    const auto px = x - centre_x;
    const auto py = y - centre_y;
    const auto local_x = px * cosine - py * sine;
    const auto local_y = px * sine + py * cosine;
    const auto radius_x = std::max(1e-6f, static_cast<float>(document.width * width * 0.5));
    const auto radius_y = std::max(1e-6f, static_cast<float>(document.height * height * 0.5));
    if (document.shape == effects::mask::Shape::Ellipse) {
      signed_distance = (1.0f - std::hypot(local_x / radius_x, local_y / radius_y)) *
                        std::min(radius_x, radius_y);
    } else {
      const auto qx = std::abs(local_x) - radius_x;
      const auto qy = std::abs(local_y) - radius_y;
      const auto outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
      signed_distance = -(outside + std::min(std::max(qx, qy), 0.0f));
    }
  } else if (!mask.polygon.empty()) {
    bool inside = false;
    auto distance = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0, previous = mask.polygon.size() - 1; index < mask.polygon.size(); previous = index++) {
      const auto& a = mask.polygon[previous];
      const auto& b = mask.polygon[index];
      distance = std::min(distance, SegmentDistance({x, y}, a, b));
      if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    signed_distance = inside ? distance : -distance;
  }
  const auto expanded = signed_distance + static_cast<float>(document.expansion);
  auto coverage = document.feather <= 0.0 ? (expanded >= 0.0f ? 1.0f : 0.0f)
                                         : std::clamp(0.5f + expanded / static_cast<float>(document.feather), 0.0f, 1.0f);
  if (document.inverted) coverage = 1.0f - coverage;
  return coverage * static_cast<float>(document.opacity);
}

void BlendMaskedEffect(const Layer& before, Layer& after, const std::vector<timeline::SampledMask>& masks) {
  std::vector<PreparedMask> prepared;
  prepared.reserve(masks.size());
  for (const auto& mask : masks) {
    prepared.push_back({&mask.document, mask.document.shape == effects::mask::Shape::Bezier
                                           ? FlattenBezier(mask.document, after.width(), after.height())
                                           : std::vector<Vec2>{}});
  }
  for (int y = 0; y < after.height(); ++y) {
    for (int x = 0; x < after.width(); ++x) {
      float combined = 0.0f;
      bool first = true;
      for (const auto& mask : prepared) {
        const auto coverage = MaskCoverage(mask, x + 0.5f, y + 0.5f, after.width(), after.height());
        if (first) {
          combined = mask.document->combine == effects::mask::Combine::Subtract ? 1.0f - coverage : coverage;
          first = false;
        } else if (mask.document->combine == effects::mask::Combine::Add) {
          combined = std::max(combined, coverage);
        } else if (mask.document->combine == effects::mask::Combine::Subtract) {
          combined *= 1.0f - coverage;
        } else {
          combined *= coverage;
        }
      }
      const auto& original = before.at(x, y);
      auto& changed = after.at(x, y);
      changed = {original.r + (changed.r - original.r) * combined,
                 original.g + (changed.g - original.g) * combined,
                 original.b + (changed.b - original.b) * combined,
                 original.a + (changed.a - original.a) * combined};
    }
  }
  after.MarkWhole();
}

// Separable box blur. A sliding window makes the cost O(width * height), rather
// than growing with radius. The radius is bounded so a corrupt project cannot
// turn one frame into an unbounded amount of work.
void ApplyBlur(Layer& layer, Layer& scratch, float requested_radius) {
  const auto radius = std::clamp(static_cast<int>(std::lround(requested_radius)), 0, 128);
  if (radius == 0 || layer.empty()) return;

  const auto source_x0 = layer.min_x();
  const auto source_x1 = layer.max_x();
  const auto source_y0 = layer.min_y();
  const auto source_y1 = layer.max_y();
  const auto horizontal_x0 = std::max(0, source_x0 - radius);
  const auto horizontal_x1 = std::min(layer.width() - 1, source_x1 + radius);
  const auto output_y0 = std::max(0, source_y0 - radius);
  const auto output_y1 = std::min(layer.height() - 1, source_y1 + radius);
  const auto divisor = 1.0f / static_cast<float>(radius * 2 + 1);
  const auto add = [](Pixel& sum, const Pixel& value, float sign) {
    sum.r += value.r * sign;
    sum.g += value.g * sign;
    sum.b += value.b * sign;
    sum.a += value.a * sign;
  };
  const auto read_layer = [&layer](int x, int y) -> Pixel {
    if (x < 0 || x >= layer.width() || y < 0 || y >= layer.height()) return {};
    return layer.at(x, y);
  };

  scratch.Reset(layer.width(), layer.height());
  for (int y = source_y0; y <= source_y1; ++y) {
    Pixel sum;
    for (int tap = -radius; tap <= radius; ++tap) add(sum, read_layer(horizontal_x0 + tap, y), 1.0f);
    for (int x = horizontal_x0; x <= horizontal_x1; ++x) {
      scratch.at(x, y) = {sum.r * divisor, sum.g * divisor, sum.b * divisor, sum.a * divisor};
      add(sum, read_layer(x - radius, y), -1.0f);
      add(sum, read_layer(x + radius + 1, y), 1.0f);
    }
  }
  scratch.MarkDirty(horizontal_x0, source_y0, horizontal_x1, source_y1);

  const auto read_scratch = [&scratch](int x, int y) -> Pixel {
    if (x < 0 || x >= scratch.width() || y < 0 || y >= scratch.height()) return {};
    return scratch.at(x, y);
  };
  layer.Reset(layer.width(), layer.height());
  for (int x = horizontal_x0; x <= horizontal_x1; ++x) {
    Pixel sum;
    for (int tap = -radius; tap <= radius; ++tap) add(sum, read_scratch(x, output_y0 + tap), 1.0f);
    for (int y = output_y0; y <= output_y1; ++y) {
      layer.at(x, y) = {sum.r * divisor, sum.g * divisor, sum.b * divisor, sum.a * divisor};
      add(sum, read_scratch(x, y - radius), -1.0f);
      add(sum, read_scratch(x, y + radius + 1), 1.0f);
    }
  }
  layer.MarkDirty(horizontal_x0, output_y0, horizontal_x1, output_y1);
}

void ApplySharpen(Layer& layer, Layer& scratch, float requested_amount) {
  const auto amount = std::clamp(requested_amount, 0.0f, 5.0f);
  if (amount <= 1e-6f || layer.empty()) return;
  const auto x0 = layer.min_x();
  const auto x1 = layer.max_x();
  const auto y0 = layer.min_y();
  const auto y1 = layer.max_y();
  scratch.Reset(layer.width(), layer.height());
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const auto& centre = layer.at(x, y);
      const auto& left = layer.at(std::max(x0, x - 1), y);
      const auto& right = layer.at(std::min(x1, x + 1), y);
      const auto& above = layer.at(x, std::max(y0, y - 1));
      const auto& below = layer.at(x, std::min(y1, y + 1));
      const auto sharpen = [amount](float value, float a, float b, float c, float d, float maximum) {
        const auto neighbourhood = (a + b + c + d) * 0.25f;
        return std::clamp(value + amount * (value - neighbourhood), 0.0f, maximum);
      };
      scratch.at(x, y) = {sharpen(centre.r, left.r, right.r, above.r, below.r, centre.a),
                          sharpen(centre.g, left.g, right.g, above.g, below.g, centre.a),
                          sharpen(centre.b, left.b, right.b, above.b, below.b, centre.a), centre.a};
    }
  }
  scratch.MarkDirty(x0, y0, x1, y1);
  CopyLayer(scratch, layer);
}

void ApplyVignette(Layer& layer, const SampledEffect& effect) {
  const auto amount = std::clamp(ScalarParameter(effect, "amount", 0.0f), 0.0f, 1.0f);
  if (amount <= 1e-6f || layer.empty()) return;
  const auto midpoint = std::clamp(ScalarParameter(effect, "midpoint", 0.5f), 0.0f, 1.0f);
  const auto feather = std::max(ScalarParameter(effect, "feather", 0.5f), 1e-4f);
  const auto centre = Vec2Parameter(effect, "center", 0.5f, 0.5f);
  const auto smoothstep = [](float edge0, float edge1, float value) {
    const auto t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    const auto ny = ((static_cast<float>(y) + 0.5f) / static_cast<float>(layer.height()) - centre[1]) * 2.0f;
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      const auto nx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(layer.width()) - centre[0]) * 2.0f;
      const auto radius = std::sqrt(nx * nx + ny * ny) / std::sqrt(2.0f);
      const auto scale = 1.0f - amount * smoothstep(midpoint, midpoint + feather, radius);
      auto& pixel = layer.at(x, y);
      pixel.r *= scale;
      pixel.g *= scale;
      pixel.b *= scale;
    }
  });
}

void ApplyLensCorrection(Layer& layer, Layer& scratch, const SampledEffect& effect) {
  const auto distortion = std::clamp(ScalarParameter(effect, "distortion", 0.0f), -1.0f, 1.0f);
  const auto quadratic = std::clamp(ScalarParameter(effect, "quadratic", 0.0f), -1.0f, 1.0f);
  const auto scale = std::clamp(ScalarParameter(effect, "scale", 100.0f), 1.0f, 400.0f) / 100.0f;
  const auto centre = Vec2Parameter(effect, "center", 0.5f, 0.5f);
  if (layer.empty() || (std::abs(distortion) < 1e-6f && std::abs(quadratic) < 1e-6f &&
                        std::abs(scale - 1.0f) < 1e-6f)) {
    return;
  }

  scratch.Reset(layer.width(), layer.height());
  for (int y = 0; y < layer.height(); ++y) {
    const auto ny = ((static_cast<float>(y) + 0.5f) / static_cast<float>(layer.height()) - centre[1]) * 2.0f;
    for (int x = 0; x < layer.width(); ++x) {
      const auto nx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(layer.width()) - centre[0]) * 2.0f;
      const auto radius_squared = nx * nx + ny * ny;
      const auto radial = 1.0f + distortion * radius_squared + quadratic * radius_squared * radius_squared;
      const auto source_u = centre[0] + nx * radial / (2.0f * scale);
      const auto source_v = centre[1] + ny * radial / (2.0f * scale);
      const auto source_x = source_u * static_cast<float>(layer.width()) - 0.5f;
      const auto source_y = source_v * static_cast<float>(layer.height()) - 0.5f;
      scratch.at(x, y) = SampleBilinear(layer, source_x, source_y);
    }
  }
  scratch.MarkWhole();
  CopyLayer(scratch, layer);
}

[[nodiscard]] const CubeLut* ResolveLut(const SampledEffect& effect, EffectResources& resources,
                                        Statistics& statistics) {
  if (effect.preset_name.empty()) {
    statistics.effect_errors.push_back("lut: no .cube asset reference");
    return nullptr;
  }
  std::filesystem::path path(effect.preset_name);
  if (path.is_relative() && !resources.asset_root.empty()) path = std::filesystem::path(resources.asset_root) / path;
  path = path.lexically_normal();
  const auto key = path.string();
  if (const auto found = resources.lut_cache.find(key); found != resources.lut_cache.end()) return &found->second;
  if (const auto failed = resources.lut_failures.find(key); failed != resources.lut_failures.end()) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error) && !error;
    const auto modified = exists ? std::filesystem::last_write_time(path, error) : std::filesystem::file_time_type{};
    if (failed->second.existed == exists && (!exists || (!error && failed->second.modified == modified))) {
      statistics.effect_errors.push_back(failed->second.message);
      return nullptr;
    }
    // An offline LUT appeared, or a malformed one was replaced. Retry without
    // requiring the playback engine and its compositor to be recreated.
    resources.lut_failures.erase(failed);
  }
  try {
    auto [entry, inserted] = resources.lut_cache.emplace(key, CubeLut::Load(path));
    (void)inserted;
    return &entry->second;
  } catch (const std::exception& error) {
    auto message = std::string("lut: ") + error.what();
    std::error_code filesystem_error;
    const auto exists = std::filesystem::exists(path, filesystem_error) && !filesystem_error;
    const auto modified = exists ? std::filesystem::last_write_time(path, filesystem_error)
                                 : std::filesystem::file_time_type{};
    resources.lut_failures.emplace(key, EffectResources::LutFailure{message, exists, modified});
    statistics.effect_errors.push_back(message);
    return nullptr;
  }
}

[[nodiscard]] const graphics::Document* ResolveGraphic(const SampledEffect& effect, EffectResources& resources,
                                                       Statistics& statistics) {
  if (!effect.inline_asset.empty()) {
    // The project's own graphic: keyed by its content, so editing it is a different document.
    const auto key = "inline|" + effect.inline_asset;
    if (const auto found = resources.graphics_cache.find(key); found != resources.graphics_cache.end()) return &found->second;
    try {
      auto [entry, inserted] = resources.graphics_cache.emplace(key, graphics::ResolveBundle(effect.inline_asset));
      (void)inserted;
      return &entry->second;
    } catch (const std::exception& error) {
      statistics.effect_errors.push_back(effect.effect_type + ": " + error.what());
      return nullptr;
    }
  }
  if (effect.preset_name.empty()) {
    statistics.effect_errors.push_back(effect.effect_type + ": no graphics package reference");
    return nullptr;
  }
  std::filesystem::path path(effect.preset_name);
  if (path.is_relative() && !resources.asset_root.empty()) path = std::filesystem::path(resources.asset_root) / path;
  path = path.lexically_normal();
  const auto key = effect.effect_type + "|" + path.string();
  if (const auto found = resources.graphics_cache.find(key); found != resources.graphics_cache.end()) return &found->second;
  try {
    auto document = effect.effect_type == "motion_graphics_template"
                        ? graphics::LoadTemplate(path.string()).document
                        : graphics::LoadDocument(path.string());
    auto [entry, inserted] = resources.graphics_cache.emplace(key, std::move(document));
    (void)inserted;
    return &entry->second;
  } catch (const std::exception& error) {
    statistics.effect_errors.push_back(effect.effect_type + ": " + error.what());
    return nullptr;
  }
}

void ApplyLut(Layer& layer, const CubeLut& lut, float requested_intensity) {
  const auto intensity = std::clamp(requested_intensity, 0.0f, 1.0f);
  if (intensity <= 1e-6f || layer.empty()) return;
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      const auto inverse_alpha = pixel.a >= 1.0f ? 1.0f : 1.0f / pixel.a;
      const float input[3] = {pixel.r * inverse_alpha, pixel.g * inverse_alpha, pixel.b * inverse_alpha};
      float mapped[3];
      lut.Map(input, mapped);
      pixel.r = std::clamp(input[0] + (mapped[0] - input[0]) * intensity, 0.0f, 1.0f) * pixel.a;
      pixel.g = std::clamp(input[1] + (mapped[1] - input[1]) * intensity, 0.0f, 1.0f) * pixel.a;
      pixel.b = std::clamp(input[2] + (mapped[2] - input[2]) * intensity, 0.0f, 1.0f) * pixel.a;
    }
  });
}

[[nodiscard]] bool DrawFrameOverwritesWholeLayer(const Layer& layer, const VideoFrame& frame,
                                                 const Transform2D& transform, const CropRect& crop) {
  if (!transform.identity || !crop.empty() || frame.width() != layer.width() || frame.height() != layer.height()) {
    return false;
  }
  const auto fit = std::min(static_cast<float>(layer.width()) / frame.width(),
                            static_cast<float>(layer.height()) / frame.height());
  return std::abs(fit - 1.0f) < 1e-6f && std::abs(transform.anchor_x - 0.5f) < 1e-6f &&
         std::abs(transform.anchor_y - 0.5f) < 1e-6f;
}

// Draws a source frame into an output-sized layer, honouring crop and motion.
// The mapping is computed destination-to-source so every output pixel is
// written exactly once and scaling never leaves holes.
void DrawFrame(Layer& layer, const VideoFrame& frame, const Transform2D& transform, const CropRect& crop) {
  const auto source_width = static_cast<float>(frame.width());
  const auto source_height = static_cast<float>(frame.height());

  // Visible region of the source after cropping.
  const auto crop_left = std::clamp(crop.left, 0.0f, 1.0f) * source_width;
  const auto crop_top = std::clamp(crop.top, 0.0f, 1.0f) * source_height;
  const auto crop_right = source_width - std::clamp(crop.right, 0.0f, 1.0f) * source_width;
  const auto crop_bottom = source_height - std::clamp(crop.bottom, 0.0f, 1.0f) * source_height;
  if (crop_right <= crop_left || crop_bottom <= crop_top) return;

  // The source is fitted to the output, then the motion transform is applied
  // about the anchor. Fitting first is what makes a 4K source and a 1080 source
  // behave the same way on a 1080 timeline.
  const auto fit = std::min(static_cast<float>(layer.width()) / source_width,
                            static_cast<float>(layer.height()) / source_height);
  const auto scale_x = fit * transform.scale_x;
  const auto scale_y = fit * transform.scale_y;
  if (std::abs(scale_x) < 1e-9f || std::abs(scale_y) < 1e-9f) return;

  // Fast path: an untransformed, uncropped clip whose source already matches the
  // output is a straight copy. Running it through the inverse transform and the
  // bilinear sampler would read four texels and do forty-odd operations per
  // pixel to arrive at the same answer, and this is the common case in an edit.
  if (DrawFrameOverwritesWholeLayer(layer, frame, transform, crop)) {
    ParallelRows(0, layer.height() - 1, [&](int y) {
      const auto* texel = frame.row_f32(y);
      for (int x = 0; x < layer.width(); ++x) {
        const auto* pixel = texel + static_cast<std::size_t>(x) * 4;
        const auto alpha = pixel[3];
        layer.at(x, y) = {pixel[0] * alpha, pixel[1] * alpha, pixel[2] * alpha, alpha};
      }
    });
    layer.MarkWhole();
    return;
  }

  const auto centre_x = static_cast<float>(layer.width()) * 0.5f + transform.translate_x;
  const auto centre_y = static_cast<float>(layer.height()) * 0.5f + transform.translate_y;
  const auto cosine = std::cos(-transform.rotation_radians);
  const auto sine = std::sin(-transform.rotation_radians);
  const auto anchor_x = transform.anchor_x * source_width;
  const auto anchor_y = transform.anchor_y * source_height;

  // Bound the sweep by where the visible source region can land once scaled,
  // rotated, and translated. Projecting the four corners and taking their
  // extent is exact for an affine transform, and for a quarter-size inset it
  // turns a full-canvas loop into a quarter of one.
  float bound_min_x = std::numeric_limits<float>::max();
  float bound_min_y = std::numeric_limits<float>::max();
  float bound_max_x = std::numeric_limits<float>::lowest();
  float bound_max_y = std::numeric_limits<float>::lowest();
  const auto forward_cos = std::cos(transform.rotation_radians);
  const auto forward_sin = std::sin(transform.rotation_radians);
  for (const auto& corner : {std::pair{crop_left, crop_top}, std::pair{crop_right, crop_top},
                             std::pair{crop_left, crop_bottom}, std::pair{crop_right, crop_bottom}}) {
    const auto ox = (corner.first - anchor_x) * scale_x;
    const auto oy = (corner.second - anchor_y) * scale_y;
    const auto px = centre_x + ox * forward_cos - oy * forward_sin;
    const auto py = centre_y + ox * forward_sin + oy * forward_cos;
    bound_min_x = std::min(bound_min_x, px);
    bound_max_x = std::max(bound_max_x, px);
    bound_min_y = std::min(bound_min_y, py);
    bound_max_y = std::max(bound_max_y, py);
  }
  // One pixel of slack covers the bilinear tap and the half-pixel centre offset.
  const auto x_start = std::max(0, static_cast<int>(std::floor(bound_min_x)) - 1);
  const auto x_end = std::min(layer.width() - 1, static_cast<int>(std::ceil(bound_max_x)) + 1);
  const auto y_start = std::max(0, static_cast<int>(std::floor(bound_min_y)) - 1);
  const auto y_end = std::min(layer.height() - 1, static_cast<int>(std::ceil(bound_max_y)) + 1);
  if (x_end < x_start || y_end < y_start) return;

  ParallelRows(y_start, y_end, [&](int y) {
    for (int x = x_start; x <= x_end; ++x) {
      // Destination -> source: undo translate, rotate, scale, then re-anchor.
      const auto dx = static_cast<float>(x) + 0.5f - centre_x;
      const auto dy = static_cast<float>(y) + 0.5f - centre_y;
      const auto rx = dx * cosine - dy * sine;
      const auto ry = dx * sine + dy * cosine;
      const auto sx = rx / scale_x + anchor_x;
      const auto sy = ry / scale_y + anchor_y;
      if (sx < crop_left || sx >= crop_right || sy < crop_top || sy >= crop_bottom) continue;
      layer.at(x, y) = SampleBilinear(frame, sx - 0.5f, sy - 0.5f);
    }
  });
  layer.MarkDirty(x_start, y_start, x_end, y_end);
}

void FillLayer(Layer& layer, float red, float green, float blue, float alpha) {
  ParallelRows(0, layer.height() - 1, [&](int y) {
    for (int x = 0; x < layer.width(); ++x) {
      layer.at(x, y) = {red * alpha, green * alpha, blue * alpha, alpha};
    }
  });
  layer.MarkWhole();
}

// Applies whichever of a request's effects the compositor understands. Motion
// and crop are consumed by DrawFrame, so they are skipped here.
void ApplyPostEffects(Layer& layer, Layer& scratch, Layer& mask_before, const std::vector<SampledEffect>& effects,
                      Statistics& statistics, EffectResources& resources,
                      const std::vector<const Layer*>& neighbours = {}) {
  for (const auto& effect : effects) {
    if (!effect.masks.empty()) CopyLayer(layer, mask_before);
    bool applied = true;
    if (effect.effect_type == "noise_reduction") {
      ReduceNoise(layer, scratch, ReadNoiseSettings(effect), neighbours);
    } else if (effect.effect_type == "opacity") {
      ApplyOpacity(layer, ScalarParameter(effect, "value", 1.0f));
    } else if (effect.effect_type == "grade" || effect.effect_type == "lumetri") {
      ApplyGrade(layer, ReadGrade(effect));
    } else if (effect.effect_type == "blur") {
      ApplyBlur(layer, scratch, ScalarParameter(effect, "radius", 0.0f));
    } else if (effect.effect_type == "sharpen") {
      ApplySharpen(layer, scratch, ScalarParameter(effect, "amount", 0.0f));
    } else if (effect.effect_type == "vignette") {
      ApplyVignette(layer, effect);
    } else if (effect.effect_type == "lens_correction" || effect.effect_type == "wide_angle") {
      ApplyLensCorrection(layer, scratch, effect);
    } else if (ApplyFilterEffect(layer, scratch, effect) || ApplyColorEffect(layer, scratch, effect)) {
      // A production filter, key or grading tool (Filters.h).
    } else if (effect.effect_type == "lut") {
      if (const auto* lut = ResolveLut(effect, resources, statistics)) {
        ApplyLut(layer, *lut, ScalarParameter(effect, "intensity", 1.0f));
      }
    } else if (effect.effect_type == "motion" || effect.effect_type == "transform" ||
               effect.effect_type == "stabilizer" || effect.effect_type == "crop" || effect.effect_type == "solid" ||
               effect.effect_type == "blend_mode" || effect.effect_type == "time_remap" ||
               effect.effect_type == "frame_interpolation" || effect.effect_type == "graphic" ||
               effect.effect_type == "motion_graphics_template" || effect.effect_type == "input_colorspace") {
      // Geometry and generators are handled while drawing.
      applied = false;
    } else {
      statistics.skipped_effects.push_back(effect.effect_type);
      applied = false;
    }
    if (applied && !effect.masks.empty()) BlendMaskedEffect(mask_before, layer, effect.masks);
  }
}

// What the compositor needs of a sequence's colour settings: whether to manage colour at all,
// and the working space frames are brought into.
struct ColorContext final {
  bool managed{false};
  color::Space working;
  color::Space display;
  // For effects that read the frames around the one being drawn (temporal noise reduction): the
  // sequence's rate, which says how far apart those frames are, and layers to draw them into.
  time::FrameRate frame_rate{25, 1};
  std::vector<Layer>* neighbour_layers{nullptr};
};

[[nodiscard]] VideoFrame FlattenSingleLayer(const Layer& layer, const CompositorConfig& config,
                                            const timeline::PlaybackPlan& plan, const ColorContext& colour) {
  auto output = VideoFrame::Allocate(PixelFormat::RgbaF32, layer.width(), layer.height());
  output.presentation_time = plan.sequence_time;
  output.color.range = model::ColorRange::Full;
  if (colour.managed) {
    const auto tags = color::TagsOf(colour.display);
    output.color.primaries = tags.primaries;
    output.color.transfer = tags.transfer;
  }

  const bool whole = !layer.empty() && layer.min_x() == 0 && layer.min_y() == 0 &&
                     layer.max_x() == layer.width() - 1 && layer.max_y() == layer.height() - 1;
  const auto write = [&](int y, int first, int last) {
    auto* row = output.row_f32(y);
    for (int x = first; x <= last; ++x) {
      const auto& top = layer.at(x, y);
      const auto keep = 1.0f - top.a;
      auto* pixel = row + static_cast<std::size_t>(x) * 4;
      pixel[0] = top.r + config.background_red * keep;
      pixel[1] = top.g + config.background_green * keep;
      pixel[2] = top.b + config.background_blue * keep;
      pixel[3] = 1.0f;
    }
  };
  if (whole) {
    ParallelRows(0, layer.height() - 1, [&](int y) { write(y, 0, layer.width() - 1); });
  } else {
    ParallelRows(0, layer.height() - 1, [&](int y) {
      auto* row = output.row_f32(y);
      for (int x = 0; x < layer.width(); ++x) {
        auto* pixel = row + static_cast<std::size_t>(x) * 4;
        pixel[0] = config.background_red;
        pixel[1] = config.background_green;
        pixel[2] = config.background_blue;
        pixel[3] = 1.0f;
      }
    });
    if (!layer.empty()) {
      ParallelRows(layer.min_y(), layer.max_y(),
                   [&](int y) { write(y, layer.min_x(), layer.max_x()); });
    }
  }
  if (config.output_format == PixelFormat::RgbaF32) return output;
  return media::ConvertFrame(output, config.output_format);
}

// Converts what a layer drew from a frame's own colour space into the working space. The layer
// is premultiplied, so each pixel is un-premultiplied for the conversion and premultiplied again.
void ConvertLayer(Layer& layer, const color::Transform& transform) {
  if (transform.identity() || layer.empty()) return;
  ParallelRows(layer.min_y(), layer.max_y(), [&](int y) {
    for (int x = layer.min_x(); x <= layer.max_x(); ++x) {
      auto& pixel = layer.at(x, y);
      if (pixel.a <= 0.0f) continue;
      const auto inverse = pixel.a >= 1.0f ? 1.0f : 1.0f / pixel.a;
      float rgb[3] = {pixel.r * inverse, pixel.g * inverse, pixel.b * inverse};
      transform.Apply(rgb);
      pixel.r = rgb[0] * pixel.a;
      pixel.g = rgb[1] * pixel.a;
      pixel.b = rgb[2] * pixel.a;
    }
  });
}

[[nodiscard]] bool HasGraphic(const std::vector<SampledEffect>& effects) {
  for (const auto& effect : effects) {
    if (effect.effect_type == "graphic" || effect.effect_type == "motion_graphics_template") return true;
  }
  return false;
}

// Renders one clip into `layer`, which the caller owns and reuses.
void RenderRequest(const SourceRequest& request, const FrameResolver& resolve, int width, int height,
                   Statistics& statistics, Layer& layer, Layer& scratch, Layer& mask_before, EffectResources& resources,
                   const ColorContext& managed) {
  const auto* graphic = FindEffect(request.effects, "graphic");
  if (graphic == nullptr) graphic = FindEffect(request.effects, "motion_graphics_template");
  if (graphic != nullptr) {
    // A graphic can leave holes inside its bounding rectangle, so its old
    // dirty pixels must be cleared before it draws.
    layer.Reset(width, height);
    if (const auto* document = ResolveGraphic(*graphic, resources, statistics)) {
      // Animated elements are drawn as they are this far into the clip.
      const auto drawn = graphics::Draw(layer, *document, resources.stills.Resolver(), static_cast<double>(request.source_time.ToTicks()) / static_cast<double>(cutline::time::kTicksPerSecond));
      statistics.graphics_elements_drawn += drawn.elements_drawn;
      for (const auto& warning : drawn.warnings) statistics.effect_errors.push_back(warning);
    }
    ApplyPostEffects(layer, scratch, mask_before, request.effects, statistics, resources);
    return;
  }

  if (const auto* solid = FindEffect(request.effects, "solid")) {
    // FillLayer writes the complete surface; clearing it first is redundant.
    layer.ResetDiscardingContents(width, height);
    // A generator clip: no decoded source, just a fill.
    const auto colour = FindParameter(*solid, "color");
    const auto red = colour.has_value() ? static_cast<float>(colour->components[0]) : 0.0f;
    const auto green = colour.has_value() ? static_cast<float>(colour->components[1]) : 0.0f;
    const auto blue = colour.has_value() ? static_cast<float>(colour->components[2]) : 0.0f;
    FillLayer(layer, red, green, blue, 1.0f);
    ApplyPostEffects(layer, scratch, mask_before, request.effects, statistics, resources);
    return;
  }

  const auto* frame = resolve(request);
  if (frame == nullptr || !frame->valid()) {
    // Clear the last use before marking this layer empty. A later partial draw
    // must not uncover pixels from the frame that went missing here.
    layer.Reset(width, height);
    ++statistics.missing_frames;
    return;
  }

  // The compositor works in float. Converting only when the resolver handed us
  // something else keeps a cache that already stores float frames zero-copy.
  const VideoFrame* source = frame;
  VideoFrame converted;
  if (frame->format() != PixelFormat::RgbaF32) {
    converted = media::ConvertFrame(*frame, PixelFormat::RgbaF32);
    source = &converted;
  }

  // Fractional source times created by a speed ramp can be synthesised from
  // the two decoded frames around that time. Mode 1 is a frame blend; mode 2
  // uses motion-compensated optical flow with confidence/occlusion fallback.
  VideoFrame interpolated;
  const auto* interpolation = FindEffect(request.effects, "frame_interpolation");
  const auto interpolation_mode = interpolation == nullptr
                                      ? 0
                                      : static_cast<int>(std::lround(ScalarParameter(*interpolation, "mode", 0.0f)));
  if (interpolation_mode > 0 && source->duration.numerator() > 0 &&
      request.source_time.Compare(source->presentation_time) > 0 &&
      request.source_time.Compare(source->presentation_time.Add(source->duration)) < 0) {
    const auto fraction = request.source_time.Subtract(source->presentation_time).Divide(source->duration);
    const auto progress = static_cast<float>(fraction.numerator()) / static_cast<float>(fraction.denominator());
    auto next_request = request;
    next_request.source_time = source->presentation_time.Add(source->duration);
    // Preserve the current decoded image before asking a stateful decoder for
    // the next one; some providers reuse their current-frame storage.
    auto current = source->Clone();
    const auto* next_frame = resolve(next_request);
    if (next_frame != nullptr && next_frame->valid() && next_frame->width() == current.width() &&
        next_frame->height() == current.height()) {
      auto next = next_frame->format() == PixelFormat::RgbaF32 ? next_frame->Clone()
                                                               : media::ConvertFrame(*next_frame, PixelFormat::RgbaF32);
      if (interpolation_mode == 1) {
        FlowField dissolve;
        dissolve.width = current.width();
        dissolve.height = current.height();
        dissolve.block_size = std::max(current.width(), current.height());
        dissolve.columns = 1;
        dissolve.rows = 1;
        dissolve.samples.push_back({0.0f, 0.0f, 0.0f, true});
        interpolated = InterpolateOpticalFlow(current, next, progress, dissolve);
      } else {
        if (resources.flow_cache != nullptr) {
          const auto lookup = EstimateOpticalFlowCached(*resources.flow_cache, current, next);
          interpolated = InterpolateOpticalFlow(current, next, progress, *lookup.field);
          if (lookup.source != FlowSource::Computed) ++statistics.optical_flow_reused;
        } else {
          const auto flow = EstimateOpticalFlow(current, next);
          interpolated = InterpolateOpticalFlow(current, next, progress, flow);
        }
        ++statistics.optical_flow_frames;
      }
      source = &interpolated;
      ++statistics.interpolated_frames;
    }
  }

  const auto transform = TransformFor(request.effects);
  const auto crop = CropFor(request.effects);
  if (DrawFrameOverwritesWholeLayer(layer, *source, transform, crop)) {
    layer.ResetDiscardingContents(width, height);
  } else {
    // A transformed layer can leave transparent holes inside its conservative
    // dirty rectangle, so clear the previous use before drawing it.
    layer.Reset(width, height);
  }
  DrawFrame(layer, *source, transform, crop);
  if (managed.managed) {
    // The frame says what it is; the layer is brought into the sequence's working space before any
    // effect sees it. A frame with tags this build does not define is taken to be in the working space.
    auto tagged = color::SpaceFromTags(source->color.primaries, source->color.transfer);
    // The clip may say what the picture is, over what the file says: the way log footage, untagged, is read.
    if (const auto* declared = FindEffect(request.effects, "input_colorspace")) {
      if (const auto space = color::ParseSpace(declared->preset_name)) {
        tagged = *space;
      } else {
        statistics.effect_errors.push_back("input_colorspace: \"" + declared->preset_name + "\" is not a colour space this build defines");
      }
    }
    if (!tagged.has_value()) {
      statistics.effect_errors.push_back("the colour tags " + source->color.primaries + "/" + source->color.transfer +
                                         " are not ones this build defines; the frame was treated as being in the working space");
    } else if (!(*tagged == managed.working)) {
      ConvertLayer(layer, color::Transform(*tagged, managed.working));
      ++statistics.color_input_conversions;
    }
  }
  // Temporal noise reduction reads the frames either side of this one, drawn the way this one was.
  std::vector<const Layer*> neighbours;
  if (managed.neighbour_layers != nullptr) {
    const auto* noise = FindEffect(request.effects, "noise_reduction");
    if (noise != nullptr) {
      const auto settings = ReadNoiseSettings(*noise);
      if (settings.temporal > 1e-6f) {
        const auto step = time::RationalTime(managed.frame_rate.denominator, managed.frame_rate.numerator).Multiply(request.playback_rate);
        std::size_t used = 0;
        for (int offset = -settings.radius; offset <= settings.radius; ++offset) {
          if (offset == 0 || used >= managed.neighbour_layers->size()) continue;
          auto neighbour_request = request;
          neighbour_request.source_time = request.source_time.Add(step.Multiply(offset));
          if (neighbour_request.source_time.Compare(time::RationalTime(0, 1)) < 0) continue;
          const auto* neighbour_frame = resolve(neighbour_request);
          if (neighbour_frame == nullptr || !neighbour_frame->valid() || neighbour_frame->width() != frame->width() ||
              neighbour_frame->height() != frame->height()) {
            continue;
          }
          VideoFrame neighbour_converted;
          const VideoFrame* neighbour_source = neighbour_frame;
          if (neighbour_frame->format() != PixelFormat::RgbaF32) {
            neighbour_converted = media::ConvertFrame(*neighbour_frame, PixelFormat::RgbaF32);
            neighbour_source = &neighbour_converted;
          }
          auto& target = (*managed.neighbour_layers)[used++];
          target.Reset(width, height);
          DrawFrame(target, *neighbour_source, TransformFor(request.effects), CropFor(request.effects));
          if (managed.managed) {
            const auto tagged = color::SpaceFromTags(neighbour_source->color.primaries, neighbour_source->color.transfer);
            if (tagged.has_value() && !(*tagged == managed.working)) ConvertLayer(target, color::Transform(*tagged, managed.working));
          }
          neighbours.push_back(&target);
          ++statistics.noise_frames_used;
        }
      }
    }
  }
  ApplyPostEffects(layer, scratch, mask_before, request.effects, statistics, resources, neighbours);
}

}  // namespace

// Reusable buffers for one compositing pass.
struct Compositor::Workspace final {
  Layer canvas;
  Layer layer;
  Layer transition_from;
  Layer transition_to;
  Layer mixed;
  // Up to two frames either side for temporal noise reduction.
  std::vector<Layer> neighbours = std::vector<Layer>(4);
  Layer effect_scratch;
  Layer mask_before;
  EffectResources effect_resources;
};

Compositor::Compositor(CompositorConfig config)
    : config_(config), workspace_(std::make_unique<Workspace>()) {}
Compositor::~Compositor() = default;
Compositor::Compositor(Compositor&&) noexcept = default;
Compositor& Compositor::operator=(Compositor&&) noexcept = default;

bool IsBuiltInEffect(const std::string& effect_type) {
  const auto* descriptor = effects::FindEffect(effect_type);
  return descriptor != nullptr && descriptor->medium == effects::Medium::Video && descriptor->cpu_available;
}

std::vector<std::string> BuiltInEffectTypes() {
  std::vector<std::string> types;
  for (const auto& descriptor : effects::BuiltInEffects()) {
    if (descriptor.medium == effects::Medium::Video && descriptor.cpu_available) types.push_back(descriptor.id);
  }
  return types;
}

media::VideoFrame Compositor::Compose(const timeline::PlaybackPlan& plan, const FrameResolver& resolve) const {
  Statistics ignored;
  return Compose(plan, resolve, ignored);
}

media::VideoFrame Compositor::Compose(const timeline::PlaybackPlan& plan, const FrameResolver& resolve,
                                      Statistics& statistics) const {
  const auto width = config_.width > 0 ? config_.width : static_cast<int>(plan.width);
  const auto height = config_.height > 0 ? config_.height : static_cast<int>(plan.height);
  if (width <= 0 || height <= 0) throw std::invalid_argument("Compositor has no output size");

  auto& workspace = *workspace_;
  workspace.effect_resources.asset_root = config_.asset_root;
  workspace.effect_resources.flow_cache = config_.flow_cache;

  // Colour: under render version 3 and later the sequence's working and display spaces mean something.
  ColorContext colour;
  colour.frame_rate = plan.frame_rate;
  colour.neighbour_layers = &workspace.neighbours;
  colour.managed = model::RenderSemantics::For(plan.render_version).color_managed;
  if (colour.managed) {
    const auto parse = [&](const std::string& name, const char* role) {
      const auto parsed = color::ParseSpace(name);
      if (parsed.has_value()) return *parsed;
      if (!name.empty()) {
        statistics.effect_errors.push_back(std::string("the ") + role + " colour space '" + name + "' is not one this build defines; it was treated as rec709");
      }
      return color::Space{};
    };
    colour.working = parse(plan.working_color_space, "working");
    colour.display = parse(plan.display_color_space, "display");
  }
  // Only this sequence level. Deeper requests belong to a nested compose, which
  // the resolver performs when it is asked for a Sequence-kind source.
  std::vector<const SourceRequest*> requests;
  for (const auto& request : plan.video) {
    if (request.depth == 0) requests.push_back(&request);
  }

  // Which track each active transition belongs to.
  std::unordered_map<std::string, const timeline::TransitionMix*> transitions;
  for (const auto& transition : plan.transitions) {
    transitions.emplace(transition.track_id, &transition);
  }

  // Group by track, preserving the plan's ascending order. A std::map keyed on
  // (order, id) gives a deterministic bottom-to-top walk.
  std::map<std::pair<std::int64_t, std::string>, std::vector<const SourceRequest*>> by_track;
  for (const auto* request : requests) {
    by_track[{request->track_order, request->track_id}].push_back(request);
  }

  // One ordinary layer is by far the common monitor case. Rendering it into an
  // opaque background through a second full-size float canvas used three more
  // memory passes (fill, source-over, flatten). Write the final pixels straight
  // from the processed layer when nothing later needs that canvas.
  const bool one_plain_layer = requests.size() == 1 && transitions.empty() && plan.sequence_effects.empty() &&
                               plan.captions.empty() &&
                               requests.front()->source_kind != model::SourceKind::Adjustment &&
                               FindEffect(requests.front()->effects, "blend_mode") == nullptr &&
                               (!colour.managed || colour.working == colour.display);
  if (one_plain_layer) {
    RenderRequest(*requests.front(), resolve, width, height, statistics, workspace.layer, workspace.effect_scratch,
                  workspace.mask_before, workspace.effect_resources, colour);
    ++statistics.layers_composited;
    return FlattenSingleLayer(workspace.layer, config_, plan, colour);
  }

  auto& canvas = workspace.canvas;
  // FillLayer overwrites the complete canvas, so clearing last frame first
  // only doubles the memory traffic.
  canvas.ResetDiscardingContents(width, height);
  FillLayer(canvas, config_.background_red, config_.background_green, config_.background_blue, 1.0f);

  for (const auto& [key, track_requests] : by_track) {
    const auto& track_id = key.second;
    const auto found = transitions.find(track_id);

    // A transition on this track replaces its normal layer, however many sides
    // it actually has: a one-sided fade has a single request on the track, and
    // treating that as an ordinary clip would draw it at full strength and skip
    // the fade entirely.
    if (found != transitions.end()) {
      // A transition contributes one layer: its two sides mixed.
      const auto* transition = found->second;
      const auto side = [&track_requests](const std::optional<std::string>& clip_id) -> const SourceRequest* {
        if (!clip_id.has_value()) return nullptr;
        for (const auto* request : track_requests) {
          if (request->clip_id == *clip_id) return request;
        }
        return nullptr;
      };
      const auto* from = side(transition->from_clip_id);
      const auto* to = side(transition->to_clip_id);

      // A missing side stays transparent, which is what makes a one-sided
      // transition read as a fade from or to the layers beneath.
      if (from != nullptr) {
        RenderRequest(*from, resolve, width, height, statistics, workspace.transition_from, workspace.effect_scratch,
                      workspace.mask_before, workspace.effect_resources, colour);
      } else {
        workspace.transition_from.Reset(width, height);
      }
      if (to != nullptr) {
        RenderRequest(*to, resolve, width, height, statistics, workspace.transition_to, workspace.effect_scratch,
                      workspace.mask_before, workspace.effect_resources, colour);
      } else {
        workspace.transition_to.Reset(width, height);
      }
      if (!IsKnownTransition(transition->kind)) {
        statistics.effect_errors.push_back("transition " + transition->kind + " is not one this build has; it was drawn as a cross dissolve");
      }
      MixTransition(transition->kind, workspace.transition_from, workspace.transition_to, static_cast<float>(transition->progress), workspace.mixed);
      workspace.mixed.CompositeOnto(canvas);
      ++statistics.transitions_mixed;
      ++statistics.layers_composited;
      continue;
    }

    for (const auto* request : track_requests) {
      // A source-less clip carrying a graphic is that graphic, a generator, not an adjustment of what is below.
      if (request->source_kind == model::SourceKind::Adjustment && !HasGraphic(request->effects)) {
        // An adjustment clip re-grades everything already beneath it.
        ApplyPostEffects(canvas, workspace.effect_scratch, workspace.mask_before, request->effects, statistics,
                         workspace.effect_resources);
        ++statistics.adjustment_layers;
        continue;
      }
      RenderRequest(*request, resolve, width, height, statistics, workspace.layer, workspace.effect_scratch,
                    workspace.mask_before,
                    workspace.effect_resources, colour);
      if (const auto* blend = FindEffect(request->effects, "blend_mode")) {
        const auto mode = BlendModeFromIndex(static_cast<int>(std::lround(ScalarParameter(*blend, "mode", 0.0f))));
        CompositeBlend(workspace.layer, canvas, mode, ScalarParameter(*blend, "opacity", 1.0f));
      } else {
        workspace.layer.CompositeOnto(canvas);
      }
      ++statistics.layers_composited;
    }
  }

  // Sequence-level effects apply to the finished composite.
  ApplyPostEffects(canvas, workspace.effect_scratch, workspace.mask_before, plan.sequence_effects, statistics,
                   workspace.effect_resources);

  // The finished picture, from the working space to the one it will be shown in.
  if (colour.managed && !(colour.working == colour.display)) {
    ConvertLayer(canvas, color::Transform(colour.working, colour.display));
    statistics.color_output_conversion = true;
  }

  // Captions go over the finished picture, in the colours the viewer sees.
  if (!plan.captions.empty()) {
    const auto drawn = DrawCaptions(canvas, plan.captions);
    statistics.captions_drawn += drawn.drawn;
    for (const auto& warning : drawn.warnings) statistics.effect_errors.push_back(warning);
  }

  // Flatten to the output format, un-premultiplying as we go.
  auto output = VideoFrame::Allocate(PixelFormat::RgbaF32, width, height);
  output.presentation_time = plan.sequence_time;
  output.color.range = model::ColorRange::Full;
  if (colour.managed) {
    const auto tags = color::TagsOf(colour.display);
    output.color.primaries = tags.primaries;
    output.color.transfer = tags.transfer;
  }
  ParallelRows(0, height - 1, [&](int y) {
    auto* row = output.row_f32(y);
    for (int x = 0; x < width; ++x) {
      const auto& pixel = canvas.at(x, y);
      auto* texel = row + static_cast<std::size_t>(x) * 4;
      if (pixel.a >= 1.0f) {
        // Opaque: the premultiplied value is already the colour.
        texel[0] = pixel.r;
        texel[1] = pixel.g;
        texel[2] = pixel.b;
      } else if (pixel.a > 0.0f) {
        const auto inverse = 1.0f / pixel.a;
        texel[0] = pixel.r * inverse;
        texel[1] = pixel.g * inverse;
        texel[2] = pixel.b * inverse;
      } else {
        texel[0] = 0.0f;
        texel[1] = 0.0f;
        texel[2] = 0.0f;
      }
      texel[3] = pixel.a;
    }
  });

  if (config_.output_format == PixelFormat::RgbaF32) return output;
  return media::ConvertFrame(output, config_.output_format);
}

}  // namespace cutline::render

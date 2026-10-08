#include "render/NoiseReduction.h"

#include <algorithm>
#include <cmath>

namespace cutline::render {
namespace {

constexpr float kKr = 0.2126f, kKg = 0.7152f, kKb = 0.0722f;
constexpr float kCbScale = 1.8556f;  // 2 (1 - Kb)
constexpr float kCrScale = 1.5748f;  // 2 (1 - Kr)

struct Straight final {
  float r{0}, g{0}, b{0}, a{0};
};

[[nodiscard]] Straight Unpremultiply(const Pixel& p) {
  if (p.a <= 0.0f) return {};
  const auto inverse = p.a >= 1.0f ? 1.0f : 1.0f / p.a;
  return {p.r * inverse, p.g * inverse, p.b * inverse, p.a};
}

[[nodiscard]] float LumaOf(const Straight& s) { return kKr * s.r + kKg * s.g + kKb * s.b; }

}  // namespace

NoiseSettings ReadNoiseSettings(const SampledEffect& effect) {
  NoiseSettings settings;
  settings.luma = std::clamp(ScalarParameter(effect, "luma", 0.0f), 0.0f, 1.0f);
  settings.chroma = std::clamp(ScalarParameter(effect, "chroma", 0.0f), 0.0f, 1.0f);
  settings.temporal = std::clamp(ScalarParameter(effect, "temporal", 0.0f), 0.0f, 1.0f);
  settings.detail = std::clamp(ScalarParameter(effect, "detail", 0.5f), 0.0f, 1.0f);
  settings.radius = std::clamp(static_cast<int>(std::lround(ScalarParameter(effect, "radius", 1.0f))), 1, 2);
  return settings;
}

void ReduceNoise(Layer& layer, Layer& scratch, const NoiseSettings& settings, const std::vector<const Layer*>& neighbours) {
  if (layer.empty() || !settings.active()) return;
  const auto x0 = layer.min_x(), x1 = layer.max_x(), y0 = layer.min_y(), y1 = layer.max_y();
  const auto width = x1 - x0 + 1, height = y1 - y0 + 1;
  const auto index = [&](int x, int y) { return static_cast<std::size_t>(y - y0) * width + (x - x0); };

  // The picture as straight colour, over the rectangle that has anything in it.
  std::vector<Straight> current(static_cast<std::size_t>(width) * height);
  ParallelRows(y0, y1, [&](int y) {
    for (int x = x0; x <= x1; ++x) current[index(x, y)] = Unpremultiply(layer.at(x, y));
  });

  // ------------------------------------------------------------------ temporal ----
  if (settings.temporal > 1e-6f) {
    struct Neighbour final {
      std::vector<Straight> pixels;
    };
    std::vector<Neighbour> others;
    for (const auto* neighbour : neighbours) {
      if (neighbour == nullptr || neighbour->width() != layer.width() || neighbour->height() != layer.height()) continue;
      Neighbour entry;
      entry.pixels.resize(current.size());
      ParallelRows(y0, y1, [&](int y) {
        for (int x = x0; x <= x1; ++x) entry.pixels[index(x, y)] = Unpremultiply(neighbour->at(x, y));
      });
      others.push_back(std::move(entry));
    }
    if (!others.empty()) {
      const auto sigma = 0.02f + 0.18f * settings.temporal;
      const auto inverse_two_sigma_squared = 1.0f / (2.0f * sigma * sigma);
      // How much the 3 x 3 patch around a pixel differs between the frames, by luma.
      std::vector<float> luma_now(current.size());
      for (std::size_t i = 0; i < current.size(); ++i) luma_now[i] = LumaOf(current[i]);
      std::vector<Straight> result = current;
      ParallelRows(y0, y1, [&](int y) {
        for (int x = x0; x <= x1; ++x) {
          const auto here = index(x, y);
          if (current[here].a <= 0.0f) continue;
          float sum_r = current[here].r, sum_g = current[here].g, sum_b = current[here].b, total = 1.0f;
          for (const auto& other : others) {
            if (other.pixels[here].a <= 0.0f) continue;
            float difference = 0.0f;
            int count = 0;
            for (int j = -1; j <= 1; ++j) {
              const auto yy = y + j;
              if (yy < y0 || yy > y1) continue;
              for (int i = -1; i <= 1; ++i) {
                const auto xx = x + i;
                if (xx < x0 || xx > x1) continue;
                const auto there = index(xx, yy);
                difference += std::abs(LumaOf(other.pixels[there]) - luma_now[there]);
                ++count;
              }
            }
            const auto mean = difference / static_cast<float>(count);
            const auto weight = std::exp(-mean * mean * inverse_two_sigma_squared);
            sum_r += weight * other.pixels[here].r;
            sum_g += weight * other.pixels[here].g;
            sum_b += weight * other.pixels[here].b;
            total += weight;
          }
          result[here].r = sum_r / total;
          result[here].g = sum_g / total;
          result[here].b = sum_b / total;
        }
      });
      current = std::move(result);
    }
  }

  // ------------------------------------------------------------------- spatial ----
  if (settings.spatial()) {
    std::vector<float> luma(current.size()), cb(current.size()), cr(current.size());
    for (std::size_t i = 0; i < current.size(); ++i) {
      const auto& p = current[i];
      luma[i] = LumaOf(p);
      cb[i] = (p.b - luma[i]) / kCbScale;
      cr[i] = (p.r - luma[i]) / kCrScale;
    }
    auto luma_out = luma, cb_out = cb, cr_out = cr;

    if (settings.luma > 1e-6f) {
      constexpr int kRadius = 2;
      constexpr float kSpatial = 1.3f;
      const auto sigma_r = (0.01f + 0.17f * settings.luma) * (1.0f - 0.7f * settings.detail);
      const auto inverse_r = 1.0f / (2.0f * sigma_r * sigma_r);
      ParallelRows(y0, y1, [&](int y) {
        for (int x = x0; x <= x1; ++x) {
          const auto here = index(x, y);
          if (current[here].a <= 0.0f) continue;
          float sum = 0.0f, total = 0.0f;
          for (int j = -kRadius; j <= kRadius; ++j) {
            const auto yy = y + j;
            if (yy < y0 || yy > y1) continue;
            for (int i = -kRadius; i <= kRadius; ++i) {
              const auto xx = x + i;
              if (xx < x0 || xx > x1) continue;
              const auto there = index(xx, yy);
              if (current[there].a <= 0.0f) continue;
              const auto d = luma[there] - luma[here];
              const auto weight = std::exp(-static_cast<float>(i * i + j * j) / (2.0f * kSpatial * kSpatial) - d * d * inverse_r);
              sum += weight * luma[there];
              total += weight;
            }
          }
          luma_out[here] = sum / total;
        }
      });
    }

    if (settings.chroma > 1e-6f) {
      constexpr int kRadius = 4;
      constexpr float kSpatial = 2.5f;
      // The colour is smoothed only across pixels whose luma agrees (so it cannot bleed over an edge)
      // and whose colour is near.
      const auto sigma_luma = 0.08f * (1.0f - 0.5f * settings.detail);
      const auto sigma_chroma = (0.02f + 0.25f * settings.chroma) * (1.0f - 0.5f * settings.detail);
      const auto inverse_luma = 1.0f / (2.0f * sigma_luma * sigma_luma);
      const auto inverse_chroma = 1.0f / (2.0f * sigma_chroma * sigma_chroma);
      ParallelRows(y0, y1, [&](int y) {
        for (int x = x0; x <= x1; ++x) {
          const auto here = index(x, y);
          if (current[here].a <= 0.0f) continue;
          float sum_cb = 0.0f, sum_cr = 0.0f, total = 0.0f;
          for (int j = -kRadius; j <= kRadius; ++j) {
            const auto yy = y + j;
            if (yy < y0 || yy > y1) continue;
            for (int i = -kRadius; i <= kRadius; ++i) {
              const auto xx = x + i;
              if (xx < x0 || xx > x1) continue;
              const auto there = index(xx, yy);
              if (current[there].a <= 0.0f) continue;
              const auto dy = luma[there] - luma[here];
              const auto dcb = cb[there] - cb[here], dcr = cr[there] - cr[here];
              const auto weight = std::exp(-static_cast<float>(i * i + j * j) / (2.0f * kSpatial * kSpatial) - dy * dy * inverse_luma -
                                           (dcb * dcb + dcr * dcr) * inverse_chroma);
              sum_cb += weight * cb[there];
              sum_cr += weight * cr[there];
              total += weight;
            }
          }
          cb_out[here] = sum_cb / total;
          cr_out[here] = sum_cr / total;
        }
      });
    }

    for (std::size_t i = 0; i < current.size(); ++i) {
      if (current[i].a <= 0.0f) continue;
      const auto y = luma_out[i];
      const auto r = y + kCrScale * cr_out[i];
      const auto b = y + kCbScale * cb_out[i];
      const auto g = (y - kKr * r - kKb * b) / kKg;
      current[i].r = r;
      current[i].g = g;
      current[i].b = b;
    }
  }

  // Back into the layer, premultiplied, with the alpha it had.
  (void)scratch;
  ParallelRows(y0, y1, [&](int y) {
    for (int x = x0; x <= x1; ++x) {
      const auto& p = current[index(x, y)];
      if (p.a <= 0.0f) continue;
      layer.at(x, y) = {std::clamp(p.r, 0.0f, 1.0f) * p.a, std::clamp(p.g, 0.0f, 1.0f) * p.a, std::clamp(p.b, 0.0f, 1.0f) * p.a, p.a};
    }
  });
}

}  // namespace cutline::render

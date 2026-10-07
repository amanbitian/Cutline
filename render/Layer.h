#pragma once

// The compositor's working surface, shared with the effect implementations.

#include "core/anim/Keyframe.h"
#include "render/ParallelRows.h"
#include "timeline/TimelineCompiler.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace cutline::render {

using timeline::SampledEffect;

// Premultiplied RGBA. Compositing with straight alpha needs a divide per pixel
// per layer and misbehaves where alpha is zero; premultiplied is the standard
// internal representation for exactly that reason.
struct Pixel final {
  float r{0.0f};
  float g{0.0f};
  float b{0.0f};
  float a{0.0f};
};

[[nodiscard]] inline std::optional<anim::Value> FindParameter(const SampledEffect& effect, const std::string& name) {
  for (const auto& parameter : effect.parameters) {
    if (parameter.name == name) return parameter.value;
  }
  return std::nullopt;
}

[[nodiscard]] inline float ScalarParameter(const SampledEffect& effect, const std::string& name, float fallback) {
  const auto value = FindParameter(effect, name);
  return value.has_value() ? static_cast<float>(value->components[0]) : fallback;
}

[[nodiscard]] inline std::array<float, 2> Vec2Parameter(const SampledEffect& effect, const std::string& name, float x,
                                                 float y) {
  const auto value = FindParameter(effect, name);
  if (!value.has_value()) return {x, y};
  return {static_cast<float>(value->components[0]), static_cast<float>(value->components[1])};
}

// ------------------------------------------------------------------ layers ----

// A layer is the output-sized premultiplied buffer one track contributes.
//
// Two things make this affordable at 1920x1080. First, the storage is reused
// between layers and between frames: allocating and zeroing 33 MB per clip per
// frame dominated everything else. Second, each layer tracks the rectangle it
// has actually written, so an effect applied to a quarter-size inset touches a
// quarter of the pixels rather than the whole canvas, and compositing an inset
// skips the rows it does not cover.
class Layer final {
 public:
  void Reset(int width, int height) {
    if (width != width_ || height != height_) {
      width_ = width;
      height_ = height;
      pixels_.assign(static_cast<std::size_t>(width) * height, Pixel{});
      MarkEmpty();
      return;
    }
    // Same size: clear only what the previous use dirtied.
    const auto clear_min_x = min_x_;
    const auto clear_max_x = max_x_;
    ParallelRows(min_y_, max_y_, [&](int y) {
      const auto row = static_cast<std::size_t>(y) * width_;
      std::fill(pixels_.begin() + static_cast<std::ptrdiff_t>(row + clear_min_x),
                pixels_.begin() + static_cast<std::ptrdiff_t>(row + clear_max_x + 1), Pixel{});
    });
    MarkEmpty();
  }

  // Starts a new layer without clearing the old pixels. Use only when the next
  // operation writes every pixel it marks dirty. Full-frame copies/fills use
  // this to avoid zeroing tens of MiB immediately before overwriting it.
  void ResetDiscardingContents(int width, int height) {
    if (width != width_ || height != height_) {
      width_ = width;
      height_ = height;
      pixels_.resize(static_cast<std::size_t>(width) * height);
    }
    MarkEmpty();
  }

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] bool empty() const noexcept { return min_x_ > max_x_ || min_y_ > max_y_; }
  [[nodiscard]] int min_x() const noexcept { return min_x_; }
  [[nodiscard]] int max_x() const noexcept { return max_x_; }
  [[nodiscard]] int min_y() const noexcept { return min_y_; }
  [[nodiscard]] int max_y() const noexcept { return max_y_; }

  [[nodiscard]] Pixel& at(int x, int y) { return pixels_[static_cast<std::size_t>(y) * width_ + x]; }
  [[nodiscard]] const Pixel& at(int x, int y) const { return pixels_[static_cast<std::size_t>(y) * width_ + x]; }

  void MarkDirty(int x0, int y0, int x1, int y1) {
    if (x1 < x0 || y1 < y0) return;
    min_x_ = std::min(min_x_, std::max(0, x0));
    min_y_ = std::min(min_y_, std::max(0, y0));
    max_x_ = std::max(max_x_, std::min(width_ - 1, x1));
    max_y_ = std::max(max_y_, std::min(height_ - 1, y1));
  }

  void MarkWhole() { MarkDirty(0, 0, width_ - 1, height_ - 1); }

  // Source-over: this layer on top of `below`, result written to `below`.
  // Only this layer's dirty rectangle can contribute; everything outside it is
  // transparent and would be a no-op.
  void CompositeOnto(Layer& below) const {
    if (empty()) return;
    ParallelRows(min_y_, max_y_, [&](int y) {
      const auto row = static_cast<std::size_t>(y) * width_;
      for (int x = min_x_; x <= max_x_; ++x) {
        const auto& top = pixels_[row + x];
        if (top.a <= 0.0f && top.r == 0.0f && top.g == 0.0f && top.b == 0.0f) continue;
        auto& bottom = below.pixels_[row + x];
        const auto keep = 1.0f - top.a;
        bottom.r = top.r + bottom.r * keep;
        bottom.g = top.g + bottom.g * keep;
        bottom.b = top.b + bottom.b * keep;
        bottom.a = top.a + bottom.a * keep;
      }
    });
    below.MarkDirty(min_x_, min_y_, max_x_, max_y_);
  }

  // Linear cross-fade of two layers into `into`, used by cross dissolves.
  static void Mix(const Layer& from, const Layer& to, float progress, Layer& into) {
    into.Reset(from.width_, from.height_);
    const auto weight = std::clamp(progress, 0.0f, 1.0f);
    if (from.empty() && to.empty()) return;

    const auto x0 = std::min(from.empty() ? to.min_x_ : from.min_x_, to.empty() ? from.min_x_ : to.min_x_);
    const auto x1 = std::max(from.empty() ? to.max_x_ : from.max_x_, to.empty() ? from.max_x_ : to.max_x_);
    const auto y0 = std::min(from.empty() ? to.min_y_ : from.min_y_, to.empty() ? from.min_y_ : to.min_y_);
    const auto y1 = std::max(from.empty() ? to.max_y_ : from.max_y_, to.empty() ? from.max_y_ : to.max_y_);

    ParallelRows(y0, y1, [&](int y) {
      const auto row = static_cast<std::size_t>(y) * into.width_;
      for (int x = x0; x <= x1; ++x) {
        const auto& left = from.pixels_[row + x];
        const auto& right = to.pixels_[row + x];
        into.pixels_[row + x] = {left.r * (1.0f - weight) + right.r * weight,
                                 left.g * (1.0f - weight) + right.g * weight,
                                 left.b * (1.0f - weight) + right.b * weight,
                                 left.a * (1.0f - weight) + right.a * weight};
      }
    });
    into.MarkDirty(x0, y0, x1, y1);
  }

 private:
  void MarkEmpty() {
    min_x_ = width_;
    min_y_ = height_;
    max_x_ = -1;
    max_y_ = -1;
  }

  int width_{0};
  int height_{0};
  std::vector<Pixel> pixels_;
  int min_x_{0};
  int min_y_{0};
  int max_x_{-1};
  int max_y_{-1};
};

}  // namespace cutline::render

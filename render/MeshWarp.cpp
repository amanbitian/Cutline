#include "render/MeshWarp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cutline::render {
namespace {

[[nodiscard]] Pixel Sample(const Layer& layer, float x, float y) {
  if (x < 0.0f || y < 0.0f || x > static_cast<float>(layer.width() - 1) ||
      y > static_cast<float>(layer.height() - 1)) return {};
  const auto x0 = static_cast<int>(std::floor(x));
  const auto y0 = static_cast<int>(std::floor(y));
  const auto x1 = std::min(x0 + 1, layer.width() - 1);
  const auto y1 = std::min(y0 + 1, layer.height() - 1);
  const auto tx = x - static_cast<float>(x0);
  const auto ty = y - static_cast<float>(y0);
  const auto& a = layer.at(x0, y0);
  const auto& b = layer.at(x1, y0);
  const auto& c = layer.at(x0, y1);
  const auto& d = layer.at(x1, y1);
  const auto mix = [](float p, float q, float t) { return p + (q - p) * t; };
  return {mix(mix(a.r, b.r, tx), mix(c.r, d.r, tx), ty),
          mix(mix(a.g, b.g, tx), mix(c.g, d.g, tx), ty),
          mix(mix(a.b, b.b, tx), mix(c.b, d.b, tx), ty),
          mix(mix(a.a, b.a, tx), mix(c.a, d.a, tx), ty)};
}

void Finish(Layer& layer, Layer& scratch) {
  layer.Reset(scratch.width(), scratch.height());
  if (scratch.empty()) return;
  for (int y = scratch.min_y(); y <= scratch.max_y(); ++y) {
    for (int x = scratch.min_x(); x <= scratch.max_x(); ++x) layer.at(x, y) = scratch.at(x, y);
  }
  layer.MarkDirty(scratch.min_x(), scratch.min_y(), scratch.max_x(), scratch.max_y());
}

}  // namespace

void ApplyMeshWarp(Layer& layer, Layer& scratch, const WarpMesh& mesh) {
  if (layer.empty() || !mesh.valid()) return;
  scratch.Reset(layer.width(), layer.height());
  for (int y = 0; y < layer.height(); ++y) {
    const auto gy = layer.height() > 1 ? static_cast<float>(y) * static_cast<float>(mesh.rows - 1) /
                                           static_cast<float>(layer.height() - 1)
                                     : 0.0f;
    const auto row = std::min(static_cast<int>(std::floor(gy)), mesh.rows - 2);
    const auto ty = gy - static_cast<float>(row);
    for (int x = 0; x < layer.width(); ++x) {
      const auto gx = layer.width() > 1 ? static_cast<float>(x) * static_cast<float>(mesh.columns - 1) /
                                             static_cast<float>(layer.width() - 1)
                                       : 0.0f;
      const auto column = std::min(static_cast<int>(std::floor(gx)), mesh.columns - 2);
      const auto tx = gx - static_cast<float>(column);
      const auto index = [columns = mesh.columns](int cx, int cy) { return cy * columns + cx; };
      const auto& a = mesh.offsets[static_cast<std::size_t>(index(column, row))];
      const auto& b = mesh.offsets[static_cast<std::size_t>(index(column + 1, row))];
      const auto& c = mesh.offsets[static_cast<std::size_t>(index(column, row + 1))];
      const auto& d = mesh.offsets[static_cast<std::size_t>(index(column + 1, row + 1))];
      const auto mix = [](float p, float q, float t) { return p + (q - p) * t; };
      const auto dx = mix(mix(a.x, b.x, tx), mix(c.x, d.x, tx), ty);
      const auto dy = mix(mix(a.y, b.y, tx), mix(c.y, d.y, tx), ty);
      scratch.at(x, y) = Sample(layer, static_cast<float>(x) + dx, static_cast<float>(y) + dy);
    }
  }
  scratch.MarkWhole();
  Finish(layer, scratch);
}

void ApplyRollingShutter(Layer& layer, Layer& scratch, const RollingShutterSettings& settings) {
  if (layer.empty()) return;
  if (std::abs(settings.horizontal) < 1e-6f && std::abs(settings.vertical) < 1e-6f &&
      std::abs(settings.rotation_degrees) < 1e-6f) return;
  scratch.Reset(layer.width(), layer.height());
  const auto center_x = static_cast<float>(layer.width() - 1) * 0.5f;
  const auto center_y = static_cast<float>(layer.height() - 1) * 0.5f;
  const auto radians = settings.rotation_degrees * std::numbers::pi_v<float> / 180.0f;
  for (int y = 0; y < layer.height(); ++y) {
    auto scan = layer.height() > 1 ? static_cast<float>(y) / static_cast<float>(layer.height() - 1) : 0.0f;
    if (settings.bottom_to_top) scan = 1.0f - scan;
    scan = std::clamp(scan + std::clamp(settings.curve, -1.0f, 1.0f) * scan * (1.0f - scan), 0.0f, 1.0f);
    const auto angle = radians * scan;
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    for (int x = 0; x < layer.width(); ++x) {
      const auto local_x = static_cast<float>(x) - center_x;
      const auto local_y = static_cast<float>(y) - center_y;
      const auto sx = cosine * local_x - sine * local_y + center_x + settings.horizontal * scan;
      const auto sy = sine * local_x + cosine * local_y + center_y + settings.vertical * scan;
      scratch.at(x, y) = Sample(layer, sx, sy);
    }
  }
  scratch.MarkWhole();
  Finish(layer, scratch);
}

}  // namespace cutline::render

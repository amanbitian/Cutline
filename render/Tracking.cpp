#include "render/Tracking.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace cutline::render::tracking {

using Complex = std::complex<double>;

// ------------------------------------------------------------------- images ----

float Gray::Sample(double x, double y) const {
  x = std::clamp(x, 0.0, static_cast<double>(width - 1));
  y = std::clamp(y, 0.0, static_cast<double>(height - 1));
  const auto x0 = static_cast<int>(x);
  const auto y0 = static_cast<int>(y);
  const auto x1 = std::min(x0 + 1, width - 1);
  const auto y1 = std::min(y0 + 1, height - 1);
  const auto tx = static_cast<float>(x - x0);
  const auto ty = static_cast<float>(y - y0);
  const auto top = At(x0, y0) * (1.0f - tx) + At(x1, y0) * tx;
  const auto bottom = At(x0, y1) * (1.0f - tx) + At(x1, y1) * tx;
  return top * (1.0f - ty) + bottom * ty;
}

Gray ToGray(const media::VideoFrame& input, int target_width) {
  if (!input.valid()) throw std::invalid_argument("Tracking needs a valid frame");
  media::VideoFrame converted;
  const media::VideoFrame* frame = &input;
  if (input.format() != media::PixelFormat::RgbaF32) {
    converted = media::ConvertFrame(input, media::PixelFormat::RgbaF32);
    frame = &converted;
  }
  const auto luma = [&](int x, int y) {
    const auto* texel = frame->row_f32(y) + static_cast<std::size_t>(x) * 4;
    return texel[0] * 0.2126f + texel[1] * 0.7152f + texel[2] * 0.0722f;
  };
  Gray out;
  if (target_width <= 0 || target_width >= frame->width()) {
    out.width = frame->width();
    out.height = frame->height();
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y) {
      for (int x = 0; x < out.width; ++x) out.pixels[static_cast<std::size_t>(y) * out.width + x] = luma(x, y);
    }
    return out;
  }
  out.width = target_width;
  out.height = std::max(8, static_cast<int>(std::lround(static_cast<double>(frame->height()) * target_width / frame->width())));
  out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
  const auto step_x = static_cast<double>(frame->width()) / out.width;
  const auto step_y = static_cast<double>(frame->height()) / out.height;
  const auto samples_x = std::max(1, static_cast<int>(std::ceil(step_x)));
  const auto samples_y = std::max(1, static_cast<int>(std::ceil(step_y)));
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      double sum = 0.0;
      for (int j = 0; j < samples_y; ++j) {
        const auto sy = std::min(frame->height() - 1, static_cast<int>((y + (j + 0.5) / samples_y) * step_y));
        for (int i = 0; i < samples_x; ++i) {
          const auto sx = std::min(frame->width() - 1, static_cast<int>((x + (i + 0.5) / samples_x) * step_x));
          sum += luma(sx, sy);
        }
      }
      out.pixels[static_cast<std::size_t>(y) * out.width + x] = static_cast<float>(sum / (samples_x * samples_y));
    }
  }
  return out;
}

Pyramid BuildPyramid(const Gray& image, int levels) {
  Pyramid pyramid;
  pyramid.levels.push_back(image);
  while (static_cast<int>(pyramid.levels.size()) < levels) {
    const auto& previous = pyramid.levels.back();
    if (previous.width < 32 || previous.height < 32) break;
    Gray next;
    next.width = previous.width / 2;
    next.height = previous.height / 2;
    next.pixels.resize(static_cast<std::size_t>(next.width) * next.height);
    for (int y = 0; y < next.height; ++y) {
      for (int x = 0; x < next.width; ++x) {
        next.pixels[static_cast<std::size_t>(y) * next.width + x] =
            0.25f * (previous.At(2 * x, 2 * y) + previous.At(2 * x + 1, 2 * y) + previous.At(2 * x, 2 * y + 1) + previous.At(2 * x + 1, 2 * y + 1));
      }
    }
    pyramid.levels.push_back(std::move(next));
  }
  return pyramid;
}

// ------------------------------------------------------------------- warps ----

Warp Warp::Then(const Warp& next) const {
  Warp out;
  out.a11 = next.a11 * a11 + next.a12 * a21;
  out.a12 = next.a11 * a12 + next.a12 * a22;
  out.a21 = next.a21 * a11 + next.a22 * a21;
  out.a22 = next.a21 * a12 + next.a22 * a22;
  out.tx = next.a11 * tx + next.a12 * ty + next.tx;
  out.ty = next.a21 * tx + next.a22 * ty + next.ty;
  return out;
}

Warp Warp::Inverse() const {
  const auto determinant = a11 * a22 - a12 * a21;
  Warp out;
  out.a11 = a22 / determinant;
  out.a12 = -a12 / determinant;
  out.a21 = -a21 / determinant;
  out.a22 = a11 / determinant;
  out.tx = -(out.a11 * tx + out.a12 * ty);
  out.ty = -(out.a21 * tx + out.a22 * ty);
  return out;
}

double Warp::Rotation() const { return std::atan2(a21 - a12, a11 + a22); }
double Warp::Scale() const { return std::hypot(a11 + a22, a21 - a12) * 0.5; }

Warp Warp::Similarity(double scale, double rotation_radians, double tx, double ty) {
  const auto c = scale * std::cos(rotation_radians);
  const auto s = scale * std::sin(rotation_radians);
  return {c, -s, s, c, tx, ty};
}

// ----------------------------------------------------------------- alignment ----

namespace {

[[nodiscard]] int ParameterCount(Model model) {
  switch (model) {
    case Model::Translation: return 2;
    case Model::Similarity: return 4;
    case Model::Affine: return 6;
  }
  return 2;
}

// Parameters are about the patch centre (cx, cy): the shift of the centre, then the model's own.
//   Translation: (dx, dy)
//   Similarity:  (dx, dy, rotation, scale)
//   Affine:      (dx, dy, a11 - 1, a12, a21, a22 - 1)
struct Parameters final {
  std::array<double, 6> p{};
};

[[nodiscard]] Parameters ParametersOf(const Warp& warp, Model model, double cx, double cy) {
  Parameters out;
  const auto centre = warp.Apply(cx, cy);
  out.p[0] = centre.first - cx;
  out.p[1] = centre.second - cy;
  if (model == Model::Similarity) {
    out.p[2] = warp.Rotation();
    out.p[3] = warp.Scale();
  } else if (model == Model::Affine) {
    out.p[2] = warp.a11 - 1.0;
    out.p[3] = warp.a12;
    out.p[4] = warp.a21;
    out.p[5] = warp.a22 - 1.0;
  }
  return out;
}

[[nodiscard]] Warp WarpOf(const Parameters& parameters, Model model, double cx, double cy) {
  Warp warp;
  if (model == Model::Translation) {
    warp.tx = parameters.p[0];
    warp.ty = parameters.p[1];
    return warp;
  }
  if (model == Model::Similarity) {
    warp = Warp::Similarity(parameters.p[3], parameters.p[2], 0.0, 0.0);
  } else {
    warp.a11 = 1.0 + parameters.p[2];
    warp.a12 = parameters.p[3];
    warp.a21 = parameters.p[4];
    warp.a22 = 1.0 + parameters.p[5];
  }
  // The centre goes to centre + (dx, dy): x' = A (x - c) + c + d.
  warp.tx = cx + parameters.p[0] - (warp.a11 * cx + warp.a12 * cy);
  warp.ty = cy + parameters.p[1] - (warp.a21 * cx + warp.a22 * cy);
  return warp;
}

// Solves the n x n system in place by Gaussian elimination with partial pivoting.
[[nodiscard]] bool Solve(std::array<std::array<double, 7>, 6>& m, int n, std::array<double, 6>& x) {
  for (int column = 0; column < n; ++column) {
    int pivot = column;
    for (int row = column + 1; row < n; ++row) {
      if (std::abs(m[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)]) > std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(column)])) pivot = row;
    }
    if (std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(column)]) < 1e-12) return false;
    std::swap(m[static_cast<std::size_t>(pivot)], m[static_cast<std::size_t>(column)]);
    for (int row = column + 1; row < n; ++row) {
      const auto factor = m[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] / m[static_cast<std::size_t>(column)][static_cast<std::size_t>(column)];
      for (int k = column; k <= n; ++k) m[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] -= factor * m[static_cast<std::size_t>(column)][static_cast<std::size_t>(k)];
    }
  }
  for (int row = n - 1; row >= 0; --row) {
    double sum = m[static_cast<std::size_t>(row)][static_cast<std::size_t>(n)];
    for (int k = row + 1; k < n; ++k) sum -= m[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] * x[static_cast<std::size_t>(k)];
    x[static_cast<std::size_t>(row)] = sum / m[static_cast<std::size_t>(row)][static_cast<std::size_t>(row)];
  }
  return true;
}

struct PatchPoint final {
  double x, y;  // reference coordinates
  float t;      // reference value
};

[[nodiscard]] AlignResult AlignLevel(const Gray& reference, const Gray& target, const Region& region, Model model, const Warp& initial,
                                     const AlignConfig& config) {
  AlignResult result;
  result.warp = initial;
  const auto cx = region.x + region.width * 0.5;
  const auto cy = region.y + region.height * 0.5;

  std::vector<PatchPoint> patch;
  const auto x_begin = std::max(1, static_cast<int>(std::ceil(region.x)));
  const auto x_end = std::min(reference.width - 2, static_cast<int>(std::ceil(region.x + region.width)) - 1);
  const auto y_begin = std::max(1, static_cast<int>(std::ceil(region.y)));
  const auto y_end = std::min(reference.height - 2, static_cast<int>(std::ceil(region.y + region.height)) - 1);
  const auto stride = std::max(1, config.stride);
  for (int y = y_begin; y <= y_end; y += stride) {
    for (int x = x_begin; x <= x_end; x += stride) patch.push_back({static_cast<double>(x), static_cast<double>(y), reference.At(x, y)});
  }
  if (patch.size() < 16) return result;

  const auto n = ParameterCount(model);
  auto parameters = ParametersOf(initial, model, cx, cy);
  const auto radius = std::max(1.0, std::hypot(region.width, region.height) * 0.5);

  struct Sample final {
    double dx, dy;
    float intensity, gx, gy, t;
  };
  std::vector<Sample> samples;
  samples.reserve(patch.size());
  const auto gather = [&](const Warp& warp) {
    samples.clear();
    for (const auto& point : patch) {
      const auto mapped = warp.Apply(point.x, point.y);
      if (mapped.first < 2.0 || mapped.second < 2.0 || mapped.first > target.width - 3.0 || mapped.second > target.height - 3.0) continue;
      const auto gx = 0.5f * (target.Sample(mapped.first + 1.0, mapped.second) - target.Sample(mapped.first - 1.0, mapped.second));
      const auto gy = 0.5f * (target.Sample(mapped.first, mapped.second + 1.0) - target.Sample(mapped.first, mapped.second - 1.0));
      samples.push_back({point.x - cx, point.y - cy, target.Sample(mapped.first, mapped.second), gx, gy, point.t});
    }
  };

  for (int iteration = 0; iteration < config.max_iterations; ++iteration) {
    const auto warp = WarpOf(parameters, model, cx, cy);
    gather(warp);
    if (samples.size() < patch.size() / 4 || samples.size() < 16) {
      result.warp = warp;
      result.iterations = iteration;
      result.confidence = 0.0;
      return result;
    }
    double mean_i = 0.0, mean_t = 0.0;
    for (const auto& s : samples) {
      mean_i += s.intensity;
      mean_t += s.t;
    }
    mean_i /= static_cast<double>(samples.size());
    mean_t /= static_cast<double>(samples.size());

    const double theta = model == Model::Similarity ? parameters.p[2] : 0.0;
    const double scale = model == Model::Similarity ? parameters.p[3] : 1.0;
    const auto cos_t = std::cos(theta), sin_t = std::sin(theta);

    std::array<std::array<double, 7>, 6> system{};
    for (const auto& s : samples) {
      const auto error = (static_cast<double>(s.intensity) - mean_i) - (static_cast<double>(s.t) - mean_t);
      // d(warped position) / d(parameter), for each parameter.
      std::array<double, 6> wx{}, wy{};
      wx[0] = 1.0;
      wy[1] = 1.0;
      if (model == Model::Similarity) {
        wx[2] = scale * (-sin_t * s.dx - cos_t * s.dy);
        wy[2] = scale * (cos_t * s.dx - sin_t * s.dy);
        wx[3] = cos_t * s.dx - sin_t * s.dy;
        wy[3] = sin_t * s.dx + cos_t * s.dy;
      } else if (model == Model::Affine) {
        wx[2] = s.dx;
        wx[3] = s.dy;
        wy[4] = s.dx;
        wy[5] = s.dy;
      }
      std::array<double, 6> jacobian{};
      for (int k = 0; k < n; ++k) jacobian[static_cast<std::size_t>(k)] = s.gx * wx[static_cast<std::size_t>(k)] + s.gy * wy[static_cast<std::size_t>(k)];
      for (int row = 0; row < n; ++row) {
        for (int column = 0; column < n; ++column) {
          system[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] += jacobian[static_cast<std::size_t>(row)] * jacobian[static_cast<std::size_t>(column)];
        }
        system[static_cast<std::size_t>(row)][static_cast<std::size_t>(n)] -= jacobian[static_cast<std::size_t>(row)] * error;
      }
    }
    double trace = 0.0;
    for (int k = 0; k < n; ++k) trace += system[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)];
    for (int k = 0; k < n; ++k) system[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)] += 1e-6 * (trace / n) + 1e-9;
    std::array<double, 6> delta{};
    if (!Solve(system, n, delta)) break;
    double largest = 0.0;
    for (int k = 0; k < n; ++k) {
      parameters.p[static_cast<std::size_t>(k)] += delta[static_cast<std::size_t>(k)];
      // Translation is in pixels; the rest scale a point at the patch's edge by this much.
      const auto weight = k < 2 ? 1.0 : radius;
      largest = std::max(largest, std::abs(delta[static_cast<std::size_t>(k)]) * weight);
    }
    result.iterations = iteration + 1;
    if (largest < config.convergence) {
      result.converged = true;
      break;
    }
    // A runaway: give up on this level rather than chase a wild answer.
    if (std::abs(parameters.p[0]) > target.width || std::abs(parameters.p[1]) > target.height) break;
    if (model == Model::Similarity && (parameters.p[3] < 0.25 || parameters.p[3] > 4.0)) break;
  }

  result.warp = WarpOf(parameters, model, cx, cy);
  gather(result.warp);
  if (samples.size() >= patch.size() / 4 && samples.size() >= 16) {
    double mean_i = 0.0, mean_t = 0.0;
    for (const auto& s : samples) {
      mean_i += s.intensity;
      mean_t += s.t;
    }
    mean_i /= static_cast<double>(samples.size());
    mean_t /= static_cast<double>(samples.size());
    double ii = 0.0, tt = 0.0, it = 0.0;
    for (const auto& s : samples) {
      const auto a = s.intensity - mean_i, b = s.t - mean_t;
      ii += a * a;
      tt += b * b;
      it += a * b;
    }
    result.rms = std::sqrt(std::max(0.0, ii + tt - 2.0 * it) / static_cast<double>(samples.size()));
    // A patch with no texture at all matches nothing in particular.
    result.confidence = (ii > 1e-8 && tt > 1e-8) ? std::clamp(it / std::sqrt(ii * tt), 0.0, 1.0) : 0.0;
  }
  return result;
}

}  // namespace

AlignResult Align(const Pyramid& reference, const Pyramid& target, const Region& region, Model model, const Warp& initial,
                  const AlignConfig& config) {
  if (reference.levels.empty() || target.levels.empty()) throw std::invalid_argument("Align needs images");
  // As many levels as the patch survives: at the coarsest it must still be a dozen pixels across.
  int levels = std::min({config.pyramid_levels, static_cast<int>(reference.levels.size()), static_cast<int>(target.levels.size())});
  while (levels > 1 && std::min(region.width, region.height) / std::pow(2.0, levels - 1) < config.minimum_patch) --levels;
  levels = std::max(levels, 1);

  Warp warp = initial;
  AlignResult result;
  for (int level = levels - 1; level >= 0; --level) {
    const auto scale = std::pow(2.0, -level);
    const Region scaled{region.x * scale, region.y * scale, region.width * scale, region.height * scale};
    Warp at_level = warp;
    at_level.tx *= scale;
    at_level.ty *= scale;
    result = AlignLevel(reference.levels[static_cast<std::size_t>(level)], target.levels[static_cast<std::size_t>(level)], scaled, model,
                        at_level, config);
    warp = result.warp;
    warp.tx /= scale;
    warp.ty /= scale;
  }
  result.warp = warp;
  return result;
}

// ------------------------------------------------------------ point tracking ----

namespace {

struct Prepared final {
  Pyramid pyramid;
  double scale{1.0};  // analysis pixels per frame pixel
};

[[nodiscard]] Prepared Prepare(const media::VideoFrame& frame, int analysis_width, int levels) {
  Prepared prepared;
  auto gray = ToGray(frame, analysis_width);
  prepared.scale = static_cast<double>(gray.width) / frame.width();
  prepared.pyramid = BuildPyramid(gray, levels);
  return prepared;
}

const media::VideoFrame& Require(const FrameResolver& resolve, std::size_t index) {
  const auto* frame = resolve(index);
  if (frame == nullptr || !frame->valid()) throw std::runtime_error("Tracking could not resolve frame " + std::to_string(index));
  return *frame;
}

}  // namespace

TrackResult TrackPoint(std::size_t frame_count, time::FrameRate rate, const FrameResolver& resolve, std::size_t first, double x, double y,
                       const PointTrackerConfig& config, const CancelCheck& cancel, const ProgressCallback& progress) {
  if (first >= frame_count) throw std::invalid_argument("The track starts after the last frame");
  if (!resolve) throw std::invalid_argument("Tracking has no frame resolver");
  if (config.patch_radius < 4) throw std::invalid_argument("The tracking patch is too small");
  TrackResult result;
  result.algorithm = "point_lk_ncc_v1";

  const auto& first_frame = Require(resolve, first);
  result.source_width = first_frame.width();
  result.source_height = first_frame.height();
  auto reference = Prepare(first_frame, config.analysis_width, config.align.pyramid_levels);
  const auto s = reference.scale;
  const auto radius = config.patch_radius;
  auto position = std::pair<double, double>{x * s, y * s};   // analysis pixels
  auto velocity = std::pair<double, double>{0.0, 0.0};
  auto anchor = position;

  result.samples.push_back({time::RationalTime::FromFrames(static_cast<std::int64_t>(first), rate), x, y, 1.0, true});
  std::int64_t invalid_run_start = -1;
  const auto total = frame_count - first;
  for (std::size_t index = first + 1; index < frame_count; ++index) {
    if (cancel && cancel()) {
      result.cancelled = true;
      break;
    }
    const auto& frame = Require(resolve, index);
    if (frame.width() != result.source_width || frame.height() != result.source_height) throw std::invalid_argument("Tracking frame size changed");
    auto current = Prepare(frame, config.analysis_width, config.align.pyramid_levels);

    const auto prediction = std::pair<double, double>{position.first + velocity.first, position.second + velocity.second};
    const auto centre = config.anchored ? anchor : position;
    const Region region{centre.first - radius, centre.second - radius, 2.0 * radius + 1.0, 2.0 * radius + 1.0};
    const auto start = Warp::Translation(prediction.first - centre.first, prediction.second - centre.second);
    const auto aligned = Align(reference.pyramid, current.pyramid, region, Model::Translation, start, config.align);
    const auto found = std::pair<double, double>{centre.first + aligned.warp.tx, centre.second + aligned.warp.ty};

    PointSample sample;
    sample.time = time::RationalTime::FromFrames(static_cast<std::int64_t>(index), rate);
    sample.confidence = aligned.confidence;
    sample.valid = aligned.confidence >= config.min_confidence;
    if (sample.valid) {
      velocity = {found.first - position.first, found.second - position.second};
      position = found;
      invalid_run_start = -1;
      if (!config.anchored) reference = std::move(current);
    } else {
      // Not found: carry on at the predicted place and keep looking from there.
      position = prediction;
      if (invalid_run_start < 0) invalid_run_start = static_cast<std::int64_t>(index);
    }
    sample.x = position.first / s;
    sample.y = position.second / s;
    result.samples.push_back(sample);
    if (progress) progress(index - first + 1, total);
  }
  result.lost_at = invalid_run_start;
  return result;
}

PlanarTrackResult TrackPlane(std::size_t frame_count, time::FrameRate rate, const FrameResolver& resolve, std::size_t first,
                             const Region& region_in_frame, const PlanarTrackerConfig& config, const CancelCheck& cancel,
                             const ProgressCallback& progress) {
  if (first >= frame_count) throw std::invalid_argument("The track starts after the last frame");
  if (!resolve) throw std::invalid_argument("Tracking has no frame resolver");
  if (region_in_frame.width < 16 || region_in_frame.height < 16) throw std::invalid_argument("The tracked region is too small");
  PlanarTrackResult result;
  result.algorithm = "planar_lk_ncc_v1";
  result.region = region_in_frame;

  const auto& first_frame = Require(resolve, first);
  result.source_width = first_frame.width();
  result.source_height = first_frame.height();
  const auto reference = Prepare(first_frame, config.analysis_width, config.align.pyramid_levels);
  const auto s = reference.scale;
  const Region region{region_in_frame.x * s, region_in_frame.y * s, region_in_frame.width * s, region_in_frame.height * s};
  const auto to_frame = [&](const Warp& analysis) {
    // x_frame' = warp(x_frame * s) / s
    Warp out = analysis;
    out.tx /= s;
    out.ty /= s;
    return out;
  };

  Warp warp;
  result.samples.push_back({time::RationalTime::FromFrames(static_cast<std::int64_t>(first), rate), Warp{}, 1.0, true});
  std::int64_t invalid_run_start = -1;
  const auto total = frame_count - first;
  for (std::size_t index = first + 1; index < frame_count; ++index) {
    if (cancel && cancel()) {
      result.cancelled = true;
      break;
    }
    const auto& frame = Require(resolve, index);
    if (frame.width() != result.source_width || frame.height() != result.source_height) throw std::invalid_argument("Tracking frame size changed");
    const auto current = Prepare(frame, config.analysis_width, config.align.pyramid_levels);
    const auto aligned = Align(reference.pyramid, current.pyramid, region, config.model, warp, config.align);
    PlanarSample sample;
    sample.time = time::RationalTime::FromFrames(static_cast<std::int64_t>(index), rate);
    sample.confidence = aligned.confidence;
    sample.valid = aligned.confidence >= config.min_confidence;
    if (sample.valid) {
      warp = aligned.warp;
      invalid_run_start = -1;
    } else if (invalid_run_start < 0) {
      invalid_run_start = static_cast<std::int64_t>(index);
    }
    sample.warp = to_frame(warp);
    result.samples.push_back(sample);
    if (progress) progress(index - first + 1, total);
  }
  result.lost_at = invalid_run_start;
  return result;
}

timeline::Effect MakeTrackedMotionEffect(const TrackResult& track, std::string effect_id, std::int64_t order) {
  if (track.samples.empty()) throw std::invalid_argument("Cannot build an effect from an empty track");
  timeline::Effect effect;
  effect.id = std::move(effect_id);
  effect.effect_type = "motion";
  effect.order = order;
  timeline::Parameter position;
  position.id = effect.id + ":position";
  position.name = "position";
  std::vector<anim::Keyframe> keys;
  const auto& origin = track.samples.front();
  for (const auto& sample : track.samples) {
    keys.push_back({sample.time, anim::Value::Vec2(sample.x - origin.x, sample.y - origin.y), anim::Interpolation::Linear, {}, {}});
  }
  position.value = anim::AnimatedValue(std::move(keys));
  effect.parameters.push_back(std::move(position));
  return effect;
}

timeline::Effect MakeTrackedMotionEffect(const PlanarTrackResult& track, std::string effect_id, std::int64_t order) {
  if (track.samples.empty()) throw std::invalid_argument("Cannot build an effect from an empty track");
  timeline::Effect effect;
  effect.id = std::move(effect_id);
  effect.effect_type = "motion";
  effect.order = order;
  // A layer rotates and scales about the middle of the frame, so the position that makes it follow
  // the plane is where the plane's warp sends that point, less where it was.
  const auto cx = track.source_width * 0.5;
  const auto cy = track.source_height * 0.5;
  std::vector<anim::Keyframe> positions, rotations, scales;
  for (const auto& sample : track.samples) {
    const auto moved = sample.warp.Apply(cx, cy);
    positions.push_back({sample.time, anim::Value::Vec2(moved.first - cx, moved.second - cy), anim::Interpolation::Linear, {}, {}});
    rotations.push_back({sample.time, anim::Value::Scalar(sample.warp.Rotation() * 180.0 / std::numbers::pi), anim::Interpolation::Linear, {}, {}});
    const auto scale = sample.warp.Scale() * 100.0;
    scales.push_back({sample.time, anim::Value::Vec2(scale, scale), anim::Interpolation::Linear, {}, {}});
  }
  for (auto& [name, keys] : std::vector<std::pair<std::string, std::vector<anim::Keyframe>>>{
           {"position", std::move(positions)}, {"rotation", std::move(rotations)}, {"scale", std::move(scales)}}) {
    timeline::Parameter parameter;
    parameter.id = effect.id + ":" + name;
    parameter.name = name;
    parameter.value = anim::AnimatedValue(std::move(keys));
    effect.parameters.push_back(std::move(parameter));
  }
  return effect;
}

bool IsStale(const std::string& analysed_fingerprint, const std::string& current_fingerprint) {
  return analysed_fingerprint != current_fingerprint;
}

// ------------------------------------------------- similarity stabilisation ----

namespace {

struct Feature final {
  double x, y;
};

// Shi-Tomasi corners: where the smaller eigenvalue of the local gradient matrix is large, which is
// where a patch can be followed in both directions. Spread out by a minimum spacing.
std::vector<Feature> DetectFeatures(const Gray& image, int count, int spacing) {
  const auto margin = 14;
  if (image.width < 2 * margin + 8 || image.height < 2 * margin + 8) return {};
  const auto width = image.width, height = image.height;
  std::vector<float> xx(static_cast<std::size_t>(width) * height, 0.0f), xy = xx, yy = xx;
  for (int y = 1; y < height - 1; ++y) {
    for (int x = 1; x < width - 1; ++x) {
      const auto gx = 0.5f * (image.At(x + 1, y) - image.At(x - 1, y));
      const auto gy = 0.5f * (image.At(x, y + 1) - image.At(x, y - 1));
      const auto i = static_cast<std::size_t>(y) * width + x;
      xx[i] = gx * gx;
      xy[i] = gx * gy;
      yy[i] = gy * gy;
    }
  }
  const auto window = 3;  // a 7 x 7 window
  const auto box = [&](const std::vector<float>& plane, int x, int y) {
    double sum = 0.0;
    for (int j = -window; j <= window; ++j) {
      for (int i = -window; i <= window; ++i) sum += plane[static_cast<std::size_t>(y + j) * width + (x + i)];
    }
    return sum;
  };
  struct Candidate final {
    double quality;
    int x, y;
  };
  std::vector<Candidate> candidates;
  double best = 0.0;
  for (int y = margin; y < height - margin; y += 2) {
    for (int x = margin; x < width - margin; x += 2) {
      const auto a = box(xx, x, y), b = box(xy, x, y), c = box(yy, x, y);
      const auto lambda = 0.5 * ((a + c) - std::sqrt((a - c) * (a - c) + 4.0 * b * b));
      if (lambda > 0.0) candidates.push_back({lambda, x, y});
      best = std::max(best, lambda);
    }
  }
  if (best < 1e-6) return {};  // nothing to follow: a blank or featureless frame
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& p, const Candidate& q) {
    if (p.quality != q.quality) return p.quality > q.quality;
    return p.y != q.y ? p.y < q.y : p.x < q.x;  // a fixed order among equals
  });
  std::vector<Feature> chosen;
  const auto cells_x = width / spacing + 1, cells_y = height / spacing + 1;
  std::vector<char> taken(static_cast<std::size_t>(cells_x) * cells_y, 0);
  for (const auto& candidate : candidates) {
    if (candidate.quality < 0.01 * best) break;
    const auto cx = candidate.x / spacing, cy = candidate.y / spacing;
    bool free = true;
    for (int j = -1; j <= 1 && free; ++j) {
      for (int i = -1; i <= 1; ++i) {
        const auto nx = cx + i, ny = cy + j;
        if (nx < 0 || ny < 0 || nx >= cells_x || ny >= cells_y) continue;
        if (taken[static_cast<std::size_t>(ny) * cells_x + nx] != 0) {
          free = false;
          break;
        }
      }
    }
    if (!free) continue;
    taken[static_cast<std::size_t>(cy) * cells_x + cx] = 1;
    chosen.push_back({static_cast<double>(candidate.x), static_cast<double>(candidate.y)});
    if (static_cast<int>(chosen.size()) >= count) break;
  }
  return chosen;
}

// z' = a z + b over complex coordinates: a rotation, a scale and a shift.
struct SimilarityFit final {
  Complex a{1.0, 0.0};
  Complex b{0.0, 0.0};
};

[[nodiscard]] SimilarityFit FitSimilarity(const std::vector<Complex>& from, const std::vector<Complex>& to,
                                          const std::vector<std::size_t>& indices) {
  Complex mean_from{}, mean_to{};
  for (const auto i : indices) {
    mean_from += from[i];
    mean_to += to[i];
  }
  mean_from /= static_cast<double>(indices.size());
  mean_to /= static_cast<double>(indices.size());
  Complex numerator{};
  double denominator = 0.0;
  for (const auto i : indices) {
    const auto f = from[i] - mean_from;
    numerator += std::conj(f) * (to[i] - mean_to);
    denominator += std::norm(f);
  }
  SimilarityFit fit;
  if (denominator < 1e-12) return fit;
  fit.a = numerator / denominator;
  fit.b = mean_to - fit.a * mean_from;
  return fit;
}

[[nodiscard]] SimilarityFit RobustSimilarity(const std::vector<Complex>& from, const std::vector<Complex>& to, int iterations,
                                             double threshold, int* inliers_out) {
  const auto n = from.size();
  *inliers_out = 0;
  SimilarityFit identity;
  if (n < 4) return identity;
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::size_t>(state >> 33);
  };
  std::vector<std::size_t> best;
  double best_error = std::numeric_limits<double>::max();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    const auto i = next() % n;
    const auto j = next() % n;
    if (i == j || std::abs(from[i] - from[j]) < 6.0) continue;
    const auto fit = FitSimilarity(from, to, {i, j});
    std::vector<std::size_t> inliers;
    double error = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
      const auto residual = std::abs(fit.a * from[k] + fit.b - to[k]);
      if (residual < threshold) {
        inliers.push_back(k);
        error += residual;
      }
    }
    if (inliers.size() > best.size() || (inliers.size() == best.size() && error < best_error)) {
      best = std::move(inliers);
      best_error = error;
    }
  }
  if (best.size() < std::max<std::size_t>(6, n * 3 / 10)) return identity;
  // Refit on everything that agreed, and once more, so the answer is not that of two points.
  auto fit = FitSimilarity(from, to, best);
  for (int round = 0; round < 2; ++round) {
    std::vector<std::size_t> inliers;
    for (std::size_t k = 0; k < n; ++k) {
      if (std::abs(fit.a * from[k] + fit.b - to[k]) < threshold) inliers.push_back(k);
    }
    if (inliers.size() < 6) break;
    fit = FitSimilarity(from, to, inliers);
    best = std::move(inliers);
  }
  *inliers_out = static_cast<int>(best.size());
  return fit;
}

// A frame-to-frame similarity: the tracked features that survive a check by tracking them back.
struct StepResult final {
  SimilarityFit fit;
  int tracked{0};
  int inliers{0};
};

StepResult EstimateStep(const Pyramid& previous, const Pyramid& current, const SimilarityStabilizerConfig& config) {
  StepResult step;
  const auto features = DetectFeatures(previous.levels.front(), config.feature_count, config.feature_spacing);
  std::vector<Complex> from, to;
  AlignConfig align;
  align.pyramid_levels = 3;
  align.max_iterations = 30;
  align.minimum_patch = 7.0;
  constexpr double kRadius = 7.0;
  for (const auto& feature : features) {
    const Region region{feature.x - kRadius, feature.y - kRadius, 2 * kRadius + 1, 2 * kRadius + 1};
    const auto forward = Align(previous, current, region, Model::Translation, Warp{}, align);
    if (forward.confidence < 0.85) continue;
    // Back again from where it landed: a feature that does not return to where it started was not followed.
    const Region landed{feature.x + forward.warp.tx - kRadius, feature.y + forward.warp.ty - kRadius, 2 * kRadius + 1, 2 * kRadius + 1};
    const auto backward = Align(current, previous, landed, Model::Translation, Warp::Translation(-forward.warp.tx, -forward.warp.ty), align);
    if (std::hypot(forward.warp.tx + backward.warp.tx, forward.warp.ty + backward.warp.ty) > 0.75) continue;
    from.emplace_back(feature.x, feature.y);
    to.emplace_back(feature.x + forward.warp.tx, feature.y + forward.warp.ty);
  }
  step.tracked = static_cast<int>(from.size());
  step.fit = RobustSimilarity(from, to, config.ransac_iterations, config.inlier_threshold, &step.inliers);
  return step;
}

struct CenterForm final {
  Complex a{1.0, 0.0};  // rotation and scale
  Complex t{0.0, 0.0};  // shift of the image centre
};

}  // namespace

SimilarityResult AnalyzeSimilarityStabilization(std::size_t frame_count, time::FrameRate rate, const FrameResolver& resolve,
                                                const SimilarityStabilizerConfig& config, const CancelCheck& cancel,
                                                const ProgressCallback& progress) {
  if (frame_count == 0) throw std::invalid_argument("Stabilisation needs at least one frame");
  if (!resolve) throw std::invalid_argument("Stabilisation has no frame resolver");
  if (config.analysis_width < 64 || config.smoothing_radius_frames < 0) throw std::invalid_argument("Stabilisation configuration is invalid");

  SimilarityResult result;
  const auto& first = Require(resolve, 0);
  result.source_width = first.width();
  result.source_height = first.height();
  const Complex centre{result.source_width * 0.5, result.source_height * 0.5};

  auto previous = Prepare(first, config.analysis_width, 3);
  const auto scale_up = 1.0 / previous.scale;  // frame pixels per analysis pixel
  std::vector<CenterForm> path(frame_count);
  std::vector<int> inliers(frame_count, 0), tracked(frame_count, 0);
  // In absolute coordinates for the composition: z' = a z + b.
  Complex abs_a{1.0, 0.0}, abs_b{0.0, 0.0};
  for (std::size_t index = 1; index < frame_count; ++index) {
    if (cancel && cancel()) {
      result.cancelled = true;
      path.resize(index);
      inliers.resize(index);
      tracked.resize(index);
      break;
    }
    const auto& frame = Require(resolve, index);
    if (frame.width() != result.source_width || frame.height() != result.source_height) throw std::invalid_argument("Stabilisation frame size changed");
    auto current = Prepare(frame, config.analysis_width, 3);
    const auto step = EstimateStep(previous.pyramid, current.pyramid, config);
    inliers[index] = step.inliers;
    tracked[index] = step.tracked;
    if (step.inliers == 0) result.uncertain_frames.push_back(index);
    // The step in frame pixels: scaling the coordinates scales the shift and leaves the rotation and zoom.
    const Complex step_a = step.fit.a;
    const Complex step_b = step.fit.b * scale_up;
    abs_a = step_a * abs_a;
    abs_b = step_a * abs_b + step_b;
    path[index].a = abs_a;
    path[index].t = abs_a * centre + abs_b - centre;
    previous = std::move(current);
    if (progress) progress(index, frame_count);
  }

  const auto count = path.size();
  // The camera's path as measured, for review.
  std::vector<double> px(count), py(count), angle(count), log_scale(count);
  for (std::size_t i = 0; i < count; ++i) {
    px[i] = path[i].t.real();
    py[i] = path[i].t.imag();
    angle[i] = std::arg(path[i].a);
    log_scale[i] = std::log(std::abs(path[i].a));
    if (i > 0) {
      // Unwrap, so a camera that turns past a half turn is not read as flipping.
      while (angle[i] - angle[i - 1] > std::numbers::pi) angle[i] -= 2.0 * std::numbers::pi;
      while (angle[i] - angle[i - 1] < -std::numbers::pi) angle[i] += 2.0 * std::numbers::pi;
    }
    if (!config.correct_rotation) angle[i] = 0.0;
    if (!config.correct_scale) log_scale[i] = 0.0;
  }
  const auto smooth = [&](const std::vector<double>& values, std::size_t i) {
    const auto radius = static_cast<std::int64_t>(config.smoothing_radius_frames);
    double sum = 0.0;
    for (std::int64_t offset = -radius; offset <= radius; ++offset) {
      sum += values[static_cast<std::size_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(i) + offset, 0, static_cast<std::int64_t>(count) - 1))];
    }
    return sum / static_cast<double>(2 * radius + 1);
  };

  // The correction for a frame carries it from where it is on the camera's path to where the smoothed
  // path says it should be, and the first frame is left where it was.
  struct Correction final {
    Complex a;
    Complex t;
  };
  std::vector<Correction> corrections(count);
  for (std::size_t i = 0; i < count; ++i) {
    const Complex measured_a = std::polar(std::exp(log_scale[i]), angle[i]);
    const Complex smooth_a = std::polar(std::exp(smooth(log_scale, i)), smooth(angle, i));
    const Complex measured_t{px[i], py[i]};
    const Complex smooth_t{smooth(px, i), smooth(py, i)};
    const auto ratio = smooth_a / measured_a;
    corrections[i] = {ratio, smooth_t - ratio * measured_t};
  }
  const auto anchor = corrections.front();
  for (auto& correction : corrections) {
    const auto relative = correction.a / anchor.a;
    correction = {relative, correction.t - relative * anchor.t};
  }

  const double half_w = result.source_width * 0.5, half_h = result.source_height * 0.5;
  double needed = 1.0;
  for (std::size_t i = 0; i < count; ++i) {
    SimilaritySample sample;
    sample.time = time::RationalTime::FromFrames(static_cast<std::int64_t>(i), rate);
    sample.translation_x = corrections[i].t.real();
    sample.translation_y = corrections[i].t.imag();
    sample.rotation_degrees = std::arg(corrections[i].a) * 180.0 / std::numbers::pi;
    sample.scale = std::abs(corrections[i].a);
    sample.inliers = inliers[i];
    sample.tracked = tracked[i];
    result.samples.push_back(sample);

    // The enlargement that keeps this frame's correction from showing a border.
    for (const auto& corner : {Complex{-half_w, -half_h}, Complex{half_w, -half_h}, Complex{-half_w, half_h}, Complex{half_w, half_h}}) {
      const auto source = (corner - corrections[i].t) / corrections[i].a;
      needed = std::max({needed, std::abs(source.real()) / half_w, std::abs(source.imag()) / half_h});
    }
    SimilaritySample path_sample;
    path_sample.time = sample.time;
    path_sample.translation_x = px[i];
    path_sample.translation_y = py[i];
    path_sample.rotation_degrees = angle[i] * 180.0 / std::numbers::pi;
    path_sample.scale = std::exp(log_scale[i]);
    result.camera_path.push_back(path_sample);
  }
  switch (config.crop) {
    case CropPolicy::AutoScale: result.auto_scale = std::clamp(needed * std::max(config.crop_safety, 1.0), 1.0, std::max(config.max_scale, 1.0)); break;
    case CropPolicy::Fixed: result.auto_scale = std::max(config.fixed_scale, 1.0); break;
    case CropPolicy::None: result.auto_scale = 1.0; break;
  }
  return result;
}

timeline::Effect MakeSimilarityStabilizerEffect(const SimilarityResult& result, std::string effect_id, std::int64_t order) {
  if (result.samples.empty()) throw std::invalid_argument("Cannot build a stabilizer effect from no samples");
  timeline::Effect effect;
  effect.id = std::move(effect_id);
  effect.effect_type = "stabilizer";
  effect.order = order;
  std::vector<anim::Keyframe> positions, rotations, scales;
  for (const auto& sample : result.samples) {
    positions.push_back({sample.time, anim::Value::Vec2(sample.translation_x, sample.translation_y), anim::Interpolation::Linear, {}, {}});
    rotations.push_back({sample.time, anim::Value::Scalar(sample.rotation_degrees), anim::Interpolation::Linear, {}, {}});
    const auto percent = sample.scale * result.auto_scale * 100.0;
    scales.push_back({sample.time, anim::Value::Vec2(percent, percent), anim::Interpolation::Linear, {}, {}});
  }
  for (auto& [name, keys] : std::vector<std::pair<std::string, std::vector<anim::Keyframe>>>{
           {"position", std::move(positions)}, {"rotation", std::move(rotations)}, {"scale", std::move(scales)}}) {
    timeline::Parameter parameter;
    parameter.id = effect.id + ":" + name;
    parameter.name = name;
    parameter.value = anim::AnimatedValue(std::move(keys));
    effect.parameters.push_back(std::move(parameter));
  }
  return effect;
}

}  // namespace cutline::render::tracking

#pragma once

// Motion tracking and similarity stabilisation: point and planar trackers, and a
// stabiliser that corrects rotation and zoom as well as shift.
//
// Everything here is analysis: it reads frames, writes numbers, and has no effect on what
// is rendered until its result is turned into an ordinary keyframed effect (see
// MakeStabilizerEffect in Stabilizer.h, MakeTrackedMotionEffect below). That keeps playback
// a pure per-frame operation, keeps the source untouched, and lets a result be stored, read,
// edited and re-run.
//
// The method throughout is Lucas-Kanade alignment on an image pyramid: find the shift,
// similarity or affine warp that makes a patch of one frame look like the same patch in
// another, by Gauss-Newton on the sum of squared differences, coarse to fine so that motions
// larger than the patch converge. The patch is compared with its mean removed, so a change
// of exposure does not pull the track. Confidence is the normalised cross-correlation of
// the patch at the answer, 1 for an exact match and falling toward 0 as the match fails.
//
// Everything is deterministic: the same frames and settings give the same numbers, on
// every machine, including the sampling in the outlier rejection (a fixed generator).

#include "media/VideoFrame.h"
#include "timeline/Sequence.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cutline::render::tracking {

// ------------------------------------------------------------------- images ----

struct Gray final {
  int width{0};
  int height{0};
  std::vector<float> pixels;

  [[nodiscard]] float At(int x, int y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
  // Bilinear, with coordinates measured so that pixel (x, y) is centred at (x, y); clamped at the edges.
  [[nodiscard]] float Sample(double x, double y) const;
};

// Rec.709 luma of the frame's straight RGB, optionally reduced to `target_width` by area averaging.
[[nodiscard]] Gray ToGray(const media::VideoFrame& frame, int target_width = 0);

struct Pyramid final {
  std::vector<Gray> levels;  // 0 is full size
};
[[nodiscard]] Pyramid BuildPyramid(const Gray& image, int levels);

// ------------------------------------------------------------------- warps ----

enum class Model { Translation, Similarity, Affine };

// x' = a11 x + a12 y + tx, y' = a21 x + a22 y + ty, in image pixels.
struct Warp final {
  double a11{1.0}, a12{0.0}, a21{0.0}, a22{1.0}, tx{0.0}, ty{0.0};

  [[nodiscard]] std::pair<double, double> Apply(double x, double y) const { return {a11 * x + a12 * y + tx, a21 * x + a22 * y + ty}; }
  [[nodiscard]] Warp Then(const Warp& next) const;  // this, then next
  [[nodiscard]] Warp Inverse() const;
  [[nodiscard]] double Rotation() const;  // radians, of the similarity part
  [[nodiscard]] double Scale() const;     // of the similarity part
  [[nodiscard]] static Warp Translation(double tx, double ty) { return {1, 0, 0, 1, tx, ty}; }
  [[nodiscard]] static Warp Similarity(double scale, double rotation_radians, double tx, double ty);
};

struct Region final {
  double x{0}, y{0}, width{0}, height{0};
};

struct AlignConfig final {
  int pyramid_levels{4};
  int max_iterations{40};
  double convergence{1e-3};  // pixels (and the equivalent for rotation and scale)
  int stride{1};             // use every nth pixel of the patch
  // Pyramid levels are used only while the patch stays at least this many pixels across.
  double minimum_patch{12.0};
};

struct AlignResult final {
  Warp warp;
  double confidence{0.0};  // normalised cross-correlation at the answer
  double rms{0.0};         // of the difference after removing each patch's mean
  int iterations{0};
  bool converged{false};
};

// Finds the warp that carries the patch (a region of `reference`) onto `target`, starting from
// `initial`. The warp maps reference coordinates to target coordinates.
[[nodiscard]] AlignResult Align(const Pyramid& reference, const Pyramid& target, const Region& region, Model model,
                                const Warp& initial, const AlignConfig& config = {});

// ------------------------------------------------------------ point tracking ----

using FrameResolver = std::function<const media::VideoFrame*(std::size_t frame_index)>;
using CancelCheck = std::function<bool()>;
using ProgressCallback = std::function<void(std::size_t done, std::size_t total)>;

struct PointTrackerConfig final {
  int patch_radius{12};
  AlignConfig align{};
  double min_confidence{0.7};
  // Compare with the first frame's patch (no drift, but it cannot follow a change of appearance), or
  // with the previous frame's (follows appearance, but drifts).
  bool anchored{true};
  int analysis_width{0};  // 0: full size
};

struct PointSample final {
  time::RationalTime time;
  double x{0}, y{0};  // in frame pixels
  double confidence{0.0};
  bool valid{false};
};

struct TrackResult final {
  std::vector<PointSample> samples;
  bool cancelled{false};
  // The first frame at which the track was lost and not found again, or -1.
  std::int64_t lost_at{-1};
  std::string algorithm;
  int source_width{0}, source_height{0};
};

// Follows the point (x, y) of frame `first` through frames [first, frame_count).
[[nodiscard]] TrackResult TrackPoint(std::size_t frame_count, time::FrameRate rate, const FrameResolver& resolve, std::size_t first,
                                     double x, double y, const PointTrackerConfig& config = {}, const CancelCheck& cancel = {},
                                     const ProgressCallback& progress = {});

// ------------------------------------------------------------ planar tracking ----

struct PlanarTrackerConfig final {
  Model model{Model::Similarity};
  AlignConfig align{};
  double min_confidence{0.6};
  int analysis_width{0};
};

struct PlanarSample final {
  time::RationalTime time;
  Warp warp;  // reference frame coordinates to this frame's
  double confidence{0.0};
  bool valid{false};
};

struct PlanarTrackResult final {
  std::vector<PlanarSample> samples;
  bool cancelled{false};
  std::int64_t lost_at{-1};
  std::string algorithm;
  Region region;  // in full-size frame pixels
  int source_width{0}, source_height{0};
};

[[nodiscard]] PlanarTrackResult TrackPlane(std::size_t frame_count, time::FrameRate rate, const FrameResolver& resolve, std::size_t first,
                                           const Region& region, const PlanarTrackerConfig& config = {}, const CancelCheck& cancel = {},
                                           const ProgressCallback& progress = {});

// Turns a track into a keyframed Motion effect that moves a layer with the tracked point (or plane):
// position follows the point's displacement from where it started, and for a plane rotation and
// scale follow too. The effect is ordinary data; removing it undoes the move.
[[nodiscard]] timeline::Effect MakeTrackedMotionEffect(const TrackResult& track, std::string effect_id, std::int64_t order = 0);
[[nodiscard]] timeline::Effect MakeTrackedMotionEffect(const PlanarTrackResult& track, std::string effect_id, std::int64_t order = 0);

// True when a track made against `fingerprint` no longer describes the media now at that place.
[[nodiscard]] bool IsStale(const std::string& analysed_fingerprint, const std::string& current_fingerprint);

// ------------------------------------------------- similarity stabilisation ----

enum class CropPolicy {
  AutoScale,  // enlarge just enough to hide every border that the correction would show
  None,       // no enlargement; borders may show
  Fixed,      // the scale in `fixed_scale`
};

struct SimilarityStabilizerConfig final {
  int analysis_width{320};
  int feature_count{150};
  int feature_spacing{10};
  int smoothing_radius_frames{12};
  int ransac_iterations{80};
  double inlier_threshold{1.2};  // pixels at the analysis size
  double max_scale{1.35};
  CropPolicy crop{CropPolicy::AutoScale};
  double fixed_scale{1.0};
  double crop_safety{1.01};
  bool correct_rotation{true};
  bool correct_scale{true};
};

struct SimilaritySample final {
  time::RationalTime time;
  double translation_x{0};
  double translation_y{0};
  double rotation_degrees{0};
  double scale{1.0};
  // How many of the tracked features agreed with the motion found between this frame and the last.
  int inliers{0};
  int tracked{0};
};

struct SimilarityResult final {
  // The correction to apply to each frame (translation about the frame centre, rotation, scale).
  std::vector<SimilaritySample> samples;
  // The camera's own path as measured, frame 0 to each frame, for review: the same fields, uncorrected.
  std::vector<SimilaritySample> camera_path;
  double auto_scale{1.0};
  bool cancelled{false};
  // Frames where too few features could be followed and no motion was estimated (a shift of zero is assumed).
  std::vector<std::size_t> uncertain_frames;
  std::string algorithm{"similarity_lk_ransac_v1"};
  int source_width{0}, source_height{0};
};

[[nodiscard]] SimilarityResult AnalyzeSimilarityStabilization(std::size_t frame_count, time::FrameRate rate,
                                                              const FrameResolver& resolve,
                                                              const SimilarityStabilizerConfig& config = {},
                                                              const CancelCheck& cancel = {},
                                                              const ProgressCallback& progress = {});

// A "stabilizer" effect: position, rotation and scale keyframes, one per analysed frame.
[[nodiscard]] timeline::Effect MakeSimilarityStabilizerEffect(const SimilarityResult& result, std::string effect_id,
                                                              std::int64_t order = 0);

}  // namespace cutline::render::tracking

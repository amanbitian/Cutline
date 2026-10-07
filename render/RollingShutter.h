#pragma once

// Rolling-shutter analysis: how much a picture is skewed by the camera moving while the sensor was
// being read, measured from the footage itself.
//
// The model. A rolling shutter reads the sensor row by row, so row y of frame k is captured
// `readout * y / H` of a frame later than row 0 (`readout` is the fraction of the frame interval the
// readout takes; 0 for a global shutter). If the picture is moving across the sensor at (vx, vy) pixels
// a frame, each row has moved that much further than the one above, a skew of (vx, vy) * readout over
// the height of the frame. Undoing it is the scan-line correction ApplyRollingShutter already does,
// with horizontal = vx * readout and vertical = vy * readout. What analysis has to supply is the velocity
// at each frame, and that is measured here by aligning the frames either side of it.
//
// The readout fraction is a property of the camera and its mode and cannot be seen in one frame, so it
// comes from a profile the user chooses or supplies. The built-in profiles are classes of camera, labelled
// as such; they are not measurements of any model, and a project that needs better should load measured
// ones from a profile file.

#include "render/Tracking.h"

#include <string>
#include <vector>

namespace cutline::render::tracking {

struct CameraProfile final {
  std::string id;
  std::string name;
  // Fraction of the frame interval taken to read the sensor, 0 to 1.
  double readout{0.0};
  bool bottom_to_top{false};
  std::string notes;
};

// Nominal classes: global shutter, and fast, typical and slow CMOS readouts.
[[nodiscard]] const std::vector<CameraProfile>& BuiltInCameraProfiles();
[[nodiscard]] std::string ProfilesToJson(const std::vector<CameraProfile>& profiles);
// Reads a profile file; throws on anything malformed (a readout outside 0..1, an empty id, a duplicate id).
[[nodiscard]] std::vector<CameraProfile> ProfilesFromJson(const std::string& json);
[[nodiscard]] const CameraProfile* FindProfile(const std::vector<CameraProfile>& profiles, const std::string& id);

struct RollingShutterConfig final {
  int analysis_width{480};
  // The picture's middle is measured: edges are where it is least reliable.
  double region_fraction{0.7};
  AlignConfig align{};
  double min_confidence{0.5};
};

struct RollingShutterSample final {
  time::RationalTime time;
  // Picture velocity in frame pixels per frame.
  double velocity_x{0.0};
  double velocity_y{0.0};
  // The correction: scan-line skew over the height of the frame, in frame pixels.
  double horizontal{0.0};
  double vertical{0.0};
  double confidence{0.0};
  bool valid{false};
};

struct RollingShutterResult final {
  std::vector<RollingShutterSample> samples;
  CameraProfile profile;
  bool cancelled{false};
  int source_width{0}, source_height{0};
  std::string algorithm{"rolling_shutter_lk_v1"};
};

[[nodiscard]] RollingShutterResult AnalyzeRollingShutter(std::size_t frame_count, time::FrameRate rate, const FrameResolver& resolve,
                                                         const CameraProfile& profile, const RollingShutterConfig& config = {},
                                                         const CancelCheck& cancel = {}, const ProgressCallback& progress = {});

// A "rolling_shutter" effect with the corrections as keyframes. A frame whose motion could not be
// measured holds the nearest measured correction instead of jumping to zero.
[[nodiscard]] timeline::Effect MakeRollingShutterEffect(const RollingShutterResult& result, std::string effect_id, std::int64_t order = 0);

}  // namespace cutline::render::tracking

#pragma once

// The multicam angle monitor: every angle's picture at one instant, tiled into one frame, with the
// angle that is live marked. It is a display aid, not part of the programme: nothing here enters the
// timeline compiler or an export. The layout is also what a click on the monitor is resolved against
// to cut to the angle under the pointer.

#include "media/VideoFrame.h"

#include <array>
#include <vector>

namespace cutline::render {

struct MonitorTile final {
  // The angle's picture at the instant, or null where the angle has none (before it began, after it ended).
  const media::VideoFrame* frame{nullptr};
  // The angle that is live on the programme.
  bool active{false};
};

struct AngleMonitorConfig final {
  int width{1280};
  int height{720};
  // 0 chooses the arrangement closest to square for the number of angles.
  int columns{0};
  int gap{6};
  int border{4};
  std::array<float, 3> background{0.04f, 0.04f, 0.05f};
  std::array<float, 3> empty{0.12f, 0.12f, 0.14f};
  std::array<float, 3> active_border{0.9f, 0.15f, 0.15f};
};

struct TileRect final {
  int x{0}, y{0}, width{0}, height{0};
  [[nodiscard]] bool Contains(int px, int py) const noexcept { return px >= x && py >= y && px < x + width && py < y + height; }
};

// The cell each of `count` angles occupies, row by row in angle order.
[[nodiscard]] std::vector<TileRect> AngleMonitorLayout(std::size_t count, const AngleMonitorConfig& config = {});

// The angle whose cell holds the point, or -1.
[[nodiscard]] int AngleAtPoint(std::size_t count, int x, int y, const AngleMonitorConfig& config = {});

// Each picture is fitted into its cell without changing its shape (area-averaged when reduced) and a
// live angle gets a border. Output is RgbaF32.
[[nodiscard]] media::VideoFrame ComposeAngleMonitor(const std::vector<MonitorTile>& tiles, const AngleMonitorConfig& config = {});

}  // namespace cutline::render

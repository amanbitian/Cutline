#pragma once

// The program monitor's logic: how large to render, where the picture sits in the window, what is drawn over
// it, and how requests for pictures are served without ever making the interface wait.
//
// Presentation is asynchronous. Scrubbing and edits are latest-wins, while continuous playback may show a completed
// frame even when the next frame is already waiting. Pending work is always coalesced to one request.

#include "core/time/RationalTime.h"
#include "media/VideoFrame.h"
#include "render/D3D11Compositor.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cutline::ui {

struct SizePx final {
  int width{0};
  int height{0};
  [[nodiscard]] bool operator==(const SizePx& other) const { return width == other.width && height == other.height; }
};

enum class MonitorQuality { Full, Half, Quarter, Eighth, Auto };

[[nodiscard]] std::string ToString(MonitorQuality quality);
[[nodiscard]] std::optional<MonitorQuality> ParseMonitorQuality(const std::string& text);
// The divisor a fixed quality means (1, 2, 4, 8); 0 for Auto.
[[nodiscard]] int Divisor(MonitorQuality quality);

// The size to render a sequence of `sequence` size at. A fixed quality divides each side. Auto picks the largest
// reduction whose picture still has at least as many pixels as the monitor shows (`displayed` already in device
// pixels), so a small monitor does not pay for a large render and a large one is never upscaled from a smaller one.
// The result is never smaller than 16 pixels on a side nor larger than the sequence.
[[nodiscard]] SizePx RenderSizeFor(SizePx sequence, MonitorQuality quality, SizePx displayed);

struct Rect2 final {
  double x{0}, y{0}, width{0}, height{0};
};

enum class ZoomMode { Fit, Fill, Actual, Custom };

struct MonitorView final {
  ZoomMode zoom{ZoomMode::Fit};
  double custom_zoom{1.0};  // Custom: display pixels per picture pixel
  // How far the picture is moved from centred, in display pixels (when it is larger than the window).
  double pan_x{0};
  double pan_y{0};
};

// The picture's width on screen for a frame of `frame` pixels with the given pixel aspect (1 for square pixels).
[[nodiscard]] double DisplayWidth(SizePx frame, double pixel_aspect);

// Where the picture lands in a window: centred, scaled per the zoom mode, moved by the pan, and the pan limited so
// the picture cannot be dragged entirely out of the window.
[[nodiscard]] Rect2 FrameRect(SizePx frame, double pixel_aspect, SizePx window, const MonitorView& view);
// The picture pixel under a window point, or nothing outside the picture.
[[nodiscard]] std::optional<std::pair<double, double>> PictureAt(SizePx frame, double pixel_aspect, SizePx window, const MonitorView& view, double x, double y);

struct OverlayOptions final {
  bool safe_margins{false};
  double action_percent{93.0};
  double title_percent{90.0};
  bool center_cross{false};
  bool thirds{false};
  // Aspect ratios (width / height) of crop frames to outline, such as 16/9 or 1.0 or 9/16.
  std::vector<double> guides;
};

struct OverlayShape final {
  enum class Kind { Rectangle, Line } kind{Kind::Rectangle};
  Rect2 rect;                       // Rectangle
  double x0{0}, y0{0}, x1{0}, y1{0};  // Line
  std::string tag;                  // "action_safe", "title_safe", "center", "third", "guide"
};

// The overlay shapes, in window coordinates, for a picture drawn at `frame_rect`.
[[nodiscard]] std::vector<OverlayShape> BuildOverlays(const Rect2& frame_rect, const OverlayOptions& options);

// ------------------------------------------------------------------- presenter ----

struct PresenterStatistics final {
  std::uint64_t requested{0};
  // Requests replaced by a newer one before they were rendered.
  std::uint64_t coalesced{0};
  std::uint64_t rendered{0};
  std::uint64_t delivered{0};
  // Pictures rendered for a request that had been overtaken by the time it finished, and so not delivered.
  std::uint64_t stale{0};
  std::uint64_t failed{0};
  double last_render_ms{0.0};
  double max_render_ms{0.0};
  std::string last_error;
};

enum class PresentationMode {
  // A newer request invalidates a render already in progress. Used for seeks, edits, resizes, and scrubbing.
  LatestOnly,
  // A completed frame may be displayed while the following frame waits. Used for continuous playback.
  Playback,
};

struct PresentedFrame final {
  media::VideoFrame pixels;
  render::gpu::PresentationFrame texture;
  PresentedFrame() = default;
  PresentedFrame(media::VideoFrame frame) : pixels(std::move(frame)) {}
  PresentedFrame(render::gpu::PresentationFrame frame) : texture(std::move(frame)) {}
  [[nodiscard]] bool valid() const noexcept { return texture.valid() || pixels.valid(); }
  [[nodiscard]] bool on_gpu() const noexcept { return texture.valid(); }
  [[nodiscard]] int width() const noexcept { return on_gpu() ? texture.width : pixels.width(); }
  [[nodiscard]] int height() const noexcept { return on_gpu() ? texture.height : pixels.height(); }
};

class FramePresenter final {
 public:
  // Renders the picture for `at` at `size`. Called from the presenter's thread only.
  using Render = std::function<PresentedFrame(const time::RationalTime& at, SizePx size)>;
  // Receives a finished picture. Called from the presenter's thread; a UI hands it to its own thread.
  using Deliver = std::function<void(PresentedFrame frame, const time::RationalTime& at, std::uint64_t serial)>;

  FramePresenter(Render render, Deliver deliver);
  ~FramePresenter();
  FramePresenter(const FramePresenter&) = delete;
  FramePresenter& operator=(const FramePresenter&) = delete;

  // Asks for the picture at a time and size. Never blocks; replaces a request that has not started. Returns the
  // serial the picture will carry.
  std::uint64_t Request(const time::RationalTime& at, SizePx size,
                        PresentationMode mode = PresentationMode::LatestOnly);
  // The sequence changed under the last request: render it again.
  std::uint64_t Invalidate();
  // Returns when nothing is being rendered or waiting, or after `timeout_ms`; true when idle.
  bool WaitIdle(int timeout_ms = 10000);
  [[nodiscard]] PresenterStatistics statistics() const;

 private:
  void Run();

  Render render_;
  Deliver deliver_;
  struct RequestJob final {
    time::RationalTime at;
    SizePx size;
    std::uint64_t serial{0};
    std::uint64_t generation{0};
    PresentationMode mode{PresentationMode::LatestOnly};
  };
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::optional<RequestJob> pending_;
  std::optional<std::pair<time::RationalTime, SizePx>> last_;
  std::uint64_t serial_{0};
  // Seeks and edits advance this so that playback frames from the old position are not delivered.
  std::uint64_t generation_{0};
  std::uint64_t rendering_serial_{0};
  bool busy_{false};
  bool stop_{false};
  PresenterStatistics stats_;
  std::thread worker_;
};

}  // namespace cutline::ui

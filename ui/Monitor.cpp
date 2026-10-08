#include "ui/Monitor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace cutline::ui {

std::string ToString(MonitorQuality quality) {
  switch (quality) {
    case MonitorQuality::Full: return "full";
    case MonitorQuality::Half: return "half";
    case MonitorQuality::Quarter: return "quarter";
    case MonitorQuality::Eighth: return "eighth";
    case MonitorQuality::Auto: return "auto";
  }
  return "auto";
}

std::optional<MonitorQuality> ParseMonitorQuality(const std::string& text) {
  for (const auto quality : {MonitorQuality::Full, MonitorQuality::Half, MonitorQuality::Quarter, MonitorQuality::Eighth, MonitorQuality::Auto}) {
    if (ToString(quality) == text) return quality;
  }
  return std::nullopt;
}

int Divisor(MonitorQuality quality) {
  switch (quality) {
    case MonitorQuality::Full: return 1;
    case MonitorQuality::Half: return 2;
    case MonitorQuality::Quarter: return 4;
    case MonitorQuality::Eighth: return 8;
    case MonitorQuality::Auto: return 0;
  }
  return 0;
}

namespace {

SizePx Reduced(SizePx sequence, int divisor) {
  // Even sizes, because most encoders and chroma subsampling want them; and never below 16.
  const auto reduce = [&](int value) {
    auto reduced = (value + divisor - 1) / divisor;
    reduced = std::max(reduced, std::min(16, value));
    return reduced + (reduced % 2 != 0 && reduced < value ? 1 : 0);
  };
  return {reduce(sequence.width), reduce(sequence.height)};
}

}  // namespace

SizePx RenderSizeFor(SizePx sequence, MonitorQuality quality, SizePx displayed) {
  if (sequence.width <= 0 || sequence.height <= 0) return {16, 16};
  if (quality != MonitorQuality::Auto) return Reduced(sequence, Divisor(quality));
  if (displayed.width <= 0 || displayed.height <= 0) return sequence;
  for (const int divisor : {8, 4, 2}) {
    const auto candidate = Reduced(sequence, divisor);
    if (candidate.width >= displayed.width && candidate.height >= displayed.height) return candidate;
  }
  return sequence;
}

double DisplayWidth(SizePx frame, double pixel_aspect) { return static_cast<double>(frame.width) * (pixel_aspect > 0.0 ? pixel_aspect : 1.0); }

Rect2 FrameRect(SizePx frame, double pixel_aspect, SizePx window, const MonitorView& view) {
  if (frame.width <= 0 || frame.height <= 0 || window.width <= 0 || window.height <= 0) return {};
  const double dw = DisplayWidth(frame, pixel_aspect);
  const double dh = frame.height;
  const double W = window.width, H = window.height;
  double scale = 1.0;
  switch (view.zoom) {
    case ZoomMode::Fit: scale = std::min(W / dw, H / dh); break;
    case ZoomMode::Fill: scale = std::max(W / dw, H / dh); break;
    case ZoomMode::Actual: scale = 1.0; break;
    case ZoomMode::Custom: scale = std::clamp(view.custom_zoom, 0.05, 64.0); break;
  }
  const double pw = dw * scale, ph = dh * scale;
  double cx = W / 2.0, cy = H / 2.0;
  if (view.zoom != ZoomMode::Fit) {
    // The picture may be dragged about, but never so far that none of it is left in the window.
    const double mx = std::min(64.0, pw / 2.0), my = std::min(64.0, ph / 2.0);
    cx = std::clamp(cx + view.pan_x, mx - pw / 2.0, W - mx + pw / 2.0);
    cy = std::clamp(cy + view.pan_y, my - ph / 2.0, H - my + ph / 2.0);
  }
  return {cx - pw / 2.0, cy - ph / 2.0, pw, ph};
}

std::optional<std::pair<double, double>> PictureAt(SizePx frame, double pixel_aspect, SizePx window, const MonitorView& view, double x, double y) {
  const auto rect = FrameRect(frame, pixel_aspect, window, view);
  if (rect.width <= 0 || rect.height <= 0) return std::nullopt;
  if (x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height) return std::nullopt;
  return std::pair{(x - rect.x) / rect.width * frame.width, (y - rect.y) / rect.height * frame.height};
}

std::vector<OverlayShape> BuildOverlays(const Rect2& frame, const OverlayOptions& options) {
  std::vector<OverlayShape> shapes;
  if (frame.width <= 0 || frame.height <= 0) return shapes;
  const auto inset = [&](double percent, const char* tag) {
    const double share = std::clamp(percent, 1.0, 100.0) / 100.0;
    OverlayShape shape;
    shape.rect = {frame.x + frame.width * (1.0 - share) / 2.0, frame.y + frame.height * (1.0 - share) / 2.0, frame.width * share, frame.height * share};
    shape.tag = tag;
    shapes.push_back(shape);
  };
  if (options.safe_margins) {
    inset(options.action_percent, "action_safe");
    inset(options.title_percent, "title_safe");
  }
  const auto line = [&](double x0, double y0, double x1, double y1, const char* tag) {
    OverlayShape shape;
    shape.kind = OverlayShape::Kind::Line;
    shape.x0 = x0; shape.y0 = y0; shape.x1 = x1; shape.y1 = y1;
    shape.tag = tag;
    shapes.push_back(shape);
  };
  if (options.center_cross) {
    const double cx = frame.x + frame.width / 2.0, cy = frame.y + frame.height / 2.0;
    const double arm = std::min(frame.width, frame.height) * 0.05;
    line(cx - arm, cy, cx + arm, cy, "center");
    line(cx, cy - arm, cx, cy + arm, "center");
  }
  if (options.thirds) {
    for (int i = 1; i <= 2; ++i) {
      const double x = frame.x + frame.width * i / 3.0, y = frame.y + frame.height * i / 3.0;
      line(x, frame.y, x, frame.y + frame.height, "third");
      line(frame.x, y, frame.x + frame.width, y, "third");
    }
  }
  for (const auto aspect : options.guides) {
    if (!(aspect > 0.0) || !std::isfinite(aspect)) continue;
    OverlayShape shape;
    // The largest rectangle of that shape that fits inside the picture, centred.
    double w = frame.width, h = frame.width / aspect;
    if (h > frame.height) {
      h = frame.height;
      w = h * aspect;
    }
    shape.rect = {frame.x + (frame.width - w) / 2.0, frame.y + (frame.height - h) / 2.0, w, h};
    shape.tag = "guide";
    shapes.push_back(shape);
  }
  return shapes;
}

// ------------------------------------------------------------------- presenter ----

FramePresenter::FramePresenter(Render render, Deliver deliver) : render_(std::move(render)), deliver_(std::move(deliver)) {
  worker_ = std::thread([this] { Run(); });
}

FramePresenter::~FramePresenter() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    pending_.reset();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::uint64_t FramePresenter::Request(const time::RationalTime& at, SizePx size, PresentationMode mode) {
  std::uint64_t serial;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.requested;
    if (pending_.has_value()) ++stats_.coalesced;
    serial = ++serial_;
    if (mode == PresentationMode::LatestOnly) ++generation_;
    pending_ = RequestJob{at, size, serial, generation_, mode};
    last_ = std::pair{at, size};
  }
  wake_.notify_one();
  return serial;
}

std::uint64_t FramePresenter::Invalidate() {
  std::uint64_t serial = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!last_.has_value()) return 0;
    ++stats_.requested;
    if (pending_.has_value()) ++stats_.coalesced;
    serial = ++serial_;
    ++generation_;
    pending_ = RequestJob{last_->first, last_->second, serial, generation_, PresentationMode::LatestOnly};
  }
  wake_.notify_one();
  return serial;
}

bool FramePresenter::WaitIdle(int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return idle_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !pending_.has_value() && !busy_; });
}

PresenterStatistics FramePresenter::statistics() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void FramePresenter::Run() {
  for (;;) {
    RequestJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      job = *pending_;
      pending_.reset();
      rendering_serial_ = job.serial;
      busy_ = true;
    }
    std::optional<media::VideoFrame> frame;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    try {
      frame = render_(job.at, job.size);
    } catch (const std::exception& e) {
      error = e.what();
    } catch (...) {
      error = "the renderer failed";
    }
    const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bool deliver = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stats_.last_render_ms = milliseconds;
      stats_.max_render_ms = std::max(stats_.max_render_ms, milliseconds);
      if (!frame.has_value()) {
        ++stats_.failed;
        stats_.last_error = error;
      } else {
        ++stats_.rendered;
        // Playback is allowed to present a completed frame while its successor waits. A seek or edit advances the
        // generation and still invalidates every frame rendered for the old state.
        if (generation_ == job.generation &&
            (job.mode == PresentationMode::Playback || serial_ == job.serial)) {
          deliver = true;
        }
        else ++stats_.stale;
      }
    }
    if (deliver) {
      deliver_(std::move(*frame), job.at, job.serial);
      const std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.delivered;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
    }
    idle_.notify_all();
  }
}

}  // namespace cutline::ui

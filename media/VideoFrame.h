#pragma once

// Decoded picture buffers.
//
// Cutline keeps a deliberately small set of pixel formats. Decoders convert to
// RGBA on the way out rather than handing planar YUV to the compositor, because
// getting the YUV matrix and range right for bt601/709/2020 and limited/full is
// exactly the kind of thing a hand-written converter gets subtly wrong; the
// FFmpeg provider delegates that to swscale, which already knows. The cost is
// one conversion per decoded frame, which a GPU path will later absorb by
// uploading planar data and converting in a shader.
//
// The compositor's working format is RgbaF32: compositing, opacity, and grading
// are linear-light operations, and 8-bit intermediates band visibly after two or
// three stacked effects.

#include "core/model/Types.h"
#include "core/time/RationalTime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cutline::media {

enum class PixelFormat {
  Rgba8,    // 8 bits per component, interleaved. Display and export.
  Rgba16,   // 16-bit unsigned. Decode target for >8-bit sources.
  RgbaF32,  // 32-bit float. Compositor working format.
};

[[nodiscard]] std::size_t BytesPerPixel(PixelFormat format);
[[nodiscard]] std::string ToString(PixelFormat format);

// How to interpret the samples in a frame. Carried alongside the pixels so a
// colour-managed pipeline has what it needs, and so a decoder can tell the
// compositor what it produced.
struct ColorSpace final {
  std::string primaries{"bt709"};
  std::string transfer{"bt709"};
  std::string matrix{"bt709"};
  model::ColorRange range{model::ColorRange::Limited};

  [[nodiscard]] bool operator==(const ColorSpace& other) const;
};

// An owning picture buffer. Moveable, not copyable: frames are large and an
// accidental copy in a per-frame path is a performance bug, so copying has to
// be explicit via Clone().
class VideoFrame final {
 public:
  VideoFrame() = default;
  VideoFrame(const VideoFrame&) = delete;
  VideoFrame& operator=(const VideoFrame&) = delete;
  VideoFrame(VideoFrame&&) noexcept = default;
  VideoFrame& operator=(VideoFrame&&) noexcept = default;

  [[nodiscard]] static VideoFrame Allocate(PixelFormat format, int width, int height);
  [[nodiscard]] VideoFrame Clone() const;

  [[nodiscard]] bool valid() const noexcept { return width_ > 0 && height_ > 0 && !pixels_.empty(); }
  [[nodiscard]] PixelFormat format() const noexcept { return format_; }
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  // Bytes per row, including any padding.
  [[nodiscard]] std::ptrdiff_t stride() const noexcept { return stride_; }

  [[nodiscard]] std::byte* data() noexcept { return pixels_.data(); }
  [[nodiscard]] const std::byte* data() const noexcept { return pixels_.data(); }
  [[nodiscard]] std::size_t size_bytes() const noexcept { return pixels_.size(); }

  [[nodiscard]] std::byte* row(int y);
  [[nodiscard]] const std::byte* row(int y) const;
  // Typed row access for the formats the compositor touches directly.
  [[nodiscard]] float* row_f32(int y);
  [[nodiscard]] const float* row_f32(int y) const;
  [[nodiscard]] std::uint8_t* row_u8(int y);
  [[nodiscard]] const std::uint8_t* row_u8(int y) const;

  // Presentation timestamp in the source's own timebase.
  time::RationalTime presentation_time;
  time::RationalTime duration;
  ColorSpace color;
  // Sample aspect, for anamorphic sources.
  time::FrameRate pixel_aspect{1, 1};
  bool keyframe{false};

 private:
  PixelFormat format_{PixelFormat::Rgba8};
  int width_{0};
  int height_{0};
  std::ptrdiff_t stride_{0};
  std::vector<std::byte> pixels_;
};

// Format conversion. Only the pairs the pipeline actually needs exist; an
// unsupported pair throws rather than silently producing wrong pixels.
[[nodiscard]] VideoFrame ConvertFrame(const VideoFrame& source, PixelFormat target);

}  // namespace cutline::media

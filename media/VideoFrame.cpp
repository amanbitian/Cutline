#include "media/VideoFrame.h"
#include "core/util/ParallelRows.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace cutline::media {
namespace {

// Rows are padded to 64 bytes so each row starts on a cache line and stays
// friendly to the wide loads a SIMD or GPU upload path will want.
constexpr std::size_t kRowAlignment = 64;

[[nodiscard]] std::size_t AlignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

[[nodiscard]] float ToUnitFloat(std::uint8_t value) { return static_cast<float>(value) / 255.0f; }
[[nodiscard]] float ToUnitFloat(std::uint16_t value) { return static_cast<float>(value) / 65535.0f; }

[[nodiscard]] std::uint8_t ToByte(float value) {
  // Round-half-away-from-zero after clamping, so 1.0 maps to 255 and not 254.
  const auto scaled = std::clamp(value, 0.0f, 1.0f) * 255.0f;
  return static_cast<std::uint8_t>(scaled + 0.5f);
}

[[nodiscard]] std::uint16_t ToWord(float value) {
  const auto scaled = std::clamp(value, 0.0f, 1.0f) * 65535.0f;
  return static_cast<std::uint16_t>(scaled + 0.5f);
}

}  // namespace

std::size_t BytesPerPixel(PixelFormat format) {
  switch (format) {
    case PixelFormat::Rgba8: return 4;
    case PixelFormat::Rgba16: return 8;
    case PixelFormat::RgbaF32: return 16;
  }
  throw std::invalid_argument("Unhandled pixel format");
}

std::string ToString(PixelFormat format) {
  switch (format) {
    case PixelFormat::Rgba8: return "rgba8";
    case PixelFormat::Rgba16: return "rgba16";
    case PixelFormat::RgbaF32: return "rgba_f32";
  }
  throw std::invalid_argument("Unhandled pixel format");
}

bool ColorSpace::operator==(const ColorSpace& other) const {
  return primaries == other.primaries && transfer == other.transfer && matrix == other.matrix &&
         range == other.range;
}

VideoFrame VideoFrame::Allocate(PixelFormat format, int width, int height) {
  if (width <= 0 || height <= 0) throw std::invalid_argument("Frame dimensions must be positive");
  VideoFrame frame;
  frame.format_ = format;
  frame.width_ = width;
  frame.height_ = height;
  frame.stride_ =
      static_cast<std::ptrdiff_t>(AlignUp(static_cast<std::size_t>(width) * BytesPerPixel(format), kRowAlignment));
  frame.pixels_.assign(static_cast<std::size_t>(frame.stride_) * static_cast<std::size_t>(height), std::byte{});
  return frame;
}

VideoFrame VideoFrame::Clone() const {
  VideoFrame copy;
  copy.format_ = format_;
  copy.width_ = width_;
  copy.height_ = height_;
  copy.stride_ = stride_;
  copy.pixels_ = pixels_;
  copy.presentation_time = presentation_time;
  copy.duration = duration;
  copy.color = color;
  copy.pixel_aspect = pixel_aspect;
  copy.keyframe = keyframe;
  return copy;
}

std::byte* VideoFrame::row(int y) {
  if (y < 0 || y >= height_) throw std::out_of_range("Frame row index is out of range");
  return pixels_.data() + static_cast<std::size_t>(stride_) * static_cast<std::size_t>(y);
}

const std::byte* VideoFrame::row(int y) const {
  if (y < 0 || y >= height_) throw std::out_of_range("Frame row index is out of range");
  return pixels_.data() + static_cast<std::size_t>(stride_) * static_cast<std::size_t>(y);
}

float* VideoFrame::row_f32(int y) {
  if (format_ != PixelFormat::RgbaF32) throw std::logic_error("Frame is not RgbaF32");
  return reinterpret_cast<float*>(row(y));
}

const float* VideoFrame::row_f32(int y) const {
  if (format_ != PixelFormat::RgbaF32) throw std::logic_error("Frame is not RgbaF32");
  return reinterpret_cast<const float*>(row(y));
}

std::uint8_t* VideoFrame::row_u8(int y) {
  if (format_ != PixelFormat::Rgba8) throw std::logic_error("Frame is not Rgba8");
  return reinterpret_cast<std::uint8_t*>(row(y));
}

const std::uint8_t* VideoFrame::row_u8(int y) const {
  if (format_ != PixelFormat::Rgba8) throw std::logic_error("Frame is not Rgba8");
  return reinterpret_cast<const std::uint8_t*>(row(y));
}

VideoFrame ConvertFrame(const VideoFrame& source, PixelFormat target) {
  if (!source.valid()) throw std::invalid_argument("Cannot convert an empty frame");
  if (source.format() == target) return source.Clone();

  auto result = VideoFrame::Allocate(target, source.width(), source.height());
  result.presentation_time = source.presentation_time;
  result.duration = source.duration;
  result.color = source.color;
  result.pixel_aspect = source.pixel_aspect;
  result.keyframe = source.keyframe;

  const auto width = source.width();
  const auto components = static_cast<std::size_t>(width) * 4;

  // Values are normalised to 0..1 in the frame's own transfer function. No
  // linearisation happens here: that belongs to the colour-managed stage, and
  // doing it implicitly would silently change the look of every composite.
  util::ParallelRows(0, source.height() - 1, [&](int y) {
    switch (source.format()) {
      case PixelFormat::Rgba8: {
        const auto* in = source.row_u8(y);
        if (target == PixelFormat::RgbaF32) {
          auto* out = result.row_f32(y);
          for (std::size_t index = 0; index < components; ++index) out[index] = ToUnitFloat(in[index]);
        } else {
          auto* out = reinterpret_cast<std::uint16_t*>(result.row(y));
          for (std::size_t index = 0; index < components; ++index) {
            out[index] = static_cast<std::uint16_t>(in[index] * 257);  // 255 -> 65535 exactly
          }
        }
        break;
      }
      case PixelFormat::Rgba16: {
        const auto* in = reinterpret_cast<const std::uint16_t*>(source.row(y));
        if (target == PixelFormat::RgbaF32) {
          auto* out = result.row_f32(y);
          for (std::size_t index = 0; index < components; ++index) out[index] = ToUnitFloat(in[index]);
        } else {
          auto* out = result.row_u8(y);
          for (std::size_t index = 0; index < components; ++index) {
            out[index] = static_cast<std::uint8_t>((in[index] + 128) / 257);
          }
        }
        break;
      }
      case PixelFormat::RgbaF32: {
        const auto* in = source.row_f32(y);
        if (target == PixelFormat::Rgba8) {
          auto* out = result.row_u8(y);
          for (std::size_t index = 0; index < components; ++index) out[index] = ToByte(in[index]);
        } else {
          auto* out = reinterpret_cast<std::uint16_t*>(result.row(y));
          for (std::size_t index = 0; index < components; ++index) out[index] = ToWord(in[index]);
        }
        break;
      }
    }
  });
  return result;
}

}  // namespace cutline::media

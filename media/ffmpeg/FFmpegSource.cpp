// FFmpeg demux and software decode.
//
// This is one implementation of media::Source. Nothing above it knows FFmpeg
// exists, which is what lets hardware decoders be added later as peers rather
// than as special cases, and what lets the whole pipeline be tested against the
// synthetic source with no media files present.
//
// Each stream is read through its own demuxer and decoder (see OpenStream), and
// audio reads are sample-accurate (see AudioReader); the reasons are stated
// where the code is.
//
// Two decisions worth stating:
//
//   * Colour conversion is delegated to swscale with explicit colourspace
//     details, rather than hand-written YUV maths. Getting bt601/709/2020 and
//     limited/full range right is fiddly and silently wrong when it isn't;
//     swscale already knows, and the GPU path will later do the same job in a
//     shader fed by the same metadata.
//   * Output depth follows the source: an 8-bit source decodes to Rgba8, and
//     anything deeper to Rgba16, so a 10-bit log source is not crushed before
//     it reaches the grade.

#include "media/Source.h"
#include "media/TimestampMap.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}
#define CUTLINE_HAVE_D3D11VA 1
#endif

namespace cutline::media {
namespace {

[[nodiscard]] std::string ErrorText(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  av_strerror(code, buffer.data(), buffer.size());
  return std::string(buffer.data());
}

void Check(int code, const std::string& context) {
  if (code < 0) throw std::runtime_error(context + ": " + ErrorText(code));
}

[[nodiscard]] time::RationalTime FromStreamTime(std::int64_t timestamp, AVRational time_base) {
  if (timestamp == AV_NOPTS_VALUE) return {};
  // pts * time_base, kept exact: RationalTime reduces the fraction itself.
  return time::RationalTime(timestamp * time_base.num, time_base.den);
}

// --------------------------------------------------- colour metadata names ----

[[nodiscard]] std::string PrimariesName(AVColorPrimaries value) {
  const char* name = av_color_primaries_name(value);
  return name != nullptr ? name : "bt709";
}

[[nodiscard]] std::string TransferName(AVColorTransferCharacteristic value) {
  const char* name = av_color_transfer_name(value);
  return name != nullptr ? name : "bt709";
}

[[nodiscard]] std::string MatrixName(AVColorSpace value) {
  const char* name = av_color_space_name(value);
  return name != nullptr ? name : "bt709";
}

[[nodiscard]] std::string FieldOrderName(AVFieldOrder order) {
  switch (order) {
    case AV_FIELD_TT:
    case AV_FIELD_TB: return "upper_first";
    case AV_FIELD_BB:
    case AV_FIELD_BT: return "lower_first";
    default: return "progressive";
  }
}

// ------------------------------------------------------------- RAII owners ----

struct FormatContextDeleter final {
  void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};
struct CodecContextDeleter final {
  void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct PacketDeleter final {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct FrameDeleter final {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct SwsDeleter final {
  void operator()(SwsContext* context) const { sws_freeContext(context); }
};
struct SwrDeleter final {
  void operator()(SwrContext* context) const { swr_free(&context); }
};

using FormatContext = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsScaler = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrResampler = std::unique_ptr<SwrContext, SwrDeleter>;

[[nodiscard]] FormatContext OpenInput(const std::string& path) {
  AVFormatContext* raw = nullptr;
  Check(avformat_open_input(&raw, path.c_str(), nullptr, nullptr), "Unable to open " + path);
  FormatContext context(raw);
  Check(avformat_find_stream_info(context.get(), nullptr), "Unable to read stream info from " + path);
  return context;
}

#ifdef CUTLINE_HAVE_D3D11VA
// ------------------------------------------------------- hardware decoding ----
//
// Direct3D 11 video acceleration, on a device the compositor owns. The decoder writes into an array of NV12 (or P010)
// textures; they are created here, rather than by the decoder's defaults, so that they can also be read by shaders.

struct BufferDeleter final {
  void operator()(AVBufferRef* buffer) const { av_buffer_unref(&buffer); }
};
using Buffer = std::unique_ptr<AVBufferRef, BufferDeleter>;

[[nodiscard]] Buffer MakeD3D11DeviceContext(void* native_device) {
  Buffer ref(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
  if (ref == nullptr) return nullptr;
  auto* context = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
  auto* device = static_cast<ID3D11Device*>(native_device);
  device->AddRef();  // the FFmpeg device context releases it when it is freed
  d3d->device = device;
  if (av_hwdevice_ctx_init(ref.get()) < 0) return nullptr;
  return ref;
}

AVPixelFormat ChooseD3D11Format(AVCodecContext* context, const AVPixelFormat* formats) {
  for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
    if (*format != AV_PIX_FMT_D3D11) continue;
    AVBufferRef* frames = nullptr;
    if (avcodec_get_hw_frames_parameters(context, context->hw_device_ctx, AV_PIX_FMT_D3D11, &frames) < 0) return AV_PIX_FMT_NONE;
    auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames->data);
    auto* d3d = static_cast<AVD3D11VAFramesContext*>(frames_context->hwctx);
    d3d->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    // The decoder's own reference pictures, plus the ones this reader and the compositor hold on to.
    frames_context->initial_pool_size += 8;
    if (av_hwframe_ctx_init(frames) < 0) {
      av_buffer_unref(&frames);
      return AV_PIX_FMT_NONE;
    }
    av_buffer_unref(&context->hw_frames_ctx);
    context->hw_frames_ctx = frames;
    return AV_PIX_FMT_D3D11;
  }
  return AV_PIX_FMT_NONE;  // this stream cannot be decoded in hardware
}

[[nodiscard]] CodecContext OpenDeviceDecoder(AVFormatContext* format, int stream_index, AVBufferRef* device) {
  auto* stream = format->streams[stream_index];
  const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) throw std::runtime_error(std::string("No decoder for codec ") + avcodec_get_name(stream->codecpar->codec_id));
  CodecContext context(avcodec_alloc_context3(codec));
  if (context == nullptr) throw std::runtime_error("Out of memory allocating a decoder");
  Check(avcodec_parameters_to_context(context.get(), stream->codecpar), "Unable to configure decoder");
  context->pkt_timebase = stream->time_base;
  context->hw_device_ctx = av_buffer_ref(device);
  context->get_format = ChooseD3D11Format;
  context->thread_count = 1;  // a hardware decoder is one pipeline
  Check(avcodec_open2(context.get(), codec, nullptr), "Unable to open the hardware decoder");
  return context;
}
#endif  // CUTLINE_HAVE_D3D11VA

[[nodiscard]] CodecContext OpenDecoder(AVFormatContext* format, int stream_index) {
  auto* stream = format->streams[stream_index];
  const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) {
    throw std::runtime_error(std::string("No decoder for codec ") +
                             avcodec_get_name(stream->codecpar->codec_id));
  }
  CodecContext context(avcodec_alloc_context3(codec));
  if (context == nullptr) throw std::runtime_error("Out of memory allocating a decoder");
  Check(avcodec_parameters_to_context(context.get(), stream->codecpar), "Unable to configure decoder");
  context->pkt_timebase = stream->time_base;
  // Let FFmpeg pick a sensible worker count; decode threading inside one
  // stream is independent of Cutline's own job scheduling.
  context->thread_count = 0;
  Check(avcodec_open2(context.get(), codec, nullptr), "Unable to open decoder");
  return context;
}

// ------------------------------------------------------------ source time ----
//
// Source time zero is the earliest timestamp in the container, the same origin
// for every stream. A recording cut from a longer one starts at, say, 1.4 s on
// its container clock; a file whose audio begins half a second after its first
// picture still has its picture at zero and its audio at 0.5 s. Without one
// shared origin, "the audio at source time t" and "the frame at source time t"
// would refer to different instants.

[[nodiscard]] time::RationalTime ContainerOrigin(const AVFormatContext* format) {
  return format->start_time == AV_NOPTS_VALUE ? time::RationalTime{}
                                              : time::RationalTime(format->start_time, AV_TIME_BASE);
}

[[nodiscard]] time::RationalTime ToSourceTime(std::int64_t timestamp, AVRational time_base,
                                              const time::RationalTime& origin) {
  return FromStreamTime(timestamp, time_base).Subtract(origin);
}

// The latest stream timestamp that is not after `source_time`: what a backward
// seek wants.
[[nodiscard]] std::int64_t ToStreamTimeFloor(const time::RationalTime& source_time, const time::RationalTime& origin,
                                             AVRational time_base) {
  const auto absolute = source_time.Add(origin);
  return av_rescale_rnd(absolute.numerator(), time_base.den, absolute.denominator() * time_base.num, AV_ROUND_DOWN);
}

// ----------------------------------------------------------- one demuxer ----
//
// Every consumer of a file owns its own demuxer, decoder and packet queue.
// A single demuxer shared by a video and an audio decoder hands each packet to
// whichever stream asked for the next one and throws away the other stream's:
// alternating reads, which is exactly what playback does, then silently drop
// pieces of whichever stream was not being read. Independent readers cost an
// extra file handle and a second parse of the headers; they cannot disturb one
// another, and they are what a decode thread per stream needs anyway.

struct OpenedStream final {
  FormatContext format;
  int index{-1};
  AVRational time_base{0, 1};
  time::RationalTime origin;
};

[[nodiscard]] OpenedStream OpenStream(const std::string& path, AVMediaType type, FormatContext existing = nullptr) {
  OpenedStream opened;
  opened.format = existing != nullptr ? std::move(existing) : OpenInput(path);
  opened.index = av_find_best_stream(opened.format.get(), type, -1, -1, nullptr, 0);
  if (opened.index < 0) throw std::runtime_error("No such stream in " + path);
  // Let the demuxer drop every other stream's packets itself, so this reader
  // never queues or inspects what it does not consume.
  for (unsigned stream = 0; stream < opened.format->nb_streams; ++stream) {
    if (static_cast<int>(stream) != opened.index) opened.format->streams[stream]->discard = AVDISCARD_ALL;
  }
  opened.time_base = opened.format->streams[opened.index]->time_base;
  opened.origin = ContainerOrigin(opened.format.get());
  return opened;
}

// ------------------------------------------------------------------ video ----

// The same reader serves pictures in memory (VideoFrame, converted by swscale) and pictures left on the GPU by a hardware
// decoder (DeviceFrame): which frame covers a time, how far to decode forward before seeking, and how long a frame lasts
// are the same questions either way.
template <bool kDevice>
class VideoReaderT final {
 public:
  using Item = std::conditional_t<kDevice, DeviceFrame, VideoFrame>;

  VideoReaderT(const std::string& path, FormatContext spare, AVBufferRef* device = nullptr)
      : path_(path), stream_(OpenStream(path, AVMEDIA_TYPE_VIDEO, std::move(spare))) {
    if constexpr (kDevice) {
#ifdef CUTLINE_HAVE_D3D11VA
      decoder_ = OpenDeviceDecoder(stream_.format.get(), stream_.index, device);
#else
      (void)device;
      throw std::runtime_error("This build has no hardware decoding");
#endif
    } else {
      (void)device;
      decoder_ = OpenDecoder(stream_.format.get(), stream_.index);
    }
    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    if (packet_ == nullptr || frame_ == nullptr) throw std::runtime_error("Out of memory opening " + path_);
  }

  // The frame that covers `time`: the one whose presentation time is the latest not
  // after it. How long a frame lasts is when the next one begins, not a duration
  // field: a container's stated frame duration is its nominal rate and is wrong for
  // every frame of a variable-rate file, so the reader decodes one frame ahead and
  // takes the gap. (At the very end of the file there is no next frame, and the
  // stated duration is all there is.)
  [[nodiscard]] std::optional<Item> Read(const time::RationalTime& time) {
    if (cached_.has_value()) {
      if (Covers(time)) return Result();
      // A frame that begins after the time asked for was returned in its place (the
      // picture before the first frame of a stream that starts late is that first
      // frame), and asking again for a time in the same gap must not seek again.
      if (held_ && time.Compare(held_from_) >= 0 && time.Compare(cached_->presentation_time) < 0) return Result();
    }

    // Seek backwards, or forwards past the point where decoding through would
    // cost more than a seek. Decoding forward is preferred for sequential
    // playback because a seek drops the decoder's reference frames.
    const bool behind = cached_.has_value() && time.Compare(cached_->presentation_time) < 0;
    const bool distant = decoded_any_ && time.Subtract(NewestDecoded()).Compare(ForwardDecodeWindow()) > 0;
    if (!decoded_any_ || behind || distant) {
      const auto target = ToStreamTimeFloor(time, stream_.origin, stream_.time_base);
      Check(av_seek_frame(stream_.format.get(), stream_.index, target, AVSEEK_FLAG_BACKWARD),
            "Unable to seek " + path_);
      avcodec_flush_buffers(decoder_.get());
      cached_.reset();
      ahead_.reset();
      at_end_ = false;
      draining_ = false;
      held_ = false;
      decoded_any_ = false;
    }

    while (true) {
      if (!cached_.has_value()) {
        cached_ = DecodeNext();
        if (!cached_.has_value()) return std::nullopt;
        decoded_any_ = true;
      }
      if (!ahead_.has_value() && !at_end_) {
        ahead_ = DecodeNext();
        if (!ahead_.has_value()) at_end_ = true;
      }
      if (cached_->presentation_time.Compare(time) > 0) {
        held_ = true;
        held_from_ = time;
        return Result();
      }
      if (Covers(time)) return Result();
      // Past this frame's end. If nothing follows it, the time is past the last
      // frame, whatever the container's duration claims.
      if (!ahead_.has_value()) return std::nullopt;
      cached_ = std::move(ahead_);
      ahead_.reset();
      held_ = false;
    }
  }

  [[nodiscard]] const AVStream* stream() const { return stream_.format->streams[stream_.index]; }

 private:
  // Decoding forward past this much costs more than seeking. RationalTime
  // reduces its fraction at construction so it is not a constant expression;
  // a function keeps the value in one place without that cost mattering.
  [[nodiscard]] static time::RationalTime ForwardDecodeWindow() { return {2, 1}; }

  // Where the cached frame stops being the picture: when the next frame begins, or,
  // at the end of the file, after its own stated duration.
  [[nodiscard]] time::RationalTime CachedEnd() const {
    if (ahead_.has_value()) return ahead_->presentation_time;
    return cached_->presentation_time.Add(cached_->duration);
  }

  [[nodiscard]] bool Covers(const time::RationalTime& time) const {
    if (time.Compare(cached_->presentation_time) < 0) return false;
    if (!ahead_.has_value() && cached_->duration.Compare({0, 1}) <= 0) return false;
    return time.Compare(CachedEnd()) < 0;
  }

  [[nodiscard]] time::RationalTime NewestDecoded() const {
    return ahead_.has_value() ? ahead_->presentation_time : cached_->presentation_time;
  }

  // A copy of the cached frame, with the duration the timeline says it has.
  [[nodiscard]] std::optional<Item> Result() const {
    auto frame = Copy(*cached_);
    frame.duration = CachedEnd().Subtract(frame.presentation_time);
    return frame;
  }

  [[nodiscard]] static Item Copy(const Item& item) {
    if constexpr (kDevice) return item;  // a reference to the same surface
    else return item.Clone();
  }

  [[nodiscard]] std::optional<Item> DecodeNext() {
    while (true) {
      const auto received = avcodec_receive_frame(decoder_.get(), frame_.get());
      if (received == 0) {
        auto converted = ConvertFrameNow();
        av_frame_unref(frame_.get());
        return converted;
      }
      if (received == AVERROR_EOF) return std::nullopt;
      if (received != AVERROR(EAGAIN)) Check(received, "Video decode failed for " + path_);

      // Need more input.
      const auto read = av_read_frame(stream_.format.get(), packet_.get());
      if (read == AVERROR_EOF) {
        if (!draining_) {
          avcodec_send_packet(decoder_.get(), nullptr);  // flush
          draining_ = true;
        }
        continue;
      }
      Check(read, "Unable to read from " + path_);
      if (packet_->stream_index == stream_.index) {
        Check(avcodec_send_packet(decoder_.get(), packet_.get()), "Unable to queue a video packet");
      }
      av_packet_unref(packet_.get());
    }
  }

  [[nodiscard]] std::optional<Item> ConvertFrameNow() {
    if constexpr (kDevice) return ConvertDevice();
    else return Convert();
  }

  [[nodiscard]] std::optional<DeviceFrame> ConvertDevice() {
#ifdef CUTLINE_HAVE_D3D11VA
    if (frame_->format != AV_PIX_FMT_D3D11 || frame_->hw_frames_ctx == nullptr) throw std::runtime_error("The decoder did not produce a Direct3D picture for " + path_);
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame_->hw_frames_ctx->data);
    DeviceFrame out;
    if (frames->sw_format == AV_PIX_FMT_NV12) out.format = DeviceFormat::Nv12;
    else if (frames->sw_format == AV_PIX_FMT_P010) out.format = DeviceFormat::P010;
    else throw std::runtime_error("Hardware decoding gave a picture format this build does not read in place");
    out.texture = frame_->data[0];
    out.array_index = static_cast<int>(reinterpret_cast<std::intptr_t>(frame_->data[1]));
    out.device = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(frames->device_ctx)->hwctx)->device;
    out.width = frame_->width;
    out.height = frame_->height;
    out.full_range = frame_->color_range == AVCOL_RANGE_JPEG;
    out.bt2020 = frame_->colorspace == AVCOL_SPC_BT2020_NCL || frame_->colorspace == AVCOL_SPC_BT2020_CL;
    // An untagged picture is taken as Rec.709, as the software path does (it hands swscale ITU-709 when the stream says nothing).
    out.bt709 = frame_->colorspace == AVCOL_SPC_UNSPECIFIED || (frame_->colorspace != AVCOL_SPC_BT470BG && frame_->colorspace != AVCOL_SPC_SMPTE170M);
    const auto pts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
    out.presentation_time = ToSourceTime(pts, stream_.time_base, stream_.origin);
    out.duration = FromStreamTime(frame_->duration, stream_.time_base);
    if (out.duration.Compare({0, 1}) <= 0) {
      const auto rate = av_guess_frame_rate(stream_.format.get(), stream_.format->streams[stream_.index], nullptr);
      if (rate.num > 0) out.duration = time::RationalTime(rate.den, rate.num);
    }
    // Keep the decoder's surface from being reused while anyone still holds this picture.
    AVFrame* held = av_frame_alloc();
    if (held == nullptr || av_frame_ref(held, frame_.get()) < 0) {
      av_frame_free(&held);
      throw std::runtime_error("Out of memory holding a decoded picture");
    }
    out.keep_alive = std::shared_ptr<void>(held, [](void* frame) {
      auto* pointer = static_cast<AVFrame*>(frame);
      av_frame_free(&pointer);
    });
    return out;
#else
    throw std::runtime_error("This build has no hardware decoding");
#endif
  }

  [[nodiscard]] std::optional<VideoFrame> Convert() {
    const auto source_format = static_cast<AVPixelFormat>(frame_->format);
    const auto* descriptor = av_pix_fmt_desc_get(source_format);
    const auto depth = descriptor != nullptr ? descriptor->comp[0].depth : 8;
    const bool deep = depth > 8;
    const auto target_format = deep ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;

    if (scaler_ == nullptr || scaler_format_ != source_format || scaler_width_ != frame_->width ||
        scaler_height_ != frame_->height || scaler_deep_ != deep) {
      scaler_.reset(sws_getContext(frame_->width, frame_->height, source_format, frame_->width, frame_->height,
                                   target_format, SWS_BILINEAR, nullptr, nullptr, nullptr));
      if (scaler_ == nullptr) throw std::runtime_error("Unable to create a colour converter for " + path_);
      scaler_format_ = source_format;
      scaler_width_ = frame_->width;
      scaler_height_ = frame_->height;
      scaler_deep_ = deep;

      // Tell swscale the source matrix and range explicitly. Without this it
      // assumes bt601 limited, which tints every Rec.709 or Rec.2020 source.
      const int* source_table = sws_getCoefficients(frame_->colorspace == AVCOL_SPC_UNSPECIFIED
                                                        ? SWS_CS_ITU709
                                                        : frame_->colorspace);
      const int* target_table = sws_getCoefficients(SWS_CS_ITU709);
      const int source_range = frame_->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
      sws_setColorspaceDetails(scaler_.get(), source_table, source_range, target_table, 1, 0, 1 << 16, 1 << 16);
    }

    auto output = VideoFrame::Allocate(deep ? PixelFormat::Rgba16 : PixelFormat::Rgba8, frame_->width,
                                       frame_->height);
    std::array<std::uint8_t*, 4> destination{reinterpret_cast<std::uint8_t*>(output.data()), nullptr, nullptr,
                                             nullptr};
    std::array<int, 4> destination_stride{static_cast<int>(output.stride()), 0, 0, 0};
    sws_scale(scaler_.get(), frame_->data, frame_->linesize, 0, frame_->height, destination.data(),
              destination_stride.data());

    const auto pts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
    output.presentation_time = ToSourceTime(pts, stream_.time_base, stream_.origin);
    output.duration = FromStreamTime(frame_->duration, stream_.time_base);
    if (output.duration.Compare({0, 1}) <= 0) {
      const auto rate = av_guess_frame_rate(stream_.format.get(), stream_.format->streams[stream_.index], nullptr);
      if (rate.num > 0) output.duration = time::RationalTime(rate.den, rate.num);
    }
    output.keyframe = frame_->flags & AV_FRAME_FLAG_KEY;
    output.color.primaries = PrimariesName(frame_->color_primaries);
    output.color.transfer = TransferName(frame_->color_trc);
    output.color.matrix = MatrixName(frame_->colorspace);
    // swscale has normalised to full-range RGB regardless of the source range.
    output.color.range = model::ColorRange::Full;
    const auto aspect = frame_->sample_aspect_ratio;
    if (aspect.num > 0 && aspect.den > 0) output.pixel_aspect = {aspect.num, aspect.den};
    return output;
  }

  std::string path_;
  OpenedStream stream_;
  CodecContext decoder_;
  Packet packet_;
  Frame frame_;
  SwsScaler scaler_;
  AVPixelFormat scaler_format_{AV_PIX_FMT_NONE};
  int scaler_width_{0};
  int scaler_height_{0};
  bool scaler_deep_{false};

  // The frame being served and the one decoded after it; the second is what gives
  // the first its duration.
  std::optional<Item> cached_;
  std::optional<Item> ahead_;
  bool at_end_{false};
  bool held_{false};
  time::RationalTime held_from_;
  bool decoded_any_{false};
  bool draining_{false};
};

using VideoReader = VideoReaderT<false>;
using DeviceVideoReader = VideoReaderT<true>;

// ------------------------------------------------------------------ audio ----
//
// Sample-accurate reads. A request is for output samples [first, first+frames)
// at the caller's rate, and what comes back is the file's signal at exactly
// those instants. Four things have to hold for that to be true.
//
//   * Position comes from the file, not from a running guess. After a seek the
//     first decoded frame says where it is (its timestamp, normalised to source
//     time zero), and samples before the requested start are discarded, not
//     played.
//   * Where a container's time base is coarser than one sample (Matroska counts
//     milliseconds, which is 48 samples at 48 kHz) a timestamp cannot place a
//     frame exactly, so the file's audio packets are counted once, by size, into
//     an index of exact start samples. Time bases of one sample or finer need no
//     index.
//   * Decoders and resamplers need history. Every seek lands well before the
//     target and the lead-in is discarded: it is what lets an MDCT codec settle
//     and a resampler's filter see real signal on both sides of the first sample
//     that is kept.
//   * Resampling is aligned to whole samples. The lead-in starts on a source
//     sample whose output position is also a whole sample, so output sample k is
//     exactly source sample k * source_rate / output_rate. At the end of the file
//     the resampler is flushed, so its last samples are not lost.
//
// When the source rate, the output rate and the stream position cannot all be
// whole numbers at once (a stream that starts partway through a sample at the
// output rate), the error is at most half an output sample.

class AudioReader final {
 public:
  AudioReader(const std::string& path, FormatContext spare)
      : path_(path), stream_(OpenStream(path, AVMEDIA_TYPE_AUDIO, std::move(spare))) {
    decoder_ = OpenDecoder(stream_.format.get(), stream_.index);
    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    if (packet_ == nullptr || frame_ == nullptr) throw std::runtime_error("Out of memory opening " + path_);
    source_rate_ = decoder_->sample_rate;
    if (source_rate_ <= 0) throw std::runtime_error("Audio stream without a sample rate in " + path_);

    const auto origin_stamp = stream_.format->start_time == AV_NOPTS_VALUE ? 0 : stream_.format->start_time;
    origin_samples_ = av_rescale_rnd(origin_stamp, source_rate_, AV_TIME_BASE, AV_ROUND_NEAR_INF);
    // One tick of the stream's time base, in samples: the finest position a
    // timestamp can state, and therefore the discrepancy between a running
    // count and a timestamp that is only rounding.
    const double tick = static_cast<double>(source_rate_) * stream_.time_base.num / stream_.time_base.den;
    coarse_ = tick > 1.0;
    tolerance_ = std::max<std::int64_t>(2, static_cast<std::int64_t>(std::ceil(tick)) + 1);
  }

  [[nodiscard]] std::optional<AudioBuffer> Read(const time::RationalTime& time, std::int64_t rate, int channels,
                                                std::int64_t frames) {
    Configure(rate, channels);
    auto output = AudioBuffer::Allocate(rate, channels, frames);
    output.presentation_time = time;

    const auto first = time.Rescale(rate, time::RoundingMode::Nearest);
    const auto last = first + frames;
    if (NeedsSeek(first)) SeekTo(first);

    std::int64_t index = std::max<std::int64_t>(first, 0);
    while (index < last) {
      while (!eof_ && PendingEnd() <= index) DecodeMore();
      if (PendingBegin() > index) {
        // Before the first sample the stream has: silence.
        index = std::min(PendingBegin(), last);
        continue;
      }
      if (PendingEnd() <= index) break;  // past the last sample: silence
      const auto take = std::min(PendingEnd(), last) - index;
      for (int channel = 0; channel < channels; ++channel) {
        std::memcpy(output.channel(channel) + (index - first),
                    pending_[static_cast<std::size_t>(channel)].data() + (index - pending_begin_),
                    static_cast<std::size_t>(take) * sizeof(float));
      }
      index += take;
    }

    // Keep the previous request's samples as well as this one's, so asking again
    // for the block just read (a crossfade, a nested clip) does not seek.
    TrimPending(std::min(first, last_first_));
    last_first_ = first;
    return output;
  }

 private:
  struct IndexEntry final {
    std::int64_t timestamp{};
    std::int64_t start{};
  };

  // Source samples of lead-in before the first sample that is kept. A resampler
  // filter is a few dozen taps; an AAC or MP3 decoder needs a frame or two.
  static constexpr std::int64_t kLeadIn = 8192;

  [[nodiscard]] std::int64_t PendingBegin() const { return pending_begin_; }
  [[nodiscard]] std::int64_t PendingEnd() const {
    return pending_begin_ + (pending_.empty() ? 0 : static_cast<std::int64_t>(pending_[0].size()));
  }
  [[nodiscard]] std::int64_t OutIndex(std::int64_t source_sample) const {
    return av_rescale_rnd(source_sample, out_rate_, source_rate_, AV_ROUND_NEAR_INF);
  }
  [[nodiscard]] std::int64_t TimestampToSamples(std::int64_t timestamp) const {
    return av_rescale_q_rnd(timestamp, stream_.time_base, AVRational{1, static_cast<int>(source_rate_)},
                            AV_ROUND_NEAR_INF) -
           origin_samples_;
  }

  void Configure(std::int64_t rate, int channels) {
    if (resampler_ != nullptr && out_rate_ == rate && channels_ == channels) return;
    AVChannelLayout target{};
    av_channel_layout_default(&target, channels);
    AVChannelLayout source{};
    Check(av_channel_layout_copy(&source, &decoder_->ch_layout), "Unable to read the audio channel layout");
    if (source.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&source, source.nb_channels);
    SwrContext* raw = nullptr;
    const auto status = swr_alloc_set_opts2(&raw, &target, AV_SAMPLE_FMT_FLTP, static_cast<int>(rate), &source,
                                            decoder_->sample_fmt, decoder_->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&target);
    av_channel_layout_uninit(&source);
    Check(status, "Unable to configure the audio resampler");
    resampler_.reset(raw);
    Check(swr_init(resampler_.get()), "Unable to start the audio resampler");
    out_rate_ = rate;
    channels_ = channels;
    // Everything buffered was at the old rate or layout.
    started_ = false;
    pending_.assign(static_cast<std::size_t>(channels), {});
    pending_begin_ = 0;
  }

  [[nodiscard]] bool NeedsSeek(std::int64_t first) const {
    if (!started_) return true;
    // Earlier than anything still buffered. Not a seek when the buffer begins
    // at the very start of the stream and nothing has been dropped: the
    // samples before it are silence by definition.
    if (first < PendingBegin() && !(at_stream_start_ && !trimmed_)) return true;
    // Far ahead: cheaper to jump than to decode through it.
    if (!eof_ && first > PendingEnd() + out_rate_) return true;
    return false;
  }

  void SeekTo(std::int64_t first_output_sample) {
    const auto target = first_output_sample <= 0
                            ? std::int64_t{0}
                            : av_rescale_rnd(first_output_sample, source_rate_, out_rate_, AV_ROUND_DOWN);
    // The lead-in begins on a source sample whose position at the output rate
    // is a whole number: a multiple of source_rate / gcd(source_rate, out_rate).
    const auto unit = source_rate_ / std::gcd(source_rate_, out_rate_);
    const auto wanted = target - kLeadIn;
    align_ = wanted <= 0 ? std::int64_t{0} : (wanted / unit) * unit;
    at_stream_start_ = align_ == 0;

    // Near the start of the file the lead-in reaches before time zero. Seek there
    // anyway: a codec's first packets are the ones its first real frame overlaps
    // with (an AAC priming frame sits before zero), and seeking to zero itself
    // would skip them.
    const auto lead_start = wanted <= 0 ? wanted : align_;
    std::int64_t seek_stamp = ToStreamTimeFloor(time::RationalTime(lead_start, source_rate_), stream_.origin, stream_.time_base);
    if (UseIndex()) {
      // Land on a packet whose exact start sample is known to be at or before
      // the lead-in.
      auto entry = std::upper_bound(index_.begin(), index_.end(), align_,
                                    [](std::int64_t value, const IndexEntry& item) { return value < item.start; });
      if (entry != index_.begin()) --entry;
      if (entry != index_.end()) seek_stamp = entry->timestamp;
    }
    const auto sought = av_seek_frame(stream_.format.get(), stream_.index, seek_stamp, AVSEEK_FLAG_BACKWARD);
    if (sought < 0) {
      // Some demuxers refuse a seek to before their first timestamp.
      Check(align_ == 0 ? av_seek_frame(stream_.format.get(), -1, stream_.format->start_time == AV_NOPTS_VALUE
                                                                        ? 0
                                                                        : stream_.format->start_time,
                                        AVSEEK_FLAG_BACKWARD)
                        : sought,
            "Unable to seek audio in " + path_);
    }
    avcodec_flush_buffers(decoder_.get());
    Check(swr_init(resampler_.get()), "Unable to restart the audio resampler");

    started_ = true;
    primed_ = false;
    have_position_ = false;
    eof_ = false;
    draining_ = false;
    trimmed_ = false;
    next_source_ = align_;
    pending_.assign(static_cast<std::size_t>(channels_), {});
    pending_begin_ = OutIndex(align_);
    last_first_ = first_output_sample;
  }

  void TrimPending(std::int64_t before) {
    if (before <= pending_begin_) return;
    const auto size = pending_.empty() ? std::int64_t{0} : static_cast<std::int64_t>(pending_[0].size());
    const auto drop = std::min(before - pending_begin_, size);
    for (auto& channel : pending_) channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(drop));
    pending_begin_ += drop;
    trimmed_ = true;
  }

  // Decodes one frame into the pending buffer, or, at the end of the stream,
  // drains the decoder and the resampler.
  void DecodeMore() {
    while (true) {
      const auto received = avcodec_receive_frame(decoder_.get(), frame_.get());
      if (received == 0) {
        Feed();
        av_frame_unref(frame_.get());
        return;
      }
      if (received == AVERROR_EOF) {
        FlushResampler();
        return;
      }
      if (received != AVERROR(EAGAIN)) Check(received, "Audio decode failed for " + path_);

      const auto read = av_read_frame(stream_.format.get(), packet_.get());
      if (read == AVERROR_EOF) {
        if (!draining_) {
          avcodec_send_packet(decoder_.get(), nullptr);
          draining_ = true;
        }
        continue;
      }
      Check(read, "Unable to read from " + path_);
      if (packet_->stream_index == stream_.index) {
        Check(avcodec_send_packet(decoder_.get(), packet_.get()), "Unable to queue an audio packet");
      }
      av_packet_unref(packet_.get());
    }
  }

  // Source sample at which the decoded frame begins.
  [[nodiscard]] std::int64_t FrameBegin() {
    const auto stamp = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
    if (stamp == AV_NOPTS_VALUE) return have_position_ ? next_source_ : align_;
    if (!have_position_ && UseIndex()) {
      const auto entry = std::lower_bound(index_.begin(), index_.end(), stamp,
                                          [](const IndexEntry& item, std::int64_t value) { return item.timestamp < value; });
      if (entry != index_.end() && entry->timestamp == stamp) return entry->start;
    }
    return TimestampToSamples(stamp);
  }

  void Feed() {
    const std::int64_t count = frame_->nb_samples;
    if (count <= 0) return;
    const auto begin = FrameBegin();
    std::int64_t skip = 0;

    if (!have_position_) {
      have_position_ = true;
      next_source_ = begin;
    }
    if (!primed_) {
      // The lead-in. Nothing is kept before the aligned start; if the stream
      // itself starts later than that, it starts where it starts.
      const auto start = std::max(begin, align_);
      if (start >= begin + count) {
        next_source_ = begin + count;
        return;
      }
      skip = start - begin;
      primed_ = true;
      next_source_ = start;
      if (start != align_) pending_begin_ = OutIndex(start);
    } else {
      const auto gap = begin - next_source_;
      if (gap > tolerance_ && gap <= kLargestGap) {
        PushSilence(gap);  // the file really has a hole here; keep time honest
      } else if (gap < -tolerance_) {
        skip = std::min(count, next_source_ - begin);  // overlapping packets
      }
    }
    if (skip >= count) return;
    Push(frame_->extended_data, skip, count - skip);
    next_source_ += count - skip;
  }

  void PushSilence(std::int64_t count) {
    Frame silence(av_frame_alloc());
    silence->format = frame_->format;
    silence->sample_rate = frame_->sample_rate;
    Check(av_channel_layout_copy(&silence->ch_layout, &frame_->ch_layout), "Unable to describe a silent frame");
    constexpr std::int64_t kChunk = 16384;
    for (std::int64_t done = 0; done < count; done += kChunk) {
      const auto piece = std::min(kChunk, count - done);
      av_frame_unref(silence.get());
      silence->format = frame_->format;
      silence->sample_rate = frame_->sample_rate;
      Check(av_channel_layout_copy(&silence->ch_layout, &frame_->ch_layout), "Unable to describe a silent frame");
      silence->nb_samples = static_cast<int>(piece);
      Check(av_frame_get_buffer(silence.get(), 0), "Unable to allocate silence");
      Check(av_samples_set_silence(silence->extended_data, 0, static_cast<int>(piece),
                                   silence->ch_layout.nb_channels, static_cast<AVSampleFormat>(silence->format)),
            "Unable to clear silence");
      Push(silence->extended_data, 0, piece);
      next_source_ += piece;
    }
  }

  // Feeds `count` samples of `data`, starting `skip` samples in, to the
  // resampler and appends whatever it produces.
  void Push(uint8_t* const* data, std::int64_t skip, std::int64_t count) {
    const auto format = static_cast<AVSampleFormat>(frame_->format);
    const auto bytes = static_cast<std::int64_t>(av_get_bytes_per_sample(format));
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int planes = planar ? decoder_->ch_layout.nb_channels : 1;
    const auto stride = planar ? bytes : bytes * decoder_->ch_layout.nb_channels;
    std::vector<const std::uint8_t*> input(static_cast<std::size_t>(planes));
    for (int plane = 0; plane < planes; ++plane) input[static_cast<std::size_t>(plane)] = data[plane] + skip * stride;

    const auto capacity = swr_get_out_samples(resampler_.get(), static_cast<int>(count)) + 64;
    Convert(input.data(), static_cast<int>(count), capacity);
  }

  void Convert(const std::uint8_t** input, int input_count, int capacity) {
    scratch_.resize(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(capacity));
    std::vector<std::uint8_t*> outputs(static_cast<std::size_t>(channels_));
    for (int channel = 0; channel < channels_; ++channel) {
      outputs[static_cast<std::size_t>(channel)] =
          reinterpret_cast<std::uint8_t*>(scratch_.data() + static_cast<std::size_t>(channel) * capacity);
    }
    const auto produced = swr_convert(resampler_.get(), outputs.data(), capacity, input, input_count);
    Check(produced, "Audio resampling failed for " + path_);
    for (int channel = 0; channel < channels_; ++channel) {
      const auto* from = scratch_.data() + static_cast<std::size_t>(channel) * capacity;
      pending_[static_cast<std::size_t>(channel)].insert(pending_[static_cast<std::size_t>(channel)].end(), from,
                                                          from + produced);
    }
  }

  // The resampler holds back a few samples of filter delay. At the end of the
  // stream they are real signal and are drained, not discarded.
  void FlushResampler() {
    constexpr int kCapacity = 4096;
    while (true) {
      const auto before = PendingEnd();
      Convert(nullptr, 0, kCapacity);
      if (PendingEnd() == before) break;
    }
    eof_ = true;
  }

  // ---- index of exact packet start samples, for coarse time bases ----

  [[nodiscard]] bool UseIndex() {
    if (!coarse_) return false;
    if (!index_attempted_) {
      index_attempted_ = true;
      BuildIndex();
    }
    return !index_.empty();
  }

  void BuildIndex() {
    try {
      auto scan = OpenStream(path_, AVMEDIA_TYPE_AUDIO);
      Packet packet(av_packet_alloc());
      const auto* parameters = scan.format->streams[scan.index]->codecpar;
      std::vector<IndexEntry> entries;
      std::int64_t running = 0;
      while (av_read_frame(scan.format.get(), packet.get()) >= 0) {
        if (packet->stream_index == scan.index) {
          const auto stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
          auto samples = static_cast<std::int64_t>(av_get_audio_frame_duration2(const_cast<AVCodecParameters*>(parameters), packet->size));
          if (samples <= 0 && packet->duration > 0) {
            samples = av_rescale_q_rnd(packet->duration, scan.time_base, AVRational{1, static_cast<int>(source_rate_)},
                                       AV_ROUND_NEAR_INF);
          }
          if (stamp == AV_NOPTS_VALUE || samples <= 0) {
            // A packet whose length or position cannot be known makes an exact
            // count impossible; fall back to timestamps alone.
            entries.clear();
            av_packet_unref(packet.get());
            break;
          }
          const auto stated = TimestampToSamples(stamp);
          auto start = entries.empty() ? stated : running;
          if (std::llabs(stated - start) > tolerance_) start = stated;  // a real gap or overlap
          entries.push_back({stamp, start});
          running = start + samples;
        }
        av_packet_unref(packet.get());
      }
      index_ = std::move(entries);
    } catch (const std::exception&) {
      index_.clear();
    }
  }

  // A gap larger than this is taken to be a timestamp fault rather than silence.
  static constexpr std::int64_t kLargestGap = 48000LL * 60;

  std::string path_;
  OpenedStream stream_;
  CodecContext decoder_;
  Packet packet_;
  Frame frame_;
  SwrResampler resampler_;

  std::int64_t source_rate_{0};
  std::int64_t origin_samples_{0};
  std::int64_t tolerance_{2};
  bool coarse_{false};
  std::vector<IndexEntry> index_;
  bool index_attempted_{false};

  std::int64_t out_rate_{0};
  int channels_{0};
  std::vector<std::vector<float>> pending_;
  std::int64_t pending_begin_{0};
  std::vector<float> scratch_;

  bool started_{false};
  bool primed_{false};
  bool have_position_{false};
  bool eof_{false};
  bool draining_{false};
  bool trimmed_{false};
  bool at_stream_start_{false};
  std::int64_t align_{0};
  std::int64_t next_source_{0};
  std::int64_t last_first_{0};
};

// ------------------------------------------------------------- timestamps ----

// A pass over the video packets of a file through a demuxer of its own, so it
// cannot move anything a reader is using. Returns nullptr and says why when the
// file's timestamps cannot be trusted to index frames.
[[nodiscard]] std::unique_ptr<TimestampMap> ScanVideoTimestamps(const std::string& path, FormatContext spare,
                                                                std::string& problem) {
  auto scan = OpenStream(path, AVMEDIA_TYPE_VIDEO, std::move(spare));
  const auto* stream = scan.format->streams[scan.index];
  const bool reorders = stream->codecpar->video_delay > 0;
  const auto fallback_rate = av_guess_frame_rate(scan.format.get(), scan.format->streams[scan.index], nullptr);

  struct Entry final {
    time::RationalTime time;
    time::RationalTime duration;
    bool keyframe{};
  };
  std::vector<Entry> entries;
  Packet packet(av_packet_alloc());
  while (av_read_frame(scan.format.get(), packet.get()) >= 0) {
    if (packet->stream_index == scan.index) {
      // A decode timestamp is only a presentation timestamp when frames are not
      // reordered; with B-frames it would put every frame in the wrong place.
      const auto stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : (reorders ? AV_NOPTS_VALUE : packet->dts);
      if (stamp == AV_NOPTS_VALUE) {
        problem = "the video packets carry no presentation timestamps";
        return nullptr;
      }
      time::RationalTime duration = FromStreamTime(packet->duration, scan.time_base);
      entries.push_back({ToSourceTime(stamp, scan.time_base, scan.origin), duration,
                         (packet->flags & AV_PKT_FLAG_KEY) != 0});
    }
    av_packet_unref(packet.get());
  }
  if (entries.empty()) {
    problem = "the file has no video packets";
    return nullptr;
  }

  std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
    return left.time.Compare(right.time) < 0;
  });
  std::vector<FrameTimestamp> frames;
  frames.reserve(entries.size());
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (index > 0 && entries[index].time.Compare(entries[index - 1].time) == 0) {
      problem = "two video frames share a presentation timestamp";
      return nullptr;
    }
    // A frame lasts until the next one begins; only the last needs a stated or
    // guessed length.
    time::RationalTime duration;
    if (index + 1 < entries.size()) {
      duration = entries[index + 1].time.Subtract(entries[index].time);
    } else if (entries[index].duration.Compare({0, 1}) > 0) {
      duration = entries[index].duration;
    } else {
      duration = fallback_rate.num > 0 ? time::RationalTime(fallback_rate.den, fallback_rate.num)
                                       : time::RationalTime(1, 25);
    }
    frames.push_back({entries[index].time, duration, entries[index].keyframe});
  }
  // One tick of the container's time base is rounding, not a change of cadence.
  return std::make_unique<TimestampMap>(std::move(frames), time::RationalTime(scan.time_base.num, scan.time_base.den));
}

// ----------------------------------------------------------------- duration ----

// Where the content actually ends, in source time, found by reading the last
// packets of the file. Needed when the container's start time is not zero: a
// muxer that stamps a 4 s recording starting at 1.4 s may declare its duration as
// 5.4 s (it counts the offset in), and every stream's own duration repeats the
// same figure, so nothing in the headers can say which is right. The last packets
// can. Returns nothing when the file cannot be sought or has no packets.
[[nodiscard]] std::optional<time::RationalTime> ScanContentEnd(const std::string& path) {
  try {
    auto format = OpenInput(path);
    if (format->duration == AV_NOPTS_VALUE || format->start_time == AV_NOPTS_VALUE) return std::nullopt;
    const auto origin = ContainerOrigin(format.get());
    if (av_seek_frame(format.get(), -1, format->start_time + format->duration, AVSEEK_FLAG_BACKWARD) < 0) {
      return std::nullopt;
    }
    Packet packet(av_packet_alloc());
    std::optional<time::RationalTime> end;
    while (av_read_frame(format.get(), packet.get()) >= 0) {
      const auto* stream = format->streams[packet->stream_index];
      const auto stamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
      if (stamp != AV_NOPTS_VALUE) {
        const auto finish = ToSourceTime(stamp + std::max<std::int64_t>(packet->duration, 0), stream->time_base, origin);
        if (!end.has_value() || finish.Compare(*end) > 0) end = finish;
      }
      av_packet_unref(packet.get());
    }
    return end;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// ----------------------------------------------------------------- source ----

class FFmpegSource final : public Source {
 public:
  explicit FFmpegSource(std::string path, const OpenOptions& options = {}) : path_(std::move(path)) {
    spare_ = OpenInput(path_);
    video_index_ = av_find_best_stream(spare_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_index_ = av_find_best_stream(spare_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (video_index_ < 0 && audio_index_ < 0) {
      throw std::runtime_error("No decodable video or audio stream in " + path_);
    }
    // Refuse a file whose codec cannot be decoded now, at open, rather than at
    // the first read. The decoders themselves are opened on first use.
    for (const int index : {video_index_, audio_index_}) {
      if (index < 0) continue;
      const auto id = spare_->streams[index]->codecpar->codec_id;
      if (avcodec_find_decoder(id) == nullptr) {
        throw std::runtime_error(std::string("No decoder for codec ") + avcodec_get_name(id));
      }
    }
    BuildProbe();
#ifdef CUTLINE_HAVE_D3D11VA
    if (options.d3d11_device != nullptr && video_index_ >= 0) {
      device_context_ = MakeD3D11DeviceContext(options.d3d11_device);
      device_available_ = device_context_ != nullptr;
    }
#else
    (void)options;
#endif
  }

  [[nodiscard]] const Probe& probe() const override { return probe_; }

  [[nodiscard]] bool device_decode_available() const override { return device_available_; }

  [[nodiscard]] std::optional<DeviceFrame> ReadDeviceVideo(const time::RationalTime& time) override {
#ifdef CUTLINE_HAVE_D3D11VA
    if (!device_available_ || video_index_ < 0) return std::nullopt;
    if (time.Compare({0, 1}) < 0) return std::nullopt;
    if (probe_.duration.Compare({0, 1}) > 0 && time.Compare(probe_.duration) >= 0) return std::nullopt;
    try {
      // Its own demuxer: the software reader keeps the one the file was probed with.
      if (device_video_ == nullptr) device_video_ = std::make_unique<DeviceVideoReader>(path_, nullptr, device_context_.get());
      return device_video_->Read(time);
    } catch (const std::exception&) {
      // Not every stream can be decoded in hardware (an unsupported profile, a 4:4:4 file, no free decoder). Once it has
      // failed it stays failed, and the caller reads the picture in software.
      device_available_ = false;
      device_video_.reset();
      return std::nullopt;
    }
#else
    (void)time;
    return std::nullopt;
#endif
  }

  [[nodiscard]] std::optional<VideoFrame> ReadVideo(const time::RationalTime& time) override {
    if (video_index_ < 0) return std::nullopt;
    if (time.Compare({0, 1}) < 0) return std::nullopt;
    if (probe_.duration.Compare({0, 1}) > 0 && time.Compare(probe_.duration) >= 0) return std::nullopt;
    if (video_ == nullptr) video_ = std::make_unique<VideoReader>(path_, TakeSpare());
    return video_->Read(time);
  }

  [[nodiscard]] std::optional<AudioBuffer> ReadAudio(const time::RationalTime& time, std::int64_t sample_rate,
                                                     int channels, std::int64_t frames) override {
    if (audio_index_ < 0) return std::nullopt;
    if (frames <= 0 || sample_rate <= 0 || channels <= 0) return std::nullopt;
    if (probe_.duration.Compare({0, 1}) > 0 && time.Compare(probe_.duration) >= 0) return std::nullopt;
    if (audio_ == nullptr) audio_ = std::make_unique<AudioReader>(path_, TakeSpare());
    return audio_->Read(time, sample_rate, channels, frames);
  }

  [[nodiscard]] const TimestampMap* timestamps() const override {
    // Built on demand: it needs a pass over the file's packets, which is cheap
    // relative to decoding but not free, and most playback never asks for it.
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    if (!timestamps_attempted_ && video_index_ >= 0) {
      timestamps_attempted_ = true;
      try {
        timestamps_ = ScanVideoTimestamps(path_, nullptr, timestamp_problem_);
      } catch (const std::exception& error) {
        timestamps_.reset();
        timestamp_problem_ = error.what();
      }
    }
    return timestamps_.get();
  }

  [[nodiscard]] std::string timestamps_problem() const override {
    (void)timestamps();
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    return timestamp_problem_;
  }

 private:
  // The first reader to start takes the context the file was probed with. Taken
  // under a lock because the picture and the sound may start from different threads.
  [[nodiscard]] FormatContext TakeSpare() {
    const std::lock_guard<std::mutex> lock(open_mutex_);
    return std::move(spare_);
  }

  void BuildProbe() {
    auto* format = spare_.get();
    probe_.container = format->iformat != nullptr && format->iformat->name != nullptr ? format->iformat->name : "";
    if (format->duration != AV_NOPTS_VALUE) {
      probe_.duration = time::RationalTime(format->duration, AV_TIME_BASE);
      // A container whose clock does not start at zero is where a declared duration
      // is most likely to include the offset. Check it against the content, and
      // believe the content when it is shorter.
      if (format->start_time != AV_NOPTS_VALUE && format->start_time > 0) {
        const auto content = ScanContentEnd(path_);
        if (content.has_value() && content->Compare({0, 1}) > 0 && content->Compare(probe_.duration) < 0) {
          probe_.duration = *content;
        }
      }
    }

    for (unsigned index = 0; index < format->nb_streams; ++index) {
      auto* stream = format->streams[index];
      const auto* parameters = stream->codecpar;
      commands::MediaStream described;
      described.stream_index = index;
      const char* codec_name = avcodec_get_name(parameters->codec_id);
      described.codec = codec_name != nullptr ? codec_name : "";

      switch (parameters->codec_type) {
        case AVMEDIA_TYPE_VIDEO: {
          described.kind = model::StreamKind::Video;
          described.width = parameters->width;
          described.height = parameters->height;
          const auto aspect = parameters->sample_aspect_ratio;
          described.pixel_aspect = aspect.num > 0 && aspect.den > 0
                                       ? time::FrameRate{aspect.num, aspect.den}
                                       : time::FrameRate{1, 1};
          const auto rate = av_guess_frame_rate(format, stream, nullptr);
          described.frame_rate = rate.num > 0 && rate.den > 0 ? time::FrameRate{rate.num, rate.den}
                                                              : time::FrameRate{0, 1};
          // avg_frame_rate differing from r_frame_rate is the usual signature
          // of a variable-rate source; the timestamp map confirms it exactly.
          const auto average = stream->avg_frame_rate;
          const auto nominal = stream->r_frame_rate;
          described.cadence = (average.num != nominal.num || average.den != nominal.den)
                                  ? model::Cadence::Variable
                                  : model::Cadence::Constant;
          const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(parameters->format));
          described.bit_depth = descriptor != nullptr ? descriptor->comp[0].depth : 8;
          described.chroma = descriptor != nullptr && descriptor->name != nullptr ? descriptor->name : "";
          described.field_order = model::ParseFieldOrder(FieldOrderName(parameters->field_order));
          described.color_primaries = PrimariesName(parameters->color_primaries);
          described.color_transfer = TransferName(parameters->color_trc);
          described.color_matrix = MatrixName(parameters->color_space);
          described.color_range =
              parameters->color_range == AVCOL_RANGE_JPEG ? model::ColorRange::Full : model::ColorRange::Limited;
          break;
        }
        case AVMEDIA_TYPE_AUDIO: {
          described.kind = model::StreamKind::Audio;
          described.sample_rate = parameters->sample_rate;
          described.channel_count = parameters->ch_layout.nb_channels;
          std::array<char, 64> layout{};
          if (av_channel_layout_describe(&parameters->ch_layout, layout.data(), layout.size()) > 0) {
            described.channel_layout = layout.data();
          }
          break;
        }
        case AVMEDIA_TYPE_SUBTITLE: described.kind = model::StreamKind::Subtitle; break;
        default: described.kind = model::StreamKind::Data; break;
      }
      probe_.streams.push_back(std::move(described));
    }

    // Start timecode, when the container carries one.
    if (const auto* entry = av_dict_get(format->metadata, "timecode", nullptr, 0); entry != nullptr) {
      const auto* video = probe_.PrimaryVideo();
      if (video != nullptr && video->frame_rate.numerator > 0) {
        try {
          probe_.start_timecode = time::RationalTime::ParseTimecode(entry->value, video->frame_rate);
        } catch (const std::exception&) {
          // A malformed container timecode is not worth failing the import for.
        }
      }
    }
  }

  std::string path_;
  // The context the file was probed with, handed to the first reader that wants
  // one so the headers are parsed once in the common single-stream case.
  FormatContext spare_;
  int video_index_{-1};
  int audio_index_{-1};
  Probe probe_;
  std::unique_ptr<VideoReader> video_;
  std::unique_ptr<AudioReader> audio_;
#ifdef CUTLINE_HAVE_D3D11VA
  Buffer device_context_;
  std::unique_ptr<DeviceVideoReader> device_video_;
#endif
  bool device_available_{false};
  std::mutex open_mutex_;

  mutable std::mutex timestamp_mutex_;
  mutable std::unique_ptr<TimestampMap> timestamps_;
  mutable bool timestamps_attempted_{false};
  mutable std::string timestamp_problem_;
};

class FFmpegProvider final : public SourceProvider {
 public:
  [[nodiscard]] std::string name() const override { return "ffmpeg"; }

  [[nodiscard]] bool CanOpen(const std::string& path) const override {
    // Anything that is a real file on disk. A failed open is reported by Open
    // rather than guessed at here, since FFmpeg reads far more than any
    // extension list would capture.
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
  }

  [[nodiscard]] std::unique_ptr<Source> Open(const std::string& path) override {
    return std::make_unique<FFmpegSource>(path);
  }

  [[nodiscard]] std::unique_ptr<Source> Open(const std::string& path, const OpenOptions& options) override {
    return std::make_unique<FFmpegSource>(path, options);
  }

  [[nodiscard]] Probe ProbeFile(const std::string& path) override { return FFmpegSource(path).probe(); }
};

}  // namespace

void RegisterFFmpegProvider() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  // Quiet by default: FFmpeg's own logging is noise in a host application, and
  // errors reach the caller as exceptions.
  av_log_set_level(AV_LOG_ERROR);
  SourceRegistry::Instance().Register(std::make_unique<FFmpegProvider>());
}

}  // namespace cutline::media

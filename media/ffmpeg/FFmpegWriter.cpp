// FFmpeg encode and mux.
//
// The counterpart to FFmpegSource. Takes RGBA frames and planar float audio
// from the compositor and the mixer, converts them into whatever the chosen
// codecs want, and interleaves them into a container.
//
// Timestamps come from the frame index and the declared frame rate, never from
// the frame's own presentation_time. An exported file must have a perfectly
// regular cadence: if the renderer were ever asked for a time it could not
// resolve, the result should be a repeated picture, not a hole in the timeline.
//
// Audio is buffered into exactly the frame size the encoder asks for. Most
// codecs refuse a partial frame, and feeding them one block per video frame
// would otherwise fail on the first frame whose sample count is not a multiple
// of the codec's own.

#ifdef CUTLINE_HAVE_FFMPEG

#include "media/Writer.h"

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
#include <atomic>
#include <chrono>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

struct FormatContextDeleter final {
  void operator()(AVFormatContext* context) const {
    if (context == nullptr) return;
    if (context->pb != nullptr && (context->oformat->flags & AVFMT_NOFILE) == 0) avio_closep(&context->pb);
    avformat_free_context(context);
  }
};
struct CodecContextDeleter final {
  void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct FrameDeleter final {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct PacketDeleter final {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct SwsDeleter final {
  void operator()(SwsContext* context) const { sws_freeContext(context); }
};
struct SwrDeleter final {
  void operator()(SwrContext* context) const { swr_free(&context); }
};

using FormatContext = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsScaler = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrResampler = std::unique_ptr<SwrContext, SwrDeleter>;

[[nodiscard]] AVColorPrimaries ParsePrimaries(const std::string& name) {
  const auto value = av_color_primaries_from_name(name.c_str());
  return value < 0 ? AVCOL_PRI_BT709 : static_cast<AVColorPrimaries>(value);
}
[[nodiscard]] AVColorTransferCharacteristic ParseTransfer(const std::string& name) {
  const auto value = av_color_transfer_from_name(name.c_str());
  return value < 0 ? AVCOL_TRC_BT709 : static_cast<AVColorTransferCharacteristic>(value);
}
[[nodiscard]] AVColorSpace ParseMatrix(const std::string& name) {
  const auto value = av_color_space_from_name(name.c_str());
  return value < 0 ? AVCOL_SPC_BT709 : static_cast<AVColorSpace>(value);
}

class FFmpegWriter final : public Writer {
 public:
  explicit FFmpegWriter(ExportSettings settings) : settings_(std::move(settings)) {
    if (settings_.path.empty()) throw std::invalid_argument("An export needs an output path");
    if (!settings_.video.has_value() && !settings_.audio.has_value()) {
      throw std::invalid_argument("An export needs at least a video or an audio stream");
    }

    const std::filesystem::path destination(settings_.path);
    const auto parent = destination.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);

    // Refuse before creating anything: replacing a delivery has to be asked for.
    std::error_code exists_error;
    if (!settings_.overwrite && std::filesystem::exists(destination, exists_error)) {
      throw std::runtime_error(settings_.path + " already exists; allow overwrite to replace it");
    }

    // All work happens in a sibling temporary and the destination is only touched
    // once the file is complete. Writing straight to the destination meant that a
    // cancelled or failed re-export deleted the delivery it was meant to replace,
    // and that anything watching the folder saw a half-written file that looked
    // finished. The suffix is unique per writer so two exports cannot collide.
    static std::atomic<unsigned> sequence{0};
    temp_path_ = settings_.path + ".cutline-" +
                 std::to_string(static_cast<unsigned long long>(
                     std::chrono::steady_clock::now().time_since_epoch().count())) +
                 "-" + std::to_string(sequence.fetch_add(1)) + ".part";

    AVFormatContext* raw = nullptr;
    const char* container = settings_.container.empty() ? nullptr : settings_.container.c_str();
    // The muxer is chosen from the *destination's* extension; the temporary has
    // an extension FFmpeg cannot interpret.
    Check(avformat_alloc_output_context2(&raw, nullptr, container, settings_.path.c_str()),
          "Unable to choose an output format for " + settings_.path);
    format_.reset(raw);
    // Muxers that finish a file by reopening it (an MP4 moving its index to the front) do so by this name, so it has to be the
    // file being written, not the destination it will be renamed to.
    av_free(format_->url);
    format_->url = av_strdup(temp_path_.c_str());

    if (settings_.video.has_value()) OpenVideo(*settings_.video);
    if (settings_.audio.has_value()) OpenAudio(*settings_.audio);

    if ((format_->oformat->flags & AVFMT_NOFILE) == 0) {
      Check(avio_open(&format_->pb, temp_path_.c_str(), AVIO_FLAG_WRITE), "Unable to create " + temp_path_);
    }
    AVDictionary* header_options = nullptr;
    for (const auto& [key, value] : settings_.container_options) av_dict_set(&header_options, key.c_str(), value.c_str(), 0);
    const auto header = avformat_write_header(format_.get(), &header_options);
    const auto* unknown = av_dict_iterate(header_options, nullptr);
    const std::string unknown_option = unknown != nullptr ? unknown->key : "";
    av_dict_free(&header_options);
    Check(header, "Unable to write the header of " + settings_.path);
    if (!unknown_option.empty()) throw std::invalid_argument("The " + std::string(format_->oformat->name) + " container does not understand the option " + unknown_option);
    packet_.reset(av_packet_alloc());
    if (packet_ == nullptr) throw std::runtime_error("Out of memory opening " + settings_.path);
  }

  ~FFmpegWriter() override {
    if (!published_) {
      // An abandoned or failed export discards its own temporary and nothing
      // else: a truncated file that still opens looks like a finished render,
      // and the destination was never touched, so a previous delivery survives.
      format_.reset();
      std::error_code error;
      std::filesystem::remove(temp_path_, error);
    }
  }

  void WriteVideo(const VideoFrame& frame) override {
    if (video_ == nullptr) throw std::logic_error("This export has no video stream");
    if (published_ || failed_) throw std::logic_error("This export is no longer accepting frames");
    if (!frame.valid()) throw std::invalid_argument("Cannot write an empty video frame");

    // The compositor hands us 8- or 16-bit RGBA; swscale converts to whatever
    // the encoder wants, with the output colour metadata stated explicitly.
    // A float picture is narrowed to 16 bits when the encoder keeps more than 8 (ProRes, 10-bit HEVC, DNxHR 444...), and to 8
    // otherwise, so a 10-bit delivery holds the 10 bits the compositor made instead of 8 stretched.
    const auto* depth = av_pix_fmt_desc_get(video_->pix_fmt);
    const bool deep_target = depth != nullptr && depth->comp[0].depth > 8;
    const auto narrowed = frame.format() == PixelFormat::RgbaF32 ? ConvertFrame(frame, deep_target ? PixelFormat::Rgba16 : PixelFormat::Rgba8) : VideoFrame{};
    const auto& source = frame.format() == PixelFormat::RgbaF32 ? narrowed : frame;
    const auto actual_format = source.format() == PixelFormat::Rgba16 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;

    if (scaler_ == nullptr || scaler_input_ != actual_format || scaler_width_ != source.width() ||
        scaler_height_ != source.height()) {
      const bool resize = source.width() != video_->width || source.height() != video_->height;
      scaler_.reset(sws_getContext(source.width(), source.height(), actual_format, video_->width, video_->height,
                                   video_->pix_fmt, resize ? SWS_BICUBIC : SWS_BILINEAR, nullptr, nullptr, nullptr));
      if (scaler_ == nullptr) throw std::runtime_error("Unable to create the export colour converter");
      scaler_input_ = actual_format;
      scaler_width_ = source.width();
      scaler_height_ = source.height();
      const int* table = sws_getCoefficients(video_->colorspace == AVCOL_SPC_UNSPECIFIED ? SWS_CS_ITU709
                                                                                         : video_->colorspace);
      const int target_range = video_->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
      sws_setColorspaceDetails(scaler_.get(), sws_getCoefficients(SWS_CS_ITU709), 1, table, target_range, 0,
                               1 << 16, 1 << 16);
    }

    Check(av_frame_make_writable(video_frame_.get()), "Unable to prepare an export video frame");
    const std::array<const std::uint8_t*, 4> input{reinterpret_cast<const std::uint8_t*>(source.data()), nullptr,
                                                   nullptr, nullptr};
    const std::array<int, 4> input_stride{static_cast<int>(source.stride()), 0, 0, 0};
    sws_scale(scaler_.get(), input.data(), input_stride.data(), 0, source.height(), video_frame_->data,
              video_frame_->linesize);

    // Timestamps come from the index, so cadence is regular by construction.
    video_frame_->pts = video_frames_;
    Encode(video_.get(), video_frame_.get(), video_stream_);
    ++video_frames_;
  }

  void WriteAudio(const AudioBuffer& audio) override {
    if (audio_ == nullptr) throw std::logic_error("This export has no audio stream");
    if (published_ || failed_) throw std::logic_error("This export is no longer accepting audio");
    if (!audio.valid()) throw std::invalid_argument("Cannot write an empty audio buffer");
    if (audio.frames() == 0) return;

    // The input format is whatever the caller delivers, and is converted to the
    // encoder's rate and layout here. This used to check only the channel count
    // and treat samples at any rate as samples at the encoder's, so audio handed
    // over at 48 kHz for a 44.1 kHz file played back about a semitone flat.
    EnsureInputFormat(audio.sample_rate(), audio.channels());

    const auto target_channels = static_cast<int>(pending_.size());
    if (input_resampler_ == nullptr) {
      // Nothing to convert: queue each channel as it is.
      for (int channel = 0; channel < target_channels; ++channel) {
        auto& queue = pending_[static_cast<std::size_t>(channel)];
        queue.insert(queue.end(), audio.channel(channel), audio.channel(channel) + audio.frames());
      }
      pending_frames_ += audio.frames();
    } else {
      std::vector<const std::uint8_t*> input(static_cast<std::size_t>(audio.channels()));
      for (int channel = 0; channel < audio.channels(); ++channel) {
        input[static_cast<std::size_t>(channel)] = reinterpret_cast<const std::uint8_t*>(audio.channel(channel));
      }
      ConvertInto(input.data(), static_cast<int>(audio.frames()));
    }
    DrainAudio(false);
  }

  void Finish() override {
    if (published_) return;
    if (failed_) throw std::logic_error("This export has already failed");
    try {
      // Anything still inside the input resampler (its filter delay) goes out
      // first, then whatever audio is left as a final, possibly short, frame.
      if (audio_ != nullptr) {
        FlushInputResampler();
        DrainAudio(true);
      }
      if (video_ != nullptr) Encode(video_.get(), nullptr, video_stream_);
      if (audio_ != nullptr) Encode(audio_.get(), nullptr, audio_stream_);
      Check(av_write_trailer(format_.get()), "Unable to finish " + settings_.path);
      // Close the temporary before publishing it.
      format_.reset();
      Publish();
      // Only now is the export finished. Setting this earlier meant a failure
      // while draining or writing the trailer was treated as completion, so the
      // cleanup that guards against invalid files never ran.
      published_ = true;
    } catch (...) {
      failed_ = true;
      throw;
    }
  }

  // Moves the finished temporary onto the destination in one step.
  void Publish() {
    const std::filesystem::path destination(settings_.path);
    const std::filesystem::path temporary(temp_path_);
    std::error_code error;
    if (!settings_.overwrite && std::filesystem::exists(destination, error)) {
      // Something appeared while we were rendering. Do not clobber it.
      throw std::runtime_error(settings_.path + " appeared during the export and overwrite is not allowed");
    }
    // rename replaces an existing file on the platforms we target, which is the
    // atomic replacement we want when overwrite is allowed.
    std::filesystem::rename(temporary, destination, error);
    if (error) throw std::runtime_error("Unable to publish " + settings_.path + ": " + error.message());
  }

  [[nodiscard]] std::int64_t video_frames_written() const override { return video_frames_; }
  [[nodiscard]] std::int64_t audio_frames_written() const override { return audio_frames_; }

 private:
  void OpenVideo(const VideoEncoderSettings& settings) {
    const auto* codec = avcodec_find_encoder_by_name(settings.codec.c_str());
    if (codec == nullptr) throw std::runtime_error("No encoder named " + settings.codec);
    if (settings.width <= 0 || settings.height <= 0) {
      throw std::invalid_argument("Export frame size must be positive");
    }

    video_stream_ = avformat_new_stream(format_.get(), nullptr);
    if (video_stream_ == nullptr) throw std::runtime_error("Unable to add a video stream");

    video_.reset(avcodec_alloc_context3(codec));
    if (video_ == nullptr) throw std::runtime_error("Out of memory allocating the video encoder");
    video_->width = static_cast<int>(settings.width);
    video_->height = static_cast<int>(settings.height);
    video_->time_base = AVRational{static_cast<int>(settings.frame_rate.denominator),
                                   static_cast<int>(settings.frame_rate.numerator)};
    video_->framerate = AVRational{static_cast<int>(settings.frame_rate.numerator),
                                   static_cast<int>(settings.frame_rate.denominator)};
    video_->sample_aspect_ratio = AVRational{static_cast<int>(settings.pixel_aspect.numerator),
                                             static_cast<int>(settings.pixel_aspect.denominator)};
    const auto pixel_format = av_get_pix_fmt(settings.pixel_format.c_str());
    if (pixel_format == AV_PIX_FMT_NONE) {
      throw std::runtime_error("Unknown pixel format " + settings.pixel_format);
    }
    video_->pix_fmt = pixel_format;
    video_->color_primaries = ParsePrimaries(settings.color_primaries);
    video_->color_trc = ParseTransfer(settings.color_transfer);
    video_->colorspace = ParseMatrix(settings.color_matrix);
    video_->color_range = settings.color_range == model::ColorRange::Full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    if (settings.bitrate > 0) video_->bit_rate = settings.bitrate;
    if (settings.gop_size > 0) video_->gop_size = static_cast<int>(settings.gop_size);
    video_->thread_count = 0;
    if ((format_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      video_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* options = nullptr;
    for (const auto& [key, value] : settings.options) av_dict_set(&options, key.c_str(), value.c_str(), 0);
    const auto opened = avcodec_open2(video_.get(), codec, &options);
    const auto* leftover = av_dict_iterate(options, nullptr);
    const std::string unknown_option = leftover != nullptr ? leftover->key : "";
    av_dict_free(&options);
    Check(opened, "Unable to open the " + settings.codec + " encoder");
    if (!unknown_option.empty()) throw std::invalid_argument("The " + settings.codec + " encoder does not understand the option " + unknown_option);
    Check(avcodec_parameters_from_context(video_stream_->codecpar, video_.get()),
          "Unable to describe the video stream");
    video_stream_->time_base = video_->time_base;
    video_stream_->avg_frame_rate = video_->framerate;

    video_frame_.reset(av_frame_alloc());
    if (video_frame_ == nullptr) throw std::runtime_error("Out of memory allocating an export frame");
    video_frame_->format = video_->pix_fmt;
    video_frame_->width = video_->width;
    video_frame_->height = video_->height;
    video_frame_->colorspace = video_->colorspace;
    video_frame_->color_range = video_->color_range;
    Check(av_frame_get_buffer(video_frame_.get(), 0), "Unable to allocate an export frame");
  }

  void OpenAudio(const AudioEncoderSettings& settings) {
    const auto* codec = avcodec_find_encoder_by_name(settings.codec.c_str());
    if (codec == nullptr) throw std::runtime_error("No encoder named " + settings.codec);
    if (settings.sample_rate <= 0 || settings.channels <= 0) {
      throw std::invalid_argument("Export audio format must be positive");
    }

    audio_stream_ = avformat_new_stream(format_.get(), nullptr);
    if (audio_stream_ == nullptr) throw std::runtime_error("Unable to add an audio stream");

    audio_.reset(avcodec_alloc_context3(codec));
    if (audio_ == nullptr) throw std::runtime_error("Out of memory allocating the audio encoder");
    audio_->sample_rate = static_cast<int>(settings.sample_rate);
    av_channel_layout_default(&audio_->ch_layout, settings.channels);
    // Take the first sample format the encoder supports; the resampler converts
    // our planar float into it.
    audio_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    const enum AVSampleFormat* formats = nullptr;
    int format_count = 0;
    if (avcodec_get_supported_config(audio_.get(), codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     reinterpret_cast<const void**>(&formats), &format_count) >= 0 &&
        formats != nullptr && format_count > 0) {
      audio_->sample_fmt = formats[0];
    }
    if (settings.bitrate > 0) audio_->bit_rate = settings.bitrate;
    audio_->time_base = AVRational{1, audio_->sample_rate};
    if ((format_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      audio_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* options = nullptr;
    for (const auto& [key, value] : settings.options) av_dict_set(&options, key.c_str(), value.c_str(), 0);
    const auto opened = avcodec_open2(audio_.get(), codec, &options);
    const auto* leftover = av_dict_iterate(options, nullptr);
    const std::string unknown_option = leftover != nullptr ? leftover->key : "";
    av_dict_free(&options);
    Check(opened, "Unable to open the " + settings.codec + " encoder");
    if (!unknown_option.empty()) throw std::invalid_argument("The " + settings.codec + " encoder does not understand the option " + unknown_option);
    Check(avcodec_parameters_from_context(audio_stream_->codecpar, audio_.get()),
          "Unable to describe the audio stream");
    audio_stream_->time_base = audio_->time_base;

    // A codec that reports no fixed frame size takes whatever it is given; one
    // that does must be fed exactly that many samples per call.
    audio_frame_size_ = audio_->frame_size > 0 ? audio_->frame_size : 1024;
    pending_.assign(static_cast<std::size_t>(settings.channels), {});

    audio_frame_.reset(av_frame_alloc());
    if (audio_frame_ == nullptr) throw std::runtime_error("Out of memory allocating an export audio frame");
    audio_frame_->format = audio_->sample_fmt;
    audio_frame_->sample_rate = audio_->sample_rate;
    Check(av_channel_layout_copy(&audio_frame_->ch_layout, &audio_->ch_layout), "Unable to set the channel layout");
    audio_frame_->nb_samples = static_cast<int>(audio_frame_size_);
    Check(av_frame_get_buffer(audio_frame_.get(), 0), "Unable to allocate an export audio frame");

    AVChannelLayout source_layout{};
    av_channel_layout_default(&source_layout, settings.channels);
    SwrContext* raw = nullptr;
    Check(swr_alloc_set_opts2(&raw, &audio_->ch_layout, audio_->sample_fmt, audio_->sample_rate, &source_layout,
                              AV_SAMPLE_FMT_FLTP, static_cast<int>(settings.sample_rate), 0, nullptr),
          "Unable to configure the export resampler");
    resampler_.reset(raw);
    Check(swr_init(resampler_.get()), "Unable to start the export resampler");
    av_channel_layout_uninit(&source_layout);
  }

  // Fixes the input format on the first block and builds the converter if it
  // differs from the encoder's. Every later block must match.
  void EnsureInputFormat(std::int64_t rate, int channels) {
    if (input_rate_ == 0) {
      input_rate_ = rate;
      input_channels_ = channels;
      const auto target_channels = audio_->ch_layout.nb_channels;
      if (rate == audio_->sample_rate && channels == target_channels) return;

      AVChannelLayout source{};
      av_channel_layout_default(&source, channels);
      SwrContext* raw = nullptr;
      const auto configured = swr_alloc_set_opts2(&raw, &audio_->ch_layout, AV_SAMPLE_FMT_FLTP, audio_->sample_rate,
                                                  &source, AV_SAMPLE_FMT_FLTP, static_cast<int>(rate), 0, nullptr);
      av_channel_layout_uninit(&source);
      Check(configured, "Unable to configure the input audio converter");
      input_resampler_.reset(raw);
      Check(swr_init(input_resampler_.get()), "Unable to start the input audio converter");
      return;
    }
    if (rate != input_rate_ || channels != input_channels_) {
      throw std::invalid_argument("Audio format changed during the export: the first block was " +
                                  std::to_string(input_rate_) + " Hz with " + std::to_string(input_channels_) +
                                  " channel(s), this one is " + std::to_string(rate) + " Hz with " +
                                  std::to_string(channels));
    }
  }

  // Converts `frames` input samples (or, with a null input, drains the
  // converter's delay) and appends the result to the per-channel queues.
  void ConvertInto(const std::uint8_t** input, int frames) {
    const auto target_channels = static_cast<int>(pending_.size());
    const auto capacity = swr_get_out_samples(input_resampler_.get(), frames) + 32;
    std::vector<std::vector<float>> converted(static_cast<std::size_t>(target_channels),
                                              std::vector<float>(static_cast<std::size_t>(capacity)));
    std::vector<std::uint8_t*> outputs(static_cast<std::size_t>(target_channels));
    for (int channel = 0; channel < target_channels; ++channel) {
      outputs[static_cast<std::size_t>(channel)] =
          reinterpret_cast<std::uint8_t*>(converted[static_cast<std::size_t>(channel)].data());
    }
    const auto produced = swr_convert(input_resampler_.get(), outputs.data(), capacity, input, frames);
    Check(produced, "Unable to convert audio for export");
    for (int channel = 0; channel < target_channels; ++channel) {
      auto& queue = pending_[static_cast<std::size_t>(channel)];
      const auto& block = converted[static_cast<std::size_t>(channel)];
      queue.insert(queue.end(), block.begin(), block.begin() + produced);
    }
    pending_frames_ += produced;
  }

  void FlushInputResampler() {
    if (input_resampler_ == nullptr) return;
    // A null input asks the converter for what it is still holding back.
    ConvertInto(nullptr, 0);
  }

  // Encodes whole audio frames out of the pending queues. `flush` allows one
  // final short frame.
  void DrainAudio(bool flush) {
    const auto channels = audio_->ch_layout.nb_channels;
    while (pending_frames_ >= audio_frame_size_ || (flush && pending_frames_ > 0)) {
      const auto take = std::min<std::int64_t>(pending_frames_, audio_frame_size_);
      Check(av_frame_make_writable(audio_frame_.get()), "Unable to prepare an export audio frame");
      audio_frame_->nb_samples = static_cast<int>(take);

      std::vector<const std::uint8_t*> input(static_cast<std::size_t>(channels));
      for (int channel = 0; channel < channels; ++channel) {
        input[static_cast<std::size_t>(channel)] =
            reinterpret_cast<const std::uint8_t*>(pending_[static_cast<std::size_t>(channel)].data());
      }
      const auto produced = swr_convert(resampler_.get(), audio_frame_->data, static_cast<int>(take), input.data(),
                                        static_cast<int>(take));
      Check(produced, "Unable to convert audio for export");
      audio_frame_->nb_samples = produced;
      audio_frame_->pts = audio_frames_;
      if (produced > 0) Encode(audio_.get(), audio_frame_.get(), audio_stream_);
      audio_frames_ += produced;

      // Drop what was consumed from the front of every queue.
      for (auto& queue : pending_) queue.erase(queue.begin(), queue.begin() + take);
      pending_frames_ -= take;
      if (flush && pending_frames_ == 0) break;
    }
  }

  void Encode(AVCodecContext* encoder, AVFrame* frame, AVStream* stream) {
    const auto sent = avcodec_send_frame(encoder, frame);
    if (sent < 0 && sent != AVERROR_EOF) Check(sent, "Unable to queue a frame for encoding");
    while (true) {
      const auto received = avcodec_receive_packet(encoder, packet_.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return;
      Check(received, "Encoding failed");
      av_packet_rescale_ts(packet_.get(), encoder->time_base, stream->time_base);
      packet_->stream_index = stream->index;
      // Interleaved so the muxer orders streams by timestamp rather than by the
      // order we happened to produce them in.
      Check(av_interleaved_write_frame(format_.get(), packet_.get()), "Unable to write a packet");
      av_packet_unref(packet_.get());
    }
  }

  ExportSettings settings_;
  FormatContext format_;
  CodecContext video_;
  CodecContext audio_;
  AVStream* video_stream_{nullptr};
  AVStream* audio_stream_{nullptr};
  Frame video_frame_;
  Frame audio_frame_;
  Packet packet_;
  SwsScaler scaler_;
  SwrResampler resampler_;
  AVPixelFormat scaler_input_{AV_PIX_FMT_NONE};
  int scaler_width_{0};
  int scaler_height_{0};

  // One queue per channel; see WriteAudio.
  std::vector<std::vector<float>> pending_;
  std::int64_t pending_frames_{0};
  std::int64_t audio_frame_size_{1024};
  std::int64_t video_frames_{0};
  std::int64_t audio_frames_{0};
  // Publishing state. `published_` is set only after the temporary has been
  // moved into place; `failed_` after any error while finishing.
  std::string temp_path_;
  bool published_{false};
  bool failed_{false};

  // Converts delivered audio to the encoder's rate and layout, built on the
  // first block once its format is known.
  SwrResampler input_resampler_;
  std::int64_t input_rate_{0};
  int input_channels_{0};
};

class FFmpegWriterProvider final : public WriterProvider {
 public:
  [[nodiscard]] std::string name() const override { return "ffmpeg"; }

  [[nodiscard]] bool CanWrite(const ExportSettings& settings) const override {
    if (settings.path.empty()) return false;
    // Whether the container and codecs are usable is decided on open, where the
    // failure can say which part was unsupported.
    return true;
  }

  [[nodiscard]] std::unique_ptr<Writer> Open(const ExportSettings& settings) override {
    return std::make_unique<FFmpegWriter>(settings);
  }
};

}  // namespace

std::vector<EncoderInfo> ListEncoders() {
  std::vector<EncoderInfo> encoders;
  void* iterator = nullptr;
  while (const auto* codec = av_codec_iterate(&iterator)) {
    if (av_codec_is_encoder(codec) == 0) continue;
    if (codec->type != AVMEDIA_TYPE_VIDEO && codec->type != AVMEDIA_TYPE_AUDIO) continue;
    EncoderInfo info;
    info.name = codec->name;
    info.description = codec->long_name != nullptr ? codec->long_name : "";
    info.video = codec->type == AVMEDIA_TYPE_VIDEO;
    info.hardware = (codec->capabilities & AV_CODEC_CAP_HARDWARE) != 0 || info.name.find("_amf") != std::string::npos ||
                    info.name.find("_nvenc") != std::string::npos || info.name.find("_qsv") != std::string::npos ||
                    info.name.find("_vaapi") != std::string::npos || info.name.find("_videotoolbox") != std::string::npos ||
                    info.name.find("_mf") != std::string::npos;
    encoders.push_back(std::move(info));
  }
  std::sort(encoders.begin(), encoders.end(), [](const EncoderInfo& a, const EncoderInfo& b) { return a.name < b.name; });
  return encoders;
}

bool TestEncoder(const std::string& codec_name, const std::string& pixel_format, std::string* why, const std::map<std::string, std::string>& options) {
  const auto fail = [&](const std::string& text) {
    if (why != nullptr) *why = text;
    return false;
  };
  const auto* codec = avcodec_find_encoder_by_name(codec_name.c_str());
  if (codec == nullptr) return fail("this build has no encoder named " + codec_name);
  CodecContext context(avcodec_alloc_context3(codec));
  if (context == nullptr) return fail("out of memory");
  if (codec->type == AVMEDIA_TYPE_AUDIO) {
    // Sound: stereo at 48 kHz in the first sample format the encoder offers.
    context->sample_rate = 48000;
    av_channel_layout_default(&context->ch_layout, 2);
    context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    const enum AVSampleFormat* formats = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(context.get(), codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, reinterpret_cast<const void**>(&formats), &count) >= 0 && formats != nullptr && count > 0) {
      context->sample_fmt = formats[0];
    }
    context->time_base = AVRational{1, 48000};
    context->bit_rate = 128000;
  } else {
    const auto format = av_get_pix_fmt(pixel_format.c_str());
    if (format == AV_PIX_FMT_NONE) return fail("unknown pixel format " + pixel_format);
    // Large enough for encoders that refuse small pictures (a hardware HEVC encoder will not start below about 144 lines).
    context->width = 1280;
    context->height = 720;
    context->time_base = AVRational{1, 25};
    context->framerate = AVRational{25, 1};
    context->pix_fmt = format;
    context->bit_rate = 2000000;
  }
  // Keep FFmpeg's own error reports (a missing driver prints one) out of the host application's output.
  const auto previous = av_log_get_level();
  av_log_set_level(AV_LOG_QUIET);
  AVDictionary* private_options = nullptr;
  for (const auto& [key, value] : options) {
    // The test picture has a data rate of its own; limits chosen for the real delivery would contradict it.
    if (key == "maxrate" || key == "minrate" || key == "bufsize") continue;
    av_dict_set(&private_options, key.c_str(), value.c_str(), 0);
  }
  const auto opened = avcodec_open2(context.get(), codec, &private_options);
  av_dict_free(&private_options);
  av_log_set_level(previous);
  if (opened < 0) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(opened, text.data(), text.size());
    return fail(std::string("the encoder would not start (") + text.data() + ")");
  }
  return true;
}

void RegisterFFmpegWriter() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  WriterRegistry::Instance().Register(std::make_unique<FFmpegWriterProvider>());
}

}  // namespace cutline::media

#endif  // CUTLINE_HAVE_FFMPEG

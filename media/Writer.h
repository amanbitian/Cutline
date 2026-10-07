#pragma once

// Media writing: the mirror of media::Source.
//
// Same arrangement and for the same reasons. Nothing above this line names
// FFmpeg, so a hardware encoder can be added later as a peer, and a build
// without FFmpeg still compiles -- it simply cannot write files.
//
// A Writer is fed in presentation order and owns no timeline knowledge. Deciding
// *what* to encode is the export worker's job; this only knows how to put frames
// and samples into a container.

#include "core/model/Types.h"
#include "core/time/RationalTime.h"
#include "media/AudioBuffer.h"
#include "media/VideoFrame.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutline::media {

struct VideoEncoderSettings final {
  // Encoder name as the provider understands it. "ffv1" is lossless, which is
  // what makes an exported file comparable to the monitor frame for frame.
  std::string codec{"ffv1"};
  // Zero means "not stated": the export worker fills these from the sequence.
  // A plausible-looking default here would be worse than none, because a caller
  // that forgot to set the rate would silently deliver at the wrong one.
  std::int64_t width{0};
  std::int64_t height{0};
  time::FrameRate frame_rate{0, 1};
  time::FrameRate pixel_aspect{1, 1};
  // Pixel format to encode into. The compositor works in RGBA; anything else is
  // a conversion the writer performs, and a lossy one for subsampled formats.
  std::string pixel_format{"yuv420p"};
  // Zero means let the encoder choose, which for a lossless codec is correct
  // and for a lossy one means its own default quality.
  std::int64_t bitrate{0};
  // Frames between keyframes. Zero leaves it to the encoder.
  std::int64_t gop_size{0};
  std::string color_primaries{"bt709"};
  std::string color_transfer{"bt709"};
  std::string color_matrix{"bt709"};
  model::ColorRange color_range{model::ColorRange::Limited};
  // Encoder-specific settings by name, as the encoder knows them ("profile", "preset", "usage", "quality", "rc", "qp",
  // "crf"...). An option the encoder does not understand is an error, not silently dropped: a delivery asked for with a
  // setting that did nothing is a wrong delivery.
  std::map<std::string, std::string> options;
};

struct AudioEncoderSettings final {
  std::string codec{"pcm_s16le"};
  // Zero means "not stated", as above: taken from the sequence.
  std::int64_t sample_rate{0};
  int channels{0};
  std::int64_t bitrate{0};
  std::map<std::string, std::string> options;
};

struct ExportSettings final {
  std::string path;
  // What to do when `path` already exists. Refusing is the default: replacing
  // someone's delivery is a decision, and the API should make the caller state
  // it. Either way the file is only touched when the export has completed.
  bool overwrite{false};
  // Container name. Inferred from the file extension when empty.
  std::string container;
  std::optional<VideoEncoderSettings> video;
  std::optional<AudioEncoderSettings> audio;
  // Muxer options ("movflags" = "+faststart" puts an MP4's index at the front so it plays while it downloads).
  std::map<std::string, std::string> container_options;
};

// What the build can encode with, and whether an encoder really works on this machine (a hardware encoder is listed
// whether or not the machine has the hardware).
struct EncoderInfo final {
  std::string name;         // "h264_amf"
  std::string description;  // "AMD AMF H.264 Encoder"
  bool video{true};
  bool hardware{false};
};
// Every encoder the registered writer knows, sorted by name; empty when the build cannot encode.
[[nodiscard]] std::vector<EncoderInfo> ListEncoders();
// Opens the encoder on a small test picture and closes it again. True when it works here: the driver is present, the
// device exists, the pixel format is accepted. `why` says what failed.
[[nodiscard]] bool TestEncoder(const std::string& codec, const std::string& pixel_format, std::string* why = nullptr, const std::map<std::string, std::string>& options = {});

// Writes one output file. Frames and audio blocks arrive in presentation order;
// the writer interleaves them into the container.
class Writer {
 public:
  virtual ~Writer() = default;

  // The frame's presentation_time is ignored: output timestamps come from the
  // frame index and the declared frame rate, so a dropped or duplicated frame
  // is impossible by construction.
  virtual void WriteVideo(const VideoFrame& frame) = 0;
  virtual void WriteAudio(const AudioBuffer& audio) = 0;

  // Flushes the encoders and writes the container trailer. Must be called for
  // the file to be readable; the destructor abandons an unfinished file rather
  // than leaving a half-written one that looks complete.
  virtual void Finish() = 0;

  [[nodiscard]] virtual std::int64_t video_frames_written() const = 0;
  [[nodiscard]] virtual std::int64_t audio_frames_written() const = 0;
};

class WriterProvider {
 public:
  virtual ~WriterProvider() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual bool CanWrite(const ExportSettings& settings) const = 0;
  [[nodiscard]] virtual std::unique_ptr<Writer> Open(const ExportSettings& settings) = 0;
};

class WriterRegistry final {
 public:
  [[nodiscard]] static WriterRegistry& Instance();

  void Register(std::unique_ptr<WriterProvider> provider);
  // Throws when nothing can write these settings, naming what is available, so
  // a build without FFmpeg fails with an explanation rather than a null.
  [[nodiscard]] std::unique_ptr<Writer> Open(const ExportSettings& settings) const;
  [[nodiscard]] std::vector<std::string> ProviderNames() const;
  [[nodiscard]] bool empty() const { return providers_.empty(); }

 private:
  WriterRegistry() = default;
  std::vector<std::unique_ptr<WriterProvider>> providers_;
};

}  // namespace cutline::media

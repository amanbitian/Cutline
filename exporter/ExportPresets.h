#pragma once

// Export presets: what a person chooses ("YouTube 1080p", "ProRes 422 HQ", "WAV 24-bit") turned into the settings the
// export worker needs, for the sequence at hand and the encoders this machine can run.
//
// A preset does not name one encoder. It names a chain: for H.264 the vendor encoders on the graphics card first (when the
// person prefers hardware) and then the software one, and the first that works here is used and reported. "Works here" is
// asked of the encoder itself (media::TestEncoder), because a hardware encoder is listed by FFmpeg whether or not the
// machine has the hardware, and a delivery that fails at the last minute with a driver error is worse than a note saying
// the card was skipped.
//
// Quality is stated as bits per pixel per frame for the rate-controlled codecs, so one preset scales from a phone-sized
// sequence to UHD; for the intra-frame professional codecs the profile fixes the data rate and none is set.

#include "exporter/ExportWorker.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cutline::exporter {

enum class PresetCategory { Web, Broadcast, Mezzanine, Archive, Audio };

[[nodiscard]] const char* ToString(PresetCategory category);

// One way of encoding the picture.
struct EncoderChoice final {
  std::string codec;         // the encoder's name: "h264_amf"
  std::string pixel_format;  // what it is fed: "nv12", "yuv422p10le"
  std::map<std::string, std::string> options;
  bool hardware{false};
  // The smallest picture it will take (a hardware HEVC encoder refuses very small ones); zero for no limit.
  std::int64_t minimum_width{0};
  std::int64_t minimum_height{0};
};

struct AudioChoice final {
  std::string codec;  // "aac", "pcm_s24le"
  std::int64_t bitrate{0};
  // Zero follows the sequence; otherwise the preset fixes it (broadcast delivery is 48 kHz).
  std::int64_t sample_rate{0};
  int channels{0};
  int max_channels{0};  // 0 = no limit (MP3 takes two)
  std::map<std::string, std::string> options;
  std::string stream_codec;  // what a file with this audio reports ("aac"), for the check after the export
};

struct ExportPreset final {
  std::string id;           // "web.h264.high"
  std::string name;         // "H.264, high quality"
  std::string description;  // one line for the dialog
  PresetCategory category{PresetCategory::Web};
  std::string extension;  // "mp4", no dot
  std::string container;  // empty: from the extension
  std::map<std::string, std::string> container_options;

  bool has_video{true};
  std::vector<EncoderChoice> software;   // tried in order
  std::vector<EncoderChoice> hardware;   // tried in order
  std::string video_stream_codec;        // what a file with this picture reports ("h264"), for the check after the export
  // Rate-controlled codecs: data rate = width x height x frame rate x bits_per_pixel, clamped. Zero: the profile decides.
  double bits_per_pixel{0.0};
  std::int64_t minimum_bitrate{0};
  std::int64_t maximum_bitrate{0};
  std::int64_t fixed_bitrate{0};  // overrides the formula (broadcast at 50 Mbit/s)
  // False for the intra-frame professional codecs, whose profile fixes the data rate: none is passed to the encoder and
  // `bits_per_pixel` is only the figure used to estimate the size of the file.
  bool pass_bitrate{true};
  double keyframe_seconds{0.0};   // 0 leaves it to the encoder
  bool needs_even_width{true};
  bool needs_even_height{true};
  bool high_precision{false};  // the picture is rendered with more than 8 bits (the encoder keeps them)
  model::ColorRange color_range{model::ColorRange::Limited};
  // Largest picture the format allows; zero for no limit.
  std::int64_t maximum_width{0};
  std::int64_t maximum_height{0};

  bool has_audio{true};
  AudioChoice audio;
};

class PresetCatalogue final {
 public:
  [[nodiscard]] static const std::vector<ExportPreset>& BuiltIn();
  [[nodiscard]] static const ExportPreset* Find(const std::string& id);
  [[nodiscard]] static std::vector<const ExportPreset*> InCategory(PresetCategory category);
};

// Whether an encoder works on this machine. The default asks the encoder to open (once per encoder and pixel format).
using EncoderAvailability = std::function<bool(const std::string& codec, const std::string& pixel_format, const std::map<std::string, std::string>& options, std::string* why)>;
[[nodiscard]] EncoderAvailability DefaultAvailability();

struct SequenceFacts final {
  std::int64_t width{0};
  std::int64_t height{0};
  time::FrameRate frame_rate{25, 1};
  std::int64_t sample_rate{48000};
  int channels{2};
  time::RationalTime duration;  // of what is exported (the range, or the whole sequence)
  time::FrameRate pixel_aspect{1, 1};
};

struct ResolveOptions final {
  std::string output_path;
  bool overwrite{false};
  bool prefer_hardware{true};
  time::RationalTime in{0, 1};
  time::RationalTime out{0, 1};  // zero: to the end
  EncoderAvailability availability;  // empty: DefaultAvailability()
};

struct ResolvedExport final {
  bool ok{false};
  std::string refusal;   // why not, when !ok
  ExportRequest request; // ready for Export()
  std::string encoder;   // what will encode the picture ("h264_amf"), empty for audio only
  bool hardware{false};
  std::string audio_encoder;
  std::string output_path;  // after the extension was corrected
  std::string video_stream_codec;
  std::string audio_stream_codec;
  std::vector<std::string> notes;   // what was decided on the person's behalf, and which encoders were skipped and why
  std::int64_t estimated_bytes{0};
};

// The settings for exporting this sequence with this preset, or why it cannot be done.
[[nodiscard]] ResolvedExport ResolvePreset(const ExportPreset& preset, const SequenceFacts& sequence, const ResolveOptions& options);

}  // namespace cutline::exporter

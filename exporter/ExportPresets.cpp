#include "exporter/ExportPresets.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>

namespace cutline::exporter {
namespace {

using Options = std::map<std::string, std::string>;

EncoderChoice Soft(std::string codec, std::string pixel_format, Options options = {}) {
  return {std::move(codec), std::move(pixel_format), std::move(options), false};
}
EncoderChoice Hard(std::string codec, std::string pixel_format, Options options = {}) {
  return {std::move(codec), std::move(pixel_format), std::move(options), true};
}

// The hardware encoders a rate-controlled codec can be had from, as each vendor names its settings. Quality is asked for
// over speed: this is a delivery, not a preview.
std::vector<EncoderChoice> VendorEncoders(const std::string& family, const std::string& pixel_format) {
  auto amf = Hard(family + "_amf", pixel_format, {{"usage", "transcoding"}, {"quality", "quality"}, {"rc", "vbr_peak"}});
  if (family == "hevc") {
    // Measured: AMD's HEVC encoder will not start at 192x108 though it starts at 1280x720 and above.
    amf.minimum_width = 128;
    amf.minimum_height = 128;
  }
  return {Hard(family + "_nvenc", pixel_format, {{"preset", "p5"}, {"tune", "hq"}, {"rc", "vbr"}}), std::move(amf), Hard(family + "_qsv", pixel_format, {{"preset", "slower"}})};
}

AudioChoice Aac(std::int64_t bitrate) {
  AudioChoice audio;
  audio.codec = "aac";
  audio.bitrate = bitrate;
  audio.stream_codec = "aac";
  return audio;
}

AudioChoice Pcm24(std::int64_t rate = 0) {
  AudioChoice audio;
  audio.codec = "pcm_s24le";
  audio.sample_rate = rate;
  audio.stream_codec = "pcm_s24le";
  return audio;
}

ExportPreset WebH264(std::string id, std::string name, std::string description, double bits_per_pixel) {
  ExportPreset preset;
  preset.id = std::move(id);
  preset.name = std::move(name);
  preset.description = std::move(description);
  preset.category = PresetCategory::Web;
  preset.extension = "mp4";
  preset.container_options = {{"movflags", "+faststart"}};
  preset.software = {Soft("libopenh264", "yuv420p")};
  preset.hardware = VendorEncoders("h264", "nv12");
  preset.video_stream_codec = "h264";
  preset.bits_per_pixel = bits_per_pixel;
  preset.minimum_bitrate = 1000000;
  preset.maximum_bitrate = 80000000;
  preset.keyframe_seconds = 2.0;
  preset.maximum_width = 4096;
  preset.maximum_height = 4096;
  preset.audio = Aac(256000);
  return preset;
}

ExportPreset ProRes(std::string id, std::string name, std::string description, std::string profile, std::string pixel_format, double bits_per_pixel) {
  ExportPreset preset;
  preset.id = std::move(id);
  preset.name = std::move(name);
  preset.description = std::move(description);
  preset.category = PresetCategory::Mezzanine;
  preset.extension = "mov";
  preset.software = {Soft("prores_ks", std::move(pixel_format), {{"profile", std::move(profile)}, {"vendor", "apl0"}})};
  preset.video_stream_codec = "prores";
  preset.bits_per_pixel = bits_per_pixel;  // for the size estimate only
  preset.pass_bitrate = false;
  preset.high_precision = true;
  preset.audio = Pcm24();
  return preset;
}

ExportPreset Dnxhr(std::string id, std::string name, std::string description, std::string profile, std::string pixel_format, double bits_per_pixel) {
  ExportPreset preset;
  preset.id = std::move(id);
  preset.name = std::move(name);
  preset.description = std::move(description);
  preset.category = PresetCategory::Mezzanine;
  preset.extension = "mov";
  preset.software = {Soft("dnxhd", std::move(pixel_format), {{"profile", std::move(profile)}})};
  preset.software[0].minimum_width = 256;   // FFmpeg: "input must be at least 256x120"
  preset.software[0].minimum_height = 120;
  preset.video_stream_codec = "dnxhd";
  preset.bits_per_pixel = bits_per_pixel;
  preset.pass_bitrate = false;
  preset.high_precision = true;
  preset.audio = Pcm24();
  return preset;
}

ExportPreset AudioOnly(std::string id, std::string name, std::string description, std::string extension, AudioChoice audio) {
  ExportPreset preset;
  preset.id = std::move(id);
  preset.name = std::move(name);
  preset.description = std::move(description);
  preset.category = PresetCategory::Audio;
  preset.extension = std::move(extension);
  preset.has_video = false;
  preset.audio = std::move(audio);
  return preset;
}

std::vector<ExportPreset> MakeCatalogue() {
  std::vector<ExportPreset> presets;

  // ---- web and delivery
  presets.push_back(WebH264("web.h264.high", "H.264, high quality (MP4)", "For YouTube, Vimeo and most players. Uses the graphics card's encoder when there is one.", 0.12));
  presets.push_back(WebH264("web.h264.balanced", "H.264, balanced (MP4)", "Smaller files for sharing and review; visibly softer on fast motion.", 0.07));
  {
    ExportPreset hevc = WebH264("web.hevc", "H.265 / HEVC (MP4)", "Half the size of H.264 at the same look; needs a graphics-card encoder (this build has no software one).", 0.06);
    hevc.software.clear();
    hevc.hardware = VendorEncoders("hevc", "nv12");
    hevc.video_stream_codec = "hevc";
    hevc.maximum_width = 8192;
    hevc.maximum_height = 4320;
    presets.push_back(std::move(hevc));
  }
  {
    ExportPreset hevc10 = WebH264("web.hevc.10bit", "H.265 / HEVC 10-bit (MP4)", "For graded footage and HDR-ready delivery; needs a graphics-card encoder with 10-bit support.", 0.07);
    hevc10.software.clear();
    hevc10.hardware = {Hard("hevc_nvenc", "p010le", {{"preset", "p5"}, {"tune", "hq"}, {"rc", "vbr"}, {"profile", "main10"}}),
                       Hard("hevc_amf", "p010le", {{"usage", "transcoding"}, {"quality", "quality"}, {"rc", "vbr_peak"}}),
                       Hard("hevc_qsv", "p010le", {{"preset", "slower"}})};
    hevc10.video_stream_codec = "hevc";
    hevc10.high_precision = true;
    hevc10.maximum_width = 8192;
    hevc10.maximum_height = 4320;
    presets.push_back(std::move(hevc10));
  }
  {
    ExportPreset av1 = WebH264("web.av1", "AV1 (MP4)", "The smallest files for the same look; software encoding is slow, graphics-card encoders are fast.", 0.05);
    av1.software = {Soft("libsvtav1", "yuv420p", {{"preset", "8"}})};
    av1.hardware = VendorEncoders("av1", "nv12");
    av1.video_stream_codec = "av1";
    av1.maximum_width = 8192;
    av1.maximum_height = 4320;
    presets.push_back(std::move(av1));
  }
  {
    ExportPreset vp9;
    vp9.id = "web.vp9";
    vp9.name = "VP9 and Opus (WebM)";
    vp9.description = "Open formats for the web; encoding is slow.";
    vp9.category = PresetCategory::Web;
    vp9.extension = "webm";
    vp9.software = {Soft("libvpx-vp9", "yuv420p", {{"deadline", "good"}, {"cpu-used", "4"}, {"row-mt", "1"}})};
    vp9.video_stream_codec = "vp9";
    vp9.bits_per_pixel = 0.07;
    vp9.minimum_bitrate = 1000000;
    vp9.maximum_bitrate = 60000000;
    vp9.keyframe_seconds = 3.0;
    vp9.audio.codec = "libopus";
    vp9.audio.bitrate = 160000;
    vp9.audio.sample_rate = 48000;
    vp9.audio.stream_codec = "opus";
    presets.push_back(std::move(vp9));
  }

  // ---- broadcast
  {
    ExportPreset xdcam;
    xdcam.id = "broadcast.xdcam422";
    xdcam.name = "MPEG-2 4:2:2 50 Mbit/s (MXF)";
    xdcam.description = "A broadcast-style MXF file: 8-bit 4:2:2 MPEG-2 at 50 Mbit/s with 24-bit 48 kHz PCM. Not a certified XDCAM HD422 file.";
    xdcam.category = PresetCategory::Broadcast;
    xdcam.extension = "mxf";
    xdcam.software = {Soft("mpeg2video", "yuv422p", {{"maxrate", "50000000"}, {"minrate", "50000000"}, {"bufsize", "17825792"}, {"bf", "2"}})};
    xdcam.video_stream_codec = "mpeg2video";
    xdcam.fixed_bitrate = 50000000;
    xdcam.keyframe_seconds = 0.5;
    xdcam.maximum_width = 1920;
    xdcam.maximum_height = 1080;
    xdcam.audio = Pcm24(48000);
    xdcam.audio.max_channels = 8;
    presets.push_back(std::move(xdcam));
  }

  // ---- mezzanine: editing and finishing codecs
  presets.push_back(ProRes("mezzanine.prores.proxy", "ProRes 422 Proxy (MOV)", "Small editing copies of full-size footage.", "proxy", "yuv422p10le", 1.0));
  presets.push_back(ProRes("mezzanine.prores.lt", "ProRes 422 LT (MOV)", "Lighter than Standard at close to the same look.", "lt", "yuv422p10le", 2.3));
  presets.push_back(ProRes("mezzanine.prores.standard", "ProRes 422 (MOV)", "The everyday mastering and exchange format.", "standard", "yuv422p10le", 3.4));
  presets.push_back(ProRes("mezzanine.prores.hq", "ProRes 422 HQ (MOV)", "Higher quality for finishing and delivery masters.", "hq", "yuv422p10le", 5.2));
  presets.push_back(ProRes("mezzanine.prores.4444", "ProRes 4444 (MOV)", "Full-colour 4:4:4 masters.", "4444", "yuv444p10le", 7.8));
  presets.push_back(Dnxhr("mezzanine.dnxhr.sq", "DNxHR SQ (MOV)", "Resolution-independent 8-bit 4:2:2 for offline and mastering.", "dnxhr_sq", "yuv422p", 2.8));
  presets.push_back(Dnxhr("mezzanine.dnxhr.hq", "DNxHR HQ (MOV)", "Higher quality 8-bit 4:2:2.", "dnxhr_hq", "yuv422p", 4.5));
  presets.push_back(Dnxhr("mezzanine.dnxhr.hqx", "DNxHR HQX (MOV)", "10-bit 4:2:2 for finishing.", "dnxhr_hqx", "yuv422p10le", 5.6));
  presets.push_back(Dnxhr("mezzanine.dnxhr.444", "DNxHR 444 (MOV)", "10-bit 4:4:4 masters.", "dnxhr_444", "yuv444p10le", 11.0));

  // ---- archive
  {
    ExportPreset ffv1;
    ffv1.id = "archive.ffv1";
    ffv1.name = "FFV1 lossless 10-bit (MKV)";
    ffv1.description = "Mathematically lossless: the exact picture that was rendered, at about half the size of uncompressed.";
    ffv1.category = PresetCategory::Archive;
    ffv1.extension = "mkv";
    ffv1.software = {Soft("ffv1", "yuv422p10le", {{"level", "3"}, {"slicecrc", "1"}})};
    ffv1.video_stream_codec = "ffv1";
    ffv1.bits_per_pixel = 8.0;
    ffv1.pass_bitrate = false;
    ffv1.high_precision = true;
    ffv1.audio.codec = "flac";
    ffv1.audio.stream_codec = "flac";
    presets.push_back(std::move(ffv1));
  }

  // ---- audio only
  presets.push_back(AudioOnly("audio.wav24", "WAV 24-bit", "Uncompressed sound at the sequence's rate.", "wav", Pcm24()));
  {
    AudioChoice flac;
    flac.codec = "flac";
    flac.stream_codec = "flac";
    presets.push_back(AudioOnly("audio.flac", "FLAC (lossless)", "Lossless and about half the size of WAV.", "flac", flac));
  }
  presets.push_back(AudioOnly("audio.aac", "AAC 256 kbit/s (M4A)", "Good for podcasts and phones.", "m4a", Aac(256000)));
  {
    AudioChoice mp3;
    mp3.codec = "libmp3lame";
    mp3.bitrate = 320000;
    mp3.max_channels = 2;
    mp3.stream_codec = "mp3";
    presets.push_back(AudioOnly("audio.mp3", "MP3 320 kbit/s", "Plays everywhere; stereo only.", "mp3", mp3));
  }
  // AMD's HEVC and AV1 encoders will not start on a picture as small as 192x108 (measured), wherever a preset lists them.
  for (auto& preset : presets) {
    for (auto& choice : preset.hardware) {
      if (choice.codec == "hevc_amf" || choice.codec == "av1_amf") choice.minimum_width = choice.minimum_height = 128;
    }
  }
  return presets;
}

std::string LowerExtension(const std::string& path) {
  auto extension = std::filesystem::path(path).extension().string();
  if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

}  // namespace

const char* ToString(PresetCategory category) {
  switch (category) {
    case PresetCategory::Web: return "Web and delivery";
    case PresetCategory::Broadcast: return "Broadcast";
    case PresetCategory::Mezzanine: return "Editing and finishing";
    case PresetCategory::Archive: return "Archive";
    case PresetCategory::Audio: return "Audio only";
  }
  return "";
}

const std::vector<ExportPreset>& PresetCatalogue::BuiltIn() {
  static const std::vector<ExportPreset> presets = MakeCatalogue();
  return presets;
}

const ExportPreset* PresetCatalogue::Find(const std::string& id) {
  for (const auto& preset : BuiltIn()) {
    if (preset.id == id) return &preset;
  }
  return nullptr;
}

std::vector<const ExportPreset*> PresetCatalogue::InCategory(PresetCategory category) {
  std::vector<const ExportPreset*> found;
  for (const auto& preset : BuiltIn()) {
    if (preset.category == category) found.push_back(&preset);
  }
  return found;
}

EncoderAvailability DefaultAvailability() {
  return [](const std::string& codec, const std::string& pixel_format, const std::map<std::string, std::string>& options, std::string* why) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<bool, std::string>> known;
    const std::lock_guard<std::mutex> lock(mutex);
    std::string key = codec + "|" + pixel_format;
    for (const auto& [name, value] : options) key += "|" + name + "=" + value;
    auto found = known.find(key);
    if (found == known.end()) {
      std::string reason;
      const bool works = media::TestEncoder(codec, pixel_format, &reason, options);
      found = known.emplace(key, std::pair{works, reason}).first;
    }
    if (!found->second.first && why != nullptr) *why = found->second.second;
    return found->second.first;
  };
}

ResolvedExport ResolvePreset(const ExportPreset& preset, const SequenceFacts& sequence, const ResolveOptions& options) {
  ResolvedExport result;
  const auto refuse = [&](std::string why) {
    result.ok = false;
    result.refusal = std::move(why);
    return result;
  };
  if (options.output_path.empty()) return refuse("Choose where to save the file");
  if (preset.has_video) {
    if (sequence.width <= 0 || sequence.height <= 0) return refuse("The sequence has no picture size");
    if (preset.needs_even_width && sequence.width % 2 != 0) return refuse(preset.name + " needs an even picture width; the sequence is " + std::to_string(sequence.width) + " wide");
    if (preset.needs_even_height && sequence.height % 2 != 0) return refuse(preset.name + " needs an even picture height; the sequence is " + std::to_string(sequence.height) + " high");
    if ((preset.maximum_width > 0 && sequence.width > preset.maximum_width) || (preset.maximum_height > 0 && sequence.height > preset.maximum_height)) {
      return refuse(preset.name + " allows at most " + std::to_string(preset.maximum_width) + "x" + std::to_string(preset.maximum_height) + "; the sequence is " +
                    std::to_string(sequence.width) + "x" + std::to_string(sequence.height));
    }
    if (sequence.frame_rate.numerator <= 0 || sequence.frame_rate.denominator <= 0) return refuse("The sequence frame rate is invalid");
  }
  if (preset.has_audio && (sequence.sample_rate <= 0 || sequence.channels <= 0)) return refuse("The sequence has no audio format");

  const auto availability = options.availability ? options.availability : DefaultAvailability();

  media::VideoEncoderSettings video;
  EncoderChoice chosen;
  if (preset.has_video) {
    std::vector<const EncoderChoice*> order;
    const auto add = [&](const std::vector<EncoderChoice>& list) {
      for (const auto& choice : list) order.push_back(&choice);
    };
    if (options.prefer_hardware) {
      add(preset.hardware);
      add(preset.software);
    } else {
      add(preset.software);
      add(preset.hardware);  // only reached when nothing in software works (HEVC in this build)
    }
    const EncoderChoice* picked = nullptr;
    std::vector<std::string> skipped;
    for (const auto* candidate : order) {
      std::string why;
      if (sequence.width < candidate->minimum_width || sequence.height < candidate->minimum_height) {
        skipped.push_back(candidate->codec + ": needs a picture of at least " + std::to_string(candidate->minimum_width) + "x" + std::to_string(candidate->minimum_height));
        continue;
      }
      if (availability(candidate->codec, candidate->pixel_format, candidate->options, &why)) {
        picked = candidate;
        break;
      }
      skipped.push_back(candidate->codec + ": " + (why.empty() ? "not available" : why));
    }
    if (picked == nullptr) {
      std::string text = "No encoder for " + preset.name + " works on this machine";
      for (const auto& line : skipped) text += "; " + line;
      return refuse(text);
    }
    chosen = *picked;
    // What was passed over, so a missing graphics card is a note and not a surprise.
    for (const auto& line : skipped) result.notes.push_back("Skipped " + line);
    if (picked->hardware && !options.prefer_hardware) result.notes.push_back("Used the " + picked->codec + " hardware encoder because no software encoder is available for this format");

    video.codec = chosen.codec;
    video.width = sequence.width;
    video.height = sequence.height;
    video.frame_rate = sequence.frame_rate;
    video.pixel_aspect = sequence.pixel_aspect;
    video.pixel_format = chosen.pixel_format;
    video.options = chosen.options;
    video.color_range = preset.color_range;
    const double fps = static_cast<double>(sequence.frame_rate.numerator) / static_cast<double>(sequence.frame_rate.denominator);
    const double pixels_per_second = static_cast<double>(sequence.width) * static_cast<double>(sequence.height) * fps;
    std::int64_t bitrate = preset.fixed_bitrate;
    if (bitrate == 0 && preset.bits_per_pixel > 0.0) {
      bitrate = static_cast<std::int64_t>(std::llround(pixels_per_second * preset.bits_per_pixel));
      if (preset.minimum_bitrate > 0) bitrate = std::max(bitrate, preset.minimum_bitrate);
      if (preset.maximum_bitrate > 0) bitrate = std::min(bitrate, preset.maximum_bitrate);
    }
    if (preset.pass_bitrate && bitrate > 0) video.bitrate = bitrate;
    if (preset.keyframe_seconds > 0.0) video.gop_size = std::max<std::int64_t>(1, std::llround(fps * preset.keyframe_seconds));
    result.encoder = chosen.codec;
    result.hardware = chosen.hardware;
    result.video_stream_codec = preset.video_stream_codec;
    const double seconds = static_cast<double>(sequence.duration.numerator()) / static_cast<double>(std::max<std::int64_t>(1, sequence.duration.denominator()));
    result.estimated_bytes = static_cast<std::int64_t>(static_cast<double>(bitrate > 0 ? bitrate : static_cast<std::int64_t>(pixels_per_second * preset.bits_per_pixel)) * seconds / 8.0);
  }

  media::AudioEncoderSettings audio;
  if (preset.has_audio) {
    // An audio encoder takes no pixel format, and TestEncoder knows to open it as sound.
    std::string why;
    if (!availability(preset.audio.codec, "", preset.audio.options, &why)) return refuse("The " + preset.audio.codec + " audio encoder does not work on this machine (" + why + ")");
    audio.codec = preset.audio.codec;
    audio.sample_rate = preset.audio.sample_rate > 0 ? preset.audio.sample_rate : sequence.sample_rate;
    audio.channels = preset.audio.channels > 0 ? preset.audio.channels : sequence.channels;
    if (preset.audio.max_channels > 0 && audio.channels > preset.audio.max_channels) {
      result.notes.push_back("Mixed " + std::to_string(audio.channels) + " channels down to " + std::to_string(preset.audio.max_channels) + " for " + preset.name);
      audio.channels = preset.audio.max_channels;
    }
    audio.bitrate = preset.audio.bitrate;
    audio.options = preset.audio.options;
    result.audio_encoder = audio.codec;
    result.audio_stream_codec = preset.audio.stream_codec;
    const double seconds = static_cast<double>(sequence.duration.numerator()) / static_cast<double>(std::max<std::int64_t>(1, sequence.duration.denominator()));
    const double audio_bits = audio.bitrate > 0 ? static_cast<double>(audio.bitrate) : static_cast<double>(audio.sample_rate) * audio.channels * 24.0 * (audio.codec == "flac" ? 0.55 : 1.0);
    result.estimated_bytes += static_cast<std::int64_t>(audio_bits * seconds / 8.0);
  }

  // The file name carries the format: a preset that writes MOV into a name ending .mp4 would be a file that lies.
  auto path = options.output_path;
  if (LowerExtension(path) != preset.extension) {
    const auto replaced = std::filesystem::path(path).replace_extension("." + preset.extension).string();
    result.notes.push_back("Named the file " + std::filesystem::path(replaced).filename().string() + " because " + preset.name + " writes ." + preset.extension + " files");
    path = replaced;
  }

  ExportRequest request;
  request.output_path = path;
  request.overwrite = options.overwrite;
  request.in = options.in;
  request.out = options.out;
  request.include_video = preset.has_video;
  request.include_audio = preset.has_audio;
  request.container = preset.container;
  request.container_options = preset.container_options;
  request.high_precision = preset.high_precision;
  if (preset.has_video) request.video = video;
  if (preset.has_audio) request.audio = audio;
  result.request = std::move(request);
  result.output_path = path;
  result.ok = true;
  return result;
}

}  // namespace cutline::exporter

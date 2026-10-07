#include "media/SyntheticSource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cutline::media {
namespace {

constexpr std::string_view kScheme = "synthetic:";

// The counter is written as a run of 8x8 blocks along the top-left, each block
// holding one bit of the frame index: white for 1, black for 0. Blocks rather
// than single pixels so the value survives a resize or a lossy round trip.
//
// Two further blocks follow, always white then always black. They are a
// signature: without them, reading a counter out of a picture that never had
// one -- colour bars, say -- would return a plausible-looking number instead of
// admitting it does not know.
constexpr int kCounterBlock = 8;
constexpr int kCounterBits = 16;
constexpr int kCounterMarkerBlocks = 2;
constexpr int kCounterTotalBlocks = kCounterBits + kCounterMarkerBlocks;

[[nodiscard]] std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

[[nodiscard]] time::FrameRate ParseRate(const std::string& value) {
  const auto slash = value.find('/');
  if (slash == std::string::npos) {
    return {std::stoll(value), 1};
  }
  return {std::stoll(value.substr(0, slash)), std::stoll(value.substr(slash + 1))};
}

[[nodiscard]] SyntheticPattern ParsePattern(const std::string& value) {
  if (value == "solid") return SyntheticPattern::Solid;
  if (value == "bars") return SyntheticPattern::Bars;
  if (value == "gradient") return SyntheticPattern::Gradient;
  if (value == "counter") return SyntheticPattern::Counter;
  throw std::invalid_argument("Unknown synthetic pattern: " + value);
}

[[nodiscard]] std::string PatternName(SyntheticPattern pattern) {
  switch (pattern) {
    case SyntheticPattern::Solid: return "solid";
    case SyntheticPattern::Bars: return "bars";
    case SyntheticPattern::Gradient: return "gradient";
    case SyntheticPattern::Counter: return "counter";
  }
  throw std::invalid_argument("Unhandled synthetic pattern");
}

// The eight bar colours, in the conventional 100% bars order.
constexpr std::array<std::array<float, 3>, 8> kBarColors{{
    {1.0f, 1.0f, 1.0f},  // white
    {1.0f, 1.0f, 0.0f},  // yellow
    {0.0f, 1.0f, 1.0f},  // cyan
    {0.0f, 1.0f, 0.0f},  // green
    {1.0f, 0.0f, 1.0f},  // magenta
    {1.0f, 0.0f, 0.0f},  // red
    {0.0f, 0.0f, 1.0f},  // blue
    {0.0f, 0.0f, 0.0f},  // black
}};

class SyntheticSource final : public Source {
 public:
  explicit SyntheticSource(SyntheticSpec spec) : spec_(spec) {
    if (spec_.width <= 0 || spec_.height <= 0) throw std::invalid_argument("Synthetic size must be positive");
    if (spec_.frame_rate.numerator <= 0 || spec_.frame_rate.denominator <= 0) {
      throw std::invalid_argument("Synthetic frame rate must be positive");
    }
    if (spec_.duration.Compare({0, 1}) <= 0) throw std::invalid_argument("Synthetic duration must be positive");

    probe_.duration = spec_.duration;
    probe_.container = "synthetic";

    commands::MediaStream video;
    video.stream_index = 0;
    video.kind = model::StreamKind::Video;
    video.codec = "synthetic";
    video.width = spec_.width;
    video.height = spec_.height;
    video.frame_rate = spec_.frame_rate;
    video.cadence = spec_.variable_frame_rate ? model::Cadence::Variable : model::Cadence::Constant;
    video.bit_depth = 8;
    video.color_range = model::ColorRange::Full;
    probe_.streams.push_back(video);

    if (spec_.tone_hz > 0.0) {
      commands::MediaStream audio;
      audio.stream_index = 1;
      audio.kind = model::StreamKind::Audio;
      audio.codec = "synthetic";
      audio.sample_rate = spec_.sample_rate;
      audio.channel_count = spec_.channels;
      audio.channel_layout = spec_.channels == 1 ? "mono" : "stereo";
      probe_.streams.push_back(audio);
    }

    BuildTimestamps();
  }

  [[nodiscard]] const Probe& probe() const override { return probe_; }

  [[nodiscard]] std::optional<VideoFrame> ReadVideo(const time::RationalTime& time) override {
    if (time.Compare({0, 1}) < 0 || time.Compare(spec_.duration) >= 0) return std::nullopt;
    // Resolve through the timestamp map, exactly as a real decoder does, so the
    // variable-frame-rate case exercises the same path.
    const auto& entry = timestamps_->AtOrBefore(time);
    const auto index = entry.presentation_time.Divide(NominalFrameDuration()).ToFrames({1, 1},
                                                                                       time::RoundingMode::Floor);
    return Render(index, entry.presentation_time, entry.duration);
  }

  [[nodiscard]] std::optional<AudioBuffer> ReadAudio(const time::RationalTime& time, std::int64_t sample_rate,
                                                     int channels, std::int64_t frames) override {
    if (spec_.tone_hz <= 0.0) return std::nullopt;
    if (time.Compare(spec_.duration) >= 0) return std::nullopt;
    auto buffer = AudioBuffer::Allocate(sample_rate, channels, frames);
    buffer.presentation_time = time;

    // Phase is derived from absolute time, not accumulated per block, so two
    // adjacent reads join without a discontinuity.
    const auto start = static_cast<double>(time.numerator()) / static_cast<double>(time.denominator());
    const auto step = 1.0 / static_cast<double>(sample_rate);
    const auto end = static_cast<double>(spec_.duration.numerator()) /
                     static_cast<double>(spec_.duration.denominator());
    for (std::int64_t frame = 0; frame < frames; ++frame) {
      const auto moment = start + static_cast<double>(frame) * step;
      const auto sample = moment < 0.0 || moment >= end
                              ? 0.0f  // outside the source (before it, or past its end): silence, not a repeat
                              : spec_.tone_amplitude *
                                    static_cast<float>(std::sin(2.0 * std::numbers::pi * spec_.tone_hz * moment));
      for (int channel = 0; channel < channels; ++channel) buffer.channel(channel)[frame] = sample;
    }
    return buffer;
  }

  [[nodiscard]] const TimestampMap* timestamps() const override { return timestamps_.get(); }

 private:
  [[nodiscard]] time::RationalTime NominalFrameDuration() const {
    return {spec_.frame_rate.denominator, spec_.frame_rate.numerator};
  }

  void BuildTimestamps() {
    const auto nominal = NominalFrameDuration();
    std::vector<FrameTimestamp> frames;
    time::RationalTime cursor;
    std::int64_t index = 0;
    while (cursor.Compare(spec_.duration) < 0) {
      // A variable-rate source alternates one and two frame intervals, which is
      // enough to make a constant-rate assumption visibly wrong.
      const auto duration = spec_.variable_frame_rate && index % 2 == 1 ? nominal.Multiply(2) : nominal;
      frames.push_back({cursor, duration, index % 12 == 0});
      cursor = cursor.Add(duration);
      ++index;
    }
    timestamps_ = std::make_unique<TimestampMap>(std::move(frames));
  }

  [[nodiscard]] VideoFrame Render(std::int64_t index, const time::RationalTime& presentation,
                                  const time::RationalTime& duration) const {
    auto frame = VideoFrame::Allocate(PixelFormat::Rgba8, spec_.width, spec_.height);
    frame.presentation_time = presentation;
    frame.duration = duration;
    frame.color.range = model::ColorRange::Full;
    frame.keyframe = index % 12 == 0;

    for (int y = 0; y < spec_.height; ++y) {
      auto* row = frame.row_u8(y);
      for (int x = 0; x < spec_.width; ++x) {
        auto* pixel = row + static_cast<std::size_t>(x) * 4;
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        switch (spec_.pattern) {
          case SyntheticPattern::Solid:
            red = spec_.red;
            green = spec_.green;
            blue = spec_.blue;
            break;
          case SyntheticPattern::Bars: {
            const auto bar = static_cast<std::size_t>(x * 8 / spec_.width);
            red = kBarColors[bar][0];
            green = kBarColors[bar][1];
            blue = kBarColors[bar][2];
            break;
          }
          case SyntheticPattern::Gradient: {
            const auto across = spec_.width > 1 ? static_cast<float>(x) / static_cast<float>(spec_.width - 1) : 0.0f;
            const auto down = spec_.height > 1 ? static_cast<float>(y) / static_cast<float>(spec_.height - 1) : 0.0f;
            red = across;
            green = down;
            blue = 0.25f;
            break;
          }
          case SyntheticPattern::Counter:
            // Mid grey background, so the black/white counter blocks stand out
            // and an accidental all-black frame is obvious.
            red = 0.25f;
            green = 0.25f;
            blue = 0.25f;
            break;
        }
        pixel[0] = static_cast<std::uint8_t>(std::clamp(red, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixel[1] = static_cast<std::uint8_t>(std::clamp(green, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixel[2] = static_cast<std::uint8_t>(std::clamp(blue, 0.0f, 1.0f) * 255.0f + 0.5f);
        pixel[3] = 255;
      }
    }

    if (spec_.pattern == SyntheticPattern::Counter) WriteCounter(frame, index);
    return frame;
  }

  static void WriteCounter(VideoFrame& frame, std::int64_t index) {
    for (int bit = 0; bit < kCounterTotalBlocks; ++bit) {
      // The two signature blocks come after the value bits.
      const bool set = bit < kCounterBits ? ((index >> bit) & 1) != 0 : bit == kCounterBits;
      const auto value = static_cast<std::uint8_t>(set ? 255 : 0);
      const auto x0 = bit * kCounterBlock;
      if (x0 + kCounterBlock > frame.width()) break;
      for (int y = 0; y < std::min(kCounterBlock, frame.height()); ++y) {
        auto* row = frame.row_u8(y);
        for (int x = x0; x < x0 + kCounterBlock; ++x) {
          auto* pixel = row + static_cast<std::size_t>(x) * 4;
          pixel[0] = value;
          pixel[1] = value;
          pixel[2] = value;
          pixel[3] = 255;
        }
      }
    }
  }

  SyntheticSpec spec_;
  Probe probe_;
  std::unique_ptr<TimestampMap> timestamps_;
};

class SyntheticProvider final : public SourceProvider {
 public:
  [[nodiscard]] std::string name() const override { return "synthetic"; }

  [[nodiscard]] bool CanOpen(const std::string& path) const override { return path.rfind(kScheme, 0) == 0; }

  [[nodiscard]] std::unique_ptr<Source> Open(const std::string& path) override {
    return std::make_unique<SyntheticSource>(SyntheticSpec::Parse(path));
  }

  [[nodiscard]] Probe ProbeFile(const std::string& path) override {
    return SyntheticSource(SyntheticSpec::Parse(path)).probe();
  }
};

}  // namespace

SyntheticSpec SyntheticSpec::Parse(const std::string& path) {
  if (path.rfind(kScheme, 0) != 0) throw std::invalid_argument("Not a synthetic media path: " + path);
  auto body = path.substr(kScheme.size());

  std::string pattern = body;
  std::string query;
  if (const auto question = body.find('?'); question != std::string::npos) {
    pattern = body.substr(0, question);
    query = body.substr(question + 1);
  }

  SyntheticSpec spec;
  spec.pattern = ParsePattern(Trim(pattern));

  std::unordered_map<std::string, std::string> values;
  std::istringstream stream(query);
  std::string pair;
  while (std::getline(stream, pair, '&')) {
    if (pair.empty()) continue;
    const auto equals = pair.find('=');
    if (equals == std::string::npos) throw std::invalid_argument("Synthetic parameter needs a value: " + pair);
    values.emplace(Trim(pair.substr(0, equals)), Trim(pair.substr(equals + 1)));
  }

  const auto take = [&values](const char* key) -> std::optional<std::string> {
    const auto found = values.find(key);
    if (found == values.end()) return std::nullopt;
    return found->second;
  };

  if (const auto value = take("w")) spec.width = std::stoi(*value);
  if (const auto value = take("h")) spec.height = std::stoi(*value);
  if (const auto value = take("fps")) spec.frame_rate = ParseRate(*value);
  if (const auto value = take("duration")) spec.duration = ParseRate(*value).numerator == 0
                                                               ? spec.duration
                                                               : time::RationalTime(ParseRate(*value).numerator,
                                                                                    ParseRate(*value).denominator);
  if (const auto value = take("r")) spec.red = std::stof(*value);
  if (const auto value = take("g")) spec.green = std::stof(*value);
  if (const auto value = take("b")) spec.blue = std::stof(*value);
  if (const auto value = take("tone")) spec.tone_hz = std::stod(*value);
  if (const auto value = take("amplitude")) spec.tone_amplitude = std::stof(*value);
  if (const auto value = take("rate")) spec.sample_rate = std::stoll(*value);
  if (const auto value = take("channels")) spec.channels = std::stoi(*value);
  if (const auto value = take("vfr")) spec.variable_frame_rate = *value == "1" || *value == "true";
  return spec;
}

std::string SyntheticSpec::ToPath() const {
  std::ostringstream path;
  path << kScheme << PatternName(pattern) << "?w=" << width << "&h=" << height << "&fps=" << frame_rate.numerator
       << "/" << frame_rate.denominator << "&duration=" << duration.numerator() << "/" << duration.denominator();
  if (pattern == SyntheticPattern::Solid) path << "&r=" << red << "&g=" << green << "&b=" << blue;
  if (tone_hz > 0.0) {
    path << "&tone=" << tone_hz << "&amplitude=" << tone_amplitude << "&rate=" << sample_rate
         << "&channels=" << channels;
  }
  if (variable_frame_rate) path << "&vfr=1";
  return path.str();
}

std::int64_t ReadFrameCounter(const VideoFrame& frame) {
  if (frame.format() != PixelFormat::Rgba8 || frame.height() < kCounterBlock) return -1;
  if (frame.width() < kCounterTotalBlocks * kCounterBlock) return -1;

  // Sample the middle of each block, so a resampled or lightly compressed frame
  // still reads back.
  const auto block = [&frame](int index) {
    const auto x = index * kCounterBlock + kCounterBlock / 2;
    return frame.row_u8(kCounterBlock / 2)[static_cast<std::size_t>(x) * 4];
  };

  // Signature first: a picture without it is not a counter frame.
  if (block(kCounterBits) <= 127 || block(kCounterBits + 1) > 127) return -1;

  std::int64_t index = 0;
  for (int bit = 0; bit < kCounterBits; ++bit) {
    if (block(bit) > 127) index |= (std::int64_t{1} << bit);
  }
  return index;
}

std::unique_ptr<Source> OpenSynthetic(const SyntheticSpec& spec) {
  return std::make_unique<SyntheticSource>(spec);
}

void RegisterSyntheticProvider() {
  static bool registered = false;
  if (registered) return;
  registered = true;
  SourceRegistry::Instance().Register(std::make_unique<SyntheticProvider>());
}

}  // namespace cutline::media

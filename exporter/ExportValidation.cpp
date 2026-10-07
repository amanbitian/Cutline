#include "exporter/ExportValidation.h"

#include "media/Source.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace cutline::exporter {
namespace {

double Seconds(const time::RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(std::max<std::int64_t>(1, t.denominator())); }

}  // namespace

ValidationResult ValidateExport(const std::string& path, const ExportExpectation& expected) {
  ValidationResult result;
  const auto problem = [&](std::string text) { result.problems.push_back(std::move(text)); };
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    problem("The file was not created: " + path);
    return result;
  }
  result.bytes = static_cast<std::int64_t>(std::filesystem::file_size(path, error));
  if (result.bytes <= 0) {
    problem("The file is empty");
    return result;
  }

  std::unique_ptr<media::Source> source;
  try {
    source = media::SourceRegistry::Instance().Open(path);
  } catch (const std::exception& failure) {
    problem(std::string("The file cannot be opened: ") + failure.what());
    return result;
  }
  if (source == nullptr) {
    problem("No reader in this build recognises the file");
    return result;
  }
  const auto& probe = source->probe();
  result.duration = probe.duration;
  const auto* video = probe.PrimaryVideo();
  const auto* audio = probe.PrimaryAudio();
  if (video != nullptr) result.video_codec = video->codec;
  if (audio != nullptr) result.audio_codec = audio->codec;

  if (expected.video) {
    if (video == nullptr) {
      problem("The file has no picture");
    } else {
      if (expected.width > 0 && (video->width != expected.width || video->height != expected.height)) {
        problem("The picture is " + std::to_string(video->width) + "x" + std::to_string(video->height) + ", not the " + std::to_string(expected.width) + "x" + std::to_string(expected.height) + " asked for");
      }
      if (expected.frame_rate.numerator > 0 && video->frame_rate.numerator > 0) {
        const double asked = static_cast<double>(expected.frame_rate.numerator) / static_cast<double>(expected.frame_rate.denominator);
        const double found = static_cast<double>(video->frame_rate.numerator) / static_cast<double>(video->frame_rate.denominator);
        if (std::abs(asked - found) > 0.01) problem("The frame rate is " + std::to_string(found) + ", not the " + std::to_string(asked) + " asked for");
      }
      if (!expected.video_codec.empty() && video->codec != expected.video_codec) problem("The picture is coded as " + video->codec + ", not " + expected.video_codec);
    }
  } else if (video != nullptr) {
    problem("The file has a picture although none was asked for");
  }
  if (expected.audio) {
    if (audio == nullptr) {
      problem("The file has no sound");
    } else {
      if (expected.sample_rate > 0 && audio->sample_rate != expected.sample_rate) problem("The sound is " + std::to_string(audio->sample_rate) + " Hz, not the " + std::to_string(expected.sample_rate) + " Hz asked for");
      if (expected.channels > 0 && audio->channel_count != expected.channels) problem("The sound has " + std::to_string(audio->channel_count) + " channels, not the " + std::to_string(expected.channels) + " asked for");
      if (!expected.audio_codec.empty() && audio->codec != expected.audio_codec) problem("The sound is coded as " + audio->codec + ", not " + expected.audio_codec);
    }
  } else if (audio != nullptr) {
    problem("The file has sound although none was asked for");
  }

  if (expected.duration.numerator() > 0) {
    const double difference = std::abs(Seconds(probe.duration) - Seconds(expected.duration));
    if (difference > Seconds(expected.tolerance)) {
      problem("The file is " + std::to_string(Seconds(probe.duration)) + " s long, not the " + std::to_string(Seconds(expected.duration)) + " s that was exported");
    }
  }

  // The first and last pictures decode, and a block of sound; a file whose index is intact but whose tail is truncated
  // passes every check above and fails here.
  try {
    if (expected.video && video != nullptr) {
      const auto first = source->ReadVideo(time::RationalTime(0, 1));
      if (!first.has_value() || !first.value().valid()) problem("The first picture cannot be decoded");
      const auto frame = expected.frame_rate.numerator > 0 ? time::RationalTime(expected.frame_rate.denominator, expected.frame_rate.numerator) : time::RationalTime(1, 25);
      const auto last_time = expected.duration.numerator() > 0 ? expected.duration.Subtract(frame.Multiply(time::RationalTime(3, 2))) : probe.duration.Subtract(frame.Multiply(time::RationalTime(3, 2)));
      if (last_time.numerator() > 0) {
        const auto last = source->ReadVideo(last_time);
        if (!last.has_value() || !last.value().valid()) problem("The last picture cannot be decoded");
      }
    }
    if (expected.audio && audio != nullptr) {
      const auto block = source->ReadAudio(time::RationalTime(0, 1), audio->sample_rate > 0 ? audio->sample_rate : 48000, std::max(1, static_cast<int>(audio->channel_count)), 1024);
      if (!block.has_value() || !block->valid()) problem("The sound cannot be decoded");
    }
  } catch (const std::exception& failure) {
    problem(std::string("Decoding failed: ") + failure.what());
  }
  result.ok = result.problems.empty();
  return result;
}

}  // namespace cutline::exporter

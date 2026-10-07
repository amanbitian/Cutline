#pragma once

// Media source interface.
//
// Everything above this line -- the compositor, the playback engine, the export
// worker -- is written against `Source` and never against FFmpeg. That matters
// for three reasons: hardware decoders slot in later as additional
// implementations, the golden-frame tests run against a deterministic synthetic
// source with no media files on disk, and FFmpeg stays an optional build
// dependency rather than a requirement for the core to compile.

#include "core/commands/Command.h"
#include "core/time/RationalTime.h"
#include "media/AudioBuffer.h"
#include "media/DeviceFrame.h"
#include "media/TimestampMap.h"
#include "media/VideoFrame.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutline::media {

// What a probe found: enough to write the media and media_streams rows, and to
// configure a decode graph.
struct Probe final {
  time::RationalTime duration;
  time::RationalTime start_timecode;
  std::vector<commands::MediaStream> streams;
  std::string container;

  [[nodiscard]] const commands::MediaStream* PrimaryVideo() const;
  [[nodiscard]] const commands::MediaStream* PrimaryAudio() const;
};

// How a source is to be opened beyond its path.
struct OpenOptions final {
  // An ID3D11Device. When set, the source may decode on that device and hand its pictures over still on it
  // (ReadDeviceVideo); a source that cannot says so and ReadVideo still works.
  void* d3d11_device{nullptr};
};

// Opened for reading. What may run at once:
//   * one thread in ReadVideo and one in ReadAudio, together: the picture and the
//     sound of a file are read through separate demuxers and decoders, which is what
//     lets playback decode audio on its own thread without a lock;
//   * timestamps() and probe() from any thread.
// Two threads in the same read method are not supported; a decode pool gives each
// worker its own Source, which is also how seek state stays sane.
class Source {
 public:
  virtual ~Source() = default;

  [[nodiscard]] virtual const Probe& probe() const = 0;

  // Decodes the video frame covering `time`, or nullopt past the end. The
  // returned frame is in an RGBA format; implementations convert.
  [[nodiscard]] virtual std::optional<VideoFrame> ReadVideo(const time::RationalTime& time) = 0;

  // Decodes the video frame covering `time` on the GPU and returns it there, without copying it to memory. Only a source
  // opened with a device (OpenOptions) and able to decode this stream with hardware does; any other returns nothing, and
  // so does one whose hardware decoder fails (it stays failed: use ReadVideo).
  [[nodiscard]] virtual std::optional<DeviceFrame> ReadDeviceVideo(const time::RationalTime&) { return std::nullopt; }
  // Whether ReadDeviceVideo can be expected to work: the source was opened with a device and has not failed.
  [[nodiscard]] virtual bool device_decode_available() const { return false; }

  // Decodes `frames` of audio starting at `time`, resampled to `sample_rate`
  // and `channels`. Short reads at the end of the file are zero-padded so a
  // caller always gets the block length it asked for.
  //
  // `time` is source time: zero is the earliest timestamp in the file, shared
  // by every stream. The result holds the file's signal at exactly the output
  // samples [round(time * rate), +frames), whatever was read before, and silence
  // where the stream has no samples (before it starts, after it ends).
  [[nodiscard]] virtual std::optional<AudioBuffer> ReadAudio(const time::RationalTime& time,
                                                             std::int64_t sample_rate, int channels,
                                                             std::int64_t frames) = 0;

  // Presentation timestamps for the video stream, built as the file is read.
  // This is what makes variable-frame-rate sources land on the right frame
  // instead of being assumed to tick at a constant rate.
  [[nodiscard]] virtual const TimestampMap* timestamps() const = 0;

  // Why timestamps() returned nullptr, when it did: a file whose frames carry no
  // presentation timestamps, or two with the same one, cannot be indexed, and
  // the caller is owed the reason rather than an unexplained absence.
  [[nodiscard]] virtual std::string timestamps_problem() const { return {}; }
};

// Opens a file. Returns nullptr when no registered provider recognises it.
//
// Registration happens at link time: building with FFmpeg adds its provider
// here, and a build without it still compiles and still runs every test that
// uses the synthetic source.
class SourceProvider {
 public:
  virtual ~SourceProvider() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual bool CanOpen(const std::string& path) const = 0;
  [[nodiscard]] virtual std::unique_ptr<Source> Open(const std::string& path) = 0;
  [[nodiscard]] virtual std::unique_ptr<Source> Open(const std::string& path, const OpenOptions&) { return Open(path); }
  [[nodiscard]] virtual Probe ProbeFile(const std::string& path) = 0;
};

// The provider registry. Providers register themselves during static
// initialisation; `Registry()` is deliberately function-local so registration
// order across translation units cannot matter.
class SourceRegistry final {
 public:
  [[nodiscard]] static SourceRegistry& Instance();

  void Register(std::unique_ptr<SourceProvider> provider);
  [[nodiscard]] std::unique_ptr<Source> Open(const std::string& path) const;
  [[nodiscard]] std::unique_ptr<Source> Open(const std::string& path, const OpenOptions& options) const;
  [[nodiscard]] Probe ProbeFile(const std::string& path) const;
  [[nodiscard]] std::vector<std::string> ProviderNames() const;
  [[nodiscard]] bool empty() const { return providers_.empty(); }

 private:
  SourceRegistry() = default;
  std::vector<std::unique_ptr<SourceProvider>> providers_;
};

}  // namespace cutline::media

#pragma once

// Audio output device.
//
// The sink pulls: it asks for a block when the device needs one, from the
// device's own thread, and advances the master clock by exactly the number of
// samples the device accepted. That is the whole reason audio is the clock in
// an NLE -- the sound card's rate is the only one that cannot be adjusted, so
// everything else chases it rather than the other way round.
//
// The render callback runs on a realtime thread. It must not allocate, lock, or
// block; implementations document how they uphold that.

#include "audio/AudioClock.h"
#include "media/AudioBuffer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cutline::audio {

struct AudioDeviceFormat final {
  std::int64_t sample_rate{48000};
  int channels{2};
  // Device buffer size in frames. Smaller means lower latency and a tighter
  // deadline for the render callback.
  std::int64_t block_frames{480};
};

// What a render callback made of a block.
enum class BlockResult {
  Audio,    // the block holds audio to play
  End,      // there is nothing left: the block is silence and the sink should finish
  Dropped,  // the block was made for a position that no longer applies (the
            // transport moved while it was being rendered): discard it unplayed
};

// Fills `buffer` with the next block. Called on the sink's producer thread, never
// on the device's own realtime thread.
// End means "no more audio": the sink stops asking, plays out what it has queued,
// outputs silence after it, and reports SinkState::Finished.
// An exception that escapes the callback is caught by the sink, which reports
// SinkState::Failed with the exception's message; it never reaches the thread
// boundary.
using RenderCallback = std::function<BlockResult(media::AudioBuffer& buffer)>;

enum class SinkState {
  Stopped,   // not started, or stopped
  Running,   // producing and playing
  Finished,  // the callback ran out of audio and everything it made has been played
  Failed,    // the device or the callback failed; see error()
};

class AudioSink {
 public:
  virtual ~AudioSink() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual const AudioDeviceFormat& format() const = 0;
  // True when this sink consumes audio on its own and its clock can lead video
  // playback. OfflineSink is pulled explicitly by tests and renderers, so using
  // its clock as the live transport clock would leave the playhead frozen.
  [[nodiscard]] virtual bool drives_clock() const = 0;

  virtual void Start(RenderCallback callback) = 0;
  virtual void Stop() = 0;
  [[nodiscard]] virtual bool running() const = 0;

  // Throws away everything queued but not yet played and restarts the clock at
  // zero, so that a seek is heard at once instead of after the queue has drained.
  // Returns when it has taken effect. A block the callback is in the middle of
  // rendering when this is called is dropped, not played: the caller is
  // responsible for making the callback produce the new position afterwards.
  virtual void Flush() = 0;

  [[nodiscard]] virtual SinkState state() const = 0;
  // Why the sink is in SinkState::Failed; empty otherwise.
  [[nodiscard]] virtual std::string error() const = 0;

  // The clock advanced by rendered samples. Playback position is read from
  // here, never from a wall clock.
  [[nodiscard]] virtual AudioClock& clock() = 0;

  // Device latency: how far ahead of what is audible the render callback is
  // working. The video path subtracts it so picture and sound line up.
  [[nodiscard]] virtual time::RationalTime latency() const = 0;

  // Frames the device asked for but could not be supplied in time. A non-zero
  // and rising count is the measurable definition of audio dropout.
  [[nodiscard]] virtual std::int64_t underruns() const = 0;
};

// A sink with no device behind it. It renders on demand rather than on a timer,
// which makes it the right sink for export, for headless rendering, and for
// tests: the same code path runs, deterministically and as fast as it can.
class OfflineSink final : public AudioSink {
 public:
  explicit OfflineSink(AudioDeviceFormat format = {}) : format_(format), clock_(format.sample_rate) {}

  [[nodiscard]] std::string name() const override { return "offline"; }
  [[nodiscard]] const AudioDeviceFormat& format() const override { return format_; }
  [[nodiscard]] bool drives_clock() const override { return false; }

  void Start(RenderCallback callback) override;
  void Stop() override;
  [[nodiscard]] bool running() const override { return running_; }
  void Flush() override { clock_.Reset(); }
  [[nodiscard]] SinkState state() const override { return state_.load(); }
  [[nodiscard]] std::string error() const override { return error_; }
  [[nodiscard]] AudioClock& clock() override { return clock_; }
  // Nothing is audible, so nothing is pending.
  [[nodiscard]] time::RationalTime latency() const override { return {}; }
  [[nodiscard]] std::int64_t underruns() const override { return 0; }

  // Pulls one block, exactly as a device would. Returns false once the callback
  // reports it has nothing left.
  [[nodiscard]] bool RenderBlock(media::AudioBuffer& into);
  // Pulls `blocks` blocks and returns how many carried audio.
  [[nodiscard]] int RenderBlocks(int blocks);

 private:
  AudioDeviceFormat format_;
  AudioClock clock_;
  RenderCallback callback_;
  bool running_{false};
  std::atomic<SinkState> state_{SinkState::Stopped};
  std::string error_;
};

// Opens the best available output device, or an OfflineSink when this build has
// no device backend or no device is present. Never returns null: a missing
// sound card degrades to silent playback rather than preventing the editor from
// opening.
[[nodiscard]] std::unique_ptr<AudioSink> OpenDefaultAudioSink(AudioDeviceFormat format = {});

// True when this build contains a real device backend.
[[nodiscard]] bool HasAudioDevice();

}  // namespace cutline::audio

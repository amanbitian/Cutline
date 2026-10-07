#include "audio/AudioSink.h"

#include <stdexcept>
#include <utility>

namespace cutline::audio {

void OfflineSink::Start(RenderCallback callback) {
  if (!callback) throw std::invalid_argument("An audio sink needs a render callback");
  callback_ = std::move(callback);
  running_ = true;
  state_ = SinkState::Running;
  error_.clear();
}

void OfflineSink::Stop() {
  running_ = false;
  callback_ = nullptr;
  state_ = SinkState::Stopped;
}

bool OfflineSink::RenderBlock(media::AudioBuffer& into) {
  if (!running_ || !callback_ || state_ != SinkState::Running) return false;
  into.Silence();
  BlockResult result = BlockResult::End;
  try {
    result = callback_(into);
  } catch (const std::exception& error) {
    // The same contract as a device sink: a callback's failure is reported, not
    // thrown through whoever is pulling.
    state_ = SinkState::Failed;
    error_ = error.what();
    into.Silence();
    return false;
  } catch (...) {
    state_ = SinkState::Failed;
    error_ = "the render callback failed";
    into.Silence();
    return false;
  }
  if (result == BlockResult::Dropped) {
    // Made for a position the transport has left: neither played nor counted.
    into.Silence();
    return true;
  }
  // The clock advances by what was rendered whether or not the callback had
  // audio, because silence still occupies time on the timeline.
  clock_.AdvanceFromAudioCallback(into.frames());
  if (result == BlockResult::End) state_ = SinkState::Finished;
  return result == BlockResult::Audio;
}

int OfflineSink::RenderBlocks(int blocks) {
  auto buffer = media::AudioBuffer::Allocate(format_.sample_rate, format_.channels, format_.block_frames);
  int rendered = 0;
  for (int index = 0; index < blocks; ++index) {
    if (!RenderBlock(buffer)) break;
    ++rendered;
  }
  return rendered;
}

#ifndef CUTLINE_HAVE_WASAPI
std::unique_ptr<AudioSink> OpenDefaultAudioSink(AudioDeviceFormat format) {
  return std::make_unique<OfflineSink>(format);
}

bool HasAudioDevice() { return false; }
#endif

}  // namespace cutline::audio

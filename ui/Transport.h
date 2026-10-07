#pragma once

// The transport: where the playhead is and how it moves. Space plays and stops; J, K and L shuttle (J backward, L
// forward, each further press of the same key doubling the speed up to 8x, K stopping, K held with J or L giving
// slow motion); the arrows step frames. Time advances by a caller-supplied clock tick, so the transport does not
// care whether a window timer, the audio device or a test drives it. Only plain forward playback at normal speed
// is driven by audio; every other rate is a picture-only shuttle, which `audio_driven()` says.

#include "core/time/RationalTime.h"

#include <optional>

namespace cutline::ui {

class Transport final {
 public:
  explicit Transport(time::FrameRate rate = time::kFrameRate25);

  void SetFrameRate(time::FrameRate rate);
  // The end of the sequence: playback stops, or loops, there.
  void SetDuration(const time::RationalTime& duration);
  // Loop playback over [in, out) when looping is on; with no range it loops the whole sequence.
  void SetLoopRange(std::optional<time::RationalTime> in, std::optional<time::RationalTime> out);
  void SetLooping(bool looping) { looping_ = looping; }
  [[nodiscard]] bool looping() const { return looping_; }

  [[nodiscard]] time::RationalTime position() const;
  void Seek(const time::RationalTime& to);
  void StepFrames(std::int64_t frames);
  void GoToStart();
  void GoToEnd();

  [[nodiscard]] double rate() const { return rate_; }
  [[nodiscard]] bool playing() const { return rate_ != 0.0; }
  [[nodiscard]] bool audio_driven() const { return rate_ == 1.0; }

  // Space: play forward at normal speed, or stop.
  void TogglePlay();
  // L and J: start, or double the speed of, playing in that direction (from the other direction, start at 1x).
  void PressForward();
  void PressReverse();
  // K.
  void PressStop();
  // K held: J and L give a quarter-speed shuttle.
  void SetSlowModifier(bool held) { slow_held_ = held; }
  void Stop() { rate_ = 0.0; }

  struct Tick final {
    time::RationalTime position;
    bool moved{false};
    // Playback came to the end (or the start, going backward) and stopped.
    bool reached_boundary{false};
    // The loop wrapped round.
    bool looped{false};
  };
  // Advances by `seconds` of the clock at the current rate.
  Tick Advance(double seconds);

 private:
  [[nodiscard]] double Frame() const;
  [[nodiscard]] double Duration() const;
  [[nodiscard]] double LoopIn() const;
  [[nodiscard]] double LoopOut() const;

  time::FrameRate frame_rate_;
  double seconds_{0.0};
  double duration_seconds_{0.0};
  std::optional<double> loop_in_, loop_out_;
  double rate_{0.0};
  bool looping_{false};
  bool slow_held_{false};
};

}  // namespace cutline::ui

#include "ui/Transport.h"

#include <algorithm>
#include <cmath>

namespace cutline::ui {
namespace {

double ToSeconds(const time::RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()); }

}  // namespace

Transport::Transport(time::FrameRate rate) : frame_rate_(rate) {}

void Transport::SetFrameRate(time::FrameRate rate) {
  frame_rate_ = rate;
  Seek(position());
}

double Transport::Frame() const {
  return frame_rate_.numerator > 0 ? static_cast<double>(frame_rate_.denominator) / static_cast<double>(frame_rate_.numerator) : 0.04;
}

double Transport::Duration() const { return duration_seconds_; }
double Transport::LoopIn() const { return loop_in_.value_or(0.0); }
double Transport::LoopOut() const { return loop_out_.value_or(duration_seconds_); }

void Transport::SetDuration(const time::RationalTime& duration) {
  duration_seconds_ = std::max(0.0, ToSeconds(duration));
  seconds_ = std::min(seconds_, duration_seconds_);
}

void Transport::SetLoopRange(std::optional<time::RationalTime> in, std::optional<time::RationalTime> out) {
  loop_in_ = in ? std::optional<double>(ToSeconds(*in)) : std::nullopt;
  loop_out_ = out ? std::optional<double>(ToSeconds(*out)) : std::nullopt;
  if (loop_in_ && loop_out_ && *loop_out_ <= *loop_in_) {
    loop_in_.reset();
    loop_out_.reset();
  }
}

time::RationalTime Transport::position() const {
  return time::RationalTime::FromFrames(static_cast<std::int64_t>(std::llround(seconds_ / Frame())), frame_rate_);
}

void Transport::Seek(const time::RationalTime& to) {
  // A frame boundary, within the sequence.
  const auto frames = static_cast<std::int64_t>(std::llround(ToSeconds(to) / Frame()));
  seconds_ = std::clamp(static_cast<double>(frames) * Frame(), 0.0, duration_seconds_);
}

void Transport::StepFrames(std::int64_t frames) {
  rate_ = 0.0;
  Seek(time::RationalTime::FromFrames(static_cast<std::int64_t>(std::llround(seconds_ / Frame())) + frames, frame_rate_));
}

void Transport::GoToStart() { seconds_ = 0.0; }

void Transport::GoToEnd() { seconds_ = duration_seconds_; }

void Transport::TogglePlay() {
  if (rate_ != 0.0) {
    rate_ = 0.0;
    return;
  }
  // At the end, play again from the start.
  if (seconds_ >= duration_seconds_ - 1e-9) seconds_ = looping_ ? LoopIn() : 0.0;
  rate_ = 1.0;
}

void Transport::PressForward() {
  if (slow_held_) {
    rate_ = 0.25;
    return;
  }
  if (rate_ <= 0.0 || rate_ < 1.0) rate_ = 1.0;
  else rate_ = std::min(rate_ * 2.0, 8.0);
}

void Transport::PressReverse() {
  if (slow_held_) {
    rate_ = -0.25;
    return;
  }
  if (rate_ >= 0.0 || rate_ > -1.0) rate_ = -1.0;
  else rate_ = std::max(rate_ * 2.0, -8.0);
}

void Transport::PressStop() { rate_ = 0.0; }

Transport::Tick Transport::Advance(double seconds) {
  Tick tick;
  if (rate_ == 0.0 || seconds <= 0.0) {
    tick.position = position();
    return tick;
  }
  const double before = seconds_;
  seconds_ += rate_ * seconds;
  const bool wraps = looping_ && duration_seconds_ > 0.0;
  const double top = wraps ? LoopOut() : duration_seconds_;
  const double bottom = wraps ? LoopIn() : 0.0;
  if (rate_ > 0.0 && seconds_ >= top) {
    if (wraps) {
      seconds_ = bottom + std::fmod(seconds_ - top, std::max(top - bottom, Frame()));
      tick.looped = true;
    } else {
      seconds_ = duration_seconds_;
      rate_ = 0.0;
      tick.reached_boundary = true;
    }
  } else if (rate_ < 0.0 && seconds_ <= bottom) {
    if (wraps) {
      seconds_ = top - std::fmod(bottom - seconds_, std::max(top - bottom, Frame()));
      tick.looped = true;
    } else {
      seconds_ = 0.0;
      rate_ = 0.0;
      tick.reached_boundary = true;
    }
  }
  tick.moved = seconds_ != before;
  tick.position = position();
  return tick;
}

}  // namespace cutline::ui

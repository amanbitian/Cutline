#pragma once

// A deterministic, generated media source.
//
// Golden-frame tests need media whose every pixel and sample is known exactly
// and identical on every machine. Real files cannot provide that: codecs differ
// between builds, and committing fixture video to the repository is both large
// and still version-dependent. This source synthesises frames from the
// timestamp alone, so a golden image is reproducible anywhere.
//
// It is also how the pipeline is tested without FFmpeg present.
//
// Paths look like a URI so a synthetic clip can be stored in a project exactly
// as a file path would be:
//
//   synthetic:counter?duration=10&fps=30000/1001&w=320&h=180&tone=440
//
// Patterns:
//   solid    -- one colour, from r/g/b parameters. Easiest to assert against.
//   bars     -- vertical colour bars. Catches channel-order and stride errors.
//   gradient -- horizontal ramp. Catches interpolation and rounding errors.
//   counter  -- encodes the source frame index into the top-left pixels, so a
//               test can prove which frame was decoded. This is what makes
//               retime, reverse, and variable-frame-rate behaviour verifiable.

#include "media/Source.h"

#include <cstdint>
#include <string>

namespace cutline::media {

enum class SyntheticPattern { Solid, Bars, Gradient, Counter };

struct SyntheticSpec final {
  SyntheticPattern pattern{SyntheticPattern::Counter};
  int width{320};
  int height{180};
  time::FrameRate frame_rate{30, 1};
  time::RationalTime duration{10, 1};
  // Solid colour, 0..1 per channel.
  float red{0.5f};
  float green{0.5f};
  float blue{0.5f};
  // Audio: a sine tone. Zero frequency means silence.
  double tone_hz{0.0};
  float tone_amplitude{0.5f};
  std::int64_t sample_rate{48000};
  int channels{2};
  // When set, frame durations alternate between one and two nominal frame
  // intervals, which is how a variable-frame-rate source is simulated.
  bool variable_frame_rate{false};

  [[nodiscard]] static SyntheticSpec Parse(const std::string& path);
  [[nodiscard]] std::string ToPath() const;
};

// Reads the frame index back out of a Counter frame. Returns -1 if the frame
// does not carry a readable counter.
[[nodiscard]] std::int64_t ReadFrameCounter(const VideoFrame& frame);

[[nodiscard]] std::unique_ptr<Source> OpenSynthetic(const SyntheticSpec& spec);

// Registers the `synthetic:` provider. Called by the core's provider setup; safe
// to call more than once.
void RegisterSyntheticProvider();

}  // namespace cutline::media

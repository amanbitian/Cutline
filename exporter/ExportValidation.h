#pragma once

// Checking a finished export: open the file the way anyone else would and see whether it is what was asked for.
//
// The export worker publishes a file only when the encoder finished cleanly, which says nothing about whether the
// delivery is right: a container that reports the wrong length, a stream missing, a size or rate other than the
// sequence's, a file that probes fine and cannot be decoded at its last frame. The check reads the file with the same
// provider anything else would use, so what it says is what a player or the next application will find.

#include "core/time/RationalTime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cutline::exporter {

struct ExportExpectation final {
  bool video{true};
  bool audio{true};
  std::int64_t width{0};
  std::int64_t height{0};
  time::FrameRate frame_rate{0, 1};
  time::RationalTime duration;  // of what was exported
  std::int64_t sample_rate{0};
  int channels{0};
  // What the streams report as their codec ("h264", "prores", "pcm_s24le"); empty skips the comparison.
  std::string video_codec;
  std::string audio_codec;
  // How far the length may be from the expected: a container rounds to its own time base. Default two frames.
  time::RationalTime tolerance{2, 25};
};

struct ValidationResult final {
  bool ok{false};
  std::vector<std::string> problems;  // each one a sentence a person can act on
  time::RationalTime duration;        // as the file reports it
  std::int64_t bytes{0};
  std::string video_codec, audio_codec;
};

// Opens the file, compares what it reports with the expectation, and decodes the first and last picture and a block of
// sound. Never throws: a file that cannot be opened is a problem in the result.
[[nodiscard]] ValidationResult ValidateExport(const std::string& path, const ExportExpectation& expected);

}  // namespace cutline::exporter

#pragma once

// Rendering a sequence to a file.
//
// The export worker renders exactly what the program monitor renders -- the
// same PlaybackPlan, the same compositor, the same mixer -- so an export cannot
// disagree with what was approved on screen. That is the property the
// round-trip tests check: decode the exported file back and compare it against
// the monitor's output for the same timecodes.
//
// Two things it does that playback does not. It ignores mute and solo by
// default, because those are monitoring state rather than editorial intent. And
// it locks audio to video by deriving each frame's sample count from absolute
// time rather than accumulating, so a 48 kHz mix against a 30000/1001 sequence
// -- 1601.6 samples per frame -- stays in sync over hours instead of drifting a
// frame every few minutes.

#include "media/Writer.h"
#include "playback/PlaybackEngine.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace cutline::exporter {

struct ExportRequest final {
  std::string output_path;
  // Replace an existing file at `output_path`. When false (the default) an
  // existing file is left alone and the export is refused. A replacement only
  // happens once the new file is complete; a cancelled or failed export never
  // disturbs what was there.
  bool overwrite{false};
  // Range to render. `out` at zero means "to the end of the sequence".
  time::RationalTime in;
  time::RationalTime out;

  media::VideoEncoderSettings video;
  media::AudioEncoderSettings audio;
  bool include_video{true};
  bool include_audio{true};

  // Export renders every track by default: a muted track is usually muted to
  // hear something else while cutting, not to leave it out of the delivery.
  bool honour_mute_and_solo{false};

  // Burn the sequence's captions into the picture. Off by default: captions are delivered as
  // separate files (captions::WriteSidecars) unless a burned-in copy is wanted.
  bool burn_in_captions{false};

  // Container name (empty: from the file extension) and its options ("movflags" = "+faststart").
  std::string container;
  std::map<std::string, std::string> container_options;
  // The encoder keeps more than 8 bits per component (ProRes, DNxHR HQX, 10-bit HEVC, FFV1 10-bit): render the pictures in
  // 16 bits so they are not 8-bit pictures in a 10-bit file.
  bool high_precision{false};
};

struct ExportProgress final {
  std::int64_t frames_written{0};
  std::int64_t frames_total{0};
  time::RationalTime position;
};

// Returning false cancels the export; the partial file is removed.
using ProgressCallback = std::function<bool(const ExportProgress&)>;

struct ExportResult final {
  std::int64_t video_frames{0};
  std::int64_t audio_frames{0};
  time::RationalTime duration;
  bool cancelled{false};
};

// Fills in whatever the request left unset from the sequence: frame size, rate,
// and colour for video; rate and channels for audio. Called by Export, and
// exposed so a UI can show what an export is actually going to produce.
[[nodiscard]] ExportRequest ResolveRequest(const playback::PlaybackEngine& engine, ExportRequest request);

[[nodiscard]] ExportResult Export(playback::PlaybackEngine& engine, const ExportRequest& request,
                                  const ProgressCallback& progress = {});

}  // namespace cutline::exporter

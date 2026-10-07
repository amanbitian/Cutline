#pragma once

// CMX 3600 edit decision lists.
//
// An EDL is a flat list of events: reel, channel, cut or dissolve, and four timecodes (source
// in and out, record in and out). It has no frame rate of its own, no nesting, no effects
// beyond a dissolve and a speed change, and one video channel. What it cannot hold is
// reported; what it can is written so that another tool and Cutline both read it.
//
// Conventions kept here:
//   * Record time starts at 01:00:00:00, as editing systems expect, unless told otherwise;
//     reading takes the hour of the earliest event as the sequence's start.
//   * A dissolve is the usual two lines: a zero-length cut event for the outgoing side and
//     the incoming event with a D and its length in frames. It starts where the incoming
//     event starts, so a transition centred on a cut is written starting half its length
//     earlier, with the clip edges moved to match (the picture is the same; the report
//     says so).
//   * A clip played at another speed gets an M2 line; a negative speed is a reverse.
//   * Lines beginning * CUTLINE carry what an EDL has no place for (media length, ids, link
//     groups). Other tools skip them as comments; without them, media lengths are taken
//     as the furthest point used.

#include "interchange/Interchange.h"

#include <optional>
#include <string>

namespace cutline::interchange {

struct EdlOptions final {
  std::string title;
  // Where the sequence's first frame sits in record time. Unset, it is 01:00:00:00 in the
  // sequence's own timecode (which is not 3600 s at a fractional rate).
  std::optional<time::RationalTime> record_start;
};

[[nodiscard]] std::string WriteEdl(const Timeline& timeline, const EdlOptions& options, Report& report);

// An EDL does not say what frame rate it is in; the caller does.
[[nodiscard]] Timeline ReadEdl(const std::string& text, time::FrameRate rate, bool drop_frame, Report& report);

}  // namespace cutline::interchange

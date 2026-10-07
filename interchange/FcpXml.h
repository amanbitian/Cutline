#pragma once

// Final Cut Pro 7 XML (xmeml version 4), the interchange format Premiere, Resolve and
// Final Cut 7 all read and write.
//
// What it holds, and so what is written: sequences with their rate and size; video and
// audio tracks of clip items with start, end, in and out as frame counts; files with path,
// length and start timecode; dissolves and crossfades with their alignment; speed and
// reverse as a Time Remap filter; links between clips; markers; and nested sequences as
// clip items that contain a sequence.
//
// What it does not hold: anything that is not a whole frame, so every time is rounded to the
// nearest frame at the sequence's rate (and the report says when that moved something);
// Cutline's own effects, ids, track mixer state and link-group names, which are carried in
// the comments of the sequence and its clip items, where other tools leave them alone.

#include "interchange/Interchange.h"

#include <string>

namespace cutline::interchange {

[[nodiscard]] std::string WriteFcpXml(const Timeline& timeline, Report& report);
[[nodiscard]] Timeline ReadFcpXml(const std::string& text, Report& report);

}  // namespace cutline::interchange

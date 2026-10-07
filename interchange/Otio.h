#pragma once

// OpenTimelineIO (https://opentimeline.io) as a file format: the JSON that OTIO tools
// read and write, for the parts of it Cutline can say.
//
// What is written. A Timeline with a Stack of Tracks; Clip (ExternalReference media),
// Gap, Transition and nested Stack items; Effect and LinearTimeWarp on clips; Marker on
// clips and the sequence. Times are RationalTime values at the sequence's frame rate
// where the time falls on a frame, and at their own exact rate where it does not.
//
// What is kept for a round trip through Cutline. Anything an OTIO tool has no field for
// (ids, the exact source range of a retimed clip, parameter keyframes, link groups, track
// mixer state, sequence settings) is written under metadata.cutline, which other tools
// carry along without reading. A file that has it comes back identical; a file from
// another tool, which does not, is read from its standard fields and anything assumed is
// reported.
//
// What is not supported is reported, never dropped quietly: generator and image-sequence
// references, freeze frames and other effect types, markers on tracks, audio buses.

#include "interchange/Interchange.h"

#include <string>

namespace cutline::interchange {

[[nodiscard]] std::string WriteOtio(const Timeline& timeline, Report& report);
[[nodiscard]] Timeline ReadOtio(const std::string& text, Report& report);

}  // namespace cutline::interchange

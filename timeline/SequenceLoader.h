#pragma once

// Builds an immutable Sequence snapshot from a project database.
//
// This is the only read path the playback side needs, and it is deliberately
// separate from the compiler: loading touches SQLite and allocates, compiling
// does neither. Load once per edit (the project revision tells you when), then
// compile per frame.

#include "timeline/Sequence.h"

#include <cstdint>
#include <string>

struct sqlite3;

namespace cutline::project {
class ProjectStore;
}

namespace cutline::timeline {

// What a load actually read. Exposed so "only loads what it needs" is a checkable
// property rather than a comment: loading one sequence must not read the
// parameters and keyframes of every other sequence in the project.
struct LoadStatistics final {
  std::int64_t effects{0};
  std::int64_t parameters{0};
  std::int64_t keyframes{0};
};

// Loads one sequence with its tracks, clips, transitions, effects, and
// keyframes. Clips arrive sorted by start time, as the compiler requires.
[[nodiscard]] Sequence LoadSequence(sqlite3* database, const std::string& sequence_id,
                                    LoadStatistics* statistics = nullptr);

// Loads a sequence together with every sequence reachable from it through
// nested clips, so the compiler never has to query mid-frame.
[[nodiscard]] SequenceGraph LoadSequenceGraph(sqlite3* database, const std::string& sequence_id,
                                              int max_depth = 8);

// Takes the store's lock and loads through it. The overload that takes a raw
// handle exists for callers that already hold the lock.
[[nodiscard]] SequenceGraph LoadSequenceGraph(const project::ProjectStore& store, const std::string& sequence_id,
                                              int max_depth = 8);

}  // namespace cutline::timeline

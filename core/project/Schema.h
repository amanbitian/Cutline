#pragma once

// Project database schema.
//
// Schema v3 adds the structures that v2 had no room for: effect stacks with
// keyframed parameters, transitions, nested sequences, structured per-stream
// media metadata, colour management fields, and the audio channel model. Those
// are kept here rather than inline in ProjectStore so the shape of a project is
// readable in one place and migrations can be reviewed as a diff.

#include <cstdint>
#include <string>

struct sqlite3;

namespace cutline::project {

// v4 adds sequences.render_version (see core/model/RenderVersion.h); v5 adds
// clips.maintain_pitch.
inline constexpr std::int64_t kSchemaVersion = 13;

// Creates every table, index, and trigger for the current schema version.
void CreateSchema(sqlite3* database);

// Brings an older database up to kSchemaVersion. Caller is responsible for
// taking a backup first; this runs inside its own transaction.
void MigrateSchema(sqlite3* database, std::int64_t from_version);

// Runs integrity and foreign-key checks plus the invariants SQLite cannot
// express (polymorphic owner references, clip source exclusivity).
void ValidateSchema(sqlite3* database);

// Why a transition is not valid, or an empty string when it is. With an id it
// checks that transition; with none it checks all of them and the answer is
// prefixed with the offending transition's id.
//
// A transition joins the clips either side of a cut, so it is only meaningful
// while: its clips exist and are on its track; the two clips are adjacent (the
// outgoing one ends exactly where the incoming one starts); its span covers the
// cut; and the span matches its alignment (a transition that says it starts at
// the cut must start there). A one-sided transition -- a fade from or to
// nothing -- has no second clip and the cut is that clip's own edge.
//
// This is the single statement of the rule. The store applies it after every
// edit that can disturb a transition, and ValidateSchema applies it to a whole
// project, so the two cannot disagree.
[[nodiscard]] std::string FindTransitionProblem(sqlite3* database, const std::string& transition_id = {});

}  // namespace cutline::project

#include "core/project/ProjectStore.h"

#include "core/db/Sql.h"
#include "core/model/RenderVersion.h"
#include "core/project/Schema.h"
#include "core/util/Base64.h"
#include "core/util/Json.h"
#include "core/util/JsonParse.h"
#include "effects/EffectRegistry.h"
#include "effects/GraphicsDocument.h"
#include "effects/MaskDocument.h"

#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cutline::project {
namespace {

using commands::CommandEnvelope;
using db::Statement;

constexpr const char* kDatabaseFileName = "project.db";

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Stops the schema's cascade triggers firing while a changeset is replayed.
//
// A changeset already records every row a cascade removed, so running the
// triggers again would delete those rows a second time. The foreign-key half of
// the same problem is handled by SQLITE_CHANGESETAPPLY_FKNOACTION in
// ChangeSet::ApplyTo, which -- unlike PRAGMA foreign_keys -- takes effect inside
// a transaction.
class CascadeSuspension final {
 public:
  explicit CascadeSuspension(sqlite3* database) : database_(database) {
    db::Execute(database_, "UPDATE project_meta SET suppress_cascades = 1 WHERE singleton = 1;");
  }

  ~CascadeSuspension() {
    try {
      db::Execute(database_, "UPDATE project_meta SET suppress_cascades = 0 WHERE singleton = 1;");
    } catch (...) {
      // Restoring during unwinding is best effort; the next statement on this
      // connection will surface anything genuinely broken.
    }
  }

  CascadeSuspension(const CascadeSuspension&) = delete;
  CascadeSuspension& operator=(const CascadeSuspension&) = delete;

 private:
  sqlite3* database_;
};

// --------------------------------------------------------- row existence ----

[[nodiscard]] bool ExistsBy(sqlite3* database, std::string_view sql, std::string_view id) {
  Statement statement(database, sql);
  statement.Bind(1, id);
  return statement.Step();
}

void RequireExists(sqlite3* database, std::string_view sql, const std::string& id, const char* label) {
  if (!ExistsBy(database, sql, id)) throw std::runtime_error(std::string("Unknown ") + label + ": " + id);
}

void RequireAbsent(sqlite3* database, std::string_view sql, const std::string& id, const char* label) {
  if (ExistsBy(database, sql, id)) throw std::runtime_error(std::string(label) + " already exists: " + id);
}

constexpr std::string_view kFindBin = "SELECT 1 FROM bins WHERE id = ?;";
constexpr std::string_view kFindMedia = "SELECT 1 FROM media WHERE id = ?;";
constexpr std::string_view kFindSequence = "SELECT 1 FROM sequences WHERE id = ?;";
constexpr std::string_view kFindTrack = "SELECT 1 FROM tracks WHERE id = ?;";
constexpr std::string_view kFindClip = "SELECT 1 FROM clips WHERE id = ?;";
constexpr std::string_view kFindTransition = "SELECT 1 FROM transitions WHERE id = ?;";
constexpr std::string_view kFindEffect = "SELECT 1 FROM effects WHERE id = ?;";
constexpr std::string_view kFindParameter = "SELECT 1 FROM effect_parameters WHERE id = ?;";
constexpr std::string_view kFindMask = "SELECT 1 FROM effect_masks WHERE id = ?;";
constexpr std::string_view kFindMarker = "SELECT 1 FROM markers WHERE id = ?;";

// ------------------------------------------------------- time to columns ----

// A rational time becomes three columns: the exact pair, which is the authority,
// plus the tick value the range indexes use. Produced here so the two forms can
// never disagree.
struct TimeColumns final {
  std::int64_t numerator{};
  std::int64_t denominator{1};
  std::int64_t ticks{};
};

[[nodiscard]] TimeColumns Columns(const time::RationalTime& value) {
  return {value.numerator(), value.denominator(), value.ToTicks()};
}

// ------------------------------------------------------------- clip rows ----

struct ClipRow final {
  std::string id;
  std::string track_id;
  model::SourceKind source_kind{model::SourceKind::Media};
  std::optional<std::string> media_id;
  std::optional<std::string> nested_sequence_id;
  time::RationalTime source_in;
  time::RationalTime source_out;
  time::RationalTime timeline_start;
  time::RationalTime playback_rate{1, 1};
  bool reversed{false};
  bool maintain_pitch{false};
  bool enabled{true};
  std::string linked_group;
  std::string name;
  std::string audio_role;

  // Source duration compressed by the playback rate: a clip played at 2x
  // occupies half as much timeline as it does source.
  [[nodiscard]] time::RationalTime TimelineDuration() const {
    return source_out.Subtract(source_in).Divide(playback_rate);
  }
  [[nodiscard]] time::RationalTime TimelineEnd() const { return timeline_start.Add(TimelineDuration()); }
};

constexpr std::string_view kSelectClip = R"sql(
  SELECT id, track_id, source_kind, media_id, nested_sequence_id,
         source_in_num, source_in_den, source_out_num, source_out_den,
         timeline_start_num, timeline_start_den, rate_num, rate_den,
         reversed, enabled, linked_group, name, maintain_pitch, audio_role
    FROM clips WHERE id = ?;
)sql";

[[nodiscard]] ClipRow ReadClip(const Statement& statement) {
  ClipRow row;
  row.id = statement.ColumnText(0);
  row.track_id = statement.ColumnText(1);
  row.source_kind = model::ParseSourceKind(statement.ColumnText(2));
  row.media_id = statement.ColumnOptionalText(3);
  row.nested_sequence_id = statement.ColumnOptionalText(4);
  row.source_in = {statement.ColumnInt(5), statement.ColumnInt(6)};
  row.source_out = {statement.ColumnInt(7), statement.ColumnInt(8)};
  row.timeline_start = {statement.ColumnInt(9), statement.ColumnInt(10)};
  row.playback_rate = {statement.ColumnInt(11), statement.ColumnInt(12)};
  row.reversed = statement.ColumnInt(13) != 0;
  row.enabled = statement.ColumnInt(14) != 0;
  row.linked_group = statement.ColumnText(15);
  row.name = statement.ColumnText(16);
  row.maintain_pitch = statement.ColumnInt(17) != 0;
  row.audio_role = statement.ColumnText(18);
  return row;
}

[[nodiscard]] ClipRow LoadClip(sqlite3* database, const std::string& id) {
  Statement statement(database, kSelectClip);
  statement.Bind(1, id);
  if (!statement.Step()) throw std::runtime_error("Unknown clip: " + id);
  return ReadClip(statement);
}

// ------------------------------------------------------------ invariants ----

// Two clips may never occupy the same frame on one track. Checked here rather
// than only in ValidateSchema so a bad edit is rejected instead of persisted.
void RequireNoOverlap(sqlite3* database, const std::string& track_id, std::int64_t start_ticks,
                      std::int64_t end_ticks, const std::string& ignore_clip_id) {
  Statement statement(database, R"sql(
    SELECT id FROM clips
     WHERE track_id = ?
       AND id <> ?
       AND timeline_start_ticks < ?
       AND timeline_end_ticks > ?
     LIMIT 1;
  )sql");
  statement.Bind(1, track_id).Bind(2, ignore_clip_id).Bind(3, end_ticks).Bind(4, start_ticks);
  if (statement.Step()) {
    throw std::runtime_error("Clip would overlap " + statement.ColumnText(0) + " on track " + track_id);
  }
}

// A media clip's source range has to fit inside the media it reads. Without
// this, a trim can run off the end of the file and the decoder has no recourse.
void RequireWithinMedia(sqlite3* database, const std::optional<std::string>& media_id,
                        const time::RationalTime& source_in, const time::RationalTime& source_out) {
  if (!media_id.has_value()) return;
  Statement statement(database, "SELECT duration_num, duration_den FROM media WHERE id = ?;");
  statement.Bind(1, *media_id);
  if (!statement.Step()) throw std::runtime_error("Unknown media: " + *media_id);
  const time::RationalTime duration{statement.ColumnInt(0), statement.ColumnInt(1)};
  // Duration 0 means the media was imported before it had been probed; allow it
  // rather than blocking the edit, and let the probe tighten the bound later.
  if (duration.Compare({0, 1}) <= 0) return;
  Require(source_in.Compare({0, 1}) >= 0, "Clip source in-point cannot be negative");
  Require(source_out.Compare(duration) <= 0,
          "Clip source out-point " + source_out.FormatTimecode(time::kFrameRate25) + " is past the end of the media");
}

[[nodiscard]] model::TrackKind TrackKindOf(sqlite3* database, const std::string& track_id) {
  Statement statement(database, "SELECT track_type FROM tracks WHERE id = ?;");
  statement.Bind(1, track_id);
  if (!statement.Step()) throw std::runtime_error("Unknown track: " + track_id);
  return model::ParseTrackKind(statement.ColumnText(0));
}

// A bus sums other tracks and holds no clips of its own.
void RequireNotBus(sqlite3* database, const std::string& track_id) {
  Statement statement(database, "SELECT is_bus FROM tracks WHERE id = ?;");
  statement.Bind(1, track_id);
  if (!statement.Step()) throw std::runtime_error("Unknown track: " + track_id);
  Require(statement.ColumnInt(0) == 0, "Track " + track_id + " is a bus and cannot hold clips");
}

// Rejects a nesting cycle: a sequence may not contain itself at any depth, or
// compiling it would recurse forever.
void RequireNoNestingCycle(sqlite3* database, const std::string& track_id, const std::string& nested_sequence_id) {
  Statement owner(database, "SELECT sequence_id FROM tracks WHERE id = ?;");
  owner.Bind(1, track_id);
  if (!owner.Step()) throw std::runtime_error("Unknown track: " + track_id);
  const auto host_sequence = owner.ColumnText(0);

  // Walk down from the candidate: if the host sequence is reachable, nesting it
  // would close a loop.
  Statement walk(database, R"sql(
    WITH RECURSIVE reachable(sequence_id) AS (
      SELECT ?
      UNION
      SELECT c.nested_sequence_id
        FROM clips c
        JOIN tracks t ON t.id = c.track_id
        JOIN reachable r ON r.sequence_id = t.sequence_id
       WHERE c.nested_sequence_id IS NOT NULL
    )
    SELECT 1 FROM reachable WHERE sequence_id = ? LIMIT 1;
  )sql");
  walk.Bind(1, nested_sequence_id).Bind(2, host_sequence);
  if (walk.Step()) {
    throw std::runtime_error("Nesting sequence " + nested_sequence_id + " inside " + host_sequence +
                             " would create a cycle");
  }
}

// ---------------------------------------------------------------- track locks ----
//
// POLICY. A locked track protects what is on it: its clips, the transitions
// between them, the effects and animation attached to any of those, and the track
// itself (it cannot be removed). Every command that changes any of that checks
// the lock, through one of the helpers below, so the answer does not depend on
// which command happened to remember to ask.
//
// Deliberately outside the lock: the track's own state (mute, solo, gain, pan,
// name and the lock itself -- otherwise it could never be unlocked), markers
// (annotations rather than edits), and effects owned by the sequence rather than
// by a track. Moving a clip is checked against both its source and destination
// track. Locks do not yet extend across linked clips on other tracks, because
// grouped edits are not implemented (see REMEDIATION.md).

void RequireTrackUnlocked(sqlite3* database, const std::string& track_id) {
  Statement statement(database, "SELECT locked FROM tracks WHERE id = ?;");
  statement.Bind(1, track_id);
  if (!statement.Step()) throw std::runtime_error("Unknown track: " + track_id);
  Require(statement.ColumnInt(0) == 0, "Track " + track_id + " is locked");
}

// The track an effect owner lives on, or nothing for an owner that is not on one.
[[nodiscard]] std::optional<std::string> TrackOfOwner(sqlite3* database, const std::string& owner_kind,
                                                      const std::string& owner_id) {
  if (owner_kind == "track") return owner_id;
  const char* sql = owner_kind == "clip"         ? "SELECT track_id FROM clips WHERE id = ?;"
                    : owner_kind == "transition" ? "SELECT track_id FROM transitions WHERE id = ?;"
                                                 : nullptr;
  if (sql == nullptr) return std::nullopt;  // a sequence
  Statement statement(database, sql);
  statement.Bind(1, owner_id);
  if (!statement.Step()) throw std::runtime_error("Unknown " + owner_kind + ": " + owner_id);
  return statement.ColumnText(0);
}

void RequireOwnerUnlocked(sqlite3* database, const std::string& owner_kind, const std::string& owner_id) {
  if (const auto track = TrackOfOwner(database, owner_kind, owner_id); track.has_value()) {
    RequireTrackUnlocked(database, *track);
  }
}

void RequireEffectUnlocked(sqlite3* database, const std::string& effect_id) {
  Statement statement(database, "SELECT owner_kind, owner_id FROM effects WHERE id = ?;");
  statement.Bind(1, effect_id);
  if (!statement.Step()) throw std::runtime_error("Unknown effect: " + effect_id);
  RequireOwnerUnlocked(database, statement.ColumnText(0), statement.ColumnText(1));
}

void RequireParameterUnlocked(sqlite3* database, const std::string& parameter_id) {
  Statement statement(database, "SELECT effect_id FROM effect_parameters WHERE id = ?;");
  statement.Bind(1, parameter_id);
  if (!statement.Step()) throw std::runtime_error("Unknown parameter: " + parameter_id);
  RequireEffectUnlocked(database, statement.ColumnText(0));
}

void RequireTransitionUnlocked(sqlite3* database, const std::string& transition_id) {
  Statement statement(database, "SELECT track_id FROM transitions WHERE id = ?;");
  statement.Bind(1, transition_id);
  if (!statement.Step()) throw std::runtime_error("Unknown transition: " + transition_id);
  RequireTrackUnlocked(database, statement.ColumnText(0));
}

// ----------------------------------------------------------------- transitions ----

// Throws if the transition, as it now stands, no longer joins its clips. The
// rule itself lives in FindTransitionProblem so that editing and validation share it.
void RequireValidTransition(sqlite3* database, const std::string& transition_id) {
  const auto problem = FindTransitionProblem(database, transition_id);
  if (!problem.empty()) throw std::runtime_error("Transition " + transition_id + " is not valid: " + problem);
}

// Transitions may not overlap one another on a track. \`ignore\` is the transition
// being changed, if any.
void RequireNoTransitionOverlap(sqlite3* database, const std::string& track_id, std::int64_t start_ticks,
                                std::int64_t end_ticks, const std::string& ignore) {
  Statement clash(database, R"sql(
    SELECT id FROM transitions
     WHERE track_id = ? AND id <> ? AND timeline_start_ticks < ? AND timeline_end_ticks > ? LIMIT 1;
  )sql");
  clash.Bind(1, track_id).Bind(2, ignore).Bind(3, end_ticks).Bind(4, start_ticks);
  if (clash.Step()) {
    throw std::runtime_error("Transition would overlap " + clash.ColumnText(0) + " on track " + track_id);
  }
}

// After an edit changes where a clip sits or how long it is, removes any
// transition attached to it that no longer joins its clips.
//
// The alternatives were to refuse the edit or to leave the transition dangling.
// Refusing would make an ordinary drag fail because of something the editor did
// not touch, and dangling is the bug this exists to prevent. Removal is part of
// the same command, so one undo restores the clip and its transition together.
// An edit that leaves the cut alone -- trimming the far edge, a slip, reversing --
// keeps the transition.
void DetachBrokenTransitions(sqlite3* database, const std::vector<std::string>& clip_ids) {
  std::vector<std::string> attached;
  Statement find(database, "SELECT id FROM transitions WHERE from_clip_id = ?1 OR to_clip_id = ?1;");
  for (const auto& clip_id : clip_ids) {
    find.Reset();
    find.Bind(1, clip_id);
    while (find.Step()) attached.push_back(find.ColumnText(0));
  }
  Statement remove(database, "DELETE FROM transitions WHERE id = ?;");
  for (const auto& transition_id : attached) {
    if (FindTransitionProblem(database, transition_id).empty()) continue;
    remove.Reset();
    remove.Bind(1, transition_id);
    remove.Run();
  }
}

// ---------------------------------------------------------------- keyframes ----

[[nodiscard]] std::vector<anim::Keyframe> LoadKeyframes(sqlite3* database, const std::string& parameter_id,
                                                         int dimension) {
  Statement statement(database, R"sql(
    SELECT time_num, time_den, c0, c1, c2, c3, interpolation, out_handle_x, out_handle_y, in_handle_x, in_handle_y
      FROM keyframes WHERE parameter_id = ? ORDER BY time_ticks;
  )sql");
  statement.Bind(1, parameter_id);
  std::vector<anim::Keyframe> keys;
  while (statement.Step()) {
    anim::Keyframe key;
    key.time = {statement.ColumnInt(0), statement.ColumnInt(1)};
    key.value.dimension = dimension;
    for (int component = 0; component < 4; ++component) {
      key.value.components[static_cast<std::size_t>(component)] = statement.ColumnDouble(2 + component);
    }
    key.interpolation = anim::ParseInterpolation(statement.ColumnText(6));
    key.out_handle = {statement.ColumnDouble(7), statement.ColumnDouble(8)};
    key.in_handle = {statement.ColumnDouble(9), statement.ColumnDouble(10)};
    keys.push_back(key);
  }
  return keys;
}

// Moves every keyframe of every effect parameter on a clip by `shift` (a change
// to clip-local time). Used when the clip's start moves but its content does not.
void ShiftClipKeyframes(sqlite3* database, const std::string& clip_id, const time::RationalTime& shift);

void ReplaceKeyframes(sqlite3* database, const std::string& parameter_id, const std::vector<anim::Keyframe>& keys) {
  Statement clear(database, "DELETE FROM keyframes WHERE parameter_id = ?;");
  clear.Bind(1, parameter_id);
  clear.Run();
  Statement insert(database, R"sql(
    INSERT INTO keyframes(parameter_id, time_num, time_den, time_ticks, c0, c1, c2, c3,
                          interpolation, out_handle_x, out_handle_y, in_handle_x, in_handle_y)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  for (const auto& key : keys) {
    const auto at = Columns(key.time);
    insert.Reset();
    insert.Bind(1, parameter_id)
        .Bind(2, at.numerator)
        .Bind(3, at.denominator)
        .Bind(4, at.ticks)
        .Bind(5, key.value.components[0])
        .Bind(6, key.value.components[1])
        .Bind(7, key.value.components[2])
        .Bind(8, key.value.components[3])
        .Bind(9, anim::ToString(key.interpolation))
        .Bind(10, key.out_handle.x)
        .Bind(11, key.out_handle.y)
        .Bind(12, key.in_handle.x)
        .Bind(13, key.in_handle.y);
    insert.Run();
  }
}

void ShiftClipKeyframes(sqlite3* database, const std::string& clip_id, const time::RationalTime& shift) {
  Statement owned(database, R"sql(
    SELECT p.id, p.dimension
      FROM effect_parameters p JOIN effects e ON e.id = p.effect_id
     WHERE e.owner_kind = 'clip' AND e.owner_id = ?;
  )sql");
  owned.Bind(1, clip_id);
  std::vector<std::pair<std::string, int>> parameters;
  while (owned.Step()) parameters.emplace_back(owned.ColumnText(0), static_cast<int>(owned.ColumnInt(1)));
  for (const auto& [parameter_id, dimension] : parameters) {
    auto keys = LoadKeyframes(database, parameter_id, dimension);
    if (keys.empty()) continue;
    for (auto& key : keys) key.time = key.time.Add(shift);
    ReplaceKeyframes(database, parameter_id, keys);
  }
}

void ShiftClipMasks(sqlite3* database, const std::string& clip_id, const time::RationalTime& shift) {
  Statement statement(database, R"sql(
    SELECT m.id, m.document_json
      FROM effect_masks m JOIN effects e ON e.id = m.effect_id
     WHERE e.owner_kind = 'clip' AND e.owner_id = ?;
  )sql");
  statement.Bind(1, clip_id);
  std::vector<std::pair<std::string, std::string>> rows;
  while (statement.Step()) rows.emplace_back(statement.ColumnText(0), statement.ColumnText(1));
  const auto seconds = static_cast<double>(shift.numerator()) / static_cast<double>(shift.denominator());
  Statement update(database, "UPDATE effect_masks SET document_json = ? WHERE id = ?;");
  for (const auto& [id, document_json] : rows) {
    const auto shifted = effects::mask::Shift(effects::mask::Parse(document_json), seconds);
    update.Reset();
    update.Bind(1, effects::mask::ToJson(shifted)).Bind(2, id);
    update.Run();
  }
}

// -------------------------------------------------------------- mutators ----

// Every Apply overload runs inside the store's transaction with the changeset
// recorder active. They validate against live state, then write.

void Apply(sqlite3* database, const CommandEnvelope& command, const commands::CreateProjectPayload& payload) {
  const auto existing = db::ScalarText(database, "SELECT project_id FROM project_meta WHERE singleton = 1;");
  Require(existing.empty(), "This project already has an identity");
  Statement meta(database,
                 "UPDATE project_meta SET project_id = ?, created_at = ?, updated_at = ? WHERE singleton = 1;");
  meta.Bind(1, command.project_id).Bind(2, command.timestamp_utc).Bind(3, command.timestamp_utc);
  meta.Run();
  Statement settings(database, "UPDATE project_settings SET name = ? WHERE singleton = 1;");
  settings.Bind(1, payload.name);
  settings.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RenameProjectPayload& payload) {
  Statement statement(database, "UPDATE project_settings SET name = ? WHERE singleton = 1;");
  statement.Bind(1, payload.name);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::CreateBinPayload& payload) {
  RequireAbsent(database, kFindBin, payload.id, "Bin");
  if (payload.parent_id.has_value()) RequireExists(database, kFindBin, *payload.parent_id, "parent bin");
  Statement statement(database, "INSERT INTO bins(id, parent_id, sort_order, name) VALUES(?, ?, ?, ?);");
  statement.Bind(1, payload.id).BindOptional(2, payload.parent_id).Bind(3, payload.order).Bind(4, payload.name);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RenameBinPayload& payload) {
  RequireExists(database, kFindBin, payload.id, "bin");
  Statement statement(database, "UPDATE bins SET name = ? WHERE id = ?;");
  statement.Bind(1, payload.name).Bind(2, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::DeleteBinPayload& payload) {
  RequireExists(database, kFindBin, payload.id, "bin");
  // Child bins cascade; media in the bin is kept and becomes unfiled rather than
  // being destroyed with the folder.
  Statement unfile(database, "UPDATE media SET bin_id = NULL WHERE bin_id = ?;");
  unfile.Bind(1, payload.id);
  unfile.Run();
  Statement statement(database, "DELETE FROM bins WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::MoveBinPayload& payload) {
  RequireExists(database, kFindBin, payload.id, "bin");
  if (payload.parent_id.has_value()) {
    RequireExists(database, kFindBin, *payload.parent_id, "parent bin");
    // Walk up from the new parent; finding this bin means the move would
    // detach a subtree into a cycle.
    Statement walk(database, R"sql(
      WITH RECURSIVE ancestry(id) AS (
        SELECT ?
        UNION
        SELECT b.parent_id FROM bins b JOIN ancestry a ON a.id = b.id WHERE b.parent_id IS NOT NULL
      )
      SELECT 1 FROM ancestry WHERE id = ? LIMIT 1;
    )sql");
    walk.Bind(1, *payload.parent_id).Bind(2, payload.id);
    Require(!walk.Step(), "Moving bin " + payload.id + " there would create a cycle");
  }
  Statement statement(database, "UPDATE bins SET parent_id = ?, sort_order = ? WHERE id = ?;");
  statement.BindOptional(1, payload.parent_id).Bind(2, payload.order).Bind(3, payload.id);
  statement.Run();
}

void WriteMediaStreams(sqlite3* database, const std::string& media_id, const std::vector<commands::MediaStream>& streams);

void Apply(sqlite3* database, const CommandEnvelope& command, const commands::ImportMediaPayload& payload) {
  RequireAbsent(database, kFindMedia, payload.id, "Media");
  if (payload.bin_id.has_value()) RequireExists(database, kFindBin, *payload.bin_id, "bin");
  const auto duration = Columns(payload.duration);
  const auto timecode = Columns(payload.start_timecode);
  Statement statement(database, R"sql(
    INSERT INTO media(id, bin_id, display_name, original_path, fingerprint,
                      duration_num, duration_den, duration_ticks,
                      start_timecode_num, start_timecode_den, missing, created_at)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?);
  )sql");
  statement.Bind(1, payload.id)
      .BindOptional(2, payload.bin_id)
      .Bind(3, payload.display_name)
      .Bind(4, payload.original_path)
      .Bind(5, payload.fingerprint)
      .Bind(6, duration.numerator)
      .Bind(7, duration.denominator)
      .Bind(8, duration.ticks)
      .Bind(9, timecode.numerator)
      .Bind(10, timecode.denominator)
      .Bind(11, command.timestamp_utc);
  statement.Run();
  WriteMediaStreams(database, payload.id, payload.streams);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveMediaPayload& payload) {
  RequireExists(database, kFindMedia, payload.id, "media");
  Statement used(database, "SELECT COUNT(*) FROM clips WHERE media_id = ?;");
  used.Bind(1, payload.id);
  const auto references = used.Step() ? used.ColumnInt(0) : 0;
  Require(references == 0, "Media " + payload.id + " is used by " + std::to_string(references) +
                               " clip(s); remove them first");
  Statement angles(database, "SELECT COUNT(*) FROM multicam_angles WHERE media_id = ?;");
  angles.Bind(1, payload.id);
  const auto angle_count = angles.Step() ? angles.ColumnInt(0) : 0;
  Require(angle_count == 0, "Media " + payload.id + " is an angle of " + std::to_string(angle_count) +
                                " multicam group(s); remove them first");
  Statement statement(database, "DELETE FROM media WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RelinkMediaPayload& payload) {
  RequireExists(database, kFindMedia, payload.id, "media");
  Statement statement(database,
                      "UPDATE media SET original_path = ?, missing = ?, "
                      "fingerprint = CASE WHEN ? = '' THEN fingerprint ELSE ? END WHERE id = ?;");
  statement.Bind(1, payload.original_path)
      .Bind(2, payload.missing)
      .Bind(3, payload.fingerprint)
      .Bind(4, payload.fingerprint)
      .Bind(5, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetMediaStreamsPayload& payload) {
  RequireExists(database, kFindMedia, payload.media_id, "media");
  WriteMediaStreams(database, payload.media_id, payload.streams);
}

void Apply(sqlite3* database, const CommandEnvelope& command, const commands::AttachProxyPayload& payload) {
  RequireExists(database, kFindMedia, payload.media_id, "media");
  Statement statement(database, R"sql(
    INSERT INTO media_proxies(media_id, path, fingerprint, source_fingerprint, codec, width, height, created_at)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(media_id) DO UPDATE SET path = excluded.path, fingerprint = excluded.fingerprint,
      source_fingerprint = excluded.source_fingerprint, codec = excluded.codec,
      width = excluded.width, height = excluded.height, created_at = excluded.created_at;
  )sql");
  statement.Bind(1, payload.media_id)
      .Bind(2, payload.path)
      .Bind(3, payload.fingerprint)
      .Bind(4, payload.source_fingerprint)
      .Bind(5, payload.codec)
      .Bind(6, payload.width)
      .Bind(7, payload.height)
      .Bind(8, command.timestamp_utc);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::DetachProxyPayload& payload) {
  RequireExists(database, kFindMedia, payload.media_id, "media");
  Statement statement(database, "DELETE FROM media_proxies WHERE media_id = ?;");
  statement.Bind(1, payload.media_id);
  statement.Run();
}

// Replaces a media item's stream rows.
void WriteMediaStreams(sqlite3* database, const std::string& media_id, const std::vector<commands::MediaStream>& streams) {
  Statement clear(database, "DELETE FROM media_streams WHERE media_id = ?;");
  clear.Bind(1, media_id);
  clear.Run();

  Statement insert(database, R"sql(
    INSERT INTO media_streams(media_id, stream_index, kind, codec, width, height,
                              pixel_aspect_num, pixel_aspect_den, frame_rate_num, frame_rate_den,
                              cadence, bit_depth, chroma, field_order,
                              color_primaries, color_transfer, color_matrix, color_range,
                              sample_rate, channel_count, channel_layout)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  for (const auto& stream : streams) {
    insert.Reset();
    insert.Bind(1, media_id)
        .Bind(2, stream.stream_index)
        .Bind(3, model::ToString(stream.kind))
        .Bind(4, stream.codec)
        .Bind(5, stream.width)
        .Bind(6, stream.height)
        .Bind(7, stream.pixel_aspect.numerator)
        .Bind(8, stream.pixel_aspect.denominator)
        .Bind(9, stream.frame_rate.numerator)
        .Bind(10, stream.frame_rate.denominator)
        .Bind(11, model::ToString(stream.cadence))
        .Bind(12, stream.bit_depth)
        .Bind(13, stream.chroma)
        .Bind(14, model::ToString(stream.field_order))
        .Bind(15, stream.color_primaries)
        .Bind(16, stream.color_transfer)
        .Bind(17, stream.color_matrix)
        .Bind(18, model::ToString(stream.color_range))
        .Bind(19, stream.sample_rate)
        .Bind(20, stream.channel_count)
        .Bind(21, stream.channel_layout);
    insert.Run();
  }
}

void BindSequenceSettings(Statement& statement, const commands::SequenceSettings& settings, int first) {
  statement.Bind(first, settings.name)
      .Bind(first + 1, settings.frame_rate.numerator)
      .Bind(first + 2, settings.frame_rate.denominator)
      .Bind(first + 3, settings.width)
      .Bind(first + 4, settings.height)
      .Bind(first + 5, settings.pixel_aspect.numerator)
      .Bind(first + 6, settings.pixel_aspect.denominator)
      .Bind(first + 7, settings.sample_rate)
      .Bind(first + 8, settings.channel_layout)
      .Bind(first + 9, settings.working_color_space)
      .Bind(first + 10, settings.display_color_space)
      .Bind(first + 11, model::ToString(settings.field_order))
      .Bind(first + 12, settings.drop_frame);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::CreateSequencePayload& payload) {
  RequireAbsent(database, kFindSequence, payload.id, "Sequence");
  Statement statement(database, R"sql(
    INSERT INTO sequences(id, name, frame_rate_num, frame_rate_den, width, height,
                          pixel_aspect_num, pixel_aspect_den, sample_rate, channel_layout,
                          working_color_space, display_color_space, field_order, drop_frame, render_version)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  statement.Bind(1, payload.id);
  BindSequenceSettings(statement, payload.settings, 2);
  statement.Bind(15, payload.settings.render_version.value_or(model::kCurrentRenderVersion));
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::UpdateSequenceSettingsPayload& payload) {
  RequireExists(database, kFindSequence, payload.id, "sequence");
  Statement statement(database, R"sql(
    UPDATE sequences SET name = ?, frame_rate_num = ?, frame_rate_den = ?, width = ?, height = ?,
                         pixel_aspect_num = ?, pixel_aspect_den = ?, sample_rate = ?, channel_layout = ?,
                         working_color_space = ?, display_color_space = ?, field_order = ?, drop_frame = ?,
                         render_version = COALESCE(?, render_version)
     WHERE id = ?;
  )sql");
  BindSequenceSettings(statement, payload.settings, 1);
  if (payload.settings.render_version.has_value()) {
    statement.Bind(14, *payload.settings.render_version);
  } else {
    statement.BindNull(14);
  }
  statement.Bind(15, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::DeleteSequencePayload& payload) {
  RequireExists(database, kFindSequence, payload.id, "sequence");
  Statement nested(database, "SELECT COUNT(*) FROM clips WHERE nested_sequence_id = ?;");
  nested.Bind(1, payload.id);
  const auto references = nested.Step() ? nested.ColumnInt(0) : 0;
  Require(references == 0, "Sequence " + payload.id + " is nested inside " + std::to_string(references) +
                               " clip(s); remove them first");
  Statement statement(database, "DELETE FROM sequences WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope& command, const commands::AddTrackPayload& payload) {
  RequireAbsent(database, kFindTrack, payload.id, "Track");
  RequireExists(database, kFindSequence, payload.sequence_id, "sequence");
  const auto kind = command.type == commands::CommandType::AddVideoTrack ? model::TrackKind::Video
                                                                         : model::TrackKind::Audio;
  Statement statement(database, R"sql(
    INSERT INTO tracks(id, sequence_id, track_type, sort_order, locked, muted, solo,
                       channel_layout, gain_db, pan, name)
    VALUES(?, ?, ?, ?, 0, 0, 0, ?, 0.0, 0.0, ?);
  )sql");
  statement.Bind(1, payload.id)
      .Bind(2, payload.sequence_id)
      .Bind(3, model::ToString(kind))
      .Bind(4, payload.order)
      .Bind(5, payload.channel_layout)
      .Bind(6, payload.name);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveTrackPayload& payload) {
  RequireExists(database, kFindTrack, payload.id, "track");
  RequireTrackUnlocked(database, payload.id);
  // Clips, transitions, and their effects cascade. The changeset records every
  // cascaded row, so undo restores the whole track with its contents.
  Statement statement(database, "DELETE FROM tracks WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetTrackStatePayload& payload) {
  RequireExists(database, kFindTrack, payload.id, "track");
  Statement statement(database, R"sql(
    UPDATE tracks SET locked = ?, muted = ?, solo = ?, gain_db = ?, pan = ?, name = ? WHERE id = ?;
  )sql");
  statement.Bind(1, payload.locked)
      .Bind(2, payload.muted)
      .Bind(3, payload.solo)
      .Bind(4, payload.gain_db)
      .Bind(5, payload.pan)
      .Bind(6, payload.name)
      .Bind(7, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetTrackRoutingPayload& payload) {
  RequireExists(database, kFindTrack, payload.id, "track");
  RequireTrackUnlocked(database, payload.id);
  Require(TrackKindOf(database, payload.id) == model::TrackKind::Audio, "Only audio tracks have routing");

  std::string sequence_id;
  {
    Statement statement(database, "SELECT sequence_id FROM tracks WHERE id = ?;");
    statement.Bind(1, payload.id);
    (void)statement.Step();
    sequence_id = statement.ColumnText(0);
  }
  const auto count = [&](const std::string& sql, const std::string& id) {
    Statement statement(database, sql);
    statement.Bind(1, id);
    (void)statement.Step();
    return statement.ColumnInt(0);
  };
  if (payload.is_bus) {
    Require(count("SELECT COUNT(*) FROM clips WHERE track_id = ?;", payload.id) == 0,
            "A track that holds clips cannot become a bus");
  } else {
    Require(count("SELECT COUNT(*) FROM tracks WHERE output_bus_id = ?;", payload.id) +
                    count("SELECT COUNT(*) FROM track_sends WHERE bus_id = ?;", payload.id) ==
                0,
            "Tracks are still routed to this bus");
  }

  // Everything the routing points at must be a bus of the same sequence.
  std::vector<std::string> targets;
  if (!payload.output_bus_id.empty()) targets.push_back(payload.output_bus_id);
  for (const auto& send : payload.sends) targets.push_back(send.bus_id);
  for (const auto& target : targets) {
    Statement statement(database, "SELECT sequence_id, is_bus FROM tracks WHERE id = ?;");
    statement.Bind(1, target);
    Require(statement.Step(), "Routing target " + target + " does not exist");
    Require(statement.ColumnText(0) == sequence_id, "Routing target " + target + " is in another sequence");
    Require(statement.ColumnInt(1) != 0, "Routing target " + target + " is not a bus");
  }

  // The routing as a graph, with this track's new edges in place of its old ones,
  // must not lead from the track back to itself.
  std::map<std::string, std::vector<std::string>> edges;
  {
    Statement statement(database,
                        "SELECT id, output_bus_id FROM tracks WHERE sequence_id = ? AND output_bus_id IS NOT NULL;");
    statement.Bind(1, sequence_id);
    while (statement.Step()) edges[statement.ColumnText(0)].push_back(statement.ColumnText(1));
  }
  {
    Statement statement(database,
                        "SELECT s.track_id, s.bus_id FROM track_sends s JOIN tracks t ON t.id = s.track_id "
                        "WHERE t.sequence_id = ?;");
    statement.Bind(1, sequence_id);
    while (statement.Step()) edges[statement.ColumnText(0)].push_back(statement.ColumnText(1));
  }
  edges[payload.id] = targets;
  std::set<std::string> visited;
  std::vector<std::string> pending = targets;
  while (!pending.empty()) {
    const auto next = pending.back();
    pending.pop_back();
    Require(next != payload.id, "That routing would feed the track back into itself");
    if (!visited.insert(next).second) continue;
    const auto found = edges.find(next);
    if (found != edges.end()) pending.insert(pending.end(), found->second.begin(), found->second.end());
  }

  {
    Statement statement(database, "UPDATE tracks SET is_bus = ?, output_bus_id = ? WHERE id = ?;");
    statement.Bind(1, payload.is_bus);
    if (payload.output_bus_id.empty()) {
      statement.BindNull(2);
    } else {
      statement.Bind(2, payload.output_bus_id);
    }
    statement.Bind(3, payload.id);
    statement.Run();
  }
  {
    Statement statement(database, "DELETE FROM track_sends WHERE track_id = ?;");
    statement.Bind(1, payload.id);
    statement.Run();
  }
  for (const auto& send : payload.sends) {
    Statement statement(database, "INSERT INTO track_sends(track_id, bus_id, gain_db, pre_fader) VALUES(?, ?, ?, ?);");
    statement.Bind(1, payload.id).Bind(2, send.bus_id).Bind(3, send.gain_db).Bind(4, send.pre_fader);
    statement.Run();
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SaveTrackingDataPayload& payload) {
  RequireExists(database, kFindClip, payload.clip_id, "clip");
  Statement statement(database, R"sql(
    INSERT INTO tracking_data(id, clip_id, kind, name, algorithm, source_fingerprint, parameters_json, data_json)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(id) DO UPDATE SET clip_id = excluded.clip_id, kind = excluded.kind, name = excluded.name,
      algorithm = excluded.algorithm, source_fingerprint = excluded.source_fingerprint,
      parameters_json = excluded.parameters_json, data_json = excluded.data_json;
  )sql");
  statement.Bind(1, payload.id)
      .Bind(2, payload.clip_id)
      .Bind(3, payload.kind)
      .Bind(4, payload.name)
      .Bind(5, payload.algorithm)
      .Bind(6, payload.source_fingerprint)
      .Bind(7, payload.parameters_json)
      .Bind(8, payload.data_json);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::DeleteTrackingDataPayload& payload) {
  RequireExists(database, "SELECT 1 FROM tracking_data WHERE id = ?;", payload.id, "tracking data");
  Statement statement(database, "DELETE FROM tracking_data WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::AddCaptionTrackPayload& payload) {
  RequireAbsent(database, "SELECT 1 FROM caption_tracks WHERE id = ?;", payload.id, "Caption track");
  RequireExists(database, kFindSequence, payload.sequence_id, "sequence");
  std::int64_t order = 0;
  {
    Statement statement(database, "SELECT COALESCE(MAX(sort_order) + 1, 0) FROM caption_tracks WHERE sequence_id = ?;");
    statement.Bind(1, payload.sequence_id);
    (void)statement.Step();
    order = statement.ColumnInt(0);
  }
  Statement statement(database, "INSERT INTO caption_tracks(id, sequence_id, name, language, style_json, sort_order) VALUES(?, ?, ?, ?, ?, ?);");
  statement.Bind(1, payload.id).Bind(2, payload.sequence_id).Bind(3, payload.name).Bind(4, payload.language).Bind(5, payload.style_json).Bind(6, order);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveCaptionTrackPayload& payload) {
  RequireExists(database, "SELECT 1 FROM caption_tracks WHERE id = ?;", payload.id, "caption track");
  Statement statement(database, "DELETE FROM caption_tracks WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::UpdateCaptionTrackPayload& payload) {
  RequireExists(database, "SELECT 1 FROM caption_tracks WHERE id = ?;", payload.id, "caption track");
  Statement statement(database, "UPDATE caption_tracks SET name = ?, language = ?, style_json = ? WHERE id = ?;");
  statement.Bind(1, payload.name).Bind(2, payload.language).Bind(3, payload.style_json).Bind(4, payload.id);
  statement.Run();
}

void InsertCue(sqlite3* database, const std::string& track_id, const commands::CaptionCuePayload& cue) {
  const auto start = Columns(cue.start);
  const auto end = Columns(cue.end);
  Statement statement(database, R"sql(
    INSERT INTO captions(id, track_id, start_num, start_den, end_num, end_den, start_ticks, end_ticks, text, style_json, speaker)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  statement.Bind(1, cue.id)
      .Bind(2, track_id)
      .Bind(3, start.numerator)
      .Bind(4, start.denominator)
      .Bind(5, end.numerator)
      .Bind(6, end.denominator)
      .Bind(7, start.ticks)
      .Bind(8, end.ticks)
      .Bind(9, cue.text)
      .Bind(10, cue.style_json)
      .Bind(11, cue.speaker);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::AddCaptionsPayload& payload) {
  RequireExists(database, "SELECT 1 FROM caption_tracks WHERE id = ?;", payload.track_id, "caption track");
  for (const auto& cue : payload.cues) {
    RequireAbsent(database, "SELECT 1 FROM captions WHERE id = ?;", cue.id, "Caption");
    InsertCue(database, payload.track_id, cue);
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::UpdateCaptionPayload& payload) {
  RequireExists(database, "SELECT 1 FROM captions WHERE id = ?;", payload.cue.id, "caption");
  const auto start = Columns(payload.cue.start);
  const auto end = Columns(payload.cue.end);
  Statement statement(database, R"sql(
    UPDATE captions SET start_num = ?, start_den = ?, end_num = ?, end_den = ?, start_ticks = ?, end_ticks = ?,
                        text = ?, style_json = ?, speaker = ?
     WHERE id = ?;
  )sql");
  statement.Bind(1, start.numerator)
      .Bind(2, start.denominator)
      .Bind(3, end.numerator)
      .Bind(4, end.denominator)
      .Bind(5, start.ticks)
      .Bind(6, end.ticks)
      .Bind(7, payload.cue.text)
      .Bind(8, payload.cue.style_json)
      .Bind(9, payload.cue.speaker)
      .Bind(10, payload.cue.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveCaptionsPayload& payload) {
  for (const auto& id : payload.ids) {
    RequireExists(database, "SELECT 1 FROM captions WHERE id = ?;", id, "caption");
    Statement statement(database, "DELETE FROM captions WHERE id = ?;");
    statement.Bind(1, id);
    statement.Run();
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::InsertClipPayload& payload) {
  RequireAbsent(database, kFindClip, payload.id, "Clip");
  RequireExists(database, kFindTrack, payload.track_id, "track");
  RequireTrackUnlocked(database, payload.track_id);
  RequireNotBus(database, payload.track_id);

  switch (payload.source_kind) {
    case model::SourceKind::Media:
      RequireExists(database, kFindMedia, *payload.media_id, "media");
      RequireWithinMedia(database, payload.media_id, payload.source_in, payload.source_out);
      break;
    case model::SourceKind::Sequence:
      RequireExists(database, kFindSequence, *payload.nested_sequence_id, "nested sequence");
      RequireNoNestingCycle(database, payload.track_id, *payload.nested_sequence_id);
      break;
    case model::SourceKind::Adjustment:
      // An adjustment clip only needs a timeline range, which is checked below.
      Require(TrackKindOf(database, payload.track_id) == model::TrackKind::Video,
              "An adjustment clip can only sit on a video track");
      break;
  }

  const auto duration = payload.source_out.Subtract(payload.source_in).Divide(payload.playback_rate);
  const auto start = Columns(payload.timeline_start);
  const auto end = Columns(payload.timeline_start.Add(duration));
  RequireNoOverlap(database, payload.track_id, start.ticks, end.ticks, payload.id);

  Statement statement(database, R"sql(
    INSERT INTO clips(id, track_id, source_kind, media_id, nested_sequence_id,
                      source_in_num, source_in_den, source_out_num, source_out_den,
                      timeline_start_num, timeline_start_den, timeline_start_ticks, timeline_end_ticks,
                      rate_num, rate_den, reversed, linked_group, enabled, name, maintain_pitch, audio_role)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?);
  )sql");
  const auto source_in = Columns(payload.source_in);
  const auto source_out = Columns(payload.source_out);
  statement.Bind(1, payload.id)
      .Bind(2, payload.track_id)
      .Bind(3, model::ToString(payload.source_kind))
      .BindOptional(4, payload.media_id)
      .BindOptional(5, payload.nested_sequence_id)
      .Bind(6, source_in.numerator)
      .Bind(7, source_in.denominator)
      .Bind(8, source_out.numerator)
      .Bind(9, source_out.denominator)
      .Bind(10, start.numerator)
      .Bind(11, start.denominator)
      .Bind(12, start.ticks)
      .Bind(13, end.ticks)
      .Bind(14, payload.playback_rate.numerator())
      .Bind(15, payload.playback_rate.denominator())
      .Bind(16, payload.reversed)
      .Bind(17, payload.linked_group)
      .Bind(18, payload.name)
      .Bind(19, payload.maintain_pitch)
      .Bind(20, payload.audio_role);
  statement.Run();
}

void DeleteClipOne(sqlite3* database, const commands::DeleteClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireTrackUnlocked(database, clip.track_id);
  Statement statement(database, "DELETE FROM clips WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void RippleDeleteClipOne(sqlite3* database, const commands::RippleDeleteClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireTrackUnlocked(database, clip.track_id);
  const auto duration = clip.TimelineDuration();
  const auto gap_end_ticks = clip.TimelineEnd().ToTicks();

  Statement remove(database, "DELETE FROM clips WHERE id = ?;");
  remove.Bind(1, payload.id);
  remove.Run();

  // Everything after the gap slides back by the clip's timeline duration.
  // Rational arithmetic cannot be done in SQL, so the rows are read, shifted in
  // exact rational space, and written back.
  struct Shift final {
    std::string id;
    time::RationalTime start;
  };
  std::vector<Shift> clips;
  {
    Statement later(database, R"sql(
      SELECT id, timeline_start_num, timeline_start_den
        FROM clips WHERE track_id = ? AND timeline_start_ticks >= ?;
    )sql");
    later.Bind(1, clip.track_id).Bind(2, gap_end_ticks);
    while (later.Step()) {
      clips.push_back({later.ColumnText(0), {later.ColumnInt(1), later.ColumnInt(2)}});
    }
  }
  std::set<std::string> moved_clips;
  Statement move(database, R"sql(
    UPDATE clips SET timeline_start_num = ?, timeline_start_den = ?,
                     timeline_start_ticks = ?, timeline_end_ticks = ?
     WHERE id = ?;
  )sql");
  for (const auto& entry : clips) {
    moved_clips.insert(entry.id);
    const auto shifted = entry.start.Subtract(duration);
    const auto existing = LoadClip(database, entry.id);
    const auto start = Columns(shifted);
    const auto end = Columns(shifted.Add(existing.TimelineDuration()));
    move.Reset();
    move.Bind(1, start.numerator)
        .Bind(2, start.denominator)
        .Bind(3, start.ticks)
        .Bind(4, end.ticks)
        .Bind(5, entry.id);
    move.Run();
  }

  // Transitions ride along with the clips they are attached to. This used to move
  // every transition whose *start* was at or after the gap, which misses one that
  // starts before its clip -- a centre-aligned fade-in begins half a transition
  // earlier than the clip it fades in -- and left it behind while the clip moved.
  struct TransitionShift final {
    std::string id;
    time::RationalTime start;
    time::RationalTime length;
  };
  std::vector<TransitionShift> transitions;
  {
    Statement attached(database, R"sql(
      SELECT id, timeline_start_num, timeline_start_den, duration_num, duration_den, from_clip_id, to_clip_id
        FROM transitions WHERE track_id = ?;
    )sql");
    attached.Bind(1, clip.track_id);
    while (attached.Step()) {
      const auto from = attached.ColumnOptionalText(5);
      const auto to = attached.ColumnOptionalText(6);
      const bool follows = (from.has_value() && moved_clips.count(*from) != 0) ||
                           (to.has_value() && moved_clips.count(*to) != 0);
      if (!follows) continue;
      transitions.push_back({attached.ColumnText(0),
                             {attached.ColumnInt(1), attached.ColumnInt(2)},
                             {attached.ColumnInt(3), attached.ColumnInt(4)}});
    }
  }
  Statement shift(database, R"sql(
    UPDATE transitions SET timeline_start_num = ?, timeline_start_den = ?,
                           timeline_start_ticks = ?, timeline_end_ticks = ?
     WHERE id = ?;
  )sql");
  for (const auto& entry : transitions) {
    const auto shifted = entry.start.Subtract(duration);
    const auto start = Columns(shifted);
    const auto end = shifted.Add(entry.length).ToTicks();
    shift.Reset();
    shift.Bind(1, start.numerator).Bind(2, start.denominator).Bind(3, start.ticks).Bind(4, end).Bind(5, entry.id);
    shift.Run();
  }
  DetachBrokenTransitions(database, std::vector<std::string>(moved_clips.begin(), moved_clips.end()));
}

void MoveClipOne(sqlite3* database, const commands::MoveClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireExists(database, kFindTrack, payload.track_id, "track");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "Clip cannot start before the sequence");
  RequireTrackUnlocked(database, clip.track_id);
  if (payload.track_id != clip.track_id) {
    RequireTrackUnlocked(database, payload.track_id);
    RequireNotBus(database, payload.track_id);
  }
  // A clip cannot change medium: moving video onto an audio track is a
  // different operation, not a move.
  Require(TrackKindOf(database, payload.track_id) == TrackKindOf(database, clip.track_id),
          "A clip cannot move between video and audio tracks");

  const auto start = Columns(payload.timeline_start);
  const auto end = Columns(payload.timeline_start.Add(clip.TimelineDuration()));
  RequireNoOverlap(database, payload.track_id, start.ticks, end.ticks, payload.id);

  Statement statement(database, R"sql(
    UPDATE clips SET track_id = ?, timeline_start_num = ?, timeline_start_den = ?,
                     timeline_start_ticks = ?, timeline_end_ticks = ?
     WHERE id = ?;
  )sql");
  statement.Bind(1, payload.track_id)
      .Bind(2, start.numerator)
      .Bind(3, start.denominator)
      .Bind(4, start.ticks)
      .Bind(5, end.ticks)
      .Bind(6, payload.id);
  statement.Run();
  // Moving a clip off the cut it was joined at, or onto another track, leaves its
  // transitions with nothing to join.
  DetachBrokenTransitions(database, {payload.id});
}

// A clip with a time-remap curve plays its source along that curve, not at a steady rate.
[[nodiscard]] bool HasTimeRemap(sqlite3* database, const std::string& clip_id) {
  Statement statement(database, "SELECT 1 FROM effects WHERE owner_kind = 'clip' AND owner_id = ? AND effect_type = 'time_remap' AND enabled = 1;");
  statement.Bind(1, clip_id);
  return statement.Step();
}

void SplitClipOne(sqlite3* database, const commands::SplitClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireTrackUnlocked(database, clip.track_id);
  RequireAbsent(database, kFindClip, payload.new_clip_id, "Clip");
  const auto end = clip.TimelineEnd();
  Require(payload.at.Compare(clip.timeline_start) > 0 && payload.at.Compare(end) < 0,
          "Split point must fall inside the clip");

  // How much source the left half consumes: the timeline time before the cut,
  // times the clip's rate.
  const auto consumed = payload.at.Subtract(clip.timeline_start).Multiply(clip.playback_rate);

  // A forward clip reads its source from the in point upwards, so the left half
  // takes the head of the range and the right half the tail. A reversed clip
  // reads from the out point *downwards*, so it is the other way round: the left
  // half plays the tail of the range (backwards) and the right half the head.
  // Applying the forward mapping to a reversed clip swapped what played before and
  // after the cut -- a cut at 4 s changed source 9 s into source 3 s.
  // A clip with a ramp is different: both halves keep the whole window the curve indexes into, and the
  // curve is divided at the cut, so each half still plays exactly what it played. What changes is
  // how long each is, and the rate that makes the window fit that length.
  const bool remapped = HasTimeRemap(database, clip.id);
  const auto window = clip.source_out.Subtract(clip.source_in);
  const auto left_rate = remapped ? window.Divide(payload.at.Subtract(clip.timeline_start)) : clip.playback_rate;
  const auto right_rate = remapped ? window.Divide(end.Subtract(payload.at)) : clip.playback_rate;
  const auto left_in = remapped ? clip.source_in : clip.reversed ? clip.source_out.Subtract(consumed) : clip.source_in;
  const auto left_out_time = remapped ? clip.source_out : clip.reversed ? clip.source_out : clip.source_in.Add(consumed);
  const auto right_in_time = remapped ? clip.source_in : clip.reversed ? clip.source_in : clip.source_in.Add(consumed);
  const auto right_out_time = remapped ? clip.source_out : clip.reversed ? clip.source_out.Subtract(consumed) : clip.source_out;

  const auto left_in_cols = Columns(left_in);
  const auto left_out = Columns(left_out_time);
  Statement shorten(database, R"sql(
    UPDATE clips SET source_in_num = ?, source_in_den = ?, source_out_num = ?, source_out_den = ?,
                     timeline_end_ticks = ?, rate_num = ?, rate_den = ?
     WHERE id = ?;
  )sql");
  shorten.Bind(1, left_in_cols.numerator)
      .Bind(2, left_in_cols.denominator)
      .Bind(3, left_out.numerator)
      .Bind(4, left_out.denominator)
      .Bind(5, payload.at.ToTicks())
      .Bind(6, left_rate.numerator())
      .Bind(7, left_rate.denominator())
      .Bind(8, clip.id);
  shorten.Run();

  const auto right_in = Columns(right_in_time);
  const auto right_out = Columns(right_out_time);
  const auto right_start = Columns(payload.at);
  const auto right_end = Columns(end);
  Statement insert(database, R"sql(
    INSERT INTO clips(id, track_id, source_kind, media_id, nested_sequence_id,
                      source_in_num, source_in_den, source_out_num, source_out_den,
                      timeline_start_num, timeline_start_den, timeline_start_ticks, timeline_end_ticks,
                      rate_num, rate_den, reversed, linked_group, enabled, name, maintain_pitch, audio_role)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  insert.Bind(1, payload.new_clip_id)
      .Bind(2, clip.track_id)
      .Bind(3, model::ToString(clip.source_kind))
      .BindOptional(4, clip.media_id)
      .BindOptional(5, clip.nested_sequence_id)
      .Bind(6, right_in.numerator)
      .Bind(7, right_in.denominator)
      .Bind(8, right_out.numerator)
      .Bind(9, right_out.denominator)
      .Bind(10, right_start.numerator)
      .Bind(11, right_start.denominator)
      .Bind(12, right_start.ticks)
      .Bind(13, right_end.ticks)
      .Bind(14, right_rate.numerator())
      .Bind(15, right_rate.denominator())
      .Bind(16, clip.reversed)
      .Bind(17, clip.linked_group)
      .Bind(18, clip.enabled)
      .Bind(19, clip.name)
      .Bind(20, clip.maintain_pitch)
      .Bind(21, clip.audio_role);
  insert.Run();

  // Where the cut falls in clip-local time, which is how keyframes are stored.
  const auto offset = payload.at.Subtract(clip.timeline_start);

  // Transitions at the end of the clip belong to whichever half now ends there.
  // The original id stays on the left half, so an outgoing transition has to be
  // moved to the right half or it would point at a clip that no longer reaches
  // the cut. An incoming transition is at the start, which the left half keeps.
  {
    Statement reattach(database, "UPDATE transitions SET from_clip_id = ? WHERE from_clip_id = ?;");
    reattach.Bind(1, payload.new_clip_id).Bind(2, clip.id);
    reattach.Run();
  }

  // The right-hand half inherits the clip's effect stack, including keyframes,
  // which is what an editor expects a razor cut to do.
  Statement effects(database,
                    "SELECT id, effect_type, sort_order, enabled, intrinsic, preset_name"
                    "  FROM effects WHERE owner_kind = 'clip' AND owner_id = ? ORDER BY sort_order;");
  effects.Bind(1, clip.id);
  struct EffectCopy final {
    std::string id;
    std::string effect_type;
    std::int64_t order;
    bool enabled;
    bool intrinsic;
    std::string preset;
  };
  std::vector<EffectCopy> stack;
  while (effects.Step()) {
    stack.push_back({effects.ColumnText(0), effects.ColumnText(1), effects.ColumnInt(2), effects.ColumnInt(3) != 0,
                     effects.ColumnInt(4) != 0, effects.ColumnText(5)});
  }
  for (const auto& effect : stack) {
    const auto new_effect_id = payload.new_clip_id + ":" + effect.id;
    Statement copy(database, R"sql(
      INSERT INTO effects(id, owner_kind, owner_id, effect_type, sort_order, enabled, intrinsic, preset_name)
      VALUES(?, 'clip', ?, ?, ?, ?, ?, ?);
    )sql");
    copy.Bind(1, new_effect_id)
        .Bind(2, payload.new_clip_id)
        .Bind(3, effect.effect_type)
        .Bind(4, effect.order)
        .Bind(5, effect.enabled)
        .Bind(6, effect.intrinsic)
        .Bind(7, effect.preset);
    copy.Run();

    Statement parameters(database,
                         "INSERT INTO effect_parameters(id, effect_id, name, dimension, c0, c1, c2, c3)"
                         " SELECT ? || ':' || name, ?, name, dimension, c0, c1, c2, c3"
                         "   FROM effect_parameters WHERE effect_id = ?;");
    parameters.Bind(1, new_effect_id).Bind(2, new_effect_id).Bind(3, effect.id);
    parameters.Run();

    // Divide each parameter's animation at the cut. Keyframe times are relative to
    // the start of the clip, and the right half starts later, so copying them
    // unchanged restarted its animation; the curves are split instead so that each
    // half reproduces the original exactly (see anim::SplitKeyframes).
    {
      Statement owned(database, "SELECT id, name, dimension FROM effect_parameters WHERE effect_id = ?;");
      owned.Bind(1, effect.id);
      struct Owned final {
        std::string id;
        std::string name;
        int dimension;
      };
      std::vector<Owned> parameter_rows;
      while (owned.Step()) {
        parameter_rows.push_back({owned.ColumnText(0), owned.ColumnText(1), static_cast<int>(owned.ColumnInt(2))});
      }
      for (const auto& parameter_row : parameter_rows) {
        auto keys = LoadKeyframes(database, parameter_row.id, parameter_row.dimension);
        if (keys.empty()) continue;
        const auto split = anim::SplitKeyframes(keys, offset);
        ReplaceKeyframes(database, parameter_row.id, split.left);
        ReplaceKeyframes(database, new_effect_id + ":" + parameter_row.name, split.right);
      }
    }

    // Masks are effect-owned and use the same clip-local clock as parameter
    // animation. Split each document so neither half restarts its geometry.
    {
      Statement masks(database, "SELECT id, sort_order, document_json FROM effect_masks WHERE effect_id = ? ORDER BY sort_order;");
      masks.Bind(1, effect.id);
      struct MaskCopy final { std::string id; std::int64_t order; std::string json; };
      std::vector<MaskCopy> rows;
      while (masks.Step()) rows.push_back({masks.ColumnText(0), masks.ColumnInt(1), masks.ColumnText(2)});
      const auto seconds = static_cast<double>(offset.numerator()) / static_cast<double>(offset.denominator());
      for (const auto& row : rows) {
        const auto halves = effects::mask::Split(effects::mask::Parse(row.json), seconds);
        Statement update(database, "UPDATE effect_masks SET document_json = ? WHERE id = ?;");
        update.Bind(1, effects::mask::ToJson(halves.first)).Bind(2, row.id);
        update.Run();
        Statement mask_insert(database, "INSERT INTO effect_masks(id, effect_id, sort_order, document_json) VALUES(?, ?, ?, ?);");
        mask_insert.Bind(1, new_effect_id + ":" + row.id).Bind(2, new_effect_id).Bind(3, row.order)
            .Bind(4, effects::mask::ToJson(halves.second));
        mask_insert.Run();
      }
    }
  }
  // A safety net: after the re-attachment above nothing should be broken, but a
  // transition that is must not survive the edit.
  DetachBrokenTransitions(database, {clip.id, payload.new_clip_id});
}

void TrimClipOne(sqlite3* database, const commands::TrimClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireTrackUnlocked(database, clip.track_id);
  Require(!HasTimeRemap(database, clip.id), "Clip " + clip.id + " has a speed ramp, which a trim would break: remove the ramp or set a new one");
  if (clip.source_kind == model::SourceKind::Media) {
    RequireWithinMedia(database, clip.media_id, payload.source_in, payload.source_out);
  }
  const auto duration = payload.source_out.Subtract(payload.source_in).Divide(clip.playback_rate);
  const auto start = Columns(payload.timeline_start);
  const auto end = Columns(payload.timeline_start.Add(duration));
  RequireNoOverlap(database, clip.track_id, start.ticks, end.ticks, clip.id);

  const auto source_in = Columns(payload.source_in);
  const auto source_out = Columns(payload.source_out);
  Statement statement(database, R"sql(
    UPDATE clips SET source_in_num = ?, source_in_den = ?, source_out_num = ?, source_out_den = ?,
                     timeline_start_num = ?, timeline_start_den = ?,
                     timeline_start_ticks = ?, timeline_end_ticks = ?
     WHERE id = ?;
  )sql");
  statement.Bind(1, source_in.numerator)
      .Bind(2, source_in.denominator)
      .Bind(3, source_out.numerator)
      .Bind(4, source_out.denominator)
      .Bind(5, start.numerator)
      .Bind(6, start.denominator)
      .Bind(7, start.ticks)
      .Bind(8, end.ticks)
      .Bind(9, payload.id);
  statement.Run();

  // Keyframes belong to the clip's local time, which starts at its first frame. A
  // head trim moves that origin without moving the picture: the same source frame
  // plays at the same timeline time, so an animation left alone would slide
  // against it. When the trim pins the content (the source in point moved exactly
  // as far as the timeline start did, at the clip's rate), the keyframes are moved
  // back by the amount the start moved, which keeps every one of them on the
  // frame it was on. A slip, a tail trim and a move leave them alone.
  const auto moved = payload.timeline_start.Subtract(clip.timeline_start);
  if (moved.Compare({0, 1}) != 0) {
    const auto head_change = clip.reversed ? clip.source_out.Subtract(payload.source_out)
                                           : payload.source_in.Subtract(clip.source_in);
    if (head_change.Compare(moved.Multiply(clip.playback_rate)) == 0) {
      ShiftClipKeyframes(database, clip.id, time::RationalTime(0, 1).Subtract(moved));
      ShiftClipMasks(database, clip.id, time::RationalTime(0, 1).Subtract(moved));
    }
  }
  // Trimming the edge a transition is joined at opens a gap there; the far edge
  // and a slip leave the cut where it was and keep it.
  DetachBrokenTransitions(database, {payload.id});
}

void SetClipEnabledOne(sqlite3* database, const commands::SetClipEnabledPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireTrackUnlocked(database, clip.track_id);
  Statement statement(database, "UPDATE clips SET enabled = ? WHERE id = ?;");
  statement.Bind(1, payload.enabled).Bind(2, payload.id);
  statement.Run();
}

void SetClipSpeedOne(sqlite3* database, const commands::SetClipSpeedPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireTrackUnlocked(database, clip.track_id);
  Require(!HasTimeRemap(database, clip.id), "Clip " + clip.id + " has a speed ramp: remove it before setting a steady speed");
  // Changing speed changes how much timeline the clip occupies, so the new
  // extent has to be checked against its neighbours.
  const auto duration = clip.source_out.Subtract(clip.source_in).Divide(payload.playback_rate);
  const auto start = Columns(clip.timeline_start);
  const auto end = Columns(clip.timeline_start.Add(duration));
  RequireNoOverlap(database, clip.track_id, start.ticks, end.ticks, clip.id);

  Statement statement(database,
                      "UPDATE clips SET rate_num = ?, rate_den = ?, reversed = ?, timeline_end_ticks = ?,"
                      " maintain_pitch = ? WHERE id = ?;");
  statement.Bind(1, payload.playback_rate.numerator())
      .Bind(2, payload.playback_rate.denominator())
      .Bind(3, payload.reversed)
      .Bind(4, end.ticks)
      .Bind(5, payload.maintain_pitch)
      .Bind(6, payload.id);
  statement.Run();
  // A change of speed moves the clip's end, and with it any cut joined there.
  DetachBrokenTransitions(database, {payload.id});
}

// ------------------------------------------------------------ linked clips ----
//
// Clips that share a link group (the picture and sound of one shot) are edited
// together. Each command below applies its single-clip implementation to the clip
// named and, unless the caller cleared `propagate_links`, to every other member of
// its group, inside the one transaction: a member that cannot take the edit (its
// track is locked, it would overlap a neighbour, it has no more media) refuses the
// whole command and nothing changes.

[[nodiscard]] std::vector<ClipRow> LinkedPartners(sqlite3* database, const ClipRow& clip) {
  std::vector<ClipRow> partners;
  if (clip.linked_group.empty()) return partners;
  Statement statement(database,
                      "SELECT id FROM clips WHERE linked_group = ? AND id <> ? ORDER BY timeline_start_ticks, id;");
  statement.Bind(1, clip.linked_group).Bind(2, clip.id);
  std::vector<std::string> ids;
  while (statement.Step()) ids.push_back(statement.ColumnText(0));
  for (const auto& id : ids) partners.push_back(LoadClip(database, id));
  return partners;
}

void SetLinkedGroup(sqlite3* database, const std::string& clip_id, const std::string& group) {
  Statement statement(database, "UPDATE clips SET linked_group = ? WHERE id = ?;");
  statement.Bind(1, group).Bind(2, clip_id);
  statement.Run();
}

// Runs an edit on a linked partner, saying which clip refused it if it does.
template <typename Edit>
void OnPartner(const ClipRow& partner, Edit&& edit) {
  try {
    edit();
  } catch (const std::exception& error) {
    throw std::runtime_error("Linked clip " + partner.id + ": " + error.what());
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::DeleteClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  DeleteClipOne(database, payload);
  for (const auto& partner : partners) {
    OnPartner(partner, [&] { DeleteClipOne(database, {partner.id, false}); });
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RippleDeleteClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  RippleDeleteClipOne(database, payload);
  // Each track closes its own gap by the length of its own clip, so a pair of
  // equal length stays in step.
  for (const auto& partner : partners) {
    OnPartner(partner, [&] { RippleDeleteClipOne(database, {partner.id, false}); });
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::MoveClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  if (partners.empty()) {
    MoveClipOne(database, payload);
    return;
  }
  // Every member moves by the same amount of time; only the clip named may change
  // track. Members that share a track would collide with each other part-way
  // through if moved in the wrong order, so the ones leading the move go first.
  const auto shift = payload.timeline_start.Subtract(clip.timeline_start);
  struct Member final {
    ClipRow row;
    bool primary;
  };
  std::vector<Member> members;
  members.push_back({clip, true});
  for (auto& partner : partners) members.push_back({std::move(partner), false});
  const bool later = shift.Compare({0, 1}) >= 0;
  std::stable_sort(members.begin(), members.end(), [later](const Member& left, const Member& right) {
    const auto order = left.row.timeline_start.Compare(right.row.timeline_start);
    return later ? order > 0 : order < 0;
  });
  for (const auto& member : members) {
    if (member.primary) {
      MoveClipOne(database, payload);
    } else {
      OnPartner(member.row, [&] {
        MoveClipOne(database, {member.row.id, member.row.track_id, member.row.timeline_start.Add(shift), false});
      });
    }
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::TrimClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};

  // Which edges of the clip this trim moves, as changes in timeline time. A slip
  // moves neither and is the one trim that stays with the clip it was made on.
  const auto head = payload.timeline_start.Subtract(clip.timeline_start);
  const auto new_end =
      payload.timeline_start.Add(payload.source_out.Subtract(payload.source_in).Divide(clip.playback_rate));
  const auto tail = new_end.Subtract(clip.TimelineEnd());
  const auto head_change = clip.reversed ? clip.source_out.Subtract(payload.source_out)
                                         : payload.source_in.Subtract(clip.source_in);
  const bool head_trim = head.Compare({0, 1}) != 0 && head_change.Compare(head.Multiply(clip.playback_rate)) == 0;
  const bool tail_trim = tail.Compare({0, 1}) != 0;

  TrimClipOne(database, payload);
  if (!head_trim && !tail_trim) return;
  for (const auto& partner : partners) {
    OnPartner(partner, [&] {
      const auto head_shift = head_trim ? head : time::RationalTime(0, 1);
      const auto tail_shift = tail_trim ? tail : time::RationalTime(0, 1);
      const auto head_source = head_shift.Multiply(partner.playback_rate);
      const auto tail_source = tail_shift.Multiply(partner.playback_rate);
      // The head of a reversed clip is the end of its source range.
      const auto source_in = partner.reversed ? partner.source_in.Subtract(tail_source)
                                              : partner.source_in.Add(head_source);
      const auto source_out = partner.reversed ? partner.source_out.Subtract(head_source)
                                               : partner.source_out.Add(tail_source);
      TrimClipOne(database, {partner.id, source_in, source_out, partner.timeline_start.Add(head_shift), false});
    });
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SplitClipPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  SplitClipOne(database, payload);
  if (clip.linked_group.empty()) return;

  // The right-hand halves become a group of their own. Left as they were, all the
  // clips on both sides of the cut would still be one group, and an edit to any of
  // them would drag the rest of the take along.
  const auto right_group =
      payload.propagate_links ? clip.linked_group + "~" + payload.new_clip_id : std::string();
  SetLinkedGroup(database, payload.new_clip_id, right_group);
  for (const auto& partner : partners) {
    OnPartner(partner, [&] {
      const auto partner_end = partner.TimelineEnd();
      if (partner.timeline_start.Compare(payload.at) < 0 && partner_end.Compare(payload.at) > 0) {
        const auto right_id = payload.new_clip_id + "~" + partner.id;
        SplitClipOne(database, {partner.id, right_id, payload.at, false});
        SetLinkedGroup(database, right_id, right_group);
      } else if (partner.timeline_start.Compare(payload.at) >= 0) {
        // Wholly after the cut: it belongs with the right-hand halves.
        SetLinkedGroup(database, partner.id, right_group);
      }
      // Wholly before the cut: stays with the left-hand halves.
    });
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetClipAudioRolePayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  RequireTrackUnlocked(database, clip.track_id);
  Statement statement(database, "UPDATE clips SET audio_role = ? WHERE id = ?;");
  statement.Bind(1, payload.role).Bind(2, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetClipEnabledPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  SetClipEnabledOne(database, payload);
  for (const auto& partner : partners) {
    OnPartner(partner, [&] { SetClipEnabledOne(database, {partner.id, payload.enabled, false}); });
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetClipSpeedPayload& payload) {
  const auto clip = LoadClip(database, payload.id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  SetClipSpeedOne(database, payload);
  for (const auto& partner : partners) {
    OnPartner(partner, [&] {
      SetClipSpeedOne(database, {partner.id, payload.playback_rate, payload.reversed, false, payload.maintain_pitch});
    });
  }
}

// ------------------------------------------------------------- speed ramps ----

void SetSpeedRampOne(sqlite3* database, const commands::SetSpeedRampPayload& payload, const std::string& clip_id) {
  const auto clip = LoadClip(database, clip_id);
  RequireTrackUnlocked(database, clip.track_id);

  // The ramp laid out: the source position (seconds from where the clip starts reading) at the end of
  // each segment, found by integrating the speed, which changes linearly within a segment.
  time::RationalTime total(0, 1);
  for (const auto& s : payload.segments) total = total.Add(s.duration);
  const auto seconds = [](const time::RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator()); };
  const auto from_seconds = [](double value) {
    constexpr std::int64_t kScale = 1'000'000;
    return time::RationalTime(static_cast<std::int64_t>(std::llround(value * static_cast<double>(kScale))), kScale);
  };

  // The sequence's frame length sets how finely the curve is written: a key every frame is exact at the
  // frames, which is where it is looked at.
  time::RationalTime step(1, 30);
  {
    Statement rate(database, "SELECT s.frame_rate_num, s.frame_rate_den FROM sequences s JOIN tracks t ON t.sequence_id = s.id WHERE t.id = ?;");
    rate.Bind(1, clip.track_id);
    if (rate.Step()) step = time::RationalTime(rate.ColumnInt(1), rate.ColumnInt(0));
  }

  struct Key final {
    time::RationalTime at;
    double offset;
  };
  std::vector<Key> keys;
  keys.push_back({time::RationalTime(0, 1), 0.0});
  double position = 0.0, low = 0.0, high = 0.0;
  time::RationalTime segment_start(0, 1);
  for (const auto& s : payload.segments) {
    const auto length = seconds(s.duration);
    const auto position_at = [&](double t) { return position + s.start_speed * t + (s.end_speed - s.start_speed) * t * t / (2.0 * length); };
    // A frame-spaced key inside the segment, then its end.
    for (auto at = step; at.Compare(s.duration) < 0; at = at.Add(step)) {
      keys.push_back({segment_start.Add(at), position_at(seconds(at))});
    }
    // Where the speed passes through zero the picture turns round: that is the extreme of the path,
    // and the window has to reach it.
    if ((s.start_speed < 0.0) != (s.end_speed < 0.0) && s.start_speed != s.end_speed) {
      const auto turn = -s.start_speed * length / (s.end_speed - s.start_speed);
      if (turn > 0.0 && turn < length) {
        const auto value = position_at(turn);
        low = std::min(low, value);
        high = std::max(high, value);
        keys.push_back({segment_start.Add(from_seconds(turn)), value});
      }
    }
    position = position_at(length);
    segment_start = segment_start.Add(s.duration);
    keys.push_back({segment_start, position});
    low = std::min(low, position);
    high = std::max(high, position);
  }
  // Interior keys can lie beyond the segment ends only if the path overshoots, which the turning points
  // above have already accounted for; sweep once more to be certain of the window.
  for (const auto& key : keys) {
    low = std::min(low, key.offset);
    high = std::max(high, key.offset);
  }
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.at.Compare(b.at) < 0; });
  keys.erase(std::unique(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.at.Compare(b.at) == 0; }), keys.end());

  Require(high - low >= 0.001, "The ramp does not move through the source, so there is nothing to play");
  // Where the clip reads at its first frame. A clip that already has a ramp starts wherever that ramp did, which is not the
  // bottom of its window if the ramp went backwards; editing the ramp must keep the start where it was.
  auto anchor = clip.source_in;
  if (HasTimeRemap(database, clip.id)) {
    Statement first(database, R"sql(
      SELECT k.c0 FROM keyframes k JOIN effect_parameters p ON p.id = k.parameter_id JOIN effects e ON e.id = p.effect_id
       WHERE e.owner_kind = 'clip' AND e.owner_id = ? AND e.effect_type = 'time_remap' AND p.name = 'source_offset'
       ORDER BY k.time_ticks LIMIT 1;
    )sql");
    first.Bind(1, clip.id);
    if (first.Step()) anchor = clip.source_in.Add(from_seconds(first.ColumnDouble(0)));
  }
  const auto new_in = anchor.Add(from_seconds(low));
  const auto new_out = anchor.Add(from_seconds(high));
  Require(new_in.Compare({0, 1}) >= 0, "The ramp runs back past the start of the source");
  if (clip.source_kind == model::SourceKind::Media) RequireWithinMedia(database, clip.media_id, new_in, new_out);

  const auto window = new_out.Subtract(new_in);
  const auto rate = window.Divide(total);
  const auto start = Columns(clip.timeline_start);
  const auto end = Columns(clip.timeline_start.Add(total));
  RequireNoOverlap(database, clip.track_id, start.ticks, end.ticks, clip.id);

  const auto in_cols = Columns(new_in);
  const auto out_cols = Columns(new_out);
  Statement update(database, R"sql(
    UPDATE clips SET source_in_num = ?, source_in_den = ?, source_out_num = ?, source_out_den = ?,
                     rate_num = ?, rate_den = ?, reversed = 0, timeline_end_ticks = ?
     WHERE id = ?;
  )sql");
  update.Bind(1, in_cols.numerator)
      .Bind(2, in_cols.denominator)
      .Bind(3, out_cols.numerator)
      .Bind(4, out_cols.denominator)
      .Bind(5, rate.numerator())
      .Bind(6, rate.denominator())
      .Bind(7, end.ticks)
      .Bind(8, clip.id);
  update.Run();

  // The curve itself: an effect on the clip with one parameter, keyed in clip-local time.
  const auto effect_id = clip.id + ":time_remap";
  const auto parameter_id = effect_id + ":source_offset";
  {
    Statement existing(database, "SELECT id FROM effects WHERE owner_kind = 'clip' AND owner_id = ? AND effect_type = 'time_remap';");
    existing.Bind(1, clip.id);
    std::vector<std::string> old;
    while (existing.Step()) old.push_back(existing.ColumnText(0));
    for (const auto& id : old) {
      Statement remove(database, "DELETE FROM effects WHERE id = ?;");
      remove.Bind(1, id);
      remove.Run();
    }
  }
  std::int64_t order = 0;
  {
    Statement next(database, "SELECT COALESCE(MAX(sort_order) + 1, 0) FROM effects WHERE owner_kind = 'clip' AND owner_id = ?;");
    next.Bind(1, clip.id);
    (void)next.Step();
    order = next.ColumnInt(0);
  }
  Statement effect(database, "INSERT INTO effects(id, owner_kind, owner_id, effect_type, sort_order, enabled, intrinsic, preset_name) VALUES(?, 'clip', ?, 'time_remap', ?, 1, 0, '');");
  effect.Bind(1, effect_id).Bind(2, clip.id).Bind(3, order);
  effect.Run();
  Statement parameter(database, "INSERT INTO effect_parameters(id, effect_id, name, dimension, c0, c1, c2, c3) VALUES(?, ?, 'source_offset', 1, 0, 0, 0, 0);");
  parameter.Bind(1, parameter_id).Bind(2, effect_id);
  parameter.Run();
  std::vector<anim::Keyframe> written;
  for (const auto& key : keys) {
    written.push_back({key.at, anim::Value::Scalar(std::clamp(key.offset - low, 0.0, high - low)), anim::Interpolation::Linear, {}, {}});
  }
  ReplaceKeyframes(database, parameter_id, written);
  DetachBrokenTransitions(database, {clip.id});
}

void ClearSpeedRampOne(sqlite3* database, const std::string& clip_id) {
  const auto clip = LoadClip(database, clip_id);
  RequireTrackUnlocked(database, clip.track_id);
  Require(HasTimeRemap(database, clip_id), "Clip " + clip_id + " has no speed ramp");
  Statement remove(database, "DELETE FROM effects WHERE owner_kind = 'clip' AND owner_id = ? AND effect_type = 'time_remap';");
  remove.Bind(1, clip_id);
  remove.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetSpeedRampPayload& payload) {
  const auto clip = LoadClip(database, payload.clip_id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  SetSpeedRampOne(database, payload, payload.clip_id);
  for (const auto& partner : partners) OnPartner(partner, [&] { SetSpeedRampOne(database, payload, partner.id); });
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::ClearSpeedRampPayload& payload) {
  const auto clip = LoadClip(database, payload.clip_id);
  const auto partners = payload.propagate_links ? LinkedPartners(database, clip) : std::vector<ClipRow>{};
  ClearSpeedRampOne(database, payload.clip_id);
  for (const auto& partner : partners) {
    // A partner without a ramp has nothing to clear and is left alone.
    if (HasTimeRemap(database, partner.id)) OnPartner(partner, [&] { ClearSpeedRampOne(database, partner.id); });
  }
}

// ------------------------------------------------------------- multicam ----

struct CutRow final {
  time::RationalTime at;
  std::string angle_id;
};

[[nodiscard]] std::vector<CutRow> LoadCuts(sqlite3* database, const std::string& group_id) {
  std::vector<CutRow> cuts;
  Statement statement(database, "SELECT time_num, time_den, angle_id FROM multicam_switches WHERE group_id = ? ORDER BY time_ticks;");
  statement.Bind(1, group_id);
  while (statement.Step()) cuts.push_back({{statement.ColumnInt(0), statement.ColumnInt(1)}, statement.ColumnText(2)});
  return cuts;
}

// Neighbouring cuts to the same angle are one cut.
void CoalesceCuts(std::vector<CutRow>& cuts) {
  for (std::size_t index = 1; index < cuts.size();) {
    if (cuts[index - 1].angle_id == cuts[index].angle_id) {
      cuts.erase(cuts.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
      ++index;
    }
  }
}

void StoreCuts(sqlite3* database, const std::string& group_id, const std::vector<CutRow>& cuts) {
  Statement clear(database, "DELETE FROM multicam_switches WHERE group_id = ?;");
  clear.Bind(1, group_id);
  clear.Run();
  Statement insert(database, "INSERT INTO multicam_switches(group_id, time_num, time_den, time_ticks, angle_id) VALUES(?, ?, ?, ?, ?);");
  for (const auto& cut : cuts) {
    const auto at = Columns(cut.at);
    insert.Reset();
    insert.Bind(1, group_id).Bind(2, at.numerator).Bind(3, at.denominator).Bind(4, at.ticks).Bind(5, cut.angle_id);
    insert.Run();
  }
}

[[nodiscard]] time::RationalTime GroupDuration(sqlite3* database, const std::string& group_id) {
  Statement statement(database, "SELECT duration_num, duration_den FROM multicam_groups WHERE id = ?;");
  statement.Bind(1, group_id);
  if (!statement.Step()) throw std::runtime_error("Unknown multicam group: " + group_id);
  return {statement.ColumnInt(0), statement.ColumnInt(1)};
}

constexpr std::string_view kFindMulticamGroup = "SELECT 1 FROM multicam_groups WHERE id = ?;";

void RequireAngle(sqlite3* database, const std::string& group_id, const std::string& angle_id) {
  Statement statement(database, "SELECT 1 FROM multicam_angles WHERE group_id = ? AND id = ?;");
  statement.Bind(1, group_id).Bind(2, angle_id);
  Require(statement.Step(), "Multicam group " + group_id + " has no angle " + angle_id);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::CreateMulticamGroupPayload& payload) {
  RequireAbsent(database, kFindMulticamGroup, payload.id, "Multicam group");
  const auto duration = Columns(payload.duration);
  Statement group(database, "INSERT INTO multicam_groups(id, name, duration_num, duration_den) VALUES(?, ?, ?, ?);");
  group.Bind(1, payload.id).Bind(2, payload.name).Bind(3, duration.numerator).Bind(4, duration.denominator);
  group.Run();
  std::int64_t order = 0;
  for (const auto& angle : payload.angles) {
    RequireExists(database, kFindMedia, angle.media_id, "media");
    Statement media(database, "SELECT start_timecode_num, start_timecode_den FROM media WHERE id = ?;");
    media.Bind(1, angle.media_id);
    Require(media.Step(), "Unknown media: " + angle.media_id);
    Statement insert(database, R"sql(
      INSERT INTO multicam_angles(group_id, id, sort_order, name, media_id, timecode_num, timecode_den, marker_num, marker_den)
      VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);
    )sql");
    insert.Bind(1, payload.id).Bind(2, angle.id).Bind(3, order++).Bind(4, angle.name).Bind(5, angle.media_id)
        .Bind(6, media.ColumnInt(0)).Bind(7, media.ColumnInt(1));
    if (angle.marker) {
      insert.Bind(8, angle.marker->numerator()).Bind(9, angle.marker->denominator());
    } else {
      insert.BindNull(8).BindNull(9);
    }
    insert.Run();
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetMulticamSyncPayload& payload) {
  RequireExists(database, kFindMulticamGroup, payload.group_id, "multicam group");
  if (!payload.reference_angle_id.empty()) RequireAngle(database, payload.group_id, payload.reference_angle_id);
  Statement group(database, "UPDATE multicam_groups SET sync_method = ?, reference_angle_id = ?, sync_confidence = ? WHERE id = ?;");
  group.Bind(1, payload.method).Bind(2, payload.reference_angle_id).Bind(3, payload.confidence).Bind(4, payload.group_id);
  group.Run();
  Statement update(database, "UPDATE multicam_angles SET offset_num = ?, offset_den = ? WHERE group_id = ? AND id = ?;");
  for (const auto& offset : payload.offsets) {
    RequireAngle(database, payload.group_id, offset.angle_id);
    update.Reset();
    update.Bind(1, offset.source_offset.numerator()).Bind(2, offset.source_offset.denominator()).Bind(3, payload.group_id).Bind(4, offset.angle_id);
    update.Run();
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RecordMulticamSwitchPayload& payload) {
  const auto duration = GroupDuration(database, payload.group_id);
  RequireAngle(database, payload.group_id, payload.angle_id);
  Require(payload.at.Compare(duration) < 0, "A multicam cut must be inside the group");
  auto cuts = LoadCuts(database, payload.group_id);
  const auto position = std::lower_bound(cuts.begin(), cuts.end(), payload.at,
                                         [](const CutRow& cut, const time::RationalTime& at) { return cut.at.Compare(at) < 0; });
  if (position != cuts.end() && position->at.Compare(payload.at) == 0) {
    position->angle_id = payload.angle_id;
  } else {
    cuts.insert(position, {payload.at, payload.angle_id});
  }
  CoalesceCuts(cuts);
  StoreCuts(database, payload.group_id, cuts);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveMulticamSwitchPayload& payload) {
  RequireExists(database, kFindMulticamGroup, payload.group_id, "multicam group");
  auto cuts = LoadCuts(database, payload.group_id);
  const auto found = std::find_if(cuts.begin(), cuts.end(), [&](const CutRow& cut) { return cut.at.Compare(payload.at) == 0; });
  Require(found != cuts.end(), "There is no multicam cut at that time");
  cuts.erase(found);
  CoalesceCuts(cuts);
  StoreCuts(database, payload.group_id, cuts);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RenameMulticamAnglePayload& payload) {
  RequireExists(database, kFindMulticamGroup, payload.group_id, "multicam group");
  RequireAngle(database, payload.group_id, payload.angle_id);
  Statement statement(database, "UPDATE multicam_angles SET name = ? WHERE group_id = ? AND id = ?;");
  statement.Bind(1, payload.name).Bind(2, payload.group_id).Bind(3, payload.angle_id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::DeleteMulticamGroupPayload& payload) {
  RequireExists(database, kFindMulticamGroup, payload.id, "multicam group");
  Statement statement(database, "DELETE FROM multicam_groups WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

struct AngleSource final {
  std::string media_id;
  time::RationalTime offset;
  time::RationalTime media_duration;
  bool has_audio{false};
};

[[nodiscard]] AngleSource LoadAngleSource(sqlite3* database, const std::string& group_id, const std::string& angle_id) {
  Statement statement(database, R"sql(
    SELECT a.media_id, a.offset_num, a.offset_den, m.duration_num, m.duration_den,
           EXISTS(SELECT 1 FROM media_streams s WHERE s.media_id = a.media_id AND s.kind = 'audio')
      FROM multicam_angles a JOIN media m ON m.id = a.media_id
     WHERE a.group_id = ? AND a.id = ?;
  )sql");
  statement.Bind(1, group_id).Bind(2, angle_id);
  Require(statement.Step(), "Multicam group " + group_id + " has no angle " + angle_id);
  return {statement.ColumnText(0), {statement.ColumnInt(1), statement.ColumnInt(2)}, {statement.ColumnInt(3), statement.ColumnInt(4)},
          statement.ColumnInt(5) != 0};
}

// The part of an angle that exists for [from, to) on the group's timeline, if any.
struct Visible final {
  time::RationalTime source_in;
  time::RationalTime source_out;
  time::RationalTime timeline_start;
};

[[nodiscard]] std::optional<Visible> VisibleRange(const AngleSource& angle, const time::RationalTime& from, const time::RationalTime& to) {
  const auto in = angle.offset.Add(from);
  const auto out = angle.offset.Add(to);
  const auto visible_in = in.Compare({0, 1}) < 0 ? time::RationalTime(0, 1) : in;
  const auto visible_out = out.Compare(angle.media_duration) > 0 ? angle.media_duration : out;
  if (visible_out.Compare(visible_in) <= 0) return std::nullopt;
  return Visible{visible_in, visible_out, from.Add(visible_in.Subtract(in))};
}

void Apply(sqlite3* database, const CommandEnvelope& command, const commands::FlattenMulticamGroupPayload& payload) {
  const auto duration = GroupDuration(database, payload.group_id);
  Require(TrackKindOf(database, payload.video_track_id) == model::TrackKind::Video, "A multicam picture goes on a video track");
  const bool with_audio = !payload.audio_track_id.empty();
  if (with_audio) Require(TrackKindOf(database, payload.audio_track_id) == model::TrackKind::Audio, "Multicam sound goes on an audio track");
  const auto cuts = LoadCuts(database, payload.group_id);
  Require(!cuts.empty(), "The multicam group has no cuts to flatten");
  const bool follow = payload.audio_angle_id.empty();
  if (with_audio && !follow) {
    RequireAngle(database, payload.group_id, payload.audio_angle_id);
    Require(LoadAngleSource(database, payload.group_id, payload.audio_angle_id).has_audio, "Angle " + payload.audio_angle_id + " has no sound");
  }

  const auto place = [&](const std::string& id, const std::string& track, const std::string& angle_id, const AngleSource& angle,
                         const Visible& visible, const std::string& link) {
    commands::InsertClipPayload clip;
    clip.id = id;
    clip.track_id = track;
    clip.source_kind = model::SourceKind::Media;
    clip.media_id = angle.media_id;
    clip.source_in = visible.source_in;
    clip.source_out = visible.source_out;
    clip.timeline_start = payload.timeline_start.Add(visible.timeline_start);
    clip.linked_group = link;
    clip.name = angle_id;
    Apply(database, command, clip);
  };

  for (std::size_t index = 0; index < cuts.size(); ++index) {
    const auto end = index + 1 < cuts.size() ? cuts[index + 1].at : duration;
    const auto angle = LoadAngleSource(database, payload.group_id, cuts[index].angle_id);
    const auto visible = VisibleRange(angle, cuts[index].at, end);
    if (!visible) continue;
    const auto number = std::to_string(index);
    const bool linked = with_audio && follow && angle.has_audio;
    const auto link = linked ? payload.id_prefix + ":link:" + number : std::string();
    place(payload.id_prefix + ":v:" + number, payload.video_track_id, cuts[index].angle_id, angle, *visible, link);
    if (linked) place(payload.id_prefix + ":a:" + number, payload.audio_track_id, cuts[index].angle_id, angle, *visible, link);
  }
  if (with_audio && !follow) {
    const auto angle = LoadAngleSource(database, payload.group_id, payload.audio_angle_id);
    const auto visible = VisibleRange(angle, cuts.front().at, duration);
    Require(visible.has_value(), "Angle " + payload.audio_angle_id + " has no sound within the cuts");
    place(payload.id_prefix + ":a:0", payload.audio_track_id, payload.audio_angle_id, angle, *visible, std::string());
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::LinkClipsPayload& payload) {
  for (const auto& id : payload.clip_ids) RequireTrackUnlocked(database, LoadClip(database, id).track_id);
  for (const auto& id : payload.clip_ids) SetLinkedGroup(database, id, payload.group_id);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::UnlinkClipsPayload& payload) {
  for (const auto& id : payload.clip_ids) RequireTrackUnlocked(database, LoadClip(database, id).track_id);
  for (const auto& id : payload.clip_ids) SetLinkedGroup(database, id, std::string());
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::AddTransitionPayload& payload) {
  RequireAbsent(database, kFindTransition, payload.id, "Transition");
  RequireExists(database, kFindTrack, payload.track_id, "track");
  RequireTrackUnlocked(database, payload.track_id);

  // Both sides must live on the track the transition is being added to, or the
  // compiler would be asked to mix sources that are never both active.
  const auto check_side = [&](const std::optional<std::string>& clip_id) {
    if (!clip_id.has_value()) return;
    const auto clip = LoadClip(database, *clip_id);
    Require(clip.track_id == payload.track_id,
            "Clip " + *clip_id + " is not on track " + payload.track_id);
  };
  check_side(payload.from_clip_id);
  check_side(payload.to_clip_id);

  const auto start = Columns(payload.timeline_start);
  const auto duration = Columns(payload.duration);
  const auto end_ticks = payload.timeline_start.Add(payload.duration).ToTicks();

  // Transitions on one track may not overlap each other.
  RequireNoTransitionOverlap(database, payload.track_id, start.ticks, end_ticks, payload.id);

  Statement statement(database, R"sql(
    INSERT INTO transitions(id, track_id, kind, alignment, from_clip_id, to_clip_id,
                            timeline_start_num, timeline_start_den, duration_num, duration_den,
                            timeline_start_ticks, timeline_end_ticks)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  statement.Bind(1, payload.id)
      .Bind(2, payload.track_id)
      .Bind(3, payload.kind)
      .Bind(4, model::ToString(payload.alignment))
      .BindOptional(5, payload.from_clip_id)
      .BindOptional(6, payload.to_clip_id)
      .Bind(7, start.numerator)
      .Bind(8, start.denominator)
      .Bind(9, duration.numerator)
      .Bind(10, duration.denominator)
      .Bind(11, start.ticks)
      .Bind(12, end_ticks);
  statement.Run();
  // Adjacency, covering the cut, and alignment. Checked on the stored row so the
  // rule is exactly the one ValidateDatabase applies; a violation throws and the
  // whole command, including this insert, rolls back.
  RequireValidTransition(database, payload.id);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveTransitionPayload& payload) {
  RequireExists(database, kFindTransition, payload.id, "transition");
  RequireTransitionUnlocked(database, payload.id);
  Statement statement(database, "DELETE FROM transitions WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetTransitionTimingPayload& payload) {
  RequireExists(database, kFindTransition, payload.id, "transition");
  RequireTransitionUnlocked(database, payload.id);
  const auto start = Columns(payload.timeline_start);
  const auto duration = Columns(payload.duration);
  const auto end_ticks = payload.timeline_start.Add(payload.duration).ToTicks();

  // The same checks creation makes: it may not run into a neighbouring
  // transition, and it still has to cover and be aligned to its cut.
  {
    Statement track(database, "SELECT track_id FROM transitions WHERE id = ?;");
    track.Bind(1, payload.id);
    if (track.Step()) RequireNoTransitionOverlap(database, track.ColumnText(0), start.ticks, end_ticks, payload.id);
  }
  Statement statement(database, R"sql(
    UPDATE transitions SET timeline_start_num = ?, timeline_start_den = ?,
                           duration_num = ?, duration_den = ?,
                           timeline_start_ticks = ?, timeline_end_ticks = ?
     WHERE id = ?;
  )sql");
  statement.Bind(1, start.numerator)
      .Bind(2, start.denominator)
      .Bind(3, duration.numerator)
      .Bind(4, duration.denominator)
      .Bind(5, start.ticks)
      .Bind(6, end_ticks)
      .Bind(7, payload.id);
  statement.Run();
  RequireValidTransition(database, payload.id);
}

void RequireEffectOwner(sqlite3* database, model::EffectOwner owner_kind, const std::string& owner_id) {
  switch (owner_kind) {
    case model::EffectOwner::Clip: RequireExists(database, kFindClip, owner_id, "clip"); return;
    case model::EffectOwner::Track: RequireExists(database, kFindTrack, owner_id, "track"); return;
    case model::EffectOwner::Sequence: RequireExists(database, kFindSequence, owner_id, "sequence"); return;
    case model::EffectOwner::Transition:
      RequireExists(database, kFindTransition, owner_id, "transition");
      return;
  }
  throw std::invalid_argument("Unhandled effect owner kind");
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::AddEffectPayload& payload) {
  RequireAbsent(database, kFindEffect, payload.id, "Effect");
  RequireEffectOwner(database, payload.owner_kind, payload.owner_id);
  RequireOwnerUnlocked(database, model::ToString(payload.owner_kind), payload.owner_id);

  Statement statement(database, R"sql(
    INSERT INTO effects(id, owner_kind, owner_id, effect_type, sort_order, enabled, intrinsic, preset_name)
    VALUES(?, ?, ?, ?, ?, 1, ?, ?);
  )sql");
  statement.Bind(1, payload.id)
      .Bind(2, model::ToString(payload.owner_kind))
      .Bind(3, payload.owner_id)
      .Bind(4, payload.effect_type)
      .Bind(5, payload.order)
      .Bind(6, payload.intrinsic)
      .Bind(7, payload.preset_name);
  statement.Run();

  Statement parameter(database, R"sql(
    INSERT INTO effect_parameters(id, effect_id, name, dimension, c0, c1, c2, c3)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  for (const auto& entry : payload.parameters) {
    parameter.Reset();
    parameter.Bind(1, entry.id)
        .Bind(2, payload.id)
        .Bind(3, entry.name)
        .Bind(4, static_cast<std::int64_t>(entry.value.dimension))
        .Bind(5, entry.value.components[0])
        .Bind(6, entry.value.components[1])
        .Bind(7, entry.value.components[2])
        .Bind(8, entry.value.components[3]);
    parameter.Run();
  }
}

// --------------------------------------------------------------- graphics ----

constexpr std::string_view kFindGraphic = "SELECT 1 FROM graphics WHERE id = ?;";

[[nodiscard]] std::string GraphicReference(const std::string& graphic_id) { return "project:" + graphic_id; }

[[nodiscard]] std::string TemplatePackageJson(sqlite3* database, const std::string& template_id, std::int64_t version) {
  Statement statement(database, "SELECT package_json FROM graphic_templates WHERE id = ? AND version = ?;");
  statement.Bind(1, template_id).Bind(2, version);
  Require(statement.Step(), "Template " + template_id + " version " + std::to_string(version) + " is not installed");
  return statement.ColumnText(0);
}

// The values must be ones the package accepts: every control known, every value usable.
void RequireValidValues(const std::string& package_json, const std::map<std::string, std::string>& values) {
  const auto package = render::graphics::ParseTemplate(package_json);
  (void)render::graphics::Instantiate(package, values);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::InstallGraphicTemplatePayload& payload) {
  const auto package = render::graphics::ParseTemplate(payload.package_json);
  // Stored in its canonical form, so installing the same package twice is recognisably the same.
  const auto canonical = render::graphics::ToJson(package);
  Statement existing(database, "SELECT package_json FROM graphic_templates WHERE id = ? AND version = ?;");
  existing.Bind(1, package.id).Bind(2, package.version);
  if (existing.Step()) {
    Require(existing.ColumnText(0) == canonical,
            "Template " + package.id + " version " + std::to_string(package.version) +
                " is already installed with different content; give the changed package a new version");
    return;
  }
  Statement insert(database, "INSERT INTO graphic_templates(id, version, name, package_json) VALUES(?, ?, ?, ?);");
  insert.Bind(1, package.id).Bind(2, package.version).Bind(3, package.name).Bind(4, canonical);
  insert.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveGraphicTemplatePayload& payload) {
  Statement existing(database, "SELECT 1 FROM graphic_templates WHERE id = ? AND version = ?;");
  existing.Bind(1, payload.template_id).Bind(2, payload.version);
  Require(existing.Step(), "Template " + payload.template_id + " version " + std::to_string(payload.version) + " is not installed");
  Statement used(database, "SELECT COUNT(*) FROM graphics WHERE template_id = ? AND template_version = ?;");
  used.Bind(1, payload.template_id).Bind(2, payload.version);
  const auto uses = used.Step() ? used.ColumnInt(0) : 0;
  Require(uses == 0, "Template " + payload.template_id + " version " + std::to_string(payload.version) + " is used by " +
                         std::to_string(uses) + " graphic(s); delete them first");
  Statement remove(database, "DELETE FROM graphic_templates WHERE id = ? AND version = ?;");
  remove.Bind(1, payload.template_id).Bind(2, payload.version);
  remove.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::CreateGraphicPayload& payload) {
  RequireAbsent(database, kFindGraphic, payload.id, "Graphic");
  std::string document;
  if (payload.kind == "graphic") {
    document = render::graphics::ToJson(render::graphics::ParseDocument(payload.document_json));
  } else {
    RequireValidValues(TemplatePackageJson(database, payload.template_id, payload.template_version), payload.values);
  }
  Statement insert(database, R"sql(
    INSERT INTO graphics(id, name, kind, document_json, template_id, template_version, values_json)
    VALUES(?, ?, ?, ?, ?, ?, ?);
  )sql");
  insert.Bind(1, payload.id).Bind(2, payload.name).Bind(3, payload.kind).Bind(4, document).Bind(5, payload.template_id)
      .Bind(6, payload.template_version).Bind(7, render::graphics::ValuesToJson(payload.values));
  insert.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::UpdateGraphicPayload& payload) {
  Statement lookup(database, "SELECT kind, template_id, template_version FROM graphics WHERE id = ?;");
  lookup.Bind(1, payload.id);
  Require(lookup.Step(), "Unknown graphic: " + payload.id);
  const auto kind = lookup.ColumnText(0);
  if (payload.document_json) {
    Require(kind == "graphic", "Only a plain graphic has a document; a template instance changes its values");
    Statement update(database, "UPDATE graphics SET document_json = ? WHERE id = ?;");
    update.Bind(1, render::graphics::ToJson(render::graphics::ParseDocument(*payload.document_json))).Bind(2, payload.id);
    update.Run();
  }
  if (payload.values) {
    Require(kind == "template", "Only a template instance has control values");
    RequireValidValues(TemplatePackageJson(database, lookup.ColumnText(1), lookup.ColumnInt(2)), *payload.values);
    Statement update(database, "UPDATE graphics SET values_json = ? WHERE id = ?;");
    update.Bind(1, render::graphics::ValuesToJson(*payload.values)).Bind(2, payload.id);
    update.Run();
  }
  if (payload.name) {
    Statement update(database, "UPDATE graphics SET name = ? WHERE id = ?;");
    update.Bind(1, *payload.name).Bind(2, payload.id);
    update.Run();
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::DeleteGraphicPayload& payload) {
  RequireExists(database, kFindGraphic, payload.id, "graphic");
  Statement used(database, "SELECT COUNT(*) FROM effects WHERE preset_name = ?;");
  used.Bind(1, GraphicReference(payload.id));
  const auto uses = used.Step() ? used.ColumnInt(0) : 0;
  Require(uses == 0, "Graphic " + payload.id + " is shown by " + std::to_string(uses) + " clip(s); remove them first");
  Statement remove(database, "DELETE FROM graphics WHERE id = ?;");
  remove.Bind(1, payload.id);
  remove.Run();
}

void Apply(sqlite3* database, const CommandEnvelope& command, const commands::AddGraphicClipPayload& payload) {
  Statement lookup(database, "SELECT kind FROM graphics WHERE id = ?;");
  lookup.Bind(1, payload.graphic_id);
  Require(lookup.Step(), "Unknown graphic: " + payload.graphic_id);
  const auto kind = lookup.ColumnText(0);
  RequireExists(database, kFindTrack, payload.track_id, "track");
  Require(TrackKindOf(database, payload.track_id) == model::TrackKind::Video, "A graphic goes on a video track");

  // A clip with no source of its own, which the compositor draws as the graphic its effect names.
  commands::InsertClipPayload clip;
  clip.id = payload.clip_id;
  clip.track_id = payload.track_id;
  clip.source_kind = model::SourceKind::Adjustment;
  clip.source_in = time::RationalTime(0, 1);
  clip.source_out = payload.duration;
  clip.timeline_start = payload.timeline_start;
  clip.name = payload.name.empty() ? payload.graphic_id : payload.name;
  Apply(database, command, clip);

  commands::AddEffectPayload effect;
  effect.id = payload.clip_id + ":graphic";
  effect.owner_kind = model::EffectOwner::Clip;
  effect.owner_id = payload.clip_id;
  effect.effect_type = kind == "template" ? "motion_graphics_template" : "graphic";
  effect.preset_name = GraphicReference(payload.graphic_id);
  Apply(database, command, effect);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveEffectPayload& payload) {
  Statement lookup(database, "SELECT intrinsic FROM effects WHERE id = ?;");
  lookup.Bind(1, payload.id);
  if (!lookup.Step()) throw std::runtime_error("Unknown effect: " + payload.id);
  RequireEffectUnlocked(database, payload.id);
  Require(lookup.ColumnInt(0) == 0, "Effect " + payload.id + " is intrinsic and can only be reset");
  Statement statement(database, "DELETE FROM effects WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetEffectEnabledPayload& payload) {
  RequireExists(database, kFindEffect, payload.id, "effect");
  RequireEffectUnlocked(database, payload.id);
  Statement statement(database, "UPDATE effects SET enabled = ? WHERE id = ?;");
  statement.Bind(1, payload.enabled).Bind(2, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::ReorderEffectPayload& payload) {
  RequireExists(database, kFindEffect, payload.id, "effect");
  RequireEffectUnlocked(database, payload.id);
  Statement statement(database, "UPDATE effects SET sort_order = ? WHERE id = ?;");
  statement.Bind(1, payload.order).Bind(2, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::AddMaskPayload& payload) {
  RequireAbsent(database, kFindMask, payload.id, "Mask");
  RequireExists(database, kFindEffect, payload.effect_id, "effect");
  RequireEffectUnlocked(database, payload.effect_id);
  const auto canonical = effects::mask::ToJson(effects::mask::Parse(payload.document_json));
  Statement statement(database,
                      "INSERT INTO effect_masks(id, effect_id, sort_order, document_json) VALUES(?, ?, ?, ?);");
  statement.Bind(1, payload.id).Bind(2, payload.effect_id).Bind(3, payload.order).Bind(4, canonical);
  statement.Run();
}

std::string MaskEffectId(sqlite3* database, const std::string& mask_id) {
  Statement statement(database, "SELECT effect_id FROM effect_masks WHERE id = ?;");
  statement.Bind(1, mask_id);
  if (!statement.Step()) throw std::runtime_error("Unknown mask: " + mask_id);
  return statement.ColumnText(0);
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::UpdateMaskPayload& payload) {
  RequireEffectUnlocked(database, MaskEffectId(database, payload.id));
  const auto canonical = effects::mask::ToJson(effects::mask::Parse(payload.document_json));
  Statement statement(database, "UPDATE effect_masks SET document_json = ? WHERE id = ?;");
  statement.Bind(1, canonical).Bind(2, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveMaskPayload& payload) {
  RequireEffectUnlocked(database, MaskEffectId(database, payload.id));
  Statement statement(database, "DELETE FROM effect_masks WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void RequireParameterValue(sqlite3* database, const std::string& parameter_id, const anim::Value& value,
                           bool require_keyframeable) {
  Statement statement(database, R"sql(
    SELECT p.dimension, p.name, e.effect_type
      FROM effect_parameters p JOIN effects e ON e.id = p.effect_id
     WHERE p.id = ?;
  )sql");
  statement.Bind(1, parameter_id);
  if (!statement.Step()) throw std::runtime_error("Unknown parameter: " + parameter_id);
  const auto declared = statement.ColumnInt(0);
  Require(declared == value.dimension, "Parameter " + parameter_id + " expects " + std::to_string(declared) +
                                           " component(s), received " + std::to_string(value.dimension));
  const auto name = statement.ColumnText(1);
  const auto effect_type = statement.ColumnText(2);
  const auto validation_error = effects::ValidateParameter(effect_type, name, value);
  Require(validation_error.empty(), validation_error);
  if (require_keyframeable) {
    const auto* effect = effects::FindEffect(effect_type);
    const auto* parameter = effect != nullptr ? effects::FindParameter(*effect, name) : nullptr;
    Require(parameter == nullptr || parameter->keyframeable,
            "Parameter " + parameter_id + " cannot be keyframed");
  }
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetParameterConstantPayload& payload) {
  RequireParameterUnlocked(database, payload.parameter_id);
  RequireParameterValue(database, payload.parameter_id, payload.value, false);
  Statement statement(database,
                      "UPDATE effect_parameters SET c0 = ?, c1 = ?, c2 = ?, c3 = ? WHERE id = ?;");
  statement.Bind(1, payload.value.components[0])
      .Bind(2, payload.value.components[1])
      .Bind(3, payload.value.components[2])
      .Bind(4, payload.value.components[3])
      .Bind(5, payload.parameter_id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::SetKeyframePayload& payload) {
  RequireParameterUnlocked(database, payload.parameter_id);
  RequireParameterValue(database, payload.parameter_id, payload.keyframe.value, true);
  const auto at = Columns(payload.keyframe.time);
  // Keyed on (parameter_id, time_ticks), so setting a keyframe where one already
  // exists replaces it, which is what dragging a keyframe onto another does.
  Statement statement(database, R"sql(
    INSERT INTO keyframes(parameter_id, time_num, time_den, time_ticks, c0, c1, c2, c3,
                          interpolation, out_handle_x, out_handle_y, in_handle_x, in_handle_y)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(parameter_id, time_ticks) DO UPDATE SET
      time_num = excluded.time_num, time_den = excluded.time_den,
      c0 = excluded.c0, c1 = excluded.c1, c2 = excluded.c2, c3 = excluded.c3,
      interpolation = excluded.interpolation,
      out_handle_x = excluded.out_handle_x, out_handle_y = excluded.out_handle_y,
      in_handle_x = excluded.in_handle_x, in_handle_y = excluded.in_handle_y;
  )sql");
  statement.Bind(1, payload.parameter_id)
      .Bind(2, at.numerator)
      .Bind(3, at.denominator)
      .Bind(4, at.ticks)
      .Bind(5, payload.keyframe.value.components[0])
      .Bind(6, payload.keyframe.value.components[1])
      .Bind(7, payload.keyframe.value.components[2])
      .Bind(8, payload.keyframe.value.components[3])
      .Bind(9, anim::ToString(payload.keyframe.interpolation))
      .Bind(10, payload.keyframe.out_handle.x)
      .Bind(11, payload.keyframe.out_handle.y)
      .Bind(12, payload.keyframe.in_handle.x)
      .Bind(13, payload.keyframe.in_handle.y);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveKeyframePayload& payload) {
  RequireExists(database, kFindParameter, payload.parameter_id, "parameter");
  RequireParameterUnlocked(database, payload.parameter_id);
  Statement statement(database, "DELETE FROM keyframes WHERE parameter_id = ? AND time_ticks = ?;");
  statement.Bind(1, payload.parameter_id).Bind(2, payload.at.ToTicks());
  statement.Run();
  if (sqlite3_changes(database) == 0) {
    throw std::runtime_error("No keyframe on parameter " + payload.parameter_id + " at that time");
  }
}

void RequireMarkerOwner(sqlite3* database, model::MarkerOwner owner_kind, const std::string& owner_id) {
  switch (owner_kind) {
    case model::MarkerOwner::Sequence: RequireExists(database, kFindSequence, owner_id, "sequence"); return;
    case model::MarkerOwner::Clip: RequireExists(database, kFindClip, owner_id, "clip"); return;
    case model::MarkerOwner::Media: RequireExists(database, kFindMedia, owner_id, "media"); return;
  }
  throw std::invalid_argument("Unhandled marker owner kind");
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::AddMarkerPayload& payload) {
  RequireAbsent(database, kFindMarker, payload.id, "Marker");
  RequireMarkerOwner(database, payload.owner_kind, payload.owner_id);
  const auto start = Columns(payload.start);
  const auto end = Columns(payload.end);
  Statement statement(database, R"sql(
    INSERT INTO markers(id, owner_kind, owner_id, start_num, start_den, end_num, end_den,
                        start_ticks, end_ticks, label, kind, color, metadata_json)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  statement.Bind(1, payload.id)
      .Bind(2, model::ToString(payload.owner_kind))
      .Bind(3, payload.owner_id)
      .Bind(4, start.numerator)
      .Bind(5, start.denominator)
      .Bind(6, end.numerator)
      .Bind(7, end.denominator)
      .Bind(8, start.ticks)
      .Bind(9, end.ticks)
      .Bind(10, payload.label)
      .Bind(11, model::ToString(payload.kind))
      .Bind(12, payload.color)
      .Bind(13, payload.metadata_json);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::RemoveMarkerPayload& payload) {
  RequireExists(database, kFindMarker, payload.id, "marker");
  Statement statement(database, "DELETE FROM markers WHERE id = ?;");
  statement.Bind(1, payload.id);
  statement.Run();
}

void Apply(sqlite3* database, const CommandEnvelope&, const commands::UpdateMarkerPayload& payload) {
  RequireExists(database, kFindMarker, payload.id, "marker");
  const auto start = Columns(payload.start);
  const auto end = Columns(payload.end);
  Statement statement(database, R"sql(
    UPDATE markers SET start_num = ?, start_den = ?, end_num = ?, end_den = ?,
                       start_ticks = ?, end_ticks = ?, label = ?, kind = ?, color = ?, metadata_json = ?
     WHERE id = ?;
  )sql");
  statement.Bind(1, start.numerator)
      .Bind(2, start.denominator)
      .Bind(3, end.numerator)
      .Bind(4, end.denominator)
      .Bind(5, start.ticks)
      .Bind(6, end.ticks)
      .Bind(7, payload.label)
      .Bind(8, model::ToString(payload.kind))
      .Bind(9, payload.color)
      .Bind(10, payload.metadata_json)
      .Bind(11, payload.id);
  statement.Run();
}

// ---------------------------------------------------------- journal records ----
//
// One file per revision, written after the commit. Format 2 carries everything
// needed to apply the revision again to the state it was made from: the changeset
// (base64) and the details of the journal row. Undo and redo are records too, since
// they are revisions, and a chain with holes in it cannot be replayed.

[[nodiscard]] std::string BuildJournalRecord(std::int64_t revision, const std::string& command_id,
                                             const std::string& project_id, const std::string& author_id,
                                             const std::string& timestamp_utc, const std::string& idempotency_key,
                                             const std::string& type, const std::string& label,
                                             const std::string& payload_json, const std::string& changeset_text) {
  return json::Object()
      .Add("format", std::int64_t{2})
      .Add("revision", revision)
      .Add("commandId", command_id)
      .Add("projectId", project_id)
      .Add("authorId", author_id)
      .Add("timestampUtc", timestamp_utc)
      .Add("idempotencyKey", idempotency_key)
      .Add("type", type)
      .Add("label", label)
      .AddRaw("payload", payload_json)
      .Add("changeset", changeset_text)
      .Build();
}

struct JournalFileEntry final {
  std::int64_t revision{};
  std::filesystem::path path;
};

// Journal files in the package, ascending by revision. Temporaries are ignored.
[[nodiscard]] std::vector<JournalFileEntry> ListJournalFiles(const std::filesystem::path& package) {
  std::vector<JournalFileEntry> files;
  const auto directory = package / "journal";
  std::error_code ignored;
  if (!std::filesystem::is_directory(directory, ignored)) return files;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (entry.path().extension() != ".json" || name.size() < 2 || name[0] != 'r') continue;
    try {
      files.push_back({std::stoll(name.substr(1)), entry.path()});
    } catch (const std::exception&) {
    }
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    return a.revision != b.revision ? a.revision < b.revision : a.path < b.path;
  });
  return files;
}

// Reads and checks one record. Empty on success; otherwise the reason it cannot be used.
[[nodiscard]] std::string ReadJournalRecord(const JournalFileEntry& file, std::string& text) {
  std::ifstream stream(file.path, std::ios::binary);
  if (!stream) return "cannot be read";
  std::ostringstream contents;
  contents << stream.rdbuf();
  text = contents.str();
  try {
    const auto record = json::Parse(text);
    if (!record.is_object()) return "is not an object";
    if (record.Integer("format") != 2) return "is not a format 2 record (no changeset to replay)";
    if (record.Integer("revision") != file.revision) return "names a different revision than its file";
    (void)record.String("commandId");
    (void)record.String("projectId");
    (void)record.String("type");
    (void)record.Require("payload").raw;
    (void)util::Base64Decode(record.String("changeset"));
  } catch (const std::exception& error) {
    return std::string("is damaged: ") + error.what();
  }
  return {};
}

}  // namespace

// ------------------------------------------------------------- lifecycle ----

ProjectStore::ProjectStore(std::string database_path, SnapshotPolicy policy)
    : database_path_(std::move(database_path)), snapshot_policy_(policy) {
  if (snapshot_policy_.every_revisions <= 0) {
    throw std::invalid_argument("Snapshot interval must be positive");
  }
  if (snapshot_policy_.history_limit == 0) {
    throw std::invalid_argument("Undo history limit must be at least one");
  }
  if (snapshot_policy_.keep_snapshots == 0) {
    throw std::invalid_argument("At least one snapshot must be kept");
  }
  Open();
}

ProjectStore::~ProjectStore() {
  if (database_ != nullptr) sqlite3_close(database_);
}

void ProjectStore::Open() {
  const auto path = database_path_.string();
  // SQLITE_OPEN_FULLMUTEX because the handle is shared across threads; the
  // store's own mutex serialises logical operations, this protects the handle.
  const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &database_, flags, nullptr) != SQLITE_OK) {
    const std::string detail = database_ != nullptr ? sqlite3_errmsg(database_) : "unknown error";
    if (database_ != nullptr) sqlite3_close(database_);
    database_ = nullptr;
    throw std::runtime_error("Unable to open project database " + path + ": " + detail);
  }
  try {
    ConfigureConnection();
  } catch (...) {
    // A constructor that throws never runs the destructor, so the handle has to be
    // released here; left open it holds the file (on Windows, locks it against being
    // moved or deleted, which is exactly what recovery needs to do).
    sqlite3_close(database_);
    database_ = nullptr;
    throw;
  }
}

void ProjectStore::ConfigureConnection() const {
  // Foreign keys are what make cascade deletes -- and therefore changeset-based
  // undo of a track or clip deletion -- work at all.
  db::Execute(database_, "PRAGMA foreign_keys = ON;");
  db::Execute(database_, "PRAGMA busy_timeout = 5000;");
  // WAL lets the compiler read a sequence while an edit is being written. An
  // in-memory database does not support it and stays on its own journal.
  if (database_path_ != ":memory:") {
    db::Execute(database_, "PRAGMA journal_mode = WAL;");
    db::Execute(database_, snapshot_policy_.durability == Durability::Full ? "PRAGMA synchronous = FULL;"
                                                                           : "PRAGMA synchronous = NORMAL;");
  }
}

void ProjectStore::Initialize() {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto has_meta = db::ScalarInt(
      database_, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'project_meta';");
  if (has_meta == 0) {
    CreateSchema(database_);
    return;
  }
  MigrateIfNeeded();
}

void ProjectStore::MigrateIfNeeded() {
  const auto version = db::ScalarInt(database_, "SELECT schema_version FROM project_meta WHERE singleton = 1;");
  if (version == kSchemaVersion) {
    // Still run CreateSchema so a database written by an older build picks up
    // tables and indexes added without a version bump.
    CreateSchema(database_);
    return;
  }
  if (!package_path_.empty()) {
    std::filesystem::create_directories(package_path_ / "snapshots");
    BackupDatabase(package_path_ / "snapshots" / ("migration-v" + std::to_string(version) + "-backup.db"));
  }
  MigrateSchema(database_, version);
}

std::unique_ptr<ProjectStore> ProjectStore::CreatePackage(const std::filesystem::path& package_path,
                                                          const CommandEnvelope& create_project,
                                                          SnapshotPolicy policy) {
  if (std::filesystem::exists(package_path) && !std::filesystem::is_empty(package_path)) {
    throw std::runtime_error("Project package already exists: " + package_path.string());
  }
  std::filesystem::create_directories(package_path);
  std::filesystem::create_directories(package_path / "snapshots");
  std::filesystem::create_directories(package_path / "journal");

  auto store = std::make_unique<ProjectStore>((package_path / kDatabaseFileName).string(), policy);
  store->package_path_ = package_path;
  store->Initialize();
  const auto result = store->Execute(create_project);
  (void)result;
  // Revision 1 (the project's creation) changes the project's identity, which a
  // changeset does not record, so recovery can only start from a snapshot at or
  // after it.
  store->CreateSnapshot();

  {
    std::ofstream manifest(package_path / "project.json", std::ios::trunc);
    if (!manifest) throw std::runtime_error("Unable to create project manifest");
    manifest << json::Object()
                    .Add("formatVersion", kSchemaVersion)
                    .Add("projectId", create_project.project_id)
                    .Build()
             << "\n";
  }
  return store;
}

std::unique_ptr<ProjectStore> ProjectStore::OpenPackage(const std::filesystem::path& package_path,
                                                        SnapshotPolicy policy) {
  const auto database = package_path / kDatabaseFileName;
  if (!std::filesystem::is_regular_file(database)) {
    throw std::runtime_error("Not a Cutline project package: " + package_path.string());
  }
  auto store = std::make_unique<ProjectStore>(database.string(), policy);
  store->package_path_ = package_path;
  store->Initialize();
  store->ValidateDatabase();
  return store;
}

std::string ProjectStore::GenerateProjectUuid() {
  // Version 4 UUID from the system entropy source.
  std::random_device entropy;
  std::uniform_int_distribution<std::uint32_t> nibble(0, 15);
  constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string uuid(36, '-');
  constexpr std::string_view pattern = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    switch (pattern[index]) {
      case 'x': uuid[index] = digits[nibble(entropy)]; break;
      case '4': uuid[index] = '4'; break;
      // The variant nibble must be one of 8, 9, a, b.
      case 'y': uuid[index] = digits[8 + (nibble(entropy) & 0x3)]; break;
      default: uuid[index] = pattern[index]; break;
    }
  }
  return uuid;
}

// --------------------------------------------------------------- command ----

CommandResult ProjectStore::Execute(const CommandEnvelope& command) {
  PostCommit post;
  CommandResult result;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    result = ExecuteLocked(command, post);
  }
  // The commit is done and the lock released. What remains is copying: a journal
  // record into the package and, now and then, the whole database. Neither can
  // un-apply the edit, and neither should hold up the next one.
  FinishPersistence(post, result);
  return result;
}

CommandResult ProjectStore::ExecuteLocked(const CommandEnvelope& command, PostCommit& post) {
  commands::Validate(command);

  const auto current_project = ProjectIdLocked();
  if (command.type == commands::CommandType::CreateProject) {
    Require(current_project.empty(), "This project already has an identity");
  } else {
    Require(!current_project.empty(), "No project has been created in this database yet");
    Require(current_project == command.project_id,
            "Command targets project " + command.project_id + " but this database holds " + current_project);
  }

  // Idempotency: a retried command returns the revision it originally produced
  // rather than applying twice.
  {
    Statement replay(database_,
                     "SELECT revision FROM command_journal WHERE command_id = ? OR idempotency_key = ? LIMIT 1;");
    replay.Bind(1, command.command_id).Bind(2, command.idempotency_key);
    if (replay.Step()) {
      return {replay.ColumnInt(0), "Idempotent command replay ignored.", true};
    }
  }

  std::int64_t next_revision{};
  db::ChangeSet changeset;
  std::string changeset_text;
  {
    db::Transaction transaction(database_);
    const auto revision = RevisionLocked();
    Require(revision == command.base_revision,
            "Stale project revision: expected " + std::to_string(revision) + ", received " +
                std::to_string(command.base_revision));

    // The recorder must be created inside the transaction and taken before
    // commit, so it captures exactly this command's rows.
    db::Recorder recorder(database_);
    ApplyMutation(command);
    changeset = recorder.Take();
    changeset_text = util::Base64Encode(changeset.bytes());

    next_revision = revision + 1;
    SetRevision(next_revision, command.timestamp_utc);

    Statement journal(database_, R"sql(
      INSERT INTO command_journal(revision, command_id, project_id, author_id, created_at_utc,
                                  command_type, payload_json, label, changeset, idempotency_key)
      VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )sql");
    const auto payload_json = commands::PayloadJson(command.payload);
    const auto label = commands::Label(command);
    journal.Bind(1, next_revision)
        .Bind(2, command.command_id)
        .Bind(3, command.project_id)
        .Bind(4, command.author_id)
        .Bind(5, command.timestamp_utc)
        .Bind(6, commands::ToString(command.type))
        .Bind(7, payload_json)
        .Bind(8, label);
    // The changeset is bound as a blob; BindBlob stores an empty one as NULL.
    journal.BindBlob(9, changeset.bytes());
    journal.Bind(10, command.idempotency_key);
    journal.Run();

    transaction.Commit();
  }

  // Only now that the transaction has committed does the undo stack change.
  history_.resize(applied_history_);
  if (!changeset.empty()) {
    history_.push_back({command.command_id, current_group_.empty() ? commands::Label(command) : current_group_label_, std::move(changeset), current_group_});
    applied_history_ = history_.size();
    TrimHistory();
  }

  if (!package_path_.empty()) {
    post.revision = next_revision;
    post.command_id = command.command_id;
    post.journal_record = BuildJournalRecord(next_revision, command.command_id, command.project_id, command.author_id,
                                             command.timestamp_utc, command.idempotency_key,
                                             commands::ToString(command.type), commands::Label(command),
                                             commands::PayloadJson(command.payload), changeset_text);
    post.snapshot_due = next_revision % snapshot_policy_.every_revisions == 0;
  }
  return {next_revision, "Command applied: " + commands::ToString(command.type), false, {}};
}

void ProjectStore::ApplyMutation(const CommandEnvelope& command) {
  std::visit([&](const auto& payload) { Apply(database_, command, payload); }, command.payload);
}

CommandResult ProjectStore::Undo(const std::string& author_id, const std::string& timestamp_utc) {
  // One step: a single command, or every command of the group the newest applied entry belongs to.
  CommandResult last;
  std::string group;
  for (bool first = true;; first = false) {
    PostCommit post;
    CommandResult result;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (first) {
        if (applied_history_ != 0) group = history_[applied_history_ - 1].group;
      } else if (group.empty() || applied_history_ == 0 || history_[applied_history_ - 1].group != group) {
        break;
      }
      result = UndoLocked(author_id, timestamp_utc, post);
    }
    FinishPersistence(post, result);
    last = result;
    if (group.empty()) break;
  }
  return last;
}

CommandResult ProjectStore::Redo(const std::string& author_id, const std::string& timestamp_utc) {
  CommandResult last;
  std::string group;
  for (bool first = true;; first = false) {
    PostCommit post;
    CommandResult result;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (first) {
        if (applied_history_ < history_.size()) group = history_[applied_history_].group;
      } else if (group.empty() || applied_history_ >= history_.size() || history_[applied_history_].group != group) {
        break;
      }
      result = RedoLocked(author_id, timestamp_utc, post);
    }
    FinishPersistence(post, result);
    last = result;
    if (group.empty()) break;
  }
  return last;
}

CommandResult ProjectStore::ExecuteGroup(std::vector<commands::CommandEnvelope> commands, const std::string& label) {
  Require(!commands.empty(), "A group of commands has no commands");
  if (commands.size() == 1) {
    return Execute(commands.front());
  }
  const auto group = [&] {
    const std::lock_guard<std::mutex> lock(mutex_);
    current_group_ = "group-" + std::to_string(++group_counter_);
    current_group_label_ = label;
    return current_group_;
  }();
  const auto finish = [&] {
    const std::lock_guard<std::mutex> lock(mutex_);
    current_group_.clear();
    current_group_label_.clear();
  };
  std::size_t applied = 0;
  CommandResult last;
  try {
    for (auto& command : commands) {
      if (applied != 0) command.base_revision = CurrentRevision();
      last = Execute(command);
      ++applied;
    }
  } catch (...) {
    finish();
    // Take back what the group had done, and make it unrecoverable by redo.
    for (std::size_t index = 0; index < applied; ++index) {
      try {
        PostCommit post;
        CommandResult result;
        {
          const std::lock_guard<std::mutex> lock(mutex_);
          if (applied_history_ == 0 || history_[applied_history_ - 1].group != group) break;
          result = UndoLocked(commands.front().author_id, commands.front().timestamp_utc, post);
        }
        FinishPersistence(post, result);
      } catch (...) {
        break;
      }
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      history_.resize(applied_history_);
    }
    throw;
  }
  finish();
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    TrimHistory();
  }
  return last;
}

CommandResult ProjectStore::UndoLocked(const std::string& author_id, const std::string& timestamp_utc,
                                       PostCommit& post) {
  if (author_id.empty() || timestamp_utc.empty()) {
    throw std::invalid_argument("Undo requires an author and a timestamp");
  }
  Require(applied_history_ != 0, "Nothing available to undo");

  const auto& entry = history_[applied_history_ - 1];
  const auto inverse = entry.changeset.Inverted();

  std::int64_t revision{};
  {
    db::Transaction transaction(database_);
    // Deferring foreign keys to COMMIT lets a changeset reinsert rows in table
    // order rather than in dependency order.
    db::Execute(database_, "PRAGMA defer_foreign_keys = ON;");
    const CascadeSuspension suspended(database_);
    inverse.ApplyTo(database_);
    revision = RevisionLocked() + 1;
    SetRevision(revision, timestamp_utc);

    Statement journal(database_, R"sql(
      INSERT INTO command_journal(revision, command_id, project_id, author_id, created_at_utc,
                                  command_type, payload_json, label, idempotency_key)
      VALUES(?, ?, ?, ?, ?, 'project.undo', ?, ?, ?);
    )sql");
    const auto marker = "undo-" + std::to_string(revision);
    const auto payload = json::Object().Add("commandId", entry.command_id).Build();
    journal.Bind(1, revision)
        .Bind(2, marker)
        .Bind(3, ProjectIdLocked())
        .Bind(4, author_id)
        .Bind(5, timestamp_utc)
        .Bind(6, payload)
        .Bind(7, "Undo " + entry.label)
        .Bind(8, marker);
    journal.Run();
    transaction.Commit();
  }

  const auto label = history_[applied_history_ - 1].label;
  if (!package_path_.empty()) {
    const auto marker = "undo-" + std::to_string(revision);
    post.revision = revision;
    post.command_id = marker;
    post.journal_record = BuildJournalRecord(revision, marker, ProjectIdLocked(), author_id, timestamp_utc, marker,
                                             "project.undo", "Undo " + label,
                                             json::Object().Add("commandId", history_[applied_history_ - 1].command_id).Build(),
                                             util::Base64Encode(inverse.bytes()));
    post.snapshot_due = revision % snapshot_policy_.every_revisions == 0;
  }
  --applied_history_;
  return {revision, "Undid " + label, false, {}};
}

CommandResult ProjectStore::RedoLocked(const std::string& author_id, const std::string& timestamp_utc,
                                       PostCommit& post) {
  if (author_id.empty() || timestamp_utc.empty()) {
    throw std::invalid_argument("Redo requires an author and a timestamp");
  }
  Require(applied_history_ < history_.size(), "Nothing available to redo");

  const auto& entry = history_[applied_history_];

  std::int64_t revision{};
  {
    db::Transaction transaction(database_);
    db::Execute(database_, "PRAGMA defer_foreign_keys = ON;");
    const CascadeSuspension suspended(database_);
    entry.changeset.ApplyTo(database_);
    revision = RevisionLocked() + 1;
    SetRevision(revision, timestamp_utc);

    Statement journal(database_, R"sql(
      INSERT INTO command_journal(revision, command_id, project_id, author_id, created_at_utc,
                                  command_type, payload_json, label, idempotency_key)
      VALUES(?, ?, ?, ?, ?, 'project.redo', ?, ?, ?);
    )sql");
    const auto marker = "redo-" + std::to_string(revision);
    const auto payload = json::Object().Add("commandId", entry.command_id).Build();
    journal.Bind(1, revision)
        .Bind(2, marker)
        .Bind(3, ProjectIdLocked())
        .Bind(4, author_id)
        .Bind(5, timestamp_utc)
        .Bind(6, payload)
        .Bind(7, "Redo " + entry.label)
        .Bind(8, marker);
    journal.Run();
    transaction.Commit();
  }

  const auto label = history_[applied_history_].label;
  if (!package_path_.empty()) {
    const auto marker = "redo-" + std::to_string(revision);
    post.revision = revision;
    post.command_id = marker;
    post.journal_record = BuildJournalRecord(revision, marker, ProjectIdLocked(), author_id, timestamp_utc, marker,
                                             "project.redo", "Redo " + label,
                                             json::Object().Add("commandId", history_[applied_history_].command_id).Build(),
                                             util::Base64Encode(entry.changeset.bytes()));
    post.snapshot_due = revision % snapshot_policy_.every_revisions == 0;
  }
  ++applied_history_;
  return {revision, "Redid " + label, false, {}};
}

void ProjectStore::TrimHistory() {
  // A group being built is not trimmed (its own earlier entries must stay with it); it is when it ends.
  if (!current_group_.empty()) return;
  // The limit is in steps, the things a person undoes: an edit of a thousand commands is one step. Whole steps go,
  // oldest first, so a group is never left with its head missing.
  const auto steps = [&] {
    std::size_t count = 0;
    for (std::size_t index = 0; index < history_.size(); ++index) {
      if (index > 0 && !history_[index].group.empty() && history_[index].group == history_[index - 1].group) continue;
      ++count;
    }
    return count;
  };
  for (auto remaining = steps(); remaining > snapshot_policy_.history_limit && !history_.empty(); --remaining) {
    std::size_t count = 1;
    if (!history_.front().group.empty()) {
      while (count < history_.size() && history_[count].group == history_.front().group) ++count;
    }
    history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(count));
    applied_history_ = applied_history_ > count ? applied_history_ - count : 0;
  }
}

// ----------------------------------------------------------- observation ----

std::vector<std::string> ProjectStore::HistoryLabels() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> labels;
  labels.reserve(history_.size());
  for (const auto& entry : history_) labels.push_back(entry.label);
  return labels;
}

void ProjectStore::ClearHistory() {
  const std::lock_guard<std::mutex> lock(mutex_);
  history_.clear();
  applied_history_ = 0;
}

std::vector<std::string> ProjectStore::HistorySteps() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> steps;
  for (std::size_t index = 0; index < history_.size(); ++index) {
    if (index > 0 && !history_[index].group.empty() && history_[index].group == history_[index - 1].group) continue;
    steps.push_back(history_[index].label);
  }
  return steps;
}

std::size_t ProjectStore::AppliedStepCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::size_t steps = 0;
  for (std::size_t index = 0; index < applied_history_; ++index) {
    if (index > 0 && !history_[index].group.empty() && history_[index].group == history_[index - 1].group) continue;
    ++steps;
  }
  return steps;
}

std::size_t ProjectStore::AppliedHistoryCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return applied_history_;
}

bool ProjectStore::CanUndo() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return applied_history_ != 0;
}

bool ProjectStore::CanRedo() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return applied_history_ < history_.size();
}

std::size_t ProjectStore::HistoryBytes() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::size_t total = 0;
  for (const auto& entry : history_) total += entry.changeset.size();
  return total;
}

std::int64_t ProjectStore::CurrentRevision() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return RevisionLocked();
}

std::int64_t ProjectStore::RevisionLocked() const {
  return db::ScalarInt(database_, "SELECT revision FROM project_meta WHERE singleton = 1;");
}

std::string ProjectStore::ProjectId() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return ProjectIdLocked();
}

std::string ProjectStore::ProjectIdLocked() const {
  return db::ScalarText(database_, "SELECT project_id FROM project_meta WHERE singleton = 1;");
}

std::string ProjectStore::ProjectName() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return db::ScalarText(database_, "SELECT name FROM project_settings WHERE singleton = 1;");
}

std::string ProjectStore::JournalMode() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return db::ScalarText(database_, "PRAGMA journal_mode;");
}

std::int64_t ProjectStore::JournalCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return db::ScalarInt(database_, "SELECT COUNT(*) FROM command_journal;");
}

std::int64_t ProjectStore::SnapshotCount() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (package_path_.empty()) return 0;
  const auto directory = package_path_ / "snapshots";
  if (!std::filesystem::is_directory(directory)) return 0;
  std::int64_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".db") ++count;
  }
  return count;
}

void ProjectStore::ValidateDatabase() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  ValidateSchema(database_);
}

void ProjectStore::CreateSnapshot() {
  if (package_path_.empty()) return;
  // A copy made through a connection of its own, not under the store's lock. An
  // explicit request reports its failure by throwing; the automatic one cannot.
  (void)WriteSnapshotCopy(package_path_ / "snapshots");
  PruneSnapshots();
}

void ProjectStore::RecordWarning(const std::string& warning) {
  const std::lock_guard<std::mutex> lock(warning_mutex_);
  persistence_warnings_.push_back(warning);
  if (persistence_warnings_.size() > 100) persistence_warnings_.erase(persistence_warnings_.begin());
}

std::vector<std::string> ProjectStore::PersistenceWarnings() const {
  const std::lock_guard<std::mutex> lock(warning_mutex_);
  return persistence_warnings_;
}

void ProjectStore::FinishPersistence(const PostCommit& post, CommandResult& result) {
  if (post.journal_record.empty()) return;
  const auto report = [&](const std::string& what, const std::exception& error) {
    const auto text = what + ": " + error.what();
    result.warnings.push_back(text);
    RecordWarning(text);
  };
  try {
    WriteJournalFile(post.revision, post.command_id, post.journal_record);
  } catch (const std::exception& error) {
    report("The journal record for revision " + std::to_string(post.revision) + " was not written", error);
  }
  if (post.snapshot_due) {
    try {
      (void)WriteSnapshotCopy(package_path_ / "snapshots");
      PruneSnapshots();
    } catch (const std::exception& error) {
      report("The snapshot for revision " + std::to_string(post.revision) + " was not written", error);
    }
  }
}

std::int64_t ProjectStore::WriteSnapshotCopy(const std::filesystem::path& directory) const {
  std::filesystem::create_directories(directory);

  // A connection of its own, read-only, in a read transaction. In WAL mode a
  // reader sees one consistent state however many commits land while it copies, so
  // the store need not be locked for the length of the copy; the revision is read
  // from the same state, so the file is named for what it contains.
  sqlite3* source = nullptr;
  if (sqlite3_open_v2(database_path_.string().c_str(), &source, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) !=
      SQLITE_OK) {
    const std::string detail = source != nullptr ? sqlite3_errmsg(source) : "unknown error";
    if (source != nullptr) sqlite3_close(source);
    throw std::runtime_error("Unable to open the project to copy it: " + detail);
  }
  struct Closer final {
    sqlite3* handle;
    ~Closer() { sqlite3_close(handle); }
  } closer{source};
  db::Execute(source, "PRAGMA busy_timeout = 5000;");
  db::Execute(source, "BEGIN;");
  const auto revision = db::ScalarInt(source, "SELECT revision FROM project_meta WHERE singleton = 1;");

  const auto destination = directory / ("snapshot-r" + std::to_string(revision) + ".db");
  // Write to a sibling temporary first, then rename, so an interrupted backup
  // cannot be mistaken for a complete one.
  const auto temporary = destination.string() + ".tmp";
  std::filesystem::remove(temporary);

  sqlite3* target = nullptr;
  if (sqlite3_open_v2(temporary.c_str(), &target, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    const std::string detail = target != nullptr ? sqlite3_errmsg(target) : "unknown error";
    if (target != nullptr) sqlite3_close(target);
    throw std::runtime_error("Unable to create snapshot file " + temporary + ": " + detail);
  }
  sqlite3_backup* backup = sqlite3_backup_init(target, "main", source, "main");
  if (backup == nullptr) {
    const std::string detail = sqlite3_errmsg(target);
    sqlite3_close(target);
    std::filesystem::remove(temporary);
    throw std::runtime_error("Unable to start snapshot: " + detail);
  }
  const auto step = sqlite3_backup_step(backup, -1);
  sqlite3_backup_finish(backup);
  sqlite3_close(target);
  if (step != SQLITE_DONE) {
    std::filesystem::remove(temporary);
    throw std::runtime_error(std::string("Snapshot did not complete: ") + sqlite3_errstr(step));
  }
  std::filesystem::remove(destination);
  std::filesystem::rename(temporary, destination);
  return revision;
}

void ProjectStore::PruneSnapshots() const {
  if (package_path_.empty()) return;
  const auto directory = package_path_ / "snapshots";
  if (!std::filesystem::is_directory(directory)) return;

  // Automatic snapshots are named snapshot-r<revision>.db; migration backups have
  // other names and are never touched.
  std::vector<std::pair<std::int64_t, std::filesystem::path>> snapshots;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".db") continue;
    const auto name = entry.path().filename().string();
    if (name.rfind("snapshot-r", 0) != 0) continue;
    try {
      snapshots.emplace_back(std::stoll(name.substr(10)), entry.path());
    } catch (const std::exception&) {
      // not one of ours
    }
  }
  std::sort(snapshots.begin(), snapshots.end());
  while (snapshots.size() > snapshot_policy_.keep_snapshots) {
    std::error_code ignored;
    std::filesystem::remove(snapshots.front().second, ignored);
    snapshots.erase(snapshots.begin());
  }
  if (snapshots.empty()) return;

  // A journal file older than the oldest snapshot kept can no longer be replayed
  // from anything, and the same record is in the database's own journal table.
  const auto oldest = snapshots.front().first;
  const auto journal = package_path_ / "journal";
  if (!std::filesystem::is_directory(journal)) return;
  for (const auto& entry : std::filesystem::directory_iterator(journal)) {
    const auto name = entry.path().filename().string();
    if (name.empty() || name[0] != 'r') continue;
    try {
      if (std::stoll(name.substr(1)) < oldest) {
        std::error_code ignored;
        std::filesystem::remove(entry.path(), ignored);
      }
    } catch (const std::exception&) {
      // not one of ours
    }
  }
}

void ProjectStore::SetRevision(std::int64_t revision, const std::string& timestamp_utc) const {
  Statement statement(database_, "UPDATE project_meta SET revision = ?, updated_at = ? WHERE singleton = 1;");
  statement.Bind(1, revision).Bind(2, timestamp_utc);
  statement.Run();
}

void ProjectStore::BackupDatabase(const std::filesystem::path& destination) const {
  std::filesystem::create_directories(destination.parent_path());
  // Write to a sibling temporary first, then rename, so an interrupted backup
  // cannot be mistaken for a complete one.
  const auto temporary = destination.string() + ".tmp";
  std::filesystem::remove(temporary);

  sqlite3* target = nullptr;
  if (sqlite3_open_v2(temporary.c_str(), &target, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    if (target != nullptr) sqlite3_close(target);
    throw std::runtime_error("Unable to create snapshot file " + temporary);
  }
  sqlite3_backup* backup = sqlite3_backup_init(target, "main", database_, "main");
  if (backup == nullptr) {
    const std::string detail = sqlite3_errmsg(target);
    sqlite3_close(target);
    throw std::runtime_error("Unable to start snapshot: " + detail);
  }
  const auto step = sqlite3_backup_step(backup, -1);
  sqlite3_backup_finish(backup);
  if (step != SQLITE_DONE) {
    const std::string detail = sqlite3_errstr(step);
    sqlite3_close(target);
    std::filesystem::remove(temporary);
    throw std::runtime_error("Snapshot did not complete: " + detail);
  }
  sqlite3_close(target);
  std::filesystem::remove(destination);
  std::filesystem::rename(temporary, destination);
}

void ProjectStore::WriteJournalFile(std::int64_t revision, const std::string& command_id,
                                    const std::string& record) const {
  if (package_path_.empty()) return;
  const auto directory = package_path_ / "journal";
  std::filesystem::create_directories(directory);
  std::ostringstream name;
  name << "r" << revision << "-" << command_id << ".json";
  // Written whole to a temporary and renamed into place, and checked after the
  // write: a full disk fails the stream, not the open.
  const auto destination = directory / name.str();
  const auto temporary = destination.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file) throw std::runtime_error("Unable to write journal record for revision " + std::to_string(revision));
    file << record << "\n";
    file.flush();
    if (!file) {
      file.close();
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      throw std::runtime_error("Journal record for revision " + std::to_string(revision) +
                               " was not fully written (disk full?)");
    }
  }
  std::filesystem::rename(temporary, destination);
}


// --------------------------------------------------------------- recovery ----

bool ProjectStore::ReplayRecordLocked(const std::string& record_text) {
  const auto record = json::Parse(record_text);
  const auto revision = record.Integer("revision");
  const auto command_id = record.String("commandId");
  const auto current = RevisionLocked();

  if (revision <= current) {
    // Already applied. Say so only if it is the same command: a different one at the
    // same revision means these records belong to a history this database left.
    Statement existing(database_, "SELECT command_id FROM command_journal WHERE revision = ?;");
    existing.Bind(1, revision);
    if (!existing.Step() || existing.ColumnText(0) != command_id) {
      throw std::runtime_error("The journal record for revision " + std::to_string(revision) + " (" + command_id +
                               ") does not match what this database recorded at that revision");
    }
    return false;
  }
  Require(revision == current + 1, "The journal has a gap: the database is at revision " + std::to_string(current) +
                                       " and the next record is revision " + std::to_string(revision));
  const auto project = record.String("projectId");
  Require(project == ProjectIdLocked(), "The journal record belongs to project " + project);

  const auto type = record.String("type");
  const bool is_history = type == "project.undo" || type == "project.redo";
  const auto bytes = util::Base64Decode(record.String("changeset"));
  const auto timestamp = record.String("timestampUtc");

  db::Transaction transaction(database_);
  db::Execute(database_, "PRAGMA defer_foreign_keys = ON;");
  const CascadeSuspension suspended(database_);
  if (!bytes.empty()) db::ChangeSet(bytes).ApplyTo(database_);
  SetRevision(revision, timestamp);

  Statement journal(database_, R"sql(
    INSERT INTO command_journal(revision, command_id, project_id, author_id, created_at_utc,
                                command_type, payload_json, label, changeset, idempotency_key)
    VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
  )sql");
  journal.Bind(1, revision)
      .Bind(2, command_id)
      .Bind(3, project)
      .Bind(4, record.String("authorId"))
      .Bind(5, timestamp)
      .Bind(6, type)
      .Bind(7, record.Require("payload").raw)
      .Bind(8, record.String("label"));
  if (is_history) {
    journal.BindNull(9);
  } else {
    journal.BindBlob(9, bytes);
  }
  journal.Bind(10, record.String("idempotencyKey"));
  journal.Run();
  transaction.Commit();
  return true;
}

namespace {

// Opens a copy of `database_file` in a scratch folder and checks it the way opening
// a project does. Returns its revision, or nullopt with the reason.
[[nodiscard]] std::optional<std::int64_t> CheckDatabaseCopy(const std::filesystem::path& database_file,
                                                           const std::filesystem::path& scratch,
                                                           std::string& problem) {
  std::error_code error;
  std::filesystem::create_directories(scratch, error);
  const auto copy = scratch / "check.db";
  std::filesystem::remove(copy, error);
  std::filesystem::copy_file(database_file, copy, std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    problem = "cannot be copied: " + error.message();
    return std::nullopt;
  }
  std::optional<std::int64_t> revision;
  try {
    ProjectStore store(copy.string());
    store.Initialize();
    store.ValidateDatabase();
    revision = store.CurrentRevision();
  } catch (const std::exception& failure) {
    problem = failure.what();
  }
  for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(copy.string() + suffix, error);
  return revision;
}

struct SnapshotFile final {
  std::int64_t revision{};
  std::filesystem::path path;
};

[[nodiscard]] std::vector<SnapshotFile> ListSnapshotFiles(const std::filesystem::path& package) {
  std::vector<SnapshotFile> files;
  const auto directory = package / "snapshots";
  std::error_code ignored;
  if (!std::filesystem::is_directory(directory, ignored)) return files;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (entry.path().extension() != ".db" || name.rfind("snapshot-r", 0) != 0) continue;
    try {
      files.push_back({std::stoll(name.substr(10)), entry.path()});
    } catch (const std::exception&) {
    }
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.revision < b.revision; });
  return files;
}

// How far an unbroken chain of readable records reaches from `from_revision`.
[[nodiscard]] std::int64_t ChainReach(const std::vector<JournalFileEntry>& files, std::int64_t from_revision,
                                      std::vector<std::string>* problems) {
  std::map<std::int64_t, const JournalFileEntry*> by_revision;
  for (const auto& file : files) by_revision.emplace(file.revision, &file);  // first file wins
  auto reach = from_revision;
  while (true) {
    const auto next = by_revision.find(reach + 1);
    if (next == by_revision.end()) break;
    std::string text;
    const auto problem = ReadJournalRecord(*next->second, text);
    if (!problem.empty()) {
      if (problems != nullptr) problems->push_back("The journal record for revision " + std::to_string(reach + 1) + " " + problem);
      break;
    }
    ++reach;
  }
  return reach;
}

[[nodiscard]] std::string TimestampForFolder() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace

RecoveryReport ProjectStore::InspectPackage(const std::filesystem::path& package_path) {
  RecoveryReport report;
  const auto database = package_path / kDatabaseFileName;
  if (!std::filesystem::is_directory(package_path)) {
    report.database_problem = "the package folder does not exist";
    report.notes.push_back("There is no project package at " + package_path.string() + ".");
    return report;
  }

  if (!std::filesystem::is_regular_file(database)) {
    report.database_problem = "the database file is missing";
  } else {
    try {
      auto store = OpenPackage(package_path);
      report.database_healthy = true;
      report.database_revision = store->CurrentRevision();
    } catch (const std::exception& failure) {
      report.database_problem = failure.what();
    }
  }
  report.notes.push_back(report.database_healthy
                             ? "The project database is healthy at revision " + std::to_string(report.database_revision) + "."
                             : "The project database cannot be used: " + report.database_problem + ".");

  const auto scratch = package_path / "recovery-work";
  const auto journal = ListJournalFiles(package_path);
  std::vector<std::string> problems;
  std::int64_t best_reach = -1;
  for (const auto& snapshot : ListSnapshotFiles(package_path)) {
    std::string problem;
    const auto revision = CheckDatabaseCopy(snapshot.path, scratch, problem);
    if (!revision.has_value()) {
      report.notes.push_back("Snapshot " + snapshot.path.filename().string() + " is not usable: " + problem + ".");
      continue;
    }
    report.snapshots.push_back(*revision);
    best_reach = std::max(best_reach, ChainReach(journal, *revision, &problems));
  }
  std::error_code ignored;
  std::filesystem::remove_all(scratch, ignored);

  report.reachable_revision = best_reach;
  if (report.database_healthy) {
    report.reachable_revision = std::max(best_reach, ChainReach(journal, report.database_revision, &problems));
  }
  for (const auto& problem : problems) report.notes.push_back(problem);
  if (!report.snapshots.empty()) {
    report.notes.push_back("Valid snapshots: " + std::to_string(report.snapshots.size()) + "; journal records reach revision " +
                           std::to_string(report.reachable_revision) + ".");
  } else if (!report.database_healthy) {
    report.notes.push_back("There is no usable snapshot to rebuild from.");
  }
  return report;
}

std::unique_ptr<ProjectStore> ProjectStore::RecoverPackage(const std::filesystem::path& package_path,
                                                           RecoveryOptions options, RecoveryReport* out) {
  RecoveryReport local;
  RecoveryReport& report = out != nullptr ? *out : local;
  report = InspectPackage(package_path);
  const auto journal = ListJournalFiles(package_path);
  const auto database = package_path / kDatabaseFileName;

  const auto read_record = [&](std::int64_t revision) {
    for (const auto& file : journal) {
      if (file.revision != revision) continue;
      std::string text;
      if (ReadJournalRecord(file, text).empty()) return text;
    }
    return std::string();
  };

  // A database that works is not touched, unless rolling forward was asked for and
  // there is something to roll forward to.
  if (report.database_healthy) {
    if (!options.roll_forward || report.reachable_revision <= report.database_revision) {
      report.recovered_revision = report.database_revision;
      if (options.dry_run) return nullptr;
      return OpenPackage(package_path);
    }
    if (options.dry_run) {
      report.recovered_revision = report.reachable_revision;
      return nullptr;
    }
    auto store = OpenPackage(package_path);
    {
      const std::lock_guard<std::mutex> lock(store->mutex_);
      for (auto revision = report.database_revision + 1; revision <= report.reachable_revision; ++revision) {
        if (store->ReplayRecordLocked(read_record(revision))) ++report.records_replayed;
      }
    }
    store->ValidateDatabase();
    report.recovered_revision = store->CurrentRevision();
    report.notes.push_back("Rolled forward " + std::to_string(report.records_replayed) + " journal record(s).");
    return store;
  }

  // Rebuild from the snapshot whose chain reaches furthest (later snapshot on a tie).
  std::int64_t chosen = -1;
  std::int64_t chosen_reach = -1;
  std::filesystem::path chosen_path;
  for (const auto& snapshot : ListSnapshotFiles(package_path)) {
    if (std::find(report.snapshots.begin(), report.snapshots.end(), snapshot.revision) == report.snapshots.end()) continue;
    const auto reach = ChainReach(journal, snapshot.revision, nullptr);
    if (reach >= chosen_reach) {
      chosen = snapshot.revision;
      chosen_reach = reach;
      chosen_path = snapshot.path;
    }
  }
  if (chosen < 0) {
    throw std::runtime_error("Project " + package_path.string() + " cannot be recovered: its database is unusable (" +
                             report.database_problem + ") and it has no valid snapshot");
  }
  report.snapshot_used = chosen;
  report.recovered_revision = chosen_reach;
  if (options.dry_run) return nullptr;

  const auto rebuilt = package_path / "project.db.recovering";
  std::error_code error;
  for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove(rebuilt.string() + suffix, error);
  std::filesystem::copy_file(chosen_path, rebuilt, std::filesystem::copy_options::overwrite_existing);

  std::int64_t applied_to = chosen;
  {
    ProjectStore working(rebuilt.string());
    working.Initialize();
    {
      const std::lock_guard<std::mutex> lock(working.mutex_);
      for (auto revision = chosen + 1; revision <= chosen_reach; ++revision) {
        if (working.ReplayRecordLocked(read_record(revision))) ++report.records_replayed;
        applied_to = revision;
      }
    }
    working.ValidateDatabase();
    report.recovered_revision = working.CurrentRevision();
  }
  (void)applied_to;
  for (const char* suffix : {"-wal", "-shm"}) std::filesystem::remove(rebuilt.string() + suffix, error);

  // Everything that is being replaced goes into a quarantine folder, not the bin.
  report.quarantine = package_path / "quarantine" / TimestampForFolder();
  std::filesystem::create_directories(report.quarantine);
  for (const char* suffix : {"", "-wal", "-shm"}) {
    const auto old_file = std::filesystem::path(database.string() + suffix);
    if (std::filesystem::exists(old_file)) std::filesystem::rename(old_file, report.quarantine / old_file.filename());
  }
  // Journal records past what was recovered describe a history this database no
  // longer has; leaving them would put two files at the same revision later.
  for (const auto& file : journal) {
    if (file.revision > report.recovered_revision) {
      std::filesystem::rename(file.path, report.quarantine / file.path.filename());
    }
  }
  std::filesystem::rename(rebuilt, database);

  auto store = OpenPackage(package_path);
  store->CreateSnapshot();  // so the next recovery starts from here
  report.notes.push_back("Rebuilt from snapshot r" + std::to_string(chosen) + " and " + std::to_string(report.records_replayed) +
                         " journal record(s); the project is at revision " + std::to_string(report.recovered_revision) +
                         ". What was replaced is in " + report.quarantine.string() + ".");
  return store;
}

}  // namespace cutline::project

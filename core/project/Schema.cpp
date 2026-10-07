#include "core/project/Schema.h"

#include "core/db/Sql.h"
#include "core/model/RenderVersion.h"

#include "sqlite3.h"

#include <stdexcept>
#include <string>

namespace cutline::project {
namespace {

// Rational columns are stored as an exact numerator/denominator pair, which is
// the authority, plus a `_ticks` integer on the kTicksPerSecond timebase used
// only for indexed range queries. Writers must keep the two in agreement; the
// store does this in one place so the duplication cannot drift.
constexpr const char* kCoreTables = R"sql(
-- Identity and counters. Deliberately NOT part of the undo changeset: undoing
-- an edit must not roll back the revision number or the project's identity.
CREATE TABLE IF NOT EXISTS project_meta(
  singleton      INTEGER PRIMARY KEY CHECK(singleton = 1),
  schema_version INTEGER NOT NULL,
  revision       INTEGER NOT NULL CHECK(revision >= 0),
  project_id     TEXT    NOT NULL DEFAULT '',
  created_at     TEXT    NOT NULL DEFAULT '',
  updated_at     TEXT    NOT NULL DEFAULT '',
  -- Set while an undo/redo changeset is being applied. A changeset already
  -- records every row a cascade removed, so letting the triggers fire again
  -- would delete rows the changeset is also about to delete, and the second
  -- attempt would find them gone and report a conflict.
  suppress_cascades INTEGER NOT NULL DEFAULT 0 CHECK(suppress_cascades IN (0, 1))
);

-- Editable project-level state, kept separate from project_meta precisely so it
-- IS recorded in changesets and therefore undoable.
CREATE TABLE IF NOT EXISTS project_settings(
  singleton      INTEGER PRIMARY KEY CHECK(singleton = 1),
  name           TEXT    NOT NULL DEFAULT '',
  scratch_path   TEXT    NOT NULL DEFAULT '',
  active_sequence_id TEXT REFERENCES sequences(id) ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS bins(
  id         TEXT PRIMARY KEY,
  parent_id  TEXT REFERENCES bins(id) ON DELETE CASCADE,
  sort_order INTEGER NOT NULL,
  name       TEXT    NOT NULL CHECK(name <> '')
);
CREATE INDEX IF NOT EXISTS bins_parent ON bins(parent_id, sort_order);

CREATE TABLE IF NOT EXISTS media(
  id            TEXT PRIMARY KEY,
  bin_id        TEXT REFERENCES bins(id) ON DELETE SET NULL,
  display_name  TEXT    NOT NULL CHECK(display_name <> ''),
  original_path TEXT    NOT NULL,
  fingerprint   TEXT    NOT NULL,
  -- Media duration is required to clamp trims and to compute the handles a
  -- transition consumes; v2 had no way to know how long a source was.
  duration_num   INTEGER NOT NULL DEFAULT 0,
  duration_den   INTEGER NOT NULL DEFAULT 1 CHECK(duration_den > 0),
  duration_ticks INTEGER NOT NULL DEFAULT 0,
  start_timecode_num INTEGER NOT NULL DEFAULT 0,
  start_timecode_den INTEGER NOT NULL DEFAULT 1 CHECK(start_timecode_den > 0),
  -- Relink state: a project opens with its media offline rather than refusing.
  missing       INTEGER NOT NULL DEFAULT 0 CHECK(missing IN (0, 1)),
  probe_json    TEXT    NOT NULL DEFAULT '{}',
  created_at    TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS media_fingerprint ON media(fingerprint);
CREATE INDEX IF NOT EXISTS media_bin ON media(bin_id);

-- Disposable local editing copies. The original remains authoritative for
-- export. A proxy is usable only while source_fingerprint still matches the
-- media row, which makes relinks and replaced source files fail safe.
CREATE TABLE IF NOT EXISTS media_proxies(
  media_id           TEXT PRIMARY KEY REFERENCES media(id) ON DELETE CASCADE,
  path               TEXT    NOT NULL,
  fingerprint        TEXT    NOT NULL,
  source_fingerprint TEXT    NOT NULL,
  codec              TEXT    NOT NULL,
  width              INTEGER NOT NULL CHECK(width > 0),
  height             INTEGER NOT NULL CHECK(height > 0),
  created_at         TEXT    NOT NULL
);

-- Structured probe output. Colour management and codec routing need these as
-- real columns; v2 kept them in an opaque probe_json blob.
CREATE TABLE IF NOT EXISTS media_streams(
  media_id         TEXT    NOT NULL REFERENCES media(id) ON DELETE CASCADE,
  stream_index     INTEGER NOT NULL,
  kind             TEXT    NOT NULL CHECK(kind IN ('video', 'audio', 'subtitle', 'data')),
  codec            TEXT    NOT NULL DEFAULT '',
  width            INTEGER NOT NULL DEFAULT 0,
  height           INTEGER NOT NULL DEFAULT 0,
  pixel_aspect_num INTEGER NOT NULL DEFAULT 1,
  pixel_aspect_den INTEGER NOT NULL DEFAULT 1 CHECK(pixel_aspect_den > 0),
  frame_rate_num   INTEGER NOT NULL DEFAULT 0,
  frame_rate_den   INTEGER NOT NULL DEFAULT 1 CHECK(frame_rate_den > 0),
  cadence          TEXT    NOT NULL DEFAULT 'constant' CHECK(cadence IN ('constant', 'variable')),
  bit_depth        INTEGER NOT NULL DEFAULT 8,
  chroma           TEXT    NOT NULL DEFAULT '',
  field_order      TEXT    NOT NULL DEFAULT 'progressive'
                     CHECK(field_order IN ('progressive', 'upper_first', 'lower_first')),
  color_primaries  TEXT    NOT NULL DEFAULT 'bt709',
  color_transfer   TEXT    NOT NULL DEFAULT 'bt709',
  color_matrix     TEXT    NOT NULL DEFAULT 'bt709',
  color_range      TEXT    NOT NULL DEFAULT 'limited' CHECK(color_range IN ('limited', 'full')),
  sample_rate      INTEGER NOT NULL DEFAULT 0,
  channel_count    INTEGER NOT NULL DEFAULT 0,
  channel_layout   TEXT    NOT NULL DEFAULT '',
  PRIMARY KEY(media_id, stream_index)
);

CREATE TABLE IF NOT EXISTS sequences(
  id               TEXT PRIMARY KEY,
  name             TEXT    NOT NULL CHECK(name <> ''),
  frame_rate_num   INTEGER NOT NULL CHECK(frame_rate_num > 0),
  frame_rate_den   INTEGER NOT NULL CHECK(frame_rate_den > 0),
  width            INTEGER NOT NULL CHECK(width > 0),
  height           INTEGER NOT NULL CHECK(height > 0),
  pixel_aspect_num INTEGER NOT NULL DEFAULT 1,
  pixel_aspect_den INTEGER NOT NULL DEFAULT 1 CHECK(pixel_aspect_den > 0),
  sample_rate      INTEGER NOT NULL CHECK(sample_rate > 0),
  channel_layout   TEXT    NOT NULL DEFAULT 'stereo',
  -- Render semantics. Changing these after the fact changes the output of every
  -- existing edit, which is why they are first-class from the start.
  working_color_space TEXT NOT NULL DEFAULT 'rec709',
  display_color_space TEXT NOT NULL DEFAULT 'rec709',
  field_order      TEXT    NOT NULL DEFAULT 'progressive'
                     CHECK(field_order IN ('progressive', 'upper_first', 'lower_first')),
  drop_frame       INTEGER NOT NULL DEFAULT 0 CHECK(drop_frame IN (0, 1)),
  -- Which version of the rendering rules this sequence was made under. The column's
  -- default is the legacy version on purpose: a row that predates the column was
  -- made under the old rules. Every insert states its own value.
  render_version   INTEGER NOT NULL DEFAULT 1 CHECK(render_version >= 1)
);

CREATE TABLE IF NOT EXISTS tracks(
  id             TEXT PRIMARY KEY,
  sequence_id    TEXT    NOT NULL REFERENCES sequences(id) ON DELETE CASCADE,
  track_type     TEXT    NOT NULL CHECK(track_type IN ('video', 'audio')),
  sort_order     INTEGER NOT NULL,
  locked         INTEGER NOT NULL DEFAULT 0 CHECK(locked IN (0, 1)),
  muted          INTEGER NOT NULL DEFAULT 0 CHECK(muted IN (0, 1)),
  solo           INTEGER NOT NULL DEFAULT 0 CHECK(solo IN (0, 1)),
  -- Audio track channel model: Premiere's track types are structural, not a
  -- per-clip property, so mapping lives here.
  channel_layout TEXT    NOT NULL DEFAULT 'stereo',
  gain_db        REAL    NOT NULL DEFAULT 0.0,
  pan            REAL    NOT NULL DEFAULT 0.0 CHECK(pan BETWEEN -1.0 AND 1.0),
  name           TEXT    NOT NULL DEFAULT '',
  -- Audio routing. A bus is an audio track that holds no clips and sums the tracks
  -- routed to it; a track with no output goes to the sequence's master.
  is_bus         INTEGER NOT NULL DEFAULT 0 CHECK(is_bus IN (0, 1)),
  output_bus_id  TEXT    REFERENCES tracks(id) ON DELETE SET NULL,
  UNIQUE(sequence_id, track_type, sort_order)
);

-- A send taps a track into a bus besides its output: before the fader or after it.
CREATE TABLE IF NOT EXISTS track_sends(
  track_id  TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  bus_id    TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  gain_db   REAL NOT NULL DEFAULT 0.0 CHECK(gain_db BETWEEN -96.0 AND 24.0),
  pre_fader INTEGER NOT NULL DEFAULT 0 CHECK(pre_fader IN (0, 1)),
  PRIMARY KEY(track_id, bus_id),
  CHECK(track_id <> bus_id)
);
CREATE INDEX IF NOT EXISTS tracks_sequence ON tracks(sequence_id, track_type, sort_order);

-- Motion tracking and stabilisation analyses: expensive to make and depended on by whatever follows
-- them, so kept with the clip they were measured on and the fingerprint of the media at the time.
CREATE TABLE IF NOT EXISTS tracking_data(
  id                 TEXT PRIMARY KEY,
  clip_id            TEXT NOT NULL REFERENCES clips(id) ON DELETE CASCADE,
  kind               TEXT NOT NULL CHECK(kind IN ('point', 'plane', 'stabilize')),
  name               TEXT NOT NULL DEFAULT '',
  algorithm          TEXT NOT NULL,
  source_fingerprint TEXT NOT NULL,
  parameters_json    TEXT NOT NULL DEFAULT '{}',
  data_json          TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS tracking_clip ON tracking_data(clip_id);

-- Caption tracks and their cues. A cue is timed text on the sequence's own timeline.
CREATE TABLE IF NOT EXISTS caption_tracks(
  id          TEXT PRIMARY KEY,
  sequence_id TEXT    NOT NULL REFERENCES sequences(id) ON DELETE CASCADE,
  name        TEXT    NOT NULL DEFAULT '',
  language    TEXT    NOT NULL DEFAULT '',
  style_json  TEXT    NOT NULL DEFAULT '{}',
  sort_order  INTEGER NOT NULL,
  UNIQUE(sequence_id, sort_order)
);
CREATE TABLE IF NOT EXISTS captions(
  id          TEXT PRIMARY KEY,
  track_id    TEXT    NOT NULL REFERENCES caption_tracks(id) ON DELETE CASCADE,
  start_num   INTEGER NOT NULL,
  start_den   INTEGER NOT NULL CHECK(start_den > 0),
  end_num     INTEGER NOT NULL,
  end_den     INTEGER NOT NULL CHECK(end_den > 0),
  start_ticks INTEGER NOT NULL,
  end_ticks   INTEGER NOT NULL,
  text        TEXT    NOT NULL CHECK(text <> ''),
  style_json  TEXT    NOT NULL DEFAULT '{}',
  speaker     TEXT    NOT NULL DEFAULT '',
  CHECK(end_ticks > start_ticks)
);
CREATE INDEX IF NOT EXISTS captions_track ON captions(track_id, start_ticks);

-- Multicam groups: recordings of one event kept in step. They sit beside the sequences, not on a
-- timeline. Offsets come from a sync analysis (timecode, marker, audio) and are stored with the
-- method and confidence so a weak match can be told from a strong one.
CREATE TABLE IF NOT EXISTS multicam_groups(
  id                 TEXT PRIMARY KEY,
  name               TEXT    NOT NULL DEFAULT '',
  duration_num       INTEGER NOT NULL CHECK(duration_num > 0),
  duration_den       INTEGER NOT NULL CHECK(duration_den > 0),
  sync_method        TEXT    NOT NULL DEFAULT 'none' CHECK(sync_method IN ('none', 'timecode', 'marker', 'audio', 'manual')),
  reference_angle_id TEXT    NOT NULL DEFAULT '',
  sync_confidence    REAL    NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS multicam_angles(
  group_id           TEXT    NOT NULL REFERENCES multicam_groups(id) ON DELETE CASCADE,
  id                 TEXT    NOT NULL,
  sort_order         INTEGER NOT NULL,
  name               TEXT    NOT NULL DEFAULT '',
  media_id           TEXT    NOT NULL REFERENCES media(id),
  timecode_num       INTEGER NOT NULL DEFAULT 0,
  timecode_den       INTEGER NOT NULL DEFAULT 1 CHECK(timecode_den > 0),
  marker_num         INTEGER,
  marker_den         INTEGER CHECK(marker_den IS NULL OR marker_den > 0),
  offset_num         INTEGER NOT NULL DEFAULT 0,
  offset_den         INTEGER NOT NULL DEFAULT 1 CHECK(offset_den > 0),
  PRIMARY KEY(group_id, id),
  UNIQUE(group_id, sort_order)
);
CREATE TABLE IF NOT EXISTS multicam_switches(
  group_id    TEXT    NOT NULL REFERENCES multicam_groups(id) ON DELETE CASCADE,
  time_num    INTEGER NOT NULL,
  time_den    INTEGER NOT NULL CHECK(time_den > 0),
  time_ticks  INTEGER NOT NULL CHECK(time_ticks >= 0),
  angle_id    TEXT    NOT NULL,
  PRIMARY KEY(group_id, time_ticks),
  FOREIGN KEY(group_id, angle_id) REFERENCES multicam_angles(group_id, id) ON DELETE CASCADE
);

-- Graphics kept in the project: a title or shape document of their own, or an instance of an installed
-- template package with the values for its controls. Clips show one through a project:<id> effect
-- reference. A template version is immutable once installed (the library holds every version a clip may
-- still use), so what a clip drew yesterday it draws tomorrow.
CREATE TABLE IF NOT EXISTS graphic_templates(
  id           TEXT    NOT NULL,
  version      INTEGER NOT NULL CHECK(version >= 1),
  name         TEXT    NOT NULL DEFAULT '',
  package_json TEXT    NOT NULL,
  PRIMARY KEY(id, version)
);
CREATE TABLE IF NOT EXISTS graphics(
  id               TEXT PRIMARY KEY,
  name             TEXT    NOT NULL DEFAULT '',
  kind             TEXT    NOT NULL CHECK(kind IN ('graphic', 'template')),
  document_json    TEXT    NOT NULL DEFAULT '',
  template_id      TEXT    NOT NULL DEFAULT '',
  template_version INTEGER NOT NULL DEFAULT 0,
  values_json      TEXT    NOT NULL DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS clips(
  id          TEXT PRIMARY KEY,
  track_id    TEXT    NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  -- A clip reads either a media item or another sequence. Nesting, multicam,
  -- compound clips, and adjustment layers all depend on this being expressible.
  source_kind TEXT    NOT NULL CHECK(source_kind IN ('media', 'sequence', 'adjustment')),
  media_id    TEXT REFERENCES media(id) ON DELETE RESTRICT,
  nested_sequence_id TEXT REFERENCES sequences(id) ON DELETE RESTRICT,
  source_in_num      INTEGER NOT NULL,
  source_in_den      INTEGER NOT NULL CHECK(source_in_den > 0),
  source_out_num     INTEGER NOT NULL,
  source_out_den     INTEGER NOT NULL CHECK(source_out_den > 0),
  timeline_start_num INTEGER NOT NULL,
  timeline_start_den INTEGER NOT NULL CHECK(timeline_start_den > 0),
  -- Indexed range columns, derived from the rational pairs above.
  timeline_start_ticks INTEGER NOT NULL,
  timeline_end_ticks   INTEGER NOT NULL,
  rate_num    INTEGER NOT NULL CHECK(rate_num > 0),
  rate_den    INTEGER NOT NULL CHECK(rate_den > 0),
  reversed    INTEGER NOT NULL DEFAULT 0 CHECK(reversed IN (0, 1)),
  -- Retimed audio keeps its pitch (time-stretch) instead of changing it (varispeed).
  maintain_pitch INTEGER NOT NULL DEFAULT 0 CHECK(maintain_pitch IN (0, 1)),
  audio_role TEXT NOT NULL DEFAULT '' CHECK(audio_role IN ('', 'dialogue', 'music', 'effects', 'ambience')),
  linked_group TEXT   NOT NULL DEFAULT '',
  enabled     INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0, 1)),
  name        TEXT    NOT NULL DEFAULT '',
  CHECK(timeline_end_ticks >= timeline_start_ticks),
  -- Exactly one source reference, matching source_kind.
  CHECK((source_kind = 'media'      AND media_id IS NOT NULL AND nested_sequence_id IS NULL)
     OR (source_kind = 'sequence'   AND nested_sequence_id IS NOT NULL AND media_id IS NULL)
     OR (source_kind = 'adjustment' AND media_id IS NULL AND nested_sequence_id IS NULL))
);
-- The hot path: "which clips overlap this range on this track" during playback.
CREATE INDEX IF NOT EXISTS clips_track_range ON clips(track_id, timeline_start_ticks, timeline_end_ticks);
CREATE INDEX IF NOT EXISTS clips_media ON clips(media_id);
CREATE INDEX IF NOT EXISTS clips_nested ON clips(nested_sequence_id);
CREATE INDEX IF NOT EXISTS clips_linked ON clips(linked_group);
)sql";

constexpr const char* kEffectTables = R"sql(
-- A transition occupies a span on a track and mixes an outgoing clip into an
-- incoming one. Either side may be absent, which is how a fade from black and a
-- fade to black are represented.
CREATE TABLE IF NOT EXISTS transitions(
  id           TEXT PRIMARY KEY,
  track_id     TEXT    NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  kind         TEXT    NOT NULL CHECK(kind <> ''),
  alignment    TEXT    NOT NULL DEFAULT 'center'
                 CHECK(alignment IN ('center', 'start', 'end', 'custom')),
  from_clip_id TEXT REFERENCES clips(id) ON DELETE CASCADE,
  to_clip_id   TEXT REFERENCES clips(id) ON DELETE CASCADE,
  timeline_start_num INTEGER NOT NULL,
  timeline_start_den INTEGER NOT NULL CHECK(timeline_start_den > 0),
  duration_num       INTEGER NOT NULL CHECK(duration_num > 0),
  duration_den       INTEGER NOT NULL CHECK(duration_den > 0),
  timeline_start_ticks INTEGER NOT NULL,
  timeline_end_ticks   INTEGER NOT NULL,
  CHECK(timeline_end_ticks > timeline_start_ticks),
  CHECK(from_clip_id IS NOT NULL OR to_clip_id IS NOT NULL)
);
CREATE INDEX IF NOT EXISTS transitions_track_range
  ON transitions(track_id, timeline_start_ticks, timeline_end_ticks);

-- Effects attach to clips, tracks, sequences, or transitions. The owner is
-- polymorphic because the parameter and keyframe tables are shared by all four;
-- SQLite cannot enforce that reference, so ValidateSchema checks it instead.
CREATE TABLE IF NOT EXISTS effects(
  id          TEXT PRIMARY KEY,
  owner_kind  TEXT    NOT NULL CHECK(owner_kind IN ('clip', 'track', 'sequence', 'transition')),
  owner_id    TEXT    NOT NULL,
  effect_type TEXT    NOT NULL CHECK(effect_type <> ''),
  sort_order  INTEGER NOT NULL,
  enabled     INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0, 1)),
  -- An intrinsic effect (Motion, Opacity, Volume) cannot be removed, only reset.
  intrinsic   INTEGER NOT NULL DEFAULT 0 CHECK(intrinsic IN (0, 1)),
  preset_name TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS effects_owner ON effects(owner_kind, owner_id, sort_order);

-- First-class masks owned by an effect. The versioned document contains the
-- geometry and local-time animation; keeping it in its own row makes mask edits
-- independently undoable and lets deleting the effect cascade naturally.
CREATE TABLE IF NOT EXISTS effect_masks(
  id            TEXT PRIMARY KEY,
  effect_id     TEXT    NOT NULL REFERENCES effects(id) ON DELETE CASCADE,
  sort_order    INTEGER NOT NULL,
  document_json TEXT    NOT NULL,
  UNIQUE(effect_id, sort_order)
);
CREATE INDEX IF NOT EXISTS effect_masks_effect ON effect_masks(effect_id, sort_order);

CREATE TABLE IF NOT EXISTS effect_parameters(
  id         TEXT PRIMARY KEY,
  effect_id  TEXT    NOT NULL REFERENCES effects(id) ON DELETE CASCADE,
  name       TEXT    NOT NULL CHECK(name <> ''),
  dimension  INTEGER NOT NULL DEFAULT 1 CHECK(dimension BETWEEN 1 AND 4),
  -- Constant value, used whenever the parameter has no keyframes.
  c0 REAL NOT NULL DEFAULT 0.0,
  c1 REAL NOT NULL DEFAULT 0.0,
  c2 REAL NOT NULL DEFAULT 0.0,
  c3 REAL NOT NULL DEFAULT 0.0,
  UNIQUE(effect_id, name)
);

CREATE TABLE IF NOT EXISTS keyframes(
  parameter_id  TEXT    NOT NULL REFERENCES effect_parameters(id) ON DELETE CASCADE,
  time_num      INTEGER NOT NULL,
  time_den      INTEGER NOT NULL CHECK(time_den > 0),
  time_ticks    INTEGER NOT NULL,
  c0 REAL NOT NULL DEFAULT 0.0,
  c1 REAL NOT NULL DEFAULT 0.0,
  c2 REAL NOT NULL DEFAULT 0.0,
  c3 REAL NOT NULL DEFAULT 0.0,
  interpolation TEXT    NOT NULL DEFAULT 'linear'
                  CHECK(interpolation IN ('hold', 'linear', 'bezier', 'ease_in', 'ease_out', 'ease_in_out')),
  out_handle_x REAL NOT NULL DEFAULT 0.33,
  out_handle_y REAL NOT NULL DEFAULT 0.0,
  in_handle_x  REAL NOT NULL DEFAULT 0.67,
  in_handle_y  REAL NOT NULL DEFAULT 0.0,
  PRIMARY KEY(parameter_id, time_ticks)
);
CREATE INDEX IF NOT EXISTS keyframes_parameter ON keyframes(parameter_id, time_ticks);

-- Markers hang off sequences, clips, or media. v2 only had sequence markers.
CREATE TABLE IF NOT EXISTS markers(
  id            TEXT PRIMARY KEY,
  owner_kind    TEXT    NOT NULL CHECK(owner_kind IN ('sequence', 'clip', 'media')),
  owner_id      TEXT    NOT NULL,
  start_num     INTEGER NOT NULL,
  start_den     INTEGER NOT NULL CHECK(start_den > 0),
  end_num       INTEGER NOT NULL,
  end_den       INTEGER NOT NULL CHECK(end_den > 0),
  start_ticks   INTEGER NOT NULL,
  end_ticks     INTEGER NOT NULL,
  label         TEXT    NOT NULL DEFAULT '',
  kind          TEXT    NOT NULL DEFAULT 'comment'
                  CHECK(kind IN ('comment', 'chapter', 'segmentation', 'web_link', 'flv_cue')),
  color         TEXT    NOT NULL DEFAULT '',
  metadata_json TEXT    NOT NULL DEFAULT '{}',
  CHECK(end_ticks >= start_ticks)
);
CREATE INDEX IF NOT EXISTS markers_owner ON markers(owner_kind, owner_id, start_ticks);
)sql";

// Effects and markers reference their owner polymorphically (owner_kind +
// owner_id), so SQLite cannot cascade them with a foreign key. These triggers
// supply the missing cascade.
//
// They are triggers rather than explicit deletes at each call site because the
// owner is often removed by a foreign-key cascade -- deleting a track removes
// its clips, and each of those clips must drop its own effect stack -- which no
// call site can intercept. Triggers also fire during changeset apply bookkeeping
// consistently, so undo and redo stay symmetrical.
constexpr const char* kCascadeTriggers = R"sql(
CREATE TRIGGER IF NOT EXISTS clips_cascade_attachments AFTER DELETE ON clips
WHEN (SELECT suppress_cascades FROM project_meta WHERE singleton = 1) = 0
BEGIN
  DELETE FROM effects WHERE owner_kind = 'clip' AND owner_id = OLD.id;
  DELETE FROM markers WHERE owner_kind = 'clip' AND owner_id = OLD.id;
END;

CREATE TRIGGER IF NOT EXISTS tracks_cascade_attachments AFTER DELETE ON tracks
WHEN (SELECT suppress_cascades FROM project_meta WHERE singleton = 1) = 0
BEGIN
  DELETE FROM effects WHERE owner_kind = 'track' AND owner_id = OLD.id;
END;

CREATE TRIGGER IF NOT EXISTS sequences_cascade_attachments AFTER DELETE ON sequences
WHEN (SELECT suppress_cascades FROM project_meta WHERE singleton = 1) = 0
BEGIN
  DELETE FROM effects WHERE owner_kind = 'sequence' AND owner_id = OLD.id;
  DELETE FROM markers WHERE owner_kind = 'sequence' AND owner_id = OLD.id;
END;

CREATE TRIGGER IF NOT EXISTS transitions_cascade_attachments AFTER DELETE ON transitions
WHEN (SELECT suppress_cascades FROM project_meta WHERE singleton = 1) = 0
BEGIN
  DELETE FROM effects WHERE owner_kind = 'transition' AND owner_id = OLD.id;
END;

CREATE TRIGGER IF NOT EXISTS media_cascade_attachments AFTER DELETE ON media
WHEN (SELECT suppress_cascades FROM project_meta WHERE singleton = 1) = 0
BEGIN
  DELETE FROM markers WHERE owner_kind = 'media' AND owner_id = OLD.id;
END;
)sql";

constexpr const char* kHistoryTables = R"sql(
-- The journal is the durable, append-only record of what happened to this
-- project. `changeset` holds the SQLite session changeset for the command: the
-- exact rows it touched, before and after. That is what makes undo cheap --
-- cost is proportional to the edit, not to the size of the project -- and it is
-- also what a future crash-recovery replay will read.
--
-- There is deliberately no second history table. The in-process undo stack is
-- derived from these rows, so there is only one source of truth.
CREATE TABLE IF NOT EXISTS command_journal(
  revision        INTEGER PRIMARY KEY,
  command_id      TEXT    NOT NULL UNIQUE,
  project_id      TEXT    NOT NULL,
  author_id       TEXT    NOT NULL,
  created_at_utc  TEXT    NOT NULL,
  command_type    TEXT    NOT NULL,
  payload_json    TEXT    NOT NULL,
  label           TEXT    NOT NULL DEFAULT '',
  changeset       BLOB,
  idempotency_key TEXT    NOT NULL UNIQUE,
  journal_file    TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS command_journal_created ON command_journal(created_at_utc);
)sql";

}  // namespace

void CreateSchema(sqlite3* database) {
  db::Execute(database, kCoreTables);
  db::Execute(database, kEffectTables);
  db::Execute(database, kCascadeTriggers);
  db::Execute(database, kHistoryTables);
  db::Execute(database, "INSERT OR IGNORE INTO project_meta(singleton, schema_version, revision) VALUES(1, " +
                            std::to_string(kSchemaVersion) + ", 0);");
  db::Execute(database, "INSERT OR IGNORE INTO project_settings(singleton) VALUES(1);");
  db::Execute(database,
              "UPDATE project_meta SET schema_version = " + std::to_string(kSchemaVersion) + " WHERE singleton = 1;");
}

void MigrateSchema(sqlite3* database, std::int64_t from_version) {
  if (from_version > kSchemaVersion) {
    throw std::runtime_error("Project was written by a newer version of Cutline (schema v" +
                             std::to_string(from_version) + ")");
  }
  if (from_version == kSchemaVersion) return;

  db::Transaction transaction(database);
  if (from_version <= 12) {
    // v12 -> v13: what a clip's sound is for.
    if (db::ScalarInt(database, "SELECT COUNT(*) FROM pragma_table_info('clips') WHERE name = 'audio_role';") == 0) {
      db::Execute(database,
                  "ALTER TABLE clips ADD COLUMN audio_role TEXT NOT NULL DEFAULT '' CHECK(audio_role IN ('', 'dialogue', 'music', 'effects', 'ambience'));");
    }
  }
  if (from_version <= 11) {
    // v11 -> v12: versioned, animated geometric masks owned by effects.
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS effect_masks("
                " id TEXT PRIMARY KEY, effect_id TEXT NOT NULL REFERENCES effects(id) ON DELETE CASCADE,"
                " sort_order INTEGER NOT NULL, document_json TEXT NOT NULL, UNIQUE(effect_id, sort_order));");
    db::Execute(database, "CREATE INDEX IF NOT EXISTS effect_masks_effect ON effect_masks(effect_id, sort_order);");
  }
  if (from_version <= 10) {
    // v10 -> v11: graphics and template packages kept in the project.
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS graphic_templates("
                " id TEXT NOT NULL, version INTEGER NOT NULL CHECK(version >= 1), name TEXT NOT NULL DEFAULT '',"
                " package_json TEXT NOT NULL, PRIMARY KEY(id, version));");
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS graphics("
                " id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL CHECK(kind IN ('graphic', 'template')),"
                " document_json TEXT NOT NULL DEFAULT '', template_id TEXT NOT NULL DEFAULT '',"
                " template_version INTEGER NOT NULL DEFAULT 0, values_json TEXT NOT NULL DEFAULT '{}');");
  }
  if (from_version <= 9) {
    // v9 -> v10: multicam groups, angles and cuts.
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS multicam_groups("
                " id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '',"
                " duration_num INTEGER NOT NULL CHECK(duration_num > 0), duration_den INTEGER NOT NULL CHECK(duration_den > 0),"
                " sync_method TEXT NOT NULL DEFAULT 'none' CHECK(sync_method IN ('none', 'timecode', 'marker', 'audio', 'manual')),"
                " reference_angle_id TEXT NOT NULL DEFAULT '', sync_confidence REAL NOT NULL DEFAULT 0);");
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS multicam_angles("
                " group_id TEXT NOT NULL REFERENCES multicam_groups(id) ON DELETE CASCADE, id TEXT NOT NULL,"
                " sort_order INTEGER NOT NULL, name TEXT NOT NULL DEFAULT '', media_id TEXT NOT NULL REFERENCES media(id),"
                " timecode_num INTEGER NOT NULL DEFAULT 0, timecode_den INTEGER NOT NULL DEFAULT 1 CHECK(timecode_den > 0),"
                " marker_num INTEGER, marker_den INTEGER CHECK(marker_den IS NULL OR marker_den > 0),"
                " offset_num INTEGER NOT NULL DEFAULT 0, offset_den INTEGER NOT NULL DEFAULT 1 CHECK(offset_den > 0),"
                " PRIMARY KEY(group_id, id), UNIQUE(group_id, sort_order));");
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS multicam_switches("
                " group_id TEXT NOT NULL REFERENCES multicam_groups(id) ON DELETE CASCADE,"
                " time_num INTEGER NOT NULL, time_den INTEGER NOT NULL CHECK(time_den > 0),"
                " time_ticks INTEGER NOT NULL CHECK(time_ticks >= 0), angle_id TEXT NOT NULL,"
                " PRIMARY KEY(group_id, time_ticks),"
                " FOREIGN KEY(group_id, angle_id) REFERENCES multicam_angles(group_id, id) ON DELETE CASCADE);");
  }
  if (from_version <= 8) {
    // v8 -> v9: persisted, replaceable proxy associations.
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS media_proxies("
                " media_id TEXT PRIMARY KEY REFERENCES media(id) ON DELETE CASCADE,"
                " path TEXT NOT NULL, fingerprint TEXT NOT NULL, source_fingerprint TEXT NOT NULL,"
                " codec TEXT NOT NULL, width INTEGER NOT NULL CHECK(width > 0),"
                " height INTEGER NOT NULL CHECK(height > 0), created_at TEXT NOT NULL);");
  }
  if (from_version <= 7) {
    // v7 -> v8: caption tracks.
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS caption_tracks("
                " id TEXT PRIMARY KEY, sequence_id TEXT NOT NULL REFERENCES sequences(id) ON DELETE CASCADE,"
                " name TEXT NOT NULL DEFAULT '', language TEXT NOT NULL DEFAULT '', style_json TEXT NOT NULL DEFAULT '{}',"
                " sort_order INTEGER NOT NULL, UNIQUE(sequence_id, sort_order));");
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS captions("
                " id TEXT PRIMARY KEY, track_id TEXT NOT NULL REFERENCES caption_tracks(id) ON DELETE CASCADE,"
                " start_num INTEGER NOT NULL, start_den INTEGER NOT NULL CHECK(start_den > 0),"
                " end_num INTEGER NOT NULL, end_den INTEGER NOT NULL CHECK(end_den > 0),"
                " start_ticks INTEGER NOT NULL, end_ticks INTEGER NOT NULL,"
                " text TEXT NOT NULL CHECK(text <> ''), style_json TEXT NOT NULL DEFAULT '{}', speaker TEXT NOT NULL DEFAULT '',"
                " CHECK(end_ticks > start_ticks));");
    db::Execute(database, "CREATE INDEX IF NOT EXISTS captions_track ON captions(track_id, start_ticks);");
  }
  if (from_version <= 6) {
    // v6 -> v7: stored tracking analyses.
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS tracking_data("
                " id TEXT PRIMARY KEY, clip_id TEXT NOT NULL REFERENCES clips(id) ON DELETE CASCADE,"
                " kind TEXT NOT NULL CHECK(kind IN ('point', 'plane', 'stabilize')), name TEXT NOT NULL DEFAULT '',"
                " algorithm TEXT NOT NULL, source_fingerprint TEXT NOT NULL, parameters_json TEXT NOT NULL DEFAULT '{}',"
                " data_json TEXT NOT NULL);");
    db::Execute(database, "CREATE INDEX IF NOT EXISTS tracking_clip ON tracking_data(clip_id);");
  }
  if (from_version <= 5) {
    // v5 -> v6: audio buses and sends. Tracks made before them route to the master.
    if (db::ScalarInt(database, "SELECT COUNT(*) FROM pragma_table_info('tracks') WHERE name = 'is_bus';") == 0) {
      db::Execute(database, "ALTER TABLE tracks ADD COLUMN is_bus INTEGER NOT NULL DEFAULT 0 CHECK(is_bus IN (0, 1));");
      db::Execute(database, "ALTER TABLE tracks ADD COLUMN output_bus_id TEXT REFERENCES tracks(id) ON DELETE SET NULL;");
    }
    db::Execute(database,
                "CREATE TABLE IF NOT EXISTS track_sends("
                " track_id TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
                " bus_id TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
                " gain_db REAL NOT NULL DEFAULT 0.0 CHECK(gain_db BETWEEN -96.0 AND 24.0),"
                " pre_fader INTEGER NOT NULL DEFAULT 0 CHECK(pre_fader IN (0, 1)),"
                " PRIMARY KEY(track_id, bus_id), CHECK(track_id <> bus_id));");
  }
  if (from_version <= 4) {
    // v4 -> v5: clips played at another speed changed pitch, and still do.
    const auto has_column =
        db::ScalarInt(database, "SELECT COUNT(*) FROM pragma_table_info('clips') WHERE name = 'maintain_pitch';");
    if (has_column == 0) {
      db::Execute(database,
                  "ALTER TABLE clips ADD COLUMN maintain_pitch INTEGER NOT NULL DEFAULT 0 CHECK(maintain_pitch IN (0, 1));");
    }
  }
  if (from_version <= 3) {
    // v3 -> v4. Sequences made before render versions existed were made under the
    // rules that become version 1, and keep them.
    const auto has_column =
        db::ScalarInt(database, "SELECT COUNT(*) FROM pragma_table_info('sequences') WHERE name = 'render_version';");
    if (has_column == 0) {
      db::Execute(database,
                  "ALTER TABLE sequences ADD COLUMN render_version INTEGER NOT NULL DEFAULT 1 CHECK(render_version >= 1);");
    }
  }
  if (from_version <= 2) {
    // v2 -> v3. Tables whose shape is unchanged are left alone; the rest are
    // created by CreateSchema and their v2 rows carried across below.
    CreateSchema(database);

    // v2 kept project identity in a separate `projects` table whose revision
    // could diverge from project_meta. Fold it into the singleton and drop it.
    const auto has_projects =
        db::ScalarInt(database, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'projects';");
    if (has_projects != 0) {
      db::Execute(database,
                  "UPDATE project_meta SET"
                  "  project_id = COALESCE((SELECT id         FROM projects LIMIT 1), project_id),"
                  "  created_at = COALESCE((SELECT created_at FROM projects LIMIT 1), created_at),"
                  "  updated_at = COALESCE((SELECT updated_at FROM projects LIMIT 1), updated_at)"
                  " WHERE singleton = 1;");
      db::Execute(database,
                  "UPDATE project_settings SET"
                  "  name = COALESCE((SELECT name FROM projects LIMIT 1), name)"
                  " WHERE singleton = 1;");
      db::Execute(database, "DROP TABLE projects;");
    }

    // A v2 clips table has neither source_kind nor the tick columns. Rewriting
    // it in place would mean reconstructing every rational pair, so we refuse
    // rather than silently producing a half-converted project.
    const auto has_source_kind =
        db::ScalarInt(database, "SELECT COUNT(*) FROM pragma_table_info('clips') WHERE name = 'source_kind';");
    if (has_source_kind == 0) {
      throw std::runtime_error(
          "This project uses the v2 clip layout and cannot be migrated in place; "
          "re-create it with ProjectStore::CreatePackage and replay its command journal");
    }
  }
  db::Execute(database,
              "UPDATE project_meta SET schema_version = " + std::to_string(kSchemaVersion) + " WHERE singleton = 1;");
  transaction.Commit();
}

std::string FindTransitionProblem(sqlite3* database, const std::string& transition_id) {
  db::Statement statement(database, R"sql(
    SELECT id, problem FROM (
      SELECT id,
        CASE
          WHEN from_clip_id IS NULL AND to_clip_id IS NULL THEN 'is not attached to any clip'
          WHEN from_clip_id IS NOT NULL AND from_track IS NULL THEN 'its outgoing clip no longer exists'
          WHEN to_clip_id IS NOT NULL AND to_track IS NULL THEN 'its incoming clip no longer exists'
          WHEN from_track IS NOT NULL AND from_track <> track_id THEN 'its outgoing clip is on another track'
          WHEN to_track IS NOT NULL AND to_track <> track_id THEN 'its incoming clip is on another track'
          WHEN from_end IS NOT NULL AND to_start IS NOT NULL AND from_end <> to_start
            THEN 'the clips it joins are not adjacent'
          WHEN cut < timeline_start_ticks OR cut > timeline_end_ticks THEN 'it does not cover the cut it joins'
          WHEN alignment = 'start' AND timeline_start_ticks <> cut THEN 'it is aligned to start at the cut but does not'
          WHEN alignment = 'end' AND timeline_end_ticks <> cut THEN 'it is aligned to end at the cut but does not'
          -- Start and end are each rounded to a tick, so the sum can be a tick out.
          WHEN alignment = 'center' AND abs(timeline_start_ticks + timeline_end_ticks - 2 * cut) > 2
            THEN 'it is aligned to the centre of the cut but is off-centre'
          ELSE NULL
        END AS problem
      FROM (
        SELECT t.id AS id, t.track_id AS track_id, t.alignment AS alignment,
               t.from_clip_id AS from_clip_id, t.to_clip_id AS to_clip_id,
               t.timeline_start_ticks AS timeline_start_ticks, t.timeline_end_ticks AS timeline_end_ticks,
               f.track_id AS from_track, f.timeline_end_ticks AS from_end,
               o.track_id AS to_track, o.timeline_start_ticks AS to_start,
               COALESCE(o.timeline_start_ticks, f.timeline_end_ticks) AS cut
          FROM transitions t
          LEFT JOIN clips f ON f.id = t.from_clip_id
          LEFT JOIN clips o ON o.id = t.to_clip_id
         WHERE (?1 IS NULL OR t.id = ?1)
      )
    ) WHERE problem IS NOT NULL LIMIT 1;
  )sql");
  if (transition_id.empty()) {
    statement.BindNull(1);
  } else {
    statement.Bind(1, transition_id);
  }
  if (!statement.Step()) return {};
  return transition_id.empty() ? statement.ColumnText(0) + ": " + statement.ColumnText(1) : statement.ColumnText(1);
}

void ValidateSchema(sqlite3* database) {
  const auto integrity = db::ScalarText(database, "PRAGMA integrity_check;");
  if (integrity != "ok") throw std::runtime_error("Project database failed integrity check: " + integrity);

  {
    db::Statement foreign_keys(database, "PRAGMA foreign_key_check;");
    if (foreign_keys.Step()) {
      throw std::runtime_error("Project database has a broken foreign key in table " + foreign_keys.ColumnText(0));
    }
  }

  // Invariants SQLite cannot express as constraints.
  const auto orphan_effects = db::ScalarInt(database, R"sql(
    SELECT COUNT(*) FROM effects e WHERE
      (e.owner_kind = 'clip'       AND NOT EXISTS(SELECT 1 FROM clips       WHERE id = e.owner_id)) OR
      (e.owner_kind = 'track'      AND NOT EXISTS(SELECT 1 FROM tracks      WHERE id = e.owner_id)) OR
      (e.owner_kind = 'sequence'   AND NOT EXISTS(SELECT 1 FROM sequences   WHERE id = e.owner_id)) OR
      (e.owner_kind = 'transition' AND NOT EXISTS(SELECT 1 FROM transitions WHERE id = e.owner_id));
  )sql");
  if (orphan_effects != 0) {
    throw std::runtime_error("Project database has " + std::to_string(orphan_effects) +
                             " effect(s) whose owner no longer exists");
  }

  const auto orphan_markers = db::ScalarInt(database, R"sql(
    SELECT COUNT(*) FROM markers m WHERE
      (m.owner_kind = 'sequence' AND NOT EXISTS(SELECT 1 FROM sequences WHERE id = m.owner_id)) OR
      (m.owner_kind = 'clip'     AND NOT EXISTS(SELECT 1 FROM clips     WHERE id = m.owner_id)) OR
      (m.owner_kind = 'media'    AND NOT EXISTS(SELECT 1 FROM media     WHERE id = m.owner_id));
  )sql");
  if (orphan_markers != 0) {
    throw std::runtime_error("Project database has " + std::to_string(orphan_markers) +
                             " marker(s) whose owner no longer exists");
  }

  // Two clips may never occupy the same frame on one track. This is the
  // invariant the v2 MoveClip path could violate silently.
  const auto overlaps = db::ScalarInt(database, R"sql(
    SELECT COUNT(*) FROM clips a JOIN clips b
      ON a.track_id = b.track_id AND a.id < b.id
     AND a.timeline_start_ticks < b.timeline_end_ticks
     AND b.timeline_start_ticks < a.timeline_end_ticks;
  )sql");
  if (overlaps != 0) {
    throw std::runtime_error("Project database has " + std::to_string(overlaps) +
                             " overlapping clip pair(s) on a single track");
  }

  // A transition must still join the clips it was made for.
  const auto transition_problem = FindTransitionProblem(database);
  if (!transition_problem.empty()) {
    throw std::runtime_error("Project database has an invalid transition (" + transition_problem + ")");
  }

  // A sequence made under rules newer than this build knows cannot be rendered
  // faithfully, so the project is refused rather than quietly rendered differently.
  const auto newer = db::ScalarInt(
      database, "SELECT COUNT(*) FROM sequences WHERE render_version > " + std::to_string(model::kCurrentRenderVersion) + ";");
  if (newer != 0) {
    throw std::runtime_error("Project database has " + std::to_string(newer) +
                             " sequence(s) made under a newer render version than this build supports (v" +
                             std::to_string(model::kCurrentRenderVersion) + ")");
  }

  const auto version = db::ScalarInt(database, "SELECT schema_version FROM project_meta WHERE singleton = 1;");
  if (version != kSchemaVersion) {
    throw std::runtime_error("Project database reports schema v" + std::to_string(version) + ", expected v" +
                             std::to_string(kSchemaVersion));
  }
}

}  // namespace cutline::project

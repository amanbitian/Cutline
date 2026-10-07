#pragma once

// The command set. Every mutation to a project goes through exactly one of
// these, which is what makes the journal complete and the undo stack reliable.
//
// Payloads describe forward intent only. There are no "previous value" fields:
// undo is driven by the row-level changeset the store records while the command
// runs (see core/db/ChangeSet.h), so a command does not need to know how to
// reverse itself. That is why adding a command here is cheap.

#include "core/anim/Keyframe.h"
#include "core/model/Types.h"
#include "core/time/RationalTime.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cutline::commands {

enum class CommandType {
  // Project
  CreateProject,
  RenameProject,
  // Bins
  CreateBin,
  RenameBin,
  DeleteBin,
  MoveBin,
  // Media
  ImportMedia,
  RemoveMedia,
  RelinkMedia,
  SetMediaStreams,
  AttachProxy,
  DetachProxy,
  // Sequences
  CreateSequence,
  UpdateSequenceSettings,
  DeleteSequence,
  // Tracks
  AddVideoTrack,
  AddAudioTrack,
  RemoveTrack,
  SetTrackState,
  SetTrackRouting,
  // Clips
  InsertClip,
  DeleteClip,
  RippleDeleteClip,
  MoveClip,
  SplitClip,
  TrimClip,
  SetClipEnabled,
  SetClipSpeed,
  LinkClips,
  UnlinkClips,
  SetSpeedRamp,
  ClearSpeedRamp,
  // Multicam
  CreateMulticamGroup,
  SetMulticamSync,
  RecordMulticamSwitch,
  RemoveMulticamSwitch,
  RenameMulticamAngle,
  DeleteMulticamGroup,
  FlattenMulticamGroup,
  // Graphics
  CreateGraphic,
  UpdateGraphic,
  DeleteGraphic,
  InstallGraphicTemplate,
  RemoveGraphicTemplate,
  AddGraphicClip,
  // Analysis
  SaveTrackingData,
  DeleteTrackingData,
  // Captions
  AddCaptionTrack,
  RemoveCaptionTrack,
  UpdateCaptionTrack,
  AddCaptions,
  UpdateCaption,
  RemoveCaptions,
  // Transitions
  AddTransition,
  RemoveTransition,
  SetTransitionTiming,
  // Effects
  AddEffect,
  RemoveEffect,
  SetEffectEnabled,
  ReorderEffect,
  AddMask,
  UpdateMask,
  RemoveMask,
  SetParameterConstant,
  SetKeyframe,
  RemoveKeyframe,
  // Markers
  AddMarker,
  RemoveMarker,
  UpdateMarker,
  SetClipAudioRole,
};

// ---------------------------------------------------------------- project ----

struct CreateProjectPayload final {
  std::string name;
};

struct RenameProjectPayload final {
  std::string name;
};

// -------------------------------------------------------------------- bins ---

struct CreateBinPayload final {
  std::string id;
  std::optional<std::string> parent_id;  // nullopt is a root bin
  std::string name;
  std::int64_t order{};
};

struct RenameBinPayload final {
  std::string id;
  std::string name;
};

struct DeleteBinPayload final {
  std::string id;
};

struct MoveBinPayload final {
  std::string id;
  std::optional<std::string> parent_id;
  std::int64_t order{};
};

// ------------------------------------------------------------------ media ----

// One decoded stream of a media file. Colour fields are first-class because a
// managed colour pipeline cannot be added later without them.
struct MediaStream final {
  std::int64_t stream_index{};
  model::StreamKind kind{model::StreamKind::Video};
  std::string codec;
  std::int64_t width{};
  std::int64_t height{};
  time::FrameRate pixel_aspect{1, 1};
  time::FrameRate frame_rate{0, 1};
  model::Cadence cadence{model::Cadence::Constant};
  std::int64_t bit_depth{8};
  std::string chroma;
  model::FieldOrder field_order{model::FieldOrder::Progressive};
  std::string color_primaries{"bt709"};
  std::string color_transfer{"bt709"};
  std::string color_matrix{"bt709"};
  model::ColorRange color_range{model::ColorRange::Limited};
  std::int64_t sample_rate{};
  std::int64_t channel_count{};
  std::string channel_layout;
};

struct ImportMediaPayload final {
  std::string id;
  std::optional<std::string> bin_id;
  std::string display_name;
  std::string original_path;
  std::string fingerprint;
  // Required: trims are clamped against it and transitions consume it as handle.
  time::RationalTime duration;
  time::RationalTime start_timecode;
  // The streams the file was probed with. Recorded by the same command that
  // creates the media, so a failure cannot leave media with no streams: the old
  // ingest ran import and stream metadata as two commands, and a failure or an
  // intervening edit between them left an item the player could not describe.
  std::vector<MediaStream> streams;
};

struct RemoveMediaPayload final {
  std::string id;
};

struct RelinkMediaPayload final {
  std::string id;
  std::string original_path;
  bool missing{false};
  // Set when the replacement has been fingerprinted. Leaving it empty keeps
  // the prior identity, which is useful when only the containing folder moved.
  std::string fingerprint;
};

struct SetMediaStreamsPayload final {
  std::string media_id;
  std::vector<MediaStream> streams;
};

// A proxy is disposable local media. `source_fingerprint` records exactly
// which original it was generated from, so a relink cannot silently reuse a
// proxy made from different footage.
struct AttachProxyPayload final {
  std::string media_id;
  std::string path;
  std::string fingerprint;
  std::string source_fingerprint;
  std::string codec;
  std::int64_t width{};
  std::int64_t height{};
};

struct DetachProxyPayload final {
  std::string media_id;
};

// -------------------------------------------------------------- sequences ----

struct SequenceSettings final {
  std::string name;
  time::FrameRate frame_rate{};
  std::int64_t width{};
  std::int64_t height{};
  time::FrameRate pixel_aspect{1, 1};
  std::int64_t sample_rate{};
  std::string channel_layout{"stereo"};
  std::string working_color_space{"rec709"};
  std::string display_color_space{"rec709"};
  model::FieldOrder field_order{model::FieldOrder::Progressive};
  bool drop_frame{false};
  // Which version of the rendering rules to make this sequence under. Left unset,
  // a new sequence takes the current version and an update changes nothing; an
  // explicit value on an update is how a sequence is upgraded (or, deliberately,
  // pinned to older rules). See core/model/RenderVersion.h.
  std::optional<std::int64_t> render_version;
};

struct CreateSequencePayload final {
  std::string id;
  SequenceSettings settings;
};

struct UpdateSequenceSettingsPayload final {
  std::string id;
  SequenceSettings settings;
};

struct DeleteSequencePayload final {
  std::string id;
};

// ----------------------------------------------------------------- tracks ----

struct AddTrackPayload final {
  std::string id;
  std::string sequence_id;
  std::int64_t order{};
  std::string channel_layout{"stereo"};  // audio tracks only
  std::string name;
};

struct RemoveTrackPayload final {
  std::string id;
};

struct SetTrackStatePayload final {
  std::string id;
  bool locked{false};
  bool muted{false};
  bool solo{false};
  double gain_db{0.0};
  double pan{0.0};
  std::string name;
};

// One send from a track to a bus: a copy of its signal, scaled, taken before the
// track's fader or after it.
struct TrackSend final {
  std::string bus_id;
  double gain_db{0.0};
  bool pre_fader{false};
};

// Where an audio track's signal goes. Replaces the whole routing: whether the track
// is a bus, its output (empty means the master) and its sends. A bus holds no clips
// and sums what is routed to it; the routing may not contain a loop.
struct SetTrackRoutingPayload final {
  std::string id;
  bool is_bus{false};
  std::string output_bus_id;
  std::vector<TrackSend> sends;
};

// ------------------------------------------------------------------ clips ----

struct InsertClipPayload final {
  std::string id;
  std::string track_id;
  model::SourceKind source_kind{model::SourceKind::Media};
  // Exactly one of these is set, matching source_kind; Adjustment sets neither.
  std::optional<std::string> media_id;
  std::optional<std::string> nested_sequence_id;
  time::RationalTime source_in;
  time::RationalTime source_out;
  time::RationalTime timeline_start;
  time::RationalTime playback_rate{1, 1};
  bool reversed{false};
  // Audio at another speed keeps its pitch (time-stretch) instead of changing it.
  bool maintain_pitch{false};
  std::string linked_group;
  std::string name;
  // What the sound is for (see SetClipAudioRolePayload); empty when nobody has said.
  std::string audio_role;
};

// What a clip's sound is: "dialogue", "music", "effects", "ambience", or empty. It decides which preset chain and which
// loudness target the audio workflow offers, and is carried by a split and by copying the clip.
struct SetClipAudioRolePayload final {
  std::string id;
  std::string role;
};

// Linked clips (the picture and sound of one shot) share a `linked_group`. The
// edits below carry a flag, true by default, saying whether the rest of the
// group follows: a move shifts every member by the same time, a head or tail trim
// trims every member by the same time, a split cuts every member that spans the cut,
// a delete removes them all, a speed change or an enable applies to them all. A
// caller that wants one clip alone (Alt-drag, in the tools it imitates) clears it.
// Slips never propagate. A locked track refuses the whole edit.
struct DeleteClipPayload final {
  std::string id;
  bool propagate_links{true};
};

struct RippleDeleteClipPayload final {
  std::string id;
  bool propagate_links{true};
};

struct MoveClipPayload final {
  std::string id;
  std::string track_id;
  time::RationalTime timeline_start;
  bool propagate_links{true};
};

// Cutting a clip that is linked cuts its partners at the same time. The right-hand
// halves get ids of the form "<new_clip_id>~<partner id>" and share a new link group
// of their own, so that picture and sound stay joined on each side of the cut
// without the two sides being joined to each other. With the flag cleared only this
// clip is cut and its right half is unlinked.
struct SplitClipPayload final {
  std::string id;
  std::string new_clip_id;
  time::RationalTime at;
  bool propagate_links{true};
};

// Covers trim-in, trim-out, and slip: the caller states the resulting source
// range and timeline position, and the store validates it against the media
// duration and the neighbouring clips.
struct TrimClipPayload final {
  std::string id;
  time::RationalTime source_in;
  time::RationalTime source_out;
  time::RationalTime timeline_start;
  bool propagate_links{true};
};

struct SetClipEnabledPayload final {
  std::string id;
  bool enabled{true};
  bool propagate_links{true};
};

struct SetClipSpeedPayload final {
  std::string id;
  time::RationalTime playback_rate{1, 1};
  bool reversed{false};
  bool propagate_links{true};
  // The clip's audio keeps its pitch at this speed (time-stretch) rather than
  // changing it (varispeed, the default). Picture is unaffected.
  bool maintain_pitch{false};
};

// Joins clips into one link group, or releases them from theirs. Clips already in
// another group leave it for the new one; clips of that old group that are not named
// stay where they were.
struct LinkClipsPayload final {
  std::vector<std::string> clip_ids;
  std::string group_id;
};

struct UnlinkClipsPayload final {
  std::vector<std::string> clip_ids;
};

// --------------------------------------------------------------- retiming ----

// One stretch of a speed ramp: it lasts `duration` on the timeline and the picture moves through the
// source at a speed that changes steadily from `start_speed` to `end_speed` (1 is normal, 2 twice as
// fast, 0 a freeze, negative backwards).
struct SpeedSegment final {
  time::RationalTime duration;
  double start_speed{1.0};
  double end_speed{1.0};
};

// Gives a clip a variable speed: the segments are laid end to end from the start of the clip, the source
// the clip uses becomes exactly what they sweep through, and the clip's length on the timeline becomes
// their total. Replaces any ramp the clip had.
struct SetSpeedRampPayload final {
  std::string clip_id;
  std::vector<SpeedSegment> segments;
  bool propagate_links{true};
};

// Takes the ramp off: the clip plays its source at one steady speed, over the length it had.
struct ClearSpeedRampPayload final {
  std::string clip_id;
  bool propagate_links{true};
};

// --------------------------------------------------------------- multicam ----

// A multicam group is several recordings of one event, kept in step. The group lives beside the
// sequences (it is not on a timeline); flattening it puts ordinary clips on a track, and the group
// stays so every angle can still be reached.
struct MulticamAngleSpec final {
  std::string id;
  std::string media_id;
  std::string name;
  // A source-local sync point (a clap, a slate), used by marker sync.
  std::optional<time::RationalTime> marker;
};

struct CreateMulticamGroupPayload final {
  std::string id;
  std::string name;
  // The length of the group's own timeline.
  time::RationalTime duration;
  std::vector<MulticamAngleSpec> angles;
};

struct MulticamAngleOffset final {
  std::string angle_id;
  // Source time of the angle that lines up with the start of the group.
  time::RationalTime source_offset;
};

// Records where each angle lines up. The analysis that found the offsets (timecode, marker or audio
// correlation) happens elsewhere; this stores its result with the method and confidence it had.
struct SetMulticamSyncPayload final {
  std::string group_id;
  // "timecode", "marker", "audio" or "manual".
  std::string method;
  std::string reference_angle_id;
  double confidence{1.0};
  std::vector<MulticamAngleOffset> offsets;
};

// A live cut: from this time the group shows this angle. A cut at an existing time replaces it, and a
// cut to the angle already showing changes nothing.
struct RecordMulticamSwitchPayload final {
  std::string group_id;
  time::RationalTime at;
  std::string angle_id;
};

struct RemoveMulticamSwitchPayload final {
  std::string group_id;
  time::RationalTime at;
};

// Gives an angle a new name ("Wide", "Close-up"); the name is only a label for the person.
struct RenameMulticamAnglePayload final {
  std::string group_id;
  std::string angle_id;
  std::string name;
};

struct DeleteMulticamGroupPayload final {
  std::string id;
};

// Lays the group's cuts out as ordinary clips from timeline_start: the picture on a video track and,
// if an audio track is named, the sound on it. The sound follows the picture's angle unless
// audio_angle_id names one angle to use throughout.
struct FlattenMulticamGroupPayload final {
  std::string group_id;
  std::string video_track_id;
  std::string audio_track_id;
  std::string audio_angle_id;
  time::RationalTime timeline_start;
  // Clip ids are id_prefix + ":v:" or ":a:" and the cut's number.
  std::string id_prefix;
};

// ---------------------------------------------------------------- graphics ----

// A graphic is a title or shape composition kept inside the project, so the picture does not depend on
// any file outside it. It is either a document of its own, or an instance of an installed template
// package with the values given for the template's controls. Clips show it through a "project:<id>"
// reference, so editing the graphic changes every clip that shows it.
struct CreateGraphicPayload final {
  std::string id;
  std::string name;
  // "graphic" or "template".
  std::string kind{"graphic"};
  // kind graphic: the document (effects/GraphicsDocument.h).
  std::string document_json;
  // kind template: which installed package and version, and the control values.
  std::string template_id;
  std::int64_t template_version{0};
  std::map<std::string, std::string> values;
};

struct UpdateGraphicPayload final {
  std::string id;
  std::optional<std::string> name;
  // For a graphic: the new document. For a template instance: the new control values.
  std::optional<std::string> document_json;
  std::optional<std::map<std::string, std::string>> values;
};

struct DeleteGraphicPayload final {
  std::string id;
};

// Adds a template package to the project's library. A package version never changes once installed:
// installing the same id and version again with different content is refused, and a new version is a
// new entry, so clips made with an older one keep drawing exactly what they drew.
struct InstallGraphicTemplatePayload final {
  std::string package_json;
};

struct RemoveGraphicTemplatePayload final {
  std::string template_id;
  std::int64_t version{0};
};

// A clip on a video track that shows the graphic for a length of time. Its time starts at 0 when the clip
// does, which is what the graphic's own animation runs on.
struct AddGraphicClipPayload final {
  std::string clip_id;
  std::string track_id;
  std::string graphic_id;
  time::RationalTime timeline_start;
  time::RationalTime duration;
  std::string name;
};

// --------------------------------------------------------------- analysis ----

// A stored tracking or stabilisation analysis (render/Tracking.h) for a clip. Saving under an id that
// exists replaces it: re-running an analysis is one undoable edit.
struct SaveTrackingDataPayload final {
  std::string id;
  std::string clip_id;
  std::string kind;  // point, plane or stabilize
  std::string name;
  std::string algorithm;
  // The fingerprint of the clip's media when it was measured; a track whose media has since
  // changed is stale.
  std::string source_fingerprint;
  std::string parameters_json{"{}"};
  std::string data_json;
};

struct DeleteTrackingDataPayload final {
  std::string id;
};

// --------------------------------------------------------------- captions ----

// One cue of timed text. Times are on the sequence's timeline.
struct CaptionCuePayload final {
  std::string id;
  time::RationalTime start;
  time::RationalTime end;
  std::string text;
  // Overrides the track's style, field by field (captions/Captions.h).
  std::string style_json{"{}"};
  std::string speaker;
};

struct AddCaptionTrackPayload final {
  std::string id;
  std::string sequence_id;
  std::string name;
  std::string language;  // BCP 47
  std::string style_json{"{}"};
};

struct RemoveCaptionTrackPayload final {
  std::string id;
};

struct UpdateCaptionTrackPayload final {
  std::string id;
  std::string name;
  std::string language;
  std::string style_json{"{}"};
};

// Adds any number of cues in one edit: importing a subtitle file is a single undo.
struct AddCaptionsPayload final {
  std::string track_id;
  std::vector<CaptionCuePayload> cues;
};

struct UpdateCaptionPayload final {
  CaptionCuePayload cue;
};

struct RemoveCaptionsPayload final {
  std::vector<std::string> ids;
};

// ------------------------------------------------------------ transitions ----

struct AddTransitionPayload final {
  std::string id;
  std::string track_id;
  std::string kind;
  model::TransitionAlignment alignment{model::TransitionAlignment::Center};
  // At least one side is required. A missing side is a fade from or to black.
  std::optional<std::string> from_clip_id;
  std::optional<std::string> to_clip_id;
  time::RationalTime timeline_start;
  time::RationalTime duration;
};

struct RemoveTransitionPayload final {
  std::string id;
};

struct SetTransitionTimingPayload final {
  std::string id;
  time::RationalTime timeline_start;
  time::RationalTime duration;
};

// ---------------------------------------------------------------- effects ----

struct EffectParameter final {
  std::string id;
  std::string name;
  anim::Value value;
};

struct AddEffectPayload final {
  std::string id;
  model::EffectOwner owner_kind{model::EffectOwner::Clip};
  std::string owner_id;
  std::string effect_type;
  std::int64_t order{};
  // Intrinsic effects (Motion, Opacity, Volume) are created with their owner and
  // may be reset but never removed.
  bool intrinsic{false};
  std::string preset_name;
  std::vector<EffectParameter> parameters;
};

struct RemoveEffectPayload final {
  std::string id;
};

struct SetEffectEnabledPayload final {
  std::string id;
  bool enabled{true};
};

struct ReorderEffectPayload final {
  std::string id;
  std::int64_t order{};
};

// A mask document is versioned JSON validated by effects/MaskDocument. Keeping
// it out of effect parameters preserves arbitrary Bezier geometry and its own
// animation as one effect-owned object.
struct AddMaskPayload final {
  std::string id;
  std::string effect_id;
  std::int64_t order{};
  std::string document_json;
};

struct UpdateMaskPayload final {
  std::string id;
  std::string document_json;
};

struct RemoveMaskPayload final {
  std::string id;
};

struct SetParameterConstantPayload final {
  std::string parameter_id;
  anim::Value value;
};

struct SetKeyframePayload final {
  std::string parameter_id;
  anim::Keyframe keyframe;
};

struct RemoveKeyframePayload final {
  std::string parameter_id;
  time::RationalTime at;
};

// ---------------------------------------------------------------- markers ----

struct AddMarkerPayload final {
  std::string id;
  model::MarkerOwner owner_kind{model::MarkerOwner::Sequence};
  std::string owner_id;
  time::RationalTime start;
  time::RationalTime end;
  std::string label;
  model::MarkerKind kind{model::MarkerKind::Comment};
  std::string color;
  std::string metadata_json{"{}"};
};

struct RemoveMarkerPayload final {
  std::string id;
};

struct UpdateMarkerPayload final {
  std::string id;
  time::RationalTime start;
  time::RationalTime end;
  std::string label;
  model::MarkerKind kind{model::MarkerKind::Comment};
  std::string color;
  std::string metadata_json{"{}"};
};

using CommandPayload =
    std::variant<CreateProjectPayload, RenameProjectPayload, CreateBinPayload, RenameBinPayload, DeleteBinPayload,
                 MoveBinPayload, ImportMediaPayload, RemoveMediaPayload, RelinkMediaPayload, SetMediaStreamsPayload,
                 AttachProxyPayload, DetachProxyPayload,
                 CreateSequencePayload, UpdateSequenceSettingsPayload, DeleteSequencePayload, AddTrackPayload,
                 RemoveTrackPayload, SetTrackStatePayload, SetTrackRoutingPayload, InsertClipPayload, DeleteClipPayload,
                 RippleDeleteClipPayload, MoveClipPayload, SplitClipPayload, TrimClipPayload, SetClipEnabledPayload,
                 SetClipSpeedPayload, LinkClipsPayload, UnlinkClipsPayload, SetSpeedRampPayload, ClearSpeedRampPayload,
                 CreateMulticamGroupPayload, SetMulticamSyncPayload, RecordMulticamSwitchPayload, RemoveMulticamSwitchPayload,
                 RenameMulticamAnglePayload, DeleteMulticamGroupPayload, FlattenMulticamGroupPayload,
                 CreateGraphicPayload, UpdateGraphicPayload, DeleteGraphicPayload, InstallGraphicTemplatePayload,
                 RemoveGraphicTemplatePayload, AddGraphicClipPayload, SaveTrackingDataPayload, DeleteTrackingDataPayload, AddCaptionTrackPayload, RemoveCaptionTrackPayload,
                 UpdateCaptionTrackPayload, AddCaptionsPayload, UpdateCaptionPayload, RemoveCaptionsPayload, AddTransitionPayload, RemoveTransitionPayload, SetTransitionTimingPayload,
                 AddEffectPayload, RemoveEffectPayload, SetEffectEnabledPayload, ReorderEffectPayload,
                 AddMaskPayload, UpdateMaskPayload, RemoveMaskPayload, SetParameterConstantPayload,
                 SetKeyframePayload, RemoveKeyframePayload, AddMarkerPayload,
                 RemoveMarkerPayload, UpdateMarkerPayload, SetClipAudioRolePayload>;

struct CommandEnvelope final {
  std::string command_id;
  std::string project_id;
  std::string author_id;
  // Optimistic concurrency: the store rejects the command if the project has
  // moved on since the caller read it.
  std::int64_t base_revision{};
  std::string timestamp_utc;
  CommandType type{};
  CommandPayload payload;
  std::string idempotency_key;
};

[[nodiscard]] std::string ToString(CommandType type);
[[nodiscard]] CommandType ParseCommandType(const std::string& name);

// A short human-readable description, used as the undo-stack label ("Undo Move
// Clip") and in the journal.
[[nodiscard]] std::string Label(const CommandEnvelope& command);

// Serialises the payload for the durable journal. Writer only: the undo stack is
// driven by changesets, so nothing needs to parse this back.
[[nodiscard]] std::string PayloadJson(const CommandPayload& payload);

// Checks envelope well-formedness and payload invariants that do not require
// reading project state. State-dependent checks live in ProjectStore.
void Validate(const CommandEnvelope& command);

// True when the command type and the payload alternative disagree, which would
// otherwise let AddVideoTrack carry an InsertClip payload.
[[nodiscard]] bool TypeMatchesPayload(const CommandEnvelope& command);

}  // namespace cutline::commands

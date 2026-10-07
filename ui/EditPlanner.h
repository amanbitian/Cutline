#pragma once

// The editing rules of the timeline: what a gesture means as commands.
//
// Every function here takes a snapshot of the sequence and a description of what the person did (drag these
// clips this far, trim this edge to that time, insert this range at the playhead) and returns an EditPlan: the
// ordered commands that do it, or the reason it cannot be done. Nothing is changed here. The caller runs the plan
// as one group (ProjectStore::ExecuteGroup), so a gesture is one undo step and happens whole or not at all.
//
// The rules follow what editors do. Dragging a clip over others overwrites what is under it (or, in insert mode,
// pushes it aside); a ripple trim moves everything after it; a roll moves a cut; a slip changes which part of the
// media a clip shows; a slide moves a clip between its neighbours. Where a gesture asks for more than is possible
// (past the end of the media, into a neighbour), the plan clamps to what is, and says so.

#include "core/commands/Command.h"
#include "timeline/Sequence.h"
#include "ui/TimelineView.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cutline::ui {

struct PlannedCommand final {
  commands::CommandType type{};
  commands::CommandPayload payload;
};

struct EditPlan final {
  bool ok{false};
  // Why it cannot be done, when !ok.
  std::string refusal;
  // The undo-step label.
  std::string label;
  std::vector<PlannedCommand> commands;
  // Things the plan did that the person might not expect (a trim clamped to the media's end, a clip cut to make room).
  std::vector<std::string> notes;
  // Where the edit's key time ended up after clamping, for a UI to show or place the playhead at.
  std::optional<RationalTime> result_time;

  [[nodiscard]] static EditPlan Refuse(std::string why) {
    EditPlan plan;
    plan.refusal = std::move(why);
    return plan;
  }
};

struct EditContext final {
  const timeline::Sequence* sequence{nullptr};
  // New ids for clips, links and effects the plan creates; must never repeat.
  std::function<std::string(const std::string& prefix)> new_id;
  // The length of a media item, or nothing if unknown (the trim is then not limited by it).
  std::function<std::optional<RationalTime>(const std::string& media_id)> media_duration;
  // The length of a sequence used as a clip source.
  std::function<std::optional<RationalTime>(const std::string& sequence_id)> sequence_duration;
  // A ripple edit shifts later clips on every unlocked track, not only its own.
  bool ripple_all_tracks{true};
  // Whether clips linked to an edited clip are edited with it.
  bool linked{true};
};

enum class OverlapMode {
  // What is under the edit is cut away or removed (what dragging a clip over another does).
  Overwrite,
  // What is at or after the edit is pushed along to make room.
  Insert,
  // Overlap is an error.
  Refuse,
};

// ------------------------------------------------------------------------ moving ----

// Moves clips by `delta` in time and, for the clips on tracks of the same kind as `primary_clip_id`'s, by
// `track_delta` tracks (positive is upward for video, downward for audio, as the rows are drawn). Linked clips
// come along in time if `ctx.linked`.
[[nodiscard]] EditPlan PlanMove(const EditContext& ctx, const std::set<std::string>& clip_ids, const std::string& primary_clip_id,
                                const RationalTime& delta, int track_delta, OverlapMode mode);

// -------------------------------------------------------------------------- trimming ----

enum class TrimEdge { Head, Tail };
enum class TrimMode {
  // The edge moves; the clip gets shorter or longer; nothing else moves.
  Normal,
  // The edge moves and everything after it moves with it.
  Ripple,
};

// Moves one edge of a clip to `to` (a time on the timeline). The result is clamped to the media, to the next clip's
// edge in Normal mode, and to at least one frame of clip.
[[nodiscard]] EditPlan PlanTrim(const EditContext& ctx, const std::string& clip_id, TrimEdge edge, const RationalTime& to, TrimMode mode);
// Moves the cut between two touching clips to `to`: one gets longer, the other shorter, by the same amount.
[[nodiscard]] EditPlan PlanRoll(const EditContext& ctx, const std::string& before_clip_id, const std::string& after_clip_id, const RationalTime& to);
// Changes which part of the media the clip shows, by `delta` of source time at the clip's speed, keeping its place
// on the timeline.
[[nodiscard]] EditPlan PlanSlip(const EditContext& ctx, const std::string& clip_id, const RationalTime& delta);
// Moves the clip by `delta` between its neighbours, which must touch it: the one before gets longer or shorter and
// the one after starts earlier or later.
[[nodiscard]] EditPlan PlanSlide(const EditContext& ctx, const std::string& clip_id, const RationalTime& delta);

// --------------------------------------------------------------------- cutting, removing ----

// Cuts the clips on the given tracks (all tracks if empty) that span `at`. Linked clips are cut together.
[[nodiscard]] EditPlan PlanSplit(const EditContext& ctx, const RationalTime& at, const std::set<std::string>& track_ids = {});
// Cuts the selected clips at `at` (those that span it).
[[nodiscard]] EditPlan PlanSplitClips(const EditContext& ctx, const RationalTime& at, const std::set<std::string>& clip_ids);
[[nodiscard]] EditPlan PlanDelete(const EditContext& ctx, const std::set<std::string>& clip_ids, bool ripple);
// Removes everything in [in, out) from the tracks (all unlocked if empty): clips inside go, clips across an edge are
// trimmed, a clip across both is cut and its middle removed. Extract also closes the gap.
[[nodiscard]] EditPlan PlanLift(const EditContext& ctx, const RationalTime& in, const RationalTime& out, const std::set<std::string>& track_ids = {});
[[nodiscard]] EditPlan PlanExtract(const EditContext& ctx, const RationalTime& in, const RationalTime& out, const std::set<std::string>& track_ids = {});

// Removes several ranges in one plan: each [in, out) as PlanLift (ripple false) or PlanExtract (ripple true) would, on
// the clips as the earlier removals leave them. Ranges may be given in any order and may touch or overlap (they are
// merged). One undo step takes them all back.
struct TimeRange final {
  RationalTime in, out;
};
[[nodiscard]] EditPlan PlanRemoveRanges(const EditContext& ctx, std::vector<TimeRange> ranges, bool ripple, const std::string& label);

// -------------------------------------------------------------------- placing clips ----

// A clip to be placed, described independently of any sequence so the same thing serves insert, overwrite, paste and
// duplicate.
struct ClipSpec final {
  model::SourceKind source_kind{model::SourceKind::Media};
  std::string media_id;
  std::string nested_sequence_id;
  RationalTime source_in, source_out;
  RationalTime playback_rate{1, 1};
  bool reversed{false};
  bool maintain_pitch{false};
  std::string audio_role;
  std::string track_id;
  // Where it goes relative to the time the placement is made at.
  RationalTime offset{0, 1};
  std::string name;
  // Clips of one placement with the same non-empty key are linked together.
  std::string link_key;
  std::vector<timeline::Effect> effects;

  [[nodiscard]] RationalTime duration() const { return source_out.Subtract(source_in).Divide(playback_rate); }
};

// A clip of the sequence as a spec (relative to its own start), to copy and paste or duplicate.
[[nodiscard]] std::optional<ClipSpec> SpecOf(const timeline::Sequence& sequence, const std::string& clip_id, const RationalTime& relative_to);

// Puts the specs on the timeline at `at`. Overwrite removes what is under them; Insert makes room first by cutting
// the clips that span `at` and moving everything from `at` onward later by the length of the placement.
[[nodiscard]] EditPlan PlanPlace(const EditContext& ctx, const std::vector<ClipSpec>& specs, const RationalTime& at, OverlapMode mode, const std::string& label);

// A range of a source, to be placed as picture and/or sound.
struct SourceRange final {
  std::string media_id;
  std::string name;
  RationalTime in, out;
  bool has_video{true};
  bool has_audio{true};
};
// The picture goes on `video_track_id` and the sound on `audio_track_id` (either may be empty to leave that part out),
// linked to each other.
[[nodiscard]] EditPlan PlanInsertEdit(const EditContext& ctx, const SourceRange& source, const RationalTime& at,
                                      const std::string& video_track_id, const std::string& audio_track_id, OverlapMode mode);

// Three- and four-point edits. Of the source's in and out and the sequence's in and out (with the playhead standing
// in for a missing sequence in), three fix the edit and the fourth follows; with all four, the source range is
// retimed to fit only if `fit_to_fill`, otherwise the source out is trimmed to the sequence range.
struct ThreePointInput final {
  std::optional<RationalTime> source_in, source_out;
  std::optional<RationalTime> sequence_in, sequence_out;
  RationalTime playhead;
  RationalTime source_duration;  // the length of the media
};
struct ThreePointResult final {
  bool ok{false};
  std::string refusal;
  RationalTime source_in, source_out;
  RationalTime at;
};
[[nodiscard]] ThreePointResult ResolveThreePoint(const ThreePointInput& input);

// --------------------------------------------------------------------- other edits ----

// Switches clips on or off, links or unlinks them: small plans for the same undo/refusal path.
[[nodiscard]] EditPlan PlanSetEnabled(const EditContext& ctx, const std::set<std::string>& clip_ids, bool enabled);
[[nodiscard]] EditPlan PlanLink(const EditContext& ctx, const std::set<std::string>& clip_ids);
[[nodiscard]] EditPlan PlanUnlink(const EditContext& ctx, const std::set<std::string>& clip_ids);

// The finished plan as envelopes ready for ExecuteGroup. Ids, author, time and idempotency keys are the caller's.
struct EnvelopeFactory final {
  std::string project_id;
  std::string author_id;
  std::string timestamp_utc;
  std::string key_prefix;
  std::int64_t base_revision{0};
};
[[nodiscard]] std::vector<commands::CommandEnvelope> ToEnvelopes(const EditPlan& plan, const EnvelopeFactory& factory, std::int64_t first_serial);

}  // namespace cutline::ui

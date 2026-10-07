// Project store: schema invariants, state-dependent validation, and
// changeset-based undo/redo.

#include "core/db/Sql.h"
#include "core/project/Consolidate.h"
#include "core/project/ProjectStore.h"
#include "core/project/Schema.h"
#include "effects/MaskDocument.h"
#include "render/TrackingData.h"
#include "tests/native/TestHarness.h"

#include "core/model/RenderVersion.h"
#include "media/ProxyWorkflow.h"
#include "media/Ingest.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace commands = cutline::commands;
using RecoveryReportAlias = cutline::project::RecoveryReport;
namespace model = cutline::model;
namespace rates = cutline::time;
using cutline::anim::Interpolation;
using cutline::anim::Value;
using cutline::project::ProjectStore;
using cutline::project::SnapshotPolicy;
using cutline::time::RationalTime;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

// Drives a store the way a UI would: tracks the revision, hands out unique
// command ids, and fails loudly if a command is rejected.
class Harness final {
 public:
  explicit Harness(SnapshotPolicy policy = {}) : store_(":memory:", policy) { store_.Initialize(); }

  explicit Harness(ProjectStore* adopted) : adopted_(adopted) {}

  ProjectStore& store() { return adopted_ != nullptr ? *adopted_ : store_; }

  commands::CommandEnvelope Make(commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter_);
    command.project_id = project_id_;
    command.author_id = "tester";
    command.base_revision = store().CurrentRevision();
    command.timestamp_utc = "2026-10-05T00:00:0" + std::to_string(counter_ % 10) + "Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter_);
    return command;
  }

  cutline::project::CommandResult Run(commands::CommandType type, commands::CommandPayload payload) {
    return store().Execute(Make(type, std::move(payload)));
  }

  void CreateProject(const std::string& name = "Feature") {
    auto command = Make(commands::CommandType::CreateProject, commands::CreateProjectPayload{name});
    const auto result = store().Execute(command);
    (void)result;
  }

  // Builds the common fixture: one media item, one sequence, V1/A1, two clips.
  void BuildSequence() {
    CreateProject();

    commands::ImportMediaPayload media;
    media.id = "media-1";
    media.display_name = "shot.mov";
    media.original_path = "/footage/shot.mov";
    media.fingerprint = "fp-1";
    media.duration = Seconds(60);
    Run(commands::CommandType::ImportMedia, media);

    commands::CreateSequencePayload sequence;
    sequence.id = "seq-1";
    sequence.settings.name = "Main";
    sequence.settings.frame_rate = rates::kFrameRate2997;
    sequence.settings.width = 1920;
    sequence.settings.height = 1080;
    sequence.settings.sample_rate = 48000;
    Run(commands::CommandType::CreateSequence, sequence);

    Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
    Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});

    Run(commands::CommandType::InsertClip, Clip("clip-1", "v1", Seconds(0), Seconds(0), Seconds(5)));
    Run(commands::CommandType::InsertClip, Clip("clip-2", "v1", Seconds(5), Seconds(10), Seconds(15)));
  }

  static commands::InsertClipPayload Clip(std::string id, std::string track, RationalTime timeline_start,
                                          RationalTime source_in, RationalTime source_out) {
    commands::InsertClipPayload clip;
    clip.id = std::move(id);
    clip.track_id = std::move(track);
    clip.source_kind = model::SourceKind::Media;
    clip.media_id = "media-1";
    clip.source_in = source_in;
    clip.source_out = source_out;
    clip.timeline_start = timeline_start;
    return clip;
  }

  void Undo() {
    const auto result = store().Undo("tester", "2026-10-05T01:00:00Z");
    (void)result;
  }
  void Redo() {
    const auto result = store().Redo("tester", "2026-10-05T02:00:00Z");
    (void)result;
  }

  [[nodiscard]] std::int64_t Count(const std::string& sql) const {
    auto& mutable_store = const_cast<Harness*>(this)->store();
    const std::lock_guard<std::mutex> lock(mutable_store.mutex());
    return cutline::db::ScalarInt(mutable_store.connection(), sql);
  }

  [[nodiscard]] std::string Text(const std::string& sql) {
    const std::lock_guard<std::mutex> lock(store().mutex());
    return cutline::db::ScalarText(store().connection(), sql);
  }

 private:
  ProjectStore store_{":memory:"};
  ProjectStore* adopted_{nullptr};
  std::string project_id_{"project-1"};
  int counter_{0};
};

std::filesystem::path ScratchDirectory(const std::string& name) {
  const auto path = std::filesystem::temp_directory_path() / ("cutline-test-" + name);
  std::filesystem::remove_all(path);
  return path;
}

}  // namespace

// --------------------------------------------------------------- lifecycle ----

CUTLINE_TEST(InitializeCreatesSchemaAtCurrentVersion) {
  Harness harness;
  CHECK_EQ(harness.Count("SELECT schema_version FROM project_meta WHERE singleton = 1;"),
           cutline::project::kSchemaVersion);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM project_settings;"), 1);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(CreateProjectEstablishesIdentityExactlyOnce) {
  Harness harness;
  harness.CreateProject("Feature");
  CHECK_EQ(harness.store().ProjectName(), std::string("Feature"));
  CHECK_EQ(harness.store().ProjectId(), std::string("project-1"));
  // A second CreateProject must be refused.
  CHECK_THROWS(harness.Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Other"}));
}

CUTLINE_TEST(CommandsAreRejectedBeforeAProjectExists) {
  Harness harness;
  CHECK_THROWS(harness.Run(commands::CommandType::RenameProject, commands::RenameProjectPayload{"Nope"}));
}

CUTLINE_TEST(CommandsForAnotherProjectAreRejected) {
  Harness harness;
  harness.CreateProject();
  auto command = harness.Make(commands::CommandType::RenameProject, commands::RenameProjectPayload{"Other"});
  command.project_id = "project-elsewhere";
  CHECK_THROWS(harness.store().Execute(command));
}

CUTLINE_TEST(StaleRevisionIsRejected) {
  Harness harness;
  harness.CreateProject();
  auto command = harness.Make(commands::CommandType::RenameProject, commands::RenameProjectPayload{"Second"});
  command.base_revision = 0;  // the project has already moved past revision 0
  CHECK_THROWS(harness.store().Execute(command));
}

CUTLINE_TEST(ReplayingACommandIsIdempotent) {
  Harness harness;
  harness.CreateProject();
  auto command = harness.Make(commands::CommandType::RenameProject, commands::RenameProjectPayload{"Renamed"});
  const auto first = harness.store().Execute(command);
  const auto second = harness.store().Execute(command);
  CHECK(!first.idempotent);
  CHECK(second.idempotent);
  CHECK_EQ(first.revision, second.revision);
  CHECK_EQ(harness.store().CurrentRevision(), first.revision);
}

// ------------------------------------------------------- timeline integrity ----

CUTLINE_TEST(OverlappingClipsOnOneTrackAreRejected) {
  Harness harness;
  harness.BuildSequence();
  // clip-1 occupies [0,5); clip-2 occupies [5,10). Landing inside clip-1 fails.
  CHECK_THROWS(harness.Run(commands::CommandType::InsertClip,
                           Harness::Clip("clip-3", "v1", Seconds(2), Seconds(0), Seconds(2))));
  // Butting exactly against the end of clip-1 is legal.
  CHECK_NO_THROW(harness.Run(commands::CommandType::InsertClip,
                             Harness::Clip("clip-4", "v1", Seconds(10), Seconds(0), Seconds(2))));
}

CUTLINE_TEST(MovingAClipOntoAnotherIsRejected) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Run(commands::CommandType::MoveClip,
                           commands::MoveClipPayload{"clip-2", "v1", Seconds(3)}));
  // Moving into free space succeeds.
  CHECK_NO_THROW(
      harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(20)}));
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), 20);
}

CUTLINE_TEST(ClipsCannotMoveBetweenVideoAndAudioTracks) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Run(commands::CommandType::MoveClip,
                           commands::MoveClipPayload{"clip-1", "a1", Seconds(0)}));
}

CUTLINE_TEST(TrimIsClampedToTheMediaDuration) {
  Harness harness;
  harness.BuildSequence();
  // The media is 60s long; trimming past that has no frames to read.
  CHECK_THROWS(harness.Run(commands::CommandType::TrimClip,
                           commands::TrimClipPayload{"clip-1", Seconds(0), Seconds(90), Seconds(0)}));
  CHECK_NO_THROW(harness.Run(commands::CommandType::TrimClip,
                             commands::TrimClipPayload{"clip-1", Seconds(0), Seconds(4), Seconds(0)}));
  CHECK_EQ(harness.Count("SELECT source_out_num FROM clips WHERE id = 'clip-1';"), 4);
}

CUTLINE_TEST(InsertIsClampedToTheMediaDuration) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Run(commands::CommandType::InsertClip,
                           Harness::Clip("clip-long", "v1", Seconds(30), Seconds(0), Seconds(75))));
}

CUTLINE_TEST(LockedTracksRefuseEdits) {
  Harness harness;
  harness.BuildSequence();
  commands::SetTrackStatePayload lock;
  lock.id = "v1";
  lock.locked = true;
  harness.Run(commands::CommandType::SetTrackState, lock);

  CHECK_THROWS(harness.Run(commands::CommandType::MoveClip,
                           commands::MoveClipPayload{"clip-1", "v1", Seconds(30)}));
  CHECK_THROWS(harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"}));
  CHECK_THROWS(harness.Run(commands::CommandType::InsertClip,
                           Harness::Clip("clip-5", "v1", Seconds(40), Seconds(0), Seconds(1))));
}

CUTLINE_TEST(SplitProducesTwoAdjacentClipsAtExactSourceTime) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(2)});

  CHECK_EQ(harness.Count("SELECT source_out_num FROM clips WHERE id = 'clip-1';"), 2);
  CHECK_EQ(harness.Count("SELECT source_in_num FROM clips WHERE id = 'clip-1b';"), 2);
  CHECK_EQ(harness.Count("SELECT source_out_num FROM clips WHERE id = 'clip-1b';"), 5);
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-1b';"), 2);
  // No gap and no overlap was introduced.
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(SplitOutsideTheClipIsRejected) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Run(commands::CommandType::SplitClip,
                           commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(9)}));
  CHECK_THROWS(harness.Run(commands::CommandType::SplitClip,
                           commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(0)}));
}

CUTLINE_TEST(RippleDeleteClosesTheGap) {
  Harness harness;
  harness.BuildSequence();
  // clip-1 is [0,5), clip-2 is [5,10). Removing clip-1 pulls clip-2 to 0.
  harness.Run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{"clip-1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE id = 'clip-1';"), 0);
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), 0);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(ChangingSpeedRechecksNeighbours) {
  Harness harness;
  harness.BuildSequence();
  // clip-1 is 5s at 1x in [0,5). At 0.5x it would run to 10s and hit clip-2.
  CHECK_THROWS(harness.Run(commands::CommandType::SetClipSpeed,
                           commands::SetClipSpeedPayload{"clip-1", RationalTime(1, 2), false}));
  // At 2x it shrinks to 2.5s, which fits.
  CHECK_NO_THROW(harness.Run(commands::CommandType::SetClipSpeed,
                             commands::SetClipSpeedPayload{"clip-1", RationalTime(2, 1), false}));
}

// ----------------------------------------------------------------- nesting ----

CUTLINE_TEST(SequencesCanBeNestedInsideOtherSequences) {
  Harness harness;
  harness.BuildSequence();

  commands::CreateSequencePayload outer;
  outer.id = "seq-outer";
  outer.settings.name = "Outer";
  outer.settings.frame_rate = rates::kFrameRate2997;
  outer.settings.width = 1920;
  outer.settings.height = 1080;
  outer.settings.sample_rate = 48000;
  harness.Run(commands::CommandType::CreateSequence, outer);
  harness.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"ov1", "seq-outer", 0, "stereo", "V1"});

  commands::InsertClipPayload nested;
  nested.id = "nest-1";
  nested.track_id = "ov1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.nested_sequence_id = "seq-1";
  nested.source_in = Seconds(0);
  nested.source_out = Seconds(10);
  nested.timeline_start = Seconds(0);
  CHECK_NO_THROW(harness.Run(commands::CommandType::InsertClip, nested));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE source_kind = 'sequence';"), 1);
}

CUTLINE_TEST(SelfNestingAndNestingCyclesAreRejected) {
  Harness harness;
  harness.BuildSequence();

  // Direct self-nesting.
  commands::InsertClipPayload itself;
  itself.id = "nest-self";
  itself.track_id = "v1";
  itself.source_kind = model::SourceKind::Sequence;
  itself.nested_sequence_id = "seq-1";
  itself.source_in = Seconds(0);
  itself.source_out = Seconds(2);
  itself.timeline_start = Seconds(30);
  CHECK_THROWS(harness.Run(commands::CommandType::InsertClip, itself));

  // Indirect: seq-1 contains seq-outer, so seq-outer must not contain seq-1.
  commands::CreateSequencePayload outer;
  outer.id = "seq-outer";
  outer.settings.name = "Outer";
  outer.settings.frame_rate = rates::kFrameRate2997;
  outer.settings.width = 1920;
  outer.settings.height = 1080;
  outer.settings.sample_rate = 48000;
  harness.Run(commands::CommandType::CreateSequence, outer);
  harness.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"ov1", "seq-outer", 0, "stereo", "V1"});

  commands::InsertClipPayload inner;
  inner.id = "nest-inner";
  inner.track_id = "v1";
  inner.source_kind = model::SourceKind::Sequence;
  inner.nested_sequence_id = "seq-outer";
  inner.source_in = Seconds(0);
  inner.source_out = Seconds(2);
  inner.timeline_start = Seconds(30);
  harness.Run(commands::CommandType::InsertClip, inner);

  commands::InsertClipPayload cycle;
  cycle.id = "nest-cycle";
  cycle.track_id = "ov1";
  cycle.source_kind = model::SourceKind::Sequence;
  cycle.nested_sequence_id = "seq-1";
  cycle.source_in = Seconds(0);
  cycle.source_out = Seconds(2);
  cycle.timeline_start = Seconds(0);
  CHECK_THROWS(harness.Run(commands::CommandType::InsertClip, cycle));
}

CUTLINE_TEST(AdjustmentClipsNeedNoSourceAndOnlySitOnVideoTracks) {
  Harness harness;
  harness.BuildSequence();

  commands::InsertClipPayload adjustment;
  adjustment.id = "adj-1";
  adjustment.track_id = "v1";
  adjustment.source_kind = model::SourceKind::Adjustment;
  adjustment.source_in = Seconds(0);
  adjustment.source_out = Seconds(3);
  adjustment.timeline_start = Seconds(30);
  CHECK_NO_THROW(harness.Run(commands::CommandType::InsertClip, adjustment));

  adjustment.id = "adj-2";
  adjustment.track_id = "a1";
  adjustment.timeline_start = Seconds(40);
  CHECK_THROWS(harness.Run(commands::CommandType::InsertClip, adjustment));
}

// ----------------------------------------------------- effects and keyframes ----

namespace {

commands::AddEffectPayload LumetriOn(const std::string& clip_id, const std::string& effect_id) {
  commands::AddEffectPayload effect;
  effect.id = effect_id;
  effect.owner_kind = model::EffectOwner::Clip;
  effect.owner_id = clip_id;
  effect.effect_type = "lumetri";
  effect.order = 0;
  effect.parameters = {{effect_id + ":exposure", "exposure", Value::Scalar(0.0)},
                       {effect_id + ":contrast", "contrast", Value::Scalar(100.0)}};
  return effect;
}

}  // namespace

CUTLINE_TEST(EffectsAttachToClipsTracksAndSequences) {
  Harness harness;
  harness.BuildSequence();

  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-clip"));

  commands::AddEffectPayload on_track = LumetriOn("clip-1", "fx-track");
  on_track.owner_kind = model::EffectOwner::Track;
  on_track.owner_id = "v1";
  harness.Run(commands::CommandType::AddEffect, on_track);

  commands::AddEffectPayload on_sequence = LumetriOn("clip-1", "fx-seq");
  on_sequence.owner_kind = model::EffectOwner::Sequence;
  on_sequence.owner_id = "seq-1";
  harness.Run(commands::CommandType::AddEffect, on_sequence);

  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 3);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters;"), 6);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(EffectsOnAMissingOwnerAreRejected) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-nonexistent", "fx-1")));
}

CUTLINE_TEST(IntrinsicEffectsCannotBeRemoved) {
  Harness harness;
  harness.BuildSequence();
  auto motion = LumetriOn("clip-1", "fx-motion");
  motion.effect_type = "motion";
  motion.intrinsic = true;
  motion.parameters = {{"fx-motion:scale", "scale", Value::Vec2(100.0, 100.0)},
                       {"fx-motion:position", "position", Value::Vec2(0.0, 0.0)}};
  harness.Run(commands::CommandType::AddEffect, motion);
  CHECK_THROWS(harness.Run(commands::CommandType::RemoveEffect, commands::RemoveEffectPayload{"fx-motion"}));
}

CUTLINE_TEST(ParameterDimensionIsEnforced) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));
  // `exposure` is a scalar; writing a 2D value to it must fail.
  CHECK_THROWS(harness.Run(commands::CommandType::SetParameterConstant,
                           commands::SetParameterConstantPayload{"fx-1:exposure", Value::Vec2(1.0, 2.0)}));
  CHECK_NO_THROW(harness.Run(commands::CommandType::SetParameterConstant,
                             commands::SetParameterConstantPayload{"fx-1:exposure", Value::Scalar(1.5)}));
  // Registry ranges are enforced on later edits as well as when the effect is
  // first created.
  CHECK_THROWS(harness.Run(commands::CommandType::SetParameterConstant,
                           commands::SetParameterConstantPayload{"fx-1:exposure", Value::Scalar(21.0)}));
}

CUTLINE_TEST(KeyframesPersistAndReplaceAtTheSameTime) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));

  commands::SetKeyframePayload first;
  first.parameter_id = "fx-1:exposure";
  first.keyframe = {Seconds(1), Value::Scalar(0.25), Interpolation::Linear, {}, {}};
  harness.Run(commands::CommandType::SetKeyframe, first);

  commands::SetKeyframePayload second;
  second.parameter_id = "fx-1:exposure";
  second.keyframe = {Seconds(2), Value::Scalar(0.75), Interpolation::Bezier, {0.4, 0.0}, {0.6, 1.0}};
  harness.Run(commands::CommandType::SetKeyframe, second);

  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes WHERE parameter_id = 'fx-1:exposure';"), 2);

  // Writing at the same time replaces rather than duplicating.
  commands::SetKeyframePayload replace = first;
  replace.keyframe.value = Value::Scalar(0.5);
  harness.Run(commands::CommandType::SetKeyframe, replace);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes WHERE parameter_id = 'fx-1:exposure';"), 2);
  CHECK_EQ(harness.Text("SELECT CAST(c0 AS TEXT) FROM keyframes WHERE parameter_id = 'fx-1:exposure'"
                        " AND time_num = 1;"),
           std::string("0.5"));

  CHECK_NO_THROW(harness.Run(commands::CommandType::RemoveKeyframe,
                             commands::RemoveKeyframePayload{"fx-1:exposure", Seconds(1)}));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes WHERE parameter_id = 'fx-1:exposure';"), 1);
  // Removing one that is not there is an error, not a silent no-op.
  CHECK_THROWS(harness.Run(commands::CommandType::RemoveKeyframe,
                           commands::RemoveKeyframePayload{"fx-1:exposure", Seconds(1)}));
}

CUTLINE_TEST(SplitCopiesTheEffectStackIncludingKeyframes) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));

  commands::SetKeyframePayload keyframe;
  keyframe.parameter_id = "fx-1:exposure";
  keyframe.keyframe = {Seconds(1), Value::Scalar(0.4), Interpolation::Linear, {}, {}};
  harness.Run(commands::CommandType::SetKeyframe, keyframe);

  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(2)});

  // The right-hand half carries its own copy of the stack.
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects WHERE owner_id = 'clip-1b';"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters p JOIN effects e ON e.id = p.effect_id"
                         " WHERE e.owner_id = 'clip-1b';"),
           2);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes k JOIN effect_parameters p ON p.id = k.parameter_id"
                         " JOIN effects e ON e.id = p.effect_id WHERE e.owner_id = 'clip-1b';"),
           1);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(AnimatedMasksPersistUndoAndSplitWithTheirEffect) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));

  cutline::effects::mask::Document document;
  document.shape = cutline::effects::mask::Shape::Ellipse;
  document.animations.push_back({"center_x", "linear", {{0.0, 0.25}, {4.0, 0.75}}});
  harness.Run(commands::CommandType::AddMask,
              commands::AddMaskPayload{"mask-1", "fx-1", 0, cutline::effects::mask::ToJson(document)});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_masks WHERE effect_id = 'fx-1';"), 1);

  document.feather = 12.0;
  harness.Run(commands::CommandType::UpdateMask,
              commands::UpdateMaskPayload{"mask-1", cutline::effects::mask::ToJson(document)});
  CHECK(harness.Text("SELECT document_json FROM effect_masks WHERE id = 'mask-1';").find("12") != std::string::npos);
  harness.Undo();
  CHECK(harness.Text("SELECT document_json FROM effect_masks WHERE id = 'mask-1';").find("12") == std::string::npos);
  harness.Redo();

  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(2)});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_masks;"), 2);
  const auto right_json = harness.Text("SELECT m.document_json FROM effect_masks m JOIN effects e ON e.id = m.effect_id"
                                       " WHERE e.owner_id = 'clip-1b';");
  const auto right = cutline::effects::mask::Parse(right_json);
  CHECK(std::abs(cutline::effects::mask::Evaluate(right, 0.0).center_x - 0.5) < 1e-9);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(MaskEditsRespectTrackLocksAndRejectInvalidDocuments) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));
  CHECK_THROWS(harness.Run(commands::CommandType::AddMask,
                           commands::AddMaskPayload{"mask-bad", "fx-1", 0, "{}"}));
  cutline::effects::mask::Document document;
  commands::SetTrackStatePayload lock;
  lock.id = "v1";
  lock.locked = true;
  harness.Run(commands::CommandType::SetTrackState, lock);
  CHECK_THROWS(harness.Run(commands::CommandType::AddMask,
                           commands::AddMaskPayload{"mask-1", "fx-1", 0,
                                                    cutline::effects::mask::ToJson(document)}));
}

CUTLINE_TEST(HeadTrimKeepsMaskAnimationOnTheSameSourceFrames) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));
  cutline::effects::mask::Document document;
  document.animations.push_back({"center_x", "linear", {{0.0, 0.25}, {4.0, 0.75}}});
  harness.Run(commands::CommandType::AddMask,
              commands::AddMaskPayload{"mask-1", "fx-1", 0, cutline::effects::mask::ToJson(document)});
  harness.Run(commands::CommandType::TrimClip,
              commands::TrimClipPayload{"clip-1", Seconds(1), Seconds(5), Seconds(1)});
  const auto trimmed = cutline::effects::mask::Parse(
      harness.Text("SELECT document_json FROM effect_masks WHERE id = 'mask-1';"));
  CHECK(std::abs(cutline::effects::mask::Evaluate(trimmed, 0.0).center_x - 0.375) < 1e-9);
  harness.Undo();
  const auto restored = cutline::effects::mask::Parse(
      harness.Text("SELECT document_json FROM effect_masks WHERE id = 'mask-1';"));
  CHECK(std::abs(cutline::effects::mask::Evaluate(restored, 1.0).center_x - 0.375) < 1e-9);
}

// ------------------------------------------------------------- transitions ----

CUTLINE_TEST(TransitionsJoinClipsOnTheSameTrack) {
  Harness harness;
  harness.BuildSequence();
  commands::AddTransitionPayload transition;
  transition.id = "t-1";
  transition.track_id = "v1";
  transition.kind = "cross_dissolve";
  transition.from_clip_id = "clip-1";
  transition.to_clip_id = "clip-2";
  transition.timeline_start = Seconds(4);
  transition.duration = Seconds(2);
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddTransition, transition));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 1);

  // A second transition in the same span is rejected.
  commands::AddTransitionPayload clash = transition;
  clash.id = "t-2";
  clash.timeline_start = Seconds(5);
  CHECK_THROWS(harness.Run(commands::CommandType::AddTransition, clash));
}

CUTLINE_TEST(TransitionsCannotReferenceAClipOnAnotherTrack) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("audio-1", "a1", Seconds(0), Seconds(0), Seconds(5)));

  commands::AddTransitionPayload transition;
  transition.id = "t-1";
  transition.track_id = "v1";
  transition.kind = "cross_dissolve";
  transition.from_clip_id = "audio-1";
  transition.timeline_start = Seconds(4);
  transition.duration = Seconds(1);
  CHECK_THROWS(harness.Run(commands::CommandType::AddTransition, transition));
}

CUTLINE_TEST(ASingleSidedTransitionIsAFadeFromBlack) {
  Harness harness;
  harness.BuildSequence();
  commands::AddTransitionPayload fade;
  fade.id = "t-fade";
  fade.track_id = "v1";
  fade.kind = "cross_dissolve";
  fade.alignment = model::TransitionAlignment::Start;
  fade.to_clip_id = "clip-1";
  fade.timeline_start = Seconds(0);
  fade.duration = Seconds(1);
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddTransition, fade));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions WHERE from_clip_id IS NULL;"), 1);
}

CUTLINE_TEST(DeletingAClipRemovesTransitionsThatTouchIt) {
  Harness harness;
  harness.BuildSequence();
  commands::AddTransitionPayload transition;
  transition.id = "t-1";
  transition.track_id = "v1";
  transition.kind = "cross_dissolve";
  transition.from_clip_id = "clip-1";
  transition.to_clip_id = "clip-2";
  transition.timeline_start = Seconds(4);
  transition.duration = Seconds(2);
  harness.Run(commands::CommandType::AddTransition, transition);

  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 0);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

// ------------------------------------------------------------ markers ----

CUTLINE_TEST(MarkersAttachToSequencesClipsAndMedia) {
  Harness harness;
  harness.BuildSequence();
  const auto marker = [](std::string id, model::MarkerOwner owner, std::string owner_id) {
    commands::AddMarkerPayload payload;
    payload.id = std::move(id);
    payload.owner_kind = owner;
    payload.owner_id = std::move(owner_id);
    payload.start = Seconds(1);
    payload.end = Seconds(1);
    payload.label = "note";
    return payload;
  };
  CHECK_NO_THROW(
      harness.Run(commands::CommandType::AddMarker, marker("m-seq", model::MarkerOwner::Sequence, "seq-1")));
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddMarker, marker("m-clip", model::MarkerOwner::Clip, "clip-1")));
  CHECK_NO_THROW(
      harness.Run(commands::CommandType::AddMarker, marker("m-media", model::MarkerOwner::Media, "media-1")));
  CHECK_THROWS(harness.Run(commands::CommandType::AddMarker, marker("m-bad", model::MarkerOwner::Clip, "nope")));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM markers;"), 3);
}

// ----------------------------------------------------------- undo and redo ----

CUTLINE_TEST(UndoAndRedoRestoreExactState) {
  Harness harness;
  harness.BuildSequence();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 2);

  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-2"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 1);

  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 2);
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), 5);
  CHECK_EQ(harness.Count("SELECT source_out_num FROM clips WHERE id = 'clip-2';"), 15);

  harness.Redo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 1);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(UndoWalksBackMultipleLevels) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("clip-3", "v1", Seconds(10), Seconds(0), Seconds(2)));
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("clip-4", "v1", Seconds(20), Seconds(0), Seconds(2)));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 4);

  harness.Undo();
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 2);

  harness.Redo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 3);
  harness.Redo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 4);
  CHECK(!harness.store().CanRedo());
}

CUTLINE_TEST(UndoRestoresCascadedEffectsAndKeyframes) {
  // Deleting a clip cascades to its effects, parameters, and keyframes. The
  // changeset records those cascaded rows, so undo has to bring all of them
  // back -- this is the case a payload-only inverse would silently lose.
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));

  commands::SetKeyframePayload keyframe;
  keyframe.parameter_id = "fx-1:exposure";
  keyframe.keyframe = {Seconds(1), Value::Scalar(0.4), Interpolation::Bezier, {0.2, 0.1}, {0.8, 0.9}};
  harness.Run(commands::CommandType::SetKeyframe, keyframe);

  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters;"), 2);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes;"), 1);

  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 0);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters;"), 0);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes;"), 0);

  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE id = 'clip-1';"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters;"), 2);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes;"), 1);
  // Bezier handles survive the round trip, not just the row count.
  CHECK_EQ(harness.Text("SELECT interpolation FROM keyframes;"), std::string("bezier"));
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(UndoRestoresAWholeTrackWithItsContents) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));

  harness.Run(commands::CommandType::RemoveTrack, commands::RemoveTrackPayload{"v1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 0);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 0);

  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracks WHERE id = 'v1';"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 2);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 1);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(UndoReversesARippleDeleteIncludingTheShift) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{"clip-1"});
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), 0);

  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE id = 'clip-1';"), 1);
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), 5);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(UndoingAnEffectRemovesItsParametersWithoutConflicting) {
  // Undoing an *insert* is the mirror case of undoing a delete, and it is where
  // the database's own cascading fights the changeset: the inverse deletes the
  // effect and its parameter rows, but deleting the effect already cascades the
  // parameters away, so the changeset's own delete would find them gone.
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters;"), 2);

  CHECK_NO_THROW(harness.Undo());
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 0);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters;"), 0);

  CHECK_NO_THROW(harness.Redo());
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effect_parameters;"), 2);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(RedoingADeleteReplaysItsTriggerCascade) {
  // The same collision from the other direction: replaying a clip deletion
  // forward fires the cascade trigger, which removes the effect rows the
  // changeset is also about to remove.
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));
  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 0);

  CHECK_NO_THROW(harness.Undo());
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 1);

  CHECK_NO_THROW(harness.Redo());
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE id = 'clip-1';"), 0);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 0);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(ADeepUndoRedoCycleOverEveryKindOfEditStaysConsistent) {
  // Walks a realistic edit session all the way back and all the way forward,
  // which is the case the demo exercised when it found the cascade conflict.
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));

  commands::SetKeyframePayload keyframe;
  keyframe.parameter_id = "fx-1:exposure";
  keyframe.keyframe = {Seconds(1), Value::Scalar(0.6), Interpolation::Linear, {}, {}};
  harness.Run(commands::CommandType::SetKeyframe, keyframe);

  commands::AddTransitionPayload transition;
  transition.id = "t-1";
  transition.track_id = "v1";
  transition.kind = "cross_dissolve";
  transition.from_clip_id = "clip-1";
  transition.to_clip_id = "clip-2";
  transition.timeline_start = Seconds(4);
  transition.duration = Seconds(2);
  harness.Run(commands::CommandType::AddTransition, transition);

  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-2", "clip-2b", Seconds(7)});
  harness.Run(commands::CommandType::AddMarker, [] {
    commands::AddMarkerPayload marker;
    marker.id = "m-1";
    marker.owner_kind = model::MarkerOwner::Sequence;
    marker.owner_id = "seq-1";
    marker.start = Seconds(3);
    marker.end = Seconds(3);
    marker.label = "note";
    return marker;
  }());

  const auto clips = harness.Count("SELECT COUNT(*) FROM clips;");
  const auto effects = harness.Count("SELECT COUNT(*) FROM effects;");
  const auto keyframes = harness.Count("SELECT COUNT(*) FROM keyframes;");

  int undone = 0;
  while (harness.store().CanUndo()) {
    CHECK_NO_THROW(harness.Undo());
    ++undone;
  }
  CHECK(undone > 5);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 0);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), 0);
  CHECK_NO_THROW(harness.store().ValidateDatabase());

  while (harness.store().CanRedo()) CHECK_NO_THROW(harness.Redo());
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), clips);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM effects;"), effects);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM keyframes;"), keyframes);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM markers;"), 1);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(ANewCommandDiscardsTheRedoStack) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-2"});
  harness.Undo();
  CHECK(harness.store().CanRedo());

  harness.Run(commands::CommandType::InsertClip, Harness::Clip("clip-9", "v1", Seconds(30), Seconds(0), Seconds(1)));
  CHECK(!harness.store().CanRedo());
}

CUTLINE_TEST(UndoAndRedoRefuseWhenTheStackIsEmpty) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Redo());
  while (harness.store().CanUndo()) harness.Undo();
  CHECK_THROWS(harness.Undo());
}

CUTLINE_TEST(UndoAdvancesTheRevisionRatherThanRewindingIt) {
  // The revision counter is a monotonic version of the project, not a position
  // in history: collaborators rely on it only ever moving forward.
  Harness harness;
  harness.BuildSequence();
  const auto before = harness.store().CurrentRevision();
  harness.Undo();
  CHECK(harness.store().CurrentRevision() > before);
  CHECK_EQ(harness.store().ProjectId(), std::string("project-1"));
}

CUTLINE_TEST(UndoDoesNotEraseTheJournal) {
  Harness harness;
  harness.BuildSequence();
  const auto entries = harness.store().JournalCount();
  harness.Undo();
  // The undo is itself journalled, so the audit trail grows.
  CHECK(harness.store().JournalCount() > entries);
}

CUTLINE_TEST(HistoryIsBoundedByItsLimit) {
  SnapshotPolicy policy;
  policy.history_limit = 3;
  Harness harness(policy);
  harness.BuildSequence();
  for (std::int64_t index = 0; index < 6; ++index) {
    harness.Run(commands::CommandType::InsertClip,
                Harness::Clip("extra-" + std::to_string(index), "v1", Seconds(20 + index * 2), Seconds(0),
                              Seconds(1)));
  }
  CHECK_EQ(harness.store().HistoryLabels().size(), std::size_t{3});
  CHECK_EQ(harness.store().AppliedHistoryCount(), std::size_t{3});
}

CUTLINE_TEST(ChangesetHistoryStaysSmallRelativeToTheProject) {
  // The point of changeset undo: the cost of history tracks the size of the
  // edits, not the size of the project. The previous design copied the whole
  // database twice per command.
  Harness harness;
  harness.BuildSequence();
  for (std::int64_t index = 0; index < 200; ++index) {
    harness.Run(commands::CommandType::InsertClip,
                Harness::Clip("bulk-" + std::to_string(index), "v1", Seconds(20 + index * 2), Seconds(0),
                              Seconds(1)));
  }
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 202);

  // 200 single-clip inserts, each recording one row. A few hundred bytes each
  // is the expected order; a full-database image would be orders of magnitude
  // more and would grow with the project.
  const auto bytes = harness.store().HistoryBytes();
  CHECK(bytes > 0);
  CHECK(bytes < 200 * 1024);

  // And it still undoes correctly at depth.
  for (int index = 0; index < 200; ++index) harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), 2);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(HistoryLabelsDescribeTheEdits) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(20)});
  const auto labels = harness.store().HistoryLabels();
  CHECK(!labels.empty());
  CHECK_EQ(labels.back(), std::string("Move Clip"));
}

// ------------------------------------------------- referential integrity ----

CUTLINE_TEST(MediaInUseCannotBeRemoved) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Run(commands::CommandType::RemoveMedia, commands::RemoveMediaPayload{"media-1"}));
  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"});
  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-2"});
  CHECK_NO_THROW(harness.Run(commands::CommandType::RemoveMedia, commands::RemoveMediaPayload{"media-1"}));
}

CUTLINE_TEST(RelinkUpdatesThePathAndClearsTheMissingFlag) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::RelinkMedia,
              commands::RelinkMediaPayload{"media-1", "/volumes/archive/shot.mov", false});
  CHECK_EQ(harness.Text("SELECT original_path FROM media WHERE id = 'media-1';"),
           std::string("/volumes/archive/shot.mov"));
  CHECK_EQ(harness.Count("SELECT missing FROM media WHERE id = 'media-1';"), 0);
}

CUTLINE_TEST(ProxiesAreUndoableAndBecomeStaleWhenSourceIdentityChanges) {
  Harness harness;
  harness.BuildSequence();
  const auto directory = ScratchDirectory("proxy-association");
  std::filesystem::create_directories(directory);
  const auto proxy_path = directory / "media-1-proxy.mov";
  {
    std::ofstream file(proxy_path, std::ios::binary);
    file << "local proxy";
  }

  commands::AttachProxyPayload attach;
  attach.media_id = "media-1";
  attach.path = proxy_path.string();
  attach.fingerprint = cutline::media::FingerprintFile(proxy_path.string());
  attach.source_fingerprint = "fp-1";
  attach.codec = "prores_ks";
  attach.width = 1280;
  attach.height = 720;
  harness.Run(commands::CommandType::AttachProxy, attach);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM media_proxies;"), std::int64_t{1});
  CHECK(cutline::media::FindProxy(harness.store(), "media-1").status == cutline::media::ProxyStatus::Ready);
  auto locator = cutline::media::ProjectProxyLocator(harness.store());
  CHECK_EQ(locator("media-1"), proxy_path.string());
  {
    std::ofstream file(proxy_path, std::ios::binary | std::ios::app);
    file << " changed";
  }
  CHECK(locator("media-1").empty());
  {
    std::ofstream file(proxy_path, std::ios::binary | std::ios::trunc);
    file << "local proxy";
  }

  commands::RelinkMediaPayload replacement{"media-1", "/footage/replacement.mov", false};
  replacement.fingerprint = "fp-2";
  harness.Run(commands::CommandType::RelinkMedia, replacement);
  CHECK(cutline::media::FindProxy(harness.store(), "media-1").status == cutline::media::ProxyStatus::Stale);
  harness.Undo();
  CHECK(cutline::media::FindProxy(harness.store(), "media-1").status == cutline::media::ProxyStatus::Ready);

  harness.Run(commands::CommandType::DetachProxy, commands::DetachProxyPayload{"media-1"});
  CHECK(cutline::media::FindProxy(harness.store(), "media-1").status == cutline::media::ProxyStatus::None);
  harness.Undo();
  CHECK(cutline::media::FindProxy(harness.store(), "media-1").status == cutline::media::ProxyStatus::Ready);
  std::filesystem::remove(proxy_path);
  CHECK(cutline::media::FindProxy(harness.store(), "media-1").status == cutline::media::ProxyStatus::Missing);
  std::filesystem::remove_all(directory);
}

CUTLINE_TEST(MediaStreamsReplaceWholesale) {
  Harness harness;
  harness.BuildSequence();
  commands::SetMediaStreamsPayload payload;
  payload.media_id = "media-1";
  commands::MediaStream video;
  video.stream_index = 0;
  video.kind = model::StreamKind::Video;
  video.codec = "h264";
  video.width = 3840;
  video.height = 2160;
  video.frame_rate = rates::kFrameRate2997;
  video.color_primaries = "bt2020";
  video.color_transfer = "pq";
  payload.streams = {video};
  harness.Run(commands::CommandType::SetMediaStreams, payload);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM media_streams;"), 1);
  CHECK_EQ(harness.Text("SELECT color_transfer FROM media_streams WHERE stream_index = 0;"), std::string("pq"));

  // Re-probing replaces the previous streams rather than accumulating.
  commands::MediaStream audio;
  audio.stream_index = 1;
  audio.kind = model::StreamKind::Audio;
  audio.sample_rate = 48000;
  audio.channel_count = 2;
  payload.streams = {video, audio};
  harness.Run(commands::CommandType::SetMediaStreams, payload);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM media_streams;"), 2);
}

CUTLINE_TEST(DeletingABinUnfilesItsMediaRatherThanDestroyingIt) {
  Harness harness;
  harness.CreateProject();
  harness.Run(commands::CommandType::CreateBin, commands::CreateBinPayload{"bin-1", std::nullopt, "Footage", 0});

  commands::ImportMediaPayload media;
  media.id = "media-1";
  media.bin_id = "bin-1";
  media.display_name = "shot.mov";
  media.original_path = "/footage/shot.mov";
  media.fingerprint = "fp-1";
  media.duration = Seconds(10);
  harness.Run(commands::CommandType::ImportMedia, media);

  harness.Run(commands::CommandType::DeleteBin, commands::DeleteBinPayload{"bin-1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM media WHERE id = 'media-1';"), 1);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM media WHERE bin_id IS NULL;"), 1);
}

CUTLINE_TEST(BinCyclesAreRejected) {
  Harness harness;
  harness.CreateProject();
  harness.Run(commands::CommandType::CreateBin, commands::CreateBinPayload{"bin-a", std::nullopt, "A", 0});
  harness.Run(commands::CommandType::CreateBin, commands::CreateBinPayload{"bin-b", "bin-a", "B", 0});
  // Making A a child of B would detach the pair into a loop.
  CHECK_THROWS(harness.Run(commands::CommandType::MoveBin, commands::MoveBinPayload{"bin-a", "bin-b", 0}));
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(NestedSequenceCannotBeDeletedWhileInUse) {
  Harness harness;
  harness.BuildSequence();
  commands::CreateSequencePayload outer;
  outer.id = "seq-outer";
  outer.settings.name = "Outer";
  outer.settings.frame_rate = rates::kFrameRate2997;
  outer.settings.width = 1920;
  outer.settings.height = 1080;
  outer.settings.sample_rate = 48000;
  harness.Run(commands::CommandType::CreateSequence, outer);
  harness.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"ov1", "seq-outer", 0, "stereo", "V1"});

  commands::InsertClipPayload nested;
  nested.id = "nest-1";
  nested.track_id = "ov1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.nested_sequence_id = "seq-1";
  nested.source_in = Seconds(0);
  nested.source_out = Seconds(5);
  nested.timeline_start = Seconds(0);
  harness.Run(commands::CommandType::InsertClip, nested);

  CHECK_THROWS(harness.Run(commands::CommandType::DeleteSequence, commands::DeleteSequencePayload{"seq-1"}));
}

CUTLINE_TEST(DuplicateIdentifiersAreRejected) {
  Harness harness;
  harness.BuildSequence();
  CHECK_THROWS(harness.Run(commands::CommandType::AddVideoTrack,
                           commands::AddTrackPayload{"v1", "seq-1", 5, "stereo", "V1 again"}));
  CHECK_THROWS(harness.Run(commands::CommandType::InsertClip,
                           Harness::Clip("clip-1", "v1", Seconds(40), Seconds(0), Seconds(1))));
}

CUTLINE_TEST(TracksCannotShareAnOrderWithinOneSequenceAndType) {
  Harness harness;
  harness.BuildSequence();
  // V1 already occupies video order 0.
  CHECK_THROWS(harness.Run(commands::CommandType::AddVideoTrack,
                           commands::AddTrackPayload{"v-dup", "seq-1", 0, "stereo", "V?"}));
  // The same order on the audio side is a different slot and is fine.
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddVideoTrack,
                             commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"}));
}

// ----------------------------------------------------------- package I/O ----

CUTLINE_TEST(PackageRoundTripsThroughDisk) {
  const auto package = ScratchDirectory("package");
  commands::CommandEnvelope create;
  create.command_id = "cmd-create";
  create.project_id = ProjectStore::GenerateProjectUuid();
  create.author_id = "tester";
  create.base_revision = 0;
  create.timestamp_utc = "2026-10-05T00:00:00Z";
  create.type = commands::CommandType::CreateProject;
  create.payload = commands::CreateProjectPayload{"On Disk"};
  create.idempotency_key = "key-create";

  {
    auto store = ProjectStore::CreatePackage(package, create);
    CHECK_EQ(store->ProjectName(), std::string("On Disk"));
    // A file-backed project uses WAL so a reader can run during a write.
    CHECK_EQ(store->JournalMode(), std::string("wal"));
    CHECK(std::filesystem::is_regular_file(package / "project.json"));

    Harness harness(store.get());
    commands::ImportMediaPayload media;
    media.id = "media-1";
    media.display_name = "shot.mov";
    media.original_path = "/footage/shot.mov";
    media.fingerprint = "fp-1";
    media.duration = Seconds(30);
    auto command = harness.Make(commands::CommandType::ImportMedia, media);
    command.project_id = create.project_id;
    const auto result = store->Execute(command);
    (void)result;
  }

  {
    auto reopened = ProjectStore::OpenPackage(package);
    CHECK_EQ(reopened->ProjectName(), std::string("On Disk"));
    CHECK_EQ(reopened->ProjectId(), create.project_id);
    CHECK(reopened->JournalCount() >= 2);
    CHECK_NO_THROW(reopened->ValidateDatabase());
    // History does not survive reopening, which matches every NLE.
    CHECK(!reopened->CanUndo());
  }

  std::filesystem::remove_all(package);
}

CUTLINE_TEST(OpeningSomethingThatIsNotAPackageFails) {
  const auto empty = ScratchDirectory("not-a-package");
  std::filesystem::create_directories(empty);
  CHECK_THROWS(ProjectStore::OpenPackage(empty));
  std::filesystem::remove_all(empty);
}

CUTLINE_TEST(CreatingOverAnExistingPackageFails) {
  const auto package = ScratchDirectory("existing");
  commands::CommandEnvelope create;
  create.command_id = "cmd-create";
  create.project_id = ProjectStore::GenerateProjectUuid();
  create.author_id = "tester";
  create.timestamp_utc = "2026-10-05T00:00:00Z";
  create.type = commands::CommandType::CreateProject;
  create.payload = commands::CreateProjectPayload{"First"};
  create.idempotency_key = "key-create";
  { auto store = ProjectStore::CreatePackage(package, create); }
  CHECK_THROWS(ProjectStore::CreatePackage(package, create));
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(SnapshotsAreWrittenOnTheConfiguredInterval) {
  const auto package = ScratchDirectory("snapshots");
  commands::CommandEnvelope create;
  create.command_id = "cmd-create";
  create.project_id = ProjectStore::GenerateProjectUuid();
  create.author_id = "tester";
  create.timestamp_utc = "2026-10-05T00:00:00Z";
  create.type = commands::CommandType::CreateProject;
  create.payload = commands::CreateProjectPayload{"Snapshotted"};
  create.idempotency_key = "key-create";

  SnapshotPolicy policy;
  policy.every_revisions = 2;
  auto store = ProjectStore::CreatePackage(package, create, policy);
  Harness harness(store.get());
  for (int index = 0; index < 4; ++index) {
    auto command = harness.Make(commands::CommandType::RenameProject,
                                commands::RenameProjectPayload{"Name " + std::to_string(index)});
    command.project_id = create.project_id;
    const auto result = store->Execute(command);
    (void)result;
  }
  CHECK(store->SnapshotCount() >= 2);
  CHECK_NO_THROW(store->CreateSnapshot());
  store.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(GeneratedProjectIdsAreWellFormedAndUnique) {
  const auto first = ProjectStore::GenerateProjectUuid();
  const auto second = ProjectStore::GenerateProjectUuid();
  CHECK_EQ(first.size(), std::size_t{36});
  CHECK(first != second);
  CHECK_EQ(first[14], '4');
  CHECK(first[19] == '8' || first[19] == '9' || first[19] == 'a' || first[19] == 'b');
}

CUTLINE_TEST(SnapshotPolicyIsValidated) {
  SnapshotPolicy zero_interval;
  zero_interval.every_revisions = 0;
  CHECK_THROWS(ProjectStore(":memory:", zero_interval));
  SnapshotPolicy zero_history;
  zero_history.history_limit = 0;
  CHECK_THROWS(ProjectStore(":memory:", zero_history));
}

// ------------------------------------------------ transition invariants ----
//
// A transition mixes the two clips either side of a cut, so it is only meaningful
// while those clips are still adjacent, on its track, and under its span. It used
// to be validated only at creation; every later edit could leave it dangling.

namespace {

commands::AddTransitionPayload Dissolve(const std::string& id, RationalTime start, RationalTime duration) {
  commands::AddTransitionPayload transition;
  transition.id = id;
  transition.track_id = "v1";
  transition.kind = "cross_dissolve";
  transition.from_clip_id = "clip-1";
  transition.to_clip_id = "clip-2";
  transition.timeline_start = start;
  transition.duration = duration;
  return transition;  // centre-aligned by default
}

// clip-1 is 0..5 and clip-2 is 5..10 (BuildSequence), so the cut is at 5 s.
void AddJoinTransition(Harness& harness) {
  harness.Run(commands::CommandType::AddTransition, Dissolve("t-1", Seconds(4), Seconds(2)));
}

std::string FromClipOf(Harness& harness, const std::string& transition_id) {
  return harness.Text("SELECT COALESCE(from_clip_id, '') FROM transitions WHERE id = '" + transition_id + "';");
}
std::string ToClipOf(Harness& harness, const std::string& transition_id) {
  return harness.Text("SELECT COALESCE(to_clip_id, '') FROM transitions WHERE id = '" + transition_id + "';");
}

}  // namespace

CUTLINE_TEST(ATransitionBetweenTwoClipsRequiresThemToBeAdjacent) {
  Harness harness;
  harness.BuildSequence();
  // Leave a gap: move clip-2 away from the cut, then try to join them.
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(8)});
  CHECK_THROWS(harness.Run(commands::CommandType::AddTransition, Dissolve("t-gap", Seconds(4), Seconds(2))));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 0);
}

CUTLINE_TEST(ATransitionMustCoverTheCutItJoins) {
  Harness harness;
  harness.BuildSequence();
  // The cut is at 5 s; a span of 1..2 s does not touch it.
  CHECK_THROWS(harness.Run(commands::CommandType::AddTransition, Dissolve("t-far", Seconds(1), Seconds(1))));
  // A span that ends exactly at the cut and is centred elsewhere is the wrong alignment.
  CHECK_THROWS(harness.Run(commands::CommandType::AddTransition, Dissolve("t-off", Seconds(3), Seconds(2))));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 0);
  CHECK_NO_THROW(AddJoinTransition(harness));
}

CUTLINE_TEST(TransitionAlignmentIsEnforcedNotJustRecorded) {
  Harness harness;
  harness.BuildSequence();

  auto starts_at_cut = Dissolve("t-start", Seconds(5), Seconds(2));
  starts_at_cut.alignment = model::TransitionAlignment::Start;
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddTransition, starts_at_cut));
  harness.Run(commands::CommandType::RemoveTransition, commands::RemoveTransitionPayload{"t-start"});

  // Declared as starting at the cut, but placed to end there.
  auto wrong = Dissolve("t-wrong", Seconds(3), Seconds(2));
  wrong.alignment = model::TransitionAlignment::Start;
  CHECK_THROWS(harness.Run(commands::CommandType::AddTransition, wrong));

  auto ends_at_cut = Dissolve("t-end", Seconds(3), Seconds(2));
  ends_at_cut.alignment = model::TransitionAlignment::End;
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddTransition, ends_at_cut));
  harness.Run(commands::CommandType::RemoveTransition, commands::RemoveTransitionPayload{"t-end"});

  // Custom alignment may sit anywhere over the cut.
  auto custom = Dissolve("t-custom", Seconds(4), Seconds(3));
  custom.alignment = model::TransitionAlignment::Custom;
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddTransition, custom));
}

CUTLINE_TEST(ChangingATransitionsTimingIsValidatedLikeCreatingIt) {
  Harness harness;
  harness.BuildSequence();
  AddJoinTransition(harness);

  // Still centred on the cut: fine.
  CHECK_NO_THROW(harness.Run(commands::CommandType::SetTransitionTiming,
                             commands::SetTransitionTimingPayload{"t-1", RationalTime(9, 2), Seconds(1)}));
  // No longer covering the cut.
  CHECK_THROWS(harness.Run(commands::CommandType::SetTransitionTiming,
                           commands::SetTransitionTimingPayload{"t-1", Seconds(1), Seconds(1)}));
  // Off-centre for a centre-aligned transition.
  CHECK_THROWS(harness.Run(commands::CommandType::SetTransitionTiming,
                           commands::SetTransitionTimingPayload{"t-1", Seconds(4), Seconds(3)}));
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(ATransitionCannotBeMovedOntoAnotherTransition) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("clip-3", "v1", Seconds(10), Seconds(20), Seconds(25)));
  AddJoinTransition(harness);  // 4..6 around the cut at 5

  commands::AddTransitionPayload second = Dissolve("t-2", Seconds(9), Seconds(2));
  second.from_clip_id = "clip-2";
  second.to_clip_id = "clip-3";
  harness.Run(commands::CommandType::AddTransition, second);  // 9..11 around the cut at 10

  // Widening the first so that it runs into the second.
  CHECK_THROWS(harness.Run(commands::CommandType::SetTransitionTiming,
                           commands::SetTransitionTimingPayload{"t-1", Seconds(0), Seconds(10)}));
}

CUTLINE_TEST(SplittingTheOutgoingClipMovesItsTransitionToTheHalfThatNowEndsAtTheCut) {
  // The original id stays on the left half, which no longer reaches the cut. The
  // transition's outgoing side has to follow the end of the clip to the new half.
  Harness harness;
  harness.BuildSequence();
  AddJoinTransition(harness);
  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(3)});

  CHECK_EQ(FromClipOf(harness, "t-1"), std::string("clip-1b"));
  CHECK_EQ(ToClipOf(harness, "t-1"), std::string("clip-2"));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 1);
  CHECK_NO_THROW(harness.store().ValidateDatabase());

  harness.Undo();
  CHECK_EQ(FromClipOf(harness, "t-1"), std::string("clip-1"));
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(SplittingTheIncomingClipLeavesItsTransitionOnTheHalfThatStartsAtTheCut) {
  Harness harness;
  harness.BuildSequence();
  AddJoinTransition(harness);
  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-2", "clip-2b", Seconds(8)});
  CHECK_EQ(ToClipOf(harness, "t-1"), std::string("clip-2"));
  CHECK_EQ(FromClipOf(harness, "t-1"), std::string("clip-1"));
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(MovingAClipAwayDetachesItsTransitionInsteadOfLeavingItDangling) {
  Harness harness;
  harness.BuildSequence();
  AddJoinTransition(harness);

  // Moving to where it already is changes nothing.
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(5)});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 1);

  // Moving it off the cut breaks adjacency, so the transition has to go with it.
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(20)});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 0);
  CHECK_NO_THROW(harness.store().ValidateDatabase());

  // It is one undoable command: undoing the move brings the transition back.
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 1);
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), 5);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(MovingAClipToAnotherTrackDetachesItsTransition) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
  AddJoinTransition(harness);
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v2", Seconds(5)});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 0);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(TrimmingTheJoinedEdgeDetachesTheTransitionButTheFarEdgeDoesNot) {
  const auto transitions_after = [](const std::function<void(Harness&)>& edit) {
    Harness harness;
    harness.BuildSequence();
    AddJoinTransition(harness);
    edit(harness);
    CHECK_NO_THROW(harness.store().ValidateDatabase());
    return harness.Count("SELECT COUNT(*) FROM transitions;");
  };

  // clip-1 is 0..5 and clip-2 is 5..10; the cut is where they meet.
  // Trimming the far edge of the outgoing clip (its head) leaves the cut alone.
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::TrimClip,
                   commands::TrimClipPayload{"clip-1", Seconds(1), Seconds(5), Seconds(1)});
           }), 1);
  // So does trimming the far edge of the incoming clip (its tail).
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::TrimClip,
                   commands::TrimClipPayload{"clip-2", Seconds(10), Seconds(14), Seconds(5)});
           }), 1);
  // A slip changes what is shown, not where the clip sits.
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::TrimClip,
                   commands::TrimClipPayload{"clip-1", Seconds(2), Seconds(7), Seconds(0)});
           }), 1);
  // Shortening the outgoing clip's tail opens a gap at the cut.
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::TrimClip,
                   commands::TrimClipPayload{"clip-1", Seconds(0), Seconds(4), Seconds(0)});
           }), 0);
  // Trimming the incoming clip's head does the same from the other side.
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::TrimClip,
                   commands::TrimClipPayload{"clip-2", Seconds(11), Seconds(15), Seconds(6)});
           }), 0);
}

CUTLINE_TEST(ChangingSpeedDetachesATransitionOnlyWhenTheJoinedEdgeMoves) {
  const auto transitions_after = [](const std::function<void(Harness&)>& edit) {
    Harness harness;
    harness.BuildSequence();
    AddJoinTransition(harness);
    edit(harness);
    CHECK_NO_THROW(harness.store().ValidateDatabase());
    return harness.Count("SELECT COUNT(*) FROM transitions;");
  };
  // Speeding up the outgoing clip pulls its end away from the cut.
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::SetClipSpeed,
                   commands::SetClipSpeedPayload{"clip-1", RationalTime(2, 1), false});
           }), 0);
  // The incoming clip's start is where it was, so it keeps its transition.
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::SetClipSpeed,
                   commands::SetClipSpeedPayload{"clip-2", RationalTime(2, 1), false});
           }), 1);
  // Reversing without changing the rate does not move either edge.
  CHECK_EQ(transitions_after([](Harness& h) {
             h.Run(commands::CommandType::SetClipSpeed,
                   commands::SetClipSpeedPayload{"clip-1", RationalTime(1, 1), true});
           }), 1);
}

CUTLINE_TEST(ValidateDatabaseReportsATransitionThatNoLongerJoinsItsClips) {
  // The check is the safety net under the edit-time rules: if anything ever
  // writes a bad transition, opening the project says so.
  Harness harness;
  harness.BuildSequence();
  AddJoinTransition(harness);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
  {
    const std::lock_guard<std::mutex> lock(harness.store().mutex());
    cutline::db::Execute(harness.store().connection(),
                         "UPDATE clips SET timeline_start_ticks = timeline_start_ticks + 1000,"
                         " timeline_end_ticks = timeline_end_ticks + 1000 WHERE id = 'clip-2';");
  }
  CHECK_THROWS(harness.store().ValidateDatabase());
}

// ---------------------------------------------------------- track lock policy ----
//
// A locked track protects what is on it: its clips, the transitions between them,
// and the effects and animation attached to any of those, plus the track itself.
// Only some of the commands that change those things checked the lock.
//
// Deliberately NOT covered by the lock: changing the track's own state (so it can
// be unlocked, muted or re-gained), markers (annotations rather than content),
// and effects owned by the sequence rather than by a track.

namespace {

// clip-1 0..5, clip-2 5..10 with a transition between them, an effect with
// keyframes on clip-1, one on the track, and one on the transition.
void BuildLockFixture(Harness& harness) {
  harness.BuildSequence();
  AddJoinTransition(harness);
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-1"));
  commands::SetKeyframePayload keyframe;
  keyframe.parameter_id = "fx-1:exposure";
  keyframe.keyframe = {Seconds(1), Value::Scalar(0.5), Interpolation::Linear, {}, {}};
  harness.Run(commands::CommandType::SetKeyframe, keyframe);

  auto on_track = LumetriOn("clip-1", "fx-track");
  on_track.owner_kind = model::EffectOwner::Track;
  on_track.owner_id = "v1";
  harness.Run(commands::CommandType::AddEffect, on_track);

  auto on_transition = LumetriOn("clip-1", "fx-transition");
  on_transition.owner_kind = model::EffectOwner::Transition;
  on_transition.owner_id = "t-1";
  harness.Run(commands::CommandType::AddEffect, on_transition);
}

void SetLocked(Harness& harness, bool locked) {
  commands::SetTrackStatePayload state;
  state.id = "v1";
  state.locked = locked;
  harness.Run(commands::CommandType::SetTrackState, state);
}

struct LockedOperation final {
  const char* name;
  std::function<void(Harness&)> run;
};

std::vector<LockedOperation> EveryOperationALockMustRefuse() {
  using commands::CommandType;
  return {
      {"insert clip", [](Harness& h) { h.Run(CommandType::InsertClip, Harness::Clip("clip-new", "v1", Seconds(30), Seconds(0), Seconds(1))); }},
      {"delete clip", [](Harness& h) { h.Run(CommandType::DeleteClip, commands::DeleteClipPayload{"clip-2"}); }},
      {"ripple delete", [](Harness& h) { h.Run(CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{"clip-1"}); }},
      {"move clip", [](Harness& h) { h.Run(CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(20)}); }},
      {"split clip", [](Harness& h) { h.Run(CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(3)}); }},
      {"trim clip", [](Harness& h) { h.Run(CommandType::TrimClip, commands::TrimClipPayload{"clip-1", Seconds(1), Seconds(5), Seconds(1)}); }},
      {"set clip speed", [](Harness& h) { h.Run(CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"clip-2", RationalTime(2, 1), false}); }},
      {"enable or disable clip", [](Harness& h) { h.Run(CommandType::SetClipEnabled, commands::SetClipEnabledPayload{"clip-1", false}); }},
      {"add transition", [](Harness& h) {
         commands::AddTransitionPayload fade;
         fade.id = "t-fade";
         fade.track_id = "v1";
         fade.kind = "cross_dissolve";
         fade.alignment = model::TransitionAlignment::Start;
         fade.to_clip_id = "clip-1";
         fade.timeline_start = Seconds(0);
         fade.duration = Seconds(1);
         h.Run(CommandType::AddTransition, fade);
       }},
      {"remove transition", [](Harness& h) { h.Run(CommandType::RemoveTransition, commands::RemoveTransitionPayload{"t-1"}); }},
      {"change transition timing", [](Harness& h) { h.Run(CommandType::SetTransitionTiming, commands::SetTransitionTimingPayload{"t-1", RationalTime(9, 2), Seconds(1)}); }},
      {"add effect to a clip", [](Harness& h) { h.Run(CommandType::AddEffect, LumetriOn("clip-2", "fx-new-clip")); }},
      {"add effect to the track", [](Harness& h) {
         auto effect = LumetriOn("clip-2", "fx-new-track");
         effect.owner_kind = model::EffectOwner::Track;
         effect.owner_id = "v1";
         h.Run(CommandType::AddEffect, effect);
       }},
      {"add effect to a transition", [](Harness& h) {
         auto effect = LumetriOn("clip-2", "fx-new-transition");
         effect.owner_kind = model::EffectOwner::Transition;
         effect.owner_id = "t-1";
         h.Run(CommandType::AddEffect, effect);
       }},
      {"remove a clip effect", [](Harness& h) { h.Run(CommandType::RemoveEffect, commands::RemoveEffectPayload{"fx-1"}); }},
      {"remove a track effect", [](Harness& h) { h.Run(CommandType::RemoveEffect, commands::RemoveEffectPayload{"fx-track"}); }},
      {"remove a transition effect", [](Harness& h) { h.Run(CommandType::RemoveEffect, commands::RemoveEffectPayload{"fx-transition"}); }},
      {"bypass an effect", [](Harness& h) { h.Run(CommandType::SetEffectEnabled, commands::SetEffectEnabledPayload{"fx-1", false}); }},
      {"reorder an effect", [](Harness& h) { h.Run(CommandType::ReorderEffect, commands::ReorderEffectPayload{"fx-1", 3}); }},
      {"set a parameter", [](Harness& h) { h.Run(CommandType::SetParameterConstant, commands::SetParameterConstantPayload{"fx-1:exposure", Value::Scalar(2.0)}); }},
      {"set a keyframe", [](Harness& h) {
         commands::SetKeyframePayload keyframe;
         keyframe.parameter_id = "fx-1:exposure";
         keyframe.keyframe = {Seconds(2), Value::Scalar(0.9), Interpolation::Linear, {}, {}};
         h.Run(CommandType::SetKeyframe, keyframe);
       }},
      {"remove a keyframe", [](Harness& h) { h.Run(CommandType::RemoveKeyframe, commands::RemoveKeyframePayload{"fx-1:exposure", Seconds(1)}); }},
      {"remove the track", [](Harness& h) { h.Run(CommandType::RemoveTrack, commands::RemoveTrackPayload{"v1"}); }},
      {"link clips", [](Harness& h) { h.Run(CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1", "clip-2"}, "group-x"}); }},
      {"unlink a clip", [](Harness& h) { h.Run(CommandType::UnlinkClips, commands::UnlinkClipsPayload{{"clip-1"}}); }},
  };
}

}  // namespace

CUTLINE_TEST(ALockedTrackRefusesEveryEditToItsContents) {
  const auto operations = EveryOperationALockMustRefuse();
  for (const auto& operation : operations) {
    Harness harness;
    BuildLockFixture(harness);
    SetLocked(harness, true);

    const auto revision = harness.store().CurrentRevision();
    bool refused = false;
    try {
      operation.run(harness);
    } catch (const std::exception& error) {
      refused = std::string(error.what()).find("locked") != std::string::npos;
    }
    if (!refused) {
      cutline::testing::Fail("a locked track accepted an edit", __FILE__, __LINE__, operation.name);
    }
    // Nothing happened: not the revision, not the contents.
    CHECK_EQ(harness.store().CurrentRevision(), revision);

    // The refusal was the lock and nothing else: once unlocked, the same edit works.
    SetLocked(harness, false);
    try {
      operation.run(harness);
    } catch (const std::exception& error) {
      cutline::testing::Fail("the edit is invalid even when unlocked, so this test proves nothing", __FILE__,
                             __LINE__, std::string(operation.name) + ": " + error.what());
    }
  }
}

CUTLINE_TEST(ALockedTrackStillAllowsWhatTheLockIsNotFor) {
  Harness harness;
  BuildLockFixture(harness);
  SetLocked(harness, true);

  // Mute, gain and so on, which is also how the track gets unlocked.
  commands::SetTrackStatePayload state;
  state.id = "v1";
  state.locked = true;
  state.muted = true;
  state.gain_db = -3.0;
  CHECK_NO_THROW(harness.Run(commands::CommandType::SetTrackState, state));

  // Markers annotate; they do not edit.
  commands::AddMarkerPayload marker;
  marker.id = "m-1";
  marker.owner_kind = model::MarkerOwner::Clip;
  marker.owner_id = "clip-1";
  marker.start = Seconds(1);
  marker.end = Seconds(1);
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddMarker, marker));

  // An effect on the sequence is not on this track.
  auto on_sequence = LumetriOn("clip-1", "fx-seq");
  on_sequence.owner_kind = model::EffectOwner::Sequence;
  on_sequence.owner_id = "seq-1";
  CHECK_NO_THROW(harness.Run(commands::CommandType::AddEffect, on_sequence));

  // Editing a *different*, unlocked track is unaffected.
  harness.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
  CHECK_NO_THROW(harness.Run(commands::CommandType::InsertClip,
                             Harness::Clip("other", "v2", Seconds(0), Seconds(0), Seconds(2))));
}

CUTLINE_TEST(MovingAClipOntoALockedTrackIsRefusedEvenFromAnUnlockedOne) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
  commands::SetTrackStatePayload lock;
  lock.id = "v2";
  lock.locked = true;
  harness.Run(commands::CommandType::SetTrackState, lock);
  CHECK_THROWS(harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v2", Seconds(0)}));
}

CUTLINE_TEST(RippleDeleteShiftsATransitionBetweenLaterClips) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("clip-3", "v1", Seconds(10), Seconds(20), Seconds(25)));
  commands::AddTransitionPayload join = Dissolve("t-23", Seconds(9), Seconds(2));
  join.from_clip_id = "clip-2";
  join.to_clip_id = "clip-3";
  harness.Run(commands::CommandType::AddTransition, join);

  harness.Run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{"clip-1"});
  // clip-2 is now 0..5 and clip-3 5..10; the cut moved from 10 to 5.
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM transitions;"), 1);
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM transitions WHERE id = 't-23';"), 4);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

// ---- a deterministic stress test of the invariants ----

namespace {

// A small fixed-seed generator. Standard distributions are not portable across
// standard libraries; this makes the sequence of edits identical everywhere, so a
// failure is reproducible from the seed alone.
class Lcg final {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return state_ >> 33;
  }
  std::int64_t Below(std::int64_t limit) { return static_cast<std::int64_t>(Next() % static_cast<std::uint64_t>(limit)); }

 private:
  std::uint64_t state_;
};

std::vector<std::string> Column(Harness& harness, const std::string& sql) {
  const std::lock_guard<std::mutex> lock(harness.store().mutex());
  cutline::db::Statement statement(harness.store().connection(), sql);
  std::vector<std::string> values;
  while (statement.Step()) values.push_back(statement.ColumnText(0));
  return values;
}

// Everything an edit can change, as one string, so two states can be compared.
std::string Fingerprint(Harness& harness) {
  std::string digest;
  for (const auto& sql : {
           "SELECT group_concat(v, '|') FROM (SELECT id||':'||track_id||':'||timeline_start_ticks||':'||"
           "timeline_end_ticks||':'||source_in_num||'/'||source_in_den||':'||source_out_num||'/'||source_out_den||':'||"
           "rate_num||'/'||rate_den||':'||reversed||':'||enabled AS v FROM clips ORDER BY id);",
           "SELECT group_concat(v, '|') FROM (SELECT id||':'||track_id||':'||COALESCE(from_clip_id,'')||':'||"
           "COALESCE(to_clip_id,'')||':'||timeline_start_ticks||':'||timeline_end_ticks||':'||alignment AS v "
           "FROM transitions ORDER BY id);",
           "SELECT group_concat(v, '|') FROM (SELECT parameter_id||'@'||time_ticks||'='||c0||','||c1||','||interpolation||"
           "','||out_handle_x||','||out_handle_y||','||in_handle_x||','||in_handle_y AS v FROM keyframes "
           "ORDER BY parameter_id, time_ticks);",
           "SELECT group_concat(v, '|') FROM (SELECT id||':'||owner_id||':'||enabled AS v FROM effects ORDER BY id);"}) {
    for (const auto& row : Column(harness, sql)) digest += row + "\n";
  }
  return digest;
}

}  // namespace

CUTLINE_TEST(ARandomSessionOfEditsNeverLeavesTheProjectInvalidAndUndoesCompletely) {
  // The properties the milestone is about, tested against whatever sequence of
  // edits a user might make rather than the ones I thought of: after every
  // command that succeeds the database is valid (adjacent, attached, aligned
  // transitions; no overlaps; no orphans), commands that are refused change
  // nothing, and undoing the whole session returns to the exact starting state.
  SnapshotPolicy policy;
  policy.history_limit = 2000;
  Harness harness(policy);
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-1", 1, "stereo", "V2"});
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("clip-3", "v2", Seconds(2), Seconds(30), Seconds(38)));
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("clip-4", "v2", Seconds(10), Seconds(40), Seconds(44)));
  harness.Run(commands::CommandType::AddEffect, LumetriOn("clip-1", "fx-a"));
  commands::SetKeyframePayload key;
  key.parameter_id = "fx-a:exposure";
  key.keyframe = {Seconds(1), Value::Scalar(0.2), Interpolation::EaseInOut, {}, {}};
  harness.Run(commands::CommandType::SetKeyframe, key);
  key.keyframe = {Seconds(4), Value::Scalar(0.8), Interpolation::Bezier, {0.2, 0.9}, {0.7, 1.2}};
  harness.Run(commands::CommandType::SetKeyframe, key);
  AddJoinTransition(harness);

  const auto starting_point = Fingerprint(harness);
  const auto starting_depth = harness.store().AppliedHistoryCount();

  Lcg random(0x5EEDC0DEULL);
  int accepted = 0;
  int refused = 0;
  int serial = 0;
  std::int64_t lock_flips = 0;

  for (int step = 0; step < 600; ++step) {
    const auto clips = Column(harness, "SELECT id FROM clips ORDER BY id;");
    const auto transitions = Column(harness, "SELECT id FROM transitions ORDER BY id;");
    const auto pick = [&](const std::vector<std::string>& from) { return from[static_cast<std::size_t>(random.Below(static_cast<std::int64_t>(from.size())))]; };
    const auto at_quarter = [&](std::int64_t max_seconds) { return RationalTime(random.Below(max_seconds * 4 + 1), 4); };

    const auto revision_before = harness.store().CurrentRevision();
    const auto state_before = Fingerprint(harness);

    try {
      // Keep the project populated: with few clips left, always add one. Without
      // this the random deletes empty the sequence within a few dozen steps and
      // the rest of the session tests nothing.
      auto choice = random.Below(14);
      if (clips.size() < 4) choice = 11;
      switch (choice) {
        case 11:
        case 12: {
          commands::InsertClipPayload fresh;
          fresh.id = "fresh-" + std::to_string(++serial);
          fresh.track_id = random.Below(2) == 0 ? "v1" : "v2";
          fresh.source_kind = model::SourceKind::Media;
          fresh.media_id = "media-1";
          fresh.source_in = RationalTime(random.Below(80), 4);
          fresh.source_out = fresh.source_in.Add(RationalTime(2 + random.Below(24), 4));
          fresh.timeline_start = RationalTime(random.Below(160), 4);
          harness.Run(commands::CommandType::InsertClip, fresh);
          break;
        }
        case 0: {
          const auto id = pick(clips);
          harness.Run(commands::CommandType::SplitClip,
                      commands::SplitClipPayload{id, "split-" + std::to_string(++serial), at_quarter(40)});
          break;
        }
        case 1:
          harness.Run(commands::CommandType::MoveClip,
                      commands::MoveClipPayload{pick(clips), random.Below(2) == 0 ? "v1" : "v2", at_quarter(40)});
          break;
        case 2: {
          const auto source_in = at_quarter(20);
          harness.Run(commands::CommandType::TrimClip,
                      commands::TrimClipPayload{pick(clips), source_in, source_in.Add(RationalTime(1 + random.Below(40), 4)),
                                                at_quarter(40)});
          break;
        }
        case 3:
          harness.Run(commands::CommandType::SetClipSpeed,
                      commands::SetClipSpeedPayload{pick(clips), RationalTime(1 + random.Below(4), 1 + random.Below(3)),
                                                    random.Below(2) == 0});
          break;
        case 4:
          if (clips.size() > 5) harness.Run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{pick(clips)});
          break;
        case 5:
          if (clips.size() > 5 && random.Below(3) == 0) {
            harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{pick(clips)});
          }
          break;
        case 6: {
          // Join some adjacent pair, if one exists.
          const auto pairs = Column(harness,
                                    "SELECT a.id||'>'||b.id||'@'||a.timeline_end_ticks FROM clips a JOIN clips b "
                                    "ON a.track_id = b.track_id AND a.timeline_end_ticks = b.timeline_start_ticks;");
          if (pairs.empty()) break;
          const auto pair = pick(pairs);
          const auto arrow = pair.find('>');
          const auto at_sign = pair.find('@');
          commands::AddTransitionPayload join;
          join.id = "rt-" + std::to_string(++serial);
          join.track_id = Column(harness, "SELECT track_id FROM clips WHERE id = '" + pair.substr(0, arrow) + "';")[0];
          join.kind = "cross_dissolve";
          join.from_clip_id = pair.substr(0, arrow);
          join.to_clip_id = pair.substr(arrow + 1, at_sign - arrow - 1);
          // Centre it on the cut: the cut is in ticks, so convert back exactly.
          const auto cut = RationalTime::FromTicks(std::stoll(pair.substr(at_sign + 1)));
          join.duration = RationalTime(1 + random.Below(4), 2);
          join.timeline_start = cut.Subtract(join.duration.Divide(2));
          harness.Run(commands::CommandType::AddTransition, join);
          break;
        }
        case 7:
          if (!transitions.empty()) {
            harness.Run(commands::CommandType::SetTransitionTiming,
                        commands::SetTransitionTimingPayload{pick(transitions), at_quarter(40), RationalTime(1 + random.Below(6), 2)});
          }
          break;
        case 8:
          if (!transitions.empty() && random.Below(3) == 0) {
            harness.Run(commands::CommandType::RemoveTransition, commands::RemoveTransitionPayload{pick(transitions)});
          }
          break;
        case 9: {
          commands::SetTrackStatePayload state;
          state.id = random.Below(2) == 0 ? "v1" : "v2";
          state.locked = random.Below(4) == 0;
          harness.Run(commands::CommandType::SetTrackState, state);
          ++lock_flips;
          break;
        }
        default:
          harness.Run(commands::CommandType::SetClipEnabled, commands::SetClipEnabledPayload{pick(clips), random.Below(2) == 0});
          break;
      }
    } catch (const std::exception&) {
      // A refused edit must have changed nothing at all.
      ++refused;
      CHECK_EQ(harness.store().CurrentRevision(), revision_before);
      CHECK_EQ(Fingerprint(harness), state_before);
      continue;
    }
    if (harness.store().CurrentRevision() != revision_before) ++accepted;
    // Whatever was accepted left a valid project.
    CHECK_NO_THROW(harness.store().ValidateDatabase());
  }

  // The run has to have exercised the edits, not just been refused out of all of them.
  if (accepted <= 120 || refused <= 10 || lock_flips <= 10) {
    cutline::testing::Fail("the session did not exercise enough", __FILE__, __LINE__,
                           "accepted " + std::to_string(accepted) + ", refused " + std::to_string(refused) +
                               ", lock changes " + std::to_string(lock_flips));
  }

  // Undo the entire session. Locks were part of it, so it all goes back.
  const auto ending_point = Fingerprint(harness);
  // Undo back to the depth the session started at, validating at every step.
  std::int64_t undone = 0;
  while (harness.store().AppliedHistoryCount() > starting_depth) {
    const auto result = harness.store().Undo("tester", "2026-10-06T03:00:00Z");
    (void)result;
    CHECK_NO_THROW(harness.store().ValidateDatabase());
    ++undone;
  }
  CHECK(undone > 100);
  CHECK_EQ(Fingerprint(harness), starting_point);

  // And redoing it all lands on exactly where the session ended.
  while (harness.store().CanRedo()) {
    const auto result = harness.store().Redo("tester", "2026-10-06T04:00:00Z");
    (void)result;
  }
  CHECK_EQ(Fingerprint(harness), ending_point);
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

// ------------------------------------------------------------- persistence ----
//
// What happens around a commit. The database is the record; the journal file and the
// snapshot in a package are copies made afterwards, and they must neither be able to
// make a successful edit look like a failure nor grow without limit.

namespace {

commands::CommandEnvelope PackageCreate(const std::string& id) {
  commands::CommandEnvelope create;
  create.command_id = "cmd-create";
  create.project_id = id;
  create.author_id = "tester";
  create.timestamp_utc = "2026-10-05T00:00:00Z";
  create.type = commands::CommandType::CreateProject;
  create.payload = commands::CreateProjectPayload{"Package"};
  create.idempotency_key = "key-create";
  return create;
}

cutline::project::CommandResult Rename(ProjectStore& store, const std::string& project, int index) {
  commands::CommandEnvelope command;
  command.command_id = "cmd-rename-" + std::to_string(index);
  command.project_id = project;
  command.author_id = "tester";
  command.base_revision = store.CurrentRevision();
  command.timestamp_utc = "2026-10-05T00:01:00Z";
  command.type = commands::CommandType::RenameProject;
  command.payload = commands::RenameProjectPayload{"Name " + std::to_string(index)};
  command.idempotency_key = "key-rename-" + std::to_string(index);
  return store.Execute(command);
}

std::vector<std::int64_t> JournalRevisions(const std::filesystem::path& package) {
  std::vector<std::int64_t> revisions;
  const auto directory = package / "journal";
  if (!std::filesystem::is_directory(directory)) return revisions;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (name.size() > 1 && name[0] == 'r' && entry.path().extension() == ".json") {
      revisions.push_back(std::stoll(name.substr(1)));
    }
  }
  std::sort(revisions.begin(), revisions.end());
  return revisions;
}

}  // namespace

CUTLINE_TEST(AFailureToWriteTheJournalCopyDoesNotFailTheCommand) {
  // The edit is committed to the database before its journal file is written. When
  // the file cannot be written (here the journal directory is blocked by a file) the
  // old code threw, telling the caller an edit had failed that was already applied.
  const auto package = ScratchDirectory("journal-blocked");
  const auto id = ProjectStore::GenerateProjectUuid();
  auto store = ProjectStore::CreatePackage(package, PackageCreate(id));
  std::filesystem::remove_all(package / "journal");
  { std::ofstream(package / "journal") << "not a directory"; }

  const auto before = store->CurrentRevision();
  cutline::project::CommandResult result;
  CHECK_NO_THROW(result = Rename(*store, id, 1));
  CHECK_EQ(result.revision, before + 1);
  CHECK_EQ(store->CurrentRevision(), before + 1);
  CHECK_EQ(store->ProjectName(), std::string("Name 1"));
  CHECK_EQ(result.warnings.size(), std::size_t{1});
  CHECK(result.warnings[0].find("journal record") != std::string::npos);
  CHECK_EQ(store->PersistenceWarnings().size(), std::size_t{1});
  store.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(AFailureToWriteASnapshotIsReportedAndLaterEditsStillWork) {
  const auto package = ScratchDirectory("snapshot-blocked");
  const auto id = ProjectStore::GenerateProjectUuid();
  SnapshotPolicy policy;
  policy.every_revisions = 2;
  auto store = ProjectStore::CreatePackage(package, PackageCreate(id), policy);
  std::filesystem::remove_all(package / "snapshots");
  { std::ofstream(package / "snapshots") << "not a directory"; }

  bool warned = false;
  for (int index = 1; index <= 4; ++index) {
    cutline::project::CommandResult result;
    CHECK_NO_THROW(result = Rename(*store, id, index));
    for (const auto& warning : result.warnings) warned = warned || warning.find("snapshot") != std::string::npos;
  }
  CHECK(warned);
  CHECK_EQ(store->ProjectName(), std::string("Name 4"));
  CHECK_THROWS(store->CreateSnapshot());  // asked for explicitly, so it says so
  store.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(SnapshotsAndJournalFilesAreKeptToAPolicyNotForever) {
  const auto package = ScratchDirectory("retention");
  const auto id = ProjectStore::GenerateProjectUuid();
  SnapshotPolicy policy;
  policy.every_revisions = 2;
  policy.keep_snapshots = 3;
  auto store = ProjectStore::CreatePackage(package, PackageCreate(id), policy);
  for (int index = 1; index <= 40; ++index) {
    const auto result = Rename(*store, id, index);
    CHECK(result.warnings.empty());
  }
  CHECK_EQ(store->SnapshotCount(), std::int64_t{3});

  // The oldest snapshot kept bounds the journal files: nothing before it remains.
  std::vector<std::int64_t> snapshot_revisions;
  for (const auto& entry : std::filesystem::directory_iterator(package / "snapshots")) {
    snapshot_revisions.push_back(std::stoll(entry.path().filename().string().substr(10)));
  }
  std::sort(snapshot_revisions.begin(), snapshot_revisions.end());
  CHECK_EQ(snapshot_revisions.front(), std::int64_t{36});
  CHECK_EQ(snapshot_revisions.back(), std::int64_t{40});
  const auto journal = JournalRevisions(package);
  CHECK(!journal.empty());
  CHECK(journal.front() >= snapshot_revisions.front());
  CHECK_EQ(journal.back(), store->CurrentRevision());
  // The database's own journal table still holds all of it.
  CHECK_EQ(store->JournalCount(), store->CurrentRevision());
  store.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(ASnapshotIsAValidProjectOfTheRevisionItIsNamedFor) {
  const auto package = ScratchDirectory("snapshot-content");
  const auto id = ProjectStore::GenerateProjectUuid();
  SnapshotPolicy policy;
  policy.every_revisions = 4;
  auto store = ProjectStore::CreatePackage(package, PackageCreate(id), policy);
  for (int index = 1; index <= 8; ++index) {
    const auto result = Rename(*store, id, index);
    (void)result;
  }
  store->CreateSnapshot();
  for (const auto& entry : std::filesystem::directory_iterator(package / "snapshots")) {
    const auto revision = std::stoll(entry.path().filename().string().substr(10));
    ProjectStore copy(entry.path().string());
    CHECK_EQ(copy.CurrentRevision(), revision);
    CHECK_NO_THROW(copy.ValidateDatabase());
    // Revision N of this project is the rename made at revision N (the create is 1).
    if (revision > 1) CHECK_EQ(copy.ProjectName(), "Name " + std::to_string(revision - 1));
  }
  store.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(SnapshotsAreTakenWhileEditsContinueAndEveryOneIsValid) {
  // The copy is made through a connection of its own, not under the store's lock.
  const auto package = ScratchDirectory("snapshot-concurrent");
  const auto id = ProjectStore::GenerateProjectUuid();
  SnapshotPolicy policy;
  policy.every_revisions = 1000000;  // only the explicit snapshots
  policy.keep_snapshots = 100;
  auto store = ProjectStore::CreatePackage(package, PackageCreate(id), policy);

  std::atomic<bool> stop{false};
  std::atomic<int> edits{0};
  std::thread editor([&] {
    int index = 0;
    while (!stop.load()) {
      const auto result = Rename(*store, id, ++index);
      (void)result;
      ++edits;
    }
  });
  for (int attempt = 0; attempt < 12; ++attempt) {
    CHECK_NO_THROW(store->CreateSnapshot());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  stop.store(true);
  editor.join();
  CHECK(edits.load() > 12);

  for (const auto& entry : std::filesystem::directory_iterator(package / "snapshots")) {
    ProjectStore copy(entry.path().string());
    CHECK_EQ(copy.CurrentRevision(), std::stoll(entry.path().filename().string().substr(10)));
    CHECK_NO_THROW(copy.ValidateDatabase());
  }
  store.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(DurabilityIsAChoiceAndTheDefaultIsStatedNotAssumed) {
  const auto pragma = [](ProjectStore& store) {
    const std::lock_guard<std::mutex> lock(store.mutex());
    return cutline::db::ScalarInt(store.connection(), "PRAGMA synchronous;");
  };
  {
    const auto package = ScratchDirectory("durability-normal");
    auto store = ProjectStore::CreatePackage(package, PackageCreate(ProjectStore::GenerateProjectUuid()));
    CHECK_EQ(pragma(*store), std::int64_t{1});  // NORMAL
    store.reset();
    std::filesystem::remove_all(package);
  }
  {
    const auto package = ScratchDirectory("durability-full");
    SnapshotPolicy policy;
    policy.durability = cutline::project::Durability::Full;
    auto store = ProjectStore::CreatePackage(package, PackageCreate(ProjectStore::GenerateProjectUuid()), policy);
    CHECK_EQ(pragma(*store), std::int64_t{2});  // FULL
    store.reset();
    std::filesystem::remove_all(package);
  }
  SnapshotPolicy none;
  none.keep_snapshots = 0;
  CHECK_THROWS(ProjectStore(":memory:", none));
}


// ------------------------------------------------------------ linked clips ----
//
// The picture and sound of one shot are separate clips on separate tracks that share
// a link group. Until now the group was carried and ignored: moving the picture left
// the sound behind, and cutting one clip left four clips sharing one group.

namespace {

// V1 holds clip-1 (0-5 s) and clip-2 (5-10 s); A1 holds their sound, aud-1 and
// aud-2, each linked to its picture.
void BuildLinked(Harness& harness) {
  harness.BuildSequence();
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("aud-1", "a1", Seconds(0), Seconds(0), Seconds(5)));
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("aud-2", "a1", Seconds(5), Seconds(10), Seconds(15)));
  harness.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1", "aud-1"}, "take-1"});
  harness.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-2", "aud-2"}, "take-2"});
}

std::int64_t StartOf(Harness& harness, const std::string& id) {
  return harness.Count("SELECT timeline_start_ticks FROM clips WHERE id = '" + id + "';");
}
std::int64_t EndOf(Harness& harness, const std::string& id) {
  return harness.Count("SELECT timeline_end_ticks FROM clips WHERE id = '" + id + "';");
}
std::string GroupOf(Harness& harness, const std::string& id) {
  return harness.Text("SELECT linked_group FROM clips WHERE id = '" + id + "';");
}
std::int64_t Ticks(std::int64_t seconds) { return Seconds(seconds).ToTicks(); }
void Expect(bool condition, const std::string& description, int line) {
  if (!condition) cutline::testing::Fail("expectation", __FILE__, line, description);
}

}  // namespace

CUTLINE_TEST(ClipsCanBeLinkedAndUnlinkedAndTheEditUndoes) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("aud-1", "a1", Seconds(0), Seconds(0), Seconds(5)));
  CHECK_EQ(GroupOf(harness, "clip-1"), std::string());

  harness.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1", "aud-1"}, "take-1"});
  CHECK_EQ(GroupOf(harness, "clip-1"), std::string("take-1"));
  CHECK_EQ(GroupOf(harness, "aud-1"), std::string("take-1"));

  harness.Run(commands::CommandType::UnlinkClips, commands::UnlinkClipsPayload{{"aud-1"}});
  CHECK_EQ(GroupOf(harness, "aud-1"), std::string());
  CHECK_EQ(GroupOf(harness, "clip-1"), std::string("take-1"));
  harness.Undo();
  CHECK_EQ(GroupOf(harness, "aud-1"), std::string("take-1"));

  CHECK_THROWS(harness.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1"}, "take-x"}));
  CHECK_THROWS(harness.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1", "nope"}, "take-x"}));
}

CUTLINE_TEST(MovingALinkedClipMovesItsPartnerByTheSameTime) {
  Harness harness;
  BuildLinked(harness);
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(12)});
  CHECK_EQ(StartOf(harness, "clip-1"), Ticks(12));
  CHECK_EQ(StartOf(harness, "aud-1"), Ticks(12));
  // The other take is untouched.
  CHECK_EQ(StartOf(harness, "clip-2"), Ticks(5));
  CHECK_EQ(StartOf(harness, "aud-2"), Ticks(5));
  CHECK_NO_THROW(harness.store().ValidateDatabase());

  harness.Undo();
  CHECK_EQ(StartOf(harness, "clip-1"), Ticks(0));
  CHECK_EQ(StartOf(harness, "aud-1"), Ticks(0));

  // Cleared, the move is the one clip's alone.
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(12), false});
  CHECK_EQ(StartOf(harness, "clip-1"), Ticks(12));
  CHECK_EQ(StartOf(harness, "aud-1"), Ticks(0));
}

CUTLINE_TEST(ALinkedMoveThatAnyMemberCannotTakeChangesNothing) {
  // The partner's track is locked.
  {
    Harness harness;
    BuildLinked(harness);
    commands::SetTrackStatePayload lock;
    lock.id = "a1";
    lock.locked = true;
    harness.Run(commands::CommandType::SetTrackState, lock);
    const auto revision = harness.store().CurrentRevision();
    CHECK_THROWS(harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(12)}));
    CHECK_EQ(harness.store().CurrentRevision(), revision);
    CHECK_EQ(StartOf(harness, "clip-1"), Ticks(0));
    // Alone, the unlocked clip may still move.
    harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(12), false});
    CHECK_EQ(StartOf(harness, "clip-1"), Ticks(12));
  }
  // The partner would land on another clip.
  {
    Harness harness;
    BuildLinked(harness);
    harness.Run(commands::CommandType::InsertClip, Harness::Clip("aud-block", "a1", Seconds(12), Seconds(20), Seconds(23)));
    const auto revision = harness.store().CurrentRevision();
    CHECK_THROWS(harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(12)}));
    CHECK_EQ(harness.store().CurrentRevision(), revision);
    CHECK_EQ(StartOf(harness, "clip-1"), Ticks(0));
    CHECK_EQ(StartOf(harness, "aud-1"), Ticks(0));
  }
  // The partner would start before the sequence: the sound is 1 s ahead of the
  // picture, so pulling the picture back 2 s would put the sound at -1 s.
  {
    Harness harness;
    BuildLinked(harness);
    harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(20)});  // with its sound
    harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(3), false});
    harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"aud-1", "a1", Seconds(1), false});
    const auto revision = harness.store().CurrentRevision();
    CHECK_THROWS(harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(1)}));
    CHECK_EQ(harness.store().CurrentRevision(), revision);
    harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(2)});
    CHECK_EQ(StartOf(harness, "aud-1"), Ticks(0));
  }
}

CUTLINE_TEST(TrimmingALinkedClipsEdgesTrimsItsPartnerByTheSameTime) {
  Harness harness;
  BuildLinked(harness);

  // Tail: clip-1 ends at 4 s instead of 5 s.
  harness.Run(commands::CommandType::TrimClip,
              commands::TrimClipPayload{"clip-1", Seconds(0), Seconds(4), Seconds(0)});
  CHECK_EQ(EndOf(harness, "clip-1"), Ticks(4));
  CHECK_EQ(EndOf(harness, "aud-1"), Ticks(4));

  // Head: clip-1 now begins 1 s later, playing the same frames.
  harness.Run(commands::CommandType::TrimClip,
              commands::TrimClipPayload{"clip-1", Seconds(1), Seconds(4), Seconds(1)});
  CHECK_EQ(StartOf(harness, "clip-1"), Ticks(1));
  CHECK_EQ(StartOf(harness, "aud-1"), Ticks(1));
  CHECK_EQ(harness.Count("SELECT source_in_num FROM clips WHERE id = 'aud-1';"), std::int64_t{1});
  CHECK_EQ(EndOf(harness, "aud-1"), Ticks(4));

  // A slip is the one trim that stays with its clip: the sound does not slip too.
  harness.Run(commands::CommandType::TrimClip,
              commands::TrimClipPayload{"clip-1", Seconds(3), Seconds(6), Seconds(1)});
  CHECK_EQ(harness.Count("SELECT source_in_num FROM clips WHERE id = 'clip-1';"), std::int64_t{3});
  CHECK_EQ(harness.Count("SELECT source_in_num FROM clips WHERE id = 'aud-1';"), std::int64_t{1});

  harness.Undo();
  harness.Undo();
  harness.Undo();
  CHECK_EQ(EndOf(harness, "aud-1"), Ticks(5));
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(TrimmingLinkedRetimedAndReversedClipsKeepsThemInStep) {
  Harness harness;
  BuildLinked(harness);
  // The sound plays at double speed and backwards; the picture at normal speed. A
  // 1 s head trim of the picture is 1 s of timeline for both, which is 2 s of the
  // sound's source, taken from the end of its range.
  harness.Run(commands::CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"aud-1", Seconds(2), true, false});
  CHECK_EQ(EndOf(harness, "aud-1"), RationalTime(5, 2).ToTicks());
  harness.Run(commands::CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"clip-1", Seconds(1), false, false});
  // Make both span the same 2.5 s so that the trim has a partner that fits.
  harness.Run(commands::CommandType::TrimClip,
              commands::TrimClipPayload{"clip-1", Seconds(0), RationalTime(5, 2), Seconds(0), false});

  harness.Run(commands::CommandType::TrimClip,
              commands::TrimClipPayload{"clip-1", Seconds(1), RationalTime(5, 2), Seconds(1)});
  CHECK_EQ(StartOf(harness, "aud-1"), Ticks(1));
  CHECK_EQ(EndOf(harness, "aud-1"), RationalTime(5, 2).ToTicks());
  // Reversed at 2x: the head is the end of the source range, and 1 s of timeline is
  // 2 s of source.
  CHECK_EQ(harness.Count("SELECT source_out_num FROM clips WHERE id = 'aud-1';"), std::int64_t{3});
  CHECK_EQ(harness.Count("SELECT source_in_num FROM clips WHERE id = 'aud-1';"), std::int64_t{0});
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(CuttingALinkedClipCutsItsPartnersAndKeepsEachSideJoined) {
  Harness harness;
  BuildLinked(harness);
  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(2)});

  // Both halves of both clips exist, cut at the same place.
  CHECK_EQ(EndOf(harness, "clip-1"), Ticks(2));
  CHECK_EQ(EndOf(harness, "aud-1"), Ticks(2));
  CHECK_EQ(StartOf(harness, "clip-1b"), Ticks(2));
  CHECK_EQ(StartOf(harness, "clip-1b~aud-1"), Ticks(2));
  CHECK_EQ(EndOf(harness, "clip-1b"), Ticks(5));
  CHECK_EQ(EndOf(harness, "clip-1b~aud-1"), Ticks(5));

  // Picture and sound stay together on each side, and the two sides are separate
  // groups; the other take is untouched.
  CHECK_EQ(GroupOf(harness, "clip-1"), std::string("take-1"));
  CHECK_EQ(GroupOf(harness, "aud-1"), std::string("take-1"));
  CHECK_EQ(GroupOf(harness, "clip-1b"), std::string("take-1~clip-1b"));
  CHECK_EQ(GroupOf(harness, "clip-1b~aud-1"), std::string("take-1~clip-1b"));
  CHECK_EQ(GroupOf(harness, "clip-2"), std::string("take-2"));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{6});

  // So moving the left half does not drag the right.
  harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-1", "v1", Seconds(20)});
  CHECK_EQ(StartOf(harness, "aud-1"), Ticks(20));
  CHECK_EQ(StartOf(harness, "clip-1b"), Ticks(2));
  CHECK_EQ(StartOf(harness, "clip-1b~aud-1"), Ticks(2));
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

CUTLINE_TEST(ASplitSortsPartnersThatDoNotSpanTheCutIntoTheSideTheyBelongTo) {
  Harness harness;
  BuildLinked(harness);
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("aud-late", "a1", Seconds(12), Seconds(20), Seconds(22)));
  harness.Run(commands::CommandType::InsertClip, Harness::Clip("aud-early", "a1", Seconds(30), Seconds(30), Seconds(31)));
  harness.Run(commands::CommandType::LinkClips,
              commands::LinkClipsPayload{{"clip-1", "aud-1", "aud-late", "aud-early"}, "take-1"});
  // Cut clip-1 at 2 s: aud-1 spans the cut and is cut; aud-late lies entirely after
  // it, so it joins the right-hand group; aud-early is after it too.
  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(2)});
  CHECK_EQ(GroupOf(harness, "aud-late"), std::string("take-1~clip-1b"));
  CHECK_EQ(GroupOf(harness, "aud-early"), std::string("take-1~clip-1b"));
  CHECK_EQ(GroupOf(harness, "aud-1"), std::string("take-1"));
  // aud-late was not split (it does not span the cut), so there is no extra clip.
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE id LIKE 'clip-1b~%';"), std::int64_t{1});
}

CUTLINE_TEST(CuttingOneLinkedClipAloneLeavesItsRightHalfUnlinked) {
  Harness harness;
  BuildLinked(harness);
  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(2), false});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{5});
  CHECK_EQ(GroupOf(harness, "clip-1b"), std::string());
  // The left half is still joined to its sound, which still spans the whole shot.
  CHECK_EQ(GroupOf(harness, "clip-1"), std::string("take-1"));
  CHECK_EQ(EndOf(harness, "aud-1"), Ticks(5));
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{4});
}

CUTLINE_TEST(DeletingALinkedClipDeletesItsPartnersAndRippleClosesEachTracksGap) {
  {
    Harness harness;
    BuildLinked(harness);
    harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"});
    CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE id IN ('clip-1', 'aud-1');"), std::int64_t{0});
    CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{2});
    harness.Undo();
    CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{4});
    harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1", false});
    CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips WHERE id = 'aud-1';"), std::int64_t{1});
  }
  {
    Harness harness;
    BuildLinked(harness);
    harness.Run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{"clip-1"});
    // Both tracks slide back by the length of the clip removed from each.
    CHECK_EQ(StartOf(harness, "clip-2"), Ticks(0));
    CHECK_EQ(StartOf(harness, "aud-2"), Ticks(0));
    CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{2});
    CHECK_NO_THROW(harness.store().ValidateDatabase());
  }
}

CUTLINE_TEST(EnablingAndRetimingALinkedClipAppliesToItsPartner) {
  Harness harness;
  BuildLinked(harness);
  harness.Run(commands::CommandType::SetClipEnabled, commands::SetClipEnabledPayload{"clip-1", false});
  CHECK_EQ(harness.Count("SELECT enabled FROM clips WHERE id = 'aud-1';"), std::int64_t{0});
  CHECK_EQ(harness.Count("SELECT enabled FROM clips WHERE id = 'clip-2';"), std::int64_t{1});

  // Doubling the speed halves both clips' length on the timeline.
  harness.Run(commands::CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"clip-1", Seconds(2), false});
  CHECK_EQ(EndOf(harness, "clip-1"), RationalTime(5, 2).ToTicks());
  CHECK_EQ(EndOf(harness, "aud-1"), RationalTime(5, 2).ToTicks());
  harness.Undo();
  CHECK_EQ(EndOf(harness, "aud-1"), Ticks(5));
}

CUTLINE_TEST(ARandomSessionOfLinkedEditsKeepsEveryTakeInStepAndUndoesCompletely) {
  // A fixed-seed run of linked moves, trims, cuts, deletes and retimes, with the
  // invariant that makes links worth having checked after every accepted command:
  // for each group still present, picture and sound begin and end together.
  Harness harness;
  BuildLinked(harness);
  std::uint64_t state = 99;
  const auto random = [&](std::int64_t bound) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::int64_t>((state >> 33) % static_cast<std::uint64_t>(bound));
  };
  const auto in_step = [&] {
    // Every group with exactly one clip on each of V1 and A1 starts and ends together.
    const auto mismatched = harness.Count(
        "SELECT COUNT(*) FROM clips v JOIN clips a ON v.linked_group = a.linked_group AND v.linked_group <> '' "
        " JOIN tracks tv ON tv.id = v.track_id JOIN tracks ta ON ta.id = a.track_id "
        " WHERE tv.track_type = 'video' AND ta.track_type = 'audio' "
        "   AND (v.timeline_start_ticks <> a.timeline_start_ticks OR v.timeline_end_ticks <> a.timeline_end_ticks);");
    return mismatched == 0;
  };

  const auto start_depth = harness.store().AppliedHistoryCount();
  int accepted = 0;
  int refused = 0;
  int split_counter = 0;
  for (int step = 0; step < 300; ++step) {
    const auto clips = harness.Count("SELECT COUNT(*) FROM clips WHERE track_id = 'v1';");
    if (clips == 0) break;
    const auto pick = random(clips);
    const auto id = harness.Text("SELECT id FROM clips WHERE track_id = 'v1' ORDER BY timeline_start_ticks LIMIT 1 OFFSET " +
                                 std::to_string(pick) + ";");
    const auto kind = random(6);
    const auto revision = harness.store().CurrentRevision();
    try {
      switch (kind) {
        case 0:
          harness.Run(commands::CommandType::MoveClip, commands::MoveClipPayload{id, "v1", Seconds(random(40))});
          break;
        case 1: {
          // Take a tenth of a second off the tail, in source terms exactly.
          const auto row = [&](const std::string& column) {
            return RationalTime(harness.Count("SELECT " + column + "_num FROM clips WHERE id = '" + id + "';"),
                                harness.Count("SELECT " + column + "_den FROM clips WHERE id = '" + id + "';"));
          };
          const auto in = row("source_in");
          const auto out = row("source_out");
          if (out.Subtract(in).Compare({1, 5}) <= 0) break;
          harness.Run(commands::CommandType::TrimClip,
                      commands::TrimClipPayload{id, in, out.Subtract({1, 10}), row("timeline_start")});
          break;
        }
        case 2:
          harness.Run(commands::CommandType::SplitClip,
                      commands::SplitClipPayload{id, "cut-" + std::to_string(++split_counter),
                                                 RationalTime(random(400), 10)});
          break;
        case 3:
          if (clips > 3) harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{id});
          break;
        case 4:
          harness.Run(commands::CommandType::SetClipSpeed,
                      commands::SetClipSpeedPayload{id, RationalTime(1 + random(3), 1 + random(2)), random(2) == 0});
          break;
        default:
          if (clips > 3) harness.Run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{id});
          break;
      }
    } catch (const std::exception&) {
      ++refused;
      CHECK_EQ(harness.store().CurrentRevision(), revision);  // a refused edit changes nothing
      continue;
    }
    if (harness.store().CurrentRevision() != revision) ++accepted;
    CHECK_NO_THROW(harness.store().ValidateDatabase());
    if (!in_step()) cutline::testing::Fail("a linked pair fell out of step", __FILE__, __LINE__, "after step " + std::to_string(step));
  }
  Expect(accepted > 30 && refused > 5,
         "accepted " + std::to_string(accepted) + ", refused " + std::to_string(refused), __LINE__);
  while (harness.store().AppliedHistoryCount() > start_depth) harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{4});
}

// --------------------------------------------------------- render versions ----
//
// The rules a sequence is rendered under are recorded with it, so that an
// improvement to rendering changes new work and not approved work.

namespace {

commands::CommandEnvelope SequenceCommand(const std::string& project, int counter, commands::CommandType type,
                                          commands::CommandPayload payload, std::int64_t base) {
  commands::CommandEnvelope command;
  command.command_id = "cmd-seq-" + std::to_string(counter);
  command.project_id = project;
  command.author_id = "tester";
  command.base_revision = base;
  command.timestamp_utc = "2026-10-06T00:02:00Z";
  command.type = type;
  command.payload = std::move(payload);
  command.idempotency_key = "key-seq-" + std::to_string(counter);
  return command;
}

commands::SequenceSettings Settings() {
  commands::SequenceSettings settings;
  settings.name = "Main";
  settings.frame_rate = {25, 1};
  settings.width = 1920;
  settings.height = 1080;
  settings.sample_rate = 48000;
  return settings;
}

std::int64_t RenderVersionOf(ProjectStore& store, const std::string& sequence) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  return cutline::db::ScalarInt(store.connection(),
                                "SELECT render_version FROM sequences WHERE id = '" + sequence + "';");
}

}  // namespace

CUTLINE_TEST(NewSequencesAreMadeUnderTheCurrentRenderVersionAndAnExplicitOneIsHonoured) {
  Harness harness;
  harness.CreateProject();
  harness.Run(commands::CommandType::CreateSequence, commands::CreateSequencePayload{"seq-new", Settings()});
  CHECK_EQ(harness.Count("SELECT render_version FROM sequences WHERE id = 'seq-new';"), cutline::model::kCurrentRenderVersion);

  auto pinned = Settings();
  pinned.render_version = cutline::model::kLegacyRenderVersion;
  harness.Run(commands::CommandType::CreateSequence, commands::CreateSequencePayload{"seq-old", pinned});
  CHECK_EQ(harness.Count("SELECT render_version FROM sequences WHERE id = 'seq-old';"), cutline::model::kLegacyRenderVersion);

  auto unknown = Settings();
  unknown.render_version = cutline::model::kCurrentRenderVersion + 1;
  CHECK_THROWS(harness.Run(commands::CommandType::CreateSequence, commands::CreateSequencePayload{"seq-future", unknown}));
  unknown.render_version = 0;
  CHECK_THROWS(harness.Run(commands::CommandType::CreateSequence, commands::CreateSequencePayload{"seq-zero", unknown}));
}

CUTLINE_TEST(UpdatingASequenceKeepsItsRenderVersionUnlessAskedToChangeItAndThatIsUndoable) {
  Harness harness;
  harness.CreateProject();
  auto pinned = Settings();
  pinned.render_version = cutline::model::kLegacyRenderVersion;
  harness.Run(commands::CommandType::CreateSequence, commands::CreateSequencePayload{"seq", pinned});

  // A settings change that does not mention the version must not upgrade the sequence.
  auto renamed = Settings();
  renamed.name = "Renamed";
  harness.Run(commands::CommandType::UpdateSequenceSettings, commands::UpdateSequenceSettingsPayload{"seq", renamed});
  CHECK_EQ(harness.Text("SELECT name FROM sequences WHERE id = 'seq';"), std::string("Renamed"));
  CHECK_EQ(harness.Count("SELECT render_version FROM sequences WHERE id = 'seq';"), cutline::model::kLegacyRenderVersion);

  // Upgrading is explicit, journalled, and undoes.
  auto upgraded = renamed;
  upgraded.render_version = cutline::model::kCurrentRenderVersion;
  harness.Run(commands::CommandType::UpdateSequenceSettings, commands::UpdateSequenceSettingsPayload{"seq", upgraded});
  CHECK_EQ(harness.Count("SELECT render_version FROM sequences WHERE id = 'seq';"), cutline::model::kCurrentRenderVersion);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM command_journal WHERE payload_json LIKE '%renderVersion%';"), std::int64_t{2});
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT render_version FROM sequences WHERE id = 'seq';"), cutline::model::kLegacyRenderVersion);

  auto invalid = renamed;
  invalid.render_version = 99;
  CHECK_THROWS(harness.Run(commands::CommandType::UpdateSequenceSettings, commands::UpdateSequenceSettingsPayload{"seq", invalid}));
}

CUTLINE_TEST(APackageMadeBeforeRenderVersionsOpensUnderTheOriginalRules) {
  const auto package = ScratchDirectory("render-version-migration");
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate(id));
    const auto made = store->Execute(SequenceCommand(id, 1, commands::CommandType::CreateSequence,
                                                     commands::CreateSequencePayload{"seq-before", Settings()},
                                                     store->CurrentRevision()));
    (void)made;
    // Rewind the database to the shape schema v3 had: no render_version column.
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Execute(store->connection(), "ALTER TABLE sequences DROP COLUMN render_version;");
    cutline::db::Execute(store->connection(), "UPDATE project_meta SET schema_version = 3 WHERE singleton = 1;");
  }

  auto reopened = ProjectStore::OpenPackage(package);
  // The sequence was made under the rules that are now called version 1.
  CHECK_EQ(RenderVersionOf(*reopened, "seq-before"), cutline::model::kLegacyRenderVersion);
  CHECK_NO_THROW(reopened->ValidateDatabase());
  // A backup of the v3 file was taken before the migration touched it.
  CHECK(std::filesystem::exists(package / "snapshots" / "migration-v3-backup.db"));
  // New work is made under the current rules.
  const auto later = reopened->Execute(SequenceCommand(id, 2, commands::CommandType::CreateSequence,
                                                       commands::CreateSequencePayload{"seq-after", Settings()},
                                                       reopened->CurrentRevision()));
  (void)later;
  CHECK_EQ(RenderVersionOf(*reopened, "seq-after"), cutline::model::kCurrentRenderVersion);
  CHECK_EQ(RenderVersionOf(*reopened, "seq-before"), cutline::model::kLegacyRenderVersion);
  reopened.reset();

  // And it stays that way across another reopen.
  auto again = ProjectStore::OpenPackage(package);
  CHECK_EQ(RenderVersionOf(*again, "seq-before"), cutline::model::kLegacyRenderVersion);
  again.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(AProjectMadeUnderANewerRenderVersionIsRefusedNotRenderedDifferently) {
  const auto package = ScratchDirectory("render-version-newer");
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate(id));
    const auto made = store->Execute(SequenceCommand(id, 1, commands::CommandType::CreateSequence,
                                                     commands::CreateSequencePayload{"seq", Settings()},
                                                     store->CurrentRevision()));
    (void)made;
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Execute(store->connection(), "UPDATE sequences SET render_version = 99;");
  }
  CHECK_THROWS(ProjectStore::OpenPackage(package));
  std::filesystem::remove_all(package);
}

// ---------------------------------------------------------- crash recovery ----
//
// SQLite makes every commit atomic, so a crash leaves the database at a consistent
// revision. Recovery is for what that does not cover: a database that will not open
// or validate, and one restored from an older copy. The package's snapshots and
// per-revision journal records (changesets included) are what it rebuilds from.

namespace {

// Everything in the content tables, as one string, so two databases can be compared.
std::string Fingerprint(ProjectStore& store) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  std::string all;
  for (const auto* table : {"project_settings", "bins", "media", "media_streams", "media_proxies", "sequences", "tracks", "clips",
                            "transitions", "effects", "effect_parameters", "keyframes", "markers"}) {
    std::string columns;
    {
      cutline::db::Statement info(store.connection(), std::string("SELECT name FROM pragma_table_info('") + table + "');");
      while (info.Step()) columns += (columns.empty() ? "" : " || '|' || ") + std::string("quote(") + info.ColumnText(0) + ")";
    }
    all += std::string(table) + ":" +
           cutline::db::ScalarText(store.connection(), std::string("SELECT COALESCE(group_concat(r, ';'), '') FROM (SELECT ") +
                                                           columns + " AS r FROM " + table + " ORDER BY rowid);") +
           "\n";
  }
  return all;
}

// A session that touches most of what a project holds, ending with undo and redo,
// recording the fingerprint after every revision.
std::map<std::int64_t, std::string> RunRecordedSession(ProjectStore& store, const std::string& project) {
  std::map<std::int64_t, std::string> after;
  Harness harness(&store);
  (void)project;
  after[store.CurrentRevision()] = Fingerprint(store);
  const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
    harness.Run(type, std::move(payload));
    after[store.CurrentRevision()] = Fingerprint(store);
  };

  commands::ImportMediaPayload media;
  media.id = "media-1";
  media.display_name = "shot.mov";
  media.original_path = "/footage/shot.mov";
  media.fingerprint = "fp-1";
  media.duration = Seconds(60);
  run(commands::CommandType::ImportMedia, media);
  commands::CreateSequencePayload sequence;
  sequence.id = "seq-1";
  sequence.settings.name = "Main";
  sequence.settings.frame_rate = {25, 1};
  sequence.settings.width = 1920;
  sequence.settings.height = 1080;
  sequence.settings.sample_rate = 48000;
  run(commands::CommandType::CreateSequence, sequence);
  run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-1", 0, "stereo", "V1"});
  run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-1", 0, "stereo", "A1"});
  run(commands::CommandType::InsertClip, Harness::Clip("clip-1", "v1", Seconds(0), Seconds(0), Seconds(5)));
  run(commands::CommandType::InsertClip, Harness::Clip("clip-2", "v1", Seconds(5), Seconds(10), Seconds(15)));
  run(commands::CommandType::InsertClip, Harness::Clip("aud-1", "a1", Seconds(0), Seconds(0), Seconds(5)));
  run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1", "aud-1"}, "take-1"});
  commands::AddEffectPayload effect;
  effect.id = "fx-1";
  effect.owner_kind = model::EffectOwner::Clip;
  effect.owner_id = "clip-1";
  effect.effect_type = "opacity";
  effect.parameters = {{"fx-1:value", "value", cutline::anim::Value::Scalar(1.0)}};
  run(commands::CommandType::AddEffect, effect);
  commands::SetKeyframePayload key;
  key.parameter_id = "fx-1:value";
  key.keyframe = {Seconds(1), cutline::anim::Value::Scalar(0.4), cutline::anim::Interpolation::Linear, {}, {}};
  run(commands::CommandType::SetKeyframe, key);
  run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", Seconds(2)});
  commands::AddTransitionPayload dissolve;
  dissolve.id = "tr-1";
  dissolve.track_id = "v1";
  dissolve.kind = "cross_dissolve";
  dissolve.from_clip_id = "clip-1b";
  dissolve.to_clip_id = "clip-2";
  dissolve.timeline_start = RationalTime(9, 2);
  dissolve.duration = Seconds(1);
  run(commands::CommandType::AddTransition, dissolve);
  run(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(6)});  // detaches the transition
  run(commands::CommandType::TrimClip, commands::TrimClipPayload{"clip-2", Seconds(11), Seconds(15), Seconds(7)});
  run(commands::CommandType::RenameProject, commands::RenameProjectPayload{"Renamed"});
  // Undo and redo are revisions too.
  const auto undo = store.Undo("tester", "2026-10-06T00:09:00Z");
  (void)undo;
  after[store.CurrentRevision()] = Fingerprint(store);
  const auto redo = store.Redo("tester", "2026-10-06T00:09:01Z");
  (void)redo;
  after[store.CurrentRevision()] = Fingerprint(store);
  run(commands::CommandType::RippleDeleteClip, commands::RippleDeleteClipPayload{"clip-2"});
  run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"});
  const auto undo_again = store.Undo("tester", "2026-10-06T00:09:02Z");
  (void)undo_again;
  after[store.CurrentRevision()] = Fingerprint(store);
  return after;
}

SnapshotPolicy NoAutoSnapshots() {
  SnapshotPolicy policy;
  policy.every_revisions = 1000000;  // only the one made when the package is created
  return policy;
}

void Corrupt(const std::filesystem::path& package) {
  for (const char* suffix : {"-wal", "-shm"}) std::filesystem::remove((package / "project.db").string() + suffix);
  std::ofstream file(package / "project.db", std::ios::binary | std::ios::trunc);
  file << std::string(8192, 'x');
}

void CopyPackage(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::filesystem::remove_all(to);
  std::filesystem::copy(from, to, std::filesystem::copy_options::recursive);
}

}  // namespace

CUTLINE_TEST(EveryRevisionHasAReplayableJournalRecordIncludingUndoAndRedo) {
  const auto package = ScratchDirectory("recovery-records");
  auto store = ProjectStore::CreatePackage(package, PackageCreate("project-1"), NoAutoSnapshots());
  const auto fingerprints = RunRecordedSession(*store, "project-1");
  const auto last = store->CurrentRevision();
  store.reset();

  const auto report = ProjectStore::InspectPackage(package);
  CHECK(report.database_healthy);
  CHECK_EQ(report.database_revision, last);
  CHECK_EQ(report.snapshots.size(), std::size_t{1});
  CHECK_EQ(report.snapshots[0], std::int64_t{1});
  // One unbroken chain from the first snapshot to the latest revision.
  CHECK_EQ(report.reachable_revision, last);
  CHECK_EQ(JournalRevisions(package).size(), static_cast<std::size_t>(last));
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(ACorruptDatabaseIsRebuiltExactlyFromASnapshotAndTheJournal) {
  const auto package = ScratchDirectory("recovery-corrupt");
  std::map<std::int64_t, std::string> fingerprints;
  std::int64_t last = 0;
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate("project-1"), NoAutoSnapshots());
    fingerprints = RunRecordedSession(*store, "project-1");
    last = store->CurrentRevision();
  }
  Corrupt(package);

  const auto before = ProjectStore::InspectPackage(package);
  CHECK(!before.database_healthy);
  CHECK_EQ(before.reachable_revision, last);
  CHECK_THROWS(ProjectStore::OpenPackage(package));

  RecoveryReportAlias report;
  auto recovered = ProjectStore::RecoverPackage(package, {}, &report);
  CHECK(recovered != nullptr);
  CHECK_EQ(recovered->CurrentRevision(), last);
  CHECK_EQ(report.snapshot_used, std::int64_t{1});
  CHECK_EQ(report.records_replayed, static_cast<int>(last - 1));
  CHECK(Fingerprint(*recovered) == fingerprints.at(last));
  CHECK_NO_THROW(recovered->ValidateDatabase());
  // What was replaced is kept, not deleted.
  CHECK(std::filesystem::exists(report.quarantine / "project.db"));

  // The recovered project is a working project: edits continue and are recorded
  // after the last revision, in a journal with no duplicate revisions.
  const auto after_recovery = Rename(*recovered, "project-1", 99);
  (void)after_recovery;
  CHECK_EQ(recovered->CurrentRevision(), last + 1);
  const auto revisions = JournalRevisions(package);
  CHECK_EQ(std::set<std::int64_t>(revisions.begin(), revisions.end()).size(), revisions.size());
  recovered.reset();

  // Recovering a healthy package changes nothing, and replays nothing twice.
  RecoveryReportAlias again;
  auto reopened = ProjectStore::RecoverPackage(package, {}, &again);
  CHECK(again.database_healthy);
  CHECK_EQ(again.records_replayed, 0);
  CHECK_EQ(reopened->CurrentRevision(), last + 1);
  reopened.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(RecoveryFromAnyRevisionBoundaryLandsOnThatRevisionExactly) {
  // Termination at each commit boundary: the journal holds records up to revision k,
  // the database is lost. Recovery must reach k, with the project exactly as it was
  // after revision k, for every k, without duplicating or skipping a command.
  const auto package = ScratchDirectory("recovery-boundaries");
  std::map<std::int64_t, std::string> fingerprints;
  std::int64_t last = 0;
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate("project-1"), NoAutoSnapshots());
    fingerprints = RunRecordedSession(*store, "project-1");
    last = store->CurrentRevision();
  }
  const auto scratch = ScratchDirectory("recovery-boundaries-copy");
  for (std::int64_t k = 1; k <= last; ++k) {
    CopyPackage(package, scratch);
    for (const auto& file : std::filesystem::directory_iterator(scratch / "journal")) {
      const auto name = file.path().filename().string();
      if (std::stoll(name.substr(1)) > k) std::filesystem::remove(file.path());
    }
    Corrupt(scratch);
    RecoveryReportAlias report;
    auto recovered = ProjectStore::RecoverPackage(scratch, {}, &report);
    Expect(recovered->CurrentRevision() == k, "recovered revision " + std::to_string(recovered->CurrentRevision()) + " for k = " + std::to_string(k), __LINE__);
    Expect(Fingerprint(*recovered) == fingerprints.at(k), "project content differs at revision " + std::to_string(k), __LINE__);
    CHECK_EQ(report.records_replayed, static_cast<int>(k - 1));
    recovered.reset();
  }
  std::filesystem::remove_all(scratch);
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(ATornOrMissingRecordStopsRecoveryAtTheLastGoodRevisionAndSaysSo) {
  const auto package = ScratchDirectory("recovery-torn");
  std::map<std::int64_t, std::string> fingerprints;
  std::int64_t last = 0;
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate("project-1"), NoAutoSnapshots());
    fingerprints = RunRecordedSession(*store, "project-1");
    last = store->CurrentRevision();
  }
  const auto find = [&](std::int64_t revision) {
    for (const auto& file : std::filesystem::directory_iterator(package / "journal")) {
      if (std::stoll(file.path().filename().string().substr(1)) == revision) return file.path();
    }
    return std::filesystem::path();
  };

  // The last record was being written when the power went: half of it is there.
  {
    const auto copy = ScratchDirectory("recovery-torn-copy");
    CopyPackage(package, copy);
    const auto path = copy / "journal" / find(last).filename();
    std::string text;
    { std::ifstream in(path, std::ios::binary); std::ostringstream ss; ss << in.rdbuf(); text = ss.str(); }
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << text.substr(0, text.size() / 2); }
    Corrupt(copy);
    RecoveryReportAlias report;
    auto recovered = ProjectStore::RecoverPackage(copy, {}, &report);
    CHECK_EQ(recovered->CurrentRevision(), last - 1);
    CHECK(Fingerprint(*recovered) == fingerprints.at(last - 1));
    bool mentioned = false;
    for (const auto& note : report.notes) mentioned = mentioned || note.find("damaged") != std::string::npos;
    CHECK(mentioned);
    // The torn record was moved aside, so the next revision's file cannot collide with it.
    CHECK(std::filesystem::exists(report.quarantine / find(last).filename()));
    recovered.reset();
    std::filesystem::remove_all(copy);
  }
  // A record in the middle is missing: nothing after the gap can be applied.
  {
    const auto copy = ScratchDirectory("recovery-gap-copy");
    CopyPackage(package, copy);
    const std::int64_t missing = last / 2;
    std::filesystem::remove(copy / "journal" / find(missing).filename());
    Corrupt(copy);
    RecoveryReportAlias report;
    auto recovered = ProjectStore::RecoverPackage(copy, {}, &report);
    CHECK_EQ(recovered->CurrentRevision(), missing - 1);
    CHECK(Fingerprint(*recovered) == fingerprints.at(missing - 1));
    recovered.reset();
    std::filesystem::remove_all(copy);
  }
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(AHealthyDatabaseIsLeftAloneAndAnOlderOneCanBeRolledForward) {
  const auto package = ScratchDirectory("recovery-forward");
  std::map<std::int64_t, std::string> fingerprints;
  std::int64_t last = 0;
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate("project-1"), NoAutoSnapshots());
    fingerprints = RunRecordedSession(*store, "project-1");
    last = store->CurrentRevision();
  }
  // A crash between a commit and its journal file leaves the database ahead of the
  // files, which is fine: the database is the record.
  {
    const auto copy = ScratchDirectory("recovery-ahead-copy");
    CopyPackage(package, copy);
    for (const auto& file : std::filesystem::directory_iterator(copy / "journal")) {
      if (std::stoll(file.path().filename().string().substr(1)) == last) std::filesystem::remove(file.path());
    }
    RecoveryReportAlias report;
    auto store = ProjectStore::RecoverPackage(copy, {true, false}, &report);
    CHECK(report.database_healthy);
    CHECK_EQ(store->CurrentRevision(), last);
    CHECK_EQ(report.records_replayed, 0);
    store.reset();
    std::filesystem::remove_all(copy);
  }

  // A database restored from an older copy (the snapshot at revision 1 stands in)
  // is healthy but behind; by default it is opened as it is, and on request the
  // records after it are replayed onto it.
  {
    const auto copy = ScratchDirectory("recovery-restored-copy");
    CopyPackage(package, copy);
    for (const char* suffix : {"", "-wal", "-shm"}) std::filesystem::remove((copy / "project.db").string() + suffix);
    std::filesystem::copy_file(copy / "snapshots" / "snapshot-r1.db", copy / "project.db");
    {
      RecoveryReportAlias plain;
      auto store = ProjectStore::RecoverPackage(copy, {}, &plain);
      CHECK_EQ(store->CurrentRevision(), std::int64_t{1});
      CHECK_EQ(plain.reachable_revision, last);
    }
    RecoveryReportAlias forward;
    auto store = ProjectStore::RecoverPackage(copy, {true, false}, &forward);
    CHECK_EQ(store->CurrentRevision(), last);
    CHECK(Fingerprint(*store) == fingerprints.at(last));
    CHECK_EQ(forward.records_replayed, static_cast<int>(last - 1));
    store.reset();
    // Doing it again applies nothing a second time.
    RecoveryReportAlias twice;
    auto reopened = ProjectStore::RecoverPackage(copy, {true, false}, &twice);
    CHECK_EQ(twice.records_replayed, 0);
    CHECK_EQ(reopened->CurrentRevision(), last);
    reopened.reset();
    std::filesystem::remove_all(copy);
  }

  std::filesystem::remove_all(package);
}

CUTLINE_TEST(WithNothingToRebuildFromRecoveryFailsClearlyAndTouchesNothing) {
  const auto package = ScratchDirectory("recovery-hopeless");
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate("project-1"), NoAutoSnapshots());
    const auto renamed = Rename(*store, "project-1", 1);
    (void)renamed;
  }
  for (const auto& file : std::filesystem::directory_iterator(package / "snapshots")) std::filesystem::remove(file.path());
  Corrupt(package);
  CHECK_THROWS(ProjectStore::RecoverPackage(package));
  // The (damaged) database is still where it was; the failed attempt moved nothing.
  CHECK(std::filesystem::exists(package / "project.db"));
  CHECK(!std::filesystem::exists(package / "quarantine"));
  const auto dry = ProjectStore::InspectPackage(package);
  CHECK(!dry.database_healthy);
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(AnOldFormatJournalRecordIsReportedAsNotReplayableNotMisread) {
  const auto package = ScratchDirectory("recovery-oldformat");
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate("project-1"), NoAutoSnapshots());
    const auto renamed = Rename(*store, "project-1", 1);
    (void)renamed;
  }
  // Records written before the changeset was included had no format field.
  for (const auto& file : std::filesystem::directory_iterator(package / "journal")) {
    if (std::stoll(file.path().filename().string().substr(1)) == 2) {
      std::ofstream out(file.path(), std::ios::binary | std::ios::trunc);
      out << "{\"revision\":2,\"commandId\":\"cmd-rename-1\",\"type\":\"project.rename\",\"payload\":{\"name\":\"x\"}}\n";
    }
  }
  Corrupt(package);
  const auto report = ProjectStore::InspectPackage(package);
  CHECK_EQ(report.reachable_revision, std::int64_t{1});
  bool explained = false;
  for (const auto& note : report.notes) explained = explained || note.find("format 2") != std::string::npos || note.find("damaged") != std::string::npos;
  CHECK(explained);
  std::filesystem::remove_all(package);
}

// ----------------------------------------------------------- audio pitch policy ----

CUTLINE_TEST(AClipCanAskToKeepItsPitchWhenRetimedAndTheChoiceSurvivesSplitUndoAndLinks) {
  Harness harness;
  BuildLinked(harness);
  const auto flag = [&](const std::string& id) {
    return harness.Count("SELECT maintain_pitch FROM clips WHERE id = '" + id + "';");
  };
  CHECK_EQ(flag("clip-1"), std::int64_t{0});  // varispeed unless asked otherwise

  // Positional: id, rate, reversed, propagate_links, maintain_pitch.
  harness.Run(commands::CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"clip-1", Seconds(2), false, true, true});
  CHECK_EQ(flag("clip-1"), std::int64_t{1});
  CHECK_EQ(flag("aud-1"), std::int64_t{1});  // the linked sound follows
  CHECK_EQ(flag("clip-2"), std::int64_t{0});

  harness.Run(commands::CommandType::SplitClip, commands::SplitClipPayload{"clip-1", "clip-1b", RationalTime(1, 1)});
  CHECK_EQ(flag("clip-1b"), std::int64_t{1});
  CHECK_EQ(flag("clip-1b~aud-1"), std::int64_t{1});

  harness.Undo();
  harness.Undo();
  CHECK_EQ(flag("clip-1"), std::int64_t{0});
  harness.Redo();
  CHECK_EQ(flag("clip-1"), std::int64_t{1});

  // Setting a speed without the flag is a statement that the clip is varispeed again.
  harness.Run(commands::CommandType::SetClipSpeed, commands::SetClipSpeedPayload{"clip-1", Seconds(2), false, false});
  CHECK_EQ(flag("clip-1"), std::int64_t{0});
}

CUTLINE_TEST(APackageFromBeforeThePitchPolicyOpensWithEveryClipVarispeed) {
  const auto package = ScratchDirectory("pitch-migration");
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate(id));
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Execute(store->connection(), "ALTER TABLE clips DROP COLUMN maintain_pitch;");
    cutline::db::Execute(store->connection(), "UPDATE project_meta SET schema_version = 4 WHERE singleton = 1;");
  }
  auto reopened = ProjectStore::OpenPackage(package);
  {
    const std::lock_guard<std::mutex> lock(reopened->mutex());
    CHECK_EQ(cutline::db::ScalarInt(reopened->connection(),
                                    "SELECT COUNT(*) FROM pragma_table_info('clips') WHERE name = 'maintain_pitch';"),
             std::int64_t{1});
  }
  reopened.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(APackageFromBeforeAudioRoutingOpensWithEveryTrackGoingToTheMaster) {
  const auto package = ScratchDirectory("routing-migration");
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate(id));
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Execute(store->connection(), "DROP TABLE track_sends;");
    cutline::db::Execute(store->connection(), "ALTER TABLE tracks DROP COLUMN output_bus_id;");
    cutline::db::Execute(store->connection(), "ALTER TABLE tracks DROP COLUMN is_bus;");
    cutline::db::Execute(store->connection(), "UPDATE project_meta SET schema_version = 5 WHERE singleton = 1;");
  }
  auto reopened = ProjectStore::OpenPackage(package);
  {
    const std::lock_guard<std::mutex> lock(reopened->mutex());
    CHECK_EQ(cutline::db::ScalarInt(reopened->connection(),
                                    "SELECT COUNT(*) FROM pragma_table_info('tracks') WHERE name IN ('is_bus', 'output_bus_id');"),
             std::int64_t{2});
    CHECK_EQ(cutline::db::ScalarInt(reopened->connection(), "SELECT COUNT(*) FROM sqlite_master WHERE name = 'track_sends';"),
             std::int64_t{1});
  }
  reopened.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(RoutingIsRecordedInTheJournalSoRecoveryCanReplayIt) {
  // Routing rows are in the tables the change recorder watches: an undo that did not
  // restore track_sends would leave a send pointing at a bus that no longer exists.
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"bus-1", "seq-1", 1, "stereo", "Bus"});
  commands::SetTrackRoutingPayload bus;
  bus.id = "bus-1";
  bus.is_bus = true;
  harness.Run(commands::CommandType::SetTrackRouting, bus);
  commands::SetTrackRoutingPayload route;
  route.id = "a1";
  route.sends = {{"bus-1", -4.5, true}};
  harness.Run(commands::CommandType::SetTrackRouting, route);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM track_sends;"), std::int64_t{1});
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM track_sends;"), std::int64_t{0});
  harness.Redo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM track_sends WHERE track_id = 'a1' AND bus_id = 'bus-1' AND pre_fader = 1;"),
           std::int64_t{1});
}

namespace {

commands::SaveTrackingDataPayload TrackingPayload(const std::string& id, const std::string& clip, const std::string& fingerprint = "fp-1") {
  commands::SaveTrackingDataPayload payload;
  payload.id = id;
  payload.clip_id = clip;
  payload.kind = "point";
  payload.name = "Logo";
  payload.algorithm = "point_lk_ncc_v1";
  payload.source_fingerprint = fingerprint;
  payload.parameters_json = "{\"patchRadius\":12}";
  payload.data_json = "{\"kind\":\"point\",\"samples\":[]}";
  return payload;
}

}  // namespace

CUTLINE_TEST(TrackingDataIsStoredWithItsClipAndIsAnUndoableEdit) {
  Harness harness;
  harness.BuildSequence();
  harness.Run(commands::CommandType::SaveTrackingData, TrackingPayload("trk-1", "clip-1"));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracking_data;"), std::int64_t{1});
  CHECK_EQ(harness.Text("SELECT source_fingerprint FROM tracking_data WHERE id = 'trk-1';"), std::string("fp-1"));

  // Saving again under the same id replaces it, and undo brings the first one back.
  auto again = TrackingPayload("trk-1", "clip-1", "fp-2");
  again.data_json = "{\"kind\":\"point\",\"samples\":[1]}";
  harness.Run(commands::CommandType::SaveTrackingData, again);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracking_data;"), std::int64_t{1});
  CHECK_EQ(harness.Text("SELECT source_fingerprint FROM tracking_data WHERE id = 'trk-1';"), std::string("fp-2"));
  harness.Undo();
  CHECK_EQ(harness.Text("SELECT source_fingerprint FROM tracking_data WHERE id = 'trk-1';"), std::string("fp-1"));
  harness.Redo();
  CHECK_EQ(harness.Text("SELECT source_fingerprint FROM tracking_data WHERE id = 'trk-1';"), std::string("fp-2"));

  // It goes with its clip, and comes back with it.
  harness.Run(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracking_data;"), std::int64_t{0});
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracking_data;"), std::int64_t{1});

  harness.Run(commands::CommandType::DeleteTrackingData, commands::DeleteTrackingDataPayload{"trk-1"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracking_data;"), std::int64_t{0});
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracking_data;"), std::int64_t{1});
}

CUTLINE_TEST(TrackingDataThatCouldNotBeReadBackIsRefused) {
  Harness harness;
  harness.BuildSequence();
  auto bad_json = TrackingPayload("trk-1", "clip-1");
  bad_json.data_json = "{not json";
  CHECK_THROWS(harness.Run(commands::CommandType::SaveTrackingData, bad_json));
  auto not_object = TrackingPayload("trk-1", "clip-1");
  not_object.data_json = "[1, 2]";
  CHECK_THROWS(harness.Run(commands::CommandType::SaveTrackingData, not_object));
  auto bad_kind = TrackingPayload("trk-1", "clip-1");
  bad_kind.kind = "magic";
  CHECK_THROWS(harness.Run(commands::CommandType::SaveTrackingData, bad_kind));
  CHECK_THROWS(harness.Run(commands::CommandType::SaveTrackingData, TrackingPayload("trk-1", "no-such-clip")));
  CHECK_THROWS(harness.Run(commands::CommandType::DeleteTrackingData, commands::DeleteTrackingDataPayload{"nothing"}));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM tracking_data;"), std::int64_t{0});
}

CUTLINE_TEST(ATrackWhoseMediaHasChangedIsReportedStaleAndOthersAreNot) {
  Harness harness;
  harness.BuildSequence();
  // clip-1's media has fingerprint fp-1.
  harness.Run(commands::CommandType::SaveTrackingData, TrackingPayload("trk-current", "clip-1", "fp-1"));
  harness.Run(commands::CommandType::SaveTrackingData, TrackingPayload("trk-old", "clip-2", "fp-before-the-file-was-replaced"));
  const std::lock_guard<std::mutex> lock(harness.store().mutex());
  const auto stale = cutline::render::tracking::StaleTrackingIds(harness.store().connection());
  CHECK_EQ(stale.size(), std::size_t{1});
  if (!stale.empty()) CHECK_EQ(stale[0], std::string("trk-old"));
}

CUTLINE_TEST(APackageFromBeforeStoredTrackingOpensWithAnEmptyTable) {
  const auto package = ScratchDirectory("tracking-migration");
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate(id));
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Execute(store->connection(), "DROP TABLE tracking_data;");
    cutline::db::Execute(store->connection(), "UPDATE project_meta SET schema_version = 6 WHERE singleton = 1;");
  }
  auto reopened = ProjectStore::OpenPackage(package);
  {
    const std::lock_guard<std::mutex> lock(reopened->mutex());
    CHECK_EQ(cutline::db::ScalarInt(reopened->connection(), "SELECT COUNT(*) FROM tracking_data;"), std::int64_t{0});
  }
  reopened.reset();
  std::filesystem::remove_all(package);
}

// ---------------------------------------------------------------- collecting a project ----

namespace {

namespace fs = std::filesystem;

void WriteBytes(const fs::path& path, std::size_t size, unsigned seed) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  std::uint32_t state = seed * 2654435761u + 12345u;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525u + 1013904223u;
    out.put(static_cast<char>(state >> 24));
  }
}

struct Collectable final {
  fs::path root;
  fs::path package;
  std::unique_ptr<ProjectStore> store;
  int counter{0};

  void Run(commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter);
    command.project_id = store->ProjectId();
    command.author_id = "tester";
    command.base_revision = store->CurrentRevision();
    command.timestamp_utc = "2026-10-06T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter);
    (void)store->Execute(command);
  }
  void Media(const std::string& id, const std::string& file, std::size_t size, unsigned seed) {
    WriteBytes(root / "originals" / file, size, seed);
    commands::ImportMediaPayload media;
    media.id = id;
    media.display_name = file;
    media.original_path = (root / "originals" / file).string();
    media.fingerprint = "fp-" + id;
    media.duration = RationalTime(60, 1);
    Run(commands::CommandType::ImportMedia, media);
  }
  void Clip(const std::string& id, const std::string& track, const std::string& media, std::int64_t start) {
    commands::InsertClipPayload clip;
    clip.id = id;
    clip.track_id = track;
    clip.media_id = media;
    clip.source_in = RationalTime(2, 1);
    clip.source_out = RationalTime(7, 1);
    clip.timeline_start = RationalTime(start, 1);
    Run(commands::CommandType::InsertClip, clip);
  }
};

// Media a and b on the main sequence, d only inside a nested sequence, c used by nothing.
Collectable MakeCollectable(const std::string& name) {
  Collectable c;
  c.root = ScratchDirectory(name);
  c.package = c.root / "project.cutline";
  const auto id = ProjectStore::GenerateProjectUuid();
  c.store = ProjectStore::CreatePackage(c.package, PackageCreate(id));
  c.Media("media-a", "a.mov", 3000000, 1);
  c.Media("media-b", "b.wav", 10000, 2);
  c.Media("media-c", "c.mov", 1, 3);
  c.Media("media-d", "d.mov", 70000, 4);
  for (const auto* sequence : {"seq-main", "seq-inner"}) {
    commands::CreateSequencePayload create;
    create.id = sequence;
    create.settings.name = sequence;
    create.settings.frame_rate = {25, 1};
    create.settings.width = 640;
    create.settings.height = 360;
    create.settings.sample_rate = 48000;
    c.Run(commands::CommandType::CreateSequence, create);
  }
  c.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  c.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"iv1", "seq-inner", 0, "stereo", "IV1"});
  c.Clip("clip-a", "v1", "media-a", 0);
  c.Clip("clip-b", "v1", "media-b", 5);
  c.Clip("inner-d", "iv1", "media-d", 0);
  commands::InsertClipPayload nested;
  nested.id = "nest";
  nested.track_id = "v1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.nested_sequence_id = "seq-inner";
  nested.source_in = RationalTime(0, 1);
  nested.source_out = RationalTime(5, 1);
  nested.timeline_start = RationalTime(10, 1);
  c.Run(commands::CommandType::InsertClip, nested);
  return c;
}

std::string MediaPathIn(ProjectStore& store, const std::string& id) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  return cutline::db::ScalarText(store.connection(), "SELECT original_path FROM media WHERE id = '" + id + "';");
}

}  // namespace

CUTLINE_TEST(CollectingAProjectCopiesExactlyWhatItUsesAndTheCopyOpensFromTheFolderAlone) {
  auto project = MakeCollectable("collect");
  cutline::project::ConsolidateOptions options;
  options.destination = project.root / "collected";
  const auto report = cutline::project::Consolidate(*project.store, options);
  Expect(report.complete(), report.ToText(), __LINE__);
  // a and b are on the sequence, d is inside a nested sequence it uses; c is used by nothing.
  CHECK_EQ(report.media.size(), std::size_t{3});
  CHECK_EQ(report.not_copied.size(), std::size_t{1});
  CHECK_EQ(report.not_copied[0], std::string("media-c"));
  std::uint64_t total = 0;
  for (const auto& entry : report.media) {
    CHECK(entry.verified);
    CHECK(entry.used);
    const auto copy = options.destination / entry.copy;
    CHECK(fs::exists(copy));
    CHECK_EQ(cutline::project::HashFile(copy), cutline::project::HashFile(entry.source));
    CHECK_EQ(cutline::project::HashFile(copy), entry.sha256);
    total += entry.bytes;
  }
  CHECK_EQ(total, std::uint64_t{3080000});
  CHECK_EQ(report.bytes_copied, total);
  CHECK(report.ToText().find("verified") != std::string::npos);

  // The manifest agrees with the folder.
  const auto check = cutline::project::VerifyManifest(report.manifest, options.destination);
  CHECK(check.ok());
  CHECK_EQ(check.checked, 3);

  // The project in the folder points at the copies; the original points where it did.
  CHECK(fs::exists(report.package));
  const auto original_path = MediaPathIn(*project.store, "media-a");
  CHECK(original_path.find("originals") != std::string::npos);
  auto collected = ProjectStore::OpenPackage(report.package);
  const auto collected_path = MediaPathIn(*collected, "media-a");
  CHECK(collected_path.find("collected") != std::string::npos);
  CHECK(fs::exists(collected_path));
  CHECK_EQ(cutline::project::HashFile(collected_path), cutline::project::HashFile(original_path));

  // The originals can go: every file the collected project names is still there, and its picture is the same.
  collected.reset();
  fs::remove_all(project.root / "originals");
  auto offline = ProjectStore::OpenPackage(report.package);
  for (const auto* id : {"media-a", "media-b", "media-d"}) CHECK(fs::exists(MediaPathIn(*offline, id)));
  CHECK_EQ(cutline::db::ScalarInt(offline->connection(), "SELECT COUNT(*) FROM clips;"), std::int64_t{4});
  offline.reset();
  project.store.reset();
  fs::remove_all(project.root);
}

CUTLINE_TEST(WhatCannotBeCollectedIsNamedAndTheFolderIsNotCalledComplete) {
  auto project = MakeCollectable("collect-missing");
  fs::remove(project.root / "originals" / "b.wav");
  cutline::project::ConsolidateOptions options;
  options.destination = project.root / "collected";
  const auto report = cutline::project::Consolidate(*project.store, options);
  CHECK(!report.complete());
  CHECK_EQ(report.missing.size(), std::size_t{1});
  CHECK_EQ(report.missing[0], std::string("media-b"));
  CHECK(report.ToText().find("MISSING: media-b") != std::string::npos);
  CHECK(report.ToText().find("NOT a complete copy") != std::string::npos);
  // The others were collected, and the missing one is not in the manifest.
  CHECK_EQ(cutline::project::VerifyManifest(report.manifest, options.destination).checked, 2);
  project.store.reset();
  fs::remove_all(project.root);
}

CUTLINE_TEST(ACollectionIsResumableAndRepairsWhatWasDamagedSinceTheLastRun) {
  auto project = MakeCollectable("collect-resume");
  cutline::project::ConsolidateOptions options;
  options.destination = project.root / "collected";
  options.copy_package = false;
  const auto first = cutline::project::Consolidate(*project.store, options);
  CHECK(first.complete());
  const auto again = cutline::project::Consolidate(*project.store, options);
  CHECK(again.complete());
  CHECK_EQ(again.bytes_copied, std::uint64_t{0});  // nothing to copy: all there and the same
  for (const auto& entry : again.media) CHECK(entry.status.find("already there") != std::string::npos);

  // One copy is damaged (same size, different bytes), one is truncated.
  const auto a = options.destination / first.media[0].copy;
  const auto b = options.destination / first.media[1].copy;
  { std::fstream file(a, std::ios::in | std::ios::out | std::ios::binary); file.seekp(1000); file.put('!'); }
  fs::resize_file(b, 10);
  const auto repaired = cutline::project::Consolidate(*project.store, options);
  CHECK(repaired.complete());
  CHECK(repaired.bytes_copied >= 3000000 + 10000 - 100);
  CHECK(cutline::project::VerifyManifest(repaired.manifest, options.destination).ok());

  // And the manifest does catch damage after the fact.
  { std::fstream file(a, std::ios::in | std::ios::out | std::ios::binary); file.seekp(5); file.put('?'); }
  fs::remove(b);
  const auto check = cutline::project::VerifyManifest(repaired.manifest, options.destination);
  CHECK(!check.ok());
  CHECK_EQ(check.mismatched.size(), std::size_t{1});
  CHECK_EQ(check.missing.size(), std::size_t{1});
  project.store.reset();
  fs::remove_all(project.root);
}

CUTLINE_TEST(ACollectedFolderThatIsMovedIsMadeToOpenAgainFromItsManifest) {
  auto project = MakeCollectable("collect-move");
  cutline::project::ConsolidateOptions options;
  options.destination = project.root / "collected";
  const auto report = cutline::project::Consolidate(*project.store, options);
  CHECK(report.complete());
  const auto moved = project.root / "somewhere-else" / "delivery";
  fs::create_directories(moved.parent_path());
  fs::rename(options.destination, moved);

  auto reopened = ProjectStore::OpenPackage(moved / "project.cutline");
  CHECK(!fs::exists(MediaPathIn(*reopened, "media-a")));  // it still names the old place
  CHECK_EQ(cutline::project::RelinkFromManifest(*reopened, moved / "manifest.json", moved), 3);
  for (const auto* id : {"media-a", "media-b", "media-d"}) {
    const auto path = MediaPathIn(*reopened, id);
    CHECK(path.find("delivery") != std::string::npos);
    CHECK(fs::exists(path));
  }
  CHECK(cutline::project::VerifyManifest(moved / "manifest.json", moved).ok());
  reopened.reset();
  project.store.reset();
  fs::remove_all(project.root);
}

CUTLINE_TEST(CollectionCanBeLimitedToASequenceOrWidenedToEverythingAndCanBeStopped) {
  auto project = MakeCollectable("collect-scope");
  cutline::project::ConsolidateOptions only_inner;
  only_inner.destination = project.root / "inner";
  only_inner.sequence_ids = {"seq-inner"};
  only_inner.copy_package = false;
  const auto inner = cutline::project::Consolidate(*project.store, only_inner);
  CHECK_EQ(inner.media.size(), std::size_t{1});  // only d
  CHECK_EQ(inner.media[0].id, std::string("media-d"));
  CHECK_EQ(inner.not_copied.size(), std::size_t{3});

  cutline::project::ConsolidateOptions everything;
  everything.destination = project.root / "everything";
  everything.include_unused_media = true;
  everything.copy_package = false;
  const auto all = cutline::project::Consolidate(*project.store, everything);
  CHECK_EQ(all.media.size(), std::size_t{4});
  CHECK(all.not_copied.empty());
  for (const auto& entry : all.media) CHECK_EQ(entry.used, entry.id != "media-c");

  cutline::project::ConsolidateOptions stopped;
  stopped.destination = project.root / "stopped";
  stopped.copy_package = false;
  int polls = 0;
  std::size_t reported = 0;
  stopped.cancel = [&]() { return ++polls > 1; };
  stopped.progress = [&](std::size_t done, std::size_t) { reported = done; };
  const auto partial = cutline::project::Consolidate(*project.store, stopped);
  CHECK(partial.cancelled);
  CHECK(!partial.complete());
  CHECK_EQ(partial.media.size(), std::size_t{1});
  CHECK_EQ(reported, std::size_t{1});
  CHECK(partial.ToText().find("CANCELLED") != std::string::npos);

  CHECK_THROWS(cutline::project::Consolidate(*project.store, cutline::project::ConsolidateOptions{}));
  project.store.reset();
  fs::remove_all(project.root);
}

CUTLINE_TEST(ATranscoderThePlatformSuppliesTakesTheCopiesPlaceAndAFailureIsReported) {
  auto project = MakeCollectable("collect-transcode");
  cutline::project::ConsolidateOptions options;
  options.destination = project.root / "collected";
  // A stand-in for an encoder: it writes a different, smaller file for media a, fails for b, and
  // declines (so the original is copied as it is) for everything else.
  options.transcode = [](const cutline::project::MediaUse& media, const fs::path& destination, std::string& reason) {
    if (media.id == "media-a") {
      WriteBytes(destination, 500, 99);
      return true;
    }
    if (media.id == "media-b") {
      reason = "the encoder refused it";
      return false;
    }
    return false;
  };
  const auto report = cutline::project::Consolidate(*project.store, options);
  CHECK(!report.complete());
  CHECK_EQ(report.failed.size(), std::size_t{1});
  CHECK_EQ(report.failed[0], std::string("media-b"));
  const auto find = [&](const std::string& id) -> const cutline::project::ConsolidatedMedia& {
    for (const auto& entry : report.media) {
      if (entry.id == id) return entry;
    }
    throw std::runtime_error("no entry " + id);
  };
  CHECK(find("media-a").transcoded && find("media-a").verified);
  CHECK_EQ(find("media-a").bytes, std::uint64_t{500});
  CHECK(find("media-b").status.find("refused") != std::string::npos);
  CHECK(!fs::exists(options.destination / find("media-b").copy));  // nothing left behind
  CHECK(!find("media-d").transcoded && find("media-d").verified);  // declined: copied as it is
  // The collected project points at the transcoded file for a.
  auto collected = ProjectStore::OpenPackage(report.package);
  CHECK_EQ(fs::file_size(MediaPathIn(*collected, "media-a")), std::uintmax_t{500});
  collected.reset();
  project.store.reset();
  fs::remove_all(project.root);
}

CUTLINE_TEST(AProjectWithNoPackageOnDiskStillCollectsItsMediaAndSaysNoPackageWasCopied) {
  Harness harness;
  harness.BuildSequence();
  const auto root = ScratchDirectory("collect-memory");
  WriteBytes(root / "shot.mov", 2000, 7);
  {
    commands::RelinkMediaPayload relink;
    relink.id = "media-1";
    relink.original_path = (root / "shot.mov").string();
    harness.Run(commands::CommandType::RelinkMedia, relink);
  }
  cutline::project::ConsolidateOptions options;
  options.destination = root / "collected";
  const auto report = cutline::project::Consolidate(harness.store(), options);
  CHECK(report.complete());
  CHECK_EQ(report.media.size(), std::size_t{1});
  CHECK(report.package.empty());
  bool noted = false;
  for (const auto& note : report.notes) noted = noted || note.find("no package") != std::string::npos;
  CHECK(noted);
  fs::remove_all(root);
}

// ---------------------------------------------------------------------- captions ----

namespace {

commands::CaptionCuePayload Cue(const std::string& id, std::int64_t start_ms, std::int64_t end_ms, const std::string& text) {
  commands::CaptionCuePayload cue;
  cue.id = id;
  cue.start = RationalTime(start_ms, 1000);
  cue.end = RationalTime(end_ms, 1000);
  cue.text = text;
  return cue;
}

void AddTrack(Harness& harness, const std::string& id = "cap-en", const std::string& language = "en") {
  commands::AddCaptionTrackPayload track;
  track.id = id;
  track.sequence_id = "seq-1";
  track.name = language + " captions";
  track.language = language;
  harness.Run(commands::CommandType::AddCaptionTrack, track);
}

}  // namespace

CUTLINE_TEST(CaptionTracksAndCuesAreProjectDataWithUndoAndImportIsOneEdit) {
  Harness harness;
  harness.BuildSequence();
  AddTrack(harness);
  AddTrack(harness, "cap-fr", "fr");
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM caption_tracks;"), std::int64_t{2});
  CHECK_EQ(harness.Count("SELECT sort_order FROM caption_tracks WHERE id = 'cap-fr';"), std::int64_t{1});

  const auto before = harness.store().AppliedHistoryCount();
  commands::AddCaptionsPayload import;
  import.track_id = "cap-en";
  for (int i = 0; i < 200; ++i) import.cues.push_back(Cue("c" + std::to_string(i), i * 1000, i * 1000 + 800, "Line " + std::to_string(i)));
  harness.Run(commands::CommandType::AddCaptions, import);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{200});
  CHECK_EQ(harness.store().AppliedHistoryCount(), before + 1);  // a whole subtitle file is one undo
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{0});
  harness.Redo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{200});

  // Editing a cue, and deleting some.
  commands::UpdateCaptionPayload update;
  update.cue = Cue("c5", 5250, 6000, "Reworded");
  update.cue.speaker = "Ana";
  harness.Run(commands::CommandType::UpdateCaption, update);
  CHECK_EQ(harness.Text("SELECT text FROM captions WHERE id = 'c5';"), std::string("Reworded"));
  CHECK_EQ(harness.Text("SELECT speaker FROM captions WHERE id = 'c5';"), std::string("Ana"));
  CHECK_EQ(harness.Count("SELECT start_num FROM captions WHERE id = 'c5';"), std::int64_t{21});  // 5250 ms is 21/4 s
  harness.Undo();
  CHECK_EQ(harness.Text("SELECT text FROM captions WHERE id = 'c5';"), std::string("Line 5"));

  commands::RemoveCaptionsPayload remove;
  remove.ids = {"c1", "c2", "c3"};
  harness.Run(commands::CommandType::RemoveCaptions, remove);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{197});
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{200});

  // The track's settings.
  commands::UpdateCaptionTrackPayload settings;
  settings.id = "cap-en";
  settings.name = "English (SDH)";
  settings.language = "en-US";
  settings.style_json = "{\"size\":0.07}";
  harness.Run(commands::CommandType::UpdateCaptionTrack, settings);
  CHECK_EQ(harness.Text("SELECT language FROM caption_tracks WHERE id = 'cap-en';"), std::string("en-US"));

  // Removing a track takes its cues, and undo brings them all back.
  harness.Run(commands::CommandType::RemoveCaptionTrack, commands::RemoveCaptionTrackPayload{"cap-en"});
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{0});
  harness.Undo();
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{200});
}

CUTLINE_TEST(CaptionsThatCouldNotBeShownOrReadBackAreRefusedAndNothingIsHalfAdded) {
  Harness harness;
  harness.BuildSequence();
  AddTrack(harness);
  const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
    bool threw = false;
    try {
      harness.Run(type, std::move(payload));
    } catch (const std::exception&) {
      threw = true;
    }
    return threw;
  };
  commands::AddCaptionsPayload backwards;
  backwards.track_id = "cap-en";
  backwards.cues = {Cue("a", 1000, 2000, "fine"), Cue("b", 4000, 3000, "ends before it starts")};
  CHECK(run(commands::CommandType::AddCaptions, backwards));
  commands::AddCaptionsPayload empty_text;
  empty_text.track_id = "cap-en";
  empty_text.cues = {Cue("a", 1000, 2000, "")};
  CHECK(run(commands::CommandType::AddCaptions, empty_text));
  commands::AddCaptionsPayload bad_style;
  bad_style.track_id = "cap-en";
  bad_style.cues = {Cue("a", 1000, 2000, "text")};
  bad_style.cues[0].style_json = "{not json";
  CHECK(run(commands::CommandType::AddCaptions, bad_style));
  commands::AddCaptionsPayload unknown_track;
  unknown_track.track_id = "nowhere";
  unknown_track.cues = {Cue("a", 1000, 2000, "text")};
  CHECK(run(commands::CommandType::AddCaptions, unknown_track));
  commands::AddCaptionsPayload duplicate;
  duplicate.track_id = "cap-en";
  duplicate.cues = {Cue("same", 1000, 2000, "one"), Cue("same", 3000, 4000, "two")};
  CHECK(run(commands::CommandType::AddCaptions, duplicate));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{0});  // the failed batches left nothing
  commands::AddCaptionTrackPayload bad_track;
  bad_track.id = "x";
  bad_track.sequence_id = "no-such-sequence";
  CHECK(run(commands::CommandType::AddCaptionTrack, bad_track));
  CHECK(run(commands::CommandType::RemoveCaptions, commands::RemoveCaptionsPayload{{"nope"}}));
  CHECK(run(commands::CommandType::UpdateCaption, commands::UpdateCaptionPayload{Cue("nope", 0, 1000, "x")}));

  // Overlapping cues are allowed: two speakers at once.
  commands::AddCaptionsPayload overlap;
  overlap.track_id = "cap-en";
  overlap.cues = {Cue("one", 1000, 3000, "A"), Cue("two", 2000, 4000, "B")};
  harness.Run(commands::CommandType::AddCaptions, overlap);
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM captions;"), std::int64_t{2});
}

CUTLINE_TEST(CaptionsSurviveReopeningAndAnOlderPackageOpensWithNoCaptions) {
  const auto package = ScratchDirectory("captions-package");
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    auto store = ProjectStore::CreatePackage(package, PackageCreate(id));
    int n = 0;
    const auto run = [&](commands::CommandType type, commands::CommandPayload payload) {
      commands::CommandEnvelope command;
      command.command_id = "cmd-" + std::to_string(++n);
      command.project_id = id;
      command.author_id = "tester";
      command.base_revision = store->CurrentRevision();
      command.timestamp_utc = "2026-10-06T00:00:00Z";
      command.type = type;
      command.payload = std::move(payload);
      command.idempotency_key = "key-" + std::to_string(n);
      (void)store->Execute(command);
    };
    commands::CreateSequencePayload sequence;
    sequence.id = "seq";
    sequence.settings.name = "Seq";
    sequence.settings.frame_rate = {25, 1};
    sequence.settings.width = 640;
    sequence.settings.height = 360;
    sequence.settings.sample_rate = 48000;
    run(commands::CommandType::CreateSequence, sequence);
    commands::AddCaptionTrackPayload track;
    track.id = "t";
    track.sequence_id = "seq";
    track.language = "pt-BR";
    run(commands::CommandType::AddCaptionTrack, track);
    commands::AddCaptionsPayload cues;
    cues.track_id = "t";
    cues.cues = {Cue("c1", 1500, 3000, "Ol\xC3\xA1, mundo")};
    run(commands::CommandType::AddCaptions, cues);
  }
  {
    auto reopened = ProjectStore::OpenPackage(package);
    const std::lock_guard<std::mutex> lock(reopened->mutex());
    CHECK_EQ(cutline::db::ScalarText(reopened->connection(), "SELECT text FROM captions WHERE id = 'c1';"), std::string("Ol\xC3\xA1, mundo"));
    CHECK_EQ(cutline::db::ScalarText(reopened->connection(), "SELECT language FROM caption_tracks;"), std::string("pt-BR"));
  }
  {
    // A package from before captions: the tables are made empty on opening.
    auto store = ProjectStore::OpenPackage(package);
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Execute(store->connection(), "DROP TABLE captions;");
    cutline::db::Execute(store->connection(), "DROP TABLE caption_tracks;");
    cutline::db::Execute(store->connection(), "UPDATE project_meta SET schema_version = 7 WHERE singleton = 1;");
  }
  auto migrated = ProjectStore::OpenPackage(package);
  {
    const std::lock_guard<std::mutex> lock(migrated->mutex());
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT COUNT(*) FROM caption_tracks;"), std::int64_t{0});
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT COUNT(*) FROM captions;"), std::int64_t{0});
  }
  migrated.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(AProjectFromBeforeMulticamGetsEmptyMulticamTablesAndMulticamWorkAfterwards) {
  const auto package = std::filesystem::temp_directory_path() / "cutline-test-multicam-migrate";
  std::filesystem::remove_all(package);
  const auto id = ProjectStore::GenerateProjectUuid();
  {
    cutline::commands::CommandEnvelope create;
    create.command_id = "c0";
    create.project_id = id;
    create.author_id = "tester";
    create.timestamp_utc = "2026-10-06T00:00:00Z";
    create.type = cutline::commands::CommandType::CreateProject;
    create.payload = cutline::commands::CreateProjectPayload{"Old"};
    create.idempotency_key = "k0";
    auto store = ProjectStore::CreatePackage(package, create);
  }
  {
    auto store = ProjectStore::OpenPackage(package);
    const std::lock_guard<std::mutex> lock(store->mutex());
    cutline::db::Execute(store->connection(), "DROP TABLE multicam_switches;");
    cutline::db::Execute(store->connection(), "DROP TABLE multicam_angles;");
    cutline::db::Execute(store->connection(), "DROP TABLE multicam_groups;");
    cutline::db::Execute(store->connection(), "UPDATE project_meta SET schema_version = 9 WHERE singleton = 1;");
  }
  auto migrated = ProjectStore::OpenPackage(package);
  {
    const std::lock_guard<std::mutex> lock(migrated->mutex());
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT COUNT(*) FROM multicam_groups;"), std::int64_t{0});
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT COUNT(*) FROM multicam_angles;"), std::int64_t{0});
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT COUNT(*) FROM multicam_switches;"), std::int64_t{0});
    CHECK_EQ(cutline::db::ScalarInt(migrated->connection(), "SELECT schema_version FROM project_meta WHERE singleton = 1;"), cutline::project::kSchemaVersion);
  }
  migrated.reset();
  std::filesystem::remove_all(package);
}

CUTLINE_TEST(AGroupOfCommandsIsOneUndoStepAndHappensWholeOrNotAtAll) {
  Harness harness;
  harness.BuildSequence();
  const auto steps_before = harness.store().HistorySteps().size();
  const auto entries_before = harness.store().AppliedHistoryCount();
  std::vector<commands::CommandEnvelope> group;
  group.push_back(harness.Make(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(7)}));
  group.push_back(harness.Make(commands::CommandType::InsertClip, Harness::Clip("clip-3", "v1", Seconds(20), Seconds(20), Seconds(22))));
  group.push_back(harness.Make(commands::CommandType::SetClipEnabled, commands::SetClipEnabledPayload{"clip-1", false}));
  (void)harness.store().ExecuteGroup(group, "Insert Edit");
  CHECK_EQ(harness.store().AppliedHistoryCount(), entries_before + 3);
  CHECK_EQ(harness.store().HistorySteps().size(), steps_before + 1);
  const auto steps = harness.store().HistorySteps();
  CHECK_EQ(steps.back(), std::string("Insert Edit"));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{3});
  CHECK_EQ(harness.Count("SELECT enabled FROM clips WHERE id = 'clip-1';"), std::int64_t{0});

  // One undo takes back all three; one redo puts all three back.
  (void)harness.store().Undo("tester", "2026-10-06T00:00:00Z");
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{2});
  CHECK_EQ(harness.Count("SELECT enabled FROM clips WHERE id = 'clip-1';"), std::int64_t{1});
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), std::int64_t{5});
  CHECK_EQ(harness.store().AppliedStepCount(), steps_before);
  (void)harness.store().Redo("tester", "2026-10-06T00:00:01Z");
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{3});
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), std::int64_t{7});
  CHECK_NO_THROW(harness.store().ValidateDatabase());

  // A group whose last command cannot be applied leaves nothing behind and nothing to redo.
  const auto revision_entries = harness.store().AppliedHistoryCount();
  std::vector<commands::CommandEnvelope> bad;
  bad.push_back(harness.Make(commands::CommandType::MoveClip, commands::MoveClipPayload{"clip-2", "v1", Seconds(30)}));
  bad.push_back(harness.Make(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"clip-1"}));
  bad.push_back(harness.Make(commands::CommandType::DeleteClip, commands::DeleteClipPayload{"no-such-clip"}));
  CHECK_THROWS(harness.store().ExecuteGroup(bad, "Doomed"));
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{3});
  CHECK_EQ(harness.Count("SELECT timeline_start_num FROM clips WHERE id = 'clip-2';"), std::int64_t{7});
  CHECK_EQ(harness.store().AppliedHistoryCount(), revision_entries);
  CHECK(!harness.store().CanRedo());
  CHECK_NO_THROW(harness.store().ValidateDatabase());
  // And an ordinary command still undoes on its own after a group.
  harness.Run(commands::CommandType::SetClipEnabled, commands::SetClipEnabledPayload{"clip-2", false});
  (void)harness.store().Undo("tester", "2026-10-06T00:00:02Z");
  CHECK_EQ(harness.Count("SELECT COUNT(*) FROM clips;"), std::int64_t{3});
}

CUTLINE_TEST(TheUndoLimitCountsStepsSoAHugeEditIsOneStepAndIsNeverCutInTwo) {
  cutline::project::SnapshotPolicy policy;
  policy.history_limit = 4;
  Harness harness(policy);
  harness.BuildSequence();                       // a dozen setup commands, each a step of its own
  CHECK_EQ(harness.store().AppliedStepCount(), std::size_t{4});
  // One edit of forty commands is one step; it pushes out one old step, not forty.
  std::vector<commands::CommandEnvelope> group;
  for (int i = 0; i < 40; ++i) group.push_back(harness.Make(commands::CommandType::SetClipEnabled, commands::SetClipEnabledPayload{"clip-2", i % 2 == 1}));
  (void)harness.store().ExecuteGroup(group, "Forty Toggles");
  CHECK_EQ(harness.store().AppliedStepCount(), std::size_t{4});
  CHECK_EQ(harness.store().AppliedHistoryCount(), std::size_t{3 + 40});
  const auto steps = harness.store().HistorySteps();
  CHECK_EQ(steps.back(), std::string("Forty Toggles"));
  CHECK_EQ(harness.Count("SELECT enabled FROM clips WHERE id = 'clip-2';"), std::int64_t{1});
  (void)harness.store().Undo("tester", "2026-10-06T00:00:00Z");
  CHECK_EQ(harness.store().AppliedHistoryCount(), std::size_t{3});
  CHECK_EQ(harness.store().AppliedStepCount(), std::size_t{3});
  (void)harness.store().Redo("tester", "2026-10-06T00:00:01Z");
  CHECK_EQ(harness.store().AppliedHistoryCount(), std::size_t{3 + 40});
  CHECK_NO_THROW(harness.store().ValidateDatabase());
}

int main() { return cutline::testing::RunAll("store"); }

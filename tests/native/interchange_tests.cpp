// Timeline interchange: the neutral model, the OpenTimelineIO file format, and what is
// reported when a file says something Cutline cannot.

#include "core/commands/Command.h"
#include "core/project/ProjectStore.h"
#include "captions/Captions.h"
#include "core/util/XmlParse.h"
#include "interchange/Edl.h"
#include "interchange/FcpXml.h"
#include "interchange/Interchange.h"
#include "interchange/Otio.h"
#include "tests/native/TestHarness.h"
#include "timeline/SequenceLoader.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace commands = cutline::commands;
namespace model = cutline::model;
namespace ic = cutline::interchange;
using cutline::anim::Interpolation;
using cutline::anim::Value;
using cutline::project::ProjectStore;
using cutline::time::RationalTime;

namespace {

void Expect(bool condition, const std::string& description, int line) {
  if (!condition) cutline::testing::Fail("interchange", __FILE__, line, description);
}

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

// A project driven through the command service.
class Project final {
 public:
  explicit Project(cutline::time::FrameRate rate = cutline::time::kFrameRate25) : store_(std::make_unique<ProjectStore>(":memory:")) {
    store_->Initialize();
    Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Interchange"});
    AddMedia("media-a", "a.mov", "/footage/a.mov", 60);
    AddMedia("media-b", "b.mov", "/footage/b.mov", 40);
    commands::CreateSequencePayload sequence;
    sequence.id = "seq-main";
    sequence.settings.name = "Main";
    sequence.settings.frame_rate = rate;
    sequence.settings.width = 1920;
    sequence.settings.height = 1080;
    sequence.settings.sample_rate = 48000;
    Run(commands::CommandType::CreateSequence, sequence);
  }

  explicit Project(std::unique_ptr<ProjectStore> adopted) : store_(std::move(adopted)) {}

  void AddMedia(const std::string& id, const std::string& name, const std::string& path, std::int64_t seconds) {
    commands::ImportMediaPayload media;
    media.id = id;
    media.display_name = name;
    media.original_path = path;
    media.fingerprint = "fp-" + id;
    media.duration = Seconds(seconds);
    Run(commands::CommandType::ImportMedia, media);
  }

  void Run(commands::CommandType type, commands::CommandPayload payload) {
    commands::CommandEnvelope command;
    command.command_id = "cmd-" + std::to_string(++counter_);
    command.project_id = "project-1";
    command.author_id = "tester";
    command.base_revision = store_->CurrentRevision();
    command.timestamp_utc = "2026-10-06T00:00:00Z";
    command.type = type;
    command.payload = std::move(payload);
    command.idempotency_key = "key-" + std::to_string(counter_);
    const auto result = store_->Execute(command);
    (void)result;
  }

  void Clip(const std::string& id, const std::string& track, const std::string& media, std::int64_t start,
            std::int64_t in, std::int64_t out, RationalTime rate = {1, 1}, bool reversed = false) {
    commands::InsertClipPayload clip;
    clip.id = id;
    clip.track_id = track;
    clip.media_id = media;
    clip.source_in = Seconds(in);
    clip.source_out = Seconds(out);
    clip.timeline_start = Seconds(start);
    clip.playback_rate = rate;
    clip.reversed = reversed;
    clip.name = id;
    Run(commands::CommandType::InsertClip, clip);
  }

  [[nodiscard]] ProjectStore& store() { return *store_; }

 private:
  std::unique_ptr<ProjectStore> store_;
  int counter_{0};
};

// An edit that uses most of what the exchange model carries.
Project RichProject() {
  Project project;
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  project.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-main", 0, "stereo", "A1"});
  project.Clip("clip-1", "v1", "media-a", 0, 5, 15);                        // 10 s, from media at 5 s
  project.Clip("clip-2", "v1", "media-b", 10, 2, 8, {2, 1});                // 6 s of source at double speed: 3 s
  project.Clip("clip-3", "v1", "media-a", 20, 30, 34, {1, 1}, true);        // reversed, after a gap
  project.Clip("aud-1", "a1", "media-a", 0, 5, 15);
  project.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1", "aud-1"}, "take-1"});

  commands::AddTransitionPayload dissolve;
  dissolve.id = "tr-1";
  dissolve.track_id = "v1";
  dissolve.kind = "cross_dissolve";
  dissolve.alignment = model::TransitionAlignment::Center;
  dissolve.from_clip_id = "clip-1";
  dissolve.to_clip_id = "clip-2";
  dissolve.timeline_start = RationalTime(9, 1);
  dissolve.duration = Seconds(2);
  project.Run(commands::CommandType::AddTransition, dissolve);

  commands::AddEffectPayload effect;
  effect.id = "fx-1";
  effect.owner_kind = model::EffectOwner::Clip;
  effect.owner_id = "clip-1";
  effect.effect_type = "opacity";
  effect.parameters = {{"fx-1:value", "value", Value::Scalar(1.0)}};
  project.Run(commands::CommandType::AddEffect, effect);
  commands::SetKeyframePayload key;
  key.parameter_id = "fx-1:value";
  key.keyframe = {Seconds(1), Value::Scalar(0.25), Interpolation::Bezier, {0.4, 0.1}, {0.6, 0.9}};
  project.Run(commands::CommandType::SetKeyframe, key);
  key.keyframe = {RationalTime(7, 3), Value::Scalar(0.75), Interpolation::Linear, {}, {}};
  project.Run(commands::CommandType::SetKeyframe, key);

  commands::AddMarkerPayload marker;
  marker.id = "m-1";
  marker.owner_kind = model::MarkerOwner::Sequence;
  marker.owner_id = "seq-main";
  marker.start = Seconds(4);
  marker.end = Seconds(5);
  marker.label = "Beat";
  marker.kind = model::MarkerKind::Chapter;
  marker.color = "red";
  project.Run(commands::CommandType::AddMarker, marker);
  marker.id = "m-2";
  marker.owner_kind = model::MarkerOwner::Clip;
  marker.owner_id = "clip-3";
  marker.start = Seconds(1);
  marker.end = Seconds(1);
  marker.label = "On the clip";
  marker.kind = model::MarkerKind::Comment;
  marker.color = "";
  project.Run(commands::CommandType::AddMarker, marker);
  return project;
}

// Two timelines hold the same edit when every exchanged field agrees, exactly.
std::string Differences(const ic::Timeline& a, const ic::Timeline& b) {
  const auto same = [](const RationalTime& x, const RationalTime& y) { return x.Compare(y) == 0; };
  if (a.sequences.size() != b.sequences.size()) return "sequence count";
  if (a.media.size() != b.media.size()) return "media count";
  for (std::size_t m = 0; m < a.media.size(); ++m) {
    const auto& x = a.media[m];
    const auto& y = b.media[m];
    if (x.id != y.id || x.name != y.name || x.url != y.url || x.fingerprint != y.fingerprint || !same(x.duration, y.duration) ||
        !same(x.start_timecode, y.start_timecode)) {
      return "media " + x.id;
    }
  }
  for (std::size_t s = 0; s < a.sequences.size(); ++s) {
    const auto& x = a.sequences[s];
    const auto& y = b.sequences[s];
    if (x.id != y.id || x.settings.name != y.settings.name || x.settings.frame_rate.numerator != y.settings.frame_rate.numerator ||
        x.settings.frame_rate.denominator != y.settings.frame_rate.denominator || x.settings.width != y.settings.width ||
        x.settings.sample_rate != y.settings.sample_rate) {
      return "sequence " + x.id + " settings";
    }
    if (x.tracks.size() != y.tracks.size()) return "track count in " + x.id;
    if (x.markers.size() != y.markers.size()) return "marker count in " + x.id;
    for (std::size_t k = 0; k < x.markers.size(); ++k) {
      if (x.markers[k].id != y.markers[k].id || x.markers[k].label != y.markers[k].label || !same(x.markers[k].start, y.markers[k].start) ||
          !same(x.markers[k].end, y.markers[k].end) || x.markers[k].kind != y.markers[k].kind || x.markers[k].color != y.markers[k].color) {
        return "marker " + x.markers[k].id;
      }
    }
    if (x.effects.size() != y.effects.size()) return "sequence effect count";
    for (std::size_t t = 0; t < x.tracks.size(); ++t) {
      const auto& p = x.tracks[t];
      const auto& q = y.tracks[t];
      if (p.id != q.id || p.kind != q.kind || p.order != q.order || p.muted != q.muted || p.gain_db != q.gain_db ||
          p.items.size() != q.items.size() || p.transitions.size() != q.transitions.size()) {
        return "track " + p.id;
      }
      for (std::size_t i = 0; i < p.items.size(); ++i) {
        const auto& u = p.items[i];
        const auto& v = q.items[i];
        if (u.id != v.id || u.source_id != v.source_id || u.source_kind != v.source_kind || !same(u.source_in, v.source_in) ||
            !same(u.source_out, v.source_out) || !same(u.timeline_start, v.timeline_start) || !same(u.playback_rate, v.playback_rate) ||
            u.reversed != v.reversed || u.maintain_pitch != v.maintain_pitch || u.enabled != v.enabled ||
            u.linked_group != v.linked_group || u.effects.size() != v.effects.size() || u.markers.size() != v.markers.size()) {
          return "clip " + u.id;
        }
        for (std::size_t e = 0; e < u.effects.size(); ++e) {
          const auto& f = u.effects[e];
          const auto& g = v.effects[e];
          if (f.id != g.id || f.effect_type != g.effect_type || f.parameters.size() != g.parameters.size()) return "effect " + f.id;
          for (std::size_t n = 0; n < f.parameters.size(); ++n) {
            const auto& pa = f.parameters[n].value;
            const auto& pb = g.parameters[n].value;
            if (pa.keyframes().size() != pb.keyframes().size() || !pa.constant().Equals(pb.constant())) return "parameter " + f.parameters[n].name;
            for (std::size_t k = 0; k < pa.keyframes().size(); ++k) {
              const auto& ka = pa.keyframes()[k];
              const auto& kb = pb.keyframes()[k];
              if (!same(ka.time, kb.time) || !ka.value.Equals(kb.value) || ka.interpolation != kb.interpolation ||
                  ka.out_handle.x != kb.out_handle.x || ka.out_handle.y != kb.out_handle.y || ka.in_handle.x != kb.in_handle.x ||
                  ka.in_handle.y != kb.in_handle.y) {
                return "keyframe " + std::to_string(k) + " of " + f.parameters[n].name;
              }
            }
          }
        }
      }
      for (std::size_t i = 0; i < p.transitions.size(); ++i) {
        const auto& u = p.transitions[i];
        const auto& v = q.transitions[i];
        if (u.id != v.id || u.kind != v.kind || u.alignment != v.alignment || u.from_item != v.from_item || u.to_item != v.to_item ||
            !same(u.start, v.start) || !same(u.duration, v.duration)) {
          return "transition " + u.id;
        }
      }
    }
  }
  return {};
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

// ------------------------------------------------------------- model edge ----

CUTLINE_TEST(AProjectIsDescribedByTheNeutralModelWithEverythingItHolds) {
  auto project = RichProject();
  ic::Report report;
  const auto timeline = ic::FromProject(project.store(), "seq-main", report);
  CHECK(!report.Lossy());
  CHECK_EQ(timeline.sequences.size(), std::size_t{1});
  CHECK_EQ(timeline.media.size(), std::size_t{2});
  const auto& sequence = timeline.sequences[0];
  CHECK_EQ(sequence.tracks.size(), std::size_t{2});
  const ic::Track* video = nullptr;
  for (const auto& track : sequence.tracks) {
    if (track.id == "v1") video = &track;
  }
  CHECK(video != nullptr);
  if (video == nullptr) return;
  CHECK_EQ(video->items.size(), std::size_t{3});
  CHECK_EQ(video->transitions.size(), std::size_t{1});
  CHECK(video->items[1].playback_rate.Compare(RationalTime(2, 1)) == 0);
  CHECK(video->items[1].duration().Compare(Seconds(3)) == 0);
  CHECK(video->items[2].reversed);
  CHECK_EQ(video->items[0].effects.size(), std::size_t{1});
  CHECK_EQ(video->items[0].effects[0].parameters[0].value.keyframes().size(), std::size_t{2});
  CHECK_EQ(video->items[2].markers.size(), std::size_t{1});
  CHECK_EQ(sequence.markers.size(), std::size_t{1});
  CHECK_EQ(video->items[0].linked_group, std::string("take-1"));
  const auto* media = timeline.FindMedia("media-a");
  CHECK(media != nullptr);
  if (media != nullptr) CHECK_EQ(media->url, std::string("/footage/a.mov"));
}

CUTLINE_TEST(AudioBusesAndAdjustmentClipsAreReportedNotSilentlyDropped) {
  Project project;
  project.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-main", 0, "stereo", "A1"});
  project.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"bus", "seq-main", 1, "stereo", "Bus"});
  commands::SetTrackRoutingPayload bus;
  bus.id = "bus";
  bus.is_bus = true;
  project.Run(commands::CommandType::SetTrackRouting, bus);
  commands::SetTrackRoutingPayload route;
  route.id = "a1";
  route.output_bus_id = "bus";
  project.Run(commands::CommandType::SetTrackRouting, route);
  ic::Report report;
  const auto timeline = ic::FromProject(project.store(), "seq-main", report);
  CHECK(report.Lossy());
  CHECK(report.Mentions("bus"));
  CHECK_EQ(timeline.sequences[0].tracks.size(), std::size_t{1});
}

// ---------------------------------------------------------------- OpenTimelineIO ----

CUTLINE_TEST(OtioRoundTripsAProjectWithoutChangingAnythingItCarries) {
  auto source = RichProject();
  ic::Report write_report;
  const auto original = ic::FromProject(source.store(), "seq-main", write_report);
  const auto text = ic::WriteOtio(original, write_report);
  CHECK(!text.empty());
  CHECK(!write_report.Lossy());

  // It is OpenTimelineIO to anything that reads it.
  CHECK(Contains(text, "\"OTIO_SCHEMA\":\"Timeline.1\""));
  CHECK(Contains(text, "\"OTIO_SCHEMA\":\"Clip.2\""));
  CHECK(Contains(text, "\"OTIO_SCHEMA\":\"Gap.1\""));
  CHECK(Contains(text, "\"OTIO_SCHEMA\":\"Transition.1\""));
  CHECK(Contains(text, "\"OTIO_SCHEMA\":\"LinearTimeWarp.1\""));
  CHECK(Contains(text, "\"OTIO_SCHEMA\":\"Marker.2\""));

  ic::Report read_report;
  const auto parsed = ic::ReadOtio(text, read_report);
  CHECK(!read_report.Lossy());
  const auto first = Differences(original, parsed);
  Expect(first.empty(), "reading it back differs in: " + first, __LINE__);

  // Into a fresh project, and out again: the same edit, down to the text.
  Project target(std::make_unique<ProjectStore>(":memory:"));
  target.store().Initialize();
  target.Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Target"});
  ic::Report apply_report;
  const auto result = ic::ApplyToProject(target.store(), parsed, {}, apply_report);
  Expect(!apply_report.HasErrors(), apply_report.ToText(), __LINE__);
  CHECK_EQ(result.sequences_created, 1);
  CHECK_EQ(result.clips_created, 4);
  CHECK_EQ(result.transitions_created, 1);
  CHECK_EQ(result.media_created, 2);
  CHECK_EQ(result.markers_created, 2);
  ic::Report again;
  const auto reloaded = ic::FromProject(target.store(), "seq-main", again);
  const auto second = Differences(original, reloaded);
  Expect(second.empty(), "after importing, the project differs in: " + second, __LINE__);
  CHECK(ic::WriteOtio(reloaded, again) == text);

  // And the compiled edit agrees tick for tick.
  const auto before = cutline::timeline::LoadSequenceGraph(source.store(), "seq-main");
  const auto after = cutline::timeline::LoadSequenceGraph(target.store(), "seq-main");
  CHECK_EQ(before.root()->tracks.size(), after.root()->tracks.size());
  for (std::size_t t = 0; t < before.root()->tracks.size(); ++t) {
    const auto& x = before.root()->tracks[t];
    const auto& y = after.root()->tracks[t];
    CHECK_EQ(x.clips.size(), y.clips.size());
    for (std::size_t c = 0; c < x.clips.size(); ++c) {
      CHECK_EQ(x.clips[c].start_ticks, y.clips[c].start_ticks);
      CHECK_EQ(x.clips[c].end_ticks, y.clips[c].end_ticks);
      CHECK(x.clips[c].source_in.Compare(y.clips[c].source_in) == 0);
      CHECK(x.clips[c].source_out.Compare(y.clips[c].source_out) == 0);
      CHECK(x.clips[c].playback_rate.Compare(y.clips[c].playback_rate) == 0);
    }
  }
}

CUTLINE_TEST(TimesFallOnFramesAtTheSequenceRateEvenAtFractionalRates) {
  Project project(cutline::time::kFrameRate23976);
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  // A clip that starts at frame 100 of a 23.976 sequence, and uses frames 240..480 of its media.
  commands::InsertClipPayload clip;
  clip.id = "clip-1";
  clip.track_id = "v1";
  clip.media_id = "media-a";
  clip.source_in = RationalTime::FromFrames(240, cutline::time::kFrameRate23976);
  clip.source_out = RationalTime::FromFrames(480, cutline::time::kFrameRate23976);
  clip.timeline_start = RationalTime::FromFrames(100, cutline::time::kFrameRate23976);
  project.Run(commands::CommandType::InsertClip, clip);

  ic::Report report;
  const auto original = ic::FromProject(project.store(), "seq-main", report);
  const auto text = ic::WriteOtio(original, report);
  // A frame count at the sequence's own rate: 240 frames, rate 24000/1001.
  CHECK(Contains(text, "\"value\":240"));
  CHECK(Contains(text, "\"rate\":23.976023976023978"));
  const auto parsed = ic::ReadOtio(text, report);
  CHECK(!report.Lossy());
  const auto difference = Differences(original, parsed);
  Expect(difference.empty(), "a fractional rate drifted: " + difference, __LINE__);
  CHECK_EQ(parsed.sequences[0].tracks[0].items[0].source_in.numerator(), RationalTime::FromFrames(240, cutline::time::kFrameRate23976).numerator());
}

CUTLINE_TEST(ANestedSequenceTravelsInsideTheClipThatUsesItWithItsOwnEffectsAndSettings) {
  Project project;
  commands::CreateSequencePayload inner;
  inner.id = "seq-inner";
  inner.settings.name = "Inner";
  inner.settings.frame_rate = {30, 1};
  inner.settings.width = 1280;
  inner.settings.height = 720;
  inner.settings.sample_rate = 44100;
  project.Run(commands::CommandType::CreateSequence, inner);
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"iv1", "seq-inner", 0, "stereo", "IV1"});
  project.Clip("inner-clip", "iv1", "media-a", 0, 0, 6);
  commands::AddEffectPayload effect;
  effect.id = "fx-inner";
  effect.owner_kind = model::EffectOwner::Sequence;
  effect.owner_id = "seq-inner";
  effect.effect_type = "blur";
  effect.parameters = {{"fx-inner:radius", "radius", Value::Scalar(3.0)}};
  project.Run(commands::CommandType::AddEffect, effect);

  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  commands::InsertClipPayload nested;
  nested.id = "nest";
  nested.track_id = "v1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.nested_sequence_id = "seq-inner";
  nested.source_in = Seconds(1);
  nested.source_out = Seconds(5);
  nested.timeline_start = Seconds(2);
  nested.name = "Nested";
  project.Run(commands::CommandType::InsertClip, nested);

  ic::Report report;
  const auto original = ic::FromProject(project.store(), "seq-main", report);
  CHECK_EQ(original.sequences.size(), std::size_t{2});
  const auto text = ic::WriteOtio(original, report);
  CHECK(Contains(text, "\"OTIO_SCHEMA\":\"Stack.1\""));
  const auto parsed = ic::ReadOtio(text, report);
  CHECK(!report.Lossy());
  CHECK_EQ(parsed.sequences.size(), std::size_t{2});
  const auto* restored = parsed.FindSequence("seq-inner");
  CHECK(restored != nullptr);
  if (restored != nullptr) {
    CHECK_EQ(restored->settings.width, std::int64_t{1280});
    CHECK_EQ(restored->settings.frame_rate.numerator, std::int64_t{30});
    CHECK_EQ(restored->effects.size(), std::size_t{1});
  }
  const auto difference = Differences(original, parsed);
  Expect(difference.empty(), "the nested sequence differs in: " + difference, __LINE__);

  Project target(std::make_unique<ProjectStore>(":memory:"));
  target.store().Initialize();
  target.Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Target"});
  ic::Report apply;
  const auto result = ic::ApplyToProject(target.store(), parsed, {}, apply);
  Expect(!apply.HasErrors(), apply.ToText(), __LINE__);
  CHECK_EQ(result.sequences_created, 2);
  const auto graph = cutline::timeline::LoadSequenceGraph(target.store(), "seq-main");
  CHECK_EQ(graph.sequences.size(), std::size_t{2});
}

CUTLINE_TEST(AFileFromAnotherToolIsReadFromItsStandardFieldsAndWhatWasAssumedIsSaid) {
  // Two clips at 24 fps with a gap and a dissolve between the second and third, the way
  // another tool writes it: no Cutline metadata at all. The first is played at double speed.
  const std::string file = R"json({
    "OTIO_SCHEMA": "Timeline.1", "name": "Reel 1", "global_start_time": null,
    "metadata": {},
    "tracks": {"OTIO_SCHEMA": "Stack.1", "name": "tracks", "metadata": {}, "markers": [], "effects": [], "source_range": null,
      "children": [
        {"OTIO_SCHEMA": "Track.1", "name": "Video 1", "kind": "Video", "metadata": {}, "markers": [], "effects": [], "source_range": null,
         "children": [
           {"OTIO_SCHEMA": "Clip.2", "name": "Wide", "metadata": {}, "markers": [], "enabled": true,
            "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 48, "rate": 24}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 72, "rate": 24}},
            "effects": [{"OTIO_SCHEMA": "LinearTimeWarp.1", "name": "", "effect_name": "LinearTimeWarp", "time_scalar": 2.0, "metadata": {}},
                        {"OTIO_SCHEMA": "Effect.1", "name": "Sparkle", "effect_name": "Sparkle", "metadata": {}}],
            "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "wide.mov", "target_url": "file:///media/wide.mov", "metadata": {},
              "available_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 24}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 2400, "rate": 24}}}},
           {"OTIO_SCHEMA": "Gap.1", "name": "", "metadata": {}, "markers": [], "effects": [], "enabled": true,
            "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 24}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 24, "rate": 24}}},
           {"OTIO_SCHEMA": "Clip.2", "name": "Close", "metadata": {}, "markers": [], "effects": [], "enabled": true,
            "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 240, "rate": 24}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 96, "rate": 24}},
            "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "close.mov", "target_url": "file:///media/close.mov", "metadata": {},
              "available_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 24}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 2400, "rate": 24}}}},
           {"OTIO_SCHEMA": "Transition.1", "name": "Dissolve", "transition_type": "SMPTE_Dissolve", "metadata": {},
            "in_offset": {"OTIO_SCHEMA": "RationalTime.1", "value": 12, "rate": 24}, "out_offset": {"OTIO_SCHEMA": "RationalTime.1", "value": 12, "rate": 24}},
           {"OTIO_SCHEMA": "Clip.2", "name": "Wide again", "metadata": {}, "markers": [], "effects": [], "enabled": true,
            "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 480, "rate": 24}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 48, "rate": 24}},
            "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "wide.mov", "target_url": "file:///media/wide.mov", "metadata": {},
              "available_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 24}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 2400, "rate": 24}}}}
         ]}
      ]}
  })json";
  ic::Report report;
  const auto timeline = ic::ReadOtio(file, report);
  CHECK(!report.HasErrors());
  CHECK_EQ(timeline.sequences.size(), std::size_t{1});
  CHECK_EQ(timeline.media.size(), std::size_t{2});  // wide.mov once, though two clips use it
  const auto& sequence = timeline.sequences[0];
  CHECK_EQ(sequence.settings.frame_rate.numerator, std::int64_t{24});
  CHECK_EQ(sequence.tracks.size(), std::size_t{1});
  const auto& track = sequence.tracks[0];
  CHECK_EQ(track.items.size(), std::size_t{3});

  const auto& wide = track.items[0];
  CHECK(wide.timeline_start.Compare(Seconds(0)) == 0);
  CHECK(wide.source_in.Compare(Seconds(2)) == 0);                    // frame 48 at 24 fps
  CHECK(wide.playback_rate.Compare(RationalTime(2, 1)) == 0);
  CHECK(wide.duration().Compare(Seconds(3)) == 0);                   // 72 frames on the timeline
  CHECK(wide.source_out.Compare(Seconds(8)) == 0);                   // 6 s of source at double speed
  const auto& close = track.items[1];
  CHECK(close.timeline_start.Compare(Seconds(4)) == 0);              // 3 s clip, 1 s gap
  CHECK(close.source_in.Compare(Seconds(10)) == 0);
  CHECK(close.duration().Compare(Seconds(4)) == 0);
  const auto& again = track.items[2];
  CHECK(again.timeline_start.Compare(Seconds(8)) == 0);
  CHECK_EQ(again.source_id, wide.source_id);                          // the same media item

  CHECK_EQ(track.transitions.size(), std::size_t{1});
  const auto& dissolve = track.transitions[0];
  CHECK(dissolve.start.Compare(RationalTime(15, 2)) == 0);           // half a second before the cut at 8 s
  CHECK(dissolve.duration.Compare(Seconds(1)) == 0);
  CHECK(dissolve.alignment == model::TransitionAlignment::Center);
  CHECK(dissolve.from_item.has_value() && *dissolve.from_item == close.id);
  CHECK(dissolve.to_item.has_value() && *dissolve.to_item == again.id);

  // What was assumed, and what was not understood, is in the report.
  CHECK(report.Mentions("Sparkle"));
  CHECK(report.Mentions("frame rate"));
  CHECK(report.Lossy());

  // And it applies: the transition has the handles it needs.
  Project target(std::make_unique<ProjectStore>(":memory:"));
  target.store().Initialize();
  target.Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Foreign"});
  ic::Report apply;
  const auto result = ic::ApplyToProject(target.store(), timeline, {}, apply);
  Expect(!apply.HasErrors(), apply.ToText(), __LINE__);
  CHECK_EQ(result.clips_created, 3);
  CHECK_EQ(result.transitions_created, 1);
}

CUTLINE_TEST(WhatCannotBeReadIsReportedAndTheRestOfTheFileIsStillUsed) {
  // A generator, a track of an unknown kind, a marker on a track and a foreign item type
  // sit around one clip that can be read.
  const std::string file = R"json({
    "OTIO_SCHEMA": "Timeline.1", "name": "Mixed", "metadata": {},
    "tracks": {"OTIO_SCHEMA": "Stack.1", "name": "tracks", "metadata": {}, "markers": [], "effects": [],
      "children": [
        {"OTIO_SCHEMA": "Track.1", "name": "V1", "kind": "Video", "metadata": {}, "effects": [],
         "markers": [{"OTIO_SCHEMA": "Marker.2", "name": "track marker", "color": "RED", "metadata": {}, "marked_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25}}}],
         "children": [
           {"OTIO_SCHEMA": "Clip.2", "name": "Bars", "metadata": {}, "markers": [], "effects": [], "enabled": true,
            "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 50, "rate": 25}},
            "media_reference": {"OTIO_SCHEMA": "GeneratorReference.1", "name": "bars", "generator_kind": "SMPTEBars", "parameters": {}, "metadata": {}}},
           {"OTIO_SCHEMA": "Clip.2", "name": "Real", "metadata": {}, "markers": [], "effects": [], "enabled": true,
            "source_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 100, "rate": 25}},
            "media_reference": {"OTIO_SCHEMA": "ExternalReference.1", "name": "real.mov", "target_url": "/media/real.mov", "metadata": {},
              "available_range": {"OTIO_SCHEMA": "TimeRange.1", "start_time": {"OTIO_SCHEMA": "RationalTime.1", "value": 0, "rate": 25}, "duration": {"OTIO_SCHEMA": "RationalTime.1", "value": 500, "rate": 25}}}},
           {"OTIO_SCHEMA": "Mystery.3", "name": "???", "metadata": {}}
         ]},
        {"OTIO_SCHEMA": "Track.1", "name": "Subtitles", "kind": "Subtitle", "metadata": {}, "children": [], "markers": [], "effects": []}
      ]}
  })json";
  ic::Report report;
  const auto timeline = ic::ReadOtio(file, report);
  CHECK(report.Lossy());
  CHECK(report.Mentions("GeneratorReference"));
  CHECK(report.Mentions("Subtitle"));
  CHECK(report.Mentions("Mystery"));
  CHECK(report.Mentions("track marker") || report.Mentions("markers on the track"));
  CHECK_EQ(timeline.sequences.size(), std::size_t{1});
  CHECK_EQ(timeline.sequences[0].tracks.size(), std::size_t{1});
  CHECK_EQ(timeline.sequences[0].tracks[0].items.size(), std::size_t{1});
  // The generator became a gap of its own length: the real clip is where it should be, at 2 s.
  CHECK(timeline.sequences[0].tracks[0].items[0].timeline_start.Compare(Seconds(2)) == 0);

  // Not JSON, and JSON that is not a timeline, are errors and not exceptions.
  ic::Report broken;
  CHECK(ic::ReadOtio("this is { not json", broken).sequences.empty());
  CHECK(broken.HasErrors());
  ic::Report wrong;
  CHECK(ic::ReadOtio("{\"OTIO_SCHEMA\": \"Clip.2\"}", wrong).sequences.empty());
  CHECK(wrong.HasErrors());
}

CUTLINE_TEST(AnImportThatTheProjectRefusesPartlyReportsWhereAndKeepsTheRest) {
  // Two clips on one track that overlap: the project refuses the second, the rest goes in.
  ic::Timeline timeline;
  timeline.media.push_back({"m", "m.mov", "/m.mov", "fp", Seconds(60), Seconds(0)});
  ic::Sequence sequence;
  sequence.id = "s";
  sequence.settings.name = "Bad";
  sequence.settings.frame_rate = {25, 1};
  sequence.settings.width = 1280;
  sequence.settings.height = 720;
  sequence.settings.sample_rate = 48000;
  ic::Track track;
  track.id = "t";
  track.name = "V1";
  track.kind = model::TrackKind::Video;
  for (const auto& [id, start] : {std::pair<std::string, int>{"c1", 0}, {"c2", 3}, {"c3", 20}}) {
    ic::Item item;
    item.id = id;
    item.name = id;
    item.source_id = "m";
    item.source_in = Seconds(0);
    item.source_out = Seconds(10);
    item.timeline_start = Seconds(start);
    track.items.push_back(item);
  }
  sequence.tracks.push_back(track);
  timeline.sequences.push_back(sequence);

  Project target(std::make_unique<ProjectStore>(":memory:"));
  target.store().Initialize();
  target.Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Target"});
  ic::Report report;
  const auto result = ic::ApplyToProject(target.store(), timeline, {}, report);
  CHECK(report.HasErrors());
  CHECK(report.Mentions("c2"));
  CHECK_EQ(result.clips_created, 2);

  // Applying the same timeline again with a prefix does not collide with the first.
  ic::Report second;
  ic::ApplyOptions options;
  options.id_prefix = "again-";
  const auto again = ic::ApplyToProject(target.store(), timeline, options, second);
  CHECK_EQ(again.clips_created, 2);
  CHECK_EQ(again.sequences_created, 1);
}

CUTLINE_TEST(RatesFromTheOtherSideOfTheFileBecomeTheExactBroadcastRates) {
  bool exact = false;
  auto rate = ic::RateFromDouble(24.0, exact);
  CHECK(exact && rate.numerator == 24 && rate.denominator == 1);
  rate = ic::RateFromDouble(23.976023976023978, exact);
  CHECK(exact && rate.numerator == 24000 && rate.denominator == 1001);
  rate = ic::RateFromDouble(29.97, exact);       // a rounded 29.97 is not 30000/1001
  CHECK(!exact);
  rate = ic::RateFromDouble(29.97002997002997, exact);
  CHECK(exact && rate.numerator == 30000 && rate.denominator == 1001);
  rate = ic::RateFromDouble(59.94005994005994, exact);
  CHECK(exact && rate.numerator == 60000 && rate.denominator == 1001);
  rate = ic::RateFromDouble(0.0, exact);
  CHECK(!exact);
  rate = ic::RateFromDouble(48000.0, exact);
  CHECK(exact && rate.numerator == 48000);
}

// ------------------------------------------------------------------------- EDL ----

namespace {

// What an EDL can hold: the layout and clip fields, not the ids of tracks or the sequence.
std::string EdlDifferences(const ic::Timeline& a, const ic::Timeline& b) {
  const auto same = [](const RationalTime& x, const RationalTime& y) { return x.Compare(y) == 0; };
  if (a.sequences.empty() || b.sequences.empty()) return "no sequence";
  const auto& x = a.sequences[0];
  const auto& y = b.sequences[0];
  if (x.settings.frame_rate.numerator != y.settings.frame_rate.numerator || x.settings.drop_frame != y.settings.drop_frame) return "frame rate";
  if (a.media.size() != b.media.size()) return "media count";
  for (std::size_t m = 0; m < a.media.size(); ++m) {
    if (a.media[m].id != b.media[m].id || a.media[m].url != b.media[m].url || !same(a.media[m].duration, b.media[m].duration) ||
        !same(a.media[m].start_timecode, b.media[m].start_timecode)) {
      return "media " + a.media[m].id;
    }
  }
  if (x.tracks.size() != y.tracks.size()) return "track count " + std::to_string(x.tracks.size()) + " vs " + std::to_string(y.tracks.size());
  for (std::size_t k = 0; k < x.tracks.size(); ++k) {
    const auto& p = x.tracks[k];
    const auto& q = y.tracks[k];
    if (p.kind != q.kind || p.items.size() != q.items.size() || p.transitions.size() != q.transitions.size()) return "track " + p.id;
    for (std::size_t i = 0; i < p.items.size(); ++i) {
      const auto& u = p.items[i];
      const auto& v = q.items[i];
      if (u.id != v.id || u.source_id != v.source_id || !same(u.source_in, v.source_in) || !same(u.source_out, v.source_out) ||
          !same(u.timeline_start, v.timeline_start) || !same(u.playback_rate, v.playback_rate) || u.reversed != v.reversed ||
          u.linked_group != v.linked_group) {
        return "clip " + u.id;
      }
    }
    for (std::size_t i = 0; i < p.transitions.size(); ++i) {
      const auto& u = p.transitions[i];
      const auto& v = q.transitions[i];
      if (u.from_item != v.from_item || u.to_item != v.to_item || !same(u.start, v.start) || !same(u.duration, v.duration)) {
        return "transition " + u.id;
      }
    }
  }
  return {};
}

Project EdlProject(cutline::time::FrameRate rate = cutline::time::kFrameRate25, bool drop = false) {
  Project project(rate);
  if (drop) {
    commands::UpdateSequenceSettingsPayload settings;
    settings.id = "seq-main";
    settings.settings.name = "Main";
    settings.settings.frame_rate = rate;
    settings.settings.width = 1920;
    settings.settings.height = 1080;
    settings.settings.sample_rate = 48000;
    settings.settings.drop_frame = true;
    project.Run(commands::CommandType::UpdateSequenceSettings, settings);
  }
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  project.Run(commands::CommandType::AddAudioTrack, commands::AddTrackPayload{"a1", "seq-main", 0, "stereo", "A1"});
  project.Clip("clip-1", "v1", "media-a", 0, 5, 9);                          // 4 s
  project.Clip("clip-2", "v1", "media-b", 4, 2, 10, {2, 1});                 // 8 s of source at double speed: 4 s
  project.Clip("clip-3", "v1", "media-a", 10, 20, 24, {1, 1}, true);         // reversed, after a gap
  project.Clip("aud-1", "a1", "media-a", 0, 5, 9);
  project.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{{"clip-1", "aud-1"}, "take-1"});
  commands::AddTransitionPayload dissolve;
  dissolve.id = "tr-1";
  dissolve.track_id = "v1";
  dissolve.kind = "cross_dissolve";
  dissolve.alignment = model::TransitionAlignment::Start;
  dissolve.from_clip_id = "clip-1";
  dissolve.to_clip_id = "clip-2";
  dissolve.timeline_start = Seconds(4);
  dissolve.duration = Seconds(1);
  project.Run(commands::CommandType::AddTransition, dissolve);
  commands::AddMarkerPayload marker;
  marker.id = "m-1";
  marker.owner_kind = model::MarkerOwner::Sequence;
  marker.owner_id = "seq-main";
  marker.start = Seconds(3);
  marker.end = Seconds(3);
  marker.label = "Check this";
  marker.color = "red";
  project.Run(commands::CommandType::AddMarker, marker);
  return project;
}

}  // namespace

CUTLINE_TEST(AnEdlHasTheEventsTheDissolveTheSpeedChangeAndTheLocator) {
  auto project = EdlProject();
  ic::Report report;
  const auto timeline = ic::FromProject(project.store(), "seq-main", report);
  const auto edl = ic::WriteEdl(timeline, {}, report);
  Expect(!report.Lossy(), report.ToText(), __LINE__);
  CHECK(Contains(edl, "TITLE: Main"));
  CHECK(Contains(edl, "FCM: NON-DROP FRAME"));
  // Event 1: the first clip, media at 5 s from 00:00:05:00, placed at 01:00:00:00.
  CHECK(Contains(edl, "001  A        V     C        00:00:05:00 00:00:09:00 01:00:00:00 01:00:04:00"));
  // The dissolve is the zero-length outgoing line and the incoming line with its length in frames.
  CHECK(Contains(edl, "003  A        V     C        00:00:09:00 00:00:09:00 01:00:04:00 01:00:04:00"));
  CHECK(Contains(edl, "003  B        V     D    025    00:00:02:00 00:00:10:00 01:00:04:00 01:00:08:00"));
  // Double speed: 50 frames a second at a 25 fps sequence.
  CHECK(Contains(edl, "M2   B        050.0"));
  // The reversed clip has a negative speed.
  CHECK(Contains(edl, "-025.0"));
  CHECK(Contains(edl, "* LOC: 01:00:03:00 RED  Check this"));
  CHECK(Contains(edl, "* FROM CLIP NAME: a.mov"));
  CHECK(Contains(edl, "* SOURCE FILE: /footage/b.mov"));
  // The audio event shares the first one's times, on channel A.
  CHECK(Contains(edl, " A     C        00:00:05:00 00:00:09:00 01:00:00:00 01:00:04:00"));
}

CUTLINE_TEST(AnEdlRoundTripsEverythingItCanHold) {
  auto project = EdlProject();
  ic::Report report;
  const auto original = ic::FromProject(project.store(), "seq-main", report);
  const auto edl = ic::WriteEdl(original, {}, report);
  ic::Report read_report;
  const auto parsed = ic::ReadEdl(edl, cutline::time::kFrameRate25, false, read_report);
  CHECK(!read_report.HasErrors());
  const auto difference = EdlDifferences(original, parsed);
  Expect(difference.empty(), "the list read back differs in: " + difference, __LINE__);
  CHECK_EQ(parsed.sequences[0].markers.size(), std::size_t{1});
  CHECK(parsed.sequences[0].markers[0].start.Compare(Seconds(3)) == 0);
  CHECK_EQ(parsed.sequences[0].markers[0].label, std::string("Check this"));

  // And it applies to a project.
  Project target(std::make_unique<ProjectStore>(":memory:"));
  target.store().Initialize();
  target.Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Target"});
  ic::Report apply;
  const auto result = ic::ApplyToProject(target.store(), parsed, {}, apply);
  Expect(!apply.HasErrors(), apply.ToText(), __LINE__);
  CHECK_EQ(result.clips_created, 4);
  CHECK_EQ(result.transitions_created, 1);
  ic::Report again;
  const auto reloaded = ic::FromProject(target.store(), "edl-sequence", again);
  const auto after = EdlDifferences(original, reloaded);
  Expect(after.empty(), "after importing, the project differs in: " + after, __LINE__);
  CHECK(ic::WriteEdl(reloaded, {}, again) == edl);  // writing what was read writes the same list
}

CUTLINE_TEST(ADissolveCentredOnACutIsWrittenFromItsStartWithTheSamePictureAfterwards) {
  // Centred on the cut at 4 s and 0.8 s long, running 3.6 s to 4.4 s (so 0.4 s before the cut). An EDL
  // starts a dissolve at its incoming clip, so the incoming clip moves 0.4 s earlier and the
  // outgoing one is cut there; the picture at every time in the dissolve must not change.
  Project project;
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  project.Clip("out", "v1", "media-a", 0, 10, 14);
  project.Clip("in", "v1", "media-b", 4, 20, 26, {3, 2});
  commands::AddTransitionPayload dissolve;
  dissolve.id = "tr-1";
  dissolve.track_id = "v1";
  dissolve.kind = "cross_dissolve";
  dissolve.alignment = model::TransitionAlignment::Center;
  dissolve.from_clip_id = "out";
  dissolve.to_clip_id = "in";
  dissolve.timeline_start = RationalTime(18, 5);
  dissolve.duration = RationalTime(4, 5);
  project.Run(commands::CommandType::AddTransition, dissolve);

  ic::Report report;
  const auto original = ic::FromProject(project.store(), "seq-main", report);
  const auto edl = ic::WriteEdl(original, {}, report);
  CHECK(report.Mentions("earlier"));
  CHECK(!report.Mentions("rounded"));
  ic::Report read_report;
  const auto parsed = ic::ReadEdl(edl, cutline::time::kFrameRate25, false, read_report);
  CHECK(!read_report.HasErrors());
  const auto& track = parsed.sequences[0].tracks[0];
  CHECK_EQ(track.items.size(), std::size_t{2});
  CHECK_EQ(track.transitions.size(), std::size_t{1});
  CHECK(track.transitions[0].start.Compare(RationalTime(18, 5)) == 0);
  CHECK(track.transitions[0].duration.Compare(RationalTime(4, 5)) == 0);

  // What each clip shows at each time across the dissolve, before and after.
  const auto source_at = [](const ic::Item& item, const RationalTime& when) {
    const auto elapsed = when.Subtract(item.timeline_start).Multiply(item.playback_rate);
    return item.reversed ? item.source_out.Subtract(elapsed) : item.source_in.Add(elapsed);
  };
  const auto& before = original.sequences[0].tracks[0];
  for (const auto& at : {RationalTime(18, 5), Seconds(4), RationalTime(21, 5), RationalTime(22, 5)}) {
    CHECK(source_at(before.items[0], at).Compare(source_at(track.items[0], at)) == 0);
    CHECK(source_at(before.items[1], at).Compare(source_at(track.items[1], at)) == 0);
  }
  // The outgoing clip now ends where the dissolve begins; the incoming one begins there.
  CHECK(track.items[0].end().Compare(RationalTime(18, 5)) == 0);
  CHECK(track.items[1].timeline_start.Compare(RationalTime(18, 5)) == 0);
}

CUTLINE_TEST(DropFrameTimecodeRoundTripsAtTwentyNinePointNineSeven) {
  // A frame count that crosses a minute, where drop-frame skips frame numbers.
  Project project(cutline::time::kFrameRate2997);
  commands::UpdateSequenceSettingsPayload settings;
  settings.id = "seq-main";
  settings.settings.name = "Main";
  settings.settings.frame_rate = cutline::time::kFrameRate2997;
  settings.settings.width = 1920;
  settings.settings.height = 1080;
  settings.settings.sample_rate = 48000;
  settings.settings.drop_frame = true;
  project.Run(commands::CommandType::UpdateSequenceSettings, settings);
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  commands::InsertClipPayload clip;
  clip.id = "clip-1";
  clip.track_id = "v1";
  clip.media_id = "media-a";
  clip.source_in = RationalTime::FromFrames(100, cutline::time::kFrameRate2997);
  clip.source_out = RationalTime::FromFrames(140, cutline::time::kFrameRate2997);
  clip.timeline_start = RationalTime::FromFrames(1790, cutline::time::kFrameRate2997);  // 59;20 in record time, running over the minute
  project.Run(commands::CommandType::InsertClip, clip);

  ic::Report report;
  const auto original = ic::FromProject(project.store(), "seq-main", report);
  const auto edl = ic::WriteEdl(original, {}, report);
  CHECK(Contains(edl, "FCM: DROP FRAME"));
  // Record time is 01:00:00;00 plus frames 1790 and 1830: in drop-frame timecode the minute turns
  // over with two frame numbers skipped, so the end is 01:01:01;02 and not 01:01:00;10.
  CHECK(Contains(edl, "01:00:59;20 01:01:01;02"));
  ic::Report read_report;
  const auto parsed = ic::ReadEdl(edl, cutline::time::kFrameRate2997, true, read_report);
  CHECK(!read_report.HasErrors());
  const auto difference = EdlDifferences(original, parsed);
  Expect(difference.empty(), "drop-frame times drifted: " + difference, __LINE__);
}

CUTLINE_TEST(AListFromAnotherSystemIsReadAndWhatItCannotSayIsReported) {
  const std::string edl =
      "TITLE: Reel One\r\n"
      "FCM: NON-DROP FRAME\r\n"
      "\r\n"
      "001  TAPE01   V     C        01:00:10:00 01:00:15:00 01:00:00:00 01:00:05:00\r\n"
      "* FROM CLIP NAME: Interview wide\r\n"
      "002  TAPE02   V     C        02:00:00:00 02:00:00:00 01:00:05:00 01:00:05:00\r\n"
      "002  TAPE02   V     D    012 02:00:00:00 02:00:02:00 01:00:05:00 01:00:09:00\r\n"
      "M2   TAPE02   012.5            02:00:00:00\r\n"
      "003  TAPE01   AA/V  C        01:00:20:00 01:00:22:00 01:00:09:00 01:00:11:00\r\n"
      "004  TAPE02   V     W001 020 02:00:10:00 02:00:12:00 01:00:11:00 01:00:13:00\r\n"
      "005  TAPE03   NONE  C        00:00:00:00 00:00:02:00 01:00:13:00 01:00:15:00\r\n"
      "* LOC: 01:00:02:00 YELLOW  Pickup here\r\n";
  ic::Report report;
  const auto timeline = ic::ReadEdl(edl, cutline::time::kFrameRate25, false, report);
  CHECK(!report.HasErrors());
  CHECK_EQ(timeline.sequences.size(), std::size_t{1});
  const auto& sequence = timeline.sequences[0];
  CHECK_EQ(sequence.settings.name, std::string("Reel One"));
  // Two reels were used by events that could be read; each became media.
  CHECK_EQ(timeline.media.size(), std::size_t{2});
  const ic::Track* video = nullptr;
  std::size_t audio_tracks = 0;
  for (const auto& track : sequence.tracks) {
    if (track.kind == model::TrackKind::Video) video = &track;
    else ++audio_tracks;
  }
  CHECK(video != nullptr);
  CHECK_EQ(audio_tracks, std::size_t{2});  // AA/V puts the audio on two channels
  if (video == nullptr) return;
  CHECK_EQ(video->items.size(), std::size_t{4});  // events 1, 2 (the dissolve's incoming side), 3's picture, and the wipe as a cut
  const auto& first = video->items[0];
  CHECK_EQ(first.name, std::string("Interview wide"));
  CHECK(first.timeline_start.Compare(Seconds(0)) == 0);          // the list starts at 01:00:00:00
  CHECK(first.duration().Compare(Seconds(5)) == 0);
  CHECK(first.playback_rate.Compare(RationalTime(1, 1)) == 0);
  // Event 2 is played at 12.5 fps in a 25 fps list: half speed, over 4 s of record time -> 2 s of source.
  const auto& second = video->items[1];
  CHECK(second.timeline_start.Compare(Seconds(5)) == 0);
  CHECK(second.duration().Compare(Seconds(4)) == 0);
  CHECK_EQ(video->transitions.size(), std::size_t{1});
  CHECK(video->transitions[0].duration.Compare(RationalTime(12, 25)) == 0);
  CHECK(video->transitions[0].from_item.has_value() && *video->transitions[0].from_item == first.id);
  CHECK_EQ(sequence.markers.size(), std::size_t{1});
  CHECK(sequence.markers[0].start.Compare(Seconds(2)) == 0);
  CHECK_EQ(sequence.markers[0].color, std::string("yellow"));
  CHECK(report.Mentions("wipe"));
  CHECK(report.Mentions("NONE"));
  CHECK(report.Mentions("long its source is"));  // media length was inferred and said so
  CHECK(report.Lossy());
}

CUTLINE_TEST(WhatAnEdlCannotHoldIsReportedOnTheWayOut) {
  Project project;
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v2", "seq-main", 1, "stereo", "V2"});
  project.Clip("c1", "v1", "media-a", 0, 0, 4);
  project.Clip("c2", "v2", "media-b", 0, 0, 4);
  commands::AddEffectPayload effect;
  effect.id = "fx";
  effect.owner_kind = model::EffectOwner::Clip;
  effect.owner_id = "c1";
  effect.effect_type = "blur";
  effect.parameters = {{"fx:radius", "radius", Value::Scalar(4.0)}};
  project.Run(commands::CommandType::AddEffect, effect);
  ic::Report report;
  const auto timeline = ic::FromProject(project.store(), "seq-main", report);
  const auto edl = ic::WriteEdl(timeline, {}, report);
  CHECK(report.Lossy());
  CHECK(report.Mentions("one video channel"));
  CHECK(report.Mentions("effects"));
  // The lowest video track is the one written.
  CHECK(Contains(edl, "* FROM CLIP NAME: a.mov"));
  CHECK(!Contains(edl, "b.mov"));
}

// --------------------------------------------------------------------- XML ----

CUTLINE_TEST(TheXmlReaderBuildsTheTreeAndRefusesWhatIsNotWellFormed) {
  const auto root = cutline::xml::Parse(
      "<?xml version=\"1.0\"?>\n<!DOCTYPE xmeml>\n<!-- a comment -->\n"
      "<a id='x &amp; y'><b>one &lt; two &#65;&#x42;</b><c/><d><![CDATA[<raw & text>]]></d><b>second</b></a>");
  CHECK_EQ(root.name, std::string("a"));
  CHECK_EQ(root.Attribute("id"), std::string("x & y"));
  CHECK_EQ(root.children.size(), std::size_t{4});
  CHECK_EQ(root.ChildText("b"), std::string("one < two AB"));
  CHECK_EQ(root.FindAll("b").size(), std::size_t{2});
  CHECK_EQ(root.ChildText("d"), std::string("<raw & text>"));
  CHECK(root.Find("c") != nullptr);
  CHECK(root.Find("nothing") == nullptr);
  CHECK_EQ(root.ChildText("nothing", "fallback"), std::string("fallback"));
  CHECK_EQ(cutline::xml::Escape("a<b>&\"'"), std::string("a&lt;b&gt;&amp;&quot;&apos;"));
  // Not well formed: each is an error that names where, never a partial tree.
  CHECK_THROWS(cutline::xml::Parse("<a><b></a></b>"));
  CHECK_THROWS(cutline::xml::Parse("<a><b>text</b>"));
  CHECK_THROWS(cutline::xml::Parse("<a></a>trailing"));
  CHECK_THROWS(cutline::xml::Parse("<a attr=unquoted/>"));
  CHECK_THROWS(cutline::xml::Parse("<a>&unknown;</a>"));
  CHECK_THROWS(cutline::xml::Parse("not xml"));
  CHECK_THROWS(cutline::xml::Parse(""));
}

CUTLINE_TEST(FcpXmlRoundTripsEverythingItCarriesAtWholeFrames) {
  auto source = RichProject();
  ic::Report report;
  const auto original = ic::FromProject(source.store(), "seq-main", report);
  const auto text = ic::WriteFcpXml(original, report);
  Expect(!report.Lossy(), report.ToText(), __LINE__);
  CHECK(Contains(text, "<xmeml version=\"4\">"));
  CHECK(Contains(text, "<timebase>25</timebase>"));
  CHECK(Contains(text, "<pathurl>file://localhost/footage/a.mov</pathurl>"));
  CHECK(Contains(text, "<transitionitem>"));
  CHECK(Contains(text, "<alignment>center</alignment>"));
  CHECK(Contains(text, "<effectid>timeremap</effectid>"));
  CHECK(Contains(text, "<value>200</value>"));  // clip-2 at double speed
  CHECK(Contains(text, "<linkclipref>"));

  ic::Report read_report;
  const auto parsed = ic::ReadFcpXml(text, read_report);
  Expect(!read_report.Lossy(), read_report.ToText(), __LINE__);
  const auto first = Differences(original, parsed);
  Expect(first.empty(), "reading it back differs in: " + first, __LINE__);

  Project target(std::make_unique<ProjectStore>(":memory:"));
  target.store().Initialize();
  target.Run(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Target"});
  ic::Report apply;
  const auto result = ic::ApplyToProject(target.store(), parsed, {}, apply);
  Expect(!apply.HasErrors(), apply.ToText(), __LINE__);
  CHECK_EQ(result.clips_created, 4);
  CHECK_EQ(result.transitions_created, 1);
  ic::Report again;
  const auto reloaded = ic::FromProject(target.store(), "seq-main", again);
  const auto second = Differences(original, reloaded);
  Expect(second.empty(), "after importing, the project differs in: " + second, __LINE__);
  CHECK(ic::WriteFcpXml(reloaded, again) == text);
}

CUTLINE_TEST(TimesBetweenFramesAreRoundedAndSaidSoWhenWritingFcpXml) {
  Project project;
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  commands::InsertClipPayload clip;
  clip.id = "clip-1";
  clip.track_id = "v1";
  clip.media_id = "media-a";
  clip.source_in = RationalTime(1, 3);   // 8.33 frames at 25 fps
  clip.source_out = RationalTime(10, 3);
  clip.timeline_start = Seconds(1);
  project.Run(commands::CommandType::InsertClip, clip);
  ic::Report report;
  const auto timeline = ic::FromProject(project.store(), "seq-main", report);
  const auto text = ic::WriteFcpXml(timeline, report);
  CHECK(report.Lossy());
  CHECK(report.Mentions("rounded"));
  // The nearest frames: 8 and 83.
  CHECK(Contains(text, "<in>8</in>"));
  CHECK(Contains(text, "<out>83</out>"));
}

CUTLINE_TEST(AFileFromAnotherEditorIsReadWithItsFilesLinksSpeedAndTransitions) {
  // 29.97 fps. One video track: a clip with its file in full, a dissolve centred on the cut, and a
  // second clip that refers to the same file by id at half speed. One audio track whose clip is
  // linked to the first. A filter this reader does not know, and a marker.
  const std::string file = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE xmeml>
<xmeml version="4">
  <sequence id="sequence-1">
    <name>Edit &amp; Mix</name>
    <duration>400</duration>
    <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate>
    <timecode><rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate><string>00:00:00:00</string><frame>0</frame><displayformat>NDF</displayformat></timecode>
    <media>
      <video>
        <format><samplecharacteristics><rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate><width>1280</width><height>720</height><fielddominance>none</fielddominance></samplecharacteristics></format>
        <track>
          <clipitem id="clipitem-1">
            <name>Interview</name><enabled>TRUE</enabled><duration>3000</duration>
            <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate>
            <start>0</start><end>200</end><in>300</in><out>500</out>
            <file id="file-1"><name>interview.mov</name><pathurl>file://localhost/Volumes/Media/My%20Shoot/interview.mov</pathurl>
              <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate><duration>3000</duration>
              <timecode><rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate><string>01:00:00:00</string><frame>108000</frame><displayformat>NDF</displayformat></timecode></file>
            <filter><effect><name>Gaussian Blur</name><effectid>GaussianBlur</effectid></effect></filter>
            <link><linkclipref>clipitem-1</linkclipref><mediatype>video</mediatype><trackindex>1</trackindex><clipindex>1</clipindex></link>
            <link><linkclipref>clipitem-3</linkclipref><mediatype>audio</mediatype><trackindex>1</trackindex><clipindex>1</clipindex></link>
          </clipitem>
          <transitionitem>
            <start>190</start><end>210</end><alignment>center</alignment>
            <effect><name>Cross Dissolve</name><effectid>Cross Dissolve</effectid><effecttype>transition</effecttype></effect>
          </transitionitem>
          <clipitem id="clipitem-2">
            <name>Interview slow</name><enabled>TRUE</enabled><duration>3000</duration>
            <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate>
            <start>200</start><end>400</end><in>600</in><out>700</out>
            <file id="file-1"/>
            <filter><effect><name>Time Remap</name><effectid>timeremap</effectid>
              <parameter><parameterid>speed</parameterid><value>50</value></parameter>
              <parameter><parameterid>reverse</parameterid><value>FALSE</value></parameter></effect></filter>
          </clipitem>
        </track>
      </video>
      <audio>
        <track>
          <clipitem id="clipitem-3">
            <name>Interview</name><enabled>TRUE</enabled><duration>3000</duration>
            <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate>
            <start>0</start><end>200</end><in>300</in><out>500</out>
            <file id="file-1"/>
            <link><linkclipref>clipitem-1</linkclipref><mediatype>video</mediatype><trackindex>1</trackindex><clipindex>1</clipindex></link>
            <link><linkclipref>clipitem-3</linkclipref><mediatype>audio</mediatype><trackindex>1</trackindex><clipindex>1</clipindex></link>
          </clipitem>
        </track>
      </audio>
    </media>
    <marker><name>Cut here</name><comment></comment><in>150</in><out>-1</out></marker>
  </sequence>
</xmeml>)xml";
  ic::Report report;
  const auto timeline = ic::ReadFcpXml(file, report);
  CHECK(!report.HasErrors());
  CHECK_EQ(timeline.sequences.size(), std::size_t{1});
  const auto& sequence = timeline.sequences[0];
  CHECK_EQ(sequence.settings.name, std::string("Edit & Mix"));
  CHECK_EQ(sequence.settings.frame_rate.numerator, std::int64_t{30000});
  CHECK_EQ(sequence.settings.frame_rate.denominator, std::int64_t{1001});
  CHECK_EQ(sequence.settings.width, std::int64_t{1280});
  CHECK_EQ(timeline.media.size(), std::size_t{1});  // one file, three clips
  CHECK_EQ(timeline.media[0].url, std::string("/Volumes/Media/My Shoot/interview.mov"));
  const cutline::time::FrameRate ntsc{30000, 1001};
  CHECK(timeline.media[0].duration.Compare(RationalTime::FromFrames(3000, ntsc)) == 0);
  CHECK(timeline.media[0].start_timecode.Compare(RationalTime::FromFrames(108000, ntsc)) == 0);

  const ic::Track* video = nullptr;
  const ic::Track* audio = nullptr;
  for (const auto& track : sequence.tracks) (track.kind == model::TrackKind::Video ? video : audio) = &track;
  CHECK(video != nullptr && audio != nullptr);
  if (video == nullptr || audio == nullptr) return;
  CHECK_EQ(video->items.size(), std::size_t{2});
  const auto& first = video->items[0];
  CHECK(first.timeline_start.Compare(Seconds(0)) == 0);
  CHECK(first.source_in.Compare(RationalTime::FromFrames(300, ntsc)) == 0);
  CHECK(first.duration().Compare(RationalTime::FromFrames(200, ntsc)) == 0);
  const auto& slow = video->items[1];
  CHECK(slow.playback_rate.Compare(RationalTime(1, 2)) == 0);                                  // 50 percent
  CHECK(slow.duration().Compare(RationalTime::FromFrames(200, ntsc)) == 0);                    // 100 frames of source over 200
  CHECK(slow.source_out.Subtract(slow.source_in).Compare(RationalTime::FromFrames(100, ntsc)) == 0);
  CHECK(!slow.reversed);
  CHECK_EQ(slow.source_id, first.source_id);

  CHECK_EQ(video->transitions.size(), std::size_t{1});
  const auto& dissolve = video->transitions[0];
  CHECK(dissolve.start.Compare(RationalTime::FromFrames(190, ntsc)) == 0);
  CHECK(dissolve.duration.Compare(RationalTime::FromFrames(20, ntsc)) == 0);
  CHECK(dissolve.alignment == model::TransitionAlignment::Center);
  CHECK(dissolve.from_item.has_value() && *dissolve.from_item == first.id);
  CHECK(dissolve.to_item.has_value() && *dissolve.to_item == slow.id);

  // The audio clip and the first video clip refer to each other: one link group.
  CHECK_EQ(audio->items.size(), std::size_t{1});
  CHECK(!audio->items[0].linked_group.empty());
  CHECK_EQ(audio->items[0].linked_group, first.linked_group);
  CHECK(slow.linked_group.empty());
  CHECK_EQ(sequence.markers.size(), std::size_t{1});
  CHECK(sequence.markers[0].start.Compare(RationalTime::FromFrames(150, ntsc)) == 0);
  CHECK(sequence.markers[0].end.Compare(sequence.markers[0].start) == 0);

  // Said, not hidden: the blur is not one Cutline has.
  CHECK(report.Mentions("Gaussian Blur"));
  CHECK(report.Lossy());
}

CUTLINE_TEST(AnXmlFileThatIsNotAFinalCutSequenceIsRefusedWithAReason) {
  ic::Report broken;
  CHECK(ic::ReadFcpXml("<xmeml><sequence>", broken).sequences.empty());
  CHECK(broken.HasErrors());
  ic::Report wrong;
  CHECK(ic::ReadFcpXml("<fcpxml version=\"1.9\"/>", wrong).sequences.empty());
  CHECK(wrong.HasErrors());
  CHECK(wrong.Mentions("xmeml"));
  ic::Report empty;
  CHECK(ic::ReadFcpXml("<xmeml version=\"4\"><project/></xmeml>", empty).sequences.empty());
  CHECK(empty.Mentions("no sequence"));
}

CUTLINE_TEST(ANestedSequenceIsAClipItemThatContainsASequence) {
  Project project;
  commands::CreateSequencePayload inner;
  inner.id = "seq-inner";
  inner.settings.name = "Inner";
  inner.settings.frame_rate = {25, 1};
  inner.settings.width = 640;
  inner.settings.height = 360;
  inner.settings.sample_rate = 48000;
  project.Run(commands::CommandType::CreateSequence, inner);
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"iv1", "seq-inner", 0, "stereo", "IV1"});
  project.Clip("inner-clip", "iv1", "media-a", 0, 0, 6);
  project.Run(commands::CommandType::AddVideoTrack, commands::AddTrackPayload{"v1", "seq-main", 0, "stereo", "V1"});
  commands::InsertClipPayload nested;
  nested.id = "nest";
  nested.track_id = "v1";
  nested.source_kind = model::SourceKind::Sequence;
  nested.nested_sequence_id = "seq-inner";
  nested.source_in = Seconds(1);
  nested.source_out = Seconds(5);
  nested.timeline_start = Seconds(2);
  nested.name = "Nested";
  project.Run(commands::CommandType::InsertClip, nested);
  ic::Report report;
  const auto original = ic::FromProject(project.store(), "seq-main", report);
  const auto text = ic::WriteFcpXml(original, report);
  CHECK(Contains(text, "<name>Inner</name>"));
  const auto parsed = ic::ReadFcpXml(text, report);
  CHECK(!report.Lossy());
  CHECK_EQ(parsed.sequences.size(), std::size_t{2});
  const auto difference = Differences(original, parsed);
  Expect(difference.empty(), "the nested sequence differs in: " + difference, __LINE__);
}

// -------------------------------------------------------------------- captions ----

namespace {

namespace cap = cutline::captions;

cap::Cue MakeCue(std::int64_t start_ms, std::int64_t end_ms, const std::string& words) {
  cap::Cue cue;
  cue.start = RationalTime(start_ms, 1000);
  cue.end = RationalTime(end_ms, 1000);
  cue.text = words;
  return cue;
}

bool SameCues(const std::vector<cap::Cue>& a, const std::vector<cap::Cue>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].start.Compare(b[i].start) != 0 || a[i].end.Compare(b[i].end) != 0 || a[i].text != b[i].text) return false;
  }
  return true;
}

}  // namespace

CUTLINE_TEST(SubRipIsReadWithItsTimesExactToTheMillisecond) {
  const std::string file =
      "\xEF\xBB\xBF" "1\r\n00:00:01,500 --> 00:00:04,200\r\nHello there.\r\n\r\n"
      "2\r\n00:01:02,003 --> 01:00:00,999\r\nTwo lines\r\nof text\r\n\r\n";
  const auto result = cap::ParseSrt(file);
  CHECK(result.clean());
  CHECK_EQ(result.cues.size(), std::size_t{2});
  CHECK(result.cues[0].start.Compare(RationalTime(1500, 1000)) == 0);
  CHECK(result.cues[0].end.Compare(RationalTime(4200, 1000)) == 0);
  CHECK_EQ(result.cues[0].text, std::string("Hello there."));
  CHECK(result.cues[1].start.Compare(RationalTime(62003, 1000)) == 0);
  CHECK(result.cues[1].end.Compare(RationalTime(3600999, 1000)) == 0);  // beyond an hour
  CHECK_EQ(result.cues[1].text, std::string("Two lines\nof text"));
}

CUTLINE_TEST(CaptionsWrittenAndReadAgainAreTheSameCuesToTheLastMillisecond) {
  std::vector<cap::Cue> cues{MakeCue(0, 1, "First"), MakeCue(1500, 4200, "Caf\xC3\xA9 \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\nSecond line"),
                             MakeCue(4000, 4001, "Overlaps the one before"), MakeCue(3599999, 3600001, "Past the hour")};
  const auto srt = cap::WriteSrt(cues);
  CHECK(srt.issues.empty());
  const auto read_srt = cap::ParseSrt(srt.text);
  CHECK(read_srt.clean());
  CHECK(SameCues(cues, read_srt.cues));
  // And the second generation is the same file as the first.
  CHECK(cap::WriteSrt(read_srt.cues).text == srt.text);

  const auto vtt = cap::WriteWebVtt(cues);
  CHECK(vtt.text.rfind("WEBVTT", 0) == 0);
  const auto read_vtt = cap::ParseWebVtt(vtt.text);
  CHECK(read_vtt.clean());
  CHECK(SameCues(cues, read_vtt.cues));
  CHECK(cap::WriteWebVtt(read_vtt.cues).text == vtt.text);
  // Either format is recognised by what it begins with.
  CHECK(SameCues(cues, cap::ParseCaptions(srt.text).cues));
  CHECK(SameCues(cues, cap::ParseCaptions(vtt.text).cues));
  CHECK(cap::ParseCaptions("   \n\n").cues.empty());
}

CUTLINE_TEST(ACueThatCannotBeReadIsSkippedWithItsLineNumberAndTheRestStillComeThrough) {
  const std::string file =
      "1\n00:00:01,000 --> 00:00:02,000\nGood\n\n"
      "2\n00:00:0x,000 --> 00:00:04,000\nBad time\n\n"
      "3\n00:00:05,000 --> 00:00:04,000\nBackwards\n\n"
      "4\n00:00:06,000 --> 00:00:07,000\n\n\n"
      "5\njust some text without any timing\n\n"
      "6\n00:00:08,000 --> 00:00:09,000\nAlso good\n";
  const auto result = cap::ParseSrt(file);
  CHECK_EQ(result.cues.size(), std::size_t{2});
  CHECK_EQ(result.cues[0].text, std::string("Good"));
  CHECK_EQ(result.cues[1].text, std::string("Also good"));
  CHECK_EQ(result.issues.size(), std::size_t{4});
  for (const auto& issue : result.issues) CHECK(issue.dropped && issue.line > 0);
  CHECK_EQ(result.issues[0].line, 6);   // the unreadable time
  CHECK_EQ(result.issues[1].line, 10);  // ends before it starts
  // A file with nothing in it that reads as a cue is nothing, not an error that throws.
  CHECK(cap::ParseSrt("hello world").cues.empty());
  CHECK(!cap::ParseSrt("hello world").issues.empty());
}

CUTLINE_TEST(LenientWhereLenienceCannotChangeTheMeaning) {
  // No cue numbers, a period for the comma, a fraction of one digit, and an hour left out.
  const std::string file = "00:00:01.5 --> 00:00:02.25\nNo number\n\n01:02.000 --> 01:03.000\nMinutes only\n";
  const auto result = cap::ParseSrt(file);
  CHECK(result.clean());
  CHECK_EQ(result.cues.size(), std::size_t{2});
  CHECK(result.cues[0].start.Compare(RationalTime(1500, 1000)) == 0);
  CHECK(result.cues[0].end.Compare(RationalTime(2250, 1000)) == 0);
  CHECK(result.cues[1].start.Compare(RationalTime(62, 1)) == 0);
  // Cues out of order in the file come out in order of time.
  const auto shuffled = cap::ParseSrt("1\n00:00:09,000 --> 00:00:10,000\nLater\n\n2\n00:00:01,000 --> 00:00:02,000\nEarlier\n");
  CHECK_EQ(shuffled.cues[0].text, std::string("Earlier"));
}

CUTLINE_TEST(InlineMarkupIsRemovedAndSaidSoAndAPositionOverrideBecomesAnAlignment) {
  const std::string file =
      "1\n00:00:01,000 --> 00:00:02,000\n<i>Italic</i> and <font color=\"red\">red</font>\n\n"
      "2\n00:00:03,000 --> 00:00:04,000\n{\\an8}At the top\n\n"
      "3\n00:00:05,000 --> 00:00:06,000\n{\\an1}Bottom left\n\n"
      "4\n00:00:07,000 --> 00:00:08,000\n{\\an6}Middle right\n";
  const auto result = cap::ParseSrt(file);
  CHECK_EQ(result.cues.size(), std::size_t{4});
  CHECK_EQ(result.cues[0].text, std::string("Italic and red"));
  CHECK_EQ(result.cues[1].text, std::string("At the top"));
  CHECK(!result.clean());
  bool said = false;
  for (const auto& issue : result.issues) said = said || (issue.message.find("styling") != std::string::npos && !issue.dropped);
  CHECK(said);
  const auto top = cap::ParseStyle(result.cues[1].style_json);
  CHECK_EQ(top.position, std::string("top"));
  CHECK_EQ(top.align, std::string("center"));
  const auto bottom_left = cap::ParseStyle(result.cues[2].style_json);
  CHECK_EQ(bottom_left.position, std::string("bottom"));
  CHECK_EQ(bottom_left.align, std::string("left"));
  const auto middle_right = cap::ParseStyle(result.cues[3].style_json);
  CHECK_EQ(middle_right.position, std::string("middle"));
  CHECK_EQ(middle_right.align, std::string("right"));
  // The position survives a write and a read.
  const auto again = cap::ParseSrt(cap::WriteSrt(result.cues).text);
  CHECK_EQ(cap::ParseStyle(again.cues[1].style_json).position, std::string("top"));
  CHECK_EQ(cap::ParseStyle(again.cues[3].style_json).align, std::string("right"));
}

CUTLINE_TEST(WebVttHasItsHeaderIdentifiersNotesAndSettingsAndEscapesWhatItMust) {
  const std::string file =
      "WEBVTT - the title\n\n"
      "NOTE this is a comment\nthat spans two lines\n\n"
      "STYLE\n::cue { color: red }\n\n"
      "intro\n00:00:01.000 --> 00:00:02.500 align:start line:10%\nTom &amp; Jerry &lt;3\n\n"
      "00:03.000 --> 00:04.000\nShort form\n";
  const auto result = cap::ParseWebVtt(file);
  CHECK(result.clean());
  CHECK_EQ(result.cues.size(), std::size_t{2});
  CHECK_EQ(result.cues[0].text, std::string("Tom & Jerry <3"));
  CHECK_EQ(cap::ParseStyle(result.cues[0].style_json).align, std::string("left"));
  CHECK_EQ(cap::ParseStyle(result.cues[0].style_json).position, std::string("top"));
  CHECK(result.cues[1].start.Compare(RationalTime(3, 1)) == 0);
  // Written back, the ampersand and the angle bracket are escaped as the format requires.
  const auto written = cap::WriteWebVtt(result.cues);
  CHECK(written.text.find("Tom &amp; Jerry &lt;3") != std::string::npos);
  CHECK(written.text.find("align:start") != std::string::npos && written.text.find("line:10%") != std::string::npos);
  // A file that is not WebVTT is refused as such.
  const auto not_vtt = cap::ParseWebVtt("1\n00:00:01,000 --> 00:00:02,000\nHi\n");
  CHECK(not_vtt.cues.empty());
  CHECK(!not_vtt.issues.empty());
}

CUTLINE_TEST(ATimeThatIsNotAWholeMillisecondIsRoundedAndTheWriterSaysSo) {
  // One frame at 30 fps: 33.33 ms.
  std::vector<cap::Cue> cues(1);
  cues[0].start = RationalTime(1, 30);
  cues[0].end = RationalTime(2, 30);
  cues[0].text = "Frame cue";
  const auto srt = cap::WriteSrt(cues);
  CHECK_EQ(srt.issues.size(), std::size_t{1});
  CHECK(srt.issues[0].message.find("rounded") != std::string::npos);
  CHECK(srt.text.find("00:00:00,033 --> 00:00:00,067") != std::string::npos);
  // Styles beyond position are not carried by the formats, and that is said too.
  cap::Cue styled = MakeCue(0, 1000, "Big");
  styled.style_json = "{\"size\":0.2,\"color\":[1,0,0,1]}";
  const auto result = cap::WriteSrt({styled});
  CHECK(!result.issues.empty());
  CHECK(result.issues[0].message.find("styling") != std::string::npos);
}

CUTLINE_TEST(OnlyTheCuesShowingAtATimeAreActiveAndOverlappingOnesShowTogether) {
  cap::Track english, french;
  english.id = "en";
  english.cues = {MakeCue(1000, 3000, "one"), MakeCue(2000, 4000, "two"), MakeCue(5000, 6000, "three")};
  french.id = "fr";
  french.cues = {MakeCue(1500, 2500, "un")};
  const std::vector<cap::Track> tracks{english, french};
  const auto at = [&](std::int64_t ms) { return cap::ActiveCues(tracks, RationalTime(ms, 1000)); };
  CHECK(at(999).empty());
  CHECK_EQ(at(1000).size(), std::size_t{1});          // the start is inside
  CHECK_EQ(at(2200).size(), std::size_t{3});          // en one, en two, fr un
  CHECK_EQ(at(2200)[0].cue->text, std::string("one"));  // track order, then start order
  CHECK_EQ(at(2200)[2].track->id, std::string("fr"));
  CHECK_EQ(at(3000).size(), std::size_t{1});          // the end is outside: only "two"
  CHECK(at(4000).empty());
  CHECK_EQ(at(5999).size(), std::size_t{1});
}

CUTLINE_TEST(StylesAreFractionsOfThePictureAndBadOnesFallBackToTheDefaultWithoutFailing) {
  const auto defaults = cap::ParseStyle("{}");
  CHECK_EQ(defaults.family, std::string("Arial"));
  CHECK(std::abs(defaults.size - 0.055) < 1e-12);
  CHECK_EQ(defaults.position, std::string("bottom"));
  // Out-of-range values are clamped, unknown names fall back, unreadable JSON is the default.
  const auto odd = cap::ParseStyle("{\"size\":9,\"align\":\"diagonal\",\"color\":[2,0,0],\"maxWidth\":0}");
  CHECK(odd.size <= 0.5 && odd.max_width >= 0.1);
  CHECK_EQ(odd.align, std::string("center"));
  CHECK(odd.color[0] == 1.0 && odd.color[3] == 1.0);
  CHECK(cap::ParseStyle("not json").size == defaults.size);
  // A cue's style is laid over its track's, field by field.
  const auto merged = cap::MergeStyles("{\"size\":0.08,\"bold\":true,\"position\":\"top\"}", "{\"size\":0.03,\"align\":\"right\"}");
  const auto style = cap::ParseStyle(merged);
  CHECK(std::abs(style.size - 0.03) < 1e-12);
  CHECK(style.bold);
  CHECK_EQ(style.position, std::string("top"));
  CHECK_EQ(style.align, std::string("right"));
  // A full style survives a trip through its JSON.
  cap::Style full;
  full.family = "Georgia";
  full.size = 0.07;
  full.italic = true;
  full.color[1] = 0.25;
  full.outline_width = 0.004;
  full.max_width = 0.6;
  const auto back = cap::ParseStyle(cap::StyleToJson(full));
  CHECK(back.family == "Georgia" && back.italic && std::abs(back.color[1] - 0.25) < 1e-12 && std::abs(back.outline_width - 0.004) < 1e-12 &&
        std::abs(back.max_width - 0.6) < 1e-12);
  // The safe areas are the middle ninety and eighty percent.
  const auto action = cap::ActionSafe(1920, 1080);
  CHECK(std::abs(action.x - 96) < 1e-9 && std::abs(action.width - 1728) < 1e-9 && std::abs(action.height - 972) < 1e-9);
  const auto title = cap::TitleSafe(1920, 1080);
  CHECK(std::abs(title.x - 192) < 1e-9 && std::abs(title.y - 108) < 1e-9 && std::abs(title.width - 1536) < 1e-9);
}

CUTLINE_TEST(SidecarFilesAreWrittenPerTrackAndLanguageAndReadBackTheSame) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-sidecars";
  std::filesystem::remove_all(directory);
  cap::Track english, french, untitled;
  english.id = "t-en";
  english.language = "en";
  english.cues = {MakeCue(1000, 2000, "Hello")};
  french.id = "t-fr";
  french.language = "fr";
  french.cues = {MakeCue(1000, 2000, "Bonjour")};
  untitled.id = "t-x";
  untitled.language = "en";  // a second track in English must not overwrite the first
  untitled.cues = {MakeCue(3000, 4000, "Again")};
  const auto written = cap::WriteSidecars({english, french, untitled}, directory.string(), "film");
  CHECK_EQ(written.size(), std::size_t{6});
  CHECK(std::filesystem::exists(directory / "film.en.srt"));
  CHECK(std::filesystem::exists(directory / "film.fr.vtt"));
  CHECK(std::filesystem::exists(directory / "film.en.3.srt"));
  {
    std::ifstream file(directory / "film.fr.srt", std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    CHECK(SameCues(french.cues, cap::ParseSrt(text.str()).cues));
  }
  const auto only_srt = cap::WriteSidecars({english}, (directory / "only").string(), "film", true, false);
  CHECK_EQ(only_srt.size(), std::size_t{1});
  std::filesystem::remove_all(directory);
}

int main() { return cutline::testing::RunAll("interchange"); }

#include "ui/Multicam.h"

#include "media/Source.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <set>
#include <stdexcept>

namespace cutline::ui {
namespace {

using time::RationalTime;
namespace mc = timeline::multicam;

constexpr int kMaxAngles = 16;
constexpr int kEnvelopeRate = 100;

RationalTime Frame(time::FrameRate rate) { return RationalTime::FromFrames(1, rate); }

RationalTime SnapToFrame(const RationalTime& t, time::FrameRate rate) {
  return RationalTime::FromFrames(std::max<std::int64_t>(0, t.ToFrames(rate, time::RoundingMode::Nearest)), rate);
}

const mc::Angle* AngleAt(const mc::Group& group, int index) {
  if (index < 0 || index >= static_cast<int>(group.angles.size())) return nullptr;
  return &group.angles[static_cast<std::size_t>(index)];
}

std::string MethodName(SyncChoice sync) {
  switch (sync) {
    case SyncChoice::Timecode: return "timecode";
    case SyncChoice::Marker: return "marker";
    case SyncChoice::Audio: return "audio";
    case SyncChoice::Manual: break;
  }
  return "manual";
}

}  // namespace

std::vector<std::string> AngleNames(const std::vector<AngleDraft>& drafts, const std::vector<std::string>& clip_names) {
  std::vector<std::string> names;
  std::set<std::string> used;
  for (std::size_t i = 0; i < drafts.size(); ++i) {
    std::string name = drafts[i].name;
    if (name.empty() && i < clip_names.size()) name = clip_names[i];
    if (name.empty()) name = "Angle " + std::to_string(i + 1);
    std::string unique = name;
    for (int n = 2; used.count(unique) != 0; ++n) unique = name + " " + std::to_string(n);
    used.insert(unique);
    names.push_back(unique);
  }
  return names;
}

SetupResult PlanCreateGroup(const GroupSetup& setup, const std::vector<std::string>& clip_names,
                            const std::function<std::string(const std::string& prefix)>& new_id) {
  SetupResult result;
  const auto refuse = [&](std::string why) {
    result.plan = EditPlan::Refuse(std::move(why));
    return result;
  };
  const int count = static_cast<int>(setup.angles.size());
  if (count < 2) return refuse("A multicam group needs at least two clips");
  if (count > kMaxAngles) return refuse("A multicam group holds at most " + std::to_string(kMaxAngles) + " angles");
  if (setup.reference < 0 || setup.reference >= count) return refuse("The reference angle is not one of the clips");
  std::set<std::string> media;
  for (std::size_t i = 0; i < setup.angles.size(); ++i) {
    const auto& draft = setup.angles[i];
    if (draft.media_id.empty()) return refuse("An angle has no clip");
    if (!media.insert(draft.media_id).second) return refuse("The same clip is used for two angles");
    if (!draft.has_video) return refuse("Angle " + std::to_string(i + 1) + " has no picture");
    if (draft.duration.numerator() <= 0) return refuse("Angle " + std::to_string(i + 1) + " has no length");
  }

  mc::Group group;
  group.id = new_id("mc");
  group.name = setup.name.empty() ? "Multicam" : setup.name;
  const auto names = AngleNames(setup.angles, clip_names);
  for (int i = 0; i < count; ++i) {
    const auto& draft = setup.angles[static_cast<std::size_t>(i)];
    mc::Angle angle;
    angle.id = new_id("angle");
    angle.name = names[static_cast<std::size_t>(i)];
    angle.source_id = draft.media_id;
    angle.duration = draft.duration;
    angle.timecode_start = draft.timecode_start;
    angle.marker = draft.marker;
    angle.audio_envelope = draft.envelope;
    angle.envelope_rate = kEnvelopeRate;
    group.angles.push_back(std::move(angle));
    result.angle_ids.push_back(group.angles.back().id);
    if (group.duration.Compare(draft.duration) < 0) group.duration = draft.duration;
  }
  const auto& reference = group.angles[static_cast<std::size_t>(setup.reference)];
  group.reference_angle_id = reference.id;

  try {
    switch (setup.sync) {
      case SyncChoice::Manual: {
        const auto base = setup.angles[static_cast<std::size_t>(setup.reference)].offset;
        for (int i = 0; i < count; ++i) group.angles[static_cast<std::size_t>(i)].source_offset = setup.angles[static_cast<std::size_t>(i)].offset.Subtract(base);
        break;
      }
      case SyncChoice::Timecode:
        result.confidence = mc::Synchronize(group, mc::SyncMethod::Timecode, reference.id);
        break;
      case SyncChoice::Marker:
        result.confidence = mc::Synchronize(group, mc::SyncMethod::Marker, reference.id);
        break;
      case SyncChoice::Audio:
        for (const auto& angle : group.angles) {
          if (angle.audio_envelope.empty()) return refuse("Angle " + angle.name + " has no sound to line up by");
        }
        result.confidence = mc::Synchronize(group, mc::SyncMethod::Audio, reference.id);
        break;
    }
  } catch (const std::exception& error) {
    return refuse(error.what());
  }

  // The group runs from the reference's start to the last end of any angle.
  RationalTime end(0, 1);
  for (const auto& angle : group.angles) {
    const auto angle_end = angle.duration.Subtract(angle.source_offset);
    if (end.Compare(angle_end) < 0) end = angle_end;
  }
  if (end.numerator() <= 0) return refuse("The angles do not overlap the reference");
  group.duration = end;

  commands::CreateMulticamGroupPayload create;
  create.id = group.id;
  create.name = group.name;
  create.duration = group.duration;
  for (const auto& angle : group.angles) create.angles.push_back({angle.id, angle.source_id, angle.name, angle.marker});
  commands::SetMulticamSyncPayload sync;
  sync.group_id = group.id;
  sync.method = MethodName(setup.sync);
  sync.reference_angle_id = reference.id;
  sync.confidence = result.confidence;
  for (const auto& angle : group.angles) sync.offsets.push_back({angle.id, angle.source_offset});
  for (const auto& angle : group.angles) result.offsets.push_back(angle.source_offset);

  result.plan.ok = true;
  result.plan.label = "Create Multicam Group";
  result.plan.commands.push_back({commands::CommandType::CreateMulticamGroup, create});
  result.plan.commands.push_back({commands::CommandType::SetMulticamSync, sync});
  result.plan.commands.push_back({commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{group.id, RationalTime(0, 1), reference.id}});
  result.group_id = group.id;
  if (setup.sync == SyncChoice::Audio && result.confidence < 0.3) {
    result.plan.notes.push_back("The sound matched poorly (" + std::to_string(static_cast<int>(result.confidence * 100)) + "%); check the sync before cutting");
  }
  return result;
}

std::vector<float> AudioEnvelope(media::Source& source, const RationalTime& limit) {
  if (source.probe().PrimaryAudio() == nullptr) return {};
  constexpr std::int64_t kRate = 8000;
  constexpr std::int64_t kHop = kRate / kEnvelopeRate;
  const double seconds = std::min(static_cast<double>(limit.numerator()) / static_cast<double>(limit.denominator()),
                                  static_cast<double>(source.probe().duration.numerator()) / static_cast<double>(source.probe().duration.denominator()));
  std::vector<float> envelope;
  for (std::int64_t second = 0; static_cast<double>(second) < seconds; ++second) {
    auto block = source.ReadAudio(RationalTime(second, 1), kRate, 1, kRate);
    if (!block || !block->valid()) break;
    const float* samples = block->channel(0);
    for (std::int64_t start = 0; start + kHop <= block->frames(); start += kHop) {
      double sum = 0.0;
      for (std::int64_t i = 0; i < kHop; ++i) sum += static_cast<double>(samples[start + i]) * samples[start + i];
      envelope.push_back(static_cast<float>(std::sqrt(sum / static_cast<double>(kHop))));
    }
  }
  return envelope;
}

// ------------------------------------------------------------------------ cuts ----

const mc::Angle* ShowingAt(const mc::Group& group, const RationalTime& at) { return mc::ActiveAngle(group, at); }

bool HasPictureAt(const mc::Angle& angle, const RationalTime& at) {
  const auto source = angle.source_offset.Add(at);
  return source.Compare(RationalTime(0, 1)) >= 0 && source.Compare(angle.duration) < 0;
}

EditPlan PlanCutTo(const mc::Group& group, int angle_index, const RationalTime& at, time::FrameRate rate) {
  const auto* angle = AngleAt(group, angle_index);
  if (angle == nullptr) return EditPlan::Refuse("There is no angle " + std::to_string(angle_index + 1));
  const auto when = SnapToFrame(at, rate);
  if (when.Compare(group.duration) >= 0) return EditPlan::Refuse("That is past the end of the group");
  if (!HasPictureAt(*angle, when)) return EditPlan::Refuse(angle->name + " has no picture at that time");
  EditPlan plan;
  plan.ok = true;
  plan.label = "Cut to " + angle->name;
  plan.result_time = when;
  if (const auto* showing = mc::ActiveAngle(group, when); showing != nullptr && showing->id == angle->id) {
    plan.notes.push_back(angle->name + " is already showing");
    return plan;  // nothing to do
  }
  plan.commands.push_back({commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{group.id, when, angle->id}});
  return plan;
}

namespace {

// The cut at exactly this time, with the cuts on either side of it.
struct CutAt final {
  std::size_t index{0};
  std::optional<RationalTime> before, after;
};

std::optional<CutAt> FindCut(const mc::Group& group, const RationalTime& at) {
  for (std::size_t i = 0; i < group.switches.size(); ++i) {
    if (group.switches[i].timeline_time.Compare(at) != 0) continue;
    CutAt found;
    found.index = i;
    if (i > 0) found.before = group.switches[i - 1].timeline_time;
    if (i + 1 < group.switches.size()) found.after = group.switches[i + 1].timeline_time;
    return found;
  }
  return std::nullopt;
}

}  // namespace

EditPlan PlanMoveCut(const mc::Group& group, const RationalTime& from, const RationalTime& to, time::FrameRate rate) {
  const auto cut = FindCut(group, from);
  if (!cut) return EditPlan::Refuse("There is no cut there");
  if (cut->index == 0) return EditPlan::Refuse("The first cut is where the programme starts; it cannot be moved");
  const auto frame = Frame(rate);
  auto target = SnapToFrame(to, rate);
  if (cut->before && target.Compare(cut->before->Add(frame)) < 0) target = cut->before->Add(frame);
  const auto limit = cut->after ? *cut->after : group.duration;
  if (target.Compare(limit.Subtract(frame)) > 0) target = limit.Subtract(frame);
  if (target.Compare(cut->before.value_or(RationalTime(0, 1))) <= 0) return EditPlan::Refuse("There is no room to move the cut");
  EditPlan plan;
  plan.ok = true;
  plan.label = "Move Multicam Cut";
  plan.result_time = target;
  if (target.Compare(from) == 0) return plan;
  const auto& angle_id = group.switches[cut->index].angle_id;
  const auto* angle = [&]() -> const mc::Angle* {
    for (const auto& a : group.angles) {
      if (a.id == angle_id) return &a;
    }
    return nullptr;
  }();
  if (angle == nullptr || !HasPictureAt(*angle, target)) return EditPlan::Refuse((angle != nullptr ? angle->name : angle_id) + " has no picture at that time");
  plan.commands.push_back({commands::CommandType::RemoveMulticamSwitch, commands::RemoveMulticamSwitchPayload{group.id, from}});
  plan.commands.push_back({commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{group.id, target, angle_id}});
  return plan;
}

EditPlan PlanNudgeCut(const mc::Group& group, const RationalTime& at, int frames, time::FrameRate rate) {
  return PlanMoveCut(group, at, at.Add(RationalTime::FromFrames(frames, rate)), rate);
}

EditPlan PlanChangeCutAngle(const mc::Group& group, const RationalTime& at, int angle_index) {
  const auto cut = FindCut(group, at);
  if (!cut) return EditPlan::Refuse("There is no cut there");
  const auto* angle = AngleAt(group, angle_index);
  if (angle == nullptr) return EditPlan::Refuse("There is no angle " + std::to_string(angle_index + 1));
  if (!HasPictureAt(*angle, at)) return EditPlan::Refuse(angle->name + " has no picture at that time");
  EditPlan plan;
  plan.ok = true;
  plan.label = "Change Multicam Cut";
  if (group.switches[cut->index].angle_id == angle->id) return plan;
  plan.commands.push_back({commands::CommandType::RecordMulticamSwitch, commands::RecordMulticamSwitchPayload{group.id, at, angle->id}});
  return plan;
}

EditPlan PlanRemoveCut(const mc::Group& group, const RationalTime& at) {
  const auto cut = FindCut(group, at);
  if (!cut) return EditPlan::Refuse("There is no cut there");
  if (cut->index == 0) return EditPlan::Refuse("The first cut is where the programme starts; change its angle instead");
  EditPlan plan;
  plan.ok = true;
  plan.label = "Remove Multicam Cut";
  plan.commands.push_back({commands::CommandType::RemoveMulticamSwitch, commands::RemoveMulticamSwitchPayload{group.id, at}});
  return plan;
}

EditPlan PlanRenameAngle(const mc::Group& group, int angle_index, const std::string& name) {
  const auto* angle = AngleAt(group, angle_index);
  if (angle == nullptr) return EditPlan::Refuse("There is no angle " + std::to_string(angle_index + 1));
  if (name.empty() || name.size() > 120) return EditPlan::Refuse("An angle name must be 1 to 120 characters");
  for (const auto& other : group.angles) {
    if (other.id != angle->id && other.name == name) return EditPlan::Refuse("Another angle is already called " + name);
  }
  EditPlan plan;
  plan.ok = true;
  plan.label = "Rename Angle";
  if (angle->name != name) plan.commands.push_back({commands::CommandType::RenameMulticamAngle, commands::RenameMulticamAnglePayload{group.id, angle->id, name}});
  return plan;
}

EditPlan PlanNudgeSync(const mc::Group& group, int angle_index, const RationalTime& delta) {
  const auto* angle = AngleAt(group, angle_index);
  if (angle == nullptr) return EditPlan::Refuse("There is no angle " + std::to_string(angle_index + 1));
  if (angle->id == group.reference_angle_id) return EditPlan::Refuse("The reference angle is what the others are lined up to; nudge another");
  commands::SetMulticamSyncPayload sync;
  sync.group_id = group.id;
  sync.method = "manual";
  sync.reference_angle_id = group.reference_angle_id;
  sync.confidence = 1.0;
  for (const auto& a : group.angles) sync.offsets.push_back({a.id, a.id == angle->id ? a.source_offset.Add(delta) : a.source_offset});
  EditPlan plan;
  plan.ok = true;
  plan.label = "Nudge Angle Sync";
  plan.commands.push_back({commands::CommandType::SetMulticamSync, sync});
  return plan;
}

EditPlan PlanFlatten(const EditContext& ctx, const mc::Group& group, const std::string& video_track, const std::string& audio_track,
                     AudioSource audio, int audio_angle_index, const RationalTime& at) {
  if (group.switches.empty()) return EditPlan::Refuse("Cut the group first: it has no cuts to lay out");
  if (ctx.sequence == nullptr) return EditPlan::Refuse("There is no sequence");
  const auto* video = ctx.sequence->FindTrack(video_track);
  if (video == nullptr || video->kind != model::TrackKind::Video) return EditPlan::Refuse("Pick a video track for the picture");
  if (video->locked) return EditPlan::Refuse("The track " + video_track + " is locked");
  if (!audio_track.empty()) {
    const auto* sound = ctx.sequence->FindTrack(audio_track);
    if (sound == nullptr || sound->kind != model::TrackKind::Audio) return EditPlan::Refuse("Pick an audio track for the sound");
    if (sound->locked) return EditPlan::Refuse("The track " + audio_track + " is locked");
  }
  // The laid-out programme is one run from `at` to the end of the group; it must not land on clips already there.
  const auto run_end = at.Add(group.duration);
  for (const auto& track_id : {video_track, audio_track}) {
    const auto* track = track_id.empty() ? nullptr : ctx.sequence->FindTrack(track_id);
    if (track == nullptr) continue;
    for (const auto& clip : track->clips) {
      if (clip.timeline_start.Compare(run_end) < 0 && clip.end().Compare(at) > 0) return EditPlan::Refuse("The track " + track_id + " already has a clip in that stretch");
    }
  }
  commands::FlattenMulticamGroupPayload payload;
  payload.group_id = group.id;
  payload.video_track_id = video_track;
  payload.audio_track_id = audio_track;
  payload.timeline_start = at;
  payload.id_prefix = ctx.new_id ? ctx.new_id("flat") : group.id;
  if (audio == AudioSource::OneAngle) {
    const auto* angle = AngleAt(group, audio_angle_index);
    if (angle == nullptr) return EditPlan::Refuse("There is no angle " + std::to_string(audio_angle_index + 1) + " to take the sound from");
    payload.audio_angle_id = angle->id;
  }
  EditPlan plan;
  plan.ok = true;
  plan.label = "Lay Out Multicam";
  plan.commands.push_back({commands::CommandType::FlattenMulticamGroup, payload});
  return plan;
}

// ------------------------------------------------------------------ the monitor ----

AngleMonitor::AngleMonitor(Locator locator) : locator_(std::move(locator)) {}
AngleMonitor::~AngleMonitor() = default;

media::Source* AngleMonitor::SourceFor(const std::string& media_id) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto found = sources_.find(media_id);
  if (found != sources_.end()) return found->second.get();
  const auto path = locator_ ? locator_(media_id) : std::string();
  std::unique_ptr<media::Source> source;
  if (!path.empty()) source = media::SourceRegistry::Instance().Open(path);
  auto* raw = source.get();
  sources_[media_id] = std::move(source);  // offline stays offline: an empty entry is remembered
  return raw;
}

media::VideoFrame AngleMonitor::Render(const mc::Group& group, const RationalTime& at, const render::AngleMonitorConfig& config) {
  const auto* showing = mc::ActiveAngle(group, at);
  // Each angle is read on its own thread: one slow file does not hold the others up. A source is touched by one
  // thread only, since an angle is one source.
  std::vector<std::future<std::optional<media::VideoFrame>>> reads;
  for (const auto& angle : group.angles) {
    auto* source = SourceFor(angle.source_id);
    const auto source_time = angle.source_offset.Add(at);
    const bool present = source != nullptr && HasPictureAt(angle, at);
    reads.push_back(std::async(std::launch::async, [source, source_time, present]() -> std::optional<media::VideoFrame> {
      if (!present) return std::nullopt;
      try {
        return source->ReadVideo(source_time);
      } catch (...) {
        return std::nullopt;
      }
    }));
  }
  std::vector<std::optional<media::VideoFrame>> frames;
  for (auto& read : reads) frames.push_back(read.get());
  std::vector<render::MonitorTile> tiles;
  for (std::size_t i = 0; i < group.angles.size(); ++i) {
    tiles.push_back({frames[i] && frames[i]->valid() ? &*frames[i] : nullptr, showing != nullptr && showing->id == group.angles[i].id});
  }
  return render::ComposeAngleMonitor(tiles, config);
}

}  // namespace cutline::ui

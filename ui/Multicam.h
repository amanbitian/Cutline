#pragma once

// The multicam workflow as a person does it, in terms of plans the command bus can run:
//
//   * setting a group up from a set of clips: which are angles, what each is called, how they are lined up (by
//     timecode, by a marker on each, by comparing their sound, or by hand), and which one is the reference;
//   * cutting live: a key or a click on an angle's tile cuts to that angle at the playhead, also while playing;
//   * refining the cuts afterwards: move a cut, nudge it a frame at a time, give a cut another angle, delete it;
//   * putting the result on the timeline as ordinary clips, with the sound following the picture or taken from one
//     angle throughout;
//   * and the angle monitor: every angle's picture at one instant in one frame, with the live angle marked.
//
// Nothing here knows about Qt. The group itself and the sync arithmetic are timeline/Multicam.h.

#include "core/commands/Command.h"
#include "render/AngleMonitor.h"
#include "timeline/Multicam.h"
#include "ui/EditPlanner.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cutline::media {
class Source;
}

namespace cutline::ui {

// One clip offered as an angle, with what the setup needs to know about it.
struct AngleDraft final {
  std::string media_id;
  std::string name;  // empty: named from the clip
  time::RationalTime duration;
  bool has_video{true};
  bool has_audio{false};
  time::RationalTime timecode_start{0, 1};
  std::optional<time::RationalTime> marker;  // a clap or slate, as a time in the clip
  // For manual sync: the time in this clip that lines up with the start of the group.
  time::RationalTime offset{0, 1};
  // For audio sync: a low-rate envelope of the sound (see AudioEnvelope).
  std::vector<float> envelope;
};

enum class SyncChoice { Manual, Timecode, Marker, Audio };

struct GroupSetup final {
  std::string name;
  std::vector<AngleDraft> angles;
  SyncChoice sync{SyncChoice::Manual};
  int reference{0};  // the angle everything else is lined up to, and the first one shown
};

struct SetupResult final {
  EditPlan plan;
  std::string group_id;
  std::vector<std::string> angle_ids;  // in the order of the setup
  double confidence{1.0};
  // The source offset found for each angle, in the order of the setup.
  std::vector<time::RationalTime> offsets;
};

// The names a setup gives its angles: what the person typed, else the clip's name, else "Angle N"; made unique.
[[nodiscard]] std::vector<std::string> AngleNames(const std::vector<AngleDraft>& drafts, const std::vector<std::string>& clip_names);

// A group from these clips: it is created, its angles lined up, and it starts on the reference angle. One undo step.
[[nodiscard]] SetupResult PlanCreateGroup(const GroupSetup& setup, const std::vector<std::string>& clip_names,
                                          const std::function<std::string(const std::string& prefix)>& new_id);

// The envelope of a clip's sound for audio sync: the loudness over each hundredth of a second, read from the first
// `limit` of the clip (a minute or two is plenty to find a clap). Empty when the clip has no sound.
[[nodiscard]] std::vector<float> AudioEnvelope(media::Source& source, const time::RationalTime& limit);

// ---- cuts
// The angle showing at a time, or nullptr before the first cut.
[[nodiscard]] const timeline::multicam::Angle* ShowingAt(const timeline::multicam::Group& group, const time::RationalTime& at);
// Whether an angle has a picture at a group time (it may start later or end sooner than the group).
[[nodiscard]] bool HasPictureAt(const timeline::multicam::Angle& angle, const time::RationalTime& at);
// Cut to an angle (by position in the group) at a time, snapped to a frame. A cut to what is already showing is a
// plan that does nothing, with a note.
[[nodiscard]] EditPlan PlanCutTo(const timeline::multicam::Group& group, int angle_index, const time::RationalTime& at, time::FrameRate rate);
// Move the cut at `from` to `to`, snapped to a frame; it cannot pass the cuts on either side or the start.
[[nodiscard]] EditPlan PlanMoveCut(const timeline::multicam::Group& group, const time::RationalTime& from, const time::RationalTime& to, time::FrameRate rate);
// The same, by whole frames (negative earlier).
[[nodiscard]] EditPlan PlanNudgeCut(const timeline::multicam::Group& group, const time::RationalTime& at, int frames, time::FrameRate rate);
// Show a different angle from this cut on, keeping its time.
[[nodiscard]] EditPlan PlanChangeCutAngle(const timeline::multicam::Group& group, const time::RationalTime& at, int angle_index);
// Take the cut away: the angle before it carries on. The first cut cannot be removed, only changed.
[[nodiscard]] EditPlan PlanRemoveCut(const timeline::multicam::Group& group, const time::RationalTime& at);
[[nodiscard]] EditPlan PlanRenameAngle(const timeline::multicam::Group& group, int angle_index, const std::string& name);
// Moves one angle against the others by `delta` (its source time that lines up with the start of the group moves by
// it), for when the automatic sync is a frame out. Recorded as a manual sync.
[[nodiscard]] EditPlan PlanNudgeSync(const timeline::multicam::Group& group, int angle_index, const time::RationalTime& delta);

enum class AudioSource { FollowsPicture, OneAngle };
// Lays the cuts out as ordinary clips on a video track (and the sound on an audio track, or none). The picture of the
// sequence keeps its multicam group so the angles stay reachable.
[[nodiscard]] EditPlan PlanFlatten(const EditContext& ctx, const timeline::multicam::Group& group, const std::string& video_track,
                                   const std::string& audio_track, AudioSource audio, int audio_angle_index, const time::RationalTime& at);

// ---- the angle monitor
// Reads every angle's picture at a group time and tiles them. Sources are opened once and kept.
class AngleMonitor final {
 public:
  // media id -> a path a provider can open; empty when offline.
  using Locator = std::function<std::string(const std::string& media_id)>;
  explicit AngleMonitor(Locator locator);
  ~AngleMonitor();

  // The tiled picture at a time. The angle showing at that time is marked.
  [[nodiscard]] media::VideoFrame Render(const timeline::multicam::Group& group, const time::RationalTime& at, const render::AngleMonitorConfig& config);
  [[nodiscard]] static int AngleAt(std::size_t count, int x, int y, const render::AngleMonitorConfig& config) { return render::AngleAtPoint(count, x, y, config); }

 private:
  [[nodiscard]] media::Source* SourceFor(const std::string& media_id);
  Locator locator_;
  std::mutex mutex_;
  std::map<std::string, std::unique_ptr<media::Source>> sources_;
};

}  // namespace cutline::ui

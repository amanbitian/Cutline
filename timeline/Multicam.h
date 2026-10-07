#pragma once

// Offline multicamera synchronisation, angle monitoring, live switch recording,
// and destructive flattening into ordinary timeline clips.

#include "core/project/ProjectStore.h"
#include "timeline/Sequence.h"
#include "timeline/TimelineCompiler.h"

#include <optional>
#include <string>
#include <vector>

namespace cutline::timeline::multicam {

enum class SyncMethod { Timecode, Marker, Audio };

struct Angle final {
  std::string id;
  std::string name;
  model::SourceKind source_kind{model::SourceKind::Media};
  std::string source_id;
  time::RationalTime duration;
  // Absolute timecode at source time zero, or the source-local sync marker.
  time::RationalTime timecode_start;
  std::optional<time::RationalTime> marker;
  // Low-rate mono envelope used only for correlation. It is stored/analysed
  // locally and never sent to a service.
  std::vector<float> audio_envelope;
  int envelope_rate{100};
  // Source time corresponding to group timeline zero, filled by Synchronize.
  time::RationalTime source_offset;
};

struct Switch final {
  time::RationalTime timeline_time;
  std::string angle_id;
};

struct Group final {
  std::string id;
  std::string name;
  // How the angles were lined up and how sure that was (0 to 1); "none" until a sync is stored.
  std::string sync_method{"none"};
  std::string reference_angle_id;
  double sync_confidence{0.0};
  time::RationalTime duration;
  std::vector<Angle> angles;
  std::vector<Switch> switches;
};

// Aligns all angles to `reference_angle_id`. Audio uses normalised
// cross-correlation and returns a confidence; timecode/marker return 1.
[[nodiscard]] double Synchronize(Group& group, SyncMethod method, const std::string& reference_angle_id,
                                 int maximum_audio_lag_samples = 1000);

// Adds/replaces a live cut and coalesces adjacent cuts to the same angle.
void RecordSwitch(Group& group, time::RationalTime at, const std::string& angle_id);
[[nodiscard]] const Angle* ActiveAngle(const Group& group, const time::RationalTime& at);

// Every valid angle at one instant, for an angle-monitor grid.
[[nodiscard]] std::vector<SourceRequest> MonitorRequests(const Group& group, const time::RationalTime& at);

// Resolves recorded switches to ordinary clips suitable for insertion on a
// normal video/audio track. The multicam relationship is intentionally gone.
[[nodiscard]] std::vector<Clip> Flatten(const Group& group);

// What the project stores: every group id, and one group with its angles, offsets and cuts.
// Audio envelopes are analysis input and are not stored; sync is stored as its result.
[[nodiscard]] std::vector<std::string> ListGroupIds(const project::ProjectStore& store);
[[nodiscard]] Group LoadGroup(const project::ProjectStore& store, const std::string& group_id);

// Each angle id with its source offset, the shape SetMulticamSync stores.
[[nodiscard]] std::vector<std::pair<std::string, time::RationalTime>> Offsets(const Group& group);

}  // namespace cutline::timeline::multicam

#include "timeline/Multicam.h"

#include "core/db/Sql.h"

#include <algorithm>
#include <mutex>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace cutline::timeline::multicam {
namespace {

[[nodiscard]] Angle* FindAngle(Group& group, const std::string& id) {
  for (auto& angle : group.angles) if (angle.id == id) return &angle;
  return nullptr;
}

[[nodiscard]] const Angle* FindAngle(const Group& group, const std::string& id) {
  for (const auto& angle : group.angles) if (angle.id == id) return &angle;
  return nullptr;
}

[[nodiscard]] double Correlation(const std::vector<float>& reference, const std::vector<float>& candidate,
                                 int lag) {
  const auto ref_start = std::max(0, -lag);
  const auto candidate_start = std::max(0, lag);
  const auto count = std::min(static_cast<int>(reference.size()) - ref_start,
                              static_cast<int>(candidate.size()) - candidate_start);
  if (count < 4) return -1.0;
  double ref_mean = 0.0, candidate_mean = 0.0;
  for (int i = 0; i < count; ++i) {
    ref_mean += reference[static_cast<std::size_t>(ref_start + i)];
    candidate_mean += candidate[static_cast<std::size_t>(candidate_start + i)];
  }
  ref_mean /= count;
  candidate_mean /= count;
  double numerator = 0.0, ref_energy = 0.0, candidate_energy = 0.0;
  for (int i = 0; i < count; ++i) {
    const auto a = reference[static_cast<std::size_t>(ref_start + i)] - ref_mean;
    const auto b = candidate[static_cast<std::size_t>(candidate_start + i)] - candidate_mean;
    numerator += a * b;
    ref_energy += a * a;
    candidate_energy += b * b;
  }
  const auto denominator = std::sqrt(ref_energy * candidate_energy);
  return denominator > 1e-12 ? numerator / denominator : -1.0;
}

void ValidateGroup(const Group& group) {
  if (group.id.empty() || group.duration.numerator() <= 0 || group.angles.empty()) {
    throw std::invalid_argument("Multicam group header is invalid");
  }
  for (const auto& angle : group.angles) {
    if (angle.id.empty() || angle.source_id.empty() || angle.duration.numerator() <= 0) {
      throw std::invalid_argument("Multicam angle is invalid");
    }
  }
}

}  // namespace

double Synchronize(Group& group, SyncMethod method, const std::string& reference_angle_id,
                   int maximum_audio_lag_samples) {
  ValidateGroup(group);
  auto* reference = FindAngle(group, reference_angle_id);
  if (reference == nullptr) throw std::invalid_argument("Multicam reference angle does not exist");
  reference->source_offset = {0, 1};
  double confidence = 1.0;
  for (auto& angle : group.angles) {
    if (&angle == reference) continue;
    if (method == SyncMethod::Timecode) {
      angle.source_offset = reference->timecode_start.Subtract(angle.timecode_start);
    } else if (method == SyncMethod::Marker) {
      if (!reference->marker.has_value() || !angle.marker.has_value()) {
        throw std::invalid_argument("Marker sync requires a marker on every angle");
      }
      angle.source_offset = angle.marker->Subtract(*reference->marker);
    } else {
      if (reference->envelope_rate <= 0 || angle.envelope_rate != reference->envelope_rate ||
          reference->audio_envelope.empty() || angle.audio_envelope.empty()) {
        throw std::invalid_argument("Audio sync requires non-empty envelopes at the same rate");
      }
      const auto limit = std::max(0, maximum_audio_lag_samples);
      int best_lag = 0;
      double best = -2.0;
      for (int lag = -limit; lag <= limit; ++lag) {
        const auto score = Correlation(reference->audio_envelope, angle.audio_envelope, lag);
        if (score > best) { best = score; best_lag = lag; }
      }
      // A positive lag means the matching material occurs later in the
      // candidate, so start that source later at group time zero.
      angle.source_offset = time::RationalTime(best_lag, reference->envelope_rate);
      confidence = std::min(confidence, std::max(0.0, best));
    }
  }
  return confidence;
}

void RecordSwitch(Group& group, time::RationalTime at, const std::string& angle_id) {
  ValidateGroup(group);
  if (FindAngle(group, angle_id) == nullptr) throw std::invalid_argument("Multicam switch names an unknown angle");
  if (at.Compare({0, 1}) < 0 || at.Compare(group.duration) >= 0) throw std::invalid_argument("Multicam switch is outside the group");
  const auto position = std::lower_bound(group.switches.begin(), group.switches.end(), at,
                                         [](const Switch& item, const auto& time) { return item.timeline_time.Compare(time) < 0; });
  if (position != group.switches.end() && position->timeline_time.Compare(at) == 0) {
    position->angle_id = angle_id;
  } else {
    group.switches.insert(position, {at, angle_id});
  }
  for (std::size_t index = 1; index < group.switches.size();) {
    if (group.switches[index - 1].angle_id == group.switches[index].angle_id) {
      group.switches.erase(group.switches.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
      ++index;
    }
  }
}

const Angle* ActiveAngle(const Group& group, const time::RationalTime& at) {
  if (group.switches.empty()) return nullptr;
  const auto position = std::upper_bound(group.switches.begin(), group.switches.end(), at,
                                         [](const auto& time, const Switch& item) { return time.Compare(item.timeline_time) < 0; });
  if (position == group.switches.begin()) return nullptr;
  return FindAngle(group, std::prev(position)->angle_id);
}

std::vector<SourceRequest> MonitorRequests(const Group& group, const time::RationalTime& at) {
  ValidateGroup(group);
  std::vector<SourceRequest> requests;
  for (std::size_t index = 0; index < group.angles.size(); ++index) {
    const auto& angle = group.angles[index];
    const auto source_time = angle.source_offset.Add(at);
    if (source_time.Compare({0, 1}) < 0 || source_time.Compare(angle.duration) >= 0) continue;
    SourceRequest request;
    request.clip_id = group.id + ":angle:" + angle.id;
    request.track_id = group.id;
    request.track_order = static_cast<std::int64_t>(index);
    request.source_kind = angle.source_kind;
    request.source_id = angle.source_id;
    request.source_time = source_time;
    requests.push_back(std::move(request));
  }
  return requests;
}

std::vector<Clip> Flatten(const Group& group) {
  ValidateGroup(group);
  std::vector<Clip> clips;
  for (std::size_t index = 0; index < group.switches.size(); ++index) {
    const auto& cut = group.switches[index];
    const auto end = index + 1 < group.switches.size() ? group.switches[index + 1].timeline_time : group.duration;
    if (end.Compare(cut.timeline_time) <= 0) continue;
    const auto* angle = FindAngle(group, cut.angle_id);
    if (angle == nullptr) throw std::invalid_argument("Multicam switch names an unknown angle");
    auto source_in = angle->source_offset.Add(cut.timeline_time);
    auto source_out = angle->source_offset.Add(end);
    const auto visible_in = source_in.Compare({0, 1}) < 0 ? time::RationalTime(0, 1) : source_in;
    const auto visible_out = source_out.Compare(angle->duration) > 0 ? angle->duration : source_out;
    if (visible_out.Compare(visible_in) <= 0) continue;
    Clip clip;
    clip.id = group.id + ":flat:" + std::to_string(index);
    clip.source_kind = angle->source_kind;
    clip.source_id = angle->source_id;
    clip.source_in = visible_in;
    clip.source_out = visible_out;
    clip.timeline_start = cut.timeline_time.Add(visible_in.Subtract(source_in));
    clip.name = angle->id;
    clip.start_ticks = clip.timeline_start.ToTicks();
    clip.end_ticks = clip.end().ToTicks();
    clips.push_back(std::move(clip));
  }
  return clips;
}

std::vector<std::string> ListGroupIds(const project::ProjectStore& store) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  std::vector<std::string> ids;
  db::Statement statement(store.connection(), "SELECT id FROM multicam_groups ORDER BY id;");
  while (statement.Step()) ids.push_back(statement.ColumnText(0));
  return ids;
}

Group LoadGroup(const project::ProjectStore& store, const std::string& group_id) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  auto* database = store.connection();
  Group group;
  {
    db::Statement statement(database, "SELECT name, duration_num, duration_den, sync_method, reference_angle_id, sync_confidence FROM multicam_groups WHERE id = ?;");
    statement.Bind(1, group_id);
    if (!statement.Step()) throw std::invalid_argument("Unknown multicam group: " + group_id);
    group.id = group_id;
    group.name = statement.ColumnText(0);
    group.duration = {statement.ColumnInt(1), statement.ColumnInt(2)};
    group.sync_method = statement.ColumnText(3);
    group.reference_angle_id = statement.ColumnText(4);
    group.sync_confidence = statement.ColumnDouble(5);
  }
  {
    db::Statement statement(database, R"sql(
      SELECT a.id, a.name, a.media_id, m.duration_num, m.duration_den, a.timecode_num, a.timecode_den,
             a.marker_num, a.marker_den, a.offset_num, a.offset_den
        FROM multicam_angles a JOIN media m ON m.id = a.media_id
       WHERE a.group_id = ? ORDER BY a.sort_order;
    )sql");
    statement.Bind(1, group_id);
    while (statement.Step()) {
      Angle angle;
      angle.id = statement.ColumnText(0);
      angle.name = statement.ColumnText(1);
      angle.source_kind = model::SourceKind::Media;
      angle.source_id = statement.ColumnText(2);
      angle.duration = {statement.ColumnInt(3), statement.ColumnInt(4)};
      angle.timecode_start = {statement.ColumnInt(5), statement.ColumnInt(6)};
      if (!statement.ColumnIsNull(7)) angle.marker = time::RationalTime(statement.ColumnInt(7), statement.ColumnInt(8));
      angle.source_offset = {statement.ColumnInt(9), statement.ColumnInt(10)};
      group.angles.push_back(std::move(angle));
    }
  }
  {
    db::Statement statement(database, "SELECT time_num, time_den, angle_id FROM multicam_switches WHERE group_id = ? ORDER BY time_ticks;");
    statement.Bind(1, group_id);
    while (statement.Step()) group.switches.push_back({{statement.ColumnInt(0), statement.ColumnInt(1)}, statement.ColumnText(2)});
  }
  return group;
}

std::vector<std::pair<std::string, time::RationalTime>> Offsets(const Group& group) {
  std::vector<std::pair<std::string, time::RationalTime>> offsets;
  for (const auto& angle : group.angles) offsets.emplace_back(angle.id, angle.source_offset);
  return offsets;
}

}  // namespace cutline::timeline::multicam

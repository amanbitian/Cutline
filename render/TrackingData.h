#pragma once

// Tracking results as stored data: how a track is written into a project and read back, and how
// a stored track is found to be out of date.
//
// A track is a number-per-frame record that took a long time to make and that other work depends
// on (a mask that follows it, a title that sits on it), so it is kept in the project, keyed to the
// clip it was measured on and to the fingerprint of the media at the time. If the media is later
// replaced the fingerprint no longer matches and the track is reported stale rather than silently
// applied to footage it was not measured on. The data is JSON so a person can read it.

#include "render/Tracking.h"

#include <string>
#include <vector>

struct sqlite3;

namespace cutline::render::tracking {

[[nodiscard]] std::string ToJson(const TrackResult& result);
[[nodiscard]] TrackResult TrackResultFromJson(const std::string& json);
[[nodiscard]] std::string ToJson(const PlanarTrackResult& result);
[[nodiscard]] PlanarTrackResult PlanarResultFromJson(const std::string& json);
[[nodiscard]] std::string ToJson(const SimilarityResult& result);
[[nodiscard]] SimilarityResult SimilarityResultFromJson(const std::string& json);

// Ids of stored tracks whose clip's media is no longer the media they were measured on.
[[nodiscard]] std::vector<std::string> StaleTrackingIds(sqlite3* database);

}  // namespace cutline::render::tracking

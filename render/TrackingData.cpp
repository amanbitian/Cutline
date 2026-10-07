#include "render/TrackingData.h"

#include "core/db/Sql.h"
#include "core/util/Json.h"
#include "core/util/JsonParse.h"

namespace cutline::render::tracking {
namespace {

std::string Time(const time::RationalTime& t) { return json::Object().Add("n", t.numerator()).Add("d", t.denominator()).Build(); }
time::RationalTime TimeOf(const json::Value& v) { return time::RationalTime(v.Integer("n"), v.Integer("d")); }

std::string WarpJson(const Warp& w) {
  return json::Array({json::Number(w.a11), json::Number(w.a12), json::Number(w.a21), json::Number(w.a22), json::Number(w.tx), json::Number(w.ty)});
}
Warp WarpOf(const json::Value& v) {
  if (!v.is_array() || v.items.size() != 6) throw json::ParseError("a warp has six numbers");
  return {v.items[0].number, v.items[1].number, v.items[2].number, v.items[3].number, v.items[4].number, v.items[5].number};
}

}  // namespace

std::string ToJson(const TrackResult& result) {
  std::vector<std::string> samples;
  for (const auto& s : result.samples) {
    samples.push_back(json::Object().AddRaw("t", Time(s.time)).Add("x", s.x).Add("y", s.y).Add("confidence", s.confidence).Add("valid", s.valid).Build());
  }
  return json::Object()
      .Add("kind", "point")
      .Add("algorithm", result.algorithm)
      .Add("width", static_cast<std::int64_t>(result.source_width))
      .Add("height", static_cast<std::int64_t>(result.source_height))
      .Add("cancelled", result.cancelled)
      .Add("lostAt", result.lost_at)
      .AddRaw("samples", json::Array(samples))
      .Build();
}

TrackResult TrackResultFromJson(const std::string& text) {
  const auto root = json::Parse(text);
  TrackResult result;
  result.algorithm = root.String("algorithm");
  result.source_width = static_cast<int>(root.Integer("width"));
  result.source_height = static_cast<int>(root.Integer("height"));
  result.cancelled = root.Bool("cancelled");
  result.lost_at = root.Integer("lostAt");
  for (const auto& s : root.Require("samples").items) {
    result.samples.push_back({TimeOf(s.Require("t")), s.Number("x"), s.Number("y"), s.Number("confidence"), s.Bool("valid")});
  }
  return result;
}

std::string ToJson(const PlanarTrackResult& result) {
  std::vector<std::string> samples;
  for (const auto& s : result.samples) {
    samples.push_back(json::Object().AddRaw("t", Time(s.time)).AddRaw("warp", WarpJson(s.warp)).Add("confidence", s.confidence).Add("valid", s.valid).Build());
  }
  return json::Object()
      .Add("kind", "plane")
      .Add("algorithm", result.algorithm)
      .Add("width", static_cast<std::int64_t>(result.source_width))
      .Add("height", static_cast<std::int64_t>(result.source_height))
      .AddRaw("region", json::Array({json::Number(result.region.x), json::Number(result.region.y), json::Number(result.region.width), json::Number(result.region.height)}))
      .Add("cancelled", result.cancelled)
      .Add("lostAt", result.lost_at)
      .AddRaw("samples", json::Array(samples))
      .Build();
}

PlanarTrackResult PlanarResultFromJson(const std::string& text) {
  const auto root = json::Parse(text);
  PlanarTrackResult result;
  result.algorithm = root.String("algorithm");
  result.source_width = static_cast<int>(root.Integer("width"));
  result.source_height = static_cast<int>(root.Integer("height"));
  const auto& region = root.Require("region").items;
  if (region.size() != 4) throw json::ParseError("a region has four numbers");
  result.region = {region[0].number, region[1].number, region[2].number, region[3].number};
  result.cancelled = root.Bool("cancelled");
  result.lost_at = root.Integer("lostAt");
  for (const auto& s : root.Require("samples").items) {
    result.samples.push_back({TimeOf(s.Require("t")), WarpOf(s.Require("warp")), s.Number("confidence"), s.Bool("valid")});
  }
  return result;
}

std::string ToJson(const SimilarityResult& result) {
  const auto write = [](const std::vector<SimilaritySample>& list) {
    std::vector<std::string> out;
    for (const auto& s : list) {
      out.push_back(json::Object()
                        .AddRaw("t", Time(s.time))
                        .Add("tx", s.translation_x)
                        .Add("ty", s.translation_y)
                        .Add("rotation", s.rotation_degrees)
                        .Add("scale", s.scale)
                        .Add("inliers", static_cast<std::int64_t>(s.inliers))
                        .Add("tracked", static_cast<std::int64_t>(s.tracked))
                        .Build());
    }
    return json::Array(out);
  };
  std::vector<std::string> uncertain;
  for (const auto index : result.uncertain_frames) uncertain.push_back(std::to_string(index));
  return json::Object()
      .Add("kind", "stabilize")
      .Add("algorithm", result.algorithm)
      .Add("width", static_cast<std::int64_t>(result.source_width))
      .Add("height", static_cast<std::int64_t>(result.source_height))
      .Add("autoScale", result.auto_scale)
      .Add("cancelled", result.cancelled)
      .AddRaw("uncertain", json::Array(uncertain))
      .AddRaw("samples", write(result.samples))
      .AddRaw("cameraPath", write(result.camera_path))
      .Build();
}

SimilarityResult SimilarityResultFromJson(const std::string& text) {
  const auto root = json::Parse(text);
  SimilarityResult result;
  result.algorithm = root.String("algorithm");
  result.source_width = static_cast<int>(root.Integer("width"));
  result.source_height = static_cast<int>(root.Integer("height"));
  result.auto_scale = root.Number("autoScale");
  result.cancelled = root.Bool("cancelled");
  for (const auto& item : root.Require("uncertain").items) result.uncertain_frames.push_back(static_cast<std::size_t>(item.AsInteger()));
  const auto read = [](const json::Value& list) {
    std::vector<SimilaritySample> out;
    for (const auto& s : list.items) {
      SimilaritySample sample;
      sample.time = TimeOf(s.Require("t"));
      sample.translation_x = s.Number("tx");
      sample.translation_y = s.Number("ty");
      sample.rotation_degrees = s.Number("rotation");
      sample.scale = s.Number("scale");
      sample.inliers = static_cast<int>(s.Integer("inliers"));
      sample.tracked = static_cast<int>(s.Integer("tracked"));
      out.push_back(sample);
    }
    return out;
  };
  result.samples = read(root.Require("samples"));
  result.camera_path = read(root.Require("cameraPath"));
  return result;
}

std::vector<std::string> StaleTrackingIds(sqlite3* database) {
  std::vector<std::string> ids;
  db::Statement statement(database, R"sql(
    SELECT t.id FROM tracking_data t
      JOIN clips c ON c.id = t.clip_id
      JOIN media m ON m.id = c.media_id
     WHERE t.source_fingerprint <> m.fingerprint
     ORDER BY t.id;
  )sql");
  while (statement.Step()) ids.push_back(statement.ColumnText(0));
  return ids;
}

}  // namespace cutline::render::tracking

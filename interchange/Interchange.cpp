#include "interchange/Interchange.h"

#include "core/db/Sql.h"
#include "core/util/Json.h"
#include "core/project/ProjectStore.h"
#include "timeline/SequenceLoader.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace cutline::interchange {

using time::RationalTime;

// ------------------------------------------------------------------ report ----

void Report::Note(std::string where, std::string message) {
  issues.push_back({Issue::Severity::Note, std::move(where), std::move(message)});
}
void Report::Warn(std::string where, std::string message) {
  issues.push_back({Issue::Severity::Warning, std::move(where), std::move(message)});
}
void Report::Error(std::string where, std::string message) {
  issues.push_back({Issue::Severity::Error, std::move(where), std::move(message)});
}

int Report::Count(Issue::Severity severity) const {
  return static_cast<int>(std::count_if(issues.begin(), issues.end(), [&](const Issue& i) { return i.severity == severity; }));
}

std::string Report::ToText() const {
  std::ostringstream out;
  for (const auto& issue : issues) {
    out << (issue.severity == Issue::Severity::Error ? "error" : issue.severity == Issue::Severity::Warning ? "warning" : "note")
        << ": ";
    if (!issue.where.empty()) out << issue.where << ": ";
    out << issue.message << "\n";
  }
  return out.str();
}

bool Report::Mentions(const std::string& text) const {
  return std::any_of(issues.begin(), issues.end(), [&](const Issue& i) {
    return i.message.find(text) != std::string::npos || i.where.find(text) != std::string::npos;
  });
}

const Media* Timeline::FindMedia(const std::string& id) const {
  const auto found = std::find_if(media.begin(), media.end(), [&](const Media& m) { return m.id == id; });
  return found == media.end() ? nullptr : &*found;
}

const Sequence* Timeline::FindSequence(const std::string& id) const {
  const auto found = std::find_if(sequences.begin(), sequences.end(), [&](const Sequence& s) { return s.id == id; });
  return found == sequences.end() ? nullptr : &*found;
}

// ------------------------------------------------------------ shared tools ----

time::FrameRate RateFromDouble(double rate, bool& exact) {
  exact = true;
  if (!(rate > 0.0) || !std::isfinite(rate)) {
    exact = false;
    return {25, 1};
  }
  const auto rounded = std::llround(rate);
  if (rounded > 0 && std::abs(rate - static_cast<double>(rounded)) < 1e-9) return {rounded, 1};
  // The 1001 family: 24000/1001, 30000/1001, 48000/1001, 60000/1001, and so on.
  for (std::int64_t base = 12; base <= 240; base += 6) {
    const auto numerator = base * 1000;
    if (std::abs(rate - static_cast<double>(numerator) / 1001.0) < 1e-6) return {numerator, 1001};
  }
  // Otherwise the best fraction with a modest denominator, and the caller is told.
  exact = false;
  std::int64_t best_n = rounded > 0 ? rounded : 1, best_d = 1;
  double best_error = std::abs(rate - static_cast<double>(best_n));
  for (std::int64_t d = 2; d <= 10000; ++d) {
    const auto n = std::llround(rate * static_cast<double>(d));
    if (n <= 0) continue;
    const auto error = std::abs(rate - static_cast<double>(n) / static_cast<double>(d));
    if (error < best_error - 1e-12) {
      best_error = error;
      best_n = n;
      best_d = d;
    }
  }
  const auto divisor = std::gcd(best_n, best_d);
  return {best_n / divisor, best_d / divisor};
}

std::string Prefixed(const std::string& prefix, const std::string& id) { return prefix.empty() ? id : prefix + id; }

std::string ExactToJson(const RationalTime& t) {
  return json::Object().Add("n", t.numerator()).Add("d", t.denominator()).Build();
}

RationalTime ExactFromJson(const json::Value* value) {
  if (value == nullptr || !value->is_object()) return RationalTime(0, 1);
  return RationalTime(value->Integer("n"), value->Integer("d"));
}

namespace {

[[nodiscard]] std::string Components(const anim::Value& value) {
  std::vector<std::string> parts;
  for (int i = 0; i < value.dimension; ++i) parts.push_back(json::Number(value.components[static_cast<std::size_t>(i)]));
  return json::Array(parts);
}

[[nodiscard]] anim::Value ValueFrom(const json::Value* list) {
  anim::Value value;
  std::vector<double> numbers;
  if (list != nullptr && list->is_array()) {
    for (const auto& item : list->items) numbers.push_back(item.number);
  }
  value.dimension = static_cast<int>(std::clamp<std::size_t>(numbers.size(), 1, 4));
  for (std::size_t i = 0; i < numbers.size() && i < 4; ++i) value.components[i] = numbers[i];
  return value;
}

}  // namespace

std::string EffectToJson(const timeline::Effect& effect) {
  std::vector<std::string> parameters;
  for (const auto& parameter : effect.parameters) {
    std::vector<std::string> keyframes;
    for (const auto& key : parameter.value.keyframes()) {
      keyframes.push_back(json::Object()
                              .AddRaw("t", ExactToJson(key.time))
                              .AddRaw("v", Components(key.value))
                              .Add("interpolation", anim::ToString(key.interpolation))
                              .AddRaw("out", json::Array({json::Number(key.out_handle.x), json::Number(key.out_handle.y)}))
                              .AddRaw("in", json::Array({json::Number(key.in_handle.x), json::Number(key.in_handle.y)}))
                              .Build());
    }
    parameters.push_back(json::Object()
                             .Add("id", parameter.id)
                             .Add("name", parameter.name)
                             .Add("dimension", static_cast<std::int64_t>(parameter.value.constant().dimension))
                             .AddRaw("constant", Components(parameter.value.constant()))
                             .AddRaw("keyframes", json::Array(keyframes))
                             .Build());
  }
  return json::Object()
      .Add("id", effect.id)
      .Add("type", effect.effect_type)
      .Add("order", effect.order)
      .Add("enabled", effect.enabled)
      .Add("intrinsic", effect.intrinsic)
      .Add("preset", effect.preset_name)
      .AddRaw("parameters", json::Array(parameters))
      .Build();
}

timeline::Effect EffectFromJson(const json::Value& value) {
  timeline::Effect effect;
  effect.id = value.String("id");
  effect.effect_type = value.String("type");
  effect.order = value.Integer("order");
  effect.enabled = value.Bool("enabled");
  effect.intrinsic = value.Bool("intrinsic");
  effect.preset_name = value.String("preset");
  for (const auto& parameter : value.Require("parameters").items) {
    timeline::Parameter p;
    p.id = parameter.String("id");
    p.name = parameter.String("name");
    anim::AnimatedValue animated(ValueFrom(parameter.Find("constant")));
    for (const auto& key : parameter.Require("keyframes").items) {
      anim::Keyframe keyframe;
      keyframe.time = ExactFromJson(key.Find("t"));
      keyframe.value = ValueFrom(key.Find("v"));
      keyframe.interpolation = anim::ParseInterpolation(key.String("interpolation"));
      const auto* out = key.Find("out");
      const auto* in = key.Find("in");
      if (out != nullptr && out->is_array() && out->items.size() == 2) keyframe.out_handle = {out->items[0].number, out->items[1].number};
      if (in != nullptr && in->is_array() && in->items.size() == 2) keyframe.in_handle = {in->items[0].number, in->items[1].number};
      animated.SetKeyframe(keyframe);
    }
    p.value = animated;
    effect.parameters.push_back(std::move(p));
  }
  return effect;
}

std::string MarkerToJson(const Marker& marker) {
  return json::Object()
      .Add("id", marker.id)
      .Add("label", marker.label)
      .AddRaw("start", ExactToJson(marker.start))
      .AddRaw("end", ExactToJson(marker.end))
      .Add("kind", model::ToString(marker.kind))
      .Add("color", marker.color)
      .Add("metadata", marker.metadata_json)
      .Build();
}

Marker MarkerFromJson(const json::Value& value) {
  Marker marker;
  marker.id = value.String("id");
  marker.label = value.Find("label") != nullptr ? value.String("label") : std::string{};
  marker.start = ExactFromJson(value.Find("start"));
  marker.end = ExactFromJson(value.Find("end"));
  try {
    marker.kind = model::ParseMarkerKind(value.String("kind"));
  } catch (const std::exception&) {
    marker.kind = model::MarkerKind::Comment;
  }
  marker.color = value.String("color");
  marker.metadata_json = value.String("metadata");
  return marker;
}

// ----------------------------------------------------------- project edge ----

namespace {

[[nodiscard]] Marker MarkerFromRow(db::Statement& statement) {
  Marker marker;
  marker.id = statement.ColumnText(0);
  marker.start = RationalTime(statement.ColumnInt(1), statement.ColumnInt(2));
  marker.end = RationalTime(statement.ColumnInt(3), statement.ColumnInt(4));
  marker.label = statement.ColumnText(5);
  marker.kind = model::ParseMarkerKind(statement.ColumnText(6));
  marker.color = statement.ColumnText(7);
  marker.metadata_json = statement.ColumnText(8);
  return marker;
}

[[nodiscard]] std::vector<Marker> MarkersOf(sqlite3* database, const std::string& owner_kind, const std::string& owner_id) {
  std::vector<Marker> markers;
  db::Statement statement(database, R"sql(
    SELECT id, start_num, start_den, end_num, end_den, label, kind, color, metadata_json
      FROM markers WHERE owner_kind = ? AND owner_id = ? ORDER BY start_ticks, id;
  )sql");
  statement.Bind(1, owner_kind).Bind(2, owner_id);
  while (statement.Step()) markers.push_back(MarkerFromRow(statement));
  return markers;
}

}  // namespace

Timeline FromProject(const project::ProjectStore& store, const std::string& sequence_id, Report& report) {
  // Loading takes the store's lock itself; the catalog and marker reads below take it again
  // afterwards, never together.
  const auto graph = timeline::LoadSequenceGraph(store, sequence_id);
  Timeline result;
  std::set<std::string> media_ids;

  const std::lock_guard<std::mutex> lock(store.mutex());
  auto* database = store.connection();
  for (const auto& source : graph.sequences) {
    Sequence sequence;
    sequence.id = source.id;
    auto& settings = sequence.settings;
    settings.name = source.name;
    settings.frame_rate = source.frame_rate;
    settings.width = source.width;
    settings.height = source.height;
    settings.pixel_aspect = source.pixel_aspect;
    settings.sample_rate = source.sample_rate;
    settings.channel_layout = source.channel_layout;
    settings.working_color_space = source.working_color_space;
    settings.display_color_space = source.display_color_space;
    settings.field_order = source.field_order;
    settings.drop_frame = source.drop_frame;
    settings.render_version = source.render_version;
    sequence.effects = source.effects;
    sequence.markers = MarkersOf(database, "sequence", source.id);

    for (const auto& track : source.tracks) {
      const auto where = "sequence " + source.name + " / track " + (track.name.empty() ? track.id : track.name);
      if (track.is_bus) {
        report.Warn(where, "audio buses have no equivalent in the exchange model and were not exported");
        continue;
      }
      if (!track.sends.empty() || !track.output_bus_id.empty()) {
        report.Warn(where, "the track's routing to buses was not exported; it will go to the master");
      }
      Track out;
      out.id = track.id;
      out.name = track.name;
      out.kind = track.kind;
      out.order = track.order;
      out.locked = track.locked;
      out.muted = track.muted;
      out.solo = track.solo;
      out.gain_db = track.gain_db;
      out.pan = track.pan;
      out.channel_layout = track.channel_layout;
      out.effects = track.effects;
      for (const auto& clip : track.clips) {
        if (clip.source_kind == model::SourceKind::Adjustment) {
          report.Warn(where + " / clip " + clip.id, "an adjustment clip has no equivalent in the exchange model and was not exported");
          continue;
        }
        Item item;
        item.id = clip.id;
        item.name = clip.name;
        item.source_kind = clip.source_kind;
        item.source_id = clip.source_id;
        item.source_in = clip.source_in;
        item.source_out = clip.source_out;
        item.timeline_start = clip.timeline_start;
        item.playback_rate = clip.playback_rate;
        item.reversed = clip.reversed;
        item.maintain_pitch = clip.maintain_pitch;
        item.enabled = clip.enabled;
        item.linked_group = clip.linked_group;
        item.effects = clip.effects;
        item.markers = MarkersOf(database, "clip", clip.id);
        if (clip.source_kind == model::SourceKind::Media) media_ids.insert(clip.source_id);
        out.items.push_back(std::move(item));
      }
      for (const auto& transition : track.transitions) {
        Transition t;
        t.id = transition.id;
        t.kind = transition.kind;
        t.alignment = transition.alignment;
        t.from_item = transition.from_clip_id;
        t.to_item = transition.to_clip_id;
        t.start = transition.timeline_start;
        t.duration = transition.duration;
        if (!transition.effects.empty()) {
          report.Warn(where + " / transition " + transition.id, "the transition's own effects were not exported");
        }
        out.transitions.push_back(std::move(t));
      }
      sequence.tracks.push_back(std::move(out));
    }
    result.sequences.push_back(std::move(sequence));
  }

  for (const auto& id : media_ids) {
    db::Statement statement(database, R"sql(
      SELECT display_name, original_path, fingerprint, duration_num, duration_den,
             start_timecode_num, start_timecode_den
        FROM media WHERE id = ?;
    )sql");
    statement.Bind(1, id);
    Media media;
    media.id = id;
    if (statement.Step()) {
      media.name = statement.ColumnText(0);
      media.url = statement.ColumnText(1);
      media.fingerprint = statement.ColumnText(2);
      media.duration = RationalTime(statement.ColumnInt(3), statement.ColumnInt(4));
      media.start_timecode = RationalTime(statement.ColumnInt(5), statement.ColumnInt(6));
    } else {
      report.Error("media " + id, "a clip refers to media the project does not have");
    }
    result.media.push_back(std::move(media));
  }
  return result;
}

// ----------------------------------------------------------- apply to project ----

namespace {

class Runner final {
 public:
  Runner(project::ProjectStore& store, const ApplyOptions& options, Report& report)
      : store_(store), options_(options), report_(report), project_id_(store.ProjectId()) {}

  bool Run(commands::CommandType type, commands::CommandPayload payload, const std::string& where) {
    try {
      commands::CommandEnvelope command;
      command.command_id = "interchange-" + project::ProjectStore::GenerateProjectUuid();
      command.project_id = project_id_;
      command.author_id = options_.author;
      command.base_revision = store_.CurrentRevision();
      command.timestamp_utc = options_.timestamp_utc;
      command.type = type;
      command.payload = std::move(payload);
      command.idempotency_key = command.command_id;
      const auto result = store_.Execute(command);
      (void)result;
      return true;
    } catch (const std::exception& error) {
      report_.Error(where, error.what());
      return false;
    }
  }

 private:
  project::ProjectStore& store_;
  const ApplyOptions& options_;
  Report& report_;
  std::string project_id_;
};

[[nodiscard]] bool MediaExists(const project::ProjectStore& store, const std::string& id) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  db::Statement statement(store.connection(), "SELECT 1 FROM media WHERE id = ?;");
  statement.Bind(1, id);
  return statement.Step();
}

}  // namespace

ApplyResult ApplyToProject(project::ProjectStore& store, const Timeline& timeline, const ApplyOptions& options,
                           Report& report) {
  ApplyResult result;
  Runner runner(store, options, report);
  const auto id = [&](const std::string& value) { return Prefixed(options.id_prefix, value); };

  for (const auto& media : timeline.media) {
    if (MediaExists(store, id(media.id))) {
      report.Note("media " + media.name, "already in the project; its existing entry is used");
      continue;
    }
    commands::ImportMediaPayload payload;
    payload.id = id(media.id);
    payload.display_name = media.name.empty() ? media.id : media.name;
    payload.original_path = media.url;
    payload.fingerprint = media.fingerprint.empty() ? "interchange:" + media.url : media.fingerprint;
    payload.duration = media.duration;
    payload.start_timecode = media.start_timecode;
    if (runner.Run(commands::CommandType::ImportMedia, payload, "media " + payload.display_name)) ++result.media_created;
  }

  // Nested sequences are made before the sequences that contain them.
  std::vector<const Sequence*> order;
  std::set<std::string> placed, visiting;
  std::function<void(const Sequence&)> visit = [&](const Sequence& sequence) {
    if (placed.count(sequence.id) != 0 || !visiting.insert(sequence.id).second) return;
    for (const auto& track : sequence.tracks) {
      for (const auto& item : track.items) {
        if (item.source_kind != model::SourceKind::Sequence) continue;
        if (const auto* nested = timeline.FindSequence(item.source_id)) visit(*nested);
      }
    }
    placed.insert(sequence.id);
    order.push_back(&sequence);
  };
  for (const auto& sequence : timeline.sequences) visit(sequence);

  const auto apply_effects = [&](const std::vector<timeline::Effect>& effects, model::EffectOwner owner_kind,
                                 const std::string& owner_id, const std::string& where) {
    for (const auto& effect : effects) {
      commands::AddEffectPayload payload;
      payload.id = id(effect.id);
      payload.owner_kind = owner_kind;
      payload.owner_id = owner_id;
      payload.effect_type = effect.effect_type;
      payload.order = effect.order;
      payload.intrinsic = effect.intrinsic;
      payload.preset_name = effect.preset_name;
      for (const auto& parameter : effect.parameters) {
        payload.parameters.push_back({id(parameter.id.empty() ? effect.id + ":" + parameter.name : parameter.id),
                                      parameter.name, parameter.value.constant()});
      }
      if (!runner.Run(commands::CommandType::AddEffect, payload, where + " / effect " + effect.effect_type)) continue;
      ++result.effects_created;
      for (std::size_t index = 0; index < effect.parameters.size(); ++index) {
        for (const auto& keyframe : effect.parameters[index].value.keyframes()) {
          commands::SetKeyframePayload key;
          key.parameter_id = payload.parameters[index].id;
          key.keyframe = keyframe;
          runner.Run(commands::CommandType::SetKeyframe, key, where + " / effect " + effect.effect_type + " / keyframe");
        }
      }
      if (!effect.enabled) {
        runner.Run(commands::CommandType::SetEffectEnabled, commands::SetEffectEnabledPayload{payload.id, false}, where);
      }
    }
  };
  const auto apply_markers = [&](const std::vector<Marker>& markers, model::MarkerOwner owner_kind,
                                 const std::string& owner_id, const std::string& where) {
    for (const auto& marker : markers) {
      commands::AddMarkerPayload payload;
      payload.id = id(marker.id);
      payload.owner_kind = owner_kind;
      payload.owner_id = owner_id;
      payload.start = marker.start;
      payload.end = marker.end;
      payload.label = marker.label;
      payload.kind = marker.kind;
      payload.color = marker.color;
      payload.metadata_json = marker.metadata_json;
      if (runner.Run(commands::CommandType::AddMarker, payload, where + " / marker " + marker.label)) ++result.markers_created;
    }
  };

  for (const auto* sequence : order) {
    const auto where = "sequence " + sequence->settings.name;
    commands::CreateSequencePayload create;
    create.id = id(sequence->id);
    create.settings = sequence->settings;
    if (!runner.Run(commands::CommandType::CreateSequence, create, where)) continue;
    ++result.sequences_created;
    result.sequence_ids.push_back(create.id);

    std::map<std::string, std::vector<std::string>> groups;  // linked groups, with prefixed clip ids
    for (const auto& track : sequence->tracks) {
      const auto track_where = where + " / track " + (track.name.empty() ? track.id : track.name);
      commands::AddTrackPayload add;
      add.id = id(track.id);
      add.sequence_id = create.id;
      add.order = track.order;
      add.channel_layout = track.channel_layout;
      add.name = track.name;
      if (!runner.Run(track.kind == model::TrackKind::Video ? commands::CommandType::AddVideoTrack
                                                            : commands::CommandType::AddAudioTrack,
                      add, track_where)) {
        continue;
      }
      if (track.locked || track.muted || track.solo || track.gain_db != 0.0 || track.pan != 0.0) {
        commands::SetTrackStatePayload state;
        state.id = add.id;
        state.muted = track.muted;
        state.solo = track.solo;
        state.gain_db = track.gain_db;
        state.pan = track.pan;
        state.name = track.name;
        state.locked = false;  // locked last, once the clips are in
        runner.Run(commands::CommandType::SetTrackState, state, track_where);
      }

      auto items = track.items;
      std::sort(items.begin(), items.end(),
                [](const Item& a, const Item& b) { return a.timeline_start.Compare(b.timeline_start) < 0; });
      for (const auto& item : items) {
        const auto item_where = track_where + " / clip " + (item.name.empty() ? item.id : item.name);
        commands::InsertClipPayload clip;
        clip.id = id(item.id);
        clip.track_id = add.id;
        clip.source_kind = item.source_kind;
        if (item.source_kind == model::SourceKind::Sequence) {
          clip.nested_sequence_id = id(item.source_id);
        } else {
          clip.media_id = id(item.source_id);
        }
        clip.source_in = item.source_in;
        clip.source_out = item.source_out;
        clip.timeline_start = item.timeline_start;
        clip.playback_rate = item.playback_rate;
        clip.reversed = item.reversed;
        clip.maintain_pitch = item.maintain_pitch;
        clip.name = item.name;
        if (!runner.Run(commands::CommandType::InsertClip, clip, item_where)) continue;
        ++result.clips_created;
        if (!item.linked_group.empty()) groups[id(item.linked_group)].push_back(clip.id);
        if (!item.enabled) {
          runner.Run(commands::CommandType::SetClipEnabled, commands::SetClipEnabledPayload{clip.id, false, false}, item_where);
        }
        apply_effects(item.effects, model::EffectOwner::Clip, clip.id, item_where);
        apply_markers(item.markers, model::MarkerOwner::Clip, clip.id, item_where);
      }

      for (const auto& transition : track.transitions) {
        commands::AddTransitionPayload payload;
        payload.id = id(transition.id);
        payload.track_id = add.id;
        payload.kind = transition.kind;
        payload.alignment = transition.alignment;
        if (transition.from_item.has_value()) payload.from_clip_id = id(*transition.from_item);
        if (transition.to_item.has_value()) payload.to_clip_id = id(*transition.to_item);
        payload.timeline_start = transition.start;
        payload.duration = transition.duration;
        if (runner.Run(commands::CommandType::AddTransition, payload, track_where + " / transition " + transition.kind)) {
          ++result.transitions_created;
        }
      }
      apply_effects(track.effects, model::EffectOwner::Track, add.id, track_where);
      if (track.locked) {
        commands::SetTrackStatePayload state;
        state.id = add.id;
        state.locked = true;
        state.muted = track.muted;
        state.solo = track.solo;
        state.gain_db = track.gain_db;
        state.pan = track.pan;
        state.name = track.name;
        runner.Run(commands::CommandType::SetTrackState, state, track_where);
      }
    }
    for (const auto& [group, members] : groups) {
      if (members.size() < 2) continue;
      runner.Run(commands::CommandType::LinkClips, commands::LinkClipsPayload{members, group}, where + " / linked clips " + group);
    }
    apply_effects(sequence->effects, model::EffectOwner::Sequence, create.id, where);
    apply_markers(sequence->markers, model::MarkerOwner::Sequence, create.id, where);
  }
  return result;
}

}  // namespace cutline::interchange

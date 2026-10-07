#include "interchange/Otio.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace cutline::interchange {

using time::RationalTime;

namespace {

// ------------------------------------------------------------------ writing ----

[[nodiscard]] std::string Str(const std::string& value) { return "\"" + json::Escape(value) + "\""; }
[[nodiscard]] std::string Num(double value) { return json::Number(value); }
[[nodiscard]] std::string Int(std::int64_t value) { return std::to_string(value); }
[[nodiscard]] std::string Bool(bool value) { return value ? "true" : "false"; }

[[nodiscard]] std::string List(const std::vector<std::string>& elements) { return json::Array(elements); }

// {"n": numerator, "d": denominator}: an exact rational, for the metadata only Cutline reads.
[[nodiscard]] std::string Exact(const RationalTime& t) {
  return json::Object().Add("n", t.numerator()).Add("d", t.denominator()).Build();
}

struct Context final {
  time::FrameRate fps{25, 1};
};

[[nodiscard]] bool OnFrame(const RationalTime& t, const time::FrameRate& fps) {
  const auto top = t.numerator() * fps.numerator;
  const auto bottom = t.denominator() * fps.denominator;
  return bottom != 0 && top % bottom == 0;
}

// An OTIO RationalTime. On a frame of the sequence it is a frame count at the frame rate,
// which is what every other tool expects; off one, its own exact fraction.
[[nodiscard]] std::string Rt(const RationalTime& t, const Context& context) {
  double value, rate;
  if (OnFrame(t, context.fps)) {
    value = static_cast<double>(t.numerator() * context.fps.numerator / (t.denominator() * context.fps.denominator));
    rate = static_cast<double>(context.fps.numerator) / static_cast<double>(context.fps.denominator);
  } else {
    value = static_cast<double>(t.numerator());
    rate = static_cast<double>(t.denominator());
  }
  return json::Object().Add("OTIO_SCHEMA", "RationalTime.1").Add("rate", rate).Add("value", value).Build();
}

[[nodiscard]] std::string Range(const RationalTime& start, const RationalTime& duration, const Context& context) {
  return json::Object()
      .Add("OTIO_SCHEMA", "TimeRange.1")
      .AddRaw("duration", Rt(duration, context))
      .AddRaw("start_time", Rt(start, context))
      .Build();
}

[[nodiscard]] std::string EffectMetadata(const timeline::Effect& effect) {
  return json::Object().AddRaw("cutline", EffectToJson(effect)).Build();
}

[[nodiscard]] std::vector<std::string> EffectsJson(const std::vector<timeline::Effect>& effects) {
  std::vector<std::string> out;
  for (const auto& effect : effects) {
    out.push_back(json::Object()
                      .Add("OTIO_SCHEMA", "Effect.1")
                      .Add("name", effect.id)
                      .Add("effect_name", effect.effect_type)
                      .AddRaw("metadata", EffectMetadata(effect))
                      .Add("enabled", effect.enabled)
                      .Build());
  }
  return out;
}

[[nodiscard]] std::string MarkerColor(const std::string& color) {
  static const char* kNames[] = {"PINK", "RED", "ORANGE", "YELLOW", "GREEN", "CYAN", "BLUE", "PURPLE", "MAGENTA", "BLACK", "WHITE"};
  std::string upper;
  for (const char c : color) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  for (const auto* name : kNames) {
    if (upper == name) return upper;
  }
  return "";
}

[[nodiscard]] std::vector<std::string> MarkersJson(const std::vector<Marker>& markers, const Context& context) {
  std::vector<std::string> out;
  for (const auto& marker : markers) {
    out.push_back(json::Object()
                      .Add("OTIO_SCHEMA", "Marker.2")
                      .Add("name", marker.label)
                      .Add("color", MarkerColor(marker.color))
                      .AddRaw("marked_range", Range(marker.start, marker.end.Subtract(marker.start), context))
                      .Add("comment", "")
                      .AddRaw("metadata", json::Object()
                                              .AddRaw("cutline", json::Object()
                                                                     .Add("id", marker.id)
                                                                     .AddRaw("start", Exact(marker.start))
                                                                     .AddRaw("end", Exact(marker.end))
                                                                     .Add("kind", model::ToString(marker.kind))
                                                                     .Add("color", marker.color)
                                                                     .Add("metadata", marker.metadata_json)
                                                                     .Build())
                                              .Build())
                      .Build());
  }
  return out;
}

class Writer final {
 public:
  Writer(const Timeline& timeline, Report& report) : timeline_(timeline), report_(report) {}

  [[nodiscard]] std::string Write() {
    if (timeline_.sequences.empty()) {
      report_.Error("timeline", "there is no sequence to write");
      return {};
    }
    const auto& root = timeline_.sequences.front();
    const Context context{root.settings.frame_rate};
    return json::Object()
        .Add("OTIO_SCHEMA", "Timeline.1")
        .Add("name", root.settings.name)
        .AddNull("global_start_time")
        .AddRaw("tracks", StackJson(root, 0, context, "tracks", nullptr))
        .AddRaw("metadata", json::Object()
                                .AddRaw("cutline", json::Object().Add("version", static_cast<std::int64_t>(1)).AddRaw("sequence", SequenceMetadata(root)).Build())
                                .Build())
        .Build();
  }

 private:
  [[nodiscard]] static std::string SequenceMetadata(const Sequence& sequence) {
    const auto& s = sequence.settings;
    return json::Object()
        .Add("id", sequence.id)
        .Add("name", s.name)
        .Add("frameRate", s.frame_rate)
        .Add("width", s.width)
        .Add("height", s.height)
        .Add("pixelAspect", s.pixel_aspect)
        .Add("sampleRate", s.sample_rate)
        .Add("channelLayout", s.channel_layout)
        .Add("workingColorSpace", s.working_color_space)
        .Add("displayColorSpace", s.display_color_space)
        .Add("fieldOrder", model::ToString(s.field_order))
        .Add("dropFrame", s.drop_frame)
        .Add("renderVersion", s.render_version.value_or(0))
        .Build();
  }

  // A sequence as a Stack of its tracks. `clip` is set when the stack stands in a track as a
  // nested clip: it then carries the clip's range, effects and markers.
  [[nodiscard]] std::string StackJson(const Sequence& sequence, int depth, const Context& context, const std::string& name,
                                      const Item* clip) {
    if (depth > 8) {
      report_.Error("sequence " + sequence.settings.name, "nested sequences are too deep to write");
      return "null";
    }
    std::vector<const Track*> ordered;
    for (const auto& track : sequence.tracks) {
      if (track.kind == model::TrackKind::Video) ordered.push_back(&track);
    }
    for (const auto& track : sequence.tracks) {
      if (track.kind == model::TrackKind::Audio) ordered.push_back(&track);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Track* a, const Track* b) {
      return a->kind == b->kind && a->order < b->order;
    });
    std::vector<std::string> children;
    for (const auto* track : ordered) children.push_back(TrackJson(sequence, *track, depth, context));

    auto effects = EffectsJson(sequence.effects);
    auto markers = MarkersJson(sequence.markers, context);
    std::string source_range = "null";
    std::string metadata = "{}";
    bool enabled = true;
    if (clip != nullptr) {
      effects = EffectsJson(clip->effects);
      markers = MarkersJson(clip->markers, context);
      source_range = Range(clip->source_in, clip->duration(), context);
      metadata = json::Object().AddRaw("cutline", ClipMetadata(*clip, &sequence)).Build();
      enabled = clip->enabled;
    } else if (depth == 0) {
      metadata = json::Object().AddRaw("cutline", json::Object().AddRaw("sequence", SequenceMetadata(sequence)).Build()).Build();
    }
    if (clip != nullptr) {
      // The nested sequence travels with the clip that uses it; every use writes it again.
      const auto key = sequence.id;
      if (!written_nested_.insert(key).second) {
        report_.Note("sequence " + sequence.settings.name, "is used more than once and is written once for each use");
      }
    }
    auto object = json::Object()
                      .Add("OTIO_SCHEMA", "Stack.1")
                      .Add("name", clip != nullptr && !clip->name.empty() ? clip->name : name)
                      .AddRaw("children", List(children))
                      .AddRaw("source_range", source_range)
                      .AddRaw("metadata", metadata)
                      .AddRaw("markers", List(markers))
                      .AddRaw("effects", List(effects))
                      .Add("enabled", enabled);
    return object.Build();
  }

  [[nodiscard]] std::string ClipMetadata(const Item& item, const Sequence* nested) const {
    auto object = json::Object();
    if (nested != nullptr) {
      // The nested sequence's own settings, effects and markers ride on the clip that uses it.
      const Context inner{nested->settings.frame_rate};
      object.AddRaw("sequence", SequenceMetadata(*nested));
      object.AddRaw("sequenceEffects", List(EffectsJson(nested->effects)));
      object.AddRaw("sequenceMarkers", List(MarkersJson(nested->markers, inner)));
    }
    return object
        .Add("id", item.id)
        .Add("sourceKind", model::ToString(item.source_kind))
        .Add("sourceId", item.source_id)
        .AddRaw("sourceIn", Exact(item.source_in))
        .AddRaw("sourceOut", Exact(item.source_out))
        .AddRaw("timelineStart", Exact(item.timeline_start))
        .AddRaw("playbackRate", Exact(item.playback_rate))
        .Add("reversed", item.reversed)
        .Add("maintainPitch", item.maintain_pitch)
        .Add("linkedGroup", item.linked_group)
        .Build();
  }

  [[nodiscard]] std::string TrackJson(const Sequence& sequence, const Track& track, int depth, const Context& context) {
    std::vector<std::string> children;
    std::vector<const Item*> items;
    for (const auto& item : track.items) items.push_back(&item);
    std::sort(items.begin(), items.end(), [](const Item* a, const Item* b) { return a->timeline_start.Compare(b->timeline_start) < 0; });
    std::set<std::string> transition_done;

    const auto transition_json = [&](const Transition& transition) {
      // The cut the transition is built around: where the incoming clip starts, or, for a
      // fade out, where the outgoing one ends.
      RationalTime cut = transition.start;
      for (const auto* item : items) {
        if (transition.to_item.has_value() && item->id == *transition.to_item) cut = item->timeline_start;
        else if (!transition.to_item.has_value() && transition.from_item.has_value() && item->id == *transition.from_item) cut = item->end();
      }
      const auto in_offset = cut.Subtract(transition.start);
      const auto out_offset = transition.start.Add(transition.duration).Subtract(cut);
      const bool dissolve = transition.kind == "cross_dissolve" || transition.kind == "dissolve" || transition.kind == "constant_power";
      auto metadata = json::Object()
                          .Add("id", transition.id)
                          .Add("kind", transition.kind)
                          .Add("alignment", model::ToString(transition.alignment))
                          .AddRaw("start", Exact(transition.start))
                          .AddRaw("duration", Exact(transition.duration));
      if (transition.from_item.has_value()) metadata.Add("from", *transition.from_item);
      if (transition.to_item.has_value()) metadata.Add("to", *transition.to_item);
      return json::Object()
          .Add("OTIO_SCHEMA", "Transition.1")
          .Add("name", transition.id)
          .Add("transition_type", dissolve ? "SMPTE_Dissolve" : "Custom_Transition")
          .AddRaw("in_offset", Rt(in_offset, context))
          .AddRaw("out_offset", Rt(out_offset, context))
          .AddRaw("metadata", json::Object().AddRaw("cutline", metadata.Build()).Build())
          .Build();
    };

    RationalTime cursor(0, 1);
    for (const auto* item : items) {
      if (item->timeline_start.Compare(cursor) > 0) {
        children.push_back(json::Object()
                               .Add("OTIO_SCHEMA", "Gap.1")
                               .Add("name", "")
                               .AddRaw("source_range", Range(RationalTime(0, 1), item->timeline_start.Subtract(cursor), context))
                               .AddRaw("metadata", "{}")
                               .AddRaw("markers", "[]")
                               .AddRaw("effects", "[]")
                               .Add("enabled", true)
                               .Build());
      }
      for (const auto& transition : track.transitions) {
        if (transition.to_item.has_value() && *transition.to_item == item->id && transition_done.insert(transition.id).second) {
          children.push_back(transition_json(transition));
        }
      }
      children.push_back(ItemJson(sequence, track, *item, depth, context));
      for (const auto& transition : track.transitions) {
        if (!transition.to_item.has_value() && transition.from_item.has_value() && *transition.from_item == item->id &&
            transition_done.insert(transition.id).second) {
          children.push_back(transition_json(transition));
        }
      }
      cursor = item->end();
    }
    for (const auto& transition : track.transitions) {
      if (transition_done.count(transition.id) == 0) {
        report_.Warn("track " + track.name, "transition " + transition.id + " refers to clips that are not on the track and was not written");
      }
    }

    auto effects = EffectsJson(track.effects);
    return json::Object()
        .Add("OTIO_SCHEMA", "Track.1")
        .Add("name", track.name.empty() ? track.id : track.name)
        .Add("kind", track.kind == model::TrackKind::Video ? "Video" : "Audio")
        .AddRaw("children", List(children))
        .AddNull("source_range")
        .AddRaw("metadata", json::Object()
                                .AddRaw("cutline", json::Object()
                                                       .Add("id", track.id)
                                                       .Add("order", track.order)
                                                       .Add("locked", track.locked)
                                                       .Add("muted", track.muted)
                                                       .Add("solo", track.solo)
                                                       .Add("gainDb", track.gain_db)
                                                       .Add("pan", track.pan)
                                                       .Add("channelLayout", track.channel_layout)
                                                       .Build())
                                .Build())
        .AddRaw("markers", "[]")
        .AddRaw("effects", List(effects))
        .Add("enabled", true)
        .Build();
  }

  [[nodiscard]] std::string ItemJson(const Sequence& sequence, const Track& track, const Item& item, int depth,
                                     const Context& context) {
    (void)sequence;
    if (item.source_kind == model::SourceKind::Sequence) {
      const auto* nested = timeline_.FindSequence(item.source_id);
      if (nested == nullptr) {
        report_.Error("track " + track.name + " / clip " + item.id, "the nested sequence " + item.source_id + " is not in the timeline");
        return "null";
      }
      // The nested stack's own times are in its own frame rate.
      const Context inner{nested->settings.frame_rate};
      auto text = StackJson(*nested, depth + 1, inner, item.name.empty() ? nested->settings.name : item.name, &item);
      return text;
    }

    const auto* media = timeline_.FindMedia(item.source_id);
    if (media == nullptr) {
      report_.Error("track " + track.name + " / clip " + item.id, "the media " + item.source_id + " is not in the timeline");
      return "null";
    }
    auto effects = EffectsJson(item.effects);
    if (item.playback_rate.Compare(RationalTime(1, 1)) != 0 || item.reversed) {
      const auto scalar = static_cast<double>(item.playback_rate.numerator()) / static_cast<double>(item.playback_rate.denominator());
      effects.push_back(json::Object()
                            .Add("OTIO_SCHEMA", "LinearTimeWarp.1")
                            .Add("name", "")
                            .Add("effect_name", "LinearTimeWarp")
                            .Add("time_scalar", item.reversed ? -scalar : scalar)
                            .AddRaw("metadata", "{}")
                            .Build());
    }
    const auto start_in_media = item.source_in.Add(media->start_timecode);
    // A clip's length in the track is its timeline duration; the media it reads is in the
    // metadata, exactly, for a retimed clip.
    const auto reference =
        json::Object()
            .Add("OTIO_SCHEMA", "ExternalReference.1")
            .Add("name", media->name)
            .AddRaw("available_range", Range(media->start_timecode, media->duration, context))
            .Add("target_url", media->url)
            .AddRaw("metadata", json::Object()
                                    .AddRaw("cutline", json::Object().Add("id", media->id).Add("fingerprint", media->fingerprint).Build())
                                    .Build())
            .Build();
    return json::Object()
        .Add("OTIO_SCHEMA", "Clip.2")
        .Add("name", item.name.empty() ? media->name : item.name)
        .AddRaw("source_range", Range(start_in_media, item.duration(), context))
        .AddRaw("media_reference", reference)
        .AddRaw("metadata", json::Object().AddRaw("cutline", ClipMetadata(item, nullptr)).Build())
        .AddRaw("markers", List(MarkersJson(item.markers, context)))
        .AddRaw("effects", List(effects))
        .Add("enabled", item.enabled)
        .Build();
  }

  const Timeline& timeline_;
  Report& report_;
  std::set<std::string> written_nested_;
};

// ------------------------------------------------------------------ reading ----

[[nodiscard]] std::string Schema(const json::Value& value) {
  const auto* schema = value.Find("OTIO_SCHEMA");
  return schema != nullptr && schema->is_string() ? schema->text : std::string{};
}

[[nodiscard]] bool SchemaIs(const json::Value& value, const std::string& name) {
  const auto schema = Schema(value);
  return schema.size() > name.size() && schema.compare(0, name.size(), name) == 0 && schema[name.size()] == '.';
}

[[nodiscard]] double NumberOf(const json::Value* value, double fallback) {
  return value != nullptr && value->is_number() ? value->number : fallback;
}

[[nodiscard]] std::string TextOf(const json::Value* value) {
  return value != nullptr && value->is_string() ? value->text : std::string{};
}

[[nodiscard]] RationalTime ExactOf(const json::Value* value) {
  if (value == nullptr || !value->is_object()) return RationalTime(0, 1);
  return RationalTime(value->Integer("n"), value->Integer("d"));
}

struct ReadContext final {
  Report& report;
  Timeline timeline;
  std::map<std::string, std::string> media_by_url;
  int counter{0};
  bool assumed_settings{false};
  commands::SequenceSettings defaults;

  [[nodiscard]] std::string NextId(const std::string& prefix) { return prefix + std::to_string(++counter); }
};

[[nodiscard]] RationalTime ReadRt(const json::Value& value, ReadContext& context, const std::string& where) {
  const auto rate_value = NumberOf(value.Find("rate"), 1.0);
  bool exact = true;
  const auto rate = RateFromDouble(rate_value, exact);
  if (!exact) context.report.Warn(where, "the rate " + std::to_string(rate_value) + " is not a standard one and was approximated as " +
                                             std::to_string(rate.numerator) + "/" + std::to_string(rate.denominator));
  const auto number = NumberOf(value.Find("value"), 0.0);
  const auto whole = std::llround(number);
  if (std::abs(number - static_cast<double>(whole)) < 1e-9) return RationalTime(whole * rate.denominator, rate.numerator);
  // A fraction of a frame: kept to a thousandth of one.
  context.report.Note(where, "a time of " + std::to_string(number) + " units was rounded to the nearest thousandth of a unit");
  return RationalTime(std::llround(number * 1000.0) * rate.denominator, rate.numerator * 1000);
}

struct TimeSpan final {
  RationalTime start;
  RationalTime duration;
  time::FrameRate rate{25, 1};
  bool present{false};
};

[[nodiscard]] TimeSpan ReadRange(const json::Value* value, ReadContext& context, const std::string& where) {
  TimeSpan range;
  if (value == nullptr || !value->is_object()) return range;
  const auto* start = value->Find("start_time");
  const auto* duration = value->Find("duration");
  if (start == nullptr || duration == nullptr) return range;
  range.start = ReadRt(*start, context, where);
  range.duration = ReadRt(*duration, context, where);
  bool exact = true;
  range.rate = RateFromDouble(NumberOf(start->Find("rate"), 25.0), exact);
  range.present = true;
  return range;
}

[[nodiscard]] const json::Value* CutlineOf(const json::Value& value) {
  const auto* metadata = value.Find("metadata");
  if (metadata == nullptr || !metadata->is_object()) return nullptr;
  const auto* cutline = metadata->Find("cutline");
  return cutline != nullptr && cutline->is_object() ? cutline : nullptr;
}

void ReadEffectList(const json::Value* list, const std::string& where, ReadContext& context,
                    std::vector<timeline::Effect>& effects, double* time_scalar) {
  if (list == nullptr || !list->is_array()) return;
  for (const auto& entry : list->items) {
    if (SchemaIs(entry, "LinearTimeWarp")) {
      if (time_scalar != nullptr) *time_scalar = NumberOf(entry.Find("time_scalar"), 1.0);
      continue;
    }
    const auto* cutline = CutlineOf(entry);
    if (!SchemaIs(entry, "Effect") || cutline == nullptr) {
      context.report.Warn(where, "the effect '" + TextOf(entry.Find("effect_name")) + "' (" + Schema(entry) +
                                     ") is not one Cutline has and was not imported");
      continue;
    }
    effects.push_back(EffectFromJson(*cutline));
  }
}

[[nodiscard]] model::MarkerKind SafeMarkerKind(const std::string& name) {
  try {
    return model::ParseMarkerKind(name);
  } catch (const std::exception&) {
    return model::MarkerKind::Comment;
  }
}

void ReadMarkerList(const json::Value* list, const std::string& where, ReadContext& context, std::vector<Marker>& markers) {
  if (list == nullptr || !list->is_array()) return;
  for (const auto& entry : list->items) {
    Marker marker;
    const auto* cutline = CutlineOf(entry);
    if (cutline != nullptr && cutline->Find("start") != nullptr) {
      marker = MarkerFromJson(*cutline);
    } else {
      const auto range = ReadRange(entry.Find("marked_range"), context, where);
      marker.id = context.NextId("otio-marker-");
      marker.start = range.start;
      marker.end = range.start.Add(range.duration);
      marker.color = TextOf(entry.Find("color"));
      for (auto& c : marker.color) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    marker.label = TextOf(entry.Find("name"));
    markers.push_back(std::move(marker));
  }
}

void ReadEffects(const json::Value& owner, const std::string& where, ReadContext& context, std::vector<timeline::Effect>& effects,
                 double* time_scalar) {
  ReadEffectList(owner.Find("effects"), where, context, effects, time_scalar);
}

void ReadMarkers(const json::Value& owner, const std::string& where, ReadContext& context, std::vector<Marker>& markers) {
  ReadMarkerList(owner.Find("markers"), where, context, markers);
}

[[nodiscard]] Sequence ReadStack(const json::Value& stack, ReadContext& context, int depth, const commands::SequenceSettings* inherited,
                                 const json::Value* sequence_metadata, const json::Value* sequence_extras = nullptr);

[[nodiscard]] commands::SequenceSettings SettingsFrom(const json::Value& m, const commands::SequenceSettings& fallback) {
  commands::SequenceSettings s = fallback;
  s.name = m.String("name");
  s.frame_rate = {m.Require("frameRate").Integer("num"), m.Require("frameRate").Integer("den")};
  s.width = m.Integer("width");
  s.height = m.Integer("height");
  s.pixel_aspect = {m.Require("pixelAspect").Integer("num"), m.Require("pixelAspect").Integer("den")};
  s.sample_rate = m.Integer("sampleRate");
  s.channel_layout = m.String("channelLayout");
  s.working_color_space = m.String("workingColorSpace");
  s.display_color_space = m.String("displayColorSpace");
  s.field_order = model::ParseFieldOrder(m.String("fieldOrder"));
  s.drop_frame = m.Bool("dropFrame");
  const auto version = m.Integer("renderVersion");
  if (version > 0) s.render_version = version;
  return s;
}

void ReadTrack(const json::Value& track_json, Sequence& sequence, ReadContext& context, int depth, int index) {
  (void)index;
  const auto kind_text = TextOf(track_json.Find("kind"));
  const auto where = "track " + TextOf(track_json.Find("name"));
  if (kind_text != "Video" && kind_text != "Audio") {
    context.report.Warn(where, "has the kind '" + kind_text + "', which is neither Video nor Audio, and was not imported");
    return;
  }
  Track track;
  const auto* cutline = CutlineOf(track_json);
  track.kind = kind_text == "Video" ? model::TrackKind::Video : model::TrackKind::Audio;
  track.name = TextOf(track_json.Find("name"));
  if (cutline != nullptr && cutline->Find("id") != nullptr) {
    track.id = cutline->String("id");
    track.order = cutline->Integer("order");
    track.locked = cutline->Bool("locked");
    track.muted = cutline->Bool("muted");
    track.solo = cutline->Bool("solo");
    track.gain_db = cutline->Number("gainDb");
    track.pan = cutline->Number("pan");
    track.channel_layout = cutline->String("channelLayout");
  } else {
    track.id = context.NextId(track.kind == model::TrackKind::Video ? "otio-video-" : "otio-audio-");
    track.order = 0;
    for (const auto& other : sequence.tracks) {
      if (other.kind == track.kind) track.order = std::max(track.order, other.order + 1);
    }
  }
  ReadEffects(track_json, where, context, track.effects, nullptr);
  if (const auto* markers = track_json.Find("markers"); markers != nullptr && markers->is_array() && !markers->items.empty()) {
    context.report.Warn(where, "has markers on the track itself, which Cutline does not have; they were not imported");
  }

  RationalTime cursor(0, 1);
  struct Pending final {
    Transition transition;
    bool from_known{false};
    bool wants_next{false};
  };
  std::vector<Pending> pending;
  std::string previous_item;  // id of the last clip, when the child just before is one
  const auto* children = track_json.Find("children");
  if (children != nullptr && children->is_array()) {
    for (const auto& child : children->items) {
      const auto schema = Schema(child);
      const auto child_where = where + " / " + (TextOf(child.Find("name")).empty() ? schema : TextOf(child.Find("name")));
      if (SchemaIs(child, "Gap")) {
        cursor = cursor.Add(ReadRange(child.Find("source_range"), context, child_where).duration);
        previous_item.clear();
        continue;
      }
      if (SchemaIs(child, "Transition")) {
        Pending entry;
        const auto* t = CutlineOf(child);
        if (t != nullptr && t->Find("start") != nullptr) {
          entry.transition.id = t->String("id");
          entry.transition.kind = t->String("kind");
          entry.transition.alignment = model::ParseTransitionAlignment(t->String("alignment"));
          entry.transition.start = ExactOf(t->Find("start"));
          entry.transition.duration = ExactOf(t->Find("duration"));
          if (t->Find("from") != nullptr) entry.transition.from_item = t->String("from");
          if (t->Find("to") != nullptr) entry.transition.to_item = t->String("to");
          entry.from_known = true;
        } else {
          const auto in_offset = ReadRt(child.Require("in_offset"), context, child_where);
          const auto out_offset = ReadRt(child.Require("out_offset"), context, child_where);
          entry.transition.id = context.NextId("otio-transition-");
          const auto type = TextOf(child.Find("transition_type"));
          entry.transition.kind = "cross_dissolve";
          if (type != "SMPTE_Dissolve") {
            context.report.Warn(child_where, "the transition type '" + type + "' is not one Cutline has; it was imported as a cross dissolve");
          }
          entry.transition.start = cursor.Subtract(in_offset);
          entry.transition.duration = in_offset.Add(out_offset);
          const bool in_zero = in_offset.Compare(RationalTime(0, 1)) == 0;
          const bool out_zero = out_offset.Compare(RationalTime(0, 1)) == 0;
          entry.transition.alignment = in_zero ? model::TransitionAlignment::Start
                                       : out_zero ? model::TransitionAlignment::End
                                       : in_offset.Compare(out_offset) == 0 ? model::TransitionAlignment::Center
                                                                              : model::TransitionAlignment::Custom;
          if (!previous_item.empty()) entry.transition.from_item = previous_item;
          entry.wants_next = true;
        }
        pending.push_back(std::move(entry));
        continue;
      }

      const bool is_clip = SchemaIs(child, "Clip");
      const bool is_stack = SchemaIs(child, "Stack");
      if (!is_clip && !is_stack) {
        context.report.Warn(child_where, "an item of type " + schema + " is not supported and was not imported");
        const auto range = ReadRange(child.Find("source_range"), context, child_where);
        if (range.present) cursor = cursor.Add(range.duration);
        previous_item.clear();
        continue;
      }

      Item item;
      item.timeline_start = cursor;
      const auto* cut = CutlineOf(child);
      const auto range = ReadRange(child.Find("source_range"), context, child_where);
      double time_scalar = 1.0;
      ReadEffects(child, child_where, context, item.effects, &time_scalar);
      ReadMarkers(child, child_where, context, item.markers);
      if (const auto* enabled = child.Find("enabled"); enabled != nullptr && enabled->kind == json::Value::Kind::Bool) {
        item.enabled = enabled->boolean;
      }
      item.name = TextOf(child.Find("name"));

      if (is_stack) {
        const auto* inner_meta = cut != nullptr ? cut->Find("sequence") : nullptr;
        const commands::SequenceSettings& fallback = context.defaults;
        auto nested = ReadStack(child, context, depth + 1, &fallback, inner_meta, cut);
        item.source_kind = model::SourceKind::Sequence;
        item.source_id = nested.id;
        if (context.timeline.FindSequence(nested.id) == nullptr) context.timeline.sequences.push_back(std::move(nested));
      } else {
        const auto* reference = child.Find("media_reference");
        const std::string schema_name = reference != nullptr ? Schema(*reference) : std::string{};
        if (reference == nullptr || (!SchemaIs(*reference, "ExternalReference") && !SchemaIs(*reference, "MissingReference"))) {
          context.report.Error(child_where, "the media reference " + (schema_name.empty() ? std::string("(none)") : schema_name) +
                                                " is not supported; the clip was left out as a gap");
          if (range.present) cursor = cursor.Add(range.duration);
          previous_item.clear();
          continue;
        }
        const auto url = SchemaIs(*reference, "ExternalReference") ? TextOf(reference->Find("target_url")) : std::string{};
        const auto available = ReadRange(reference->Find("available_range"), context, child_where);
        const auto* ref_cut = CutlineOf(*reference);
        std::string media_id;
        const auto key = url.empty() ? "missing:" + TextOf(child.Find("name")) : url;
        const auto found = context.media_by_url.find(key);
        if (found != context.media_by_url.end()) {
          media_id = found->second;
        } else {
          Media media;
          media.id = ref_cut != nullptr && ref_cut->Find("id") != nullptr ? ref_cut->String("id") : context.NextId("otio-media-");
          media.name = TextOf(reference->Find("name"));
          if (media.name.empty()) media.name = TextOf(child.Find("name"));
          if (media.name.empty()) media.name = media.id;
          media.url = url.empty() ? "otio:offline:" + media.name : url;
          media.fingerprint = ref_cut != nullptr && ref_cut->Find("fingerprint") != nullptr ? ref_cut->String("fingerprint") : "";
          if (available.present) {
            media.duration = available.duration;
            media.start_timecode = available.start;
          } else {
            context.report.Warn(child_where, "the media has no available range; its length was taken from the part used");
            media.duration = range.start.Add(range.duration);
          }
          if (SchemaIs(*reference, "MissingReference")) {
            context.report.Warn(child_where, "the media is missing in the file; the clip refers to an offline item");
          }
          context.media_by_url.emplace(key, media.id);
          media_id = media.id;
          context.timeline.media.push_back(std::move(media));
        }
        item.source_kind = model::SourceKind::Media;
        item.source_id = media_id;
        const auto* media = context.timeline.FindMedia(media_id);
        const auto offset = media != nullptr ? media->start_timecode : RationalTime(0, 1);
        item.source_in = range.start.Subtract(offset);
        const auto scalar = std::abs(time_scalar) < 1e-9 ? 1.0 : std::abs(time_scalar);
        bool exact = true;
        const auto rate = RateFromDouble(scalar, exact);
        item.playback_rate = RationalTime(rate.numerator, rate.denominator);
        if (!exact) context.report.Warn(child_where, "the speed " + std::to_string(scalar) + " was approximated");
        item.reversed = time_scalar < 0;
        item.source_out = item.source_in.Add(range.duration.Multiply(item.playback_rate));
      }

      if (cut != nullptr && cut->Find("id") != nullptr) {
        item.id = cut->String("id");
        item.timeline_start = ExactOf(cut->Find("timelineStart"));
        item.source_in = ExactOf(cut->Find("sourceIn"));
        item.source_out = ExactOf(cut->Find("sourceOut"));
        item.playback_rate = ExactOf(cut->Find("playbackRate"));
        item.reversed = cut->Bool("reversed");
        item.maintain_pitch = cut->Bool("maintainPitch");
        item.linked_group = cut->String("linkedGroup");
        if (is_stack) item.source_id = cut->String("sourceId");
        else if (item.source_kind == model::SourceKind::Media) {
          // The media id this file used for it, where the file said.
        }
      } else {
        item.id = context.NextId("otio-clip-");
        if (is_stack) {
          item.source_in = range.present ? range.start : RationalTime(0, 1);
          item.source_out = item.source_in.Add(range.duration);
        }
      }
      const auto duration = range.present ? range.duration : item.duration();
      cursor = (cut != nullptr && cut->Find("id") != nullptr) ? item.end() : cursor.Add(duration);
      for (auto& entry : pending) {
        if (entry.wants_next && !entry.transition.to_item.has_value()) entry.transition.to_item = item.id;
      }
      previous_item = item.id;
      track.items.push_back(std::move(item));
    }
  }
  for (auto& entry : pending) track.transitions.push_back(std::move(entry.transition));
  sequence.tracks.push_back(std::move(track));
}

Sequence ReadStack(const json::Value& stack, ReadContext& context, int depth, const commands::SequenceSettings* inherited,
                   const json::Value* sequence_metadata, const json::Value* sequence_extras) {
  Sequence sequence;
  const auto* cutline = sequence_metadata;
  if (cutline == nullptr) {
    const auto* own = CutlineOf(stack);
    if (own != nullptr && own->Find("sequence") != nullptr) cutline = own->Find("sequence");
  }
  if (cutline != nullptr && cutline->is_object() && cutline->Find("frameRate") != nullptr) {
    sequence.settings = SettingsFrom(*cutline, inherited != nullptr ? *inherited : context.defaults);
    sequence.id = cutline->String("id");
  } else {
    sequence.settings = inherited != nullptr ? *inherited : context.defaults;
    sequence.settings.name = TextOf(stack.Find("name"));
    sequence.id = context.NextId("otio-sequence-");
  }
  if (depth > 8) {
    context.report.Error("stack " + sequence.settings.name, "nested stacks are too deep to import");
    return sequence;
  }
  const auto* children = stack.Find("children");
  if (children != nullptr && children->is_array()) {
    int index = 0;
    for (const auto& child : children->items) {
      if (SchemaIs(child, "Track")) {
        ReadTrack(child, sequence, context, depth, index++);
      } else {
        context.report.Warn("stack " + sequence.settings.name, "a " + Schema(child) + " directly in a stack is not supported and was not imported");
      }
    }
  }
  const auto where = "sequence " + sequence.settings.name;
  if (depth == 0) {
    ReadEffects(stack, where, context, sequence.effects, nullptr);
    ReadMarkers(stack, where, context, sequence.markers);
  } else if (sequence_extras != nullptr) {
    ReadEffectList(sequence_extras->Find("sequenceEffects"), where, context, sequence.effects, nullptr);
    ReadMarkerList(sequence_extras->Find("sequenceMarkers"), where, context, sequence.markers);
  }
  return sequence;
}

}  // namespace

std::string WriteOtio(const Timeline& timeline, Report& report) { return Writer(timeline, report).Write(); }

Timeline ReadOtio(const std::string& text, Report& report) {
  ReadContext context{report, {}, {}, 0, false, {}};
  json::Value root;
  try {
    root = json::Parse(text);
  } catch (const std::exception& error) {
    report.Error("file", std::string("this is not valid JSON: ") + error.what());
    return {};
  }
  if (!SchemaIs(root, "Timeline")) {
    report.Error("file", "the top-level object is " + (Schema(root).empty() ? std::string("not an OpenTimelineIO object") : Schema(root)) +
                             ", not a Timeline");
    return {};
  }
  if (const auto* start = root.Find("global_start_time"); start != nullptr && start->is_object()) {
    const auto offset = ReadRt(*start, context, "timeline");
    if (offset.Compare(RationalTime(0, 1)) != 0) {
      report.Warn("timeline", "the timeline starts at a non-zero time; clips were placed from zero");
    }
  }

  // Defaults, replaced by what the file records of its sequence, or else guessed from its clips.
  context.defaults.name = TextOf(root.Find("name"));
  context.defaults.frame_rate = {25, 1};
  context.defaults.width = 1920;
  context.defaults.height = 1080;
  context.defaults.sample_rate = 48000;
  const auto* tracks = root.Find("tracks");
  if (tracks == nullptr || !SchemaIs(*tracks, "Stack")) {
    report.Error("timeline", "has no stack of tracks");
    return {};
  }
  const auto* own = CutlineOf(*tracks);
  const auto* top = CutlineOf(root);
  const json::Value* sequence_metadata = top != nullptr ? top->Find("sequence") : nullptr;
  if (sequence_metadata == nullptr && own != nullptr) sequence_metadata = own->Find("sequence");
  if (sequence_metadata == nullptr) {
    // No sequence settings in the file: take the rate of the first clip's range, and say so.
    std::function<const json::Value*(const json::Value&)> first_range = [&](const json::Value& node) -> const json::Value* {
      if (const auto* range = node.Find("source_range"); range != nullptr && range->is_object() && SchemaIs(node, "Clip")) return range;
      if (const auto* children = node.Find("children"); children != nullptr && children->is_array()) {
        for (const auto& child : children->items) {
          if (const auto* found = first_range(child)) return found;
        }
      }
      return nullptr;
    };
    if (const auto* range = first_range(*tracks)) {
      if (const auto* start = range->Find("start_time"); start != nullptr) {
        bool exact = true;
        context.defaults.frame_rate = RateFromDouble(NumberOf(start->Find("rate"), 25.0), exact);
      }
    }
    report.Note("timeline", "the file has no sequence settings; the frame rate " + std::to_string(context.defaults.frame_rate.numerator) + "/" +
                                std::to_string(context.defaults.frame_rate.denominator) + " was taken from its first clip and the size set to 1920x1080");
  }
  auto sequence = ReadStack(*tracks, context, 0, &context.defaults, sequence_metadata);
  if (sequence.settings.name.empty()) sequence.settings.name = context.defaults.name;
  // The root sequence goes first, the sequences it nests after it.
  context.timeline.sequences.insert(context.timeline.sequences.begin(), std::move(sequence));
  return std::move(context.timeline);
}

}  // namespace cutline::interchange

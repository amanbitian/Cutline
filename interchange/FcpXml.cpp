#include "interchange/FcpXml.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"
#include "core/util/XmlParse.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace cutline::interchange {

using time::RationalTime;

namespace {

constexpr const char* kCutlinePrefix = "cutline:";

[[nodiscard]] bool OnFrame(const RationalTime& t, const time::FrameRate& fps) {
  const auto top = t.numerator() * fps.numerator;
  const auto bottom = t.denominator() * fps.denominator;
  return bottom != 0 && top % bottom == 0;
}

[[nodiscard]] time::FrameRate RateOfXml(const xml::Node* rate, const time::FrameRate& fallback) {
  if (rate == nullptr) return fallback;
  const auto base = std::atoll(rate->ChildText("timebase", "0").c_str());
  if (base <= 0) return fallback;
  const auto ntsc = rate->ChildText("ntsc", "FALSE");
  const bool is_ntsc = ntsc == "TRUE" || ntsc == "true";
  return is_ntsc ? time::FrameRate{base * 1000, 1001} : time::FrameRate{base, 1};
}

// ------------------------------------------------------------------- writing ----

class Writer final {
 public:
  Writer(const Timeline& timeline, Report& report) : timeline_(timeline), report_(report) {}

  [[nodiscard]] std::string Write() {
    if (timeline_.sequences.empty()) {
      report_.Error("timeline", "there is no sequence to write");
      return {};
    }
    out_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE xmeml>\n";
    Open("xmeml", {{"version", "4"}});
    WriteSequence(timeline_.sequences.front(), 0, true);
    Close("xmeml");
    if (rounded_) {
      report_.Warn("timeline", "some times are not on a frame boundary and were rounded to the nearest frame; the format counts whole frames");
    }
    return out_.str();
  }

 private:
  using Attributes = std::vector<std::pair<std::string, std::string>>;

  void Indent() { out_ << std::string(static_cast<std::size_t>(depth_) * 2, ' '); }

  void Open(const std::string& name, const Attributes& attributes = {}) {
    Indent();
    out_ << "<" << name;
    for (const auto& [key, value] : attributes) out_ << " " << key << "=\"" << xml::Escape(value) << "\"";
    out_ << ">\n";
    ++depth_;
  }
  void Close(const std::string& name) {
    --depth_;
    Indent();
    out_ << "</" << name << ">\n";
  }
  void Leaf(const std::string& name, const std::string& value) {
    Indent();
    out_ << "<" << name << ">" << xml::Escape(value) << "</" << name << ">\n";
  }
  void Empty(const std::string& name, const Attributes& attributes) {
    Indent();
    out_ << "<" << name;
    for (const auto& [key, value] : attributes) out_ << " " << key << "=\"" << xml::Escape(value) << "\"";
    out_ << "/>\n";
  }

  [[nodiscard]] std::int64_t Frames(const RationalTime& t, const time::FrameRate& fps) {
    if (!OnFrame(t, fps)) rounded_ = true;
    return t.ToFrames(fps, time::RoundingMode::Nearest);
  }

  void WriteRate(const time::FrameRate& fps) {
    Open("rate");
    if (fps.denominator == 1001) {
      Leaf("timebase", std::to_string((fps.numerator + 500) / 1000));
      Leaf("ntsc", "TRUE");
    } else {
      if (fps.denominator != 1) {
        report_.Warn("rate", "the frame rate " + std::to_string(fps.numerator) + "/" + std::to_string(fps.denominator) +
                                 " cannot be written exactly in this format; it was written as " + std::to_string(fps.numerator / fps.denominator));
      }
      Leaf("timebase", std::to_string(fps.numerator / std::max<std::int64_t>(1, fps.denominator)));
      Leaf("ntsc", "FALSE");
    }
    Close("rate");
  }

  void WriteTimecode(const RationalTime& start, const time::FrameRate& fps, bool drop) {
    Open("timecode");
    WriteRate(fps);
    Leaf("string", start.FormatTimecode(fps, drop, time::RoundingMode::Nearest));
    Leaf("frame", std::to_string(Frames(start, fps)));
    Leaf("displayformat", drop ? "DF" : "NDF");
    Close("timecode");
  }

  [[nodiscard]] static std::string UrlOf(const std::string& path) {
    if (path.rfind("file:", 0) == 0) return path;
    if (!path.empty() && path[0] == '/') return "file://localhost" + path;
    return "file://localhost/" + path;
  }

  void WriteFile(const Media& media, const time::FrameRate& fps, bool drop) {
    const auto id = "file-" + media.id;
    if (!written_files_.insert(id).second) {
      Empty("file", {{"id", id}});
      return;
    }
    Open("file", {{"id", id}});
    Leaf("name", media.name);
    Leaf("pathurl", UrlOf(media.url));
    WriteRate(fps);
    Leaf("duration", std::to_string(Frames(media.duration, fps)));
    WriteTimecode(media.start_timecode, fps, drop);
    Close("file");
  }

  static std::string ClipBlob(const Item& item) {
    std::vector<std::string> effects, markers;
    for (const auto& effect : item.effects) effects.push_back(EffectToJson(effect));
    for (const auto& marker : item.markers) markers.push_back(MarkerToJson(marker));
    return kCutlinePrefix + json::Object()
                                .Add("id", item.id)
                                .Add("name", item.name)
                                .Add("sourceKind", model::ToString(item.source_kind))
                                .Add("sourceId", item.source_id)
                                .Add("maintainPitch", item.maintain_pitch)
                                .Add("linkedGroup", item.linked_group)
                                .AddRaw("effects", json::Array(effects))
                                .AddRaw("markers", json::Array(markers))
                                .Build();
  }

  void WriteComment(const std::string& blob) {
    Open("comments");
    Leaf("mastercomment1", blob);
    Close("comments");
  }

  void WriteTimeRemap(const Item& item) {
    const auto percent = static_cast<double>(item.playback_rate.numerator()) * 100.0 / static_cast<double>(item.playback_rate.denominator());
    Open("filter");
    Open("effect");
    Leaf("name", "Time Remap");
    Leaf("effectid", "timeremap");
    Leaf("effectcategory", "motion");
    Leaf("effecttype", "motion");
    Leaf("mediatype", "video");
    Open("parameter", {{"authoringApp", "PremierePro"}});
    Leaf("parameterid", "speed");
    Leaf("name", "speed");
    Leaf("valuemin", "-100000");
    Leaf("valuemax", "100000");
    Leaf("value", json::Number(percent));
    Close("parameter");
    Open("parameter", {{"authoringApp", "PremierePro"}});
    Leaf("parameterid", "reverse");
    Leaf("name", "reverse");
    Leaf("value", item.reversed ? "TRUE" : "FALSE");
    Close("parameter");
    Close("effect");
    Close("filter");
  }

  struct Position final {
    std::string id;
    std::string media_type;
    int track{0};
    int clip{0};
  };

  void WriteSequence(const Sequence& sequence, int nesting, bool root) {
    if (nesting > 8) {
      report_.Error("sequence " + sequence.settings.name, "nested sequences are too deep to write");
      return;
    }
    const auto fps = sequence.settings.frame_rate;
    const bool drop = sequence.settings.drop_frame;
    Open("sequence", {{"id", "sequence-" + std::to_string(++sequence_counter_)}});
    Leaf("name", sequence.settings.name);
    RationalTime end(0, 1);
    for (const auto& track : sequence.tracks) {
      for (const auto& item : track.items) {
        if (item.end().Compare(end) > 0) end = item.end();
      }
    }
    Leaf("duration", std::to_string(Frames(end, fps)));
    WriteRate(fps);
    WriteTimecode(RationalTime(0, 1), fps, drop);

    // The ids of this sequence's clip items, and where each sits, for the links between them.
    std::map<std::string, std::string> ids;
    std::map<std::string, std::vector<Position>> groups;
    {
      int video_index = 0, audio_index = 0;
      std::vector<const Track*> video, audio;
      for (const auto& track : sequence.tracks) (track.kind == model::TrackKind::Video ? video : audio).push_back(&track);
      const auto number = [&](const std::vector<const Track*>& list, const char* type) {
        int track_index = 0;
        for (const auto* track : list) {
          ++track_index;
          int clip_index = 0;
          for (const auto& item : track->items) {
            ++clip_index;
            const auto id = "clipitem-" + std::to_string(++clip_counter_);
            ids[track->id + "/" + item.id] = id;
            if (!item.linked_group.empty()) groups[item.linked_group].push_back({id, type, track_index, clip_index});
          }
        }
      };
      number(video, "video");
      number(audio, "audio");
      (void)video_index;
      (void)audio_index;
    }

    Open("media");
    for (const auto kind : {model::TrackKind::Video, model::TrackKind::Audio}) {
      const bool is_video = kind == model::TrackKind::Video;
      Open(is_video ? "video" : "audio");
      if (is_video) {
        Open("format");
        Open("samplecharacteristics");
        WriteRate(fps);
        Leaf("width", std::to_string(sequence.settings.width));
        Leaf("height", std::to_string(sequence.settings.height));
        Leaf("anamorphic", "FALSE");
        Leaf("pixelaspectratio", "square");
        Leaf("fielddominance", sequence.settings.field_order == model::FieldOrder::UpperFirst   ? "upper"
                               : sequence.settings.field_order == model::FieldOrder::LowerFirst ? "lower"
                                                                                                 : "none");
        Close("samplecharacteristics");
        Close("format");
      } else {
        Leaf("numOutputChannels", "2");
        Open("format");
        Open("samplecharacteristics");
        Leaf("depth", "16");
        Leaf("samplerate", std::to_string(sequence.settings.sample_rate));
        Close("samplecharacteristics");
        Close("format");
      }
      std::vector<const Track*> tracks;
      for (const auto& track : sequence.tracks) {
        if (track.kind == kind) tracks.push_back(&track);
      }
      std::sort(tracks.begin(), tracks.end(), [](const Track* a, const Track* b) { return a->order < b->order; });
      for (const auto* track : tracks) WriteTrack(sequence, *track, ids, groups, nesting);
      Close(is_video ? "video" : "audio");
    }
    Close("media");

    for (const auto& marker : sequence.markers) {
      Open("marker");
      Leaf("name", marker.label);
      Leaf("comment", "");
      Leaf("in", std::to_string(Frames(marker.start, fps)));
      Leaf("out", marker.end.Compare(marker.start) == 0 ? "-1" : std::to_string(Frames(marker.end, fps)));
      Close("marker");
    }
    WriteComment(SequenceBlob(sequence, root));
    Close("sequence");
  }

  [[nodiscard]] std::string SequenceBlob(const Sequence& sequence, bool root) const {
    std::vector<std::string> tracks;
    for (const auto& track : sequence.tracks) {
      std::vector<std::string> effects, transitions;
      for (const auto& effect : track.effects) effects.push_back(EffectToJson(effect));
      for (const auto& transition : track.transitions) {
        auto object = json::Object().Add("id", transition.id).Add("kind", transition.kind);
        if (transition.from_item.has_value()) object.Add("from", *transition.from_item);
        if (transition.to_item.has_value()) object.Add("to", *transition.to_item);
        transitions.push_back(object.Build());
      }
      tracks.push_back(json::Object()
                           .Add("id", track.id)
                           .Add("name", track.name)
                           .Add("kind", model::ToString(track.kind))
                           .Add("order", track.order)
                           .Add("locked", track.locked)
                           .Add("muted", track.muted)
                           .Add("solo", track.solo)
                           .Add("gainDb", track.gain_db)
                           .Add("pan", track.pan)
                           .Add("channelLayout", track.channel_layout)
                           .AddRaw("effects", json::Array(effects))
                           .AddRaw("transitions", json::Array(transitions))
                           .Build());
    }
    std::vector<std::string> sequence_effects, markers, media;
    for (const auto& effect : sequence.effects) sequence_effects.push_back(EffectToJson(effect));
    for (const auto& marker : sequence.markers) markers.push_back(MarkerToJson(marker));
    if (root) {
      for (const auto& m : timeline_.media) {
        media.push_back(json::Object().Add("fileId", "file-" + m.id).Add("id", m.id).Add("name", m.name).Add("url", m.url).Add("fingerprint", m.fingerprint).Build());
      }
    }
    const auto& s = sequence.settings;
    return kCutlinePrefix + json::Object()
                                .Add("id", sequence.id)
                                .Add("channelLayout", s.channel_layout)
                                .Add("workingColorSpace", s.working_color_space)
                                .Add("displayColorSpace", s.display_color_space)
                                .Add("pixelAspect", s.pixel_aspect)
                                .Add("renderVersion", s.render_version.value_or(0))
                                .AddRaw("tracks", json::Array(tracks))
                                .AddRaw("effects", json::Array(sequence_effects))
                                .AddRaw("markers", json::Array(markers))
                                .AddRaw("media", json::Array(media))
                                .Build();
  }

  void WriteTrack(const Sequence& sequence, const Track& track, const std::map<std::string, std::string>& ids,
                  const std::map<std::string, std::vector<Position>>& groups, int nesting) {
    const auto fps = sequence.settings.frame_rate;
    const bool video = track.kind == model::TrackKind::Video;
    Open("track");
    Leaf("enabled", track.muted ? "FALSE" : "TRUE");
    Leaf("locked", track.locked ? "TRUE" : "FALSE");

    std::vector<const Item*> items;
    for (const auto& item : track.items) items.push_back(&item);
    std::sort(items.begin(), items.end(), [](const Item* a, const Item* b) { return a->timeline_start.Compare(b->timeline_start) < 0; });
    // Items and transitions in timeline order, a transition after the clip it follows.
    struct Entry final {
      RationalTime at;
      int rank;  // 0 clip, 1 transition at the same time
      const Item* item;
      const Transition* transition;
    };
    std::vector<Entry> entries;
    for (const auto* item : items) entries.push_back({item->timeline_start, 0, item, nullptr});
    for (const auto& transition : track.transitions) entries.push_back({transition.start, 1, nullptr, &transition});
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      const auto order = a.at.Compare(b.at);
      return order != 0 ? order < 0 : a.rank < b.rank;
    });
    for (const auto& entry : entries) {
      if (entry.item != nullptr) WriteClipItem(sequence, track, *entry.item, fps, video, ids, groups, nesting);
      else WriteTransition(track, *entry.transition, fps, video);
    }
    Close("track");
  }

  void WriteTransition(const Track& track, const Transition& transition, const time::FrameRate& fps, bool video) {
    std::string alignment = "center";
    if (!transition.from_item.has_value()) alignment = "start-black";
    else if (!transition.to_item.has_value()) alignment = "end-black";
    else if (transition.alignment == model::TransitionAlignment::Start) alignment = "start";
    else if (transition.alignment == model::TransitionAlignment::End) alignment = "end";
    else if (transition.alignment == model::TransitionAlignment::Custom) {
      report_.Warn("track " + track.name + " / transition " + transition.id, "a custom alignment cannot be written; it was written centred");
    }
    Open("transitionitem");
    Leaf("start", std::to_string(Frames(transition.start, fps)));
    Leaf("end", std::to_string(Frames(transition.start.Add(transition.duration), fps)));
    Leaf("alignment", alignment);
    Open("effect");
    if (video) {
      Leaf("name", "Cross Dissolve");
      Leaf("effectid", "Cross Dissolve");
      Leaf("effectcategory", "Dissolve");
      Leaf("effecttype", "transition");
      Leaf("mediatype", "video");
      Leaf("wipecode", "0");
    } else {
      const bool equal_power = transition.kind == "constant_power";
      Leaf("name", equal_power ? "Cross Fade (+3dB)" : "Cross Fade (0dB)");
      Leaf("effectid", equal_power ? "Cross Fade (+3dB)" : "Cross Fade (0dB)");
      Leaf("effectcategory", "Crossfades");
      Leaf("effecttype", "transition");
      Leaf("mediatype", "audio");
    }
    Close("effect");
    Close("transitionitem");
  }

  void WriteClipItem(const Sequence& sequence, const Track& track, const Item& item, const time::FrameRate& fps, bool video,
                     const std::map<std::string, std::string>& ids, const std::map<std::string, std::vector<Position>>& groups,
                     int nesting) {
    const auto where = "track " + track.name + " / clip " + (item.name.empty() ? item.id : item.name);
    const Sequence* nested = nullptr;
    const Media* media = nullptr;
    time::FrameRate clip_rate = fps;
    if (item.source_kind == model::SourceKind::Sequence) {
      nested = timeline_.FindSequence(item.source_id);
      if (nested == nullptr) {
        report_.Error(where, "the nested sequence " + item.source_id + " is not in the timeline");
        return;
      }
      clip_rate = nested->settings.frame_rate;
    } else {
      media = timeline_.FindMedia(item.source_id);
      if (media == nullptr) {
        report_.Error(where, "the media " + item.source_id + " is not in the timeline");
        return;
      }
    }
    Open("clipitem", {{"id", ids.at(track.id + "/" + item.id)}});
    Leaf("name", item.name.empty() ? (media != nullptr ? media->name : nested->settings.name) : item.name);
    Leaf("enabled", item.enabled ? "TRUE" : "FALSE");
    Leaf("duration", std::to_string(Frames(media != nullptr ? media->duration : item.source_out, media != nullptr ? fps : clip_rate)));
    WriteRate(clip_rate);
    Leaf("start", std::to_string(Frames(item.timeline_start, fps)));
    Leaf("end", std::to_string(Frames(item.end(), fps)));
    Leaf("in", std::to_string(Frames(item.source_in, clip_rate)));
    Leaf("out", std::to_string(Frames(item.source_out, clip_rate)));
    if (media != nullptr) WriteFile(*media, fps, sequence.settings.drop_frame);
    if (nested != nullptr) WriteSequence(*nested, nesting + 1, false);
    if (item.playback_rate.Compare(RationalTime(1, 1)) != 0 || item.reversed) WriteTimeRemap(item);
    if (!video && item.maintain_pitch) {
      // Time Remap carries the pitch policy in FCP7 as a separate audio filter; this format has no
      // other way to say it, and the comment keeps it.
    }
    for (const auto& marker : item.markers) {
      Open("marker");
      Leaf("name", marker.label);
      Leaf("comment", "");
      Leaf("in", std::to_string(Frames(marker.start, fps)));
      Leaf("out", marker.end.Compare(marker.start) == 0 ? "-1" : std::to_string(Frames(marker.end, fps)));
      Close("marker");
    }
    if (!item.linked_group.empty()) {
      const auto found = groups.find(item.linked_group);
      if (found != groups.end()) {
        for (const auto& member : found->second) {
          Open("link");
          Leaf("linkclipref", member.id);
          Leaf("mediatype", member.media_type);
          Leaf("trackindex", std::to_string(member.track));
          Leaf("clipindex", std::to_string(member.clip));
          Close("link");
        }
      }
    }
    WriteComment(ClipBlob(item));
    Close("clipitem");
  }

  const Timeline& timeline_;
  Report& report_;
  std::ostringstream out_;
  int depth_{0};
  int clip_counter_{0};
  int sequence_counter_{0};
  bool rounded_{false};
  std::set<std::string> written_files_;
};

// ------------------------------------------------------------------- reading ----

struct ReadState final {
  Report& report;
  Timeline timeline;
  std::map<std::string, std::string> media_by_file;  // xml file id -> media id
  std::map<std::string, const xml::Node*> files;      // xml file id -> its full definition
  std::map<std::string, json::Value> blob_media;      // by xml file id
  int counter{0};

  [[nodiscard]] std::string NextId(const std::string& prefix) { return prefix + std::to_string(++counter); }
};

[[nodiscard]] std::int64_t IntOf(const xml::Node& node, const char* child, std::int64_t fallback) {
  const auto* found = node.Find(child);
  if (found == nullptr || found->text.empty()) return fallback;
  return std::atoll(found->text.c_str());
}

[[nodiscard]] std::string DecodeUrl(const std::string& url) {
  std::string path = url;
  for (const char* prefix : {"file://localhost", "file://"}) {
    if (path.rfind(prefix, 0) == 0) {
      path = path.substr(std::string(prefix).size());
      break;
    }
  }
  std::string out;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
      out.push_back(static_cast<char>(std::strtol(path.substr(i + 1, 2).c_str(), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(path[i]);
    }
  }
  return out;
}

[[nodiscard]] std::optional<json::Value> BlobOf(const xml::Node& node, ReadState& state, const std::string& where) {
  const auto* comments = node.Find("comments");
  if (comments == nullptr) return std::nullopt;
  const auto text = comments->ChildText("mastercomment1");
  if (text.rfind(kCutlinePrefix, 0) != 0) return std::nullopt;
  try {
    return json::Parse(text.substr(std::string(kCutlinePrefix).size()));
  } catch (const std::exception& error) {
    state.report.Warn(where, std::string("the Cutline data in the comments could not be read and was ignored: ") + error.what());
    return std::nullopt;
  }
}

void IndexFiles(const xml::Node& node, ReadState& state) {
  if (node.name == "file" && !node.children.empty()) {
    const auto id = node.Attribute("id");
    if (!id.empty() && state.files.count(id) == 0) state.files[id] = &node;
  }
  for (const auto& child : node.children) IndexFiles(child, state);
}

[[nodiscard]] std::string MediaFor(const xml::Node& file_ref, const time::FrameRate& fps, ReadState& state, const std::string& where) {
  const auto file_id = file_ref.Attribute("id");
  const auto cached = state.media_by_file.find(file_id);
  if (cached != state.media_by_file.end()) return cached->second;
  const auto defined = state.files.find(file_id);
  const xml::Node& file = defined != state.files.end() ? *defined->second : file_ref;
  Media media;
  const auto blob = state.blob_media.find(file_id);
  media.id = blob != state.blob_media.end() ? blob->second.String("id") : "xml-" + (file_id.empty() ? state.NextId("file-") : file_id);
  media.name = file.ChildText("name", media.id);
  media.url = blob != state.blob_media.end() ? blob->second.String("url") : DecodeUrl(file.ChildText("pathurl", "xml:offline:" + media.name));
  if (blob != state.blob_media.end()) media.fingerprint = blob->second.String("fingerprint");
  const auto file_rate = RateOfXml(file.Find("rate"), fps);
  const auto duration = IntOf(file, "duration", 0);
  media.duration = RationalTime::FromFrames(duration, file_rate);
  if (const auto* timecode = file.Find("timecode")) {
    media.start_timecode = RationalTime::FromFrames(IntOf(*timecode, "frame", 0), RateOfXml(timecode->Find("rate"), file_rate));
  }
  if (duration <= 0) state.report.Warn(where, "the file " + media.name + " has no length in the file; it was taken from the part used");
  state.media_by_file[file_id] = media.id;
  state.timeline.media.push_back(std::move(media));
  return state.timeline.media.back().id;
}

[[nodiscard]] Sequence ReadSequence(const xml::Node& node, ReadState& state, int nesting, bool root);

void ReadTrack(const xml::Node& track_node, model::TrackKind kind, int index, const time::FrameRate& fps, Sequence& sequence,
               const std::optional<json::Value>& blob, ReadState& state, int nesting) {
  Track track;
  track.kind = kind;
  const json::Value* meta = nullptr;
  if (blob.has_value()) {
    if (const auto* tracks = blob->Find("tracks")) {
      int seen = 0;
      for (const auto& candidate : tracks->items) {
        if (candidate.String("kind") == model::ToString(kind) && seen++ == index) meta = &candidate;
      }
    }
  }
  const auto kind_label = kind == model::TrackKind::Video ? std::string("V") : std::string("A");
  if (meta != nullptr) {
    track.id = meta->String("id");
    track.name = meta->String("name");
    track.order = meta->Integer("order");
    track.locked = meta->Bool("locked");
    track.muted = meta->Bool("muted");
    track.solo = meta->Bool("solo");
    track.gain_db = meta->Number("gainDb");
    track.pan = meta->Number("pan");
    track.channel_layout = meta->String("channelLayout");
    for (const auto& effect : meta->Require("effects").items) track.effects.push_back(EffectFromJson(effect));
  } else {
    track.id = "xml-" + kind_label + std::to_string(index + 1);
    track.name = kind_label + std::to_string(index + 1);
    track.order = index;
    track.muted = track_node.ChildText("enabled", "TRUE") == "FALSE";
    track.locked = track_node.ChildText("locked", "FALSE") == "TRUE";
  }
  const auto where = "track " + track.name;

  struct Raw final {
    std::string xml_id;
    std::int64_t start{0}, end{0};
    std::size_t item_index{0};
  };
  std::map<std::string, std::string> item_by_xml_id;
  std::size_t transition_index = 0;
  struct PendingTransition final {
    const xml::Node* node;
    std::size_t index;
  };
  std::vector<PendingTransition> transitions;

  for (const auto& child : track_node.children) {
    if (child.name == "transitionitem") {
      transitions.push_back({&child, transition_index++});
      continue;
    }
    if (child.name != "clipitem") continue;
    const auto xml_id = child.Attribute("id");
    const auto clip_where = where + " / " + child.ChildText("name", "clip");
    const auto clip_blob = BlobOf(child, state, clip_where);
    Item item;
    item.name = child.ChildText("name");
    item.enabled = child.ChildText("enabled", "TRUE") != "FALSE";
    const auto clip_rate = RateOfXml(child.Find("rate"), fps);

    // The speed filter; any other filter is something this reader does not have unless the Cutline
    // comment carries it.
    double speed_percent = 100.0;
    bool reverse = false;
    for (const auto* filter : child.FindAll("filter")) {
      const auto* effect = filter->Find("effect");
      if (effect == nullptr) continue;
      if (effect->ChildText("effectid") == "timeremap") {
        for (const auto* parameter : effect->FindAll("parameter")) {
          const auto id = parameter->ChildText("parameterid");
          if (id == "speed") speed_percent = std::atof(parameter->ChildText("value", "100").c_str());
          else if (id == "reverse") reverse = parameter->ChildText("value") == "TRUE";
        }
      } else if (!clip_blob.has_value()) {
        state.report.Warn(clip_where, "the filter '" + effect->ChildText("name", effect->ChildText("effectid")) + "' is not one Cutline has and was not imported");
      }
    }

    std::int64_t in = IntOf(child, "in", 0);
    std::int64_t out = IntOf(child, "out", 0);
    std::int64_t start = IntOf(child, "start", -1);
    std::int64_t end = IntOf(child, "end", -1);
    if (speed_percent <= 0.0) speed_percent = 100.0;
    bool exact = true;
    const auto rate_fraction = RateFromDouble(speed_percent / 100.0, exact);
    item.playback_rate = RationalTime(rate_fraction.numerator, rate_fraction.denominator);
    if (!exact) state.report.Warn(clip_where, "the speed " + std::to_string(speed_percent) + "% was approximated");
    item.reversed = reverse;
    // A start or end of -1 means the position is set by a transition: work it from the other end.
    const auto source_length = RationalTime::FromFrames(out - in, clip_rate);
    const auto timeline_length = source_length.Divide(item.playback_rate);
    if (start < 0 && end >= 0) start = end - timeline_length.ToFrames(fps, time::RoundingMode::Nearest);
    if (end < 0 && start >= 0) end = start + timeline_length.ToFrames(fps, time::RoundingMode::Nearest);
    if (start < 0 || end < 0) {
      state.report.Warn(clip_where, "its position is not given and could not be worked out; it was left out");
      continue;
    }
    item.timeline_start = RationalTime::FromFrames(start, fps);
    item.source_in = RationalTime::FromFrames(in, clip_rate);
    item.source_out = RationalTime::FromFrames(out, clip_rate);
    const auto listed = RationalTime::FromFrames(end - start, fps);
    if (listed.Compare(timeline_length) != 0 && speed_percent == 100.0) {
      // No speed filter but the lengths disagree: trust the timeline length and say so.
      item.playback_rate = source_length.Divide(listed);
      state.report.Note(clip_where, "its source length and its length on the timeline differ without a speed filter; the speed was worked out from them");
    }

    if (const auto* nested_node = child.Find("sequence")) {
      auto nested = ReadSequence(*nested_node, state, nesting + 1, false);
      item.source_kind = model::SourceKind::Sequence;
      item.source_id = nested.id;
      if (state.timeline.FindSequence(nested.id) == nullptr) state.timeline.sequences.push_back(std::move(nested));
    } else if (const auto* file = child.Find("file")) {
      item.source_kind = model::SourceKind::Media;
      item.source_id = MediaFor(*file, fps, state, clip_where);
    } else {
      state.report.Warn(clip_where, "has neither a file nor a sequence; it was left out");
      continue;
    }

    item.id = state.NextId("xml-clip-");
    if (clip_blob.has_value()) {
      item.id = clip_blob->String("id");
      if (!clip_blob->String("name").empty()) item.name = clip_blob->String("name");
      item.maintain_pitch = clip_blob->Bool("maintainPitch");
      item.linked_group = clip_blob->String("linkedGroup");
      for (const auto& effect : clip_blob->Require("effects").items) item.effects.push_back(EffectFromJson(effect));
      for (const auto& marker : clip_blob->Require("markers").items) item.markers.push_back(MarkerFromJson(marker));
    } else {
      for (const auto* marker : child.FindAll("marker")) {
        Marker m;
        m.id = state.NextId("xml-marker-");
        m.label = marker->ChildText("name");
        m.start = RationalTime::FromFrames(IntOf(*marker, "in", 0), fps);
        const auto marker_out = IntOf(*marker, "out", -1);
        m.end = marker_out < 0 ? m.start : RationalTime::FromFrames(marker_out, fps);
        item.markers.push_back(std::move(m));
      }
    }
    item_by_xml_id[xml_id] = item.id;
    track.items.push_back(std::move(item));
  }

  for (const auto& pending : transitions) {
    const auto& node = *pending.node;
    const auto where_t = where + " / transition";
    const auto effect_name = node.Find("effect") != nullptr ? node.Find("effect")->ChildText("name") : std::string{};
    const auto start = IntOf(node, "start", 0);
    const auto end = IntOf(node, "end", 0);
    const auto alignment = node.ChildText("alignment", "center");
    Transition transition;
    transition.id = state.NextId("xml-transition-");
    transition.kind = effect_name.find("+3dB") != std::string::npos ? "constant_power" : "cross_dissolve";
    if (effect_name.find("Dissolve") == std::string::npos && effect_name.find("Cross Fade") == std::string::npos) {
      state.report.Warn(where_t, "the transition '" + effect_name + "' is not one Cutline has; it was imported as a cross dissolve");
    }
    transition.start = RationalTime::FromFrames(start, fps);
    transition.duration = RationalTime::FromFrames(end - start, fps);
    transition.alignment = alignment == "start" ? model::TransitionAlignment::Start
                           : alignment == "end" ? model::TransitionAlignment::End
                                                : model::TransitionAlignment::Center;
    // The clips it joins: the one ending at the cut and the one starting there.
    std::int64_t cut = (start + end) / 2;
    if (alignment == "start" || alignment == "start-black") cut = start;
    if (alignment == "end" || alignment == "end-black") cut = end;
    const auto cut_time = RationalTime::FromFrames(cut, fps);
    for (const auto& item : track.items) {
      if (alignment != "start-black" && item.end().Compare(cut_time) == 0) transition.from_item = item.id;
      if (alignment != "end-black" && item.timeline_start.Compare(cut_time) == 0) transition.to_item = item.id;
    }
    if (alignment == "start-black") transition.from_item.reset();
    if (alignment == "end-black") transition.to_item.reset();
    if (!transition.from_item.has_value() && !transition.to_item.has_value()) {
      state.report.Warn(where_t, "it does not sit against any clip and was not imported");
      continue;
    }
    if (meta != nullptr && pending.index < meta->Require("transitions").items.size()) {
      const auto& saved = meta->Require("transitions").items[pending.index];
      transition.id = saved.String("id");
      transition.kind = saved.String("kind");
    }
    track.transitions.push_back(std::move(transition));
  }
  (void)item_by_xml_id;
  (void)nesting;
  sequence.tracks.push_back(std::move(track));
}

Sequence ReadSequence(const xml::Node& node, ReadState& state, int nesting, bool root) {
  Sequence sequence;
  const auto where = "sequence " + node.ChildText("name");
  if (nesting > 8) {
    state.report.Error(where, "nested sequences are too deep to import");
    return sequence;
  }
  const auto blob = BlobOf(node, state, where);
  auto& s = sequence.settings;
  s.name = node.ChildText("name", "Sequence");
  s.frame_rate = RateOfXml(node.Find("rate"), {25, 1});
  s.width = 1920;
  s.height = 1080;
  s.sample_rate = 48000;
  const auto* media = node.Find("media");
  if (media != nullptr) {
    if (const auto* video = media->Find("video")) {
      if (const auto* format = video->Find("format")) {
        if (const auto* sc = format->Find("samplecharacteristics")) {
          s.width = IntOf(*sc, "width", s.width);
          s.height = IntOf(*sc, "height", s.height);
          const auto dominance = sc->ChildText("fielddominance", "none");
          s.field_order = dominance == "upper" ? model::FieldOrder::UpperFirst : dominance == "lower" ? model::FieldOrder::LowerFirst : model::FieldOrder::Progressive;
        }
      }
    }
    if (const auto* audio = media->Find("audio")) {
      if (const auto* format = audio->Find("format")) {
        if (const auto* sc = format->Find("samplecharacteristics")) s.sample_rate = IntOf(*sc, "samplerate", s.sample_rate);
      }
    }
  }
  if (const auto* timecode = node.Find("timecode")) s.drop_frame = timecode->ChildText("displayformat") == "DF";
  if (blob.has_value()) {
    sequence.id = blob->String("id");
    s.channel_layout = blob->String("channelLayout");
    s.working_color_space = blob->String("workingColorSpace");
    s.display_color_space = blob->String("displayColorSpace");
    const auto& aspect = blob->Require("pixelAspect");
    s.pixel_aspect = {aspect.Integer("num"), aspect.Integer("den")};
    const auto version = blob->Integer("renderVersion");
    if (version > 0) s.render_version = version;
    if (root) {
      for (const auto& m : blob->Require("media").items) state.blob_media[m.String("fileId")] = m;
    }
    for (const auto& effect : blob->Require("effects").items) sequence.effects.push_back(EffectFromJson(effect));
  } else {
    sequence.id = state.NextId("xml-sequence-");
  }

  // Media definitions anywhere under this sequence are indexed first, so that a later reference
  // to a file defined earlier in the document finds it.
  if (root) IndexFiles(node, state);

  if (media != nullptr) {
    int video_index = 0, audio_index = 0;
    if (const auto* video = media->Find("video")) {
      for (const auto* track : video->FindAll("track")) ReadTrack(*track, model::TrackKind::Video, video_index++, s.frame_rate, sequence, blob, state, nesting);
    }
    if (const auto* audio = media->Find("audio")) {
      for (const auto* track : audio->FindAll("track")) ReadTrack(*track, model::TrackKind::Audio, audio_index++, s.frame_rate, sequence, blob, state, nesting);
    }
  }

  // Link groups: clips that refer to each other. A group named in the Cutline comment keeps its
  // name; others are named here.
  {
    std::map<std::string, std::string> parent;
    for (const auto* track_set : {media != nullptr ? media->Find("video") : nullptr, media != nullptr ? media->Find("audio") : nullptr}) {
      if (track_set == nullptr) continue;
      for (const auto* track : track_set->FindAll("track")) {
        for (const auto* clip : track->FindAll("clipitem")) {
          const auto id = clip->Attribute("id");
          for (const auto* link : clip->FindAll("link")) {
            const auto ref = link->ChildText("linkclipref");
            if (!ref.empty() && ref != id) {
              // union by smallest id
              auto a = id, b = ref;
              while (parent.count(a) != 0 && parent[a] != a) a = parent[a];
              while (parent.count(b) != 0 && parent[b] != b) b = parent[b];
              if (a != b) parent[std::max(a, b)] = std::min(a, b);
              parent.emplace(std::min(a, b), std::min(a, b));
            }
          }
        }
      }
    }
    if (!blob.has_value() && !parent.empty()) {
      // Names by root; applied to items by matching order: items were created in document order.
      std::size_t index = 0;
      std::map<std::string, std::string> names;
      std::vector<std::string> order;
      for (const auto* track_set : {media->Find("video"), media->Find("audio")}) {
        if (track_set == nullptr) continue;
        for (const auto* track : track_set->FindAll("track")) {
          for (const auto* clip : track->FindAll("clipitem")) order.push_back(clip->Attribute("id"));
        }
      }
      for (auto& track : sequence.tracks) {
        for (auto& item : track.items) {
          if (index >= order.size()) break;
          auto root_id = order[index++];
          while (parent.count(root_id) != 0 && parent[root_id] != root_id) root_id = parent[root_id];
          if (parent.count(root_id) == 0) continue;
          auto& name = names[root_id];
          if (name.empty()) name = state.NextId("xml-link-");
          item.linked_group = name;
        }
      }
    }
  }

  const auto fps = s.frame_rate;
  if (blob.has_value()) {
    const auto& saved = blob->Require("markers").items;
    std::size_t index = 0;
    for (const auto* marker : node.FindAll("marker")) {
      Marker m = index < saved.size() ? MarkerFromJson(saved[index]) : Marker{};
      if (index >= saved.size()) {
        m.id = state.NextId("xml-marker-");
        m.label = marker->ChildText("name");
        m.start = RationalTime::FromFrames(IntOf(*marker, "in", 0), fps);
        const auto marker_out = IntOf(*marker, "out", -1);
        m.end = marker_out < 0 ? m.start : RationalTime::FromFrames(marker_out, fps);
      }
      ++index;
      sequence.markers.push_back(std::move(m));
    }
  } else {
    for (const auto* marker : node.FindAll("marker")) {
      Marker m;
      m.id = state.NextId("xml-marker-");
      m.label = marker->ChildText("name");
      m.start = RationalTime::FromFrames(IntOf(*marker, "in", 0), fps);
      const auto marker_out = IntOf(*marker, "out", -1);
      m.end = marker_out < 0 ? m.start : RationalTime::FromFrames(marker_out, fps);
      sequence.markers.push_back(std::move(m));
    }
  }
  return sequence;
}

}  // namespace

std::string WriteFcpXml(const Timeline& timeline, Report& report) { return Writer(timeline, report).Write(); }

Timeline ReadFcpXml(const std::string& text, Report& report) {
  ReadState state{report, {}, {}, {}, {}, 0};
  xml::Node root;
  try {
    root = xml::Parse(text);
  } catch (const std::exception& error) {
    report.Error("file", error.what());
    return {};
  }
  if (root.name != "xmeml") {
    report.Error("file", "the top-level element is <" + root.name + ">, not <xmeml>");
    return {};
  }
  if (const auto version = root.Attribute("version"); !version.empty() && version != "4" && version != "5") {
    report.Warn("file", "this is xmeml version " + version + "; version 4 is the one this reader is written against");
  }
  // The first sequence, wherever it sits: directly, or in a project's children.
  const xml::Node* sequence = nullptr;
  std::vector<const xml::Node*> stack{&root};
  while (!stack.empty() && sequence == nullptr) {
    const auto* node = stack.back();
    stack.pop_back();
    if (node->name == "sequence") {
      sequence = node;
      break;
    }
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) stack.push_back(&*it);
  }
  if (sequence == nullptr) {
    report.Error("file", "there is no sequence in the file");
    return {};
  }
  auto first = ReadSequence(*sequence, state, 0, true);
  state.timeline.sequences.insert(state.timeline.sequences.begin(), std::move(first));
  return std::move(state.timeline);
}

}  // namespace cutline::interchange

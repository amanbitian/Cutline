#include "interchange/Edl.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace cutline::interchange {

using time::RationalTime;

namespace {

[[nodiscard]] bool OnFrame(const RationalTime& t, const time::FrameRate& fps) {
  const auto top = t.numerator() * fps.numerator;
  const auto bottom = t.denominator() * fps.denominator;
  return bottom != 0 && top % bottom == 0;
}

[[nodiscard]] std::string Pad(const std::string& text, std::size_t width) {
  return text.size() >= width ? text : text + std::string(width - text.size(), ' ');
}

[[nodiscard]] std::string Number3(int value) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%03d", value);
  return buffer;
}

[[nodiscard]] RationalTime Scale(const RationalTime& t, const RationalTime& factor) { return t.Multiply(factor); }

// The start of the hour a time falls in, by timecode: at 29.97 an hour of timecode is not
// 3600 seconds, so the hour is read from the timecode and turned back into a time.
[[nodiscard]] RationalTime HourStart(const RationalTime& t, const time::FrameRate& rate, bool drop) {
  const auto code = t.FormatTimecode(rate, drop, time::RoundingMode::Floor);
  if (code.size() < 2 || code[0] == '-') return RationalTime(0, 1);
  const auto hours = code.substr(0, code.find(':'));
  return RationalTime::ParseTimecode(hours + (drop ? ":00:00;00" : ":00:00:00"), rate, drop);
}

// -------------------------------------------------------------------- writing ----

struct Span final {
  RationalTime rec_in, rec_out, src_in, src_out;
};

struct Event final {
  const Item* item{};
  std::string channel;
  Span span;
  const Transition* dissolve{};  // into this event
  RationalTime from_source;      // the outgoing clip's source position at the dissolve's start
  const Item* from_item{};
  std::string reel;
  const Media* media{};
};

class Writer final {
 public:
  Writer(const Timeline& timeline, const EdlOptions& options, Report& report)
      : timeline_(timeline), options_(options), report_(report) {}

  [[nodiscard]] std::string Write() {
    if (timeline_.sequences.empty()) {
      report_.Error("timeline", "there is no sequence to write");
      return {};
    }
    const auto& root = timeline_.sequences.front();
    fps_ = root.settings.frame_rate;
    drop_ = root.settings.drop_frame;
    record_start_ = options_.record_start.value_or(RationalTime::ParseTimecode(drop_ ? "01:00:00;00" : "01:00:00:00", fps_, drop_));
    BuildEvents(root);

    std::ostringstream out;
    out << "TITLE: " << (options_.title.empty() ? root.settings.name : options_.title) << "\n";
    out << "FCM: " << (drop_ ? "DROP FRAME" : "NON-DROP FRAME") << "\n\n";
    for (const auto& [id, reel] : reels_) {
      const auto* media = timeline_.FindMedia(id);
      if (media == nullptr) continue;
      out << "* CUTLINE MEDIA "
          << json::Object()
                 .Add("reel", reel)
                 .Add("id", media->id)
                 .Add("name", media->name)
                 .Add("url", media->url)
                 .Add("fingerprint", media->fingerprint)
                 .AddRaw("duration", Exact(media->duration))
                 .AddRaw("start", Exact(media->start_timecode))
                 .Build()
          << "\n";
    }
    if (!reels_.empty()) out << "\n";

    int number = 0;
    for (const auto& event : events_) {
      ++number;
      const auto media_start = event.media != nullptr ? event.media->start_timecode : RationalTime(0, 1);
      if (event.dissolve != nullptr) {
        // The outgoing side, as a cut of no length at the point the dissolve begins.
        const auto from_tc = Tc(event.from_source.Add(media_start));
        out << Number3(number) << "  " << Pad(ReelOf(event.from_item), 8) << " " << Pad(event.channel, 5) << " C        " << from_tc << " "
            << from_tc << " " << Tc(Record(event.span.rec_in)) << " " << Tc(Record(event.span.rec_in)) << "\n";
        char frames[16];
        std::snprintf(frames, sizeof frames, "%03lld", static_cast<long long>(event.dissolve->duration.ToFrames(fps_, time::RoundingMode::Nearest)));
        if (!OnFrame(event.dissolve->duration, fps_)) report_.Warn("transition " + event.dissolve->id, "its length is not a whole number of frames and was rounded");
        out << Number3(number) << "  " << Pad(event.reel, 8) << " " << Pad(event.channel, 5) << " D    " << frames << "    ";
      } else {
        out << Number3(number) << "  " << Pad(event.reel, 8) << " " << Pad(event.channel, 5) << " C        ";
      }
      out << Tc(event.span.src_in.Add(media_start)) << " " << Tc(event.span.src_out.Add(media_start)) << " " << Tc(Record(event.span.rec_in))
          << " " << Tc(Record(event.span.rec_out)) << "\n";
      const auto& item = *event.item;
      if (item.playback_rate.Compare(RationalTime(1, 1)) != 0 || item.reversed) {
        const auto speed = static_cast<double>(item.playback_rate.numerator()) / static_cast<double>(item.playback_rate.denominator()) *
                           static_cast<double>(fps_.numerator) / static_cast<double>(fps_.denominator);
        char text[32];
        std::snprintf(text, sizeof text, "%05.1f", speed);
        out << "M2   " << Pad(event.reel, 8) << " " << (item.reversed ? "-" : "") << text << "            " << Tc(event.span.src_in.Add(media_start)) << "\n";
      }
      out << "* FROM CLIP NAME: " << (event.media != nullptr ? event.media->name : item.name) << "\n";
      if (event.media != nullptr) out << "* SOURCE FILE: " << event.media->url << "\n";
      out << "* CUTLINE CLIP "
          << json::Object()
                 .Add("id", item.id)
                 .Add("name", item.name)
                 .Add("linkedGroup", item.linked_group)
                 .Add("enabled", item.enabled)
                 .Add("maintainPitch", item.maintain_pitch)
                 .Build()
          << "\n";
    }
    // Locators.
    for (const auto& marker : root.markers) {
      out << "* LOC: " << Tc(Record(marker.start)) << " " << (marker.color.empty() ? std::string("RED") : Upper(marker.color)) << "  "
          << marker.label << "\n";
    }
    if (!root.effects.empty()) report_.Warn("sequence " + root.settings.name, "the sequence's effects have no place in an EDL and were not written");
    if (rounded_) report_.Warn("timeline", "some times are not on a frame boundary and were rounded to the nearest frame");
    return out.str();
  }

 private:
  [[nodiscard]] static std::string Exact(const RationalTime& t) {
    return json::Object().Add("n", t.numerator()).Add("d", t.denominator()).Build();
  }
  [[nodiscard]] static std::string Upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
  }

  [[nodiscard]] RationalTime Record(const RationalTime& t) const { return record_start_.Add(t); }

  [[nodiscard]] std::string Tc(const RationalTime& t) {
    if (!OnFrame(t, fps_)) rounded_ = true;
    return t.FormatTimecode(fps_, drop_, time::RoundingMode::Nearest);
  }

  [[nodiscard]] std::string ReelFor(const Media& media) {
    const auto found = reels_.find(media.id);
    if (found != reels_.end()) return found->second;
    std::string base;
    for (const char c : media.name.substr(0, media.name.find_last_of('.'))) {
      if (std::isalnum(static_cast<unsigned char>(c))) base.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (base.empty()) base = "AX";
    base = base.substr(0, 8);
    auto reel = base;
    for (int suffix = 2; used_reels_.count(reel) != 0; ++suffix) {
      const auto digits = std::to_string(suffix);
      reel = base.substr(0, 8 - digits.size()) + digits;
    }
    used_reels_.insert(reel);
    reels_.emplace(media.id, reel);
    return reel;
  }

  [[nodiscard]] std::string ReelOf(const Item* item) {
    if (item == nullptr) return "AX";
    const auto* media = timeline_.FindMedia(item->source_id);
    return media != nullptr ? ReelFor(*media) : std::string("AX");
  }

  void BuildEvents(const Sequence& root) {
    // One video channel and up to four audio ones.
    const Track* video = nullptr;
    std::vector<const Track*> audio;
    for (const auto& track : root.tracks) {
      if (track.kind == model::TrackKind::Video) {
        if (video == nullptr || track.order < video->order) {
          if (video != nullptr) report_.Warn("track " + video->name, "an EDL has one video channel; this track was not written");
          video = &track;
        } else {
          report_.Warn("track " + track.name, "an EDL has one video channel; this track was not written");
        }
      } else {
        audio.push_back(&track);
      }
    }
    std::sort(audio.begin(), audio.end(), [](const Track* a, const Track* b) { return a->order < b->order; });
    if (audio.size() > 4) {
      for (std::size_t i = 4; i < audio.size(); ++i) report_.Warn("track " + audio[i]->name, "an EDL has four audio channels; this track was not written");
      audio.resize(4);
    }
    if (video != nullptr) AddTrack(*video, "V");
    for (std::size_t i = 0; i < audio.size(); ++i) AddTrack(*audio[i], i == 0 ? "A" : "A" + std::to_string(i + 1));
    std::stable_sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
      const auto order = a.span.rec_in.Compare(b.span.rec_in);
      if (order != 0) return order < 0;
      return a.channel == "V" && b.channel != "V";
    });
  }

  void AddTrack(const Track& track, const std::string& channel) {
    std::map<std::string, Span> spans;
    for (const auto& item : track.items) {
      if (item.source_kind != model::SourceKind::Media) {
        report_.Warn("track " + track.name + " / clip " + item.name, "a nested sequence cannot be written to an EDL and was left out");
        continue;
      }
      spans[item.id] = {item.timeline_start, item.end(), item.source_in, item.source_out};
      if (!item.effects.empty()) {
        report_.Warn("track " + track.name + " / clip " + item.name, "its effects have no place in an EDL and were not written");
      }
    }
    std::map<std::string, std::pair<const Transition*, RationalTime>> dissolves;  // incoming id -> (transition, outgoing source position)
    for (const auto& transition : track.transitions) {
      const auto where = "track " + track.name + " / transition " + transition.id;
      if (!transition.from_item.has_value() || !transition.to_item.has_value()) {
        report_.Warn(where, "a fade from or to black has no equivalent here and was written as a cut");
        continue;
      }
      const auto from = std::find_if(track.items.begin(), track.items.end(), [&](const Item& i) { return i.id == *transition.from_item; });
      const auto to = std::find_if(track.items.begin(), track.items.end(), [&](const Item& i) { return i.id == *transition.to_item; });
      if (from == track.items.end() || to == track.items.end() || spans.count(from->id) == 0 || spans.count(to->id) == 0) continue;
      if (transition.kind != "cross_dissolve" && transition.kind != "dissolve" && transition.kind != "constant_power") {
        report_.Warn(where, "the transition '" + transition.kind + "' is not a dissolve; it was written as a cut");
        continue;
      }
      const auto start = transition.start;
      const auto cut = to->timeline_start;
      if (start.Compare(cut) > 0) {
        report_.Warn(where, "it begins after the cut, which an EDL cannot say; it was written as a cut");
        continue;
      }
      auto& incoming = spans[to->id];
      auto& outgoing = spans[from->id];
      const auto lead = cut.Subtract(start);        // timeline time the dissolve begins before the cut
      if (lead.Compare(RationalTime(0, 1)) > 0) {
        report_.Note(where, "an EDL dissolve begins at its incoming clip, so the transition was written starting " +
                                std::to_string(lead.numerator()) + "/" + std::to_string(lead.denominator()) +
                                " s earlier and the two clips' edges moved to match; the picture is unchanged");
        // The incoming clip starts earlier, reading its media from that much sooner.
        incoming.rec_in = start;
        const auto media_lead = Scale(lead, to->playback_rate);
        if (to->reversed) incoming.src_out = to->source_out.Add(media_lead);
        else incoming.src_in = to->source_in.Subtract(media_lead);
        // The outgoing clip stops where the dissolve begins.
        const auto kept = start.Subtract(from->timeline_start);
        const auto kept_media = Scale(kept, from->playback_rate);
        outgoing.rec_out = start;
        if (from->reversed) outgoing.src_in = from->source_out.Subtract(kept_media);
        else outgoing.src_out = from->source_in.Add(kept_media);
      }
      const auto outgoing_position = from->reversed ? outgoing.src_in : outgoing.src_out;
      dissolves[to->id] = {&transition, outgoing_position};
    }
    for (const auto& item : track.items) {
      if (item.source_kind != model::SourceKind::Media) continue;
      Event event;
      event.item = &item;
      event.channel = channel;
      event.span = spans[item.id];
      event.media = timeline_.FindMedia(item.source_id);
      if (event.media == nullptr) {
        report_.Error("track " + track.name + " / clip " + item.name, "its media " + item.source_id + " is not in the timeline");
        continue;
      }
      event.reel = ReelFor(*event.media);
      const auto found = dissolves.find(item.id);
      if (found != dissolves.end()) {
        event.dissolve = found->second.first;
        event.from_source = found->second.second;
        for (const auto& candidate : track.items) {
          if (event.dissolve->from_item.has_value() && candidate.id == *event.dissolve->from_item) event.from_item = &candidate;
        }
      }
      events_.push_back(std::move(event));
    }
  }

  const Timeline& timeline_;
  const EdlOptions& options_;
  Report& report_;
  time::FrameRate fps_{25, 1};
  bool drop_{false};
  bool rounded_{false};
  RationalTime record_start_{3600, 1};
  std::vector<Event> events_;
  std::map<std::string, std::string> reels_;  // media id -> reel
  std::set<std::string> used_reels_;
};

// -------------------------------------------------------------------- reading ----

struct RawEvent final {
  int number{0};
  std::string reel, channel, transition;
  int dissolve_frames{0};
  RationalTime src_in, src_out, rec_in, rec_out;
  double speed{0.0};
  bool has_speed{false};
  std::string clip_name, source_file;
  std::string cutline_clip;  // the JSON of a CUTLINE CLIP comment
  int line{0};
};

[[nodiscard]] std::vector<std::string> Words(const std::string& line) {
  std::vector<std::string> words;
  std::istringstream in(line);
  std::string word;
  while (in >> word) words.push_back(word);
  return words;
}

[[nodiscard]] bool IsTimecode(const std::string& word) {
  // hh:mm:ss:ff or hh:mm:ss;ff
  if (word.size() < 11) return false;
  int colons = 0;
  for (const char c : word) {
    if (c == ':' || c == ';') ++colons;
    else if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return colons == 3;
}

[[nodiscard]] bool IsNumber(const std::string& word) {
  return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

[[nodiscard]] std::string Trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  std::size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  return text.substr(first);
}

}  // namespace

std::string WriteEdl(const Timeline& timeline, const EdlOptions& options, Report& report) {
  return Writer(timeline, options, report).Write();
}

Timeline ReadEdl(const std::string& text, time::FrameRate rate, bool drop_frame, Report& report) {
  Timeline timeline;
  std::string title;
  std::vector<RawEvent> events;
  std::map<std::string, json::Value> media_extensions;  // by reel
  struct Locator final {
    RationalTime at;
    std::string color, label;
  };
  std::vector<Locator> locators;
  bool timecode_problem = false;

  const auto parse_tc = [&](const std::string& word, int line) -> RationalTime {
    // The timecode's own separator says whether it is drop-frame; the header's FCM can disagree.
    const bool semicolon = word.find(';') != std::string::npos;
    try {
      return RationalTime::ParseTimecode(word, rate, semicolon);
    } catch (const std::exception& error) {
      if (!timecode_problem) report.Error("line " + std::to_string(line), "the timecode " + word + " is not valid at this frame rate: " + error.what());
      timecode_problem = true;
      return RationalTime(0, 1);
    }
  };

  std::istringstream input(text);
  std::string line;
  int line_number = 0;
  RawEvent* current = nullptr;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto trimmed = Trim(line);
    if (trimmed.empty()) continue;
    if (trimmed.rfind("TITLE:", 0) == 0) {
      title = Trim(trimmed.substr(6));
      continue;
    }
    if (trimmed.rfind("FCM:", 0) == 0) {
      const bool drop_header = trimmed.find("NON-DROP") == std::string::npos && trimmed.find("DROP") != std::string::npos;
      if (drop_header != drop_frame) report.Warn("line " + std::to_string(line_number), "the list says " + Trim(trimmed.substr(4)) + ", which disagrees with the frame mode it is being read as");
      continue;
    }
    if (trimmed[0] == '*') {
      const auto body = Trim(trimmed.substr(1));
      if (body.rfind("CUTLINE MEDIA ", 0) == 0) {
        try {
          const auto value = json::Parse(body.substr(14));
          media_extensions[value.String("reel")] = value;
        } catch (const std::exception& error) {
          report.Warn("line " + std::to_string(line_number), std::string("a CUTLINE MEDIA comment could not be read: ") + error.what());
        }
      } else if (body.rfind("CUTLINE CLIP ", 0) == 0) {
        if (current != nullptr) current->cutline_clip = body.substr(13);
      } else if (body.rfind("FROM CLIP NAME:", 0) == 0) {
        if (current != nullptr) current->clip_name = Trim(body.substr(15));
      } else if (body.rfind("SOURCE FILE:", 0) == 0) {
        if (current != nullptr) current->source_file = Trim(body.substr(12));
      } else if (body.rfind("LOC:", 0) == 0) {
        const auto words = Words(body.substr(4));
        if (!words.empty() && IsTimecode(words[0])) {
          Locator locator;
          locator.at = parse_tc(words[0], line_number);
          if (words.size() > 1) locator.color = words[1];
          for (std::size_t i = 2; i < words.size(); ++i) locator.label += (i > 2 ? " " : "") + words[i];
          locators.push_back(std::move(locator));
        }
      }
      continue;
    }
    const auto words = Words(trimmed);
    if (words.empty()) continue;
    if (words[0] == "M2" && current != nullptr && words.size() >= 3) {
      try {
        current->speed = std::stod(words[2]);
        current->has_speed = true;
      } catch (const std::exception&) {
        report.Warn("line " + std::to_string(line_number), "the speed '" + words[2] + "' could not be read");
      }
      continue;
    }
    if (!IsNumber(words[0]) || words.size() < 8) {
      // FCM, GPI, SPLIT, AUD ... lines this reader does not use.
      if (IsNumber(words[0])) report.Warn("line " + std::to_string(line_number), "an event line with too few fields was skipped");
      else report.Note("line " + std::to_string(line_number), "the line '" + words[0] + "' is not used by this reader");
      continue;
    }
    RawEvent event;
    event.number = std::stoi(words[0]);
    event.reel = words[1];
    event.channel = words[2];
    event.transition = words[3];
    event.line = line_number;
    std::size_t at = 4;
    if (event.transition[0] == 'D' || event.transition[0] == 'W' || event.transition[0] == 'K') {
      if (at < words.size() && IsNumber(words[at])) event.dissolve_frames = std::stoi(words[at++]);
    }
    if (at + 4 > words.size() || !IsTimecode(words[at]) || !IsTimecode(words[at + 1]) || !IsTimecode(words[at + 2]) ||
        !IsTimecode(words[at + 3])) {
      report.Warn("line " + std::to_string(line_number), "the event does not have four timecodes and was skipped");
      continue;
    }
    event.src_in = parse_tc(words[at], line_number);
    event.src_out = parse_tc(words[at + 1], line_number);
    event.rec_in = parse_tc(words[at + 2], line_number);
    event.rec_out = parse_tc(words[at + 3], line_number);
    events.push_back(std::move(event));
    current = &events.back();
  }
  if (timecode_problem) return timeline;

  // Where the sequence starts: the hour of the earliest record time.
  RationalTime origin(0, 1);
  if (!events.empty()) {
    auto earliest = events.front().rec_in;
    for (const auto& event : events) {
      if (event.rec_in.Compare(earliest) < 0) earliest = event.rec_in;
    }
    origin = HourStart(earliest, rate, drop_frame);
  }

  Sequence sequence;
  sequence.id = "edl-sequence";
  sequence.settings.name = title.empty() ? "EDL" : title;
  sequence.settings.frame_rate = rate;
  sequence.settings.width = 1920;
  sequence.settings.height = 1080;
  sequence.settings.sample_rate = 48000;
  sequence.settings.drop_frame = drop_frame;
  report.Note("timeline", "an EDL has no picture size or sample rate; 1920x1080 and 48 kHz were assumed");

  // Media: from the CUTLINE comments where there are some, otherwise from the reel and the
  // furthest point used (an EDL does not say how long a source is).
  std::map<std::string, std::string> media_by_reel;
  const auto media_for = [&](const std::string& reel, const RawEvent& event) -> const Media* {
    const auto found = media_by_reel.find(reel);
    if (found != media_by_reel.end()) return timeline.FindMedia(found->second);
    Media media;
    const auto extension = media_extensions.find(reel);
    if (extension != media_extensions.end()) {
      const auto& value = extension->second;
      media.id = value.String("id");
      media.name = value.String("name");
      media.url = value.String("url");
      media.fingerprint = value.String("fingerprint");
      media.duration = RationalTime(value.Require("duration").Integer("n"), value.Require("duration").Integer("d"));
      media.start_timecode = RationalTime(value.Require("start").Integer("n"), value.Require("start").Integer("d"));
    } else {
      media.id = "edl-media-" + reel;
      media.name = event.clip_name.empty() ? reel : event.clip_name;
      media.url = event.source_file.empty() ? "edl:reel:" + reel : event.source_file;
      // Source timecodes carry the camera's clock; the hour they start in is its origin.
      RationalTime earliest = event.src_in;
      RationalTime furthest = event.src_out;
      for (const auto& other : events) {
        if (other.reel != reel) continue;
        if (other.src_in.Compare(earliest) < 0) earliest = other.src_in;
        const auto end = other.src_out.Compare(other.src_in) > 0 ? other.src_out : other.src_in;
        if (end.Compare(furthest) > 0) furthest = end;
        if (other.dissolve_frames > 0) furthest = furthest.Add(RationalTime::FromFrames(other.dissolve_frames, rate).Multiply(RationalTime(4, 1)));
      }
      media.start_timecode = HourStart(earliest, rate, drop_frame);
      media.duration = furthest.Subtract(media.start_timecode);
      report.Note("reel " + reel, "an EDL does not say how long its source is; it was taken as the furthest point used");
    }
    media_by_reel.emplace(reel, media.id);
    timeline.media.push_back(std::move(media));
    return &timeline.media.back();
  };

  std::map<std::string, std::size_t> track_slots;  // channel -> index into sequence.tracks
  const auto track_for = [&](const std::string& channel, model::TrackKind kind, std::int64_t order) -> Track& {
    const auto found = track_slots.find(channel);
    if (found != track_slots.end()) return sequence.tracks[found->second];
    Track track;
    track.id = "edl-" + channel;
    track.name = channel;
    track.kind = kind;
    track.order = order;
    track_slots.emplace(channel, sequence.tracks.size());
    sequence.tracks.push_back(std::move(track));
    return sequence.tracks.back();
  };

  int clip_counter = 0;
  for (std::size_t index = 0; index < events.size(); ++index) {
    const auto& event = events[index];
    const auto where = "event " + Number3(event.number);
    const bool zero_length = event.rec_out.Compare(event.rec_in) == 0;
    if (zero_length) continue;  // the outgoing side of a dissolve: its clip is the previous event
    if (event.transition[0] == 'W') {
      report.Warn(where, "a wipe is not supported; it was imported as a cut");
    } else if (event.transition[0] == 'K') {
      report.Warn(where, "a key is not supported; it was imported as a cut");
    }

    std::vector<std::pair<std::string, model::TrackKind>> channels;
    const auto& c = event.channel;
    if (c == "V" || c == "B" || c == "V1") channels.emplace_back("V", model::TrackKind::Video);
    else if (c == "A" || c == "A1") channels.emplace_back("A", model::TrackKind::Audio);
    else if (c == "AA") {
      channels.emplace_back("A", model::TrackKind::Audio);
      channels.emplace_back("A2", model::TrackKind::Audio);
    } else if (c == "AA/V") {
      channels.emplace_back("V", model::TrackKind::Video);
      channels.emplace_back("A", model::TrackKind::Audio);
      channels.emplace_back("A2", model::TrackKind::Audio);
    } else if (c.size() >= 2 && c[0] == 'A' && IsNumber(c.substr(1))) channels.emplace_back(c, model::TrackKind::Audio);
    else {
      report.Warn(where, "the channel '" + c + "' is not one this reader knows; the event was skipped");
      continue;
    }

    const auto* media = media_for(event.reel, event);
    if (media == nullptr) continue;
    for (const auto& [channel, kind] : channels) {
      std::int64_t order = 0;
      if (kind == model::TrackKind::Audio && channel.size() > 1) order = std::stoi(channel.substr(1)) - 1;
      auto& track = track_for(channel, kind, order);
      Item item;
      item.id = "edl-" + Number3(event.number) + "-" + channel;
      item.name = event.clip_name.empty() ? media->name : event.clip_name;
      item.source_kind = model::SourceKind::Media;
      item.source_id = media->id;
      item.timeline_start = event.rec_in.Subtract(origin);
      const auto record_length = event.rec_out.Subtract(event.rec_in);
      item.source_in = event.src_in.Subtract(media->start_timecode);
      item.source_out = event.src_out.Subtract(media->start_timecode);
      if (item.source_out.Compare(item.source_in) <= 0) {
        report.Warn(where, "its source range is empty or backwards; it was skipped");
        continue;
      }
      item.playback_rate = item.source_out.Subtract(item.source_in).Divide(record_length);
      if (event.has_speed && event.speed < 0) item.reversed = true;
      if (event.has_speed) {
        const auto stated = std::abs(event.speed) / (static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator));
        const auto computed = static_cast<double>(item.playback_rate.numerator()) / static_cast<double>(item.playback_rate.denominator());
        if (std::abs(stated - computed) > 1e-3 * std::max(1.0, computed)) {
          report.Warn(where, "its M2 speed disagrees with its source and record lengths; the lengths were used");
        }
      }
      if (!event.cutline_clip.empty()) {
        try {
          const auto value = json::Parse(event.cutline_clip);
          if (channels.size() == 1) item.id = value.String("id");
          item.name = value.String("name");
          item.linked_group = value.String("linkedGroup");
          item.enabled = value.Bool("enabled");
          item.maintain_pitch = value.Bool("maintainPitch");
        } catch (const std::exception&) {
          report.Warn(where, "its CUTLINE CLIP comment could not be read and was ignored");
        }
      }
      ++clip_counter;

      if (event.transition[0] == 'D' && event.dissolve_frames > 0) {
        Transition transition;
        transition.id = "edl-transition-" + Number3(event.number) + "-" + channel;
        transition.kind = "cross_dissolve";
        transition.alignment = model::TransitionAlignment::Start;
        transition.start = item.timeline_start;
        transition.duration = RationalTime::FromFrames(event.dissolve_frames, rate);
        transition.to_item = item.id;
        // The clip it dissolves from is the one on this channel that ends where this begins.
        for (const auto& other : track.items) {
          if (other.end().Compare(item.timeline_start) == 0) transition.from_item = other.id;
        }
        if (!transition.from_item.has_value()) {
          report.Warn(where, "a dissolve with no clip before it was imported as a fade in");
        }
        track.transitions.push_back(std::move(transition));
      }
      track.items.push_back(std::move(item));
    }
  }
  (void)clip_counter;

  int marker_counter = 0;
  for (const auto& locator : locators) {
    Marker marker;
    marker.id = "edl-marker-" + std::to_string(++marker_counter);
    marker.start = locator.at.Subtract(origin);
    marker.end = marker.start;
    marker.label = locator.label;
    marker.color = locator.color;
    for (auto& ch : marker.color) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    sequence.markers.push_back(std::move(marker));
  }
  // Audio tracks are made in the order channels appeared; fix their order from the channel number.
  std::stable_sort(sequence.tracks.begin(), sequence.tracks.end(), [](const Track& a, const Track& b) {
    if (a.kind != b.kind) return a.kind == model::TrackKind::Video;
    return a.order < b.order;
  });
  timeline.sequences.push_back(std::move(sequence));
  return timeline;
}

}  // namespace cutline::interchange

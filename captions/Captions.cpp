#include "captions/Captions.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cutline::captions {

using time::RationalTime;

// ---------------------------------------------------------------------- style ----

namespace {

std::string Serialize(const json::Value& value);

std::string SerializeObject(const std::vector<std::pair<std::string, json::Value>>& members) {
  std::string out = "{";
  bool first = true;
  for (const auto& [key, member] : members) {
    if (!first) out += ",";
    first = false;
    out += "\"" + json::Escape(key) + "\":" + Serialize(member);
  }
  return out + "}";
}

std::string Serialize(const json::Value& value) {
  switch (value.kind) {
    case json::Value::Kind::Null: return "null";
    case json::Value::Kind::Bool: return value.boolean ? "true" : "false";
    case json::Value::Kind::Number: return value.number_text.empty() ? json::Number(value.number) : value.number_text;
    case json::Value::Kind::String: return "\"" + json::Escape(value.text) + "\"";
    case json::Value::Kind::Array: {
      std::string out = "[";
      for (std::size_t i = 0; i < value.items.size(); ++i) out += (i ? "," : "") + Serialize(value.items[i]);
      return out + "]";
    }
    case json::Value::Kind::Object: return SerializeObject(value.members);
  }
  return "null";
}

void ReadColour(const json::Value& object, const char* key, double (&target)[4]) {
  const auto* value = object.Find(key);
  if (value == nullptr || !value->is_array() || value->items.size() < 3) return;
  for (std::size_t i = 0; i < 4; ++i) {
    if (i < value->items.size() && value->items[i].is_number()) target[i] = std::clamp(value->items[i].number, 0.0, 1.0);
    else if (i == 3) target[i] = 1.0;
  }
}

}  // namespace

Style ParseStyle(const std::string& text) {
  Style style;
  if (text.empty()) return style;
  json::Value object;
  try {
    object = json::Parse(text);
  } catch (const std::exception&) {
    return style;  // a style that cannot be read is the default one, not a failure to show the caption
  }
  if (!object.is_object()) return style;
  const auto string_of = [&](const char* key, std::string& target) {
    if (const auto* v = object.Find(key); v != nullptr && v->is_string()) target = v->text;
  };
  const auto number_of = [&](const char* key, double& target, double low, double high) {
    if (const auto* v = object.Find(key); v != nullptr && v->is_number()) target = std::clamp(v->number, low, high);
  };
  const auto bool_of = [&](const char* key, bool& target) {
    if (const auto* v = object.Find(key); v != nullptr && v->kind == json::Value::Kind::Bool) target = v->boolean;
  };
  string_of("family", style.family);
  number_of("size", style.size, 0.005, 0.5);
  bool_of("bold", style.bold);
  bool_of("italic", style.italic);
  ReadColour(object, "color", style.color);
  ReadColour(object, "background", style.background);
  ReadColour(object, "outline", style.outline);
  number_of("outlineWidth", style.outline_width, 0.0, 0.05);
  string_of("align", style.align);
  string_of("position", style.position);
  number_of("margin", style.margin, 0.0, 0.5);
  number_of("maxWidth", style.max_width, 0.1, 1.0);
  number_of("padding", style.padding, 0.0, 0.2);
  number_of("letterSpacing", style.letter_spacing, -0.05, 0.2);
  if (style.align != "left" && style.align != "center" && style.align != "right") style.align = "center";
  if (style.position != "top" && style.position != "middle" && style.position != "bottom") style.position = "bottom";
  return style;
}

std::string StyleToJson(const Style& s) {
  const auto colour = [](const double (&c)[4]) { return json::Array({json::Number(c[0]), json::Number(c[1]), json::Number(c[2]), json::Number(c[3])}); };
  return json::Object()
      .Add("family", s.family)
      .Add("size", s.size)
      .Add("bold", s.bold)
      .Add("italic", s.italic)
      .AddRaw("color", colour(s.color))
      .AddRaw("background", colour(s.background))
      .AddRaw("outline", colour(s.outline))
      .Add("outlineWidth", s.outline_width)
      .Add("align", s.align)
      .Add("position", s.position)
      .Add("margin", s.margin)
      .Add("maxWidth", s.max_width)
      .Add("padding", s.padding)
      .Add("letterSpacing", s.letter_spacing)
      .Build();
}

std::string MergeStyles(const std::string& base_json, const std::string& override_json) {
  const auto parse = [](const std::string& text) {
    try {
      auto value = json::Parse(text.empty() ? "{}" : text);
      return value.is_object() ? value : json::Value{};
    } catch (const std::exception&) {
      return json::Value{};
    }
  };
  auto merged = parse(base_json);
  if (!merged.is_object()) merged.kind = json::Value::Kind::Object;
  const auto over = parse(override_json);
  for (const auto& [key, value] : over.members) {
    const auto found = std::find_if(merged.members.begin(), merged.members.end(), [&](const auto& m) { return m.first == key; });
    if (found != merged.members.end()) found->second = value;
    else merged.members.emplace_back(key, value);
  }
  return SerializeObject(merged.members);
}

// ------------------------------------------------------------------- parsing ----

namespace {

std::vector<std::string> SplitLines(std::string text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  std::vector<std::string> lines;
  std::string current;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\r') {
      lines.push_back(current);
      current.clear();
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  lines.push_back(current);
  return lines;
}

std::string Trim(const std::string& s) {
  std::size_t first = 0, last = s.size();
  while (first < last && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
  return s.substr(first, last - first);
}

// HH:MM:SS,mmm  HH:MM:SS.mmm  MM:SS.mmm; the fraction may be one to three digits.
bool ParseTimestamp(const std::string& raw, RationalTime& out) {
  const auto text = Trim(raw);
  std::int64_t fields[3] = {0, 0, 0};
  int count = 0;
  std::size_t i = 0;
  std::int64_t current = 0;
  bool have_digits = false;
  std::int64_t milliseconds = 0;
  int fraction_digits = 0;
  bool in_fraction = false;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      if (in_fraction) {
        if (fraction_digits < 3) {
          milliseconds = milliseconds * 10 + (c - '0');
          ++fraction_digits;
        } else {
          return false;
        }
      } else {
        current = current * 10 + (c - '0');
        have_digits = true;
        if (current > 100000000) return false;
      }
    } else if (c == ':' && !in_fraction) {
      if (!have_digits || count >= 2) return false;
      fields[count++] = current;
      current = 0;
      have_digits = false;
    } else if ((c == ',' || c == '.') && !in_fraction) {
      if (!have_digits) return false;
      fields[count++] = current;
      in_fraction = true;
      current = 0;
      have_digits = false;
    } else {
      return false;
    }
  }
  if (!in_fraction) {
    if (!have_digits) return false;
    fields[count++] = current;
  } else if (fraction_digits == 0) {
    return false;
  }
  while (fraction_digits < 3 && in_fraction) {
    milliseconds *= 10;
    ++fraction_digits;
  }
  // count is the number of whole fields: HH:MM:SS (3) or MM:SS (2).
  std::int64_t hours = 0, minutes = 0, seconds = 0;
  if (count == 3) {
    hours = fields[0];
    minutes = fields[1];
    seconds = fields[2];
  } else if (count == 2) {
    minutes = fields[0];
    seconds = fields[1];
  } else {
    return false;
  }
  if (minutes >= 60 || seconds >= 60) return false;
  out = RationalTime(((hours * 60 + minutes) * 60 + seconds) * 1000 + milliseconds, 1000);
  return true;
}

bool SplitTiming(const std::string& line, std::string& start, std::string& end, std::string& rest) {
  const auto arrow = line.find("-->");
  if (arrow == std::string::npos) return false;
  start = line.substr(0, arrow);
  auto after = Trim(line.substr(arrow + 3));
  const auto space = after.find_first_of(" \t");
  end = space == std::string::npos ? after : after.substr(0, space);
  rest = space == std::string::npos ? std::string{} : Trim(after.substr(space));
  return true;
}

std::string StyleForAn(int an) {
  const char* position = an >= 7 ? "top" : an >= 4 ? "middle" : "bottom";
  const char* align = (an % 3 == 1) ? "left" : (an % 3 == 0) ? "right" : "center";
  return std::string("{\"align\":\"") + align + "\",\"position\":\"" + position + "\"}";
}

// Strips {...} override blocks (returning the \an value if there was one) and, optionally, tags.
std::string CleanText(const std::string& raw, int& an, bool& stripped_markup) {
  std::string out;
  an = 0;
  stripped_markup = false;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '{') {
      const auto close = raw.find('}', i);
      if (close != std::string::npos && close > i + 1 && raw[i + 1] == '\\') {
        const auto block = raw.substr(i, close - i + 1);
        const auto pos = block.find("\\an");
        if (pos != std::string::npos && pos + 3 < block.size() && std::isdigit(static_cast<unsigned char>(block[pos + 3]))) an = block[pos + 3] - '0';
        stripped_markup = true;
        i = close;
        continue;
      }
    }
    if (raw[i] == '<') {
      const auto close = raw.find('>', i);
      if (close != std::string::npos && close > i + 1 &&
          (std::isalpha(static_cast<unsigned char>(raw[i + 1])) || raw[i + 1] == '/')) {
        stripped_markup = true;
        i = close;
        continue;
      }
    }
    out.push_back(raw[i]);
  }
  return out;
}

std::string DecodeEntities(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      const auto semi = s.find(';', i);
      if (semi != std::string::npos && semi - i <= 6) {
        const auto name = s.substr(i + 1, semi - i - 1);
        if (name == "amp") { out.push_back('&'); i = semi; continue; }
        if (name == "lt") { out.push_back('<'); i = semi; continue; }
        if (name == "gt") { out.push_back('>'); i = semi; continue; }
        if (name == "nbsp") { out.push_back(' '); i = semi; continue; }
        if (name == "lrm" || name == "rlm") { i = semi; continue; }
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

struct Block final {
  int first_line{0};  // 1-based
  std::vector<std::string> lines;
};

std::vector<Block> Blocks(const std::vector<std::string>& lines, std::size_t from) {
  std::vector<Block> blocks;
  Block current;
  for (std::size_t i = from; i < lines.size(); ++i) {
    if (Trim(lines[i]).empty()) {
      if (!current.lines.empty()) blocks.push_back(std::move(current));
      current = {};
      continue;
    }
    if (current.lines.empty()) current.first_line = static_cast<int>(i) + 1;
    current.lines.push_back(lines[i]);
  }
  if (!current.lines.empty()) blocks.push_back(std::move(current));
  return blocks;
}

// A cue block: optional identifier, a timing line, then text.
void ReadCue(const Block& block, bool vtt, int& counter, ParseResult& result) {
  std::size_t timing = block.lines.size();
  for (std::size_t i = 0; i < block.lines.size(); ++i) {
    if (block.lines[i].find("-->") != std::string::npos) {
      timing = i;
      break;
    }
  }
  const auto line_number = block.first_line + static_cast<int>(timing < block.lines.size() ? timing : 0);
  if (timing == block.lines.size()) {
    result.issues.push_back({block.first_line, "a block with no timing line was skipped", true});
    return;
  }
  std::string start_text, end_text, settings;
  SplitTiming(block.lines[timing], start_text, end_text, settings);
  Cue cue;
  if (!ParseTimestamp(start_text, cue.start) || !ParseTimestamp(end_text, cue.end)) {
    result.issues.push_back({line_number, "the times '" + Trim(start_text) + "' and '" + Trim(end_text) + "' could not be read; the cue was skipped", true});
    return;
  }
  if (cue.end.Compare(cue.start) <= 0) {
    result.issues.push_back({line_number, "the cue ends at or before its start; it was skipped", true});
    return;
  }
  std::string text;
  for (std::size_t i = timing + 1; i < block.lines.size(); ++i) {
    if (!text.empty()) text += "\n";
    text += Trim(block.lines[i]);
  }
  int an = 0;
  bool stripped = false;
  text = CleanText(text, an, stripped);
  if (vtt) text = DecodeEntities(text);
  if (stripped) result.issues.push_back({line_number, "inline styling was removed from the cue's text", false});
  if (Trim(text).empty()) {
    result.issues.push_back({line_number, "the cue has no text; it was skipped", true});
    return;
  }
  cue.text = text;
  if (an >= 1 && an <= 9) cue.style_json = StyleForAn(an);
  if (vtt && !settings.empty()) {
    // Cue settings: align:start|end|left|right|center, line:NN%.
    std::string align, position;
    std::istringstream words(settings);
    std::string word;
    while (words >> word) {
      if (word.rfind("align:", 0) == 0) {
        const auto v = word.substr(6);
        align = (v == "start" || v == "left") ? "left" : (v == "end" || v == "right") ? "right" : "center";
      } else if (word.rfind("line:", 0) == 0) {
        const auto v = word.substr(5);
        if (!v.empty() && v.back() == '%') {
          const auto percent = std::atof(v.c_str());
          position = percent <= 25.0 ? "top" : percent <= 65.0 ? "middle" : "bottom";
        }
      }
    }
    if (!align.empty() || !position.empty()) {
      std::string json = "{";
      if (!align.empty()) json += "\"align\":\"" + align + "\"";
      if (!position.empty()) json += std::string(align.empty() ? "" : ",") + "\"position\":\"" + position + "\"";
      cue.style_json = json + "}";
    }
  }
  cue.id = "cue-" + std::to_string(++counter);
  result.cues.push_back(std::move(cue));
}

}  // namespace

ParseResult ParseSrt(const std::string& text) {
  ParseResult result;
  const auto lines = SplitLines(text);
  int counter = 0;
  for (const auto& block : Blocks(lines, 0)) ReadCue(block, false, counter, result);
  std::stable_sort(result.cues.begin(), result.cues.end(), [](const Cue& a, const Cue& b) { return a.start.Compare(b.start) < 0; });
  return result;
}

ParseResult ParseWebVtt(const std::string& text) {
  ParseResult result;
  const auto lines = SplitLines(text);
  std::size_t first = 0;
  while (first < lines.size() && Trim(lines[first]).empty()) ++first;
  if (first >= lines.size() || lines[first].rfind("WEBVTT", 0) != 0) {
    result.issues.push_back({static_cast<int>(first) + 1, "the file does not begin with WEBVTT; it was not read", true});
    return result;
  }
  int counter = 0;
  for (const auto& block : Blocks(lines, first + 1)) {
    const auto head = block.lines.front();
    if (head.rfind("NOTE", 0) == 0 || head.rfind("STYLE", 0) == 0 || head.rfind("REGION", 0) == 0) continue;
    ReadCue(block, true, counter, result);
  }
  std::stable_sort(result.cues.begin(), result.cues.end(), [](const Cue& a, const Cue& b) { return a.start.Compare(b.start) < 0; });
  return result;
}

ParseResult ParseCaptions(const std::string& text) {
  const auto lines = SplitLines(text);
  for (const auto& line : lines) {
    const auto trimmed = Trim(line);
    if (trimmed.empty()) continue;
    return trimmed.rfind("WEBVTT", 0) == 0 ? ParseWebVtt(text) : ParseSrt(text);
  }
  return {};
}

// ------------------------------------------------------------------- writing ----

namespace {

std::int64_t Milliseconds(const RationalTime& t, bool& rounded) {
  // Exact when the denominator divides a millisecond: 1000 * num is a multiple of den.
  if ((t.numerator() * 1000) % t.denominator() != 0) rounded = true;
  return t.Rescale(1000, time::RoundingMode::Nearest);
}

std::string Stamp(std::int64_t ms, char separator) {
  const auto hours = ms / 3600000;
  const auto minutes = (ms / 60000) % 60;
  const auto seconds = (ms / 1000) % 60;
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%02lld:%02lld:%02lld%c%03lld", static_cast<long long>(hours), static_cast<long long>(minutes),
                static_cast<long long>(seconds), separator, static_cast<long long>(ms % 1000));
  return buffer;
}

int AnFor(const Style& style) {
  const int row = style.position == "top" ? 2 : style.position == "middle" ? 1 : 0;
  const int column = style.align == "left" ? 1 : style.align == "right" ? 3 : 2;
  return row * 3 + column;
}

}  // namespace

WriteResult WriteSrt(const std::vector<Cue>& cues) {
  WriteResult result;
  std::ostringstream out;
  bool rounded = false, styled = false;
  int index = 0;
  for (const auto& cue : cues) {
    const auto start = Milliseconds(cue.start, rounded);
    const auto end = Milliseconds(cue.end, rounded);
    out << ++index << "\n" << Stamp(start, ',') << " --> " << Stamp(end, ',') << "\n";
    const auto style = ParseStyle(cue.style_json);
    const int an = AnFor(style);
    if (an != 2) out << "{\\an" << an << "}";
    if (cue.style_json != "{}" && !cue.style_json.empty()) {
      const auto parsed = json::Parse(cue.style_json);
      for (const auto& [key, value] : parsed.members) {
        if (key != "align" && key != "position") styled = true;
      }
    }
    out << cue.text << "\n\n";
  }
  if (rounded) result.issues.push_back({0, "some cue times are not whole milliseconds and were rounded to the nearest one", false});
  if (styled) result.issues.push_back({0, "SubRip has no styling beyond position; fonts, colours and sizes were not written", false});
  result.text = out.str();
  return result;
}

WriteResult WriteWebVtt(const std::vector<Cue>& cues) {
  WriteResult result;
  std::ostringstream out;
  bool rounded = false, styled = false;
  out << "WEBVTT\n\n";
  int index = 0;
  for (const auto& cue : cues) {
    const auto start = Milliseconds(cue.start, rounded);
    const auto end = Milliseconds(cue.end, rounded);
    out << ++index << "\n" << Stamp(start, '.') << " --> " << Stamp(end, '.');
    const auto style = ParseStyle(cue.style_json);
    if (style.align != "center") out << " align:" << (style.align == "left" ? "start" : "end");
    if (style.position == "top") out << " line:10%";
    else if (style.position == "middle") out << " line:50%";
    out << "\n";
    for (const char c : cue.text) {
      if (c == '&') out << "&amp;";
      else if (c == '<') out << "&lt;";
      else if (c == '>') out << "&gt;";
      else out << c;
    }
    out << "\n\n";
    if (cue.style_json != "{}" && !cue.style_json.empty()) {
      const auto parsed = json::Parse(cue.style_json);
      for (const auto& [key, value] : parsed.members) {
        if (key != "align" && key != "position") styled = true;
      }
    }
  }
  if (rounded) result.issues.push_back({0, "some cue times are not whole milliseconds and were rounded to the nearest one", false});
  if (styled) result.issues.push_back({0, "this writer carries only position and alignment; fonts, colours and sizes were not written", false});
  result.text = out.str();
  return result;
}

// -------------------------------------------------------------------- layout ----

Rect ActionSafe(double w, double h) { return {w * 0.05, h * 0.05, w * 0.9, h * 0.9}; }
Rect TitleSafe(double w, double h) { return {w * 0.1, h * 0.1, w * 0.8, h * 0.8}; }

std::vector<ActiveCue> ActiveCues(const std::vector<Track>& tracks, const RationalTime& at) {
  std::vector<ActiveCue> active;
  for (const auto& track : tracks) {
    for (const auto& cue : track.cues) {
      if (cue.start.Compare(at) > 0) break;  // sorted: nothing later can be showing
      if (cue.end.Compare(at) > 0) active.push_back({&track, &cue});
    }
  }
  return active;
}

std::vector<Sidecar> WriteSidecars(const std::vector<Track>& tracks, const std::string& directory, const std::string& base_name, bool srt,
                                   bool vtt) {
  std::vector<Sidecar> written;
  std::filesystem::create_directories(directory);
  int index = 0;
  std::vector<std::string> used;
  for (const auto& track : tracks) {
    ++index;
    std::string stem = base_name;
    if (!track.language.empty()) stem += "." + track.language;
    if (std::find(used.begin(), used.end(), stem) != used.end()) stem += "." + std::to_string(index);
    used.push_back(stem);
    const auto write = [&](const std::string& extension, const std::string& format, const WriteResult& result) {
      const auto path = (std::filesystem::path(directory) / (stem + "." + extension)).string();
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      file << result.text;
      written.push_back({path, track.id, format});
    };
    if (srt) write("srt", "srt", WriteSrt(track.cues));
    if (vtt) write("vtt", "vtt", WriteWebVtt(track.cues));
  }
  return written;
}

}  // namespace cutline::captions

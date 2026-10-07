#include "speech/Transcript.h"

#include "core/util/Json.h"
#include "core/util/JsonParse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cutline::speech {
namespace {

bool IsPunctuationByte(unsigned char c) { return c < 0x80 && std::ispunct(c) != 0; }

bool OnlyPunctuation(const std::string& text) {
  if (text.empty()) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return IsPunctuationByte(static_cast<unsigned char>(c)) || c == ' '; });
}

// The engine's control tokens: [_BEG_], [_TT_150], [_SOT_] and the like, and <|...|> markers.
bool ControlToken(const std::string& text) {
  if (text.size() >= 4 && text.front() == '[' && text[1] == '_' && text.back() == ']') return true;
  return text.size() >= 4 && text.compare(0, 2, "<|") == 0;
}

std::string TrimSpaces(const std::string& text) {
  std::size_t a = 0, b = text.size();
  while (a < b && std::isspace(static_cast<unsigned char>(text[a])) != 0) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1])) != 0) --b;
  return text.substr(a, b - a);
}

double Seconds(std::int64_t milliseconds) { return static_cast<double>(milliseconds) / 1000.0; }

struct Builder final {
  std::vector<Word> words;
  Word word;
  bool open{false};
  bool spoken{false};  // the end has been set by a token that is not punctuation
  double confidence_sum{0.0};
  int confidence_count{0};

  void Flush() {
    if (!open) return;
    if (!word.text.empty()) {
      word.confidence = confidence_count > 0 ? static_cast<float>(confidence_sum / confidence_count) : 1.0f;
      if (word.end < word.start) word.end = word.start;
      // Words are in time order, even where the engine's token times wobble.
      if (!words.empty() && word.start < words.back().start) {
        word.end = std::max(word.end, words.back().start);
        word.start = words.back().start;
      }
      words.push_back(word);
    }
    word = {};
    open = false;
    spoken = false;
    confidence_sum = 0.0;
    confidence_count = 0;
  }

  void Add(const std::string& raw, double from, double to, double probability) {
    if (raw.empty()) return;
    const bool starts_word = raw.front() == ' ' || !open;
    if (starts_word) {
      Flush();
      open = true;
      word.start = from;
      word.end = to;
    }
    word.text += TrimSpaces(raw).empty() ? std::string() : (starts_word ? TrimSpaces(raw) : raw);
    const bool punctuation = OnlyPunctuation(TrimSpaces(raw));
    if (!punctuation) {
      word.end = to;
      spoken = true;
      confidence_sum += probability;
      ++confidence_count;
    } else if (!spoken) {
      word.end = std::max(word.end, to);
    }
  }
};

}  // namespace

// ------------------------------------------------------------------------------ whisper ----

Transcript ParseWhisperJson(std::string_view json) {
  json::Value root;
  try {
    root = json::Parse(json);
  } catch (const json::ParseError& error) {
    throw std::runtime_error(std::string("The transcript is not valid JSON: ") + error.what());
  }
  const auto* segments = root.Find("transcription");
  if (segments == nullptr || !segments->is_array()) throw std::runtime_error("The transcript has no \"transcription\" list");

  Transcript transcript;
  if (const auto* result = root.Find("result"); result != nullptr && result->is_object()) {
    if (const auto* language = result->Find("language"); language != nullptr && language->is_string()) transcript.language = language->text;
  }
  if (const auto* params = root.Find("params"); params != nullptr && params->is_object()) {
    if (const auto* model = params->Find("model"); model != nullptr && model->is_string()) {
      transcript.engine = "whisper.cpp " + std::filesystem::path(model->text).stem().string();
    }
  }

  Builder builder;
  double last_offset = 0.0;
  for (const auto& segment : segments->items) {
    if (!segment.is_object()) continue;
    const auto* offsets = segment.Find("offsets");
    const double segment_from = offsets != nullptr && offsets->Find("from") != nullptr ? Seconds(offsets->Find("from")->AsInteger()) : 0.0;
    const double segment_to = offsets != nullptr && offsets->Find("to") != nullptr ? Seconds(offsets->Find("to")->AsInteger()) : segment_from;
    last_offset = std::max(last_offset, segment_to);
    const auto* tokens = segment.Find("tokens");
    if (tokens != nullptr && tokens->is_array() && !tokens->items.empty()) {
      for (const auto& token : tokens->items) {
        const auto* text = token.Find("text");
        if (text == nullptr || !text->is_string() || ControlToken(text->text)) continue;
        const auto* token_offsets = token.Find("offsets");
        if (token_offsets == nullptr) continue;
        const auto* from = token_offsets->Find("from");
        const auto* to = token_offsets->Find("to");
        if (from == nullptr || to == nullptr) continue;
        const auto* p = token.Find("p");
        builder.Add(text->text, Seconds(from->AsInteger()), Seconds(to->AsInteger()), p != nullptr && p->is_number() ? p->number : 1.0);
      }
      continue;
    }
    // A segment without tokens: its words share its time in proportion to their length.
    const auto* text = segment.Find("text");
    if (text == nullptr || !text->is_string()) continue;
    std::istringstream stream(text->text);
    std::vector<std::string> parts;
    for (std::string part; stream >> part;) {
      if (!ControlToken(part)) parts.push_back(part);
    }
    std::size_t total = 0;
    for (const auto& part : parts) total += part.size() + 1;
    double cursor = segment_from;
    for (const auto& part : parts) {
      const double share = (segment_to - segment_from) * static_cast<double>(part.size() + 1) / static_cast<double>(std::max<std::size_t>(total, 1));
      builder.Add(" " + part, cursor, cursor + share, 1.0);
      cursor += share;
    }
  }
  builder.Flush();
  transcript.words = std::move(builder.words);
  transcript.duration = last_offset;
  return transcript;
}

// -------------------------------------------------------------------------------- storage ----

namespace {

constexpr int kVersion = 1;

std::string WordJson(const Word& word) {
  json::Object object;
  object.Add("t", word.text).AddRaw("s", json::Number(word.start)).AddRaw("e", json::Number(word.end));
  object.AddRaw("c", json::Number(static_cast<double>(word.confidence)));
  if (word.speaker >= 0) object.Add("k", static_cast<std::int64_t>(word.speaker));
  return object.Build();
}

}  // namespace

std::string ToJson(const Transcript& transcript) {
  json::Object object;
  object.Add("version", static_cast<std::int64_t>(kVersion));
  object.Add("media_id", transcript.media_id).Add("fingerprint", transcript.fingerprint);
  object.Add("language", transcript.language).Add("engine", transcript.engine);
  object.AddRaw("duration", json::Number(transcript.duration));
  std::vector<std::string> speakers;
  for (const auto& name : transcript.speakers) speakers.push_back("\"" + json::Escape(name) + "\"");
  object.AddRaw("speakers", json::Array(speakers));
  std::vector<std::string> words;
  words.reserve(transcript.words.size());
  for (const auto& word : transcript.words) words.push_back(WordJson(word));
  object.AddRaw("words", json::Array(words));
  return object.Build();
}

Transcript FromJson(std::string_view text) {
  json::Value root;
  try {
    root = json::Parse(text);
  } catch (const json::ParseError& error) {
    throw std::runtime_error(std::string("The transcript file is damaged: ") + error.what());
  }
  if (!root.is_object() || root.Find("words") == nullptr || !root.Find("words")->is_array()) throw std::runtime_error("The transcript file holds no words");
  if (const auto* version = root.Find("version"); version == nullptr || version->AsInteger() != kVersion) throw std::runtime_error("The transcript file is of another version");
  Transcript transcript;
  const auto text_of = [&](const char* key) { const auto* v = root.Find(key); return v != nullptr && v->is_string() ? v->text : std::string(); };
  transcript.media_id = text_of("media_id");
  transcript.fingerprint = text_of("fingerprint");
  transcript.language = text_of("language");
  transcript.engine = text_of("engine");
  if (const auto* duration = root.Find("duration"); duration != nullptr && duration->is_number()) transcript.duration = duration->number;
  if (const auto* speakers = root.Find("speakers"); speakers != nullptr && speakers->is_array()) {
    for (const auto& name : speakers->items) transcript.speakers.push_back(name.text);
  }
  for (const auto& item : root.Find("words")->items) {
    Word word;
    word.text = item.String("t");
    word.start = item.Number("s");
    word.end = item.Number("e");
    if (const auto* c = item.Find("c"); c != nullptr && c->is_number()) word.confidence = static_cast<float>(c->number);
    if (const auto* k = item.Find("k"); k != nullptr && k->is_number()) word.speaker = static_cast<int>(k->AsInteger());
    if (word.speaker >= static_cast<int>(transcript.speakers.size())) word.speaker = -1;
    transcript.words.push_back(std::move(word));
  }
  return transcript;
}

bool Save(const std::string& path, const Transcript& transcript) {
  std::error_code ignored;
  const std::filesystem::path target(path);
  if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ignored);
  const auto temporary = target.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << ToJson(transcript);
    out.flush();
    if (!out) return false;
  }
  std::filesystem::rename(temporary, target, ignored);
  if (ignored) {
    std::filesystem::remove(temporary, ignored);
    return false;
  }
  return true;
}

std::optional<Transcript> Load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  try {
    return FromJson(buffer.str());
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// ------------------------------------------------------------------------------- reading ----

std::string Normalised(const std::string& text) {
  std::size_t a = 0, b = text.size();
  while (a < b && IsPunctuationByte(static_cast<unsigned char>(text[a]))) ++a;
  while (b > a && IsPunctuationByte(static_cast<unsigned char>(text[b - 1]))) --b;
  std::string out;
  out.reserve(b - a);
  for (std::size_t i = a; i < b; ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    out.push_back(c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c));
  }
  return out;
}

namespace {

std::vector<std::string> SplitWords(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream stream(text);
  for (std::string part; stream >> part;) {
    auto normal = Normalised(part);
    if (!normal.empty()) out.push_back(std::move(normal));
  }
  return out;
}

// Matches the words of `phrase` at `at`, skipping words that are nothing but punctuation.
bool MatchesAt(const std::vector<std::string>& normal, std::size_t at, const std::vector<std::string>& phrase) {
  if (at + phrase.size() > normal.size()) return false;
  for (std::size_t i = 0; i < phrase.size(); ++i) {
    if (normal[at + i] != phrase[i]) return false;
  }
  return true;
}

std::vector<std::string> NormalisedWords(const Transcript& transcript) {
  std::vector<std::string> out;
  out.reserve(transcript.words.size());
  for (const auto& word : transcript.words) out.push_back(Normalised(word.text));
  return out;
}

}  // namespace

std::vector<WordRange> Find(const Transcript& transcript, const std::string& query) {
  const auto phrase = SplitWords(query);
  std::vector<WordRange> found;
  if (phrase.empty()) return found;
  const auto normal = NormalisedWords(transcript);
  for (std::size_t i = 0; i + phrase.size() <= normal.size();) {
    if (MatchesAt(normal, i, phrase)) {
      found.push_back({i, phrase.size()});
      i += phrase.size();
    } else {
      ++i;
    }
  }
  return found;
}

std::vector<WordRange> FindFillers(const Transcript& transcript, const FillerOptions& options) {
  static const char* const kSingles[] = {"um", "umm", "ummm", "uh", "uhh", "uhhh", "uhm", "er", "err", "erm", "ah", "ahh", "hm", "hmm", "hmmm", "mm", "mmm", "mhm"};
  std::vector<std::vector<std::string>> phrases;
  for (const auto* single : kSingles) phrases.push_back({single});
  if (options.phrases) {
    phrases.push_back({"you", "know"});
    phrases.push_back({"i", "mean"});
  }
  for (const auto& extra : options.extra) {
    auto phrase = SplitWords(extra);
    if (!phrase.empty()) phrases.push_back(std::move(phrase));
  }
  // The longest first, so "you know" is not half-taken by a shorter entry.
  std::stable_sort(phrases.begin(), phrases.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
  const auto normal = NormalisedWords(transcript);
  std::vector<WordRange> found;
  for (std::size_t i = 0; i < normal.size();) {
    bool matched = false;
    for (const auto& phrase : phrases) {
      if (MatchesAt(normal, i, phrase)) {
        found.push_back({i, phrase.size()});
        i += phrase.size();
        matched = true;
        break;
      }
    }
    if (!matched) ++i;
  }
  return found;
}

std::vector<Pause> FindPauses(const Transcript& transcript, double minimum) {
  std::vector<Pause> pauses;
  for (std::size_t i = 0; i + 1 < transcript.words.size(); ++i) {
    const double start = transcript.words[i].end, end = transcript.words[i + 1].start;
    if (end - start >= minimum && end > start) pauses.push_back({i, start, end});
  }
  return pauses;
}

// --------------------------------------------------------------------------------- captions ----

namespace {

bool EndsSentence(const std::string& text) {
  auto end = text.size();
  while (end > 0 && (text[end - 1] == '"' || text[end - 1] == '\'' || text[end - 1] == ')')) --end;
  return end > 0 && (text[end - 1] == '.' || text[end - 1] == '!' || text[end - 1] == '?');
}

// The characters of words [a, b] joined by single spaces.
std::size_t TextLength(const std::vector<TimedWord>& words, std::size_t a, std::size_t b) {
  std::size_t total = 0;
  for (std::size_t i = a; i <= b; ++i) total += words[i].text.size() + (i > a ? 1 : 0);
  return total;
}

// Whether the words fit in `lines` lines of at most `max_chars`, filling each in turn.
bool FitsLines(const std::vector<TimedWord>& words, std::size_t a, std::size_t b, int max_chars, int lines) {
  int used = 1;
  std::size_t line = 0;
  for (std::size_t i = a; i <= b; ++i) {
    const std::size_t size = words[i].text.size();
    if (line == 0) {
      line = size;
    } else if (line + 1 + size <= static_cast<std::size_t>(max_chars)) {
      line += 1 + size;
    } else {
      ++used;
      line = size;
    }
    if (used > lines) return false;
  }
  return true;
}

// Breaks words [a, b] into at most `lines` lines, as even as the words allow. Two lines are broken where the longer
// one is shortest; more are filled in turn.
std::string BreakLines(const std::vector<TimedWord>& words, std::size_t a, std::size_t b, int max_chars, int lines) {
  const auto join = [&](std::size_t from, std::size_t to) {
    std::string text;
    for (std::size_t i = from; i <= to; ++i) text += (i > from ? " " : "") + words[i].text;
    return text;
  };
  if (TextLength(words, a, b) <= static_cast<std::size_t>(max_chars) || a == b || lines <= 1) return join(a, b);
  if (lines == 2) {
    std::size_t best = a;
    std::size_t best_longest = SIZE_MAX;
    for (std::size_t split = a; split < b; ++split) {
      const auto left = TextLength(words, a, split), right = TextLength(words, split + 1, b);
      const auto longest = std::max(left, right);
      if (longest < best_longest) {
        best_longest = longest;
        best = split;
      }
    }
    return join(a, best) + "\n" + join(best + 1, b);
  }
  std::string text, line;
  for (std::size_t i = a; i <= b; ++i) {
    if (!line.empty() && line.size() + 1 + words[i].text.size() > static_cast<std::size_t>(max_chars)) {
      text += line + "\n";
      line.clear();
    }
    line += (line.empty() ? "" : " ") + words[i].text;
  }
  return text + line;
}

}  // namespace

std::vector<CaptionGroup> GroupCaptions(const std::vector<TimedWord>& words, const CaptionOptions& options) {
  std::vector<CaptionGroup> groups;
  const std::size_t n = words.size();
  const int max_chars = std::max(options.max_line_chars, 8);
  const int max_lines = std::max(options.max_lines, 1);
  std::size_t first = 0;
  while (first < n) {
    std::size_t last = first;
    while (last + 1 < n) {
      const auto& current = words[last];
      const auto& next = words[last + 1];
      if (next.start - current.end >= options.break_pause) break;
      if (last - first + 1 >= 3 && EndsSentence(current.text)) break;
      if (next.end - words[first].start > options.max_seconds) break;
      if (!FitsLines(words, first, last + 1, max_chars, max_lines)) break;
      ++last;
    }
    groups.push_back({first, last, BreakLines(words, first, last, max_chars, max_lines)});
    first = last + 1;
  }
  return groups;
}

}  // namespace cutline::speech

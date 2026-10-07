#pragma once

// A transcript: what is said in a piece of media, word by word, with when each word is said.
//
// The words are the editing surface of text-based editing. Each carries its start and end in the media's own time
// (seconds from the start of the file, the same clock a clip's source range runs on), so a word maps onto any clip
// that shows that part of the media, however it has been trimmed or moved since. The transcript is kept as a sidecar
// file next to the project (never in the project database: it is derived data, can be large, and is regenerated
// from the media), keyed by the media's fingerprint so a replaced file is not given the old words.
//
// Everything here is pure: parsing what an engine wrote, finding fillers and pauses, searching, and deciding where
// caption lines break. Running an engine is speech/Whisper.h; turning words into edits on a timeline is ui/TextEdit.h.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::speech {

struct Word final {
  std::string text;      // as it is shown, with its punctuation and capitals
  double start{0.0};     // seconds in the media
  double end{0.0};
  float confidence{1.0f};  // 0 to 1, the engine's own estimate
  int speaker{-1};       // index into Transcript::speakers, or -1 when nobody has been named
};

struct Transcript final {
  std::string media_id;
  // The media's fingerprint when it was transcribed: a transcript of another file is not this one's.
  std::string fingerprint;
  std::string language;  // "en", or empty when unknown
  std::string engine;    // what made it, for the record ("whisper.cpp ggml-base.en")
  // Length of the audio that was analysed, in seconds.
  double duration{0.0};
  // Names given to speakers by hand. Detecting who is speaking is not something the offline engine does.
  std::vector<std::string> speakers;
  std::vector<Word> words;
};

// ---------------------------------------------------------------------- reading and writing ----

// Reads the JSON that whisper.cpp writes with --output-json-full and --max-len 1: segments holding tokens, each
// with an offset in milliseconds. Tokens are joined into words (a token starting with a space starts a word), a
// word starts where its first token starts and ends where its last spoken token ends (punctuation is given the
// time of the silence after it, so it is not counted), and the engine's control tokens ([_BEG_], [_TT_...]) are
// dropped. Throws std::runtime_error when the text is not that document.
[[nodiscard]] Transcript ParseWhisperJson(std::string_view json);

// The transcript in the form it is stored in, and back. FromJson throws std::runtime_error on a document that is
// not one (a torn write, another version).
[[nodiscard]] std::string ToJson(const Transcript& transcript);
[[nodiscard]] Transcript FromJson(std::string_view json);

// The sidecar file. Save writes a temporary file and renames it, so a crash leaves the old transcript or the new one.
[[nodiscard]] bool Save(const std::string& path, const Transcript& transcript);
[[nodiscard]] std::optional<Transcript> Load(const std::string& path);

// ------------------------------------------------------------------------------- reading it ----

// The word as it is compared: lower case, without the punctuation around it ("Umm," is "umm", "don't" is kept).
[[nodiscard]] std::string Normalised(const std::string& text);

// A run of words, for results of a search.
struct WordRange final {
  std::size_t first{0};
  std::size_t count{0};
  [[nodiscard]] std::size_t last() const { return first + count - 1; }
};

// Where the words of `query` occur one after another (case and punctuation ignored). An empty query finds nothing.
[[nodiscard]] std::vector<WordRange> Find(const Transcript& transcript, const std::string& query);

struct FillerOptions final {
  // "you know" and "i mean" are often meant; they are found only when asked for.
  bool phrases{false};
  // Words to treat as fillers besides the built-in ones, compared Normalised.
  std::vector<std::string> extra;
};

// The sounds people make while they think (um, uh, er, ah, hmm and their stretched spellings), and optionally the
// phrases that do the same job. Each result is one filler: a word, or the words of a phrase.
[[nodiscard]] std::vector<WordRange> FindFillers(const Transcript& transcript, const FillerOptions& options = {});

struct Pause final {
  // The pause is between word `after` and the next; both are indices into the transcript.
  std::size_t after{0};
  double start{0.0};
  double end{0.0};
  [[nodiscard]] double length() const { return end - start; }
};

// The silences between words that run at least `minimum` seconds. (Speech before the first word and after the last
// is not a pause between words.)
[[nodiscard]] std::vector<Pause> FindPauses(const Transcript& transcript, double minimum);

// ----------------------------------------------------------------------------- caption lines ----

struct TimedWord final {
  std::string text;
  double start{0.0};
  double end{0.0};
};

struct CaptionOptions final {
  // The longest a line of text may be, in characters, and how many lines a cue may have.
  int max_line_chars{42};
  int max_lines{2};
  // The longest a cue may stay on the screen.
  double max_seconds{6.0};
  // A silence this long ends a cue even if there is room.
  double break_pause{0.8};
};

struct CaptionGroup final {
  std::size_t first{0};
  std::size_t last{0};  // inclusive
  std::string text;     // lines separated by \n
};

// Groups the words into cues: a cue ends at a sentence end when it has three words or more, at a long pause, when
// it would be on the screen too long, or when its lines are full; the text is broken into lines of balanced length.
[[nodiscard]] std::vector<CaptionGroup> GroupCaptions(const std::vector<TimedWord>& words, const CaptionOptions& options = {});

}  // namespace cutline::speech

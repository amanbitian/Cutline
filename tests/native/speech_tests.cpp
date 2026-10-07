// Transcripts: reading what the speech engine writes, finding fillers and pauses, searching, caption lines, the
// sidecar file, and (where the engine and a model are on this machine) a real run of it on a recorded sentence.

#include "tests/native/TestHarness.h"

#include "media/Providers.h"
#include "speech/Transcript.h"
#include "speech/Whisper.h"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace cutline::speech;

namespace {

// The shape of whisper.cpp's --output-json-full output with --max-len 1: a segment per word, tokens inside with
// offsets in milliseconds. Control tokens carry no speech; punctuation takes the time of the silence after it.
const char* const kWhisper = R"JSON({
  "systeminfo": "AVX = 1",
  "model": {"type": "base"},
  "params": {"model": "..\\ggml-base.en.bin", "language": "en", "translate": false},
  "result": {"language": "en"},
  "transcription": [
    {"timestamps": {"from": "00:00:00,000", "to": "00:00:00,120"}, "offsets": {"from": 0, "to": 120}, "text": " ",
     "tokens": [{"text": "[_BEG_]", "timestamps": {}, "offsets": {"from": 0, "to": 0}, "id": 50363, "p": 0.99, "t_dtw": -1}]},
    {"offsets": {"from": 120, "to": 570}, "text": " Welcome",
     "tokens": [{"text": " Welcome", "offsets": {"from": 120, "to": 570}, "id": 1, "p": 0.9}]},
    {"offsets": {"from": 570, "to": 1560}, "text": " show.",
     "tokens": [{"text": " show", "offsets": {"from": 780, "to": 1480}, "id": 2, "p": 0.8},
                {"text": ".", "offsets": {"from": 1480, "to": 1560}, "id": 3, "p": 0.9}]},
    {"offsets": {"from": 1560, "to": 2030}, "text": " Umm,",
     "tokens": [{"text": " U", "offsets": {"from": 1650, "to": 1650}, "id": 4, "p": 0.36},
                {"text": "mm", "offsets": {"from": 1680, "to": 1840}, "id": 5, "p": 0.56},
                {"text": ",", "offsets": {"from": 1840, "to": 2030}, "id": 6, "p": 0.9}]},
    {"offsets": {"from": 2030, "to": 2510}, "text": " today",
     "tokens": [{"text": " today", "offsets": {"from": 2030, "to": 2510}, "id": 7, "p": 0.95}]},
    {"offsets": {"from": 2510, "to": 5000}, "text": " thanks.",
     "tokens": [{"text": " thanks", "offsets": {"from": 4600, "to": 5000}, "id": 8, "p": 0.7},
                {"text": "[_TT_250]", "offsets": {"from": 5000, "to": 5000}, "id": 50614, "p": 0.1}]}
  ]
})JSON";

Transcript Sample() { return ParseWhisperJson(kWhisper); }

std::filesystem::path Scratch(const std::string& name) {
  const auto directory = std::filesystem::temp_directory_path() / "cutline-speech-tests";
  std::filesystem::create_directories(directory);
  return directory / name;
}

std::filesystem::path RepositoryRoot() {
  const auto golden = cutline::testing::EnvironmentValue("CUTLINE_GOLDEN_DIR");
  if (golden.empty()) return {};
  return std::filesystem::path(golden).parent_path().parent_path();
}

}  // namespace

CUTLINE_TEST(TheEnginesWordsBecomeTimedWordsWithoutControlTokensAndWithTheSpokenEnd) {
  const auto transcript = Sample();
  CHECK_EQ(transcript.words.size(), std::size_t{5});
  CHECK(transcript.language == "en" && transcript.engine == "whisper.cpp ggml-base.en");
  CHECK(transcript.words[0].text == "Welcome" && std::abs(transcript.words[0].start - 0.12) < 1e-9 && std::abs(transcript.words[0].end - 0.57) < 1e-9);
  // The word ends where its sound does: the full stop is given the silence after it.
  CHECK(transcript.words[1].text == "show." && std::abs(transcript.words[1].start - 0.78) < 1e-9 && std::abs(transcript.words[1].end - 1.48) < 1e-9);
  // Tokens of one word are joined, and its confidence is the mean over what was spoken.
  CHECK(transcript.words[2].text == "Umm," && std::abs(transcript.words[2].start - 1.65) < 1e-9 && std::abs(transcript.words[2].end - 1.84) < 1e-9);
  CHECK(std::abs(transcript.words[2].confidence - 0.46) < 1e-3);
  CHECK(transcript.words[4].text == "thanks" || transcript.words[4].text == "thanks.");
  for (std::size_t i = 1; i < transcript.words.size(); ++i) CHECK(transcript.words[i].start >= transcript.words[i - 1].start);
  CHECK_THROWS(ParseWhisperJson("not json"));
  CHECK_THROWS(ParseWhisperJson("{\"a\": 1}"));
}

CUTLINE_TEST(ASegmentWithoutTokensSharesItsTimeAmongItsWords) {
  const auto transcript = ParseWhisperJson(R"({"transcription":[{"offsets":{"from":1000,"to":3000},"text":" hello big world"}]})");
  CHECK_EQ(transcript.words.size(), std::size_t{3});
  CHECK(std::abs(transcript.words[0].start - 1.0) < 1e-9 && std::abs(transcript.words[2].end - 3.0) < 1e-6);
  CHECK(transcript.words[1].start == transcript.words[0].end);
}

CUTLINE_TEST(TheTranscriptIsAFileThatComesBackWholeAndAWronglyShapedOneIsRefused) {
  auto transcript = Sample();
  transcript.media_id = "m1";
  transcript.fingerprint = "abc123";
  transcript.speakers = {"Ana \"the host\"", "Ben"};
  transcript.words[0].speaker = 0;
  transcript.words[3].speaker = 1;
  transcript.words[1].text = "shöw\n.";
  const auto path = Scratch("roundtrip.json");
  CHECK(Save(path.string(), transcript));
  CHECK(!std::filesystem::exists(path.string() + ".tmp"));
  const auto loaded = Load(path.string());
  CHECK(loaded.has_value());
  CHECK(loaded->media_id == "m1" && loaded->fingerprint == "abc123" && loaded->language == "en" && loaded->engine == transcript.engine);
  CHECK_EQ(loaded->words.size(), transcript.words.size());
  for (std::size_t i = 0; i < transcript.words.size(); ++i) {
    CHECK(loaded->words[i].text == transcript.words[i].text);
    CHECK(loaded->words[i].start == transcript.words[i].start && loaded->words[i].end == transcript.words[i].end);
    CHECK(loaded->words[i].speaker == transcript.words[i].speaker);
  }
  CHECK(loaded->speakers == transcript.speakers);
  CHECK(!Load(Scratch("missing.json").string()).has_value());
  { std::ofstream(Scratch("torn.json").string(), std::ios::binary) << ToJson(transcript).substr(0, 200); }
  CHECK(!Load(Scratch("torn.json").string()).has_value());
  CHECK_THROWS(FromJson("{\"version\": 99, \"words\": []}"));
  // A speaker index that points at no speaker is dropped rather than kept dangling.
  auto dangling = FromJson(R"({"version":1,"speakers":["A"],"words":[{"t":"x","s":0,"e":1,"c":1,"k":4}]})");
  CHECK_EQ(dangling.words[0].speaker, -1);
}

CUTLINE_TEST(FillersPausesAndPhrasesAreFoundIgnoringCaseAndPunctuation) {
  auto transcript = Sample();
  const auto fillers = FindFillers(transcript);
  CHECK_EQ(fillers.size(), std::size_t{1});
  CHECK(fillers[0].first == 2 && fillers[0].count == 1);
  // The longer pauses only.
  CHECK_EQ(FindPauses(transcript, 0.5).size(), std::size_t{1});
  const auto pause = FindPauses(transcript, 0.5)[0];
  CHECK(pause.after == 3 && std::abs(pause.start - 2.51) < 1e-9 && std::abs(pause.end - 4.6) < 1e-9);
  CHECK(FindPauses(transcript, 3.0).empty());
  CHECK_EQ(FindPauses(transcript, 0.1).size(), std::size_t{4});

  // Phrases: only on request; a phrase is one filler of several words; extras are the person's own.
  Transcript spoken;
  for (const auto* text : {"So,", "you", "know,", "I", "mean", "it", "is", "basically", "fine.", "Uh"}) spoken.words.push_back({text, 0, 0});
  CHECK_EQ(FindFillers(spoken).size(), std::size_t{1});
  FillerOptions phrases;
  phrases.phrases = true;
  const auto with_phrases = FindFillers(spoken, phrases);
  CHECK_EQ(with_phrases.size(), std::size_t{3});
  CHECK(with_phrases[0].first == 1 && with_phrases[0].count == 2 && with_phrases[1].first == 3 && with_phrases[1].count == 2);
  phrases.extra = {"Basically"};
  CHECK_EQ(FindFillers(spoken, phrases).size(), std::size_t{4});

  // Search: case and punctuation ignored, words in order.
  CHECK_EQ(Find(transcript, "SHOW umm").size(), std::size_t{1});
  CHECK(Find(transcript, "show, umm")[0].first == 1 && Find(transcript, "show umm")[0].count == 2);
  CHECK(Find(transcript, "umm show").empty());
  CHECK(Find(transcript, "  ").empty());
  CHECK_EQ(Find(spoken, "i").size(), std::size_t{1});
  CHECK(Normalised("\"Don't!\"") == "don't" && Normalised("Umm,") == "umm");
}

CUTLINE_TEST(CaptionsBreakAtPausesSentencesAndFullLinesAndLinesAreBalanced) {
  const auto transcript = Sample();
  std::vector<TimedWord> words;
  for (const auto& w : transcript.words) words.push_back({w.text, w.start, w.end});
  auto groups = GroupCaptions(words);
  CHECK_EQ(groups.size(), std::size_t{2});   // the two-second silence ends the first cue
  CHECK(groups[0].first == 0 && groups[0].last == 3 && groups[0].text == "Welcome show. Umm, today");
  CHECK(groups[1].first == 4 && groups[1].last == 4);

  // A run of words with no pauses: cues stay within the lines they are given, and a long one is split into even lines.
  std::vector<TimedWord> talk;
  const char* const sentence[] = {"This", "is", "a", "long", "sentence", "that", "keeps", "going", "without", "any", "pause", "at", "all", "and", "then", "ends."};
  double t = 0;
  for (const auto* text : sentence) {
    talk.push_back({text, t, t + 0.3});
    t += 0.3;
  }
  CaptionOptions options;
  options.max_line_chars = 24;
  options.max_lines = 2;
  groups = GroupCaptions(talk, options);
  CHECK(groups.size() >= 2);
  std::size_t next = 0;
  for (const auto& group : groups) {
    CHECK_EQ(group.first, next);
    next = group.last + 1;
    int lines = 1;
    std::size_t line = 0;
    for (const char c : group.text) {
      if (c == '\n') {
        ++lines;
        line = 0;
      } else {
        ++line;
        CHECK(line <= 24);
      }
    }
    CHECK(lines <= 2);
  }
  CHECK_EQ(next, talk.size());
  // The time limit also ends a cue.
  CaptionOptions short_cues;
  short_cues.max_seconds = 1.0;
  CHECK(GroupCaptions(talk, short_cues).size() >= 4);
  CHECK(GroupCaptions({}, options).empty());
}

CUTLINE_TEST(TheEngineIsFoundByExplicitPathOrBySearchingAndNeedsAModel) {
  const auto root = Scratch("engine-root");
  std::filesystem::remove_all(root);
  const auto folder = root / "whisper" / "Release";
  std::filesystem::create_directories(folder);
  CHECK(!FindWhisper({root.string()}).has_value());
#ifdef _WIN32
  const auto tool = folder / "whisper-cli.exe";
#else
  const auto tool = folder / "whisper-cli";
#endif
  { std::ofstream(tool.string()) << "x"; }
  CHECK(!FindWhisper({root.string()}).has_value());   // a tool with no model is not an engine
  { std::ofstream((root / "whisper" / "ggml-tiny.en.bin").string()) << "x"; }
  { std::ofstream((root / "whisper" / "ggml-base.en.bin").string()) << "x"; }
  const auto found = FindWhisper({root.string()});
  CHECK(found.has_value());
  CHECK(std::filesystem::path(found->executable).filename() == tool.filename());
  CHECK(std::filesystem::path(found->model).filename() == "ggml-base.en.bin");   // the same choice every time
  const auto explicit_model = (root / "whisper" / "ggml-tiny.en.bin").string();
  CHECK(FindWhisper({}, tool.string(), explicit_model)->model == explicit_model);
  CHECK(!FindWhisper({}, (folder / "nothing.exe").string()).has_value());
  const auto args = WhisperArguments(*found, "in file.wav", "out prefix", {"de", 4});
  CHECK(args[0] == found->executable);
  bool language = false, threads = false;
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    language = language || (args[i] == "-l" && args[i + 1] == "de");
    threads = threads || (args[i] == "-t" && args[i + 1] == "4");
  }
  CHECK(language && threads);
  std::filesystem::remove_all(root);
}

CUTLINE_TEST(ARecordedSentenceIsTranscribedByTheRealEngineAndFollowsTheWordsSpoken) {
  const auto root = RepositoryRoot();
  const auto tools = root.empty() ? std::nullopt : FindWhisper({root.string()});
  SKIP_INAPPLICABLE(tools.has_value(), "whisper.cpp and a model are not installed under .tools/whisper");
  cutline::media::RegisterAllProviders();
  const auto sample = (root / "tests" / "golden" / "speech.wav").string();
  auto source = cutline::media::SourceRegistry::Instance().Open(sample);
  SKIP_UNLESS(source != nullptr, "no media provider can open the sample");
  const auto wav = Scratch("engine-input.wav").string();
  const auto written = WriteSpeechWav(*source, cutline::time::RationalTime(0, 1), 11.0, wav);
  CHECK(written.has_value() && *written == 11 * 16000);
  // What was written is a 16 kHz mono 16-bit WAV with sizes that match.
  CHECK_EQ(std::filesystem::file_size(wav), static_cast<std::uintmax_t>(44 + *written * 2));

  double last_progress = 0.0;
  const auto result = Transcribe(*tools, wav, Scratch("engine-output").string(), {"en", 0}, {}, [&](double p) { last_progress = p; });
  CHECK(!result.cancelled);
  CHECK(!std::filesystem::exists(Scratch("engine-output").string() + ".json"));   // the engine's file is cleaned up
  CHECK(last_progress == 1.0);
  const auto& transcript = result.transcript;
  CHECK(transcript.words.size() >= 20);
  // The sentence: "Welcome to the show. Um, today we are going to talk about editing video. ..."
  const auto welcome = Find(transcript, "welcome to the show");
  CHECK_EQ(welcome.size(), std::size_t{1});
  CHECK(transcript.words[welcome[0].first].start < 1.0);
  const auto editing = Find(transcript, "editing video");
  CHECK_EQ(editing.size(), std::size_t{1});
  const double at = transcript.words[editing[0].first].start;
  CHECK(at > 3.5 && at < 6.0);   // spoken about five seconds in
  CHECK(!Find(transcript, "thank you for watching").empty());
  for (std::size_t i = 1; i < transcript.words.size(); ++i) CHECK(transcript.words[i].start >= transcript.words[i - 1].start);

  // A run that is cancelled ends the engine and says so.
  const auto stopped = Transcribe(*tools, wav, Scratch("engine-stop").string(), {"en", 0}, [] { return true; });
  CHECK(stopped.cancelled && stopped.transcript.words.empty());
}

int main() { return cutline::testing::RunAll("speech"); }

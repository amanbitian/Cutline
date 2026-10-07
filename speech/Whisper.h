#pragma once

// Speech to text with whisper.cpp, run on this machine.
//
// Cutline does not link an engine. It writes the media's speech as a 16 kHz mono WAV and runs whisper.cpp's own
// command-line tool on it as a child process, then reads the JSON the tool writes (speech/Transcript.h). That keeps
// the engine replaceable (a newer build, a larger model, a GPU build of the same tool, all just files on disk),
// keeps a crash in the engine from taking the editor with it, and lets a transcription be cancelled by ending the
// process. Nothing is sent anywhere: the model is a file, found with the tool, and the tool is run without a network.
//
// What the engine is honest about. whisper.cpp writes what it can hear as clean text: a small model often leaves
// out the "um"s and false starts that text-based editing most wants to cut, a larger model keeps more of them. Word
// times come from the model's token timestamps and are good to a tenth of a second or so, not to a frame; they
// are a guide to where a word is, and a cut made from them may need a nudge. It does not tell speakers apart.

#include "core/time/RationalTime.h"
#include "media/Source.h"
#include "speech/Transcript.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cutline::speech {

struct WhisperTools final {
  std::string executable;  // whisper-cli(.exe)
  std::string model;       // a ggml-*.bin model file
};

// Looks for the tool and a model. `executable` and `model` are explicit choices (from preferences) and win when they
// name files that exist; otherwise each of `roots` is searched for a "whisper" folder (and a "Release" folder inside
// it, and the same under ".tools") holding whisper-cli, and the model beside it, in its "models" folder, or one level
// up. Nothing is returned unless both are found.
[[nodiscard]] std::optional<WhisperTools> FindWhisper(const std::vector<std::string>& roots, const std::string& executable = {}, const std::string& model = {});

// Writes `seconds` of the source's sound from `start` (source time) as 16-bit mono PCM at 16 kHz, which is what the
// engine reads. Returns the number of sample frames written, or nothing if `cancelled` ended it. Throws
// std::runtime_error when the file cannot be written or the source has no sound.
[[nodiscard]] std::optional<std::int64_t> WriteSpeechWav(media::Source& source, const time::RationalTime& start, double seconds, const std::string& path,
                                                         const std::function<bool()>& cancelled = {}, const std::function<void(double)>& progress = {});

struct WhisperOptions final {
  // A language code ("en", "de"), or "auto" to let the model decide (a model trained on English only has none to
  // decide among).
  std::string language{"auto"};
  // Threads for the engine; zero leaves it to choose.
  int threads{0};
};

struct WhisperResult final {
  bool cancelled{false};
  Transcript transcript;
};

// Transcribes a WAV file. `progress` gets 0 to 1 as the engine reports it. Throws std::runtime_error with the engine's
// own message when it fails or cannot be started. The work is a child process, ended at once when `cancelled` returns
// true. `json_path_prefix` is where the tool's output goes (it adds ".json"); the file is removed afterwards.
[[nodiscard]] WhisperResult Transcribe(const WhisperTools& tools, const std::string& wav_path, const std::string& json_path_prefix, const WhisperOptions& options = {},
                                       const std::function<bool()>& cancelled = {}, const std::function<void(double)>& progress = {});

// What is needed to run: the full command line the tool is started with, for the log and for tests.
[[nodiscard]] std::vector<std::string> WhisperArguments(const WhisperTools& tools, const std::string& wav_path, const std::string& json_path_prefix, const WhisperOptions& options);

}  // namespace cutline::speech

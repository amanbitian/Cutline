#include "speech/Whisper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cutline::speech {
namespace fs = std::filesystem;
namespace {

bool IsFile(const fs::path& path) {
  std::error_code ignored;
  return fs::is_regular_file(path, ignored);
}

#ifdef _WIN32
constexpr const char* kToolName = "whisper-cli.exe";
#else
constexpr const char* kToolName = "whisper-cli";
#endif

std::optional<std::string> FirstModelIn(const fs::path& directory) {
  std::error_code ignored;
  if (!fs::is_directory(directory, ignored)) return std::nullopt;
  std::vector<std::string> found;
  for (const auto& entry : fs::directory_iterator(directory, ignored)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file(ignored) && name.rfind("ggml-", 0) == 0 && entry.path().extension() == ".bin") found.push_back(entry.path().string());
  }
  if (found.empty()) return std::nullopt;
  std::sort(found.begin(), found.end());
  return found.front();
}

std::optional<std::string> ModelNear(const fs::path& executable_directory) {
  for (const auto& directory : {executable_directory, executable_directory / "models", executable_directory.parent_path(), executable_directory.parent_path() / "models"}) {
    if (auto model = FirstModelIn(directory)) return model;
  }
  return std::nullopt;
}

}  // namespace

std::optional<WhisperTools> FindWhisper(const std::vector<std::string>& roots, const std::string& executable, const std::string& model) {
  if (!executable.empty() && IsFile(executable)) {
    std::string chosen_model = IsFile(model) ? model : std::string();
    if (chosen_model.empty()) {
      if (auto beside = ModelNear(fs::path(executable).parent_path())) chosen_model = *beside;
    }
    if (chosen_model.empty()) return std::nullopt;
    return WhisperTools{executable, chosen_model};
  }
  for (const auto& root : roots) {
    for (const auto& base : {fs::path(root), fs::path(root) / ".tools"}) {
      for (const auto& directory : {base / "whisper", base / "whisper" / "Release", base / "whisper" / "bin"}) {
        const auto tool = directory / kToolName;
        if (!IsFile(tool)) continue;
        std::string chosen_model = IsFile(model) ? model : std::string();
        if (chosen_model.empty()) {
          if (auto beside = ModelNear(directory)) chosen_model = *beside;
        }
        if (!chosen_model.empty()) return WhisperTools{tool.string(), chosen_model};
      }
    }
  }
  return std::nullopt;
}

// ----------------------------------------------------------------------------------- audio ----

namespace {

void PutU16(std::ofstream& out, std::uint16_t value) {
  const char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff)};
  out.write(bytes, 2);
}
void PutU32(std::ofstream& out, std::uint32_t value) {
  const char bytes[4] = {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff), static_cast<char>((value >> 16) & 0xff), static_cast<char>((value >> 24) & 0xff)};
  out.write(bytes, 4);
}

constexpr std::int64_t kSpeechRate = 16000;

}  // namespace

std::optional<std::int64_t> WriteSpeechWav(media::Source& source, const time::RationalTime& start, double seconds, const std::string& path,
                                           const std::function<bool()>& cancelled, const std::function<void(double)>& progress) {
  if (source.probe().PrimaryAudio() == nullptr) throw std::runtime_error("The media has no sound to transcribe");
  const auto total = static_cast<std::int64_t>(std::llround(std::max(seconds, 0.0) * static_cast<double>(kSpeechRate)));
  if (total <= 0) throw std::runtime_error("There is no sound in the range to transcribe");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("The audio file for the engine could not be written: " + path);
  // The header, with sizes filled in at the end.
  out.write("RIFF", 4);
  PutU32(out, 0);
  out.write("WAVEfmt ", 8);
  PutU32(out, 16);
  PutU16(out, 1);  // PCM
  PutU16(out, 1);  // mono
  PutU32(out, static_cast<std::uint32_t>(kSpeechRate));
  PutU32(out, static_cast<std::uint32_t>(kSpeechRate * 2));
  PutU16(out, 2);
  PutU16(out, 16);
  out.write("data", 4);
  PutU32(out, 0);

  constexpr std::int64_t kBlock = kSpeechRate * 10;
  std::vector<std::int16_t> pcm;
  std::int64_t written = 0;
  while (written < total) {
    if (cancelled && cancelled()) return std::nullopt;
    const auto frames = std::min(kBlock, total - written);
    const auto at = start.Add(time::RationalTime(written, kSpeechRate));
    const auto block = source.ReadAudio(at, kSpeechRate, 1, frames);
    if (!block || !block->valid()) throw std::runtime_error("The sound of the media could not be read");
    const float* samples = block->channel(0);
    pcm.resize(static_cast<std::size_t>(frames));
    for (std::int64_t i = 0; i < frames; ++i) {
      const float clipped = std::clamp(samples[i], -1.0f, 1.0f);
      pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lrintf(clipped * 32767.0f));
    }
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
    written += frames;
    if (progress) progress(static_cast<double>(written) / static_cast<double>(total));
  }
  const auto data_bytes = static_cast<std::uint32_t>(written * 2);
  out.seekp(4);
  PutU32(out, 36 + data_bytes);
  out.seekp(40);
  PutU32(out, data_bytes);
  out.flush();
  if (!out) throw std::runtime_error("The audio file for the engine could not be written: " + path);
  return written;
}

// ----------------------------------------------------------------------------------- engine ----

std::vector<std::string> WhisperArguments(const WhisperTools& tools, const std::string& wav_path, const std::string& json_path_prefix, const WhisperOptions& options) {
  std::vector<std::string> args{tools.executable, "-m", tools.model, "-f", wav_path, "-ojf", "-of", json_path_prefix, "-ml", "1", "-sow", "-pp", "-np", "-l",
                                options.language.empty() ? "auto" : options.language};
  if (options.threads > 0) {
    args.push_back("-t");
    args.push_back(std::to_string(options.threads));
  }
  return args;
}

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

// One argument, quoted the way CommandLineToArgvW reads it back.
std::wstring Quoted(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (auto it = arg.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(*it);
    }
  }
  out.push_back(L'"');
  return out;
}

struct Handle final {
  HANDLE h{nullptr};
  Handle() = default;
  explicit Handle(HANDLE handle) : h(handle) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { Close(); }
  void Close() {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = nullptr;
  }
};

// Reads the percentage from lines like "whisper_print_progress_callback: progress =  45%".
void ScanProgress(std::string& pending, const std::function<void(double)>& progress, std::string& tail) {
  std::size_t newline;
  while ((newline = pending.find('\n')) != std::string::npos) {
    const auto line = pending.substr(0, newline);
    pending.erase(0, newline + 1);
    if (line.find("progress") != std::string::npos) {
      const auto equals = line.find('=');
      if (equals != std::string::npos && progress) {
        const int percent = std::atoi(line.c_str() + equals + 1);
        if (percent >= 0 && percent <= 100) progress(percent / 100.0);
      }
    } else if (!line.empty()) {
      tail = line;
      if (tail.size() > 300) tail.resize(300);
    }
  }
}

}  // namespace

WhisperResult Transcribe(const WhisperTools& tools, const std::string& wav_path, const std::string& json_path_prefix, const WhisperOptions& options,
                         const std::function<bool()>& cancelled, const std::function<void(double)>& progress) {
  const auto args = WhisperArguments(tools, wav_path, json_path_prefix, options);
  std::wstring command;
  for (const auto& arg : args) command += (command.empty() ? L"" : L" ") + Quoted(Wide(arg));

  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  Handle read_end, write_end;
  if (!CreatePipe(&read_end.h, &write_end.h, &security, 0)) throw std::runtime_error("The speech engine could not be started (no pipe)");
  SetHandleInformation(read_end.h, HANDLE_FLAG_INHERIT, 0);
  Handle null_input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_input.h;
  startup.hStdOutput = write_end.h;
  startup.hStdError = write_end.h;
  PROCESS_INFORMATION info{};
  const auto directory = Wide(fs::path(tools.executable).parent_path().string());
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &info)) {
    throw std::runtime_error("The speech engine could not be started: " + tools.executable);
  }
  Handle process(info.hProcess), thread(info.hThread);
  write_end.Close();

  std::string pending, tail;
  bool was_cancelled = false;
  for (;;) {
    DWORD available = 0;
    if (PeekNamedPipe(read_end.h, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
      char buffer[4096];
      DWORD got = 0;
      if (ReadFile(read_end.h, buffer, static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), available)), &got, nullptr) && got > 0) {
        pending.append(buffer, got);
        ScanProgress(pending, progress, tail);
      }
      continue;
    }
    if (cancelled && cancelled()) {
      TerminateProcess(process.h, 1);
      was_cancelled = true;
      break;
    }
    if (WaitForSingleObject(process.h, 40) == WAIT_OBJECT_0) {
      // Whatever it wrote last.
      DWORD left = 0;
      while (PeekNamedPipe(read_end.h, nullptr, 0, nullptr, &left, nullptr) && left > 0) {
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(read_end.h, buffer, static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), left)), &got, nullptr) || got == 0) break;
        pending.append(buffer, got);
      }
      pending.push_back('\n');
      ScanProgress(pending, progress, tail);
      break;
    }
  }
  WaitForSingleObject(process.h, 5000);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.h, &exit_code);

  const auto json_path = json_path_prefix + ".json";
  std::error_code ignored;
  WhisperResult result;
  if (was_cancelled) {
    result.cancelled = true;
    fs::remove(json_path, ignored);
    return result;
  }
  if (exit_code != 0 || !IsFile(json_path)) {
    fs::remove(json_path, ignored);
    throw std::runtime_error("The speech engine failed" + (tail.empty() ? std::string() : ": " + tail));
  }
  std::ifstream in(json_path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  in.close();
  fs::remove(json_path, ignored);
  result.transcript = ParseWhisperJson(text.str());
  if (progress) progress(1.0);
  return result;
}

#else

WhisperResult Transcribe(const WhisperTools&, const std::string&, const std::string&, const WhisperOptions&, const std::function<bool()>&, const std::function<void(double)>&) {
  throw std::runtime_error("Running the speech engine is supported on Windows only in this build");
}

#endif

}  // namespace cutline::speech

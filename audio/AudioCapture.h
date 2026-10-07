#pragma once

// Recording from an input device (a microphone) to a file: what a voice-over needs.
//
// The device is read in shared mode through WASAPI, converted by the system to the format asked for (32-bit float at the
// sequence's sample rate), and written to a WAV file as it arrives, so a take that is stopped by a crash or a full disk
// keeps what was recorded and a long take costs no memory. The level of the last moments is published for a meter. The
// reading thread does nothing but copy: no allocation, no lock, no decoding.
//
// Not here: hearing yourself while recording (input monitoring) and latency compensation against what is playing; a take
// starts when the device delivers its first block, which is not the instant the button was pressed.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cutline::audio {

struct InputDevice final {
  std::string id;    // the endpoint id, to open exactly this device
  std::string name;
  bool is_default{false};
};

// The microphones and other capture endpoints that are working now. Empty where the platform has no capture support.
[[nodiscard]] std::vector<InputDevice> ListInputDevices();

struct RecordingResult final {
  std::int64_t frames{0};     // sample frames written
  double peak_db{-200.0};     // the highest sample of the take
  bool ok{false};             // a complete file was written
  std::string error;
};

class Recorder {
 public:
  virtual ~Recorder() = default;

  // Starts a take into `wav_path` (the file is created, the folder must exist). `device_id` empty means the default input.
  // Returns nothing, with `error` saying why, if the device cannot be opened in that format.
  [[nodiscard]] static std::unique_ptr<Recorder> Start(const std::string& device_id, const std::string& wav_path, std::int64_t sample_rate, int channels,
                                                       std::string* error);

  // The highest sample of the last ~100 ms, in decibels (-200 for silence), and frames written so far.
  [[nodiscard]] virtual double level_db() const = 0;
  [[nodiscard]] virtual std::int64_t frames() const = 0;
  // Whether the device failed after starting (unplugged): the take so far is kept.
  [[nodiscard]] virtual bool failed() const = 0;
  // Ends the take, finishes the file and says what was recorded. Safe to call once.
  virtual RecordingResult Stop() = 0;
};

}  // namespace cutline::audio

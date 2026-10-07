#include "audio/AudioCapture.h"

#ifdef CUTLINE_HAVE_WASAPI

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

namespace cutline::audio {
namespace {

// PKEY_Device_FriendlyName, written out so that the property-key headers are not needed.
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string Narrow(const wchar_t* wide) {
  if (wide == nullptr) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

template <typename T>
struct ComPtr {
  T* p{nullptr};
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ~ComPtr() { if (p != nullptr) p->Release(); }
  T** operator&() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

void PutU16(std::ofstream& out, std::uint16_t v) { const char b[2] = {static_cast<char>(v & 0xff), static_cast<char>(v >> 8)}; out.write(b, 2); }
void PutU32(std::ofstream& out, std::uint32_t v) {
  const char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff), static_cast<char>((v >> 16) & 0xff), static_cast<char>(v >> 24)};
  out.write(b, 4);
}

class WasapiRecorder final : public Recorder {
 public:
  ~WasapiRecorder() override {
    if (!stopped_) (void)Stop();
  }

  bool Open(const std::string& device_id, const std::string& wav_path, std::int64_t sample_rate, int channels, std::string* error) {
    rate_ = sample_rate;
    channels_ = std::max(channels, 1);
    path_ = wav_path;
    out_.open(wav_path, std::ios::binary | std::ios::trunc);
    if (!out_) {
      if (error != nullptr) *error = "The recording file could not be created: " + wav_path;
      return false;
    }
    WriteHeader(0);
    std::mutex gate;
    std::condition_variable_any ready_signal;
    bool ready = false;
    thread_ = std::thread([&, device_id] {
      Run(device_id, [&](bool ok, const std::string& why) {
        std::lock_guard<std::mutex> lock(gate);
        opened_ = ok;
        problem_ = why;
        ready = true;
        ready_signal.notify_all();
      });
    });
    {
      std::unique_lock<std::mutex> lock(gate);
      ready_signal.wait(lock, [&] { return ready; });
    }
    if (!opened_) {
      if (thread_.joinable()) thread_.join();
      out_.close();
      std::remove(wav_path.c_str());
      if (error != nullptr) *error = problem_;
      stopped_ = true;
      return false;
    }
    return true;
  }

  [[nodiscard]] double level_db() const override {
    const double peak = level_.load(std::memory_order_relaxed);
    return peak <= 1e-10 ? -200.0 : 20.0 * std::log10(peak);
  }
  [[nodiscard]] std::int64_t frames() const override { return frames_.load(std::memory_order_relaxed); }
  [[nodiscard]] bool failed() const override { return failed_.load(std::memory_order_relaxed); }

  RecordingResult Stop() override {
    RecordingResult result;
    if (stopped_) return result;
    stopped_ = true;
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    result.frames = frames_.load();
    result.peak_db = take_peak_ <= 1e-10 ? -200.0 : 20.0 * std::log10(take_peak_);
    if (out_.is_open()) {
      WriteHeader(static_cast<std::uint32_t>(result.frames * channels_ * 4));
      out_.flush();
      result.ok = static_cast<bool>(out_);
      out_.close();
    }
    if (!result.ok) result.error = "The recording file could not be finished";
    if (failed_.load() && result.frames > 0) result.error = "The input device stopped; the take so far was kept";
    return result;
  }

 private:
  void WriteHeader(std::uint32_t data_bytes) {
    out_.seekp(0);
    out_.write("RIFF", 4);
    PutU32(out_, 36 + data_bytes);
    out_.write("WAVEfmt ", 8);
    PutU32(out_, 16);
    PutU16(out_, 3);  // IEEE float
    PutU16(out_, static_cast<std::uint16_t>(channels_));
    PutU32(out_, static_cast<std::uint32_t>(rate_));
    PutU32(out_, static_cast<std::uint32_t>(rate_ * channels_ * 4));
    PutU16(out_, static_cast<std::uint16_t>(channels_ * 4));
    PutU16(out_, 32);
    out_.write("data", 4);
    PutU32(out_, data_bytes);
  }

  template <typename Ready>
  void Run(const std::string& device_id, Ready&& ready) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    {
      ComPtr<IMMDeviceEnumerator> enumerator;
      if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
        ready(false, "Windows audio is not available");
      } else {
        ComPtr<IMMDevice> device;
        const HRESULT found = device_id.empty() ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device) : enumerator->GetDevice(Wide(device_id).c_str(), &device);
        ComPtr<IAudioClient> client;
        if (FAILED(found) || !device) {
          ready(false, device_id.empty() ? "There is no input device (microphone)" : "That input device is not available");
        } else if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
          ready(false, "The input device could not be opened");
        } else {
          WAVEFORMATEXTENSIBLE format{};
          format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
          format.Format.nChannels = static_cast<WORD>(channels_);
          format.Format.nSamplesPerSec = static_cast<DWORD>(rate_);
          format.Format.wBitsPerSample = 32;
          format.Format.nBlockAlign = static_cast<WORD>(channels_ * 4);
          format.Format.nAvgBytesPerSec = static_cast<DWORD>(rate_ * channels_ * 4);
          format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
          format.Samples.wValidBitsPerSample = 32;
          format.dwChannelMask = channels_ == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
          format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
          // The system converts rate, channel count and sample type from what the device delivers.
          const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
          HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          ComPtr<IAudioCaptureClient> capture;
          if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &format.Format, nullptr))) {
            ready(false, "The input device does not accept this recording format");
          } else if (FAILED(client->SetEventHandle(event)) || FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) || FAILED(client->Start())) {
            ready(false, "The input device could not be started");
          } else {
            ready(true, {});
            Loop(*capture.p, event);
            client->Stop();
          }
          if (event != nullptr) CloseHandle(event);
        }
      }
    }
    if (com) CoUninitialize();
  }

  void Loop(IAudioCaptureClient& capture, HANDLE event) {
    std::vector<float> silence;
    while (!stop_.load(std::memory_order_acquire)) {
      const DWORD waited = WaitForSingleObject(event, 100);
      if (waited == WAIT_FAILED) {
        failed_.store(true);
        return;
      }
      for (;;) {
        UINT32 available = 0;
        if (FAILED(capture.GetNextPacketSize(&available))) {
          failed_.store(true);
          return;
        }
        if (available == 0) break;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture.GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
          failed_.store(true);
          return;
        }
        const auto count = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_);
        const float* samples = reinterpret_cast<const float*>(data);
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
          silence.assign(count, 0.0f);
          samples = silence.data();
        }
        float peak = 0.0f;
        for (std::size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(samples[i]));
        out_.write(reinterpret_cast<const char*>(samples), static_cast<std::streamsize>(count * sizeof(float)));
        frames_.fetch_add(frames, std::memory_order_relaxed);
        take_peak_ = std::max<double>(take_peak_, peak);
        // A meter that falls: the last peak decays unless a louder one arrives.
        const double held = level_.load(std::memory_order_relaxed) * 0.9;
        level_.store(std::max<double>(peak, held), std::memory_order_relaxed);
        capture.ReleaseBuffer(frames);
      }
    }
  }

  std::int64_t rate_{48000};
  int channels_{2};
  std::string path_;
  std::ofstream out_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::atomic<std::int64_t> frames_{0};
  std::atomic<double> level_{0.0};
  double take_peak_{0.0};
  bool stopped_{false};
  bool opened_{false};
  std::string problem_;
};

}  // namespace

std::vector<InputDevice> ListInputDevices() {
  std::vector<InputDevice> devices;
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
      std::string default_id;
      {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) && device) {
          LPWSTR id = nullptr;
          if (SUCCEEDED(device->GetId(&id))) {
            default_id = Narrow(id);
            CoTaskMemFree(id);
          }
        }
      }
      ComPtr<IMMDeviceCollection> collection;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
          ComPtr<IMMDevice> device;
          if (FAILED(collection->Item(i, &device)) || !device) continue;
          InputDevice entry;
          LPWSTR id = nullptr;
          if (SUCCEEDED(device->GetId(&id))) {
            entry.id = Narrow(id);
            CoTaskMemFree(id);
          }
          ComPtr<IPropertyStore> store;
          if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(store->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR) entry.name = Narrow(name.pwszVal);
            PropVariantClear(&name);
          }
          entry.is_default = !default_id.empty() && entry.id == default_id;
          devices.push_back(std::move(entry));
        }
      }
    }
  }
  if (com) CoUninitialize();
  return devices;
}

std::unique_ptr<Recorder> Recorder::Start(const std::string& device_id, const std::string& wav_path, std::int64_t sample_rate, int channels, std::string* error) {
  auto recorder = std::make_unique<WasapiRecorder>();
  if (!recorder->Open(device_id, wav_path, sample_rate, channels, error)) return nullptr;
  return recorder;
}

}  // namespace cutline::audio

#else

namespace cutline::audio {
std::vector<InputDevice> ListInputDevices() { return {}; }
std::unique_ptr<Recorder> Recorder::Start(const std::string&, const std::string&, std::int64_t, int, std::string* error) {
  if (error != nullptr) *error = "Recording is supported on Windows only in this build";
  return nullptr;
}
}  // namespace cutline::audio

#endif

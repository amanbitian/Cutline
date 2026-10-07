// WASAPI shared-mode output.
//
// The structure here is the important part, not the API calls.
//
// A device callback is a realtime deadline: miss it and the user hears a click.
// The mixer cannot run there -- it allocates, it decodes, it may touch the
// database. So there are two threads:
//
//   producer  -- runs the mixer, writes interleaved frames into a lock-free ring
//   device    -- woken by WASAPI's event, copies from the ring into the device
//                buffer and does nothing else
//
// The device thread allocates nothing, takes no lock, and performs no I/O. When
// the ring is empty it writes silence and counts an underrun, which is the
// honest measurable definition of a dropout and is exposed for the UI to show.
//
// The master clock is advanced on the device thread by the number of frames
// actually handed to the device, because that is what becomes audible.
//
// Three more things this file is responsible for, because a seek, the end of the
// timeline and a failing device all cross the two threads:
//
//   * Flush. The ring has one writer and one reader, so neither side may reset it
//     on the other's behalf. A flush is a handshake: the caller raises a request;
//     the producer, at the top of its loop, records where its write position is
//     (everything before it is stale) and acknowledges; the device thread, which
//     owns the read position, then moves it to that mark, resets the device's own
//     buffer and the clock, and publishes completion. Until it has done that the
//     device thread plays silence, so stale audio is never heard after the call.
//     A block that was being rendered while the request arrived is dropped, not
//     written: it was made for the old position.
//   * End of stream. When the callback says it has no more, the producer stops; the
//     device thread plays out what is queued and the sink reports Finished once
//     it has been heard. Nothing keeps rendering silence into the ring.
//   * Failure. An exception from the callback, or a device call that fails, ends
//     the threads cleanly and is published as state Failed with a message. Neither
//     can escape a thread, where it would terminate the process.

#ifdef CUTLINE_HAVE_WASAPI

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "audio/AudioSink.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cutline::audio {
namespace {

// A single-producer single-consumer ring of interleaved float frames. Lock-free
// by construction: the producer only advances the write index, the consumer only
// the read index, and both are atomics with acquire/release ordering.
class FrameRing final {
 public:
  FrameRing(int channels, std::int64_t capacity_frames)
      : channels_(channels),
        capacity_(capacity_frames),
        samples_(static_cast<std::size_t>(capacity_frames) * channels) {}

  [[nodiscard]] std::int64_t ReadableFrames() const {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::int64_t WritableFrames() const { return capacity_ - ReadableFrames(); }
  // Where the next frame will be written. Producer side.
  [[nodiscard]] std::int64_t WritePosition() const { return write_.load(std::memory_order_acquire); }

  // Producer side. Returns false when there is not room for the whole block,
  // which keeps a partially written block from ever being readable.
  bool Write(const float* interleaved, std::int64_t frames) {
    if (WritableFrames() < frames) return false;
    const auto start = write_.load(std::memory_order_relaxed);
    for (std::int64_t frame = 0; frame < frames; ++frame) {
      const auto slot = static_cast<std::size_t>((start + frame) % capacity_) * channels_;
      std::memcpy(samples_.data() + slot, interleaved + frame * channels_,
                  static_cast<std::size_t>(channels_) * sizeof(float));
    }
    write_.store(start + frames, std::memory_order_release);
    return true;
  }

  // Consumer side. Fills what it can and reports how many frames were real; the
  // caller zeroes the remainder.
  [[nodiscard]] std::int64_t Read(float* interleaved, std::int64_t frames) {
    const auto available = std::min(frames, ReadableFrames());
    const auto start = read_.load(std::memory_order_relaxed);
    for (std::int64_t frame = 0; frame < available; ++frame) {
      const auto slot = static_cast<std::size_t>((start + frame) % capacity_) * channels_;
      std::memcpy(interleaved + frame * channels_, samples_.data() + slot,
                  static_cast<std::size_t>(channels_) * sizeof(float));
    }
    read_.store(start + available, std::memory_order_release);
    return available;
  }

  // Consumer side: gives up everything written before `position`.
  void SkipTo(std::int64_t position) {
    const auto current = read_.load(std::memory_order_relaxed);
    if (position > current) read_.store(position, std::memory_order_release);
  }

  // Only valid while neither thread is running.
  void Reset() {
    read_.store(0, std::memory_order_release);
    write_.store(0, std::memory_order_release);
  }

 private:
  int channels_;
  std::int64_t capacity_;
  std::vector<float> samples_;
  std::atomic<std::int64_t> read_{0};
  std::atomic<std::int64_t> write_{0};
};

template <typename T>
void SafeRelease(T*& pointer) {
  if (pointer != nullptr) {
    pointer->Release();
    pointer = nullptr;
  }
}

[[nodiscard]] std::string HResultText(HRESULT result) {
  char buffer[16]{};
  std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(result));
  return std::string(buffer);
}

void Check(HRESULT result, const char* context) {
  if (FAILED(result)) {
    throw std::runtime_error(std::string(context) + " failed (hresult 0x" + HResultText(result) + ")");
  }
}

class WasapiSink final : public AudioSink {
 public:
  explicit WasapiSink(AudioDeviceFormat requested) : clock_(requested.sample_rate) {
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
    com_initialised_ = true;

    IMMDeviceEnumerator* enumerator = nullptr;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                           reinterpret_cast<void**>(&enumerator)),
          "CoCreateInstance(MMDeviceEnumerator)");
    const auto release_enumerator = [&] { SafeRelease(enumerator); };

    IMMDevice* device = nullptr;
    const auto endpoint = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    release_enumerator();
    Check(endpoint, "GetDefaultAudioEndpoint");

    const auto activate = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&client_));
    SafeRelease(device);
    Check(activate, "IMMDevice::Activate");

    // Shared mode runs at the device's mix format. Converting to it is cheaper
    // and far more compatible than demanding exclusive mode, which many devices
    // refuse and which stops every other application playing sound.
    WAVEFORMATEX* mix = nullptr;
    Check(client_->GetMixFormat(&mix), "GetMixFormat");
    format_.sample_rate = mix->nSamplesPerSec;
    format_.channels = mix->nChannels;
    device_is_float_ = IsFloatFormat(mix);
    device_bytes_per_sample_ = mix->wBitsPerSample / 8;

    // A 200 ms buffer: long enough that a scheduling hiccup on the producer
    // thread does not reach the ear, short enough that transport response still
    // feels immediate.
    constexpr REFERENCE_TIME kBufferDuration = 2'000'000;  // 100 ns units
    const auto initialised =
        client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0, mix,
                            nullptr);
    CoTaskMemFree(mix);
    Check(initialised, "IAudioClient::Initialize");

    UINT32 buffer_frames = 0;
    Check(client_->GetBufferSize(&buffer_frames), "GetBufferSize");
    device_buffer_frames_ = buffer_frames;
    format_.block_frames = requested.block_frames > 0 ? requested.block_frames : buffer_frames / 4;

    Check(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_)),
          "GetService(IAudioRenderClient)");

    ready_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ready_event_ == nullptr) throw std::runtime_error("Unable to create the audio device event");
    Check(client_->SetEventHandle(ready_event_), "SetEventHandle");

    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(client_->GetStreamLatency(&latency))) {
      // 100 ns units, plus the buffer we keep ahead of the device.
      latency_ = time::RationalTime(latency, 10'000'000)
                     .Add(time::RationalTime(device_buffer_frames_, format_.sample_rate));
    } else {
      latency_ = time::RationalTime(device_buffer_frames_, format_.sample_rate);
    }

    clock_.Reconfigure(format_.sample_rate);
    ring_ = std::make_unique<FrameRing>(format_.channels, device_buffer_frames_ * 4);
    device_scratch_.assign(static_cast<std::size_t>(device_buffer_frames_) * format_.channels, 0.0f);
  }

  ~WasapiSink() override {
    Stop();
    SafeRelease(render_);
    SafeRelease(client_);
    if (ready_event_ != nullptr) CloseHandle(ready_event_);
    if (com_initialised_) CoUninitialize();
  }

  [[nodiscard]] std::string name() const override { return "wasapi"; }
  [[nodiscard]] const AudioDeviceFormat& format() const override { return format_; }
  [[nodiscard]] bool running() const override { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] AudioClock& clock() override { return clock_; }
  [[nodiscard]] time::RationalTime latency() const override { return latency_; }
  [[nodiscard]] std::int64_t underruns() const override { return underruns_.load(std::memory_order_relaxed); }

  [[nodiscard]] SinkState state() const override {
    if (!running()) return SinkState::Stopped;
    if (failed_.load(std::memory_order_acquire)) return SinkState::Failed;
    if (finished_.load(std::memory_order_acquire)) return SinkState::Finished;
    return SinkState::Running;
  }

  [[nodiscard]] std::string error() const override {
    if (!failed_.load(std::memory_order_acquire)) return {};
    const std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
  }

  void Start(RenderCallback callback) override {
    if (!callback) throw std::invalid_argument("An audio sink needs a render callback");
    if (running()) return;
    callback_ = std::move(callback);
    ring_->Reset();
    underruns_.store(0, std::memory_order_relaxed);
    producer_done_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    {
      const std::lock_guard<std::mutex> lock(error_mutex_);
      error_.clear();
    }
    flush_requested_.store(0, std::memory_order_relaxed);
    flush_acknowledged_.store(0, std::memory_order_relaxed);
    flush_applied_.store(0, std::memory_order_relaxed);
    flush_mark_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    producer_ = std::thread([this] { ProducerLoop(); });
    device_ = std::thread([this] { DeviceLoop(); });
  }

  void Stop() override {
    if (!running()) return;
    running_.store(false, std::memory_order_release);
    SetEvent(ready_event_);
    if (producer_.joinable()) producer_.join();
    if (device_.joinable()) device_.join();
    callback_ = nullptr;
  }

  void Flush() override {
    if (!running()) {
      clock_.Reset();
      return;
    }
    const auto request = flush_requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
    SetEvent(ready_event_);
    // Wait for the device thread to have moved the read position and reset the
    // device. Bounded, so a wedged device cannot freeze the caller.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (flush_applied_.load(std::memory_order_acquire) < request && running() &&
           !failed_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      SetEvent(ready_event_);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

 private:
  [[nodiscard]] static bool IsFloatFormat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
      return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
  }

  // Records the first failure and tells both threads to stop. Callable from
  // either thread.
  void Fail(const std::string& message) {
    {
      const std::lock_guard<std::mutex> lock(error_mutex_);
      if (!failed_.load(std::memory_order_relaxed)) error_ = message;
    }
    failed_.store(true, std::memory_order_release);
    SetEvent(ready_event_);
  }

  [[nodiscard]] bool Stopping() const { return !running() || failed_.load(std::memory_order_acquire); }

  // Runs the mixer and keeps the ring topped up. Allocation and blocking are
  // fine here; this thread has no deadline beyond staying ahead of the device.
  void ProducerLoop() {
    try {
      auto block = media::AudioBuffer::Allocate(format_.sample_rate, format_.channels, format_.block_frames);
      std::vector<float> interleaved(static_cast<std::size_t>(format_.block_frames) * format_.channels);
      std::uint64_t seen = 0;

      while (!Stopping()) {
        // A flush request is answered before anything else, even with a full ring:
        // the position recorded here is the line between stale and new audio.
        const auto requested = flush_requested_.load(std::memory_order_acquire);
        if (requested != seen) {
          flush_mark_.store(ring_->WritePosition(), std::memory_order_release);
          seen = requested;
          flush_acknowledged_.store(requested, std::memory_order_release);
          SetEvent(ready_event_);
        }

        if (producer_done_.load(std::memory_order_relaxed)) {
          // Nothing more to make. Stay alive to answer a flush, otherwise idle.
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (ring_->WritableFrames() < format_.block_frames) {
          // The ring is full, so the device is comfortably supplied. Sleeping for
          // a fraction of a block keeps this thread off a spin.
          std::this_thread::sleep_for(std::chrono::microseconds(500));
          continue;
        }

        block.Silence();
        const auto result = callback_ != nullptr ? callback_(block) : BlockResult::End;
        const bool produced = result == BlockResult::Audio;

        // The callback says the block was made for a position the transport has
        // since left; and a flush that arrived while it was being rendered means
        // the same thing. Either way the block is dropped rather than played.
        if (result == BlockResult::Dropped || flush_requested_.load(std::memory_order_acquire) != seen) continue;

        for (std::int64_t frame = 0; frame < block.frames(); ++frame) {
          for (int channel = 0; channel < format_.channels; ++channel) {
            interleaved[static_cast<std::size_t>(frame) * format_.channels + channel] =
                block.channel(channel)[frame];
          }
        }
        ring_->Write(interleaved.data(), block.frames());
        if (!produced) producer_done_.store(true, std::memory_order_release);
      }
    } catch (const std::exception& error) {
      Fail(std::string("the render callback failed: ") + error.what());
    } catch (...) {
      Fail("the render callback failed");
    }
  }

  // Performs a flush on the device side once the producer has marked its position.
  // Returns true while a flush is still waiting for the producer.
  [[nodiscard]] bool ServiceFlush() {
    const auto requested = flush_requested_.load(std::memory_order_acquire);
    if (requested == flush_applied_.load(std::memory_order_relaxed)) return false;
    if (flush_acknowledged_.load(std::memory_order_acquire) != requested) return true;

    ring_->SkipTo(flush_mark_.load(std::memory_order_acquire));
    // What the device already holds is stale too.
    client_->Stop();
    client_->Reset();
    client_->Start();
    clock_.Reset();
    flush_applied_.store(requested, std::memory_order_release);
    return false;
  }

  // The realtime path. No allocation, no locks, no I/O.
  void DeviceLoop() {
    // Ask the OS for realtime-ish scheduling; failure is not fatal, it just
    // makes dropouts more likely under load.
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    if (const auto started = client_->Start(); FAILED(started)) {
      Fail("the audio device would not start (hresult 0x" + HResultText(started) + ")");
      if (task != nullptr) AvRevertMmThreadCharacteristics(task);
      return;
    }

    while (!Stopping()) {
      if (WaitForSingleObject(ready_event_, 200) != WAIT_OBJECT_0) {
        if (!Stopping() && producer_done_.load(std::memory_order_acquire) && ring_->ReadableFrames() == 0) {
          finished_.store(true, std::memory_order_release);
        }
        continue;
      }
      if (Stopping()) break;
      const bool flush_pending = ServiceFlush();

      UINT32 padding = 0;
      if (const auto result = client_->GetCurrentPadding(&padding); FAILED(result)) {
        Fail("the audio device was lost (GetCurrentPadding, hresult 0x" + HResultText(result) + ")");
        break;
      }
      const auto available = static_cast<std::int64_t>(device_buffer_frames_) - padding;
      if (available <= 0) continue;

      BYTE* target = nullptr;
      if (const auto result = render_->GetBuffer(static_cast<UINT32>(available), &target); FAILED(result)) {
        Fail("the audio device was lost (GetBuffer, hresult 0x" + HResultText(result) + ")");
        break;
      }

      // While a flush is waiting for the producer the ring still holds stale audio:
      // play silence rather than the old position.
      const auto supplied = flush_pending ? 0 : ring_->Read(device_scratch_.data(), available);
      if (supplied < available) {
        // Underrun: zero the tail rather than repeating stale audio, and record
        // it. Repeating would be a louder artefact than a brief silence.
        std::memset(device_scratch_.data() + supplied * format_.channels, 0,
                    static_cast<std::size_t>((available - supplied) * format_.channels) * sizeof(float));
        // Silence after the end of the timeline, or during a flush, is not a dropout.
        if (!flush_pending && !producer_done_.load(std::memory_order_acquire)) {
          underruns_.fetch_add(available - supplied, std::memory_order_relaxed);
        }
      }

      WriteToDevice(target, available);
      render_->ReleaseBuffer(static_cast<UINT32>(available), 0);

      // The clock follows what the device accepted, which is the definition of
      // playback position everything else chases.
      clock_.AdvanceFromAudioCallback(available);

      if (producer_done_.load(std::memory_order_acquire) && ring_->ReadableFrames() == 0 && supplied == 0) {
        finished_.store(true, std::memory_order_release);
      }
    }

    client_->Stop();
    if (task != nullptr) AvRevertMmThreadCharacteristics(task);
  }

  void WriteToDevice(BYTE* target, std::int64_t frames) {
    const auto samples = static_cast<std::size_t>(frames) * format_.channels;
    if (device_is_float_) {
      std::memcpy(target, device_scratch_.data(), samples * sizeof(float));
      return;
    }
    // The only common non-float shared-mode format.
    if (device_bytes_per_sample_ == 2) {
      auto* output = reinterpret_cast<std::int16_t*>(target);
      for (std::size_t index = 0; index < samples; ++index) {
        const auto value = std::clamp(device_scratch_[index], -1.0f, 1.0f);
        output[index] = static_cast<std::int16_t>(value * 32767.0f);
      }
      return;
    }
    std::memset(target, 0, samples * static_cast<std::size_t>(device_bytes_per_sample_));
  }

  AudioDeviceFormat format_{};
  AudioClock clock_;
  time::RationalTime latency_;

  IAudioClient* client_{nullptr};
  IAudioRenderClient* render_{nullptr};
  HANDLE ready_event_{nullptr};
  bool com_initialised_{false};
  bool device_is_float_{true};
  int device_bytes_per_sample_{4};
  std::int64_t device_buffer_frames_{0};

  std::unique_ptr<FrameRing> ring_;
  std::vector<float> device_scratch_;
  RenderCallback callback_;

  std::thread producer_;
  std::thread device_;
  std::atomic<bool> running_{false};
  std::atomic<bool> producer_done_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> failed_{false};
  std::atomic<std::int64_t> underruns_{0};
  mutable std::mutex error_mutex_;
  std::string error_;

  // Flush handshake: requested by the caller, acknowledged by the producer with the
  // ring position that separates stale from new, applied by the device thread.
  std::atomic<std::uint64_t> flush_requested_{0};
  std::atomic<std::uint64_t> flush_acknowledged_{0};
  std::atomic<std::uint64_t> flush_applied_{0};
  std::atomic<std::int64_t> flush_mark_{0};
};

}  // namespace

std::unique_ptr<AudioSink> OpenDefaultAudioSink(AudioDeviceFormat format) {
  try {
    return std::make_unique<WasapiSink>(format);
  } catch (const std::exception&) {
    // No device, no driver, or a session that cannot open one. Falling back to
    // an offline sink means the editor still opens and still edits; only
    // monitoring is silent.
    return std::make_unique<OfflineSink>(format);
  }
}

bool HasAudioDevice() { return true; }

}  // namespace cutline::audio

#endif  // CUTLINE_HAVE_WASAPI

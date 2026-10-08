#include "exporter/ExportWorker.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace cutline::exporter {
namespace {

struct ExportPacket final {
  std::optional<media::VideoFrame> video;
  std::optional<media::AudioBuffer> audio;
};

// One producer overlaps decode/composition with encoding. One queued packet bounds
// memory independently of programme duration; the writer stays on the calling thread.
class FramePipeline final {
 public:
  FramePipeline(std::int64_t count, std::function<ExportPacket(std::int64_t)> render)
      : worker_([this, count, render = std::move(render)] {
          try {
            for (std::int64_t i = 0; i < count; ++i) {
              {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || ready_.empty(); });
                if (stop_) return;
              }
              auto packet = render(i);
              {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (stop_) return;
                ready_.push_back(std::move(packet));
              }
              wake_.notify_all();
            }
          } catch (...) {
            const std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
          }
          {
            const std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
          }
          wake_.notify_all();
        }) {}

  ~FramePipeline() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
  }

  ExportPacket Next() {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [this] { return !ready_.empty() || done_; });
    if (ready_.empty()) {
      if (error_) std::rethrow_exception(error_);
      throw std::runtime_error("Export renderer ended before the requested range");
    }
    auto packet = std::move(ready_.front());
    ready_.pop_front();
    lock.unlock();
    wake_.notify_all();
    return packet;
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<ExportPacket> ready_;
  std::exception_ptr error_;
  bool stop_{false};
  bool done_{false};
  std::thread worker_;
};

// Sample index at the start of video frame `index`, derived from absolute time.
//
// Accumulating a per-frame sample count instead would drift: at 48 kHz against
// 30000/1001 a frame is 1601.6 samples, and rounding each one independently
// loses roughly a sample every two or three frames, which is a frame of
// desynchronisation every few minutes of programme.
[[nodiscard]] std::int64_t SampleAt(std::int64_t frame_index, const time::FrameRate& rate,
                                    std::int64_t sample_rate) {
  const time::RationalTime moment(frame_index * rate.denominator, rate.numerator);
  return moment.Rescale(sample_rate, time::RoundingMode::Nearest);
}

}  // namespace

ExportRequest ResolveRequest(const playback::PlaybackEngine& engine, ExportRequest request) {
  const auto* sequence = engine.graph()->root();
  if (sequence == nullptr) throw std::invalid_argument("The engine has no sequence to export");

  if (request.video.width <= 0) request.video.width = sequence->width;
  if (request.video.height <= 0) request.video.height = sequence->height;
  if (request.video.frame_rate.numerator <= 0) request.video.frame_rate = sequence->frame_rate;
  request.video.pixel_aspect = sequence->pixel_aspect;
  // The sequence's display space is what the monitor was showing, so it is what
  // the file should declare.
  if (request.video.color_primaries == "bt709" && sequence->display_color_space != "rec709") {
    request.video.color_primaries = sequence->display_color_space;
  }

  if (request.audio.sample_rate <= 0) request.audio.sample_rate = sequence->sample_rate;
  if (request.audio.channels <= 0) {
    request.audio.channels = static_cast<int>(media::ChannelCountForLayout(sequence->channel_layout));
  }

  if (request.out.Compare({0, 1}) <= 0) request.out = sequence->Duration();
  return request;
}

ExportResult Export(playback::PlaybackEngine& engine, const ExportRequest& raw_request,
                    const ProgressCallback& progress) {
  // Freeze the graph and use originals at sequence size even when the caller's
  // monitor uses reduced resolution, proxies, or changes during export.
  const bool wants_precision = raw_request.high_precision && raw_request.include_video && engine.output_format() == media::PixelFormat::Rgba8;
  auto originals = engine.ExportClone(wants_precision ? media::PixelFormat::Rgba16 : engine.output_format());
  auto& render_engine = *originals;
  const auto request = ResolveRequest(render_engine, raw_request);
  if (request.output_path.empty()) throw std::invalid_argument("An export needs an output path");
  if (!request.include_video && !request.include_audio) {
    throw std::invalid_argument("An export must include video, audio, or both");
  }
  if (request.out.Compare(request.in) <= 0) {
    throw std::invalid_argument("The export range is empty");
  }

  const auto* sequence = render_engine.graph()->root();
  if (request.include_video &&
      (request.video.width != sequence->width || request.video.height != sequence->height)) {
    // Rendering at another size needs its own render context (the engine's
    // compositor is sized to the sequence, and effect offsets are authored in
    // sequence pixels). Until that exists, say so before any file is created,
    // rather than failing on the first frame with a file already half-made.
    throw std::invalid_argument("Exporting at a frame size other than the sequence's (" +
                                std::to_string(sequence->width) + "x" + std::to_string(sequence->height) +
                                ") is not supported yet; requested " + std::to_string(request.video.width) + "x" +
                                std::to_string(request.video.height));
  }

  const auto rate = request.video.frame_rate;
  if (rate.numerator <= 0 || rate.denominator <= 0) throw std::invalid_argument("The export frame rate is invalid");
  const time::RationalTime frame_duration(rate.denominator, rate.numerator);

  // Frames are counted, not stepped to, so the last partial frame of a range
  // that does not divide evenly is still rendered.
  const auto span = request.out.Subtract(request.in);
  const auto total_frames = span.Divide(frame_duration).ToFrames({1, 1}, time::RoundingMode::Ceil);

  media::ExportSettings settings;
  settings.path = request.output_path;
  settings.overwrite = request.overwrite;
  settings.container = request.container;
  settings.container_options = request.container_options;
  if (request.include_video) settings.video = request.video;
  if (request.include_audio) settings.audio = request.audio;
  auto writer = media::WriterRegistry::Instance().Open(settings);

  // One policy for picture and sound. Export renders every track unless asked
  // otherwise; the engine's own compile options are left untouched so the
  // monitor is unaffected.
  auto options = render_engine.compile_options();
  options.honour_mute_and_solo = request.honour_mute_and_solo;
  options.include_captions = request.burn_in_captions;
  // A delivery is rendered for quality: retimed audio that keeps its pitch uses the
  // offline stretch parameters.
  options.high_quality_audio = true;

  // Audio is rendered in the engine's own format and the writer converts it to
  // the requested rate and layout. Sizing blocks for the requested rate while the
  // engine rendered at its own is what played a 44.1 kHz export a semitone flat.
  const auto engine_rate = render_engine.audio_sample_rate();
  // Where the export starts on the timeline's sample grid. Every block is placed by
  // counting samples from here, not by rounding its own start time, so two blocks
  // can never disagree about the sample they share an edge at.
  const auto in_sample = request.in.Rescale(engine_rate, time::RoundingMode::Nearest);

  FramePipeline pipeline(total_frames, [&](std::int64_t index) {
    ExportPacket packet;
    const auto at = request.in.Add(frame_duration.Multiply(index));
    if (request.include_video) packet.video = render_engine.RenderFrame(at, options);
    if (request.include_audio) {
      const auto from = SampleAt(index, rate, engine_rate);
      const auto until = SampleAt(index + 1, rate, engine_rate);
      packet.audio = render_engine.RenderAudioAt(in_sample + from, until - from, options);
    }
    return packet;
  });

  ExportResult result;
  for (std::int64_t index = 0; index < total_frames; ++index) {
    const auto at = request.in.Add(frame_duration.Multiply(index));

    auto packet = pipeline.Next();
    if (packet.video) writer->WriteVideo(*packet.video);
    if (packet.audio) writer->WriteAudio(*packet.audio);

    result.video_frames = writer->video_frames_written();
    result.audio_frames = writer->audio_frames_written();

    if (progress) {
      const ExportProgress reported{index + 1, total_frames, at.Add(frame_duration)};
      if (!progress(reported)) {
        // The writer's destructor discards its temporary file. The destination
        // is never touched until an export completes, so cancelling cannot
        // destroy a previous delivery.
        result.cancelled = true;
        return result;
      }
    }
  }

  writer->Finish();
  result.video_frames = writer->video_frames_written();
  result.audio_frames = writer->audio_frames_written();
  result.duration = frame_duration.Multiply(total_frames);
  return result;
}

}  // namespace cutline::exporter

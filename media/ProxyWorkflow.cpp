#include "media/ProxyWorkflow.h"

#include "core/db/Sql.h"
#include "media/Ingest.h"
#include "media/Source.h"
#include "media/VideoFrame.h"
#include "media/Writer.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cutline::media {
namespace {

VideoFrame Resize(const VideoFrame& input, int width, int height) {
  auto source = input.format() == PixelFormat::Rgba8 ? input.Clone() : ConvertFrame(input, PixelFormat::Rgba8);
  if (source.width() == width && source.height() == height) return source;
  auto output = VideoFrame::Allocate(PixelFormat::Rgba8, width, height);
  output.presentation_time = input.presentation_time;
  output.duration = input.duration;
  output.color = input.color;
  output.pixel_aspect = input.pixel_aspect;
  output.keyframe = input.keyframe;

  // Bilinear scaling is deterministic and adequate for edit proxies. The
  // expensive high-quality resize remains an export concern.
  const double sx = static_cast<double>(source.width()) / static_cast<double>(width);
  const double sy = static_cast<double>(source.height()) / static_cast<double>(height);
  for (int y = 0; y < height; ++y) {
    const double source_y = std::max(0.0, (static_cast<double>(y) + 0.5) * sy - 0.5);
    const int y0 = std::min(static_cast<int>(source_y), source.height() - 1);
    const int y1 = std::min(y0 + 1, source.height() - 1);
    const double fy = source_y - static_cast<double>(y0);
    auto* out = output.row_u8(y);
    for (int x = 0; x < width; ++x) {
      const double source_x = std::max(0.0, (static_cast<double>(x) + 0.5) * sx - 0.5);
      const int x0 = std::min(static_cast<int>(source_x), source.width() - 1);
      const int x1 = std::min(x0 + 1, source.width() - 1);
      const double fx = source_x - static_cast<double>(x0);
      const auto* row0 = source.row_u8(y0);
      const auto* row1 = source.row_u8(y1);
      for (int channel = 0; channel < 4; ++channel) {
        const double top = row0[x0 * 4 + channel] * (1.0 - fx) + row0[x1 * 4 + channel] * fx;
        const double bottom = row1[x0 * 4 + channel] * (1.0 - fx) + row1[x1 * 4 + channel] * fx;
        out[x * 4 + channel] = static_cast<std::uint8_t>(std::clamp(std::lround(top * (1.0 - fy) + bottom * fy), 0l, 255l));
      }
    }
  }
  return output;
}

std::pair<int, int> ProxySize(int source_width, int source_height, const ProxyPreset& preset) {
  if (preset.max_width <= 0 || preset.max_height <= 0) throw std::invalid_argument("Proxy limits must be positive");
  const auto scale = std::min({1.0, static_cast<double>(preset.max_width) / source_width,
                               static_cast<double>(preset.max_height) / source_height});
  auto width = std::max(2, static_cast<int>(std::floor(source_width * scale)));
  auto height = std::max(2, static_cast<int>(std::floor(source_height * scale)));
  width -= width % 2;
  height -= height % 2;
  return {width, height};
}

}  // namespace

ProxyDecision ChooseProxyPreset(const ProxyComplexity& media, const PlaybackHealth& playback) {
  if (media.width <= 0 || media.height <= 0 || media.frames_per_second <= 0.0 || media.bit_depth <= 0 ||
      playback.frame_budget_ms <= 0.0 || !std::isfinite(media.frames_per_second) ||
      !std::isfinite(playback.average_decode_ms) || !std::isfinite(playback.frame_budget_ms)) {
    throw std::invalid_argument("Proxy automation needs valid media and playback measurements");
  }
  const auto pixels = static_cast<double>(media.width) * static_cast<double>(media.height);
  auto score = pixels / (1920.0 * 1080.0);
  score *= std::max(1.0, media.frames_per_second / 30.0);
  if (media.bit_depth > 8) score *= 1.35;
  std::string codec = media.codec;
  std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (codec.find("hevc") != std::string::npos || codec.find("h265") != std::string::npos) score *= 1.35;
  if (codec.find("av1") != std::string::npos) score *= 1.55;
  if (media.intra_only) score *= 0.7;

  const auto miss_rate = playback.requested_frames == 0 ? 0.0 :
      static_cast<double>(playback.deadline_misses) / static_cast<double>(playback.requested_frames);
  const auto decode_ratio = playback.average_decode_ms <= 0.0 ? 0.0 :
      playback.average_decode_ms / playback.frame_budget_ms;
  const bool pressured = playback.memory_pressure != resource::Pressure::Normal;

  ProxyDecision decision;
  decision.complexity_score = score;
  decision.miss_rate = miss_rate;
  decision.generate = score >= 1.5 || miss_rate >= 0.03 || decode_ratio >= 0.8 || pressured;
  if (!decision.generate) {
    decision.reason = "Original media is within the measured playback budget";
    return decision;
  }

  const bool severe = score >= 5.0 || miss_rate >= 0.12 || decode_ratio >= 1.5 ||
                      playback.memory_pressure == resource::Pressure::Critical;
  const bool heavy = severe || score >= 2.5 || miss_rate >= 0.06 || decode_ratio >= 1.0 ||
                     playback.memory_pressure == resource::Pressure::Elevated;
  if (severe) {
    decision.preset.max_width = 960;
    decision.preset.max_height = 540;
    decision.preset.video_bitrate = 18'000'000;
  } else if (heavy) {
    decision.preset.max_width = 1280;
    decision.preset.max_height = 720;
    decision.preset.video_bitrate = 30'000'000;
  } else {
    decision.preset.max_width = 1920;
    decision.preset.max_height = 1080;
    decision.preset.video_bitrate = 60'000'000;
  }
  decision.reason = miss_rate >= 0.03 || decode_ratio >= 0.8
                        ? "Measured playback misses require a lighter local proxy"
                        : pressured ? "Memory pressure requires a smaller local proxy"
                                    : "Media complexity exceeds the real-time original-media threshold";
  return decision;
}

ProxyRecord FindProxy(const project::ProjectStore& store, const std::string& media_id) {
  if (media_id.empty()) throw std::invalid_argument("Proxy lookup needs a media id");
  const std::lock_guard<std::mutex> lock(store.mutex());
  db::Statement statement(store.connection(), R"sql(
    SELECT p.media_id, p.path, p.fingerprint, p.source_fingerprint, p.codec, p.width, p.height,
           m.fingerprint
      FROM media m LEFT JOIN media_proxies p ON p.media_id = m.id
     WHERE m.id = ?;
  )sql");
  statement.Bind(1, media_id);
  if (!statement.Step() || statement.ColumnText(0).empty()) return {};
  ProxyRecord record;
  record.media_id = statement.ColumnText(0);
  record.path = statement.ColumnText(1);
  record.fingerprint = statement.ColumnText(2);
  record.source_fingerprint = statement.ColumnText(3);
  record.codec = statement.ColumnText(4);
  record.width = statement.ColumnInt(5);
  record.height = statement.ColumnInt(6);
  const auto current_source = statement.ColumnText(7);
  if (record.source_fingerprint != current_source) {
    record.status = ProxyStatus::Stale;
  } else {
    std::error_code error;
    record.status = std::filesystem::is_regular_file(record.path, error) ? ProxyStatus::Ready : ProxyStatus::Missing;
  }
  return record;
}

playback::MediaLocator ProjectProxyLocator(const project::ProjectStore& store) {
  struct Verification final {
    std::string path;
    std::string fingerprint;
    std::uintmax_t size{};
    std::filesystem::file_time_type written{};
    bool valid{false};
  };
  struct VerificationCache final {
    std::mutex mutex;
    std::unordered_map<std::string, Verification> entries;
  };
  auto cache = std::make_shared<VerificationCache>();
  return [&store, cache](const std::string& media_id) {
    const auto proxy = FindProxy(store, media_id);
    if (proxy.status != ProxyStatus::Ready) return std::string();
    std::error_code error;
    const auto size = std::filesystem::file_size(proxy.path, error);
    if (error) return std::string();
    const auto written = std::filesystem::last_write_time(proxy.path, error);
    if (error) return std::string();

    const std::lock_guard<std::mutex> lock(cache->mutex);
    auto& verified = cache->entries[media_id];
    if (verified.path != proxy.path || verified.fingerprint != proxy.fingerprint || verified.size != size ||
        verified.written != written) {
      verified = {proxy.path, proxy.fingerprint, size, written, false};
      try {
        verified.valid = FingerprintFile(proxy.path) == proxy.fingerprint;
      } catch (const std::exception&) {
        verified.valid = false;
      }
    }
    return verified.valid ? proxy.path : std::string();
  };
}

ProxyGenerationResult GenerateProxy(const ProxyGenerationRequest& request, const ProxyProgress& progress,
                                    const ProxyCancel& cancel) {
  if (request.source_path.empty() || request.output_path.empty()) {
    throw std::invalid_argument("Proxy generation needs source and output paths");
  }
  if (request.source_fingerprint.empty()) throw std::invalid_argument("Proxy generation needs a source fingerprint");

  auto source = SourceRegistry::Instance().Open(request.source_path);
  if (source == nullptr) throw std::runtime_error("No local media provider can open " + request.source_path);
  const auto& probe = source->probe();
  const auto* video = probe.PrimaryVideo();
  if (video == nullptr) throw std::runtime_error("Cannot make a proxy from media with no video stream");
  const auto [width, height] = ProxySize(static_cast<int>(video->width), static_cast<int>(video->height), request.preset);
  const time::RationalTime frame_duration(video->frame_rate.denominator, video->frame_rate.numerator);
  const auto total = probe.duration.Divide(frame_duration).ToFrames({1, 1}, time::RoundingMode::Ceil);

  ExportSettings settings;
  settings.path = request.output_path;
  settings.overwrite = true;
  VideoEncoderSettings video_settings;
  video_settings.codec = request.preset.video_codec;
  video_settings.width = width;
  video_settings.height = height;
  video_settings.frame_rate = video->frame_rate;
  video_settings.pixel_format = request.preset.pixel_format;
  video_settings.bitrate = request.preset.video_bitrate;
  video_settings.gop_size = 1;
  video_settings.color_primaries = video->color_primaries;
  video_settings.color_transfer = video->color_transfer;
  video_settings.color_matrix = video->color_matrix;
  video_settings.color_range = video->color_range;
  settings.video = video_settings;

  const auto* audio = probe.PrimaryAudio();
  if (audio != nullptr) {
    AudioEncoderSettings audio_settings;
    audio_settings.codec = request.preset.audio_codec;
    audio_settings.sample_rate = audio->sample_rate;
    audio_settings.channels = static_cast<int>(std::min<std::int64_t>(audio->channel_count, 2));
    settings.audio = audio_settings;
  }

  auto writer = WriterRegistry::Instance().Open(settings);
  for (std::int64_t index = 0; index < total; ++index) {
    if (cancel && cancel()) return {request.output_path, {}, request.source_fingerprint, request.preset.video_codec,
                                    width, height, writer->video_frames_written(), true};
    const auto at = frame_duration.Multiply(index);
    auto frame = source->ReadVideo(at);
    if (frame.has_value()) writer->WriteVideo(Resize(*frame, width, height));
    if (audio != nullptr) {
      const auto first = at.Rescale(audio->sample_rate, time::RoundingMode::Nearest);
      const auto next = at.Add(frame_duration).Rescale(audio->sample_rate, time::RoundingMode::Nearest);
      if (auto block = source->ReadAudio(at, audio->sample_rate,
                                         static_cast<int>(std::min<std::int64_t>(audio->channel_count, 2)),
                                         next - first)) {
        writer->WriteAudio(*block);
      }
    }
    if (progress) progress({index + 1, total});
  }
  writer->Finish();

  ProxyGenerationResult result;
  result.path = request.output_path;
  result.fingerprint = FingerprintFile(request.output_path);
  result.source_fingerprint = request.source_fingerprint;
  result.codec = request.preset.video_codec;
  result.width = width;
  result.height = height;
  result.video_frames = writer->video_frames_written();
  return result;
}

ProxyJob::ProxyJob(ProxyGenerationRequest request, ProxyProgress progress)
    : cancel_(std::make_shared<std::atomic_bool>(false)),
      future_(std::async(std::launch::async, [request = std::move(request), progress = std::move(progress),
                                             cancel = cancel_] {
        return GenerateProxy(request, progress, [cancel] { return cancel->load(std::memory_order_relaxed); });
      })) {}

ProxyJob::~ProxyJob() {
  Cancel();
  if (future_.valid()) future_.wait();
}

ProxyJob::ProxyJob(ProxyJob&&) noexcept = default;
ProxyJob& ProxyJob::operator=(ProxyJob&&) noexcept = default;
void ProxyJob::Cancel() { if (cancel_) cancel_->store(true, std::memory_order_relaxed); }
bool ProxyJob::Ready() const {
  return future_.valid() && future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
ProxyGenerationResult ProxyJob::Get() { return future_.get(); }

project::CommandResult AttachGeneratedProxy(project::ProjectStore& store,
                                            const AttachGeneratedProxyRequest& request,
                                            const ProxyGenerationResult& generated) {
  if (generated.cancelled) throw std::invalid_argument("A cancelled proxy cannot be attached");
  commands::AttachProxyPayload payload;
  payload.media_id = request.media_id;
  payload.path = generated.path;
  payload.fingerprint = generated.fingerprint;
  payload.source_fingerprint = generated.source_fingerprint;
  payload.codec = generated.codec;
  payload.width = generated.width;
  payload.height = generated.height;
  commands::CommandEnvelope command;
  command.command_id = request.command_id;
  command.project_id = request.project_id;
  command.author_id = request.author_id;
  command.base_revision = request.base_revision;
  command.timestamp_utc = request.timestamp_utc;
  command.type = commands::CommandType::AttachProxy;
  command.payload = std::move(payload);
  command.idempotency_key = command.command_id;
  return store.Execute(command);
}

}  // namespace cutline::media

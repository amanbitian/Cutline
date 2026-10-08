#pragma once

// The program monitor.
//
// Ties the pieces together: compile a plan for a time, decode what it names,
// composite the picture, mix the sound. Everything below it stays a pure
// function; this is the layer that owns mutable state -- open decoders, a frame
// cache, the transport position.
//
// Synchronisation follows the audio clock. The sink pulls audio blocks and
// advances the clock by what the device accepted; the picture is then rendered
// for whatever time that clock reports, minus the device's output latency. The
// alternative -- driving both from a wall clock -- drifts, because the sound
// card's rate is nobody's to adjust.
//
// Ownership and threads. The engine is used from three places at once while it
// plays: the sink's producer thread renders audio, a UI thread renders pictures,
// and edits arrive from wherever the user's command ran. State is divided so that
// none of them waits on another's rendering:
//
//   * the sequence is an immutable snapshot, swapped by UpdateSequence and read by
//     taking a reference to the current one; a render in progress finishes on the
//     snapshot it started with;
//   * the picture path (frame cache, nested composites, the compositor's workspace)
//     is guarded by video_mutex_; RenderFrame serialises its callers, while a
//     decode worker holds it only long enough to publish one raw frame;
//   * the audio path belongs to the audio renders, likewise;
//   * open decoders are shared, and are safe to share because a media::Source lets
//     one thread read its picture while another reads its sound; the table that
//     holds them has a short lock that is never held across a decode;
//   * read-ahead workers own independent Source instances, so two video readers
//     never share seek or codec state;
//   * the transport (cursor, origin, generation) has its own lock, and a seek
//     bumps a generation so that a block rendered for the old position is
//     recognised and dropped instead of overwriting the new one.
//
// There is no lock around "all rendering". Two threads rendering pictures at once
// take turns (an export should use an engine of its own); a picture and a block of
// audio never wait for each other.

#include "audio/AudioMixer.h"
#include "audio/AudioSink.h"
#include "media/Source.h"
#include "playback/DecodePool.h"
#include "render/Compositor.h"
#include "render/D3D11Compositor.h"
#include "render/RenderCache.h"
#include "timeline/Sequence.h"
#include "timeline/TimelineCompiler.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace cutline::playback {

// Resolves a media id to something a provider can open: a file path, or a
// synthetic: URI. Returning an empty string means the media is offline, which
// renders as a gap rather than an error.
using MediaLocator = std::function<std::string(const std::string& media_id)>;

struct EngineConfig final {
  std::int64_t sample_rate{48000};
  int channels{2};
  // Decoded frames are held to a byte budget rather than a frame count. A frame
  // count cannot be chosen sensibly: 32 frames is 8 MB at SD and over a
  // gigabyte at 4K, so the same setting is either useless or ruinous depending
  // on the footage. 256 MB holds plenty of HD frames and stays bounded at 8K.
  std::size_t frame_cache_bytes{256u * 1024u * 1024u};
  // Independent video decoders fill a bounded read-ahead window. Set either
  // value to zero to disable background decoding (useful for export and tests
  // that need fully synchronous behaviour).
  std::size_t decode_workers{2};
  std::size_t read_ahead_frames{8};
  std::size_t max_pending_decodes{64};
  // Per worker, so decoder memory stays bounded even after traversing a project
  // with thousands of media items.
  std::size_t worker_source_limit{8};
  render::CompositorConfig compositor;
  timeline::CompileOptions compile;
  // Rendered pictures, kept by what they are made from (render/RenderCache.h). Shared, so a monitor
  // and an export can read each other's work. Empty: every frame is composed afresh.
  std::shared_ptr<render::RenderCache> render_cache;
  // Monitor-only optimisation. Export engines leave this false, so deliveries
  // always decode the camera originals. An empty or unavailable proxy falls
  // back to `MediaLocator` without turning the clip offline.
  bool prefer_proxies{false};
  MediaLocator proxy_locator;
  // Compose on the GPU (render/D3D11Compositor.h) the frames it can render exactly as the software compositor would; every
  // other frame, and every frame on a machine without a usable adapter, is composed in software. A monitor turns this
  // on; an export leaves it off, because a delivery is rendered by the reference path.
  bool use_gpu{false};
  // The adapter to use (an id from D3D11Compositor::EnumerateDevices); empty for the best one.
  std::string gpu_adapter_id;
  // Allow Direct3D's software rasteriser when there is no hardware adapter. Only for machines and tests that want it.
  bool gpu_allow_software{false};
  // With the GPU compositor, decode on the GPU too (Direct3D 11 video acceleration) and composite the pictures where the
  // decoder left them. A stream the hardware cannot decode is decoded in software and uploaded.
  bool hardware_decode{true};
};

struct EngineStatistics final {
  std::int64_t frames_rendered{0};
  // Pictures served from the render cache without composing, and pictures composed and put into it.
  std::int64_t render_cache_hits{0};
  std::int64_t render_cache_stores{0};
  std::int64_t audio_blocks_mixed{0};
  std::int64_t cache_hits{0};
  std::int64_t cache_misses{0};
  std::int64_t cache_evictions{0};
  std::int64_t decode_failures{0};
  std::int64_t offline_media{0};
  std::int64_t read_ahead_queued{0};
  std::int64_t read_ahead_completed{0};
  std::int64_t read_ahead_cache_hits{0};
  std::int64_t read_ahead_queue_full{0};
  std::int64_t total_frame_render_microseconds{0};
  std::int64_t max_frame_render_microseconds{0};
  // Blocks whose sum exceeded full scale and was clamped, and clips whose source
  // gave no audio (offline, or without an audio stream) while they were due to sound.
  std::int64_t audio_blocks_clipped{0};
  std::int64_t audio_missing_sources{0};
  // Blocks the sink asked for that were made for a position the transport had left
  // and were dropped.
  std::int64_t audio_blocks_dropped{0};
  // The GPU compositor: frames it composed, frames it handed back to the software compositor (something in the plan it
  // does not render, or a colour conversion it does not do), frames lost to a device error (after which the engine stays
  // in software), time spent, and pictures decoded by hardware and composed without leaving the device.
  std::int64_t gpu_frames{0};
  std::int64_t gpu_fallback_frames{0};
  std::int64_t gpu_failures{0};
  std::int64_t gpu_microseconds{0};
  std::int64_t gpu_device_microseconds{0};
  std::int64_t hardware_pictures{0};
  std::int64_t uploaded_pictures{0};
};

class PlaybackEngine final {
 public:
  PlaybackEngine(timeline::SequenceGraph graph, MediaLocator locator, EngineConfig config = {});
  ~PlaybackEngine();
  PlaybackEngine(const PlaybackEngine&) = delete;
  PlaybackEngine& operator=(const PlaybackEngine&) = delete;

  // Renders the picture at a timeline position. Safe to call repeatedly for the
  // same time; the frame cache makes that cheap.
  [[nodiscard]] media::VideoFrame RenderFrame(const time::RationalTime& at);
  // Renders every frame of [in, out) that the render cache does not already hold, so that playing
  // the range afterwards is reading it. Frames already cached are skipped. `cancel` is polled between
  // frames and `progress` told how many are done of how many.
  struct PreRenderResult final {
    std::int64_t rendered{0};
    std::int64_t already_cached{0};
    bool cancelled{false};
  };
  [[nodiscard]] PreRenderResult PreRender(const time::RationalTime& in, const time::RationalTime& out,
                                          const std::function<bool()>& cancel = {},
                                          const std::function<void(std::int64_t, std::int64_t)>& progress = {});
  // Which frames of the range are in the cache now, one entry per frame of the sequence.
  [[nodiscard]] std::vector<bool> CachedFrames(const time::RationalTime& in, std::int64_t count);
  // Renders with different compile options, which is how export ignores mute
  // and solo without changing what the monitor does.
  [[nodiscard]] media::VideoFrame RenderFrame(const time::RationalTime& at,
                                              const timeline::CompileOptions& options);

  // Mixes `frames` of audio starting at a timeline position, in the engine's
  // own sample rate and channel count.
  [[nodiscard]] media::AudioBuffer RenderAudio(const time::RationalTime& at, std::int64_t frames);
  // The same, under explicit compile options. Picture and sound must be rendered
  // under one policy: export supplied its mute/solo choice to the picture only,
  // so a muted audio track exported as silence even when every track was asked for.
  [[nodiscard]] media::AudioBuffer RenderAudio(const time::RationalTime& at, std::int64_t frames,
                                               const timeline::CompileOptions& options);
  // The same, from an absolute sample position on the timeline (a count of
  // samples at audio_sample_rate()). A caller that tiles a long render into blocks
  // should count samples and use this, so that no two blocks can disagree about
  // where one ends and the next begins.
  [[nodiscard]] media::AudioBuffer RenderAudioAt(std::int64_t first_sample, std::int64_t frames,
                                                 const timeline::CompileOptions& options);

  // The format RenderAudio produces. An export reads this to size its blocks and
  // lets the writer convert, rather than assuming the two agree.
  [[nodiscard]] std::int64_t audio_sample_rate() const noexcept { return config_.sample_rate; }
  [[nodiscard]] int audio_channels() const noexcept { return config_.channels; }

  // Replaces the snapshot after an edit. The frame cache is dropped because the
  // edit may have changed what any clip resolves to. Open decoders are kept only
  // while the locator still gives their media the path they were opened from: a
  // relink closes and reopens them, and media that was offline is tried again.
  // Renders in progress finish on the snapshot they began with.
  // Keep raw pictures and device decoders for edits that cannot change source media.
  // Relink, proxy changes, undo/redo and external updates use the conservative default.
  void UpdateSequence(timeline::SequenceGraph graph, bool invalidate_media = true);

  // Renders pictures at this size from now on (0 by 0: the sequence's own). A monitor at reduced resolution asks for
  // less work this way; the render cache keeps pictures of each size apart, since the size is part of what a picture is.
  void SetOutputSize(int width, int height);

  // ------------------------------------------------------------- transport ----

  // Begins pulling audio from `sink`, which drives the clock. Picture is not
  // pushed anywhere: a caller renders at `position()` when it is ready to
  // display, which is what keeps a dropped video frame from stalling audio.
  // The sound is rendered at the sink's own rate and channel count, whatever the
  // engine's configuration says.
  void Play(audio::AudioSink& sink);
  void Pause();
  // Moves the playhead. Audio already queued for the old position is discarded, so
  // the new one is heard at once; a block being rendered for the old position when
  // this is called is dropped. After the end of the timeline has been reached and
  // played out, a seek restarts playback from the new position.
  void Seek(const time::RationalTime& to);
  [[nodiscard]] bool playing() const;

  // What the audio device is doing: Running, Finished once the timeline's end has
  // been played, or Failed with a reason. A UI polls this; nothing here throws on
  // a device error.
  [[nodiscard]] audio::SinkState playback_state() const;
  [[nodiscard]] std::string playback_error() const;

  // Current timeline position, compensated for device latency so the picture
  // matches what is being heard rather than what has been queued. Never beyond
  // the end of the sequence.
  [[nodiscard]] time::RationalTime position() const;

  // A consistent copy of the counters; they are updated from several threads.
  [[nodiscard]] EngineStatistics statistics() const;
  // The latest post-fader levels observed by the realtime mixer. Reading them is
  // cheap and never asks the decoder to render a second copy of the audio.
  [[nodiscard]] std::map<std::string, audio::TrackLevel> audio_levels() const;
  // The sequence currently being played. A snapshot: it does not change under the
  // caller, and a later UpdateSequence does not affect it.
  [[nodiscard]] std::shared_ptr<const timeline::SequenceGraph> graph() const;
  [[nodiscard]] const timeline::CompileOptions& compile_options() const noexcept { return config_.compile; }
  [[nodiscard]] bool prefers_proxies() const noexcept { return prefer_proxies_.load(std::memory_order_relaxed); }
  // Plays proxies where they are ready (or the originals), from the next frame on.
  void SetPreferProxies(bool on);
  // The adapter frames are being composed on, or empty when everything is software (not asked for, nothing usable, or lost).
  [[nodiscard]] std::string gpu_device_name() const;
  // Why the GPU is not in use when it was asked for; empty otherwise.
  [[nodiscard]] std::string gpu_unavailable_reason() const;
  // Makes an independent engine over the same immutable sequence with proxy
  // selection disabled. Export uses this automatically, so a monitor running
  // on low-resolution media cannot accidentally produce a proxy-quality file.
  [[nodiscard]] std::unique_ptr<PlaybackEngine> OriginalQualityClone() const;
  // The same, rendering pictures in another format: a delivery that keeps more than 8 bits (ProRes, 10-bit HEVC) needs
  // 16-bit pictures from the compositor, or the extra bits would be 8 bits stretched.
  [[nodiscard]] std::unique_ptr<PlaybackEngine> ExportClone(media::PixelFormat output_format) const;
  [[nodiscard]] media::PixelFormat output_format() const noexcept { return config_.compositor.output_format; }

  // Cancels older queued read-ahead, then starts decoding the requested timeline
  // frame and the configured window after it. A scrubber can call this as soon
  // as the playhead moves; rendering remains synchronous and correct if the
  // request has not completed yet.
  void PrimeReadAhead(const time::RationalTime& at);

 private:
  using Graph = std::shared_ptr<const timeline::SequenceGraph>;

  // Opens, and keeps open, one decoder per media id. Shared so a render holds the
  // decoder it is reading even if an edit replaces it meanwhile.
  [[nodiscard]] std::shared_ptr<media::Source> SourceFor(const std::string& media_id);
  [[nodiscard]] std::string ResolveMedia(const std::string& media_id) const;
  [[nodiscard]] const media::VideoFrame* FrameFor(const timeline::SourceRequest& request);
  // The picture for a request still on the GPU, when the hardware can decode this media; invalid when not.
  [[nodiscard]] media::DeviceFrame DeviceFrameFor(const timeline::SourceRequest& request);
  [[nodiscard]] std::shared_ptr<media::Source> DeviceSourceFor(const std::string& media_id);
  [[nodiscard]] const media::VideoFrame* InsertCachedFrame(std::string key, media::VideoFrame frame,
                                                            bool read_ahead);
  void ScheduleReadAhead(const Graph& graph, const time::RationalTime& at,
                         const timeline::CompileOptions& options, bool include_current,
                         std::uint64_t generation = 0);
  [[nodiscard]] std::uint64_t AdvanceReadAheadGeneration();
  void RunReadAheadPrimer();
  [[nodiscard]] media::VideoFrame ComposeSequence(const timeline::SequenceGraph& graph,
                                                  const timeline::Sequence& sequence, const time::RationalTime& at,
                                                  int depth, const timeline::CompileOptions& options);
  [[nodiscard]] render::SourceVersions Versions() const;
  [[nodiscard]] media::AudioBuffer MixBlock(const audio::AudioMixer& mixer, std::int64_t first_sample,
                                            std::int64_t frames, const timeline::CompileOptions& options);
  [[nodiscard]] Graph Snapshot() const;

  MediaLocator locator_;
  EngineConfig config_;
  timeline::TimelineCompiler compiler_;
  render::Compositor compositor_;
  // Stretched audio, shared by the monitor's mixer and the device's. Cleared on an
  // edit: a chain is keyed by what it was made from, but the media under it can change.
  std::shared_ptr<audio::StretchCache> stretch_cache_;
  audio::AudioMixer mixer_;

  // The sequence. Replaced whole; readers hold the pointer they took.
  mutable std::mutex graph_mutex_;
  Graph graph_;

  // Open decoders and the media known to be offline. The lock is held only to look
  // up or change the tables, never across opening or reading a file.
  struct OpenSource final {
    std::string path;
    std::shared_ptr<media::Source> source;
  };
  std::mutex sources_mutex_;
  std::unordered_map<std::string, OpenSource> sources_;
  // Media that failed to open. Remembered so a missing file is not retried once
  // per frame, which would make an offline clip grind the monitor to a halt.
  // Cleared by UpdateSequence, so a fixed file is retried once per edit.
  std::unordered_map<std::string, bool> offline_;

  // The picture path. RenderFrame holds video_mutex_ for the whole of a render.
  mutable std::mutex video_mutex_;
  // Least-recently-used raw-frame cache, keyed by media id and source time.
  struct CacheEntry final {
    std::string key;
    media::VideoFrame frame;
    bool read_ahead{false};
  };
  std::list<CacheEntry> cache_;
  std::unordered_map<std::string, std::list<CacheEntry>::iterator> cache_index_;
  std::size_t cache_bytes_{0};
  // Composed nested sequences, rebuilt per frame; kept alive so the compositor's
  // resolver can hand back a pointer.
  std::unordered_map<std::string, media::VideoFrame> nested_frames_;

  // The audio path. Audio renders take turns on audio_mutex_; none of them touches
  // the picture's state.
  std::mutex audio_mutex_;
  std::atomic<bool> prefer_proxies_{false};
  mutable std::mutex audio_levels_mutex_;
  std::map<std::string, audio::TrackLevel> audio_levels_;

  struct Counters final {
    std::atomic<std::int64_t> frames_rendered{0};
    std::atomic<std::int64_t> render_cache_hits{0};
    std::atomic<std::int64_t> render_cache_stores{0};
    std::atomic<std::int64_t> audio_blocks_mixed{0};
    std::atomic<std::int64_t> cache_hits{0};
    std::atomic<std::int64_t> cache_misses{0};
    std::atomic<std::int64_t> cache_evictions{0};
    std::atomic<std::int64_t> decode_failures{0};
    std::atomic<std::int64_t> offline_media{0};
    std::atomic<std::int64_t> read_ahead_queued{0};
    std::atomic<std::int64_t> read_ahead_completed{0};
    std::atomic<std::int64_t> read_ahead_cache_hits{0};
    std::atomic<std::int64_t> read_ahead_queue_full{0};
    std::atomic<std::int64_t> total_frame_render_microseconds{0};
    std::atomic<std::int64_t> max_frame_render_microseconds{0};
    std::atomic<std::int64_t> audio_blocks_clipped{0};
    std::atomic<std::int64_t> audio_missing_sources{0};
    std::atomic<std::int64_t> audio_blocks_dropped{0};
    std::atomic<std::int64_t> gpu_frames{0};
    std::atomic<std::int64_t> gpu_fallback_frames{0};
    std::atomic<std::int64_t> gpu_failures{0};
    std::atomic<std::int64_t> gpu_microseconds{0};
    std::atomic<std::int64_t> gpu_device_microseconds{0};
    std::atomic<std::int64_t> hardware_pictures{0};
    std::atomic<std::int64_t> uploaded_pictures{0};
  };
  Counters counters_;

  // The GPU compositor and the decoders that hand it pictures on its device; guarded by video_mutex_ like the rest of the
  // picture path. A decoder that cannot do hardware is remembered as null, so it is tried once.
  std::unique_ptr<render::gpu::D3D11Compositor> gpu_;
  std::string gpu_unavailable_reason_;
  std::unordered_map<std::string, std::shared_ptr<media::Source>> device_sources_;

  std::unique_ptr<DecodePool> decode_pool_;
  std::atomic<std::uint64_t> read_ahead_generation_{1};
  struct PrimeRequest final {
    Graph graph;
    time::RationalTime at;
    timeline::CompileOptions options;
    std::uint64_t generation{0};
  };
  std::mutex prime_mutex_;
  std::condition_variable prime_wake_;
  std::optional<PrimeRequest> prime_pending_;
  bool prime_stop_{false};
  std::thread prime_worker_;

  // Transport. Guarded by transport_mutex_; `generation_` counts seeks.
  mutable std::mutex transport_mutex_;
  audio::AudioSink* sink_{nullptr};
  audio::RenderCallback render_callback_;
  time::RationalTime play_origin_;
  time::RationalTime audio_cursor_;
  std::uint64_t generation_{0};
  bool playing_{false};
};

}  // namespace cutline::playback

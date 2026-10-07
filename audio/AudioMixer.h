#pragma once

// Mixes the audio side of a sequence into a block of samples.
//
// The counterpart to the compositor, and deliberately the same shape: a pure
// function over a sequence snapshot plus a resolver, with no I/O of its own. That
// is what lets the same code serve the monitor, the export worker, and the tests.
//
// Why the mixer reads the snapshot and not a playback plan. A plan is the state of
// the timeline at one instant, and audio is not an instant: a block of 1024
// samples spans clip edges, transitions and keyframes that all fall at particular
// samples inside it. Mixing from a plan made at the block's start put every one of
// those at a block boundary instead, and applied one effect value and one
// crossfade weight to the whole block. The mixer here decides every sample for
// itself.
//
// Mix model:
//   * One contribution per clip, summed. Audio mixes rather than occludes, but
//     the track order is still followed so a sum is bit-reproducible.
//   * Time is measured in output samples. A clip starts and ends on the nearest
//     sample, so a clip at the sequence's own rate is read without interpolation.
//   * A clip's rate and direction apply continuously: output sample n reads the
//     source at in + n * rate (or, reversed, at out - 1 - n * rate). At a rate of
//     one that is an exact copy; otherwise it is 4-point cubic interpolation.
//   * Pitch policy: VARISPEED by default. A clip played at twice the speed sounds
//     an octave higher and at half the speed an octave lower, as a tape does and as
//     Premiere does unless "Maintain Audio Pitch" is on. There is no anti-alias
//     filter on rates above one, so material with energy near the top of the band
//     will alias. A clip marked maintain_pitch is instead time-stretched (WSOLA, see
//     TimeStretch.h) between 0.25x and 4x; outside that it falls back to varispeed
//     and the statistics say so. Stretched audio has no handles: past its source
//     range it is silent.
//   * A nested sequence is rendered by the same code at the inner time its clip
//     maps to, then treated as that clip's source. Its own track strips and
//     sequence effects apply inside it and nowhere else.
//   * Effects are evaluated per sample. Clip effects use the clip's local time,
//     track and sequence effects the timeline's.
//   * The track's effects, then its gain and pan, apply to the sum of the track's
//     clips, not to each clip.
//   * A transition between two clips on a track crossfades them per sample. The
//     outgoing clip keeps playing past its out point, and the incoming one starts
//     before its in point, reading the media either side of the cut (its
//     handles); where the source has no more, that is silence. "constant_power"
//     uses cos/sin weights, every other kind a linear ramp.

#include "audio/StretchCache.h"
#include "media/AudioBuffer.h"
#include "timeline/TimelineCompiler.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cutline::audio {

// Supplies decoded audio from a media item: `frames` samples starting at source
// time `time`, already resampled to `sample_rate` and `channels`. The time may be
// negative or past the end of the media (a transition's handles); the resolver
// returns silence for those samples. Returning nullopt means the source has no
// audio or is not available, which the mixer treats as silence rather than as a
// failure of the block.
using AudioResolver = std::function<std::optional<media::AudioBuffer>(
    const std::string& media_id, const time::RationalTime& time, std::int64_t sample_rate, int channels,
    std::int64_t frames)>;

struct MixerConfig final {
  std::int64_t sample_rate{48000};
  int channels{2};
  // Clamps the sum into range. This is a clamp, not a limiter: it has no attack
  // or release and distorts whatever exceeds full scale. It exists so an overload
  // cannot wrap, and `MixStatistics::clipped` reports when it was needed.
  bool limit_output{true};
  // Where stretched chains are kept between blocks. Without one, a chain is
  // recomputed for every block that touches it, which is correct and very slow.
  std::shared_ptr<StretchCache> stretch_cache;
  // Keep the output of each cell of every stateful effect (equaliser, dynamics,
  // noise reduction) in that cache, so that the next block in the same cell does not
  // run the effect again. Off, every block recomputes the cell it touches.
  bool cache_dsp{true};
};

struct TrackLevel final {
  double peak_db{-200.0};
  double rms_db{-200.0};
};

struct MixStatistics final {
  int sources_mixed{0};
  int missing_sources{0};
  int transitions_mixed{0};
  float peak{0.0f};
  bool clipped{false};
  // Pitch-preserving retiming: chains computed, chains found in the cache, and clips
  // that asked for it at a speed too extreme to stretch and were played varispeed.
  int stretch_chains_computed{0};
  int stretch_cache_hits{0};
  int stretch_fallbacks{0};
  // Blocks whose samples followed a time-remap curve (including freezes and
  // reverse segments). A block of a clip that keeps its pitch is stretched along the curve when the curve
  // runs forward within the stretchable speed range for the whole block (remap_stretched_blocks); a block
  // that holds, reverses or goes beyond the range is played varispeed and counted as a fallback.
  int time_remap_blocks{0};
  int remap_stretched_blocks{0};
  // Stateful effects (see Dsp.h): cells processed, cells found in the cache, and
  // effects keyed by a side-chain that could not be found or would have looped.
  int dsp_cells_processed{0};
  int dsp_cache_hits{0};
  int missing_sidechains{0};
  std::vector<std::string> skipped_effects;
  // Post-fader levels for each audio track rendered while producing this block.
  // Playback publishes these to the mixer interface without rendering a second
  // copy of the block just to drive meters.
  std::map<std::string, TrackLevel> track_levels;
};

// What a meter shows for a block: levels in decibels (-200 for silence) and loudness
// in LUFS as ITU-R BS.1770 defines it. Meters never alter the signal.
struct LevelReading final {
  double peak_db{-200.0};
  double true_peak_db{-200.0};
  double rms_db{-200.0};
  double momentary_lufs{-200.0};
  double short_term_lufs{-200.0};
  double integrated_lufs{-200.0};
};
[[nodiscard]] LevelReading MeasureLevels(const media::AudioBuffer& buffer);

// Effects the mixer implements natively.
[[nodiscard]] bool IsBuiltInAudioEffect(const std::string& effect_type);

class AudioMixer final {
 public:
  explicit AudioMixer(MixerConfig config = {}) : config_(config) {}

  // Mixes `frames` samples of the graph's root sequence, starting at `first_sample`
  // on its timeline (a count of samples at the mixer's rate). Blocks that tile the
  // timeline reproduce exactly what one large block would.
  [[nodiscard]] media::AudioBuffer MixSamples(const timeline::SequenceGraph& graph, std::int64_t first_sample,
                                       std::int64_t frames, const AudioResolver& resolve,
                                       MixStatistics& statistics, const timeline::CompileOptions& options = {}) const;
  [[nodiscard]] media::AudioBuffer MixSamples(const timeline::SequenceGraph& graph, std::int64_t first_sample,
                                       std::int64_t frames, const AudioResolver& resolve) const;

  // One audio track's signal alone: after its effects and fader, before the master,
  // which is what its meter reads. A bus gives what is routed to it. A track that is
  // muted, or silenced by another's solo, gives silence. Unknown tracks throw.
  [[nodiscard]] media::AudioBuffer MixTrack(const timeline::SequenceGraph& graph, const std::string& track_id,
                                            std::int64_t first_sample, std::int64_t frames, const AudioResolver& resolve,
                                            MixStatistics& statistics, const timeline::CompileOptions& options = {}) const;

  // The same, from a time. The time is rounded to the nearest sample.
  [[nodiscard]] media::AudioBuffer Mix(const timeline::SequenceGraph& graph, const time::RationalTime& at,
                                       std::int64_t frames, const AudioResolver& resolve,
                                       MixStatistics& statistics, const timeline::CompileOptions& options = {}) const;
  [[nodiscard]] media::AudioBuffer Mix(const timeline::SequenceGraph& graph, const time::RationalTime& at,
                                       std::int64_t frames, const AudioResolver& resolve) const;

  [[nodiscard]] const MixerConfig& config() const noexcept { return config_; }

 private:
  MixerConfig config_;
};

}  // namespace cutline::audio

#pragma once

// Audio processors as plain functions over a block of planar float samples.
//
// Every processor here is built so that its output at a sample depends only on the
// input in a bounded neighbourhood of that sample (its *memory* before and its
// *lookahead* after), never on how far back the stream goes. That is a deliberate
// restriction. A classic compressor or gate keeps a running envelope that carries
// information from the whole past, which means the same sample can come out
// differently depending on where playback started, how the render was cut into
// blocks, or whether it was a seek. Here, attack and release are finite
// (moving averages and sliding-window extrema), so the mixer can render any span by
// running the processor over that span plus its memory and lookahead and keeping the
// middle, and get the same samples every time. IIR filters (EQ) cannot be exactly
// finite, so they are run from silence over a long enough lead-in (`memory`) that the
// difference is far below anything audible, and from the same start every time, which
// makes them deterministic.
//
// The functions take whole blocks and treat the block's edges as silence; the caller
// supplies the margin (see Margins).

#include "media/AudioBuffer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cutline::audio::dsp {

// ---------------------------------------------------------------- filters ----

struct Biquad final {
  double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
};

[[nodiscard]] Biquad Peaking(double sample_rate, double frequency, double gain_db, double q);
[[nodiscard]] Biquad LowShelf(double sample_rate, double frequency, double gain_db, double slope = 1.0);
[[nodiscard]] Biquad HighShelf(double sample_rate, double frequency, double gain_db, double slope = 1.0);
[[nodiscard]] Biquad LowPass(double sample_rate, double frequency, double q);
[[nodiscard]] Biquad HighPass(double sample_rate, double frequency, double q);

// Magnitude of the filter's response at `frequency`, in decibels.
[[nodiscard]] double ResponseDb(const Biquad& filter, double sample_rate, double frequency);

// Runs one filter over a channel in place, from silence.
void Filter(float* samples, std::int64_t count, const Biquad& filter);

// ---------------------------------------------------------------- processors ----

// A three-band equaliser: low shelf, mid peak, high shelf.
struct EqSettings final {
  double low_gain_db{0.0};
  double low_frequency{120.0};
  double mid_gain_db{0.0};
  double mid_frequency{1000.0};
  double mid_q{1.0};
  double high_gain_db{0.0};
  double high_frequency{8000.0};
};
void ApplyEq(media::AudioBuffer& buffer, const EqSettings& settings);

struct CompressorSettings final {
  double threshold_db{-18.0};
  double ratio{4.0};
  double attack_ms{10.0};
  double release_ms{150.0};
  double makeup_db{0.0};
  double knee_db{6.0};
};
// `detector` is the signal the level is measured on (the buffer itself, or a
// side-chain); it must have the same length.
void Compress(media::AudioBuffer& buffer, const media::AudioBuffer& detector, const CompressorSettings& settings);

struct LimiterSettings final {
  double ceiling_db{-1.0};
  double release_ms{100.0};
  double lookahead_ms{3.0};
};
// Guarantees no sample exceeds the ceiling.
void Limit(media::AudioBuffer& buffer, const LimiterSettings& settings);

struct GateSettings final {
  double threshold_db{-50.0};
  double range_db{-60.0};  // attenuation when closed
  double attack_ms{2.0};
  double hold_ms{40.0};
  double release_ms{120.0};
};
void Gate(media::AudioBuffer& buffer, const media::AudioBuffer& detector, const GateSettings& settings);

// Lowers the signal while the detector (another signal) is above a threshold: music
// under dialogue.
struct DuckSettings final {
  double threshold_db{-35.0};
  double reduction_db{-12.0};
  double attack_ms{20.0};
  double release_ms{400.0};
};
void Duck(media::AudioBuffer& buffer, const media::AudioBuffer& detector, const DuckSettings& settings);

// Broadband noise reduction: spectral subtraction against a noise floor estimated,
// per frequency bin, as the quietest recent level (so it needs no sample of noise).
struct DenoiseSettings final {
  double amount{0.7};        // 0..1
  double sensitivity{1.0};   // scales the noise estimate: above 1 removes more noise, and more signal
};
void Denoise(media::AudioBuffer& buffer, const DenoiseSettings& settings);

// Late-reverberation suppression: each bin's recent, decayed energy is taken as the
// reverberant tail and subtracted.
struct DereverbSettings final {
  double amount{0.6};
  double decay_ms{300.0};
};
void Dereverb(media::AudioBuffer& buffer, const DereverbSettings& settings);

// ---------------------------------------------------------------- margins ----

// How much input before (`memory`) and after (`lookahead`) a span a processor needs
// to produce that span exactly, in samples.
struct Margins final {
  std::int64_t memory{0};
  std::int64_t lookahead{0};
};
[[nodiscard]] Margins CompressorMargins(const CompressorSettings& settings, double sample_rate);
[[nodiscard]] Margins LimiterMargins(const LimiterSettings& settings, double sample_rate);
[[nodiscard]] Margins GateMargins(const GateSettings& settings, double sample_rate);
[[nodiscard]] Margins DuckMargins(const DuckSettings& settings, double sample_rate);
[[nodiscard]] Margins EqMargins(const EqSettings& settings, double sample_rate);
[[nodiscard]] Margins DenoiseMargins(double sample_rate);
[[nodiscard]] Margins DereverbMargins(const DereverbSettings& settings, double sample_rate);

// ------------------------------------------------------------- loudness ----

// ITU-R BS.1770-4 / EBU R128 measurement of a block: integrated loudness (LUFS,
// with the absolute and relative gates) and momentary loudness of the last 400 ms.
// Channel weights are 1.0 for the first two channels (left, right) and 1.41 for
// the surrounds when there are five; others are 1.0.
struct Loudness final {
  double integrated_lufs{-200.0};  // -200 means nothing above the absolute gate
  double momentary_lufs{-200.0};
  double short_term_lufs{-200.0};
  double loudest_momentary_lufs{-200.0};
  double sample_peak_db{-200.0};
  double true_peak_db{-200.0};
};
[[nodiscard]] Loudness MeasureLoudness(const media::AudioBuffer& buffer);

// The gain, in decibels, that brings the block's integrated loudness to `target`.
[[nodiscard]] double GainToReachLoudness(const media::AudioBuffer& buffer, double target_lufs);

// The same measurement made as the sound arrives, block after block, in memory that does not grow with the length of
// the programme (one number per 100 ms): what measuring an hour of mix needs, where MeasureLoudness would want the hour
// in memory at once. Blocks must be consecutive; the result after pushing them is MeasureLoudness of their
// concatenation (integrated and momentary to a thousandth of a unit, peaks exactly).
class LoudnessMeter final {
 public:
  LoudnessMeter(std::int64_t sample_rate, int channels);
  void Push(const media::AudioBuffer& block);
  [[nodiscard]] Loudness Result() const;
  [[nodiscard]] std::int64_t frames() const noexcept { return frames_; }

 private:
  struct Channel final {
    double shelf[4]{};      // x1, x2, y1, y2 of the shelving stage
    double high_pass[4]{};  // and of the high-pass stage
    double step_sum{0.0};   // sum of squares of the K-weighted signal in the current 100 ms
    double total_sum{0.0};  // and over everything so far
    std::vector<float> window;  // samples whose true peak is not yet known (and the context they need)
  };
  std::int64_t sample_rate_;
  std::int64_t step_;
  std::vector<Channel> channels_;
  std::vector<double> powers_;   // per completed 100 ms: weighted mean square over the channels
  std::int64_t frames_{0};
  std::int64_t in_step_{0};
  std::int64_t window_start_{0};  // stream index of window[0]
  std::int64_t peak_next_{0};     // next stream index whose true peak is to be taken
  double peak_{0.0};
  double true_peak_{0.0};
};

[[nodiscard]] double DecibelsOf(double linear);

}  // namespace cutline::audio::dsp

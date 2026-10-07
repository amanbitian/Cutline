#include "audio/AudioClock.h"

#include <stdexcept>

namespace cutline::audio {
AudioClock::AudioClock(std::int64_t sample_rate) : sample_rate_(sample_rate) { if (sample_rate_ <= 0) throw std::invalid_argument("Audio clock requires a positive sample rate"); }
void AudioClock::Reconfigure(std::int64_t sample_rate) { if (sample_rate <= 0) throw std::invalid_argument("Audio clock requires a positive sample rate"); sample_rate_ = sample_rate; Reset(); }
void AudioClock::Reset(std::int64_t sample_position) noexcept { sample_position_.store(sample_position, std::memory_order_release); }
void AudioClock::AdvanceFromAudioCallback(std::int64_t rendered_samples) noexcept { if (rendered_samples > 0) sample_position_.fetch_add(rendered_samples, std::memory_order_acq_rel); }
time::RationalTime AudioClock::Now() const { return {sample_position_.load(std::memory_order_acquire), sample_rate_}; }
}  // namespace cutline::audio

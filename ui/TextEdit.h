#pragma once

// Text-based editing: the words of a transcript as a way to edit the timeline.
//
// A transcript says when each word is said in the media (speech/Transcript.h). The timeline shows pieces of that media,
// trimmed and moved and repeated, so the first job is the mapping: where on this sequence each word is heard. From that,
// editing by text is ordinary timeline editing in a different dress. Deleting words removes the time they occupy
// (ripple or lift, the same plans the razor and the Delete key make); taking out the "um"s and the long pauses is
// the same removal over found ranges; captions are the words regrouped into cues at their timeline times.
//
// Every function here returns an EditPlan to be run as one group, so any of them is one undo step.
//
// What is mapped. A clip of media (not a nested sequence), enabled, at its own speed or any constant speed, forwards.
// A reversed clip and a clip with a speed ramp are left out and the mapping says so: the words of a ramp are not at a
// constant rate and cutting them by text would only look precise. Where the media is on audio tracks, those clips carry
// the words (the picture follows them by the link); a video-only use of the media carries them when there is no audio.

#include "speech/Transcript.h"
#include "ui/EditPlanner.h"

#include <string>
#include <vector>

namespace cutline::ui {

struct SequenceWord final {
  // The word's index in the transcript.
  std::size_t word{0};
  // Where it is heard on the timeline, snapped to frames and at least one frame long.
  RationalTime start, end;
  std::string clip_id;
  // Only part of the word is in the clip (the clip's edge cuts through it).
  bool partial{false};
};

struct WordMapping final {
  // In timeline order. A word appears once for each place it is heard.
  std::vector<SequenceWord> words;
  // What was left out and why, for the person to read.
  std::vector<std::string> notes;
};

// Where the words of `transcript` (of the media `media_id`) are heard on `sequence`.
[[nodiscard]] WordMapping MapWords(const timeline::Sequence& sequence, const speech::Transcript& transcript, const std::string& media_id);

// The mapped word being said at `at` (or the one about to be said, within a gap), or nothing before the first.
[[nodiscard]] std::optional<std::size_t> WordAt(const WordMapping& mapping, const RationalTime& at);

// The positions in the mapping of the words in these transcript ranges (words that are not on the timeline are
// skipped).
[[nodiscard]] std::vector<std::size_t> MappedIndices(const WordMapping& mapping, const std::vector<speech::WordRange>& ranges);

struct DeleteWordsOptions final {
  // Close the gap (extract) or leave it (lift).
  bool ripple{true};
  // Time of each deleted run's edges to leave in, in seconds: the engine's word times are good to a tenth of a second or
  // so, and a little left on keeps the end of a neighbouring word from being clipped. Never takes a run below a frame.
  double keep_seconds{0.0};
};

// Removes the words at these positions of the mapping. Neighbouring words of one clip are removed as one run, with the
// time between them.
[[nodiscard]] EditPlan PlanDeleteWords(const EditContext& ctx, const WordMapping& mapping, const std::vector<std::size_t>& positions, const DeleteWordsOptions& options = {});

// The time of the pauses between words (of one clip) that are at least `minimum` seconds long, less `keep` seconds left at
// each side, as ranges ready to remove.
[[nodiscard]] std::vector<TimeRange> PauseRanges(const WordMapping& mapping, const RationalTime& frame, double minimum, double keep);
// Takes out those pauses (always rippling: a removed pause that left its gap would not be removed).
[[nodiscard]] EditPlan PlanRemovePauses(const EditContext& ctx, const WordMapping& mapping, double minimum, double keep);

struct CaptionPlanOptions final {
  speech::CaptionOptions grouping;
  // The caption track to add to. When it is not in the sequence it is made, with this name and language.
  std::string track_id;
  std::string track_name{"Transcript"};
  std::string language;
};

// Captions from the words that are heard on the timeline, one cue per group of words, at their timeline times.
// Cues of a track are not made to overlap: a cue ends where the next begins if it would run into it.
[[nodiscard]] EditPlan PlanCaptions(const EditContext& ctx, const WordMapping& mapping, const speech::Transcript& transcript, const CaptionPlanOptions& options);

}  // namespace cutline::ui

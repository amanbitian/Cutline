#include "ui/TextEdit.h"

#include <algorithm>
#include <cmath>

namespace cutline::ui {
namespace {

using timeline::Clip;
using timeline::Track;

RationalTime FrameOf(const timeline::Sequence& sequence) { return RationalTime(sequence.frame_rate.denominator, sequence.frame_rate.numerator); }

RationalTime FromSeconds(double seconds) { return RationalTime(static_cast<std::int64_t>(std::llround(seconds * 1000.0)), 1000); }

double ToSeconds(const RationalTime& time) { return static_cast<double>(time.numerator()) / static_cast<double>(time.denominator()); }

RationalTime SnapToFrame(const RationalTime& time, const timeline::Sequence& sequence) {
  return RationalTime::FromFrames(time.ToFrames(sequence.frame_rate, time::RoundingMode::Nearest), sequence.frame_rate);
}

bool Ramped(const Clip& clip) {
  return std::any_of(clip.effects.begin(), clip.effects.end(), [](const timeline::Effect& e) { return e.effect_type == "time_remap"; });
}

struct Carrier final {
  const Track* track{nullptr};
  const Clip* clip{nullptr};
};

}  // namespace

WordMapping MapWords(const timeline::Sequence& sequence, const speech::Transcript& transcript, const std::string& media_id) {
  WordMapping mapping;
  std::vector<Carrier> audio, video;
  int reversed = 0, ramped = 0;
  for (const auto& track : sequence.tracks) {
    if (track.is_bus) continue;
    for (const auto& clip : track.clips) {
      if (clip.source_kind != model::SourceKind::Media || clip.source_id != media_id || !clip.enabled) continue;
      if (clip.reversed) {
        ++reversed;
        continue;
      }
      if (Ramped(clip)) {
        ++ramped;
        continue;
      }
      (track.kind == model::TrackKind::Audio ? audio : video).push_back({&track, &clip});
    }
  }
  if (reversed > 0) mapping.notes.push_back(std::to_string(reversed) + (reversed == 1 ? " reversed clip" : " reversed clips") + " of this media not mapped");
  if (ramped > 0) mapping.notes.push_back(std::to_string(ramped) + (ramped == 1 ? " clip with a speed ramp" : " clips with a speed ramp") + " not mapped");
  const auto& carriers = audio.empty() ? video : audio;

  const auto frame = FrameOf(sequence);
  for (const auto& carrier : carriers) {
    const auto& clip = *carrier.clip;
    const double in = ToSeconds(clip.source_in), out = ToSeconds(clip.source_out);
    for (std::size_t i = 0; i < transcript.words.size(); ++i) {
      const auto& word = transcript.words[i];
      if (word.start >= out) break;
      if (word.end <= in && !(word.end == word.start && word.start >= in)) continue;
      const bool partial = word.start < in || word.end > out;
      const auto s = FromSeconds(std::max(word.start, in)), e = FromSeconds(std::min(word.end, out));
      auto start = clip.timeline_start.Add(s.Subtract(clip.source_in).Divide(clip.playback_rate));
      auto end = clip.timeline_start.Add(e.Subtract(clip.source_in).Divide(clip.playback_rate));
      start = SnapToFrame(start, sequence);
      end = SnapToFrame(end, sequence);
      if (start.Compare(clip.timeline_start) < 0) start = clip.timeline_start;
      if (end.Compare(clip.end()) > 0) end = clip.end();
      if (end.Compare(start.Add(frame)) < 0) end = start.Add(frame);
      mapping.words.push_back({i, start, end, clip.id, partial});
    }
  }
  std::stable_sort(mapping.words.begin(), mapping.words.end(), [](const SequenceWord& a, const SequenceWord& b) {
    const int c = a.start.Compare(b.start);
    return c != 0 ? c < 0 : a.word < b.word;
  });
  return mapping;
}

std::optional<std::size_t> WordAt(const WordMapping& mapping, const RationalTime& at) {
  // The last word that starts at or before `at`.
  const auto it = std::upper_bound(mapping.words.begin(), mapping.words.end(), at, [](const RationalTime& t, const SequenceWord& w) { return t.Compare(w.start) < 0; });
  if (it == mapping.words.begin()) return std::nullopt;
  return static_cast<std::size_t>(std::distance(mapping.words.begin(), it) - 1);
}

std::vector<std::size_t> MappedIndices(const WordMapping& mapping, const std::vector<speech::WordRange>& ranges) {
  std::vector<std::size_t> positions;
  for (std::size_t p = 0; p < mapping.words.size(); ++p) {
    const auto word = mapping.words[p].word;
    for (const auto& range : ranges) {
      if (word >= range.first && word < range.first + range.count) {
        positions.push_back(p);
        break;
      }
    }
  }
  return positions;
}

EditPlan PlanDeleteWords(const EditContext& ctx, const WordMapping& mapping, const std::vector<std::size_t>& positions_in, const DeleteWordsOptions& options) {
  if (ctx.sequence == nullptr) return EditPlan::Refuse("There is no sequence");
  auto positions = positions_in;
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  positions.erase(std::remove_if(positions.begin(), positions.end(), [&](std::size_t p) { return p >= mapping.words.size(); }), positions.end());
  if (positions.empty()) return EditPlan::Refuse("No words are chosen");

  const auto frame = FrameOf(*ctx.sequence);
  const auto keep = FromSeconds(std::max(options.keep_seconds, 0.0));
  std::vector<TimeRange> ranges;
  std::size_t i = 0;
  while (i < positions.size()) {
    // A run: words next to each other in the mapping and in one clip.
    std::size_t j = i;
    while (j + 1 < positions.size() && positions[j + 1] == positions[j] + 1 && mapping.words[positions[j + 1]].clip_id == mapping.words[positions[i]].clip_id) ++j;
    auto in = mapping.words[positions[i]].start;
    auto out = mapping.words[positions[j]].end;
    for (std::size_t k = i; k <= j; ++k) {
      if (mapping.words[positions[k]].end.Compare(out) > 0) out = mapping.words[positions[k]].end;
    }
    const auto shrunk_in = in.Add(keep), shrunk_out = out.Subtract(keep);
    if (shrunk_out.Subtract(shrunk_in).Compare(frame) >= 0) {
      in = shrunk_in;
      out = shrunk_out;
    }
    ranges.push_back({SnapToFrame(in, *ctx.sequence), SnapToFrame(out, *ctx.sequence)});
    i = j + 1;
  }
  const std::string label = positions.size() == 1 ? "Delete Word" : "Delete Words";
  auto plan = PlanRemoveRanges(ctx, std::move(ranges), options.ripple, label);
  if (plan.ok) plan.notes.push_back(std::to_string(positions.size()) + (positions.size() == 1 ? " word" : " words") + " removed");
  return plan;
}

std::vector<TimeRange> PauseRanges(const WordMapping& mapping, const RationalTime& frame, double minimum, double keep) {
  std::vector<TimeRange> ranges;
  const auto minimum_time = FromSeconds(std::max(minimum, 0.0));
  const auto keep_time = FromSeconds(std::max(keep, 0.0));
  for (std::size_t p = 0; p + 1 < mapping.words.size(); ++p) {
    const auto& a = mapping.words[p];
    const auto& b = mapping.words[p + 1];
    if (a.clip_id != b.clip_id) continue;
    const auto gap = b.start.Subtract(a.end);
    if (gap.Compare(minimum_time) < 0 || gap.Compare(frame) < 0) continue;
    const auto in = a.end.Add(keep_time), out = b.start.Subtract(keep_time);
    if (out.Subtract(in).Compare(frame) >= 0) ranges.push_back({in, out});
  }
  return ranges;
}

EditPlan PlanRemovePauses(const EditContext& ctx, const WordMapping& mapping, double minimum, double keep) {
  if (ctx.sequence == nullptr) return EditPlan::Refuse("There is no sequence");
  auto ranges = PauseRanges(mapping, FrameOf(*ctx.sequence), minimum, keep);
  if (ranges.empty()) return EditPlan::Refuse("There is no pause that long between words");
  const auto count = ranges.size();
  for (auto& range : ranges) {
    range.in = SnapToFrame(range.in, *ctx.sequence);
    range.out = SnapToFrame(range.out, *ctx.sequence);
  }
  auto plan = PlanRemoveRanges(ctx, std::move(ranges), true, "Remove Pauses");
  if (plan.ok) plan.notes.push_back(std::to_string(count) + (count == 1 ? " pause" : " pauses") + " removed");
  return plan;
}

EditPlan PlanCaptions(const EditContext& ctx, const WordMapping& mapping, const speech::Transcript& transcript, const CaptionPlanOptions& options) {
  if (ctx.sequence == nullptr || !ctx.new_id) return EditPlan::Refuse("There is no sequence");
  if (mapping.words.empty()) return EditPlan::Refuse("There are no transcribed words on the timeline");
  std::vector<speech::TimedWord> timed;
  timed.reserve(mapping.words.size());
  for (const auto& word : mapping.words) timed.push_back({transcript.words[word.word].text, ToSeconds(word.start), ToSeconds(word.end)});
  const auto groups = speech::GroupCaptions(timed, options.grouping);

  EditPlan plan;
  plan.ok = true;
  plan.label = "Captions from Transcript";
  std::string track_id = options.track_id;
  const bool exists = !track_id.empty() && std::any_of(ctx.sequence->caption_tracks.begin(), ctx.sequence->caption_tracks.end(), [&](const auto& t) { return t.id == track_id; });
  if (!exists) {
    commands::AddCaptionTrackPayload track;
    track.id = track_id.empty() ? ctx.new_id("captrack") : track_id;
    track.sequence_id = ctx.sequence->id;
    track.name = options.track_name;
    track.language = options.language.empty() ? transcript.language : options.language;
    track_id = track.id;
    plan.commands.push_back({commands::CommandType::AddCaptionTrack, track});
  }
  commands::AddCaptionsPayload cues;
  cues.track_id = track_id;
  for (std::size_t g = 0; g < groups.size(); ++g) {
    const auto& group = groups[g];
    commands::CaptionCuePayload cue;
    cue.id = ctx.new_id("cap");
    cue.start = mapping.words[group.first].start;
    cue.end = mapping.words[group.last].end;
    if (g + 1 < groups.size()) {
      const auto& next = mapping.words[groups[g + 1].first].start;
      if (cue.end.Compare(next) > 0) cue.end = next;
    }
    if (cue.end.Compare(cue.start) <= 0) continue;
    cue.text = group.text;
    const int speaker = transcript.words[mapping.words[group.first].word].speaker;
    if (speaker >= 0 && speaker < static_cast<int>(transcript.speakers.size())) cue.speaker = transcript.speakers[static_cast<std::size_t>(speaker)];
    cues.cues.push_back(std::move(cue));
  }
  if (cues.cues.empty()) return EditPlan::Refuse("The words are too short to caption");
  plan.notes.push_back(std::to_string(cues.cues.size()) + " captions made");
  plan.commands.push_back({commands::CommandType::AddCaptions, std::move(cues)});
  return plan;
}

}  // namespace cutline::ui

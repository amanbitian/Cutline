#pragma once

// Domain vocabulary shared by the command layer, the project store, and the
// timeline compiler. These live in one place so the three cannot drift: a
// source kind added here is a compile error everywhere it is not handled.

#include <string>

namespace cutline::model {

enum class TrackKind { Video, Audio };

// What a clip reads from. `Adjustment` is a clip with no source at all: it
// exists only to apply its effect stack to the tracks beneath it.
enum class SourceKind { Media, Sequence, Adjustment };

// Effects attach to any of these. Track and sequence effects are how a master
// grade or a mix bus is expressed.
enum class EffectOwner { Clip, Track, Sequence, Transition };

enum class MarkerOwner { Sequence, Clip, Media };

enum class MarkerKind { Comment, Chapter, Segmentation, WebLink, FlvCue };

// Where a transition sits relative to the cut it spans.
enum class TransitionAlignment { Center, Start, End, Custom };

enum class FieldOrder { Progressive, UpperFirst, LowerFirst };

enum class StreamKind { Video, Audio, Subtitle, Data };

enum class Cadence { Constant, Variable };

enum class ColorRange { Limited, Full };

// Each of these round-trips: Parse(ToString(value)) == value. The string form is
// what the schema's CHECK constraints and the journal JSON use.
[[nodiscard]] std::string ToString(TrackKind value);
[[nodiscard]] std::string ToString(SourceKind value);
[[nodiscard]] std::string ToString(EffectOwner value);
[[nodiscard]] std::string ToString(MarkerOwner value);
[[nodiscard]] std::string ToString(MarkerKind value);
[[nodiscard]] std::string ToString(TransitionAlignment value);
[[nodiscard]] std::string ToString(FieldOrder value);
[[nodiscard]] std::string ToString(StreamKind value);
[[nodiscard]] std::string ToString(Cadence value);
[[nodiscard]] std::string ToString(ColorRange value);

[[nodiscard]] TrackKind ParseTrackKind(const std::string& value);
[[nodiscard]] SourceKind ParseSourceKind(const std::string& value);
[[nodiscard]] EffectOwner ParseEffectOwner(const std::string& value);
[[nodiscard]] MarkerOwner ParseMarkerOwner(const std::string& value);
[[nodiscard]] MarkerKind ParseMarkerKind(const std::string& value);
[[nodiscard]] TransitionAlignment ParseTransitionAlignment(const std::string& value);
[[nodiscard]] FieldOrder ParseFieldOrder(const std::string& value);
[[nodiscard]] StreamKind ParseStreamKind(const std::string& value);
[[nodiscard]] Cadence ParseCadence(const std::string& value);
[[nodiscard]] ColorRange ParseColorRange(const std::string& value);

}  // namespace cutline::model

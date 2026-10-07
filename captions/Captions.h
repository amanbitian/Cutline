#pragma once

// Captions: timed text as project data, in and out of the two formats the world exchanges it in,
// and laid out for burning into a picture.
//
// What a caption is here. A cue of text with a start and an end, in exact rational time like
// everything else, on a caption track of a sequence. A track has a language and a default style;
// a cue may override that style. Cues may overlap (two speakers talking at once is not an error),
// and are shown together, stacked.
//
// SubRip (.srt) and WebVTT (.vtt). Both are read leniently where lenience cannot change what is
// meant (a byte order mark, CRLF or LF, a missing cue number, a period for a comma) and strictly
// where it could: a cue with unreadable timing, or that ends before it starts, is skipped and
// reported with its line number rather than guessed at. Times are millisecond-exact in both
// formats, which is exact in rational time, so a file read and written again is the same file; a
// time in the project that is not a whole millisecond (a frame boundary at 29.97) is written to
// the nearest millisecond and the report says it was rounded. Inline styling (<i>, <b>, font
// tags) is not kept and is reported, and the ASS-style position override {\an1}..{\an9} that
// subtitle files commonly carry is understood and becomes the cue's alignment and position.

#include "core/time/RationalTime.h"

#include <string>
#include <vector>

namespace cutline::captions {

struct Cue final {
  std::string id;
  time::RationalTime start;
  time::RationalTime end;
  std::string text;      // lines separated by \n
  std::string style_json{"{}"};  // overrides the track's style, field by field
  // The track's style with this cue's laid over it. Filled in when a snapshot is loaded, so that
  // drawing a cue does not have to merge two JSON documents on every frame.
  std::string resolved_style_json;
  std::string speaker;
};

struct Track final {
  std::string id;
  std::string name;
  std::string language;  // BCP 47, e.g. "en", "pt-BR"
  std::string style_json{"{}"};
  std::vector<Cue> cues;  // by start time
};

// How a cue is drawn. Sizes are fractions of the picture's height, so a caption stays the same
// proportion of the picture at any resolution.
struct Style final {
  std::string family{"Arial"};
  double size{0.055};
  bool bold{false};
  bool italic{false};
  double color[4]{1.0, 1.0, 1.0, 1.0};
  double background[4]{0.0, 0.0, 0.0, 0.6};
  double outline[4]{0.0, 0.0, 0.0, 1.0};
  double outline_width{0.0};
  std::string align{"center"};      // left, center, right
  std::string position{"bottom"};   // top, middle, bottom
  // Distance from the edge of the title-safe area, as a fraction of the picture's height.
  double margin{0.02};
  // The widest a line may be, as a fraction of the picture's width.
  double max_width{0.8};
  // Space between the text and the edge of its background box.
  double padding{0.012};
  double letter_spacing{0.0};
};

[[nodiscard]] Style ParseStyle(const std::string& json);
[[nodiscard]] std::string StyleToJson(const Style& style);
// The style of `override` laid over `base`: a field present in the override wins.
[[nodiscard]] std::string MergeStyles(const std::string& base_json, const std::string& override_json);

// ---------------------------------------------------------------------- reports ----

struct Issue final {
  int line{0};
  std::string message;
  bool dropped{false};  // the cue was left out
};

struct ParseResult final {
  std::vector<Cue> cues;
  std::vector<Issue> issues;
  [[nodiscard]] bool clean() const { return issues.empty(); }
};

// ---------------------------------------------------------------------- formats ----

[[nodiscard]] ParseResult ParseSrt(const std::string& text);
[[nodiscard]] ParseResult ParseWebVtt(const std::string& text);
// Which of the two a file is, from its first line: WEBVTT, or timing lines.
[[nodiscard]] ParseResult ParseCaptions(const std::string& text);

struct WriteResult final {
  std::string text;
  std::vector<Issue> issues;
};
[[nodiscard]] WriteResult WriteSrt(const std::vector<Cue>& cues);
[[nodiscard]] WriteResult WriteWebVtt(const std::vector<Cue>& cues);

// ----------------------------------------------------------------------- layout ----

struct Rect final {
  double x{0}, y{0}, width{0}, height{0};
};

// The regions a broadcaster treats as safe: action-safe is the middle 90 percent, title-safe the
// middle 80, each side of the frame.
[[nodiscard]] Rect ActionSafe(double frame_width, double frame_height);
[[nodiscard]] Rect TitleSafe(double frame_width, double frame_height);

// The cues showing at `at` (start <= at < end), in track order then start order.
struct ActiveCue final {
  const Track* track{};
  const Cue* cue{};
};
[[nodiscard]] std::vector<ActiveCue> ActiveCues(const std::vector<Track>& tracks, const time::RationalTime& at);

// Writes one file per format per track, named <base>.<language>.srt / .vtt, into `directory`.
// Returns the paths written. This is the sidecar delivery of captions that are not burned in.
struct Sidecar final {
  std::string path;
  std::string track_id;
  std::string format;
};
[[nodiscard]] std::vector<Sidecar> WriteSidecars(const std::vector<Track>& tracks, const std::string& directory,
                                                 const std::string& base_name, bool srt = true, bool vtt = true);

}  // namespace cutline::captions

#include "core/model/Types.h"

#include <stdexcept>

namespace cutline::model {
namespace {

[[noreturn]] void Unknown(const char* what, const std::string& value) {
  throw std::invalid_argument(std::string("Unknown ") + what + ": " + value);
}

}  // namespace

std::string ToString(TrackKind value) {
  switch (value) {
    case TrackKind::Video: return "video";
    case TrackKind::Audio: return "audio";
  }
  throw std::invalid_argument("Unhandled TrackKind");
}

std::string ToString(SourceKind value) {
  switch (value) {
    case SourceKind::Media: return "media";
    case SourceKind::Sequence: return "sequence";
    case SourceKind::Adjustment: return "adjustment";
  }
  throw std::invalid_argument("Unhandled SourceKind");
}

std::string ToString(EffectOwner value) {
  switch (value) {
    case EffectOwner::Clip: return "clip";
    case EffectOwner::Track: return "track";
    case EffectOwner::Sequence: return "sequence";
    case EffectOwner::Transition: return "transition";
  }
  throw std::invalid_argument("Unhandled EffectOwner");
}

std::string ToString(MarkerOwner value) {
  switch (value) {
    case MarkerOwner::Sequence: return "sequence";
    case MarkerOwner::Clip: return "clip";
    case MarkerOwner::Media: return "media";
  }
  throw std::invalid_argument("Unhandled MarkerOwner");
}

std::string ToString(MarkerKind value) {
  switch (value) {
    case MarkerKind::Comment: return "comment";
    case MarkerKind::Chapter: return "chapter";
    case MarkerKind::Segmentation: return "segmentation";
    case MarkerKind::WebLink: return "web_link";
    case MarkerKind::FlvCue: return "flv_cue";
  }
  throw std::invalid_argument("Unhandled MarkerKind");
}

std::string ToString(TransitionAlignment value) {
  switch (value) {
    case TransitionAlignment::Center: return "center";
    case TransitionAlignment::Start: return "start";
    case TransitionAlignment::End: return "end";
    case TransitionAlignment::Custom: return "custom";
  }
  throw std::invalid_argument("Unhandled TransitionAlignment");
}

std::string ToString(FieldOrder value) {
  switch (value) {
    case FieldOrder::Progressive: return "progressive";
    case FieldOrder::UpperFirst: return "upper_first";
    case FieldOrder::LowerFirst: return "lower_first";
  }
  throw std::invalid_argument("Unhandled FieldOrder");
}

std::string ToString(StreamKind value) {
  switch (value) {
    case StreamKind::Video: return "video";
    case StreamKind::Audio: return "audio";
    case StreamKind::Subtitle: return "subtitle";
    case StreamKind::Data: return "data";
  }
  throw std::invalid_argument("Unhandled StreamKind");
}

std::string ToString(Cadence value) {
  switch (value) {
    case Cadence::Constant: return "constant";
    case Cadence::Variable: return "variable";
  }
  throw std::invalid_argument("Unhandled Cadence");
}

std::string ToString(ColorRange value) {
  switch (value) {
    case ColorRange::Limited: return "limited";
    case ColorRange::Full: return "full";
  }
  throw std::invalid_argument("Unhandled ColorRange");
}

TrackKind ParseTrackKind(const std::string& value) {
  if (value == "video") return TrackKind::Video;
  if (value == "audio") return TrackKind::Audio;
  Unknown("track kind", value);
}

SourceKind ParseSourceKind(const std::string& value) {
  if (value == "media") return SourceKind::Media;
  if (value == "sequence") return SourceKind::Sequence;
  if (value == "adjustment") return SourceKind::Adjustment;
  Unknown("source kind", value);
}

EffectOwner ParseEffectOwner(const std::string& value) {
  if (value == "clip") return EffectOwner::Clip;
  if (value == "track") return EffectOwner::Track;
  if (value == "sequence") return EffectOwner::Sequence;
  if (value == "transition") return EffectOwner::Transition;
  Unknown("effect owner", value);
}

MarkerOwner ParseMarkerOwner(const std::string& value) {
  if (value == "sequence") return MarkerOwner::Sequence;
  if (value == "clip") return MarkerOwner::Clip;
  if (value == "media") return MarkerOwner::Media;
  Unknown("marker owner", value);
}

MarkerKind ParseMarkerKind(const std::string& value) {
  if (value == "comment") return MarkerKind::Comment;
  if (value == "chapter") return MarkerKind::Chapter;
  if (value == "segmentation") return MarkerKind::Segmentation;
  if (value == "web_link") return MarkerKind::WebLink;
  if (value == "flv_cue") return MarkerKind::FlvCue;
  Unknown("marker kind", value);
}

TransitionAlignment ParseTransitionAlignment(const std::string& value) {
  if (value == "center") return TransitionAlignment::Center;
  if (value == "start") return TransitionAlignment::Start;
  if (value == "end") return TransitionAlignment::End;
  if (value == "custom") return TransitionAlignment::Custom;
  Unknown("transition alignment", value);
}

FieldOrder ParseFieldOrder(const std::string& value) {
  if (value == "progressive") return FieldOrder::Progressive;
  if (value == "upper_first") return FieldOrder::UpperFirst;
  if (value == "lower_first") return FieldOrder::LowerFirst;
  Unknown("field order", value);
}

StreamKind ParseStreamKind(const std::string& value) {
  if (value == "video") return StreamKind::Video;
  if (value == "audio") return StreamKind::Audio;
  if (value == "subtitle") return StreamKind::Subtitle;
  if (value == "data") return StreamKind::Data;
  Unknown("stream kind", value);
}

Cadence ParseCadence(const std::string& value) {
  if (value == "constant") return Cadence::Constant;
  if (value == "variable") return Cadence::Variable;
  Unknown("cadence", value);
}

ColorRange ParseColorRange(const std::string& value) {
  if (value == "limited") return ColorRange::Limited;
  if (value == "full") return ColorRange::Full;
  Unknown("color range", value);
}

}  // namespace cutline::model

// Caption authoring for the desktop application. The format parser, project data and compositor
// already live below the UI; this file keeps the panel thin and makes every mutation undoable.

#include "app/Session.h"

#include "captions/Captions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace cutline::app {
namespace {

using time::RationalTime;

QString LocalCaptionPath(const QString& path_or_url) {
  const QUrl url(path_or_url);
  return url.isLocalFile() ? url.toLocalFile() : path_or_url;
}

RationalTime CaptionTime(double seconds) {
  if (!std::isfinite(seconds)) return {0, 1};
  return {static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * 1000.0)), 1000};
}

double CaptionSeconds(const RationalTime& time) {
  return static_cast<double>(time.numerator()) / static_cast<double>(std::max<std::int64_t>(1, time.denominator()));
}

const captions::Track* FindTrack(const timeline::Sequence* sequence, const std::string& id) {
  if (sequence == nullptr) return nullptr;
  const auto found = std::find_if(sequence->caption_tracks.begin(), sequence->caption_tracks.end(),
                                  [&](const captions::Track& track) { return track.id == id; });
  return found == sequence->caption_tracks.end() ? nullptr : &*found;
}

const captions::Cue* FindCue(const timeline::Sequence* sequence, const std::string& id) {
  if (sequence == nullptr) return nullptr;
  for (const auto& track : sequence->caption_tracks) {
    const auto found = std::find_if(track.cues.begin(), track.cues.end(), [&](const captions::Cue& cue) { return cue.id == id; });
    if (found != track.cues.end()) return &*found;
  }
  return nullptr;
}

commands::CaptionCuePayload CuePayload(const captions::Cue& cue) {
  commands::CaptionCuePayload payload;
  payload.id = cue.id;
  payload.start = cue.start;
  payload.end = cue.end;
  payload.text = cue.text;
  payload.style_json = cue.style_json;
  payload.speaker = cue.speaker;
  return payload;
}

}  // namespace

QVariantList Session::captionTracks() const {
  QVariantList result;
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return result;
  for (const auto& track : sequence->caption_tracks) {
    const auto style = captions::ParseStyle(track.style_json);
    result.push_back(QVariantMap{{"id", QString::fromStdString(track.id)},
                                 {"name", QString::fromStdString(track.name)},
                                 {"language", QString::fromStdString(track.language)},
                                 {"cueCount", static_cast<int>(track.cues.size())},
                                 {"family", QString::fromStdString(style.family)},
                                 {"size", style.size}, {"bold", style.bold}, {"italic", style.italic},
                                 {"align", QString::fromStdString(style.align)},
                                 {"position", QString::fromStdString(style.position)},
                                 {"outlineWidth", style.outline_width},
                                 {"backgroundOpacity", style.background[3]},
                                 {"maxWidth", style.max_width}});
  }
  return result;
}

QVariantList Session::captions() const {
  QVariantList result;
  const auto* track = FindTrack(graph_.root(), caption_track_);
  if (track == nullptr) return result;
  for (const auto& cue : track->cues) {
    result.push_back(QVariantMap{{"id", QString::fromStdString(cue.id)},
                                 {"start", CaptionSeconds(cue.start)}, {"end", CaptionSeconds(cue.end)},
                                 {"duration", CaptionSeconds(cue.end.Subtract(cue.start))},
                                 {"text", QString::fromStdString(cue.text)},
                                 {"speaker", QString::fromStdString(cue.speaker)}});
  }
  return result;
}

void Session::captionSelectTrack(const QString& id) {
  const auto wanted = id.toStdString();
  if (caption_track_ == wanted || FindTrack(graph_.root(), wanted) == nullptr) return;
  caption_track_ = wanted;
  emit captionsChanged();
}

QString Session::captionAddTrack(const QString& requested_name, const QString& requested_language) {
  if (graph_.root() == nullptr) return {};
  const auto id = NewId("captions");
  commands::AddCaptionTrackPayload track;
  track.id = id;
  track.sequence_id = sequence_id_;
  track.name = requested_name.trimmed().isEmpty() ? tr("Captions").toStdString() : requested_name.trimmed().toStdString();
  track.language = requested_language.trimmed().left(35).toStdString();
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Add Caption Track";
  plan.commands.push_back({commands::CommandType::AddCaptionTrack, track});
  if (!Apply(plan)) return {};
  caption_track_ = id;
  emit captionsChanged();
  return QString::fromStdString(id);
}

bool Session::captionUpdateTrack(const QString& id, const QString& name, const QString& language) {
  const auto* track = FindTrack(graph_.root(), id.toStdString());
  if (track == nullptr) return false;
  commands::UpdateCaptionTrackPayload update;
  update.id = track->id;
  update.name = name.trimmed().isEmpty() ? track->name : name.trimmed().toStdString();
  update.language = language.trimmed().left(35).toStdString();
  update.style_json = track->style_json;
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Update Caption Track";
  plan.commands.push_back({commands::CommandType::UpdateCaptionTrack, update});
  return Apply(plan);
}

bool Session::captionSetTrackStyle(const QString& id, const QString& family, double size, bool bold, bool italic,
                                   const QString& align, const QString& position, double outline_width,
                                   double background_opacity, double max_width) {
  const auto* track = FindTrack(graph_.root(), id.toStdString());
  if (track == nullptr) return false;
  auto style = captions::ParseStyle(track->style_json);
  if (!family.trimmed().isEmpty()) style.family = family.trimmed().toStdString();
  style.size = std::clamp(size, 0.005, 0.5);
  style.bold = bold;
  style.italic = italic;
  const auto requested_align = align.toStdString();
  style.align = requested_align == "left" || requested_align == "right" ? requested_align : "center";
  const auto requested_position = position.toStdString();
  style.position = requested_position == "top" || requested_position == "middle" ? requested_position : "bottom";
  style.outline_width = std::clamp(outline_width, 0.0, 0.05);
  style.background[3] = std::clamp(background_opacity, 0.0, 1.0);
  style.max_width = std::clamp(max_width, 0.1, 1.0);
  commands::UpdateCaptionTrackPayload update{track->id, track->name, track->language, captions::StyleToJson(style)};
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Style Caption Track";
  plan.commands.push_back({commands::CommandType::UpdateCaptionTrack, update});
  return Apply(plan);
}

bool Session::captionRemoveTrack(const QString& id) {
  if (FindTrack(graph_.root(), id.toStdString()) == nullptr) return false;
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Remove Caption Track";
  plan.commands.push_back({commands::CommandType::RemoveCaptionTrack, commands::RemoveCaptionTrackPayload{id.toStdString()}});
  return Apply(plan);
}

QString Session::captionAdd(const QString& requested_track, double start, double end, const QString& text, const QString& speaker) {
  const auto track_id = requested_track.isEmpty() ? caption_track_ : requested_track.toStdString();
  if (FindTrack(graph_.root(), track_id) == nullptr || text.trimmed().isEmpty() || !std::isfinite(start) || !std::isfinite(end) || end <= start) {
    ShowStatus(tr("A caption needs a track, text, and an end after its start"));
    return {};
  }
  commands::CaptionCuePayload cue;
  cue.id = NewId("caption");
  cue.start = CaptionTime(start);
  cue.end = CaptionTime(end);
  cue.text = text.trimmed().toStdString();
  cue.speaker = speaker.trimmed().toStdString();
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Add Caption";
  plan.commands.push_back({commands::CommandType::AddCaptions, commands::AddCaptionsPayload{track_id, {cue}}});
  return Apply(plan) ? QString::fromStdString(cue.id) : QString{};
}

bool Session::captionUpdate(const QString& id, double start, double end, const QString& text, const QString& speaker) {
  const auto* stored = FindCue(graph_.root(), id.toStdString());
  if (stored == nullptr || text.trimmed().isEmpty() || !std::isfinite(start) || !std::isfinite(end) || end <= start) {
    ShowStatus(tr("A caption needs text and an end after its start"));
    return false;
  }
  auto cue = CuePayload(*stored);
  cue.start = CaptionTime(start);
  cue.end = CaptionTime(end);
  cue.text = text.trimmed().toStdString();
  cue.speaker = speaker.trimmed().toStdString();
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Edit Caption";
  plan.commands.push_back({commands::CommandType::UpdateCaption, commands::UpdateCaptionPayload{cue}});
  return Apply(plan);
}

bool Session::captionRemove(const QString& id) {
  if (FindCue(graph_.root(), id.toStdString()) == nullptr) return false;
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Remove Caption";
  plan.commands.push_back({commands::CommandType::RemoveCaptions, commands::RemoveCaptionsPayload{{id.toStdString()}}});
  return Apply(plan);
}

bool Session::captionImport(const QString& requested_track, const QString& path_or_url) {
  const auto path = LocalCaptionPath(path_or_url);
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    ShowStatus(tr("Could not open the caption file"));
    return false;
  }
  const auto bytes = input.readAll();
  auto parsed = captions::ParseCaptions(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));
  if (parsed.cues.empty()) {
    ShowStatus(parsed.issues.empty() ? tr("The caption file contains no cues")
                                     : QString::fromStdString(parsed.issues.front().message));
    return false;
  }
  std::string track_id = requested_track.toStdString();
  ui::EditPlan plan;
  plan.ok = true;
  plan.label = "Import Captions";
  if (FindTrack(graph_.root(), track_id) == nullptr) {
    track_id = NewId("captions");
    const auto language = QFileInfo(path).completeBaseName().section('.', -1).left(35).toStdString();
    plan.commands.push_back({commands::CommandType::AddCaptionTrack,
                             commands::AddCaptionTrackPayload{track_id, sequence_id_, QFileInfo(path).baseName().toStdString(), language, "{}"}});
  }
  commands::AddCaptionsPayload add;
  add.track_id = track_id;
  for (auto& imported : parsed.cues) {
    imported.id = NewId("caption");
    add.cues.push_back(CuePayload(imported));
  }
  plan.commands.push_back({commands::CommandType::AddCaptions, std::move(add)});
  if (!Apply(plan)) return false;
  caption_track_ = track_id;
  emit captionsChanged();
  ShowStatus(tr("Imported %1 captions (%2 issues)").arg(parsed.cues.size()).arg(parsed.issues.size()));
  return true;
}

bool Session::captionExport(const QString& requested_track, const QString& path_or_url, const QString& format) {
  const auto* track = FindTrack(graph_.root(), requested_track.toStdString());
  if (track == nullptr) return false;
  const auto path = LocalCaptionPath(path_or_url);
  const auto lowered = format.trimmed().toLower();
  const auto written = lowered == "vtt" ? captions::WriteWebVtt(track->cues) : captions::WriteSrt(track->cues);
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile output(path);
  if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    ShowStatus(tr("Could not write the caption file"));
    return false;
  }
  const auto bytes = QByteArray::fromStdString(written.text);
  if (output.write(bytes) != bytes.size() || !output.flush()) {
    ShowStatus(tr("The caption file could not be completed"));
    return false;
  }
  ShowStatus(tr("Exported %1 captions to %2").arg(track->cues.size()).arg(QFileInfo(path).fileName()));
  return true;
}

}  // namespace cutline::app

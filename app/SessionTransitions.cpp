// Transitions in the application: choosing one from the library for the cut at (or nearest) the playhead, and changing or
// removing one that is on the timeline. The rules are ui/Transitions.h and the drawing is render/Transitions.h.

#include "app/Session.h"

#include "render/Transitions.h"
#include "ui/Transitions.h"

namespace cutline::app {

using time::RationalTime;

QVariantList Session::transitionCatalogue(const QString& query) const {
  QVariantList list;
  const auto needle = query.trimmed().toLower();
  for (const auto& info : render::TransitionLibrary()) {
    const auto name = QString::fromStdString(info.name);
    if (!needle.isEmpty() && !name.toLower().contains(needle) && !QString::fromStdString(info.category).toLower().contains(needle)) continue;
    list << QVariantMap{{"id", QString::fromStdString(info.id)}, {"name", name}, {"category", QString::fromStdString(info.category)}, {"description", QString::fromStdString(info.description)}};
  }
  return list;
}

bool Session::addTransition(const QString& kind, double seconds) {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return false;
  // The cut that is meant: on the chosen clip's track if a clip is chosen, else the nearest on any picture track, within two seconds.
  std::string track;
  if (const auto selected = PrimaryClip()) {
    for (const auto& t : sequence->tracks) {
      for (const auto& clip : t.clips) {
        if (clip.id == *selected && t.kind == model::TrackKind::Video) track = t.id;
      }
    }
  }
  const auto sites = ui::TransitionSitesNear(*sequence, transport_.position(), RationalTime(2, 1), track);
  if (sites.empty()) {
    ShowStatus(tr("Move the playhead to a cut (or the edge of a clip) first"));
    return false;
  }
  ui::TransitionRequest request;
  request.kind = kind.toStdString();
  request.duration = RationalTime(static_cast<std::int64_t>(std::llround(std::max(seconds, 0.04) * 1000.0)), 1000);
  // Centred on the cut is the usual way; where the clips have no spare picture on one side it is tried starting at the cut, then ending there.
  auto plan = ui::PlanAddTransition(EditContextFor(), sites.front(), request);
  for (const auto alignment : {model::TransitionAlignment::Start, model::TransitionAlignment::End}) {
    if (plan.ok || !sites.front().cut()) break;
    request.alignment = alignment;
    auto attempt = ui::PlanAddTransition(EditContextFor(), sites.front(), request);
    if (attempt.ok) plan = std::move(attempt);
  }
  return Apply(plan);
}

QVariantMap Session::transitionInfo(const QString& id) const {
  const auto* sequence = graph_.root();
  if (sequence == nullptr) return {};
  for (const auto& track : sequence->tracks) {
    for (const auto& transition : track.transitions) {
      if (transition.id != id.toStdString()) continue;
      const auto seconds = static_cast<double>(transition.duration.numerator()) / static_cast<double>(std::max<std::int64_t>(1, transition.duration.denominator()));
      return QVariantMap{{"id", id},
                         {"kind", QString::fromStdString(transition.kind)},
                         {"seconds", seconds},
                         {"alignment", QString::fromStdString(model::ToString(transition.alignment))},
                         {"track", QString::fromStdString(track.id)},
                         {"oneSided", !(transition.from_clip_id && transition.to_clip_id)}};
    }
  }
  return {};
}

bool Session::changeTransition(const QString& id, const QString& kind, double seconds, const QString& alignment) {
  ui::TransitionRequest request;
  request.kind = kind.toStdString();
  request.duration = RationalTime(static_cast<std::int64_t>(std::llround(std::max(seconds, 0.04) * 1000.0)), 1000);
  request.alignment = alignment == "start" ? model::TransitionAlignment::Start : alignment == "end" ? model::TransitionAlignment::End : model::TransitionAlignment::Center;
  return Apply(ui::PlanChangeTransition(EditContextFor(), id.toStdString(), request));
}

bool Session::removeTransition(const QString& id) { return Apply(ui::PlanRemoveTransition(EditContextFor(), id.toStdString())); }

void Session::openTransition(const QString& id) { emit transitionRequested(id); }

}  // namespace cutline::app

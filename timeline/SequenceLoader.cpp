#include "timeline/SequenceLoader.h"

#include "core/db/Sql.h"
#include "core/project/ProjectStore.h"
#include "effects/GraphicsDocument.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cutline::timeline {
namespace {

using db::Statement;

// Effects are loaded for a whole sequence in three queries rather than per
// owner, then handed out by owner id. A per-clip query would turn loading a
// long sequence into thousands of round trips.
struct EffectIndex final {
  std::unordered_map<std::string, std::vector<Effect>> by_owner;

  [[nodiscard]] std::vector<Effect> Take(const std::string& owner_id) {
    const auto found = by_owner.find(owner_id);
    if (found == by_owner.end()) return {};
    auto effects = std::move(found->second);
    by_owner.erase(found);
    return effects;
  }
};

[[nodiscard]] anim::Value ValueFrom(const Statement& statement, int first, int dimension) {
  anim::Value value;
  value.dimension = dimension;
  for (int index = 0; index < 4; ++index) {
    value.components[static_cast<std::size_t>(index)] = statement.ColumnDouble(first + index);
  }
  return value;
}

// Loads every effect owned by anything in this sequence, with its parameters and
// keyframes attached.
// The effects this sequence owns, directly or through its tracks, clips and
// transitions. Every query below is scoped by it; the parameter and keyframe
// queries used to read the whole project, and nesting repeated that per
// sequence, so a project with many sequences paid for all of them every time.
// Every query that uses this puts `scoped` first with CROSS JOIN. SQLite takes
// CROSS JOIN as a fixed order, so the (small) scoped set drives and the large
// tables are reached through their indexes. Left to choose, the planner scans
// effect_parameters or keyframes and probes the scope, which is linear in the
// size of the whole project -- the very thing the scoping is meant to avoid.
constexpr const char* kScopedEffects = R"sql(
  scoped(id) AS (
    -- One indexed lookup per kind of owner, unioned. The first version of this
    -- was a single OR of IN-subqueries, which SQLite answers by scanning every
    -- effect in the project and testing membership: correct, but it made loading
    -- a small sequence slower than before when the project had no other sequences.
    -- Each branch here uses effects_owner(owner_kind, owner_id) directly.
    SELECT e.id FROM effects e
     WHERE e.owner_kind = 'sequence' AND e.owner_id = ?1
    UNION ALL
    SELECT e.id FROM tracks t
      CROSS JOIN effects e ON e.owner_kind = 'track' AND e.owner_id = t.id
     WHERE t.sequence_id = ?1
    UNION ALL
    SELECT e.id FROM tracks t
      CROSS JOIN clips c ON c.track_id = t.id
      CROSS JOIN effects e ON e.owner_kind = 'clip' AND e.owner_id = c.id
     WHERE t.sequence_id = ?1
    UNION ALL
    SELECT e.id FROM tracks t
      CROSS JOIN transitions tr ON tr.track_id = t.id
      CROSS JOIN effects e ON e.owner_kind = 'transition' AND e.owner_id = tr.id
     WHERE t.sequence_id = ?1
  )
)sql";

// An image element bound to project media (asset "media:<id>") is given the media's file, so the compositor, which
// reads files and not the project, can draw it. The bundle is returned as it was when nothing is bound.
[[nodiscard]] std::string BindMediaAssets(sqlite3* database, const std::string& bundle) {
  if (bundle.find("\"media:") == std::string::npos) return bundle;
  try {
    auto document = render::graphics::ResolveBundle(bundle);
    bool changed = false;
    for (auto& element : document.elements) {
      if (element.type != render::graphics::ElementType::Image || element.asset.rfind("media:", 0) != 0) continue;
      Statement media(database, "SELECT original_path FROM media WHERE id = ?;");
      media.Bind(1, element.asset.substr(6));
      if (!media.Step()) continue;
      element.asset = media.ColumnText(0);
      changed = true;
    }
    return changed ? render::graphics::MakeGraphicBundle(render::graphics::ToJson(document)) : bundle;
  } catch (const std::exception&) {
    return bundle;
  }
}

// The project's own graphic named by a project:<id> reference, as the one JSON text the compositor reads.
// Empty when there is no such graphic.
[[nodiscard]] std::string ProjectGraphicBundle(sqlite3* database, const std::string& graphic_id) {
  Statement graphic(database, "SELECT kind, document_json, template_id, template_version, values_json FROM graphics WHERE id = ?;");
  graphic.Bind(1, graphic_id);
  if (!graphic.Step()) return {};
  if (graphic.ColumnText(0) == "graphic") {
    return "{\"kind\":\"graphic\",\"document\":" + graphic.ColumnText(1) + "}";
  }
  Statement package(database, "SELECT package_json FROM graphic_templates WHERE id = ? AND version = ?;");
  package.Bind(1, graphic.ColumnText(2)).Bind(2, graphic.ColumnInt(3));
  if (!package.Step()) return {};
  return "{\"kind\":\"template\",\"package\":" + package.ColumnText(0) + ",\"values\":" + graphic.ColumnText(4) + "}";
}

[[nodiscard]] EffectIndex LoadEffects(sqlite3* database, const std::string& sequence_id, LoadStatistics* statistics) {
  EffectIndex index;
  std::unordered_map<std::string, std::string> project_graphics;
  std::unordered_map<std::string, Effect*> effects_by_id;
  std::vector<std::pair<std::string, Effect>> ordered;

  {
    // Every effect whose owner belongs to this sequence: the sequence itself,
    // one of its tracks, a clip on one of those tracks, or a transition on one.
    Statement statement(database, std::string("WITH ") + kScopedEffects + R"sql(
      SELECT e.id, e.owner_kind, e.owner_id, e.effect_type, e.sort_order, e.enabled, e.intrinsic, e.preset_name
        FROM scoped s CROSS JOIN effects e ON e.id = s.id
       ORDER BY e.owner_id, e.sort_order;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      Effect effect;
      effect.id = statement.ColumnText(0);
      effect.effect_type = statement.ColumnText(3);
      effect.order = statement.ColumnInt(4);
      effect.enabled = statement.ColumnInt(5) != 0;
      effect.intrinsic = statement.ColumnInt(6) != 0;
      effect.preset_name = statement.ColumnText(7);
      if (effect.preset_name.rfind("project:", 0) == 0) {
        const auto graphic_id = effect.preset_name.substr(8);
        auto found = project_graphics.find(graphic_id);
        if (found == project_graphics.end()) found = project_graphics.emplace(graphic_id, BindMediaAssets(database, ProjectGraphicBundle(database, graphic_id))).first;
        effect.inline_asset = found->second;
      }
      ordered.emplace_back(statement.ColumnText(2), std::move(effect));
      if (statistics != nullptr) ++statistics->effects;
    }
  }

  // Parameters, keyed to their effect.
  std::unordered_map<std::string, std::vector<Parameter>> parameters_by_effect;
  std::unordered_map<std::string, std::vector<EffectMask>> masks_by_effect;
  std::unordered_map<std::string, std::vector<anim::Keyframe>> keyframes_by_parameter;
  {
    Statement statement(database, std::string("WITH ") + kScopedEffects + R"sql(
      SELECT k.parameter_id, k.time_num, k.time_den, k.c0, k.c1, k.c2, k.c3,
             k.interpolation, k.out_handle_x, k.out_handle_y, k.in_handle_x, k.in_handle_y, p.dimension
        FROM scoped s
        CROSS JOIN effect_parameters p ON p.effect_id = s.id
        CROSS JOIN keyframes k ON k.parameter_id = p.id
       ORDER BY k.parameter_id, k.time_ticks;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      if (statistics != nullptr) ++statistics->keyframes;
      anim::Keyframe keyframe;
      keyframe.time = {statement.ColumnInt(1), statement.ColumnInt(2)};
      keyframe.value = ValueFrom(statement, 3, static_cast<int>(statement.ColumnInt(12)));
      keyframe.interpolation = anim::ParseInterpolation(statement.ColumnText(7));
      keyframe.out_handle = {statement.ColumnDouble(8), statement.ColumnDouble(9)};
      keyframe.in_handle = {statement.ColumnDouble(10), statement.ColumnDouble(11)};
      keyframes_by_parameter[statement.ColumnText(0)].push_back(std::move(keyframe));
    }
  }

  {
    Statement statement(database, std::string("WITH ") + kScopedEffects + R"sql(
      SELECT m.id, m.effect_id, m.sort_order, m.document_json
        FROM scoped s CROSS JOIN effect_masks m ON m.effect_id = s.id
       ORDER BY m.effect_id, m.sort_order;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      EffectMask mask;
      mask.id = statement.ColumnText(0);
      mask.order = statement.ColumnInt(2);
      mask.document = effects::mask::Parse(statement.ColumnText(3));
      masks_by_effect[statement.ColumnText(1)].push_back(std::move(mask));
    }
  }
  {
    Statement statement(database, std::string("WITH ") + kScopedEffects + R"sql(
      SELECT p.id, p.effect_id, p.name, p.dimension, p.c0, p.c1, p.c2, p.c3
        FROM scoped s CROSS JOIN effect_parameters p ON p.effect_id = s.id
       ORDER BY p.effect_id, p.name;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      if (statistics != nullptr) ++statistics->parameters;
      Parameter parameter;
      parameter.id = statement.ColumnText(0);
      parameter.name = statement.ColumnText(2);
      const auto dimension = static_cast<int>(statement.ColumnInt(3));
      const auto found = keyframes_by_parameter.find(parameter.id);
      if (found != keyframes_by_parameter.end() && !found->second.empty()) {
        parameter.value = anim::AnimatedValue(std::move(found->second));
      } else {
        parameter.value = anim::AnimatedValue(ValueFrom(statement, 4, dimension));
      }
      parameters_by_effect[statement.ColumnText(1)].push_back(std::move(parameter));
    }
  }

  for (auto& [owner_id, effect] : ordered) {
    const auto found = parameters_by_effect.find(effect.id);
    if (found != parameters_by_effect.end()) effect.parameters = std::move(found->second);
    const auto masks = masks_by_effect.find(effect.id);
    if (masks != masks_by_effect.end()) effect.masks = std::move(masks->second);
    index.by_owner[owner_id].push_back(std::move(effect));
  }
  (void)effects_by_id;
  return index;
}

}  // namespace

Sequence LoadSequence(sqlite3* database, const std::string& sequence_id, LoadStatistics* statistics) {
  Sequence sequence;
  {
    Statement statement(database, R"sql(
      SELECT id, name, frame_rate_num, frame_rate_den, width, height,
             pixel_aspect_num, pixel_aspect_den, sample_rate, channel_layout,
             working_color_space, display_color_space, field_order, drop_frame, render_version
        FROM sequences WHERE id = ?;
    )sql");
    statement.Bind(1, sequence_id);
    if (!statement.Step()) throw std::runtime_error("Unknown sequence: " + sequence_id);
    sequence.id = statement.ColumnText(0);
    sequence.name = statement.ColumnText(1);
    sequence.frame_rate = {statement.ColumnInt(2), statement.ColumnInt(3)};
    sequence.width = statement.ColumnInt(4);
    sequence.height = statement.ColumnInt(5);
    sequence.pixel_aspect = {statement.ColumnInt(6), statement.ColumnInt(7)};
    sequence.sample_rate = statement.ColumnInt(8);
    sequence.channel_layout = statement.ColumnText(9);
    sequence.working_color_space = statement.ColumnText(10);
    sequence.display_color_space = statement.ColumnText(11);
    sequence.field_order = model::ParseFieldOrder(statement.ColumnText(12));
    sequence.drop_frame = statement.ColumnInt(13) != 0;
    sequence.render_version = statement.ColumnInt(14);
    // Rules this build does not have cannot be followed; say so here, where the
    // snapshot is made, rather than render with the nearest ones.
    (void)model::RenderSemantics::For(sequence.render_version);
  }
  sequence.source_revision = db::ScalarInt(database, "SELECT revision FROM project_meta WHERE singleton = 1;");

  auto effects = LoadEffects(database, sequence_id, statistics);
  sequence.effects = effects.Take(sequence_id);

  // Tracks, video first then audio, each ascending by order. The compiler sorts
  // its output anyway, but loading in composite order keeps the snapshot easy to
  // read when debugging.
  std::unordered_map<std::string, std::size_t> track_slots;
  {
    Statement statement(database, R"sql(
      SELECT id, track_type, sort_order, locked, muted, solo, channel_layout, gain_db, pan, name,
             is_bus, output_bus_id
        FROM tracks WHERE sequence_id = ?
       ORDER BY CASE track_type WHEN 'video' THEN 0 ELSE 1 END, sort_order;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      Track track;
      track.id = statement.ColumnText(0);
      track.kind = model::ParseTrackKind(statement.ColumnText(1));
      track.order = statement.ColumnInt(2);
      track.locked = statement.ColumnInt(3) != 0;
      track.muted = statement.ColumnInt(4) != 0;
      track.solo = statement.ColumnInt(5) != 0;
      track.channel_layout = statement.ColumnText(6);
      track.gain_db = statement.ColumnDouble(7);
      track.pan = statement.ColumnDouble(8);
      track.name = statement.ColumnText(9);
      track.is_bus = statement.ColumnInt(10) != 0;
      track.output_bus_id = statement.ColumnText(11);
      track.effects = effects.Take(track.id);
      track_slots.emplace(track.id, sequence.tracks.size());
      sequence.tracks.push_back(std::move(track));
    }
  }

  {
    Statement statement(database, R"sql(
      SELECT s.track_id, s.bus_id, s.gain_db, s.pre_fader
        FROM track_sends s JOIN tracks t ON t.id = s.track_id
       WHERE t.sequence_id = ?
       ORDER BY s.track_id, s.bus_id;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      const auto found = track_slots.find(statement.ColumnText(0));
      if (found == track_slots.end()) continue;
      sequence.tracks[found->second].sends.push_back(
          {statement.ColumnText(1), statement.ColumnDouble(2), statement.ColumnInt(3) != 0});
    }
  }

  {
    Statement statement(database, R"sql(
      SELECT id, track_id, source_kind, media_id, nested_sequence_id,
             source_in_num, source_in_den, source_out_num, source_out_den,
             timeline_start_num, timeline_start_den, timeline_start_ticks, timeline_end_ticks,
             rate_num, rate_den, reversed, enabled, linked_group, name, maintain_pitch, audio_role
        FROM clips
       WHERE track_id IN (SELECT id FROM tracks WHERE sequence_id = ?)
       ORDER BY track_id, timeline_start_ticks;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      Clip clip;
      clip.id = statement.ColumnText(0);
      const auto track_id = statement.ColumnText(1);
      clip.source_kind = model::ParseSourceKind(statement.ColumnText(2));
      // Exactly one of these is non-null, enforced by a schema CHECK.
      clip.source_id = clip.source_kind == model::SourceKind::Sequence ? statement.ColumnText(4)
                                                                        : statement.ColumnText(3);
      clip.source_in = {statement.ColumnInt(5), statement.ColumnInt(6)};
      clip.source_out = {statement.ColumnInt(7), statement.ColumnInt(8)};
      clip.timeline_start = {statement.ColumnInt(9), statement.ColumnInt(10)};
      clip.start_ticks = statement.ColumnInt(11);
      clip.end_ticks = statement.ColumnInt(12);
      clip.playback_rate = {statement.ColumnInt(13), statement.ColumnInt(14)};
      clip.reversed = statement.ColumnInt(15) != 0;
      clip.enabled = statement.ColumnInt(16) != 0;
      clip.linked_group = statement.ColumnText(17);
      clip.name = statement.ColumnText(18);
      clip.maintain_pitch = statement.ColumnInt(19) != 0;
      clip.audio_role = statement.ColumnText(20);
      clip.effects = effects.Take(clip.id);

      const auto slot = track_slots.find(track_id);
      if (slot != track_slots.end()) sequence.tracks[slot->second].clips.push_back(std::move(clip));
    }
  }

  {
    Statement statement(database, R"sql(
      SELECT id, track_id, kind, alignment, from_clip_id, to_clip_id,
             timeline_start_num, timeline_start_den, duration_num, duration_den,
             timeline_start_ticks, timeline_end_ticks
        FROM transitions
       WHERE track_id IN (SELECT id FROM tracks WHERE sequence_id = ?)
       ORDER BY track_id, timeline_start_ticks;
    )sql");
    statement.Bind(1, sequence_id);
    while (statement.Step()) {
      Transition transition;
      transition.id = statement.ColumnText(0);
      const auto track_id = statement.ColumnText(1);
      transition.kind = statement.ColumnText(2);
      transition.alignment = model::ParseTransitionAlignment(statement.ColumnText(3));
      transition.from_clip_id = statement.ColumnOptionalText(4);
      transition.to_clip_id = statement.ColumnOptionalText(5);
      transition.timeline_start = {statement.ColumnInt(6), statement.ColumnInt(7)};
      transition.duration = {statement.ColumnInt(8), statement.ColumnInt(9)};
      transition.start_ticks = statement.ColumnInt(10);
      transition.end_ticks = statement.ColumnInt(11);
      transition.effects = effects.Take(transition.id);

      const auto slot = track_slots.find(track_id);
      if (slot != track_slots.end()) sequence.tracks[slot->second].transitions.push_back(std::move(transition));
    }
  }

  {
    Statement tracks(database, "SELECT id, name, language, style_json FROM caption_tracks WHERE sequence_id = ? ORDER BY sort_order;");
    tracks.Bind(1, sequence_id);
    while (tracks.Step()) {
      captions::Track track;
      track.id = tracks.ColumnText(0);
      track.name = tracks.ColumnText(1);
      track.language = tracks.ColumnText(2);
      track.style_json = tracks.ColumnText(3);
      Statement cues(database, R"sql(
        SELECT id, start_num, start_den, end_num, end_den, text, style_json, speaker
          FROM captions WHERE track_id = ? ORDER BY start_ticks, id;
      )sql");
      cues.Bind(1, track.id);
      while (cues.Step()) {
        captions::Cue cue;
        cue.id = cues.ColumnText(0);
        cue.start = {cues.ColumnInt(1), cues.ColumnInt(2)};
        cue.end = {cues.ColumnInt(3), cues.ColumnInt(4)};
        cue.text = cues.ColumnText(5);
        cue.style_json = cues.ColumnText(6);
        cue.speaker = cues.ColumnText(7);
        cue.resolved_style_json = captions::MergeStyles(track.style_json, cue.style_json);
        track.cues.push_back(std::move(cue));
      }
      sequence.caption_tracks.push_back(std::move(track));
    }
  }

  // The compiler binary searches clips, so the sort order is a precondition
  // rather than a convenience. ORDER BY above provides it; this asserts it.
  for (const auto& track : sequence.tracks) {
    for (std::size_t index = 1; index < track.clips.size(); ++index) {
      if (track.clips[index - 1].start_ticks > track.clips[index].start_ticks) {
        throw std::logic_error("Clips on track " + track.id + " were loaded out of order");
      }
    }
  }
  return sequence;
}

SequenceGraph LoadSequenceGraph(sqlite3* database, const std::string& sequence_id, int max_depth) {
  SequenceGraph graph;
  std::unordered_set<std::string> loaded;
  // Breadth-first so the root stays at index 0, which the compiler relies on.
  std::vector<std::pair<std::string, int>> pending{{sequence_id, 0}};
  while (!pending.empty()) {
    const auto [current, depth] = pending.front();
    pending.erase(pending.begin());
    if (!loaded.insert(current).second) continue;

    auto sequence = LoadSequence(database, current);
    if (depth < max_depth) {
      for (const auto& track : sequence.tracks) {
        for (const auto& clip : track.clips) {
          if (clip.source_kind == model::SourceKind::Sequence && !loaded.count(clip.source_id)) {
            pending.emplace_back(clip.source_id, depth + 1);
          }
        }
      }
    }
    graph.sequences.push_back(std::move(sequence));
  }
  return graph;
}

SequenceGraph LoadSequenceGraph(const project::ProjectStore& store, const std::string& sequence_id, int max_depth) {
  const std::lock_guard<std::mutex> lock(store.mutex());
  return LoadSequenceGraph(store.connection(), sequence_id, max_depth);
}

}  // namespace cutline::timeline

#include "ui/Inspector.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace cutline::ui {
namespace {

using commands::CommandType;

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

const timeline::Clip* FindClipIn(const timeline::Sequence& sequence, const std::string& id, const timeline::Track** track = nullptr) {
  for (const auto& t : sequence.tracks) {
    for (const auto& clip : t.clips) {
      if (clip.id == id) {
        if (track != nullptr) *track = &t;
        return &clip;
      }
    }
  }
  return nullptr;
}

const timeline::Effect* FindEffectIn(const timeline::Clip& clip, const std::string& id) {
  for (const auto& effect : clip.effects) {
    if (effect.id == id) return &effect;
  }
  return nullptr;
}

const timeline::Parameter* FindParameterIn(const timeline::Effect& effect, const std::string& name) {
  for (const auto& parameter : effect.parameters) {
    if (parameter.name == name) return &parameter;
  }
  return nullptr;
}

anim::Interpolation InterpolationBefore(const anim::AnimatedValue& value, const RationalTime& at) {
  anim::Interpolation found = anim::Interpolation::Linear;
  for (const auto& key : value.keyframes()) {
    if (key.time.Compare(at) <= 0) found = key.interpolation;
  }
  return found;
}

bool IsRendered(const effects::EffectDescriptor& descriptor) { return descriptor.medium == effects::Medium::Video && descriptor.cpu_available; }

}  // namespace

InspectorState BuildInspector(const timeline::Clip& clip, const RationalTime& local_time) {
  InspectorState state;
  state.clip_id = clip.id;
  state.clip_name = clip.name.empty() ? clip.id : clip.name;
  std::vector<const timeline::Effect*> ordered;
  for (const auto& effect : clip.effects) ordered.push_back(&effect);
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->order < b->order; });
  for (const auto* effect : ordered) {
    EffectRow row;
    row.id = effect->id;
    row.type = effect->effect_type;
    row.enabled = effect->enabled;
    row.intrinsic = effect->intrinsic;
    row.order = effect->order;
    row.preset_name = effect->preset_name;
    const auto* descriptor = effects::FindEffect(effect->effect_type);
    row.known = descriptor != nullptr;
    row.display_name = descriptor != nullptr ? descriptor->display_name : effect->effect_type;
    row.category = descriptor != nullptr ? descriptor->category : "Other";
    if (descriptor != nullptr) row.asset_kind = descriptor->asset_kind;
    // Registry order for what the registry knows, then anything else the clip holds.
    std::vector<const timeline::Parameter*> parameters;
    if (descriptor != nullptr) {
      for (const auto& declared : descriptor->parameters) {
        if (const auto* parameter = FindParameterIn(*effect, declared.id)) parameters.push_back(parameter);
      }
    }
    for (const auto& parameter : effect->parameters) {
      if (std::find(parameters.begin(), parameters.end(), &parameter) == parameters.end()) parameters.push_back(&parameter);
    }
    for (const auto* parameter : parameters) {
      ParameterRow p;
      p.effect_id = effect->id;
      p.parameter_id = parameter->id;
      p.name = parameter->name;
      p.display_name = parameter->name;
      p.value = parameter->value.Sample(local_time);
      p.dimension = p.value.dimension;
      p.default_value = p.value;
      if (descriptor != nullptr) {
        if (const auto* declared = effects::FindParameter(*descriptor, parameter->name)) {
          p.display_name = declared->display_name;
          p.default_value = declared->default_value;
          p.minimum = declared->minimum;
          p.maximum = declared->maximum;
          p.unit = declared->unit;
          p.keyframeable = declared->keyframeable;
          p.dimension = declared->dimension;
        }
      }
      p.keyframed = !parameter->value.keyframes().empty();
      for (const auto& key : parameter->value.keyframes()) {
        p.key_times.push_back(key.time);
        if (key.time.Compare(local_time) == 0) p.key_here = true;
      }
      row.parameters.push_back(std::move(p));
    }
    state.effects.push_back(std::move(row));
  }
  return state;
}

std::vector<CatalogueEntry> EffectCatalogue(const std::string& query) {
  const auto needle = Lower(query);
  std::vector<CatalogueEntry> entries;
  for (const auto& descriptor : effects::BuiltInEffects()) {
    if (!IsRendered(descriptor)) continue;
    // Effects a clip is born with, generators that are clips themselves and internal ones are not offered here.
    static const std::set<std::string> hidden{"opacity", "motion", "transform", "time_remap", "solid", "graphic", "motion_graphics_template", "frame_interpolation", "blend_mode"};
    if (hidden.count(descriptor.id) != 0) continue;
    if (!needle.empty() && Lower(descriptor.display_name).find(needle) == std::string::npos && Lower(descriptor.category).find(needle) == std::string::npos) continue;
    entries.push_back({descriptor.id, descriptor.display_name, descriptor.category, !descriptor.asset_kind.empty(), descriptor.asset_kind});
  }
  std::sort(entries.begin(), entries.end(), [](const CatalogueEntry& a, const CatalogueEntry& b) {
    return std::tie(a.category, a.name) < std::tie(b.category, b.name);
  });
  return entries;
}

// ------------------------------------------------------------------------- edits ----

namespace {

struct Target {
  const timeline::Clip* clip{nullptr};
  const timeline::Effect* effect{nullptr};
  const timeline::Parameter* parameter{nullptr};
  std::string refusal;
};

Target Resolve(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name = {}) {
  Target target;
  const timeline::Track* track = nullptr;
  target.clip = FindClipIn(*ctx.sequence, clip_id, &track);
  if (target.clip == nullptr) {
    target.refusal = "The clip is not in the sequence";
    return target;
  }
  if (track->locked) {
    target.refusal = "The track " + track->id + " is locked";
    return target;
  }
  target.effect = FindEffectIn(*target.clip, effect_id);
  if (target.effect == nullptr) {
    target.refusal = "The clip has no such effect";
    return target;
  }
  if (!parameter_name.empty()) {
    target.parameter = FindParameterIn(*target.effect, parameter_name);
    if (target.parameter == nullptr) target.refusal = "The effect has no parameter " + parameter_name;
  }
  return target;
}

EditPlan Plan(std::string label, std::vector<PlannedCommand> commands) {
  EditPlan plan;
  plan.ok = true;
  plan.label = std::move(label);
  plan.commands = std::move(commands);
  return plan;
}

PlannedCommand Command(CommandType type, commands::CommandPayload payload) { return {type, std::move(payload)}; }

std::string ParameterLabel(const timeline::Effect& effect, const std::string& name) {
  if (const auto* descriptor = effects::FindEffect(effect.effect_type)) {
    if (const auto* declared = effects::FindParameter(*descriptor, name)) return declared->display_name;
  }
  return name;
}

}  // namespace

EditPlan PlanAddEffect(const EditContext& ctx, const std::string& clip_id, const std::string& effect_type, const std::string& preset_name) {
  const timeline::Track* track = nullptr;
  const auto* clip = FindClipIn(*ctx.sequence, clip_id, &track);
  if (clip == nullptr) return EditPlan::Refuse("The clip is not in the sequence");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  const auto* descriptor = effects::FindEffect(effect_type);
  if (descriptor == nullptr) return EditPlan::Refuse("This build has no effect " + effect_type);
  if (!IsRendered(*descriptor)) return EditPlan::Refuse(descriptor->display_name + " cannot be added to a clip");
  if (!descriptor->asset_kind.empty() && preset_name.empty()) return EditPlan::Refuse(descriptor->display_name + " needs a " + descriptor->asset_kind + " file");
  commands::AddEffectPayload add;
  add.id = ctx.new_id ? ctx.new_id("fx") : "fx-new";
  add.owner_kind = model::EffectOwner::Clip;
  add.owner_id = clip_id;
  add.effect_type = effect_type;
  add.preset_name = preset_name;
  std::int64_t next = 0;
  for (const auto& effect : clip->effects) next = std::max(next, effect.order + 1);
  add.order = next;
  for (const auto& declared : descriptor->parameters) {
    commands::EffectParameter parameter;
    parameter.id = add.id + ":" + declared.id;
    parameter.name = declared.id;
    parameter.value = declared.default_value;
    add.parameters.push_back(std::move(parameter));
  }
  return Plan("Add " + descriptor->display_name, {Command(CommandType::AddEffect, add)});
}

EditPlan PlanAddAnalysedEffect(const EditContext& ctx, const std::string& clip_id, const timeline::Effect& effect, const std::string& label) {
  const timeline::Track* track = nullptr;
  const auto* clip = FindClipIn(*ctx.sequence, clip_id, &track);
  if (clip == nullptr) return EditPlan::Refuse("The clip is no longer in the sequence");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  commands::AddEffectPayload add;
  add.id = ctx.new_id ? ctx.new_id("fx") : "fx-new";
  add.owner_kind = model::EffectOwner::Clip;
  add.owner_id = clip_id;
  add.effect_type = effect.effect_type;
  add.preset_name = effect.preset_name;
  std::int64_t next = 0;
  for (const auto& existing : clip->effects) next = std::max(next, existing.order + 1);
  add.order = next;
  std::vector<PlannedCommand> commands;
  std::vector<commands::SetKeyframePayload> keys;
  for (const auto& parameter : effect.parameters) {
    commands::EffectParameter init;
    init.id = add.id + ":" + parameter.name;
    init.name = parameter.name;
    init.value = parameter.value.keyframes().empty() ? parameter.value.constant() : parameter.value.keyframes().front().value;
    add.parameters.push_back(init);
    for (const auto& key : parameter.value.keyframes()) keys.push_back({init.id, key});
  }
  commands.push_back(Command(CommandType::AddEffect, add));
  for (const auto& key : keys) commands.push_back(Command(CommandType::SetKeyframe, key));
  return Plan(label, std::move(commands));
}

EditPlan PlanRemoveEffect(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id) {
  const auto target = Resolve(ctx, clip_id, effect_id);
  if (!target.refusal.empty()) return EditPlan::Refuse(target.refusal);
  if (target.effect->intrinsic) return EditPlan::Refuse("A clip's own " + target.effect->effect_type + " controls cannot be removed");
  return Plan("Remove Effect", {Command(CommandType::RemoveEffect, commands::RemoveEffectPayload{effect_id})});
}

EditPlan PlanSetEffectEnabled(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, bool enabled) {
  const auto target = Resolve(ctx, clip_id, effect_id);
  if (!target.refusal.empty()) return EditPlan::Refuse(target.refusal);
  if (target.effect->enabled == enabled) return EditPlan::Refuse(enabled ? "The effect is already on" : "The effect is already off");
  return Plan(enabled ? "Enable Effect" : "Disable Effect", {Command(CommandType::SetEffectEnabled, commands::SetEffectEnabledPayload{effect_id, enabled})});
}

EditPlan PlanMoveEffect(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, int places) {
  const auto target = Resolve(ctx, clip_id, effect_id);
  if (!target.refusal.empty()) return EditPlan::Refuse(target.refusal);
  std::vector<const timeline::Effect*> stack;
  for (const auto& effect : target.clip->effects) stack.push_back(&effect);
  std::stable_sort(stack.begin(), stack.end(), [](const auto* a, const auto* b) { return a->order < b->order; });
  const auto at = static_cast<int>(std::find(stack.begin(), stack.end(), target.effect) - stack.begin());
  const auto to = std::clamp(at + places, 0, static_cast<int>(stack.size()) - 1);
  if (to == at) return EditPlan::Refuse("The effect cannot move further");
  const auto* moving = stack[static_cast<std::size_t>(at)];
  stack.erase(stack.begin() + at);
  stack.insert(stack.begin() + to, moving);
  std::vector<PlannedCommand> commands;
  for (std::size_t index = 0; index < stack.size(); ++index) {
    if (stack[index]->order != static_cast<std::int64_t>(index)) {
      commands.push_back(Command(CommandType::ReorderEffect, commands::ReorderEffectPayload{stack[index]->id, static_cast<std::int64_t>(index)}));
    }
  }
  return Plan("Reorder Effects", std::move(commands));
}

anim::Value ClampToRange(const ParameterRow& row, const anim::Value& value) {
  auto clamped = value;
  for (int i = 0; i < clamped.dimension; ++i) {
    auto& c = clamped.components[static_cast<std::size_t>(i)];
    if (!std::isfinite(c)) c = row.default_value.components[static_cast<std::size_t>(i)];
    if (row.minimum && c < *row.minimum) c = *row.minimum;
    if (row.maximum && c > *row.maximum) c = *row.maximum;
  }
  return clamped;
}

EditPlan PlanSetParameter(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name,
                          const anim::Value& value, const RationalTime& local_time) {
  const auto target = Resolve(ctx, clip_id, effect_id, parameter_name);
  if (!target.refusal.empty()) return EditPlan::Refuse(target.refusal);
  if (value.dimension != target.parameter->value.constant().dimension && target.parameter->value.keyframes().empty()) {
    return EditPlan::Refuse("The value has the wrong number of components for this parameter");
  }
  if (const auto problem = effects::ValidateParameter(target.effect->effect_type, parameter_name, value); !problem.empty()) return EditPlan::Refuse(problem);
  const auto label = "Change " + ParameterLabel(*target.effect, parameter_name);
  if (!target.parameter->value.keyframes().empty()) {
    commands::SetKeyframePayload key;
    key.parameter_id = target.parameter->id;
    key.keyframe.time = local_time;
    key.keyframe.value = value;
    key.keyframe.interpolation = InterpolationBefore(target.parameter->value, local_time);
    return Plan(label, {Command(CommandType::SetKeyframe, key)});
  }
  return Plan(label, {Command(CommandType::SetParameterConstant, commands::SetParameterConstantPayload{target.parameter->id, value})});
}

EditPlan PlanToggleAnimation(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name,
                             const RationalTime& local_time) {
  const auto target = Resolve(ctx, clip_id, effect_id, parameter_name);
  if (!target.refusal.empty()) return EditPlan::Refuse(target.refusal);
  if (const auto* descriptor = effects::FindEffect(target.effect->effect_type)) {
    if (const auto* declared = effects::FindParameter(*descriptor, parameter_name); declared != nullptr && !declared->keyframeable) {
      return EditPlan::Refuse(ParameterLabel(*target.effect, parameter_name) + " cannot be animated");
    }
  }
  const auto current = target.parameter->value.Sample(local_time);
  std::vector<PlannedCommand> commands;
  if (target.parameter->value.keyframes().empty()) {
    commands::SetKeyframePayload key;
    key.parameter_id = target.parameter->id;
    key.keyframe.time = local_time;
    key.keyframe.value = current;
    commands.push_back(Command(CommandType::SetKeyframe, key));
    return Plan("Animate " + ParameterLabel(*target.effect, parameter_name), std::move(commands));
  }
  for (const auto& key : target.parameter->value.keyframes()) {
    commands.push_back(Command(CommandType::RemoveKeyframe, commands::RemoveKeyframePayload{target.parameter->id, key.time}));
  }
  commands.push_back(Command(CommandType::SetParameterConstant, commands::SetParameterConstantPayload{target.parameter->id, current}));
  return Plan("Stop Animating " + ParameterLabel(*target.effect, parameter_name), std::move(commands));
}

EditPlan PlanToggleKeyframe(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name,
                            const RationalTime& local_time) {
  const auto target = Resolve(ctx, clip_id, effect_id, parameter_name);
  if (!target.refusal.empty()) return EditPlan::Refuse(target.refusal);
  const auto& keys = target.parameter->value.keyframes();
  const bool here = std::any_of(keys.begin(), keys.end(), [&](const anim::Keyframe& k) { return k.time.Compare(local_time) == 0; });
  if (here) {
    if (keys.size() == 1) return PlanToggleAnimation(ctx, clip_id, effect_id, parameter_name, local_time);
    return Plan("Delete Keyframe", {Command(CommandType::RemoveKeyframe, commands::RemoveKeyframePayload{target.parameter->id, local_time})});
  }
  commands::SetKeyframePayload key;
  key.parameter_id = target.parameter->id;
  key.keyframe.time = local_time;
  key.keyframe.value = target.parameter->value.Sample(local_time);
  key.keyframe.interpolation = InterpolationBefore(target.parameter->value, local_time);
  return Plan("Add Keyframe", {Command(CommandType::SetKeyframe, key)});
}

EditPlan PlanResetParameter(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const std::string& parameter_name) {
  const auto target = Resolve(ctx, clip_id, effect_id, parameter_name);
  if (!target.refusal.empty()) return EditPlan::Refuse(target.refusal);
  const auto* descriptor = effects::FindEffect(target.effect->effect_type);
  const auto* declared = descriptor != nullptr ? effects::FindParameter(*descriptor, parameter_name) : nullptr;
  if (declared == nullptr) return EditPlan::Refuse("There is no default to return to");
  std::vector<PlannedCommand> commands;
  for (const auto& key : target.parameter->value.keyframes()) {
    commands.push_back(Command(CommandType::RemoveKeyframe, commands::RemoveKeyframePayload{target.parameter->id, key.time}));
  }
  commands.push_back(Command(CommandType::SetParameterConstant, commands::SetParameterConstantPayload{target.parameter->id, declared->default_value}));
  return Plan("Reset " + declared->display_name, std::move(commands));
}

std::optional<RationalTime> NextKeyframe(const ParameterRow& row, const RationalTime& at) {
  std::optional<RationalTime> best;
  for (const auto& t : row.key_times) {
    if (t.Compare(at) > 0 && (!best || t.Compare(*best) < 0)) best = t;
  }
  return best;
}

std::optional<RationalTime> PreviousKeyframe(const ParameterRow& row, const RationalTime& at) {
  std::optional<RationalTime> best;
  for (const auto& t : row.key_times) {
    if (t.Compare(at) < 0 && (!best || t.Compare(*best) > 0)) best = t;
  }
  return best;
}

}  // namespace cutline::ui

#include "ui/AudioWorkflow.h"

#include "effects/EffectRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cutline::ui {
namespace {

using commands::CommandType;

const RationalTime kZero(0, 1);

struct Located final {
  const timeline::Track* track{nullptr};
  const timeline::Clip* clip{nullptr};
};

Located FindClipById(const timeline::Sequence& sequence, const std::string& id) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id == id) return {&track, &clip};
    }
  }
  return {};
}

double Seconds(const RationalTime& t) { return static_cast<double>(t.numerator()) / static_cast<double>(std::max<std::int64_t>(1, t.denominator())); }

bool IsChainEffectId(const std::string& id) { return id.rfind("role-", 0) == 0; }

std::int64_t NextOrder(const std::vector<timeline::Effect>& effects, bool skip_chain) {
  std::int64_t next = 0;
  for (const auto& effect : effects) {
    if (skip_chain && IsChainEffectId(effect.id)) continue;
    next = std::max(next, effect.order + 1);
  }
  return next;
}

// An AddEffect for a descriptor, with its own defaults except where `overrides` say otherwise.
commands::AddEffectPayload MakeAdd(const std::string& id, model::EffectOwner owner, const std::string& owner_id, const std::string& type,
                                   std::int64_t order, const std::vector<std::pair<std::string, double>>& overrides, const std::string& preset = {}) {
  commands::AddEffectPayload add;
  add.id = id;
  add.owner_kind = owner;
  add.owner_id = owner_id;
  add.effect_type = type;
  add.order = order;
  add.preset_name = preset;
  if (const auto* descriptor = effects::FindEffect(type)) {
    for (const auto& declared : descriptor->parameters) {
      commands::EffectParameter parameter;
      parameter.id = id + ":" + declared.id;
      parameter.name = declared.id;
      parameter.value = declared.default_value;
      for (const auto& [name, value] : overrides) {
        if (name == declared.id) parameter.value = anim::Value::Scalar(value);
      }
      add.parameters.push_back(std::move(parameter));
    }
  }
  return add;
}

const timeline::Parameter* ParameterNamed(const timeline::Effect& effect, const std::string& name) {
  for (const auto& parameter : effect.parameters) {
    if (parameter.name == name) return &parameter;
  }
  return nullptr;
}

}  // namespace

// ----------------------------------------------------------------------------------- roles ----

std::string ToString(AudioRole role) {
  switch (role) {
    case AudioRole::Dialogue: return "dialogue";
    case AudioRole::Music: return "music";
    case AudioRole::Effects: return "effects";
    case AudioRole::Ambience: return "ambience";
    case AudioRole::None: break;
  }
  return {};
}

std::optional<AudioRole> ParseAudioRole(const std::string& text) {
  if (text.empty()) return AudioRole::None;
  for (const auto role : {AudioRole::Dialogue, AudioRole::Music, AudioRole::Effects, AudioRole::Ambience}) {
    if (ToString(role) == text) return role;
  }
  return std::nullopt;
}

std::string RoleLabel(AudioRole role) {
  switch (role) {
    case AudioRole::Dialogue: return "Dialogue";
    case AudioRole::Music: return "Music";
    case AudioRole::Effects: return "Effects";
    case AudioRole::Ambience: return "Ambience";
    case AudioRole::None: break;
  }
  return "No role";
}

std::vector<ChainEffect> RoleChain(AudioRole role, const RoleChainOptions& options) {
  std::vector<ChainEffect> chain;
  const auto repair = std::clamp(options.repair, 0.0, 1.0);
  const auto dereverb = std::clamp(options.dereverb, 0.0, 1.0);
  switch (role) {
    case AudioRole::Dialogue:
      if (repair > 0.01) chain.push_back({"denoise", {}, {{"amount", repair}}});
      if (dereverb > 0.01) chain.push_back({"dereverb", {}, {{"amount", dereverb}}});
      if (options.tone) chain.push_back({"eq", {}, {{"low_gain", -9.0}, {"low_frequency", 90.0}, {"mid_gain", 2.5}, {"mid_frequency", 3200.0}, {"mid_q", 1.0}}});
      if (options.dynamics) {
        chain.push_back({"compressor", {}, {{"threshold", -22.0}, {"ratio", 3.0}, {"attack", 8.0}, {"release", 140.0}, {"makeup", 3.0}, {"knee", 6.0}}});
        chain.push_back({"limiter", {}, {{"ceiling", -2.0}}});
      }
      break;
    case AudioRole::Music:
      if (options.tone) chain.push_back({"eq", {}, {{"mid_gain", -3.0}, {"mid_frequency", 1800.0}, {"mid_q", 0.8}}});
      if (options.dynamics) chain.push_back({"compressor", {}, {{"threshold", -20.0}, {"ratio", 2.0}, {"attack", 20.0}, {"release", 250.0}, {"makeup", 1.0}}});
      if (!options.duck_under_track.empty()) chain.push_back({"duck", options.duck_under_track, {{"threshold", -38.0}, {"reduction", -10.0}, {"attack", 30.0}, {"release", 500.0}}});
      if (options.dynamics) chain.push_back({"limiter", {}, {{"ceiling", -2.0}}});
      break;
    case AudioRole::Effects:
      if (options.tone) chain.push_back({"eq", {}, {{"low_gain", -6.0}, {"low_frequency", 60.0}}});
      if (options.dynamics) chain.push_back({"limiter", {}, {{"ceiling", -3.0}}});
      break;
    case AudioRole::Ambience:
      if (repair > 0.01) chain.push_back({"denoise", {}, {{"amount", repair * 0.6}}});
      if (options.tone) chain.push_back({"eq", {}, {{"low_gain", -6.0}, {"low_frequency", 150.0}, {"high_gain", -3.0}, {"high_frequency", 7000.0}}});
      if (options.dynamics) {
        chain.push_back({"compressor", {}, {{"threshold", -26.0}, {"ratio", 2.0}, {"attack", 30.0}, {"release", 300.0}}});
        chain.push_back({"limiter", {}, {{"ceiling", -6.0}}});
      }
      break;
    case AudioRole::None: break;
  }
  return chain;
}

namespace {

// Checks that every clip can have a role, and collects them in time order for a stable plan.
bool CollectSoundClips(const EditContext& ctx, const std::set<std::string>& ids, std::vector<Located>& found, std::string& problem) {
  for (const auto& id : ids) {
    const auto where = FindClipById(*ctx.sequence, id);
    if (where.clip == nullptr) {
      problem = "A selected clip is not in the sequence";
      return false;
    }
    if (where.track->kind != model::TrackKind::Audio) {
      problem = "Only the sound of a clip has a role: " + (where.clip->name.empty() ? where.clip->id : where.clip->name) + " is on a picture track";
      return false;
    }
    if (where.track->locked) {
      problem = "The track " + where.track->id + " is locked";
      return false;
    }
    found.push_back(where);
  }
  if (found.empty()) {
    problem = "Select the sound of a clip first";
    return false;
  }
  return true;
}

}  // namespace

EditPlan PlanApplyRole(const EditContext& ctx, const std::set<std::string>& clip_ids, AudioRole role, bool with_chain, const RoleChainOptions& options) {
  if (ctx.sequence == nullptr || !ctx.new_id) return EditPlan::Refuse("There is no sequence");
  std::vector<Located> clips;
  std::string problem;
  if (!CollectSoundClips(ctx, clip_ids, clips, problem)) return EditPlan::Refuse(problem);
  EditPlan plan;
  plan.ok = true;
  plan.label = role == AudioRole::None ? "Clear Audio Role" : "Set Audio Role: " + RoleLabel(role);
  const auto chain = with_chain ? RoleChain(role, options) : std::vector<ChainEffect>{};
  for (const auto& where : clips) {
    const auto& clip = *where.clip;
    if (clip.audio_role != ToString(role)) plan.commands.push_back({CommandType::SetClipAudioRole, commands::SetClipAudioRolePayload{clip.id, ToString(role)}});
    if (!with_chain) continue;
    // The chain of an earlier choice goes, so applying a role twice does not stack it.
    for (const auto& effect : clip.effects) {
      if (IsChainEffectId(effect.id)) plan.commands.push_back({CommandType::RemoveEffect, commands::RemoveEffectPayload{effect.id}});
    }
    auto order = NextOrder(clip.effects, true);
    for (const auto& step : chain) {
      auto add = MakeAdd(ctx.new_id("role-" + step.effect_type), model::EffectOwner::Clip, clip.id, step.effect_type, order++, step.parameters, step.preset_name);
      plan.commands.push_back({CommandType::AddEffect, std::move(add)});
    }
  }
  if (plan.commands.empty()) return EditPlan::Refuse("The clips already have this role");
  if (with_chain && !chain.empty()) plan.notes.push_back(std::to_string(chain.size()) + " effects added: they are ordinary effects and can be changed or removed");
  return plan;
}

EditPlan PlanClearRole(const EditContext& ctx, const std::set<std::string>& clip_ids) {
  return PlanApplyRole(ctx, clip_ids, AudioRole::None, true);
}

// -------------------------------------------------------------------------------- loudness ----

const std::vector<LoudnessTarget>& LoudnessTargets() {
  static const std::vector<LoudnessTarget> targets = {
      {"r128", "EBU R128 (broadcast, Europe)", -23.0, -1.0, "Integrated -23 LUFS, true peak at most -1 dBTP"},
      {"a85", "ATSC A/85 (broadcast, US)", -24.0, -2.0, "Integrated -24 LKFS, true peak at most -2 dBTP"},
      {"streaming", "Streaming platforms", -14.0, -1.0, "About -14 LUFS, true peak at most -1 dBTP; platforms turn louder programmes down"},
      {"podcast", "Podcast and voice", -16.0, -1.0, "About -16 LUFS, true peak at most -1 dBTP"},
  };
  return targets;
}

const LoudnessTarget* FindLoudnessTarget(const std::string& id) {
  for (const auto& target : LoudnessTargets()) {
    if (target.id == id) return &target;
  }
  return nullptr;
}

double RoleLoudness(AudioRole role) {
  switch (role) {
    case AudioRole::Dialogue: return -23.0;
    case AudioRole::Music: return -27.0;
    case AudioRole::Effects: return -26.0;
    case AudioRole::Ambience: return -32.0;
    case AudioRole::None: break;
  }
  return -23.0;
}

LoudnessAdvice AdviseLoudness(const audio::dsp::Loudness& measured, const LoudnessTarget& target) {
  LoudnessAdvice advice;
  if (measured.integrated_lufs <= -199.0) {
    advice.message = "There is nothing above the silence gate to measure";
    return advice;
  }
  advice.measurable = true;
  advice.gain_db = target.lufs - measured.integrated_lufs;
  advice.result_true_peak_db = measured.true_peak_db + advice.gain_db;
  advice.gain_for_ceiling_db = target.true_peak_db - measured.true_peak_db;
  advice.over_ceiling = advice.result_true_peak_db > target.true_peak_db + 1e-9;
  char text[200];
  std::snprintf(text, sizeof(text), "Measured %.1f LUFS; %+.1f dB reaches %.0f LUFS%s", measured.integrated_lufs, advice.gain_db, target.lufs,
                advice.over_ceiling ? ", but the true peak would pass the ceiling: a limiter is needed" : "");
  advice.message = text;
  return advice;
}

namespace {

EditPlan PlanSetVolume(const std::vector<timeline::Effect>& existing, model::EffectOwner owner, const std::string& owner_id, const EditContext& ctx,
                       double gain_db, const std::string& label) {
  const auto level = std::clamp(gain_db, -96.0, 24.0);
  for (const auto& effect : existing) {
    if (effect.effect_type != "volume" || IsChainEffectId(effect.id) || effect.id.rfind("auto-", 0) == 0) continue;
    const auto* parameter = ParameterNamed(effect, "level");
    if (parameter == nullptr) continue;
    EditPlan plan;
    plan.ok = true;
    plan.label = label;
    plan.commands.push_back({CommandType::SetParameterConstant, commands::SetParameterConstantPayload{parameter->id, anim::Value::Scalar(level)}});
    if (level != gain_db) plan.notes.push_back("The gain was held to the Volume effect's range");
    return plan;
  }
  if (!ctx.new_id) return EditPlan::Refuse("There is no way to name a new effect");
  EditPlan plan;
  plan.ok = true;
  plan.label = label;
  plan.commands.push_back({CommandType::AddEffect, MakeAdd(ctx.new_id("fx"), owner, owner_id, "volume", NextOrder(existing, false), {{"level", level}})});
  return plan;
}

}  // namespace

EditPlan PlanSetClipGain(const EditContext& ctx, const std::string& clip_id, double gain_db, const std::string& label) {
  if (ctx.sequence == nullptr) return EditPlan::Refuse("There is no sequence");
  const auto where = FindClipById(*ctx.sequence, clip_id);
  if (where.clip == nullptr) return EditPlan::Refuse("The clip is not in the sequence");
  if (where.track->kind != model::TrackKind::Audio) return EditPlan::Refuse("Gain is set on the sound of a clip");
  if (where.track->locked) return EditPlan::Refuse("The track " + where.track->id + " is locked");
  return PlanSetVolume(where.clip->effects, model::EffectOwner::Clip, clip_id, ctx, gain_db, label);
}

EditPlan PlanSetMasterGain(const EditContext& ctx, double gain_db, const std::string& label) {
  if (ctx.sequence == nullptr) return EditPlan::Refuse("There is no sequence");
  return PlanSetVolume(ctx.sequence->effects, model::EffectOwner::Sequence, ctx.sequence->id, ctx, gain_db, label);
}

// ------------------------------------------------------------------------------- automation ----

std::string ToString(AutomationMode mode) {
  switch (mode) {
    case AutomationMode::Read: return "read";
    case AutomationMode::Write: return "write";
    case AutomationMode::Touch: return "touch";
    case AutomationMode::Latch: return "latch";
  }
  return "read";
}

std::optional<AutomationMode> ParseAutomationMode(const std::string& text) {
  for (const auto mode : {AutomationMode::Read, AutomationMode::Write, AutomationMode::Touch, AutomationMode::Latch}) {
    if (ToString(mode) == text) return mode;
  }
  return std::nullopt;
}

namespace {

using Key = std::pair<RationalTime, double>;

// The value of a curve of keys (linear between them, held past the ends) at a time.
double CurveAt(const std::vector<Key>& keys, const RationalTime& at, double fallback) {
  if (keys.empty()) return fallback;
  if (at.Compare(keys.front().first) <= 0) return keys.front().second;
  if (at.Compare(keys.back().first) >= 0) return keys.back().second;
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (at.Compare(keys[i].first) <= 0) {
      const double a = Seconds(keys[i - 1].first), b = Seconds(keys[i].first), t = Seconds(at);
      const double f = b > a ? (t - a) / (b - a) : 1.0;
      return keys[i - 1].second + (keys[i].second - keys[i - 1].second) * f;
    }
  }
  return keys.back().second;
}

// Ramer-Douglas-Peucker on (seconds, value): the fewest keys that stay within `tolerance` of the moves.
void Thin(const std::vector<Key>& points, std::size_t first, std::size_t last, double tolerance, std::vector<bool>& keep) {
  if (last <= first + 1) return;
  const double x0 = Seconds(points[first].first), y0 = points[first].second, x1 = Seconds(points[last].first), y1 = points[last].second;
  double worst = -1.0;
  std::size_t at = first;
  for (std::size_t i = first + 1; i < last; ++i) {
    const double x = Seconds(points[i].first);
    const double f = x1 > x0 ? (x - x0) / (x1 - x0) : 0.0;
    const double distance = std::abs(points[i].second - (y0 + (y1 - y0) * f));
    if (distance > worst) {
      worst = distance;
      at = i;
    }
  }
  if (worst > tolerance) {
    keep[at] = true;
    Thin(points, first, at, tolerance, keep);
    Thin(points, at, last, tolerance, keep);
  }
}

std::vector<Key> Thinned(const std::vector<Key>& points, double tolerance) {
  if (points.size() <= 2) return points;
  std::vector<bool> keep(points.size(), false);
  keep.front() = keep.back() = true;
  Thin(points, 0, points.size() - 1, tolerance, keep);
  std::vector<Key> out;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (keep[i]) out.push_back(points[i]);
  }
  return out;
}

void AppendKey(std::vector<Key>& keys, const RationalTime& time, double value) {
  // Two keys at one time would be one: the later wins.
  if (!keys.empty() && keys.back().first.Compare(time) == 0) keys.back().second = value;
  else keys.push_back({time, value});
}

}  // namespace

AutomationWrite AutomationKeys(AutomationMode mode, const std::vector<AutomationSample>& samples, const RationalTime& stop, const std::vector<Key>& existing,
                               const AutomationOptions& options) {
  AutomationWrite write;
  if (mode == AutomationMode::Read || samples.empty()) return write;
  const auto tolerance = std::max(options.tolerance, 0.0);

  if (mode == AutomationMode::Write || mode == AutomationMode::Latch) {
    // Write: everything from the first move to the stop. Latch: from the first touch, and the value stays where the hand left it.
    std::vector<Key> points;
    bool started = mode == AutomationMode::Write;
    for (const auto& sample : samples) {
      if (!started && sample.touching) started = true;
      if (started) AppendKey(points, sample.time, sample.value);
    }
    if (points.empty()) return write;
    if (stop.Compare(points.back().first) > 0) AppendKey(points, stop, points.back().second);
    write.keys = Thinned(points, tolerance);
    write.span_start = write.keys.front().first;
    write.span_end = write.keys.back().first;
    write.any = true;
    return write;
  }

  // Touch: each hold of the control is written, and after the release the value goes back to what was there.
  std::vector<Key> all;
  bool have_span = false;
  std::size_t i = 0;
  while (i < samples.size()) {
    if (!samples[i].touching) {
      ++i;
      continue;
    }
    std::vector<Key> hold;
    const double before = samples[i].value;
    RationalTime release_time = stop;
    bool released = false;
    while (i < samples.size()) {
      AppendKey(hold, samples[i].time, samples[i].value);
      const bool hand_off = !samples[i].touching;
      ++i;
      if (hand_off) {
        released = true;
        release_time = hold.back().first;
        break;
      }
    }
    auto thinned = Thinned(hold, tolerance);
    if (released) {
      const auto back_at = release_time.Add(options.release);
      const double back_value = existing.empty() ? before : CurveAt(existing, back_at, before);
      AppendKey(thinned, back_at, back_value);
    } else if (stop.Compare(thinned.back().first) > 0) {
      AppendKey(thinned, stop, thinned.back().second);
    }
    if (!have_span) {
      write.span_start = thinned.front().first;
      have_span = true;
    }
    write.span_end = thinned.back().first;
    for (const auto& key : thinned) AppendKey(all, key.first, key.second);
  }
  if (!have_span) return write;
  write.keys = all;
  write.any = true;
  return write;
}

std::vector<std::pair<RationalTime, double>> AutomationOf(const timeline::Track& track, AutomationTarget target) {
  std::vector<std::pair<RationalTime, double>> keys;
  const std::string id = std::string(target == AutomationTarget::Volume ? "auto-volume-" : "auto-pan-") + track.id;
  for (const auto& effect : track.effects) {
    if (effect.id != id) continue;
    if (const auto* parameter = ParameterNamed(effect, target == AutomationTarget::Volume ? "level" : "value")) {
      for (const auto& key : parameter->value.keyframes()) keys.emplace_back(key.time, key.value.scalar());
    }
  }
  return keys;
}

std::optional<double> AutomationValueAt(const timeline::Track& track, AutomationTarget target, const RationalTime& at) {
  const auto keys = AutomationOf(track, target);
  if (keys.empty()) return std::nullopt;
  return CurveAt(keys, at, 0.0);
}

EditPlan PlanWriteAutomation(const EditContext& ctx, const std::string& track_id, AutomationTarget target, const AutomationWrite& write) {
  if (ctx.sequence == nullptr || !ctx.new_id) return EditPlan::Refuse("There is no sequence");
  if (!write.any || write.keys.empty()) return EditPlan::Refuse("There is nothing to write");
  const auto* track = ctx.sequence->FindTrack(track_id);
  if (track == nullptr || track->kind != model::TrackKind::Audio) return EditPlan::Refuse("Automation is written on an audio track");
  if (track->locked) return EditPlan::Refuse("The track " + track_id + " is locked");
  const std::string effect_id = std::string(target == AutomationTarget::Volume ? "auto-volume-" : "auto-pan-") + track_id;
  const char* const type = target == AutomationTarget::Volume ? "volume" : "pan";
  const char* const name = target == AutomationTarget::Volume ? "level" : "value";
  EditPlan plan;
  plan.ok = true;
  plan.label = target == AutomationTarget::Volume ? "Write Volume Automation" : "Write Pan Automation";
  const timeline::Effect* effect = nullptr;
  for (const auto& candidate : track->effects) {
    if (candidate.id == effect_id) effect = &candidate;
  }
  const auto parameter_id = effect_id + ":" + name;
  if (effect == nullptr) {
    plan.commands.push_back({CommandType::AddEffect, MakeAdd(effect_id, model::EffectOwner::Track, track_id, type, NextOrder(track->effects, false), {})});
  } else if (const auto* parameter = ParameterNamed(*effect, name)) {
    // What was written in the span goes: the new moves replace it.
    for (const auto& key : parameter->value.keyframes()) {
      if (key.time.Compare(write.span_start) >= 0 && key.time.Compare(write.span_end) <= 0) {
        plan.commands.push_back({CommandType::RemoveKeyframe, commands::RemoveKeyframePayload{parameter->id, key.time}});
      }
    }
  }
  for (const auto& [time, value] : write.keys) {
    anim::Keyframe key;
    key.time = time;
    key.value = anim::Value::Scalar(target == AutomationTarget::Volume ? std::clamp(value, -96.0, 24.0) : std::clamp(value, -1.0, 1.0));
    key.interpolation = anim::Interpolation::Linear;
    plan.commands.push_back({CommandType::SetKeyframe, commands::SetKeyframePayload{parameter_id, key}});
  }
  plan.notes.push_back(std::to_string(write.keys.size()) + " keys written");
  return plan;
}

}  // namespace cutline::ui

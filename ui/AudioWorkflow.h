#pragma once

// The audio workflow: what the sound of a clip is for, and the jobs that follow from it.
//
//  * Roles. A clip's sound is dialogue, music, effects or ambience (or nobody has said). Choosing a role sets it on the
//    clip and, if asked, lays down a starting chain of the ordinary audio effects for that role (repair, tone, dynamics,
//    ducking under dialogue). The chain is plain effects with ids starting "role-": editable and removable like any
//    other, and replaced, not stacked, when the role is applied again.
//  * Loudness. Programme and clip loudness are measured to ITU-R BS.1770 (audio/Dsp.h) and compared with a delivery
//    target (EBU R128, ATSC A/85, streaming, podcast); the gain that reaches it is written as a Volume effect on the clip
//    or on the sequence, so it is an undoable edit like any other, and the plan says when the true peak would end up over
//    the target's ceiling.
//  * Automation. A fader or pan moved while the sequence plays is written as keyframes on a Volume or Pan effect of the
//    track, by one of three rules (write, touch, latch), thinned to what the curve needs and replacing what was there
//    in the span that was written.
//
// Everything here is a pure function of a snapshot and returns an EditPlan to run as one undo step.

#include "audio/Dsp.h"
#include "ui/EditPlanner.h"

#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

// -------------------------------------------------------------------------------- roles ----

enum class AudioRole { None, Dialogue, Music, Effects, Ambience };

[[nodiscard]] std::string ToString(AudioRole role);                 // "dialogue", ..., "" for None
[[nodiscard]] std::optional<AudioRole> ParseAudioRole(const std::string& text);
[[nodiscard]] std::string RoleLabel(AudioRole role);                // "Dialogue", ..., "No role"

struct RoleChainOptions final {
  // Dialogue and ambience: how much noise reduction (0 none, 1 hard) and reverb reduction to apply.
  double repair{0.5};
  double dereverb{0.0};
  // The tone shaping and the dynamics of the role.
  bool tone{true};
  bool dynamics{true};
  // Music only: the audio track whose signal pushes this clip down (empty for none).
  std::string duck_under_track;
};

// One effect of a role's chain, as the recipe lists it: the type and the parameters that are not the descriptor's own default.
struct ChainEffect final {
  std::string effect_type;
  std::string preset_name;  // the side-chain track of a duck
  std::vector<std::pair<std::string, double>> parameters;
};

// The recipe, in the order the effects run. These are starting points chosen by ear-neutral reasoning (cut the rumble
// under a voice, a little presence, gentle compression, a ceiling), not measurements of any recording; they are meant
// to be edited.
[[nodiscard]] std::vector<ChainEffect> RoleChain(AudioRole role, const RoleChainOptions& options = {});

// Sets the role of each clip and, with `with_chain`, replaces its role chain with the recipe's. Clips that are not sound,
// or whose track is locked, make the plan refuse.
[[nodiscard]] EditPlan PlanApplyRole(const EditContext& ctx, const std::set<std::string>& clip_ids, AudioRole role, bool with_chain,
                                     const RoleChainOptions& options = {});
// Takes the role and its chain off.
[[nodiscard]] EditPlan PlanClearRole(const EditContext& ctx, const std::set<std::string>& clip_ids);

// ----------------------------------------------------------------------------- loudness ----

struct LoudnessTarget final {
  std::string id;
  std::string name;
  double lufs{-23.0};
  double true_peak_db{-1.0};
  std::string note;
};

[[nodiscard]] const std::vector<LoudnessTarget>& LoudnessTargets();
[[nodiscard]] const LoudnessTarget* FindLoudnessTarget(const std::string& id);
// The target a role is usually delivered to when no programme target applies (dialogue sits higher than music).
[[nodiscard]] double RoleLoudness(AudioRole role);

struct LoudnessAdvice final {
  bool measurable{false};     // there was something above the gate
  double gain_db{0.0};        // to add to reach the target
  double result_true_peak_db{-200.0};
  bool over_ceiling{false};   // the true peak would pass the target's ceiling: a limiter (or less gain) is needed
  double gain_for_ceiling_db{0.0};  // the most gain the ceiling allows
  std::string message;
};
[[nodiscard]] LoudnessAdvice AdviseLoudness(const audio::dsp::Loudness& measured, const LoudnessTarget& target);

// Writes a gain (decibels) as the level of the clip's Volume effect, adding the effect when there is none.
[[nodiscard]] EditPlan PlanSetClipGain(const EditContext& ctx, const std::string& clip_id, double gain_db, const std::string& label = "Set Clip Gain");
// The same for the whole sequence, as a Volume effect of the sequence (what the master carries).
[[nodiscard]] EditPlan PlanSetMasterGain(const EditContext& ctx, double gain_db, const std::string& label = "Set Master Gain");

// ---------------------------------------------------------------------------- automation ----

enum class AutomationMode { Read, Write, Touch, Latch };
[[nodiscard]] std::string ToString(AutomationMode mode);
[[nodiscard]] std::optional<AutomationMode> ParseAutomationMode(const std::string& text);

enum class AutomationTarget { Volume, Pan };

// What a control did while the sequence played: where it was and whether a hand was on it. The first sample of a
// gesture has `touching` true; `time` is the timeline time of the playhead.
struct AutomationSample final {
  RationalTime time;
  double value{0.0};
  bool touching{false};
};

struct AutomationOptions final {
  // How far a written curve may stray from the moves that were made, in the control's own units (decibels, or pan).
  double tolerance{0.1};
  // Touch mode: how long the control takes to go back to what was automated after the hand leaves it.
  RationalTime release{1, 4};
};

// The keyframes a pass of moves leaves on a track's automation, by the rule of the mode. `samples` are in time order; `stop` is
// when the transport stopped. Read writes nothing. The existing curve is `existing` (time, value), which Touch returns to.
struct AutomationWrite final {
  std::vector<std::pair<RationalTime, double>> keys;
  RationalTime span_start, span_end;  // what the new keys replace
  bool any{false};
};
[[nodiscard]] AutomationWrite AutomationKeys(AutomationMode mode, const std::vector<AutomationSample>& samples, const RationalTime& stop,
                                             const std::vector<std::pair<RationalTime, double>>& existing, const AutomationOptions& options = {});

// The plan that writes them to the track's "auto-volume-<track>" or "auto-pan-<track>" effect (made if absent): the keys inside
// the written span come out, the new ones go in.
[[nodiscard]] EditPlan PlanWriteAutomation(const EditContext& ctx, const std::string& track_id, AutomationTarget target, const AutomationWrite& write);
// What the automation says at a time (linear between keys, held past the ends), or nothing when the track has none.
[[nodiscard]] std::optional<double> AutomationValueAt(const timeline::Track& track, AutomationTarget target, const RationalTime& at);
// The automation of a track as time/value pairs ({} when there is none).
[[nodiscard]] std::vector<std::pair<RationalTime, double>> AutomationOf(const timeline::Track& track, AutomationTarget target);

}  // namespace cutline::ui

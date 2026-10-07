#pragma once

// Authoritative metadata for built-in effects.
//
// The command layer uses this to validate authored values, the CPU and future
// GPU renderers use it to identify effects they own, and the UI can build an
// inspector without maintaining a second list of names, ranges, or defaults.

#include "core/anim/Keyframe.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::effects {

enum class Medium { Video, Audio };

enum class Unit {
  None,
  Normalized,
  Percent,
  Pixels,
  Degrees,
  Stops,
  Decibels,
  Multiplier,
};

struct ParameterDescriptor final {
  std::string id;
  std::string display_name;
  int dimension{1};
  anim::Value default_value;
  // Bounds apply component-by-component. Missing bounds mean that direction is
  // intentionally unbounded (position and rotation, for example).
  std::optional<double> minimum;
  std::optional<double> maximum;
  Unit unit{Unit::None};
  bool keyframeable{true};
};

struct EffectDescriptor final {
  std::string id;
  std::string display_name;
  std::string category;
  Medium medium{Medium::Video};
  bool cpu_available{true};
  bool gpu_available{false};
  // Non-empty when preset_name carries an external asset of this kind.
  std::string asset_kind;
  std::vector<ParameterDescriptor> parameters;
};

[[nodiscard]] const std::vector<EffectDescriptor>& BuiltInEffects();
[[nodiscard]] const EffectDescriptor* FindEffect(std::string_view effect_id);
[[nodiscard]] const ParameterDescriptor* FindParameter(const EffectDescriptor& effect,
                                                       std::string_view parameter_id);
[[nodiscard]] std::string ToString(Medium medium);
[[nodiscard]] std::string ToString(Unit unit);

// Empty means valid. Unknown effect ids are valid here: plug-ins and projects
// made by a newer version must remain serializable even when this build cannot
// render them. Once an effect id is registered, however, its parameter names,
// dimensions, finite values, ranges, and required asset reference are strict.
[[nodiscard]] std::string ValidateParameter(std::string_view effect_id, std::string_view parameter_id,
                                            const anim::Value& value);
[[nodiscard]] std::string ValidateAssetReference(std::string_view effect_id, std::string_view asset_reference);

}  // namespace cutline::effects

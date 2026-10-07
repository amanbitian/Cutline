#pragma once

// The render semantic version of a sequence.
//
// Rendering is a function of the project, and the function is allowed to improve:
// a better resampler, a correct colour transform, a fade that really is equal
// power. Each improvement changes the output of every project that was edited
// under the old behaviour, silently, the next time anyone opens it. That is not
// acceptable for work that has been approved, so every sequence records which
// version of the rendering rules it was made under, and the renderers consult it
// instead of assuming the newest.
//
// The rules for changing this table:
//   * A change that alters rendered output for existing content gets a new
//     version. The old behaviour stays selectable here for as long as projects
//     that use it can exist.
//   * A change that cannot alter output (a faster path with identical results) does
//     not.
//   * New sequences are made at kCurrentRenderVersion. Existing sequences keep the
//     version they have until someone upgrades them on purpose, which is a
//     journalled, undoable edit (UpdateSequenceSettings with a render version).
//   * A project whose sequences claim a version newer than this build knows is
//     refused rather than rendered with rules it does not have.
//
// History:
//   v1  The behaviour before versions existed; what every earlier project was
//       made with. Audio: a transition of kind "constant_power" crossfaded
//       linearly (the kind's name was ignored).
//   v2  Audio: "constant_power" crossfades are equal-power (cos/sin weights).
//   v3  Colour management: a sequence's working and display colour spaces are honoured. Decoded
//       frames are converted from the colour space they are tagged with into the working space,
//       and the finished picture from the working space into the display space (render/
//       ColorManagement.h). Before v3 the settings were recorded and ignored. Sequences whose
//       frames, working space and display space all agree are unchanged by it.

#include <cstdint>
#include <stdexcept>
#include <string>

namespace cutline::model {

inline constexpr std::int64_t kLegacyRenderVersion = 1;
inline constexpr std::int64_t kCurrentRenderVersion = 3;

// What each version does, as a set of named decisions. Renderers ask for the
// decision they are about to make; none of them compares version numbers itself.
struct RenderSemantics final {
  // Whether a crossfade named "constant_power" uses equal-power weights.
  bool equal_power_crossfades{true};
  // Whether frames are converted between colour spaces (input to working to display).
  bool color_managed{true};

  [[nodiscard]] static bool Known(std::int64_t version) {
    return version >= kLegacyRenderVersion && version <= kCurrentRenderVersion;
  }

  [[nodiscard]] static RenderSemantics For(std::int64_t version) {
    if (!Known(version)) {
      throw std::invalid_argument("Unknown render version " + std::to_string(version) +
                                  (version > kCurrentRenderVersion ? "; this project was made by a newer Cutline" : ""));
    }
    RenderSemantics semantics;
    semantics.equal_power_crossfades = version >= 2;
    semantics.color_managed = version >= 3;
    return semantics;
  }
};

}  // namespace cutline::model

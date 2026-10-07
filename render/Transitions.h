#pragma once

// The video transitions: how one picture becomes another over a cut.
//
// A transition is a function of two pictures and a progress from 0 (all of the outgoing picture) to 1 (all of the incoming
// one). The compiler hands the compositor the kind's name and the progress at the frame being drawn; this turns the two
// pictures (drawn as layers, premultiplied) into the mixed one. Every kind is plain arithmetic on positions and colours, so
// the same name gives the same picture in the monitor, on the card and in an export, and a golden-image test can check it.
//
// The library, by family:
//   Dissolve   cross_dissolve (the straight mix; also what an unknown name does), additive_dissolve, film_dissolve (mixed
//              in light, not in code values)
//   Dip        dip_to_black, dip_to_white
//   Wipe       wipe_left, wipe_right, wipe_up, wipe_down (the incoming picture grows from the named side's opposite edge:
//              wipe_right reveals it from the left, moving right), barn_door_h, barn_door_v (from the middle outward),
//              clock_wipe
//   Iris       iris_box, iris_circle
//   Push       push_left, push_right, push_up, push_down (both pictures slide, the outgoing one pushed out)
//   Slide      slide_left, slide_right, slide_up, slide_down (the incoming one slides over the outgoing, which stays)
//   Zoom       zoom_dissolve (the outgoing picture grows as it dissolves)
//
// Wipes and irises have a soft edge a twentieth of the picture wide. Pushes and slides move whole pixels (the offset is
// rounded), so the pictures stay sharp. Directions name where the motion goes: in push_left the pictures move left, the
// incoming one arriving from the right.

#include "render/Layer.h"

#include <string>
#include <vector>

namespace cutline::render {

struct TransitionInfo final {
  std::string id;
  std::string name;
  std::string category;
  std::string description;
};

[[nodiscard]] const std::vector<TransitionInfo>& TransitionLibrary();
// Whether the name is one of the library's. The straight dissolve's other spellings (dissolve, constant_power) count too.
[[nodiscard]] bool IsKnownTransition(const std::string& kind);
// Whether the kind is a plain linear mix of the two pictures, which is all a card's mix pass does.
[[nodiscard]] bool IsStraightDissolve(const std::string& kind);

// Mixes `from` and `to` (same size) into `into` at `progress` (clamped to 0..1). An unknown kind is a cross dissolve.
void MixTransition(const std::string& kind, const Layer& from, const Layer& to, float progress, Layer& into);

}  // namespace cutline::render

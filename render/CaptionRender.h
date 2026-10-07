#pragma once

// Draws the captions of a plan into a picture.
//
// One implementation serves the monitor and an export that burns captions in, so the two cannot
// differ: both call this with the same plan. Captions are drawn last, over the finished picture and
// after any conversion to the display's colour space, so their colours are the colours the viewer
// sees and the picture beneath them is untouched except where they are.
//
// Layout. The text is wrapped to the style's maximum width, set in the style's typeface at a size
// that is a fraction of the picture's height, and placed in the title-safe area: at the bottom, top
// or middle, aligned left, centre or right, `margin` in from the edge. Cues showing together stack,
// the first nearest the edge. A background box with padding sits behind the text if the style has
// one, and an outline, if it has one, around the letters.

#include "render/Layer.h"
#include "timeline/TimelineCompiler.h"

#include <string>
#include <vector>

namespace cutline::render {

struct CaptionRenderResult final {
  int drawn{0};
  // Things the viewer would want to know: a font that is not installed, text that could not be drawn.
  std::vector<std::string> warnings;
};

CaptionRenderResult DrawCaptions(Layer& canvas, const std::vector<timeline::CaptionDraw>& captions);

}  // namespace cutline::render

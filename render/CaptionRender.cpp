#include "render/CaptionRender.h"

#include "captions/Captions.h"
#include "render/TextRaster.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace cutline::render {
namespace {

struct Placed final {
  text::Bitmap bitmap;
  captions::Style style;
  int pad{0};
  int outline{0};
  int box_width{0};
  int box_height{0};
  int x{0};
  int y{0};
};

void Over(Layer& canvas, int x, int y, double r, double g, double b, double alpha) {
  if (alpha <= 0.0 || x < 0 || y < 0 || x >= canvas.width() || y >= canvas.height()) return;
  auto& p = canvas.at(x, y);
  const auto a = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
  const auto keep = 1.0f - a;
  p.r = static_cast<float>(r) * a + p.r * keep;
  p.g = static_cast<float>(g) * a + p.g * keep;
  p.b = static_cast<float>(b) * a + p.b * keep;
  p.a = a + p.a * keep;
}

}  // namespace

CaptionRenderResult DrawCaptions(Layer& canvas, const std::vector<timeline::CaptionDraw>& captions_to_draw) {
  CaptionRenderResult result;
  if (captions_to_draw.empty() || canvas.width() <= 0 || canvas.height() <= 0) return result;
  if (!text::Available()) {
    result.warnings.push_back("this build cannot draw text, so the captions were not burned in");
    return result;
  }
  const auto width = static_cast<double>(canvas.width());
  const auto height = static_cast<double>(canvas.height());
  const auto safe = captions::TitleSafe(width, height);

  std::vector<Placed> placed;
  std::set<std::string> substituted;
  for (const auto& caption : captions_to_draw) {
    Placed item;
    item.style = captions::ParseStyle(caption.style_json);
    const auto& style = item.style;
    item.pad = static_cast<int>(std::lround(style.padding * height));
    item.outline = std::clamp(static_cast<int>(std::lround(style.outline_width * height)), 0, 8);
    text::Style font;
    font.family = style.family;
    font.size = style.size * height;
    font.bold = style.bold;
    font.italic = style.italic;
    font.align = style.align == "left" ? text::Align::Left : style.align == "right" ? text::Align::Right : text::Align::Center;
    font.letter_spacing = style.letter_spacing * height;
    const auto wrap = std::max(8, static_cast<int>(std::lround(style.max_width * width)) - 2 * item.pad - 2 * item.outline);
    item.bitmap = text::Rasterize(caption.text, font, wrap);
    if (item.bitmap.empty()) {
      result.warnings.push_back("a caption could not be drawn: " + caption.cue_id);
      continue;
    }
    if (item.bitmap.substituted) substituted.insert(style.family + " -> " + item.bitmap.resolved_family);
    item.box_width = item.bitmap.width + 2 * (item.pad + item.outline);
    item.box_height = item.bitmap.height + 2 * (item.pad + item.outline);
    placed.push_back(std::move(item));
  }
  for (const auto& note : substituted) result.warnings.push_back("a caption font is not installed (" + note + ")");

  // Stack by position: bottom upward from the edge, top downward, middle centred on the picture.
  const int gap = std::max(1, static_cast<int>(std::lround(height * 0.004)));
  double bottom_cursor = safe.y + safe.height, top_cursor = safe.y;
  std::vector<std::size_t> middle;
  bool first_bottom = true, first_top = true;
  for (std::size_t i = 0; i < placed.size(); ++i) {
    auto& item = placed[i];
    const auto margin = item.style.margin * height;
    if (item.style.position == "bottom") {
      if (first_bottom) bottom_cursor -= margin;
      first_bottom = false;
      item.y = static_cast<int>(std::lround(bottom_cursor - item.box_height));
      bottom_cursor = item.y - gap;
    } else if (item.style.position == "top") {
      if (first_top) top_cursor += margin;
      first_top = false;
      item.y = static_cast<int>(std::lround(top_cursor));
      top_cursor += item.box_height + gap;
    } else {
      middle.push_back(i);
    }
    const auto& s = item.style;
    item.x = static_cast<int>(std::lround(s.align == "left" ? safe.x : s.align == "right" ? safe.x + safe.width - item.box_width
                                                                                           : safe.x + (safe.width - item.box_width) / 2.0));
  }
  if (!middle.empty()) {
    double total = 0.0;
    for (const auto i : middle) total += placed[i].box_height + gap;
    double cursor = height / 2.0 - (total - gap) / 2.0;
    for (const auto i : middle) {
      placed[i].y = static_cast<int>(std::lround(cursor));
      cursor += placed[i].box_height + gap;
    }
  }

  for (auto& item : placed) {
    const auto& s = item.style;
    const int inset = item.pad + item.outline;
    // Background box.
    if (s.background[3] > 0.0) {
      for (int y = 0; y < item.box_height; ++y) {
        for (int x = 0; x < item.box_width; ++x) Over(canvas, item.x + x, item.y + y, s.background[0], s.background[1], s.background[2], s.background[3]);
      }
    }
    // Outline: the coverage dilated by a disc.
    if (item.outline > 0 && s.outline[3] > 0.0) {
      const auto radius = item.outline;
      for (int y = -radius; y < item.bitmap.height + radius; ++y) {
        for (int x = -radius; x < item.bitmap.width + radius; ++x) {
          float widest = 0.0f;
          for (int j = -radius; j <= radius && widest < 1.0f; ++j) {
            for (int i = -radius; i <= radius; ++i) {
              if (i * i + j * j > radius * radius) continue;
              const auto sx = x + i, sy = y + j;
              if (sx < 0 || sy < 0 || sx >= item.bitmap.width || sy >= item.bitmap.height) continue;
              widest = std::max(widest, item.bitmap.at(sx, sy));
            }
          }
          if (widest > 0.0f) Over(canvas, item.x + inset + x, item.y + inset + y, s.outline[0], s.outline[1], s.outline[2], s.outline[3] * widest);
        }
      }
    }
    for (int y = 0; y < item.bitmap.height; ++y) {
      for (int x = 0; x < item.bitmap.width; ++x) {
        const auto coverage = item.bitmap.at(x, y);
        if (coverage > 0.0f) Over(canvas, item.x + inset + x, item.y + inset + y, s.color[0], s.color[1], s.color[2], s.color[3] * coverage);
      }
    }
    canvas.MarkDirty(std::max(0, item.x), std::max(0, item.y), std::min(canvas.width() - 1, item.x + item.box_width),
                     std::min(canvas.height() - 1, item.y + item.box_height));
    ++result.drawn;
  }
  return result;
}

}  // namespace cutline::render

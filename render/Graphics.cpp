#include "render/Graphics.h"

#include "media/Source.h"
#include "render/ParallelRows.h"
#include "render/TextRaster.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <set>
#include <stdexcept>

namespace cutline::render::graphics {
namespace {

void Over(Layer& layer, int x, int y, const std::array<double, 4>& colour, double coverage, double opacity) {
  if (x < 0 || y < 0 || x >= layer.width() || y >= layer.height()) return;
  const auto alpha = static_cast<float>(std::clamp(colour[3] * coverage * opacity, 0.0, 1.0));
  if (alpha <= 0.0f) return;
  auto& pixel = layer.at(x, y);
  const auto keep = 1.0f - alpha;
  pixel.r = static_cast<float>(colour[0]) * alpha + pixel.r * keep;
  pixel.g = static_cast<float>(colour[1]) * alpha + pixel.g * keep;
  pixel.b = static_cast<float>(colour[2]) * alpha + pixel.b * keep;
  pixel.a = alpha + pixel.a * keep;
}

[[nodiscard]] std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

[[nodiscard]] std::string Trim(const std::string& value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

[[nodiscard]] std::vector<std::string> Families(const std::string& list) {
  std::vector<std::string> families;
  std::size_t start = 0;
  while (start <= list.size()) {
    const auto comma = list.find(',', start);
    const auto part = Trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!part.empty()) families.push_back(part);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return families;
}

// The installed families, looked up once: the list does not change while a project is being rendered,
// and asking the system for it for every text element of every frame would cost more than the drawing.
[[nodiscard]] bool Installed(const std::string& family) {
  static std::once_flag once;
  static std::set<std::string> installed;
  std::call_once(once, []() {
    for (const auto& name : text::InstalledFamilies()) installed.insert(Lower(name));
  });
  return installed.count(Lower(family)) != 0;
}

// ------------------------------------------------------------ effects path ----
//
// An element that is rotated, outlined, shadowed or rounded is drawn in four steps: the element upright on a
// sprite of its own (premultiplied, a margin around it for an outline), the sprite turned about the middle of the
// element, a shadow made from the turned sprite's alpha, and both composited on the layer. An element with none of
// these never comes here, so a plain title costs what it did before.

struct Sprite final {
  int x{0}, y{0}, width{0}, height{0};  // where its top-left pixel lies on the layer
  std::vector<Pixel> pixels;
  // The point the element turns about, on the layer.
  double pivot_x{0.0}, pivot_y{0.0};

  [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
  [[nodiscard]] Pixel& at(int i, int j) { return pixels[static_cast<std::size_t>(j) * width + i]; }
  [[nodiscard]] const Pixel& at(int i, int j) const { return pixels[static_cast<std::size_t>(j) * width + i]; }
  void Allocate(int left, int top, int w, int h) {
    x = left;
    y = top;
    width = std::max(w, 0);
    height = std::max(h, 0);
    pixels.assign(static_cast<std::size_t>(width) * height, Pixel{});
  }
};

constexpr int kMaximumSpriteSide = 16384;

// How far a point is outside a box with rounded corners (negative inside): half sizes and radius in pixels.
[[nodiscard]] double BoxDistance(double px, double py, double half_width, double half_height, double radius) {
  radius = std::clamp(radius, 0.0, std::min(half_width, half_height));
  const auto qx = std::abs(px) - (half_width - radius);
  const auto qy = std::abs(py) - (half_height - radius);
  return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
}

// An approximation of the distance to an ellipse that is exact for a circle and good to within a fraction of a
// pixel at the edge of any ellipse, which is where it is used.
[[nodiscard]] double EllipseDistance(double px, double py, double a, double b) {
  if (a < 1e-6 || b < 1e-6) return 1.0;
  const auto k0 = std::hypot(px / a, py / b);
  const auto k1 = std::hypot(px / (a * a), py / (b * b));
  if (k1 < 1e-12) return -std::min(a, b);
  return k0 * (k0 - 1.0) / k1;
}

[[nodiscard]] float Coverage(double distance) { return static_cast<float>(std::clamp(0.5 - distance, 0.0, 1.0)); }

void AddPremultiplied(Pixel& pixel, const std::array<double, 4>& colour, double coverage) {
  const auto alpha = static_cast<float>(colour[3] * coverage);
  pixel.r += static_cast<float>(colour[0]) * alpha;
  pixel.g += static_cast<float>(colour[1]) * alpha;
  pixel.b += static_cast<float>(colour[2]) * alpha;
  pixel.a += alpha;
}

// The upright element: a rectangle, ellipse or image (converted to float, or null for a plain shape) drawn from its
// own distance field, so edges, rounded corners and an inside outline are exact at any size.
[[nodiscard]] Sprite ShapeSprite(const Element& element, const Placement& placed, const media::VideoFrame* image, double stroke_px, double radius_px) {
  Sprite sprite;
  const auto margin = 1;
  sprite.Allocate(placed.x - margin, placed.y - margin, placed.width + 2 * margin, placed.height + 2 * margin);
  sprite.pivot_x = placed.x + placed.width / 2.0;
  sprite.pivot_y = placed.y + placed.height / 2.0;
  const auto half_width = placed.width / 2.0, half_height = placed.height / 2.0;
  const bool ellipse = element.type == ElementType::Ellipse;
  util::ParallelRows(0, sprite.height - 1, [&](int j) {
    for (int i = 0; i < sprite.width; ++i) {
      const auto px = (i - margin) + 0.5 - half_width, py = (j - margin) + 0.5 - half_height;
      const auto distance = ellipse ? EllipseDistance(px, py, half_width, half_height) : BoxDistance(px, py, half_width, half_height, radius_px);
      const auto inside = Coverage(distance);
      if (inside <= 0.0f) continue;
      const auto inner = stroke_px > 0.0 ? Coverage(distance + stroke_px) : inside;
      auto& pixel = sprite.at(i, j);
      if (image != nullptr) {
        const auto u = ((i - margin) + 0.5) / std::max(placed.width, 1) * image->width() - 0.5;
        const auto v = ((j - margin) + 0.5) / std::max(placed.height, 1) * image->height() - 0.5;
        const auto x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
        const auto fx = static_cast<float>(u - x0), fy = static_cast<float>(v - y0);
        float acc[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
          const auto sx = std::clamp(x0 + (k & 1), 0, image->width() - 1), sy = std::clamp(y0 + (k >> 1), 0, image->height() - 1);
          const auto weight = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
          const auto* texel = image->row_f32(sy) + static_cast<std::size_t>(sx) * 4;
          acc[0] += texel[0] * texel[3] * weight;
          acc[1] += texel[1] * texel[3] * weight;
          acc[2] += texel[2] * texel[3] * weight;
          acc[3] += texel[3] * weight;
        }
        pixel = {acc[0] * inner, acc[1] * inner, acc[2] * inner, acc[3] * inner};
      } else {
        AddPremultiplied(pixel, element.fill, inner);
      }
      if (stroke_px > 0.0 && inside > inner) AddPremultiplied(pixel, element.stroke, inside - inner);
    }
  });
  return sprite;
}

// Letters: the coverage bitmap filled, with an outline made by growing the coverage by the stroke width.
[[nodiscard]] Sprite TextSprite(const Element& element, const Placement& placed, const text::Bitmap& bitmap, double stroke_px) {
  Sprite sprite;
  const auto margin = stroke_px > 0.0 ? static_cast<int>(std::ceil(stroke_px)) + 1 : 1;
  sprite.Allocate(placed.x - margin, placed.y - margin, bitmap.width + 2 * margin, bitmap.height + 2 * margin);
  sprite.pivot_x = placed.x + bitmap.width / 2.0;
  sprite.pivot_y = placed.y + bitmap.height / 2.0;
  std::vector<std::pair<int, int>> disc;
  if (stroke_px > 0.0) {
    const auto reach = static_cast<int>(std::ceil(stroke_px));
    for (int dy = -reach; dy <= reach; ++dy) for (int dx = -reach; dx <= reach; ++dx) {
      if (dx * dx + dy * dy <= stroke_px * stroke_px + 0.25) disc.emplace_back(dx, dy);
    }
  }
  util::ParallelRows(0, sprite.height - 1, [&](int j) {
    for (int i = 0; i < sprite.width; ++i) {
      const auto bx = i - margin, by = j - margin;
      const auto own = (bx >= 0 && by >= 0 && bx < bitmap.width && by < bitmap.height) ? bitmap.at(bx, by) : 0.0f;
      auto& pixel = sprite.at(i, j);
      if (!disc.empty()) {
        float grown = own;
        for (const auto& [dx, dy] : disc) {
          const auto sx = bx + dx, sy = by + dy;
          if (sx < 0 || sy < 0 || sx >= bitmap.width || sy >= bitmap.height) continue;
          grown = std::max(grown, bitmap.at(sx, sy));
          if (grown >= 1.0f) break;
        }
        AddPremultiplied(pixel, element.stroke, grown);
      }
      if (own > 0.0f) {
        // The letters over their outline.
        const auto alpha = static_cast<float>(element.fill[3] * own);
        const auto keep = 1.0f - alpha;
        pixel.r = static_cast<float>(element.fill[0]) * alpha + pixel.r * keep;
        pixel.g = static_cast<float>(element.fill[1]) * alpha + pixel.g * keep;
        pixel.b = static_cast<float>(element.fill[2]) * alpha + pixel.b * keep;
        pixel.a = alpha + pixel.a * keep;
      }
    }
  });
  return sprite;
}

// The sprite turned clockwise by the angle (degrees) about its pivot, sampled bilinearly.
[[nodiscard]] Sprite Turn(const Sprite& source, double degrees) {
  const auto radians = degrees * 3.14159265358979323846 / 180.0;
  const auto c = std::cos(radians), s = std::sin(radians);
  // Where the corners go (y runs down, so a positive angle turns clockwise on the screen).
  double min_x = 1e18, min_y = 1e18, max_x = -1e18, max_y = -1e18;
  for (int corner = 0; corner < 4; ++corner) {
    const auto dx = ((corner & 1) ? source.x + source.width : source.x) - source.pivot_x;
    const auto dy = ((corner >> 1) ? source.y + source.height : source.y) - source.pivot_y;
    const auto rx = source.pivot_x + dx * c - dy * s, ry = source.pivot_y + dx * s + dy * c;
    min_x = std::min(min_x, rx); max_x = std::max(max_x, rx);
    min_y = std::min(min_y, ry); max_y = std::max(max_y, ry);
  }
  Sprite turned;
  turned.pivot_x = source.pivot_x;
  turned.pivot_y = source.pivot_y;
  const auto left = static_cast<int>(std::floor(min_x)) - 1, top = static_cast<int>(std::floor(min_y)) - 1;
  const auto right = static_cast<int>(std::ceil(max_x)) + 1, bottom = static_cast<int>(std::ceil(max_y)) + 1;
  if (right - left > kMaximumSpriteSide || bottom - top > kMaximumSpriteSide) return turned;
  turned.Allocate(left, top, right - left, bottom - top);
  util::ParallelRows(0, turned.height - 1, [&](int j) {
    for (int i = 0; i < turned.width; ++i) {
      const auto dx = turned.x + i + 0.5 - source.pivot_x, dy = turned.y + j + 0.5 - source.pivot_y;
      // The inverse turn, into the sprite's own pixel grid (sample centres at whole numbers + 0.5).
      const auto u = source.pivot_x + dx * c + dy * s - source.x - 0.5;
      const auto v = source.pivot_y - dx * s + dy * c - source.y - 0.5;
      const auto x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
      if (x0 < -1 || y0 < -1 || x0 >= source.width || y0 >= source.height) continue;
      const auto fx = static_cast<float>(u - x0), fy = static_cast<float>(v - y0);
      Pixel acc;
      for (int k = 0; k < 4; ++k) {
        const auto sx = x0 + (k & 1), sy = y0 + (k >> 1);
        if (sx < 0 || sy < 0 || sx >= source.width || sy >= source.height) continue;
        const auto weight = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
        const auto& texel = source.at(sx, sy);
        acc.r += texel.r * weight;
        acc.g += texel.g * weight;
        acc.b += texel.b * weight;
        acc.a += texel.a * weight;
      }
      turned.at(i, j) = acc;
    }
  });
  return turned;
}

// A running-sum box blur of a mask, three times over, which is close to a Gaussian of the same width.
void Blur(std::vector<float>& mask, int width, int height, int radius) {
  if (radius <= 0 || width <= 0 || height <= 0) return;
  const auto window = 2 * radius + 1;
  std::vector<float> scratch(mask.size());
  for (int pass = 0; pass < 3; ++pass) {
    util::ParallelRows(0, height - 1, [&](int y) {
      const auto* in = mask.data() + static_cast<std::size_t>(y) * width;
      auto* out = scratch.data() + static_cast<std::size_t>(y) * width;
      double sum = 0.0;
      for (int x = 0; x < std::min(radius, width); ++x) sum += in[x];
      for (int x = 0; x < width; ++x) {
        if (x + radius < width) sum += in[x + radius];
        if (x - radius - 1 >= 0) sum -= in[x - radius - 1];
        out[x] = static_cast<float>(sum / window);
      }
    });
    util::ParallelRows(0, width - 1, [&](int x) {
      double sum = 0.0;
      for (int y = 0; y < std::min(radius, height); ++y) sum += scratch[static_cast<std::size_t>(y) * width + x];
      for (int y = 0; y < height; ++y) {
        if (y + radius < height) sum += scratch[static_cast<std::size_t>(y + radius) * width + x];
        if (y - radius - 1 >= 0) sum -= scratch[static_cast<std::size_t>(y - radius - 1) * width + x];
        mask[static_cast<std::size_t>(y) * width + x] = static_cast<float>(sum / window);
      }
    });
  }
}

// The shadow of a sprite: its alpha moved by (dx, dy) and softened, in the shadow's colour.
[[nodiscard]] Sprite ShadowOf(const Sprite& source, const std::array<double, 4>& colour, int dx, int dy, double blur_px) {
  Sprite shadow;
  const auto radius = static_cast<int>(std::lround(blur_px / 2.0));
  const auto margin = 3 * radius + 1;
  if (source.width + 2 * margin > kMaximumSpriteSide || source.height + 2 * margin > kMaximumSpriteSide) return shadow;
  shadow.Allocate(source.x + dx - margin, source.y + dy - margin, source.width + 2 * margin, source.height + 2 * margin);
  std::vector<float> mask(static_cast<std::size_t>(shadow.width) * shadow.height, 0.0f);
  for (int j = 0; j < source.height; ++j) for (int i = 0; i < source.width; ++i) {
    mask[static_cast<std::size_t>(j + margin) * shadow.width + i + margin] = source.at(i, j).a;
  }
  Blur(mask, shadow.width, shadow.height, radius);
  for (std::size_t n = 0; n < mask.size(); ++n) {
    const auto alpha = static_cast<float>(colour[3]) * mask[n];
    shadow.pixels[n] = {static_cast<float>(colour[0]) * alpha, static_cast<float>(colour[1]) * alpha, static_cast<float>(colour[2]) * alpha, alpha};
  }
  return shadow;
}

void CompositeSprite(Layer& layer, const Sprite& sprite, double opacity) {
  if (sprite.empty() || opacity <= 0.0) return;
  const auto x0 = std::max(sprite.x, 0), x1 = std::min(sprite.x + sprite.width, layer.width());
  const auto y0 = std::max(sprite.y, 0), y1 = std::min(sprite.y + sprite.height, layer.height());
  if (x1 <= x0 || y1 <= y0) return;
  const auto weight = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
  util::ParallelRows(y0, y1 - 1, [&](int y) {
    for (int x = x0; x < x1; ++x) {
      const auto& source = sprite.at(x - sprite.x, y - sprite.y);
      if (source.a <= 0.0f) continue;
      auto& pixel = layer.at(x, y);
      const auto keep = 1.0f - source.a * weight;
      pixel.r = source.r * weight + pixel.r * keep;
      pixel.g = source.g * weight + pixel.g * keep;
      pixel.b = source.b * weight + pixel.b * keep;
      pixel.a = source.a * weight + pixel.a * keep;
    }
  });
  layer.MarkDirty(x0, y0, x1 - 1, y1 - 1);
}

// Finishes an upright sprite: turns it, shadows it, and puts both on the layer.
void Finish(Layer& layer, Sprite sprite, const Element& element, const Placement& placed) {
  if (sprite.empty()) return;
  if (std::fmod(element.rotation, 360.0) != 0.0) sprite = Turn(sprite, element.rotation);
  if (element.shadow[3] > 0.0) {
    const auto shadow = ShadowOf(sprite, element.shadow, static_cast<int>(std::lround(element.shadow_x * placed.unit)),
                                 static_cast<int>(std::lround(element.shadow_y * placed.unit)), element.shadow_blur * placed.unit);
    CompositeSprite(layer, shadow, element.opacity);
  }
  CompositeSprite(layer, sprite, element.opacity);
}

}  // namespace

const media::VideoFrame* StillCache::Find(const std::string& asset) {
  if (asset.empty()) return nullptr;
  std::filesystem::path path(asset);
  if (path.is_relative() && !root_.empty()) path = std::filesystem::path(root_) / path;
  std::error_code error;
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error) return nullptr;
  const auto key = path.lexically_normal().string();
  auto found = entries_.find(key);
  if (found != entries_.end() && found->second.modified == modified) return found->second.frame.get();
  Entry entry;
  entry.modified = modified;
  try {
    if (auto source = media::SourceRegistry::Instance().Open(key)) {
      if (auto frame = source->ReadVideo(time::RationalTime(0, 1))) entry.frame = std::make_unique<media::VideoFrame>(std::move(*frame));
    }
  } catch (const std::exception&) {
    entry.frame.reset();
  }
  return (entries_[key] = std::move(entry)).frame.get();
}

std::string ResolveFontFamily(const std::string& font_list) {
  const auto families = Families(font_list);
  if (families.empty()) return "Arial";
  if (families.size() == 1) return families.front();
  for (const auto& family : families) {
    if (Installed(family)) return family;
  }
  return families.front();
}

std::vector<std::string> MissingFonts(const Document& document) {
  std::set<std::string> missing;
  for (const auto& element : document.elements) {
    if (element.type != ElementType::Text) continue;
    const auto families = Families(element.font);
    const bool any = std::any_of(families.begin(), families.end(), [](const std::string& family) { return Installed(family); });
    if (!any) missing.insert(families.empty() ? std::string("Arial") : families.front());
  }
  return {missing.begin(), missing.end()};
}

DrawResult Draw(Layer& layer, const Document& document, const ImageResolver& images, double seconds) {
  Validate(document);
  DrawResult result;
  for (const auto& authored : document.elements) {
    const auto element = authored.animations.empty() ? authored : Evaluate(authored, seconds);
    const auto placed = Place(document, element, layer.width(), layer.height());
    const auto x0 = placed.x;
    const auto y0 = placed.y;
    const auto width = placed.width;
    const auto height = placed.height;
    const bool effects = UsesEffects(element);
    const auto stroke_px = std::min(element.stroke_width * placed.unit, 256.0);
    const auto radius_px = element.corner_radius * placed.unit;
    if (element.type == ElementType::Text) {
      if (!text::Available()) { result.warnings.push_back("text rasterisation is unavailable for graphic " + element.id); continue; }
      text::Style style;
      style.family = ResolveFontFamily(element.font);
      style.size = placed.font_pixels;
      style.bold = element.bold;
      style.italic = element.italic;
      style.align = element.align == "left" ? text::Align::Left : element.align == "right" ? text::Align::Right : text::Align::Center;
      const auto bitmap = text::Rasterize(element.text, style, width);
      if (bitmap.empty()) { result.warnings.push_back("text could not be drawn for graphic " + element.id); continue; }
      if (bitmap.substituted) {
        result.warnings.push_back("font " + style.family + " is not installed; graphic " + element.id + " is drawn in " + bitmap.resolved_family);
      }
      if (effects) {
        Finish(layer, TextSprite(element, placed, bitmap, stroke_px), element, placed);
        ++result.elements_drawn;
        continue;
      }
      for (int y = 0; y < bitmap.height; ++y) for (int x = 0; x < bitmap.width; ++x)
        Over(layer, x0 + x, y0 + y, element.fill, bitmap.at(x, y), element.opacity);
      layer.MarkDirty(x0, y0, x0 + bitmap.width - 1, y0 + bitmap.height - 1);
    } else if (element.type == ElementType::Image) {
      const auto* image = images ? images(element.asset) : nullptr;
      if (image == nullptr || !image->valid()) { result.warnings.push_back("image is unavailable for graphic " + element.id); continue; }
      auto converted = image->format() == media::PixelFormat::RgbaF32 ? image->Clone()
                                                                      : media::ConvertFrame(*image, media::PixelFormat::RgbaF32);
      if (effects) {
        Finish(layer, ShapeSprite(element, placed, &converted, stroke_px, radius_px), element, placed);
        ++result.elements_drawn;
        continue;
      }
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto sx = std::clamp(x * converted.width() / std::max(width, 1), 0, converted.width() - 1);
        const auto sy = std::clamp(y * converted.height() / std::max(height, 1), 0, converted.height() - 1);
        const auto* pixel = converted.row_f32(sy) + static_cast<std::size_t>(sx) * 4;
        Over(layer, x0 + x, y0 + y, {pixel[0], pixel[1], pixel[2], pixel[3]}, 1.0, element.opacity);
      }
      layer.MarkDirty(x0, y0, x0 + width - 1, y0 + height - 1);
    } else if (effects) {
      Finish(layer, ShapeSprite(element, placed, nullptr, stroke_px, radius_px), element, placed);
    } else {
      for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        double coverage = 1.0;
        if (element.type == ElementType::Ellipse) {
          const auto nx = (static_cast<double>(x) + 0.5) / std::max(width, 1) * 2.0 - 1.0;
          const auto ny = (static_cast<double>(y) + 0.5) / std::max(height, 1) * 2.0 - 1.0;
          coverage = nx * nx + ny * ny <= 1.0 ? 1.0 : 0.0;
        }
        Over(layer, x0 + x, y0 + y, element.fill, coverage, element.opacity);
      }
      layer.MarkDirty(x0, y0, x0 + width - 1, y0 + height - 1);
    }
    ++result.elements_drawn;
  }
  return result;
}

}  // namespace cutline::render::graphics

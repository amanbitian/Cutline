#include "render/Transitions.h"

#include <algorithm>
#include <cmath>

namespace cutline::render {
namespace {

constexpr float kSoft = 0.05f;   // half the width of a soft edge, as a fraction of the picture

float Smooth(float edge0, float edge1, float v) {
  const float t = std::clamp((v - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// How much of the incoming picture shows where `position` (0..1 across the shape of the wipe) is, at this progress.
float Reveal(float position, float progress) {
  const float edge = -kSoft + progress * (1.0f + 2.0f * kSoft);
  return 1.0f - Smooth(edge - kSoft, edge + kSoft, position);
}

Pixel Mix(const Pixel& a, const Pixel& b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Pixel Bilinear(const Layer& layer, float x, float y) {
  const int w = layer.width(), h = layer.height();
  x = std::clamp(x - 0.5f, 0.0f, static_cast<float>(w - 1));
  y = std::clamp(y - 0.5f, 0.0f, static_cast<float>(h - 1));
  const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
  const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
  const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
  return Mix(Mix(layer.at(x0, y0), layer.at(x1, y0), tx), Mix(layer.at(x0, y1), layer.at(x1, y1), tx), ty);
}

float Light(float v) { return std::pow(std::max(v, 0.0f), 2.2f); }
float Code(float v) { return std::pow(std::max(v, 0.0f), 1.0f / 2.2f); }

enum class Shape {
  Dissolve, Additive, Film, DipBlack, DipWhite,
  WipeLeft, WipeRight, WipeUp, WipeDown, BarnH, BarnV, Clock, IrisBox, IrisCircle,
  PushLeft, PushRight, PushUp, PushDown, SlideLeft, SlideRight, SlideUp, SlideDown, Zoom,
};

struct Entry final {
  const char* id;
  const char* name;
  const char* category;
  const char* description;
  Shape shape;
};

const Entry kEntries[] = {
    {"cross_dissolve", "Cross Dissolve", "Dissolve", "The outgoing picture fades into the incoming one", Shape::Dissolve},
    {"additive_dissolve", "Additive Dissolve", "Dissolve", "Both pictures are at full strength in the middle, so it brightens there", Shape::Additive},
    {"film_dissolve", "Film Dissolve", "Dissolve", "A dissolve mixed in light rather than in code values, which keeps the middle from sagging", Shape::Film},
    {"dip_to_black", "Dip to Black", "Dip", "Fades out to black, then in", Shape::DipBlack},
    {"dip_to_white", "Dip to White", "Dip", "Fades out to white, then in", Shape::DipWhite},
    {"wipe_right", "Wipe Right", "Wipe", "The incoming picture is revealed from the left edge, moving right", Shape::WipeRight},
    {"wipe_left", "Wipe Left", "Wipe", "The incoming picture is revealed from the right edge, moving left", Shape::WipeLeft},
    {"wipe_down", "Wipe Down", "Wipe", "The incoming picture is revealed from the top edge, moving down", Shape::WipeDown},
    {"wipe_up", "Wipe Up", "Wipe", "The incoming picture is revealed from the bottom edge, moving up", Shape::WipeUp},
    {"barn_door_h", "Barn Door (horizontal)", "Wipe", "The incoming picture opens from the middle, outward to the sides", Shape::BarnH},
    {"barn_door_v", "Barn Door (vertical)", "Wipe", "The incoming picture opens from the middle, outward to the top and bottom", Shape::BarnV},
    {"clock_wipe", "Clock Wipe", "Wipe", "A hand sweeps clockwise from twelve o'clock, revealing the incoming picture", Shape::Clock},
    {"iris_box", "Iris Box", "Iris", "A growing rectangle from the middle", Shape::IrisBox},
    {"iris_circle", "Iris Circle", "Iris", "A growing circle from the middle", Shape::IrisCircle},
    {"push_left", "Push Left", "Push", "Both pictures move left; the incoming one arrives from the right", Shape::PushLeft},
    {"push_right", "Push Right", "Push", "Both pictures move right; the incoming one arrives from the left", Shape::PushRight},
    {"push_up", "Push Up", "Push", "Both pictures move up; the incoming one arrives from below", Shape::PushUp},
    {"push_down", "Push Down", "Push", "Both pictures move down; the incoming one arrives from above", Shape::PushDown},
    {"slide_left", "Slide Left", "Slide", "The incoming picture slides in from the right over the outgoing one", Shape::SlideLeft},
    {"slide_right", "Slide Right", "Slide", "The incoming picture slides in from the left over the outgoing one", Shape::SlideRight},
    {"slide_up", "Slide Up", "Slide", "The incoming picture slides in from below over the outgoing one", Shape::SlideUp},
    {"slide_down", "Slide Down", "Slide", "The incoming picture slides in from above over the outgoing one", Shape::SlideDown},
    {"zoom_dissolve", "Zoom Dissolve", "Zoom", "The outgoing picture grows by half as it dissolves into the incoming one", Shape::Zoom},
};

const Entry* Find(const std::string& kind) {
  for (const auto& entry : kEntries) {
    if (kind == entry.id) return &entry;
  }
  return nullptr;
}

}  // namespace

const std::vector<TransitionInfo>& TransitionLibrary() {
  static const std::vector<TransitionInfo> library = [] {
    std::vector<TransitionInfo> out;
    for (const auto& entry : kEntries) out.push_back({entry.id, entry.name, entry.category, entry.description});
    return out;
  }();
  return library;
}

bool IsKnownTransition(const std::string& kind) { return Find(kind) != nullptr || kind == "dissolve" || kind == "constant_power"; }

bool IsStraightDissolve(const std::string& kind) { return kind == "cross_dissolve" || kind == "dissolve" || kind == "constant_power" || Find(kind) == nullptr; }

void MixTransition(const std::string& kind, const Layer& from, const Layer& to, float requested, Layer& into) {
  const auto* entry = Find(kind);
  if (entry == nullptr || entry->shape == Shape::Dissolve) {
    Layer::Mix(from, to, requested, into);
    return;
  }
  const float p = std::clamp(requested, 0.0f, 1.0f);
  const int w = from.width(), h = from.height();
  into.Reset(w, h);
  if (w <= 0 || h <= 0 || to.width() != w || to.height() != h) return;
  const float fw = static_cast<float>(w), fh = static_cast<float>(h);
  const int dx = static_cast<int>(std::lround(p * fw)), dy = static_cast<int>(std::lround(p * fh));
  const float aspect = fw / fh;
  const float max_radius = std::sqrt((aspect * 0.5f) * (aspect * 0.5f) + 0.25f);
  constexpr float kPi = 3.14159265358979f;

  const auto run = [&](auto&& pixel_at) {
    ParallelRows(0, h - 1, [&](int y) {
      for (int x = 0; x < w; ++x) into.at(x, y) = pixel_at(x, y);
    });
    into.MarkWhole();
  };

  switch (entry->shape) {
    case Shape::Dissolve: break;   // handled above
    case Shape::Additive: {
      const float a = std::clamp(2.0f * (1.0f - p), 0.0f, 1.0f), b = std::clamp(2.0f * p, 0.0f, 1.0f);
      run([&](int x, int y) {
        const auto& f = from.at(x, y);
        const auto& t = to.at(x, y);
        return Pixel{std::min(f.r * a + t.r * b, 1.0f), std::min(f.g * a + t.g * b, 1.0f), std::min(f.b * a + t.b * b, 1.0f), std::min(f.a * a + t.a * b, 1.0f)};
      });
      break;
    }
    case Shape::Film: {
      run([&](int x, int y) {
        const auto& f = from.at(x, y);
        const auto& t = to.at(x, y);
        // On colour (not premultiplied) values, in light; alpha is mixed as it is.
        const auto straight = [](const Pixel& q, int channel) {
          const float v = channel == 0 ? q.r : channel == 1 ? q.g : q.b;
          return q.a > 0.0f ? v / q.a : 0.0f;
        };
        const float alpha = f.a + (t.a - f.a) * p;
        Pixel out{0, 0, 0, alpha};
        float* channels[3] = {&out.r, &out.g, &out.b};
        for (int c = 0; c < 3; ++c) {
          const float mixed = Light(straight(f, c)) * (1.0f - p) + Light(straight(t, c)) * p;
          *channels[c] = Code(mixed) * alpha;
        }
        return out;
      });
      break;
    }
    case Shape::DipBlack:
    case Shape::DipWhite: {
      const bool white = entry->shape == Shape::DipWhite;
      const Pixel colour{white ? 1.0f : 0.0f, white ? 1.0f : 0.0f, white ? 1.0f : 0.0f, 1.0f};
      run([&](int x, int y) {
        if (p < 0.5f) return Mix(from.at(x, y), colour, p * 2.0f);
        return Mix(colour, to.at(x, y), p * 2.0f - 1.0f);
      });
      break;
    }
    case Shape::WipeRight: case Shape::WipeLeft: case Shape::WipeDown: case Shape::WipeUp:
    case Shape::BarnH: case Shape::BarnV: case Shape::Clock: case Shape::IrisBox: case Shape::IrisCircle: {
      const auto shape = entry->shape;
      run([&](int x, int y) {
        const float u = (static_cast<float>(x) + 0.5f) / fw, v = (static_cast<float>(y) + 0.5f) / fh;
        float position = 0.0f;
        switch (shape) {
          case Shape::WipeRight: position = u; break;
          case Shape::WipeLeft: position = 1.0f - u; break;
          case Shape::WipeDown: position = v; break;
          case Shape::WipeUp: position = 1.0f - v; break;
          case Shape::BarnH: position = std::abs(u - 0.5f) * 2.0f; break;
          case Shape::BarnV: position = std::abs(v - 0.5f) * 2.0f; break;
          case Shape::IrisBox: position = std::max(std::abs(u - 0.5f), std::abs(v - 0.5f)) * 2.0f; break;
          case Shape::IrisCircle: position = std::sqrt((u - 0.5f) * (u - 0.5f) * aspect * aspect + (v - 0.5f) * (v - 0.5f)) / max_radius; break;
          case Shape::Clock: {
            // Clockwise from twelve o'clock, 0 to 1 round the middle.
            float angle = std::atan2(u - 0.5f, -(v - 0.5f)) / (2.0f * kPi);
            if (angle < 0.0f) angle += 1.0f;
            position = angle;
            break;
          }
          default: break;
        }
        return Mix(from.at(x, y), to.at(x, y), Reveal(position, p));
      });
      break;
    }
    case Shape::PushLeft: run([&](int x, int y) { return x + dx < w ? from.at(x + dx, y) : to.at(x + dx - w, y); }); break;
    case Shape::PushRight: run([&](int x, int y) { return x - dx >= 0 ? from.at(x - dx, y) : to.at(x - dx + w, y); }); break;
    case Shape::PushUp: run([&](int x, int y) { return y + dy < h ? from.at(x, y + dy) : to.at(x, y + dy - h); }); break;
    case Shape::PushDown: run([&](int x, int y) { return y - dy >= 0 ? from.at(x, y - dy) : to.at(x, y - dy + h); }); break;
    case Shape::SlideLeft: run([&](int x, int y) { return x >= w - dx ? to.at(x - (w - dx), y) : from.at(x, y); }); break;
    case Shape::SlideRight: run([&](int x, int y) { return x < dx ? to.at(x + (w - dx), y) : from.at(x, y); }); break;
    case Shape::SlideUp: run([&](int x, int y) { return y >= h - dy ? to.at(x, y - (h - dy)) : from.at(x, y); }); break;
    case Shape::SlideDown: run([&](int x, int y) { return y < dy ? to.at(x, y + (h - dy)) : from.at(x, y); }); break;
    case Shape::Zoom: {
      const float scale = 1.0f + 0.5f * p;
      run([&](int x, int y) {
        const float sx = (static_cast<float>(x) + 0.5f - fw * 0.5f) / scale + fw * 0.5f;
        const float sy = (static_cast<float>(y) + 0.5f - fh * 0.5f) / scale + fh * 0.5f;
        return Mix(Bilinear(from, sx, sy), to.at(x, y), p);
      });
      break;
    }
  }
}

}  // namespace cutline::render

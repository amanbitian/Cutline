#include "ui/MaskEditor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cutline::ui {
namespace {

using effects::mask::Animation;
using effects::mask::Key;
using effects::mask::Point;
using effects::mask::Shape;
using Pair = std::pair<double, double>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRotateKnobPixels = 28.0;
constexpr double kMinimumPixels = 2.0;

// ---- the frame of a rectangle or ellipse: normalised <-> the shape's own pixels
struct Frame final {
  double cx, cy;      // centre, normalised
  double hx, hy;      // half sizes, pixels
  double cosine, sine;
  double w, h;        // the picture, pixels
  [[nodiscard]] Pair ToLocal(double x, double y) const {
    const double dx = (x - cx) * w, dy = (y - cy) * h;
    return {dx * cosine + dy * sine, -dx * sine + dy * cosine};
  }
  [[nodiscard]] Pair ToPicture(double lx, double ly) const {
    return {cx + (lx * cosine - ly * sine) / w, cy + (lx * sine + ly * cosine) / h};
  }
};

Frame FrameOf(const MaskDocument& document, PictureSize picture) {
  const double radians = document.rotation * kPi / 180.0;
  return {document.center_x, document.center_y, document.width * picture.width * 0.5, document.height * picture.height * 0.5,
          std::cos(radians), std::sin(radians), picture.width, picture.height};
}

[[nodiscard]] bool IsPath(const MaskDocument& document) { return document.shape == Shape::Bezier; }

Pair Lerp(Pair a, Pair b, double t) { return {a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t}; }

// The polyline of one path segment, `steps` pieces, from vertex `index` to the next.
std::vector<Pair> SegmentPoints(const MaskDocument& document, std::size_t index, int steps) {
  const auto& a = document.points[index];
  const auto& b = document.points[(index + 1) % document.points.size()];
  std::vector<Pair> points;
  for (int step = 0; step <= steps; ++step) {
    const double t = static_cast<double>(step) / steps, u = 1.0 - t;
    points.push_back({u * u * u * a.x + 3 * u * u * t * a.out_x + 3 * u * t * t * b.in_x + t * t * t * b.x,
                      u * u * u * a.y + 3 * u * u * t * a.out_y + 3 * u * t * t * b.in_y + t * t * t * b.y});
  }
  return points;
}

double DistanceToSegment(double px, double py, Pair a, Pair b, double* along = nullptr) {
  const double dx = b.first - a.first, dy = b.second - a.second;
  const double length_squared = dx * dx + dy * dy;
  const double t = length_squared <= 1e-18 ? 0.0 : std::clamp(((px - a.first) * dx + (py - a.second) * dy) / length_squared, 0.0, 1.0);
  if (along != nullptr) *along = t;
  return std::hypot(px - (a.first + dx * t), py - (a.second + dy * t));
}

bool InsidePolygon(const std::vector<Pair>& polygon, double x, double y) {
  bool inside = false;
  for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    if ((polygon[i].second > y) != (polygon[j].second > y) &&
        x < (polygon[j].first - polygon[i].first) * (y - polygon[i].second) / (polygon[j].second - polygon[i].second) + polygon[i].first) {
      inside = !inside;
    }
  }
  return inside;
}

std::string PointProperty(std::size_t index, const char* component) { return "point_" + std::to_string(index) + "_" + component; }

void DropPointAnimation(MaskDocument& document) {
  document.animations.erase(std::remove_if(document.animations.begin(), document.animations.end(),
                                           [](const Animation& animation) { return animation.property.rfind("point_", 0) == 0; }),
                            document.animations.end());
}

void Clamp(MaskDocument& document) {
  document.width = std::max(0.0, document.width);
  document.height = std::max(0.0, document.height);
  document.feather = std::max(0.0, document.feather);
  document.opacity = std::clamp(document.opacity, 0.0, 1.0);
}

const Animation* FindAnimation(const MaskDocument& document, const std::string& property) {
  for (const auto& animation : document.animations) {
    if (animation.property == property) return &animation;
  }
  return nullptr;
}

Animation* FindAnimation(MaskDocument& document, const std::string& property) {
  for (auto& animation : document.animations) {
    if (animation.property == property) return &animation;
  }
  return nullptr;
}

void SetBase(MaskDocument& document, const std::string& property, double value) {
  if (property == "center_x") document.center_x = value;
  else if (property == "center_y") document.center_y = value;
  else if (property == "width") document.width = value;
  else if (property == "height") document.height = value;
  else if (property == "rotation") document.rotation = value;
  else if (property == "feather") document.feather = value;
  else if (property == "expansion") document.expansion = value;
  else if (property == "opacity") document.opacity = value;
  else {
    const auto separator = property.find('_', 6);
    const auto index = static_cast<std::size_t>(std::stoull(property.substr(6, separator - 6)));
    if (index >= document.points.size()) return;
    auto& point = document.points[index];
    const auto component = property.substr(separator + 1);
    if (component == "x") point.x = value;
    else if (component == "y") point.y = value;
    else if (component == "in_x") point.in_x = value;
    else if (component == "in_y") point.in_y = value;
    else if (component == "out_x") point.out_x = value;
    else if (component == "out_y") point.out_y = value;
  }
}

void SetKey(Animation& animation, double seconds, double value) {
  for (auto& key : animation.keys) {
    if (std::abs(key.time - seconds) < 1e-6) {
      key.value = value;
      return;
    }
  }
  animation.keys.push_back({seconds, value});
  std::sort(animation.keys.begin(), animation.keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
}

}  // namespace

// ----------------------------------------------------------------- the shape ----

std::vector<Pair> Outline(const MaskDocument& document, PictureSize picture, int steps_per_segment) {
  std::vector<Pair> outline;
  if (IsPath(document)) {
    if (document.points.size() < 2) return outline;
    for (std::size_t i = 0; i < document.points.size(); ++i) {
      auto piece = SegmentPoints(document, i, std::max(1, steps_per_segment));
      piece.pop_back();   // the next segment starts there
      outline.insert(outline.end(), piece.begin(), piece.end());
    }
    return outline;
  }
  const auto frame = FrameOf(document, picture);
  if (document.shape == Shape::Rectangle) {
    for (const auto& [lx, ly] : {Pair{-frame.hx, -frame.hy}, Pair{frame.hx, -frame.hy}, Pair{frame.hx, frame.hy}, Pair{-frame.hx, frame.hy}}) {
      outline.push_back(frame.ToPicture(lx, ly));
    }
  } else {
    const int steps = std::max(16, steps_per_segment * 4);
    for (int i = 0; i < steps; ++i) {
      const double angle = 2.0 * kPi * i / steps;
      outline.push_back(frame.ToPicture(frame.hx * std::cos(angle), frame.hy * std::sin(angle)));
    }
  }
  return outline;
}

std::vector<HandlePosition> Handles(const MaskDocument& document, PictureSize picture, int selected_point) {
  std::vector<HandlePosition> handles;
  using Kind = MaskHandle::Kind;
  if (IsPath(document)) {
    for (std::size_t i = 0; i < document.points.size(); ++i) handles.push_back({{Kind::Point, static_cast<int>(i)}, document.points[i].x, document.points[i].y});
    if (selected_point >= 0 && selected_point < static_cast<int>(document.points.size())) {
      const auto& p = document.points[static_cast<std::size_t>(selected_point)];
      if (p.in_x != p.x || p.in_y != p.y) handles.push_back({{Kind::TangentIn, selected_point}, p.in_x, p.in_y});
      if (p.out_x != p.x || p.out_y != p.y) handles.push_back({{Kind::TangentOut, selected_point}, p.out_x, p.out_y});
    }
    return handles;
  }
  const auto frame = FrameOf(document, picture);
  const auto add = [&](Kind kind, double lx, double ly) {
    const auto [x, y] = frame.ToPicture(lx, ly);
    handles.push_back({{kind, 0}, x, y});
  };
  add(Kind::EdgeLeft, -frame.hx, 0);
  add(Kind::EdgeRight, frame.hx, 0);
  add(Kind::EdgeTop, 0, -frame.hy);
  add(Kind::EdgeBottom, 0, frame.hy);
  add(Kind::Rotate, 0, -frame.hy - kRotateKnobPixels);
  return handles;
}

bool Inside(const MaskDocument& document, PictureSize picture, double x, double y) {
  if (IsPath(document)) {
    const auto outline = Outline(document, picture, 12);
    return outline.size() >= 3 && InsidePolygon(outline, x, y);
  }
  const auto frame = FrameOf(document, picture);
  const auto [lx, ly] = frame.ToLocal(x, y);
  if (document.shape == Shape::Rectangle) return std::abs(lx) <= frame.hx && std::abs(ly) <= frame.hy;
  if (frame.hx <= 0.0 || frame.hy <= 0.0) return false;
  return (lx / frame.hx) * (lx / frame.hx) + (ly / frame.hy) * (ly / frame.hy) <= 1.0;
}

MaskHandle HitTest(const MaskDocument& document, PictureSize picture, double x, double y, double tolerance_pixels, int selected_point) {
  using Kind = MaskHandle::Kind;
  double best = tolerance_pixels;
  MaskHandle found;
  // Tangent handles first: they can sit close to a vertex and must stay reachable.
  const auto handles = Handles(document, picture, selected_point);
  const auto consider = [&](bool tangents) {
    for (const auto& h : handles) {
      const bool is_tangent = h.handle.kind == Kind::TangentIn || h.handle.kind == Kind::TangentOut;
      if (is_tangent != tangents) continue;
      const double distance = std::hypot((h.x - x) * picture.width, (h.y - y) * picture.height);
      if (distance <= best) {
        best = distance;
        found = h.handle;
      }
    }
  };
  consider(true);
  if (found.kind != Kind::None) return found;
  consider(false);
  if (found.kind != Kind::None) return found;
  if (IsPath(document) && document.points.size() >= 2) {
    // On a side, between the vertices.
    double nearest = tolerance_pixels;
    for (std::size_t i = 0; i < document.points.size(); ++i) {
      const auto piece = SegmentPoints(document, i, 16);
      for (std::size_t k = 0; k + 1 < piece.size(); ++k) {
        const Pair a{piece[k].first * picture.width, piece[k].second * picture.height};
        const Pair b{piece[k + 1].first * picture.width, piece[k + 1].second * picture.height};
        const double distance = DistanceToSegment(x * picture.width, y * picture.height, a, b);
        if (distance <= nearest) {
          nearest = distance;
          found = {Kind::Segment, static_cast<int>(i)};
        }
      }
    }
    if (found.kind != Kind::None) return found;
  }
  if (Inside(document, picture, x, y)) return {Kind::Body, 0};
  return {};
}

// ---------------------------------------------------------------- dragging ----

MaskDocument Drag(const MaskDocument& before, const MaskHandle& handle, double press_x, double press_y, double x, double y, PictureSize picture, const DragOptions& options) {
  using Kind = MaskHandle::Kind;
  auto result = before;
  const double dx = x - press_x, dy = y - press_y;
  switch (handle.kind) {
    case Kind::None:
    case Kind::Segment:
      return result;
    case Kind::Body:
      if (IsPath(result)) {
        for (auto& p : result.points) {
          p.x += dx; p.y += dy; p.in_x += dx; p.in_y += dy; p.out_x += dx; p.out_y += dy;
        }
      } else {
        result.center_x += dx;
        result.center_y += dy;
      }
      return result;
    case Kind::Point: {
      if (handle.index < 0 || handle.index >= static_cast<int>(result.points.size())) return result;
      auto& p = result.points[static_cast<std::size_t>(handle.index)];
      double tx = x, ty = y;
      if (options.constrain) {
        // Along the nearer axis from where it started.
        const auto& start = before.points[static_cast<std::size_t>(handle.index)];
        if (std::abs((x - start.x) * picture.width) >= std::abs((y - start.y) * picture.height)) ty = start.y;
        else tx = start.x;
      }
      const double mx = tx - p.x, my = ty - p.y;
      p.x += mx; p.y += my; p.in_x += mx; p.in_y += my; p.out_x += mx; p.out_y += my;
      return result;
    }
    case Kind::TangentIn:
    case Kind::TangentOut: {
      if (handle.index < 0 || handle.index >= static_cast<int>(result.points.size())) return result;
      auto& p = result.points[static_cast<std::size_t>(handle.index)];
      const auto& start = before.points[static_cast<std::size_t>(handle.index)];
      const bool out = handle.kind == Kind::TangentOut;
      (out ? p.out_x : p.in_x) = x;
      (out ? p.out_y : p.in_y) = y;
      if (!options.break_tangent && IsSmooth(before, handle.index)) {
        // The partner keeps its length and points the opposite way.
        const double other_x = (out ? start.in_x : start.out_x) - start.x, other_y = (out ? start.in_y : start.out_y) - start.y;
        const double other_length = std::hypot(other_x * picture.width, other_y * picture.height);
        const double mine_x = (x - p.x) * picture.width, mine_y = (y - p.y) * picture.height;
        const double mine_length = std::hypot(mine_x, mine_y);
        if (mine_length > 1e-9) {
          const double scale = other_length / mine_length;
          (out ? p.in_x : p.out_x) = p.x - (x - p.x) * scale;
          (out ? p.in_y : p.out_y) = p.y - (y - p.y) * scale;
        }
      }
      return result;
    }
    case Kind::Rotate: {
      const auto frame = FrameOf(before, picture);
      const double px = (x - frame.cx) * picture.width, py = (y - frame.cy) * picture.height;
      double degrees = std::atan2(py, px) * 180.0 / kPi + 90.0;
      if (options.constrain) degrees = std::round(degrees / 15.0) * 15.0;
      while (degrees > 180.0) degrees -= 360.0;
      while (degrees <= -180.0) degrees += 360.0;
      result.rotation = degrees;
      return result;
    }
    case Kind::EdgeLeft:
    case Kind::EdgeRight:
    case Kind::EdgeTop:
    case Kind::EdgeBottom: {
      const auto frame = FrameOf(before, picture);
      const auto [lx, ly] = frame.ToLocal(x, y);
      const bool horizontal = handle.kind == Kind::EdgeLeft || handle.kind == Kind::EdgeRight;
      const double fixed = handle.kind == Kind::EdgeLeft ? frame.hx : handle.kind == Kind::EdgeRight ? -frame.hx : handle.kind == Kind::EdgeTop ? frame.hy : -frame.hy;
      double moving = horizontal ? lx : ly;
      // The opposite edge stays where it is; the dragged one cannot cross it.
      if (handle.kind == Kind::EdgeLeft || handle.kind == Kind::EdgeTop) moving = std::min(moving, fixed - kMinimumPixels);
      else moving = std::max(moving, fixed + kMinimumPixels);
      const double size = std::abs(fixed - moving);
      const double middle = (fixed + moving) * 0.5;
      const auto [nx, ny] = horizontal ? frame.ToPicture(middle, 0.0) : frame.ToPicture(0.0, middle);
      result.center_x = nx;
      result.center_y = ny;
      if (horizontal) {
        result.width = size / picture.width;
        if (options.constrain && frame.hx > 0.0) result.height = before.height * (size / (2.0 * frame.hx));
      } else {
        result.height = size / picture.height;
        if (options.constrain && frame.hy > 0.0) result.width = before.width * (size / (2.0 * frame.hy));
      }
      Clamp(result);
      return result;
    }
  }
  return result;
}

// ------------------------------------------------------------- beginning one ----

namespace {

MaskDocument Box(Shape shape, double x0, double y0, double x1, double y1, PictureSize picture) {
  MaskDocument document;
  document.shape = shape;
  document.center_x = (x0 + x1) * 0.5;
  document.center_y = (y0 + y1) * 0.5;
  document.width = std::max(kMinimumPixels / picture.width, std::abs(x1 - x0));
  document.height = std::max(kMinimumPixels / picture.height, std::abs(y1 - y0));
  return document;
}

}  // namespace

MaskDocument NewRectangle(double x0, double y0, double x1, double y1, PictureSize picture) { return Box(Shape::Rectangle, x0, y0, x1, y1, picture); }
MaskDocument NewEllipse(double x0, double y0, double x1, double y1, PictureSize picture) { return Box(Shape::Ellipse, x0, y0, x1, y1, picture); }

MaskDocument NewPath(const std::vector<Pair>& points) {
  MaskDocument document;
  document.shape = Shape::Bezier;
  for (const auto& [x, y] : points) {
    Point p;
    p.x = p.in_x = p.out_x = x;
    p.y = p.in_y = p.out_y = y;
    document.points.push_back(p);
  }
  return document;
}

MaskDocument Smooth(const MaskDocument& document, int index) {
  auto result = document;
  const auto count = static_cast<int>(result.points.size());
  if (!IsPath(result) || index < 0 || index >= count || count < 3) return result;
  auto& p = result.points[static_cast<std::size_t>(index)];
  const auto& previous = document.points[static_cast<std::size_t>((index + count - 1) % count)];
  const auto& next = document.points[static_cast<std::size_t>((index + 1) % count)];
  const double tx = (next.x - previous.x) / 6.0, ty = (next.y - previous.y) / 6.0;
  p.in_x = p.x - tx; p.in_y = p.y - ty;
  p.out_x = p.x + tx; p.out_y = p.y + ty;
  return result;
}

MaskDocument Corner(const MaskDocument& document, int index) {
  auto result = document;
  if (!IsPath(result) || index < 0 || index >= static_cast<int>(result.points.size())) return result;
  auto& p = result.points[static_cast<std::size_t>(index)];
  p.in_x = p.out_x = p.x;
  p.in_y = p.out_y = p.y;
  return result;
}

bool IsSmooth(const MaskDocument& document, int index) {
  if (!IsPath(document) || index < 0 || index >= static_cast<int>(document.points.size())) return false;
  const auto& p = document.points[static_cast<std::size_t>(index)];
  const double ix = p.in_x - p.x, iy = p.in_y - p.y, ox = p.out_x - p.x, oy = p.out_y - p.y;
  const double li = std::hypot(ix, iy), lo = std::hypot(ox, oy);
  if (li < 1e-9 || lo < 1e-9) return false;
  // Opposite directions: the cross product is nothing and the dot product is negative.
  return std::abs(ix * oy - iy * ox) / (li * lo) < 1e-3 && (ix * ox + iy * oy) < 0.0;
}

MaskDocument InsertPoint(const MaskDocument& document, PictureSize picture, double x, double y, double tolerance_pixels) {
  if (!IsPath(document) || document.points.size() < 2 || document.points.size() >= 512) return document;
  constexpr int kSteps = 24;
  double best = tolerance_pixels;
  int segment = -1;
  double parameter = 0.0;
  for (std::size_t i = 0; i < document.points.size(); ++i) {
    const auto piece = SegmentPoints(document, i, kSteps);
    for (int k = 0; k < kSteps; ++k) {
      const Pair a{piece[static_cast<std::size_t>(k)].first * picture.width, piece[static_cast<std::size_t>(k)].second * picture.height};
      const Pair b{piece[static_cast<std::size_t>(k) + 1].first * picture.width, piece[static_cast<std::size_t>(k) + 1].second * picture.height};
      double along = 0.0;
      const double distance = DistanceToSegment(x * picture.width, y * picture.height, a, b, &along);
      if (distance <= best) {
        best = distance;
        segment = static_cast<int>(i);
        parameter = (static_cast<double>(k) + along) / kSteps;
      }
    }
  }
  if (segment < 0) return document;
  auto result = document;
  const auto count = result.points.size();
  auto& a = result.points[static_cast<std::size_t>(segment)];
  auto& b = result.points[(static_cast<std::size_t>(segment) + 1) % count];
  const Pair p0{a.x, a.y}, p1{a.out_x, a.out_y}, p2{b.in_x, b.in_y}, p3{b.x, b.y};
  const auto ab = Lerp(p0, p1, parameter), bc = Lerp(p1, p2, parameter), cd = Lerp(p2, p3, parameter);
  const auto abc = Lerp(ab, bc, parameter), bcd = Lerp(bc, cd, parameter);
  const auto m = Lerp(abc, bcd, parameter);
  Point added;
  added.x = m.first; added.y = m.second;
  added.in_x = abc.first; added.in_y = abc.second;
  added.out_x = bcd.first; added.out_y = bcd.second;
  a.out_x = ab.first; a.out_y = ab.second;
  b.in_x = cd.first; b.in_y = cd.second;
  result.points.insert(result.points.begin() + segment + 1, added);
  DropPointAnimation(result);
  return result;
}

MaskDocument RemovePoint(const MaskDocument& document, int index) {
  if (!IsPath(document) || document.points.size() <= 3 || index < 0 || index >= static_cast<int>(document.points.size())) return document;
  auto result = document;
  result.points.erase(result.points.begin() + index);
  DropPointAnimation(result);
  return result;
}

// ---------------------------------------------------------- values over time ----

std::vector<std::string> PropertyNames(const MaskDocument& document) {
  std::vector<std::string> names{"center_x", "center_y", "width", "height", "rotation", "feather", "expansion", "opacity"};
  for (std::size_t i = 0; i < document.points.size(); ++i) {
    for (const char* component : {"x", "y", "in_x", "in_y", "out_x", "out_y"}) names.push_back(PointProperty(i, component));
  }
  return names;
}

bool IsAnimated(const MaskDocument& document, const std::string& property) { return FindAnimation(document, property) != nullptr; }

double ValueOf(const MaskDocument& document, const std::string& property) {
  auto copy = document;
  copy.animations.clear();
  copy.points = document.points;
  // Read through the setter's twin: set a sentinel and compare would be silly; read directly.
  if (property == "center_x") return document.center_x;
  if (property == "center_y") return document.center_y;
  if (property == "width") return document.width;
  if (property == "height") return document.height;
  if (property == "rotation") return document.rotation;
  if (property == "feather") return document.feather;
  if (property == "expansion") return document.expansion;
  if (property == "opacity") return document.opacity;
  if (property.rfind("point_", 0) == 0) {
    const auto separator = property.find('_', 6);
    if (separator == std::string::npos) return 0.0;
    const auto index = static_cast<std::size_t>(std::stoull(property.substr(6, separator - 6)));
    if (index >= document.points.size()) return 0.0;
    const auto& p = document.points[index];
    const auto component = property.substr(separator + 1);
    if (component == "x") return p.x;
    if (component == "y") return p.y;
    if (component == "in_x") return p.in_x;
    if (component == "in_y") return p.in_y;
    if (component == "out_x") return p.out_x;
    if (component == "out_y") return p.out_y;
  }
  return 0.0;
}

MaskDocument ApplyEdit(const MaskDocument& stored, const MaskDocument& evaluated, const MaskDocument& edited, double seconds) {
  auto result = stored;
  // What is not animated and not a number is simply what was edited.
  result.shape = edited.shape;
  result.combine = edited.combine;
  result.inverted = edited.inverted;
  const auto names = PropertyNames(edited);
  for (const auto& property : names) {
    const double was = ValueOf(evaluated, property), now = ValueOf(edited, property);
    if (std::abs(was - now) < 1e-12) continue;
    if (auto* animation = FindAnimation(result, property)) {
      SetKey(*animation, std::max(0.0, seconds), now);
    } else {
      SetBase(result, property, now);
    }
  }
  // A path whose vertex count changed has no vertex animation left to speak of.
  if (result.points.size() != stored.points.size()) DropPointAnimation(result);
  Clamp(result);
  return result;
}

MaskDocument StartAnimating(const MaskDocument& document, const std::string& property, double seconds) {
  if (FindAnimation(document, property) != nullptr) return document;
  auto result = document;
  Animation animation;
  animation.property = property;
  animation.keys.push_back({std::max(0.0, seconds), ValueOf(document, property)});
  result.animations.push_back(std::move(animation));
  return result;
}

MaskDocument StopAnimating(const MaskDocument& document, const std::string& property, double seconds) {
  const auto* animation = FindAnimation(document, property);
  if (animation == nullptr) return document;
  const auto evaluated = effects::mask::Evaluate(document, seconds);
  auto result = document;
  SetBase(result, property, ValueOf(evaluated, property));
  result.animations.erase(std::remove_if(result.animations.begin(), result.animations.end(), [&](const Animation& a) { return a.property == property; }), result.animations.end());
  return result;
}

MaskDocument RemoveKey(const MaskDocument& document, const std::string& property, double seconds) {
  auto result = document;
  auto* animation = FindAnimation(result, property);
  if (animation == nullptr) return document;
  animation->keys.erase(std::remove_if(animation->keys.begin(), animation->keys.end(), [&](const Key& key) { return std::abs(key.time - seconds) < 1e-6; }), animation->keys.end());
  if (animation->keys.empty()) {
    const auto name = property;
    result.animations.erase(std::remove_if(result.animations.begin(), result.animations.end(), [&](const Animation& a) { return a.property == name; }), result.animations.end());
  }
  return result;
}

// ------------------------------------------------------------------- tracking ----

MaskDocument FollowTrack(const MaskDocument& document, const render::tracking::TrackResult& track, double reference_seconds, PictureSize picture) {
  std::vector<const render::tracking::PointSample*> usable;
  for (const auto& sample : track.samples) {
    if (sample.valid) usable.push_back(&sample);
  }
  if (usable.size() < 2) return document;
  const auto seconds_of = [](const render::tracking::PointSample& s) { return static_cast<double>(s.time.numerator()) / static_cast<double>(s.time.denominator()); };
  // Where the track was when the mask was drawn: the usable sample nearest the reference time.
  const render::tracking::PointSample* origin = usable.front();
  for (const auto* sample : usable) {
    if (std::abs(seconds_of(*sample) - reference_seconds) < std::abs(seconds_of(*origin) - reference_seconds)) origin = sample;
  }
  auto result = document;
  result.animations.clear();
  const auto base = document;
  const auto add = [&](const std::string& property, double base_value, bool horizontal) {
    Animation animation;
    animation.property = property;
    for (const auto* sample : usable) {
      const double moved = horizontal ? (sample->x - origin->x) / picture.width : (sample->y - origin->y) / picture.height;
      const double time = std::max(0.0, seconds_of(*sample));
      if (!animation.keys.empty() && time <= animation.keys.back().time) continue;
      animation.keys.push_back({time, base_value + moved});
    }
    if (!animation.keys.empty()) result.animations.push_back(std::move(animation));
  };
  if (IsPath(base)) {
    for (std::size_t i = 0; i < base.points.size(); ++i) {
      const auto& p = base.points[i];
      add(PointProperty(i, "x"), p.x, true);
      add(PointProperty(i, "y"), p.y, false);
      add(PointProperty(i, "in_x"), p.in_x, true);
      add(PointProperty(i, "in_y"), p.in_y, false);
      add(PointProperty(i, "out_x"), p.out_x, true);
      add(PointProperty(i, "out_y"), p.out_y, false);
    }
  } else {
    add("center_x", base.center_x, true);
    add("center_y", base.center_y, false);
  }
  return result;
}

// ---------------------------------------------------------------------- plans ----

namespace {

const timeline::Clip* FindClip(const EditContext& ctx, const std::string& clip_id, const timeline::Track** track) {
  for (const auto& t : ctx.sequence->tracks) {
    for (const auto& clip : t.clips) {
      if (clip.id == clip_id) {
        *track = &t;
        return &clip;
      }
    }
  }
  return nullptr;
}

EditPlan OnePlan(std::string label, commands::CommandType type, commands::CommandPayload payload) {
  EditPlan plan;
  plan.ok = true;
  plan.label = std::move(label);
  plan.commands.push_back({type, std::move(payload)});
  return plan;
}

}  // namespace

EditPlan PlanAddMask(const EditContext& ctx, const std::string& clip_id, const std::string& effect_id, const MaskDocument& document) {
  const timeline::Track* track = nullptr;
  const auto* clip = ctx.sequence == nullptr ? nullptr : FindClip(ctx, clip_id, &track);
  if (clip == nullptr) return EditPlan::Refuse("The clip is no longer in the sequence");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  const timeline::Effect* owner = nullptr;
  for (const auto& effect : clip->effects) {
    if (effect.id == effect_id) owner = &effect;
  }
  if (owner == nullptr) return EditPlan::Refuse("The effect is no longer on the clip");
  try {
    effects::mask::Validate(document);
  } catch (const std::exception& error) {
    return EditPlan::Refuse(error.what());
  }
  commands::AddMaskPayload add;
  add.id = ctx.new_id ? ctx.new_id("mask") : "mask-new";
  add.effect_id = effect_id;
  std::int64_t order = 0;
  for (const auto& mask : owner->masks) order = std::max(order, mask.order + 1);
  add.order = order;
  add.document_json = effects::mask::ToJson(document);
  return OnePlan("Add Mask", commands::CommandType::AddMask, add);
}

EditPlan PlanUpdateMask(const EditContext& ctx, const std::string& clip_id, const std::string& mask_id, const MaskDocument& document, const std::string& label) {
  const timeline::Track* track = nullptr;
  const auto* clip = ctx.sequence == nullptr ? nullptr : FindClip(ctx, clip_id, &track);
  if (clip == nullptr) return EditPlan::Refuse("The clip is no longer in the sequence");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  bool found = false;
  for (const auto& effect : clip->effects) {
    for (const auto& mask : effect.masks) found = found || mask.id == mask_id;
  }
  if (!found) return EditPlan::Refuse("The mask is no longer on the clip");
  try {
    effects::mask::Validate(document);
  } catch (const std::exception& error) {
    return EditPlan::Refuse(error.what());
  }
  commands::UpdateMaskPayload update;
  update.id = mask_id;
  update.document_json = effects::mask::ToJson(document);
  return OnePlan(label, commands::CommandType::UpdateMask, update);
}

EditPlan PlanRemoveMask(const EditContext& ctx, const std::string& clip_id, const std::string& mask_id) {
  const timeline::Track* track = nullptr;
  const auto* clip = ctx.sequence == nullptr ? nullptr : FindClip(ctx, clip_id, &track);
  if (clip == nullptr) return EditPlan::Refuse("The clip is no longer in the sequence");
  if (track->locked) return EditPlan::Refuse("The track " + track->id + " is locked");
  return OnePlan("Remove Mask", commands::CommandType::RemoveMask, commands::RemoveMaskPayload{mask_id});
}

}  // namespace cutline::ui

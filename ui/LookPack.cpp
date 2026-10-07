#include "ui/LookPack.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <system_error>

namespace cutline::ui {
namespace {

// The first line of every file the pack writes: how a refresh knows which files are its own and current.
constexpr const char* kStamp = "# Cutline look pack 1";

using Rgb = std::array<float, 3>;

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float Smooth(float edge0, float edge1, float v) {
  const float t = Clamp01((v - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}
float Luma(const Rgb& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
// An S-shaped curve through the end points: amount 0 leaves the value, 1 is the full smooth step.
float Curve(float x, float amount) {
  x = Clamp01(x);
  return x + (x * x * (3.0f - 2.0f * x) - x) * amount;
}
Rgb Curve(const Rgb& c, float amount) { return {Curve(c[0], amount), Curve(c[1], amount), Curve(c[2], amount)}; }
Rgb Saturate(const Rgb& c, float amount) {
  const float l = Luma(c);
  return {l + (c[0] - l) * amount, l + (c[1] - l) * amount, l + (c[2] - l) * amount};
}
Rgb Mix(const Rgb& a, const Rgb& b, float t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }
Rgb Clamped(const Rgb& c) { return {Clamp01(c[0]), Clamp01(c[1]), Clamp01(c[2])}; }

Rgb TealAndOrange(float r, float g, float b) {
  Rgb c{r, g, b};
  const float l = Luma(c);
  const float shadows = 1.0f - Smooth(0.0f, 0.6f, l), highlights = Smooth(0.4f, 1.0f, l);
  c = {c[0] + shadows * -0.06f + highlights * 0.08f, c[1] + shadows * 0.02f + highlights * 0.02f, c[2] + shadows * 0.07f + highlights * -0.07f};
  return Clamped(Curve(Saturate(c, 1.1f), 0.25f));
}

Rgb WarmFilm(float r, float g, float b) {
  Rgb c{0.03f + r * 0.97f, 0.03f + g * 0.97f, 0.03f + b * 0.97f};
  c = Curve(c, 0.2f);
  const float warm = Smooth(0.3f, 1.0f, Luma(c));
  c = {c[0] * (1.0f + 0.05f * warm), c[1] * (1.0f + 0.01f * warm), c[2] * (1.0f - 0.07f * warm)};
  return Clamped(Saturate(c, 0.92f));
}

Rgb CoolNight(float r, float g, float b) {
  Rgb c{r * 0.85f * 0.92f, g * 0.85f * 0.98f, b * 0.85f * 1.08f};
  return Clamped(Curve(Saturate(c, 0.85f), 0.15f));
}

Rgb BleachBypass(float r, float g, float b) {
  Rgb c{r, g, b};
  const float l = Luma(c);
  return Clamped(Curve(Mix(c, {l, l, l}, 0.5f), 0.45f));
}

Rgb FadedMatte(float r, float g, float b) {
  Rgb c{0.06f + r * 0.88f, 0.06f + g * 0.88f, 0.07f + b * 0.86f};
  return Clamped(Saturate(c, 0.85f));
}

Rgb VividPop(float r, float g, float b) { return Clamped(Curve(Saturate({r, g, b}, 1.35f), 0.2f)); }

Rgb BlackAndWhite(float r, float g, float b) {
  const float l = Curve(Clamp01(Luma({r, g, b})), 0.3f);
  return {l, l, l};
}

Rgb Sepia(float r, float g, float b) {
  const float l = Curve(Clamp01(Luma({r, g, b})), 0.12f);
  const Rgb black{0.08f, 0.05f, 0.03f}, white{1.0f, 0.92f, 0.78f};
  return Clamped(Mix(black, white, l));
}

Rgb GoldenHour(float r, float g, float b) {
  Rgb c{(0.02f + r) * 1.08f, (0.02f + g), (0.02f + b) * 0.88f};
  const float shadows = 1.0f - Smooth(0.0f, 0.5f, Luma(c));
  c = {c[0] + 0.02f * shadows, c[1], c[2] + 0.03f * shadows};
  return Clamped(Curve(Saturate(c, 1.1f), 0.12f));
}

}  // namespace

const std::vector<Look>& BuiltInLooks() {
  static const std::vector<Look> looks = {
      {"Teal and Orange", "Cool shadows, warm highlights, a little more colour and contrast", TealAndOrange},
      {"Warm Film", "Raised blacks, a soft S-curve, warm highlights, slightly less colour", WarmFilm},
      {"Cool Night", "Darker, blue, quieter", CoolNight},
      {"Bleach Bypass", "Half the colour and a hard curve", BleachBypass},
      {"Faded Matte", "Milky blacks, soft whites, muted colour", FadedMatte},
      {"Vivid Pop", "More colour, a touch more contrast", VividPop},
      {"Black and White", "Rec.709 luma with a gentle curve", BlackAndWhite},
      {"Sepia", "Luma mapped from brown-black to cream", Sepia},
      {"Golden Hour", "Warm gain, a little magenta in the shadows", GoldenHour},
  };
  return looks;
}

int WriteBuiltInLooks(const std::filesystem::path& folder, int size) {
  size = std::clamp(size, 2, 65);
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  int written = 0;
  for (const auto& look : BuiltInLooks()) {
    const auto path = folder / (look.name + ".cube");
    if (std::filesystem::exists(path, error)) {
      std::ifstream existing(path);
      std::string first;
      std::getline(existing, first);
      while (!first.empty() && (first.back() == '\r' || first.back() == ' ')) first.pop_back();
      if (first != kStamp) continue;   // the person's own file under the same name
      // Ours: current, unless this build writes a different size.
      std::string second, third;
      std::getline(existing, second);
      std::getline(existing, third);
      if (third == "LUT_3D_SIZE " + std::to_string(size)) continue;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) continue;
    out << kStamp << "\n# " << look.description << "\nLUT_3D_SIZE " << size << "\nTITLE \"" << look.name << "\"\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    for (int b = 0; b < size; ++b) {
      for (int g = 0; g < size; ++g) {
        for (int r = 0; r < size; ++r) {
          const auto mapped = look.map(static_cast<float>(r) / static_cast<float>(size - 1), static_cast<float>(g) / static_cast<float>(size - 1), static_cast<float>(b) / static_cast<float>(size - 1));
          char line[80];
          std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\n", mapped[0], mapped[1], mapped[2]);
          out << line;
        }
      }
    }
    ++written;
  }
  return written;
}

}  // namespace cutline::ui

#pragma once

// Colour management: what a pixel's numbers mean, and how to move them between meanings.
//
// The model. Every picture is described by two things: its primaries (which colours its
// red, green and blue are) and its transfer function (how stored numbers relate to light).
// To convert, a pixel is decoded to linear light, rotated from one set of primaries to the
// other by a 3x3 matrix, and encoded again. Between decoding and encoding the unit is
// *relative to diffuse white*: 1.0 is the brightness of SDR reference white (203 nits, per
// ITU-R BT.2408), so SDR content lives in 0..1, and HDR content has headroom above 1 that
// an SDR display cannot show and a tone mapper must fit into it.
//
//   * PQ (SMPTE ST 2084) is absolute: a code value is a number of nits (up to 10,000).
//   * HLG (ARIB STD-B67 / BT.2100) is scene-referred; it is turned into display light with
//     the BT.2100 system gamma of 1.2 at a 1,000-nit display.
//   * The SDR transfers (BT.709, sRGB, a pure 2.4 gamma, linear) map 0..1 to 0..1 of
//     reference white.
//
// Scene-referred spaces. The ACES working spaces (AP0 and AP1 primaries, linear or as ACEScc and
// ACEScct) and the log encodings of five cameras (ARRI LogC3 with ARRI Wide Gamut 3, Sony S-Log3
// with S-Gamut3.Cine, Panasonic V-Log with V-Gamut, RED Log3G10 with REDWideGamutRGB) are defined
// by their published formulas and chromaticities. They hold light as it was in the scene, with
// 0.18 for a mid grey and far more than 1.0 for a highlight; sending them to an SDR display fits
// the light above 0.75 under 1.0 with the same soft knee HDR uses, to a peak of sixteen times
// reference white. That is a plain view transform, NOT the ACES Reference Rendering Transform and
// Output Device Transform, which this build does not contain, so a picture shown through it does
// not match one shown through an ACES output transform.
//
// What is deliberately not here: OpenColorIO. Its transforms are configuration files this
// build does not ship; the formulas above are the published ones for these spaces and are
// tested against their published reference values. A space this file does not define cannot
// be named; it is reported and treated as Rec.709, which is what every project made before
// colour management behaved as.
//
// The compositor uses this under render version 3 and later (core/model/RenderVersion.h);
// earlier sequences ignore their colour space settings exactly as they always did.

#include <array>
#include <optional>
#include <string>

namespace cutline::render::color {

enum class Primaries { Bt709, DisplayP3, Bt2020, AcesAp0, AcesAp1, ArriWideGamut3, SGamut3Cine, VGamut, RedWideGamut };
enum class Transfer { Bt709, Srgb, Gamma24, Linear, Pq, Hlg, AcesCc, AcesCct, LogC3, SLog3, VLog, Log3G10 };

struct Space final {
  Primaries primaries{Primaries::Bt709};
  Transfer transfer{Transfer::Bt709};

  [[nodiscard]] bool operator==(const Space& other) const { return primaries == other.primaries && transfer == other.transfer; }
  [[nodiscard]] bool hdr() const { return transfer == Transfer::Pq || transfer == Transfer::Hlg; }
  [[nodiscard]] bool linear() const { return transfer == Transfer::Linear; }
  // Light as it was in the scene: a camera log encoding, ACEScc/ACEScct, or a linear ACES space.
  [[nodiscard]] bool scene_referred() const {
    switch (transfer) {
      case Transfer::AcesCc: case Transfer::AcesCct: case Transfer::LogC3: case Transfer::SLog3: case Transfer::VLog: case Transfer::Log3G10: return true;
      default: return transfer == Transfer::Linear && (primaries == Primaries::AcesAp0 || primaries == Primaries::AcesAp1);
    }
  }
};

// Sequence colour space names: rec709, srgb, p3-d65, rec2020, rec2020-pq, rec2020-hlg, and the
// linear working spaces linear-rec709, linear-srgb, linear-p3, linear-rec2020; the scene-referred
// acescg (AP1, linear), aces2065-1 (AP0, linear), acescc, acescct, arri-logc3, sony-slog3,
// panasonic-vlog, red-log3g10 and the linear spaces of those gamuts (linear-arri-wide-gamut3,
// linear-sgamut3cine, linear-vgamut, linear-rwg). Case and the bt709 / display-p3 / bt2020 spellings
// are accepted. nullopt for anything else.
[[nodiscard]] std::optional<Space> ParseSpace(const std::string& name);
[[nodiscard]] std::string Name(const Space& space);

// A decoded frame's tags (the names FFmpeg reports: bt709, bt2020, smpte432, smpte2084,
// arib-std-b67, iec61966-2-1, ...) as a space. nullopt when a tag is not one defined here.
[[nodiscard]] std::optional<Space> SpaceFromTags(const std::string& primaries, const std::string& transfer);
// The tags to put on a frame in this space, the other way.
struct Tags final {
  std::string primaries;
  std::string transfer;
};
[[nodiscard]] Tags TagsOf(const Space& space);

// ---------------------------------------------------------------- transfers ----

// Encoded 0..1 to linear light relative to reference white (PQ and HLG may exceed 1).
[[nodiscard]] double ToLinear(Transfer transfer, double encoded);
// Linear light relative to reference white to encoded 0..1.
[[nodiscard]] double FromLinear(Transfer transfer, double linear);
// The HLG curve itself (BT.2100): scene light 0..1 to signal 0..1, and back.
[[nodiscard]] double HlgOetf(double scene_linear);
[[nodiscard]] double HlgInverseOetf(double signal);
// The PQ curve itself: nits to signal 0..1, and back.
[[nodiscard]] double NitsToPq(double nits);
[[nodiscard]] double PqToNits(double signal);

inline constexpr double kReferenceWhiteNits = 203.0;
inline constexpr double kPqPeakNits = 10000.0;
inline constexpr double kHlgDisplayNits = 1000.0;
inline constexpr double kHlgSystemGamma = 1.2;

// ----------------------------------------------------------------- primaries ----

using Matrix = std::array<std::array<double, 3>, 3>;

// Linear RGB of one set of primaries to CIE XYZ (D65, the white every space here is shown against: a set whose
// own white is another, as ACES's is, is adapted to D65 with the Bradford transform), and the rotation between two sets.
[[nodiscard]] Matrix RgbToXyz(Primaries primaries);
[[nodiscard]] Matrix Rotation(Primaries from, Primaries to);
[[nodiscard]] std::array<double, 3> LumaCoefficients(Primaries primaries);

// ---------------------------------------------------------------- the transform ----

enum class GamutMapping {
  Clip,        // clamp each channel into range
  Desaturate,  // move toward the pixel's own luminance just far enough to be in range
};

struct Options final {
  // What an SDR output does with HDR light above reference white. The peak is the brightest
  // value the source can carry (nits): a soft knee brings it to 1.0 and leaves what is below
  // three quarters of white alone.
  bool tone_map{true};
  double source_peak_nits{1000.0};
  GamutMapping gamut{GamutMapping::Desaturate};
};

// Converts one pixel's encoded RGB (straight, not premultiplied) between spaces.
class Transform final {
 public:
  Transform(const Space& from, const Space& to, const Options& options = {});

  [[nodiscard]] bool identity() const { return identity_; }
  void Apply(float* rgb) const;
  [[nodiscard]] std::array<double, 3> Apply(double r, double g, double b) const;

  // The soft-knee tone mapping curve used for HDR to SDR (exposed for the tests).
  [[nodiscard]] static double SoftKnee(double linear, double peak_relative);

 private:
  Space from_, to_;
  Options options_;
  Matrix matrix_;
  bool identity_{false};
  bool same_gamut_{false};
  std::array<double, 3> target_luma_{};
};

}  // namespace cutline::render::color

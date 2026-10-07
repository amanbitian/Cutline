#include "render/ColorManagement.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cutline::render::color {
namespace {

std::string Lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// ------------------------------------------------------------ transfer curves ----

// BT.709 / BT.2020 SDR: the camera OETF, with its published constants.
constexpr double kBtAlpha = 1.09929682680944;
constexpr double kBtBeta = 0.018053968510807;

double Bt709Decode(double v) {
  if (v < 4.5 * kBtBeta) return v / 4.5;
  return std::pow((v + kBtAlpha - 1.0) / kBtAlpha, 1.0 / 0.45);
}
double Bt709Encode(double l) {
  if (l < kBtBeta) return 4.5 * l;
  return kBtAlpha * std::pow(l, 0.45) - (kBtAlpha - 1.0);
}

double SrgbDecode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double SrgbEncode(double l) { return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055; }

// SMPTE ST 2084.
constexpr double kM1 = 2610.0 / 16384.0;
constexpr double kM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kC1 = 3424.0 / 4096.0;
constexpr double kC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kC3 = 2392.0 / 4096.0 * 32.0;

double PqToNitsImpl(double v) {
  v = std::clamp(v, 0.0, 1.0);
  const auto p = std::pow(v, 1.0 / kM2);
  const auto numerator = std::max(p - kC1, 0.0);
  const auto denominator = kC2 - kC3 * p;
  return denominator <= 0.0 ? kPqPeakNits : kPqPeakNits * std::pow(numerator / denominator, 1.0 / kM1);
}
double NitsToPqImpl(double nits) {
  const auto y = std::clamp(nits / kPqPeakNits, 0.0, 1.0);
  const auto p = std::pow(y, kM1);
  return std::pow((kC1 + kC2 * p) / (1.0 + kC3 * p), kM2);
}

// BT.2100 HLG.
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892;
constexpr double kHlgC = 0.55991073;

double HlgInverseOetfImpl(double v) {
  v = std::clamp(v, 0.0, 1.0);
  if (v <= 0.5) return v * v / 3.0;
  return (std::exp((v - kHlgC) / kHlgA) + kHlgB) / 12.0;
}
double HlgOetfImpl(double e) {
  e = std::clamp(e, 0.0, 1.0);
  if (e <= 1.0 / 12.0) return std::sqrt(3.0 * e);
  return kHlgA * std::log(12.0 * e - kHlgB) + kHlgC;
}

// ACES and camera log encodings. Each pair is the published decode (to scene linear, 0.18 for a mid grey) and encode.

// ACEScc and ACEScct (S-2014-003, S-2016-001).
double AccsDecode(double v) {
  if (v < (9.72 - 15.0) / 17.52) return (std::pow(2.0, v * 17.52 - 9.72) - std::pow(2.0, -16.0)) * 2.0;
  if (v < (std::log2(65504.0) + 9.72) / 17.52) return std::pow(2.0, v * 17.52 - 9.72);
  return 65504.0;
}
double AccsEncode(double l) {
  if (l <= 0.0) return (std::log2(std::pow(2.0, -16.0)) + 9.72) / 17.52;
  if (l < std::pow(2.0, -15.0)) return (std::log2(std::pow(2.0, -16.0) + l * 0.5) + 9.72) / 17.52;
  return (std::log2(l) + 9.72) / 17.52;
}
constexpr double kCctCut = 0.0078125, kCctA = 10.5402377416545, kCctB = 0.0729055341958355, kCctCodeCut = 0.155251141552511;
double AcesCctDecode(double v) { return v <= kCctCodeCut ? (v - kCctB) / kCctA : std::pow(2.0, v * 17.52 - 9.72); }
double AcesCctEncode(double l) { return l <= kCctCut ? kCctA * l + kCctB : (std::log2(l) + 9.72) / 17.52; }

// ARRI LogC (version 3, exposure index 800).
constexpr double kLcCut = 0.010591, kLcA = 5.555556, kLcB = 0.052272, kLcC = 0.247190, kLcD = 0.385537, kLcE = 5.367655, kLcF = 0.092809;
double LogC3Decode(double t) { return t > kLcE * kLcCut + kLcF ? (std::pow(10.0, (t - kLcD) / kLcC) - kLcB) / kLcA : (t - kLcF) / kLcE; }
double LogC3Encode(double l) { return l > kLcCut ? kLcC * std::log10(kLcA * l + kLcB) + kLcD : kLcE * l + kLcF; }

// Sony S-Log3.
constexpr double kSlKnee = 171.2102946929 / 1023.0;
double SLog3Decode(double x) {
  if (x >= kSlKnee) return std::pow(10.0, (x * 1023.0 - 420.0) / 261.5) * (0.18 + 0.01) - 0.01;
  return (x * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
}
double SLog3Encode(double l) {
  if (l >= 0.01125) return (420.0 + std::log10((l + 0.01) / (0.18 + 0.01)) * 261.5) / 1023.0;
  return (l * (171.2102946929 - 95.0) / 0.01125 + 95.0) / 1023.0;
}

// Panasonic V-Log.
constexpr double kVlCut1 = 0.01, kVlCut2 = 0.181, kVlB = 0.00873, kVlC = 0.241514, kVlD = 0.598206;
double VLogDecode(double x) { return x < kVlCut2 ? (x - 0.125) / 5.6 : std::pow(10.0, (x - kVlD) / kVlC) - kVlB; }
double VLogEncode(double l) { return l < kVlCut1 ? 5.6 * l + 0.125 : kVlC * std::log10(l + kVlB) + kVlD; }

// RED Log3G10 (the 2017 version).
constexpr double kRlA = 0.224282, kRlB = 155.975327, kRlC = 0.01, kRlG = 15.1927;
double Log3G10Decode(double x) { return x < 0.0 ? x / kRlG - kRlC : (std::pow(10.0, x / kRlA) - 1.0) / kRlB - kRlC; }
double Log3G10Encode(double l) {
  const double shifted = l + kRlC;
  return shifted < 0.0 ? shifted * kRlG : kRlA * std::log10(shifted * kRlB + 1.0);
}

// ---------------------------------------------------------------- matrices ----

struct Chromaticity {
  double x, y;
};
struct Gamut {
  Chromaticity r, g, b;
};
constexpr Chromaticity kD65{0.3127, 0.3290};
constexpr Chromaticity kAcesWhite{0.32168, 0.33767};   // what ACES calls D60

Gamut GamutOf(Primaries primaries) {
  switch (primaries) {
    case Primaries::DisplayP3: return {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};
    case Primaries::Bt2020: return {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
    case Primaries::AcesAp0: return {{0.7347, 0.2653}, {0.0, 1.0}, {0.0001, -0.0770}};
    case Primaries::AcesAp1: return {{0.713, 0.293}, {0.165, 0.830}, {0.128, 0.044}};
    case Primaries::ArriWideGamut3: return {{0.6840, 0.3130}, {0.2210, 0.8480}, {0.0861, -0.1020}};
    case Primaries::SGamut3Cine: return {{0.766, 0.275}, {0.225, 0.800}, {0.089, -0.087}};
    case Primaries::VGamut: return {{0.730, 0.280}, {0.165, 0.840}, {0.100, -0.030}};
    case Primaries::RedWideGamut: return {{0.780308, 0.304253}, {0.121595, 1.493994}, {0.095612, -0.084589}};
    case Primaries::Bt709: break;
  }
  return {{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}};
}

Chromaticity WhiteOf(Primaries primaries) { return primaries == Primaries::AcesAp0 || primaries == Primaries::AcesAp1 ? kAcesWhite : kD65; }

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      for (int k = 0; k < 3; ++k) out[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] += a[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] * b[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)];
    }
  }
  return out;
}

Matrix Inverse(const Matrix& m) {
  const auto a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
  const auto determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  const auto inverse = 1.0 / determinant;
  return {{{(e * i - f * h) * inverse, (c * h - b * i) * inverse, (b * f - c * e) * inverse},
           {(f * g - d * i) * inverse, (a * i - c * g) * inverse, (c * d - a * f) * inverse},
           {(d * h - e * g) * inverse, (b * g - a * h) * inverse, (a * e - b * d) * inverse}}};
}

// The Bradford chromatic adaptation from one white to another, as a matrix on XYZ.
Matrix Bradford(const Chromaticity& from, const Chromaticity& to) {
  const Matrix cone{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
  const auto xyz = [](const Chromaticity& c) { return std::array<double, 3>{c.x / c.y, 1.0, (1.0 - c.x - c.y) / c.y}; };
  const auto s = xyz(from), d = xyz(to);
  std::array<double, 3> rs{}, rd{};
  for (std::size_t i = 0; i < 3; ++i) {
    rs[i] = cone[i][0] * s[0] + cone[i][1] * s[1] + cone[i][2] * s[2];
    rd[i] = cone[i][0] * d[0] + cone[i][1] * d[1] + cone[i][2] * d[2];
  }
  const Matrix scale{{{rd[0] / rs[0], 0, 0}, {0, rd[1] / rs[1], 0}, {0, 0, rd[2] / rs[2]}}};
  return Multiply(Inverse(cone), Multiply(scale, cone));
}

}  // namespace

// -------------------------------------------------------------------- names ----

std::optional<Space> ParseSpace(const std::string& raw) {
  const auto name = Lower(raw);
  if (name == "rec709" || name == "bt709" || name == "rec.709") return Space{Primaries::Bt709, Transfer::Bt709};
  if (name == "srgb") return Space{Primaries::Bt709, Transfer::Srgb};
  if (name == "p3-d65" || name == "display-p3" || name == "p3") return Space{Primaries::DisplayP3, Transfer::Srgb};
  if (name == "rec2020" || name == "bt2020" || name == "rec.2020") return Space{Primaries::Bt2020, Transfer::Bt709};
  if (name == "rec2020-pq" || name == "bt2020-pq" || name == "pq") return Space{Primaries::Bt2020, Transfer::Pq};
  if (name == "rec2020-hlg" || name == "bt2020-hlg" || name == "hlg") return Space{Primaries::Bt2020, Transfer::Hlg};
  if (name == "linear-rec709" || name == "linear-srgb" || name == "linear-bt709") return Space{Primaries::Bt709, Transfer::Linear};
  if (name == "linear-p3" || name == "linear-p3-d65") return Space{Primaries::DisplayP3, Transfer::Linear};
  if (name == "linear-rec2020" || name == "linear-bt2020") return Space{Primaries::Bt2020, Transfer::Linear};
  if (name == "acescg" || name == "aces-ap1" || name == "linear-ap1") return Space{Primaries::AcesAp1, Transfer::Linear};
  if (name == "aces2065-1" || name == "aces" || name == "linear-ap0") return Space{Primaries::AcesAp0, Transfer::Linear};
  if (name == "acescc") return Space{Primaries::AcesAp1, Transfer::AcesCc};
  if (name == "acescct") return Space{Primaries::AcesAp1, Transfer::AcesCct};
  if (name == "arri-logc3" || name == "logc3" || name == "arri-logc" || name == "logc") return Space{Primaries::ArriWideGamut3, Transfer::LogC3};
  if (name == "sony-slog3" || name == "slog3" || name == "s-log3") return Space{Primaries::SGamut3Cine, Transfer::SLog3};
  if (name == "panasonic-vlog" || name == "vlog" || name == "v-log") return Space{Primaries::VGamut, Transfer::VLog};
  if (name == "red-log3g10" || name == "log3g10") return Space{Primaries::RedWideGamut, Transfer::Log3G10};
  if (name == "linear-arri-wide-gamut3" || name == "linear-awg3") return Space{Primaries::ArriWideGamut3, Transfer::Linear};
  if (name == "linear-sgamut3cine") return Space{Primaries::SGamut3Cine, Transfer::Linear};
  if (name == "linear-vgamut") return Space{Primaries::VGamut, Transfer::Linear};
  if (name == "linear-rwg" || name == "linear-redwidegamutrgb") return Space{Primaries::RedWideGamut, Transfer::Linear};
  return std::nullopt;
}

std::string Name(const Space& space) {
  switch (space.transfer) {
    case Transfer::AcesCc: return "acescc";
    case Transfer::AcesCct: return "acescct";
    case Transfer::LogC3: return "arri-logc3";
    case Transfer::SLog3: return "sony-slog3";
    case Transfer::VLog: return "panasonic-vlog";
    case Transfer::Log3G10: return "red-log3g10";
    default: break;
  }
  if (space.transfer == Transfer::Linear) {
    switch (space.primaries) {
      case Primaries::DisplayP3: return "linear-p3";
      case Primaries::Bt2020: return "linear-rec2020";
      case Primaries::AcesAp1: return "acescg";
      case Primaries::AcesAp0: return "aces2065-1";
      case Primaries::ArriWideGamut3: return "linear-arri-wide-gamut3";
      case Primaries::SGamut3Cine: return "linear-sgamut3cine";
      case Primaries::VGamut: return "linear-vgamut";
      case Primaries::RedWideGamut: return "linear-rwg";
      case Primaries::Bt709: break;
    }
    return "linear-rec709";
  }
  if (space.primaries == Primaries::Bt2020) {
    if (space.transfer == Transfer::Pq) return "rec2020-pq";
    if (space.transfer == Transfer::Hlg) return "rec2020-hlg";
    return "rec2020";
  }
  if (space.primaries == Primaries::DisplayP3) return "p3-d65";
  return space.transfer == Transfer::Srgb ? "srgb" : "rec709";
}

std::optional<Space> SpaceFromTags(const std::string& primaries_tag, const std::string& transfer_tag) {
  const auto p = Lower(primaries_tag);
  const auto t = Lower(transfer_tag);
  Space space;
  if (p == "bt709" || p.empty() || p == "unknown" || p == "unspecified" || p == "smpte240m" || p == "smpte170m" || p == "bt470bg") space.primaries = Primaries::Bt709;
  else if (p == "bt2020" || p == "bt2020nc" || p == "bt2020c") space.primaries = Primaries::Bt2020;
  else if (p == "smpte432" || p == "display-p3" || p == "p3-d65") space.primaries = Primaries::DisplayP3;
  else return std::nullopt;
  if (t == "bt709" || t.empty() || t == "unknown" || t == "unspecified" || t == "smpte240m" || t == "smpte170m" || t == "bt2020-10" || t == "bt2020-12") space.transfer = Transfer::Bt709;
  else if (t == "iec61966-2-1" || t == "srgb") space.transfer = Transfer::Srgb;
  else if (t == "gamma24" || t == "bt1886") space.transfer = Transfer::Gamma24;
  else if (t == "linear") space.transfer = Transfer::Linear;
  else if (t == "smpte2084" || t == "pq") space.transfer = Transfer::Pq;
  else if (t == "arib-std-b67" || t == "hlg") space.transfer = Transfer::Hlg;
  else return std::nullopt;
  return space;
}

Tags TagsOf(const Space& space) {
  Tags tags;
  switch (space.primaries) {
    case Primaries::Bt2020: tags.primaries = "bt2020"; break;
    case Primaries::DisplayP3: tags.primaries = "smpte432"; break;
    case Primaries::Bt709: tags.primaries = "bt709"; break;
    default: tags.primaries = "unknown"; break;   // no container tag names these gamuts
  }
  switch (space.transfer) {
    case Transfer::AcesCc: case Transfer::AcesCct: case Transfer::LogC3: case Transfer::SLog3: case Transfer::VLog: case Transfer::Log3G10: tags.transfer = "unknown"; break;
    case Transfer::Srgb: tags.transfer = "iec61966-2-1"; break;
    case Transfer::Gamma24: tags.transfer = "gamma24"; break;
    case Transfer::Linear: tags.transfer = "linear"; break;
    case Transfer::Pq: tags.transfer = "smpte2084"; break;
    case Transfer::Hlg: tags.transfer = "arib-std-b67"; break;
    case Transfer::Bt709: tags.transfer = "bt709"; break;
  }
  return tags;
}

// ---------------------------------------------------------------- transfers ----

double ToLinear(Transfer transfer, double v) {
  switch (transfer) {
    case Transfer::Bt709: return Bt709Decode(v);
    case Transfer::Srgb: return SrgbDecode(v);
    case Transfer::Gamma24: return std::pow(std::max(v, 0.0), 2.4);
    case Transfer::Linear: return v;
    case Transfer::Pq: return PqToNitsImpl(v) / kReferenceWhiteNits;
    case Transfer::Hlg: {
      // For a grey, where luminance is the channel itself: light = 1000 nits * E^1.2.
      const auto e = HlgInverseOetfImpl(v);
      return kHlgDisplayNits * std::pow(e, kHlgSystemGamma) / kReferenceWhiteNits;
    }
    case Transfer::AcesCc: return AccsDecode(v);
    case Transfer::AcesCct: return AcesCctDecode(v);
    case Transfer::LogC3: return LogC3Decode(v);
    case Transfer::SLog3: return SLog3Decode(v);
    case Transfer::VLog: return VLogDecode(v);
    case Transfer::Log3G10: return Log3G10Decode(v);
  }
  return v;
}

double FromLinear(Transfer transfer, double l) {
  switch (transfer) {
    case Transfer::Bt709: return Bt709Encode(std::max(l, 0.0));
    case Transfer::Srgb: return SrgbEncode(std::max(l, 0.0));
    case Transfer::Gamma24: return std::pow(std::max(l, 0.0), 1.0 / 2.4);
    case Transfer::Linear: return l;
    case Transfer::Pq: return NitsToPqImpl(l * kReferenceWhiteNits);
    case Transfer::Hlg: {
      const auto e = std::pow(std::max(l, 0.0) * kReferenceWhiteNits / kHlgDisplayNits, 1.0 / kHlgSystemGamma);
      return HlgOetfImpl(e);
    }
    case Transfer::AcesCc: return AccsEncode(l);
    case Transfer::AcesCct: return AcesCctEncode(l);
    case Transfer::LogC3: return LogC3Encode(l);
    case Transfer::SLog3: return SLog3Encode(l);
    case Transfer::VLog: return VLogEncode(l);
    case Transfer::Log3G10: return Log3G10Encode(l);
  }
  return l;
}

double HlgOetf(double scene_linear) { return HlgOetfImpl(scene_linear); }
double HlgInverseOetf(double signal) { return HlgInverseOetfImpl(signal); }
double NitsToPq(double nits) { return NitsToPqImpl(nits); }
double PqToNits(double signal) { return PqToNitsImpl(signal); }

// ---------------------------------------------------------------- primaries ----

Matrix RgbToXyz(Primaries primaries) {
  const auto gamut = GamutOf(primaries);
  const auto xyz = [](const Chromaticity& c) { return std::array<double, 3>{c.x / c.y, 1.0, (1.0 - c.x - c.y) / c.y}; };
  const auto own_white = WhiteOf(primaries);
  const auto r = xyz(gamut.r), g = xyz(gamut.g), b = xyz(gamut.b), w = xyz(own_white);
  const Matrix columns{{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
  const auto inverse = Inverse(columns);
  std::array<double, 3> scale{};
  for (std::size_t i = 0; i < 3; ++i) scale[i] = inverse[i][0] * w[0] + inverse[i][1] * w[1] + inverse[i][2] * w[2];
  Matrix out{};
  for (std::size_t row = 0; row < 3; ++row) {
    out[row] = {columns[row][0] * scale[0], columns[row][1] * scale[1], columns[row][2] * scale[2]};
  }
  // A set with another white than D65 is brought to it, so that its white and D65's coincide.
  if (own_white.x != kD65.x || own_white.y != kD65.y) out = Multiply(Bradford(own_white, kD65), out);
  return out;
}

Matrix Rotation(Primaries from, Primaries to) {
  if (from == to) return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  return Multiply(Inverse(RgbToXyz(to)), RgbToXyz(from));
}

std::array<double, 3> LumaCoefficients(Primaries primaries) {
  const auto m = RgbToXyz(primaries);
  return {m[1][0], m[1][1], m[1][2]};
}

// ---------------------------------------------------------------- transform ----

double Transform::SoftKnee(double linear, double peak_relative) {
  constexpr double kKnee = 0.75;
  if (linear <= kKnee) return linear;
  if (peak_relative <= 1.0) return std::min(linear, 1.0);
  // Continues the identity's slope at the knee and arrives at exactly 1.0 at the peak.
  const auto a = (peak_relative - kKnee) / (1.0 - kKnee) - 1.0;
  const auto u = std::min((linear - kKnee) / (peak_relative - kKnee), 1.0);
  return kKnee + (1.0 - kKnee) * ((1.0 + a) * u / (1.0 + a * u));
}

Transform::Transform(const Space& from, const Space& to, const Options& options)
    : from_(from), to_(to), options_(options), matrix_(Rotation(from.primaries, to.primaries)),
      identity_(from == to), same_gamut_(from.primaries == to.primaries), target_luma_(LumaCoefficients(to.primaries)) {}

std::array<double, 3> Transform::Apply(double r, double g, double b) const {
  if (identity_) return {r, g, b};

  // Decode to linear light, relative to reference white, in the source primaries.
  std::array<double, 3> linear;
  if (from_.transfer == Transfer::Hlg) {
    // Scene light per channel, then the BT.2100 system gamma applied through luminance.
    const std::array<double, 3> scene{HlgInverseOetfImpl(r), HlgInverseOetfImpl(g), HlgInverseOetfImpl(b)};
    const auto k = LumaCoefficients(from_.primaries);
    const auto ys = k[0] * scene[0] + k[1] * scene[1] + k[2] * scene[2];
    const auto gain = ys > 0.0 ? kHlgDisplayNits * std::pow(ys, kHlgSystemGamma - 1.0) / kReferenceWhiteNits : 0.0;
    linear = {scene[0] * gain, scene[1] * gain, scene[2] * gain};
  } else {
    linear = {ToLinear(from_.transfer, r), ToLinear(from_.transfer, g), ToLinear(from_.transfer, b)};
  }

  // Rotate into the target primaries.
  std::array<double, 3> rgb = linear;
  if (!same_gamut_) {
    for (std::size_t i = 0; i < 3; ++i) rgb[i] = matrix_[i][0] * linear[0] + matrix_[i][1] * linear[1] + matrix_[i][2] * linear[2];
  }

  // HDR light an SDR target cannot show is fitted into it, on luminance so that hue holds.
  // A linear working space keeps its headroom; only a display-encoded SDR target needs the light fitted.
  const bool to_sdr = !to_.hdr() && !to_.linear();
  if (to_sdr && (from_.hdr() || from_.scene_referred()) && options_.tone_map) {
    const auto luminance = target_luma_[0] * rgb[0] + target_luma_[1] * rgb[1] + target_luma_[2] * rgb[2];
    if (luminance > 0.0) {
      // Scene light has no stated peak: four stops over reference white are fitted.
      constexpr double kSceneReferredPeak = 16.0;
      const auto mapped = SoftKnee(luminance, from_.scene_referred() ? kSceneReferredPeak : options_.source_peak_nits / kReferenceWhiteNits);
      const auto scale = mapped / luminance;
      for (auto& channel : rgb) channel *= scale;
    }
  }

  // Colours the target's primaries cannot make.
  const double upper = to_sdr ? 1.0 : 1e30;
  const auto luminance = target_luma_[0] * rgb[0] + target_luma_[1] * rgb[1] + target_luma_[2] * rgb[2];
  const bool out_of_range = std::any_of(rgb.begin(), rgb.end(), [&](double c) { return c < 0.0 || c > upper; });
  if (out_of_range) {
    if (options_.gamut == GamutMapping::Desaturate && luminance > 0.0) {
      const auto grey = std::min(luminance, upper);
      auto t = 1.0;
      for (const auto c : rgb) {
        if (c < 0.0) t = std::min(t, grey / (grey - c));
        else if (c > upper) t = std::min(t, (upper - grey) / (c - grey));
      }
      for (auto& c : rgb) c = grey + (c - grey) * t;
    }
    for (auto& c : rgb) c = std::clamp(c, 0.0, upper);
  }

  // Encode.
  if (to_.transfer == Transfer::Hlg) {
    const auto light_y = target_luma_[0] * rgb[0] + target_luma_[1] * rgb[1] + target_luma_[2] * rgb[2];
    const auto nits_y = light_y * kReferenceWhiteNits;
    if (nits_y <= 0.0) return {0.0, 0.0, 0.0};
    // Undo the system gamma: scene luminance from display luminance, then each channel's share of it.
    const auto ys = std::pow(nits_y / kHlgDisplayNits, 1.0 / kHlgSystemGamma);
    const auto gain = kHlgDisplayNits * std::pow(ys, kHlgSystemGamma - 1.0);
    return {HlgOetfImpl(rgb[0] * kReferenceWhiteNits / gain), HlgOetfImpl(rgb[1] * kReferenceWhiteNits / gain), HlgOetfImpl(rgb[2] * kReferenceWhiteNits / gain)};
  }
  std::array<double, 3> encoded{FromLinear(to_.transfer, rgb[0]), FromLinear(to_.transfer, rgb[1]), FromLinear(to_.transfer, rgb[2])};
  if (to_.transfer != Transfer::Linear) {
    for (auto& c : encoded) c = std::clamp(c, 0.0, 1.0);
  }
  return encoded;
}

void Transform::Apply(float* rgb) const {
  if (identity_) return;
  const auto out = Apply(rgb[0], rgb[1], rgb[2]);
  rgb[0] = static_cast<float>(out[0]);
  rgb[1] = static_cast<float>(out[1]);
  rgb[2] = static_cast<float>(out[2]);
}

}  // namespace cutline::render::color

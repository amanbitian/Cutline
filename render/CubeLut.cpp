#include "render/CubeLut.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cutline::render {
namespace {

[[nodiscard]] std::string_view Trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

// Reads up to `count` numbers from the front of `text`, advancing it. Returns how many were read. Locale-independent
// (a table is read the same on a machine that writes its decimals with commas), and without a stream's per-number cost,
// which is most of the time a 65^3 table takes to load.
[[nodiscard]] int ReadNumbers(std::string_view& text, float* out, int count) {
  int read = 0;
  while (read < count) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    if (text.empty()) break;
    std::string_view number = text;
    if (number.front() == '+') number.remove_prefix(1);
    float value = 0.0f;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (error != std::errc{}) break;
    text = std::string_view(end, static_cast<std::size_t>(text.data() + text.size() - end));
    out[read++] = value;
  }
  return read;
}

[[nodiscard]] std::array<float, 3> ReadTriple(std::string_view text, const char* field) {
  std::array<float, 3> result{};
  if (ReadNumbers(text, result.data(), 3) != 3) throw std::runtime_error(std::string("Invalid .cube ") + field);
  if (!Trim(text).empty()) throw std::runtime_error(std::string("Unexpected data in .cube ") + field);
  return result;
}

[[nodiscard]] bool StartsWithKeyword(std::string_view line, std::string_view keyword) {
  return line.size() >= keyword.size() && line.compare(0, keyword.size(), keyword) == 0 &&
         (line.size() == keyword.size() || line[keyword.size()] == ' ' || line[keyword.size()] == '\t');
}

}  // namespace

CubeLut CubeLut::Parse(std::istream& input) {
  CubeLut lut;
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::size_t line_number = 0;
  bool seen_1d = false, seen_3d = false;
  std::size_t expected = 0;
  std::size_t position = 0;
  while (position <= text.size()) {
    const auto end = std::min(text.find('\n', position), text.size());
    std::string_view line(text.data() + position, end - position);
    position = end + 1;
    ++line_number;
    if (const auto comment = line.find('#'); comment != std::string_view::npos) line = line.substr(0, comment);
    line = Trim(line);
    if (line.empty()) continue;

    if (StartsWithKeyword(line, "TITLE")) continue;
    if (StartsWithKeyword(line, "LUT_1D_SIZE") || StartsWithKeyword(line, "LUT_3D_SIZE")) {
      const bool is_1d = line[4] == '1';
      if (lut.size_ != 0) throw std::runtime_error("Invalid or repeated .cube LUT_" + std::string(is_1d ? "1D" : "3D") + "_SIZE");
      float size = 0.0f;
      auto rest = line.substr(11);
      if (ReadNumbers(rest, &size, 1) != 1 || size != std::floor(size) || !Trim(rest).empty()) {
        throw std::runtime_error("Invalid or repeated .cube LUT_" + std::string(is_1d ? "1D" : "3D") + "_SIZE");
      }
      lut.size_ = static_cast<int>(size);
      if (lut.size_ < 2 || lut.size_ > (is_1d ? 65536 : 256)) throw std::runtime_error("Invalid or repeated .cube LUT_" + std::string(is_1d ? "1D" : "3D") + "_SIZE");
      lut.kind_ = is_1d ? Kind::Curves1D : Kind::Cube3D;
      (is_1d ? seen_1d : seen_3d) = true;
      expected = is_1d ? static_cast<std::size_t>(lut.size_) : static_cast<std::size_t>(lut.size_) * lut.size_ * lut.size_;
      lut.entries_.reserve(expected * 3);
      continue;
    }
    if (StartsWithKeyword(line, "DOMAIN_MIN") || StartsWithKeyword(line, "DOMAIN_MAX")) {
      const bool is_min = line[8] == 'I';
      const auto values = ReadTriple(line.substr(10), is_min ? "DOMAIN_MIN" : "DOMAIN_MAX");
      (is_min ? lut.domain_min_ : lut.domain_max_) = values;
      continue;
    }
    if (StartsWithKeyword(line, "LUT_1D_INPUT_RANGE") || StartsWithKeyword(line, "LUT_3D_INPUT_RANGE")) {
      // Adobe's spelling: one range for all three channels.
      auto rest = line.substr(18);
      float range[2];
      if (ReadNumbers(rest, range, 2) != 2 || !Trim(rest).empty()) throw std::runtime_error("Invalid .cube input range");
      lut.domain_min_ = {range[0], range[0], range[0]};
      lut.domain_max_ = {range[1], range[1], range[1]};
      continue;
    }

    if (lut.size_ == 0) {
      throw std::runtime_error(".cube data appears before LUT_3D_SIZE at line " + std::to_string(line_number));
    }
    float triple[3];
    auto rest = line;
    if (ReadNumbers(rest, triple, 3) != 3) throw std::runtime_error("Invalid .cube entry");
    if (!Trim(rest).empty()) throw std::runtime_error("Unexpected data in .cube entry");
    lut.entries_.insert(lut.entries_.end(), triple, triple + 3);
  }

  if (seen_1d && seen_3d) throw std::runtime_error(".cube files holding both a 1D and a 3D table are not supported");
  if (lut.size_ == 0) throw std::runtime_error(".cube file has no LUT_3D_SIZE");
  if (lut.entries_.size() != expected * 3) {
    throw std::runtime_error(".cube entry count is " + std::to_string(lut.entries_.size() / 3) + ", expected " + std::to_string(expected));
  }
  for (int channel = 0; channel < 3; ++channel) {
    if (!(lut.domain_max_[channel] > lut.domain_min_[channel])) {
      throw std::runtime_error(".cube DOMAIN_MAX must be greater than DOMAIN_MIN");
    }
  }
  lut.default_domain_ = lut.domain_min_ == std::array<float, 3>{0.0f, 0.0f, 0.0f} && lut.domain_max_ == std::array<float, 3>{1.0f, 1.0f, 1.0f};
  return lut;
}

CubeLut CubeLut::Load(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open LUT: " + path.string());
  return Parse(input);
}

std::array<float, 3> CubeLut::Sample(float red, float green, float blue) const {
  if (size_ < 2 || entries_.empty()) throw std::logic_error("Cannot sample an empty LUT");
  const float input[3] = {red, green, blue};
  std::array<float, 3> output{};
  Map(input, output.data());
  return output;
}

void CubeLut::Map(const float input[3], float output[3]) const noexcept {
  const float scale = static_cast<float>(size_ - 1);
  int low[3], high[3];
  float fraction[3];
  for (int channel = 0; channel < 3; ++channel) {
    float normalised = default_domain_ ? input[channel] : (input[channel] - domain_min_[channel]) / (domain_max_[channel] - domain_min_[channel]);
    // Held at the edges of the domain; written so that a NaN lands on the first entry rather than on an undefined index.
    normalised = normalised > 0.0f ? (normalised < 1.0f ? normalised : 1.0f) : 0.0f;
    const float coordinate = normalised * scale;
    low[channel] = static_cast<int>(std::floor(coordinate));
    high[channel] = std::min(low[channel] + 1, size_ - 1);
    fraction[channel] = coordinate - static_cast<float>(low[channel]);
  }
  const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  const float* table = entries_.data();

  if (kind_ == Kind::Curves1D) {
    for (int channel = 0; channel < 3; ++channel) {
      output[channel] = lerp(table[static_cast<std::size_t>(low[channel]) * 3 + channel], table[static_cast<std::size_t>(high[channel]) * 3 + channel], fraction[channel]);
    }
    return;
  }

  // The eight corners of the cell, found once for all three channels.
  const std::size_t stride_g = static_cast<std::size_t>(size_) * 3;
  const std::size_t stride_b = stride_g * static_cast<std::size_t>(size_);
  const auto corner = [&](int r, int g, int b) { return table + static_cast<std::size_t>(r) * 3 + static_cast<std::size_t>(g) * stride_g + static_cast<std::size_t>(b) * stride_b; };
  const float* p000 = corner(low[0], low[1], low[2]);
  const float* p100 = corner(high[0], low[1], low[2]);
  const float* p010 = corner(low[0], high[1], low[2]);
  const float* p110 = corner(high[0], high[1], low[2]);
  const float* p001 = corner(low[0], low[1], high[2]);
  const float* p101 = corner(high[0], low[1], high[2]);
  const float* p011 = corner(low[0], high[1], high[2]);
  const float* p111 = corner(high[0], high[1], high[2]);
  for (int channel = 0; channel < 3; ++channel) {
    const float c00 = lerp(p000[channel], p100[channel], fraction[0]);
    const float c10 = lerp(p010[channel], p110[channel], fraction[0]);
    const float c01 = lerp(p001[channel], p101[channel], fraction[0]);
    const float c11 = lerp(p011[channel], p111[channel], fraction[0]);
    output[channel] = lerp(lerp(c00, c10, fraction[1]), lerp(c01, c11, fraction[1]), fraction[2]);
  }
}

}  // namespace cutline::render

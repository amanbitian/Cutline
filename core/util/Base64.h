#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::util {

[[nodiscard]] inline std::string Base64Encode(const std::vector<std::byte>& bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const unsigned a = static_cast<unsigned char>(bytes[i]);
    const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
    const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
    const unsigned triple = (a << 16) | (b << 8) | c;
    out.push_back(kAlphabet[(triple >> 18) & 63]);
    out.push_back(kAlphabet[(triple >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 63] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 63] : '=');
  }
  return out;
}

// Strict: any character outside the alphabet, or a length that is not a multiple
// of four, is an error, so a damaged record is never half-decoded.
[[nodiscard]] inline std::vector<std::byte> Base64Decode(std::string_view text) {
  if (text.size() % 4 != 0) throw std::invalid_argument("Base64 text has the wrong length");
  const auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<std::byte> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const bool pad2 = text[i + 2] == '=';
    const bool pad3 = text[i + 3] == '=';
    if ((pad2 && !pad3) || ((pad2 || pad3) && i + 4 != text.size())) throw std::invalid_argument("Base64 padding is misplaced");
    const int a = value(text[i]), b = value(text[i + 1]);
    const int c = pad2 ? 0 : value(text[i + 2]), d = pad3 ? 0 : value(text[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) throw std::invalid_argument("Base64 text has an invalid character");
    const unsigned triple = (static_cast<unsigned>(a) << 18) | (static_cast<unsigned>(b) << 12) |
                            (static_cast<unsigned>(c) << 6) | static_cast<unsigned>(d);
    out.push_back(static_cast<std::byte>((triple >> 16) & 255));
    if (!pad2) out.push_back(static_cast<std::byte>((triple >> 8) & 255));
    if (!pad3) out.push_back(static_cast<std::byte>(triple & 255));
  }
  return out;
}

}  // namespace cutline::util

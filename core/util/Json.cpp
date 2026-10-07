#include "core/util/Json.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace cutline::json {
namespace {

constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

}  // namespace

std::string Escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char character : value) {
    switch (character) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20) {
          // \u00XX is the only legal form for the remaining control characters.
          out += "\\u00";
          out += kHexDigits[(byte >> 4) & 0x0F];
          out += kHexDigits[byte & 0x0F];
        } else {
          // Bytes >= 0x20 pass through, which keeps valid UTF-8 intact.
          out += character;
        }
        break;
      }
    }
  }
  return out;
}

std::string Number(double value) {
  // Non-finite values have no JSON representation; null is the honest encoding.
  if (!std::isfinite(value)) return "null";
  // 17 significant digits round-trips an IEEE-754 double exactly.
  std::array<char, 32> buffer{};
  const auto written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
  if (written <= 0) return "null";
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

void Object::Separate() {
  if (!body_.empty()) body_ += ',';
}

Object& Object::Add(std::string_view key, std::string_view value) {
  Separate();
  body_ += '"';
  body_ += Escape(key);
  body_ += "\":\"";
  body_ += Escape(value);
  body_ += '"';
  return *this;
}

Object& Object::Add(std::string_view key, std::int64_t value) {
  Separate();
  body_ += '"';
  body_ += Escape(key);
  body_ += "\":";
  body_ += std::to_string(value);
  return *this;
}

Object& Object::Add(std::string_view key, double value) {
  Separate();
  body_ += '"';
  body_ += Escape(key);
  body_ += "\":";
  body_ += Number(value);
  return *this;
}

Object& Object::Add(std::string_view key, bool value) {
  Separate();
  body_ += '"';
  body_ += Escape(key);
  body_ += "\":";
  body_ += value ? "true" : "false";
  return *this;
}

Object& Object::Add(std::string_view key, const time::RationalTime& value) {
  // Rational times are written as a pair, never as a decimal: a journal that
  // rounded 1001/30000 to a float would not replay frame-accurately.
  Object pair;
  pair.Add("num", value.numerator()).Add("den", value.denominator());
  return AddRaw(key, pair.Build());
}

Object& Object::Add(std::string_view key, const time::FrameRate& value) {
  Object pair;
  pair.Add("num", value.numerator).Add("den", value.denominator);
  return AddRaw(key, pair.Build());
}

Object& Object::Add(std::string_view key, const anim::Value& value) {
  std::vector<std::string> components;
  components.reserve(static_cast<std::size_t>(value.dimension));
  for (int index = 0; index < value.dimension; ++index) {
    components.push_back(Number(value.components[static_cast<std::size_t>(index)]));
  }
  return AddRaw(key, Array(components));
}

Object& Object::Add(std::string_view key, const std::optional<std::string>& value) {
  return value.has_value() ? Add(key, std::string_view(*value)) : AddNull(key);
}

Object& Object::AddRaw(std::string_view key, std::string_view json) {
  Separate();
  body_ += '"';
  body_ += Escape(key);
  body_ += "\":";
  body_.append(json);
  return *this;
}

Object& Object::AddNull(std::string_view key) {
  Separate();
  body_ += '"';
  body_ += Escape(key);
  body_ += "\":null";
  return *this;
}

std::string Object::Build() const { return "{" + body_ + "}"; }

std::string Array(const std::vector<std::string>& elements) {
  std::string out = "[";
  for (std::size_t index = 0; index < elements.size(); ++index) {
    if (index != 0) out += ',';
    out += elements[index];
  }
  out += ']';
  return out;
}

}  // namespace cutline::json

#include "core/util/JsonParse.h"

#include <cmath>
#include <cstdlib>

namespace cutline::json {
namespace {

class Parser final {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Value Document() {
    SkipSpace();
    auto value = ParseValue(0);
    SkipSpace();
    if (position_ != text_.size()) Fail("unexpected text after the document");
    return value;
  }

 private:
  [[noreturn]] void Fail(const std::string& what) const {
    throw ParseError("Invalid JSON at byte " + std::to_string(position_) + ": " + what);
  }

  void SkipSpace() {
    while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                         text_[position_] == '\n' || text_[position_] == '\r')) {
      ++position_;
    }
  }

  [[nodiscard]] char Peek() const { return position_ < text_.size() ? text_[position_] : '\0'; }

  void Expect(char c) {
    if (Peek() != c) Fail(std::string("expected '") + c + "'");
    ++position_;
  }

  Value ParseValue(int depth) {
    if (depth > 64) Fail("nesting is too deep");
    SkipSpace();
    if (position_ >= text_.size()) Fail("the document ends early");
    const auto start = position_;
    Value value;
    switch (Peek()) {
      case '{': ParseObject(value, depth); value.raw = std::string(text_.substr(start, position_ - start)); break;
      case '[': ParseArray(value, depth); value.raw = std::string(text_.substr(start, position_ - start)); break;
      case '"': value.kind = Value::Kind::String; value.text = ParseString(); break;
      case 't': Literal("true"); value.kind = Value::Kind::Bool; value.boolean = true; break;
      case 'f': Literal("false"); value.kind = Value::Kind::Bool; value.boolean = false; break;
      case 'n': Literal("null"); break;
      default: ParseNumber(value); break;
    }
    return value;
  }

  void Literal(std::string_view word) {
    if (text_.substr(position_, word.size()) != word) Fail("unrecognised literal");
    position_ += word.size();
  }

  void ParseNumber(Value& value) {
    const auto start = position_;
    if (Peek() == '-') ++position_;
    if (Peek() == '0') {
      ++position_;
    } else if (Peek() >= '1' && Peek() <= '9') {
      while (Peek() >= '0' && Peek() <= '9') ++position_;
    } else {
      Fail("expected a value");
    }
    if (Peek() == '.') {
      ++position_;
      if (!(Peek() >= '0' && Peek() <= '9')) Fail("digits expected after the decimal point");
      while (Peek() >= '0' && Peek() <= '9') ++position_;
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++position_;
      if (Peek() == '+' || Peek() == '-') ++position_;
      if (!(Peek() >= '0' && Peek() <= '9')) Fail("digits expected in the exponent");
      while (Peek() >= '0' && Peek() <= '9') ++position_;
    }
    value.kind = Value::Kind::Number;
    value.number_text = std::string(text_.substr(start, position_ - start));
    value.number = std::strtod(value.number_text.c_str(), nullptr);
  }

  static void AppendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  std::uint32_t Hex4() {
    std::uint32_t code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = Peek();
      code <<= 4;
      if (c >= '0' && c <= '9') code |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') code |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') code |= static_cast<std::uint32_t>(c - 'A' + 10);
      else Fail("bad \\u escape");
      ++position_;
    }
    return code;
  }

  std::string ParseString() {
    Expect('"');
    std::string out;
    while (true) {
      if (position_ >= text_.size()) Fail("the string is not closed");
      const char c = text_[position_++];
      if (c == '"') break;
      if (static_cast<unsigned char>(c) < 0x20) Fail("a control character in a string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (position_ >= text_.size()) Fail("the string is not closed");
      const char e = text_[position_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          auto code = Hex4();
          if (code >= 0xD800 && code <= 0xDBFF && Peek() == '\\' && position_ + 1 < text_.size() &&
              text_[position_ + 1] == 'u') {
            position_ += 2;
            const auto low = Hex4();
            if (low < 0xDC00 || low > 0xDFFF) Fail("a bad surrogate pair");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(out, code);
          break;
        }
        default: Fail("an unknown escape");
      }
    }
    return out;
  }

  void ParseArray(Value& value, int depth) {
    value.kind = Value::Kind::Array;
    Expect('[');
    SkipSpace();
    if (Peek() == ']') {
      ++position_;
      return;
    }
    while (true) {
      value.items.push_back(ParseValue(depth + 1));
      SkipSpace();
      if (Peek() == ',') {
        ++position_;
        continue;
      }
      Expect(']');
      return;
    }
  }

  void ParseObject(Value& value, int depth) {
    value.kind = Value::Kind::Object;
    Expect('{');
    SkipSpace();
    if (Peek() == '}') {
      ++position_;
      return;
    }
    while (true) {
      SkipSpace();
      auto key = ParseString();
      SkipSpace();
      Expect(':');
      value.members.emplace_back(std::move(key), ParseValue(depth + 1));
      SkipSpace();
      if (Peek() == ',') {
        ++position_;
        continue;
      }
      Expect('}');
      return;
    }
  }

  std::string_view text_;
  std::size_t position_{0};
};

}  // namespace

const Value* Value::Find(std::string_view key) const {
  if (kind != Kind::Object) return nullptr;
  for (const auto& [name, member] : members) {
    if (name == key) return &member;
  }
  return nullptr;
}

const Value& Value::Require(std::string_view key) const {
  const auto* member = Find(key);
  if (member == nullptr) throw ParseError("Missing member \"" + std::string(key) + "\"");
  return *member;
}

const std::string& Value::String(std::string_view key) const {
  const auto& member = Require(key);
  if (!member.is_string()) throw ParseError("Member \"" + std::string(key) + "\" is not a string");
  return member.text;
}

std::int64_t Value::AsInteger() const {
  if (kind != Kind::Number) throw ParseError("Expected a number");
  char* end = nullptr;
  const auto parsed = std::strtoll(number_text.c_str(), &end, 10);
  if (end != nullptr && *end == '\0') return parsed;
  const auto rounded = std::llround(number);
  if (std::abs(number - static_cast<double>(rounded)) > 1e-9) throw ParseError("Expected an integer, found " + number_text);
  return rounded;
}

std::int64_t Value::Integer(std::string_view key) const { return Require(key).AsInteger(); }

double Value::Number(std::string_view key) const {
  const auto& member = Require(key);
  if (!member.is_number()) throw ParseError("Member \"" + std::string(key) + "\" is not a number");
  return member.number;
}

bool Value::Bool(std::string_view key) const {
  const auto& member = Require(key);
  if (member.kind != Kind::Bool) throw ParseError("Member \"" + std::string(key) + "\" is not a boolean");
  return member.boolean;
}

Value Parse(std::string_view text) { return Parser(text).Document(); }

}  // namespace cutline::json

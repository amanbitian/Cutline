#pragma once

// A small JSON reader, for the files the project writes itself (journal records,
// interchange documents). It is strict: anything that is not valid JSON is an error
// naming the byte where it went wrong, because a torn or corrupted record must be
// recognised as such, not half-read.
//
// Objects and arrays keep the exact text they were parsed from (`raw`), so a nested
// document can be handed on unchanged.

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::json {

class ParseError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Value final {
 public:
  enum class Kind { Null, Bool, Number, String, Array, Object };

  Kind kind{Kind::Null};
  bool boolean{false};
  double number{0.0};
  // The number as written, for integers too large to be exact as a double.
  std::string number_text;
  std::string text;  // string contents, unescaped
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> members;
  std::string raw;  // source text of an object or array

  [[nodiscard]] bool is_null() const { return kind == Kind::Null; }
  [[nodiscard]] bool is_object() const { return kind == Kind::Object; }
  [[nodiscard]] bool is_array() const { return kind == Kind::Array; }
  [[nodiscard]] bool is_string() const { return kind == Kind::String; }
  [[nodiscard]] bool is_number() const { return kind == Kind::Number; }

  // Member lookup; nullptr when absent or not an object.
  [[nodiscard]] const Value* Find(std::string_view key) const;
  // Typed access with a clear error when the member is missing or the wrong kind.
  [[nodiscard]] const Value& Require(std::string_view key) const;
  [[nodiscard]] const std::string& String(std::string_view key) const;
  [[nodiscard]] std::int64_t Integer(std::string_view key) const;
  [[nodiscard]] double Number(std::string_view key) const;
  [[nodiscard]] bool Bool(std::string_view key) const;
  [[nodiscard]] std::int64_t AsInteger() const;
};

// Parses a complete document. Trailing characters other than whitespace are an error.
[[nodiscard]] Value Parse(std::string_view text);

}  // namespace cutline::json

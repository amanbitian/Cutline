#pragma once

// Minimal JSON writer for the command journal.
//
// Write-only on purpose. The journal is an audit and replay record that is
// produced here and consumed by tools, and the undo stack is driven by database
// changesets rather than by re-parsing payloads, so the project needs no JSON
// parser and therefore no JSON dependency.

#include "core/anim/Keyframe.h"
#include "core/time/RationalTime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::json {

// Escapes per RFC 8259, including the control characters below 0x20 that a
// naive escaper misses and that would make the journal unparseable.
[[nodiscard]] std::string Escape(std::string_view value);

class Object final {
 public:
  Object& Add(std::string_view key, std::string_view value);
  Object& Add(std::string_view key, const std::string& value) { return Add(key, std::string_view(value)); }
  // Without this, a string literal converts to bool (a standard conversion) in preference to
  // string_view (a user-defined one) and is written as true.
  Object& Add(std::string_view key, const char* value) { return Add(key, std::string_view(value)); }
  Object& Add(std::string_view key, std::int64_t value);
  Object& Add(std::string_view key, double value);
  Object& Add(std::string_view key, bool value);
  Object& Add(std::string_view key, const time::RationalTime& value);
  Object& Add(std::string_view key, const time::FrameRate& value);
  Object& Add(std::string_view key, const anim::Value& value);
  Object& Add(std::string_view key, const std::optional<std::string>& value);
  // Inserts already-formed JSON, for nested objects and arrays.
  Object& AddRaw(std::string_view key, std::string_view json);
  Object& AddNull(std::string_view key);

  [[nodiscard]] std::string Build() const;

 private:
  void Separate();
  std::string body_;
};

[[nodiscard]] std::string Array(const std::vector<std::string>& elements);
[[nodiscard]] std::string Number(double value);

}  // namespace cutline::json

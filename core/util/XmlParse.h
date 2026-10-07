#pragma once

// A small XML reader for the interchange files the project reads (Final Cut Pro 7
// XML). It builds an element tree: names, attributes, character data and children.
// It understands the XML declaration, comments, CDATA, the five predefined entities and
// numeric character references; it skips a DOCTYPE; it does not resolve external entities
// or validate, and it is strict about nesting: a mismatched or unclosed tag is an error
// naming the line, because a truncated file must not be read as a short but valid one.

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::xml {

class ParseError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct Node final {
  std::string name;
  std::map<std::string, std::string> attributes;
  std::string text;  // the element's own character data, trimmed
  std::vector<Node> children;

  [[nodiscard]] const Node* Find(std::string_view child_name) const;
  [[nodiscard]] std::vector<const Node*> FindAll(std::string_view child_name) const;
  // The text of a child, or the fallback when it is absent.
  [[nodiscard]] std::string ChildText(std::string_view child_name, const std::string& fallback = {}) const;
  [[nodiscard]] std::string Attribute(std::string_view attribute_name, const std::string& fallback = {}) const;
};

[[nodiscard]] Node Parse(std::string_view text);

// Escapes text for element content or an attribute value.
[[nodiscard]] std::string Escape(std::string_view text);

}  // namespace cutline::xml

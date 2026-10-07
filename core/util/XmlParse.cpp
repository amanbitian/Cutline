#include "core/util/XmlParse.h"

#include <cctype>
#include <cstdlib>

namespace cutline::xml {

const Node* Node::Find(std::string_view child_name) const {
  for (const auto& child : children) {
    if (child.name == child_name) return &child;
  }
  return nullptr;
}

std::vector<const Node*> Node::FindAll(std::string_view child_name) const {
  std::vector<const Node*> found;
  for (const auto& child : children) {
    if (child.name == child_name) found.push_back(&child);
  }
  return found;
}

std::string Node::ChildText(std::string_view child_name, const std::string& fallback) const {
  const auto* child = Find(child_name);
  return child != nullptr ? child->text : fallback;
}

std::string Node::Attribute(std::string_view attribute_name, const std::string& fallback) const {
  const auto found = attributes.find(std::string(attribute_name));
  return found != attributes.end() ? found->second : fallback;
}

std::string Escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

namespace {

class Parser final {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Node ParseDocument() {
    SkipProlog();
    if (position_ >= text_.size() || text_[position_] != '<') Fail("expected an element");
    auto root = ParseElement(0);
    SkipMisc();
    if (position_ < text_.size()) Fail("unexpected content after the root element");
    return root;
  }

 private:
  [[noreturn]] void Fail(const std::string& what) const {
    int line = 1;
    for (std::size_t i = 0; i < position_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') ++line;
    }
    throw ParseError("XML error at line " + std::to_string(line) + ": " + what);
  }

  [[nodiscard]] bool Starts(std::string_view prefix) const { return text_.substr(position_, prefix.size()) == prefix; }

  void SkipSpace() {
    while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
  }

  void SkipUntil(std::string_view end, const char* what) {
    const auto found = text_.find(end, position_);
    if (found == std::string_view::npos) Fail(std::string("unterminated ") + what);
    position_ = found + end.size();
  }

  // Everything that may precede the root: BOM, declaration, comments, a DOCTYPE.
  void SkipProlog() {
    if (Starts("\xEF\xBB\xBF")) position_ += 3;
    SkipMisc();
  }

  void SkipMisc() {
    for (;;) {
      SkipSpace();
      if (Starts("<?")) SkipUntil("?>", "processing instruction");
      else if (Starts("<!--")) SkipUntil("-->", "comment");
      else if (Starts("<!DOCTYPE")) SkipDoctype();
      else return;
    }
  }

  void SkipDoctype() {
    int depth = 0;
    while (position_ < text_.size()) {
      const auto c = text_[position_++];
      if (c == '[') ++depth;
      else if (c == ']') --depth;
      else if (c == '>' && depth <= 0) return;
    }
    Fail("unterminated DOCTYPE");
  }

  [[nodiscard]] std::string ParseName() {
    const auto start = position_;
    while (position_ < text_.size()) {
      const auto c = static_cast<unsigned char>(text_[position_]);
      if (std::isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.') ++position_;
      else break;
    }
    if (position_ == start) Fail("expected a name");
    return std::string(text_.substr(start, position_ - start));
  }

  [[nodiscard]] std::string Decode(std::string_view raw) const {
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] != '&') {
        out.push_back(raw[i]);
        continue;
      }
      const auto end = raw.find(';', i);
      if (end == std::string_view::npos) Fail("an entity is missing its semicolon");
      const auto name = raw.substr(i + 1, end - i - 1);
      if (name == "amp") out.push_back('&');
      else if (name == "lt") out.push_back('<');
      else if (name == "gt") out.push_back('>');
      else if (name == "quot") out.push_back('"');
      else if (name == "apos") out.push_back('\'');
      else if (!name.empty() && name[0] == '#') {
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const auto code = std::strtoul(std::string(name.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10);
        AppendUtf8(out, static_cast<unsigned long>(code));
      } else {
        Fail("the entity &" + std::string(name) + "; is not defined");
      }
      i = end;
    }
    return out;
  }

  static void AppendUtf8(std::string& out, unsigned long code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) {
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

  static std::string Trim(const std::string& text) {
    std::size_t first = 0, last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
  }

  Node ParseElement(int depth) {
    if (depth > 256) Fail("elements are nested too deeply");
    ++position_;  // '<'
    Node node;
    node.name = ParseName();
    for (;;) {
      SkipSpace();
      if (position_ >= text_.size()) Fail("the tag <" + node.name + "> is not closed");
      if (text_[position_] == '/' ) {
        ++position_;
        if (position_ >= text_.size() || text_[position_] != '>') Fail("expected > after /");
        ++position_;
        return node;
      }
      if (text_[position_] == '>') {
        ++position_;
        break;
      }
      const auto key = ParseName();
      SkipSpace();
      if (position_ >= text_.size() || text_[position_] != '=') Fail("the attribute " + key + " has no value");
      ++position_;
      SkipSpace();
      if (position_ >= text_.size() || (text_[position_] != '"' && text_[position_] != '\'')) Fail("the attribute " + key + " is not quoted");
      const auto quote = text_[position_++];
      const auto end = text_.find(quote, position_);
      if (end == std::string_view::npos) Fail("the attribute " + key + " is not closed");
      node.attributes[key] = Decode(text_.substr(position_, end - position_));
      position_ = end + 1;
    }
    std::string text;
    for (;;) {
      if (position_ >= text_.size()) Fail("the element <" + node.name + "> is not closed");
      if (Starts("</")) {
        position_ += 2;
        const auto closing = ParseName();
        if (closing != node.name) Fail("</" + closing + "> closes <" + node.name + ">");
        SkipSpace();
        if (position_ >= text_.size() || text_[position_] != '>') Fail("expected > in the closing tag");
        ++position_;
        break;
      }
      if (Starts("<!--")) {
        SkipUntil("-->", "comment");
      } else if (Starts("<![CDATA[")) {
        position_ += 9;
        const auto end = text_.find("]]>", position_);
        if (end == std::string_view::npos) Fail("unterminated CDATA");
        text += std::string(text_.substr(position_, end - position_));
        position_ = end + 3;
      } else if (Starts("<?")) {
        SkipUntil("?>", "processing instruction");
      } else if (text_[position_] == '<') {
        node.children.push_back(ParseElement(depth + 1));
      } else {
        const auto next = text_.find('<', position_);
        const auto end = next == std::string_view::npos ? text_.size() : next;
        text += Decode(text_.substr(position_, end - position_));
        position_ = end;
      }
    }
    node.text = Trim(text);
    return node;
  }

  std::string_view text_;
  std::size_t position_{0};
};

}  // namespace

Node Parse(std::string_view text) { return Parser(text).ParseDocument(); }

}  // namespace cutline::xml

// Kite Engine - HTML tokenizer and tree builder
#ifndef KITE_HTML_PARSER_H
#define KITE_HTML_PARSER_H

#include <memory>
#include <string>
#include <vector>

#include "dom/node.h"

namespace kite {

struct HtmlToken {
  enum Type { kStartTag, kEndTag, kText, kComment, kDoctype, kEof };
  Type type;
  std::string name;  // tag name (lower-case) or doctype name
  std::string data;  // text / comment / doctype public id
  std::vector<Attribute> attrs;
  bool selfClosing;
  HtmlToken() : type(kEof), selfClosing(false) {}
  std::string AttrValue(const std::string& n) const {
    for (size_t i = 0; i < attrs.size(); ++i)
      if (attrs[i].name == n) return attrs[i].value;
    return std::string();
  }
};

class HtmlTokenizer {
 public:
  explicit HtmlTokenizer(const std::string& input);
  bool Next(HtmlToken& tok);
  // Switches to raw text mode until </endTag>. RCDATA decodes entities.
  void SetRawText(const std::string& endTag, bool rcdata);
  void SetPlaintext() { plaintext_ = true; }

 private:
  void ParseTag(HtmlToken& tok);
  const std::string& in_;
  size_t pos_;
  std::string rawEnd_;
  bool rcdata_;
  bool plaintext_;
};

// Parses a complete HTML document (UTF-8) into a DOM.
std::unique_ptr<Document> ParseHtml(const std::string& utf8);

// Looks for a charset in a BOM or <meta> within the first bytes of a document.
std::string SniffHtmlCharset(const std::string& bytes);

}  // namespace kite

#endif

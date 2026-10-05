// Kite Engine - CSS object model (selectors, rules, stylesheets) and parser
#ifndef KITE_CSS_STYLESHEET_H
#define KITE_CSS_STYLESHEET_H

#include <memory>
#include <string>
#include <vector>

namespace kite {

struct ComplexSelector;

struct SimpleSelector {
  enum Kind {
    kType,
    kUniversal,
    kId,
    kClass,
    kAttr,
    kPseudoClass,
  };
  enum AttrOp { kExists, kEquals, kIncludes, kDashMatch, kPrefix, kSuffix, kSubstring };

  Kind kind;
  std::string name;   // tag / id / class / attribute / pseudo-class name
  std::string value;  // attribute value
  AttrOp op;
  bool caseInsensitive;
  int nthA, nthB;     // :nth-child(an+b)
  std::vector<ComplexSelector> args;  // :not(), :is(), :where()

  SimpleSelector()
      : kind(kUniversal), op(kExists), caseInsensitive(false), nthA(0), nthB(0) {}
};

struct CompoundSelector {
  std::vector<SimpleSelector> parts;
};

enum PseudoElement { kPseudoNone, kPseudoBefore, kPseudoAfter, kPseudoUnsupported };

struct ComplexSelector {
  // compounds[0] is the leftmost; combinators[i] joins compounds[i] and
  // compounds[i + 1]: ' ' descendant, '>' child, '+' adjacent, '~' sibling.
  std::vector<CompoundSelector> compounds;
  std::vector<char> combinators;
  int specificity;
  PseudoElement pseudo;
  ComplexSelector() : specificity(0), pseudo(kPseudoNone) {}
};

struct Declaration {
  std::string property;  // lower-case except custom properties
  std::string value;     // raw value text (trimmed, comments removed)
  bool important;
  Declaration() : important(false) {}
};

struct StyleRule {
  std::vector<ComplexSelector> selectors;
  std::vector<Declaration> declarations;
  std::vector<std::string> media;  // all must match ("" entries skipped)
  size_t order;
  StyleRule() : order(0) {}
};

struct FontFace {
  std::string family;  // lower-case, unquoted
  int weight;
  bool italic;
  bool coversLatin;  // unicode-range includes basic Latin letters
  std::vector<std::pair<std::string, std::string> > sources;  // url, format
  FontFace() : weight(400), italic(false), coversLatin(true) {}
};

class Stylesheet {
 public:
  std::vector<StyleRule> rules;
  std::vector<FontFace> fontFaces;
  std::vector<std::string> imports;  // @import URLs (unresolved)
};

// Parses a stylesheet. |orderBase| is the starting source-order index.
void ParseStylesheet(const std::string& css, Stylesheet& out);
// Parses the contents of a style="" attribute or a declaration block.
std::vector<Declaration> ParseDeclarations(const std::string& block);
// Parses a selector list; returns false if any selector is invalid.
bool ParseSelectorList(const std::string& text, std::vector<ComplexSelector>& out);

// Media query evaluation against a viewport.
struct MediaContext {
  float width;
  float height;
  MediaContext() : width(1024), height(768) {}
};
bool EvaluateMediaQueryList(const std::string& query, const MediaContext& ctx);

std::string StripCssComments(const std::string& css);

}  // namespace kite

#endif

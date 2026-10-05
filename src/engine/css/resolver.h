// Kite Engine - style resolution (selector matching + cascade)
#ifndef KITE_CSS_RESOLVER_H
#define KITE_CSS_RESOLVER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "css/style.h"
#include "css/stylesheet.h"
#include "dom/node.h"

namespace kite {

struct ElementState {
  const Node* focused;
  const Node* hovered;
  ElementState() : focused(0), hovered(0) {}
};

bool MatchesSelector(const ComplexSelector& sel, const Node* element,
                     const ElementState& state);

class StyleResolver {
 public:
  StyleResolver();

  // Author stylesheets are applied in the order they are added.
  void AddAuthorSheet(std::shared_ptr<Stylesheet> sheet, const std::string& baseUrl);
  void ClearAuthorSheets();

  // Computes styles for every element in |doc| (stored on Node::style).
  void ResolveDocument(Document& doc, const MediaContext& media,
                       const std::string& documentUrl, const ElementState& state);

  static const char* UserAgentCss();

 private:
  struct SheetEntry {
    std::shared_ptr<Stylesheet> sheet;
    std::string baseUrl;
    int origin;  // 0 = user agent, 1 = author
  };
  struct IndexedRule {
    const StyleRule* rule;
    const ComplexSelector* selector;
    int sheetIndex;
    int origin;
    size_t order;
  };
  struct RuleIndex {
    std::map<std::string, std::vector<IndexedRule> > byId, byClass, byTag;
    std::vector<IndexedRule> universal;
  };

  void BuildIndex(const MediaContext& media);
  void ResolveElement(Node* el, const ComputedStyle& parent, const MediaContext& media,
                      const std::string& docUrl, const ElementState& state);
  void CollectMatches(const Node* el, const ElementState& state,
                      std::vector<IndexedRule>& out);

  std::vector<SheetEntry> sheets_;
  RuleIndex index_;
  float rootFontSize_;
};

}  // namespace kite

#endif

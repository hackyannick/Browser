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
  // Shadow root whose stylesheet is being matched (null: the document).
  const Node* scope = nullptr;
  ElementState() : focused(0), hovered(0) {}
};

// Replaces var() references using the custom properties of |style|.
bool SubstituteCssVars(const std::string& value, const ComputedStyle& style, std::string& out);

bool MatchesSelector(const ComplexSelector& sel, const Node* element,
                     const ElementState& state);

class StyleResolver {
 public:
  StyleResolver();

  // Author stylesheets are applied in the order they are added.
  // |scope|: the shadow root a <style> belongs to (null for the document).
  void AddAuthorSheet(std::shared_ptr<Stylesheet> sheet, const std::string& baseUrl,
                      const Node* scope = nullptr);
  void ClearAuthorSheets();

  // Computes styles for every element in |doc| (stored on Node::style).
  void ResolveDocument(Document& doc, const MediaContext& media,
                       const std::string& documentUrl, const ElementState& state);

  static const char* UserAgentCss();

  // @keyframes rule with |name| from the author sheets (last one wins).
  const KeyframesRule* FindKeyframes(const std::string& name) const;
  float rootFontSize() const { return rootFontSize_; }

 private:
  struct SheetEntry {
    std::shared_ptr<Stylesheet> sheet;
    std::string baseUrl;
    int origin;  // 0 = user agent, 1 = author
    const Node* scope = nullptr;
  };
  struct IndexedRule {
    const StyleRule* rule;
    const ComplexSelector* selector;
    int sheetIndex;
    int origin;
    size_t order;
    const Node* scope;
    bool hasHost, hasSlotted;  // :host / ::slotted() (match outside the scope)
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

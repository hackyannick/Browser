// Kite Engine - Document Object Model
#ifndef KITE_DOM_NODE_H
#define KITE_DOM_NODE_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kite {

struct ComputedStyle;
class LayoutBox;

struct Attribute {
  std::string name;   // lower-case
  std::string value;
};

class Node {
 public:
  enum Type { kDocument, kElement, kText, kComment, kDoctype };

  explicit Node(Type t) : type(t), parent(0), style(0), index(0) {}
  ~Node();

  Type type;
  std::string tag;   // lower-case element name
  std::string text;  // text/comment data
  std::vector<Attribute> attrs;
  std::vector<std::unique_ptr<Node> > children;
  Node* parent;

  // Owned computed style (set by the style resolver). Raw pointer to keep the
  // DOM header independent of the CSS headers.
  ComputedStyle* style;
  size_t index;  // position in parent's children
  LayoutBox* layoutBox = nullptr;  // principal box (set by the box builder)

  // Cached values for selector matching.
  std::string id;
  std::vector<std::string> classes;

  // Form control state.
  std::string formValue;
  bool formValueSet = false;
  bool checked = false;
  bool checkedSet = false;
  int selectedIndex = -1;

  bool IsElement() const { return type == kElement; }
  bool IsText() const { return type == kText; }
  bool Is(const char* name) const { return type == kElement && tag == name; }

  const std::string* GetAttr(const std::string& name) const;
  std::string Attr(const std::string& name) const;
  bool HasAttr(const std::string& name) const { return GetAttr(name) != 0; }
  void SetAttr(const std::string& name, const std::string& value);
  void UpdateCaches();

  Node* AppendChild(std::unique_ptr<Node> child);
  Node* InsertBefore(std::unique_ptr<Node> child, Node* ref);
  std::unique_ptr<Node> RemoveChild(Node* child);
  void ReindexChildren();

  Node* FirstElementChild() const;
  Node* PreviousElementSibling() const;
  Node* NextElementSibling() const;
  Node* ParentElement() const {
    return parent && parent->type == kElement ? parent : 0;
  }

  // Concatenated text of all descendants.
  std::string TextContent() const;
  // First descendant (pre-order) with the given tag name.
  Node* FindFirst(const char* tagName);
  void FindAll(const char* tagName, std::vector<Node*>& out);
  Node* FindById(const std::string& id);
  // Nearest ancestor-or-self with the given tag.
  Node* Closest(const char* tagName);

 private:
  Node(const Node&);
  Node& operator=(const Node&);
};

class Document {
 public:
  Document() : root(new Node(Node::kDocument)), quirksMode(false) {}

  std::unique_ptr<Node> root;
  bool quirksMode;

  Node* DocumentElement() const;
  Node* Head() const;
  Node* Body() const;
  std::string Title() const;
};

}  // namespace kite

#endif

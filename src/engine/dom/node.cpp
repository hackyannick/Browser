#include "dom/node.h"

#include "base/strings.h"
#include "css/style.h"
#include "media/media.h"

namespace kite {

Node::~Node() {
  delete style;
  if (mediaId) DestroyMediaPlayer(mediaId);
}

const Node* Node::TreeRoot() const {
  const Node* n = this;
  while (n->parent) n = n->parent;
  return n;
}

namespace {

// Elements of a shadow tree, in tree order, that are <slot>s.
void CollectSlots(const Node* n, std::vector<Node*>& out) {
  for (size_t i = 0; i < n->children.size(); ++i) {
    Node* c = n->children[i].get();
    if (!c->IsElement()) continue;
    if (c->tag == "slot") out.push_back(c);
    CollectSlots(c, out);
  }
}

// The slot of |root| a host child with slot name |name| goes to.
Node* FindSlot(const Node* root, const std::string& name) {
  std::vector<Node*> slots;
  CollectSlots(root, slots);
  for (size_t i = 0; i < slots.size(); ++i)
    if (slots[i]->Attr("name") == name) return slots[i];
  return 0;
}

}  // namespace

Node* Node::AssignedSlot() const {
  if (!parent || !parent->shadowRoot || (type != kElement && type != kText)) return 0;
  return FindSlot(parent->shadowRoot.get(), type == kElement ? Attr("slot") : std::string());
}

void Node::FlatChildren(std::vector<Node*>& out) const {
  if (shadowRoot) {
    for (size_t i = 0; i < shadowRoot->children.size(); ++i) out.push_back(shadowRoot->children[i].get());
    return;
  }
  if (type == kElement && tag == "slot") {
    const Node* root = TreeRoot();
    if (root->type == kShadowRoot && root->host) {
      size_t before = out.size();
      const Node* host = root->host;
      for (size_t i = 0; i < host->children.size(); ++i) {
        Node* c = host->children[i].get();
        if (c->AssignedSlot() == this) out.push_back(c);
      }
      if (out.size() > before) return;
    }
  }
  for (size_t i = 0; i < children.size(); ++i) out.push_back(children[i].get());
}

const std::string* Node::GetAttr(const std::string& name) const {
  for (size_t i = 0; i < attrs.size(); ++i)
    if (attrs[i].name == name) return &attrs[i].value;
  return 0;
}

std::string Node::Attr(const std::string& name) const {
  const std::string* v = GetAttr(name);
  return v ? *v : std::string();
}

void Node::SetAttr(const std::string& name, const std::string& value) {
  for (size_t i = 0; i < attrs.size(); ++i) {
    if (attrs[i].name == name) {
      attrs[i].value = value;
      UpdateCaches();
      return;
    }
  }
  Attribute a;
  a.name = name;
  a.value = value;
  attrs.push_back(a);
  UpdateCaches();
}

void Node::UpdateCaches() {
  id = Attr("id");
  classes = SplitWhitespace(Attr("class"));
}

Node* Node::AppendChild(std::unique_ptr<Node> child) {
  child->parent = this;
  child->index = children.size();
  children.push_back(std::move(child));
  return children.back().get();
}

Node* Node::InsertBefore(std::unique_ptr<Node> child, Node* ref) {
  if (!ref) return AppendChild(std::move(child));
  child->parent = this;
  Node* raw = child.get();
  for (size_t i = 0; i < children.size(); ++i) {
    if (children[i].get() == ref) {
      children.insert(children.begin() + i, std::move(child));
      ReindexChildren();
      return raw;
    }
  }
  return AppendChild(std::move(child));
}

std::unique_ptr<Node> Node::RemoveChild(Node* c) {
  for (size_t i = 0; i < children.size(); ++i) {
    if (children[i].get() == c) {
      std::unique_ptr<Node> out = std::move(children[i]);
      children.erase(children.begin() + i);
      ReindexChildren();
      out->parent = 0;
      return out;
    }
  }
  return std::unique_ptr<Node>();
}

void Node::ReindexChildren() {
  for (size_t i = 0; i < children.size(); ++i) children[i]->index = i;
}

Node* Node::FirstElementChild() const {
  for (size_t i = 0; i < children.size(); ++i)
    if (children[i]->IsElement()) return children[i].get();
  return 0;
}

Node* Node::PreviousElementSibling() const {
  if (!parent) return 0;
  for (size_t i = index; i-- > 0;)
    if (parent->children[i]->IsElement()) return parent->children[i].get();
  return 0;
}

Node* Node::NextElementSibling() const {
  if (!parent) return 0;
  for (size_t i = index + 1; i < parent->children.size(); ++i)
    if (parent->children[i]->IsElement()) return parent->children[i].get();
  return 0;
}

static void CollectText(const Node* n, std::string& out) {
  if (n->type == Node::kText) {
    out += n->text;
    return;
  }
  for (size_t i = 0; i < n->children.size(); ++i)
    CollectText(n->children[i].get(), out);
}

std::string Node::TextContent() const {
  std::string s;
  CollectText(this, s);
  return s;
}

Node* Node::FindFirst(const char* tagName) {
  if (Is(tagName)) return this;
  for (size_t i = 0; i < children.size(); ++i) {
    Node* r = children[i]->FindFirst(tagName);
    if (r) return r;
  }
  return 0;
}

void Node::FindAll(const char* tagName, std::vector<Node*>& out) {
  if (Is(tagName)) out.push_back(this);
  for (size_t i = 0; i < children.size(); ++i)
    children[i]->FindAll(tagName, out);
}

Node* Node::FindById(const std::string& wanted) {
  if (type == kElement && id == wanted) return this;
  for (size_t i = 0; i < children.size(); ++i) {
    Node* r = children[i]->FindById(wanted);
    if (r) return r;
  }
  return 0;
}

Node* Node::Closest(const char* tagName) {
  for (Node* n = this; n; n = n->parent)
    if (n->Is(tagName)) return n;
  return 0;
}

Node* Document::DocumentElement() const { return root->FirstElementChild(); }

Node* Document::Head() const {
  Node* html = DocumentElement();
  if (!html) return 0;
  for (size_t i = 0; i < html->children.size(); ++i)
    if (html->children[i]->Is("head")) return html->children[i].get();
  return 0;
}

Node* Document::Body() const {
  Node* html = DocumentElement();
  if (!html) return 0;
  for (size_t i = 0; i < html->children.size(); ++i)
    if (html->children[i]->Is("body")) return html->children[i].get();
  return 0;
}

std::string Document::Title() const {
  Node* t = root->FindFirst("title");
  if (!t) return std::string();
  return CollapseWhitespace(t->TextContent());
}

}  // namespace kite

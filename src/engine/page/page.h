// Kite Engine - Page: ties together DOM, styles, layout and painting for one
// document. The platform layer feeds it network data and draws its display
// list.
#ifndef KITE_PAGE_PAGE_H
#define KITE_PAGE_PAGE_H

#include <memory>
#include <string>
#include <vector>

#include "css/resolver.h"
#include "dom/node.h"
#include "layout/box.h"
#include "layout/layout.h"
#include "net/url.h"
#include "paint/display_list.h"

namespace kite {

struct FormSubmission {
  std::string method;  // "GET" or "POST"
  std::string url;
  std::string body;
  std::string contentType;
};

class Page {
 public:
  Page(FontProvider* fonts, ImageProvider* images);
  ~Page();

  // Loads a document. |url| is the final URL (after redirects).
  void LoadHtml(const std::string& utf8, const std::string& url);
  void LoadPlainText(const std::string& utf8, const std::string& url);
  void LoadImage(const std::string& url);

  const std::string& url() const { return url_; }
  std::string Title() const;
  Document* document() { return doc_.get(); }

  // Stylesheets referenced via <link> / @import that still need fetching.
  std::vector<std::string> PendingStylesheets() const;
  void ProvideStylesheet(const std::string& url, const std::string& css, bool ok);
  bool HasPendingStylesheets() const { return !PendingStylesheets().empty(); }

  // Recomputes styles (needed after stylesheets arrive or the viewport
  // width changes media query results).
  void Restyle(float viewportW, float viewportH);
  // Rebuilds the box tree and lays out + paints. Call after Restyle or when
  // images finish loading.
  void Relayout(float viewportW, float viewportH);
  // Re-runs only the painter (e.g. after a form control value changed).
  void Repaint();

  const DisplayList& display() const { return display_; }
  float ContentHeight() const { return display_.height; }
  float ContentWidth() const { return display_.width; }

  // Image URLs referenced by the current layout (img, backgrounds).
  std::vector<std::string> ReferencedImages() const;

  // Hit testing in document coordinates.
  Node* HitTest(float x, float y) const;
  static Node* LinkFor(Node* n);
  std::string LinkUrl(Node* link) const;  // resolved href
  Url ResolveUrl(const std::string& ref) const;
  // Y coordinate of #fragment target, or -1.
  float AnchorPosition(const std::string& fragment) const;
  bool BoxRect(Node* n, Rect& out) const;

  // <meta http-equiv=refresh>
  int refreshDelay() const { return refreshDelay_; }
  const std::string& refreshUrl() const { return refreshUrl_; }

  // Forms.
  static Node* FormFor(Node* control);
  bool BuildFormSubmission(Node* form, Node* submitter, FormSubmission& out) const;
  static Node* DefaultSubmitButton(Node* form);

  // Plain text of the whole document (for "find in page").
  std::vector<std::pair<Rect, std::string> > TextRuns() const;

 private:
  struct SheetEntry {
    bool isLink;
    std::string url;     // for links / imports
    std::string media;   // media attribute
    std::string css;
    bool loaded;
    bool failed;
    std::shared_ptr<Stylesheet> sheet;
  };
  void CollectDocumentInfo();
  void AddSheetsFromNode(Node* n);

  FontProvider* fonts_;
  ImageProvider* images_;
  std::string url_;
  Url baseUrl_;
  std::unique_ptr<Document> doc_;
  std::vector<SheetEntry> sheets_;
  StyleResolver resolver_;
  std::unique_ptr<LayoutBox> root_;
  LayoutEngine engine_;
  DisplayList display_;
  float lastViewportW_ = 800, lastViewportH_ = 600;
  int refreshDelay_;
  std::string refreshUrl_;
};

}  // namespace kite

#endif
